#pragma once

namespace fuse::mechanics {

struct TriggerLatch {
    bool inside = false;
    bool fired = false;
};

inline bool trigger_enter(TriggerLatch& latch, bool nowInside) {
    if (!nowInside) {
        latch.inside = false;
        return false;
    }
    if (!latch.inside && !latch.fired) {
        latch.inside = true;
        latch.fired = true;
        return true;
    }
    latch.inside = true;
    return false;
}

} // namespace fuse::mechanics
