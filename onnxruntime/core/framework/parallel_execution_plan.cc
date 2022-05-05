// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/common/spin_pause.h"
#include "core/framework/parallel_execution_plan.h"
#include "core/framework/session_state.h"
#include "core/framework/execution_frame.h"
#include "core/graph/constants.h"
#include <vector>

namespace onnxruntime {

struct Barrier {
  std::atomic_bool set_{false};
  void set() {
    set_.store(true);
  }
  void wait() {
    while (!set_.load()) {
      onnxruntime::concurrency::SpinPause(); 
    }
  }
};
using NotificationIndex = size_t;


// similiar as Stream
struct Notification {
  NotificationHandle handle;
  const IExecutionProvider* provider;
};


void RegisterStreamCommandHanler(const SessionState& session_state) {
  auto& eps = session_state.GetExecutionProviders();
  for (auto& ep : eps) {
    ep->RegisterStreamHandlers(GetStreamHandleRegistryInstance());
  }
}

// execution context that support to execute a command on stream.
// The notifications got instantiated when execution context is constructed.
// TODO: if we merge the notifications to execution frame, we might don't need this.
struct ExecutionContext {
  const SessionState* session_state;
  ExecutionFrame* frame;
  const logging::Logger* logger;
  std::unique_ptr<Notification[]> notifications;
  std::vector<ReleaseNotificationFn> notification_release_fns;

  ExecutionContext(const SessionState& sess_state,
      ExecutionFrame* execution_frame,
      std::vector<Stream*> notification_owners,
      const logging::Logger& sess_logger) : session_state(&sess_state), 
                                            frame(execution_frame), 
                                            logger(&sess_logger),
                                            notifications(new Notification[notification_owners.size()]){
    for (auto i = 0; i < notification_owners.size(); ++i) {
      auto create_notification_fn = GetStreamHandleRegistryInstance().GetCreateNotificationFn(notification_owners[i]);
      notifications[i].handle = create_notification_fn(notification_owners[i]->handle);
      notifications[i].provider = notification_owners[i]->provider;
      notification_release_fns.push_back(
          GetStreamHandleRegistryInstance().GetReleaseNotificationFn(notification_owners[i])
      );
    }
  }

  ~ExecutionContext() {
    for (auto i = 0; i < notification_release_fns.size(); ++i) {
      notification_release_fns[i](notifications[i].handle);
    }
  }
};

using CommandFn = std::function<void(ExecutionContext&)>;

// a logic stream to execute command.
// each command in the logic stream will be executed in FIFO
// a logic stream will be binded to multiple device stream, as the command in the same logic stream may be executed on different EPs.
// i.e., if we set concurrency level to 1, the single logic stream will be equal to our sequential execution plan, which has both cpu and gpu kernels
struct LogicStream {
 
  std::vector<std::unique_ptr<Stream>> device_streams_;
  std::vector<CommandFn> commands_;
  
  void Run(ExecutionContext& ctx) {
    for (auto& command : commands_) {
      command(ctx);
    }
    // flush
    for (auto& device_stream : device_streams_) {
      auto flush_stream_fn = GetStreamHandleRegistryInstance().GetFlushStreamFn(device_stream->provider->Type());
      flush_stream_fn(device_stream->handle);
    }
  }

  ~LogicStream() {
    for (auto& device_stream : device_streams_) {
      auto release_stream_fn = GetStreamHandleRegistryInstance().GetReleaseStreamFn(device_stream->provider->Type());
      release_stream_fn(device_stream->handle);
    }
  }

};

struct LogicStream;

struct ParallelExecutionPlanImpl {
  ParallelExecutionPlanImpl(const SessionState& session_state, int num_logic_streams);
  ~ParallelExecutionPlanImpl();
  common::Status Execute(const SessionState& session_state,
                         const std::vector<int>& feed_mlvalue_idxs,
                         const std::vector<OrtValue>& feeds,
                         const std::vector<int>& fetch_mlvalue_idxs,
                         std::vector<OrtValue>& fetches,
                         const std::unordered_map<size_t, IExecutor::CustomAllocator>& fetch_allocators,
                         const logging::Logger& logger);

  Stream* GetComputeStreamForNode(NodeIndex index) const {
    auto it = node_to_stream_map_.find(index);
    return it == node_to_stream_map_.end() ? nullptr : it->second;
  }

  std::vector<std::unique_ptr<LogicStream>> logic_streams_;
  const SessionState& session_state_;
  int num_logic_streams_{};
  // the stream where the notificaiton got created.
  std::vector<Stream*> notification_owners_;
  std::unordered_map<NodeIndex, Stream*> node_to_stream_map_;
};

std::once_flag populate_command_handle_flag;

//todo: remove dependency on session_state
ParallelExecutionPlanImpl::ParallelExecutionPlanImpl(const SessionState& session_state,
                                                     int num_logic_streams) : session_state_(session_state), num_logic_streams_(num_logic_streams) {
  // register handle once
  std::call_once(
      populate_command_handle_flag, [](const SessionState& sess_state) { RegisterStreamCommandHanler(sess_state); }, session_state);
  // instantiate logic streams
  std::vector<std::vector<std::string>> streams_stdout;
  for (int i = 0; i < num_logic_streams_; ++i) {
    logic_streams_.push_back(std::make_unique<LogicStream>());
    streams_stdout.push_back(std::vector<std::string>{});
  }
  
  const auto& graph_viewer = session_state_.GetGraphViewer();
  
  //1. partition the nodes into streams
  std::unique_ptr<std::vector<NodeIndex>[]> nodes_in_stream { new std::vector<NodeIndex>[num_logic_streams_] };
  std::unique_ptr<size_t[]> node_stream_map{new size_t[graph_viewer.MaxNodeIndex()]};
  // todo: devise a better allocation algo, with benchmarks
  int stream_iter = 0;
  for (auto node_index : graph_viewer.GetNodesInTopologicalOrder()) {
    nodes_in_stream[stream_iter].push_back(node_index);
    streams_stdout[stream_iter].push_back(graph_viewer.GetNode(node_index)->OpType());
    node_stream_map[node_index] = stream_iter;
    stream_iter = (stream_iter + 1) % num_logic_streams_;
  }
  //2. for each node, if any of its consumer partitioned to another stream, generate a notification
  size_t num_notifications=0;
  std::unordered_map<NodeIndex, NotificationIndex> node_to_notification;
  for (auto i = 0; i < num_logic_streams_; ++i) {
    for (auto node_index : nodes_in_stream[i]) {
      auto* node = graph_viewer.GetNode(node_index);
      for (auto it = node->OutputNodesBegin(); it != node->OutputNodesEnd(); ++it) {
        if (std::find(nodes_in_stream[i].begin(), nodes_in_stream[i].end(), it->Index()) == nodes_in_stream[i].end()) {
          node_to_notification[node_index] = num_notifications++;
          break;
        }
      }
    }
  }
  //3. Check the nodes in each logical stream, bind it to device streams
  for (auto i = 0; i < num_logic_streams_; ++i) {
    std::set<const IExecutionProvider*> providers;
    for (auto node_index : nodes_in_stream[i]) {
      auto* node = graph_viewer.GetNode(node_index);
      onnxruntime::ProviderType exec_provider_name = node->GetExecutionProviderType();
      const IExecutionProvider* ep = session_state.GetExecutionProviders().Get(exec_provider_name);
      if (providers.find(ep) == providers.end()) {
        auto create_stream_fn = GetStreamHandleRegistryInstance().GetCreateStreamFn(ep->Type());
        ORT_ENFORCE(create_stream_fn);
        logic_streams_[i]->device_streams_.emplace_back(std::make_unique<Stream>(create_stream_fn(), ep));
        providers.insert(ep);
      }
      // setup node to stream map
      auto& streams = logic_streams_[node_stream_map[node_index]]->device_streams_;
      auto stream_it = std::find_if(streams.begin(),
                                    streams.end(),
                                    [&](std::unique_ptr<Stream>& stream) { return stream->provider == ep; });
      ORT_ENFORCE(stream_it != streams.end());
      node_to_stream_map_[node_index] = stream_it->get();
    }
  }
  //4. set notification owners
  notification_owners_.resize(num_notifications);
  for (auto node_index : graph_viewer.GetNodesInTopologicalOrder()) {
    auto it = node_to_notification.find(node_index);
    if (it != node_to_notification.end()) {
      // notification owned by the node who produced it.
      // use the producer's EP instance poitner as owner id
      auto* node = graph_viewer.GetNode(node_index);
      onnxruntime::ProviderType exec_provider_name = node->GetExecutionProviderType();
      const IExecutionProvider* ep = session_state.GetExecutionProviders().Get(exec_provider_name);
      auto& streams = logic_streams_[node_stream_map[node_index]]->device_streams_;
      auto stream_it = std::find_if(streams.begin(),
                                    streams.end(),
                                    [&](std::unique_ptr<Stream>& stream) { return stream->provider == ep; });
      ORT_ENFORCE(stream_it != streams.end());
      notification_owners_[it->second] = stream_it->get();
    }
  }
  //5. add commands to logic queue
  for (auto i = 0; i < num_logic_streams_; ++i) {
    for (auto node_index : nodes_in_stream[i]) {
      // check if any producer is not in current stream, if yes, create a wait
      auto* node = graph_viewer.GetNode(node_index);
      for (auto it = node->InputNodesBegin(); it != node->InputNodesEnd(); ++it) {
        if (std::find(nodes_in_stream[i].begin(), nodes_in_stream[i].end(), it->Index()) == nodes_in_stream[i].end()) {
          // find the notificaiton id
          auto notfication_it = node_to_notification.find(it->Index());
          ORT_ENFORCE(notfication_it != node_to_notification.end());
          // push a wait command
          auto wait_handle = GetStreamHandleRegistryInstance().GetWaitHandle(notification_owners_[notfication_it->second], node->GetExecutionProviderType());
          NotificationIndex notification_index = notfication_it->second;
          auto* cur_stream = node_to_stream_map_[node_index];
          logic_streams_[i]->commands_.push_back([wait_handle, cur_stream, notification_index](ExecutionContext& ctx) {
            wait_handle(cur_stream, ctx.notifications[notification_index].handle);
          });
        }
      }
      // push launch kernel command
      auto& streams = logic_streams_[i]->device_streams_;
      onnxruntime::ProviderType exec_provider_name = node->GetExecutionProviderType();
      const IExecutionProvider* ep = session_state.GetExecutionProviders().Get(exec_provider_name);
      auto stream_it = std::find_if(streams.begin(),
                                    streams.end(),
                                    [&](std::unique_ptr<Stream>& stream) { return stream->provider == ep; });
      ORT_ENFORCE(stream_it != streams.end());
      logic_streams_[i]->commands_.push_back([node_index](ExecutionContext& ctx) {
        auto* p_kernel = ctx.session_state->GetKernel(node_index);
        auto* intra_tp = ctx.session_state->GetThreadPool();
        OpKernelContext kernel_ctx(ctx.frame, p_kernel, intra_tp, *ctx.logger);
        ORT_ENFORCE(p_kernel->Compute(&kernel_ctx).IsOK(), MakeString("kernel fail!"));
      });
      // check if any notification generated by this node, if yes, push a notify
      auto notification_it = node_to_notification.find(node_index);
      if (notification_it != node_to_notification.end()) {
        auto notify_handle = GetStreamHandleRegistryInstance().GetNotifyHandle(stream_it->get());
        NotificationIndex notification_index = notification_it->second;
        logic_streams_[i]->commands_.push_back([notify_handle, notification_index](ExecutionContext& ctx) {
          notify_handle(ctx.notifications[notification_index].handle);
        });
      }
    }
  }

  std::function<std::string(const std::string&)> shape_output = [&](const std::string& s) {
    if (s.size() < 10) {
      return "node_" + s + "_computation";
    } else {
      return s;
    }
  };

  std::cout << logic_streams_.size() << " logic stream created" << std::endl;
  for (int i = 0; i < logic_streams_.size(); ++i) {
    std::cout << " -------- logic stream " << i;
  }
  std::cout << std::endl;
  for (int i = 0;; ++i) {
    bool has_out = false;
    for (int j = 0; j < streams_stdout.size(); ++j) {
      if (i < streams_stdout[j].size()) {
        has_out = true;
        std::cout << "      " << shape_output(streams_stdout[j][i]);
      } else {
        std::cout << "               ";
      }
    }
    std::cout << std::endl;
    if (!has_out) break;
  }
}

ParallelExecutionPlanImpl::~ParallelExecutionPlanImpl() {

}

common::Status ParallelExecutionPlanImpl::Execute(const SessionState& session_state, const std::vector<int>& feed_mlvalue_idxs,
                                                  const std::vector<OrtValue>& feeds, const std::vector<int>& fetch_mlvalue_idxs,
                                                  std::vector<OrtValue>& fetches,
                                                  const std::unordered_map<size_t, IExecutor::CustomAllocator>& fetch_allocators,
                                                  const logging::Logger& logger) {
  ExecutionFrame frame(feed_mlvalue_idxs, feeds, fetch_mlvalue_idxs, fetches, fetch_allocators, session_state);
  auto* tp = session_state.GetInterOpThreadPool();
  // prepare the execution context, notifications got initialized.
  ExecutionContext execution_context(session_state, &frame, notification_owners_, logger);
  std::unique_ptr<Barrier[]> barriers{new Barrier[num_logic_streams_-1]}; //todo: handle case when num_logic_streams_ == 0

  for (int i = 0; i < num_logic_streams_-1; ++i) {
    LogicStream* stream = logic_streams_[i].get();
    Barrier* barrier = &barriers.get()[i];
    concurrency::ThreadPool::Schedule(tp, [&]() {
      stream->Run(execution_context);
      barrier->set();
    });
  }//for

  // run last stream in main thread
  LogicStream* stream = logic_streams_[num_logic_streams_-1].get();
  stream->Run(execution_context);

  for (int i = 0; i < num_logic_streams_-1; ++i) {
    barriers[i].wait();
  }

  //TODO: we might need to flush all the stream before return the result.

  ORT_RETURN_IF_ERROR(frame.GetOutputs(fetches));
  return Status::OK();
}

ParallelExecutionPlan::ParallelExecutionPlan(const SessionState& session_state, int num_logic_streams) {
  impl_ = std::make_unique<ParallelExecutionPlanImpl>(session_state, num_logic_streams);
}

ParallelExecutionPlan::~ParallelExecutionPlan() {
}

common::Status ParallelExecutionPlan::Execute(const SessionState& session_state, const std::vector<int>& feed_mlvalue_idxs,
                                              const std::vector<OrtValue>& feeds, const std::vector<int>& fetch_mlvalue_idxs,
                                              std::vector<OrtValue>& fetches,
                                              const std::unordered_map<size_t, CustomAllocator>& fetch_allocators,
                                              const logging::Logger& logger) {
  return impl_->Execute(session_state, feed_mlvalue_idxs, feeds, fetch_mlvalue_idxs, fetches, fetch_allocators, logger);
}


Stream* ParallelExecutionPlan::GetComputeStreamForNode(NodeIndex index) const {
  return impl_->GetComputeStreamForNode(index);
}

}  // namespace onnxruntime