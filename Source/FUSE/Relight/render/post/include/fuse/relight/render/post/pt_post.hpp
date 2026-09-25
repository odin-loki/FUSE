// FUSE Relight RL-5.7: the post pipeline inside relight.frame.mode = pathtrace (the hook RL-5.1's PathTraceFrameRenderer
// calls when it is built with FUSE_RELIGHT_POST; docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8 steps 7-9).
//
// PtPostHook owns a PostPipeline configured from the options (relight.post.enable, rtx.upscalerType, ...):
//   attach(context, executor)   binds fsr3 to the frame renderer's device / executor (FUSE_UPSCALER_FSR3 builds)
//   renderExtent(w, h, rw, rh)  the path tracer renders at the selected upscaler's render extent (w x h when disabled);
//                               configures on the first call and on an output-size change
//   process(...)                composite -> upscale -> Look -> the swapchain-sized 8-bit present slot; takes the host
//                               queue lock around a device backend (fsr3)
//   recordJson()                ,"post":{...} appended to the frame record
#pragma once

#include <fuse/relight/render/post/post_pipeline.hpp>

#include <memory>

namespace fuse::renderer::rg {
class Executor;
}
namespace fuse::relight::render::frame {
class RendererContext;
}

namespace fuse::relight::render::post {

class PtPostHook {
public:
    PtPostHook();
    ~PtPostHook();
    PtPostHook(const PtPostHook&) = delete;
    PtPostHook& operator=(const PtPostHook&) = delete;

    void attach(frame::RendererContext& context, renderer::rg::Executor* executor);
    void detach();
    bool enabled() const { return m_enabled; }
    void renderExtent(u32 width, u32 height, u32& renderWidth, u32& renderHeight);
    bool process(const void* outputs, u64 stride, u32 renderWidth, u32 renderHeight, bool accumulated, float exposure,
                 u8* pixels, u32 width, u32 height, bool bgra);
    const char* error() const { return m_error; }
    /// "" when disabled.
    const char* recordJson() const;
    const PostPipeline& pipeline() const { return *m_pipeline; }

private:
    bool m_enabled = false;
    const char* m_error = "";
    frame::RendererContext* m_context = nullptr;
    std::unique_ptr<PostPipeline> m_pipeline;
    struct Binding;
    std::unique_ptr<Binding> m_binding;
};

} // namespace fuse::relight::render::post
