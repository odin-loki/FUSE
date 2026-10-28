#pragma once

namespace fuse::renderer {

/// True only for slots 0 through 1023 inclusive.
[[nodiscard]] inline bool churn_slot_ok(int slot) {
    return slot >= 0 && slot <= 1023;
}

} // namespace fuse::renderer
