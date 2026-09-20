// Engine probe smoke wrapper — only linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "platform/platform.h"
#include "core/color.h"
#include "core/frameAllocator.h"
#include "core/stream/fileStream.h"
#include "core/stream/memStream.h"
#include "core/util/path.h"
#include "gfx/bitmap/bitmapUtils.h"
#include "gfx/bitmap/gBitmap.h"
#include "math/mRect.h"
#include "platform/types.h"

#include <cstdio>
#include <cstring>

extern void bitmapExtrude5551_c(const void* srcMip, void* mip, U32 srcHeight, U32 srcWidth);

namespace fuse::legacy::t3d::engineProbe {

void bitmapStbRegisterAnchor();
#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
void bitmapPngRegisterAnchor();
#endif

void bitmapExtrude5551Smoke(const void* srcMip, void* mip, u32 srcHeight, u32 srcWidth) {
    bitmapExtrude5551_c(srcMip, mip, srcHeight, srcWidth);
}

void bitmapConvertRGB5551Smoke(u8* rgb, u32 pixels) {
    bitmapConvertRGB_to_5551(rgb, pixels);
}

float convertHalfFloatSmoke(u16 half) {
    return convertHalfToFloat(half);
}

bool bitmapStbMemoryLoadSmoke() {
    bitmapStbRegisterAnchor();

    static const U8 kBmp1x1[] = {
        0x42, 0x4D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    };
    const U32 len = static_cast<U32>(sizeof(kBmp1x1));

    MemStream stream(len, true, true);
    if (!stream.write(len, kBmp1x1)) {
        return false;
    }
    stream.setPosition(0);

    GBitmap bitmap;
    if (!bitmap.readBitmapStream(String("bmp"), stream, len)) {
        return false;
    }
    return bitmap.getWidth() == 1u && bitmap.getHeight() == 1u && bitmap.getByteSize() > 0u;
}

bool readBitmapRejectsUnknownSmoke() {
    MemStream stream(16, true, true);
    GBitmap bitmap;
    return !bitmap.readBitmapStream(String("unknown_fmt"), stream, 0u);
}

bool writeBitmapRejectsUnknownSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(1, 1, false, GFXFormatR8G8B8A8);
    MemStream stream(64, true, true);
    return !bitmap.writeBitmapStream(String("unknown_fmt"), stream);
}

bool writeBitmapStreamRoundTripSmoke() {
    bitmapStbRegisterAnchor();

    GBitmap bitmap;
    bitmap.allocateBitmap(2, 2, false, GFXFormatR8G8B8);
    U8* bits = bitmap.getWritableBits();
    for (U32 i = 0; i < 12; ++i) {
        bits[i] = static_cast<U8>(i);
    }

    // STB stream writer prefixes chunks (stbiWriteFunc) — exercise encode only.
    MemStream writer(256, true, true);
    if (!bitmap.writeBitmapStream(String("tga"), writer)) {
        return false;
    }
    return writer.getPosition() > 0u;
}

bool writeBitmapPathSmoke() {
#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
    bitmapPngRegisterAnchor();
    FrameAllocator::init(4 * 1024 * 1024);

    GBitmap bitmap;
    bitmap.allocateBitmap(1, 1, false, GFXFormatR8G8B8A8);
    U8* bits = bitmap.getWritableBits();
    bits[0] = 0xAA;
    bits[1] = 0xBB;
    bits[2] = 0xCC;
    bits[3] = 0xFF;

    const String path("/tmp/fuse_u2_writebitmap_probe.png");
    const bool wrote = bitmap.writeBitmap(String("png"), Torque::Path(path), 1u);
    GBitmap loaded;
    const bool ok =
        wrote && loaded.readBitmap(String("png"), Torque::Path(path)) && loaded.getWidth() == 1u &&
        loaded.getHeight() == 1u;
    std::remove(path.c_str());
    FrameAllocator::destroy();
    return ok;
#else
    bitmapStbRegisterAnchor();

    GBitmap bitmap;
    bitmap.allocateBitmap(1, 1, false, GFXFormatR8G8B8);
    U8* bits = bitmap.getWritableBits();
    bits[0] = 0xAA;
    bits[1] = 0xBB;
    bits[2] = 0xCC;

    const String path("/tmp/fuse_u2_writebitmap_stb_probe.bmp");
    const Torque::Path torquePath(path);
    const bool wrote = bitmap.writeBitmap(String("bmp"), torquePath, 1u);
    GBitmap loaded;
    const bool ok = wrote && torquePath.getFullPath().isNotEmpty() &&
                    loaded.readBitmap(String("bmp"), torquePath) && loaded.getWidth() == 1u &&
                    loaded.getHeight() == 1u && loaded.getByteSize() > 0u;
    std::remove(path.c_str());
    return ok;
#endif
}

#if defined(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
bool writeBitmapPngRoundTripSmoke() {
    bitmapPngRegisterAnchor();

    FrameAllocator::init(4 * 1024 * 1024);

    GBitmap bitmap;
    bitmap.allocateBitmap(2, 2, false, GFXFormatR8G8B8A8);
    U8* bits = bitmap.getWritableBits();
    for (U32 i = 0; i < 16; ++i) {
        bits[i] = static_cast<U8>(0x10 + i);
    }

    MemStream writer(4096, true, true);
    if (!bitmap.writeBitmapStream(String("png"), writer, 1u)) {
        FrameAllocator::destroy();
        return false;
    }
    const U32 len = writer.getPosition();
    writer.setPosition(0);

    GBitmap loaded;
    const bool ok =
        loaded.readBitmapStream(String("png"), writer, len) && loaded.getWidth() == 2u && loaded.getHeight() == 2u;
    FrameAllocator::destroy();
    return ok;
}
#endif

bool readBitmapPathSmoke() {
    bitmapStbRegisterAnchor();

    static const U8 kBmp1x1[] = {
        0x42, 0x4D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    };
    const String path("/tmp/fuse_u2_readbitmap_probe.bmp");
    {
        FileStream writer;
        if (!writer.open(path, Torque::FS::File::Write)) {
            return false;
        }
        if (!writer.write(static_cast<U32>(sizeof(kBmp1x1)), kBmp1x1)) {
            return false;
        }
        writer.close();
    }

    GBitmap bitmap;
    const bool ok = bitmap.readBitmap(String("bmp"), Torque::Path(path));
    std::remove(path.c_str());
    return ok && bitmap.getWidth() == 1u && bitmap.getHeight() == 1u;
}

bool gbitmapTransparencySmoke() {
    GBitmap opaque;
    opaque.allocateBitmap(2, 2, false, GFXFormatR8G8B8A8);
    U8* bits = opaque.getWritableBits();
    for (U32 i = 0; i < 16; ++i) {
        bits[i] = 255;
    }
    if (opaque.checkForTransparency() || opaque.getHasTransparency()) {
        return false;
    }

    GBitmap transparent;
    transparent.allocateBitmap(1, 1, false, GFXFormatR8G8B8A8);
    U8* alphaBits = transparent.getWritableBits();
    alphaBits[0] = 255;
    alphaBits[1] = 255;
    alphaBits[2] = 255;
    alphaBits[3] = 128;
    return transparent.checkForTransparency() && transparent.getHasTransparency();
}

bool gbitmapFillWhiteSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(2, 2, false, GFXFormatR8G8B8);
    U8* bits = bitmap.getWritableBits();
    dMemset(bits, 0, bitmap.getByteSize());

    bitmap.fillWhite();
    if (bitmap.getHasTransparency()) {
        return false;
    }
    for (U32 i = 0; i < bitmap.getByteSize(); ++i) {
        if (bits[i] != 255) {
            return false;
        }
    }
    return bitmap.getSurfaceSize(0) == 2u * 2u * 3u;
}

bool gbitmapExtensionListSmoke() {
    bitmapStbRegisterAnchor();
    const String extensions = GBitmap::sGetExtensionList();
    return extensions.find("bmp", 0, String::NoCase) != String::NPos;
}

bool gbitmapColorRgba8Smoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(2, 2, false, GFXFormatR8G8B8A8);

    const ColorI written(10, 20, 30, 40);
    if (!bitmap.setColor(1, 0, written)) {
        return false;
    }

    ColorI read;
    if (!bitmap.getColor(1, 0, read)) {
        return false;
    }

    return read.red == 10 && read.green == 20 && read.blue == 30 && read.alpha == 40 &&
           !bitmap.getColor(9, 0, read);
}

bool gbitmapColorRgb8Smoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(2, 2, false, GFXFormatR8G8B8);

    const ColorI written(11, 22, 33, 255);
    if (!bitmap.setColor(0, 1, written)) {
        return false;
    }

    ColorI read;
    if (!bitmap.getColor(0, 1, read)) {
        return false;
    }

    return read.red == 11 && read.green == 22 && read.blue == 33 && read.alpha == 255;
}

bool gbitmapCopyRectSmoke() {
    GBitmap src;
    src.allocateBitmap(4, 4, false, GFXFormatR8G8B8A8);
    const ColorI marker(0xAA, 0x00, 0x00, 0xFF);
    if (!src.setColor(1, 1, marker)) {
        return false;
    }

    GBitmap dst;
    dst.allocateBitmap(4, 4, false, GFXFormatR8G8B8A8);
    dst.copyRect(&src, RectI(1, 1, 1, 1), Point2I(2, 2));

    ColorI sampled;
    return dst.getColor(2, 2, sampled) && sampled.red == 0xAA && sampled.green == 0x00 && sampled.blue == 0x00;
}

bool gbitmapCopyRectRgb8Smoke() {
    GBitmap src;
    src.allocateBitmap(3, 3, false, GFXFormatR8G8B8);
    const ColorI marker(0x11, 0x22, 0x33, 255);
    if (!src.setColor(0, 0, marker)) {
        return false;
    }

    GBitmap dst;
    dst.allocateBitmap(3, 3, false, GFXFormatR8G8B8);
    dst.copyRect(&src, RectI(0, 0, 1, 1), Point2I(2, 1));

    ColorI sampled;
    return dst.getColor(2, 1, sampled) && sampled.red == 0x11 && sampled.green == 0x22 && sampled.blue == 0x33;
}

bool gbitmapExtrudeMipLevelsSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(4, 4, true, GFXFormatR8G8B8A8);

    if (bitmap.getNumMipLevels() != 3u) {
        return false;
    }
    if (bitmap.getWidth(0) != 4u || bitmap.getHeight(0) != 4u) {
        return false;
    }
    if (bitmap.getWidth(1) != 2u || bitmap.getHeight(1) != 2u) {
        return false;
    }
    return bitmap.getWidth(2) == 1u && bitmap.getHeight(2) == 1u && bitmap.getByteSize() > 16u;
}

bool gbitmapSurfaceSizeMipSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(4, 4, true, GFXFormatR8G8B8A8);

    if (bitmap.getSurfaceSize(0) != 4u * 4u * 4u) {
        return false;
    }
    if (bitmap.getSurfaceSize(1) != 2u * 2u * 4u) {
        return false;
    }
    return bitmap.getSurfaceSize(2) == 1u * 1u * 4u;
}

bool gbitmapSurfaceSizeCompressedSmoke() {
    GBitmap bc1;
    bc1.allocateBitmap(8, 8, false, GFXFormatBC1);
    if (bc1.getSurfaceSize(0) != 2u * 2u * 8u) {
        return false;
    }

    GBitmap bc3;
    bc3.allocateBitmap(4, 4, false, GFXFormatBC3);
    return bc3.getSurfaceSize(0) == 1u * 1u * 16u;
}

bool gbitmapDeleteImageSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(2, 2, false, GFXFormatR8G8B8A8);
    if (bitmap.getByteSize() == 0u) {
        return false;
    }

    bitmap.deleteImage();
    if (bitmap.getByteSize() != 0u) {
        return false;
    }

    bitmap.allocateBitmap(1, 1, false, GFXFormatR8G8B8);
    return bitmap.getWidth() == 1u && bitmap.getHeight() == 1u && bitmap.getByteSize() == 3u;
}

bool gbitmapAllocateBitmapWithMipsSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmapWithMips(4, 4, 2, GFXFormatR8G8B8A8, 1);

    if (bitmap.getNumMipLevels() != 2u) {
        return false;
    }
    if (bitmap.getWidth(0) != 4u || bitmap.getHeight(0) != 4u) {
        return false;
    }
    return bitmap.getWidth(1) == 2u && bitmap.getHeight(1) == 2u &&
           bitmap.getSurfaceSize(0) == 4u * 4u * 4u && bitmap.getSurfaceSize(1) == 2u * 2u * 4u;
}

bool gbitmapChopTopMipsSmoke() {
    GBitmap bitmap;
    bitmap.allocateBitmap(4, 4, true, GFXFormatR8G8B8A8);

    const ColorI marker(0x12, 0x34, 0x56, 0xFF);
    if (!bitmap.setColor(0, 0, marker, 1)) {
        return false;
    }

    bitmap.chopTopMips(1);

    if (bitmap.getWidth() != 2u || bitmap.getHeight() != 2u) {
        return false;
    }
    if (bitmap.getNumMipLevels() != 2u) {
        return false;
    }
    if (bitmap.getSurfaceSize(0) != 2u * 2u * 4u) {
        return false;
    }

    ColorI read;
    return bitmap.getColor(0, 0, read) && read.red == 0x12 && read.green == 0x34 && read.blue == 0x56;
}

} // namespace fuse::legacy::t3d::engineProbe
