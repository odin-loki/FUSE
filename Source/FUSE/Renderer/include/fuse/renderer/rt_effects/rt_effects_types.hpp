#pragma once

// WP-6.2 ray-traced shadows and reflections (tier T2): records shared by the C++ side, the kernels
// (shaders/rt_effects/rtfx_*.{comp,slang}), the lighting's light loop (lc_ltc.{glsl,slang}) and the CPU
// references. Device-safe (only <fuse/types.hpp>); the static_asserts pin the std430 layouts and
// fuse_rp_rt_effects_layout checks the shader twins declare the same fields in the same order.
//
// Outputs (one GPU buffer, RtEffects::outputBuffer(), sections at RtEffectsOutputLayout offsets):
//   visibility   f32 [kRtfxMaxShadowLights][width * height]   this frame's shadow visibility per shadowed
//                light (fraction of the light's samples that reached it; 1 = lit / sky)
//   hitDistance  f32 [kRtfxMaxShadowLights][width * height]   mean distance from the ray origin to the
//                occluder over the occluded samples (denoiser penumbra input); kRtfxNoHit when none
//   reflection   RtfxReflectionTexel [width * height]         mean reflected radiance over the frame's
//                samples + mean hit distance over the samples that hit (kRtfxNoHit when none)
// Every value is this frame's estimate: temporal accumulation / denoising is WP-6.4's (the gates
// accumulate on the host and compare with the offline reference, rt_effects_reference.hpp).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::rt_effects {

inline constexpr u32 kRtfxMaxShadowLights = 4u; ///< shadowed lights per frame (visibility channels)
inline constexpr u32 kRtfxMaxHitLights = 2u;    ///< directional lights lighting reflection hits
inline constexpr u32 kRtfxMaxSamples = 64u;     ///< rays per pixel per light (or reflection) per frame
inline constexpr u32 kRtfxTile = 8u;            ///< 8 x 8 pixels per workgroup
inline constexpr f32 kRtfxNoHit = -1.f;         ///< hit distance of "nothing was hit"

/// How a shadowed light is sampled (RtfxShadowLight::kind).
enum RtfxLightKind : u32 {
    kRtfxLightNone = 0,
    kRtfxLightDirectional = 1, ///< punctual direction: one hard ray along `direction`
    kRtfxLightSun = 2,         ///< sun disk: cone of half angle acos(cosCone) around `direction` (soft)
    kRtfxLightPoint = 3,       ///< point / spot: one hard ray to `position`
    kRtfxLightRect = 4,        ///< rectangle: uniform by area, centre +- axisX +- axisY (soft)
    kRtfxLightDisk = 5,        ///< disk / ellipse: uniform by area (concentric map) over axisX / axisY (soft)
};

/// One shadowed light, resolved on the host from its GpuLight row (packShadowLight): 64 bytes.
struct RtfxShadowLight {
    f32 position[3] = {0.f, 0.f, 0.f}; ///< point / spot position, rectangle / disk centre
    u32 kind = kRtfxLightNone;         ///< RtfxLightKind
    f32 axisX[3] = {0.f, 0.f, 0.f};    ///< rectangle / disk half axis along the width
    u32 slot = 0xFFFFFFFFu;            ///< GpuScene light slot (the light loop's key)
    f32 axisY[3] = {0.f, 0.f, 0.f};    ///< rectangle / disk half axis along the height
    f32 cosCone = 1.f;                 ///< sun: cos(angular radius)
    f32 direction[3] = {0.f, 1.f, 0.f}; ///< directional / sun: unit vector TOWARD the light; area: lit-side normal
    u32 samples = 1u;                  ///< rays per pixel this frame (1 for the hard kinds)
};
static_assert(sizeof(RtfxShadowLight) == 64u, "RtfxShadowLight layout (rtfx_common.glsl / .slang)");

/// A directional light that lights reflection hits (Lambert, optional shadow ray): 32 bytes.
struct RtfxHitLight {
    f32 direction[3] = {0.f, 1.f, 0.f}; ///< unit vector toward the light
    u32 shadowed = 0u;                  ///< 1 = trace a shadow ray from the hit
    f32 irradiance[3] = {0.f, 0.f, 0.f}; ///< colour x intensity (irradiance at normal incidence)
    u32 slot = 0xFFFFFFFFu;
};
static_assert(sizeof(RtfxHitLight) == 32u, "RtfxHitLight layout");

/// The view the light loop reads (LightingFrameDesc::rtShadows points at it; it is the head of the
/// frame constants): 48 bytes. Light `slots[c]` (c < count) takes visibility[c * width * height + pixel]
/// instead of its VSM visibility; other lights are unchanged.
struct RtfxShadowView {
    u64 visibility = 0; ///< BDA of the visibility planes (f32)
    u32 width = 0;
    u32 height = 0;
    u32 count = 0;      ///< shadowed lights (<= kRtfxMaxShadowLights)
    u32 pad0 = 0;
    u32 slots[kRtfxMaxShadowLights] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    u32 pad1[2] = {0u, 0u};
};
static_assert(sizeof(RtfxShadowView) == 48u, "RtfxShadowView layout (lc_ltc.glsl / .slang, rtfx_common)");

/// RtfxFrameConstants::flags.
enum RtfxFrameFlag : u32 {
    kRtfxFlagHitShadows = 1u << 0, ///< reflection hits trace shadow rays toward shadowed hit lights
};

/// Per-frame constants, read through BDA from a host-visible ring (RtEffects::beginFrame): 576 bytes.
struct RtfxFrameConstants {
    RtfxShadowView view{};         ///< offset 0: what the light loop reads
    u64 tlas = 0;                  ///< TLAS device address (AccelerationStructures::tlasAddress())
    u64 hitDistance = 0;           ///< BDA of the shadow hit-distance planes (f32)
    u64 reflection = 0;            ///< BDA of RtfxReflectionTexel[width * height]
    u64 reserved0 = 0;
    f32 invViewProj[16] = {};      ///< column-major, clip (Vulkan, forward z/w) -> world
    f32 cameraPosition[3] = {0.f, 0.f, 0.f};
    u32 frameIndex = 0;            ///< sequence index: sample n of a pixel = frameIndex * samples + s
    f32 invWidth = 0.f;
    f32 invHeight = 0.f;
    u32 scene = 0;                 ///< GpuScene::headerHandle()
    u32 flags = 0;                 ///< RtfxFrameFlag
    u32 gbufferNormal = 0;         ///< bindless sampled-image handles of RT0 (normal + AO),
    u32 gbufferRoughMetal = 0;     ///< RT2 (roughness, metallic) and RT4 (forward z/w depth, 1 = sky)
    u32 gbufferDepth = 0;
    u32 reflectionSamples = 0;     ///< reflection rays per pixel (0 = no reflections)
    f32 normalBias = 0.f;          ///< ray origin = P + N (normalBias + viewBias |P - camera|)
    f32 viewBias = 0.f;
    f32 farDistance = 0.f;         ///< tMax of directional / sun shadow rays and of reflection rays
    f32 mirrorRoughness = 0.f;     ///< roughness below which reflections are a perfect mirror
    f32 ambient[3] = {0.f, 0.f, 0.f}; ///< uniform ambient radiance at reflection hits
    u32 hitLightCount = 0;
    f32 sky[3] = {0.f, 0.f, 0.f};     ///< radiance of reflection rays that miss
    u32 shadowCullMask = 0;        ///< ray cull masks (always within rt::kRtMaskAll)
    u32 reflectionCullMask = 0;
    u32 seed = 0;                  ///< decorrelates runs (pixel seeds are hashed with it)
    u32 pad[2] = {0u, 0u};
    RtfxShadowLight shadowLights[kRtfxMaxShadowLights]{};
    RtfxHitLight hitLights[kRtfxMaxHitLights]{};
};
static_assert(sizeof(RtfxFrameConstants) == 576u && offsetof(RtfxFrameConstants, tlas) == 48u &&
                  offsetof(RtfxFrameConstants, invViewProj) == 80u && offsetof(RtfxFrameConstants, cameraPosition) == 144u &&
                  offsetof(RtfxFrameConstants, invWidth) == 160u && offsetof(RtfxFrameConstants, gbufferNormal) == 176u &&
                  offsetof(RtfxFrameConstants, normalBias) == 192u && offsetof(RtfxFrameConstants, ambient) == 208u &&
                  offsetof(RtfxFrameConstants, sky) == 224u && offsetof(RtfxFrameConstants, reflectionCullMask) == 240u &&
                  offsetof(RtfxFrameConstants, shadowLights) == 256u && offsetof(RtfxFrameConstants, hitLights) == 512u,
              "RtfxFrameConstants layout (rtfx_common.glsl / .slang)");

/// Push constants of both kernels: 16 bytes.
struct RtfxPush {
    u64 frame = 0; ///< BDA of this frame's RtfxFrameConstants
    u64 dump = 0;  ///< BDA of RtfxRayRecord[] (debug / parity; 0 = none)
};
static_assert(sizeof(RtfxPush) == 16u, "RtfxPush layout");

/// One reflection output texel: 16 bytes.
struct RtfxReflectionTexel {
    f32 radiance[3] = {0.f, 0.f, 0.f};
    f32 hitDistance = kRtfxNoHit;
};
static_assert(sizeof(RtfxReflectionTexel) == 16u, "RtfxReflectionTexel layout");

/// RtfxRayRecord::flags.
enum RtfxRayFlag : u32 {
    kRtfxRayTraced = 1u << 0, ///< a ray was traced (sky pixels / below-horizon reflections trace none)
    kRtfxRayHit = 1u << 1,    ///< committed triangle hit
};

/// Debug record of a pixel's first sample (rtfx_shadows: [light][pixel]; rtfx_reflections: [pixel]):
/// the exact ray traced and its committed hit. 48 bytes.
struct RtfxRayRecord {
    f32 origin[3] = {0.f, 0.f, 0.f};
    f32 tMin = 0.f;
    f32 direction[3] = {0.f, 0.f, 0.f};
    f32 tMax = 0.f;
    f32 t = kRtfxNoHit;
    u32 instance = 0xFFFFFFFFu;
    u32 primitive = 0xFFFFFFFFu;
    u32 flags = 0u; ///< RtfxRayFlag
};
static_assert(sizeof(RtfxRayRecord) == 48u, "RtfxRayRecord layout");

/// Byte offsets of the output buffer's sections for an extent (256-aligned).
struct RtEffectsOutputLayout {
    u64 visibility = 0;
    u64 hitDistance = 0;
    u64 reflection = 0;
    u64 bytes = 0;

    static RtEffectsOutputLayout compute(u32 width, u32 height) {
        const u64 pixels = static_cast<u64>(width) * height;
        auto align = [](u64 v) { return (v + 255u) & ~u64{255u}; };
        RtEffectsOutputLayout l{};
        l.visibility = 0;
        l.hitDistance = align(l.visibility + pixels * kRtfxMaxShadowLights * 4u);
        l.reflection = align(l.hitDistance + pixels * kRtfxMaxShadowLights * 4u);
        l.bytes = align(l.reflection + pixels * sizeof(RtfxReflectionTexel));
        return l;
    }
};

} // namespace fuse::renderer::rt_effects
