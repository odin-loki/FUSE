// P5 exit on a discrete GPU (docs/unification/RENDERER-HW-RUN.md, "P5"): "a reference scene of more than 100M source
// triangles renders within budget".
//
// Scene: one heightfield terrain tile (640 x 640 quads = 819,200 triangles, ~3 m bumps + fine detail) -> WP-5.2 cluster
// DAG -> WP-5.3 FCPG pages (32 KiB), instanced 16 x 8 = 128 times (64 m tiles over 1024 x 512 m): 104.9M source
// triangles. Camera 3 m above the near edge looking across the field, 2560 x 1440, fovY 1 rad, 1 px error threshold.
//
// Two measurements, because the WP-5.1 mesh path does not draw DAG clusters yet (RENDERER-EXECUTION.md, P5 row):
//   1. LOD selection on the GPU: one GeometryStreaming per instance (shared DAG links + page file, per-instance
//      residency: budget = coarse + 192 pages), the camera in each tile's object space; after the residency settles
//      the stream.* zones of all 128 instances are summed per frame, timed inside the draw frame of (2) (one graph:
//      cut, then cull + visbuffer + resolve, as a page-aware draw path would run it).
//   2. Drawing the selected cut: the fully-resident cut of every tile (dag::select_dag_cut, same view) is decoded
//      into a per-tile meshlet mesh and drawn through WP-1.3 cull + WP-1.4 visibility buffer + WP-1.5 resolve by a
//      T0 FrameComposer with only its mandatory stages, native 2560 x 1440. "Culling and visibility buffer" =
//      gpu_scene.* + cull.* + vis.* zones; material resolve (resolve.* + temporal.motion) is reported.
// Exit: (1) + (2) within the §5.3 "Culling and visibility buffer" budget (2.0 ms).
//
// Exit 77 = skip (stub build, no discrete GPU, streaming kernel not built).
#include "test_rp_geometry_streaming_common.hpp"

#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/geometry_streaming/geometry_streaming.hpp>

#include "test_rp_hw_bench_common.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
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
using namespace hw_bench;
namespace dag = fuse::renderer::geometry::dag;
namespace gsm = fuse::renderer::geometry_streaming;

constexpr u32 kGrid = 640; // quads per tile side
constexpr f32 kTile = 64.f;
constexpr u32 kTilesX = 16;
constexpr u32 kTilesZ = 8;
constexpr u32 kTiles = kTilesX * kTilesZ;
constexpr u32 kWidth = 2560;
constexpr u32 kHeight = 1440;
constexpr f32 kFovY = 1.0f;
constexpr f32 kEye[3] = {0.f, 3.f, 6.f};
constexpr f32 kTarget[3] = {0.f, 0.f, -200.f};

f64 secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
}

f32 height(f32 x, f32 z) {
    return 1.6f * std::sin(0.11f * x) * std::cos(0.13f * z) + 0.9f * std::sin(0.37f * x + 1.3f) * std::sin(0.29f * z + 0.4f) +
           0.12f * std::sin(2.1f * x) * std::cos(1.7f * z);
}

/// Tile in object space: x, z in [-kTile/2, kTile/2], CCW seen from +y.
stream_test::SourceMesh makeTile() {
    stream_test::SourceMesh m;
    m.positions.reserve(static_cast<usize>(kGrid + 1u) * (kGrid + 1u) * 3u);
    m.indices.reserve(static_cast<usize>(kGrid) * kGrid * 6u);
    const f32 step = kTile / static_cast<f32>(kGrid);
    for (u32 j = 0; j <= kGrid; ++j) {
        for (u32 i = 0; i <= kGrid; ++i) {
            const f32 x = -0.5f * kTile + step * static_cast<f32>(i);
            const f32 z = -0.5f * kTile + step * static_cast<f32>(j);
            m.positions.insert(m.positions.end(), {x, height(x, z), z});
        }
    }
    for (u32 j = 0; j < kGrid; ++j) {
        for (u32 i = 0; i < kGrid; ++i) {
            const u32 a = j * (kGrid + 1u) + i;
            const u32 b = a + 1u;
            const u32 c = a + kGrid + 1u;
            const u32 d = c + 1u;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

void tileOffset(u32 t, f32 out[3]) {
    const u32 ix = t % kTilesX;
    const u32 iz = t / kTilesX;
    out[0] = (static_cast<f32>(ix) - 0.5f * static_cast<f32>(kTilesX) + 0.5f) * kTile;
    out[1] = 0.f;
    out[2] = -(static_cast<f32>(iz) + 0.5f) * kTile;
}

dag::cut_kernel::DagView viewFor(u32 tile) {
    f32 off[3];
    tileOffset(tile, off);
    const f32 cam[3] = {kEye[0] - off[0], kEye[1] - off[1], kEye[2] - off[2]};
    return dag::cut_kernel::make_dag_view(cam, 1.f / std::tan(0.5f * kFovY), static_cast<f32>(kHeight), 1.f, 0.1f);
}

using Zones = std::map<std::string, f64>;

// --- 1. GPU LOD selection --------------------------------------------------------------------------------
struct StreamRig {
    std::vector<std::unique_ptr<gsm::GeometryStreaming>> streams;
    rg::Graph graph;
    ~StreamRig() {
        for (auto& s : streams) {
            s->destroy();
        }
    }
};

bool streamFrame(HwContext& ctx, StreamRig& rig, Zones* zones, u32* uploads) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    u32 issued = 0;
    for (u32 t = 0; t < rig.streams.size(); ++t) {
        if (!rig.streams[t]->beginFrame(ctx.serial, viewFor(t), ctx.serial - 1u)) {
            return false;
        }
        issued += rig.streams[t]->stats().uploadsIssued;
    }
    ctx.upload.flush();
    rig.graph.reset();
    for (auto& s : rig.streams) {
        const gsm::StreamGraphRefs r = s->importInto(rig.graph);
        s->addFrame(rig.graph, r);
    }
    ctx.profiler->beginFrame();
    const rg::ExecuteResult result = ctx.executor->execute(rig.graph);
    ctx.profiler->endFrame();
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    ctx.bindless.collectRetired(ctx.serial);
    ctx.profiler->resolve();
    if (zones != nullptr) {
        const GpuFrameTimings& t = ctx.profiler->latest();
        if (!t.valid || t.skippedZones != 0u) {
            std::fprintf(stderr, "FAIL: profiler frame invalid (skipped %u)\n", t.skippedZones);
            return false;
        }
        *zones = zoneTimes(t);
    }
    if (uploads != nullptr) {
        *uploads = issued;
    }
    return result.ok && waited;
}

f64 sumPrefix(const Zones& z, const char* p) {
    f64 s = 0.0;
    for (const auto& [n, ms] : z) {
        s += n.rfind(p, 0) == 0 ? ms : 0.0;
    }
    return s;
}

// --- 2. drawing the cut ---------------------------------------------------------------------------------------
/// The fully-resident cut of `tile` decoded into a meshlet mesh (object space). Adds its cluster / triangle counts.
bool cutMesh(const dag::ClusterDagMesh& mesh, const geometry::DecodedVertices& dv, u32 tile, geometry::MeshletMesh& out,
             u64& clusters, u64& triangles) {
    std::vector<u32> ids;
    dag::select_dag_cut(mesh.dag, viewFor(tile), ids);
    std::vector<u32> remap(dv.positions.size() / 3u, ~0u);
    std::vector<f32> pos;
    std::vector<u32> idx;
    for (u32 id : ids) {
        const dag::ClusterRef r = dag::cluster_ref(mesh, id);
        for (u32 t = 0; t < r.record->triangle_count; ++t) {
            const u32 packed = r.triangles[t];
            const u32 local[3] = {packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu};
            for (u32 k = 0; k < 3u; ++k) {
                const u32 v = r.vertices[local[k]];
                if (remap[v] == ~0u) {
                    remap[v] = static_cast<u32>(pos.size() / 3u);
                    pos.insert(pos.end(), {dv.positions[v * 3u], dv.positions[v * 3u + 1u], dv.positions[v * 3u + 2u]});
                }
                idx.push_back(remap[v]);
            }
        }
    }
    clusters += ids.size();
    triangles += idx.size() / 3u;
    geometry::MeshletSource s{};
    s.positions = pos.data();
    s.vertex_count = static_cast<u32>(pos.size() / 3u);
    s.indices = idx.data();
    s.index_count = static_cast<u32>(idx.size());
    std::string error;
    if (!geometry::build_meshlets(s, geometry::MeshletBuildOptions{}, out, &error)) {
        std::fprintf(stderr, "build_meshlets (cut of tile %u): %s\n", tile, error.c_str());
        return false;
    }
    return true;
}

struct DrawRig {
    gpu_scene::GpuScene scene;
    std::vector<geometry::MeshletMesh> meshes;
    frame::FrameComposer composer;
    frame::FrameSettings settings;
    rg::Graph graph;
    ~DrawRig() {
        composer.destroy();
        scene.destroy();
    }
};

/// One composer frame; with `streams`, the 128 GeometryStreaming cuts are recorded into the same graph first (the
/// frame a page-aware draw path would run: cut, then cull + draw). This also keeps the GPU clocks up: the stream passes
/// alone are so small that the 3090 drops to idle clocks between frames (1.3 -> 10 ms swings).
bool drawFrame(HwContext& ctx, DrawRig& rig, u32 index, Zones* zones, StreamRig* streams = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (streams != nullptr) {
        for (u32 t = 0; t < streams->streams.size(); ++t) {
            if (!streams->streams[t]->beginFrame(ctx.serial, viewFor(t), ctx.serial - 1u)) {
                return false;
            }
        }
    }
    rig.scene.beginFrame(ctx.serial);
    rig.composer.beginSceneFrame(ctx.serial);
    const gpu_scene::GpuSceneCommitStats st = rig.scene.commit();
    rig.composer.commitScene();
    ctx.upload.flush();
    frame::FrameDesc fd{};
    fd.serial = ctx.serial;
    fd.frameIndex = index;
    for (u32 k = 0; k < 3u; ++k) {
        fd.camera.eye[k] = kEye[k];
        fd.camera.target[k] = kTarget[k];
    }
    fd.camera.fovY = kFovY;
    fd.camera.nearPlane = 0.1f;
    fd.camera.farPlane = 2000.f;
    fd.resetHistory = index == 0u;
    if (!rig.composer.beginFrame(fd, rig.settings)) {
        std::fprintf(stderr, "FAIL: FrameComposer::beginFrame\n");
        return false;
    }
    rig.graph.reset();
    if (streams != nullptr) {
        for (auto& st : streams->streams) {
            const gsm::StreamGraphRefs r = st->importInto(rig.graph);
            st->addFrame(rig.graph, r);
        }
    }
    rig.composer.addFrame(rig.graph);
    ctx.profiler->beginFrame();
    const rg::ExecuteResult result = ctx.executor->execute(rig.graph);
    ctx.profiler->endFrame();
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.composer.collectRetired(ctx.serial);
    rig.scene.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    ctx.profiler->resolve();
    if (zones != nullptr) {
        const GpuFrameTimings& t = ctx.profiler->latest();
        if (!t.valid) {
            return false;
        }
        *zones = zoneTimes(t);
    }
    return st.ok && result.ok && waited;
}

/// K trials x N frames of `frame(zones)`; per frame `metric(zones)`; median of the trial medians + min / max.
template <typename FrameFn, typename MetricFn>
bool timed(u32 warmup, u32 frames, u32 k, FrameFn&& frame, MetricFn&& metric, Stats& out, f64& lo, f64& hi) {
    std::vector<f64> trialMedians;
    for (u32 t = 0; t < k; ++t) {
        for (u32 i = 0; i < (t == 0u ? warmup : 8u); ++i) {
            if (!frame(nullptr)) {
                return false;
            }
        }
        std::vector<f64> v;
        for (u32 i = 0; i < frames; ++i) {
            Zones z;
            if (!frame(&z)) {
                return false;
            }
            v.push_back(metric(z));
        }
        trialMedians.push_back(stats(v).median);
    }
    out = stats(trialMedians);
    lo = out.min;
    hi = out.max;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    u32 frames = 48;
    u32 trials = 5;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            frames = static_cast<u32>(std::atoi(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--trials") == 0) {
            trials = static_cast<u32>(std::atoi(argv[i + 1]));
        }
    }
    HwContext ctx;
    HwSetupDesc sd{};
    sd.appName = "fuse_rp_hw_bench_dag";
    sd.maxZonesPerFrame = 4u * kTiles + 64u;
    if (const int rc = setup(ctx, sd); rc != 0) {
        return rc;
    }
    if (!gsm::queryStreamingCapabilities(ctx.device.get()).ok) {
        std::printf("SKIP: streaming: %s\n", gsm::queryStreamingCapabilities(ctx.device.get()).reason);
        return kSkip;
    }

    // --- asset ---------------------------------------------------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();
    const stream_test::SourceMesh tile = makeTile();
    stream_test::StreamAsset asset;
    std::string error;
    if (!stream_test::build_asset(tile, 32u * 1024u, asset, &error)) {
        std::fprintf(stderr, "FAIL: DAG / pages: %s\n", error.c_str());
        return 1;
    }
    const u64 tileTris = tile.indices.size() / 3u;
    const u64 sourceTris = tileTris * kTiles;
    std::printf("tile: %llu triangles -> DAG %u clusters (%u leaves), %zu groups, %u levels; %u pages (%u coarse) of 32 KiB; "
                "build %.1f s\n",
                static_cast<unsigned long long>(tileTris), asset.mesh.dag.cluster_count(), asset.mesh.dag.leaf_cluster_count,
                asset.mesh.dag.groups.size(), asset.mesh.dag.level_count, asset.pages.page_count(), asset.pages.coarse_page_count,
                secondsSince(t0));
    std::printf("scene: %u x %u tiles = %llu source triangles (%.1fM)\n", kTilesX, kTilesZ, static_cast<unsigned long long>(sourceTris),
                static_cast<f64>(sourceTris) * 1e-6);
    expect(sourceTris > 100000000ull, "scene has more than 100M source triangles");

    // --- 1. GPU cut ------------------------------------------------------------------------------------------
    Stats cut{};
    f64 cutLo = 0.0;
    f64 cutHi = 0.0;
    u32 residentPages = 0;
    StreamRig streamRig;
    {
        StreamRig& rig = streamRig;
        gsm::StreamerDesc streamer{};
        streamer.budget_pages = asset.pages.coarse_page_count + 192u;
        streamer.reserve_slots = 16u;
        streamer.max_loads_per_frame = 16u;
        for (u32 t = 0; t < kTiles; ++t) {
            auto s = std::make_unique<gsm::GeometryStreaming>();
            gsm::GeometryStreamingDesc d{};
            d.device = ctx.device.get();
            d.allocator = ctx.allocator.get();
            d.bindless = &ctx.bindless;
            d.upload = &ctx.upload;
            d.dag = &asset.mesh.dag;
            d.file = &asset.pages;
            d.streamer = streamer;
            d.framesInFlight = 3u;
            d.name = "stream";
            if (!s->init(d)) {
                std::printf("SKIP: GeometryStreaming::init failed (tile %u)\n", t);
                return kSkip;
            }
            rig.streams.push_back(std::move(s));
        }
        // settle the residency (loads stop or 400 frames)
        u32 settle = 0;
        for (; settle < 400u; ++settle) {
            u32 uploads = 0;
            if (!streamFrame(ctx, rig, nullptr, &uploads)) {
                std::fprintf(stderr, "FAIL: stream frame\n");
                return 1;
            }
            if (uploads == 0u && settle > 8u) {
                break;
            }
        }
        for (auto& s : rig.streams) {
            residentPages += s->streamer().stats().resident_pages;
        }
        std::printf("residency settled after %u frames: %u resident pages over %u instances (budget %u each)\n", settle, residentPages,
                    kTiles, streamer.budget_pages);
        // Standalone stream-only frames (diagnostic: the GPU drops to idle clocks between these tiny frames).
        Stats alone{};
        f64 aLo = 0.0;
        f64 aHi = 0.0;
        if (!timed(
                30u, frames, 1u, [&](Zones* z) { return streamFrame(ctx, rig, z, nullptr); },
                [](const Zones& z) { return sumPrefix(z, "stream."); }, alone, aLo, aHi)) {
            std::fprintf(stderr, "FAIL: stream frames\n");
            return 1;
        }
        std::printf("GPU LOD cut alone (stream-only frames, clocks may idle between them): %.3f ms\n", alone.median);
    }

    // --- 2. drawing the cut --------------------------------------------------------------------------------------
    t0 = std::chrono::steady_clock::now();
    geometry::DecodedVertices dv;
    geometry::decode_vertices(asset.mesh.base, dv);
    DrawRig rig;
    rig.meshes.resize(kTiles);
    u64 clusters = 0;
    u64 cutTris = 0;
    for (u32 t = 0; t < kTiles; ++t) {
        if (!cutMesh(asset.mesh, dv, t, rig.meshes[t], clusters, cutTris)) {
            return 1;
        }
    }
    std::printf("CPU cut (fully resident, same views): %llu clusters, %llu triangles (%.2f%% of the source); decode + meshlets %.1f s\n",
                static_cast<unsigned long long>(clusters), static_cast<unsigned long long>(cutTris),
                100.0 * static_cast<f64>(cutTris) / static_cast<f64>(sourceTris), secondsSince(t0));
    gpu_scene::GpuSceneDesc gd{};
    gd.device = ctx.device.get();
    gd.allocator = ctx.allocator.get();
    gd.upload = &ctx.upload;
    gd.bindless = &ctx.bindless;
    gd.instanceCapacity = kTiles + 8u;
    gd.meshCapacity = kTiles + 8u;
    if (!rig.scene.init(gd) || !rig.scene.gpuEnabled()) {
        std::fprintf(stderr, "FAIL: GpuScene\n");
        return 1;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
    Material m{};
    m.baseColor = fuse::math::Vec3{0.45f, 0.5f, 0.35f};
    m.roughness = 0.8f;
    rig.scene.setMaterial(0, m);
    for (u32 t = 0; t < kTiles; ++t) {
        if (rig.scene.addMeshletMesh(rig.meshes[t]) != t) {
            std::fprintf(stderr, "FAIL: addMeshletMesh\n");
            return 1;
        }
        // uploads are staged per mesh: flush every few tiles so the staging ring never overflows
        if (t % 8u == 7u) {
            rig.scene.commit();
            ctx.upload.flush();
            ctx.upload.waitAll();
            ++ctx.serial;
            ctx.bindless.setFrameSerial(ctx.serial);
            rig.scene.beginFrame(ctx.serial);
        }
        gpu_scene::InstanceDesc id{};
        id.mesh = t;
        id.material = 0;
        f32 off[3];
        tileOffset(t, off);
        id.transform.rows[0][0] = id.transform.rows[1][1] = id.transform.rows[2][2] = 1.f;
        id.transform.rows[0][3] = off[0];
        id.transform.rows[1][3] = off[1];
        id.transform.rows[2][3] = off[2];
        rig.scene.addInstance(id);
    }
    gpu_scene::GpuLight sun{};
    sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    sun.direction[0] = -0.4f;
    sun.direction[1] = -0.8f;
    sun.direction[2] = -0.3f;
    sun.intensity = 3.f;
    rig.scene.addLight(sun);
    const gpu_scene::GpuSceneCommitStats cs = rig.scene.commit();
    ctx.upload.flush();
    if (!cs.ok || !ctx.upload.waitAll()) {
        std::fprintf(stderr, "FAIL: scene commit (staging too small?)\n");
        return 1;
    }
    frame::FrameComposerDesc fd{};
    fd.device = ctx.device.get();
    fd.allocator = ctx.allocator.get();
    fd.bindless = &ctx.bindless;
    fd.upload = &ctx.upload;
    fd.scene = &rig.scene;
    fd.tier = frame::FrameTier::T0;
    fd.renderWidth = kWidth;
    fd.renderHeight = kHeight;
    fd.displayWidth = kWidth;
    fd.displayHeight = kHeight;
    fd.instanceCapacity = kTiles + 8u;
    fd.meshCapacity = kTiles + 8u;
    fd.lightCapacity = 8u;
    fd.frameGen = false;
    if (!rig.composer.init(fd)) {
        std::printf("SKIP: FrameComposer::init: %s\n", rig.composer.reason());
        return kSkip;
    }
    frame::FrameSettings& s = rig.settings;
    s.vsm = false;
    s.ddgi = false;
    s.ddgiAtmosphereSky = false;
    s.ssfx = false;
    s.sky = false;
    s.aerialPerspective = false;
    s.fog = false;
    s.clouds = false;
    s.forward = false;
    s.post = false;
    s.upscaler = frame::FrameUpscaler::None;
    s.ui = false;
    u32 index = 0;
    auto draw = [&](Zones* z) { return drawFrame(ctx, rig, index++, z, &streamRig); };
    if (!timed(60u, frames, trials, draw, [](const Zones& z) { return sumPrefix(z, "stream."); }, cut, cutLo, cutHi)) {
        std::fprintf(stderr, "FAIL: cut + draw frames\n");
        return 1;
    }
    auto cullVis = [](const Zones& z) { return sumPrefix(z, "gpu_scene.") + sumPrefix(z, "cull.") + sumPrefix(z, "vis."); };
    auto resolve = [](const Zones& z) { return sumPrefix(z, "resolve.") + sumPrefix(z, "temporal.motion"); };
    Stats cv{};
    Stats rs{};
    f64 cvLo = 0.0;
    f64 cvHi = 0.0;
    f64 rsLo = 0.0;
    f64 rsHi = 0.0;
    if (!timed(8u, frames, trials, draw, cullVis, cv, cvLo, cvHi) || !timed(8u, frames, trials, draw, resolve, rs, rsLo, rsHi)) {
        std::fprintf(stderr, "FAIL: draw frames\n");
        return 1;
    }
    {
        Zones z;
        draw(&z);
        std::printf("passes of one frame (stream.* summed over the 128 instances):\n");
        for (const auto& [n, ms] : z) {
            std::printf("    %-24s %8.3f ms\n", n.c_str(), ms);
        }
    }
    const f64 total = cut.median + cv.median;
    std::printf("\nP5, %.1fM source triangles, 2560x1440:\n", static_cast<f64>(sourceTris) * 1e-6);
    std::printf("  GPU LOD cut (128 instances)          %8.3f ms [%.3f..%.3f]\n", cut.median, cutLo, cutHi);
    std::printf("  cull + visibility buffer of the cut  %8.3f ms [%.3f..%.3f]  (%llu triangles)\n", cv.median, cvLo, cvHi,
                static_cast<unsigned long long>(cutTris));
    std::printf("  = culling and visibility buffer      %8.3f ms (plan budget 2.0 ms) %s\n", total, total <= 2.0 ? "within" : "OVER");
    std::printf("  material resolve of the cut          %8.3f ms [%.3f..%.3f] (plan budget 1.5 ms)\n", rs.median, rsLo, rsHi);
    expect(cutTris > 0u && cutTris < sourceTris / 4u, "the cut is a small fraction of the source");
    expect(total <= 2.0, "P5: >100M-triangle scene within the 2.0 ms culling + visibility budget");
    return g_failures == 0 ? 0 : 1;
}

#endif // FUSE_VULKAN_BACKEND
