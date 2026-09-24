// FUSE Relight RL-2.2: memory layout of D3DFORMAT surfaces as the bridge client stores and ships
// them (tightly packed rows; BC formats in 4x4 blocks).
// Copyright (c) 2026 FUSE contributors (MIT). Replaces dxvk-remix bridge/src/util/
// util_texture_and_volume.h (calcRowSize / calcTotalSizeOfRect / calcImageByteOffset), which covered
// fewer formats and treated every FOURCC as 4 bytes per pixel.
//
// Portable (no Windows headers): the formats are numeric D3DFORMAT values, so the layout is unit
// tested in the Linux tree too.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fuse::relight::bridge::client {

constexpr uint32_t fourCC(char a, char b, char c, char d) noexcept {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

struct FormatLayout {
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
    uint32_t bytesPerBlock = 4;
    bool bitsPerPixel1 = false;  // D3DFMT_A1: 1 bit per pixel, rows padded to bytes
    bool planar420 = false;      // NV12 / YV12: luma rows then half-height chroma rows
    bool known = true;           // false: unknown format, 4 bytes per pixel assumed
};

FormatLayout formatLayout(uint32_t d3dFormat) noexcept;

// Bytes of one row of blocks for `width` pixels.
uint32_t rowPitch(const FormatLayout& f, uint32_t width) noexcept;
// Number of block rows (plus the chroma rows of planar formats) for `height` pixels.
uint32_t rowCount(const FormatLayout& f, uint32_t height) noexcept;

// A rectangular region of a subresource in pixels: [left, right) x [top, bottom) x [front, back).
struct Region {
    uint32_t left = 0, top = 0, right = 0, bottom = 0, front = 0, back = 1;
};

// Where a region lives in a tightly packed subresource of width x height (x depth) pixels.
struct RegionLayout {
    size_t offset = 0;       // byte offset of the region's first block
    uint32_t rowBytes = 0;   // bytes per region row
    uint32_t rows = 0;       // block rows per slice
    uint32_t slices = 1;
    uint32_t pitch = 0;      // subresource row pitch
    size_t slicePitch = 0;   // subresource slice pitch
};

// False if the region is empty, outside the subresource, or not block aligned (BC formats need
// 4-aligned edges except at the subresource border).
bool regionLayout(const FormatLayout& f, uint32_t width, uint32_t height, uint32_t depth, const Region& r,
                  RegionLayout& out) noexcept;

size_t subresourceBytes(const FormatLayout& f, uint32_t width, uint32_t height, uint32_t depth) noexcept;

}  // namespace fuse::relight::bridge::client
