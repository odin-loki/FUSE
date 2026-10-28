#pragma once

namespace fuse::adventure {

/// True when `candidate` is strictly higher than `current`.
/// Equal priority does not replace.
[[nodiscard]] inline bool interact_priority_wins(int candidate, int current) {
    return candidate > current;
}

} // namespace fuse::adventure
