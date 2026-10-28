#pragma once

namespace fuse::platform {

/// True when `bucket` is in the inclusive range 0..4.
[[nodiscard]] inline bool dpi_bucket_ok(int bucket) {
    return bucket >= 0 && bucket <= 4;
}

} // namespace fuse::platform
