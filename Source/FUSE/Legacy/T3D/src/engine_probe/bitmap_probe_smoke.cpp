// Engine probe smoke wrapper — only linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "core/stream/memStream.h"
#include "core/util/path.h"
#include "gfx/bitmap/bitmapUtils.h"
#include "gfx/bitmap/gBitmap.h"
#include "platform/types.h"

extern void bitmapExtrude5551_c(const void* srcMip, void* mip, U32 srcHeight, U32 srcWidth);

namespace fuse::legacy::t3d::engineProbe {

void bitmapStbRegisterAnchor();

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

    const GBitmap::Registration* reg = GBitmap::sFindRegInfo(String("bmp"));
    if (reg == nullptr || reg->readStreamFunc == nullptr) {
        return false;
    }

    MemStream stream(len, true, true);
    if (!stream.write(len, kBmp1x1)) {
        return false;
    }
    stream.setPosition(0);

    GBitmap bitmap;
    if (!reg->readStreamFunc(stream, &bitmap, len)) {
        return false;
    }
    return bitmap.getWidth() == 1u && bitmap.getHeight() == 1u && bitmap.getByteSize() > 0u;
}

} // namespace fuse::legacy::t3d::engineProbe
