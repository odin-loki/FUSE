#pragma once

#include <cstdint>

namespace fuse::renderer {

/// Vulkan version word: 10-bit major, 10-bit minor, 12-bit patch.
/// Layout matches VK_MAKE_VERSION: `(major << 22) | (minor << 12) | patch`.
[[nodiscard]] inline std::uint32_t pack_api_version(std::uint32_t major, std::uint32_t minor,
                                                    std::uint32_t patch) {
    const std::uint32_t majorBits = major & 0x3FFu;
    const std::uint32_t minorBits = minor & 0x3FFu;
    const std::uint32_t patchBits = patch & 0xFFFu;
    return (majorBits << 22) | (minorBits << 12) | patchBits;
}

[[nodiscard]] inline std::uint32_t api_version_major(std::uint32_t version) {
    return (version >> 22) & 0x3FFu;
}

[[nodiscard]] inline std::uint32_t api_version_minor(std::uint32_t version) {
    return (version >> 12) & 0x3FFu;
}

[[nodiscard]] inline std::uint32_t api_version_patch(std::uint32_t version) {
    return version & 0xFFFu;
}

} // namespace fuse::renderer
