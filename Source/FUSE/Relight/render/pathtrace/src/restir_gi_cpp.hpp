// FUSE Relight RL-5.3: the C++ dialect of the ReSTIR GI core (render/pathtrace/shaders/restir_gi_core.h), compiled
// after the path tracer's C++ core (kernels/pt_reference_cpp.hpp): the CPU oracle of the "relight.restir_gi.*" passes.
// Everything here is allocation-free (the WP-6.0 CPU BVH behind ptTraceScene is the path tracer's).
#pragma once

#include "pt_reference_cpp.hpp"

#include <fuse/types.hpp>

namespace fuse::relight::ptk {

using lightk::rlConcentric;

/// The buffers the ReSTIR GI core reads (not owned; kRgi*Words float4 per pixel).
struct RgiCpuContext {
    const PtCpuContext* pt = nullptr;
    const float4* surfaces[2] = {nullptr, nullptr};   ///< this frame, previous frame
    const float4* reservoirs[2] = {nullptr, nullptr}; ///< stage input, history
};

#define PT_FN inline
#define PT_CONST inline constexpr
#define PT_OUT(T) T&
#define PT_INOUT(T) T&
#define PT_L3(v) toLight3(v)
#define PT_B3(v) toBsdf3(v)
#define RGI_CTX_PARAM const RgiCpuContext &gc,
#define RGI_CTX_ARG gc,
#define RGI_PT *gc.pt,
#define RGI_LUT gc.pt->lut,
#define RGI_PARAM_WORDS(name) const float4* name

inline float4 rgiSurfaceWord(const RgiCpuContext& gc, uint slot, uint pixel, uint k) {
    return gc.surfaces[slot][pixel * 4u + k];
}
inline float4 rgiReservoirWord(const RgiCpuContext& gc, uint which, uint pixel, uint k) {
    return gc.reservoirs[which][pixel * 9u + k]; // kRgiReservoirWords
}

#include "restir_gi_core.h"

#undef PT_FN
#undef PT_CONST
#undef PT_OUT
#undef PT_INOUT
#undef PT_L3
#undef PT_B3
#undef RGI_CTX_PARAM
#undef RGI_CTX_ARG
#undef RGI_PT
#undef RGI_LUT
#undef RGI_PARAM_WORDS

} // namespace fuse::relight::ptk
