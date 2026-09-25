// FUSE Relight RL-5.6: volumetrics and the particle composite (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1 "Volumetrics
// (froxel, RIS light sampling), particles", §5.7, §5.8). The model is described in shaders/rl_vol_core.h (the single
// source the CPU reference below and the GPU kernels, volumetrics_gpu.hpp, both run):
//
//   inject      froxel media (exponential height fog; parameters from the D3D fog state, mediumFromD3dFog) lit by the
//               RL-4.4 light set through RIS over the WP-7.1 light tree + temporal reservoir reuse, shadow rays on the
//               path tracer's scene (WP-6.0 CPU reference BVH here, the TLAS by ray query on the GPU);
//   temporal    reprojected exponential history (the temporal filter);
//   integrate   front-to-back energy-conserving integration per column;
//   apply       the medium and the ray-traced particle billboards (RL-3.6 GpuParticleVertex quads, D3D blend modes,
//               per-pixel sorted) composited over a radiance buffer with view depth (the path tracer's
//               kPtOutRadiance / kPtOutDepth, or any other frame).
//
// Vulkan-free: builds in every tree (the stub backend runs the CPU reference and the CPU gates).
#pragma once

#include "light_cpp.hpp"

#include <fuse/relight/particles/particle_types.hpp>
#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/types.hpp>

#include <span>
#include <vector>

namespace fuse::relight::render::volumetrics {

namespace lk = fuse::relight::lightk;
using Word = lk::float4;

struct VolUint4 {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
    u32 w = 0;
};

inline constexpr u32 kVolParamWordCount = 11u;
inline constexpr u32 kVolIntWordCount = 3u;
inline constexpr u32 kVolSystemCount = 8u;
inline constexpr u32 kVolLayerCount = 16u;
inline constexpr u32 kVolBufferCount = 8u;

// Buffers / stages / flags (rl_vol_types.h; the rl_vol_layout gate checks the values against the shader text).
enum VolBuffer : u32 {
    kVolCurrent = 0,
    kVolHistPrev = 1,
    kVolHistCur = 2,
    kVolResPrev = 3,
    kVolResCur = 4,
    kVolIntegrated = 5,
    kVolColorIn = 6,
    kVolColorOut = 7,
};
enum VolStage : u32 {
    kVolInject = 0,
    kVolTemporal = 1,
    kVolIntegrate = 2,
    kVolApply = 3,
};
enum VolFlag : u32 {
    kVolHistory = 1u,
    kVolReproject = 2u,
    kVolReuse = 4u,
    kVolShadows = 8u,
    kVolLights = 16u,
    kVolFog = 32u,
    kVolParticles = 64u,
};
enum VolBlend : u32 {
    kVolAlpha = 0,          ///< SRCALPHA, INVSRCALPHA
    kVolAdditive = 1,       ///< SRCALPHA, ONE
    kVolPremultiplied = 2,  ///< ONE, INVSRCALPHA
    kVolMultiply = 3,       ///< DESTCOLOR, ZERO
};
inline constexpr u32 kVolSoftDisc = 1u; ///< VolParticleSystem::flags

/// Homogeneous exponential-height medium.
struct VolMedium {
    float density = 0.f;    ///< extinction (1 / world unit) at and below baseHeight
    float falloff = 0.f;    ///< height falloff (0: homogeneous)
    float baseHeight = 0.f;
    float albedo[3] = {1.f, 1.f, 1.f};
    float anisotropy = 0.f; ///< Henyey-Greenstein g, clamped to [-0.95, 0.95]
    float ambient[3] = {0.f, 0.f, 0.f};
};

/// D3DFOGMODE.
enum class D3dFogMode : u32 { None = 0, Exp = 1, Exp2 = 2, Linear = 3 };

/// The game's fixed-function fog (D3DRS_FOG* render states; colour in linear RGB).
struct D3dFogState {
    D3dFogMode mode = D3dFogMode::None;
    float density = 0.f;
    float start = 0.f;
    float end = 1.f;
    float color[3] = {0.f, 0.f, 0.f};
};

/// D3D fog -> medium (rtx fog remapping, FUSE rule): a homogeneous medium with albedo 1 and ambient = the fog colour,
/// so a fully fogged ray reaches the fog colour and, with no lights, the composite equals D3D range fog
/// lerp(fogColour, colour, f(distance)) exactly for EXP (sigma = density), and matches EXP2 (sigma = density: equal at
/// f = 1/e) and LINEAR (sigma = ln 2 / ((start + end) / 2): equal at the midpoint) at one distance. `sceneScale`
/// (rtx.sceneScale) divides the extinction (world units per game unit). None / invalid: density 0.
VolMedium mediumFromD3dFog(const D3dFogState& fog, float sceneScale = 1.f);

struct VolParticleSystem {
    u32 firstQuad = 0;  ///< quad q = vertices [4q, 4q + 4) (RL-3.6 billboard vertices, strip order)
    u32 quadCount = 0;
    u32 blend = kVolAlpha;
    u32 flags = 0;
};

struct VolFrameDesc {
    render::pathtrace::PtCamera camera{};
    render::pathtrace::PtCamera prevCamera{};
    u32 width = 0;   ///< composite extent (pixels)
    u32 height = 0;
    u32 gridX = 16;
    u32 gridY = 16;
    u32 gridZ = 32;
    float nearZ = 0.1f;
    float farZ = 32.f;
    VolMedium medium{};
    float temporalAlpha = 0.1f;
    u32 candidates = 4;       ///< RIS candidates per froxel (<= 32)
    float mCap = 8.f;
    float lightScale = 1.f;
    float rayEps = 1e-3f;
    u32 flags = kVolLights | kVolFog | kVolReproject;
    u32 frame = 0;
    VolParticleSystem systems[kVolSystemCount]{};
    u32 systemCount = 0;
};

/// The packed frame (params: kVolParamWordCount float4, ints: kVolIntWordCount uint4) the kernels unpack.
/// kVolHistory is added by the runners when they hold a history (callers need not set it).
void packVolParams(const VolFrameDesc& desc, u32 lightCount, Word* params, VolUint4* ints);

/// Inputs of a frame that the runner does not own.
struct VolCpuInputs {
    const lights::RelightLightSet* lights = nullptr;         ///< null / empty: ambient only
    const renderer::rt::RtReferenceScene* scene = nullptr;   ///< shadow rays (kVolShadows); null: unshadowed
    const Word* color = nullptr;                             ///< width x height (kVolApply)
    const float* depth = nullptr;                            ///< width x height view depth, 0 = sky (null: all sky)
    std::span<const particles::GpuParticleVertex> vertices{}; ///< kVolParticles
};

/// The CPU reference runner. resize() allocates; run() / runStage() make no heap allocation.
class VolumetricsCpu {
public:
    void resize(u32 gridX, u32 gridY, u32 gridZ, u32 width, u32 height);
    /// inject, temporal, integrate, apply; then this frame's history / reservoirs become the previous ones.
    bool run(const VolFrameDesc& desc, const VolCpuInputs& in);
    /// One stage on the current buffers (GPU parity replays), with `history` the kVolHistory bit to use.
    bool runStage(u32 stage, const VolFrameDesc& desc, const VolCpuInputs& in, bool history);
    void swapHistory();
    void resetHistory() { m_historyValid = false; }
    bool historyValid() const { return m_historyValid; }

    std::vector<Word>& buffer(u32 b) { return m_buffers[b]; }
    const std::vector<Word>& buffer(u32 b) const { return m_buffers[b]; }
    u32 froxelCount() const { return m_gx * m_gy * m_gz; }

private:
    std::vector<Word> m_buffers[kVolBufferCount];
    u32 m_gx = 0, m_gy = 0, m_gz = 0, m_w = 0, m_h = 0;
    bool m_historyValid = false;
};

/// Monte Carlo reference of the in-scattered light-set radiance at one point (tests): sum over lights of
/// `samples` stratified-free samples of each light's own sampler, unshadowed, x phase x volumetricScale. Returns
/// the mean and writes the standard error.
lk::float3 referenceInScatter(const lights::RelightLightSet& set, const lk::float3& p, const lk::float3& view, float g,
                              u32 samples, lk::float3* standardError);

} // namespace fuse::relight::render::volumetrics
