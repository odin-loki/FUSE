#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/types.hpp>

#include <functional>

namespace fuse::jobs {

/// Opaque CUDA stream handle — `native` is a `cudaStream_t` when `cudaJobsAvailable()`.
struct CUDAStreamHandle {
    void* native = nullptr;
};

struct CUDAJobDesc {
    std::function<void(CUDAStreamHandle stream)> kernel_launcher;
    JobCounter* counter = nullptr;
    const char* tag = nullptr;
};

/// True when configured with `FUSE_BUILD_CUDA=ON`, toolkit found, and runtime init succeeded.
bool cudaJobsAvailable();

/// Dispatch a CUDA kernel launch through the job scheduler.
/// Stub path (no toolkit / no device): runs `kernel_launcher` on a worker with a null stream.
void submit_cuda(CUDAJobDesc desc);

} // namespace fuse::jobs
