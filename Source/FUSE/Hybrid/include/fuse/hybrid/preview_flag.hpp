#pragma once

namespace fuse::hybrid {

/// True when software preview is forced, or the process is headless.
[[nodiscard]] inline bool software_preview_enabled(bool headless, bool forceSoftware) {
    return forceSoftware || headless;
}

} // namespace fuse::hybrid
