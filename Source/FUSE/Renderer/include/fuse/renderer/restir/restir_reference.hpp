#pragma once

// WP-7.2 ReSTIR DI and GI: CPU side.
//
// RestirCpu        the whole per-frame chain of the GPU (restir.di.initial -> .temporal -> .spatial x N,
//                  restir.gi.initial -> .temporal -> .spatial x N, restir.shade) over a caller-supplied surface
//                  buffer, run with the single-source kernel (restir_kernel.hpp, f32, bit-identical to the
//                  GPU kernels on the same inputs) and a caller tracer (RestirCpuScene). Keeps the history,
//                  surface and reservoir buffers of the GPU (same slots, same stage sections); steady-state
//                  frames make no heap allocation.
// RestirReference  the ground truth of the estimators in double precision, independent of the kernel: direct
//                  lighting 1 / pi x integral of Le G V dA (power-proportional light selection, uniform area
//                  points), one-bounce indirect 1 / pi x integral of Lo cos d omega (cosine-weighted rays,
//                  next-event estimation at the hit with the same light distribution). Each estimate returns its
//                  standard error, so the gates compare ReSTIR's ensemble mean with the reference within
//                  confidence bounds (fuse_rp_restir_unbiased).

#include <fuse/renderer/light_tree/light_tree_kernel.hpp>
#include <fuse/renderer/restir/restir.hpp>
#include <fuse/renderer/restir/restir_kernel.hpp>
#include <fuse/renderer/restir/restir_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::restir {

/// Rays of the CPU runner (f32 inputs, the caller decides the precision of the test).
class RestirCpuScene {
public:
    virtual ~RestirCpuScene() = default;
    virtual bool occluded(const RV3& origin, const RV3& dir, f32 tMax) const = 0;
    virtual bool traceHit(const RV3& origin, const RV3& dir, f32 tMin, f32 tMax, RestirHitF& hit) const = 0;
};

struct RestirCpuStats {
    u64 shadowRays = 0; ///< cumulative
    u64 giRays = 0;
    u32 frames = 0;
};

class RestirCpu {
public:
    /// Next frame starts without history.
    void reset() { m_history = false; }
    /// Pre-sizes every buffer for an extent (runFrame then allocates nothing).
    void reserve(u32 width, u32 height);

    /// One frame. `surfaces`: width x height visible points of this frame; `motion`: 2 floats per pixel (UV
    /// motion current - previous) or null (zero motion). False on a bad extent / camera.
    bool runFrame(const RestirSettings& settings, const RestirCamera& camera, u32 width, u32 height, u32 frameIndex, u32 seed,
                  const RestirSurfaceF* surfaces, const f32* motion, const light_tree::LightTreeView& tree,
                  const RestirLight* lights, u32 lightCount, const RestirCpuScene& scene);

    const RestirFrameConstants& constants() const { return m_constants; }
    /// f32x4 per pixel (rgb, 0), demodulated.
    const std::vector<f32>& diSignal() const { return m_diSignal; }
    const std::vector<f32>& giSignal() const { return m_giSignal; }
    const std::vector<f32>& depth() const { return m_depth; }
    /// This frame's final reservoirs (== the history the next frame reads).
    const std::vector<RestirDiReservoir>& diFinal() const { return m_diHistory[m_slot]; }
    const std::vector<RestirGiReservoir>& giFinal() const { return m_giHistory[m_slot]; }
    const RestirCpuStats& stats() const { return m_stats; }

private:
    struct Env;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_slot = 0; ///< surfaces / history slot written this frame
    bool m_history = false;
    RestirFrameConstants m_constants{};
    std::vector<RestirSurfaceF> m_surfaces[2];
    std::vector<RestirDiReservoir> m_diStage[3];
    std::vector<RestirGiReservoir> m_giStage[3];
    std::vector<RestirDiReservoir> m_diHistory[2];
    std::vector<RestirGiReservoir> m_giHistory[2];
    std::vector<f32> m_motion;
    std::vector<f32> m_diSignal;
    std::vector<f32> m_giSignal;
    std::vector<f32> m_depth;
    RestirCpuStats m_stats{};
};

// --- f64 ground truth --------------------------------------------------------------------------------------
/// Rays in double precision (the test scene's exact geometry).
class RestirReferenceScene {
public:
    virtual ~RestirReferenceScene() = default;
    virtual bool occluded(const f64 origin[3], const f64 dir[3], f64 tMax) const = 0;
    /// Closest hit in (tMin, tMax): t, a geometric normal (any side / length) and the albedo.
    virtual bool traceHit(const f64 origin[3], const f64 dir[3], f64 tMin, f64 tMax, f64& t, f64 normal[3],
                          f64 albedo[3]) const = 0;
};

/// A visible point in double precision.
struct RestirRefSurface {
    f64 p[3] = {0.0, 0.0, 0.0};
    f64 n[3] = {0.0, 0.0, 1.0};
    bool valid = false;
};

/// Monte Carlo estimate: mean (rgb) and the standard error of the mean of its luminance.
struct RestirRefEstimate {
    f64 mean[3] = {0.0, 0.0, 0.0};
    f64 luminance = 0.0;
    f64 stdError = 0.0; ///< of `luminance`
    u32 samples = 0;
};

struct RestirRefParams {
    f64 normalBias = 1.0e-3; ///< shadow-ray origin offset n (normalBias + viewBias |p - camera|)
    f64 viewBias = 1.0e-4;
    f64 camera[3] = {0.0, 0.0, 0.0};
    f64 giRayTMin = 1.0e-3;
    f64 farDistance = 1.0e4;
};

class RestirReference {
public:
    /// Light distribution proportional to luminance(radiance) x area (area kinds) / luminance(intensity).
    bool build(const light_tree::LightTreeView& tree, const RestirLight* lights, u32 count);
    RestirRefEstimate direct(const RestirRefSurface& s, const RestirReferenceScene& scene, const RestirRefParams& params,
                             u32 samples, u64 seed) const;
    RestirRefEstimate indirect(const RestirRefSurface& s, const RestirReferenceScene& scene, const RestirRefParams& params,
                               u32 samples, u64 seed) const;
    u32 lightCount() const { return static_cast<u32>(m_cdf.size()); }

private:
    struct Rng {
        u64 state = 0;
        f64 next();
    };
    /// One next-event sample of the demodulated direct radiance Le G V / (pi p) at (p, n) (rgb).
    void sampleDirect(const f64 p[3], const f64 n[3], const RestirReferenceScene& scene, const RestirRefParams& params,
                      Rng& rng, f64 out[3]) const;
    std::vector<light_tree::LightTreeEmitter> m_emitters;
    std::vector<RestirLight> m_lights;
    std::vector<f64> m_cdf;  ///< inclusive, normalised
    std::vector<f64> m_prob; ///< per light
};

} // namespace fuse::renderer::restir
