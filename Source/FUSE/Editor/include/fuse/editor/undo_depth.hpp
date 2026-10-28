#pragma once

namespace fuse::editor {

/// True when another undo entry may be pushed.
/// `limit` must be positive and `depth` must lie in `[0, limit)`.
[[nodiscard]] inline bool undo_can_push(int depth, int limit) {
    return limit > 0 && depth >= 0 && depth < limit;
}

/// Oldest entries to drop so `depth` is at most `limit`.
/// Returns 0 when `depth` is negative, `limit` is not positive, or `depth` is already within `limit`.
[[nodiscard]] inline int undo_drop_count(int depth, int limit) {
    if (depth < 0 || limit <= 0 || depth <= limit) {
        return 0;
    }
    return depth - limit;
}

} // namespace fuse::editor
