#pragma once

namespace fuse::platform {

/// True when `code` is in the closed range 1..511.
[[nodiscard]] inline bool key_code_ok(int code) {
    return code >= 1 && code <= 511;
}

} // namespace fuse::platform
