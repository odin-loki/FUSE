#pragma once

#include <fuse/ai/bt_status.hpp>

namespace fuse::ai {

/// Swap Success and Failure. Running and any other value stay unchanged.
[[nodiscard]] inline BtStatus invert_status(BtStatus status) {
    if (status == BtStatus::Success) {
        return BtStatus::Failure;
    }
    if (status == BtStatus::Failure) {
        return BtStatus::Success;
    }
    return status;
}

} // namespace fuse::ai
