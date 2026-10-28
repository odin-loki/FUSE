#pragma once

namespace fuse::editor {

/// Closest toolbar icon edge in {16, 24, 32}. An equal distance takes the larger size.
/// `requested <= 0` returns 16.
[[nodiscard]] inline int snap_icon_size(int requested) {
    if (requested <= 0) {
        return 16;
    }
    // Ascending, so `dist <= bestDist` keeps the larger size on a tie.
    constexpr int kSizes[] = {16, 24, 32};
    int best = kSizes[0];
    int bestDist = requested >= best ? requested - best : best - requested;
    for (const int size : kSizes) {
        const int dist = requested >= size ? requested - size : size - requested;
        if (dist <= bestDist) {
            best = size;
            bestDist = dist;
        }
    }
    return best;
}

} // namespace fuse::editor
