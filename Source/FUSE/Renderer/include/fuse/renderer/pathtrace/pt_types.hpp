#pragma once

// WP-7.3 path-tracing mode: the records shared by the C++ host code, the CPU reference (pt_reference.hpp) and
// the GPU kernels (shaders/pathtrace/pt_common.{glsl,slang} declare the same fields in the same order;
// fuse_rp_pathtrace_layout checks names, order, offsets and sizes).
//
// Layout rules (as restir_types.hpp): 4-byte scalars, fixed scalar arrays and 8-byte addresses only, no vec3
// members, no implicit padding, every record a multiple of 16 bytes, so std430 == scalar layout and the GPU
// reads everything through buffer device addresses.
//
// Buffers (PtBufferLayout, sections 256-aligned):
//   state   (persistent) accumulation: f32x4 (sum r, g, b, samples) and f32x4 (sum r^2, g^2, b^2, 0) per pixel
//   output  f32x4 (mean r, g, b, samples) per pixel: the converged image (the frame composer's PT input)
//           f32x4 signal: this frame's samples averaged and demodulated by the primary albedo (w = 0; the WP-6.4
//           RGB signal layout), f32 linear view depth (0 = sky), f32x4 primary normal (the WP-6.4 normal buffer),
//           f32x4 demodulation factor (rgb: the floored primary albedo, 1 for the sky and near-black surfaces;
//           w: 1 where the camera ray hit) so signal x factor == the frame's radiance (remodulation)
//
// Emitter map (u32 words, PathTracerGpu uploads it with the light table): words [0, instanceSlots) hold, per GPU-
// scene instance slot, the word offset of that instance's triangle table (kPtInvalid: no emissive triangle in the
// light tree); a triangle table holds, per mesh triangle (BLAS primitive / MTRI order), the WP-7.1 light-tree
// emitter index of that triangle (kPtInvalid: not an emitter). A BSDF-sampled ray that hits a mapped triangle adds
// the light table's radiance with the MIS weight against the light-tree pmf at the previous vertex.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::pathtrace {

inline constexpr u32 kPtTile = 8u;               ///< 8 x 8 pixels per compute workgroup
inline constexpr u32 kPtInvalid = 0xFFFFFFFFu;
inline constexpr u32 kPtMaxBounces = 32u;
inline constexpr u32 kPtMaxSamplesPerFrame = 64u;
inline constexpr f32 kPtPi = 3.14159265358979323846f;
/// Shadow rays toward a point stop at this fraction of the distance (the point lies on traced geometry).
inline constexpr f32 kPtShadowShorten = 0.9999f;
/// Default material of an instance without a valid material row (Lambert, albedo 0.8).
inline constexpr f32 kPtDefaultAlbedo = 0.8f;

/// PtFrameConstants::flags.
enum PtFlag : u32 {
    kPtFlagNee = 1u << 0,            ///< next-event estimation with the WP-7.1 light tree at every vertex
    kPtFlagEmitterHits = 1u << 1,    ///< BSDF-sampled rays that hit a light-tree emitter add its radiance
                                     ///< (with kPtFlagNee: MIS power heuristic; without: weight 1)
    kPtFlagRussianRoulette = 1u << 2,///< from bounce rrStartBounce on
    kPtFlagAccumulate = 1u << 3,     ///< add to the accumulation (off: the output holds this frame only)
    kPtFlagGuides = 1u << 4,         ///< write signal / depth / normal / albedo (denoiser and RR inputs)
    kPtFlagClamp = 1u << 5,          ///< clamp each path's radiance to clampRadiance (biased; off for the gates)
};

/// Light-sampling strategies (PtSettings::strategy).
enum class PtStrategy : u8 {
    Mis = 0,     ///< NEE + emitter hits, power-heuristic MIS (the default)
    NeeOnly = 1, ///< NEE; BSDF rays that hit a light-tree emitter add nothing (except camera rays)
    BsdfOnly = 2,///< no NEE: emission only where BSDF rays hit it (delta lights contribute nothing)
};

/// Per-frame constants, read through BDA from a host-visible ring. 272 bytes.
struct PtFrameConstants {
    u64 tlas = 0;          ///< TLAS device address (AccelerationStructures::tlasAddress)
    u64 scene = 0;         ///< GpuScene header address (instances, transforms, meshes, materials, indices)
    u64 lightTree = 0;     ///< LightTreeHeader BDA (0: no lights -> no NEE, no emitter hits)
    u64 lights = 0;        ///< restir::RestirLight[lightCount] (RGB table, same index as the tree emitters)
    u64 emitterMap = 0;    ///< u32 words (see the header comment; 0: no emitter hits)
    u64 accum = 0;         ///< f32x4 per pixel: sum rgb, samples
    u64 accumSq = 0;       ///< f32x4 per pixel: sum rgb^2, 0
    u64 mean = 0;          ///< f32x4 per pixel: mean rgb, samples (the converged image)
    u64 signal = 0;        ///< f32x4 per pixel (kPtFlagGuides)
    u64 depth = 0;         ///< f32 per pixel
    u64 normal = 0;        ///< f32x4 per pixel
    u64 albedo = 0;        ///< f32x4 per pixel
    f32 invViewProj[16] = {}; ///< column-major, Vulkan clip -> world
    f32 cameraPosition[3] = {0.f, 0.f, 0.f};
    u32 width = 0;
    f32 cameraForward[3] = {0.f, 0.f, -1.f}; ///< unit view axis (linear depth = dot(p - camera, forward))
    u32 height = 0;
    f32 sky[3] = {0.f, 0.f, 0.f};            ///< radiance of rays that escape (not light-sampled)
    u32 flags = 0;                           ///< PtFlag
    u32 frameIndex = 0;
    u32 seed = 0;
    u32 samplesPerFrame = 1;
    u32 maxBounces = 0;     ///< scattering events per path (0: emission seen by the camera only)
    u32 rrStartBounce = 0;
    u32 lightCount = 0;
    u32 cullMask = 0;       ///< ray cull mask (within rt::kRtMaskAll)
    u32 emitterMapSlots = 0;///< instance slots covered by the emitter map
    f32 rayTMin = 0.f;      ///< tMin of every ray (the origin is offset along the normal, the origin triangle skipped)
    f32 normalBias = 0.f;   ///< origin offset: n (normalBias + viewBias |p - camera|)
    f32 viewBias = 0.f;
    f32 clampRadiance = 0.f;///< kPtFlagClamp: per-path luminance cap
    f32 farDistance = 0.f;  ///< tMax of camera / BSDF rays and directional shadow rays
    f32 minRoughness = 0.f; ///< GGX roughness floor (alpha = roughness^2)
    u32 sampleBase = 0;     ///< samples accumulated before this frame (the RNG sample index offset)
    u32 reserved = 0;
};
static_assert(sizeof(PtFrameConstants) == 272u && offsetof(PtFrameConstants, invViewProj) == 96u &&
                  offsetof(PtFrameConstants, cameraPosition) == 160u && offsetof(PtFrameConstants, sky) == 192u &&
                  offsetof(PtFrameConstants, frameIndex) == 208u && offsetof(PtFrameConstants, rayTMin) == 240u &&
                  offsetof(PtFrameConstants, farDistance) == 256u && offsetof(PtFrameConstants, sampleBase) == 264u,
              "PtFrameConstants layout (pt_common.glsl / .slang)");

/// Push constants of every path-tracing kernel (compute and ray-tracing stages). 16 bytes.
struct PtPush {
    u64 frame = 0; ///< BDA of this frame's PtFrameConstants
    u32 pass = 0;  ///< reserved (0)
    u32 reserved = 0;
};
static_assert(sizeof(PtPush) == 16u, "PtPush layout");

/// Surface interaction handed from the closest-hit shader to the ray-generation shader (the RT pipeline's
/// payload; the ray-query kernel computes the same record in place). 80 bytes. Also the CPU reference's record.
struct PtHit {
    f32 position[3] = {0.f, 0.f, 0.f}; ///< world hit point (barycentric interpolation of the world vertices)
    f32 t = -1.f;                      ///< ray parameter (-1: miss)
    f32 normal[3] = {0.f, 0.f, 0.f};   ///< unit geometric normal e1 x e2 (winding order, not face-forwarded)
    u32 instance = kPtInvalid;
    f32 albedo[3] = {0.f, 0.f, 0.f};   ///< base colour
    u32 primitive = kPtInvalid;
    f32 emission[3] = {0.f, 0.f, 0.f}; ///< material emission (emissive colour x intensity), front side only
    u32 emitter = kPtInvalid;          ///< light-tree emitter index from the emitter map
    f32 roughness = 1.f;
    f32 metallic = 0.f;
    u32 flags = 0;                     ///< reserved
    u32 reserved = 0;
};
static_assert(sizeof(PtHit) == 80u && offsetof(PtHit, normal) == 16u && offsetof(PtHit, albedo) == 32u &&
                  offsetof(PtHit, emission) == 48u && offsetof(PtHit, roughness) == 64u,
              "PtHit layout");

/// Byte offsets of the two buffers for an extent (sections 256-aligned).
struct PtBufferLayout {
    u64 accum = 0;
    u64 accumSq = 0;
    u64 stateBytes = 0;
    u64 output = 0;
    u64 signal = 0;
    u64 depth = 0;
    u64 normal = 0;
    u64 albedo = 0;
    u64 outputBytes = 0;
    u32 width = 0;
    u32 height = 0;

    static PtBufferLayout compute(u32 width, u32 height) {
        PtBufferLayout l{};
        l.width = width;
        l.height = height;
        const u64 pixels = static_cast<u64>(width) * height;
        u64 cursor = 0;
        auto take = [&](u64 bytes) {
            const u64 at = cursor;
            cursor = (cursor + bytes + 255u) & ~u64{255u};
            return at;
        };
        l.accum = take(pixels * 16u);
        l.accumSq = take(pixels * 16u);
        l.stateBytes = cursor;
        cursor = 0;
        l.output = take(pixels * 16u);
        l.signal = take(pixels * 16u);
        l.depth = take(pixels * 4u);
        l.normal = take(pixels * 16u);
        l.albedo = take(pixels * 16u);
        l.outputBytes = cursor;
        return l;
    }
};

} // namespace fuse::renderer::pathtrace
