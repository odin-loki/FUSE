#include <fuse/compute/screen_space_effects_job.hpp>
#include <fuse/jobs/cuda_jobs.hpp>

namespace fuse::compute {

void submit_ssao_job(SSAOJobDesc desc) {
    jobs::CUDAJobDesc cudaDesc{};
    cudaDesc.counter = desc.counter;
    cudaDesc.tag = desc.tag;
    cudaDesc.kernel_launcher = [params = desc.params](jobs::CUDAStreamHandle stream) {
        launch_ssao(params, stream.native);
    };
    jobs::submit_cuda(std::move(cudaDesc));
}

void submit_ssr_job(SSRJobDesc desc) {
    jobs::CUDAJobDesc cudaDesc{};
    cudaDesc.counter = desc.counter;
    cudaDesc.tag = desc.tag;
    cudaDesc.kernel_launcher = [params = desc.params](jobs::CUDAStreamHandle stream) {
        launch_ssr(params, stream.native);
    };
    jobs::submit_cuda(std::move(cudaDesc));
}

void submit_ssgi_job(SSGIJobDesc desc) {
    jobs::CUDAJobDesc cudaDesc{};
    cudaDesc.counter = desc.counter;
    cudaDesc.tag = desc.tag;
    cudaDesc.kernel_launcher = [params = desc.params](jobs::CUDAStreamHandle stream) {
        launch_ssgi(params, stream.native);
    };
    jobs::submit_cuda(std::move(cudaDesc));
}

} // namespace fuse::compute
