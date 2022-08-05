// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <utility>

#include "gsl/gsl"

#if !defined(ORT_MINIMAL_BUILD)
#include "onnx/defs/schema.h"
#endif  // !defined(ORT_MINIMAL_BUILD)

#include "core/common/inlined_containers.h"
#include "core/common/status.h"
#include "core/graph/basic_types.h"
#include "core/graph/graph.h"

namespace flatbuffers {
class FlatBufferBuilder;
template <typename T>
struct Offset;
}  // namespace flatbuffers

namespace onnxruntime {

namespace fbs {
struct KernelTypeStrResolver;
}  // namespace fbs

using common::Status;

using ArgTypeAndIndex = std::pair<ArgType, size_t>;

class KernelTypeStrResolver {
 public:
  using KernelTypeStrToArgsMap = InlinedHashMap<std::string, InlinedVector<ArgTypeAndIndex>>;

  /**
   * Resolves an op's kernel type string to its associated arguments.
   * @param op_id The op identifier.
   * @param kernel_type_str The op kernel type string.
   * @param[out] resolved_args The op arguments associated with kernel_type_str.
   */
  Status ResolveKernelTypeStr(const OpIdentifier& op_id,
                              const std::string& kernel_type_str,
                              gsl::span<const ArgTypeAndIndex>& resolved_args) const;

  /**
   * Registers an op's kernel type string to argument mapping.
   * @param op_id The op identifier.
   * @param kernel_type_str_args The kernel type str to argument mapping.
   * @return True if the op's mapping was registered or false if there is already an existing mapping.
   */
  bool RegisterKernelTypeStrToArgsMap(OpIdentifier op_id, KernelTypeStrToArgsMap kernel_type_str_to_args);

#if !defined(ORT_MINIMAL_BUILD)
  Status RegisterOpSchema(const ONNX_NAMESPACE::OpSchema& op_schema, bool* registered = nullptr);

  Status RegisterNodeOpSchema(const Node& node);

  static KernelTypeStrResolver CreateFromNodeOpSchema(const Node& node) {
    KernelTypeStrResolver k{};
    ORT_THROW_IF_ERROR(k.RegisterNodeOpSchema(node));
    return k;
  }

  Status RegisterGraphNodeOpSchemas(const Graph& graph);

  static KernelTypeStrResolver CreateFromGraphNodeOpSchemas(const Graph& graph) {
    KernelTypeStrResolver k{};
    ORT_THROW_IF_ERROR(k.RegisterGraphNodeOpSchemas(graph));
    return k;
  }

  Status SaveToOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                         flatbuffers::Offset<fbs::KernelTypeStrResolver>& fbs_kernel_type_str_resolver) const;
#endif  // !defined(ORT_MINIMAL_BUILD)

  Status LoadFromOrtFormat(const fbs::KernelTypeStrResolver& fbs_kernel_type_str_resolver);

 private:
  using OpKernelTypeStrMap = InlinedHashMap<OpIdentifier, KernelTypeStrToArgsMap>;
  OpKernelTypeStrMap op_kernel_type_str_map_;
};

}  // namespace onnxruntime