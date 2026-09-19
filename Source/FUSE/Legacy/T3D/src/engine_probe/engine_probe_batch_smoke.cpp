// Engine probe smoke wrappers — linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "platform/platform.h"
#include "core/util/hashFunction.h"
#include "core/util/swizzle.h"
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

u32 hash32Smoke(const char* text) {
    const auto len = static_cast<U32>(std::strlen(text));
    return Torque::hash(reinterpret_cast<const U8*>(text), len, 0);
}

u64 hash64Smoke(const char* text) {
    const auto len = static_cast<U32>(std::strlen(text));
    return Torque::hash64(reinterpret_cast<const U8*>(text), len, 0);
}

const char* stringHash64Smoke(const char* text) {
    static String cached;
    cached = Torque::getStringHash64(String(text));
    return cached.c_str();
}

bool swizzleBgraSmoke() {
    const U8 src[4] = {0, 1, 2, 3};
    U8 dst[4] = {};
    Swizzles::bgra.ToBuffer(dst, src, 4);
    return dst[0] == 2 && dst[1] == 1 && dst[2] == 0 && dst[3] == 3;
}

} // namespace fuse::legacy::t3d::engineProbe
