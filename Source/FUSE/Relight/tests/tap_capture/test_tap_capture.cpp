// FUSE Relight RL-1.1: CPU unit tests of the capture tap (ctest rl_tap_capture_unit).
//
// One synthetic event stream (textures: managed upload, UpdateSurface full / partial rect, render
// target back buffer; buffers: indexed u16 draw, UP draw, a DISCARD rewrite; two frames) is fed to
// CaptureTap and, separately, to the packages the replay tools drive (TextureTracker,
// GeometryCapture with its default texture facts, ClassifyTap with the tracker's final hashes set
// up front, as rl_classify_replay does). Every draw's asset key and geometry hashes, bound texture
// hashes and classification must be equal; the capture record must hold the documented lines;
// createTapForDevice must build every mode.
#include <fuse/relight/tap/capture_tap.hpp>
#include <fuse/relight/tap/device_tap.hpp>
#include <fuse/relight/tap/null_tap.hpp>
#include <fuse/relight/tap/recording_tap.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

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

using namespace fuse::relight;
using namespace fuse::relight::tap;
namespace geo = fuse::relight::capture::geometry;
namespace tex = fuse::relight::capture::texture;

constexpr std::uint32_t kFmtA8R8G8B8 = 21, kFmtIndex16 = 101;
constexpr std::uint32_t kPoolDefault = 0, kPoolManaged = 1, kPoolSystemMem = 2;
constexpr std::uint32_t kTypeSurface = 1, kTypeTexture = 3;
constexpr std::uint32_t kUsageRenderTarget = 1;
constexpr std::uint32_t kLockDiscard = 0x2000;
constexpr std::uint32_t kTriangleList = 4;

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string l; std::getline(in, l);) {
        lines.push_back(l);
    }
    return lines;
}

int countPrefix(const std::vector<std::string>& lines, const std::string& prefix) {
    int n = 0;
    for (const std::string& l : lines) {
        n += l.compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
    }
    return n;
}

/// The synthetic application: owns the bytes and state arrays the events point at.
struct App {
    std::uint32_t renderStates[kRenderStateCount] = {};
    std::uint32_t tss[kTextureStageCount][32] = {};
    std::uint32_t samplers[kSamplerSlotCount][kSamplerStateCount] = {};
    float transforms[kTransformCount][16] = {};
    std::vector<std::uint8_t> vb, ib, up, tex2, tex3;

    App() {
        renderStates[7] = 1;      // ZENABLE
        renderStates[14] = 1;     // ZWRITEENABLE
        renderStates[168] = 0xF;  // COLORWRITEENABLE
        renderStates[137] = 1;    // LIGHTING
        for (std::uint32_t s = 0; s < kTextureStageCount; ++s) {
            tss[s][0] = s == 0 ? 4u : 1u; // COLOROP: MODULATE / DISABLE
            tss[s][1] = 2;                // COLORARG1 TEXTURE
            tss[s][2] = 1;                // COLORARG2 CURRENT
            tss[s][3] = s == 0 ? 2u : 1u; // ALPHAOP
            tss[s][4] = 2;
            tss[s][5] = 1;
            tss[s][10] = s;               // TEXCOORDINDEX
        }
        for (auto& m : transforms) {
            for (int k = 0; k < 16; ++k) {
                m[k] = (k % 5 == 0) ? 1.0f : 0.0f;
            }
        }
        // Perspective projection (not orthographic: [3][3] = 0).
        const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, -0.1f, 0};
        std::memcpy(transforms[kTransformProjection], proj, sizeof proj);
        vb = quad(0.0f);
        const std::uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
        ib.assign(reinterpret_cast<const std::uint8_t*>(idx), reinterpret_cast<const std::uint8_t*>(idx) + 12);
        up = quad(5.0f);
        up.resize(3 * 20);
        tex2.resize(4 * 4 * 4);
        tex3.resize(4 * 4 * 4);
        for (std::size_t i = 0; i < tex2.size(); ++i) {
            tex2[i] = std::uint8_t(i * 13 + 1);
            tex3[i] = std::uint8_t(i * 7 + 5);
        }
    }

    static std::vector<std::uint8_t> quad(float z) {
        // FLOAT3 position + FLOAT2 texcoord, stride 20.
        const float v[4][5] = {{-1, -1, z, 0, 1}, {-1, 1, z, 0, 0}, {1, -1, z, 1, 1}, {1, 1, z, 1, 0}};
        std::vector<std::uint8_t> out(sizeof v);
        std::memcpy(out.data(), v, sizeof v);
        return out;
    }

    DrawState state(ResourceId texture) const {
        DrawState s;
        s.renderStates = renderStates;
        s.textureStageStates = tss;
        s.samplerStates = samplers;
        s.transforms = transforms;
        s.textures[0] = texture;
        s.elements[0] = VertexElement{0, 0, 2, 0, 0, 0};  // FLOAT3 POSITION
        s.elements[1] = VertexElement{0, 12, 1, 0, 5, 0}; // FLOAT2 TEXCOORD
        s.elementCount = 2;
        s.fvf = 0x102; // XYZ | TEX1
        s.viewport = Viewport{0, 0, 64, 64, 0, 1};
        s.renderTargets[0] = 1;
        return s;
    }

    void texture(IRelightTap& t, ResourceId id, std::uint32_t type, std::uint32_t pool, std::uint32_t usage,
                 bool backBuffer) {
        TextureDesc d;
        d.id = id;
        d.type = type;
        d.width = backBuffer ? 64 : 4;
        d.height = backBuffer ? 64 : 4;
        d.depth = 1;
        d.mipLevels = 1;
        d.arraySize = 1;
        d.format = kFmtA8R8G8B8;
        d.usage = usage;
        d.pool = pool;
        d.isBackBuffer = backBuffer;
        d.vkImage = pool == kPoolSystemMem ? 0 : 0x1000 + id;
        t.onTextureCreate(d);
    }

    void upload(IRelightTap& t, ResourceId id, const std::vector<std::uint8_t>& data) {
        t.onTextureWriteLock(TextureWriteLock{id, 0, 0, 0});
        TextureUpload u;
        u.texture = id;
        u.width = u.height = 4;
        u.depth = 1;
        u.data = data.data();
        u.rowPitch = 16;
        u.slicePitch = 64;
        u.rows = 4;
        u.fullUpdate = true;
        u.box = Box{0, 0, 4, 4, 0, 1};
        t.onTextureUpload(u);
    }

    void bufferWrite(IRelightTap& t, ResourceId id, const std::vector<std::uint8_t>& data, std::uint32_t flags) {
        BufferWrite w;
        w.buffer = id;
        w.offset = 0;
        w.size = std::uint32_t(data.size());
        w.lockFlags = flags;
        w.data = data.data();
        w.base = data.data();
        w.bufferSize = std::uint32_t(data.size());
        t.onBufferWrite(w);
    }

    void drawIndexed(IRelightTap& t, ResourceId texture, bool mapped) {
        DrawCall c;
        c.call = DrawCallType::DrawIndexedPrimitive;
        c.primitiveType = kTriangleList;
        c.primitiveCount = 2;
        c.numVertices = 4;
        c.indexCount = 6;
        DrawState s = state(texture);
        s.streams[0] = StreamBinding{1, 0, 20, 1, mapped ? vb.data() : nullptr, mapped ? 80u : 0u};
        s.indices = IndexBinding{2, kFmtIndex16, mapped ? ib.data() : nullptr, mapped ? 12u : 0u};
        CHECK(t.onDraw(c, s) == DrawDecision::Raster);
    }

    void drawUP(IRelightTap& t, ResourceId texture) {
        DrawCall c;
        c.call = DrawCallType::DrawPrimitiveUP;
        c.primitiveType = kTriangleList;
        c.primitiveCount = 1;
        c.vertexCount = 3;
        c.upVertexData = up.data();
        c.upVertexStride = 20;
        c.upVertexBytes = 60;
        CHECK(t.onDraw(c, state(texture)) == DrawDecision::Raster);
    }

    /// The whole stream. `mapped`: bindings carry CPU pointers (DXVK mappings) in frame 0.
    void run(IRelightTap& t, bool mapped) {
        DeviceEvent dev;
        dev.present.backBufferWidth = dev.present.backBufferHeight = 64;
        dev.present.backBufferFormat = kFmtA8R8G8B8;
        dev.backBuffer = 1;
        texture(t, 1, kTypeSurface, kPoolDefault, kUsageRenderTarget, true);
        t.onDeviceCreate(dev);
        texture(t, 2, kTypeTexture, kPoolManaged, 0, false);
        upload(t, 2, tex2);
        texture(t, 3, kTypeTexture, kPoolSystemMem, 0, false);
        upload(t, 3, tex3);
        texture(t, 4, kTypeTexture, kPoolDefault, 0, false);
        texture(t, 5, kTypeTexture, kPoolDefault, 0, false);
        TextureCopy full;
        full.method = CopyMethod::UpdateSurface;
        full.source = 3;
        full.destination = 4;
        full.hasSourceRect = true;
        full.width = full.height = 4;
        t.onTextureCopy(full);
        TextureCopy part = full;
        part.destination = 5;
        part.width = 2;
        t.onTextureCopy(part);

        t.onBufferCreate(BufferDesc{1, BufferKind::Vertex, 80, 0, kPoolDefault, 0x102, 0});
        t.onBufferCreate(BufferDesc{2, BufferKind::Index, 12, 0, kPoolDefault, 0, kFmtIndex16});
        bufferWrite(t, 1, vb, 0);
        bufferWrite(t, 2, ib, 0);

        drawIndexed(t, 2, mapped);
        drawUP(t, 4);
        drawIndexed(t, 5, mapped); // texture without a hash (partial UpdateSurface)
        t.onInjectPoint(FrameEvent{0, 1, 0x1001, 64, 64, kFmtA8R8G8B8});
        t.onPresent(FrameEvent{0, 1, 0x1001, 64, 64, kFmtA8R8G8B8});

        vb = quad(2.0f);
        bufferWrite(t, 1, vb, kLockDiscard);
        drawIndexed(t, 2, false); // no mapping: the capture's buffer shadow
        t.onPresent(FrameEvent{1, 1, 0x1001, 64, 64, kFmtA8R8G8B8});
        t.onImageDestroy(ImageDestroy{5, 0x1005});
        t.onDeviceDestroy();
    }
};

/// The replay tools' pipeline, in-process: each package fed on its own.
struct Separate {
    std::atomic<std::uint32_t> counter{0};
    std::unique_ptr<tex::TextureTracker> textures;
    std::map<ResourceId, std::uint64_t> hashes; ///< the tracker's hashes before destruction
    std::unique_ptr<geo::GeometryCapture> geometry;
    std::vector<scene::ClassifiedDraw> classified;

    explicit Separate(bool mapped) {
        tex::TextureTrackerConfig tc;
        tc.renderTargetCounter = &counter;
        textures = std::make_unique<tex::TextureTracker>(tc);
        App a;
        a.run(*textures, mapped);
        // TextureTracker's hashes (a texture keeps its first hash: no recompute-on-write here),
        // supplied to the classifier up front as rl_classify_replay --texture-hashes does. Taken
        // before the stream's image_destroy / device_destroy by a second tracker run to the Present.
        {
            std::atomic<std::uint32_t> c2{0};
            tex::TextureTrackerConfig tc2;
            tc2.renderTargetCounter = &c2;
            tex::TextureTracker t2(tc2);
            struct UntilDestroy final : IRelightTap {
                tex::TextureTracker& t;
                explicit UntilDestroy(tex::TextureTracker& tr) : t(tr) {}
                void onTextureCreate(const TextureDesc& d) override { t.onTextureCreate(d); }
                void onTextureUpload(const TextureUpload& u) override { t.onTextureUpload(u); }
                void onTextureCopy(const TextureCopy& c) override { t.onTextureCopy(c); }
                void onTextureWriteLock(const TextureWriteLock& l) override { t.onTextureWriteLock(l); }
                void onDeviceCreate(const DeviceEvent& e) override { t.onDeviceCreate(e); }
                DrawDecision onDraw(const DrawCall& c, const DrawState& s) override { return t.onDraw(c, s); }
            } until(t2);
            App b;
            b.run(until, mapped);
            for (const tex::TrackedTexture& x : t2.textures()) {
                hashes[x.desc.id] = x.imageHash;
            }
        }
        scene::ClassifyTap classify(nullptr, [this](const scene::ClassifiedDraw& d) { classified.push_back(d); });
        for (const auto& [id, h] : hashes) {
            classify.tracker().setTextureHash(id, h);
        }
        App c;
        c.run(classify, mapped);
        geometry = std::make_unique<geo::GeometryCapture>(geo::GeometryCaptureConfig{});
        App d;
        d.run(*geometry, mapped);
    }
};

bool sameClassification(const scene::DrawClassification& a, const scene::DrawClassification& b) {
    return a.drawCallId == b.drawCallId && a.status == b.status && a.reason == b.reason &&
           a.triggerRtxInjection == b.triggerRtxInjection && a.prepareFlags == b.prepareFlags &&
           a.categories.toString() == b.categories.toString() && a.skyAutoDetected == b.skyAutoDetected &&
           a.isDrawingToRaytracedRenderTarget == b.isDrawingToRaytracedRenderTarget &&
           a.isUsingRaytracedRenderTarget == b.isUsingRaytracedRenderTarget &&
           a.colorTextureHash == b.colorTextureHash && a.texcoordIndex == b.texcoordIndex;
}

void testLiveEqualsSeparate(bool mapped) {
    const std::string path = mapped ? "rl_tap_capture_mapped.jsonl" : "rl_tap_capture_shadow.jsonl";
    std::atomic<std::uint32_t> counter{0};
    std::vector<CaptureDrawRecord> live;
    std::vector<std::uint64_t> frames;
    {
        CaptureTapConfig config;
        config.path = path;
        config.texture.renderTargetCounter = &counter;
        CaptureTap tap(std::move(config));
        CHECK(tap.isOpen());
        tap.setFrameSink([&](std::uint64_t frame, const std::vector<CaptureDrawRecord>& draws) {
            frames.push_back(frame);
            live.insert(live.end(), draws.begin(), draws.end());
            if (frame == 0) {
                // UpdateSurface extent (tap interface 2): the 4x4 rect inherits, the 2x4 one does not.
                CHECK(tap.textures().imageHash(4) != 0);
                const std::optional<tex::TrackedTexture> t4 = tap.textures().find(4);
                CHECK(t4 && t4->origin == tex::HashOrigin::Inherited && t4->inheritedFrom == 3);
                CHECK(tap.textures().imageHash(5) == 0);
                CHECK(tap.imageRegistry().liveCount() >= 3); // back buffer, 2, 4, 5 have images
            }
        });
        App app;
        app.run(tap, mapped);
        CHECK(tap.imageRegistry().liveCount() == 0); // released at device destruction
    }
    Separate sep(mapped);
    const std::vector<geo::CapturedDrawPtr> geometry = sep.geometry->takeDraws();

    CHECK(frames.size() == 2 && frames[0] == 0 && frames[1] == 1);
    CHECK(live.size() == 4);
    CHECK(geometry.size() == live.size());
    CHECK(sep.classified.size() == live.size());
    const geo::GeometryCaptureConfig gc;
    for (std::size_t i = 0; i < live.size() && i < geometry.size() && i < sep.classified.size(); ++i) {
        const CaptureDrawRecord& r = live[i];
        CHECK(r.n == i);
        CHECK(r.geometry != nullptr);
        if (!r.geometry) {
            continue;
        }
        CHECK(r.geometry->status == geometry[i]->status);
        CHECK(r.geometry->captured());
        if (r.geometry->captured() && geometry[i]->captured()) {
            CHECK(r.geometry->assetHash(gc.assetRule) == geometry[i]->assetHash(gc.assetRule));
            CHECK(r.geometry->assetHash(gc.assetRule) != 0);
            const hash::DrawGeometryHashes a = r.geometry->geometryHashes(), b = geometry[i]->geometryHashes();
            for (std::uint32_t k = 0; k < hash::kHashComponentCount; ++k) {
                CHECK(a.hashes.fields[k] == b.hashes.fields[k]);
            }
        }
        CHECK(r.geometry->texcoord.texcoordIndex == geometry[i]->texcoord.texcoordIndex);
        CHECK(sameClassification(r.classification, sep.classified[i].result));
        CHECK(r.frame == sep.classified[i].frame && r.drawInFrame == sep.classified[i].indexInFrame);
        CHECK(r.textures.size() == 1);
        for (const CaptureDrawRecord::BoundTexture& t : r.textures) {
            CHECK(t.slot == 0);
            CHECK(sep.hashes.count(t.texture) && t.imageHash == sep.hashes[t.texture]);
        }
    }
    if (live.size() == 4) {
        CHECK(live[0].textures[0].imageHash != 0); // managed, hashed at its first draw
        CHECK(live[1].textures[0].imageHash != 0); // inherited through the full UpdateSurface
        CHECK(live[2].textures[0].imageHash == 0); // partial rect: no hash
        CHECK(live[2].classification.reason == scene::ClassifyReason::ColorTextureWithoutHash);
        // The DISCARD rewrite changed the positions: a new asset key in frame 1.
        CHECK(live[3].geometry && live[0].geometry &&
              live[3].geometry->assetHash(gc.assetRule) != live[0].geometry->assetHash(gc.assetRule));
    }

    const std::vector<std::string> lines = readLines(path);
    CHECK(!lines.empty() && lines[0].find("\"schema\":\"fuse.relight.capture/1\"") != std::string::npos);
    CHECK(countPrefix(lines, "{\"ev\":\"draw\"") == 4);
    CHECK(countPrefix(lines, "{\"ev\":\"textures\"") == 2);
    CHECK(countPrefix(lines, "{\"ev\":\"frame\"") == 2);
    CHECK(countPrefix(lines, "{\"ev\":\"device_destroy\"") == 1);
    bool keyed = false;
    for (const std::string& l : lines) {
        keyed = keyed || (l.find("\"key\":\"") != std::string::npos && l.find("\"classification\":{") != std::string::npos);
    }
    CHECK(keyed);
    std::remove(path.c_str());
}

void testFactory() {
    RuntimeConfig cfg;
    cfg.tapMode = TapMode::Off;
    CHECK(createTapForDevice(cfg, 0) == nullptr);
    cfg.tapMode = TapMode::Null;
    {
        auto t = createTapForDevice(cfg, 0);
        CHECK(dynamic_cast<NullTap*>(t.get()) != nullptr);
    }
    cfg.tapMode = TapMode::Capture;
    cfg.relightEnabled = false;
    CHECK(createTapForDevice(cfg, 0) == nullptr);
    cfg.relightEnabled = true;
    cfg.capturePath = "rl_tap_capture_factory.jsonl";
    cfg.recordPath = "rl_tap_capture_factory_record.jsonl";
    cfg.captureRecord = true;
    {
        auto t = createTapForDevice(cfg, 1);
        auto* c = dynamic_cast<CaptureTap*>(t.get());
        CHECK(c != nullptr && c->isOpen());
        if (c) {
            App app;
            app.run(*c, true);
        }
    }
    const std::vector<std::string> cap = readLines("rl_tap_capture_factory.jsonl.1");
    const std::vector<std::string> rec = readLines("rl_tap_capture_factory_record.jsonl.1");
    CHECK(countPrefix(cap, "{\"ev\":\"draw\"") == 4);
    CHECK(countPrefix(rec, "{\"ev\":\"draw\"") == 4); // the recording tap saw the same events
    bool extent = false;
    for (const std::string& l : rec) {
        extent = extent || (l.find("\"ev\":\"texture_copy\"") != std::string::npos &&
                            l.find("\"width\":2,\"height\":4") != std::string::npos);
    }
    CHECK(extent);
    std::remove("rl_tap_capture_factory.jsonl.1");
    std::remove("rl_tap_capture_factory_record.jsonl.1");
}

} // namespace

int main() {
    testLiveEqualsSeparate(true);
    testLiveEqualsSeparate(false);
    testFactory();
    std::printf("rl_tap_capture_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
