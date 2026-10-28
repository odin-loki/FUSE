#pragma once

namespace fuse::editor {

/// Sibling slot when `siblingCount` is positive and `indexInParent` lies in range.
/// Otherwise -1.
[[nodiscard]] inline int sibling_index(int indexInParent, int siblingCount) {
    if (siblingCount > 0 && indexInParent >= 0 && indexInParent < siblingCount) {
        return indexInParent;
    }
    return -1;
}

/// `index + delta` wrapped into `[0, siblingCount)`. `siblingCount <= 0` returns -1.
/// The remainder is non-negative.
[[nodiscard]] inline int sibling_wrap(int index, int delta, int siblingCount) {
    if (siblingCount <= 0) {
        return -1;
    }
    const long long count = siblingCount;
    long long slot = static_cast<long long>(index) + static_cast<long long>(delta);
    slot %= count;
    if (slot < 0) {
        slot += count;
    }
    return static_cast<int>(slot);
}

} // namespace fuse::editor
