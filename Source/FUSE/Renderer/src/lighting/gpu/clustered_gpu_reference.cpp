// WP-2.1 CPU references: see include/fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp.
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::lighting_gpu {

namespace {
f32 coneDegrees(f32 cosine) {
    return std::acos(std::clamp(cosine, -1.f, 1.f)) * (180.f / kPi);
}
} // namespace

void makeOracleLights(const gpu_scene::GpuLight* lights, u32 count, OracleLights& out) {
    out.points.clear();
    out.spots.clear();
    out.slotOfIndex.clear();
    out.directional.clear();
    out.monotone = true;
    std::vector<u32> spotSlots;
    u32 maxPointSlot = 0;
    bool anyPoint = false;
    for (u32 slot = 0; slot < count; ++slot) {
        const gpu_scene::GpuLight& l = lights[slot];
        const math::Vec3 position{l.position[0], l.position[1], l.position[2]};
        const math::Vec3 color{l.color[0], l.color[1], l.color[2]};
        // WP-2.2 area lights are clustered by their range sphere around the centre, like points.
        if (l.type == static_cast<u32>(gpu_scene::GpuLightType::Point) || l.type == ltc::kLightRect ||
            l.type == ltc::kLightDisk) {
            PointLightInput p{};
            p.position = position;
            p.color = color;
            p.intensity = l.intensity;
            p.radius = l.range;
            out.points.push_back(p);
            out.slotOfIndex.push_back(slot);
            maxPointSlot = slot;
            anyPoint = true;
        } else if (l.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
            SpotLightInput s{};
            s.position = position;
            s.direction = {l.direction[0], l.direction[1], l.direction[2]};
            s.color = color;
            s.intensity = l.intensity;
            s.innerConeDeg = coneDegrees(l.cosInner);
            s.outerConeDeg = coneDegrees(l.cosOuter);
            s.radius = l.range;
            out.spots.push_back(s);
            spotSlots.push_back(slot);
        } else if (l.type == static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
            out.directional.push_back(slot);
        }
    }
    if (anyPoint && !spotSlots.empty() && spotSlots.front() < maxPointSlot) {
        out.monotone = false;
    }
    out.slotOfIndex.insert(out.slotOfIndex.end(), spotSlots.begin(), spotSlots.end());
}

void translateToSlots(const ClusterGridSoA& oracle, const OracleLights& lights, ClusterGridSoA& out) {
    out.aabbs = oracle.aabbs;
    out.grid = oracle.grid;
    out.lightList.resize(oracle.lightList.size());
    for (usize i = 0; i < oracle.lightList.size(); ++i) {
        const u32 index = oracle.lightList[i];
        out.lightList[i] = index < lights.slotOfIndex.size() ? lights.slotOfIndex[index] : ~0u;
    }
}

ClusterCullResult oracleLightGrid(const ClusterDesc& rawDesc,
                                  const ClusterCameraDesc& camera,
                                  const OracleLights& lights,
                                  ClusterGridSoA& outGrid,
                                  kernel::Backend backend) {
    // ClusteredLightCuller::init / updateClusters / cullLights / rebuildLightGrid, step for step.
    const ClusterDesc desc = ClusterDesc::clampCounts(rawDesc);
    outGrid.allocate(desc);
    ClusterCullResult result{};
    if (desc.clusterCount() == 0u) {
        outGrid.clear();
        return result;
    }
    cluster_math::buildClusterAabbs(desc, camera, outGrid.aabbs, backend);
    ClusterCullLists lists{};
    result = cluster_math::cullLightsToClusterLists(desc, camera, outGrid.aabbs, lights.points, lights.spots, lists,
                                                    backend);
    cluster_math::compactClusterLists(lists, desc.clusterCount(), outGrid, backend);
    return result;
}

ShadeParams makeShadeParams(const ShadeReferenceDesc& desc, math::Vec4* out) {
    ShadeParams p{};
    p.width = desc.width;
    p.height = desc.height;
    p.inv_width = desc.width > 0u ? 1.f / static_cast<f32>(desc.width) : 0.f;
    p.inv_height = desc.height > 0u ? 1.f / static_cast<f32>(desc.height) : 0.f;
    p.grid = clustered_kernel::make_grid(ClusterDesc::clampCounts(desc.desc));
    p.camera = clustered_kernel::make_camera(desc.camera);
    p.ambient = desc.ambient;
    p.gbuffer = desc.gbuffer;
    p.lights = {desc.lights, desc.lights != nullptr ? desc.lightCount : 0u};
    if (desc.grid != nullptr) {
        p.cluster_grid = {desc.grid->grid.data(), static_cast<u32>(desc.grid->grid.size())};
        p.light_list = {desc.grid->lightList.data(), static_cast<u32>(desc.grid->lightList.size())};
    }
    if (desc.directional != nullptr) {
        p.directional = {desc.directional->data(), static_cast<u32>(desc.directional->size())};
    }
    p.out = out;
    if (desc.brdfLut != nullptr) {
        p.brdf_lut = {desc.brdfLut, ltc::kLutWords};
    }
    p.shadow = desc.shadow;
    p.shadow_user = desc.shadowUser;
    return p;
}

u32 shadeReferenceFrame(const ShadeReferenceDesc& desc, std::vector<math::Vec4>& out, kernel::Backend backend) {
    const usize pixels = static_cast<usize>(desc.width) * desc.height;
    out.assign(pixels, math::Vec4{});
    if (pixels == 0u || desc.gbuffer.depth == nullptr || desc.gbuffer.normalAo == nullptr ||
        desc.gbuffer.albedo == nullptr || desc.gbuffer.roughMetal == nullptr || desc.gbuffer.emissive == nullptr) {
        return 0u;
    }
    const ShadeParams params = makeShadeParams(desc, out.data());
    kernel::launch(backend, make_shade_launch(desc.width, desc.height), ShadeKernel{}, params);
    u32 shaded = 0;
    for (const math::Vec4& v : out) {
        shaded += v.w > 0.f ? 1u : 0u;
    }
    return shaded;
}

} // namespace fuse::renderer::lighting_gpu
