// FUSE Relight package discovery: major.minor.patch compares. No pre-release and no build metadata.
#pragma once

#include <limits>
#include <optional>
#include <string_view>

namespace fuse::relight::package {

struct SemVer {
    int major;
    int minor;
    int patch;
};

/// "1.2.3" only. A missing component, an extra component, or a pre-release suffix fails.
[[nodiscard]] inline std::optional<SemVer> parse_semver(std::string_view text) noexcept {
    const auto majorDot = text.find('.');
    if (majorDot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto minorDot = text.find('.', majorDot + 1);
    if (minorDot == std::string_view::npos || text.find('.', minorDot + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    const auto component = [](std::string_view token, int& value) noexcept {
        if (token.empty() || (token.size() > 1 && token.front() == '0')) {
            return false;
        }
        int parsed = 0;
        for (const char ch : token) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            const int digit = ch - '0';
            if (parsed > (std::numeric_limits<int>::max() - digit) / 10) {
                return false;
            }
            parsed = parsed * 10 + digit;
        }
        value = parsed;
        return true;
    };

    SemVer version{};
    if (!component(text.substr(0, majorDot), version.major) ||
        !component(text.substr(majorDot + 1, minorDot - majorDot - 1), version.minor) ||
        !component(text.substr(minorDot + 1), version.patch)) {
        return std::nullopt;
    }
    return version;
}

/// -1, 0, or 1 by major, then minor, then patch.
[[nodiscard]] inline int compare_semver(SemVer a, SemVer b) noexcept {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return 0;
}

} // namespace fuse::relight::package
