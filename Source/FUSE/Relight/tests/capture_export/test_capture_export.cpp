// FUSE Relight RL-1.8: CPU unit tests of the capture writers (ctest rl_capture_export_unit).
//
//   digest     SHA-256 / SHA-1 FIPS 180 vectors, RFC 4122 UUIDv5 (python uuid values), the oaid namespace;
//   json       round trip, escapes, errors;
//   dds        every D3DFORMAT of the RL-0.4 texture_formats app (plus ATI1 / ATI2) at several extents:
//              canonical mip 0 -> DDS -> canonical is byte-identical and XXH3 gives the texture hash again;
//              BC blocks pass through unchanged; the DDS pitch rule; mip chains; bad files are rejected;
//              the RGBA8 decode of known texels (A8R8G8B8, R5G6B5, L8, A8, DXT1, DXT5);
//   builder    GameCapturer on synthetic RL-1.3 / RL-1.5 / RL-1.7 records: strip / fan -> triangle list,
//              index reduction, instance names and mesh / material dedup, transform samples only on change,
//              the LHS camera (mirror correction, winding swap, light root flip), light colour / intensity,
//              skinning (skeleton, joint primvars), the key set;
//   writer     the capture written to disk parses (minimal USDA parser: TinyUSDZ is Remaster W2.2, not in the
//              tree yet), re-ingests with the identical key set, and is byte-identical when written twice;
//              seeded faults (a flipped blob byte, a renamed mesh, a corrupted DDS, a dropped DB row) are
//              caught by the re-ingest checks.
#include "capture_check.hpp"

#include <fuse/relight/capture/export/capture_builder.hpp>
#include <fuse/relight/capture/export/capture_writer.hpp>
#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/digest.hpp>
#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/capture/export/usda_writer.hpp>
#include <fuse/relight/hash/texture_hash.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

namespace ex = fuse::relight::capture::exporter;
namespace geo = fuse::relight::capture::geometry;
namespace rh = fuse::relight::hash;
namespace sc = fuse::relight::scene;
namespace inst = fuse::relight::scene::instances;
namespace fs = std::filesystem;
using rh::D3DFormat;

// ---- digest ---------------------------------------------------------------------------------------------

void testDigest() {
    CHECK(ex::sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ex::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(ex::sha256Hex(m56, std::strlen(m56)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        ex::Sha256 h; // one million 'a', fed in odd chunks
        const std::string chunk(997, 'a');
        std::size_t left = 1000000;
        while (left) {
            const std::size_t n = std::min(left, chunk.size());
            h.update(chunk.data(), n);
            left -= n;
        }
        CHECK(h.hexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    {
        ex::Sha1 h;
        h.update("abc", 3);
        const auto d = h.digest();
        CHECK(ex::toHex(d.data(), d.size()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    }
    const ex::Uuid dns{0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    CHECK(ex::uuidString(ex::uuidV5(dns, "python.org")) == "886313e1-3b8a-5372-9b90-0c9aee199e5d");
    CHECK(ex::uuidString(ex::uuidV5(ex::uuidNamespaceUrl(), "https://fuse.invalid/remaster/oaid")) ==
          "4837225f-19af-5a78-8665-cedd519c0292");
    CHECK(ex::originalAssetId("game", "remix.tex", "0123456789ABCDEF") == "a7b57772-8c3f-500b-8010-456110b2ae9c");
}

// ---- json -----------------------------------------------------------------------------------------------

void testJson() {
    std::string err;
    const auto v = ex::json::parse(R"({"a":[1,2.5,-3e2,true,false,null],"s":"q\"\\\né","o":{}})", &err);
    CHECK(v.has_value());
    if (v) {
        CHECK(v->get("a")->a.size() == 6);
        CHECK(v->get("a")->a[2].n == -300.0);
        CHECK(v->str("s") == "q\"\\\n\xc3\xa9");
        const auto again = ex::json::parse(ex::json::write(*v));
        CHECK(again && ex::json::write(*again) == ex::json::write(*v));
        const auto pretty = ex::json::parse(ex::json::writePretty(*v));
        CHECK(pretty && ex::json::write(*pretty) == ex::json::write(*v));
    }
    CHECK(!ex::json::parse("{\"a\":}", &err));
    CHECK(!ex::json::parse("[1,2", &err));
    CHECK(!ex::json::parse("{} x", &err));
    CHECK(ex::json::write(ex::json::Value::number(0.1)) == "0.1");
    CHECK(ex::json::write(ex::json::Value::number(4294967295.0)) == "4294967295");
}

// ---- dds ------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> randomCanonical(D3DFormat f, std::uint32_t w, std::uint32_t h, std::mt19937& rng) {
    const rh::TextureMip0Layout l = rh::textureMip0Layout(f, w, h, 1);
    std::vector<std::uint8_t> bytes(l.size, 0);
    const rh::TextureFormatInfo info = rh::textureFormatInfo(f);
    const std::uint64_t used = std::uint64_t(info.elementSize) * l.blocksWide;
    for (std::uint64_t r = 0; r < l.rowCount; ++r) {
        for (std::uint64_t b = 0; b < used; ++b) {
            bytes[r * l.rowBytes + b] = static_cast<std::uint8_t>(rng());
        }
    }
    return bytes;
}

void testDds() {
    const D3DFormat formats[] = {
        D3DFormat::A8R8G8B8, D3DFormat::X8R8G8B8, D3DFormat::A8B8G8R8, D3DFormat::X8B8G8R8, D3DFormat::R5G6B5,
        D3DFormat::X1R5G5B5, D3DFormat::A1R5G5B5, D3DFormat::A4R4G4B4, D3DFormat::X4R4G4B4, D3DFormat::R3G3B2,
        D3DFormat::A8R3G3B2, D3DFormat::L8, D3DFormat::A8L8, D3DFormat::A4L4, D3DFormat::A8, D3DFormat::L16,
        D3DFormat::DXT1, D3DFormat::DXT2, D3DFormat::DXT3, D3DFormat::DXT4, D3DFormat::DXT5, D3DFormat::G16R16,
        D3DFormat::A2B10G10R10, D3DFormat::A2R10G10B10, D3DFormat::A16B16G16R16, D3DFormat::R16F, D3DFormat::G16R16F,
        D3DFormat::A16B16G16R16F, D3DFormat::R32F, D3DFormat::G32R32F, D3DFormat::A32B32G32R32F, D3DFormat::V8U8,
        D3DFormat::L6V5U5, D3DFormat::X8L8V8U8, D3DFormat::Q8W8V8U8, D3DFormat::V16U16, D3DFormat::A2W10V10U10,
        D3DFormat::Q16W16V16U16, D3DFormat::R8G8B8, D3DFormat::ATI1, D3DFormat::ATI2,
    };
    std::mt19937 rng(1234);
    int roundTrips = 0;
    for (const D3DFormat f : formats) {
        const auto pf = ex::ddsPixelFormat(f);
        CHECK(pf.has_value());
        if (!pf) {
            std::fprintf(stderr, "  no DDS pixel format for %u\n", unsigned(f));
            continue;
        }
        CHECK(ex::d3dFormatFromDds(*pf) == f);
        for (const auto& [w, h] : {std::pair{8u, 8u}, std::pair{5u, 3u}, std::pair{1u, 1u}, std::pair{13u, 7u}}) {
            ex::DdsImage img;
            img.format = f;
            img.width = w;
            img.height = h;
            img.mips.push_back(randomCanonical(f, w, h, rng));
            std::string err;
            const std::vector<std::uint8_t> file = ex::writeDds(img, &err);
            CHECK(!file.empty());
            if (file.empty()) {
                std::fprintf(stderr, "  format %u %ux%u: %s\n", unsigned(f), w, h, err.c_str());
                continue;
            }
            CHECK(file.size() == 128 + ex::ddsLevelSize(f, w, h));
            const auto back = ex::readDds(file, &err);
            CHECK(back.has_value());
            if (!back) {
                std::fprintf(stderr, "  format %u %ux%u read: %s\n", unsigned(f), w, h, err.c_str());
                continue;
            }
            CHECK(back->format == f && back->width == w && back->height == h && back->mips.size() == 1);
            CHECK(back->mips[0] == img.mips[0]);
            CHECK(rh::hashTextureMip0(back->mips[0].data(), back->mips[0].size()) ==
                  rh::hashTextureMip0(img.mips[0].data(), img.mips[0].size()));
            // BC passthrough: the payload is the canonical blocks, unchanged.
            if (rh::textureFormatInfo(f).blockWidth == 4) {
                CHECK(std::memcmp(file.data() + 128, img.mips[0].data(), img.mips[0].size()) == 0);
            }
            ++roundTrips;
        }
    }
    CHECK(roundTrips == int(sizeof(formats) / sizeof(formats[0])) * 4);

    // The DDS pitch rule: R8G8B8 rows are 3 * width bytes in the file, align(3 * width, 4) canonically.
    CHECK(ex::ddsLevelSize(D3DFormat::R8G8B8, 5, 2) == 30);
    CHECK(rh::textureMip0Layout(D3DFormat::R8G8B8, 5, 2, 1).size == 32);
    CHECK(ex::ddsLevelSize(D3DFormat::DXT1, 5, 5) == 32);

    // A mip chain.
    {
        ex::DdsImage img;
        img.format = D3DFormat::A8R8G8B8;
        img.width = 8;
        img.height = 4;
        for (std::uint32_t m = 0; m < 4; ++m) {
            img.mips.push_back(randomCanonical(img.format, std::max(1u, 8u >> m), std::max(1u, 4u >> m), rng));
        }
        const auto file = ex::writeDds(img);
        const auto back = ex::readDds(file);
        CHECK(back && back->mips == img.mips);
        img.mips[2].pop_back();
        std::string err;
        CHECK(ex::writeDds(img, &err).empty() && !err.empty());
    }
    // Bad files.
    {
        ex::DdsImage img;
        img.format = D3DFormat::DXT5;
        img.width = img.height = 4;
        img.mips.push_back(std::vector<std::uint8_t>(16, 7));
        std::vector<std::uint8_t> file = ex::writeDds(img);
        std::string err;
        CHECK(!ex::readDds(std::span(file.data(), file.size() - 1), &err));
        std::vector<std::uint8_t> bad = file;
        bad[0] = 'X';
        CHECK(!ex::readDds(bad, &err));
        bad = file;
        bad[4 + 80] = 'D';
        bad[4 + 81] = 'X';
        bad[4 + 82] = '1';
        bad[4 + 83] = '0';
        CHECK(!ex::readDds(bad, &err));
        file.push_back(0);
        CHECK(!ex::readDds(file, &err));
        ex::DdsImage depth;
        depth.format = D3DFormat::D24S8;
        depth.width = depth.height = 4;
        depth.mips.push_back(std::vector<std::uint8_t>(64, 0));
        CHECK(ex::writeDds(depth, &err).empty());
    }
    // RGBA8 decode.
    {
        const std::uint8_t argb[4] = {0x20, 0x40, 0xFF, 0x80}; // 0x80FF4020
        const auto d = ex::decodeRgba8(D3DFormat::A8R8G8B8, 1, 1, argb);
        CHECK(d && (*d)[0] == 0xFF && (*d)[1] == 0x40 && (*d)[2] == 0x20 && (*d)[3] == 0x80);
        const std::uint8_t rgb565[4] = {0xFF, 0xFF, 0, 0}; // canonical row: 2 texel bytes + 2 padding
        const auto w = ex::decodeRgba8(D3DFormat::R5G6B5, 1, 1, rgb565);
        CHECK(w && (*w)[0] == 255 && (*w)[1] == 255 && (*w)[2] == 255 && (*w)[3] == 255);
        const std::uint8_t l8[4] = {0x7F, 0, 0, 0};
        const auto g = ex::decodeRgba8(D3DFormat::L8, 1, 1, l8);
        CHECK(g && (*g)[0] == 0x7F && (*g)[1] == 0x7F && (*g)[2] == 0x7F && (*g)[3] == 255);
        const std::uint8_t a8[4] = {0x33, 0, 0, 0};
        const auto a = ex::decodeRgba8(D3DFormat::A8, 1, 1, a8);
        CHECK(a && (*a)[3] == 0x33 && (*a)[0] == 0);
        // DXT1: c0 = red (0xF800) > c1 = blue (0x001F), every index 0 -> red; index 1 -> blue.
        const std::uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x40};
        const auto r = ex::decodeRgba8(D3DFormat::DXT1, 4, 4, bc1);
        CHECK(r && (*r)[0] == 255 && (*r)[1] == 0 && (*r)[2] == 0 && (*r)[3] == 255);
        CHECK(r && (*r)[15 * 4 + 2] == 255 && (*r)[15 * 4 + 0] == 0); // texel 15: index 1
        // DXT5: alpha endpoints 200 / 100, all alpha indices 1 -> 100.
        std::uint8_t bc3[16] = {200, 100, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
        const auto a5 = ex::decodeRgba8(D3DFormat::DXT5, 4, 4, bc3);
        CHECK(a5 && (*a5)[3] == 100 && (*a5)[0] == 255);
        const std::uint8_t f16[4] = {0, 0x3c, 0, 0};
        CHECK(!ex::decodeRgba8(D3DFormat::R16F, 1, 1, f16));
    }
}

// ---- builder --------------------------------------------------------------------------------------------

/// A captured draw: `positions` (xyz), optional uv, rebased 16-bit indices, the given topology.
std::shared_ptr<geo::CapturedDraw> makeDraw(const std::vector<float>& positions, const std::vector<float>& uvs,
                                            const std::vector<std::uint16_t>& indices, std::uint32_t topology,
                                            rh::Hash64 positionsHash) {
    auto d = std::make_shared<geo::CapturedDraw>();
    const std::uint32_t vc = static_cast<std::uint32_t>(positions.size() / 3);
    d->status = geo::CaptureStatus::Captured;
    d->vertexCount = vc;
    d->topology = topology;
    d->indexCount = static_cast<std::uint32_t>(indices.size());
    if (!indices.empty()) {
        d->indices = geo::rebaseIndices(indices.data(), d->indexCount, 2);
        d->indexType = 0;
    }
    auto stream = std::make_shared<std::vector<std::uint8_t>>();
    const std::uint32_t stride = uvs.empty() ? 12 : 20;
    for (std::uint32_t i = 0; i < vc; ++i) {
        const float* p = &positions[i * 3];
        stream->insert(stream->end(), reinterpret_cast<const std::uint8_t*>(p), reinterpret_cast<const std::uint8_t*>(p) + 12);
        if (!uvs.empty()) {
            const float* t = &uvs[i * 2];
            stream->insert(stream->end(), reinterpret_cast<const std::uint8_t*>(t), reinterpret_cast<const std::uint8_t*>(t) + 8);
        }
    }
    stream->resize(stream->size() + 64, 0);
    d->vertices.position = {stream, 0, stride, rh::D3DDeclType::Float3, 0, 0, std::size_t(stride) * vc};
    if (!uvs.empty()) {
        d->vertices.texcoord = {stream, 12, stride, rh::D3DDeclType::Float2, 0, 0, std::size_t(stride) * vc};
    }
    rh::GeometryHashes g;
    g[rh::HashComponent::Positions] = positionsHash;
    g[rh::HashComponent::Indices] = 0x1111;
    g[rh::HashComponent::GeometryDescriptor] = 0x2222 + topology;
    d->hashes = geo::JobFuture<rh::GeometryHashes>::fromValue(g);
    return d;
}

sc::TranslatedDraw makeTranslated(rh::Hash64 colorTexture) {
    sc::TranslatedDraw t;
    t.translated = true;
    t.material.colorTextureHashes[0] = colorTexture;
    t.material.colorTextureSlots[0] = colorTexture ? 0 : -1;
    t.cameraType = sc::CameraType::Main;
    return t;
}

sc::CameraState lhsCamera() {
    // D3DXMatrixPerspectiveFovLH-like decomposition result, camera at (0, 2, -10) looking down +Z.
    sc::CameraState c;
    c.fov = 1.0f;
    c.aspectRatio = 4.0f / 3.0f;
    c.nearPlane = 0.1f;
    c.farPlane = 1000.f;
    c.isLHS = true;
    c.viewToProjection = {1.3f, 0, 0, 0, 0, 1.7f, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -0.1f, 0};
    c.viewToWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 2, -10, 1};
    return c;
}

std::optional<ex::CaptureTexture> textureFor(rh::Hash64 h) {
    // An 4x4 A8R8G8B8 texture whose canonical bytes hash to the requested hash is impossible to construct,
    // so the synthetic capture keys its material by the hash of these bytes (see testBuilder).
    (void)h;
    ex::CaptureTexture t;
    t.d3dFormat = static_cast<std::uint32_t>(D3DFormat::A8R8G8B8);
    t.width = t.height = 4;
    for (int i = 0; i < 64; ++i) {
        t.mip0.push_back(static_cast<std::uint8_t>(i * 7));
    }
    return t;
}

rh::Hash64 syntheticTextureHash() {
    const auto t = textureFor(0);
    return rh::hashTextureMip0(t->mip0.data(), t->mip0.size());
}

ex::CaptureData buildSyntheticCapture(int* textureCalls = nullptr, std::size_t frames = 2) {
    const rh::Hash64 tex = syntheticTextureHash();
    // A quad as a strip (4 vertices), drawn twice (two instances); a fan (5 vertices), untextured.
    const std::vector<float> quad{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    const std::vector<float> quadUv{0, 1, 1, 1, 0, 0, 1, 0};
    auto strip = makeDraw(quad, quadUv, {0, 1, 2, 3}, 4, 0xAAAA);
    const std::vector<float> fan{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, -1, 0.5f, 0};
    auto fanDraw = makeDraw(fan, {}, {}, 5, 0xBBBB);
    const sc::TranslatedDraw texturedT = makeTranslated(tex);
    const sc::TranslatedDraw plainT = makeTranslated(0);

    ex::CaptureOptions opt;
    opt.meta.gameId = "synthetic";
    opt.meta.windowTitle = "Synthetic";
    opt.meta.exeName = "synthetic.exe";
    opt.meta.stageName = "capture_synthetic";
    int calls = 0;
    ex::GameCapturer cap(opt, [&](rh::Hash64 h, fuse::relight::tap::ResourceId id) {
        ++calls;
        (void)id;
        return textureFor(h);
    });
    sc::LightRecord sphere;
    sphere.type = rh::LightType::Sphere;
    sphere.position = {1, 2, 3};
    sphere.radius = 4;
    sphere.radiance = {2.f, 1.f, 0.5f};
    sphere.shaping.enabled = true;
    sphere.shaping.direction = {0, -1, 0};
    sphere.shaping.cosConeAngle = 0.5f;
    sphere.hash = 0x5151;
    sc::LightRecord distant;
    distant.type = rh::LightType::Distant;
    distant.direction = {0, -1, 0};
    distant.halfAngle = 0.01745329f;
    distant.radiance = {0.5f, 0.5f, 0.5f};
    distant.hash = 0xD1D1;
    for (std::size_t f = 0; f < frames; ++f) {
        ex::CaptureFrame frame;
        frame.mainCamera = lhsCamera();
        frame.lights = {sphere, distant};
        for (int k = 0; k < 2; ++k) {
            ex::CaptureDraw d;
            d.geometry = strip.get();
            d.translation = &texturedT;
            d.instance.instanceId = 10 + std::uint64_t(k);
            d.instance.objectToWorld = inst::identityMatrix();
            d.instance.objectToWorld[12] = float(k) * 5.f + (k == 1 ? float(f) : 0.f); // instance 11 moves
            d.instance.created = f == 0;
            d.instance.hasTransformChanged = f == 0 || k == 1;
            d.colorSampler.addressU = 3; // CLAMP
            d.colorSampler.magFilter = 1; // POINT
            d.colorTexture = 7;
            frame.draws.push_back(d);
        }
        ex::CaptureDraw d;
        d.geometry = fanDraw.get();
        d.translation = &plainT;
        d.instance.instanceId = 20;
        d.instance.objectToWorld = inst::identityMatrix();
        d.cullMode = 1; // D3DCULL_NONE
        frame.draws.push_back(d);
        d.instance.instanceId = 20; // a second draw of the same instance in the frame is ignored
        frame.draws.push_back(d);
        CHECK(cap.captureFrame(frame));
    }
    CHECK(cap.stats().duplicateInstanceDraws == frames);
    if (textureCalls) {
        *textureCalls = calls;
    }
    return cap.finish();
}

void testBuilder() {
    // generateIndices.
    {
        const std::uint16_t strip[4] = {0, 1, 2, 3};
        auto ri = geo::rebaseIndices(strip, 4, 2);
        CHECK((ex::triangleListIndices(4, ri.get(), 4) == std::vector<std::int32_t>{0, 1, 2, 1, 3, 2}));
        CHECK((ex::triangleListIndices(5, nullptr, 5) == std::vector<std::int32_t>{0, 1, 2, 0, 2, 3, 0, 3, 4}));
        CHECK((ex::triangleListIndices(3, nullptr, 6) == std::vector<std::int32_t>{0, 1, 2, 3, 4, 5}));
        const std::uint16_t degen[6] = {0, 1, 1, 2, 3, 9};
        auto rd = geo::rebaseIndices(degen, 6, 2);
        // (0,1,1) is degenerate, (2,3,9) is outside vertexCount 4: both collapse to vertex 0.
        CHECK((ex::triangleListIndices(3, rd.get(), 4) == std::vector<std::int32_t>{0, 0, 0, 0, 0, 0}));
        std::vector<std::int32_t> used;
        CHECK((ex::reduceIndices({5, 7, 5, 9}, used) == std::vector<std::int32_t>{0, 1, 0, 2}));
        CHECK((used == std::vector<std::int32_t>{5, 7, 9}));
    }
    // Matrices.
    {
        const ex::Mat4d r = ex::rotationBetween({0, 0, -1}, {0, -1, 0}, {1, 2, 3});
        // Row vector (0, 0, -1, 0) * r = (0, -1, 0).
        CHECK(std::fabs(-r[8] - 0.0) < 1e-12 && std::fabs(-r[9] + 1.0) < 1e-12 && std::fabs(-r[10]) < 1e-12);
        CHECK(r[12] == 1 && r[13] == 2 && r[14] == 3);
        const ex::Mat4d id = ex::multiply(r, ex::inverse(r));
        for (int i = 0; i < 16; ++i) {
            CHECK(std::fabs(id[i] - (i % 5 == 0 ? 1.0 : 0.0)) < 1e-9);
        }
        const ex::Mat4d flip = ex::rotationBetween({0, 0, -1}, {0, 0, 1}, {0, 0, 0});
        CHECK(std::fabs(flip[10] + 1.0) < 1e-12);
    }
    // Attribute decoding.
    {
        auto buf = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{0x10, 0x20, 0x30, 0x40, 0, 0x3c, 0, 0xc0});
        geo::VertexAttribute a{buf, 0, 8, rh::D3DDeclType::D3DColor, 0, 0, 8};
        const ex::Vec4f c = ex::readAttribute(a, 0);
        CHECK(c[0] == 0x30 / 255.f && c[1] == 0x20 / 255.f && c[2] == 0x10 / 255.f && c[3] == 0x40 / 255.f);
        geo::VertexAttribute h{buf, 4, 8, rh::D3DDeclType::Float16_2, 0, 0, 8};
        const ex::Vec4f hv = ex::readAttribute(h, 0);
        CHECK(hv[0] == 1.0f && hv[1] == -2.0f);
    }

    int textureCalls = 0;
    const ex::CaptureData c = buildSyntheticCapture(&textureCalls);
    const rh::Hash64 tex = syntheticTextureHash();
    CHECK(textureCalls == 1);
    CHECK(c.meta.numFramesCaptured == 2 && c.meta.endTimeCode == 1.0);
    CHECK(c.meshes.size() == 2);
    CHECK(c.materials.size() == 1 && c.materials.count(tex) == 1);
    CHECK(c.textures.size() == 1 && c.textures.at(tex).hash == tex);
    CHECK(c.instances.size() == 3);
    // Instance naming: inst_<mesh>_<n> in creation order.
    const ex::CaptureInstance& i10 = c.instances.at(10);
    const ex::CaptureInstance& i11 = c.instances.at(11);
    const ex::CaptureInstance& i20 = c.instances.at(20);
    CHECK(i10.mesh == i11.mesh && i10.meshInstNum == 0 && i11.meshInstNum == 1);
    CHECK(i10.primName() == "inst_" + rh::hashToString(i10.mesh) + "_0");
    CHECK(i10.material == tex && i20.material == 0);
    // Transform samples: instance 10 only at frame 0, instance 11 in both frames; the LHS camera mirrors x.
    CHECK(i10.xforms.size() == 1 && i11.xforms.size() == 2);
    CHECK(i10.xforms[0].xform[0] == -1.0 && i11.xforms[1].xform[12] == -6.0);
    CHECK(i10.firstTime == 0.0 && i10.finalTime == 1.0);
    // Mesh buffers: strip -> 2 triangles with swapped winding (LHS camera), uv flipped vertically.
    const ex::CaptureMesh& quad = c.meshes.at(i10.mesh);
    CHECK(quad.points.size() == 4 && quad.indices.size() == 6);
    CHECK((quad.indices == std::vector<std::int32_t>{2, 1, 0, 2, 3, 1}));
    CHECK(quad.texcoords.size() == 4 && quad.texcoords[0][1] == 0.f && quad.texcoords[2][1] == 1.f);
    CHECK(quad.materialHash == tex && !quad.isDoubleSided);
    const ex::CaptureMesh& fanMesh = c.meshes.at(i20.mesh);
    CHECK(fanMesh.indices.size() == 9 && fanMesh.isDoubleSided && fanMesh.materialHash == 0);
    CHECK(fanMesh.bounds[0] == -1.f && fanMesh.bounds[4] == 1.f);
    // Material sampler state.
    const ex::CaptureMaterial& mat = c.materials.at(tex);
    CHECK(mat.wrapU == ex::mdl::kWrapClamp && mat.wrapV == ex::mdl::kWrapRepeat && mat.filter == ex::mdl::kFilterNearest);
    // Camera: LHS projection, RHS view -> mirrored; USD camera looks down -Z at the (mirrored) position.
    CHECK(c.camera.valid && c.camera.isLHS() && !c.camera.projInv && !c.camera.viewInv);
    CHECK(c.camera.xforms.size() == 2);
    const ex::Mat4d& cv = c.camera.xforms[0].xform;
    CHECK(cv[12] == -0.0 && cv[13] == 2.0 && cv[14] == -10.0);
    CHECK(std::fabs(cv[10] + 1.0) < 1e-12); // -forward = -(+Z)
    CHECK(c.globalXform[0] == -1.0 && c.globalXform[5] == 1.0);
    // Lights: colour normalised by the max component, intensity = max.
    const ex::CaptureSphereLight& sl = c.sphereLights.at(0x5151);
    CHECK(sl.intensity == 2.f && sl.color[0] == 1.f && sl.color[1] == 0.5f && sl.color[2] == 0.25f);
    CHECK(sl.shapingEnabled && std::fabs(sl.coneAngleDegrees - 60.f) < 1e-4f && sl.xforms.size() == 2);
    const ex::CaptureDistantLight& dl = c.distantLights.at(0xD1D1);
    CHECK(std::fabs(dl.angleDegrees - 2.f) < 1e-4f && dl.intensity == 0.5f && dl.firstTime == 0.0 && dl.finalTime == 1.0);
    // Keys.
    const auto keys = ex::captureKeys(c, rh::parseHashRule(rh::rules::kDefaultAssetRuleString));
    int geom = 0, texKeys = 0, lights = 0, shas = 0;
    for (const ex::CaptureKey& k : keys) {
        geom += k.algo == ex::key_algo::kGeomAsset;
        texKeys += k.algo == ex::key_algo::kTexture;
        lights += k.algo == ex::key_algo::kLight;
        shas += k.algo == ex::key_algo::kCaptureSha256;
    }
    CHECK(geom == 2 && texKeys == 1 && lights == 2 && shas == 3);

    // maxFrames.
    ex::CaptureOptions one;
    one.maxFrames = 1;
    ex::GameCapturer limited(one);
    CHECK(limited.captureFrame({}));
    CHECK(!limited.captureFrame({}));

    // Skinning: generateSkeleton / sanitizeBoneXforms.
    ex::CaptureMesh skinned;
    skinned.numBones = 2;
    skinned.bonesPerVertex = 1;
    skinned.points = {{0, 0, 0}, {2, 0, 0}, {10, 0, 0}};
    skinned.jointWeights = {1, 1, 1};
    skinned.jointIndices = {0, 0, 1};
    const ex::CaptureSkeleton sk = ex::generateSkeleton(skinned);
    CHECK(sk.jointNames.size() == 2 && sk.jointNames[0] == "root" && sk.jointNames[1] == "root/joint1");
    CHECK(sk.bindPose[0][12] == 1.0 && sk.bindPose[1][12] == 10.0 && sk.restPose[1][12] == 9.0);
    const auto bones = ex::sanitizeBoneXforms({ex::identity4d(), ex::identity4d()}, sk.bindPose);
    CHECK(bones.size() == 2 && bones[0][12] == 1.0 && bones[1][12] == 9.0);
}

/// Dynamic geometry and a render-target material: one instance whose vertices move each frame (same topology),
/// textured with a texture whose bytes are not available (a render target with a descriptor hash).
ex::CaptureData buildDynamicCapture() {
    const std::vector<float> uv{0, 0, 1, 0, 0, 1};
    ex::CaptureOptions opt;
    opt.meta.gameId = "dynamic";
    opt.meta.stageName = "capture_dynamic";
    ex::GameCapturer cap(opt, [](rh::Hash64 h, fuse::relight::tap::ResourceId) {
        ex::CaptureTexture t;
        t.hash = h;
        t.descriptorHash = 0xDE5C;
        t.d3dFormat = static_cast<std::uint32_t>(D3DFormat::A8R8G8B8);
        t.width = t.height = 64; // no bytes: a render target
        return std::optional<ex::CaptureTexture>(t);
    });
    const sc::TranslatedDraw t = makeTranslated(0xABCDEF);
    const float offsets[4] = {0.f, 0.1f, 1.f, 1.1f};
    std::vector<std::shared_ptr<geo::CapturedDraw>> keep;
    for (int f = 0; f < 4; ++f) {
        const float o = offsets[f];
        keep.push_back(makeDraw({o, 0, 0, 1 + o, 0, 0, o, 1, 0}, uv, {0, 1, 2}, 3, 0x5000 + std::uint64_t(f)));
        ex::CaptureFrame frame;
        ex::CaptureDraw d;
        d.geometry = keep.back().get();
        d.translation = &t;
        d.instance.instanceId = 1;
        d.instance.objectToWorld = inst::identityMatrix();
        d.instance.created = f == 0;
        frame.draws.push_back(d);
        CHECK(cap.captureFrame(frame));
    }
    // A different topology later (4 vertices): counted, not captured.
    keep.push_back(makeDraw({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}, {}, {0, 1, 2, 3}, 4, 0x6000));
    ex::CaptureFrame frame;
    ex::CaptureDraw d;
    d.geometry = keep.back().get();
    d.translation = &t;
    d.instance.instanceId = 1;
    d.instance.objectToWorld = inst::identityMatrix();
    frame.draws.push_back(d);
    CHECK(cap.captureFrame(frame));
    CHECK(cap.stats().meshUpdates == 4 && cap.stats().meshSamples == 1 && cap.stats().meshTopologyChanges == 1);
    CHECK(cap.stats().texturesMissing == 1);
    return cap.finish();
}

void testDynamic() {
    const ex::CaptureData c = buildDynamicCapture();
    CHECK(c.meshes.size() == 1 && c.instances.size() == 1);
    const ex::CaptureMesh& m = c.meshes.begin()->second;
    // Frame 1 moved 0.1 (< rtx.captureMeshPositionDelta 0.3): no sample; frame 2 moved 1.0: a sample; frame 3
    // moved 0.1 from frame 2's sample: none.
    CHECK(m.pointSamples.size() == 1 && m.pointSamples.count(2.0) == 1);
    CHECK(m.pointSamples.count(2.0) && m.pointSamples.at(2.0)[0][0] == 1.f && m.points[0][0] == 0.f);
    CHECK(c.textures.size() == 1 && c.textures.begin()->second.mip0.empty());
    const rh::HashRule rule = rh::parseHashRule(rh::rules::kDefaultAssetRuleString);
    const auto keys = ex::captureKeys(c, rule);
    bool tex = false, desc = false, texSha = false;
    for (const ex::CaptureKey& k : keys) {
        tex = tex || (k.algo == ex::key_algo::kTexture && k.value == rh::hashToString(0xABCDEF));
        desc = desc || (k.algo == ex::key_algo::kRtDescriptor && k.value == rh::hashToString(0xDE5C));
        texSha = texSha || (k.algo == ex::key_algo::kCaptureSha256 && k.kind == "texture");
    }
    CHECK(tex && desc && !texSha);
    const fs::path dir = fs::current_path() / "rl_capture_export_unit_tmp" / "dynamic";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ex::CaptureWriteReport rep;
    CHECK(ex::writeCapture(dir, c, rule, &rep) && rep.ok() && rep.textures == 0);
    const rl_capture_check::Result r = rl_capture_check::checkCapture(dir, ex::captureStageFileName(c.meta),
                                                                      std::string(rh::rules::kDefaultAssetRuleString));
    for (const std::string& e : r.errors) {
        std::fprintf(stderr, "  dynamic check: %s\n", e.c_str());
    }
    CHECK(r.ok() && r.storeKeys == keys && r.materials == 1 && r.textures == 0);
    std::string mesh;
    ex::readFile(dir / "meshes" / ("mesh_" + rh::hashToString(m.hash) + ".usda"), mesh);
    CHECK(mesh.find("point3f[] points.timeSamples = {") != std::string::npos);
    CHECK(mesh.find("point3f[] points = ") == std::string::npos);
}

// ---- writer ---------------------------------------------------------------------------------------------

std::map<std::string, std::vector<std::uint8_t>> readTree(const fs::path& dir) {
    std::map<std::string, std::vector<std::uint8_t>> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file()) {
            std::vector<std::uint8_t> b;
            ex::readFile(e.path(), b);
            out[fs::relative(e.path(), dir).generic_string()] = std::move(b);
        }
    }
    return out;
}

void writeBytes(const fs::path& p, const std::vector<std::uint8_t>& b) { ex::writeFile(p, b); }

void testWriter() {
    const fs::path base = fs::current_path() / "rl_capture_export_unit_tmp";
    std::error_code ec;
    fs::remove_all(base, ec);
    const ex::CaptureData c = buildSyntheticCapture();
    const rh::HashRule rule = rh::parseHashRule(rh::rules::kDefaultAssetRuleString);
    const std::string ruleString(rh::rules::kDefaultAssetRuleString);
    ex::CaptureWriteReport rep;
    CHECK(ex::writeCapture(base / "a", c, rule, &rep));
    CHECK(rep.ok() && rep.textures == 1);
    for (const std::string& e : rep.errors) {
        std::fprintf(stderr, "  writer: %s\n", e.c_str());
    }
    const std::string stage = ex::captureStageFileName(c.meta);
    const rl_capture_check::Result r = rl_capture_check::checkCapture(base / "a", stage, ruleString);
    for (const std::string& e : r.errors) {
        std::fprintf(stderr, "  check: %s\n", e.c_str());
    }
    CHECK(r.ok());
    CHECK(r.meshes == 2 && r.materials == 1 && r.instances == 3 && r.lights == 2 && r.textures == 1 && r.camera);
    CHECK(r.layers == 1 + 2 + 1 + 1); // stage, 2 meshes, 1 material, 1 sphere light
    // Re-ingest gives the identical key set.
    const auto keys = ex::captureKeys(c, rule);
    CHECK(r.storeKeys == keys);
    CHECK(rep.keys == keys);
    // Deterministic: a second write is byte-identical.
    CHECK(ex::writeCapture(base / "b", c, rule));
    const auto ta = readTree(base / "a"), tb = readTree(base / "b");
    CHECK(ta == tb);
    CHECK(ta.count("textures/" + rh::hashToString(syntheticTextureHash()) + ".dds") == 1);
    CHECK(ta.count("store/db/remaster_db.json") == 1);

    // A single-frame capture writes default values (no time samples) and still checks out.
    {
        const ex::CaptureData one = buildSyntheticCapture(nullptr, 1);
        CHECK(ex::writeCapture(base / "one", one, rule));
        CHECK(rl_capture_check::checkCapture(base / "one", stage, ruleString).ok());
        std::string text;
        ex::readFile(base / "one" / stage, text);
        CHECK(text.find(".timeSamples") == std::string::npos);
        std::string multi;
        ex::readFile(base / "a" / stage, multi);
        CHECK(multi.find("xformOp:transform.timeSamples") != std::string::npos);
    }

    // Seeded faults: each must be caught by the re-ingest.
    auto fresh = [&](const char* name) {
        const fs::path d = base / name;
        fs::remove_all(d, ec);
        ex::writeCapture(d, c, rule);
        return d;
    };
    {
        const fs::path d = fresh("fault_blob");
        for (const auto& e : fs::recursive_directory_iterator(d / "store" / "blobs")) {
            if (e.is_regular_file()) {
                std::vector<std::uint8_t> b;
                ex::readFile(e.path(), b);
                b[0] ^= 1;
                writeBytes(e.path(), b);
                break;
            }
        }
        CHECK(!rl_capture_check::checkCapture(d, stage, ruleString).ok());
    }
    {
        const fs::path d = fresh("fault_mesh_name");
        std::string text;
        ex::readFile(d / stage, text);
        const std::string from = "mesh_" + rh::hashToString(c.meshes.begin()->first);
        const std::size_t at = text.find("\"" + from + "\"");
        CHECK(at != std::string::npos);
        text.replace(at + 1 + 5, 16, "0123456789ABCDEF");
        ex::writeFile(d / stage, text);
        CHECK(!rl_capture_check::checkCapture(d, stage, ruleString).ok());
    }
    {
        const fs::path d = fresh("fault_dds");
        const fs::path p = d / ex::captureTexturePath(syntheticTextureHash());
        std::vector<std::uint8_t> b;
        ex::readFile(p, b);
        b.back() ^= 0x80;
        writeBytes(p, b);
        CHECK(!rl_capture_check::checkCapture(d, stage, ruleString).ok());
    }
    {
        const fs::path d = fresh("fault_db_row");
        std::string text;
        ex::readFile(d / "store" / "db" / "remaster_db.json", text);
        auto db = ex::json::parse(text);
        CHECK(db.has_value());
        if (db) {
            (*db)["hash_key"].a.pop_back();
            ex::writeFile(d / "store" / "db" / "remaster_db.json", ex::json::writePretty(*db));
            CHECK(!rl_capture_check::checkCapture(d, stage, ruleString).ok());
        }
    }
    {
        const fs::path d = fresh("fault_usda_syntax");
        std::string text;
        ex::readFile(d / stage, text);
        text.insert(text.find("def \"RootNode\""), "def Xform \"broken\" {\n");
        ex::writeFile(d / stage, text);
        CHECK(!rl_capture_check::checkCapture(d, stage, ruleString).ok());
    }
    if (!std::getenv("RL_CAPTURE_EXPORT_KEEP")) { // debugging aid: keep the written captures
        fs::remove_all(base, ec);
    }
}

// ---- the USDA parser itself -------------------------------------------------------------------------------

void testParser() {
    const char* text = R"(#usda 1.0
(
    customLayerData = {
        dictionary d = {
            string s = "x"
        }
        uint64 big = 18446744073709551615
    }
    defaultPrim = "A"
)

def Xform "A" (
    prepend apiSchemas = ["MaterialBindingAPI"]
    prepend references = @./b.usda@</B>
)
{
    rel material:binding = </Looks/m>
    float3[] f = [(1, 2, 3), (-4.5e-1, inf, -inf)]
    matrix4d xformOp:transform.timeSamples = {
        0: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ),
        1.5: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 0, 0, 1) ),
    }
    custom uniform bool remix_category:sky = 0
    float inputs:shaping:focus
    token outputs:out
    def Mesh "mesh"
    {
        texCoord2f[] primvars:st = [(0, 1)] (
            interpolation = "vertex"
        )
    }
}
)";
    std::string err;
    const auto l = rl_usda::parse(text, &err);
    CHECK(l.has_value());
    if (!l) {
        std::fprintf(stderr, "  parser: %s\n", err.c_str());
        return;
    }
    CHECK(l->meta("customLayerData")->get("big")->s == "18446744073709551615");
    CHECK(l->meta("customLayerData")->get("d")->get("s")->s == "x");
    const rl_usda::Prim* a = l->find("/A");
    CHECK(a && a->type == "Xform" && a->meta("prepend references")->s == "./b.usda" && a->meta("prepend references")->target == "/B");
    CHECK(a && a->property("xformOp:transform")->timeSamples.size() == 2);
    CHECK(a && a->property("xformOp:transform")->timeSamples[1].first == 1.5);
    CHECK(a && a->property("f")->value.items.size() == 2 && std::isinf(a->property("f")->value.items[1].items[1].n));
    CHECK(a && a->property("remix_category:sky")->custom && a->property("remix_category:sky")->uniform);
    CHECK(a && !a->property("inputs:shaping:focus")->hasDefault);
    CHECK(l->find("/A/mesh") && l->find("/A/mesh")->property("primvars:st")->metadata.size() == 1);
    CHECK(!rl_usda::parse("#usda 1.0\ndef \"A\" {\n", &err));
    CHECK(!rl_usda::parse("not usda", &err));
    CHECK(!rl_usda::parse("#usda 1.0\ndef \"A\" { float x = [1, 2 }\n", &err));
}

} // namespace

int main() {
    testDigest();
    testJson();
    testDds();
    testBuilder();
    testDynamic();
    testParser();
    testWriter();
    std::printf("rl_capture_export_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
