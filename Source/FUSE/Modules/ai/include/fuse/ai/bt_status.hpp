#pragma once

namespace fuse::ai {

enum class BtStatus { Success, Failure, Running };

/// Sequence combine: Failure if either child failed, otherwise Running if either is running, otherwise Success.
[[nodiscard]] inline BtStatus sequence_status(BtStatus a, BtStatus b) {
    if (a == BtStatus::Failure || b == BtStatus::Failure) {
        return BtStatus::Failure;
    }
    if (a == BtStatus::Running || b == BtStatus::Running) {
        return BtStatus::Running;
    }
    return BtStatus::Success;
}

} // namespace fuse::ai
