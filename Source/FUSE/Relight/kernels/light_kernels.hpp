// FUSE Relight RL-4.4: the light-model oracles as single-source kernels (docs/compute-kernels.md, plan §5.8 row
// `light_sample/pdf`, `light_tree_*`). The bodies call the shared core (kernels/light_core.h through light_cpp.hpp),
// the same text the Slang shaders (render/lights/shaders/rl_lights.slang) and the GLSL fallback (rl_lights.glsl)
// compile, and run on kernel::Backend::CpuReference / CpuParallel (bit-identical: no cross-item reductions).
//
//   light_d3d_convert   item kernel over game lights: packed D3DLIGHT9 (float4[5]) -> packed RlLight (float4[6]);
//                       the CPU twin of the "relight.lights.convert" pass (rl_light_convert.{slang,comp}).
//   light_set_sample    item kernel over queries: WP-7.1 light-tree selection (lt_sample / lt_pmf, the tree's own
//                       single-source sampler) + rlLightSample / rlLightPdf / rlLightEval on the chosen light; the CPU
//                       twin of the "relight.lights.sample" pass (rl_light_sample.{slang,comp}).
#pragma once

#include "light_cpp.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/light_tree/light_tree_kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::relight::lightk {

inline constexpr const char* kConvertName = "light_d3d_convert";
inline constexpr const char* kSetSampleName = "light_set_sample";
inline constexpr kernel::Dim3 kWorkgroup{64u, 1u, 1u};

struct ConvertParams {
    kernel::Span<const float4> d3d; ///< kRlD3dWords per light
    kernel::Span<float4> out;       ///< kRlLightWords per light
    RlConvertParams params{};
    u32 count = 0;
};

struct ConvertKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ConvertParams& p) const {
        const u32 i = idx.linear;
        const RlLight L = rlConvertD3dLight(rlD3dLightUnpack(p.d3d.data + i * kRlD3dWords), p.params);
        rlLightPack(L, p.out.data + i * kRlLightWords);
    }
};

struct SetSampleParams {
    renderer::light_tree::LightTreeView tree{};
    kernel::Span<const float4> table;   ///< kRlLightWords per light (tree light index = table index)
    kernel::Span<const float4> queries; ///< kRlQueryWords per query
    kernel::Span<float4> results;       ///< kRlResultWords per query
    u32 lightCount = 0;
    u32 count = 0;
};

/// One light-set query (see kRlQueryWords / kRlResultWords in light_core.h).
FUSE_HOST_DEVICE inline void rlSetSampleOne(const SetSampleParams& prm, const float4* q, float4* r) {
    const f32 p[3] = {q[0].x, q[0].y, q[0].z};
    const f32 n[3] = {q[1].x, q[1].y, q[1].z};
    const float3 pos(q[0].x, q[0].y, q[0].z);
    const renderer::light_tree::LightTreeSample ts = renderer::light_tree::lt_sample(prm.tree, p, n, q[0].w, q[1].w, q[2].w);
    for (uint k = 0; k < kRlResultWords; ++k) {
        r[k] = float4(0.f, 0.f, 0.f, 0.f);
    }
    r[0].w = -1.f;
    if (ts.light < prm.lightCount) {
        const RlLight L = rlLightUnpack(prm.table.data + ts.light * kRlLightWords);
        const RlLightSample s = rlLightSample(L, pos, q[1].w, q[2].w);
        const bool delta = (s.flags & kRlSampleDelta) != 0u;
        r[0] = float4(s.position.x, s.position.y, s.position.z, float(ts.light));
        r[1] = float4(s.wi.x, s.wi.y, s.wi.z, delta ? ts.pmf : ts.pmf * s.pdf);
        r[2] = float4(s.radiance.x, s.radiance.y, s.radiance.z, ts.pmf);
        r[4] = float4(s.dist, float(s.flags), s.pdf, 0.f);
    }
    const float e = q[3].x;
    if (e >= 0.f && uint(e) < prm.lightCount) {
        const uint light = uint(e);
        const RlLight L = rlLightUnpack(prm.table.data + light * kRlLightWords);
        const float3 wi(q[2].x, q[2].y, q[2].z);
        const float3 rad = rlLightEval(L, pos, wi);
        const f32 pmf = renderer::light_tree::lt_pmf(prm.tree, p, n, light);
        r[3] = float4(rad.x, rad.y, rad.z, pmf * rlLightPdf(L, pos, wi));
    }
}

struct SetSampleKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const SetSampleParams& p) const {
        const u32 i = idx.linear;
        rlSetSampleOne(p, p.queries.data + i * kRlQueryWords, p.results.data + i * kRlResultWords);
    }
};

} // namespace fuse::relight::lightk
