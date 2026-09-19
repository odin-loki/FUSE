// Engine probe smoke wrappers — linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "gfx/bitmap/loaders/ies/ies_loader.h"
#include "core/util/md5.h"

#include <cstring>

namespace fuse::legacy::t3d::engineProbe {

bool iesLoadEmptySmoke() {
    IESFileInfo info;
    IESLoadHelper helper;
    return !helper.load("", info) && !info.valid();
}

u32 md5DigestSmoke(const char* text) {
    MD5Context ctx;
    MD5Init(&ctx);
    MD5Update(&ctx, reinterpret_cast<unsigned char*>(const_cast<char*>(text)),
              static_cast<unsigned>(std::strlen(text)));
    unsigned char digest[16] = {};
    MD5Final(digest, &ctx);
    u32 sum = 0;
    for (int i = 0; i < 16; ++i) {
        sum += digest[i];
    }
    return sum;
}

} // namespace fuse::legacy::t3d::engineProbe
