// Engine probe smoke wrappers — linked when FUSE_T3D_LEGACY_ENGINE_PROBE=ON.
#include <fuse/legacy/t3d/api.hpp>

#include "platform/platform.h"
#include "core/util/fourcc.h"
#include "core/util/hashFunction.h"
#include "core/util/swizzle.h"
#include "core/filterStream.h"
#include "core/stream/fileStream.h"
#include "core/stream/memStream.h"
#include "core/util/byteBuffer.h"
#include "core/strings/stringUnit.h"
#include "core/bitVector.h"
#include "core/crc.h"
#include "core/idGenerator.h"
#include "core/util/tSignal.h"
#include "core/util/timeClass.h"
#include "gfx/bitmap/loaders/ies/ies_loader.h"
#include "core/util/md5.h"
#include "core/dataChunker.h"
#include "core/frameAllocator.h"
#include "core/stringTable.h"
#include "core/resizeStream.h"
#include "core/tagDictionary.h"
#include "core/tokenizer.h"
#include "core/strings/findMatch.h"
#include "core/util/rgb2xyz.h"
#include "core/util/rgb2luv.h"
#include "console/consoleObject.h"
#include "core/stream/bitStream.h"
#include "core/stringBuffer.h"
#include "core/util/uuid.h"
#include "core/threadStatic.h"
#include "core/bitRender.h"

#include <cstdio>
#include <cstring>

namespace {

DITTS(U32, gThreadStaticProbeCounter, 7u);

} // namespace

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

bool crcSmoke() {
    const char payload[] = "fuse_u2_crc";
    const U32 crc = CRC::calculateCRC(payload, static_cast<S32>(sizeof(payload) - 1));
    return crc != CRC::INITIAL_CRC_VALUE && crc != CRC::INVALID_CRC;
}

bool idGeneratorSmoke() {
    IdGenerator gen(100, 10);
    const U32 id1 = gen.alloc();
    gen.free(id1);
    const U32 id2 = gen.alloc();
    return id1 == id2;
}

bool bitVectorSmoke() {
    BitVector bits;
    bits.setSize(8);
    bits.clear();
    bits.set(3);
    return bits.test(3) && !bits.test(0);
}

bool dataChunkerSmoke() {
    DataChunker chunker;
    void* a = chunker.alloc(64);
    void* b = chunker.alloc(128);
    if (!a || !b) {
        return false;
    }
    const bool managed = chunker.isManagedByChunker(a) && chunker.isManagedByChunker(b);
    const dsize_t used = chunker.countUsedBytes();
    chunker.freeBlocks(false);
    return managed && used >= 192;
}

bool resizeFilterStreamSmoke() {
    const char payload[] = "fuse_u2_resize_stream";
    MemStream mem(static_cast<U32>(sizeof(payload)), true, true);
    if (!mem.write(static_cast<U32>(sizeof(payload)), payload)) {
        return false;
    }
    mem.setPosition(0);

    ResizeFilterStream resize;
    if (!resize.attachStream(&mem)) {
        return false;
    }
    if (!resize.setStreamOffset(8, 6)) {
        return false;
    }

    char out[7] = {};
    if (!resize.read(6, out)) {
        return false;
    }
    return std::strncmp(out, "resize", 6) == 0 && resize.getLastBytesRead() == 6u;
}

bool filterStreamDelegateSmoke() {
    const char payload[] = "fuse_u2_filter_delegate";
    MemStream mem(static_cast<U32>(sizeof(payload)), true, true);
    if (!mem.write(static_cast<U32>(sizeof(payload)), payload)) {
        return false;
    }
    mem.setPosition(0);

    ResizeFilterStream filter;
    if (!filter.attachStream(&mem)) {
        return false;
    }
    if (!filter.setStreamOffset(8, 8)) {
        return false;
    }
    if (filter.getStreamSize() != 8u) {
        return false;
    }
    if (!filter.hasCapability(Stream::StreamRead)) {
        return false;
    }
    if (!filter.setPosition(2)) {
        return false;
    }
    if (filter.getPosition() != 2u) {
        return false;
    }
    if (!filter.setPosition(0)) {
        return false;
    }

    char out[4] = {};
    if (!filter.read(3, out)) {
        return false;
    }
    if (std::strncmp(out, "fil", 3) != 0) {
        return false;
    }
    if (filter.getPosition() != 3u) {
        return false;
    }

    filter.detachStream();
    if (filter.getStream() != nullptr) {
        return false;
    }
    return filter.getStatus() == Stream::Closed;
}

bool streamTypedRoundTripSmoke() {
    MemStream stream(32, true, true);
    const U32 written = 0xDEADBEEFu;
    const F32 writtenFloat = 3.25f;
    if (!stream.write(written) || !stream.write(writtenFloat)) {
        return false;
    }
    stream.setPosition(0);

    U32 readU32 = 0;
    F32 readFloat = 0.0f;
    if (!stream.read(&readU32) || !stream.read(&readFloat)) {
        return false;
    }
    return readU32 == written && readFloat > 3.24f && readFloat < 3.26f;
}

bool byteBufferSmoke() {
    const U8 chunkA[] = {1, 2, 3};
    const U8 chunkB[] = {4, 5};

    Torque::ByteBuffer buffer;
    buffer.appendBuffer(chunkA, static_cast<U32>(sizeof(chunkA)));
    buffer.appendBuffer(chunkB, static_cast<U32>(sizeof(chunkB)));
    if (buffer.getBufferSize() != 5u) {
        return false;
    }

    Torque::ByteBuffer shared = buffer;
    if (shared.getBuffer() != buffer.getBuffer()) {
        return false;
    }

    Torque::ByteBuffer copy = buffer.getCopy();
    if (copy.getBufferSize() != 5u) {
        return false;
    }
    if (copy.getBuffer() == buffer.getBuffer()) {
        return false;
    }
    return copy.getBuffer()[0] == 1 && copy.getBuffer()[4] == 5;
}

bool stringUnitSmoke() {
    static const char kFields[] = "alpha beta gamma";
    if (StringUnit::getUnitCount(kFields, " ") != 3u) {
        return false;
    }
    const char* second = StringUnit::getUnit(kFields, 1, " ");
    if (second == nullptr || std::strcmp(second, "beta") != 0) {
        return false;
    }
    const char* range = StringUnit::getUnits(kFields, 0, 1, " ");
    return range != nullptr && std::strcmp(range, "alpha beta") == 0;
}

bool tagDictionarySmoke() {
    static const char kDefine[] = "TAG_FUSE_PROBE";
    static const char kLabel[] = "FUSE probe tag";

    // Stable string-literal pointers — TagDictionary compares StringTableEntry by address.
    const StringTableEntry define = kDefine;
    const StringTableEntry label = kLabel;

    TagDictionary dict;
    constexpr S32 kTagId = 43;
    const bool added = dict.addEntry(kTagId, define, label);
    return added && dict.defineToId(define) == kTagId && dict.idToDefine(kTagId) == define;
}

bool findMatchSmoke() {
    if (!FindMatch::isMatch("*.bmp", "textures/foo.bmp", false)) {
        return false;
    }
    if (FindMatch::isMatch("*.png", "textures/foo.bmp", false)) {
        return false;
    }

    FindMatch matcher("test_?.dat", 8);
    return matcher.findMatch("test_1.dat") && matcher.numMatches() == 1;
}

bool tokenizerSmoke() {
    static const char kBuffer[] = "alpha beta gamma";
    Tokenizer tokenizer;
    tokenizer.setBuffer(kBuffer, static_cast<U32>(std::strlen(kBuffer)));

    const char* first = tokenizer.getNextToken();
    if (first == nullptr || std::strcmp(first, "alpha") != 0) {
        return false;
    }
    const char* second = tokenizer.getNextToken();
    if (second == nullptr || std::strcmp(second, "beta") != 0) {
        return false;
    }
    const char* third = tokenizer.getNextToken();
    return third != nullptr && std::strcmp(third, "gamma") == 0;
}

bool rgb2xyzSmoke() {
    const LinearColorF white(1.0f, 1.0f, 1.0f, 1.0f);
    const LinearColorF xyz = ConvertRGB::toXYZ(white);
    if (xyz.green < 0.99f || xyz.green > 1.01f) {
        return false;
    }

    const LinearColorF roundTrip = ConvertRGB::fromXYZ(xyz);
    return roundTrip.red > 0.99f && roundTrip.red < 1.01f && roundTrip.green > 0.99f &&
           roundTrip.green < 1.01f && roundTrip.blue > 0.99f && roundTrip.blue < 1.01f;
}

bool rgb2luvSmoke() {
    const LinearColorF white(1.0f, 1.0f, 1.0f, 1.0f);
    const LinearColorF luv = ConvertRGB::toLUV(white);
    if (luv.blue < 0.99f || luv.blue > 1.01f) {
        return false;
    }

    const LinearColorF scaled = ConvertRGB::toLUVScaled(white);
    return scaled.red > luv.red && scaled.green > luv.green && scaled.blue > 0.99f && scaled.blue < 1.01f;
}

bool bitStreamRoundTripSmoke() {
    U8 buffer[16] = {};
    BitStream stream(buffer, static_cast<S32>(sizeof(buffer)), static_cast<S32>(sizeof(buffer)));
    stream.write(static_cast<U32>(0xA5A5A5A5u));
    stream.setPosition(0);

    U32 value = 0;
    if (!stream.read(&value)) {
        return false;
    }
    return value == 0xA5A5A5A5u;
}

bool bitStreamClassIdSmoke() {
    U8 buffer[32] = {};
    BitStream stream(buffer, static_cast<S32>(sizeof(buffer)), static_cast<S32>(sizeof(buffer)));

    constexpr U32 kClassId = 7u;
    stream.writeClassId(kClassId, NetClassTypeObject, NetClassGroupGame);
    stream.setPosition(0);

    const S32 read = stream.readClassId(NetClassTypeObject, NetClassGroupGame);
    return read == static_cast<S32>(kClassId);
}

bool bitStreamHuffmanStringSmoke() {
    U8 buffer[256] = {};
    BitStream stream(buffer, static_cast<S32>(sizeof(buffer)), static_cast<S32>(sizeof(buffer)));

    static const char kPayload[] = "fuse_u2_huffman";
    stream.writeString(kPayload, 255);
    stream.setPosition(0);

    char out[256] = {};
    stream.readString(out);
    return std::strcmp(out, kPayload) == 0;
}

bool bitStreamStringBufferSmoke() {
    U8 buffer[512] = {};
    char shared[256] = {};

    static const char kPrefix[] = "fuse_u2_prefix_";
    static const char kExtended[] = "fuse_u2_prefix_suffix";
    std::strncpy(shared, kPrefix, sizeof(shared) - 1);
    shared[sizeof(shared) - 1] = '\0';

    BitStream writer(buffer, static_cast<S32>(sizeof(buffer)), static_cast<S32>(sizeof(buffer)));
    writer.setStringBuffer(shared);
    writer.writeString(kExtended, 255);
    const U32 encodedBytes = writer.getPosition();

    BitStream reader(buffer, static_cast<S32>(encodedBytes), static_cast<S32>(encodedBytes));
    reader.setStringBuffer(shared);
    char readOut[256] = {};
    reader.readString(readOut);
    return std::strcmp(readOut, kExtended) == 0;
}

bool stringStartsEndsSmoke() {
    const String path("textures/foo.bmp");
    if (!path.startsWith("textures/")) {
        return false;
    }
    if (path.startsWith("audio/")) {
        return false;
    }
    return path.endsWith(".bmp") && !path.endsWith(".png");
}

bool uuidRoundTripSmoke() {
    Torque::UUID generated;
    generated.generate();
    if (generated.isNull()) {
        return false;
    }

    const String text = generated.toString();
    if (text.length() != 36u) {
        return false;
    }

    Torque::UUID parsed;
    if (!parsed.fromString(text.c_str())) {
        return false;
    }
    return parsed == generated;
}

bool stringBufferUtf8Smoke() {
    FrameAllocator::init(4 * 1024 * 1024);

    StringBuffer buffer;
    buffer.set("fuse_u2");
    buffer.append("_probe");

    const UTF8* utf8 = buffer.getPtr8();
    const bool ok =
        utf8 != nullptr && std::strcmp(utf8, "fuse_u2_probe") == 0 && buffer.length() == 13u;
    FrameAllocator::destroy();
    return ok;
}

bool threadStaticSmoke() {
    ATTS(gThreadStaticProbeCounter) = 11u;
    if (ATTS(gThreadStaticProbeCounter) != 11u) {
        return false;
    }
    ATTS(gThreadStaticProbeCounter) = 7u;
    return ATTS(gThreadStaticProbeCounter) == 7u;
}

bool bitRenderTriangleSmoke() {
    constexpr S32 dim = 8;
    U32 bits[(dim * dim + 31) / 32] = {};
    const Point2I v0(1, 1);
    const Point2I v1(6, 1);
    const Point2I v2(3, 6);
    BitRender::render(&v0, &v1, &v2, dim, bits);

    U32 sum = 0;
    for (const U32 word : bits) {
        sum += word;
    }
    return sum != 0u;
}

} // namespace fuse::legacy::t3d::engineProbe
