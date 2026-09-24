// WP-6.1 DDGI on Vulkan: CPU gates (stub-safe). Lavapipe gates: test_rp_ddgi_gpu.cpp.
//
//   layout     record sizes / offsets; the GLSL and Slang mirrors (ddgi_common.*, ddgi_sample.*, ddgi_trace.*)
//              declare the C++ fields in order; DdgiSdfObject == compute::SdfObject; the lighting constants
//              keep the DDGI address slot
//   constants  makeFrameConstants == the oracle's BlendParams / DdgiCpuVolume::updateProbes settings (clamps,
//              distance_min_cos, rotation == ddgi_cpu::updateRotation, flags)
//   sdf        the T0 reference (sdf_trace_radiance over sdfSceneFromBoxes) against the oracle's analytic box
//              trace (ddgi_kernel::trace_radiance) on the same rays: hit / miss / backface class equal on every
//              non-grazing ray, |dt| <= minDistance x steps bound, radiance equal where the hit is on the same
//              face; the SDF distance == the box distance
//   blend      runBlendReference (the oracle's BlendKernel on explicit ray results) == DdgiCpuVolume::updateProbes
//              bit for bit over 6 updates (the harness the Vulkan gates use to compare the GPU atlases)
//   api        DdgiWorkLayout sections; DdgiGpu without a device fails init with a reason; setSdfScene bounds
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_types.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::gi_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec2;
using fuse::math::Vec3;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Body of `struct <name> {...}` (first match), or empty.
std::string structBody(const std::string& text, const std::string& name) {
    const usize at = text.find("struct " + name + " ");
    if (at == std::string::npos) {
        return {};
    }
    const usize open = text.find('{', at);
    const usize close = text.find("};", open);
    return open == std::string::npos || close == std::string::npos ? std::string{} : text.substr(open, close - open);
}

/// Every name appears as a declared field (`<ws>name;` or `<ws>name[`) after the previous one.
bool fieldsInOrder(const std::string& body, const std::vector<std::string>& fields) {
    usize cursor = 0;
    for (const std::string& f : fields) {
        bool found = false;
        while (true) {
            const usize at = body.find(f, cursor);
            if (at == std::string::npos) {
                break;
            }
            const char before = at > 0 ? body[at - 1] : ' ';
            const char after = at + f.size() < body.size() ? body[at + f.size()] : ' ';
            if ((before == ' ' || before == '\t') && (after == ';' || after == '[')) {
                cursor = at + f.size();
                found = true;
                break;
            }
            cursor = at + 1;
        }
        if (!found) {
            std::fprintf(stderr, "  field %s missing / out of order\n", f.c_str());
            return false;
        }
    }
    return true;
}

// --- shared scene -------------------------------------------------------------------------------------------
/// Room (floor, 3 walls, open side), a thin wall, blockers, one emissive panel; sun + sky.
DdgiCpuScene roomScene() {
    DdgiCpuScene s;
    DdgiCpuSurface grey{};
    grey.albedo = {0.7f, 0.7f, 0.7f};
    DdgiCpuSurface red{};
    red.albedo = {0.8f, 0.15f, 0.1f};
    DdgiCpuSurface green{};
    green.albedo = {0.1f, 0.7f, 0.2f};
    DdgiCpuSurface lamp{};
    lamp.albedo = {0.2f, 0.2f, 0.2f};
    lamp.emissive = {3.f, 2.5f, 2.f};
    s.addBox({-4.f, -0.5f, -4.f}, {4.f, 0.f, 4.f}, grey);  // floor
    s.addBox({-4.f, 0.f, -4.f}, {-3.8f, 3.f, 4.f}, red);   // left wall
    s.addBox({3.8f, 0.f, -4.f}, {4.f, 3.f, 4.f}, green);   // right wall
    s.addBox({-4.f, 0.f, -4.f}, {4.f, 3.f, -3.8f}, grey);  // back wall
    s.addBox({-1.f, 0.f, -0.5f}, {0.5f, 1.2f, 0.7f}, grey); // blocker
    s.addBox({1.4f, 0.f, 1.f}, {2.2f, 2.1f, 1.6f}, red);    // pillar
    s.addBox({-2.5f, 2.f, 1.5f}, {-1.5f, 2.1f, 2.5f}, lamp); // emissive panel
    s.sun_direction = Vec3{0.3f, 0.8f, 0.5f}.normalized();
    s.sun_irradiance = {3.f, 2.8f, 2.5f};
    s.sky_radiance = {0.25f, 0.3f, 0.4f};
    return s;
}

DDGIDesc roomVolume() {
    DDGIDesc d{};
    d.grid_origin = {-3.f, 0.5f, -3.f};
    d.probe_spacing = {1.5f, 1.f, 1.5f};
    d.grid_dims = {5u, 3u, 5u};
    d.rays_per_probe = 64;
    d.probes_per_frame = 75;
    d.irradiance_res = 8;
    d.depth_res = 16;
    d.hysteresis = 0.9f;
    d.max_ray_distance = 12.f;
    return d;
}

// --- layout -----------------------------------------------------------------------------------------------
void testLayout() {
    expect(sizeof(DdgiVolumeView) == 80u && sizeof(DdgiFrameConstants) == 352u && sizeof(DdgiPush) == 32u &&
               sizeof(DdgiSdfObject) == 40u && sizeof(DdgiSurface) == 32u && sizeof(DdgiProbePoint) == 32u,
           "record sizes");
    expect(offsetof(DdgiFrameConstants, volume) == 0u, "the volume view heads the frame constants (one address)");
    expect(sizeof(fuse::compute::SdfObject) == sizeof(DdgiSdfObject) &&
               offsetof(fuse::compute::SdfObject, params) == offsetof(DdgiSdfObject, params) &&
               offsetof(fuse::compute::SdfObject, type) == offsetof(DdgiSdfObject, type) &&
               offsetof(fuse::compute::SdfObject, alpha) == offsetof(DdgiSdfObject, alpha) &&
               offsetof(fuse::compute::SdfObject, material_id) == offsetof(DdgiSdfObject, materialId) &&
               offsetof(fuse::compute::SdfObject, rounding) == offsetof(DdgiSdfObject, rounding),
           "DdgiSdfObject == compute::SdfObject");
    expect(sizeof(Vec3) == 12u && sizeof(Vec2) == 8u, "atlas texels are the oracle's Vec3 / Vec2 (3 / 2 f32)");
    expect(sizeof(lighting_gpu::LightingFrameConstants) == 768u &&
               offsetof(lighting_gpu::LightingFrameConstants, reserved1) == 760u,
           "the lighting constants carry the DDGI address in reserved1 (offset 760)");

    const std::vector<std::string> volume = {"origin", "probeCount", "spacing", "irradianceRes", "dims", "depthRes",
                                             "normalBias", "weightCrushThreshold", "intensity", "flags", "irradiance",
                                             "distance"};
    const std::vector<std::string> frame = {
        "volume", "rotation", "sunDirection", "maxRayDistance", "sunIrradiance", "backfaceDistanceScale", "skyRadiance",
        "rayEpsilon", "raysPerProbe", "scheduled", "frameIndex", "flags", "hysteresis", "probeChangeHysteresis",
        "probeChangeThreshold", "changeThreshold", "changeHysteresisDrop", "changeFloor", "distancePower", "distanceMinCos",
        "sdfMinDistance", "sdfMaxSteps", "sdfCount", "sdfShadowBias", "traceMask", "shadowMask", "initialIrradiance",
        "sdfSurfaceCount", "schedule", "rayDirs", "rays", "updateCounts", "irradianceTexelDirs", "distanceTexelDirs", "tlas",
        "scene", "sdfObjects", "sdfSurfaces", "slotStats"};
    const std::vector<std::string> sdf = {"position", "params", "type", "alpha", "materialId", "rounding"};
    const std::vector<std::string> push = {"frame", "aux", "out_", "count", "pad"};
    const std::string dir = FUSE_DDGI_SHADER_DIR;
    const std::string sampleGlsl = readFile(dir + "/ddgi_sample.glsl");
    const std::string sampleSlang = readFile(dir + "/ddgi_sample.slang");
    const std::string commonGlsl = readFile(dir + "/ddgi_common.glsl");
    const std::string commonSlang = readFile(dir + "/ddgi_common.slang");
    expect(!sampleGlsl.empty() && !sampleSlang.empty() && !commonGlsl.empty() && !commonSlang.empty(), "shader sources readable");
    expect(fieldsInOrder(structBody(sampleGlsl, "FuseDdgiVolume"), volume) && fieldsInOrder(structBody(sampleSlang, "DdgiVolume"), volume),
           "DdgiVolumeView mirrors");
    expect(fieldsInOrder(structBody(commonGlsl, "FuseDdgiFrame"), frame) && fieldsInOrder(structBody(commonSlang, "DdgiFrame"), frame),
           "DdgiFrameConstants mirrors");
    expect(fieldsInOrder(structBody(commonGlsl, "FuseDdgiSdfObject"), sdf) && fieldsInOrder(structBody(commonSlang, "DdgiSdfObject"), sdf),
           "DdgiSdfObject mirrors");
    expect(fieldsInOrder(commonGlsl.substr(commonGlsl.find("uniform FuseDdgiPush")), push) &&
               fieldsInOrder(structBody(commonSlang, "DdgiPush"), push),
           "DdgiPush mirrors");
    std::printf("layout: records pinned, GLSL / Slang mirrors in C++ field order\n");
}

// --- constants --------------------------------------------------------------------------------------------
void testConstants() {
    DDGIDesc d = roomVolume();
    d.hysteresis = 1.7f; // clamped like the oracle
    DdgiCpuConfig c{};
    c.probe_change_hysteresis = -0.5f;
    c.distance_power = 0.f;
    c.multi_bounce = false;
    c.initial_irradiance = {0.1f, 0.2f, 0.3f};
    DdgiGpuTuning t{};
    t.traceMask = 0xFFu; // kRtMaskDead must never reach a cull mask
    DdgiFrameDesc f{};
    f.frameIndex = 17;
    f.sunDirection = {0.f, 2.f, 0.f};
    f.sunIrradiance = {1.f, 1.f, 1.f};
    const DdgiFrameConstants k = makeFrameConstants(d, c, t, f, 12u);
    expect(k.hysteresis == 1.f && k.probeChangeHysteresis == 0.f, "hysteresis clamps (BlendParams)");
    expect(k.distancePower == 1e-3f && k.distanceMinCos == std::pow(1e-6f, 1.f / 1e-3f), "distance power floor / min cos");
    const DdgiRayRotation r = ddgi_cpu::updateRotation(c.rotation_seed, 17u);
    expect(k.rotation[0][0] == r.row0.x && k.rotation[1][2] == r.row1.z && k.rotation[2][1] == r.row2.y, "rotation rows");
    expect(k.sunDirection[1] == 1.f && k.sunDirection[0] == 0.f, "sun direction normalised");
    expect((k.flags & kDdgiMultiBounce) == 0u && (k.flags & kDdgiSunEnabled) != 0u && (k.flags & kDdgiFrontFaceCcw) != 0u, "flags");
    expect(k.traceMask == 0x7Fu, "trace mask never carries kRtMaskDead");
    expect(k.volume.probeCount == 75u && k.volume.dims[1] == 3u && k.scheduled == 12u && k.raysPerProbe == 64u, "grid");
    expect(k.initialIrradiance[2] == 0.3f && k.maxRayDistance == 12.f, "reset values");
    std::printf("constants: oracle settings mapped (clamps, min cos %.6g, rotation, flags)\n", static_cast<f64>(k.distanceMinCos));
}

// --- sdf ----------------------------------------------------------------------------------------------------
void testSdf() {
    const DdgiCpuScene scene = roomScene();
    const DDGIDesc desc = roomVolume();
    std::vector<fuse::compute::SdfObject> objects;
    std::vector<DdgiSurface> surfaces;
    sdfSceneFromBoxes(scene, objects, surfaces);
    // A lit previous volume (multi-bounce reads it): the oracle after two updates.
    DdgiCpuVolume oracle;
    expect(oracle.init(desc), "oracle init");
    std::vector<u32> all(ddgi_util::probeCount(desc));
    for (u32 i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    oracle.updateProbes(scene, all.data(), static_cast<u32>(all.size()), 0u);
    oracle.updateProbes(scene, all.data(), static_cast<u32>(all.size()), 1u);

    SdfTraceScene s{};
    s.objects = objects.data();
    s.objectCount = static_cast<u32>(objects.size());
    s.surfaces = surfaces.data();
    s.surfaceCount = static_cast<u32>(surfaces.size());
    s.sunDirection = scene.sun_direction;
    s.sunIrradiance = scene.sun_irradiance;
    s.skyRadiance = scene.sky_radiance;
    s.maxDistance = desc.max_ray_distance;
    ddgi_kernel::VolumeView vv{};
    vv.desc = desc;
    vv.probe_count = oracle.probeCount();
    vv.irradiance = oracle.irradianceAtlas().data();
    vv.distance = oracle.distanceAtlas().data();
    vv.normal_bias = oracle.config().normal_bias;
    vv.weight_crush_threshold = oracle.config().weight_crush_threshold;
    s.volume = vv;

    ddgi_kernel::TraceParams tp{};
    tp.scene.boxes = scene.boxes.data();
    tp.scene.box_count = static_cast<u32>(scene.boxes.size());
    tp.scene.sun_direction = scene.sun_direction;
    tp.scene.sun_irradiance = scene.sun_irradiance;
    tp.scene.sky_radiance = scene.sky_radiance;
    tp.volume = vv;
    tp.backface_distance_scale = oracle.config().backface_distance_scale;
    tp.multi_bounce = true;

    // SDF distance == box distance (outside points).
    std::mt19937 rng(61);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    f64 maxDistErr = 0.0;
    const fuse::compute::RayMarchParams rm = sdf_params(s);
    for (u32 i = 0; i < 2000u; ++i) {
        const Vec3 p{u(rng) * 4.5f, 0.5f + u(rng) * 3.f, u(rng) * 4.5f};
        f32 best = 1e30f;
        for (const DdgiCpuBox& b : scene.boxes) {
            const Vec3 c = (b.min + b.max) * 0.5f;
            const Vec3 h = (b.max - b.min) * 0.5f;
            best = std::min(best, fuse::math::SDF::box(p - c, h));
        }
        maxDistErr = std::max(maxDistErr, static_cast<f64>(std::fabs(fuse::compute::ray_march_kernel::scene_eval(rm, p, nullptr) - best)));
    }
    expect(maxDistErr == 0.0, "global SDF == hard union of the box distances");

    u32 rays = 0, same = 0, grazing = 0, classBad = 0, tBad = 0, radBad = 0, radCompared = 0;
    f64 maxDt = 0.0, maxRad = 0.0;
    const u32 n = desc.rays_per_probe;
    for (u32 probe = 0; probe < all.size(); ++probe) {
        const Vec3 origin = ddgi_kernel::probe_world_position(desc, probe);
        for (u32 r = 0; r < n; ++r) {
            const Vec3 dir = ddgi_cpu::sphericalFibonacci(r, n);
            f32 dOracle = 0.f, dSdf = 0.f;
            const Vec3 lo = ddgi_kernel::trace_radiance(tp, origin, dir, dOracle);
            const Vec3 ls = sdf_trace_radiance(s, origin, dir, dSdf);
            DdgiCpuHit hit{};
            const bool hitOracle = scene.intersect(origin, dir, 0.f, desc.max_ray_distance, hit);
            ++rays;
            // Grazing: the ray passes within 2 cm of a box it does not hit (an edge / near miss the sphere
            // trace resolves only to minDistance), so the two traces may legitimately disagree.
            bool graze = false;
            {
                f32 t = 0.f;
                const f32 end = hitOracle ? hit.t : desc.max_ray_distance;
                for (u32 k = 0; k < 4000u && t < end; ++k) {
                    const Vec3 p = origin + dir * t;
                    const f32 dd = fuse::compute::ray_march_kernel::scene_eval(rm, p, nullptr);
                    if (dd < 0.02f && (!hitOracle || sdf_material(s, p) != hit.box_index)) {
                        graze = true;
                        break;
                    }
                    t += std::max(dd, 0.005f);
                }
            }
            if (graze) {
                ++grazing;
                continue;
            }
            const bool oracleBack = hitOracle && hit.backface;
            const bool sdfMiss = dSdf == desc.max_ray_distance && ls.x == scene.sky_radiance.x;
            const bool oracleMiss = !hitOracle;
            if (oracleMiss != sdfMiss || (oracleBack && (ls.x != 0.f || ls.y != 0.f || ls.z != 0.f))) {
                ++classBad;
                continue;
            }
            ++same;
            if (!oracleMiss && !oracleBack) {
                const f64 dt = std::fabs(static_cast<f64>(dSdf) - dOracle);
                maxDt = std::max(maxDt, dt);
                tBad += dt > 2e-3 ? 1u : 0u; // sphere tracing stops within minDistance (+ f32 steps)
                const f64 ref = std::max({static_cast<f64>(lo.x), static_cast<f64>(lo.y), static_cast<f64>(lo.z), 1e-3});
                const f64 err = std::max({std::fabs(static_cast<f64>(ls.x) - lo.x), std::fabs(static_cast<f64>(ls.y) - lo.y),
                                          std::fabs(static_cast<f64>(ls.z) - lo.z)}) / ref;
                ++radCompared;
                maxRad = std::max(maxRad, err);
                radBad += err > 2e-2 ? 1u : 0u;
            }
        }
    }
    std::printf("sdf: %u rays, %u grazing skipped, %u same class, %u class mismatches, max |dt| %.3g (%u > 2e-3), "
                "radiance compared %u, max rel %.3g (%u > 2%%)\n",
                rays, grazing, same, classBad, maxDt, tBad, radCompared, maxRad, radBad);
    expect(classBad == 0u, "T0 reference: hit / miss / backface class == the analytic oracle on every non-grazing ray");
    expect(tBad == 0u, "T0 reference: hit distance within 2e-3 of the analytic box hit");
    expect(radBad * 200u <= radCompared, "T0 reference: radiance within 2% of the oracle on >= 99.5% of the hits");
    expect(grazing * 5u <= rays, "grazing rays are a minority");
}

// --- blend --------------------------------------------------------------------------------------------------
void testBlend() {
    const DdgiCpuScene scene = roomScene();
    const DDGIDesc desc = roomVolume();
    DdgiCpuConfig config{};
    DdgiCpuVolume oracle;
    expect(oracle.init(desc, config), "oracle init");
    BlendReferenceState state;
    initialVolumeState(desc, config, state);
    std::vector<Vec3> irrDirs(desc.irradiance_res * desc.irradiance_res), distDirs(desc.depth_res * desc.depth_res);
    for (u32 y = 0; y < desc.irradiance_res; ++y) {
        for (u32 x = 0; x < desc.irradiance_res; ++x) {
            irrDirs[y * desc.irradiance_res + x] = ddgi_cpu::texelDirection(x, y, desc.irradiance_res);
        }
    }
    for (u32 y = 0; y < desc.depth_res; ++y) {
        for (u32 x = 0; x < desc.depth_res; ++x) {
            distDirs[y * desc.depth_res + x] = ddgi_cpu::texelDirection(x, y, desc.depth_res);
        }
    }
    const u32 rays = desc.rays_per_probe;
    u32 mismatches = 0;
    u32 fastTotal = 0;
    for (u32 frame = 0; frame < 6u; ++frame) {
        // Schedules of different sizes, rolling like the GPU's.
        std::vector<u32> schedule;
        const u32 count = frame % 2u == 0u ? 75u : 31u;
        for (u32 i = 0; i < count; ++i) {
            schedule.push_back((frame * 13u + i) % 75u);
        }
        // Ray results: the oracle's trace on the pre-update volume, same rotation.
        const DdgiRayRotation rot = ddgi_cpu::updateRotation(config.rotation_seed, frame);
        std::vector<Vec3> dirs(rays);
        for (u32 r = 0; r < rays; ++r) {
            dirs[r] = rot.apply(ddgi_cpu::sphericalFibonacci(r, rays)).normalized();
        }
        ddgi_kernel::TraceParams tp{};
        tp.scene.boxes = scene.boxes.data();
        tp.scene.box_count = static_cast<u32>(scene.boxes.size());
        tp.scene.sun_direction = scene.sun_direction;
        tp.scene.sun_irradiance = scene.sun_irradiance;
        tp.scene.sky_radiance = scene.sky_radiance;
        tp.volume = volumeView(desc, config, state);
        tp.backface_distance_scale = config.backface_distance_scale;
        tp.multi_bounce = config.multi_bounce;
        std::vector<Vec3> radiance(schedule.size() * rays);
        std::vector<f32> distance(schedule.size() * rays);
        for (u32 slot = 0; slot < schedule.size(); ++slot) {
            const Vec3 origin = ddgi_kernel::probe_world_position(desc, schedule[slot]);
            for (u32 r = 0; r < rays; ++r) {
                radiance[slot * rays + r] = ddgi_kernel::trace_radiance(tp, origin, dirs[r], distance[slot * rays + r]);
            }
        }
        BlendReferenceInput in{};
        in.volume = &desc;
        in.config = &config;
        in.schedule = schedule.data();
        in.scheduled = static_cast<u32>(schedule.size());
        in.rayDirs = dirs.data();
        in.radiance = radiance.data();
        in.distance = distance.data();
        in.irradianceTexelDirs = irrDirs.data();
        in.distanceTexelDirs = distDirs.data();
        const u32 before = state.fastResponseTexels;
        expect(runBlendReference(in, state), "runBlendReference");
        const DdgiCpuUpdateStats st = oracle.updateProbes(scene, schedule.data(), static_cast<u32>(schedule.size()), frame);
        fastTotal += st.fast_response_texels;
        expect(state.fastResponseTexels - before == st.fast_response_texels, "fast-response texel count == the oracle's");
        mismatches += std::memcmp(state.irradiance.data(), oracle.irradianceAtlas().data(), state.irradiance.size() * sizeof(Vec3)) != 0;
        mismatches += std::memcmp(state.distance.data(), oracle.distanceAtlas().data(), state.distance.size() * sizeof(Vec2)) != 0;
        for (u32 p = 0; p < oracle.probeCount(); ++p) {
            mismatches += state.updateCounts[p] != oracle.probeUpdateCount(p) ? 1u : 0u;
        }
    }
    std::printf("blend: 6 updates, runBlendReference == DdgiCpuVolume::updateProbes bit for bit (%u mismatches, %u fast texels)\n",
                mismatches, fastTotal);
    expect(mismatches == 0u, "runBlendReference == the oracle's update, bit for bit");
}

// --- api ----------------------------------------------------------------------------------------------------
void testApi() {
    const DDGIDesc desc = roomVolume();
    const DdgiWorkLayout l = DdgiWorkLayout::compute(desc, 40u);
    const u64 probes = 75u;
    expect(l.irradiance == 0u && l.irradianceBytes == probes * 100u * 12u && l.distanceBytes == probes * 324u * 8u,
           "atlas sections sized like the oracle's atlases");
    expect(l.distance >= l.irradianceBytes && l.updateCounts >= l.distance + l.distanceBytes && l.rayDirs >= l.updateCounts + probes * 4u &&
               l.rays >= l.rayDirs + 64u * 16u && l.slotStats >= l.rays + 40u * 64u * 16u && l.bytes >= l.slotStats + 40u * 4u,
           "sections disjoint");
    expect(l.distance % 256u == 0u && l.rays % 256u == 0u && l.slotStats % 256u == 0u, "sections 256-aligned");
    DdgiGpu gpu;
    DdgiGpuDesc d{};
    d.volume = desc;
    expect(!gpu.init(d) && !gpu.ready() && std::strlen(gpu.reason()) > 0u, "init without a device fails with a reason");
    expect(!gpu.beginFrame(1u, DdgiFrameDesc{}), "beginFrame before init fails");
    const DdgiGpuCapabilities caps = queryDdgiGpuCapabilities(nullptr);
    expect(!caps.compute && !caps.rayQuery, "no device: no capabilities");
    std::printf("api: layout sections, no-device init refused (%s)\n", gpu.reason());
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "layout" || suite == "all") {
        testLayout();
    }
    if (suite == "constants" || suite == "all") {
        testConstants();
    }
    if (suite == "sdf" || suite == "all") {
        testSdf();
    }
    if (suite == "blend" || suite == "all") {
        testBlend();
    }
    if (suite == "api" || suite == "all") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
