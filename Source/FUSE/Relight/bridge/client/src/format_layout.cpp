// FUSE Relight RL-2.2: D3DFORMAT memory layout (see format_layout.hpp).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0). New code; the D3DFORMAT values are the public
// d3d9types.h enumeration.
#include <fuse/relight/bridge/client/format_layout.hpp>

namespace fuse::relight::bridge::client {

FormatLayout formatLayout(uint32_t fmt) noexcept {
    FormatLayout f;
    auto bpp = [&](uint32_t bytes) {
        f.bytesPerBlock = bytes;
        return f;
    };
    auto block = [&](uint32_t bytes) {
        f.blockWidth = 4;
        f.blockHeight = 4;
        f.bytesPerBlock = bytes;
        return f;
    };
    switch (fmt) {
    case 20: return bpp(3);                                  // R8G8B8
    case 21: case 22: case 31: case 32: case 33: case 34:     // A8R8G8B8 X8R8G8B8 A2B10G10R10 A8B8G8R8 X8B8G8R8 G16R16
    case 35: case 62: case 63: case 64: case 67:              // A2R10G10B10 X8L8V8U8 Q8W8V8U8 V16U16 A2W10V10U10
    case 71: case 75: case 77: case 79:                       // D32 D24S8 D24X8 D24X4S4
    case 82: case 83: case 84: case 112: case 114: case 119:  // D32F_LOCKABLE D24FS8 D32_LOCKABLE G16R16F R32F A2B10G10R10_XR_BIAS
        return bpp(4);
    case 23: case 24: case 25: case 26: case 29: case 30:     // R5G6B5 X1R5G5B5 A1R5G5B5 A4R4G4B4 A8R3G3B2 X4R4G4B4
    case 40: case 51: case 60: case 61:                       // A8P8 A8L8 V8U8 L6V5U5
    case 70: case 73: case 80: case 81: case 111: case 117:   // D16_LOCKABLE D15S1 D16 L16 R16F CxV8U8
        return bpp(2);
    case 27: case 28: case 41: case 50: case 52: case 85:     // R3G3B2 A8 P8 L8 A4L4 S8_LOCKABLE
        return bpp(1);
    case 36: case 110: case 113: case 115:                    // A16B16G16R16 Q16W16V16U16 A16B16G16R16F G32R32F
        return bpp(8);
    case 116:                                                 // A32B32G32R32F
        return bpp(16);
    case 118:                                                 // A1
        f.bitsPerPixel1 = true;
        f.bytesPerBlock = 1;
        return f;
    default:
        break;
    }
    switch (fmt) {
    case fourCC('D', 'X', 'T', '1'):
    case fourCC('A', 'T', 'I', '1'):
        return block(8);
    case fourCC('D', 'X', 'T', '2'):
    case fourCC('D', 'X', 'T', '3'):
    case fourCC('D', 'X', 'T', '4'):
    case fourCC('D', 'X', 'T', '5'):
    case fourCC('A', 'T', 'I', '2'):
        return block(16);
    case fourCC('U', 'Y', 'V', 'Y'):
    case fourCC('Y', 'U', 'Y', '2'):
    case fourCC('R', 'G', 'B', 'G'):  // D3DFMT_R8G8_B8G8
    case fourCC('G', 'R', 'G', 'B'):  // D3DFMT_G8R8_G8B8
        f.blockWidth = 2;
        f.bytesPerBlock = 4;
        return f;
    case fourCC('N', 'V', '1', '2'):
    case fourCC('Y', 'V', '1', '2'):
        f.planar420 = true;
        f.bytesPerBlock = 1;
        return f;
    case fourCC('I', 'N', 'T', 'Z'):
    case fourCC('D', 'F', '2', '4'):
    case fourCC('R', 'A', 'W', 'Z'):
        return bpp(4);
    case fourCC('D', 'F', '1', '6'):
        return bpp(2);
    case fourCC('N', 'U', 'L', 'L'):
        return bpp(4);
    default:
        break;
    }
    f.known = false;
    return f;
}

uint32_t rowPitch(const FormatLayout& f, uint32_t width) noexcept {
    if (f.bitsPerPixel1) {
        return (width + 7) / 8;
    }
    return ((width + f.blockWidth - 1) / f.blockWidth) * f.bytesPerBlock;
}

uint32_t rowCount(const FormatLayout& f, uint32_t height) noexcept {
    if (f.planar420) {
        return height + (height + 1) / 2;
    }
    return (height + f.blockHeight - 1) / f.blockHeight;
}

size_t subresourceBytes(const FormatLayout& f, uint32_t width, uint32_t height, uint32_t depth) noexcept {
    return size_t(rowPitch(f, width)) * rowCount(f, height) * (depth == 0 ? 1 : depth);
}

bool regionLayout(const FormatLayout& f, uint32_t width, uint32_t height, uint32_t depth, const Region& r,
                  RegionLayout& out) noexcept {
    if (depth == 0) {
        depth = 1;
    }
    if (r.left >= r.right || r.top >= r.bottom || r.front >= r.back || r.right > width || r.bottom > height ||
        r.back > depth) {
        return false;
    }
    out.pitch = rowPitch(f, width);
    out.slicePitch = size_t(out.pitch) * rowCount(f, height);
    out.slices = r.back - r.front;
    if (f.planar420 || f.bitsPerPixel1) {
        // Only whole-subresource (per slice) access is meaningful for these layouts.
        if (r.left != 0 || r.top != 0 || r.right != width || r.bottom != height) {
            return false;
        }
        out.offset = out.slicePitch * r.front;
        out.rowBytes = out.pitch;
        out.rows = rowCount(f, height);
        return true;
    }
    const bool alignedX = r.left % f.blockWidth == 0 && (r.right % f.blockWidth == 0 || r.right == width);
    const bool alignedY = r.top % f.blockHeight == 0 && (r.bottom % f.blockHeight == 0 || r.bottom == height);
    if (!alignedX || !alignedY) {
        return false;
    }
    const uint32_t bx0 = r.left / f.blockWidth;
    const uint32_t bx1 = (r.right + f.blockWidth - 1) / f.blockWidth;
    const uint32_t by0 = r.top / f.blockHeight;
    const uint32_t by1 = (r.bottom + f.blockHeight - 1) / f.blockHeight;
    out.offset = out.slicePitch * r.front + size_t(by0) * out.pitch + size_t(bx0) * f.bytesPerBlock;
    out.rowBytes = (bx1 - bx0) * f.bytesPerBlock;
    out.rows = by1 - by0;
    return true;
}

}  // namespace fuse::relight::bridge::client
