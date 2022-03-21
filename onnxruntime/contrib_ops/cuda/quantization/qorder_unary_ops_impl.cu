// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contrib_ops/cuda/quantization/qorder_unary_ops_impl.h"
#include "core/providers/cuda/cu_inc/common.cuh"
#include "core/providers/cuda/shared_inc/cuda_utils.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

using namespace onnxruntime::cuda;

constexpr int kNumLinePerThread = 4;
constexpr int kNumThreadsPerBlock = 256;
constexpr int kNumElementsPerBlockLine = sizeof(char4) * kNumThreadsPerBlock;
constexpr int kNumElementsPerBlock = sizeof(char4) * kNumLinePerThread * kNumThreadsPerBlock;

// Half2 kernel
template <typename FuncT>
__global__ void _QOrderUnaryElementWiseKernel(
    const int8_t* input_data, half2 input_scale, int8_t* output_data, half2 inverse_output_scale, const FuncT functor, CUDA_LONG N) {
  CUDA_LONG id = kNumElementsPerBlock * blockIdx.x + threadIdx.x * (CUDA_LONG)sizeof(char4);
  char4 i4;
  unsigned int u32;

  #pragma unroll
  for (int line = 0; line < kNumLinePerThread; line++) {
    if (id < N) {
      i4 = *(const char4*)(input_data + id);
      half2 low = __halves2half2(__short2half_rn((short)i4.x), __short2half_rn((short)i4.y));
      low = functor(low * input_scale) * inverse_output_scale;
      half2 high = __halves2half2(__short2half_rn((short)i4.z), __short2half_rn((short)i4.w));
      high = functor(high * input_scale) * inverse_output_scale;

      u32 = (unsigned)(unsigned short)__half2short_rn(__low2half(low));
      u32 |= ((unsigned)(unsigned short)__half2short_rn(__high2half(low))) << 16;
      u32 = __vmaxs2(u32, 0x00800080U); // -128
      u32 = __vmins2(u32, 0x007F007FU); // 127
      i4.x = (char)u32;
      i4.y = (char)(u32 >> 8);

      u32 = (unsigned)(unsigned short)__half2short_rn(__low2half(high));
      u32 |= ((unsigned)(unsigned short)__half2short_rn(__high2half(high))) << 16;
      u32 = __vmaxs2(u32, 0x00800080U); // -128
      u32 = __vmins2(u32, 0x007F007FU); // 127
      i4.z = (char)u32;
      i4.w = (char)(u32 >> 8);

      *(char4*)(output_data + id) = i4;
      id += kNumElementsPerBlockLine;
    }
  }
}

template <typename FuncT>
void QOrderUnaryElementWiseImpl(
    cudaStream_t stream,
    const int8_t* input_data,
    const float* input_scale,
    int8_t* output_data,
    const float* output_scale,
    const FuncT& func,
    size_t count) {
  if (count & 0x3) {
    throw std::runtime_error("Count must group in 4");
  }

  if (count > 0) {
    int blocksPerGrid = static_cast<int>(CeilDiv(count, kNumElementsPerBlock));
    half2 half_input_scale = __floats2half2_rn(*input_scale, *input_scale);
    half2 half_inverse_output_scale = __floats2half2_rn(1.0 / *output_scale, 1.0 / *output_scale);
    _QOrderUnaryElementWiseKernel<FuncT><<<blocksPerGrid, kNumThreadsPerBlock, 0, stream>>>(
        input_data, half_input_scale, output_data, half_inverse_output_scale, func, static_cast<CUDA_LONG>(count));
  }
}

struct QOrderUnaryOpFastGeluHalf2 {
  static constexpr float A = 0.5f;
  static constexpr float B = 0.7978845608028654f;  // sqrt(2.0/M_PI)
  static constexpr float C = 0.035677408136300125f;  // 0.044715 * sqrt(2.0/M_PI)

  const half2 A2 = __floats2half2_rn(A, A);
  const half2 B2 = __floats2half2_rn(B, B);
  const half2 C2 = __floats2half2_rn(C, C);

  __device__ __inline__ half2 operator()(const half2& x) const {
    return x * (A2 + A2 * _Tanh(x * (C2 * x * x + B2)));
  }
};


QORDER_UNARY_OP_DECLARATION(Gelu) {
  QOrderUnaryElementWiseImpl<QOrderUnaryOpFastGeluHalf2>(
    stream, input_data, input_scale, output_data, output_scale, QOrderUnaryOpFastGeluHalf2(), count);
}

/*
#define LIST_OF_QORDER_UNARY_OPS()          \
  QORDER_UNARY_OP_NAME_EXPR(Gelu, _Gelu(a))


#define DEFINE_QORDER_OP(name, expr)                                 \
  struct QOrderUnaryOp##name {                                       \
    __device__ __inline__ float operator()(const float& a) const {   \
      return expr;                                                   \
    }                                                                \
  };

#define QORDER_UNARY_OP_IMPL(name)                                                         \
  QORDER_UNARY_OP_DECLARATION(name) {                                                      \
    QOrderUnaryElementWiseImpl<QOrderUnaryOp##name>(stream, input_data, input_scale, output_data, output_scale, \
                               QOrderUnaryOp##name(), count);                              \
  }


#define QORDER_UNARY_OP_NAME_EXPR(name, expr) \
  DEFINE_QORDER_OP(name, expr)                \
  QORDER_UNARY_OP_IMPL(name)

LIST_OF_QORDER_UNARY_OPS()
#undef QORDER_UNARY_OP_NAME_EXPR

*/

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime