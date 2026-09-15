#include <fuse/renderer/vk/fence_wait.hpp>

namespace fuse::renderer {

bool waitInFlightFenceForSlot(FrameManager& manager, u32 slotIndex) {
    if (!manager.isReady()) {
        return false;
    }
    return manager.waitInFlightFence(slotIndex);
}

bool waitAllInFlightFences(FrameManager& manager) {
    if (!manager.isReady()) {
        return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (!manager.waitInFlightFence(i)) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::renderer
