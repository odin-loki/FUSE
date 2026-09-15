#pragma once

#include <fuse/compute/ray_march.hpp>
#include <fuse/jobs/job_counter.hpp>

namespace fuse::compute {

struct RayMarchJobDesc {
    RayMarchParams params;
    jobs::JobCounter* counter = nullptr;
    const char* tag = "ray_march";
};

/// Submits an SDF ray-march pass via `submit_cuda` / `CUDAJobDesc`.
void submit_ray_march_job(RayMarchJobDesc desc);

} // namespace fuse::compute
