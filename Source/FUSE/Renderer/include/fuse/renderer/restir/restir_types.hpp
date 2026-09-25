#pragma once

// WP-7.2 ReSTIR DI and GI: the records shared by the C++ host code, the single-source CPU kernel
// (restir_kernel.hpp) and the compute kernels (shaders/restir/restir_common.{glsl,slang} declare the same
// fields in the same order; fuse_rp_restir_layout checks names, order, offsets and sizes).
//
// Layout rules (as light_tree_types.hpp): 4-byte scalars, fixed scalar arrays and 8-byte addresses only, no
// vec3 members, no implicit padding, every record a multiple of 16 bytes, so std430 == scalar layout and the
// GPU reads everything through buffer device addresses.
//
// Buffers (RestirBufferLayout, all sections 256-aligned):
//   state   (persistent) surface[2] x {position + linear depth f32x4, normal f32x4, albedo f32x4},
//           DI history[2] (RestirDiReservoir), GI history[2] (RestirGiReservoir); slot 0 / 1 swap every frame
//   work    DI stages (initial, temporal, spatial ...) and GI stages; ping-pong unless keepIntermediates
//   output  DI signal f32x4, GI signal f32x4 (demodulated radiance, w = 0: the WP-6.4 denoiser's RGB signal
//           layout), linear depth f32 (0 = no surface). The current surface-normal section is the denoiser's
//           f32x4 normal input.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::restir {

inline constexpr u32 kRestirTile = 8u;             ///< 8 x 8 pixels per workgroup
inline constexpr u32 kRestirMaxCandidates = 32u;   ///< initial light-tree candidates per pixel (DI)
inline constexpr u32 kRestirMaxNeighbors = 8u;     ///< spatial neighbours per iteration
inline constexpr u32 kRestirMaxSpatial = 4u;       ///< spatial iterations per chain
inline constexpr u32 kRestirStages = 2u + kRestirMaxSpatial; ///< initial, temporal, spatial 0..3
inline constexpr u32 kRestirInvalid = 0xFFFFFFFFu;
inline constexpr f32 kRestirPi = 3.14159265358979323846f;
/// Shadow rays toward a point stop at this fraction of the distance (the point may lie on traced geometry).
inline constexpr f32 kRestirShadowShorten = 0.9999f;

/// RestirFrameConstants::flags.
enum RestirFlag : u32 {
    kRestirFlagUnbiased = 1u << 0,         ///< Talbot MIS weights with visibility-tested target pdfs
    kRestirFlagHistory = 1u << 1,          ///< the previous frame's reservoirs / surfaces are valid
    kRestirFlagVisibilityReuse = 1u << 2,  ///< DI initial: W = 0 when the chosen candidate is occluded
    kRestirFlagMotion = 1u << 3,           ///< motion buffer present (else zero motion)
    kRestirFlagGiShadeVisibility = 1u << 4,///< GI shade traces the reconnection (biased mode needs it)
};

/// RestirPush::mode of the reuse kernels.
enum RestirReuseMode : u32 {
    kRestirModeTemporal = 0,
    kRestirModeSpatial = 1,
};

/// Per light-tree emitter (same index as LightTreeEmitter): RGB emission and the spot cone. 32 bytes.
///   area kinds (triangle / rect / disk): radiance = emitted radiance L (W / sr / m^2)
///   point / spot: radiance = radiant intensity I (W / sr); spot: smoothstep(cosOuter, cosInner, cos)
///   directional: radiance = irradiance at normal incidence (W / m^2)
struct RestirLight {
    f32 radiance[3] = {0.f, 0.f, 0.f};
    u32 kind = 0;          ///< light_tree::LtKind (copied from the emitter; informational)
    f32 cosInner = 1.f;    ///< spot only
    f32 cosOuter = 0.f;    ///< spot only (cosOuter >= cosInner: hard cut at cosOuter)
    u32 flags = 0;
    u32 reserved = 0;
};
static_assert(sizeof(RestirLight) == 32u, "RestirLight layout (restir_common.glsl / .slang)");

/// DI reservoir: the sample is a light-tree emitter plus the (u1, u2) that place the point on it
/// (lt_sample_point), so every pixel regenerates the same world point bit for bit. 32 bytes.
struct RestirDiReservoir {
    u32 light = kRestirInvalid; ///< emitter index (kRestirInvalid: no sample)
    f32 u1 = 0.f;
    f32 u2 = 0.f;
    f32 W = 0.f;         ///< unbiased contribution weight (area measure on the light / counting for deltas)
    f32 M = 0.f;         ///< confidence (0: no surface)
    f32 targetPdf = 0.f; ///< p-hat of the sample at the owner when it was chosen (diagnostic)
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(RestirDiReservoir) == 32u && offsetof(RestirDiReservoir, W) == 12u, "RestirDiReservoir layout");

/// GI reservoir (Ouyang et al. 2021): the secondary vertex x_s with its normal and outgoing radiance toward the
/// visible point; W is in solid angle at the owner's visible point. 48 bytes.
struct RestirGiReservoir {
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 W = 0.f;
    f32 normal[3] = {0.f, 0.f, 0.f}; ///< unit, facing the path's previous vertex
    f32 M = 0.f;
    f32 radiance[3] = {0.f, 0.f, 0.f}; ///< Lo(x_s): one-bounce next-event estimate (Lambert at x_s)
    f32 targetPdf = 0.f;
};
static_assert(sizeof(RestirGiReservoir) == 48u && offsetof(RestirGiReservoir, normal) == 16u &&
                  offsetof(RestirGiReservoir, radiance) == 32u,
              "RestirGiReservoir layout");

/// RestirGiHitRecord::flags.
enum RestirHitFlag : u32 {
    kRestirHitTraced = 1u << 0,
    kRestirHitHit = 1u << 1,
};

/// Debug record of a pixel's GI secondary ray (restir.gi.initial with a dump address). 32 bytes.
struct RestirGiHitRecord {
    f32 t = -1.f;
    u32 instance = kRestirInvalid;
    u32 primitive = kRestirInvalid;
    u32 flags = 0; ///< RestirHitFlag
    f32 direction[3] = {0.f, 0.f, 0.f};
    f32 reserved = 0.f;
};
static_assert(sizeof(RestirGiHitRecord) == 32u, "RestirGiHitRecord layout");

/// Per-frame constants, read through BDA from a host-visible ring. 336 bytes.
/// Slot convention: surf*[0] / *History[0] = this frame (written), [1] = the previous frame (read).
struct RestirFrameConstants {
    u64 surfPos[2] = {0u, 0u};    ///< f32x4 (world xyz, linear view depth; depth 0 = no surface)
    u64 surfNormal[2] = {0u, 0u}; ///< f32x4 (unit world normal, 0)
    u64 surfAlbedo[2] = {0u, 0u}; ///< f32x4 (albedo rgb, 1)
    u64 diHistory[2] = {0u, 0u};  ///< RestirDiReservoir per pixel
    u64 giHistory[2] = {0u, 0u};  ///< RestirGiReservoir per pixel
    u64 diSignal = 0;             ///< f32x4 per pixel
    u64 giSignal = 0;             ///< f32x4 per pixel
    u64 depth = 0;                ///< f32 per pixel (linear view depth, 0 = none)
    u64 motion = 0;               ///< f32x2 per pixel: UV motion current - previous (WP-4.1)
    u64 lightTree = 0;            ///< LightTreeHeader BDA (LightTreeGpu::headerAddress)
    u64 lights = 0;               ///< RestirLight[lightCount]
    u64 tlas = 0;                 ///< TLAS device address
    u64 scene = 0;                ///< GpuScene header address (GI hit attributes)
    f32 invViewProj[16] = {};     ///< column-major, Vulkan clip, forward z/w -> world
    f32 cameraPosition[3] = {0.f, 0.f, 0.f};
    u32 width = 0;
    f32 cameraForward[3] = {0.f, 0.f, -1.f}; ///< unit view axis (linear depth = dot(p - camera, forward))
    u32 height = 0;
    f32 invWidth = 0.f;
    f32 invHeight = 0.f;
    u32 frameIndex = 0;           ///< sequence index (increment every frame)
    u32 seed = 0;
    u32 flags = 0;                ///< RestirFlag
    u32 lightCount = 0;
    u32 diCandidates = 0;         ///< initial light-tree candidates (1..kRestirMaxCandidates)
    u32 diNeighbors = 0;          ///< spatial neighbours per DI iteration
    u32 giNeighbors = 0;          ///< spatial neighbours per GI iteration
    u32 gbufferNormal = 0;        ///< bindless sampled-image handles: RT0 (oct normal), RT1 (albedo), RT4 (z/w)
    u32 gbufferAlbedo = 0;
    u32 gbufferDepth = 0;
    f32 diRadius = 0.f;           ///< spatial radius in pixels
    f32 giRadius = 0.f;
    f32 diMCap = 0.f;             ///< temporal confidence cap (history M <= cap x canonical M)
    f32 giMCap = 0.f;
    f32 normalThreshold = 0.f;    ///< neighbour accepted when n . n_q >= threshold
    f32 depthThreshold = 0.f;     ///< ... and |z_q - z| <= threshold x z
    f32 normalBias = 0.f;         ///< shadow-ray origin offset: n (normalBias + viewBias |p - camera|)
    f32 viewBias = 0.f;
    f32 farDistance = 0.f;        ///< tMax of directional shadow rays and GI rays
    f32 giJacobianClamp = 0.f;    ///< biased GI: reject reconnections with J outside [1 / c, c]
    f32 giRayTMin = 0.f;          ///< tMin of the GI secondary ray (origin on the surface)
    u32 cullMask = 0;             ///< ray cull mask (within rt::kRtMaskAll)
};
static_assert(sizeof(RestirFrameConstants) == 336u && offsetof(RestirFrameConstants, invViewProj) == 144u &&
                  offsetof(RestirFrameConstants, cameraPosition) == 208u && offsetof(RestirFrameConstants, invWidth) == 240u &&
                  offsetof(RestirFrameConstants, flags) == 256u && offsetof(RestirFrameConstants, diRadius) == 288u &&
                  offsetof(RestirFrameConstants, farDistance) == 320u,
              "RestirFrameConstants layout (restir_common.glsl / .slang)");

/// Push constants of every kernel. 48 bytes.
struct RestirPush {
    u64 frame = 0; ///< BDA of this frame's RestirFrameConstants
    u64 src = 0;   ///< reuse: input reservoirs; shade: the DI final reservoirs
    u64 dst = 0;   ///< output reservoirs (initial / reuse)
    u64 aux = 0;   ///< gi.initial: RestirGiHitRecord[] dump (0 = none); shade: the GI final reservoirs
    u32 mode = 0;  ///< RestirReuseMode
    u32 iteration = 0;
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(RestirPush) == 48u, "RestirPush layout");

/// Byte offsets of the three buffers for an extent (sections 256-aligned).
struct RestirBufferLayout {
    // state
    u64 surfPos[2] = {0, 0};
    u64 surfNormal[2] = {0, 0};
    u64 surfAlbedo[2] = {0, 0};
    u64 diHistory[2] = {0, 0};
    u64 giHistory[2] = {0, 0};
    u64 stateBytes = 0;
    // work: stage s (0 initial, 1 temporal, 2 + i spatial i) -> section
    u64 diStage[kRestirStages] = {};
    u64 giStage[kRestirStages] = {};
    u64 workBytes = 0;
    // output
    u64 diSignal = 0;
    u64 giSignal = 0;
    u64 depth = 0;
    u64 outputBytes = 0;
    u32 width = 0;
    u32 height = 0;
    bool keepIntermediates = false;

    static RestirBufferLayout compute(u32 width, u32 height, bool keepIntermediates) {
        RestirBufferLayout l{};
        l.width = width;
        l.height = height;
        l.keepIntermediates = keepIntermediates;
        const u64 pixels = static_cast<u64>(width) * height;
        auto align = [](u64 v) { return (v + 255u) & ~u64{255u}; };
        u64 cursor = 0;
        auto take = [&](u64 bytes) {
            const u64 at = cursor;
            cursor = align(cursor + bytes);
            return at;
        };
        for (u32 k = 0; k < 2u; ++k) {
            l.surfPos[k] = take(pixels * 16u);
            l.surfNormal[k] = take(pixels * 16u);
            l.surfAlbedo[k] = take(pixels * 16u);
            l.diHistory[k] = take(pixels * sizeof(RestirDiReservoir));
            l.giHistory[k] = take(pixels * sizeof(RestirGiReservoir));
        }
        l.stateBytes = cursor;
        cursor = 0;
        const u32 sections = keepIntermediates ? kRestirStages : 3u;
        u64 di[kRestirStages] = {};
        u64 gi[kRestirStages] = {};
        for (u32 s = 0; s < sections; ++s) {
            di[s] = take(pixels * sizeof(RestirDiReservoir));
        }
        for (u32 s = 0; s < sections; ++s) {
            gi[s] = take(pixels * sizeof(RestirGiReservoir));
        }
        for (u32 s = 0; s < kRestirStages; ++s) {
            // Ping-pong: initial -> 0, temporal -> 1, spatial i -> 2, 1, 2, ... (never its own input).
            const u32 section = keepIntermediates ? s : (s == 0u ? 0u : (s == 1u ? 1u : ((s & 1u) == 0u ? 2u : 1u)));
            l.diStage[s] = di[section];
            l.giStage[s] = gi[section];
        }
        l.workBytes = cursor;
        cursor = 0;
        l.diSignal = take(pixels * 16u);
        l.giSignal = take(pixels * 16u);
        l.depth = take(pixels * 4u);
        l.outputBytes = cursor;
        return l;
    }
};

} // namespace fuse::renderer::restir
