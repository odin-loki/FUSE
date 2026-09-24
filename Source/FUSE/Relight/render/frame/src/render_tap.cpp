// FUSE Relight RL-4.1: the tap that renders (see render_tap.hpp).
#include <fuse/relight/render/frame/render_tap.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>
#include <fuse/relight/tap/frame_host.hpp>
#include <fuse/relight/tap/tap_config.hpp>

#if defined(FUSE_RELIGHT_HAVE_REPLACE)
#include <fuse/relight/replace/replace_live.hpp>
#endif
#if defined(FUSE_RELIGHT_HAVE_LOGIC)
#include <fuse/relight/logic/logic_live.hpp>
#endif

#include <cinttypes>

namespace fuse::relight::render::frame {

namespace inst = fuse::relight::scene::instances;

struct RenderTap::Scene {
    inst::SceneModel model;
    hash::HashRule assetRule;
};

namespace {

/// The replacement engine's processor behind CaptureTap::frameProcessor (possibly wrapped by the logic
/// runtime); null when replacements are off.
const replace::ReplacedDraw* replacedDraw([[maybe_unused]] tap::IFrameProcessor* processor,
                                          [[maybe_unused]] std::size_t index) {
#if defined(FUSE_RELIGHT_HAVE_REPLACE)
#if defined(FUSE_RELIGHT_HAVE_LOGIC)
    if (auto* logic = dynamic_cast<logic::LogicFrameProcessor*>(processor)) {
        processor = logic->inner();
    }
#endif
    if (auto* rp = dynamic_cast<replace::CaptureReplaceProcessor*>(processor)) {
        const auto& draws = rp->lastDraws();
        if (index < draws.size() && draws[index]) {
            return &*draws[index];
        }
    }
#endif
    return nullptr;
}

const replace::ReplacedFrame* replacedFrame([[maybe_unused]] tap::IFrameProcessor* processor) {
#if defined(FUSE_RELIGHT_HAVE_REPLACE)
#if defined(FUSE_RELIGHT_HAVE_LOGIC)
    if (auto* logic = dynamic_cast<logic::LogicFrameProcessor*>(processor)) {
        processor = logic->inner();
    }
#endif
    if (auto* rp = dynamic_cast<replace::CaptureReplaceProcessor*>(processor)) {
        return &rp->lastFrame();
    }
#endif
    return nullptr;
}

std::uint32_t viewTypeOf(const tap::TextureDesc& d) {
    return d.type == 4u ? 2u : (d.type == 5u ? 3u : 1u); // volume -> 3D, cube -> CUBE, else 2D
}

} // namespace

RenderTap::RenderTap(std::unique_ptr<tap::CaptureTap> capture, FrameConfig config)
    : m_capture(std::move(capture)),
      m_config(std::move(config)),
      m_bindless(m_heap, nullptr),
      m_orchestrator(m_config, &m_bindless),
      m_scene(std::make_unique<Scene>()) {
    inst::registerInstanceOptions();
    m_scene->assetRule = capture::geometry::GeometryCaptureConfig::fromOptions().assetRule;
    m_capture->setFrameSink(
        [this](std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) { onFlushedFrame(frame, draws); });
    if (!m_config.statsPath.empty()) {
        m_stats = std::fopen(m_config.statsPath.c_str(), "w");
        if (m_stats) {
            std::fprintf(m_stats,
                         "{\"ev\":\"header\",\"schema\":\"fuse.relight.frame/1\",\"mode\":\"%s\",\"texture_swap\":%s,"
                         "\"inject_at_ui\":%s,\"solid_color\":\"%06" PRIx32 "\"}\n",
                         frameModeName(m_config.mode), m_config.textureSwap ? "true" : "false",
                         m_config.injectAtUi ? "true" : "false", m_config.solidColor);
            std::fflush(m_stats);
        }
    }
}

RenderTap::~RenderTap() {
    m_orchestrator.detach();
    m_bindless.releaseAll();
    if (m_stats) {
        std::fclose(m_stats);
    }
}

void RenderTap::attachHost(tap::IFrameHost* host) {
    m_host = host;
    if (!m_orchestrator.attach(host)) {
        std::fprintf(stderr, "fuse-relight: frame: %s; frame orchestration disabled for this device\n",
                     m_orchestrator.lastError().c_str());
    }
}

void RenderTap::onDeviceCreate(const tap::DeviceEvent& e) {
    m_capture->onDeviceCreate(e);
    attachHost(e.host);
}

void RenderTap::onDeviceReset(const tap::DeviceEvent& e) {
    m_capture->onDeviceReset(e);
    if (e.host != m_host) {
        attachHost(e.host);
    }
}

void RenderTap::onDeviceDestroy() {
    m_capture->onDeviceDestroy(); // flushes the last frame (FrameSink)
    m_orchestrator.detach();
    m_bindless.releaseAll();
    if (m_sink) {
        m_sink->clear();
    }
    if (m_stats) {
        std::fprintf(m_stats, "{\"ev\":\"device_destroy\",\"injections\":%" PRIu64 ",\"failures\":%" PRIu64 "}\n",
                     m_orchestrator.stats().injections, m_orchestrator.stats().failures);
        std::fflush(m_stats);
    }
    m_host = nullptr;
}

void RenderTap::onTextureCreate(const tap::TextureDesc& d) {
    m_capture->onTextureCreate(d);
    if (d.vkImage == 0) {
        return;
    }
    ExternalImageDesc x;
    x.texture = d.id;
    x.vkImage = d.vkImage;
    x.viewType = viewTypeOf(d);
    x.width = d.width;
    x.height = d.height;
    x.depth = d.depth ? d.depth : 1u;
    x.mipLevels = d.mipLevels ? d.mipLevels : 1u;
    x.arrayLayers = d.arraySize ? d.arraySize : 1u;
    tap::HostImageInfo info;
    if (m_host && m_host->textureInfo(d.id, info)) {
        x.vkFormat = info.format;
    }
    m_bindless.registerImage(x, m_orchestrator.retireSerial());
    if (m_config.textureSwap) {
        m_orchestrator.swapTexture(d);
    }
}

void RenderTap::onTextureUpload(const tap::TextureUpload& u) { m_capture->onTextureUpload(u); }
void RenderTap::onTextureCopy(const tap::TextureCopy& c) { m_capture->onTextureCopy(c); }
void RenderTap::onTextureWriteLock(const tap::TextureWriteLock& l) { m_capture->onTextureWriteLock(l); }

void RenderTap::onImageDestroy(const tap::ImageDestroy& d) {
    m_capture->onImageDestroy(d);
    m_bindless.release(d.texture, m_orchestrator.retireSerial());
    m_orchestrator.onTextureDestroyed(d.texture);
}

void RenderTap::onBufferCreate(const tap::BufferDesc& d) { m_capture->onBufferCreate(d); }
void RenderTap::onBufferWrite(const tap::BufferWrite& w) { m_capture->onBufferWrite(w); }
void RenderTap::onBufferDestroy(tap::ResourceId id) { m_capture->onBufferDestroy(id); }

tap::DrawDecision RenderTap::onDraw(const tap::DrawCall& call, const tap::DrawState& state) {
    scene::DrawClassifier& classifier = m_capture->classifier().classifier();
    const bool before = classifier.rtxInjectTriggered();
    const tap::DrawDecision decision = m_capture->onDraw(call, state);
    // Remix injects before the draw that triggers it (the first UI draw); DXVK records this draw after the hook.
    if (!m_injected && m_config.injectAtUi && m_config.mode != FrameMode::Off && !before &&
        classifier.rtxInjectTriggered()) {
        m_current.injectDraw = m_drawInFrame;
        doInject("ui");
    }
    ++m_drawInFrame;
    return decision;
}

bool RenderTap::substituteVertexShader(const tap::ShaderModule& m, std::vector<std::uint32_t>& replacement) {
    return m_capture->substituteVertexShader(m, replacement);
}

// RL-1.6 vertex capture (tap interface 5): the capture tap decides; substituteVertexShader arrives on DXVK's
// compile threads and is forwarded as is.
bool RenderTap::wantsVertexCapture() { return m_capture->wantsVertexCapture(); }
void RenderTap::onVertexCapture(const tap::VertexCaptureFrame& f) { m_capture->onVertexCapture(f); }

void RenderTap::onQueryBegin(const tap::QueryEvent& q) { m_capture->onQueryBegin(q); }
void RenderTap::onQueryEnd(const tap::QueryEvent& q) { m_capture->onQueryEnd(q); }
void RenderTap::onClear(const tap::ClearEvent& c) { m_capture->onClear(c); }
void RenderTap::onSetRenderTarget(const tap::SetRenderTargetEvent& e) { m_capture->onSetRenderTarget(e); }

void RenderTap::doInject(const char* where) {
    m_injected = true;
    m_current.inject = where;
    m_current.result = m_orchestrator.inject();
    if (!m_current.result.injected) {
        m_current.inject = m_config.mode == FrameMode::Off ? "none" : "failed";
    }
}

void RenderTap::onInjectPoint(const tap::FrameEvent& f) {
    m_capture->onInjectPoint(f);
    if (!m_injected) {
        doInject("present");
    }
}

void RenderTap::onPresent(const tap::FrameEvent& f) {
    m_current.frame = f.frame;
    m_capture->onPresent(f); // flushes the frame: onFlushedFrame -> GPU scene
    m_current.bindless = m_bindless.stats();
    m_current.orchestrator = m_orchestrator.stats();
    writeRecord();
    m_last = m_current;
    m_current = FrameRecord{};
    m_injected = false;
    m_drawInFrame = 0;
}

void RenderTap::onFlushedFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) {
    tap::IFrameProcessor* processor = m_capture->frameProcessor();
    SceneRecord& rec = m_current.scene;
    rec = SceneRecord{};
    rec.sink = m_sink != nullptr;
    if (m_sink) {
        m_sink->beginFrame(frame);
    }
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const tap::CaptureDrawRecord& r = draws[i];
        if (!r.translated || !r.classification.committed()) {
            continue;
        }
        const bool captured = r.geometry && r.geometry->captured();
        inst::SceneDrawInput input = captured ? inst::sceneDrawInput(r.translation, *r.geometry, m_scene->assetRule)
                                              : inst::sceneDrawInput(r.translation);
        input.categories = r.classification.categories;
        const inst::SceneDrawResult res = m_scene->model.submitDraw(input);
        tap::ResourceId color = tap::kNoResource;
        const std::int32_t slot = r.classification.colorTextureSlots[0];
        for (const tap::CaptureDrawRecord::BoundTexture& t : r.textures) {
            if (slot >= 0 && t.slot == static_cast<std::uint32_t>(slot)) {
                color = t.texture;
            }
        }
        AdapterDraw d = adapterDraw(res, input, r.translation.material, color);
        d.replaced = replacedDraw(processor, i);
        ++rec.draws;
        rec.replaced += d.replaced && d.replaced->affected() ? 1u : 0u;
        if (m_sink) {
            m_sink->submit(d);
        }
    }
    const scene::TranslatedFrame& tf = m_capture->lastTranslatedFrame();
    const replace::ReplacedFrame* rf = replacedFrame(processor);
    const std::vector<AdapterLight> lights = rf ? adapterLights(rf->lights) : adapterLights(tf.lights);
    rec.lights = static_cast<std::uint32_t>(lights.size());
    const scene::CameraState* mainCamera = nullptr;
    for (const scene::CameraState& c : tf.cameras) {
        if (c.type == scene::CameraType::Main) {
            mainCamera = &c;
        }
    }
    rec.instances = m_scene->model.endFrame(mainCamera).activeInstances;
    if (m_sink) {
        m_sink->submitLights(lights);
        m_sink->endFrame();
        rec.gpuInstances = m_sink->instanceCount();
    }
}

void RenderTap::writeRecord() {
    if (!m_stats) {
        return;
    }
    const FrameRecord& r = m_current;
    const InjectResult& i = r.result;
    std::fprintf(m_stats,
                 "{\"ev\":\"frame\",\"frame\":%" PRIu64 ",\"inject\":\"%s\",\"inject_draw\":%u,\"acquire\":%" PRIu64
                 ",\"release\":%" PRIu64 ",\"passes\":%u,\"image_barriers\":%u,\"barrier_calls\":%u,"
                 "\"error\":\"%s\",\"swaps\":%u,\"swaps_rejected\":%u,\"pending_destroy\":%u,"
                 "\"bindless\":{\"live\":%u,\"external\":%u,\"owned\":%u,\"retired\":%u,\"gpu\":%s},"
                 "\"scene\":{\"draws\":%u,\"instances\":%u,\"replaced\":%u,\"lights\":%u,\"sink\":%s,"
                 "\"gpu_instances\":%u}}\n",
                 r.frame, r.inject.c_str(), r.injectDraw, i.acquire, i.release, i.submit.passes, i.submit.imageBarriers,
                 i.submit.barrierCalls, i.injected ? "" : i.error.c_str(), r.orchestrator.swaps,
                 r.orchestrator.swapsRejected, r.orchestrator.pendingDestroy, r.bindless.live, r.bindless.external,
                 r.bindless.owned, r.bindless.retired, m_bindless.heap().gpuDescriptors() ? "true" : "false",
                 r.scene.draws, r.scene.instances, r.scene.replaced, r.scene.lights, r.scene.sink ? "true" : "false",
                 r.scene.gpuInstances);
    std::fflush(m_stats);
}

std::unique_ptr<tap::IRelightTap> attachFrameTap(std::unique_ptr<tap::CaptureTap> capture, unsigned deviceOrdinal) {
    registerFrameOptions();
    FrameConfig config = FrameConfig::fromOptions();
    if (!config.enabled()) {
        return capture;
    }
    config.statsPath = config.statsPath.empty() ? std::string() : tap::devicePath(config.statsPath, deviceOrdinal);
    return std::make_unique<RenderTap>(std::move(capture), std::move(config));
}

} // namespace fuse::relight::render::frame
