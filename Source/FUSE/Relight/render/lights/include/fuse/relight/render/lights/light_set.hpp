// FUSE Relight RL-4.4: the Relight light set of one frame (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.3; Wave R4).
//
// Every frame the light set gathers
//   * game lights: the enabled D3DLIGHT9s (tap::Light), converted to sphere / distant lights exactly as RL-1.5
//     (rtx.lightConversion* options, the "off" and same-frame hash rules, rtx.ignoreGame*Lights). The raw D3D records
//     are kept: RelightLightsGpu converts them ON THE GPU ("relight.lights.convert", the same single-source kernel),
//     and the CPU runs the same kernel (light_d3d_convert) for the tree;
//   * authored lights: Remix / UsdLux sphere, rect, disk, cylinder and distant lights with shaping (mods, RL-3.2's
//     LightParams via lightFromUsd, or the rtx.fallbackLight* fallback);
//   * emissive triangles of captured geometry with an emissive material (addEmissiveTriangles);
// into one light table (RlLight, kernels/light_core.h) and builds the WP-7.1 LightTree over proxies of those lights
// (same order: tree light index == table index), refitting instead of rebuilding when the frame's light kinds match
// the previous frame's.
//
//   set.beginFrame();
//   set.addGameLights(lights, count);              // tap::Light table (disabled slots skipped)
//   set.addLight(lightFromUsd(params, xform), key); // mod lights
//   set.addEmissiveTriangles(mesh);                 // captured draws with emissive materials
//   set.addFallbackLight(...);                      // rtx.fallbackLightMode
//   set.build();                                    // CPU conversion + tree build / refit
//   ... RelightLightsGpu::beginFrame(serial, set)  (light_set_gpu.hpp) for the GPU view
//   set.sample(p, n, u0, u1, u2) / set.pdf(p, n, light, wi) / set.eval(...)   CPU reference of the GPU sampler
//
// Allocations: steady-state frames (no more lights than a previous frame) make no heap allocation (every list keeps
// its capacity; reserve() pre-sizes them).
#pragma once

#include "light_cpp.hpp"

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/tap/relight_tap.hpp>
#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::mods::import {
struct LightParams;
}

namespace fuse::relight::render::lights {

namespace lk = fuse::relight::lightk;

/// Where a table entry came from.
enum class LightOrigin : u8 {
    Game = 0,     ///< converted D3DLIGHT9 (GPU conversion)
    Authored = 1, ///< addLight (mods, API, fallback)
    Emissive = 2, ///< emissive triangle
};

struct LightEntry {
    u64 key = 0;                  ///< game: RL-1.5 stable light hash; authored: caller key; emissive: caller source
    LightOrigin origin = LightOrigin::Game;
    u32 source = 0;               ///< game: D3D light index; emissive: triangle index within its mesh
};

/// rtx.lightConversion* (RL-1.5 LightOptions) as the conversion kernel reads them.
lk::RlConvertParams convertParamsFromOptions();

/// tap::Light -> the packed D3D record the conversion reads (float4[kRlD3dWords]).
void packD3dLight(const tap::Light& light, lk::float4* words);

/// The WP-7.1 tree proxy of a light: sphere -> point (spot when shaped: cone = the shaping cone), rect / disk /
/// triangle -> the same kinds (radiance = max channel), cylinder -> point (the tree has no cylinder kind: the point
/// bounds nothing but is never zero), distant -> directional. `source` is copied to the emitter record.
renderer::light_tree::LightTreeLight treeProxy(const lk::RlLight& light, u32 source);

/// UsdLux / Remix light (RL-3.2 LightParams: SphereLight, RectLight, DiskLight, CylinderLight, DistantLight) placed by
/// `objectToWorld` (row-major D3D layout, row vectors: p' = p M; null = identity). radiance = color x intensity x
/// 2^exposure; UsdLux axes (emission along -Z, cylinder along X), shaping (cone angle in degrees, softness, focus),
/// volumetric_radiance_scale. False (out = none) for DomeLight and unknown types. enableColorTemperature is not
/// applied (reported in `warning` when set).
bool lightFromUsd(const mods::import::LightParams& params, const float* objectToWorld, lk::RlLight& out,
                  const char** warning = nullptr);

/// Record constructors (world space). Radiance is linear RGB; see light_core.h for the conventions.
lk::RlLight makeSphereLight(const lk::float3& center, float radius, const lk::float3& radiance);
lk::RlLight makeRectLight(const lk::float3& center, const lk::float3& halfU, const lk::float3& halfV,
                          const lk::float3& radiance, bool twoSided = false);
lk::RlLight makeDiskLight(const lk::float3& center, const lk::float3& radiusU, const lk::float3& radiusV,
                          const lk::float3& radiance, bool twoSided = false);
lk::RlLight makeCylinderLight(const lk::float3& center, const lk::float3& axisHalfLength, float radius,
                              const lk::float3& radiance);
/// `direction` = where the light travels; `irradiance` at normal incidence; halfAngle 0 = delta.
lk::RlLight makeDistantLight(const lk::float3& direction, float halfAngle, const lk::float3& irradiance);
lk::RlLight makeTriangleLight(const lk::float3& v0, const lk::float3& v1, const lk::float3& v2,
                              const lk::float3& radiance, bool twoSided = false);
/// Adds shaping (axis, cone half angle in radians, softness as a cosine delta, focus exponent).
void setShaping(lk::RlLight& light, const lk::float3& axis, float coneAngle, float softness, float focus);

/// One draw's emissive geometry.
struct EmissiveMesh {
    const float* positions = nullptr; ///< object space xyz, `stride` bytes apart
    u32 stride = 12;
    u32 vertexCount = 0;
    const void* indices = nullptr;    ///< null: non-indexed triangle list
    u32 indexCount = 0;               ///< indexed: indices; non-indexed: ignored (vertexCount / 3 triangles)
    bool index32 = false;
    const float* objectToWorld = nullptr; ///< row-major D3D layout (p' = p M); null = identity
    lk::float3 radiance{0.f, 0.f, 0.f};   ///< emissive radiance of the material
    bool twoSided = false;
    u64 key = 0;                      ///< caller id (instance / draw), LightEntry::key
};

/// rtx.fallbackLightMode (read by name: RL-4.2 declares the option): 0 never, 1 when the frame has no other light,
/// 2 always.
struct FallbackLight {
    u32 mode = 1;
    lk::float3 radiance{1.6f, 1.8f, 2.0f}; ///< rtx.fallbackLightRadiance (irradiance of the distant light)
    lk::float3 direction{-0.2f, -1.0f, 0.4f}; ///< rtx.fallbackLightDirection (travel direction)
    float angle = 5.0f;                     ///< rtx.fallbackLightAngle (degrees, full angle)
};
FallbackLight fallbackLightFromOptions();

struct LightSetStats {
    u32 gameLights = 0;
    u32 gameRejected = 0;  ///< invalid type, ignored type, off, duplicate hash
    u32 authored = 0;
    u32 emissiveTriangles = 0;
    u32 emissiveSkipped = 0; ///< degenerate or dark triangles, or over the cap
    bool fallback = false;
    u32 builds = 0;
    u32 refits = 0;
};

/// Light-set sample (CPU reference of "relight.lights.sample").
struct LightSetSample {
    u32 light = renderer::light_tree::kLtInvalid;
    float pmf = 0.f;             ///< tree selection probability
    lk::RlLightSample shape{};   ///< the light's own sample
    float pdf = 0.f;             ///< pmf x shape.pdf (delta: pmf)
};

class RelightLightSet {
public:
    /// Pre-sizes every list for `lights` lights (of which `gameLights` game lights).
    void reserve(u32 lights, u32 gameLights = 64u);
    /// Clears the frame's lists (capacity kept). Reads rtx.lightConversion* once.
    void beginFrame();

    /// One enabled D3D light (RL-1.5 rules: invalid type, rtx.ignoreGame*Lights, "off", same hash this frame ->
    /// rejected, false).
    bool addGameLight(const tap::Light& light);
    /// Every enabled light of a tap light table. Returns the number added.
    u32 addGameLights(const tap::Light* lights, u32 count);
    /// An authored light (kind none / zero radiance: rejected).
    bool addLight(const lk::RlLight& light, u64 key);
    /// The emissive triangles of one mesh. Returns the number added.
    u32 addEmissiveTriangles(const EmissiveMesh& mesh);
    /// The fallback distant light when `f.mode` asks for it (call after every other add). True when added.
    bool addFallbackLight(const FallbackLight& f);
    /// Cap on emissive triangles per frame (0 = none).
    void setEmissiveTriangleLimit(u32 limit) { m_emissiveLimit = limit; }

    /// Converts the game lights on the CPU (light_d3d_convert, CpuReference), assembles the table and builds or
    /// refits the tree. False when the tree build fails.
    bool build();

    // --- results ----------------------------------------------------------------------------------------------------
    u32 lightCount() const { return static_cast<u32>(m_entries.size()); }
    u32 gameLightCount() const { return m_gameCount; }
    /// Packed table, kRlLightWords per light: [0, gameLightCount) game lights, then authored, then emissive.
    const std::vector<lk::float4>& table() const { return m_table; }
    /// Packed raw D3D records of the game lights (kRlD3dWords each), the GPU conversion's input.
    const std::vector<lk::float4>& gameInputs() const { return m_d3d; }
    const std::vector<LightEntry>& entries() const { return m_entries; }
    lk::RlLight light(u32 index) const;
    const renderer::light_tree::LightTree& tree() const { return m_tree; }
    const std::vector<renderer::light_tree::LightTreeLight>& proxies() const { return m_proxies; }
    const lk::RlConvertParams& convertParams() const { return m_params; }
    const LightSetStats& stats() const { return m_stats; }

    // --- CPU sampler (the GPU pass's reference) -----------------------------------------------------------------------
    LightSetSample sample(const lk::float3& p, const lk::float3& n, float u0, float u1, float u2) const;
    /// Light-set density of direction wi through light `light` (pmf x rlLightPdf).
    float pdf(const lk::float3& p, const lk::float3& n, u32 light, const lk::float3& wi) const;
    /// Sum over lights of pmf x rlLightPdf: the density of the whole set for wi.
    float pdfAll(const lk::float3& p, const lk::float3& n, const lk::float3& wi) const;
    float pmf(const lk::float3& p, const lk::float3& n, u32 light) const;

private:
    bool pushLight(const lk::RlLight& light, LightOrigin origin, u64 key, u32 source);

    lk::RlConvertParams m_params{};
    std::vector<lk::float4> m_d3d;       ///< game lights, raw
    std::vector<lk::float4> m_hostTable; ///< authored + emissive, packed
    std::vector<LightEntry> m_gameEntries;
    std::vector<LightEntry> m_hostEntries;
    std::vector<lk::float4> m_table;     ///< assembled by build()
    std::vector<LightEntry> m_entries;
    std::vector<hash::Hash64> m_frameHashes;
    std::vector<renderer::light_tree::LightTreeLight> m_proxies;
    std::vector<u32> m_prevKinds; ///< proxy kinds of the last build (refit when equal)
    renderer::light_tree::LightTree m_tree;
    u32 m_gameCount = 0;
    u32 m_emissiveLimit = 0;
    bool m_treeValid = false;
    LightSetStats m_stats{};
};

} // namespace fuse::relight::render::lights
