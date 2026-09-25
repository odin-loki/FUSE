#pragma once

#include <cstdint>

/// Honest ispc_texcomp API stub for U7 cook encoder hooks when the real library is absent.
namespace ispc {

enum class TextureFormat : uint32_t {
    BC7_RGBA = 0,
    BC5_RG = 1,
};

struct CompressionResult {
    bool ok = false;
    uint32_t blockCount = 0;
    const char* note = nullptr;
};

inline uint32_t blockCountForSize(uint32_t width, uint32_t height) {
    const uint32_t blockWidth = (width + 3u) / 4u;
    const uint32_t blockHeight = (height + 3u) / 4u;
    return blockWidth * blockHeight;
}

inline CompressionResult CompressBlocks(const uint8_t* rgba, uint32_t width, uint32_t height,
                                      TextureFormat format, uint8_t* outBlocks) {
    CompressionResult result{};
    if (rgba == nullptr || outBlocks == nullptr || width == 0u || height == 0u) {
        result.note = "invalid compress input";
        return result;
    }

    result.blockCount = blockCountForSize(width, height);
    const uint32_t bytesPerBlock = (format == TextureFormat::BC5_RG) ? 16u : 16u;
    for (uint32_t block = 0; block < result.blockCount; ++block) {
        const uint32_t offset = block * bytesPerBlock;
        outBlocks[offset + 0] = 0x00;
        outBlocks[offset + 1] = 0xFC;
        outBlocks[offset + 2] = static_cast<uint8_t>(format == TextureFormat::BC5_RG ? 0x55 : 0x07);
        for (uint32_t byte = 3; byte < bytesPerBlock; ++byte) {
            outBlocks[offset + byte] = rgba[(block * 4u) % (width * height * 4u)];
        }
    }

    result.ok = true;
    result.note = "ispc_texcomp stub";
    return result;
}

} // namespace ispc
