// FUSE Relight RL-5.7: the post pipeline inside relight.frame.mode = pathtrace (the hook RL-5.1's PathTraceFrameRenderer
// calls when it is built with FUSE_RELIGHT_POST; docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8 steps 7-9).
//
// PtPostHook owns a PostPipeline configured from the options (relight.post.enable, rtx.upscalerType, ...):
//   attach(context, executor)   binds fsr3 to the frame renderer's device / executor (FUSE_UPSCALER_FSR3 builds)
//   renderExtent(w, h, rw, rh)  the path tracer renders at the selected upscaler's render extent (w x h when disabled);
//                               configures on the first call and on an output-size change
//   process(...)                composite -> upscale -> Look -> the swapchain-sized 8-bit present slot; takes the host
//                               queue lock around a device backend (fsr3)
//   recordJson()                ,"post":{...} appended to the frame record (+ ,"denoise":{...} with RL-5.5)
// RL-5.5 (built with FUSE_RELIGHT_DENOISE, render/denoise): the path tracer's denoiser runs inside the same hook when
// relight.post.enable, rtx.useDenoiser and relight.denoise.enable are set (pt_denoise.hpp):
//   frameSeed(serial, seed)     a fresh frame seed per frame while denoising (the temporal filter needs independent
//                               noise), `seed` otherwise
//   addDenoisePasses(...)       after "relight.pt.trace": the A-SVGF producer + the rdn.* passes (+ a HostRead); a
//                               frame that continues the accumulation (relight.post.useAccumulation) or runs dlss_rr
//                               adds none
//   collectDenoise(serial)      after the frame completed
//   process(...)                composites the denoised diffuse / specular channels of this frame when they ran
#pragma once

#include <fuse/relight/render/post/post_pipeline.hpp>

#include <memory>

namespace fuse::renderer::rg {
class Executor;
class Graph;
} // namespace fuse::renderer::rg
namespace fuse::relight::render::frame {
class RendererContext;
}
namespace fuse::relight::render::pathtrace {
class PathTracerGpu;
class PtCompiledScene;
struct PtGraphRefs;
struct PtFrameDesc;
} // namespace fuse::relight::render::pathtrace

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

    // --- RL-5.5 denoiser (no-ops without FUSE_RELIGHT_DENOISE) ---------------------------------------------------------
    /// The denoiser is configured to run (post enabled, rtx.useDenoiser, relight.denoise.enable, not dlss_rr).
    bool denoising() const;
    u32 frameSeed(u64 serial, u32 seed) const;
    /// False only on a denoiser failure (error()); true when it does not run this frame.
    bool addDenoisePasses(renderer::rg::Graph& graph, const pathtrace::PathTracerGpu& gpu, const pathtrace::PtGraphRefs& refs,
                          const pathtrace::PtCompiledScene& scene, const pathtrace::PtFrameDesc& frame, u64 serial);
    void collectDenoise(u64 completedSerial);

private:
    bool m_enabled = false;
    const char* m_error = "";
    frame::RendererContext* m_context = nullptr;
    std::unique_ptr<PostPipeline> m_pipeline;
    struct Binding;
    std::unique_ptr<Binding> m_binding;
    struct Denoise; ///< RL-5.5
    std::unique_ptr<Denoise> m_denoise;
    mutable char m_json[1536] = {};
};

} // namespace fuse::relight::render::post
