#pragma once

namespace fuse::platform {

/// True when `length` is in the closed range 1..128.
[[nodiscard]] inline bool title_len_ok(int length) {
    return length >= 1 && length <= 128;
}

} // namespace fuse::platform
