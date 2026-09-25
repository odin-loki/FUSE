// FUSE Relight RL-4.3: host side of the Relight BSDF (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.8).
//
//   AlbedoLut lut; bakeAlbedoLut(lut);          // bsdf_albedo_lut + bsdf_albedo_avg kernels (CpuParallel)
//   BsdfCases cases; cases.add(material, wo, wi, u);
//   runBsdfKernels(backend, lut, cases, out);   // bsdf_eval / bsdf_pdf / bsdf_sample kernels
//
// The math is the single-source core Relight/kernels/bsdf_core.h (C++ dialect: bsdf_cpp.hpp; Slang:
// Relight/shaders/material/bsdf.slang; GLSL fallback: bsdf.glsl). The albedo table is baked on the CPU once and
// uploaded as-is (kBsdfLutWords floats) for the shaders, so CPU and GPU read identical table values.
#pragma once

#include "bsdf_kernels.hpp" // Relight/kernels (PUBLIC include directory of fuse_relight_bsdf)

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::render::material {

namespace bsdf = fuse::relight::bsdf;

/// The Kulla-Conty albedo table (kBsdfLutWords floats: A, B, Aavg, Bavg; see bsdf_core.h).
class AlbedoLut {
public:
    const float* data() const { return m_words.empty() ? nullptr : m_words.data(); }
    u32 size() const { return static_cast<u32>(m_words.size()); }
    bool valid() const { return m_words.size() == static_cast<std::size_t>(bsdf::kBsdfLutWords); }

private:
    friend bool bakeAlbedoLut(AlbedoLut& lut, kernel::Backend backend);
    std::vector<float> m_words;
};

/// Runs bsdf_albedo_lut then bsdf_albedo_avg on `backend` (CpuReference and CpuParallel are bit-identical).
bool bakeAlbedoLut(AlbedoLut& lut, kernel::Backend backend = kernel::Backend::CpuParallel);

/// Process-wide table (baked once on first use; thread-safe).
const AlbedoLut& sharedAlbedoLut();

/// SoA case list for the three BSDF kernels.
struct BsdfCases {
    std::vector<bsdf::BsdfMaterial> materials;
    std::vector<bsdf::float3> wo;
    std::vector<bsdf::float3> wi;
    std::vector<bsdf::float4> u;

    void add(const bsdf::BsdfMaterial& m, const bsdf::float3& o, const bsdf::float3& i, const bsdf::float4& r) {
        materials.push_back(m);
        wo.push_back(o);
        wi.push_back(i);
        u.push_back(r);
    }
    u32 count() const { return static_cast<u32>(materials.size()); }
};

struct BsdfResults {
    std::vector<bsdf::float3> eval;
    std::vector<float> pdf;
    std::vector<bsdf::BsdfSample> samples;
};

/// Runs bsdf_eval, bsdf_pdf and bsdf_sample over `cases` on `backend`. False when a launch fails.
bool runBsdfKernels(kernel::Backend backend, const AlbedoLut& lut, const BsdfCases& cases, BsdfResults& out);

} // namespace fuse::relight::render::material
