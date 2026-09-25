// FUSE Relight RL-5.1: the CPU reference path tracer (plan §5.8 row `pt_reference`, §6.5): the oracle of the GPU path
// tracer, over the same PtCompiledScene (identical cooked triangles, tables and light set) with the WP-6.0 CPU
// acceleration-structure reference (double-precision intersection) and per-pixel sums in double.
//
//   PtReferenceImage img; img.resize(w, h);
//   renderReference(scene, settings, w, h, frameSeed, sampleBase, samples, img);   // accumulates into img
//   img.mean(x, y) / img.variance(x, y)                                             // radiance statistics
//
// Deterministic: sample i of pixel (x, y) uses the counter-based random numbers of (x, y, frameSeed, i) on both the
// CPU and the GPU, and CpuReference == CpuParallel bit for bit.
#pragma once

#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::ptk {
struct PtReferencePixel;
struct PtDiHook;
struct PtGiHook;
struct PtRcHook;
}

namespace fuse::relight::render::pathtrace {

struct PtReferenceStats {
    double seconds = 0.0;
    u64 paths = 0;
};

class PtReferenceImage {
public:
    PtReferenceImage();
    ~PtReferenceImage();
    PtReferenceImage(const PtReferenceImage&);
    PtReferenceImage& operator=(const PtReferenceImage&);

    void resize(u32 width, u32 height); ///< clears
    void clear();
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const ptk::PtReferencePixel& pixel(u32 x, u32 y) const;
    ptk::PtReferencePixel* data();
    u32 samples(u32 x, u32 y) const;
    /// Mean radiance of channel c.
    double mean(u32 x, u32 y, u32 c) const;
    /// Unbiased per-sample variance of channel c (0 below two samples).
    double variance(u32 x, u32 y, u32 c) const;
    /// Mean of the demodulated channels (emissive, diffuse, specular: 0, 1, 2) of channel c.
    double channel(u32 x, u32 y, u32 which, u32 c) const;

private:
    u32 m_width = 0;
    u32 m_height = 0;
    std::vector<ptk::PtReferencePixel> m_pixels;
};

/// Traces `samples` paths per pixel (sample indices sampleBase .. sampleBase + samples - 1) and adds them to `image`
/// (sized width x height; its G-buffer fields come from sample `sampleBase`). False on invalid input.
/// `diHook`: RL-5.2's ReSTIR DI hook (ptRestirDiVertex; render/pathtrace/restir_di.hpp), null: none.
/// `giHook`: RL-5.3's ReSTIR GI hook (ptRestirGiVertex; render/pathtrace/restir_gi.hpp), null: none.
/// `rcHook`: RL-5.4's radiance cache hook (ptRadianceCacheVertex / End; radiance_cache/radiance_cache.hpp), null: none.
bool renderReference(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
                     u32 sampleBase, u32 samples, PtReferenceImage& image,
                     kernel::Backend backend = kernel::Backend::CpuParallel, PtReferenceStats* stats = nullptr,
                     const ptk::PtDiHook* diHook = nullptr, const ptk::PtGiHook* giHook = nullptr,
                     const ptk::PtRcHook* rcHook = nullptr);

} // namespace fuse::relight::render::pathtrace
