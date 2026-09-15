#pragma once

#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/jobs/job_counter.hpp>

namespace fuse::compute {

struct SSAOJobDesc {
    SSAOParams params;
    jobs::JobCounter* counter = nullptr;
    const char* tag = "ssao";
};

struct SSRJobDesc {
    SSRParams params;
    jobs::JobCounter* counter = nullptr;
    const char* tag = "ssr";
};

struct SSGIJobDesc {
    SSGIParams params;
    jobs::JobCounter* counter = nullptr;
    const char* tag = "ssgi";
};

/// Submit screen-space effect passes via `submit_cuda` / `CUDAJobDesc`.
void submit_ssao_job(SSAOJobDesc desc);
void submit_ssr_job(SSRJobDesc desc);
void submit_ssgi_job(SSGIJobDesc desc);

} // namespace fuse::compute
