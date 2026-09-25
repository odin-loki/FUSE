// FUSE Relight RL-5.2: the C++ dialect of the ReSTIR DI core (render/pathtrace/shaders/restir_di_core.h), compiled
// after the path tracer's C++ core (kernels/pt_reference_cpp.hpp): the CPU oracle of the "relight.restir_di.*" passes.
// Everything here is allocation-free (the WP-6.0 CPU BVH behind ptVisible is the path tracer's).
#pragma once

#include "pt_reference_cpp.hpp"

#include <fuse/types.hpp>

namespace fuse::relight::ptk {

using lightk::kRlFlagTwoSided;
using lightk::kRlSampleValid;
using lightk::RlLightSample;
using lightk::rlConcentric;
using lightk::rlLightSample;
using lightk::rlPlanarNormal;
using lightk::rlShapingAt;

/// The buffers the ReSTIR DI core reads (not owned; kRdi*Words float4 per pixel).
struct RdiCpuContext {
    const PtCpuContext* pt = nullptr;
    const float4* surfaces[2] = {nullptr, nullptr}; ///< this frame, previous frame
    const float4* reservoirs[2] = {nullptr, nullptr}; ///< stage input, history
    const uint* tiles = nullptr;
    const float* pmf = nullptr;
    const float* cdf = nullptr;
};

#define PT_FN inline
#define PT_CONST inline constexpr
#define PT_OUT(T) T&
#define PT_INOUT(T) T&
#define PT_L3(v) toLight3(v)
#define PT_B3(v) toBsdf3(v)
#define RDI_CTX_PARAM const RdiCpuContext &rc,
#define RDI_CTX_ARG rc,
#define RDI_PT *rc.pt,
#define RDI_LUT rc.pt->lut,
#define RDI_PARAM_WORDS(name) const float4* name

inline float4 rdiSurfaceWord(const RdiCpuContext& rc, uint slot, uint pixel, uint k) {
    return rc.surfaces[slot][pixel * 4u + k];
}
inline float4 rdiReservoirWord(const RdiCpuContext& rc, uint which, uint pixel, uint k) {
    return rc.reservoirs[which][pixel * 2u + k];
}
inline uint rdiTileLight(const RdiCpuContext& rc, uint entry) { return rc.tiles[entry]; }
inline float rdiTilePmf(const RdiCpuContext& rc, uint light) { return rc.pmf != nullptr ? rc.pmf[light] : 0.f; }
inline float rdiCdf(const RdiCpuContext& rc, uint light) { return rc.cdf[light]; }
inline float rdiTreePmf(const RdiCpuContext& rc, float3 p, float3 n, uint light) {
    if (rc.pt->lights == nullptr || light >= rc.pt->lights->lightCount()) {
        return 0.f;
    }
    return rc.pt->lights->pmf(toLight3(p), toLight3(n), light);
}

#include "restir_di_core.h"

#undef PT_FN
#undef PT_CONST
#undef PT_OUT
#undef PT_INOUT
#undef PT_L3
#undef PT_B3
#undef RDI_CTX_PARAM
#undef RDI_CTX_ARG
#undef RDI_PT
#undef RDI_LUT
#undef RDI_PARAM_WORDS

} // namespace fuse::relight::ptk
