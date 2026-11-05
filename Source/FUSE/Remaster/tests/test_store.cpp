// FUSE Remaster W0.2 gate remaster_store_canonical (plan §7 Wave 0, W0.2 exit criteria): identical content under
// different encodings gives the same canonical hash - one image as PNG, TGA (RLE and bottom-up) and BMP; one mesh
// with its attributes in another order, interleaved in two layouts, or with 16-bit indices; one sound as 16-bit
// PCM, WAVE_FORMAT_EXTENSIBLE with extra chunks - and different content gives a different hash. Plus the blob
// store: put is idempotent and atomic, verify catches a corrupted blob, records round-trip through the store.
// Usage: remaster_store_tests <scratch dir>
#include "check.hpp"

#include <fuse/remaster/store.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <filesystem>

namespace st = fuse::remaster::store;
namespace poco = fuse::remaster::poco;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> encode(int (*fn)(stbi_write_func*, void*, int, int, int, const void*), int w, int h, const std::vector<std::uint8_t>& px) {
    std::vector<std::uint8_t> out;
    fn([](void* ctx, void* data, int size) {
        auto* o = static_cast<std::vector<std::uint8_t>*>(ctx);
        o->insert(o->end(), static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
    }, &out, w, h, 4, px.data());
    return out;
}
int writePng(stbi_write_func* f, void* c, int w, int h, int comp, const void* d) { return stbi_write_png_to_func(f, c, w, h, comp, d, w * comp); }
int writeTga(stbi_write_func* f, void* c, int w, int h, int comp, const void* d) { return stbi_write_tga_to_func(f, c, w, h, comp, d); }
int writeBmp(stbi_write_func* f, void* c, int w, int h, int comp, const void* d) { return stbi_write_bmp_to_func(f, c, w, h, comp, d); }

/// An uncompressed, bottom-up TGA written by hand (stb writes top-down RLE by default).
std::vector<std::uint8_t> tgaBottomUp(int w, int h, const std::vector<std::uint8_t>& px) {
    std::vector<std::uint8_t> t(18, 0);
    t[2] = 2; // uncompressed true-colour
    t[12] = static_cast<std::uint8_t>(w), t[13] = static_cast<std::uint8_t>(w >> 8);
    t[14] = static_cast<std::uint8_t>(h), t[15] = static_cast<std::uint8_t>(h >> 8);
    t[16] = 32;
    t[17] = 8; // 8 alpha bits, origin bottom-left
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &px[std::size_t((y * w + x) * 4)];
            t.push_back(p[2]), t.push_back(p[1]), t.push_back(p[0]), t.push_back(p[3]); // BGRA
        }
    }
    return t;
}

void testTextures() {
    const int w = 13, h = 7; // odd sizes: row padding in BMP
    std::vector<std::uint8_t> px(std::size_t(w * h * 4));
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<std::uint8_t>((i * 37 + i / 5) & 0xFF);
    }
    for (std::size_t i = 3; i < px.size(); i += 4) {
        px[i] = 255; // BMP via stb keeps alpha, but keep it opaque to be encoder-neutral
    }
    const std::string expect = st::canonicalTextureSha256(px);
    const auto png = encode(writePng, w, h, px), tga = encode(writeTga, w, h, px), bmp = encode(writeBmp, w, h, px);
    const auto tga2 = tgaBottomUp(w, h, px);
    CHECK(png != tga && tga != tga2 && png != bmp);
    for (const auto* enc : {&png, &tga, &tga2, &bmp}) {
        std::string err;
        const auto img = st::decodeImage(*enc, &err);
        CHECK(img.has_value());
        if (img) {
            CHECK(img->width == std::uint32_t(w) && img->height == std::uint32_t(h));
            CHECK(st::canonicalTextureSha256(*img) == expect);
        }
    }
    // Different content -> different hash.
    auto px2 = px;
    px2[42] ^= 1;
    const auto img2 = st::decodeImage(encode(writePng, w, h, px2));
    CHECK(img2 && st::canonicalTextureSha256(*img2) != expect);
    // Garbage is rejected, not crashed on.
    const std::vector<std::uint8_t> junk = {1, 2, 3, 4, 5};
    std::string err;
    CHECK(!st::decodeImage(junk, &err) && !err.empty());
    CHECK(!st::decodeImage({}, &err));
}

template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
    std::uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
}

void testMeshes() {
    // A quad: positions, normals, uv0, colour.
    const float pos[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, -0.0f}};
    const float nrm[4][3] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const std::uint8_t col[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {9, 9, 9, 9}};
    const std::vector<std::uint32_t> idx = {0, 1, 2, 0, 2, 3};

    auto stream = [&](poco::Semantic s) {
        st::MeshAttribute a{s, poco::Format::F32x3, {}};
        for (int v = 0; v < 4; ++v) {
            if (s == poco::Semantic::Position) {
                for (float f : pos[v]) put(a.data, f);
            } else if (s == poco::Semantic::Normal) {
                for (float f : nrm[v]) put(a.data, f);
            } else if (s == poco::Semantic::Uv0) {
                a.format = poco::Format::F32x2;
                for (float f : uv[v]) put(a.data, f);
            } else {
                a.format = poco::Format::Unorm8x4;
                for (std::uint8_t c : col[v]) a.data.push_back(c);
            }
        }
        return a;
    };
    st::MeshData a;
    a.vertexCount = 4;
    a.attributes = {stream(poco::Semantic::Position), stream(poco::Semantic::Normal), stream(poco::Semantic::Uv0), stream(poco::Semantic::Color0)};
    a.indices = idx;
    st::MeshData b = a;
    std::swap(b.attributes[0], b.attributes[3]); // reordered attributes
    std::swap(b.attributes[1], b.attributes[2]);
    const std::string ha = st::canonicalMeshSha256(a), hb = st::canonicalMeshSha256(b);
    CHECK(!ha.empty() && ha == hb);

    // The same mesh interleaved in two layouts (P N T C vs C T P N, different strides), and with 16-bit indices.
    auto interleave = [&](const std::vector<st::InterleavedAttribute>& layout, std::uint32_t stride) {
        std::vector<std::uint8_t> vb(std::size_t(stride) * 4, 0xCD); // padding bytes must not matter
        for (int v = 0; v < 4; ++v) {
            for (const auto& l : layout) {
                const auto src = stream(l.semantic);
                const std::uint32_t sz = st::formatSize(src.format);
                std::memcpy(&vb[std::size_t(v) * stride + l.offset], &src.data[std::size_t(v) * sz], sz);
            }
        }
        return vb;
    };
    const std::vector<st::InterleavedAttribute> l1 = {{poco::Semantic::Position, poco::Format::F32x3, 0},
                                                      {poco::Semantic::Normal, poco::Format::F32x3, 12},
                                                      {poco::Semantic::Uv0, poco::Format::F32x2, 24},
                                                      {poco::Semantic::Color0, poco::Format::Unorm8x4, 32}};
    const std::vector<st::InterleavedAttribute> l2 = {{poco::Semantic::Color0, poco::Format::Unorm8x4, 0},
                                                      {poco::Semantic::Uv0, poco::Format::F32x2, 4},
                                                      {poco::Semantic::Position, poco::Format::F32x3, 16},
                                                      {poco::Semantic::Normal, poco::Format::F32x3, 28}};
    const auto vb1 = interleave(l1, 36), vb2 = interleave(l2, 48);
    const std::vector<std::uint16_t> idx16(idx.begin(), idx.end());
    const auto m1 = st::meshFromInterleaved(vb1, 36, l1, idx);
    const auto m2 = st::meshFromInterleaved(vb2, 48, l2, {}, idx16);
    CHECK(m1 && m2);
    if (m1 && m2) {
        CHECK(st::canonicalMeshSha256(*m1) == ha);
        CHECK(st::canonicalMeshSha256(*m2) == ha);
    }
    // -0.0 and +0.0 are the same content.
    st::MeshData z = a;
    const float plusZero = 0.f;
    std::memcpy(&z.attributes[0].data[3 * 12 + 8], &plusZero, 4);
    CHECK(st::canonicalMeshSha256(z) == ha);
    // Different content -> different hash: a moved vertex, a swapped index, a different colour, a dropped attribute.
    {
        st::MeshData c = a;
        c.attributes[0].data[0] ^= 1;
        CHECK(st::canonicalMeshSha256(c) != ha);
        c = a;
        std::swap(c.indices[0], c.indices[1]);
        CHECK(st::canonicalMeshSha256(c) != ha);
        c = a;
        c.attributes[3].data[5] ^= 1;
        CHECK(st::canonicalMeshSha256(c) != ha);
        c = a;
        c.attributes.pop_back();
        CHECK(st::canonicalMeshSha256(c) != ha);
    }
    // Positions + indices only reproduce the Relight capture stream (u32 n, u32 m, f32x3 points, u32 indices).
    {
        st::MeshData p;
        p.vertexCount = 4;
        p.attributes = {stream(poco::Semantic::Position)};
        p.indices = idx;
        std::vector<std::uint8_t> relight;
        put(relight, std::uint32_t(4));
        put(relight, std::uint32_t(idx.size()));
        for (int v = 0; v < 4; ++v) {
            for (float f : pos[v]) put(relight, f == 0.f ? 0.f : f);
        }
        for (std::uint32_t i : idx) put(relight, i);
        const auto s = st::canonicalMeshStream(p);
        CHECK(s && *s == relight);
    }
    // Malformed meshes are refused.
    {
        std::string err;
        st::MeshData c = a;
        c.indices.push_back(4);
        CHECK(st::canonicalMeshSha256(c, &err).empty() && !err.empty());
        c = a;
        c.attributes.erase(c.attributes.begin());
        CHECK(st::canonicalMeshSha256(c).empty());
        c = a;
        c.attributes.push_back(stream(poco::Semantic::Normal));
        CHECK(st::canonicalMeshSha256(c).empty());
        c = a;
        c.attributes[1].data.pop_back();
        CHECK(st::canonicalMeshSha256(c).empty());
        CHECK(!st::meshFromInterleaved(vb1, 36, std::vector<st::InterleavedAttribute>{{poco::Semantic::Position, poco::Format::F32x3, 30}}, idx));
    }
}

std::vector<std::uint8_t> wav(std::uint16_t tag, std::uint16_t bits, std::uint16_t ch, std::uint32_t rate,
                              const std::vector<std::uint8_t>& data, bool extensible, bool listChunk) {
    std::vector<std::uint8_t> fmt;
    put(fmt, std::uint16_t(extensible ? 0xFFFE : tag));
    put(fmt, ch);
    put(fmt, rate);
    put(fmt, std::uint32_t(rate * ch * bits / 8));
    put(fmt, std::uint16_t(ch * bits / 8));
    put(fmt, bits);
    if (extensible) {
        put(fmt, std::uint16_t(22));
        put(fmt, bits);
        put(fmt, std::uint32_t(3));
        put(fmt, tag); // sub-format GUID: tag + KSDATAFORMAT base
        const std::uint8_t rest[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.insert(fmt.end(), rest, rest + 14);
    }
    std::vector<std::uint8_t> body = {'W', 'A', 'V', 'E'};
    auto chunk = [&](const char* id, const std::vector<std::uint8_t>& c) {
        body.insert(body.end(), id, id + 4);
        put(body, std::uint32_t(c.size()));
        body.insert(body.end(), c.begin(), c.end());
        if (c.size() & 1) {
            body.push_back(0);
        }
    };
    if (listChunk) {
        chunk("LIST", {'I', 'N', 'F', 'O', 'x'});
    }
    chunk("fmt ", fmt);
    chunk("data", data);
    std::vector<std::uint8_t> out = {'R', 'I', 'F', 'F'};
    put(out, std::uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void testSound() {
    std::vector<std::int16_t> s16;
    for (int i = 0; i < 200; ++i) {
        s16.push_back(static_cast<std::int16_t>((i * 997) % 65536 - 32768));
    }
    std::vector<std::uint8_t> d16, d24, df;
    for (std::int16_t s : s16) {
        put(d16, s);
        const std::int32_t s24 = std::int32_t(s) * 256;
        d24.push_back(static_cast<std::uint8_t>(s24)), d24.push_back(static_cast<std::uint8_t>(s24 >> 8)), d24.push_back(static_cast<std::uint8_t>(s24 >> 16));
        put(df, float(s) / 32768.f);
    }
    const auto a = st::decodeWav(wav(1, 16, 2, 22050, d16, false, false));
    const auto b = st::decodeWav(wav(1, 16, 2, 22050, d16, true, true));   // extensible + LIST chunk
    const auto c = st::decodeWav(wav(1, 24, 2, 22050, d24, false, false));  // 24-bit, same samples
    const auto d = st::decodeWav(wav(3, 32, 2, 22050, df, false, true));    // float, same samples
    CHECK(a && b && c && d);
    if (a && b && c && d) {
        const std::string h = st::canonicalPcmSha256(*a);
        CHECK(a->samples.size() == 200 && a->channels == 2);
        CHECK(st::canonicalPcmSha256(*b) == h);
        CHECK(st::canonicalPcmSha256(*c) == h);
        CHECK(st::canonicalPcmSha256(*d) == h);
        const auto e = st::decodeWav(wav(1, 16, 2, 44100, d16, false, false)); // other rate -> other content
        const auto f = st::decodeWav(wav(1, 16, 1, 22050, d16, false, false)); // mono -> other content
        CHECK(e && st::canonicalPcmSha256(*e) != h);
        CHECK(f && st::canonicalPcmSha256(*f) != h);
    }
    std::vector<std::uint8_t> trunc = wav(1, 16, 2, 22050, d16, false, false);
    trunc.resize(trunc.size() - 10);
    std::string err;
    CHECK(!st::decodeWav(trunc, &err) && !err.empty());
    CHECK(!st::decodeWav(wav(2, 4, 1, 8000, d16, false, false))); // ADPCM: unsupported
}

void testStore(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    st::BlobStore store(dir);
    const std::vector<std::uint8_t> bytes = {'h', 'e', 'l', 'l', 'o'};
    const auto ref = store.put(bytes, "text/plain");
    CHECK(ref && ref->sha256 == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824" && ref->size == 5);
    if (!ref) {
        return;
    }
    CHECK(store.blobPath(ref->sha256) == dir / "blobs" / "sha256" / "2c" / ref->sha256);
    CHECK(store.has(ref->sha256) && store.verify(*ref));
    const auto again = store.put(bytes, "text/plain");
    CHECK(again && *again == *ref && store.list().size() == 1);
    CHECK(!fs::exists(store.blobPath(ref->sha256).string() + ".tmp"));
    // Corruption is caught.
    {
        std::vector<std::uint8_t> bad = bytes;
        bad[0] ^= 1;
        st::writeFileAtomic(store.blobPath(ref->sha256), bad);
        std::string err;
        CHECK(!store.verify(*ref, &err) && err.find("does not hash") != std::string::npos);
        poco::BlobRef wrongSize = *ref;
        wrongSize.size = 6;
        CHECK(!store.verify(wrongSize, &err));
        CHECK(!store.verify(poco::BlobRef{"../../etc", 1, ""}, &err));
        CHECK(!store.get("../x"));
    }
    // Records.
    poco::Record r;
    r.kind = poco::Kind::Sound;
    r.id = "snd/test/beep";
    poco::Sound s;
    s.pcm = store.put(bytes, "audio/wav").value();
    r.payload = s;
    std::string err;
    CHECK(store.writeRecord(r, &err));
    const auto back = store.readRecord("sound", "snd/test/beep", &err);
    CHECK(back && *back == r);
    CHECK(store.listRecords() == std::vector<std::string>{"poco/sound/snd/test/beep.poco.json"});
    poco::Record bad = r;
    bad.provenance.origin = poco::Origin::Original; // distribution ship is too permissive for an original
    CHECK(!store.writeRecord(bad, &err) && !err.empty());
    fs::remove_all(dir, ec);
}

} // namespace

int main(int argc, char** argv) {
    testTextures();
    testMeshes();
    testSound();
    testStore(argc > 1 ? fs::path(argv[1]) : fs::current_path() / "remaster_store_tmp");
    return remaster_test::finish("remaster_store_canonical");
}
