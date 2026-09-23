#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// One reconstructed G-buffer surface sample (world space) for deferred point-light shading.
struct DeferredSurfaceSample {
    fuse::math::Vec3 worldPos{};
    fuse::math::Vec3 normal{0.f, 1.f, 0.f};
    fuse::math::Vec3 albedo{1.f, 1.f, 1.f};
};

/// Read-only CPU view of the G-buffer channels the deferred shade pass consumes.
/// Arrays are row-major, `width * height` entries, row 0 at the top of the screen.
struct DeferredGBufferView {
    u32 width = 0;
    u32 height = 0;
    const f32* deviceDepth = nullptr;            ///< RT4 depth in the camera's device convention.
    const fuse::math::Vec3* normals = nullptr;   ///< World-space unit normals.
    const fuse::math::Vec3* albedo = nullptr;    ///< Linear base colour.
};

struct DeferredShadeStats {
    u32 shadedPixels = 0;     ///< Pixels with valid depth inside the cluster grid's range.
    u32 skippedPixels = 0;    ///< Sky / out-of-range pixels (written as black).
    u64 lightEvaluations = 0; ///< Point-light evaluations performed.
};

/// CPU reference of the clustered deferred shade kernel (B5.4). Point lights only; spot-light
/// indices encoded after the point lights in a cluster list are ignored here.
namespace clustered_shading {

/// Windowed inverse-square falloff: saturate(1 - (d/r)^4)^2 / max(d^2, 1e-4).
/// Exactly 0 for d >= r (and for r <= 0), so a light never contributes outside its range sphere.
f32 pointLightFalloff(f32 distance, f32 radius);

/// Lambert diffuse radiance from one point light (colour * intensity * falloff * N.L * albedo).
fuse::math::Vec3 pointLightContribution(const PointLightInput& light, const DeferredSurfaceSample& surface);

/// Brute-force reference: sum over every point light, in ascending index order.
fuse::math::Vec3 shadeAllLights(const std::vector<PointLightInput>& lights,
                                const DeferredSurfaceSample& surface,
                                u64* outEvaluations = nullptr);

/// Sum over the lights stored for one cluster in the rebuilt light grid (ascending index order).
fuse::math::Vec3 shadeClusterLights(const ClusterGridSoA& grid,
                                    u32 clusterIdx,
                                    const std::vector<PointLightInput>& lights,
                                    const DeferredSurfaceSample& surface,
                                    u64* outEvaluations = nullptr);

/// World position for normalized screen coords and a device depth; false for sky / invalid depth.
bool reconstructWorldPosition(const ClusterCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 deviceDepth,
                              fuse::math::Vec3& outWorld,
                              f32& outViewDepth);

/// Shade every pixel: reconstruct position from depth, then either walk the pixel's cluster list
/// (`useClusters`) or loop all lights. Pixels outside [near, far] or with cleared depth are black.
DeferredShadeStats shadeDeferredFrame(const DeferredGBufferView& gbuffer,
                                      const ClusterDesc& desc,
                                      const ClusterCameraDesc& camera,
                                      const ClusterGridSoA& grid,
                                      const std::vector<PointLightInput>& lights,
                                      bool useClusters,
                                      std::vector<fuse::math::Vec3>& outRadiance);

} // namespace clustered_shading
} // namespace fuse::renderer
