#pragma once

#include <cstdint>

namespace fuse::renderer {

inline std::uint32_t align_row_pitch(std::uint32_t widthBytes, std::uint32_t alignment) {
    if (widthBytes == 0) {
        return 0;
    }
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0u) {
        return widthBytes;
    }
    const std::uint32_t mask = alignment - 1u;
    if (widthBytes > 0xffffffffu - mask) {
        return 0xffffffffu & ~mask;
    }
    return (widthBytes + mask) & ~mask;
}

inline std::uint64_t upload_bytes(std::uint32_t rowPitch, std::uint32_t height) {
    if (rowPitch == 0 || height == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(rowPitch) * static_cast<std::uint64_t>(height);
}

} // namespace fuse::renderer
