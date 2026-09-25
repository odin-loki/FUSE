// FUSE Relight RL-5.1: the CPU reference path tracer as a single-source kernel (docs/compute-kernels.md; plan §5.8 row
// `pt_reference`). The body runs the shared core (kernels/pt_reference_core.h through pt_reference_cpp.hpp) - the text
// the GPU pass "relight.pt.trace" (render/pathtrace/shaders/rl_pt_trace.{slang,comp}) compiles - on
// kernel::Backend::CpuReference / CpuParallel (bit-identical: one pixel per item, no cross-item reduction).
//
//   pt_reference_render   item kernel over pixels: `samples` paths per pixel (sample indices sampleBase ..), summed
//                         in DOUBLE into PtReferencePixel (radiance sum and sum of squares - the per-pixel variance
//                         of the parity gates -, the demodulated channels), the G-buffer of the first sample.
#pragma once

#include "pt_reference_cpp.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::relight::ptk {

inline constexpr const char* kReferenceName = "pt_reference_render";
inline constexpr kernel::Dim3 kReferenceWorkgroup{8u, 8u, 1u};

/// One pixel of the reference (sums in double).
struct PtReferencePixel {
    double sum[3] = {0.0, 0.0, 0.0};
    double sumSq[3] = {0.0, 0.0, 0.0};
    double emissive[3] = {0.0, 0.0, 0.0};
    double diffuse[3] = {0.0, 0.0, 0.0};
    double specular[3] = {0.0, 0.0, 0.0};
    u32 samples = 0;
    // G-buffer (first sample of the call)
    float albedoD[3] = {0.f, 0.f, 0.f};
    float albedoS[3] = {0.f, 0.f, 0.f};
    float normal[3] = {0.f, 0.f, 0.f};
    float roughness = 0.f;
    float depth = 0.f;
    float hitDist = 0.f;
    float motion[2] = {0.f, 0.f};
    u32 instance = 0xFFFFFFFFu;
    u32 psr = 0;
    u32 flags = 0;
};

struct PtReferenceParams {
    const PtCpuContext* ctx = nullptr;
    const float4* params = nullptr; ///< kPtParamWords
    kernel::Span<PtReferencePixel> pixels;
    u32 width = 0;
    u32 height = 0;
    u32 samples = 0;
    bool writeGbuffer = true;
};

struct PtReferenceKernel {
    void operator()(const kernel::LaunchIndex& idx, const PtReferenceParams& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        if (x >= p.width || y >= p.height) {
            return;
        }
        const PtParams P = ptParamsUnpack(p.params);
        PtReferencePixel& px = p.pixels.data[std::size_t(y) * p.width + x];
        for (u32 s = 0; s < p.samples; ++s) {
            const PtSample r = ptRenderSample(*p.ctx, P, x, y, P.sampleBase + s);
            const double c[3] = {double(r.radiance.x), double(r.radiance.y), double(r.radiance.z)};
            for (int k = 0; k < 3; ++k) {
                px.sum[k] += c[k];
                px.sumSq[k] += c[k] * c[k];
            }
            px.emissive[0] += double(r.emissive.x);
            px.emissive[1] += double(r.emissive.y);
            px.emissive[2] += double(r.emissive.z);
            px.diffuse[0] += double(r.diffuse.x);
            px.diffuse[1] += double(r.diffuse.y);
            px.diffuse[2] += double(r.diffuse.z);
            px.specular[0] += double(r.specular.x);
            px.specular[1] += double(r.specular.y);
            px.specular[2] += double(r.specular.z);
            ++px.samples;
            if (s == 0u && p.writeGbuffer) {
                px.albedoD[0] = r.albedoD.x;
                px.albedoD[1] = r.albedoD.y;
                px.albedoD[2] = r.albedoD.z;
                px.albedoS[0] = r.albedoS.x;
                px.albedoS[1] = r.albedoS.y;
                px.albedoS[2] = r.albedoS.z;
                px.normal[0] = r.normal.x;
                px.normal[1] = r.normal.y;
                px.normal[2] = r.normal.z;
                px.roughness = r.roughness;
                px.depth = r.depth;
                px.hitDist = r.hitDist;
                px.motion[0] = r.motionX;
                px.motion[1] = r.motionY;
                px.instance = r.instance;
                px.psr = r.psr;
                px.flags = r.flags;
            }
        }
    }
};

} // namespace fuse::relight::ptk
