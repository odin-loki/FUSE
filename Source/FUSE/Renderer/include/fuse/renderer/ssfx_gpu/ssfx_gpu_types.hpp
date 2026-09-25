#pragma once

// WP-6.3 screen-space fallback on Vulkan: the records shared by the C++ host code and the compute kernels
// (shaders/ssfx/sx_common.{glsl,slang} declare the same fields in the same order; fuse_rp_ssfx_gpu_layout
// checks names, order and offsets). Vulkan-free: builds in the stub backend.
//
// Storage: every per-pixel intermediate lives in one device-local work buffer reached through buffer device
// addresses, as f32 / f32x4, so each pass computes in the CPU oracle's f32 and the parity gates compare like
// with like (the oracle runs on the read-back prepared sections). The inputs (WP-1.5 G-buffer RT0 / RT1 / RT2
// / RT4 and the WP-2.1 lit image) are bindless sampled images; the output is an RGBA16F storage image (+ an
// optional f32x4 dump).

#include <fuse/types.hpp>

namespace fuse::renderer::ssfx_gpu {

/// Workgroup edge of every kernel (8 x 8 threads, one pixel each), as the CPU kernels' kWorkgroup.
inline constexpr u32 kSsfxTile = 8u;
/// GTAO slice table size (fuse::ssfx::gtao_kernel::kMaxSlices).
inline constexpr u32 kSsfxMaxSlices = 32u;

/// SsfxFrameConstants::flags
enum SsfxFlag : u32 {
    kSsfxFlagAo = 1u << 0,          ///< GTAO ran: compose re-weights the lighting ambient term by it
    kSsfxFlagSsr = 1u << 1,         ///< SSR ran: compose adds reflection x confidence x Fresnel
    kSsfxFlagSsgi = 1u << 2,        ///< SSGI ran: compose adds the indirect radiance
    kSsfxFlagReversedZ = 1u << 3,   ///< RT4 is infinite-far reversed Z (else forward z/w in [0, 1])
    kSsfxFlagMultiBounce = 1u << 4, ///< compose applies gtao_kernel::multi_bounce per albedo channel
    kSsfxFlagSsrRoughness = 1u << 5,///< SSR gates / gloss-fades by the G-buffer roughness
    kSsfxFlagSsrContact = 1u << 6,  ///< SSR contact hardening (ssr_kernel::ContactHardening::enabled)
    kSsfxFlagAoJitter = 1u << 7,    ///< GtaoParams::jitter
    /// SSR sky fallback (SsfxGpuSettings::skyFallback with a sky source, SsfxFrameConstants::sky): a reflection ray
    /// that misses and leaves the screen or ends over a sky pixel (depth 0) returns the WP-8.2 sky radiance along its
    /// world direction (at_sky_radiance, no sun disk) at confidence 1 x the gloss fade.
    kSsfxFlagSky = 1u << 8,
    /// SSGI sky fallback (SsfxGpuSettings::ssgiSkyFallback): the same for the gather rays (every bounce).
    kSsfxFlagSkyGi = 1u << 9,
};

/// Per-frame constants, one host-visible ring slot per frame in flight, read through BDA.
struct SsfxFrameConstants {
    // --- work-buffer sections (BDA; 0 = absent) ----------------------------------------------------
    u64 prepared = 0; ///< f32x4: linear view depth (0 = sky), roughness, metallic, material AO (RT0.w)
    u64 normals = 0;  ///< f32x4: view-space normal in the ssfx convention (+X right, +Y down, +Z forward), 0
    u64 radiance = 0; ///< f32x4: lit radiance (the WP-2.1 image), 0
    u64 albedo = 0;   ///< f32x4: base albedo (RT1.rgb), 0
    u64 diffuse = 0;  ///< f32x4: diffuse albedo = albedo x (1 - metallic), 0 (the SSGI albedo)
    u64 ao = 0;       ///< f32: GTAO visibility
    u64 ssr = 0;      ///< f32x4: reflected radiance (unfaded), confidence
    u64 gi = 0;       ///< f32x4: SSGI outgoing indirect radiance, 0
    u64 bounce0 = 0;  ///< f32x4: SSGI re-lighting ping-pong (direct + indirect of the previous bounce)
    u64 bounce1 = 0;
    u64 dump = 0;     ///< f32x4: composed radiance before RGBA16F rounding (optional)
    u64 sky = 0;      ///< AtParams (WP-8.2 AtmosphereGpu::frameAddress()) of the sky fallback (kSsfxFlagSky), 0 = none
    // --- extent and bindless handles ---------------------------------------------------------------
    u32 width = 0;
    u32 height = 0;
    u32 inputDepth = 0;     ///< RT4 (R32F device depth)
    u32 inputNormal = 0;    ///< RT0 (RGBA16F signed-octahedral world normal, AO)
    u32 inputAlbedo = 0;    ///< RT1 (RGBA8 albedo)
    u32 inputRoughMetal = 0;///< RT2 (RGBA8 roughness, metallic)
    u32 inputLit = 0;       ///< lit image (RGBA16F)
    u32 output = 0;         ///< storage image (RGBA16F)
    u32 flags = 0;          ///< SsfxFlag
    u32 reserved1 = 0;
    // --- camera (fuse::ssfx::SsfxCamera + depth linearisation + world -> view rotation) -------------
    f32 fx = 1.f;
    f32 fy = 1.f;
    f32 cx = 0.f;
    f32 cy = 0.f;
    f32 nearZ = 0.01f;     ///< SsfxCamera::near_z (from the projection)
    f32 nearPlane = 0.01f; ///< device depth -> linear view depth
    f32 farPlane = 1000.f;
    f32 reserved2 = 0.f;
    f32 viewRot[12] = {};  ///< world -> view rotation, column c at [c * 4 + 0..2] (column-major view matrix)
    f32 ambient[4] = {};   ///< the lighting's ambient radiance (LightingFrameDesc::ambient)
    // --- GTAO (fuse::ssfx::GtaoParams, clamped) ------------------------------------------------------
    f32 aoRadius = 1.f;
    f32 aoFalloff = 0.f;
    f32 aoBias = 0.f;
    f32 aoStrength = 1.f;
    f32 aoMaxRadiusPx = 64.f;
    u32 aoSlices = 4;
    u32 aoSteps = 8;
    u32 aoFrame = 0;
    f32 aoSliceCos[kSsfxMaxSlices] = {};
    f32 aoSliceSin[kSsfxMaxSlices] = {};
    // --- SSR (fuse::ssfx::SsrParams, clamped + ContactHardening) ------------------------------------
    u32 ssrMaxSteps = 64;
    u32 ssrRefineSteps = 8;
    f32 ssrStride = 1.f;
    f32 ssrThickness = 0.5f;
    f32 ssrMaxDistance = 20.f;
    f32 ssrFadeEdge = 0.1f;
    f32 ssrContactDistance = 0.5f;
    f32 ssrContactFloor = 0.02f;
    f32 ssrContactExponent = 2.f;
    u32 reserved3 = 0;
    // --- SSGI (fuse::ssfx::SsgiParams, clamped) ------------------------------------------------------
    u32 giSampleSqrt = 4;
    u32 giBounces = 1;
    u32 giMaxSteps = 64;
    u32 giRefineSteps = 6;
    f32 giStride = 1.f;
    f32 giThickness = 0.5f;
    f32 giMaxDistance = 10.f;
    f32 giIntensity = 1.f;
};
static_assert(sizeof(SsfxFrameConstants) == 592u, "SsfxFrameConstants layout (sx_common mirrors it)");

/// Push constants (32 bytes).
struct SsfxPush {
    u64 frame = 0; ///< BDA of this frame's SsfxFrameConstants
    u64 src = 0;   ///< ssfx.ssgi: radiance the bounce gathers
    u64 dst = 0;   ///< ssfx.ssgi: next bounce's radiance (direct + indirect), 0 on the last bounce
    u32 mode = 0;
    u32 reserved = 0;
};
static_assert(sizeof(SsfxPush) == 32u, "SsfxPush layout");

} // namespace fuse::renderer::ssfx_gpu
