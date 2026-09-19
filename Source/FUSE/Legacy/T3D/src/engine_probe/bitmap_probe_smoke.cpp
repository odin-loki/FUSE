// Engine probe smoke wrapper — only linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "gfx/bitmap/bitmapUtils.h"
#include "platform/types.h"

extern void bitmapExtrude5551_c(const void* srcMip, void* mip, U32 srcHeight, U32 srcWidth);

namespace fuse::legacy::t3d::engineProbe {

void bitmapExtrude5551Smoke(const void* srcMip, void* mip, u32 srcHeight, u32 srcWidth) {
    bitmapExtrude5551_c(srcMip, mip, srcHeight, srcWidth);
}

void bitmapConvertRGB5551Smoke(u8* rgb, u32 pixels) {
    bitmapConvertRGB_to_5551(rgb, pixels);
}

float convertHalfFloatSmoke(u16 half) {
    return convertHalfToFloat(half);
}

} // namespace fuse::legacy::t3d::engineProbe
