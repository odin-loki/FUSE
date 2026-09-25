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
#if defined(FUSE_RELIGHT_HAVE_RASTER)
#include <fuse/relight/render/raster/raster_renderer.hpp>
#endif
#if defined(FUSE_RELIGHT_HAVE_PATHTRACE)
#include <fuse/relight/render/pathtrace/pt_frame_renderer.hpp>
#endif

#include <cinttypes>
#include <string_view>

namespace fuse::relight::replace {
class CaptureReplaceProcessor;
}

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

/// The replacement engine's processor itself (unwrapping the logic runtime); null when replacements are off.
replace::CaptureReplaceProcessor* replaceProcessor([[maybe_unused]] tap::IFrameProcessor* processor) {
#if defined(FUSE_RELIGHT_HAVE_REPLACE)
#if defined(FUSE_RELIGHT_HAVE_LOGIC)
    if (auto* logic = dynamic_cast<logic::LogicFrameProcessor*>(processor)) {
        processor = logic->inner();
    }
#endif
    return dynamic_cast<replace::CaptureReplaceProcessor*>(processor);
#else
    return nullptr;
#endif
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

/// The replacement engine's material for a legacy material hash (RL-3.2 via RL-3.4's index); null without one.
const replace::MaterialDef* replacementMaterial([[maybe_unused]] tap::IFrameProcessor* processor,
                                                [[maybe_unused]] hash::Hash64 materialHash) {
#if defined(FUSE_RELIGHT_HAVE_REPLACE)
#if defined(FUSE_RELIGHT_HAVE_LOGIC)
    if (auto* logic = dynamic_cast<logic::LogicFrameProcessor*>(processor)) {
        processor = logic->inner();
    }
#endif
    if (auto* rp = dynamic_cast<replace::CaptureReplaceProcessor*>(processor)) {
        const replace::ReplacementIndex::MaterialHit* hit = rp->engine().index().material(materialHash);
        return hit ? hit->def : nullptr;
    }
#endif
    return nullptr;
}

std::uint32_t viewTypeOf(const tap::TextureDesc& d) {
    return d.type == 4u ? 2u : (d.type == 5u ? 3u : 1u); // volume -> 3D, cube -> CUBE, else 2D
}

} // namespace

static std::unique_ptr<IFrameRenderer> makeFrameRenderer([[maybe_unused]] const FrameConfig& config) {
#if defined(FUSE_RELIGHT_HAVE_RASTER)
    if (config.mode == FrameMode::Raster) {
        return raster::createRasterRenderer(config);
    }
#endif
#if defined(FUSE_RELIGHT_HAVE_PATHTRACE)
    if (config.mode == FrameMode::PathTrace) {
        return pathtrace::createPathTraceRenderer(config); // RL-5.1
    }
#endif
    return nullptr;
}

RenderTap::RenderTap(std::unique_ptr<tap::CaptureTap> capture, FrameConfig config)
    : m_capture(std::move(capture)),
      m_config(std::move(config)),
      m_bindless(std::make_unique<BindlessImageRegistry>(m_heap, nullptr)),
      m_orchestrator(m_config, m_bindless.get()),
      m_frameRenderer(makeFrameRenderer(m_config)),
      m_scene(std::make_unique<Scene>()) {
    inst::registerInstanceOptions();
    m_scene->assetRule = capture::geometry::GeometryCaptureConfig::fromOptions().assetRule;
    m_capture->setFrameSink(
        [this](std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) { onFlushedFrame(frame, draws); });
    if (!m_config.statsPath.empty()) {
        m_stats = std::fopen(m_config.statsPath.c_str(), "w");
    }
}

RenderTap::~RenderTap() {
    releaseDevice();
    if (m_stats) {
        std::fclose(m_stats);
    }
}

IGpuSceneSink* RenderTap::sink() {
    if (m_externalSink) {
        return m_externalSink;
    }
    // The last flush at device destruction feeds nothing to the GPU (it is torn down next).
    return m_renderer && m_renderer->attached() && !m_destroying ? &m_renderer->sink() : nullptr;
}

void RenderTap::releaseDevice() {
    // Waits for the host and FUSE (orchestrator.detach), then drops every GPU object in dependency order.
    m_orchestrator.detach();
    if (m_frameRenderer) {
        m_frameRenderer->detach();
    }
    if (m_bindless) {
        m_bindless->releaseAll();
    }
    if (m_externalSink) {
        m_externalSink->clear();
    }
    if (m_renderer) {
        m_renderer->detach();
    }
    m_bindless = std::make_unique<BindlessImageRegistry>(m_heap, nullptr);
    m_orchestrator.setBindless(m_bindless.get());
}

void RenderTap::writeHeader() {
    if (!m_stats || m_headerWritten) {
        return;
    }
    m_headerWritten = true;
    const LoaderReport lr = RendererContext::loaderReport();
    const RendererContextStats* rs = m_renderer ? &m_renderer->stats() : nullptr;
    std::string raster = m_frameRenderer ? m_frameRenderer->headerJson() : std::string();
    std::fprintf(m_stats,
                 "{\"ev\":\"header\",\"schema\":\"fuse.relight.frame/2\",\"mode\":\"%s\",\"texture_swap\":%s,"
                 "\"inject_at_ui\":%s,\"solid_color\":\"%06" PRIx32 "\","
                 "\"loader\":{\"auto_init\":%s,\"loaded_before_attach\":%s,\"proc_addr\":%s,\"loaded\":%s},"
                 "\"renderer\":{\"adopted\":%s,\"device\":\"%s\",\"bindless\":\"%s\",\"gpu_descriptors\":%s,"
                 "\"gpu_scene\":%s,\"tier\":%u,\"hardware_tier\":%u,\"error\":\"%s\"},\"raster\":{%s}}\n",
                 frameModeName(m_config.mode), m_config.textureSwap ? "true" : "false",
                 m_config.injectAtUi ? "true" : "false", m_config.solidColor, lr.autoInitLinked ? "true" : "false",
                 lr.loadedBeforeAttach ? "true" : "false", lr.throughProcAddr ? "true" : "false",
                 lr.loaded ? "true" : "false", rs && rs->adopted ? "true" : "false", rs ? rs->device.c_str() : "",
                 rs ? rs->bindlessBackend.c_str() : "none", rs && rs->gpuDescriptors ? "true" : "false",
                 rs && rs->gpuScene ? "true" : "false", rs ? rs->tier : 0u, rs ? rs->hardwareTier : 0u,
                 rs ? rs->error.c_str() : "", raster.c_str());
    std::fflush(m_stats);
}

void RenderTap::attachHost(tap::IFrameHost* host) {
    releaseDevice();
    m_host = host;
    if (host) {
        if (!m_renderer) {
            m_renderer = std::make_unique<RendererContext>();
        }
        if (m_renderer->attach(*host, [this](tap::ResourceId id) { return m_bindless ? m_bindless->shaderHandle(id) : 0u; })) {
            m_bindless = std::make_unique<BindlessImageRegistry>(m_renderer->heap(), &m_renderer->views());
            m_orchestrator.setBindless(m_bindless.get());
        } else {
            std::fprintf(stderr, "fuse-relight: frame: renderer not adopted (%s); CPU bindless slots, no GPU scene\n",
                         m_renderer->stats().error.c_str());
        }
    }
    m_orchestrator.gpu().setDumpEnabled(!m_config.dumpPath.empty());
    if (!m_orchestrator.attach(host)) {
        std::fprintf(stderr, "fuse-relight: frame: %s; frame orchestration disabled for this device\n",
                     m_orchestrator.lastError().c_str());
    }
    if (m_frameRenderer && host && m_renderer && m_renderer->attached() && m_orchestrator.attached() &&
        !m_frameRenderer->attach(*m_renderer, *host, *m_bindless)) {
        std::fprintf(stderr, "fuse-relight: frame: raster renderer unavailable (%s); frames are passthrough\n",
                     m_frameRenderer->lastError().c_str());
    }
    writeHeader();
}

void RenderTap::onDeviceCreate(const tap::DeviceEvent& e) {
    m_capture->onDeviceCreate(e);
    m_backBuffer = e.backBuffer;
    attachHost(e.host);
}

void RenderTap::onDeviceReset(const tap::DeviceEvent& e) {
    m_capture->onDeviceReset(e);
    m_backBuffer = e.backBuffer;
    if (e.host != m_host) {
        attachHost(e.host);
    }
}

void RenderTap::onDeviceDestroy() {
    m_destroying = true;
    m_capture->onDeviceDestroy(); // flushes the last frame (FrameSink)
    releaseDevice();
    m_destroying = false;
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
    m_bindless->registerImage(x, m_orchestrator.retireSerial());
    if (m_config.textureSwap) {
        m_orchestrator.swapTexture(d);
    }
}

void RenderTap::onTextureUpload(const tap::TextureUpload& u) { m_capture->onTextureUpload(u); }
void RenderTap::onTextureCopy(const tap::TextureCopy& c) { m_capture->onTextureCopy(c); }
void RenderTap::onTextureWriteLock(const tap::TextureWriteLock& l) { m_capture->onTextureWriteLock(l); }

void RenderTap::onImageDestroy(const tap::ImageDestroy& d) {
    m_capture->onImageDestroy(d);
    m_bindless->release(d.texture, m_orchestrator.retireSerial());
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
void RenderTap::onClear(const tap::ClearEvent& c) {
    m_capture->onClear(c);
    // The back buffer's clear colour: the raster remaster's background (D3DCLEAR_TARGET, render target 0).
    if ((c.flags & 1u) != 0 && (c.renderTargets[0] == m_backBuffer || m_backBuffer == tap::kNoResource)) {
        m_haveClear = true;
        m_clearColor = c.color;
    }
}
void RenderTap::onSetRenderTarget(const tap::SetRenderTargetEvent& e) { m_capture->onSetRenderTarget(e); }

void RenderTap::doInject(const char* where) {
    m_injected = true;
    m_current.inject = where;
    const bool atUi = std::string_view(where) == "ui";
    IFrameRecorder* recorder = nullptr;
    tap::IFrameProcessor* processor = m_capture->frameProcessor();
    IGpuSceneSink* s = sink();
    // The injection-time feed: the draws recorded so far are the frame's scene. With RL-3.4's engine they are
    // processed now (CaptureReplaceProcessor::processPending; its flush continues after them, same record), so the GPU
    // scene holds this frame's replaced draws and lights. Only an unknown processor keeps the feed at the flush.
    replace::CaptureReplaceProcessor* rp = replaceProcessor(processor);
    const bool feedNow = processor == nullptr || rp != nullptr;
    const bool raster = (m_config.mode == FrameMode::Raster || m_config.mode == FrameMode::PathTrace) &&
                        m_frameRenderer && m_renderer && m_renderer->attached();
    if (m_frameRenderer) {
        // Before the orchestrator's collect (in inject): per-frame views die before the images they view.
        m_frameRenderer->collect(m_orchestrator.gpu().acquireCompleted());
    }
    std::vector<AdapterLight> sceneLights; // the GPU scene's lights of this frame (inject feed)
    m_capture->visitPendingDraws([&](std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) {
        std::size_t n = draws.size();
        if (atUi) {
            n = 0;
            while (n < draws.size() && draws[n].drawInFrame < m_current.injectDraw) {
                ++n;
            }
        }
        // The flush applies the geometry categories (asset hash rules) before the frame is used; do the same.
        std::vector<scene::DrawClassification> cls(n);
        std::vector<std::uint8_t> sceneDraw(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const tap::CaptureDrawRecord& r = draws[i];
            cls[i] = r.classification;
            const bool captured = r.geometry && r.geometry->captured();
            if (captured) {
                scene::DrawClassifier::applyGeometryCategories(
                    cls[i], r.geometry->assetHash(m_capture->geometry().config().assetRule));
            }
            // Committed draws, plus the pre-transformed draws RL-1.5 translated for the raster remaster only.
            sceneDraw[i] = (r.translated && captured && (cls[i].committed() || r.translation.rasterOnly)) ? 1u : 0u;
        }
        if (feedNow) {
            m_current.scene = SceneRecord{};
            m_current.scene.sink = s != nullptr;
            m_current.scene.feed = "inject";
            if (s) {
                s->beginFrame(m_orchestrator.retireSerial());
                if (m_renderer && m_renderer->attached() && s == &m_renderer->sink()) {
                    m_renderer->setRetireSerial(m_orchestrator.retireSerial());
                }
            }
#if defined(FUSE_RELIGHT_HAVE_REPLACE)
            if (rp) {
                rp->processPending(frame, draws, n, cls);
            }
#endif
            feedDraws(draws, 0, n, &cls, s, rp ? processor : nullptr);
            const std::vector<scene::LightRecord>& gameLights = m_capture->translator().lights().frameLights();
#if defined(FUSE_RELIGHT_HAVE_REPLACE)
            sceneLights = rp ? adapterLights(rp->previewLights(gameLights)) : adapterLights(gameLights);
#else
            sceneLights = adapterLights(gameLights);
#endif
            const std::vector<AdapterLight>& lights = sceneLights;
            m_current.scene.lights = static_cast<std::uint32_t>(lights.size());
            if (s) {
                s->submitLights(lights);
                s->endFrame();
                m_current.scene.gpuInstances = s->instanceCount();
                m_current.scene.gpuFrame = frame;
            }
            m_fedAtInject = true;
            m_fedCount = n;
        }
        if (raster) {
            tap::HostImageInfo bb;
            const bool haveBb = m_host && m_host->backBufferInfo(bb);
            FrameInputs in;
            in.frame = frame;
            in.draws = &draws;
            in.count = static_cast<std::uint32_t>(n);
            in.classifications = &cls;
            in.sceneDraw = &sceneDraw;
            in.lights = &m_capture->translator().lights().frameLights();
            in.sceneLights = feedNow ? &sceneLights : nullptr;
            in.haveClear = m_haveClear;
            in.clearColor = m_clearColor;
            in.capture = m_capture.get();
            in.backBuffer = haveBb ? &bb : nullptr;
            if (processor) {
                in.replacementMaterial = [processor](hash::Hash64 h) { return replacementMaterial(processor, h); };
            }
            if (m_frameRenderer->prepare(in, m_orchestrator.retireSerial())) {
                recorder = m_frameRenderer.get();
            }
        }
    });
    m_current.result = m_orchestrator.inject(recorder);
    if (!m_current.result.injected) {
        m_current.inject = m_config.mode == FrameMode::Off ? "none" : "failed";
    }
    if (m_renderer && m_renderer->attached()) {
        m_renderer->collect(m_orchestrator.gpu().acquireCompleted());
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
    m_current.bindless = m_bindless->stats();
    if (!m_config.dumpPath.empty() && m_current.result.injected) {
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        if (m_orchestrator.gpu().readDump(m_current.result.release, rgba, w, h)) {
            if (std::FILE* f = std::fopen(m_config.dumpPath.c_str(), "wb")) {
                std::fwrite(rgba.data(), 1, rgba.size(), f);
                std::fclose(f);
            }
        }
    }
    m_current.orchestrator = m_orchestrator.stats();
    writeRecord();
    m_last = m_current;
    m_current = FrameRecord{};
    m_injected = false;
    m_fedAtInject = false;
    m_fedCount = 0;
    m_drawInFrame = 0;
}

void RenderTap::feedDraws(const std::vector<tap::CaptureDrawRecord>& draws, std::size_t begin, std::size_t end,
                          const std::vector<scene::DrawClassification>* classifications, IGpuSceneSink* toSink,
                          tap::IFrameProcessor* processor) {
    SceneRecord& rec = m_current.scene;
    for (std::size_t i = begin; i < end && i < draws.size(); ++i) {
        const tap::CaptureDrawRecord& r = draws[i];
        const scene::DrawClassification& c = classifications ? (*classifications)[i - begin] : r.classification;
        if (!r.translated || !c.committed()) {
            continue;
        }
        const bool captured = r.geometry && r.geometry->captured();
        inst::SceneDrawInput input = captured ? inst::sceneDrawInput(r.translation, *r.geometry, m_scene->assetRule)
                                              : inst::sceneDrawInput(r.translation);
        input.categories = c.categories;
        const inst::SceneDrawResult res = m_scene->model.submitDraw(input);
        tap::ResourceId color = tap::kNoResource;
        const std::int32_t slot = c.colorTextureSlots[0];
        for (const tap::CaptureDrawRecord::BoundTexture& t : r.textures) {
            if (slot >= 0 && t.slot == static_cast<std::uint32_t>(slot)) {
                color = t.texture;
            }
        }
        AdapterDraw d = adapterDraw(res, input, r.translation.material, color);
        d.replaced = replacedDraw(processor, i);
        ++rec.draws;
        rec.replaced += d.replaced && d.replaced->affected() ? 1u : 0u;
        if (toSink) {
            toSink->submit(d);
        }
    }
}

void RenderTap::onFlushedFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) {
    tap::IFrameProcessor* processor = m_capture->frameProcessor();
    SceneRecord& rec = m_current.scene;
    const scene::TranslatedFrame& tf = m_capture->lastTranslatedFrame();
    if (m_fedAtInject) {
        // The sink already has this frame (injection point); the committed draws after it join the SceneModel.
        const std::uint32_t before = rec.draws;
        feedDraws(draws, m_fedCount, draws.size(), nullptr, nullptr, processor);
        rec.lateDraws = rec.draws - before;
    } else {
        IGpuSceneSink* s = sink();
        rec = SceneRecord{};
        rec.sink = s != nullptr;
        rec.feed = "flush";
        if (s) {
            s->beginFrame(m_orchestrator.retireSerial());
            if (m_renderer && m_renderer->attached() && s == &m_renderer->sink()) {
                m_renderer->setRetireSerial(m_orchestrator.retireSerial());
            }
        }
        feedDraws(draws, 0, draws.size(), nullptr, s, processor);
        const replace::ReplacedFrame* rf = replacedFrame(processor);
        const std::vector<AdapterLight> lights = rf ? adapterLights(rf->lights) : adapterLights(tf.lights);
        rec.lights = static_cast<std::uint32_t>(lights.size());
        if (s) {
            s->submitLights(lights);
            s->endFrame();
            rec.gpuInstances = s->instanceCount();
        }
    }
    const scene::CameraState* mainCamera = nullptr;
    for (const scene::CameraState& c : tf.cameras) {
        if (c.type == scene::CameraType::Main) {
            mainCamera = &c;
        }
    }
    (void)frame;
    rec.instances = m_scene->model.endFrame(mainCamera).activeInstances;
}

void RenderTap::writeRecord() {
    if (!m_stats) {
        return;
    }
    const FrameRecord& r = m_current;
    const InjectResult& i = r.result;
    const RendererContextStats* rs = m_renderer && m_renderer->attached() ? &m_renderer->stats() : nullptr;
    const std::string raster = m_frameRenderer ? m_frameRenderer->recordJson() : std::string();
    std::fprintf(m_stats,
                 "{\"ev\":\"frame\",\"frame\":%" PRIu64 ",\"inject\":\"%s\",\"inject_draw\":%u,\"pass\":\"%s\","
                 "\"acquire\":%" PRIu64 ",\"release\":%" PRIu64 ",\"passes\":%u,\"image_barriers\":%u,"
                 "\"buffer_barriers\":%u,\"barrier_calls\":%u,"
                 "\"error\":\"%s\",\"swaps\":%u,\"swaps_rejected\":%u,\"pending_destroy\":%u,"
                 "\"bindless\":{\"live\":%u,\"external\":%u,\"owned\":%u,\"retired\":%u,\"gpu\":%s},"
                 "\"scene\":{\"draws\":%u,\"instances\":%u,\"replaced\":%u,\"lights\":%u,\"sink\":%s,"
                 "\"gpu_instances\":%u,\"feed\":\"%s\",\"gpu_frame\":%" PRIu64 ",\"late_draws\":%u},"
                 "\"renderer\":{\"adopted\":%s,\"upload_batches\":%" PRIu64 ",\"queue_locks\":%" PRIu64 "},"
                 "\"raster\":{%s}}\n",
                 r.frame, r.inject.c_str(), r.injectDraw, i.pass, i.acquire, i.release, i.submit.passes,
                 i.submit.imageBarriers, i.submit.bufferBarriers, i.submit.barrierCalls,
                 i.injected ? "" : i.error.c_str(), r.orchestrator.swaps, r.orchestrator.swapsRejected,
                 r.orchestrator.pendingDestroy, r.bindless.live, r.bindless.external, r.bindless.owned,
                 r.bindless.retired, m_bindless->heap().gpuDescriptors() ? "true" : "false", r.scene.draws,
                 r.scene.instances, r.scene.replaced, r.scene.lights, r.scene.sink ? "true" : "false",
                 r.scene.gpuInstances, r.scene.feed, r.scene.gpuFrame, r.scene.lateDraws, rs ? "true" : "false",
                 rs ? rs->uploadBatches : std::uint64_t{0}, rs ? rs->queueLocks : std::uint64_t{0}, raster.c_str());
    std::fflush(m_stats);
}

std::unique_ptr<tap::IRelightTap> attachFrameTap(std::unique_ptr<tap::CaptureTap> capture, unsigned deviceOrdinal) {
    registerFrameOptions();
    FrameConfig config = FrameConfig::fromOptions();
    if (!config.enabled()) {
        return capture;
    }
    config.statsPath = config.statsPath.empty() ? std::string() : tap::devicePath(config.statsPath, deviceOrdinal);
    config.dumpPath = config.dumpPath.empty() ? std::string() : tap::devicePath(config.dumpPath, deviceOrdinal);
    return std::make_unique<RenderTap>(std::move(capture), std::move(config));
}

} // namespace fuse::relight::render::frame
