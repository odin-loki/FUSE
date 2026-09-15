#include <fuse/jobs/cuda_jobs.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

namespace fuse::jobs {

namespace {

#if defined(FUSE_HAS_CUDA)
cudaStream_t g_managedStream = nullptr;
bool g_cudaInitAttempted = false;
bool g_cudaReady = false;

bool ensureCudaReady() {
    if (g_cudaInitAttempted) {
        return g_cudaReady;
    }
    g_cudaInitAttempted = true;
    int deviceCount = 0;
    if (cudaGetDeviceCount(&deviceCount) != cudaSuccess || deviceCount <= 0) {
        return false;
    }
    if (cudaSetDevice(0) != cudaSuccess) {
        return false;
    }
    if (cudaStreamCreate(&g_managedStream) != cudaSuccess) {
        g_managedStream = nullptr;
        return false;
    }
    g_cudaReady = true;
    return true;
}
#endif

void runCudaJob(CUDAJobDesc desc) {
    CUDAStreamHandle stream{};
#if defined(FUSE_HAS_CUDA)
    if (g_cudaReady && g_managedStream != nullptr) {
        stream.native = g_managedStream;
    }
#endif

    if (desc.kernel_launcher) {
        desc.kernel_launcher(stream);
    }

#if defined(FUSE_HAS_CUDA)
    if (g_cudaReady && g_managedStream != nullptr) {
        cudaStreamSynchronize(g_managedStream);
    }
#endif

    if (desc.counter != nullptr) {
        desc.counter->signal();
    }
}

} // namespace

bool cudaJobsAvailable() {
#if defined(FUSE_HAS_CUDA)
    return ensureCudaReady();
#else
    return false;
#endif
}

void submit_cuda(CUDAJobDesc desc) {
    auto& scheduler = JobScheduler::instance();
    if (scheduler.isInitialized()) {
        scheduler.submit([desc = std::move(desc)]() { runCudaJob(std::move(desc)); });
        return;
    }
    runCudaJob(std::move(desc));
}

} // namespace fuse::jobs
