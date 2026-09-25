// FUSE Relight RL-5.7: the path tracer's post hook (see pt_post.hpp).
#include <fuse/relight/render/post/pt_post.hpp>

#include <fuse/relight/render/frame/renderer_context.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_upscaler.hpp>

namespace fuse::relight::render::post {

struct PtPostHook::Binding {
    renderer::fsr3::Fsr3BackendBinding fsr3{};
};

PtPostHook::PtPostHook() : m_pipeline(std::make_unique<PostPipeline>()), m_binding(std::make_unique<Binding>()) {}

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
}

void PtPostHook::detach() {
    if (m_pipeline) {
        m_pipeline->unbindFsr3();
    }
    m_pipeline = std::make_unique<PostPipeline>();
    m_context = nullptr;
    m_enabled = false;
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

const char* PtPostHook::recordJson() const { return m_enabled ? m_pipeline->recordJson() : ""; }

} // namespace fuse::relight::render::post
