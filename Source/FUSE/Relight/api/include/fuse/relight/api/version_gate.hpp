#pragma once

namespace fuse::relight::api {

// Same major, and the plugin minor is not newer than the host. A different major, or any
// negative component, is incompatible.
[[nodiscard]] inline bool version_compatible(int hostMajor, int hostMinor, int pluginMajor, int pluginMinor) {
    if (hostMajor < 0 || hostMinor < 0 || pluginMajor < 0 || pluginMinor < 0) {
        return false;
    }
    return hostMajor == pluginMajor && pluginMinor <= hostMinor;
}

} // namespace fuse::relight::api
