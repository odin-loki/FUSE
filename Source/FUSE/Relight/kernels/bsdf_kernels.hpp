// FUSE Relight RL-4.3: the BSDF oracles as single-source kernels (docs/compute-kernels.md, plan §5.8 row
// `bsdf_eval/sample/pdf`). The bodies call the shared core (kernels/bsdf_core.h through bsdf_cpp.hpp), the same text
// the Slang shader (shaders/material/bsdf.slang) and the GLSL fallback (bsdf.glsl) compile, and run on
// kernel::Backend::CpuReference / CpuParallel (bit-identical: no cross-item reductions).
//
//   bsdf_eval         item kernel over cases: out = bsdfEval(material, wo, wi)       (projected f |cos|)
//   bsdf_pdf          item kernel over cases: out = bsdfPdf(material, wo, wi)
//   bsdf_sample       item kernel over cases: out = bsdfSample(material, wo, u)
//   bsdf_albedo_lut   item kernel over the kBsdfLutSize^2 cells: the Schlick split (A, B) of the single-scattering
//                     GGX albedo at (mu, perceptual roughness), by stratified VNDF quadrature; then
//   bsdf_albedo_avg   item kernel over the rows: the exact cosine-weighted averages of the piecewise-linear columns.
#pragma once

#include "bsdf_cpp.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::relight::bsdf {

inline constexpr const char* kEvalName = "bsdf_eval";
inline constexpr const char* kPdfName = "bsdf_pdf";
inline constexpr const char* kSampleName = "bsdf_sample";
inline constexpr const char* kAlbedoLutName = "bsdf_albedo_lut";
inline constexpr const char* kAlbedoAvgName = "bsdf_albedo_avg";
inline constexpr kernel::Dim3 kWorkgroup{64u, 1u, 1u};

/// SoA cases shared by the three BSDF kernels (one material per case).
struct BsdfCaseParams {
    kernel::Span<const BsdfMaterial> materials;
    kernel::Span<const float3> wo;
    kernel::Span<const float3> wi;   ///< eval / pdf
    kernel::Span<const float4> u;    ///< sample
    const float* lut = nullptr;      ///< kBsdfLutWords
    kernel::Span<float3> outEval;
    kernel::Span<float> outPdf;
    kernel::Span<BsdfSample> outSample;
    u32 count = 0;
};

inline bool caseParamsValid(const BsdfCaseParams& p) {
    return p.lut != nullptr && p.materials.size >= p.count && p.wo.size >= p.count;
}

struct BsdfEvalKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BsdfCaseParams& p) const {
        const u32 i = idx.linear;
        p.outEval[i] = bsdfEval(p.lut, p.materials[i], p.wo[i], p.wi[i]);
    }
};

struct BsdfPdfKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BsdfCaseParams& p) const {
        const u32 i = idx.linear;
        p.outPdf[i] = bsdfPdf(p.lut, p.materials[i], p.wo[i], p.wi[i]);
    }
};

struct BsdfSampleKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BsdfCaseParams& p) const {
        const u32 i = idx.linear;
        p.outSample[i] = bsdfSample(p.lut, p.materials[i], p.wo[i], p.u[i]);
    }
};

// ---- albedo table -----------------------------------------------------------------------------------------------

/// Stratification per axis of the table quadrature (kLutStrata^2 VNDF samples per cell).
inline constexpr u32 kLutStrata = 64u;

/// Table abscissa: mu_j = max(j / (N - 1), kLutMinMu); rho_i = i / (N - 1).
inline constexpr float kLutMinMu = 1e-3f;
FUSE_HOST_DEVICE inline float lutMu(int j) { return max(float(j) / float(kBsdfLutSize - 1), kLutMinMu); }
FUSE_HOST_DEVICE inline float lutRho(int i) { return float(i) / float(kBsdfLutSize - 1); }

struct AlbedoLutParams {
    float* lut = nullptr; ///< kBsdfLutWords
};

/// One cell (row = roughness, column = mu): E_F0 = int F D G2 / (4 mu_o) dh with VNDF importance sampling: the
/// estimator is F G2 / G1; Schlick F = F0 (1 - Fc) + Fc splits it into A = E[(1 - Fc) G2/G1], B = E[Fc G2/G1].
struct AlbedoLutKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const AlbedoLutParams& p) const {
        const int col = int(idx.linear % u32(kBsdfLutSize));
        const int row = int(idx.linear / u32(kBsdfLutSize));
        const float mu = lutMu(col);
        const float rho = lutRho(row);
        float ax = 0.f;
        float ay = 0.f;
        bsdfAlphas(rho, 0.f, ax, ay);
        const float3 wo(sqrt(max(0.f, 1.f - mu * mu)), 0.f, mu);
        double a = 0.0;
        double b = 0.0;
        for (u32 sy = 0; sy < kLutStrata; ++sy) {
            for (u32 sx = 0; sx < kLutStrata; ++sx) {
                const float u1 = (float(sx) + 0.5f) / float(kLutStrata);
                const float u2 = (float(sy) + 0.5f) / float(kLutStrata);
                const float3 h = bsdfGgxVndfSample(wo, ax, ay, u1, u2);
                const float3 wi = bsdfReflect(wo, h);
                if (!(wi.z > 0.f)) {
                    continue;
                }
                const float g1o = bsdfGgxG1(wo, ax, ay);
                const float g2 = 4.f * wo.z * wi.z * bsdfGgxVisibility(wo, wi, ax, ay);
                const float w = g2 / g1o;
                const float fc = bsdfPow5(1.f - bsdfSaturate(dot(wo, h)));
                a += double((1.f - fc) * w);
                b += double(fc * w);
            }
        }
        const double n = double(kLutStrata) * double(kLutStrata);
        p.lut[kBsdfLutA + int(idx.linear)] = float(a / n);
        p.lut[kBsdfLutB + int(idx.linear)] = float(b / n);
    }
};

/// Per row: X_avg = 2 int_0^1 X(mu) mu dmu of the piecewise-linear interpolant through the table columns (exactly
/// what bsdfLutFetch returns), so E_avg is consistent with the E(mu) the albedo scaling divides by.
struct AlbedoAvgKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const AlbedoLutParams& p) const {
        const int row = int(idx.linear);
        for (int t = 0; t < 2; ++t) {
            const int base = (t == 0 ? kBsdfLutA : kBsdfLutB) + row * kBsdfLutSize;
            double sum = 0.0;
            for (int j = 0; j + 1 < kBsdfLutSize; ++j) {
                // bsdfLutFetch interpolates between column nodes j / (N - 1) (the node abscissa, not lutMu).
                const double x0 = double(j) / double(kBsdfLutSize - 1);
                const double x1 = double(j + 1) / double(kBsdfLutSize - 1);
                const double e0 = double(p.lut[base + j]);
                const double e1 = double(p.lut[base + j + 1]);
                const double h = x1 - x0;
                // int_x0^x1 (e0 + (e1 - e0)(x - x0)/h) x dx
                const double i0 = e0 * (x1 * x1 - x0 * x0) * 0.5;
                const double i1 = (e1 - e0) / h * ((x1 * x1 * x1 - x0 * x0 * x0) / 3.0 - x0 * (x1 * x1 - x0 * x0) * 0.5);
                sum += i0 + i1;
            }
            p.lut[(t == 0 ? kBsdfLutAavg : kBsdfLutBavg) + row] = float(2.0 * sum);
        }
    }
};

} // namespace fuse::relight::bsdf
