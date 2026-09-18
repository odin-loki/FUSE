#pragma once

#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Returns true when the slot index is within the frame ring.
inline bool isValidFrameSlotIndex(u32 slotIndex) {
    return slotIndex < kFramesInFlight;
}

/// Returns true when the slot has a pending GPU submission (fence must be waited on).
inline bool isInFlightFenceSignaled(const FrameSyncData& slot) {
    return slot.fenceSignaled;
}

/// Count slots with `fenceSignaled` set (submitted work not yet waited).
u32 countPendingInFlightFences(const FrameManager& manager);

/// Returns true when any slot has a pending in-flight fence.
inline bool hasPendingInFlightFences(const FrameManager& manager) {
    return countPendingInFlightFences(manager) > 0;
}

/// Wait on one slot's in-flight fence before acquire (B2.2 present path).
/// Returns false when the manager is not ready or `slotIndex` is out of range.
bool waitInFlightFenceForSlot(FrameManager& manager, u32 slotIndex);

/// Wait the manager's current ring slot (convenience for acquire path).
bool waitCurrentInFlightFence(FrameManager& manager);

/// Wait all in-flight slots before swapchain teardown/recreate (resize path).
bool waitAllInFlightFences(FrameManager& manager);

/// Wait only when the slot fence is signaled; no-op success when already clear.
bool waitInFlightFenceIfSignaled(FrameManager& manager, u32 slotIndex);

/// Preflight: returns false when the manager is not ready or `slotIndex` is out of range.
inline bool canWaitInFlightFenceForSlot(const FrameManager& manager, u32 slotIndex) {
    return manager.isReady() && isValidFrameSlotIndex(slotIndex);
}

/// Wait fences required before swapchain recreate — all slots when GPU path, current when headless.
bool waitInFlightFencesBeforeRecreate(FrameManager& manager, bool waitAllSlots);

} // namespace fuse::renderer
