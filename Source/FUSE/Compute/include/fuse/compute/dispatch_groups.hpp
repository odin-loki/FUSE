#pragma once

namespace fuse::compute {

/// Ceiling of `count / group`.
/// Returns 0 when `count` is negative, or when `group` is not a power of two in [1, 256].
/// Zero is not a power of two. Otherwise returns (count + group - 1) / group.
[[nodiscard]] inline int dispatch_groups(int count, int group) {
    if (count < 0 || group < 1 || group > 256 || (group & (group - 1)) != 0) {
        return 0;
    }
    return (count + group - 1) / group;
}

} // namespace fuse::compute
