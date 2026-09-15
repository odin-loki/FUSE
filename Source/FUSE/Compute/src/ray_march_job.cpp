#include <fuse/compute/ray_march_job.hpp>
#include <fuse/jobs/cuda_jobs.hpp>

namespace fuse::compute {

void submit_ray_march_job(RayMarchJobDesc desc) {
    jobs::CUDAJobDesc cudaDesc{};
    cudaDesc.counter = desc.counter;
    cudaDesc.tag = desc.tag;
    cudaDesc.kernel_launcher = [params = desc.params](jobs::CUDAStreamHandle stream) {
        launch_ray_march(params, stream.native);
    };
    jobs::submit_cuda(std::move(cudaDesc));
}

} // namespace fuse::compute
