#include <cuda_runtime.h>

__global__ void interop_fill_kernel(cudaSurfaceObject_t surface, unsigned int width, unsigned int height,
                                    unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const uchar4 pixel = make_uchar4(r, g, b, a);
    surf2Dwrite(pixel, surface, x * static_cast<int>(sizeof(uchar4)), y);
}

extern "C" void launch_interop_fill_kernel(void* surfaceObject, unsigned int width, unsigned int height,
                                           const unsigned char rgba[4], void* stream) {
    if (surfaceObject == nullptr || width == 0 || height == 0 || rgba == nullptr) {
        return;
    }

    const cudaSurfaceObject_t surface =
        static_cast<cudaSurfaceObject_t>(reinterpret_cast<uintptr_t>(surfaceObject));
    const dim3 block(16, 16);
    const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    cudaStream_t cudaStream = stream != nullptr ? static_cast<cudaStream_t>(stream) : 0;
    interop_fill_kernel<<<grid, block, 0, cudaStream>>>(surface, width, height, rgba[0], rgba[1], rgba[2],
                                                        rgba[3]);
}
