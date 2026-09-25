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
//   state      the oracle's probe relocation + classification rules (ddgi_kernel::update_probe_state, RTXGI):
//              inside geometry -> through the closest backface + inactive, too close to a front face -> steps
//              away, offsets bounded by max_offset, drift back when clear, no surface in the cell -> inactive,
//              features off -> unchanged; sample_irradiance skips inactive probes
//   leak       the thin-wall leak criterion on the CPU oracle (the Vulkan leak gate's room, both orientations):
//              max dark-side luminance <= 2% of the mean lit side with the features on; the pre-package rules
//              are shown to leak (sun outside)
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

DdgiCpuConfig statesConfig(const DDGIDesc& desc);

// --- layout -----------------------------------------------------------------------------------------------
void testLayout() {
    expect(sizeof(DdgiVolumeView) == 96u && sizeof(DdgiFrameConstants) == 400u && sizeof(DdgiPush) == 32u &&
               sizeof(DdgiSdfObject) == 40u && sizeof(DdgiSurface) == 32u && sizeof(DdgiProbePoint) == 48u,
           "record sizes");
    expect(sizeof(fuse::math::Vec4) == 16u, "probe data texels are the oracle's Vec4 (offset xyz, state w)");
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
               offsetof(lighting_gpu::LightingFrameConstants, ddgiLo) == 760u &&
               offsetof(lighting_gpu::LightingFrameConstants, ddgiHi) == 764u,
           "the lighting constants carry the DDGI address in ddgiLo / ddgiHi (offsets 760 / 764, 768 bytes)");

    const std::vector<std::string> volume = {"origin", "probeCount", "spacing", "irradianceRes", "dims", "depthRes",
                                             "normalBias", "weightCrushThreshold", "intensity", "flags", "irradiance",
                                             "distance", "probeData", "viewBias", "pad"};
    const std::vector<std::string> frame = {
        "volume", "rotation", "sunDirection", "maxRayDistance", "sunIrradiance", "backfaceDistanceScale", "skyRadiance",
        "rayEpsilon", "raysPerProbe", "scheduled", "frameIndex", "flags", "hysteresis", "probeChangeHysteresis",
        "probeChangeThreshold", "changeThreshold", "changeHysteresisDrop", "changeFloor", "distancePower", "distanceMinCos",
        "sdfMinDistance", "sdfMaxSteps", "sdfCount", "sdfShadowBias", "traceMask", "shadowMask", "initialIrradiance",
        "sdfSurfaceCount", "distanceClamp", "probeMinFrontfaceDistance", "probeBackfaceThreshold", "probeMaxOffset",
        "probeRelocationStep", "atmosphereLo", "atmosphereHi", "statePad", "schedule", "rayDirs", "rays", "updateCounts", "irradianceTexelDirs", "distanceTexelDirs", "tlas",
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
    const std::vector<std::string> point = {"position", "normal", "view"};
    expect(fieldsInOrder(structBody(commonGlsl, "FuseDdgiPoint"), point) && fieldsInOrder(structBody(commonSlang, "DdgiPoint"), point),
           "DdgiProbePoint mirrors (vec4 rows)");
    // The lighting shade's frame tail (WP-2.1 / 6.2 / 6.1): ... rtShadowsLo, rtShadowsHi, ddgiLo, ddgiHi.
    const std::string lc = std::string(FUSE_DDGI_SHADER_DIR) + "/../lighting";
    const std::vector<std::string> tail = {"rtShadowsLo", "rtShadowsHi", "ddgiLo", "ddgiHi"};
    expect(fieldsInOrder(structBody(readFile(lc + "/lc_common.glsl"), "FuseLcFrame"), tail) &&
               fieldsInOrder(structBody(readFile(lc + "/lc_common.slang"), "LcFrame"), tail),
           "lc_common.{glsl,slang}: the frame ends with rtShadowsLo / Hi, ddgiLo / Hi");
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
    expect(k.distanceClamp == 12.f && k.volume.viewBias == 0.f && (k.flags & (kDdgiRelocation | kDdgiClassification)) == 0u,
           "defaults: clamp = max ray distance, no view bias, no probe states");
    const DdgiCpuConfig sc = statesConfig(d);
    const DdgiFrameConstants ks = makeFrameConstants(d, sc, t, f, 12u);
    expect(ks.distanceClamp == sc.distance_clamp && ks.volume.viewBias == 0.5f && ks.volume.normalBias == 0.2f &&
               (ks.flags & kDdgiRelocation) != 0u && (ks.flags & kDdgiClassification) != 0u && ks.probeMinFrontfaceDistance == 0.5f &&
               ks.probeBackfaceThreshold == 0.25f && ks.probeMaxOffset == 0.45f && ks.probeRelocationStep == 0.1f,
           "probe-state settings mapped");
    DdgiCpuConfig big = sc;
    big.distance_clamp = 100.f;
    expect(makeFrameConstants(d, big, t, f, 1u).distanceClamp == 12.f, "distance clamp never above the max ray distance");
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
/// runBlendReference (+ runStateReference) fed the oracle's own trace == DdgiCpuVolume::updateProbes, bit for bit.
void blendRun(const DdgiCpuConfig& config, const char* label) {
    const DdgiCpuScene scene = roomScene();
    const DDGIDesc desc = roomVolume();
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
            const Vec3 origin = ddgi_kernel::probe_position(tp.volume, schedule[slot]); // relocated when on
            for (u32 r = 0; r < rays; ++r) {
                bool back = false;
                f32 d = 0.f;
                radiance[slot * rays + r] = ddgi_kernel::trace_radiance(tp, origin, dirs[r], d, &back);
                distance[slot * rays + r] = back ? -d : d; // ddgi.trace's signed distances
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
        expect(runStateReference(in, state), "runStateReference");
        const DdgiCpuUpdateStats st = oracle.updateProbes(scene, schedule.data(), static_cast<u32>(schedule.size()), frame);
        fastTotal += st.fast_response_texels;
        expect(state.fastResponseTexels - before == st.fast_response_texels, "fast-response texel count == the oracle's");
        mismatches += std::memcmp(state.irradiance.data(), oracle.irradianceAtlas().data(), state.irradiance.size() * sizeof(Vec3)) != 0;
        mismatches += std::memcmp(state.distance.data(), oracle.distanceAtlas().data(), state.distance.size() * sizeof(Vec2)) != 0;
        for (u32 p = 0; p < oracle.probeCount(); ++p) {
            mismatches += state.updateCounts[p] != oracle.probeUpdateCount(p) ? 1u : 0u;
        }
        mismatches += std::memcmp(state.probeData.data(), oracle.probeData().data(), state.probeData.size() * sizeof(fuse::math::Vec4)) != 0;
    }
    u32 inactive = 0;
    u32 moved = 0;
    for (u32 p = 0; p < oracle.probeCount(); ++p) {
        inactive += oracle.probeActive(p) ? 0u : 1u;
        const fuse::math::Vec4& d = oracle.probeData()[p];
        moved += d.x != 0.f || d.y != 0.f || d.z != 0.f ? 1u : 0u;
    }
    std::printf("blend (%s): 6 updates, runBlendReference + runStateReference == DdgiCpuVolume::updateProbes bit for bit "
                "(%u mismatches, %u fast texels, %u inactive, %u relocated probes)\n",
                label, mismatches, fastTotal, inactive, moved);
    expect(mismatches == 0u, "runBlendReference == the oracle's update, bit for bit");
}

/// The volume with every WP-6.1 follow-up feature on (the leak gate's settings).
DdgiCpuConfig statesConfig(const DDGIDesc& desc) {
    DdgiCpuConfig c{};
    c.probe_relocation = true;
    c.probe_classification = true;
    c.probe_min_frontface_distance = 0.5f;
    c.probe_relocation_step = 0.1f;
    c.normal_bias = 0.2f;
    c.view_bias = 0.5f;
    c.distance_clamp = 1.5f * desc.probe_spacing.length();
    return c;
}

void testBlend() {
    blendRun(DdgiCpuConfig{}, "default config");
    blendRun(statesConfig(roomVolume()), "relocation + classification + view bias + distance clamp");
}

// --- state ------------------------------------------------------------------------------------------------
/// ddgi_kernel::update_probe_state on synthetic ray sets (RTXGI relocation / classification rules).
void testState() {
    const u32 rays = 64;
    std::vector<Vec3> dirs(rays);
    for (u32 r = 0; r < rays; ++r) {
        dirs[r] = ddgi_cpu::sphericalFibonacci(r, rays);
    }
    ddgi_kernel::ProbeStateParams p{};
    p.spacing = {1.f, 1.f, 1.f};
    p.max_distance = 10.f;
    p.backface_distance_scale = 0.2f;
    p.min_frontface_distance = 0.3f;
    p.backface_threshold = 0.25f;
    p.max_offset = 0.45f;
    p.relocation_step = 0.1f;
    p.relocation = true;
    p.classification = true;
    std::vector<f32> dist(rays);
    const fuse::math::Vec4 origin{0.f, 0.f, 0.f, ddgi_kernel::kProbeActive};

    // Inside a box: 50% backfaces (upper hemisphere, distances 0.2 -> stored -0.04), the rest far misses.
    for (u32 r = 0; r < rays; ++r) {
        dist[r] = dirs[r].z > 0.f ? -(0.2f + 0.01f * static_cast<f32>(r % 3u)) * 0.2f : 10.f;
    }
    fuse::math::Vec4 out = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, origin);
    const Vec3 moved{out.x, out.y, out.z};
    expect(out.w == ddgi_kernel::kProbeInactive, "inside geometry (backface ratio > threshold): inactive");
    expect(moved.z > 0.f && std::fabs(moved.length() - (0.2f + 0.15f)) < 0.02f,
           "inside geometry: moved through the closest backface by its distance + half the minimum front-face distance");

    // Near a front face (0.1 below along -z) with open space above: moves up by the step, stays active.
    for (u32 r = 0; r < rays; ++r) {
        dist[r] = dirs[r].z < -0.9f ? 0.1f / -dirs[r].z : (dirs[r].z > 0.9f ? 3.f : 0.9f);
    }
    out = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, origin);
    expect(out.w == ddgi_kernel::kProbeActive && out.z > 0.05f && out.z <= 0.1f + 1e-6f, "too close to a front face: steps away (<= step)");
    // Repeated updates never leave the max offset.
    fuse::math::Vec4 cur = origin;
    for (u32 k = 0; k < 40u; ++k) {
        cur = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, cur);
    }
    expect(Vec3{cur.x, cur.y, cur.z}.length() < 0.45f, "offset bounded by max_offset x spacing");
    // Clear of surfaces: drifts back to the grid position.
    for (u32 r = 0; r < rays; ++r) {
        dist[r] = 0.8f;
    }
    const fuse::math::Vec4 displaced{0.2f, 0.f, 0.f, ddgi_kernel::kProbeActive};
    out = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, displaced);
    expect(out.x == 0.f && out.y == 0.f && out.z == 0.f && out.w == ddgi_kernel::kProbeActive,
           "clear of surfaces: back to the grid position (margin 0.5 > offset 0.2)");

    // Nothing within the cell (every hit farther than one spacing / misses): inactive, not moved.
    for (u32 r = 0; r < rays; ++r) {
        dist[r] = r % 2u == 0u ? 10.f : 1.9f;
    }
    out = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, origin);
    expect(out.w == ddgi_kernel::kProbeInactive && out.x == 0.f && out.y == 0.f && out.z == 0.f, "no surface in the cell: inactive");
    dist[5] = 0.5f;
    out = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, origin);
    expect(out.w == ddgi_kernel::kProbeActive, "one front face inside the cell: active");

    // Features off: the data passes through unchanged.
    p.relocation = false;
    p.classification = false;
    out = ddgi_kernel::update_probe_state(p, dirs.data(), dist.data(), rays, displaced);
    expect(out.x == 0.2f && out.w == ddgi_kernel::kProbeActive, "relocation / classification off: unchanged");

    // Sampling: an inactive corner probe takes no part; with every probe inactive the sample is 0.
    const DDGIDesc desc = roomVolume();
    BlendReferenceState st;
    DdgiCpuConfig cfg = statesConfig(desc);
    initialVolumeState(desc, cfg, st);
    for (Vec3& t : st.irradiance) {
        t = {1.f, 1.f, 1.f};
    }
    const ddgi_kernel::VolumeView v = volumeView(desc, cfg, st);
    const Vec3 pos{0.1f, 1.f, 0.2f};
    const Vec3 all = ddgi_kernel::sample_irradiance(v, pos, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f});
    for (fuse::math::Vec4& d : st.probeData) {
        d.w = ddgi_kernel::kProbeInactive;
    }
    const Vec3 none = ddgi_kernel::sample_irradiance(v, pos, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f});
    expect(std::fabs(all.y - ddgi_kernel::kPi) < 1e-5f && none.y == 0.f, "inactive probes skipped by sample_irradiance");
    std::printf("state: relocation (inside, near a face, bounded, drift back) and classification rules hold\n");
}

// --- leak -------------------------------------------------------------------------------------------------
// Thin-wall leak (the WP-6.1 exit criterion) on the CPU oracle: the Vulkan leak gate's closed room (5 cm walls
// and roof, probes 10 cm outside and 95 cm inside the walls, spacing 1.1) in two orientations, 16 updates of
// every probe, irradiance sampled on the inner and outer faces of the four walls (60 points each) with the view
// direction of a camera on that side (room centre / 3 m out):
//   light inside   an emissive panel under the roof, no sun, no sky: the outer faces are dark
//   sun outside    sun + sky, the room closed: the inner faces are dark
// The physically correct dark-side irradiance is exactly 0 in both, so every bit of it is leak. Criterion:
// max dark-side luminance <= eps = 2% of the mean lit-side luminance (2% is below the ~2% Weber contrast
// threshold of vision, i.e. the leak is not visible next to the lit side; it is also what one 8-bit sRGB
// code step is worth at mid grey).
constexpr f64 kLeakEpsilon = 0.02;

namespace leak_test {

DdgiCpuScene scene(bool lightInside) {
    DdgiCpuScene s;
    DdgiCpuSurface grey{};
    grey.albedo = {0.75f, 0.75f, 0.75f};
    constexpr f32 t = 0.05f;
    s.addBox({-6.f, -0.5f, -6.f}, {6.f, 0.f, 6.f}, grey);
    s.addBox({-1.5f - t, 0.f, -1.5f - t}, {-1.5f, 2.f + t, 1.5f + t}, grey);
    s.addBox({1.5f, 0.f, -1.5f - t}, {1.5f + t, 2.f + t, 1.5f + t}, grey);
    s.addBox({-1.5f, 0.f, -1.5f - t}, {1.5f, 2.f + t, -1.5f}, grey);
    s.addBox({-1.5f, 0.f, 1.5f}, {1.5f, 2.f + t, 1.5f + t}, grey);
    s.addBox({-1.5f, 2.f, -1.5f}, {1.5f, 2.f + t, 1.5f}, grey);
    if (lightInside) {
        DdgiCpuSurface lamp{};
        lamp.albedo = {0.2f, 0.2f, 0.2f};
        lamp.emissive = {8.f, 8.f, 8.f};
        s.addBox({-0.5f, 1.85f, -0.5f}, {0.5f, 1.9f, 0.5f}, lamp);
    } else {
        s.sun_direction = Vec3{-0.6f, 0.7f, -0.3f}.normalized();
        s.sun_irradiance = {4.f, 4.f, 4.f};
        s.sky_radiance = {0.4f, 0.45f, 0.5f};
    }
    return s;
}

DDGIDesc volume() {
    DDGIDesc d{};
    d.grid_origin = {-2.75f, 0.5f, -2.75f};
    d.probe_spacing = {1.1f, 1.1f, 1.1f};
    d.grid_dims = {6u, 3u, 6u};
    d.rays_per_probe = 128;
    d.probes_per_frame = 108;
    d.irradiance_res = 8;
    d.depth_res = 16;
    d.hysteresis = 0.9f;
    d.max_ray_distance = 12.f;
    return d;
}

struct Point {
    Vec3 position;
    Vec3 normal;
    Vec3 view;
    bool inner = false;
};

std::vector<Point> points() {
    std::vector<Point> pts;
    constexpr f32 t = 0.05f;
    for (u32 wall = 0; wall < 4u; ++wall) {
        for (u32 side = 0; side < 2u; ++side) {
            for (u32 k = 0; k < 15u; ++k) {
                const f32 a = -1.2f + 2.4f * static_cast<f32>(k % 5u) / 4.f;
                const f32 y = 0.3f + 1.4f * static_cast<f32>(k / 5u) / 2.f;
                const f32 sgn = wall % 2u == 0u ? -1.f : 1.f;
                const f32 face = side == 0u ? 1.5f : 1.5f + t;
                const f32 nrm = side == 0u ? -sgn : sgn;
                Point p{};
                p.position = {0.f, y, 0.f};
                if (wall < 2u) {
                    p.position.x = sgn * face;
                    p.position.z = a;
                    p.normal.x = nrm;
                } else {
                    p.position.x = a;
                    p.position.z = sgn * face;
                    p.normal.z = nrm;
                }
                p.inner = side == 0u;
                const Vec3 camera = p.inner ? Vec3{0.f, 1.f, 0.f} : p.position + p.normal * 3.f;
                p.view = (camera - p.position).normalized();
                pts.push_back(p);
            }
        }
    }
    return pts;
}

f64 luminance(const Vec3& e) {
    return 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z;
}

struct Result {
    f64 lit = 0.0;     ///< mean lit-side luminance
    f64 darkMean = 0.0;
    f64 darkMax = 0.0;
    bool pass() const { return darkMax <= kLeakEpsilon * lit; }
};

/// Lit / dark statistics of a sampler (lightInside: lit = inner faces).
template <typename Sample>
Result measure(const std::vector<Point>& pts, bool lightInside, Sample&& sample) {
    Result r{};
    f64 lit = 0.0, dark = 0.0;
    u32 nLit = 0, nDark = 0;
    for (u32 i = 0; i < pts.size(); ++i) {
        const f64 l = luminance(sample(i));
        if (pts[i].inner == lightInside) {
            lit += l;
            ++nLit;
        } else {
            dark += l;
            ++nDark;
            r.darkMax = std::max(r.darkMax, l);
        }
    }
    r.lit = lit / nLit;
    r.darkMean = dark / nDark;
    return r;
}

} // namespace leak_test

void testLeak() {
    const DDGIDesc desc = leak_test::volume();
    const std::vector<leak_test::Point> pts = leak_test::points();
    for (const bool lightInside : {true, false}) {
        const DdgiCpuScene scene = leak_test::scene(lightInside);
        const char* name = lightInside ? "light inside" : "sun outside";
        for (u32 variant = 0; variant < 3u; ++variant) {
            DdgiCpuConfig c = statesConfig(desc);
            const char* label = "relocation + classification + surface bias + clamp";
            if (variant == 1u) {
                c = DdgiCpuConfig{}; // the oracle's rules before this package
                label = "baseline (no relocation / classification / view bias / clamp)";
            } else if (variant == 2u) {
                c.probe_relocation = false;
                c.probe_classification = false;
                label = "surface bias + clamp only";
            }
            DdgiCpuVolume v;
            expect(v.init(desc, c), "leak volume init");
            std::vector<u32> all(v.probeCount());
            for (u32 i = 0; i < all.size(); ++i) {
                all[i] = i;
            }
            for (u32 f = 0; f < 16u; ++f) {
                v.updateProbes(scene, all.data(), static_cast<u32>(all.size()), f);
            }
            const leak_test::Result r = leak_test::measure(pts, lightInside, [&](u32 i) {
                return v.sampleIrradiance(pts[i].position, pts[i].normal, pts[i].view);
            });
            u32 inactive = 0;
            for (u32 i = 0; i < v.probeCount(); ++i) {
                inactive += v.probeActive(i) ? 0u : 1u;
            }
            std::printf("leak (%s, %s): lit mean %.4f, dark mean %.5f (%.2f%%), dark max %.5f (%.2f%% of lit; eps %.0f%%), "
                        "%u inactive probes -> %s\n",
                        name, label, r.lit, r.darkMean, 100.0 * r.darkMean / r.lit, r.darkMax, 100.0 * r.darkMax / r.lit,
                        100.0 * kLeakEpsilon, inactive, r.pass() ? "pass" : "leaks");
            expect(r.lit > 0.1, "the lit side is lit");
            if (variant == 0u) {
                expect(r.pass(), "thin-wall leak: dark-side luminance <= 2% of the lit side (oracle, all features)");
            }
            if (variant == 1u && !lightInside) {
                expect(!r.pass(), "the baseline rules leak through the 5 cm walls (what the features fix)");
            }
        }
    }
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
    if (suite == "state" || suite == "all") {
        testState();
    }
    if (suite == "leak" || suite == "all") {
        testLeak();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
