// Engine probe smoke wrappers — linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "platform/platform.h"
#include "core/util/fourcc.h"
#include "core/util/hashFunction.h"
#include "core/util/swizzle.h"
#include "core/stream/fileStream.h"
#include "core/stream/memStream.h"
#include "core/util/tSignal.h"
#include "core/util/timeClass.h"
#include "gfx/bitmap/loaders/ies/ies_loader.h"
#include "core/util/md5.h"

#include <cstdio>
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

u32 fourccSmoke() {
    return MakeFourCC('F', 'U', 'S', 'E');
}

bool memStreamRoundTripSmoke() {
    MemStream stream(256, true, true);
    const char payload[] = "fuse_u2_memstream";
    if (!stream.write(static_cast<U32>(sizeof(payload)), payload)) {
        return false;
    }
    stream.setPosition(0);
    char out[sizeof(payload)] = {};
    if (!stream.read(sizeof(payload), out)) {
        return false;
    }
    return std::memcmp(payload, out, sizeof(payload)) == 0;
}

bool fileStreamTempRoundTripSmoke() {
    const char payload[] = "fuse_u2_filestream";
    const String path("/tmp/fuse_u2_filestream_probe.bin");
    {
        FileStream writer;
        if (!writer.open(path, Torque::FS::File::Write)) {
            return false;
        }
        if (!writer.write(static_cast<U32>(sizeof(payload)), payload)) {
            return false;
        }
        writer.close();
    }

    FileStream reader;
    if (!reader.open(path, Torque::FS::File::Read)) {
        return false;
    }
    char out[sizeof(payload)] = {};
    if (!reader.read(sizeof(payload), out)) {
        return false;
    }
    reader.close();
    std::remove(path.c_str());
    return std::memcmp(payload, out, sizeof(payload)) == 0;
}

bool timeClassSmoke() {
    Torque::Time t(2020, 1, 15, 12, 30, 45, 0);
    S32 year = 0;
    S32 month = 0;
    S32 day = 0;
    t.get(&year, &month, &day, nullptr, nullptr, nullptr, nullptr);
    return year == 2020 && month == 1 && day == 15 && t.getSeconds() > 0;
}

namespace {
int g_signalSmokeCount = 0;
void signalSmokeIncrement() {
    ++g_signalSmokeCount;
}
} // namespace

bool signalSmoke() {
    Signal<void()> sig;
    g_signalSmokeCount = 0;
    sig.notify(signalSmokeIncrement);
    sig.trigger();
    return g_signalSmokeCount == 1 && !sig.isEmpty();
}

} // namespace fuse::legacy::t3d::engineProbe
