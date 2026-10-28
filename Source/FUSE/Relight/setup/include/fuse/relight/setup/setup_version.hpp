// FUSE Relight setup: accepted setup schema versions.
//
// setup_version_ok() is true for versions 1 through 8 inclusive.
#pragma once

namespace fuse::relight::setup {

/// True when \p version is a supported setup schema version (1 through 8).
inline bool setup_version_ok(int version) {
    return version >= 1 && version <= 8;
}

} // namespace fuse::relight::setup
