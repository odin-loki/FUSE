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

/// Returns true when the ring has no signaled in-flight fences.
inline bool allInFlightFencesClear(const FrameManager& manager) {
    return !hasPendingInFlightFences(manager);
}

/// Why a fence wait would be skipped or rejected for a slot (B2.2 deepen).
enum class FenceWaitSkipReason : u8 {
    None = 0,
    ManagerNotReady,
    SlotOutOfRange,
    FenceNotSignaled,
};

const char* fenceWaitSkipReasonLabel(FenceWaitSkipReason reason);

/// Classify whether a slot fence wait is needed, invalid, or already clear.
FenceWaitSkipReason classifyFenceWaitForSlot(const FrameManager& manager, u32 slotIndex);

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

/// Returns true when the slot has submitted work that must be waited before acquire.
inline bool needsInFlightFenceWaitForSlot(const FrameManager& manager, u32 slotIndex) {
    return manager.isReady() && isValidFrameSlotIndex(slotIndex) &&
           isInFlightFenceSignaled(manager.slot(slotIndex));
}

/// Acquire-path fence wait — current slot only; no-op success when already clear.
bool waitInFlightFencesBeforeAcquire(FrameManager& manager);

/// Wait the current slot only when its fence is signaled; no-op success when already clear.
bool waitCurrentInFlightFenceIfSignaled(FrameManager& manager);

} // namespace fuse::renderer
