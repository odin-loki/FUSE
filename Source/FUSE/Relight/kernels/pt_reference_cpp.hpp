// FUSE Relight RL-5.1: the C++ dialect of the single-source path-tracing core (kernels/pt_reference_core.h): the CPU
// reference path tracer's scene accessors over a PtCpuContext.
//
//   scene      the WP-6.0 CPU reference of the acceleration structures (renderer rt::RtReferenceScene: the same
//              (instance slot, BLAS primitive) naming as the TLAS, double-precision Moller-Trumbore on a binned-SAH
//              BVH), traced with the path tracer's cull masks;
//   lights     the RL-4.4 RelightLightSet's CPU sampler (the WP-7.1 tree's single-source selection x the light's own
//              sample; the GPU runs the same text) and its table;
//   tables     the packed instance / triangle / material / portal / light-map words the GPU reads (PtCompiledScene,
//              render/pathtrace/pt_scene.hpp), so both sides shade from identical data;
//   textures   CPU images (linear float RGBA, bilinear, repeat, LOD 0: the GPU sampler's semantics).
//
// The BSDF and light cores keep their own C++ vector types (bsdf::float3, lightk::float3); the core converts with
// PT_L3 / PT_B3. Everything here is allocation-free; the per-pixel accumulation (render/pathtrace/src/pt_reference.cpp)
// is in double.
#pragma once

#include "bsdf_cpp.hpp"
#include "light_cpp.hpp"

#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdint>

namespace fuse::relight::ptk {

using namespace fuse::relight::bsdf;
using lightk::RlCylinderHits;
using lightk::RlLight;
using lightk::kRlKindCylinder;
using lightk::kRlKindDisk;
using lightk::kRlKindDistant;
using lightk::kRlKindRect;
using lightk::kRlKindSphere;
using lightk::kRlKindTriangle;
using lightk::kRlSampleDelta;
using lightk::rlCylinderHits;
using lightk::rlLightEval;
using lightk::rlPlanarHit;

inline lightk::float3 toLight3(const float3& v) { return lightk::float3(v.x, v.y, v.z); }
inline float3 toBsdf3(const lightk::float3& v) { return float3(v.x, v.y, v.z); }

/// A CPU texture (linear RGBA, row-major, top row first), addressed by its bindless handle in the material.
struct PtCpuTexture {
    uint handle = 0;
    uint width = 0;
    uint height = 0;
    const float4* texels = nullptr;
};

/// Everything the core reads on the CPU (not owned).
struct PtCpuContext {
    const float* lut = nullptr;                       ///< RL-4.3 albedo table (kBsdfLutWords)
    const renderer::rt::RtReferenceScene* rt = nullptr;
    const render::lights::RelightLightSet* lights = nullptr;
    const lightk::RlLight* lightRecords = nullptr;    ///< unpacked light table (lights->lightCount() entries)
    const float4* instances = nullptr;
    u32 instanceCount = 0;
    const float4* triangles = nullptr;
    u32 triangleCount = 0;
    const float4* materials = nullptr;
    u32 materialCount = 0;
    const float4* portals = nullptr;
    u32 portalCount = 0;
    const float* lightMap = nullptr;
    u32 lightMapCount = 0;
    const PtCpuTexture* textures = nullptr;
    u32 textureCount = 0;
};

#define PT_FN inline
#define PT_CONST inline constexpr
#define PT_OUT(T) T&
#define PT_INOUT(T) T&
#define PT_CTX_PARAM const PtCpuContext &ctx,
#define PT_CTX_ARG ctx,
#define PT_LUT_ARG ctx.lut,
#define PT_L3(v) toLight3(v)
#define PT_B3(v) toBsdf3(v)
#define PT_PARAM_WORDS(name) const float4* name
#define PT_MATERIAL_WORDS(name) const float4* name

#include "pt_reference_types.h"

// ---- accessors ----------------------------------------------------------------------------------------------------
PtRawHit ptRawTrace(const PtCpuContext& ctx, float3 o, float3 d, float tmin, float tmax, uint cullMask);
inline float4 ptInstanceWord(const PtCpuContext& ctx, uint slot, uint k) {
    return slot < ctx.instanceCount ? ctx.instances[slot * 4u + k] : float4(0.f, 0.f, 0.f, 0.f);
}
inline float4 ptTriangleWord(const PtCpuContext& ctx, uint tri, uint k) {
    return tri < ctx.triangleCount ? ctx.triangles[tri * 10u + k] : float4(0.f, 0.f, 0.f, 0.f);
}
inline float4 ptMaterialWord(const PtCpuContext& ctx, uint material, uint k) {
    return material < ctx.materialCount ? ctx.materials[material * 13u + k] : float4(0.f, 0.f, 0.f, 0.f);
}
inline float4 ptPortalWord(const PtCpuContext& ctx, uint portal, uint k) {
    return portal < ctx.portalCount ? ctx.portals[portal * 3u + k] : float4(0.f, 0.f, 0.f, 0.f);
}
inline float ptLightMapValue(const PtCpuContext& ctx, uint index) {
    return index < ctx.lightMapCount ? ctx.lightMap[index] : -1.f;
}
/// Bilinear, repeat, LOD 0, texel centres at (i + 0.5) / size (VkSampler LINEAR / REPEAT semantics).
inline float4 ptTextureSample(const PtCpuContext& ctx, uint texture, uint /*smp*/, float u, float v) {
    const PtCpuTexture* t = nullptr;
    for (u32 i = 0; i < ctx.textureCount; ++i) {
        if (ctx.textures[i].handle == texture) {
            t = &ctx.textures[i];
            break;
        }
    }
    if (t == nullptr || t->width == 0u || t->height == 0u) {
        return float4(1.f, 1.f, 1.f, 1.f);
    }
    const float x = u * float(t->width) - 0.5f;
    const float y = v * float(t->height) - 0.5f;
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float ax = x - fx;
    const float ay = y - fy;
    auto wrap = [](long long i, uint n) { return static_cast<uint>(((i % static_cast<long long>(n)) + n) % n); };
    const uint x0 = wrap(static_cast<long long>(fx), t->width);
    const uint x1 = wrap(static_cast<long long>(fx) + 1, t->width);
    const uint y0 = wrap(static_cast<long long>(fy), t->height);
    const uint y1 = wrap(static_cast<long long>(fy) + 1, t->height);
    const float4 a = t->texels[y0 * t->width + x0];
    const float4 b = t->texels[y0 * t->width + x1];
    const float4 c = t->texels[y1 * t->width + x0];
    const float4 d = t->texels[y1 * t->width + x1];
    auto lerp4 = [](const float4& p, const float4& q, float s) {
        return float4(p.x + (q.x - p.x) * s, p.y + (q.y - p.y) * s, p.z + (q.z - p.z) * s, p.w + (q.w - p.w) * s);
    };
    return lerp4(lerp4(a, b, ax), lerp4(c, d, ax), ay);
}
inline lightk::RlLight ptLoadLight(const PtCpuContext& ctx, uint light) { return ctx.lightRecords[light]; }
PtLightPick ptSampleLightSet(const PtCpuContext& ctx, float3 p, float3 n, float u0, float u1, float u2);
float ptLightSetPdf(const PtCpuContext& ctx, float3 p, float3 n, uint light, float3 wi);

#include "pt_reference_core.h"

inline PtRawHit ptRawTrace(const PtCpuContext& ctx, float3 o, float3 d, float tmin, float tmax, uint cullMask) {
    PtRawHit h{};
    h.hit = false;
    h.t = 0.f;
    h.instance = kPtInvalid;
    h.primitive = kPtInvalid;
    h.u = 0.f;
    h.v = 0.f;
    if (ctx.rt == nullptr) {
        return h;
    }
    renderer::rt::RtProbeRay ray{};
    ray.origin[0] = o.x;
    ray.origin[1] = o.y;
    ray.origin[2] = o.z;
    ray.direction[0] = d.x;
    ray.direction[1] = d.y;
    ray.direction[2] = d.z;
    ray.tMin = tmin;
    ray.tMax = tmax;
    const renderer::rt::RtRefHit r = ctx.rt->trace(ray, cullMask);
    if (!r.hit) {
        return h;
    }
    h.hit = true;
    h.t = static_cast<float>(r.t);
    h.instance = r.instance;
    h.primitive = r.primitive;
    h.u = static_cast<float>(r.u);
    h.v = static_cast<float>(r.v);
    return h;
}

inline PtLightPick ptSampleLightSet(const PtCpuContext& ctx, float3 p, float3 n, float u0, float u1, float u2) {
    PtLightPick pick{};
    pick.light = kPtInvalid;
    pick.pdf = 0.f;
    pick.wi = float3(0.f, 0.f, 1.f);
    pick.dist = 0.f;
    pick.radiance = float3(0.f, 0.f, 0.f);
    pick.delta = false;
    if (ctx.lights == nullptr) {
        return pick;
    }
    const render::lights::LightSetSample s = ctx.lights->sample(toLight3(p), toLight3(n), u0, u1, u2);
    if (s.light >= ctx.lights->lightCount() || (s.shape.flags & lightk::kRlSampleValid) == 0u) {
        return pick;
    }
    pick.light = s.light;
    pick.pdf = s.pdf;
    pick.wi = toBsdf3(s.shape.wi);
    pick.dist = s.shape.dist;
    pick.radiance = toBsdf3(s.shape.radiance);
    pick.delta = (s.shape.flags & lightk::kRlSampleDelta) != 0u;
    return pick;
}

inline float ptLightSetPdf(const PtCpuContext& ctx, float3 p, float3 n, uint light, float3 wi) {
    if (ctx.lights == nullptr || light >= ctx.lights->lightCount()) {
        return 0.f;
    }
    return ctx.lights->pdf(toLight3(p), toLight3(n), light, toLight3(wi));
}

#undef PT_FN
#undef PT_CONST
#undef PT_OUT
#undef PT_INOUT
#undef PT_CTX_PARAM
#undef PT_CTX_ARG
#undef PT_LUT_ARG
#undef PT_L3
#undef PT_B3
#undef PT_PARAM_WORDS
#undef PT_MATERIAL_WORDS

} // namespace fuse::relight::ptk
