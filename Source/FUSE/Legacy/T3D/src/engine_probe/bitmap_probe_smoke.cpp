// Engine probe smoke wrapper — only linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "platform/types.h"

extern void bitmapExtrude5551_c(const void* srcMip, void* mip, U32 srcHeight, U32 srcWidth);

namespace fuse::legacy::t3d::engineProbe {

void bitmapExtrude5551Smoke(const void* srcMip, void* mip, u32 srcHeight, u32 srcWidth) {
    bitmapExtrude5551_c(srcMip, mip, srcHeight, srcWidth);
}

} // namespace fuse::legacy::t3d::engineProbe
