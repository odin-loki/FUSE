#include <fuse/renderer/vk/fence_wait.hpp>

namespace fuse::renderer {

u32 countPendingInFlightFences(const FrameManager& manager) {
    if (!manager.isReady()) {
        return 0;
    }

    u32 pending = 0;
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (isInFlightFenceSignaled(manager.slot(i))) {
            ++pending;
        }
    }
    return pending;
}

bool waitInFlightFenceForSlot(FrameManager& manager, u32 slotIndex) {
    if (!manager.isReady() || !isValidFrameSlotIndex(slotIndex)) {
        return false;
    }
    return manager.waitInFlightFence(slotIndex);
}

bool waitCurrentInFlightFence(FrameManager& manager) {
    if (!manager.isReady()) {
        return false;
    }
    return manager.waitInFlightFence(manager.currentIndex());
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

bool waitInFlightFenceIfSignaled(FrameManager& manager, u32 slotIndex) {
    if (!manager.isReady() || !isValidFrameSlotIndex(slotIndex)) {
        return false;
    }
    if (!isInFlightFenceSignaled(manager.slot(slotIndex))) {
        return true;
    }
    return manager.waitInFlightFence(slotIndex);
}

bool waitInFlightFencesBeforeRecreate(FrameManager& manager, bool waitAllSlots) {
    if (!manager.isReady()) {
        return false;
    }
    if (waitAllSlots) {
        return waitAllInFlightFences(manager);
    }
    return waitCurrentInFlightFence(manager);
}

bool waitInFlightFencesBeforeAcquire(FrameManager& manager) {
    if (!manager.isReady()) {
        return false;
    }
    return waitInFlightFenceIfSignaled(manager, manager.currentIndex());
}

} // namespace fuse::renderer
