#pragma once

namespace fuse::adventure {

/// First free slot at or after `preferred`, wrapping once through the ring.
/// Returns -1 when `count <= 0`, `occupied` is null, or every slot is taken.
/// `preferred` is wrapped into `[0, count)` when `count > 0`.
[[nodiscard]] inline int pick_spawn_slot(const bool* occupied, int count, int preferred) {
    if (count <= 0 || occupied == nullptr) {
        return -1;
    }
    const long long span = count;
    long long start = static_cast<long long>(preferred) % span;
    if (start < 0) {
        start += span;
    }
    for (long long step = 0; step < span; ++step) {
        const long long cursor = start + step;
        const int slot = static_cast<int>(cursor < span ? cursor : cursor - span);
        if (!occupied[slot]) {
            return slot;
        }
    }
    return -1;
}

} // namespace fuse::adventure
