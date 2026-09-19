#include <fuse/renderer/cuda/vk_sync.hpp>

namespace fuse::renderer::cuda {

SharedTimeline SharedTimeline::create(void* /*vkDevice*/) {
    SharedTimeline timeline{};
    timeline.message = "SharedTimeline stub — timeline semaphore pair deferred to B2.6";
    return timeline;
}

void SharedTimeline::destroy(void* /*vkDevice*/) {
    vkSemaphore = nullptr;
    cudaSemaphore = nullptr;
    value = 0;
    valid = false;
    message = nullptr;
}

bool SharedTimeline::signalVulkan(void* /*vkDevice*/, u64 /*newValue*/) const {
    return false;
}

bool SharedTimeline::waitCuda(void* /*cudaStream*/, u64 /*waitValue*/) const {
    if (!valid) {
        return false;
    }
    return false;
}

} // namespace fuse::renderer::cuda
