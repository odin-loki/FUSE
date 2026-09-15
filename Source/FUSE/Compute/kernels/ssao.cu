#include <cuda_runtime.h>

__global__ void ssao_kernel_stub() {}

extern "C" void launch_ssao_kernel_stub(void* stream) {
    cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    ssao_kernel_stub<<<1, 1, 0, cudaStream>>>();
}
