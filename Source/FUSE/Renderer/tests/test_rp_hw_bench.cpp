// Renderer plan §5.3 per-pass budget harness at 1440p + the P2 / P3 / P8 budget numbers on a discrete GPU
// (docs/unification/RENDERER-HW-RUN.md). One FrameComposer frame (T2: TLAS, RT shadows + SVGF, ray-query DDGI, RT
// reflections; VSM, clustered lighting, SSFX, sky / aerial, froxel fog, clouds, forward transparency, TAAU, post) over
// a procedural Sponza / Bistro stand-in (test_rp_hw_bench_scene.hpp), render 1706 x 960 -> TAAU -> 2560 x 1440.
// Every executed render-graph pass gets a GpuProfiler timestamp zone (executor pass hooks); passes are grouped into
// the §5.3 stages by name (stageOf).
//
//   --mode passes        every pass name with its median GPU time (diagnostics)
//   --mode report        both scenes: the §5.3 table (median, p10..p90, plan budget)
//   --mode gate          perf-regression gate: each §5.3 stage and the frame total vs the committed baseline
//                        (tests/perf/rp_hw_bench_baseline.json, per device + scene); fails on > 5 % (+ 0.02 ms
//                        timestamp floor for sub-0.4 ms stages). Noise: 90 warm-up frames, 5 trials x 48 frames,
//                        median of the trial medians; one full re-measure before failing.
//                        FUSE_UPDATE_PERF_BASELINE=1 writes the measured values as the device's baseline.
//   --mode lights4096    P2: Bistro stand-in with 4,096 local lights (+ sun): "Direct lighting and shadows" <= 3.0 ms
//   --mode vsm_cached    P3: static camera + scene, VSM cached frame: every vsm.* pass <= 1.0 ms in sum
//   --mode volumetrics   P8: froxel fog + clouds (+ frame.gather) <= 1.5 ms
//   --scene sponza | bistro    (passes / gate; default sponza)
//   --frames N --trials K --warmup W
//   --preset realtime | defaults | reference   (applyPreset; knobs --ssgi 0|1 --gtao-slices N --gtao-steps N --atrous N --refl-atrous N)
//
// Exit 77 = skip (stub build, no discrete GPU, below the T2 gate, no baseline entry for this device).
//
// NOTE: renderer headers come before test_rp_hw_bench_common.hpp (vulkan.h -> windows.h `far` / `near` macros).
#include "test_rp_hw_bench_scene.hpp"

#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>

#include "test_rp_hw_bench_common.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if !defined(FUSE_VULKAN_BACKEND)
int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return 77;
}
#else

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::frame;
using namespace hw_bench;

constexpr u32 kRenderW = 1706;
constexpr u32 kRenderH = 960;
constexpr u32 kDisplayW = 2560;
constexpr u32 kDisplayH = 1440;

// --- §5.3 stages -----------------------------------------------------------------------------------------
enum Stage : u32 { kCullVis = 0, kMaterial, kDirect, kGi, kVolTransp, kPost, kUi, kOther, kStageCount };
constexpr const char* kStageId[kStageCount] = {"culling_visbuffer", "material_resolve", "direct_lighting_shadows",
                                                "global_illumination", "volumetrics_transparency", "post_upscale",
                                                "ui", "other"};
constexpr const char* kStageName[kStageCount] = {"Culling and visibility buffer", "Material resolve",
                                                  "Direct lighting and shadows", "Global illumination",
                                                  "Volumetrics and transparency", "Post-process and upscale",
                                                  "UI", "(unclassified)"};
/// Plan §5.3 budgets (ms). UI shares 3.6 ms with the headroom.
constexpr f64 kBudget[kStageCount] = {2.0, 1.5, 3.0, 3.0, 1.5, 2.0, 3.6, 0.0};
constexpr f64 kBudgetTotal = 16.6;

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

Stage stageOf(const std::string& n) {
    // scene upload, culling, visibility buffer, acceleration-structure refit / build (scene preparation)
    if (starts(n, "gpu_scene.") || starts(n, "cull.") || starts(n, "vis.") || starts(n, "meshlet.") || starts(n, "swraster.") ||
        starts(n, "stream.") || starts(n, "dgc.") || starts(n, "rt.tlas") || starts(n, "rt.blas")) {
        return kCullVis;
    }
    if (starts(n, "resolve.") || starts(n, "materials.") || starts(n, "temporal.motion")) {
        return kMaterial;
    }
    // clustered lighting, VSM, RT shadows + their SVGF + pack, ReSTIR DI (when on).
    if (starts(n, "light.") || starts(n, "vsm.") || starts(n, "rt.shadow") || starts(n, "frame.shadow") || starts(n, "denoise.") ||
        starts(n, "svgf.") || starts(n, "light_tree.") || starts(n, "restir.") || starts(n, "frame.restir")) {
        return kDirect;
    }
    if (starts(n, "refl/")) {
        return kGi; // the RT-reflection SVGF (zoneTimes renames it by recording order)
    }
    if (starts(n, "restir/")) {
        return kDirect;
    }
    if (starts(n, "ddgi.") || starts(n, "ssfx.") || starts(n, "ssrc.") || starts(n, "rt.reflections") || starts(n, "frame.reflect") ||
        starts(n, "frame.rc_pack") || starts(n, "rc.")) {
        return kGi;
    }
    if (starts(n, "fog.") || starts(n, "clouds.") || starts(n, "atmos.") || starts(n, "frame.sky") || starts(n, "frame.aerial") ||
        starts(n, "frame.gather") || starts(n, "forward.") || starts(n, "gsplat.") || starts(n, "radix.")) {
        return kVolTransp;
    }
    if (starts(n, "taau.") || starts(n, "fsr3.") || starts(n, "post.") || starts(n, "frame.resolve") || starts(n, "fg.")) {
        return kPost;
    }
    if (starts(n, "frame.ui") || starts(n, "present.")) {
        return kUi;
    }
    return kOther;
}

bool isVolumetric(const std::string& n) { return starts(n, "fog.") || starts(n, "clouds.") || starts(n, "frame.gather"); }
bool isAtmosphere(const std::string& n) { return starts(n, "atmos.") || starts(n, "frame.sky") || starts(n, "frame.aerial"); }

// --- 1440p T2 preset ---------------------------------------------------------------------------------------
/// The frame settings the harness measures. `realtime` / `defaults` (default): FrameSettings{} as shipped, which is
/// the T2 1440p configuration of RENDERER-HW-RUN.md, "Preset" (SSGI off while DDGI runs, GTAO 2 slices x 6 steps,
/// RT-shadow SVGF 3 a-trous iterations; frameSsfxPreset / frameShadowDenoiserPreset / ssgiWithDdgi). `reference`:
/// the former reference-quality defaults (16-ray x 64-step SSGI next to DDGI, GTAO 4 x 8, shadow SVGF 5 iterations).
struct Preset {
    bool reference = false;
    int ssgi = -1;        ///< -1: preset value
    int gtaoSlices = -1;
    int gtaoSteps = -1;
    int atrous = -1;
    int reflAtrous = -1; ///< reflection SVGF a-trous iterations (DenoiserSettings::spatial_iterations)
};
Preset g_preset{};

void applyPreset(FrameSettings& fs) {
    if (g_preset.reference) {
        fs.ssgiWithDdgi = true;
        fs.ssfxSettings.gtao.slices = 4;
        fs.ssfxSettings.gtao.steps = 8;
        fs.denoiser.atrousIterations = 5;
    }
    if (g_preset.ssgi >= 0) {
        fs.ssfxSettings.ssgi = g_preset.ssgi != 0;
        fs.ssgiWithDdgi = g_preset.ssgi != 0;
    }
    if (g_preset.gtaoSlices > 0) {
        fs.ssfxSettings.gtao.slices = static_cast<u32>(g_preset.gtaoSlices);
    }
    if (g_preset.gtaoSteps > 0) {
        fs.ssfxSettings.gtao.steps = static_cast<u32>(g_preset.gtaoSteps);
    }
    if (g_preset.atrous > 0) {
        fs.denoiser.atrousIterations = static_cast<u32>(g_preset.atrous);
    }
    if (g_preset.reflAtrous > 0) {
        fs.reflectionDenoiser.spatial_iterations = static_cast<u32>(g_preset.reflAtrous);
    }
}

// --- rig ---------------------------------------------------------------------------------------------------
struct Rig {
    hw_scene::Desc desc;
    gpu_scene::GpuScene scene;
    std::vector<geometry::MeshletMesh> meshes;
    FrameComposer composer;
    FrameSettings settings;
    rg::Graph graph;
    bool moving = false;
    u32 frameIndex = 0;
    ~Rig() { composer.destroy(); }
};

using FrameZones = std::map<std::string, f64>;

int initRig(HwContext& ctx, Rig& rig, hw_scene::Desc desc) {
    rig.desc = std::move(desc);
    hw_scene::countTriangles(rig.desc);
    rig.meshes.resize(rig.desc.meshes.size());
    for (usize i = 0; i < rig.desc.meshes.size(); ++i) {
        if (!mr_test::build(rig.desc.meshes[i], rig.meshes[i])) {
            std::fprintf(stderr, "FAIL: meshlet build\n");
            return 1;
        }
    }
    const u32 instances = static_cast<u32>(rig.desc.instances.size());
    const u32 lights = static_cast<u32>(rig.desc.lights.size());
    gpu_scene::GpuSceneDesc sd{};
    sd.device = ctx.device.get();
    sd.allocator = ctx.allocator.get();
    sd.upload = &ctx.upload;
    sd.bindless = &ctx.bindless;
    sd.instanceCapacity = instances + 16u;
    sd.meshCapacity = 16u;
    sd.materialCapacity = static_cast<u32>(rig.desc.materials.size()) + 8u;
    sd.lightCapacity = lights + 16u;
    if (!rig.scene.init(sd) || !rig.scene.gpuEnabled()) {
        std::fprintf(stderr, "FAIL: GpuScene\n");
        return 1;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
    for (usize i = 0; i < rig.meshes.size(); ++i) {
        if (rig.scene.addMeshletMesh(rig.meshes[i]) != i) {
            std::fprintf(stderr, "FAIL: addMeshletMesh\n");
            return 1;
        }
    }
    for (usize i = 0; i < rig.desc.materials.size(); ++i) {
        rig.scene.setMaterial(static_cast<u32>(i), rig.desc.materials[i]);
    }
    std::vector<std::pair<gpu_scene::InstanceHandle, f32>> transparent;
    for (const hw_scene::Instance& in : rig.desc.instances) {
        gpu_scene::InstanceDesc id{};
        id.mesh = in.mesh;
        id.material = in.material;
        id.transform = hw_scene::transformOf(in);
        if (in.transparent) {
            id.flags |= gpu_scene::kInstanceTransparent;
        }
        const gpu_scene::InstanceHandle h = rig.scene.addInstance(id);
        if (!h.valid()) {
            std::fprintf(stderr, "FAIL: addInstance\n");
            return 1;
        }
        if (in.transparent) {
            transparent.emplace_back(h, in.opacity);
        }
    }
    for (const gpu_scene::GpuLight& l : rig.desc.lights) {
        rig.scene.addLight(l);
    }
    const gpu_scene::GpuSceneCommitStats stats = rig.scene.commit();
    ctx.upload.flush();
    if (!stats.ok || !ctx.upload.waitAll()) {
        std::fprintf(stderr, "FAIL: scene commit\n");
        return 1;
    }

    FrameComposerDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.upload = &ctx.upload;
    d.scene = &rig.scene;
    d.tier = FrameTier::T2;
    d.renderWidth = kRenderW;
    d.renderHeight = kRenderH;
    d.displayWidth = kDisplayW;
    d.displayHeight = kDisplayH;
    d.instanceCapacity = instances + 16u;
    d.meshCapacity = 16u;
    d.lightCapacity = lights + 16u;
    d.vsmPoolPagesX = 64;
    d.vsmPoolPagesY = 64;
    d.vsmLocalPagesX = 16;
    d.vsmLocalPagesY = 8;
    // DDGI volume over the scene bounds, ~2.5 m (Sponza) / 5 m (Bistro) spacing
    const f32 spacing = rig.desc.kind == hw_scene::Kind::Sponza ? 2.5f : 5.f;
    DDGIDesc& v = d.ddgiVolume;
    v.grid_origin = {rig.desc.bounds[0][0], rig.desc.bounds[0][1] + 0.5f, rig.desc.bounds[0][2]};
    v.probe_spacing = {spacing, spacing * 0.8f, spacing};
    v.grid_dims = {static_cast<u32>((rig.desc.bounds[1][0] - rig.desc.bounds[0][0]) / spacing) + 1u,
                   static_cast<u32>((rig.desc.bounds[1][1] - rig.desc.bounds[0][1]) / (spacing * 0.8f)) + 1u,
                   static_cast<u32>((rig.desc.bounds[1][2] - rig.desc.bounds[0][2]) / spacing) + 1u};
    v.rays_per_probe = 128;
    v.probes_per_frame = 256;
    v.max_ray_distance = 40.f;
    d.frameGen = false;
    if (!rig.composer.init(d)) {
        std::printf("SKIP: FrameComposer::init: %s\n", rig.composer.reason());
        return kSkip;
    }
    for (const auto& [h, o] : transparent) {
        rig.composer.setOpacity(h, o);
    }
    FrameSettings& fs = rig.settings;
    fs = FrameSettings{};
    fs.restir = false;  // ReSTIR DI is measured on its own (fuse_rp_hw_p7_restir_10k)
    fs.frameGen = false;
    fs.splats = false;
    fs.upscaler = FrameUpscaler::Taau;
    fs.fogSettings.farPlane = rig.desc.kind == hw_scene::Kind::Sponza ? 48.f : 96.f;
    fs.fogSettings.medium.density = 0.02f;
    applyPreset(fs);
    std::printf("scene %s: %zu instances, %llu source triangles, %zu materials, %zu lights; composer %s kernel, available 0x%07x\n",
                hw_scene::kindName(rig.desc.kind), rig.desc.instances.size(), static_cast<unsigned long long>(rig.desc.triangles),
                rig.desc.materials.size(), rig.desc.lights.size(), rig.composer.kernelLanguage(), rig.composer.available());
    return 0;
}

FrameDesc frameDesc(const Rig& rig, u64 serial) {
    FrameDesc fd{};
    fd.serial = serial;
    fd.frameIndex = rig.frameIndex;
    const f32 t = rig.moving ? static_cast<f32>(rig.frameIndex % 240u) * 0.05f : 0.f;
    for (u32 k = 0; k < 3u; ++k) {
        fd.camera.eye[k] = rig.desc.eye[k];
        fd.camera.target[k] = rig.desc.target[k];
    }
    fd.camera.eye[0] += t;
    fd.camera.target[0] += t;
    fd.camera.fovY = 1.0f;
    fd.camera.nearPlane = 0.1f;
    fd.camera.farPlane = 300.f;
    fd.sun.slot = 0;
    std::memcpy(fd.sun.toSun, rig.desc.toSun, sizeof(fd.sun.toSun));
    fd.sun.illuminance[0] = fd.sun.illuminance[1] = fd.sun.illuminance[2] = 3.f;
    fd.resetHistory = rig.frameIndex == 0u;
    return fd;
}

/// One frame; `zones` (optional) receives the per-pass GPU ms of this frame.
bool runFrame(HwContext& ctx, Rig& rig, FrameZones* zones, f64* span = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
    rig.composer.beginSceneFrame(ctx.serial);
    const gpu_scene::GpuSceneCommitStats stats = rig.scene.commit();
    const bool committed = rig.composer.commitScene();
    ctx.upload.flush();
    if (!rig.composer.beginFrame(frameDesc(rig, ctx.serial), rig.settings)) {
        std::fprintf(stderr, "FAIL: FrameComposer::beginFrame\n");
        return false;
    }
    rig.graph.reset();
    rig.composer.addFrame(rig.graph);
    ctx.profiler->beginFrame();
    const rg::ExecuteResult result = ctx.executor->execute(rig.graph);
    ctx.profiler->endFrame();
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.composer.collectRetired(ctx.serial);
    rig.scene.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    ++rig.frameIndex;
    ctx.profiler->resolve();
    if (zones != nullptr) {
        const GpuFrameTimings& t = ctx.profiler->latest();
        if (!t.valid || t.skippedZones != 0u) {
            std::fprintf(stderr, "FAIL: profiler frame invalid (skipped zones %u)\n", t.skippedZones);
            return false;
        }
        *zones = zoneTimes(t);
        if (span != nullptr) {
            *span = frameSpanMs(t);
        }
    }
    return stats.ok && committed && result.ok && waited;
}

struct StageFrame {
    f64 stage[kStageCount] = {};
    f64 total = 0.0; ///< sum of every pass
    f64 span = 0.0;  ///< first begin .. last end
};

StageFrame classify(const FrameZones& z, f64 span) {
    StageFrame f{};
    for (const auto& [name, ms] : z) {
        f.stage[stageOf(name)] += ms;
        f.total += ms;
    }
    f.span = span;
    return f;
}

/// `frames` measured frames (after `warmup`): per-pass samples (ms, one per frame; 0 when a pass did not run).
bool measure(HwContext& ctx, Rig& rig, u32 warmup, u32 frames, std::map<std::string, std::vector<f64>>& passSamples,
             std::vector<StageFrame>& stageSamples) {
    for (u32 i = 0; i < warmup; ++i) {
        if (!runFrame(ctx, rig, nullptr)) {
            return false;
        }
    }
    for (u32 i = 0; i < frames; ++i) {
        FrameZones z;
        f64 span = 0.0;
        if (!runFrame(ctx, rig, &z, &span)) {
            return false;
        }
        for (const auto& [name, ms] : z) {
            std::vector<f64>& v = passSamples[name];
            v.resize(i, 0.0);
            v.push_back(ms);
        }
        stageSamples.push_back(classify(z, span));
    }
    for (auto& [name, v] : passSamples) {
        v.resize(frames, 0.0);
    }
    return true;
}

struct StageResult {
    f64 stage[kStageCount] = {};
    f64 total = 0.0;
    f64 span = 0.0;
    Stats stageStats[kStageCount]{};
    Stats totalStats{};
};

StageResult reduce(const std::vector<StageFrame>& frames) {
    StageResult r{};
    for (u32 s = 0; s < kStageCount; ++s) {
        std::vector<f64> v;
        for (const StageFrame& f : frames) {
            v.push_back(f.stage[s]);
        }
        r.stageStats[s] = stats(v);
        r.stage[s] = r.stageStats[s].median;
    }
    std::vector<f64> t;
    std::vector<f64> sp;
    for (const StageFrame& f : frames) {
        t.push_back(f.total);
        sp.push_back(f.span);
    }
    r.totalStats = stats(t);
    r.total = r.totalStats.median;
    r.span = stats(sp).median;
    return r;
}

/// K trials of `frames` frames, the median of the trial medians per stage (+ the spread of the trial medians).
struct TrialResult {
    StageResult median;          ///< median of trial medians
    f64 lo[kStageCount + 1] = {}; ///< min / max trial median (index kStageCount = total)
    f64 hi[kStageCount + 1] = {};
    std::map<std::string, f64> passMedian; ///< over all measured frames
};

bool trials(HwContext& ctx, Rig& rig, u32 warmup, u32 frames, u32 k, TrialResult& out) {
    std::vector<StageResult> per;
    std::map<std::string, std::vector<f64>> allPasses;
    for (u32 t = 0; t < k; ++t) {
        std::map<std::string, std::vector<f64>> passes;
        std::vector<StageFrame> sf;
        if (!measure(ctx, rig, t == 0u ? warmup : 8u, frames, passes, sf)) {
            return false;
        }
        per.push_back(reduce(sf));
        for (auto& [n, v] : passes) {
            allPasses[n].insert(allPasses[n].end(), v.begin(), v.end());
        }
    }
    for (u32 s = 0; s <= kStageCount; ++s) {
        std::vector<f64> v;
        for (const StageResult& r : per) {
            v.push_back(s == kStageCount ? r.total : r.stage[s]);
        }
        const Stats st = stats(v);
        if (s == kStageCount) {
            out.median.total = st.median;
        } else {
            out.median.stage[s] = st.median;
        }
        out.lo[s] = st.min;
        out.hi[s] = st.max;
    }
    std::vector<f64> spans;
    for (const StageResult& r : per) {
        spans.push_back(r.span);
    }
    out.median.span = stats(spans).median;
    for (const auto& [n, v] : allPasses) {
        out.passMedian[n] = stats(v).median;
    }
    return true;
}

void printTable(const char* title, const TrialResult& r) {
    std::printf("\n%s (2560x1440 output, 1706x960 render + TAAU; median of trial medians [min..max trial])\n", title);
    std::printf("  %-32s %9s %19s %8s %s\n", "stage", "ms", "trial spread", "budget", "");
    for (u32 s = 0; s < kStageCount; ++s) {
        if (s == kOther && r.median.stage[s] == 0.0) {
            continue;
        }
        std::printf("  %-32s %9.3f   [%6.3f..%6.3f] %8.1f %s\n", kStageName[s], r.median.stage[s], r.lo[s], r.hi[s], kBudget[s],
                    kBudget[s] > 0.0 ? (r.median.stage[s] <= kBudget[s] ? "within" : "OVER") : "");
    }
    std::printf("  %-32s %9.3f   [%6.3f..%6.3f] %8.1f %s\n", "Total (sum of passes)", r.median.total, r.lo[kStageCount],
                r.hi[kStageCount], kBudgetTotal, r.median.total <= kBudgetTotal ? "within" : "OVER");
    std::printf("  %-32s %9.3f\n", "Frame span (first..last zone)", r.median.span);
}

void printPasses(const TrialResult& r) {
    std::vector<std::pair<f64, std::string>> v;
    for (const auto& [n, ms] : r.passMedian) {
        v.emplace_back(ms, n);
    }
    std::sort(v.rbegin(), v.rend());
    for (const auto& [ms, n] : v) {
        std::printf("    %-28s %8.3f ms  [%s]\n", n.c_str(), ms, kStageId[stageOf(n)]);
    }
}

hw_scene::Desc sceneFor(const std::string& name, u32 lights = 256) {
    return name == "bistro" ? hw_scene::makeBistro(lights) : hw_scene::makeSponza();
}

void gpuLoadNote() {
    std::printf("note: timestamps are GPU time of this process's passes; another process sharing the GPU shows up as a slow "
                "trial (see the trial spread)\n");
}

// --- modes -------------------------------------------------------------------------------------------------
struct Args {
    std::string mode = "report";
    std::string scene = "sponza";
    u32 frames = 48;
    u32 trials = 5;
    u32 warmup = 90;
};

int runReport(HwContext& ctx, const Args& a, bool passesOnly) {
    const std::vector<std::string> scenes = passesOnly || a.mode == "passes" ? std::vector<std::string>{a.scene}
                                                                             : std::vector<std::string>{"sponza", "bistro"};
    for (const std::string& name : scenes) {
        Rig rig;
        if (const int rc = initRig(ctx, rig, sceneFor(name)); rc != 0) {
            return rc;
        }
        TrialResult r;
        if (!trials(ctx, rig, a.warmup, a.frames, a.trials, r)) {
            std::fprintf(stderr, "FAIL: frames\n");
            return 1;
        }
        printTable((std::string("§5.3 budget, ") + name).c_str(), r);
        printPasses(r);
    }
    gpuLoadNote();
    return g_failures == 0 ? 0 : 1;
}

int runGate(HwContext& ctx, const Args& a) {
    Rig rig;
    if (const int rc = initRig(ctx, rig, sceneFor(a.scene)); rc != 0) {
        return rc;
    }
    const std::string section = ctx.device->info().deviceName + " | " + a.scene;
    JsonDoc doc;
    jsonRead(FUSE_RP_HW_BASELINE, doc);
    const bool update = envFlag("FUSE_UPDATE_PERF_BASELINE");
    if (!update && doc.find(section) == doc.end()) {
        std::printf("SKIP: no baseline for \"%s\" in %s (record one with FUSE_UPDATE_PERF_BASELINE=1)\n", section.c_str(),
                    FUSE_RP_HW_BASELINE);
        return kSkip;
    }
    constexpr f64 kTolerance = 0.05; // plan §8: fail on a regression above 5 %
    constexpr f64 kFloorMs = 0.02;   // timestamp / scheduling floor for small stages
    TrialResult r;
    u32 failures = 0;
    for (u32 attempt = 0; attempt < 2u; ++attempt) {
        r = TrialResult{};
        if (!trials(ctx, rig, a.warmup, a.frames, a.trials, r)) {
            std::fprintf(stderr, "FAIL: frames\n");
            return 1;
        }
        printTable((std::string("§5.3 gate, ") + a.scene + (attempt == 0u ? "" : " (re-measure)")).c_str(), r);
        if (update) {
            break;
        }
        const JsonSection& base = doc[section];
        failures = 0;
        auto check = [&](const char* key, f64 v) {
            const auto it = base.find(key);
            if (it == base.end()) {
                return;
            }
            const f64 limit = it->second * (1.0 + kTolerance) + kFloorMs;
            const bool ok = v <= limit;
            std::printf("  gate %-26s %8.3f ms vs baseline %8.3f (limit %8.3f, %+6.1f %%) %s\n", key, v, it->second, limit,
                        it->second > 0.0 ? (v / it->second - 1.0) * 100.0 : 0.0, ok ? "ok" : "REGRESSION");
            failures += ok ? 0u : 1u;
        };
        for (u32 s = 0; s < kStageCount; ++s) {
            check(kStageId[s], r.median.stage[s]);
        }
        check("total", r.median.total);
        if (failures == 0u) {
            break;
        }
        std::printf("  %u regression(s): re-measuring once before failing\n", failures);
    }
    if (update) {
        JsonSection& sec = doc[section];
        sec.clear();
        for (u32 s = 0; s < kStageCount; ++s) {
            sec[kStageId[s]] = r.median.stage[s];
        }
        sec["total"] = r.median.total;
        sec["span"] = r.median.span;
        const bool ok = jsonWrite(FUSE_RP_HW_BASELINE, doc,
                                  "fuse_rp_hw_bench --mode gate baselines (ms, median of 5 trial medians x 48 frames at "
                                  "2560x1440 / 1706x960 TAAU). Sections: '<device> | <scene>'. Rewrite: "
                                  "FUSE_UPDATE_PERF_BASELINE=1 ctest -L hw -R budget");
        std::printf("baseline \"%s\" %s %s\n", section.c_str(), ok ? "written to" : "NOT written to", FUSE_RP_HW_BASELINE);
        return ok ? 0 : 1;
    }
    gpuLoadNote();
    expect(failures == 0u, "every §5.3 stage within 5 % of the baseline");
    return g_failures == 0 ? 0 : 1;
}

int runLights(HwContext& ctx, const Args& a) {
    f64 direct[2] = {};
    const u32 counts[2] = {256u, 4096u};
    for (u32 i = 0; i < 2u; ++i) {
        Rig rig;
        if (const int rc = initRig(ctx, rig, hw_scene::makeBistro(counts[i])); rc != 0) {
            return rc;
        }
        TrialResult r;
        if (!trials(ctx, rig, a.warmup, a.frames, a.trials, r)) {
            std::fprintf(stderr, "FAIL: frames\n");
            return 1;
        }
        printTable((std::string("P2, Bistro stand-in, ") + std::to_string(counts[i]) + " local lights").c_str(), r);
        printPasses(r);
        f64 light = 0.0;
        f64 vsm = 0.0;
        f64 rt = 0.0;
        for (const auto& [n, ms] : r.passMedian) {
            if (starts(n, "light.")) {
                light += ms;
            } else if (starts(n, "vsm.")) {
                vsm += ms;
            } else if (stageOf(n) == kDirect) {
                rt += ms;
            }
        }
        std::printf("  direct lighting + shadows: %.3f ms = clustered light.* %.3f + vsm.* %.3f + RT shadows / SVGF / pack %.3f "
                    "(per-pass medians)\n",
                    r.median.stage[kDirect], light, vsm, rt);
        direct[i] = r.median.stage[kDirect];
    }
    std::printf("\nP2 exit: 4,096 lights -> direct lighting + shadows %.3f ms (plan budget %.1f ms; 256 lights: %.3f ms)\n", direct[1],
                kBudget[kDirect], direct[0]);
    gpuLoadNote();
    expect(direct[1] <= kBudget[kDirect], "P2: 4,096 lights within the 3.0 ms direct lighting + shadows budget");
    return g_failures == 0 ? 0 : 1;
}

f64 sumPasses(const TrialResult& r, bool (*pred)(const std::string&)) {
    f64 s = 0.0;
    for (const auto& [n, ms] : r.passMedian) {
        s += pred(n) ? ms : 0.0;
    }
    return s;
}

bool isVsm(const std::string& n) { return starts(n, "vsm."); }

int runVsm(HwContext& ctx, const Args& a) {
    f64 cached[2] = {};
    f64 movingMs[2] = {};
    const char* names[2] = {"sponza", "bistro"};
    for (u32 i = 0; i < 2u; ++i) {
        for (u32 m = 0; m < 2u; ++m) {
            Rig rig;
            if (const int rc = initRig(ctx, rig, sceneFor(names[i])); rc != 0) {
                return rc;
            }
            rig.moving = m == 1u;
            TrialResult r;
            if (!trials(ctx, rig, a.warmup, a.frames, a.trials, r)) {
                std::fprintf(stderr, "FAIL: frames\n");
                return 1;
            }
            const f64 v = sumPasses(r, isVsm);
            std::printf("\nP3 %s, %s camera: vsm.* %.3f ms (per-pass medians):\n", names[i], rig.moving ? "moving" : "static (cached)", v);
            for (const auto& [n, ms] : r.passMedian) {
                if (isVsm(n)) {
                    std::printf("    %-24s %8.3f ms\n", n.c_str(), ms);
                }
            }
            (m == 0u ? cached : movingMs)[i] = v;
        }
    }
    std::printf("\nP3 exit: cached-frame VSM cost sponza %.3f ms, bistro %.3f ms (plan < 1 ms); moving camera %.3f / %.3f ms\n", cached[0],
                cached[1], movingMs[0], movingMs[1]);
    gpuLoadNote();
    expect(cached[0] < 1.0 && cached[1] < 1.0, "P3: cached-frame shadow cost under 1 ms");
    return g_failures == 0 ? 0 : 1;
}

int runVolumetrics(HwContext& ctx, const Args& a) {
    const char* names[2] = {"sponza", "bistro"};
    f64 vol[2] = {};
    for (u32 i = 0; i < 2u; ++i) {
        Rig rig;
        if (const int rc = initRig(ctx, rig, sceneFor(names[i])); rc != 0) {
            return rc;
        }
        rig.moving = true; // fog reprojection active
        TrialResult r;
        if (!trials(ctx, rig, a.warmup, a.frames, a.trials, r)) {
            std::fprintf(stderr, "FAIL: frames\n");
            return 1;
        }
        vol[i] = sumPasses(r, isVolumetric);
        const f64 atm = sumPasses(r, isAtmosphere);
        std::printf("\nP8 %s (moving camera): fog + clouds (+ frame.gather) %.3f ms; atmosphere LUTs + sky + aerial %.3f ms; "
                    "§5.3 volumetrics + transparency stage %.3f ms\n",
                    names[i], vol[i], atm, r.median.stage[kVolTransp]);
        for (const auto& [n, ms] : r.passMedian) {
            if (isVolumetric(n) || isAtmosphere(n)) {
                std::printf("    %-24s %8.3f ms\n", n.c_str(), ms);
            }
        }
    }
    std::printf("\nP8 exit: volumetrics sponza %.3f ms, bistro %.3f ms (plan budget 1.5 ms)\n", vol[0], vol[1]);
    gpuLoadNote();
    expect(vol[0] <= 1.5 && vol[1] <= 1.5, "P8: volumetrics within 1.5 ms");
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--mode") {
            a.mode = v;
        } else if (k == "--scene") {
            a.scene = v;
        } else if (k == "--frames") {
            a.frames = static_cast<u32>(std::stoul(v));
        } else if (k == "--trials") {
            a.trials = static_cast<u32>(std::stoul(v));
        } else if (k == "--warmup") {
            a.warmup = static_cast<u32>(std::stoul(v));
        } else if (k == "--preset") {
            g_preset.reference = v == "reference";
        } else if (k == "--ssgi") {
            g_preset.ssgi = std::stoi(v);
        } else if (k == "--gtao-slices") {
            g_preset.gtaoSlices = std::stoi(v);
        } else if (k == "--gtao-steps") {
            g_preset.gtaoSteps = std::stoi(v);
        } else if (k == "--refl-atrous") {
            g_preset.reflAtrous = std::stoi(v);
        } else if (k == "--atrous") {
            g_preset.atrous = std::stoi(v);
        }
    }
    HwContext ctx;
    HwSetupDesc sd{};
    sd.appName = "fuse_rp_hw_bench";
    if (const int rc = setup(ctx, sd); rc != 0) {
        return rc;
    }
    if (!rt::queryRtCapabilities(ctx.device.get()).usable) {
        std::printf("SKIP: device below the T2 gate\n");
        return kSkip;
    }
    int rc = 0;
    if (a.mode == "passes") {
        rc = runReport(ctx, a, true);
    } else if (a.mode == "report") {
        rc = runReport(ctx, a, false);
    } else if (a.mode == "gate") {
        rc = runGate(ctx, a);
    } else if (a.mode == "lights4096") {
        rc = runLights(ctx, a);
    } else if (a.mode == "vsm_cached") {
        rc = runVsm(ctx, a);
    } else if (a.mode == "volumetrics") {
        rc = runVolumetrics(ctx, a);
    } else {
        std::fprintf(stderr, "unknown --mode %s\n", a.mode.c_str());
        return 2;
    }
    return rc;
}

#endif // FUSE_VULKAN_BACKEND
