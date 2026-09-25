// FUSE Relight RL-5.7: post and upscale wiring of the path tracer (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8 steps 7-9,
// §5.6; Remix's demodulate / composite, upscaler selection, local tone mapper and mip-bias policy).
//
//   PostPipeline post;
//   post.configure(PostConfig::fromOptions(h), displayW, displayH);   // selection + every buffer (allocates)
//   post.renderWidth() / renderHeight()                                // what the path tracer renders at
//   post.process(ptInput, target);                                     // per frame: no heap allocation (CPU backends)
//
// Per frame, at the render resolution the selected upscaler asks for:
//   1. composite    emissive + diffuse x albedoD + specular x albedoS (composite.hpp; denoised channels when given),
//                   or the accumulated mean when the path tracer continued its accumulation (relight.post.
//                   useAccumulation), x the path tracer's exposure;
//   2. upscale      temporal backends (native_taau, fsr3, dlss / dlss_rr / xess through plugins) take the linear HDR
//                   colour, view depth and UV motion (the path tracer's previous - current, negated) and write the
//                   display-resolution HDR image; then the Look runs at the display resolution;
//                   spatial backends (fsr1, nis) and the sharpener (cas) run after the Look, on the output-encoded
//                   image (their contract: tone-mapped perceptual input), so the Look runs at the render resolution;
//                   "none" renders at the output resolution;
//   3. Look         the local tone map node (rtx.tonemappingMode = Local; local_tonemap.hpp) then the renderer's
//                   LookPostChain with the graph exposure -> toneMap -> outputTransform (sRGB) and the neutral look
//                   (auto exposure off: deterministic);
//   4. pack         8-bit RGBA / BGRA of the swapchain-sized output image; non-finite values are counted and written
//                   as 0.
//
// Selection: the requested id (PostConfig::upscaler) from the process-wide UpscalerRegistry (where the runtime plugins
// register dlss / dlss_rr / xess when their providers load) or the pipeline's own registry (built-ins + fsr3 when a
// Vulkan binding was given). A missing or stub backend falls back - temporal requests to native_taau, everything else
// to fsr1 - and the reason is kept (selection().reason, the frame record). A ratio outside the backend's range is
// clamped to the range. The texture mip bias is textureMipBias(render, output) (post_config.hpp).
#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/relight/render/post/local_tonemap.hpp>
#include <fuse/relight/render/post/post_config.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/look_post_chain.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>
#include <fuse/types.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer::fsr3 {
struct Fsr3BackendBinding;
}

namespace fuse::relight::render::post {

/// The path tracer's per-frame output (PathTracerGpu's host-visible buffer: kPtOutSections sections of `stride` bytes).
struct PtPostInput {
    const void* outputs = nullptr;
    u64 stride = 0;
    u32 renderWidth = 0;
    u32 renderHeight = 0;
    bool accumulated = false; ///< this frame continued the accumulation
    float exposure = 1.f;
    const float* denoisedDiffuse = nullptr;  ///< optional f32x4 (RL-5.5)
    const float* denoisedSpecular = nullptr;
};

/// The swapchain-sized 8-bit output.
struct PostTarget {
    u8* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool bgra = false;
};

enum class PostUpscaleKind : u8 { None = 0, Temporal, Spatial };

struct PostSelection {
    std::string requested;
    std::string backend = "none"; ///< registry id actually running ("none": native)
    std::string reason;           ///< why `backend` differs from `requested` / the ratio was clamped ("" otherwise)
    PostUpscaleKind kind = PostUpscaleKind::None;
    bool fallback = false;
    u32 renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
    float mipBias = 0.f;
};

struct PostFrameStats {
    u32 frames = 0;
    u32 nonFinite = 0; ///< non-finite values of the last frame (written as 0)
    renderer::upscale::UpscaleStatus status = renderer::upscale::UpscaleStatus::Ok;
    bool lookOk = true;
    bool accumulatedInput = false;
    /// process(): max over pixels / channels of |composite - radiance| / max(|radiance|, 1e-3) against the path
    /// tracer's own radiance section (exact at 1 spp with the denoiser off; -1: accumulated input).
    double compositeMaxRel = -1.0;
};

class PostPipeline {
public:
    PostPipeline();
    ~PostPipeline();
    PostPipeline(const PostPipeline&) = delete;
    PostPipeline& operator=(const PostPipeline&) = delete;

    /// Makes "fsr3" selectable (host-array adapter on this device). Before configure(); the binding must outlive the
    /// pipeline. False when the build / device cannot run it.
    bool bindFsr3(const renderer::fsr3::Fsr3BackendBinding& binding);
    void unbindFsr3();

    /// Selects the upscaler for `displayWidth` x `displayHeight` and allocates every buffer. False (error()) when no
    /// backend at all can run.
    bool configure(const PostConfig& config, u32 displayWidth, u32 displayHeight);
    bool configured() const { return m_configured; }
    const PostConfig& config() const { return m_config; }
    const PostSelection& selection() const { return m_sel; }
    u32 renderWidth() const { return m_sel.renderWidth; }
    u32 renderHeight() const { return m_sel.renderHeight; }
    /// The selected backend runs GPU work on the bound executor (fsr3): the caller holds the queue lock around process.
    bool usesDevice() const { return m_sel.backend == renderer::upscale::kFsr3Name; }

    /// The path tracer's frame -> `target` (see the header comment).
    bool process(const PtPostInput& input, const PostTarget& target);
    /// Steps 2-4 on a render-resolution scene-linear image (depth / motion may be null: sky / static).
    bool processHdr(const math::Vec3* color, const float* depth, const math::Vec2* motion, const PostTarget& target);
    /// Drops the temporal history (next frame starts over).
    void resetHistory() { m_resetHistory = true; }

    const PostFrameStats& stats() const { return m_stats; }
    const char* error() const { return m_error; }
    /// ,"post":{...} for the frame record (writes into a fixed buffer; returns it).
    const char* recordJson() const;
    /// Tests: the last composite (render resolution) and the display-resolution scene-linear image (temporal / none).
    const std::vector<math::Vec3>& composite() const { return m_color; }
    const std::vector<math::Vec3>& displayHdr() const { return m_display; }

private:
    bool select(const PostConfig& config, u32 displayWidth, u32 displayHeight);
    bool runLook(const math::Vec3* hdr, u32 width, u32 height, math::Vec3* encoded);
    void pack(const math::Vec3* encoded, const PostTarget& target);
    void packRgba(const math::Vec4* encoded, const PostTarget& target);
    const renderer::upscale::UpscalerCaps* findCaps(const std::string& name, bool& global) const;

    PostConfig m_config{};
    PostSelection m_sel{};
    PostFrameStats m_stats{};
    bool m_configured = false;
    bool m_resetHistory = true;
    bool m_fsr3Bound = false;
    const char* m_error = "";
    renderer::upscale::UpscalerRegistry m_registry;
    std::unique_ptr<renderer::upscale::IUpscaler> m_upscaler;
    std::vector<math::Vec3> m_color;   ///< render resolution scene-linear
    std::vector<float> m_depth;        ///< render resolution
    std::vector<math::Vec2> m_motion;  ///< render resolution, current - previous UV
    std::vector<math::Vec3> m_display; ///< display resolution scene-linear (temporal)
    std::vector<math::Vec3> m_ltm;     ///< local tone map output (Look resolution)
    std::vector<math::Vec3> m_encoded; ///< Look output (Look resolution)
    std::vector<math::Vec4> m_rgbaIn;  ///< spatial input (render resolution)
    std::vector<math::Vec4> m_rgbaOut; ///< spatial output (display resolution)
    LocalToneMapper m_localToneMap;
    renderer::look::LookPostChain m_look;
    renderer::look::LookEffectGraph m_graph;
    renderer::look::LookResolved m_resolved{};
    renderer::look::Lut3D m_lut;
    mutable char m_json[1024] = {};
};

} // namespace fuse::relight::render::post
