// FUSE Relight RL-4.3: albedo-table bake and the CPU launches of the BSDF kernels (see bsdf_host.hpp).
#include <fuse/relight/render/material/bsdf_host.hpp>

#include <mutex>

namespace fuse::relight::render::material {

bool bakeAlbedoLut(AlbedoLut& lut, kernel::Backend backend) {
    lut.m_words.assign(static_cast<std::size_t>(bsdf::kBsdfLutWords), 0.f);
    const bsdf::AlbedoLutParams params{lut.m_words.data()};
    constexpr u32 kCells = static_cast<u32>(bsdf::kBsdfLutSize * bsdf::kBsdfLutSize);
    const kernel::LaunchResult cells = kernel::launch(
        backend, kernel::KernelLaunch{bsdf::kAlbedoLutName, kernel::extent1(kCells), {32u, 1u, 1u}},
        bsdf::AlbedoLutKernel{}, params);
    if (!cells.ok) {
        lut.m_words.clear();
        return false;
    }
    const kernel::LaunchResult rows = kernel::launch(
        backend,
        kernel::KernelLaunch{bsdf::kAlbedoAvgName, kernel::extent1(static_cast<u32>(bsdf::kBsdfLutSize)), {32u, 1u, 1u}},
        bsdf::AlbedoAvgKernel{}, params);
    if (!rows.ok) {
        lut.m_words.clear();
        return false;
    }
    return true;
}

const AlbedoLut& sharedAlbedoLut() {
    static AlbedoLut lut;
    static std::once_flag once;
    std::call_once(once, [] { bakeAlbedoLut(lut, kernel::Backend::CpuParallel); });
    return lut;
}

bool runBsdfKernels(kernel::Backend backend, const AlbedoLut& lut, const BsdfCases& cases, BsdfResults& out) {
    const u32 n = cases.count();
    if (!lut.valid() || cases.wo.size() != n || cases.wi.size() != n || cases.u.size() != n) {
        return false;
    }
    out.eval.assign(n, bsdf::float3{});
    out.pdf.assign(n, 0.f);
    out.samples.assign(n, bsdf::bsdfSampleInvalid());
    if (n == 0u) {
        return true;
    }
    bsdf::BsdfCaseParams p{};
    p.materials = kernel::Span<const bsdf::BsdfMaterial>{cases.materials.data(), n};
    p.wo = kernel::Span<const bsdf::float3>{cases.wo.data(), n};
    p.wi = kernel::Span<const bsdf::float3>{cases.wi.data(), n};
    p.u = kernel::Span<const bsdf::float4>{cases.u.data(), n};
    p.lut = lut.data();
    p.outEval = kernel::Span<bsdf::float3>{out.eval.data(), n};
    p.outPdf = kernel::Span<float>{out.pdf.data(), n};
    p.outSample = kernel::Span<bsdf::BsdfSample>{out.samples.data(), n};
    p.count = n;
    if (!bsdf::caseParamsValid(p)) {
        return false;
    }
    const kernel::Dim3 grid = kernel::extent1(n);
    const bool ok = kernel::launch(backend, kernel::KernelLaunch{bsdf::kEvalName, grid, bsdf::kWorkgroup},
                                   bsdf::BsdfEvalKernel{}, p).ok &&
                    kernel::launch(backend, kernel::KernelLaunch{bsdf::kPdfName, grid, bsdf::kWorkgroup},
                                   bsdf::BsdfPdfKernel{}, p).ok &&
                    kernel::launch(backend, kernel::KernelLaunch{bsdf::kSampleName, grid, bsdf::kWorkgroup},
                                   bsdf::BsdfSampleKernel{}, p).ok;
    return ok;
}

} // namespace fuse::relight::render::material
