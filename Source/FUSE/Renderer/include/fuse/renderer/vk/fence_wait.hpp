#pragma once

#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Returns true when the slot has a pending GPU submission (fence must be waited on).
inline bool isInFlightFenceSignaled(const FrameSyncData& slot) {
    return slot.fenceSignaled;
}

/// Wait on one slot's in-flight fence before acquire (B2.2 present path).
bool waitInFlightFenceForSlot(FrameManager& manager, u32 slotIndex);

/// Wait all in-flight slots before swapchain teardown/recreate (resize path).
bool waitAllInFlightFences(FrameManager& manager);

} // namespace fuse::renderer
