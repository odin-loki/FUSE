#include <fuse/renderer/cuda/vk_sync.hpp>

namespace fuse::renderer::cuda {

SharedTimeline SharedTimeline::create(void* /*vkDevice*/) {
    SharedTimeline timeline{};
    return timeline;
}

void SharedTimeline::destroy(void* /*vkDevice*/) {
    vkSemaphore = nullptr;
    cudaSemaphore = nullptr;
    value = 0;
    valid = false;
}

} // namespace fuse::renderer::cuda
