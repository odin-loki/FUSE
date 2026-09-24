// WP-6.1 DDGI on Vulkan: CPU references and host helpers (see include/fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp).
#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::gi_gpu {

void sdfSceneFromBoxes(const DdgiCpuScene& scene, std::vector<compute::SdfObject>& objects, std::vector<DdgiSurface>& surfaces) {
    objects.clear();
    surfaces.clear();
    objects.reserve(scene.boxes.size());
    surfaces.reserve(scene.boxes.size());
    for (usize i = 0; i < scene.boxes.size(); ++i) {
        const DdgiCpuBox& box = scene.boxes[i];
        compute::SdfObject o{};
        o.position = (box.min + box.max) * 0.5f;
        o.params = (box.max - box.min) * 0.5f;
        o.type = static_cast<u32>(compute::SdfPrimitiveType::Box);
        o.alpha = 0.f; // hard union: the boxes' exact distance
        o.material_id = static_cast<u32>(i);
        o.rounding = 0.f;
        objects.push_back(o);
        DdgiSurface s{};
        s.albedo[0] = box.surface.albedo.x;
        s.albedo[1] = box.surface.albedo.y;
        s.albedo[2] = box.surface.albedo.z;
        s.emissive[0] = box.surface.emissive.x;
        s.emissive[1] = box.surface.emissive.y;
        s.emissive[2] = box.surface.emissive.z;
        surfaces.push_back(s);
    }
}

DdgiFrameConstants makeFrameConstants(const DDGIDesc& volume, const DdgiCpuConfig& config, const DdgiGpuTuning& tuning,
                                      const DdgiFrameDesc& frame, u32 scheduled) {
    DdgiFrameConstants c{};
    DdgiVolumeView& v = c.volume;
    v.origin[0] = volume.grid_origin.x;
    v.origin[1] = volume.grid_origin.y;
    v.origin[2] = volume.grid_origin.z;
    v.probeCount = ddgi_util::probeCount(volume);
    v.spacing[0] = volume.probe_spacing.x;
    v.spacing[1] = volume.probe_spacing.y;
    v.spacing[2] = volume.probe_spacing.z;
    v.irradianceRes = volume.irradiance_res;
    v.dims[0] = volume.grid_dims.x;
    v.dims[1] = volume.grid_dims.y;
    v.dims[2] = volume.grid_dims.z;
    v.depthRes = volume.depth_res;
    v.normalBias = config.normal_bias;
    v.weightCrushThreshold = config.weight_crush_threshold;
    v.intensity = tuning.intensity;

    const DdgiRayRotation rotation = ddgi_cpu::updateRotation(config.rotation_seed, frame.frameIndex);
    const math::Vec3 rows[3] = {rotation.row0, rotation.row1, rotation.row2};
    for (u32 r = 0; r < 3u; ++r) {
        c.rotation[r][0] = rows[r].x;
        c.rotation[r][1] = rows[r].y;
        c.rotation[r][2] = rows[r].z;
        c.rotation[r][3] = 0.f;
    }
    const math::Vec3 sun = ddgi_kernel::resolve_direction(frame.sunDirection);
    c.sunDirection[0] = sun.x;
    c.sunDirection[1] = sun.y;
    c.sunDirection[2] = sun.z;
    c.maxRayDistance = volume.max_ray_distance;
    c.sunIrradiance[0] = frame.sunIrradiance.x;
    c.sunIrradiance[1] = frame.sunIrradiance.y;
    c.sunIrradiance[2] = frame.sunIrradiance.z;
    c.backfaceDistanceScale = config.backface_distance_scale;
    c.skyRadiance[0] = frame.skyRadiance.x;
    c.skyRadiance[1] = frame.skyRadiance.y;
    c.skyRadiance[2] = frame.skyRadiance.z;
    c.rayEpsilon = tuning.rayEpsilon;
    c.raysPerProbe = volume.rays_per_probe;
    c.scheduled = scheduled;
    c.frameIndex = frame.frameIndex;
    c.flags = (config.multi_bounce ? kDdgiMultiBounce : 0u) | (tuning.frontFaceCounterClockwise ? kDdgiFrontFaceCcw : 0u) |
              (ddgi_kernel::max_component(frame.sunIrradiance) > 0.f ? kDdgiSunEnabled : 0u);
    v.flags = c.flags;
    // The oracle's BlendParams (DdgiCpuVolume::updateProbes).
    c.hysteresis = std::clamp(volume.hysteresis, 0.f, 1.f);
    c.probeChangeHysteresis = std::clamp(config.probe_change_hysteresis, 0.f, 1.f);
    c.probeChangeThreshold = config.probe_change_threshold;
    c.changeThreshold = config.change_threshold;
    c.changeHysteresisDrop = config.change_hysteresis_drop;
    c.changeFloor = config.change_floor;
    c.distancePower = std::max(config.distance_power, 1e-3f);
    c.distanceMinCos = std::pow(1e-6f, 1.f / c.distancePower);
    c.sdfMinDistance = tuning.sdfMinDistance;
    c.sdfMaxSteps = tuning.sdfMaxSteps;
    c.sdfShadowBias = tuning.sdfShadowBias;
    c.traceMask = tuning.traceMask & 0x7Fu;
    c.shadowMask = tuning.shadowMask & 0x7Fu;
    c.initialIrradiance[0] = config.initial_irradiance.x;
    c.initialIrradiance[1] = config.initial_irradiance.y;
    c.initialIrradiance[2] = config.initial_irradiance.z;
    return c;
}

bool runBlendReference(const BlendReferenceInput& in, BlendReferenceState& state) {
    if (in.volume == nullptr || in.config == nullptr || in.schedule == nullptr || in.scheduled == 0u || in.rayDirs == nullptr ||
        in.radiance == nullptr || in.distance == nullptr || in.irradianceTexelDirs == nullptr || in.distanceTexelDirs == nullptr) {
        return false;
    }
    const DDGIDesc& desc = *in.volume;
    const DdgiCpuConfig& config = *in.config;
    const u32 probes = ddgi_util::probeCount(desc);
    const u32 rays = desc.rays_per_probe;
    const u32 ir = desc.irradiance_res;
    const u32 dr = desc.depth_res;
    const usize irrTile = static_cast<usize>(ir + 2u) * (ir + 2u);
    const usize distTile = static_cast<usize>(dr + 2u) * (dr + 2u);
    if (state.irradiance.size() != irrTile * probes || state.distance.size() != distTile * probes ||
        state.updateCounts.size() != probes) {
        return false;
    }
    std::vector<math::Vec4> incoming(static_cast<usize>(in.scheduled) * ir * ir);
    const u32 total = in.scheduled * rays;
    u32 fast = 0u;
    ddgi_kernel::BlendParams blend{};
    blend.probe_indices = {in.schedule, in.scheduled};
    blend.probe_count = probes;
    blend.ray_dirs = {in.rayDirs, rays};
    blend.radiance = {in.radiance, total};
    blend.distance = {in.distance, total};
    blend.irradiance_texel_dirs = {in.irradianceTexelDirs, ir * ir};
    blend.distance_texel_dirs = {in.distanceTexelDirs, dr * dr};
    blend.irradiance = state.irradiance.data();
    blend.distance_moments = state.distance.data();
    blend.update_counts = state.updateCounts.data();
    blend.incoming = {incoming.data(), static_cast<u32>(incoming.size())};
    blend.fast_response_texels = &fast;
    blend.irradiance_res = ir;
    blend.depth_res = dr;
    blend.hysteresis = std::clamp(desc.hysteresis, 0.f, 1.f);
    blend.probe_change_hysteresis = std::clamp(config.probe_change_hysteresis, 0.f, 1.f);
    blend.probe_change_threshold = config.probe_change_threshold;
    blend.change_threshold = config.change_threshold;
    blend.change_hysteresis_drop = config.change_hysteresis_drop;
    blend.change_floor = config.change_floor;
    blend.distance_power = std::max(config.distance_power, 1e-3f);
    blend.distance_min_cos = std::pow(1e-6f, 1.f / blend.distance_power);
    blend.max_distance = desc.max_ray_distance;
    kernel::launch(kernel::Backend::CpuReference, ddgi_kernel::make_blend_launch(in.scheduled), ddgi_kernel::BlendKernel{}, blend);
    state.fastResponseTexels += fast;
    return true;
}

void initialVolumeState(const DDGIDesc& volume, const DdgiCpuConfig& config, BlendReferenceState& state) {
    const u32 probes = ddgi_util::probeCount(volume);
    const usize irrTile = static_cast<usize>(volume.irradiance_res + 2u) * (volume.irradiance_res + 2u);
    const usize distTile = static_cast<usize>(volume.depth_res + 2u) * (volume.depth_res + 2u);
    state.irradiance.assign(irrTile * probes, config.initial_irradiance);
    const f32 d = volume.max_ray_distance;
    state.distance.assign(distTile * probes, math::Vec2{d, d * d});
    state.updateCounts.assign(probes, 0u);
    state.fastResponseTexels = 0u;
}

ddgi_kernel::VolumeView volumeView(const DDGIDesc& volume, const DdgiCpuConfig& config, const BlendReferenceState& state) {
    ddgi_kernel::VolumeView v{};
    v.desc = volume;
    v.probe_count = ddgi_util::probeCount(volume);
    v.irradiance = state.irradiance.data();
    v.distance = state.distance.data();
    v.normal_bias = config.normal_bias;
    v.weight_crush_threshold = config.weight_crush_threshold;
    return v;
}

} // namespace fuse::renderer::gi_gpu
