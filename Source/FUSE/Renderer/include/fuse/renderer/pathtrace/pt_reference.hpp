#pragma once

// WP-7.3 path-tracing mode: the CPU reference path tracer (the ground truth of the GPU integrator's convergence
// gates). Double precision and independent of the kernels where it matters for the estimate:
//
//   geometry    the same GPU scene (CPU mirror: instances, transforms, meshes, materials, the scene index buffer),
//               vertices decoded exactly like the WP-6.0 BLAS input (rtDecodePosition) and placed in double; traced
//               by brute force (instance AABB, then double-precision Moller-Trumbore): allocation-free and
//               independent of both the GPU traversal and the WP-6.0 CPU BVH
//   materials   pt_bsdf.hpp in double (the model the kernels implement in float)
//   lights      its own light selection: probability proportional to a power estimate (luminance x area for area
//               lights), independent of the WP-7.1 light tree the GPU samples; uniform area sampling; MIS against
//               the BSDF with that pmf. Both estimators are unbiased for the same integral, so their means agree
//               within their standard errors (the fuse_rp_pathtrace convergence gates)
//   randomness  splitmix64 per (pixel, sample), unrelated to the GPU's PCG
//
// Path semantics shared with the GPU (pathtrace.hpp): pinhole camera through the view-projection inverse at clip
// z = 0.5, box-filtered pixel jitter, vertex emission (front side of the emitter / material), maxBounces scattering
// events, NEE + BSDF + MIS per PtStrategy, Russian roulette from rrStartBounce with continuation probability
// min(max throughput, 0.95), a constant sky for escaping rays, origin offset n (normalBias + viewBias |p - camera|).
//
// render() returns per pixel the mean and the variance of the mean; renderPixel() (one pixel, allocation-free)
// serves the zero-allocation gate.

#include <fuse/renderer/light_tree/light_tree_types.hpp>
#include <fuse/renderer/pathtrace/pathtrace.hpp>
#include <fuse/renderer/pathtrace/pt_bsdf.hpp>
#include <fuse/renderer/restir/restir_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::gpu_scene {
class GpuScene;
}

namespace fuse::renderer::pathtrace {

using PtV3d = PtVec3<f64>;

/// A surface hit of the reference (world space, double).
struct PtRefHit {
    bool hit = false;
    f64 t = -1.0;
    PtV3d position{};
    PtV3d normal{}; ///< unit geometric normal (winding order)
    u32 instance = kPtInvalid;
    u32 primitive = kPtInvalid;
    PtMaterialT<f64> material{};
    PtV3d emission{};
    u32 emitter = kPtInvalid;
};

struct PtRefLight {
    u32 kind = 0;       ///< light_tree::LtKind
    PtV3d p0{};
    PtV3d e1{};
    PtV3d e2{};
    PtV3d normal{};     ///< unit (area normal / spot axis / directional travel direction)
    f64 area = 0.0;
    bool twoSided = false;
    PtV3d radiance{};   ///< restir::RestirLight radiance
    f64 cosInner = 1.0;
    f64 cosOuter = 0.0;
};

class PtCpuScene {
public:
    /// Geometry and materials from the scene's CPU mirror (works in CPU-only mode). `vpos[m]`: the WP-1.2 VPOS
    /// words of mesh m (MeshletMesh::positions), null to skip a mesh. False when a live instance's mesh is missing.
    bool build(const gpu_scene::GpuScene& scene, const u16* const* vpos, u32 meshCount);
    /// Lights: the light-tree emitters (geometry, kind) with their RGB table rows, and the emitter map words the
    /// GPU uses (which traced triangle is which emitter). Selection weights: luminance x area / intensity.
    void setLights(const light_tree::LightTreeEmitter* emitters, const restir::RestirLight* table, u32 count, const u32* emitterMap,
                   u32 mapWords, u32 mapSlots);

    /// Closest hit along o + t d, t in (tMin, tMax), instances with (mask & cullMask) != 0.
    PtRefHit trace(const PtV3d& o, const PtV3d& d, f64 tMin, f64 tMax, u32 cullMask) const;
    bool occluded(const PtV3d& o, const PtV3d& d, f64 tMax, u32 cullMask) const;

    u32 lightCount() const { return static_cast<u32>(m_lights.size()); }
    const PtRefLight& light(u32 i) const { return m_lights[i]; }
    /// Selection probability of light i (sums to 1).
    f64 lightPmf(u32 i) const { return m_pmf[i]; }
    /// Light chosen by u in [0, 1) (inverse CDF, binary search).
    u32 pickLight(f64 u) const;

private:
    struct Instance {
        u32 slot = 0;
        u32 mask = 0;          ///< rt::rtInstanceMask bits
        u32 firstTriangle = 0; ///< into m_triangles
        u32 triangleCount = 0;
        PtV3d lo{};            ///< world AABB
        PtV3d hi{};
        PtMaterialT<f64> material{};
        PtV3d emission{};
    };
    struct Triangle {
        PtV3d p0{};
        PtV3d e1{};
        PtV3d e2{};
        PtV3d normal{}; ///< unit e1 x e2
        u32 primitive = 0;
    };
    bool hitTriangles(const Instance& inst, const PtV3d& o, const PtV3d& d, f64 tMin, f64& tBest, u32& best, f64& bu, f64& bv) const;

    std::vector<Instance> m_instances;
    std::vector<Triangle> m_triangles;
    std::vector<PtRefLight> m_lights;
    std::vector<f64> m_pmf;
    std::vector<f64> m_cdf;
    std::vector<u32> m_map;
    u32 m_mapSlots = 0;
};

struct PtReferenceImage {
    u32 width = 0;
    u32 height = 0;
    u32 samples = 0;
    std::vector<f64> mean;     ///< 3 per pixel
    std::vector<f64> variance; ///< 3 per pixel: variance of the mean (sample variance / n)
};

/// Per-pixel accumulator of renderPixel().
struct PtPixelStats {
    f64 sum[3] = {0.0, 0.0, 0.0};
    f64 sumSq[3] = {0.0, 0.0, 0.0};
    u32 samples = 0;
};

class PtReference {
public:
    /// False for a zero extent or a singular camera.
    bool setup(const PtSettings& settings, const PtCamera& camera, u32 width, u32 height);
    /// `samples` paths of pixel (x, y) with sample indices [firstSample, firstSample + samples). No allocation.
    void renderPixel(const PtCpuScene& scene, u32 x, u32 y, u32 firstSample, u32 samples, u64 seed, PtPixelStats& stats) const;
    /// Every pixel, `threads` worker threads (0 or 1: the calling thread). The result does not depend on `threads`.
    bool render(const PtCpuScene& scene, u32 samples, u64 seed, u32 threads, PtReferenceImage& out) const;
    /// One path (exposed for the estimator tests). `rngState` is the splitmix64 state.
    PtV3d path(const PtCpuScene& scene, u32 x, u32 y, u64& rngState) const;

    const PtSettings& settings() const { return m_settings; }

private:
    PtSettings m_settings{};
    f64 m_inv[16] = {};
    PtV3d m_camera{};
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer::pathtrace
