// CUDA side of the B1 shared-header gate (compile-only on machines without an NVIDIA GPU).
// Includes the same fuse/types.hpp, fuse/math/*, fuse/gria.hpp that the C++23 engine uses and
// exercises them from __global__ code; shared_layout_asserts.hpp is re-evaluated in the device pass.

#include "shared_layout_asserts.hpp"

#include <cuda_runtime.h>

static_assert(__cplusplus >= 202002L, "FUSE CUDA TUs compile as C++20 (nvcc 12.x device dialect)");
#if defined(__CUDA_ARCH__)
static_assert(__CUDA_ARCH__ >= 520, "device pass must target a real SM architecture");
#endif

namespace fuse::cuda_gate {

__global__ void sharedHeadersKernel(const GateSample* samples, f32* results, u32 count) {
    const u32 i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) {
        return;
    }
    evaluateGateSample(samples[i], results + i * kGateResultFloats);
}

} // namespace fuse::cuda_gate

/// nvcc host-pass evaluation of the same FUSE_HOST_DEVICE function (runs without a GPU).
extern "C" void fuse_cuda_gate_eval_nvcc_host(const fuse::cuda_gate::GateSample* samples, fuse::f32* results,
                                              fuse::u32 count) {
    for (fuse::u32 i = 0; i < count; ++i) {
        fuse::cuda_gate::evaluateGateSample(samples[i], results + i * fuse::cuda_gate::kGateResultFloats);
    }
}

/// Device evaluation. Returns false (and touches nothing) when no CUDA device is present.
extern "C" bool fuse_cuda_gate_eval_device(const fuse::cuda_gate::GateSample* samples, fuse::f32* results,
                                           fuse::u32 count) {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices <= 0) {
        return false;
    }
    using fuse::cuda_gate::GateSample;
    GateSample* dSamples = nullptr;
    fuse::f32* dResults = nullptr;
    const size_t resultBytes = sizeof(fuse::f32) * fuse::cuda_gate::kGateResultFloats * count;
    bool ok = cudaMalloc(reinterpret_cast<void**>(&dSamples), sizeof(GateSample) * count) == cudaSuccess &&
              cudaMalloc(reinterpret_cast<void**>(&dResults), resultBytes) == cudaSuccess &&
              cudaMemcpy(dSamples, samples, sizeof(GateSample) * count, cudaMemcpyHostToDevice) == cudaSuccess;
    if (ok) {
        fuse::cuda_gate::sharedHeadersKernel<<<1, 64>>>(dSamples, dResults, count);
        ok = cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess &&
             cudaMemcpy(results, dResults, resultBytes, cudaMemcpyDeviceToHost) == cudaSuccess;
    }
    cudaFree(dSamples);
    cudaFree(dResults);
    return ok;
}
