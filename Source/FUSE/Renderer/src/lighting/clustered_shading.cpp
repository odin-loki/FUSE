// CPU entry points of the clustered deferred shade. The point-light / reconstruction math lives once
// in fuse/renderer/lighting/clustered_kernel.hpp (FUSE_HOST_DEVICE); the full-frame shade is the
// `deferred_shading` kernel launched through kernel::launch.

#include <fuse/renderer/lighting/clustered_shading.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

#if defined(FUSE_HAS_CUDA)
/// kernels/clustered_lighting.cu: stages the G-buffer, lights and light grid and runs the shade body.
bool launchDeferredShadingCuda(const clustered_kernel::ShadeParams& params, void* stream);
#endif

f32 clustered_shading::pointLightFalloff(f32 distance, f32 radius) {
    return clustered_kernel::point_light_falloff(distance, radius);
}

fuse::math::Vec3 clustered_shading::pointLightContribution(const PointLightInput& light,
                                                           const DeferredSurfaceSample& surface) {
    return clustered_kernel::point_light_contribution(light, surface.worldPos, surface.normal, surface.albedo);
}

fuse::math::Vec3 clustered_shading::shadeAllLights(const std::vector<PointLightInput>& lights,
                                                   const DeferredSurfaceSample& surface,
                                                   u64* outEvaluations) {
    u32 evaluations = 0u;
    const fuse::math::Vec3 radiance = clustered_kernel::shade_all_lights(
        {lights.data(), static_cast<u32>(lights.size())}, surface.worldPos, surface.normal, surface.albedo, evaluations);
    if (outEvaluations != nullptr) {
        *outEvaluations += evaluations;
    }
    return radiance;
}

fuse::math::Vec3 clustered_shading::shadeClusterLights(const ClusterGridSoA& grid,
                                                       u32 clusterIdx,
                                                       const std::vector<PointLightInput>& lights,
                                                       const DeferredSurfaceSample& surface,
                                                       u64* outEvaluations) {
    u32 evaluations = 0u;
    const fuse::math::Vec3 radiance = clustered_kernel::shade_cluster_lights(
        {grid.grid.data(), static_cast<u32>(grid.grid.size())},
        {grid.lightList.data(), static_cast<u32>(grid.lightList.size())}, clusterIdx,
        {lights.data(), static_cast<u32>(lights.size())}, surface.worldPos, surface.normal, surface.albedo,
        evaluations);
    if (outEvaluations != nullptr) {
        *outEvaluations += evaluations;
    }
    return radiance;
}

bool clustered_shading::reconstructWorldPosition(const ClusterCameraDesc& camera,
                                                 f32 screenX,
                                                 f32 screenY,
                                                 f32 deviceDepth,
                                                 fuse::math::Vec3& outWorld,
                                                 f32& outViewDepth) {
    const f32 viewDepth = cluster_math::viewDepthFromDeviceDepth(deviceDepth, camera);
    outViewDepth = viewDepth;
    if (!(viewDepth > 0.f) || !std::isfinite(viewDepth)) {
        return false;
    }
    outWorld = cluster_math::viewToWorld(camera,
                                         cluster_math::viewPositionFromScreen(screenX, screenY, viewDepth, camera));
    return true;
}

DeferredShadeStats clustered_shading::shadeDeferredFrame(const DeferredGBufferView& gbuffer,
                                                         const ClusterDesc& desc,
                                                         const ClusterCameraDesc& camera,
                                                         const ClusterGridSoA& grid,
                                                         const std::vector<PointLightInput>& lights,
                                                         bool useClusters,
                                                         std::vector<fuse::math::Vec3>& outRadiance,
                                                         kernel::Backend backend) {
    DeferredShadeStats stats{};
    const usize pixelCount = static_cast<usize>(gbuffer.width) * gbuffer.height;
    outRadiance.assign(pixelCount, fuse::math::Vec3{});
    if (pixelCount == 0u || gbuffer.deviceDepth == nullptr || gbuffer.normals == nullptr ||
        gbuffer.albedo == nullptr) {
        return stats;
    }

    u32 shaded = 0u;
    u32 skipped = 0u;
    u32 evaluations[2] = {0u, 0u};
    clustered_kernel::ShadeParams params{};
    params.width = gbuffer.width;
    params.height = gbuffer.height;
    params.inv_width = 1.f / static_cast<f32>(gbuffer.width);
    params.inv_height = 1.f / static_cast<f32>(gbuffer.height);
    params.grid = clustered_kernel::make_grid(desc);
    params.camera = clustered_kernel::make_camera(camera);
    params.device_depth = gbuffer.deviceDepth;
    params.normals = gbuffer.normals;
    params.albedo = gbuffer.albedo;
    params.lights = {lights.data(), static_cast<u32>(lights.size())};
    params.cluster_grid = {grid.grid.data(), static_cast<u32>(grid.grid.size())};
    params.light_list = {grid.lightList.data(), static_cast<u32>(grid.lightList.size())};
    params.use_clusters = useClusters;
    params.out_radiance = outRadiance.data();
    params.shaded_pixels = &shaded;
    params.skipped_pixels = &skipped;
    params.evaluations = evaluations;
    bool onDevice = false;
#if defined(FUSE_HAS_CUDA)
    onDevice = (backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
               kernel::backend_available(kernel::Backend::Cuda) && launchDeferredShadingCuda(params, nullptr);
#endif
    if (!onDevice) {
        // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback.
        kernel::launch(backend, clustered_kernel::make_shade_launch(gbuffer.width, gbuffer.height),
                       clustered_kernel::ShadeKernel{}, params);
    }

    stats.shadedPixels = shaded;
    stats.skippedPixels = skipped;
    stats.lightEvaluations = (static_cast<u64>(evaluations[1]) << 32u) | evaluations[0];
    return stats;
}

} // namespace fuse::renderer
