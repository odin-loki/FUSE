// FUSE Relight RL-1.1: the capture tap (relight.tap.mode = capture). See capture_tap.hpp.
#include <fuse/relight/tap/capture_tap.hpp>

#include <fuse/relight/capture/texture/texture_options.hpp>
#include <fuse/relight/scene/classify/classify_options.hpp>
#include <fuse/relight/tap/device_tap.hpp>
#include <fuse/relight/tap/recording_tap.hpp>

#include <cinttypes>
#include <cstring>
#include <utility>

namespace fuse::relight::tap {

namespace geo = capture::geometry;
namespace tex = capture::texture;

namespace {

// ---- formatting (the replay tools' spelling, so the checks compare strings) ------------------------

std::string h64(std::uint64_t v) { // geometry_replay: lower-case, no prefix
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016" PRIx64, v);
    return buf;
}

std::string H64(std::uint64_t v) { // texture replay: upper-case, no prefix
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016" PRIX64, v);
    return buf;
}

std::string hex0x(std::uint64_t v) { // rl_classify_replay: 0x + lower-case
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016" PRIx64, v);
    return buf;
}

std::string f32bits(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, 4);
    char buf[12];
    std::snprintf(buf, sizeof buf, "%08x", b);
    return buf;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

/// Minimal JSON object builder (keys in insertion order).
class Obj {
public:
    Obj& raw(const char* key, const std::string& value) {
        if (!m_s.empty()) {
            m_s += ',';
        }
        m_s += '"';
        m_s += key;
        m_s += "\":";
        m_s += value;
        return *this;
    }
    Obj& str(const char* key, const std::string& v) { return raw(key, quote(v)); }
    Obj& u(const char* key, std::uint64_t v) { return raw(key, std::to_string(v)); }
    Obj& b(const char* key, bool v) { return raw(key, v ? "true" : "false"); }
    std::string done() const { return "{" + m_s + "}"; }

private:
    std::string m_s;
};

std::string array(const std::vector<std::string>& items) {
    std::string s = "[";
    for (std::size_t k = 0; k < items.size(); ++k) {
        s += (k ? "," : "") + items[k];
    }
    return s + "]";
}

const char* decisionName(DrawDecision d) {
    switch (d) {
    case DrawDecision::Raster: return "raster";
    case DrawDecision::Ignore: return "ignore";
    case DrawDecision::RayTracedPreserveRaster: return "raytraced_preserve_raster";
    }
    return "?";
}

/// geometry_replay's printDraw fields, as a JSON object of strings.
std::string geometryJson(const geo::CapturedDraw& d, const geo::GeometryCaptureConfig& config) {
    Obj o;
    o.str("status", std::string(geo::captureStatusName(d.status)))
        .str("tci", std::to_string(d.texcoord.texcoordIndex))
        .str("stage", std::to_string(d.texcoord.firstStage));
    if (!d.captured()) {
        return o.done();
    }
    const hash::DrawGeometryHashes g = d.geometryHashes();
    std::string f;
    for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
        f += (i ? "," : "") + h64(g.hashes.fields[i]);
    }
    o.str("f", f)
        .str("ic", std::to_string(g.indexCount))
        .str("vc", std::to_string(g.vertexCount))
        .str("min", std::to_string(g.minIndex))
        .str("max", std::to_string(g.maxIndex))
        .str("topo", std::to_string(g.topology))
        .str("it", std::to_string(g.indexType))
        .str("ps", std::to_string(g.positionStride))
        .str("key", h64(d.assetHash(config.assetRule)))
        .str("leg0", h64(hash::meshReplacementHashLegacy(g, hash::rules::kLegacyAsset0, 0)))
        .str("leg1", h64(hash::meshReplacementHashLegacy(g, hash::rules::kLegacyAsset1, 0)))
        .str("memo", d.indicesMemoized ? "1" : "0");
    if (d.boundingBox.valid()) {
        const geo::BoundingBox& bb = d.boundingBox.get();
        o.str("aabb", f32bits(bb.minPos[0]) + "," + f32bits(bb.minPos[1]) + "," + f32bits(bb.minPos[2]) + "," +
                          f32bits(bb.maxPos[0]) + "," + f32bits(bb.maxPos[1]) + "," + f32bits(bb.maxPos[2]));
    }
    if (d.skinning.valid()) {
        const geo::SkinningData& s = d.skinning.get();
        o.str("skin", std::to_string(s.numBones) + ":" + std::to_string(s.numBonesPerVertex) + ":" +
                          std::to_string(s.minBoneIndex) + ":" + h64(s.boneHash));
    } else {
        o.str("skin", "-");
    }
    return o.done();
}

std::string classificationJson(const scene::DrawClassification& r) {
    return Obj()
        .u("draw_call_id", r.drawCallId)
        .str("status", scene::geometryStatusName(r.status))
        .str("reason", scene::classifyReasonName(r.reason))
        .b("inject", r.triggerRtxInjection)
        .str("categories", r.categories.toString())
        .str("decision", decisionName(scene::toTapDecision(r.prepareFlags)))
        .b("sky_auto", r.skyAutoDetected)
        .b("using_rt_rt", r.isUsingRaytracedRenderTarget)
        .b("drawing_to_rt_rt", r.isDrawingToRaytracedRenderTarget)
        .str("color_texture", hex0x(r.colorTextureHash))
        .done();
}

} // namespace

// ---- configuration ----------------------------------------------------------------------------------

CaptureTapConfig CaptureTapConfig::fromOptions() {
    CaptureTapConfig c;
    c.texture = tex::textureTrackerConfigFromOptions();
    c.geometry = geo::GeometryCaptureConfig::fromOptions();
    return c;
}

geo::GeometryCaptureConfig CaptureTap::wireGeometry(geo::GeometryCaptureConfig config) {
    // Texture facts come from TextureTracker (Remix keeps them on the DxvkImage): a texture has a
    // hash once RL-1.4 set one (upload, inheritance, render-target counter). Hooks the caller set
    // explicitly are kept.
    if (!config.textureHashKnown) {
        config.textureHashKnown = [this](ResourceId id) { return m_textures.imageHash(id) != hash::kEmptyHash; };
    }
    if (!config.isLightmapTexture) {
        config.isLightmapTexture = [this](ResourceId id) {
            const hash::Hash64 h = m_textures.imageHash(id);
            return h != hash::kEmptyHash && scene::ClassifyOptions::lightmapTextures.containsHash(h);
        };
    }
    if (!config.ignoreBakedLightingTexture) {
        config.ignoreBakedLightingTexture = [this](ResourceId id) {
            const hash::Hash64 h = m_textures.imageHash(id);
            return h != hash::kEmptyHash && scene::ClassifyOptions::ignoreBakedLightingTextures.containsHash(h);
        };
    }
    return config;
}

CaptureTap::CaptureTap(CaptureTapConfig config)
    : m_forward(std::move(config.forward)),
      m_textures(config.texture, &m_registry),
      m_classify(nullptr,
                 [this](const scene::ClassifiedDraw& d) {
                     m_lastClassified = d;
                     m_haveClassified = true;
                 }),
      m_geometry(wireGeometry(std::move(config.geometry))) {
    m_geometry.setDrawSink([this](const geo::CapturedDrawPtr& d) { m_lastGeometry = d; });
    if (!config.path.empty()) {
        m_file = std::fopen(config.path.c_str(), "wb");
        if (!m_file) {
            std::fprintf(stderr, "fuse-relight: capture tap cannot open '%s'\n", config.path.c_str());
        }
    }
    writeLine(Obj()
                  .str("ev", "header")
                  .str("schema", "fuse.relight.capture/1")
                  .u("interface_version", kTapInterfaceVersion)
                  .done());
}

CaptureTap::~CaptureTap() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_destroyed) {
        flushFrame(true);
    }
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void CaptureTap::setFrameSink(FrameSink sink) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

void CaptureTap::writeLine(const std::string& line) {
    if (!m_file) {
        return;
    }
    std::fwrite(line.data(), 1, line.size(), m_file);
    std::fputc('\n', m_file);
}

// ---- events -----------------------------------------------------------------------------------------

void CaptureTap::onDeviceCreate(const DeviceEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onDeviceCreate(e);
    }
    m_textures.onDeviceCreate(e);
    m_classify.onDeviceCreate(e);
    m_geometry.onDeviceCreate(e);
}

void CaptureTap::onDeviceReset(const DeviceEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onDeviceReset(e);
    }
    m_textures.onDeviceReset(e);
    m_classify.onDeviceReset(e);
    m_geometry.onDeviceReset(e);
}

void CaptureTap::onDeviceDestroy() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onDeviceDestroy();
    }
    flushFrame(true);
    m_textures.onDeviceDestroy();
    m_classify.onDeviceDestroy();
    m_geometry.onDeviceDestroy();
    writeLine(Obj().str("ev", "device_destroy").u("frame", m_frame).u("draws", m_drawCount).done());
    if (m_file) {
        std::fflush(m_file);
    }
    m_destroyed = true;
}

void CaptureTap::onTextureCreate(const TextureDesc& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onTextureCreate(d);
    }
    m_textures.onTextureCreate(d);
    m_classify.onTextureCreate(d);
    m_geometry.onTextureCreate(d);
}

void CaptureTap::onTextureUpload(const TextureUpload& u) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onTextureUpload(u);
    }
    m_textures.onTextureUpload(u);
    m_classify.onTextureUpload(u);
    m_geometry.onTextureUpload(u);
}

void CaptureTap::onTextureCopy(const TextureCopy& c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onTextureCopy(c);
    }
    m_textures.onTextureCopy(c);
    m_classify.onTextureCopy(c);
    m_geometry.onTextureCopy(c);
}

void CaptureTap::onTextureWriteLock(const TextureWriteLock& l) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onTextureWriteLock(l);
    }
    m_textures.onTextureWriteLock(l);
    m_classify.onTextureWriteLock(l);
    m_geometry.onTextureWriteLock(l);
}

void CaptureTap::onImageDestroy(const ImageDestroy& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onImageDestroy(d);
    }
    m_textures.onImageDestroy(d);
    m_classify.onImageDestroy(d);
    m_geometry.onImageDestroy(d);
}

void CaptureTap::onBufferCreate(const BufferDesc& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onBufferCreate(d);
    }
    m_textures.onBufferCreate(d);
    m_classify.onBufferCreate(d);
    m_geometry.onBufferCreate(d);
}

void CaptureTap::onBufferWrite(const BufferWrite& w) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onBufferWrite(w);
    }
    m_textures.onBufferWrite(w);
    m_classify.onBufferWrite(w);
    m_geometry.onBufferWrite(w); // keeps the written bytes (GeometryCaptureConfig::shadowBuffers)
}

void CaptureTap::onBufferDestroy(ResourceId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onBufferDestroy(id);
    }
    m_textures.onBufferDestroy(id);
    m_classify.onBufferDestroy(id);
    m_geometry.onBufferDestroy(id);
}

void CaptureTap::syncClassifierTextures(const DrawState& state) {
    // The classifier reads Remix's texture hashes (DxvkImage::getHash): TextureTracker's, now that
    // it has flushed the managed textures this draw samples.
    scene::D3DStateTracker& tracker = m_classify.tracker();
    auto sync = [&](ResourceId id) {
        if (id != kNoResource) {
            tracker.setTextureHash(id, m_textures.imageHash(id));
        }
    };
    for (ResourceId id : state.textures) {
        sync(id);
    }
    for (ResourceId id : state.renderTargets) {
        sync(id);
    }
}

DrawDecision CaptureTap::onDraw(const DrawCall& call, const DrawState& state) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onDraw(call, state);
    }
    m_textures.onDraw(call, state); // flushes (hashes) the managed textures the draw samples
    syncClassifierTextures(state);
    m_haveClassified = false;
    m_classify.onDraw(call, state);
    m_lastGeometry.reset();
    m_geometry.onDraw(call, state);

    CaptureDrawRecord r;
    r.n = m_drawCount++;
    r.frame = m_frame;
    r.drawInFrame = m_drawInFrame++;
    r.geometry = std::move(m_lastGeometry);
    if (m_haveClassified) {
        r.classification = m_lastClassified.result;
    }
    for (std::uint32_t slot = 0; slot < kSamplerSlotCount; ++slot) {
        if (const ResourceId id = state.textures[slot]; id != kNoResource) {
            r.textures.push_back({slot, id, m_textures.imageHash(id), m_textures.descriptorHash(id)});
        }
    }
    m_pending.push_back(std::move(r));
    // Advisory until Relight renders: DXVK keeps drawing everything.
    return DrawDecision::Raster;
}

bool CaptureTap::substituteVertexShader(const ShaderModule& m, std::vector<std::uint32_t>& replacement) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_forward ? m_forward->substituteVertexShader(m, replacement) : false;
}

void CaptureTap::onQueryBegin(const QueryEvent& q) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onQueryBegin(q);
    }
    m_classify.onQueryBegin(q);
}

void CaptureTap::onQueryEnd(const QueryEvent& q) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onQueryEnd(q);
    }
    m_classify.onQueryEnd(q);
}

void CaptureTap::onClear(const ClearEvent& c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onClear(c);
    }
    m_classify.onClear(c);
}

void CaptureTap::onSetRenderTarget(const SetRenderTargetEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onSetRenderTarget(e);
    }
    m_classify.onSetRenderTarget(e);
}

void CaptureTap::onInjectPoint(const FrameEvent& f) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onInjectPoint(f);
    }
    m_classify.onInjectPoint(f);
}

void CaptureTap::onPresent(const FrameEvent& f) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_forward) {
        m_forward->onPresent(f);
    }
    m_textures.onPresent(f);
    m_classify.onPresent(f);
    m_geometry.onPresent(f);
    flushFrame(false);
    ++m_frame;
    m_drawInFrame = 0;
    if (m_file) {
        std::fflush(m_file);
    }
}

// ---- the per-frame record -----------------------------------------------------------------------------

void CaptureTap::flushFrame(bool final) {
    // Geometry jobs finish here (JobFuture::get waits), off the draw call's path when the
    // JobScheduler runs them on workers.
    for (CaptureDrawRecord& r : m_pending) {
        if (r.geometry && r.geometry->captured()) {
            scene::DrawClassifier::applyGeometryCategories(r.classification,
                                                           r.geometry->assetHash(m_geometry.config().assetRule));
        }
    }
    if (m_sink && (!m_pending.empty() || !final)) {
        m_sink(m_frame, m_pending);
    }
    std::uint64_t captured = 0;
    for (const CaptureDrawRecord& r : m_pending) {
        std::vector<std::string> textures;
        for (const CaptureDrawRecord::BoundTexture& t : r.textures) {
            textures.push_back(Obj()
                                   .u("slot", t.slot)
                                   .u("texture", t.texture)
                                   .str("hash", H64(t.imageHash))
                                   .str("desc", H64(t.descriptorHash))
                                   .done());
        }
        if (r.geometry && r.geometry->captured()) {
            ++captured;
        }
        writeLine(Obj()
                      .str("ev", "draw")
                      .u("n", r.n)
                      .u("frame", r.frame)
                      .u("di", r.drawInFrame)
                      .raw("geometry", r.geometry ? geometryJson(*r.geometry, m_geometry.config()) : "null")
                      .raw("textures", array(textures))
                      .raw("classification", classificationJson(r.classification))
                      .done());
    }
    if (final && m_pending.empty()) {
        return;
    }
    std::vector<std::string> live;
    for (const tex::TrackedTexture& t : m_textures.textures()) {
        const bool registered = t.image.valid() && m_registry.lookup(t.image).has_value() &&
                                m_registry.lookup(t.image)->imageHash == t.imageHash;
        live.push_back(Obj()
                           .u("id", t.desc.id)
                           .str("hash", H64(t.imageHash))
                           .str("desc", H64(t.descriptorHash))
                           .str("origin", tex::hashOriginName(t.origin))
                           .u("from", t.inheritedFrom)
                           .u("pending", t.flushPending ? 1 : 0)
                           .u("obsolete", t.obsoleteHash ? 1 : 0)
                           .str("preview", H64(m_textures.previewImageHash(t.desc.id)))
                           .u("registered", registered ? 1 : 0)
                           .done());
    }
    writeLine(Obj().str("ev", "textures").u("frame", m_frame).raw("textures", array(live)).done());
    writeLine(Obj()
                  .str("ev", "frame")
                  .u("frame", m_frame)
                  .u("draws", m_pending.size())
                  .u("captured", captured)
                  .b("presented", !final)
                  .done());
    m_pending.clear();
}

// ---- the device factory -----------------------------------------------------------------------------

std::unique_ptr<IRelightTap> createTapForDevice(const RuntimeConfig& config, unsigned deviceOrdinal) {
    if (!config.relightEnabled || config.tapMode != TapMode::Capture) {
        return createTap(config, deviceOrdinal);
    }
    CaptureTapConfig c = CaptureTapConfig::fromOptions();
    c.path = devicePath(config.capturePath, deviceOrdinal);
    if (config.captureRecord) {
        c.forward = std::make_unique<RecordingTap>(devicePath(config.recordPath, deviceOrdinal));
    }
    return std::make_unique<CaptureTap>(std::move(c));
}

} // namespace fuse::relight::tap
