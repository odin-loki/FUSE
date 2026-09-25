#pragma once

// Deterministic CPU-rendered reference sequences for upscaler evaluation (library fuse_upscale_refscenes,
// kernel "upscale_ref_scene" — reference_scene_kernel.hpp). Each sequence is a pure function of the frame index:
//
//   Showcase        strafing + yawing camera past pillars (disocclusion), a fast thin rod (ghosting), a rotating
//                   articulated arm (per-point "skinned" motion), rising alpha-blended particles (reactive mask),
//                   a checker + fine-stripe ground (moire) and sky (camera-only motion).
//   ThinFastObject  static camera; the thin rod sweeps across a textured backdrop at ~10 display px / frame, the
//                   arm rotates — isolates ghosting / trail rejection.
//   MoireFlight     low camera flying forward over the high-frequency ground towards the horizon with a slight
//                   yaw oscillation — texture shimmer / moire / temporal flicker.
//
// `renderFrame` produces the jittered render-resolution frame as `UpscaleInputs` (colour, depth, motion incl.
// dynamic objects and sky, reactive + transparency masks, jitter, mip-biased textures, cameras, exposure) and
// `renderGroundTruth` the native display-resolution image (stratified box supersampling, no jitter) plus display
// depth / object ids / exact disocclusion flags for the masked metrics. `renderUi` draws the separate HUD target.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::refscene {

enum class ReferenceSceneKind : u8 { Showcase = 0, ThinFastObject = 1, MoireFlight = 2 };

const char* referenceSceneLabel(ReferenceSceneKind kind);

/// Object ids in the id buffers (same values as refscene_kernel::kId*).
inline constexpr u32 kObjectSky = 0u;
inline constexpr u32 kObjectGround = 1u;
inline constexpr u32 kObjectThin = 100u;
inline constexpr u32 kObjectArm = 101u;

/// One jittered render-resolution frame with every upscaler input (owning storage).
struct ReferenceFrame {
    UpscaleResolution resolution{};
    std::vector<math::Vec3> color;
    std::vector<f32> depth;
    std::vector<math::Vec2> motion;
    std::vector<f32> reactive;
    std::vector<f32> transparency;
    std::vector<u32> object_id;
    std::vector<math::Vec4> ui; ///< Display-resolution HUD (filled when requested).
    math::Vec2 jitter_px{};
    u32 jitter_phase = 0;
    u32 jitter_phase_count = 8;
    f32 mip_bias = 0.f;
    f32 exposure = 1.f;
    u32 frame_index = 0;
    UpscaleCamera camera{};
    UpscaleCamera previous_camera{};

    /// Non-owning view of this frame as the upscaler input contract (valid while the frame lives).
    UpscaleInputs inputs() const;
};

/// Native display-resolution ground truth.
struct GroundTruthFrame {
    u32 width = 0;
    u32 height = 0;
    std::vector<math::Vec3> color;  ///< Box-filtered over samples_per_axis^2 stratified samples.
    std::vector<f32> depth;         ///< Pixel-centre linear depth (0 = sky).
    std::vector<math::Vec2> motion; ///< Pixel-centre UV motion.
    std::vector<u32> object_id;     ///< Pixel-centre object id.
    std::vector<u8> disoccluded;    ///< 1 = surface not visible from the previous camera.
};

class ReferenceScene {
public:
    explicit ReferenceScene(ReferenceSceneKind kind);

    ReferenceSceneKind kind() const { return m_kind; }
    /// Unjittered camera of `frame` (view / projection / fov / aspect).
    UpscaleCamera camera(u32 frame, f32 aspect) const;

    /// Jittered frame at `resolution.render_*` (jitter phase = frame % upscaleJitterPhaseCount, mip bias =
    /// upscaleTextureMipBias). `withUi` also draws the display-resolution HUD into `out.ui`.
    bool renderFrame(u32 frame, const UpscaleResolution& resolution, ReferenceFrame& out,
                     kernel::Backend backend = kernel::Backend::CpuParallel, bool withUi = false) const;
    /// Native ground truth at display resolution (samplesPerAxis^2 stratified samples per pixel, <= 8).
    bool renderGroundTruth(u32 frame, u32 width, u32 height, u32 samplesPerAxis, GroundTruthFrame& out,
                           kernel::Backend backend = kernel::Backend::CpuParallel) const;

    /// Display-resolution HUD (premultiplied RGBA): crosshair, a bar and tick marks with 1-px anti-aliased edges.
    static void renderUi(u32 width, u32 height, u32 frame, std::vector<math::Vec4>& out);

private:
    ReferenceSceneKind m_kind;
};

} // namespace fuse::renderer::refscene
