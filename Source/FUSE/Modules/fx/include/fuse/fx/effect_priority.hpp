#pragma once

namespace fuse::fx {

/// Effect sort rank. A negative priority is non-usable and becomes 0.
/// Values above 100 clamp to 100.
[[nodiscard]] inline int effect_priority_rank(int priority) {
    if (priority < 0) {
        return 0;
    }
    if (priority > 100) {
        return 100;
    }
    return priority;
}

/// True when \p a ranks strictly above \p b after clamping.
[[nodiscard]] inline bool effect_outranks(int a, int b) {
    return effect_priority_rank(a) > effect_priority_rank(b);
}

} // namespace fuse::fx
