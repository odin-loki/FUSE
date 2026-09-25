// FUSE Relight RL-5.6: the C++ dialect of the volumetrics core (shaders/rl_vol_types.h + rl_vol_core.h) over a
// VolCpuContext: the CPU reference of the "relight.vol.*" kernels. Float semantics: the <cmath> float overloads
// (no double promotion); the GPU runs the same text (Slang -fp-mode precise / GLSL), the differences are the device's
// transcendental and division rounding (parity gates in tests/).
#pragma once

#include "light_cpp.hpp"

#include <fuse/relight/particles/particle_types.hpp>
#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdint>

namespace fuse::relight::volk {

using namespace fuse::relight::lightk;
using lightk::max;
using lightk::min;

struct uint4 {
    uint x = 0;
    uint y = 0;
    uint z = 0;
    uint w = 0;
};

inline float exp(float x) { return std::exp(x); }
inline float log(float x) { return std::log(x); }
inline float floor(float x) { return std::floor(x); }
inline constexpr uint min(uint a, uint b) { return b < a ? b : a; }
inline constexpr uint max(uint a, uint b) { return a < b ? b : a; }

/// Everything the core reads and writes on the CPU (not owned).
struct VolCpuContext {
    float4* buffers[8] = {};
    const float* depth = nullptr;
    const particles::GpuParticleVertex* vertices = nullptr;
    uint vertexCount = 0;
    const uint4* systems = nullptr;
    const render::lights::RelightLightSet* lights = nullptr;
    const renderer::rt::RtReferenceScene* scene = nullptr;
};

#define VOL_FN inline
#define VOL_CONST inline constexpr
#define VOL_OUT(T) T&
#define VOL_INOUT(T) T&
#define VOL_CTX_PARAM const VolCpuContext &ctx,
#define VOL_CTX_ARG ctx,
#define VOL_PARAM_WORDS(name) const float4* name
#define VOL_INT_WORDS(name) const uint4* name

#include "rl_vol_types.h"

inline float4 volLoad(const VolCpuContext& ctx, uint buf, uint index) { return ctx.buffers[buf][index]; }
inline void volStore(const VolCpuContext& ctx, uint buf, uint index, float4 v) { ctx.buffers[buf][index] = v; }
inline float volLoadDepth(const VolCpuContext& ctx, uint pixel) {
    return ctx.depth != nullptr ? ctx.depth[pixel] : 0.f;
}
inline float volUnorm8(uint c, uint shift) { return float((c >> shift) & 255u) * (1.0f / 255.0f); }
inline VolVertex volLoadVertex(const VolCpuContext& ctx, uint vertex) {
    VolVertex v;
    if (ctx.vertices == nullptr || vertex >= ctx.vertexCount) {
        v.position = float3(0.f, 0.f, 0.f);
        v.color = float4(0.f, 0.f, 0.f, 0.f);
        return v;
    }
    const particles::GpuParticleVertex& g = ctx.vertices[vertex];
    v.position = float3(g.position[0], g.position[1], g.position[2]);
    v.color = float4(volUnorm8(g.color, 16u), volUnorm8(g.color, 8u), volUnorm8(g.color, 0u), volUnorm8(g.color, 24u));
    return v;
}
inline uint4 volLoadSystem(const VolCpuContext& ctx, uint system) { return ctx.systems[system]; }
inline RlLight volLoadLight(const VolCpuContext& ctx, uint light) { return ctx.lights->light(light); }
inline VolLightPick volSampleLight(const VolCpuContext& ctx, float3 p, float u0, float u1, float u2) {
    const render::lights::LightSetSample s = ctx.lights->sample(p, float3(0.f, 0.f, 0.f), u0, u1, u2);
    VolLightPick r;
    r.light = s.light;
    r.pdf = s.pdf;
    r.wi = s.shape.wi;
    r.dist = s.shape.dist;
    r.radiance = s.shape.radiance;
    r.flags = s.shape.flags;
    return r;
}
inline bool volOccluded(const VolCpuContext& ctx, float3 o, float3 d, float tmin, float tmax) {
    if (ctx.scene == nullptr) {
        return false;
    }
    renderer::rt::RtProbeRay ray;
    ray.origin[0] = o.x;
    ray.origin[1] = o.y;
    ray.origin[2] = o.z;
    ray.direction[0] = d.x;
    ray.direction[1] = d.y;
    ray.direction[2] = d.z;
    ray.tMin = tmin;
    ray.tMax = tmax;
    return ctx.scene->trace(ray, 2u /* kPtMaskShadow */).hit;
}

#include "rl_vol_core.h"

#undef VOL_FN
#undef VOL_CONST
#undef VOL_OUT
#undef VOL_INOUT
#undef VOL_CTX_PARAM
#undef VOL_CTX_ARG
#undef VOL_PARAM_WORDS
#undef VOL_INT_WORDS

} // namespace fuse::relight::volk
