// FUSE Relight RL-5.5: the C++ dialect of the A-SVGF gradient producer (shaders/rl_dn_gradient_core.h), compiled after
// the path tracer's C++ core (kernels/pt_reference_cpp.hpp): the CPU oracle of "relight.denoise.gradient".
// Allocation-free (the WP-6.0 CPU BVH behind ptTraceScene is the path tracer's).
#pragma once

#include "pt_reference_cpp.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::relight::ptk {

#define PT_FN inline
#define PT_CONST inline constexpr
#define PT_OUT(T) T&
#define PT_CTX_PARAM const PtCpuContext &ctx,
#define PT_CTX_ARG ctx,

#include "rl_dn_gradient_core.h"

#undef PT_FN
#undef PT_CONST
#undef PT_OUT
#undef PT_CTX_PARAM
#undef PT_CTX_ARG

inline constexpr const char* kGradientName = "relight_denoise_gradient";

/// compute_kernel params of the producer (one item per stratum).
struct RldnParams {
    const PtCpuContext* ctx = nullptr;
    const float4* params = nullptr; ///< kPtParamWords of this frame, then the previous frame's
    kernel::Span<float4> records; ///< per stratum (read, then rewritten)
    kernel::Span<float4> gradient; ///< per stratum (out)
    u32 strataW = 0;
    u32 strataH = 0;
    bool havePrev = false;
};

struct RldnKernel {
    void operator()(const kernel::LaunchIndex& idx, const RldnParams& p) const {
        const u32 sx = idx.global.x;
        const u32 sy = idx.global.y;
        if (sx >= p.strataW || sy >= p.strataH) {
            return;
        }
        const PtParams cur = ptParamsUnpack(p.params);
        const PtParams prev = ptParamsUnpack(p.params + kPtParamWords);
        const u32 s = sy * p.strataW + sx;
        float4 grad;
        float4 next;
        rldnStratum(*p.ctx, cur, prev, p.havePrev, p.records[s], sx, sy, grad, next);
        p.gradient[s] = grad;
        p.records[s] = next;
    }
};

} // namespace fuse::relight::ptk
