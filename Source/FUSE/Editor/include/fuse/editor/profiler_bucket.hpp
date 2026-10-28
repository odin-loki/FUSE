#pragma once

namespace fuse::editor {

/// True when `bucket` is in [0, 15].
[[nodiscard]] inline bool profiler_bucket_ok(int bucket) {
    return bucket >= 0 && bucket <= 15;
}

} // namespace fuse::editor
