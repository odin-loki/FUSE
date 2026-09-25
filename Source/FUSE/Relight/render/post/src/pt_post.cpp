// FUSE Relight RL-5.7: the path tracer's post hook (see pt_post.hpp).
#include <fuse/relight/render/post/pt_post.hpp>

#include <fuse/relight/render/frame/renderer_context.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_upscaler.hpp>

#if defined(FUSE_RELIGHT_DENOISE)
#include <fuse/relight/render/denoise/pt_denoise.hpp> // RL-5.5 (render/denoise links it in)
#endif

#include <cstdio>

namespace fuse::relight::render::post {

struct PtPostHook::Binding {
    renderer::fsr3::Fsr3BackendBinding fsr3{};
};

/// RL-5.5: the path tracer's denoiser (pt_denoise.hpp).
struct PtPostHook::Denoise {
#if defined(FUSE_RELIGHT_DENOISE)
    denoise::PtDenoiser den;
    denoise::PtDenoiseConfig config{};
    denoise::PtDenoiseSelection selection{};
#endif
    bool ran = false;       ///< this frame's denoised channels exist
    const char* status = "off";
};

PtPostHook::PtPostHook()
    : m_pipeline(std::make_unique<PostPipeline>()), m_binding(std::make_unique<Binding>()),
      m_denoise(std::make_unique<Denoise>()) {}

PtPostHook::~PtPostHook() { detach(); }

void PtPostHook::attach(frame::RendererContext& context, renderer::rg::Executor* executor) {
    detach();
    registerPostOptions();
    m_enabled = PostOptions::enable();
    m_context = &context;
    if (!m_enabled) {
        return;
    }
    m_binding->fsr3.device = &context.device();
    m_binding->fsr3.allocator = &context.allocator();
    m_binding->fsr3.executor = executor;
    (void)m_pipeline->bindFsr3(m_binding->fsr3); // false: "fsr3" requests fall back with the reason recorded
#if defined(FUSE_RELIGHT_DENOISE)
    Denoise& d = *m_denoise;
    d.config = denoise::PtDenoiseConfig::fromOptions();
    d.status = d.config.enabled ? "configured" : "off";
    if (d.config.enabled) {
        denoise::PtDenoiserDesc dd{};
        dd.device = &context.device();
        dd.allocator = &context.allocator();
        dd.bindless = &context.bindless();
        dd.framesInFlight = 2;
        if (d.den.init(dd)) {
            d.den.setSettings(d.config.settings);
        } else {
            d.status = d.den.reason(); // the frames go through undenoised; the record says why
        }
    }
#endif
}

void PtPostHook::detach() {
    if (m_pipeline) {
        m_pipeline->unbindFsr3();
    }
    m_pipeline = std::make_unique<PostPipeline>();
#if defined(FUSE_RELIGHT_DENOISE)
    m_denoise->den.destroy(); // the frame renderer waited for its frames before detaching
#endif
    m_denoise->ran = false;
    m_denoise->status = "off";
    m_context = nullptr;
    m_enabled = false;
}

bool PtPostHook::denoising() const {
#if defined(FUSE_RELIGHT_DENOISE)
    return m_enabled && m_denoise->config.enabled && m_denoise->den.valid() &&
           m_pipeline->selection().backend != "dlss_rr";
#else
    return false;
#endif
}

u32 PtPostHook::frameSeed(u64 serial, u32 seed) const {
    // The parameter words carry 24 bits of the seed; 0 stays reserved for "no frame".
    return denoising() ? static_cast<u32>(serial % 0xFFFFFFull) + 1u : seed;
}

bool PtPostHook::addDenoisePasses(renderer::rg::Graph& graph, const pathtrace::PathTracerGpu& gpu,
                                  const pathtrace::PtGraphRefs& refs, const pathtrace::PtCompiledScene& scene,
                                  const pathtrace::PtFrameDesc& frame, u64 serial) {
    m_denoise->ran = false;
#if defined(FUSE_RELIGHT_DENOISE)
    Denoise& d = *m_denoise;
    if (!m_enabled || !d.config.enabled) {
        return true;
    }
    d.selection = denoise::selectPtDenoiser(d.config, m_pipeline->selection().backend,
                                            renderer::denoise::DenoiserRegistry::instance());
    if (!d.selection.active || !d.den.valid()) {
        d.status = d.selection.backend == "dlss_rr" ? "dlss_rr" : d.status;
        return true;
    }
    if (frame.accumulate && m_pipeline->config().useAccumulation) {
        d.den.reset(); // the post shows the accumulated mean; the history restarts with the next moving frame
        d.status = "accumulating";
        return true;
    }
    if (!d.den.beginFrame(serial, gpu, scene, frame) || !d.den.addPasses(graph, gpu, refs)) {
        m_error = "RL-5.5 denoiser frame setup failed";
        d.status = "failed";
        return false;
    }
    d.status = "ran";
    d.ran = true;
    return true;
#else
    (void)graph;
    (void)gpu;
    (void)refs;
    (void)scene;
    (void)frame;
    (void)serial;
    return true;
#endif
}

void PtPostHook::collectDenoise(u64 completedSerial) {
#if defined(FUSE_RELIGHT_DENOISE)
    if (m_denoise->den.valid()) {
        m_denoise->den.collectRetired(completedSerial);
    }
#else
    (void)completedSerial;
#endif
}

void PtPostHook::renderExtent(u32 width, u32 height, u32& renderWidth, u32& renderHeight) {
    renderWidth = width;
    renderHeight = height;
    if (!m_enabled) {
        return;
    }
    const PostSelection& s = m_pipeline->selection();
    if (!m_pipeline->configured() || s.displayWidth != width || s.displayHeight != height) {
        if (!m_pipeline->configure(PostConfig::fromOptions(height), width, height)) {
            m_error = m_pipeline->error();
            return;
        }
    }
    renderWidth = m_pipeline->renderWidth();
    renderHeight = m_pipeline->renderHeight();
}

bool PtPostHook::process(const void* outputs, u64 stride, u32 renderWidth, u32 renderHeight, bool accumulated,
                         float exposure, u8* pixels, u32 width, u32 height, bool bgra) {
    PtPostInput in;
    in.outputs = outputs;
    in.stride = stride;
    in.renderWidth = renderWidth;
    in.renderHeight = renderHeight;
    in.accumulated = accumulated;
    in.exposure = exposure;
#if defined(FUSE_RELIGHT_DENOISE)
    if (m_denoise->ran) {
        in.denoisedDiffuse = m_denoise->den.denoisedDiffuse();
        in.denoisedSpecular = m_denoise->den.denoisedSpecular();
    }
#endif
    PostTarget t;
    t.pixels = pixels;
    t.width = width;
    t.height = height;
    t.bgra = bgra;
    const bool device = m_pipeline->usesDevice() && m_context != nullptr;
    if (device) {
        m_context->lockQueue();
    }
    const bool ok = m_pipeline->process(in, t);
    if (device) {
        m_context->unlockQueue();
    }
    m_error = ok ? "" : m_pipeline->error();
    return ok;
}

const char* PtPostHook::recordJson() const {
    if (!m_enabled) {
        return "";
    }
#if defined(FUSE_RELIGHT_DENOISE)
    const Denoise& d = *m_denoise;
    std::snprintf(m_json, sizeof(m_json),
                  "%s,\"denoise\":{\"status\":\"%s\",\"backend\":\"%s\",\"reason\":\"%s\",\"ran\":%s,"
                  "\"history\":%s,\"gradients\":%s,\"kernel\":\"%s\"}",
                  m_pipeline->recordJson(), d.status, d.selection.backend.c_str(), d.selection.reason.c_str(),
                  d.ran ? "true" : "false", d.den.stats().history ? "true" : "false",
                  d.den.stats().gradients ? "true" : "false", d.den.kernelLanguage());
    return m_json;
#else
    return m_pipeline->recordJson();
#endif
}

} // namespace fuse::relight::render::post
