// Renderer hardware exit criterion P7 (docs/unification/RENDERER-HW-RUN.md): "10k+ emissive lights with stable,
// low-noise output after denoising", measured on the device against a 4096-spp f64 reference.
//
// Scene: tests/test_rp_restir_scene.hpp (ground + 4 boxes as WP-1.1 GPU-scene meshlet meshes / WP-6.0 TLAS, lit by a
// ceiling panel of 10 000 emissive triangles in the WP-7.1 light tree, 1 % hot spots x 40), static camera. Every
// frame is one render graph: gpu_scene.* -> cull / vis -> resolve (WP-1.5 G-buffer) -> restir.* (WP-7.2, DI chain
// only) [-> WP-6.4 SvgfDenoiser on the DI signal] -> readback.
//
//   quality (default 320 x 240, --width / --height)
//     reference      RestirReference::direct (f64, exact analytic tracer), --ref-spp (4096) samples per pixel at the
//                    GPU's own read-back visible points (surfPos / surfNormal of the RestirGpu state buffer)
//     (a) RIS-only   diCandidates light-tree candidates + visibility reuse, no temporal / spatial reuse, history
//                    reset every frame (independent frames, seeds from the frame index)
//     (b) ReSTIR DI  temporal + spatial reuse (production defaults; biased 1 / M and unbiased Talbot MIS), measured
//                    after 32 frames of convergence with the static camera
//     (c) (b) + SVGF the WP-6.4 denoiser on (b)'s DI signal (GI preset, zero motion, ReSTIR depth / normals)
//     metrics        relMSE = mean over valid pixels of (L - Lref)^2 / (Lref^2 + eps), eps = (0.1 mean Lref)^2,
//                    L = Rec.709 luminance of the demodulated DI signal (averaged over the measured frames);
//                    bias = mean(L) / mean(Lref) - 1; temporal change = sum |L_t - L_t-1| / sum Lref over the last
//                    8 frame pairs; RIS-only frame averages (N = 1 .. 64) for the equal-quality frame count
//   timing (2560 x 1440, --time-width / --time-height; --no-timing skips)
//                    GpuProfiler zone per render-graph pass: sum of the restir.* zones and of the denoiser zones per
//                    frame, 30 warm-up frames then --trials x --frames frames, median / p10 / p90 per trial
//
// Gates (thresholds from RTX 3090 measurements with margin, see the summary print): every value finite; unbiased
// ReSTIR relMSE at least kRestirGain x lower than RIS-only (measured ~12x), biased at least kBiasedGain x (measured
// ~3x: dominated by its ~-6 % bias); denoised relMSE below the raw ReSTIR relMSE; |bias| of unbiased ReSTIR (+ SVGF)
// and of RIS-only within kUnbiasedBias (measured < 0.6 %), of the biased mode within kBiasedBias; the denoised
// frame-to-frame change below the raw ReSTIR change and below kStableChange (measured ~0.4 %).
//
// Exit 0 pass, 1 failure, 77 skip (stub build, no discrete GPU, device below the T2 ray-query gate, no kernels).
#include "test_rp_hw_bench_common.hpp"

#include <cstdio>

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return 77;
}

#else

#include "test_rp_restir_scene.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/renderer/denoise/svgf_reference.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/restir/restir_gpu.hpp>
#include <fuse/renderer/restir/restir_reference.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::culling;
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::restir;
using namespace fuse::renderer::rt;
using namespace fuse::renderer::visbuffer;
using hw_bench::expect;
using hw_bench::f32;
using hw_bench::f64;
using hw_bench::u32;
using hw_bench::u64;
using hw_bench::u8;
using hw_bench::usize;
using vsmr_test::Camera;

constexpr u32 kPanelX = 50; // 50 x 100 cells -> 10 000 emissive triangles
constexpr u32 kPanelZ = 100;
constexpr u32 kRisFrames = 64;
constexpr u32 kWarmFrames = 32;
constexpr u32 kMeasureFrames = 16;
constexpr u32 kStableFrames = 8;
// Gates (see the header and the summary print).
constexpr f64 kRestirGain = 5.0;       ///< relMSE(RIS-only) / relMSE(unbiased ReSTIR) at least this
constexpr f64 kBiasedGain = 2.0;       ///< relMSE(RIS-only) / relMSE(biased ReSTIR): its relMSE is bias-dominated
constexpr f64 kUnbiasedBias = 0.02;    ///< |mean / mean_ref - 1| of unbiased ReSTIR (+ SVGF) and RIS-only
constexpr f64 kBiasedBias = 0.10;      ///< |bias| of the biased 1 / M mode (+ SVGF): it darkens (visibility in reuse)
constexpr f64 kStableChange = 0.02;    ///< denoised mean relative frame-to-frame luminance change

f64 lum(const f32* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

struct Args {
    u32 width = 320;
    u32 height = 240;
    u32 refSpp = 4096;
    u32 threads = 0;
    u32 timeWidth = 2560;
    u32 timeHeight = 1440;
    u32 frames = 100;
    u32 trials = 3;
    bool timing = true;
};

struct Bench {
    hw_bench::HwContext ctx;
    BindlessSlotHandle sampler{};
    u32 samplerHandle = 0;
    Buffer motion{};
    Buffer readback{};

    ~Bench() {
        if (ctx.vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(ctx.vkDevice);
        }
        if (ctx.allocator != nullptr) {
            ctx.allocator->destroyBuffer(motion);
            ctx.allocator->destroyBuffer(readback);
        }
        if (sampler.isValid()) {
            ctx.bindless.releaseSampler(sampler);
        }
    }
};

bool hostBuffer(Bench& b, Buffer& buf, usize bytes, MemoryUsage memory, const char* name) {
    if (buf.handle != nullptr && buf.desc.size >= bytes) {
        return true;
    }
    if (buf.handle != nullptr) {
        b.ctx.allocator->destroyBuffer(buf);
    }
    BufferDesc d{};
    d.size = bytes;
    d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                       static_cast<u32>(BufferUsage::TransferDst));
    d.memoryUsage = memory;
    d.name = name;
    return b.ctx.allocator->createBuffer(d, buf) && buf.mapped != nullptr && buf.deviceAddress != 0u;
}

// --- scene (test_rp_restir.cpp) ---------------------------------------------------------------------------
struct GpuWorld {
    restir_test::Scene cpu;
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<Material::GPUMaterial> materials;
};

Material::GPUMaterial material(f32 r, f32 g, f32 b) {
    Material::GPUMaterial m{};
    m.baseColor = {r, g, b, 0.f};
    m.roughnessEmissive = {0.8f, 0.f, 0.f, 0.f};
    return m;
}

bool buildWorld(Bench& b, GpuWorld& w, u32 width, u32 height) {
    hw_bench::HwContext& ctx = b.ctx;
    if (!restir_test::makeScene(w.cpu, width, height, kPanelX, kPanelZ)) {
        return false;
    }
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 16;
    if (!w.gpu.init(d) || !w.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    w.gpu.beginFrame(ctx.serial);
    if (!vsmr_test::buildMeshes(w.meshes, restir_test::kGroundSize)) {
        return false;
    }
    for (u32 i = 0; i < w.meshes.size(); ++i) {
        if (w.gpu.addMeshletMesh(w.meshes[i]) != i) {
            return false;
        }
    }
    const u32 rows = 2u * (1u + static_cast<u32>(w.cpu.world.boxes.size()));
    w.materials.resize(rows);
    w.materials[0] = material(restir_test::kGroundAlbedo[0], restir_test::kGroundAlbedo[1], restir_test::kGroundAlbedo[2]);
    for (u32 i = 0; i < w.cpu.world.boxes.size(); ++i) {
        w.materials[2u + 2u * i] = material(w.cpu.boxAlbedo[i][0], w.cpu.boxAlbedo[i][1], w.cpu.boxAlbedo[i][2]);
    }
    for (u32 i = 0; i < rows; i += 2u) {
        w.materials[i + 1u] = w.materials[i];
    }
    for (u32 i = 0; i < rows; ++i) {
        w.gpu.setMaterial(i, w.materials[i]);
    }
    InstanceDesc ground{};
    ground.mesh = 0;
    ground.material = 0;
    if (!w.gpu.addInstance(ground).valid()) {
        return false;
    }
    for (u32 i = 0; i < w.cpu.world.boxes.size(); ++i) {
        InstanceDesc id{};
        id.mesh = 1;
        id.material = 2u + 2u * i;
        id.transform = vsmr_test::World::boxTransform(w.cpu.world.boxes[i]);
        if (!w.gpu.addInstance(id).valid()) {
            return false;
        }
    }
    const GpuSceneCommitStats stats = w.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

struct Rig {
    u32 width = 0;
    u32 height = 0;
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    AccelerationStructures as;
    light_tree::LightTreeGpu tree;
    RestirGpu restir;
    denoise::SvgfDenoiser dn;
    bool denoiser = false;
};

/// 1 ok, 0 unavailable (skip), -1 failure.
int initRig(Bench& b, GpuWorld& w, Rig& rig, u32 width, u32 height) {
    hw_bench::HwContext& ctx = b.ctx;
    rig.width = width;
    rig.height = height;
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 16;
    if (!rig.culler.init(cd) || !rig.culler.setResolution(width, height)) {
        return -1;
    }
    VisBufferDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.width = width;
    vd.height = height;
    vd.mode = VisMode::Raster;
    if (!rig.vb.init(vd)) {
        return -1;
    }
    MaterialResolveDesc md{};
    md.device = ctx.device.get();
    md.allocator = ctx.allocator.get();
    md.bindless = &ctx.bindless;
    md.width = width;
    md.height = height;
    if (!rig.resolve.init(md)) {
        return -1;
    }
    AccelerationStructuresDesc ad{};
    ad.device = ctx.device.get();
    ad.allocator = ctx.allocator.get();
    ad.upload = &ctx.upload;
    ad.scene = &w.gpu;
    ad.instanceCapacity = 16;
    ad.meshCapacity = 4;
    if (!rig.as.init(ad)) {
        std::printf("  acceleration structures: %s\n", rig.as.reason());
        return 0;
    }
    light_tree::LightTreeGpuDesc td{};
    td.device = ctx.device.get();
    td.allocator = ctx.allocator.get();
    td.bindless = &ctx.bindless;
    td.initialLights = static_cast<u32>(w.cpu.lights.size());
    if (!rig.tree.init(td)) {
        std::printf("  light tree GPU unavailable\n");
        return 0;
    }
    RestirGpuDesc rd{};
    rd.device = ctx.device.get();
    rd.allocator = ctx.allocator.get();
    rd.bindless = &ctx.bindless;
    rd.width = width;
    rd.height = height;
    rd.initialLights = static_cast<u32>(w.cpu.table.size());
    rd.language = RestirKernelLanguage::Auto;
    if (!rig.restir.init(rd)) {
        std::printf("  ReSTIR kernels unavailable: %s\n", rig.restir.reason());
        return 0;
    }
    denoise::SvgfDenoiserDesc dd{};
    dd.device = ctx.device.get();
    dd.allocator = ctx.allocator.get();
    dd.bindless = &ctx.bindless;
    if (!rig.dn.init(dd)) {
        std::printf("  denoiser unavailable\n");
        return 0;
    }
    rig.dn.setSettings(denoise::svgf_preset(denoise::DenoiseSignal::Gi));
    rig.denoiser = true;
    const usize pixels = static_cast<usize>(width) * height;
    if (!hostBuffer(b, b.motion, pixels * 8u, MemoryUsage::CpuToGpu, "hw_restir.motion")) {
        return -1;
    }
    std::memset(b.motion.mapped, 0, pixels * 8u);
    return 1;
}

void destroyRig(Rig& rig) {
    rig.dn.destroy();
    rig.restir.destroy();
    rig.tree.destroy();
    rig.as.destroy();
    rig.resolve.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
}

// --- per-frame graph -------------------------------------------------------------------------------------
struct CopyRecord {
    rg::BufferRef src;
    rg::BufferRef dst;
    u64 srcOffset = 0;
    u64 dstOffset = 0;
    u64 bytes = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const VkBufferCopy region{c.srcOffset, c.dstOffset, c.bytes};
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.src)), static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

/// Readback sections (pixels x 16 bytes each, 256-aligned): DI signal, denoised output, surface position, normal.
enum Section : u32 { kSecDi = 0, kSecDenoised, kSecPos, kSecNormal, kSecCount };

struct FrameOptions {
    u32 frameIndex = 0;
    bool reset = false;      ///< no temporal reuse this frame (RIS-only: every frame), denoiser history reset
    bool denoise = false;
    bool readback = false;
    bool surfaces = false;   ///< also read the visible points
};

u64 sectionBytes(const Rig& rig) { return (static_cast<u64>(rig.width) * rig.height * 16u + 255u) & ~u64{255u}; }

/// One frame; returns false on any failure. GPU timings: ctx.profiler->latest() afterwards.
bool renderFrame(Bench& b, GpuWorld& w, Rig& rig, rg::Graph& graph, const Camera& cam, const FrameOptions& o) {
    hw_bench::HwContext& ctx = b.ctx;
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    w.gpu.beginFrame(ctx.serial);
    rig.as.beginFrame(ctx.serial);
    const GpuSceneCommitStats stats = w.gpu.commit();
    const RtCommitStats rtStats = rig.as.commit();
    ctx.upload.flush();

    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, cam.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = w.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, w.gpu.headerHandle(), w.gpu.instanceHighWater()) && ok;
    ResolveFrameDesc rf{};
    std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
    std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
    rf.scene = w.gpu.headerHandle();
    rf.vis = rig.vb.visStorageHandle();
    rf.sampler = b.samplerHandle;
    ok = rig.resolve.beginFrame(ctx.serial, rf) && ok;
    ok = rig.tree.beginFrame(ctx.serial, w.cpu.tree) && ok;
    RestirFrameDesc desc{};
    desc.camera = restir_test::restirCamera(cam);
    desc.gbuffer = &rig.resolve;
    desc.tlasAddress = rig.as.tlasAddress();
    desc.lightTreeHeader = rig.tree.headerAddress();
    desc.sceneHeader = w.gpu.headerAddress();
    desc.lights = w.cpu.table.data();
    desc.lightCount = static_cast<u32>(w.cpu.table.size());
    desc.lightsVersion = w.cpu.tree.version();
    desc.motion = 0; // static camera: zero motion
    desc.frameIndex = o.frameIndex;
    desc.seed = 0xBEEFu;
    desc.reset = o.reset;
    ok = rig.restir.beginFrame(ctx.serial, desc) && ok;
    if (o.denoise) {
        denoise::DenoiseFrameDesc df{};
        df.width = rig.width;
        df.height = rig.height;
        df.signal = rig.restir.diSignalAddress();
        df.motion = b.motion.deviceAddress;
        df.depth = rig.restir.depthAddress();
        df.normal = rig.restir.normalAddress();
        df.reset = o.reset;
        ok = ok && rig.dn.beginFrame(ctx.serial, df);
    }
    if (!ok) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }

    graph.reset();
    const GpuSceneGraphRefs sceneRefs = w.gpu.importInto(graph);
    const RtGraphRefs rtRefs = rig.as.importInto(graph, sceneRefs);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, w.gpu.headerHandle(), rig.culler, cull);
    const ResolveGraphRefs gbuffer = rig.resolve.importInto(graph);
    rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
    const light_tree::LightTreeGraphRefs treeRefs = rig.tree.importInto(graph);
    const RestirGraphRefs refs = rig.restir.importInto(graph);
    RestirGraphInputs in{};
    in.tlas = rtRefs.tlas;
    in.lightTree = treeRefs;
    in.gbuffer = &gbuffer;
    in.scene = sceneRefs;
    if (!rig.restir.addPasses(graph, refs, in)) {
        std::fprintf(stderr, "  RestirGpu::addPasses failed\n");
        return false;
    }
    denoise::DenoiseGraphRefs dref{};
    if (o.denoise) {
        const rg::BufferRef motion =
            graph.importBuffer(rg::ImportedBuffer{b.motion.handle, b.motion.desc.size, rg::kNoQueue, nullptr, "hw_restir.motion"});
        dref = rig.dn.importInto(graph);
        denoise::DenoiseGraphInputs din{};
        din.signal = refs.output;
        din.motion = motion;
        din.depth = refs.output;
        din.normal = refs.state;
        rig.dn.addPasses(graph, dref, din);
    }
    CopyRecord copies[kSecCount]{};
    if (o.readback) {
        const u64 sec = sectionBytes(rig);
        const u64 bytes = static_cast<u64>(rig.width) * rig.height * 16u;
        const rg::BufferRef rb =
            graph.importBuffer(rg::ImportedBuffer{b.readback.handle, b.readback.desc.size, rg::kNoQueue, nullptr, "hw_restir.readback"});
        const RestirBufferLayout& l = rig.restir.layout();
        auto addCopy = [&](Section s, rg::BufferRef src, u64 srcOffset) {
            copies[s] = CopyRecord{src, rb, srcOffset, sec * s, bytes};
            graph.addPass("readback.copy", &recordCopy, &copies[s])
                .use(src, rg::Access::TransferSrc, rg::BufferRange{srcOffset, bytes})
                .use(rb, rg::Access::TransferDst, rg::BufferRange{sec * s, bytes});
        };
        addCopy(kSecDi, refs.output, l.diSignal);
        if (o.denoise) {
            addCopy(kSecDenoised, dref.output, 0u);
        }
        if (o.surfaces) {
            addCopy(kSecPos, refs.state, l.surfPos[rig.restir.slot()]);
            addCopy(kSecNormal, refs.state, l.surfNormal[rig.restir.slot()]);
        }
        graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
    } else {
        // Where a consumer (lighting composite) would read the signals.
        rg::PassBuilder use = graph.addPass("consume.di", nullptr, nullptr);
        use.neverCull();
        use.use(refs.output, rg::Access::StorageRead, {}, rg::kStageCompute);
        if (o.denoise) {
            rg::PassBuilder dn = graph.addPass("consume.denoised", nullptr, nullptr);
            dn.neverCull();
            dn.use(dref.output, rg::Access::StorageRead, {}, rg::kStageCompute);
        }
    }
    ctx.profiler->beginFrame();
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    ctx.profiler->endFrame();
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    ctx.profiler->resolve();
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.resolve.collectRetired(ctx.serial);
    rig.as.collectRetired(ctx.serial);
    rig.tree.collectRetired(ctx.serial);
    rig.restir.collectRetired(ctx.serial);
    rig.dn.collectRetired(ctx.serial);
    w.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return stats.ok && rtStats.ok && result.ok && waited;
}

const f32* section(const Bench& b, const Rig& rig, Section s) {
    return reinterpret_cast<const f32*>(static_cast<const u8*>(b.readback.mapped) + sectionBytes(rig) * s);
}

// --- reference ---------------------------------------------------------------------------------------------
struct Reference {
    std::vector<u8> valid;
    std::vector<f64> lum;
    std::vector<f64> se;
    usize validCount = 0;
    f64 mean = 0.0;
    f64 eps = 0.0;      ///< relMSE denominator offset
    f64 floor = 0.0;    ///< mean se^2 / (ref^2 + eps): the reference's own contribution to every relMSE
    f64 seconds = 0.0;
    u32 threads = 0;
};

void computeReference(const GpuWorld& w, const RestirSettings& settings, const std::vector<f32>& pos, const std::vector<f32>& nrm,
                      u32 spp, u32 threads, Reference& ref) {
    const usize n = pos.size() / 4u;
    ref.valid.assign(n, 0u);
    ref.lum.assign(n, 0.0);
    ref.se.assign(n, 0.0);
    RestirReference reference;
    expect(reference.build(w.cpu.tree.view(), w.cpu.table.data(), static_cast<u32>(w.cpu.table.size())), "reference build");
    const restir_test::Tracer tracer(w.cpu);
    RestirRefParams params{};
    params.normalBias = settings.normalBias;
    params.viewBias = settings.viewBias;
    params.camera[0] = static_cast<f32>(w.cpu.camera.eye.x);
    params.camera[1] = static_cast<f32>(w.cpu.camera.eye.y);
    params.camera[2] = static_cast<f32>(w.cpu.camera.eye.z);
    params.giRayTMin = settings.giRayTMin;
    params.farDistance = settings.farDistance;
    std::atomic<usize> next{0};
    const auto t0 = std::chrono::steady_clock::now();
    auto worker = [&]() {
        for (;;) {
            const usize begin = next.fetch_add(256u);
            if (begin >= n) {
                return;
            }
            const usize end = std::min(n, begin + 256u);
            for (usize p = begin; p < end; ++p) {
                if (!(pos[p * 4u + 3u] > 0.f)) {
                    continue;
                }
                RestirRefSurface s{};
                s.valid = true;
                f64 len = 0.0;
                for (u32 k = 0; k < 3u; ++k) {
                    s.p[k] = pos[p * 4u + k];
                    s.n[k] = nrm[p * 4u + k];
                    len += s.n[k] * s.n[k];
                }
                len = std::sqrt(len);
                if (!(len > 0.5)) {
                    continue;
                }
                for (u32 k = 0; k < 3u; ++k) {
                    s.n[k] /= len;
                }
                const RestirRefEstimate e = reference.direct(s, tracer, params, spp, 1000u + p);
                ref.valid[p] = 1u;
                ref.lum[p] = e.luminance;
                ref.se[p] = e.stdError;
            }
        }
    };
    std::vector<std::thread> pool;
    for (u32 i = 0; i < threads; ++i) {
        pool.emplace_back(worker);
    }
    for (std::thread& t : pool) {
        t.join();
    }
    ref.seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
    ref.threads = threads;
    f64 sum = 0.0;
    ref.validCount = 0;
    for (usize p = 0; p < n; ++p) {
        if (ref.valid[p] != 0u) {
            sum += ref.lum[p];
            ++ref.validCount;
        }
    }
    ref.mean = ref.validCount > 0u ? sum / static_cast<f64>(ref.validCount) : 0.0;
    ref.eps = 0.01 * ref.mean * ref.mean;
    f64 fl = 0.0;
    for (usize p = 0; p < n; ++p) {
        if (ref.valid[p] != 0u) {
            fl += ref.se[p] * ref.se[p] / (ref.lum[p] * ref.lum[p] + ref.eps);
        }
    }
    ref.floor = ref.validCount > 0u ? fl / static_cast<f64>(ref.validCount) : 0.0;
}

// --- metrics -----------------------------------------------------------------------------------------------
struct ImageMetrics {
    f64 relMse = 0.0;
    f64 bias = 0.0;
    bool finite = true;
};

ImageMetrics measure(const Reference& ref, const std::vector<f64>& l) {
    ImageMetrics m{};
    f64 err = 0.0;
    f64 sum = 0.0;
    for (usize p = 0; p < l.size(); ++p) {
        if (ref.valid[p] == 0u) {
            continue;
        }
        if (!std::isfinite(l[p])) {
            m.finite = false;
            continue;
        }
        const f64 d = l[p] - ref.lum[p];
        err += d * d / (ref.lum[p] * ref.lum[p] + ref.eps);
        sum += l[p];
    }
    const f64 cnt = static_cast<f64>(std::max<usize>(ref.validCount, 1u));
    m.relMse = err / cnt;
    m.bias = ref.mean > 0.0 ? (sum / cnt) / ref.mean - 1.0 : 0.0;
    return m;
}

/// sum |a - b| / sum ref over valid pixels.
f64 change(const Reference& ref, const std::vector<f64>& a, const std::vector<f64>& b) {
    f64 d = 0.0;
    f64 s = 0.0;
    for (usize p = 0; p < a.size(); ++p) {
        if (ref.valid[p] != 0u) {
            d += std::fabs(a[p] - b[p]);
            s += ref.lum[p];
        }
    }
    return s > 0.0 ? d / s : 0.0;
}

void lumImage(const f32* rgba, usize pixels, std::vector<f64>& out) {
    out.resize(pixels);
    for (usize p = 0; p < pixels; ++p) {
        out[p] = lum(rgba + p * 4u);
    }
}

struct ConfigResult {
    const char* name = "";
    f64 relMse = 0.0;       ///< mean over the measured frames of the per-frame relMSE
    f64 relMseMin = 0.0;
    f64 relMseMax = 0.0;
    f64 bias = 0.0;         ///< mean over the measured frames
    f64 change = -1.0;      ///< mean relative frame-to-frame change (last kStableFrames pairs), -1 n/a
    bool finite = true;
    bool ok = true;
};

void accumulate(ConfigResult& r, const std::vector<ImageMetrics>& frames) {
    r.relMse = 0.0;
    r.bias = 0.0;
    r.relMseMin = 1e300;
    r.relMseMax = 0.0;
    for (const ImageMetrics& m : frames) {
        r.relMse += m.relMse;
        r.bias += m.bias;
        r.relMseMin = std::min(r.relMseMin, m.relMse);
        r.relMseMax = std::max(r.relMseMax, m.relMse);
        r.finite = r.finite && m.finite && std::isfinite(m.relMse) && std::isfinite(m.bias);
    }
    const f64 n = static_cast<f64>(std::max<usize>(frames.size(), 1u));
    r.relMse /= n;
    r.bias /= n;
}

f64 meanChange(const Reference& ref, const std::vector<std::vector<f64>>& imgs) {
    if (imgs.size() < 2u) {
        return -1.0;
    }
    const usize first = imgs.size() > kStableFrames ? imgs.size() - kStableFrames - 1u : 0u;
    f64 c = 0.0;
    u32 pairs = 0;
    for (usize i = first + 1u; i < imgs.size(); ++i) {
        c += change(ref, imgs[i], imgs[i - 1u]);
        ++pairs;
    }
    return c / static_cast<f64>(pairs);
}

RestirSettings diOnly(bool unbiased) {
    RestirSettings s{}; // production defaults: 8 candidates, 1 spatial iteration x 3 neighbours, radius 16, M cap 20
    s.gi = false;
    s.giTemporal = false;
    s.unbiased = unbiased;
    return s;
}

RestirSettings risOnly() {
    RestirSettings s = diOnly(true);
    s.diTemporal = false;
    s.diSpatialIterations = 0;
    return s;
}

// --- timing ------------------------------------------------------------------------------------------------
struct TimingResult {
    const char* name = "";
    std::vector<hw_bench::Stats> restir; ///< per trial
    std::vector<hw_bench::Stats> denoise;
};

bool isDenoiseZone(const std::string& n) {
    return n.rfind("svgf", 0) == 0 || n.rfind("denoise", 0) == 0 || n.find("svgf") != std::string::npos;
}

int runTiming(Bench& b, const Args& args, std::vector<TimingResult>& out) {
    GpuWorld w;
    if (!buildWorld(b, w, args.timeWidth, args.timeHeight)) {
        std::fprintf(stderr, "FAIL: timing scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(b, w, rig, args.timeWidth, args.timeHeight);
    if (rc <= 0) {
        destroyRig(rig);
        w.gpu.destroy();
        return rc < 0 ? 1 : hw_bench::kSkip;
    }
    rg::Graph graph;
    const Camera cam = w.cpu.camera;
    struct Cfg {
        const char* name;
        RestirSettings s;
        bool reset;
        bool denoise;
    };
    const Cfg cfgs[] = {
        {"RIS-only", risOnly(), true, false},
        {"ReSTIR biased + SVGF", diOnly(false), false, true},
        {"ReSTIR unbiased + SVGF", diOnly(true), false, true},
    };
    bool printed = false;
    bool ok = true;
    u32 frameIndex = 100000u;
    for (const Cfg& c : cfgs) {
        TimingResult r{};
        r.name = c.name;
        rig.restir.setSettings(c.s);
        for (u32 i = 0; i < 30u && ok; ++i) {
            FrameOptions o{};
            o.frameIndex = frameIndex++;
            o.reset = c.reset || i == 0u;
            o.denoise = c.denoise;
            ok = renderFrame(b, w, rig, graph, cam, o);
        }
        for (u32 t = 0; t < args.trials && ok; ++t) {
            std::vector<f64> rs;
            std::vector<f64> ds;
            for (u32 i = 0; i < args.frames && ok; ++i) {
                FrameOptions o{};
                o.frameIndex = frameIndex++;
                o.reset = c.reset;
                o.denoise = c.denoise;
                ok = renderFrame(b, w, rig, graph, cam, o);
                const GpuFrameTimings& ft = b.ctx.profiler->latest();
                if (!ft.valid) {
                    continue;
                }
                const std::map<std::string, f64> z = hw_bench::zoneTimes(ft);
                if (!printed && c.denoise) {
                    printed = true;
                    std::printf("  zones:");
                    for (const auto& [name, ms] : z) {
                        std::printf(" %s=%.3f", name.c_str(), ms);
                    }
                    std::printf("\n");
                }
                f64 restirMs = 0.0;
                f64 dnMs = 0.0;
                for (const auto& [name, ms] : z) {
                    if (name.rfind("restir.", 0) == 0) {
                        restirMs += ms;
                    } else if (isDenoiseZone(name)) {
                        dnMs += ms;
                    }
                }
                rs.push_back(restirMs);
                ds.push_back(dnMs);
            }
            r.restir.push_back(hw_bench::stats(rs));
            r.denoise.push_back(hw_bench::stats(ds));
            std::printf("  %-24s trial %u: restir.* median %.3f ms (p10 %.3f, p90 %.3f, n %zu)", c.name, t, r.restir.back().median,
                        r.restir.back().p10, r.restir.back().p90, r.restir.back().count);
            if (c.denoise) {
                std::printf("; denoiser median %.3f ms (p10 %.3f, p90 %.3f)", r.denoise.back().median, r.denoise.back().p10,
                            r.denoise.back().p90);
            }
            std::printf("\n");
        }
        out.push_back(r);
    }
    expect(ok, "timing frames ok");
    destroyRig(rig);
    w.gpu.destroy();
    return ok ? 0 : 1;
}

// --- quality -----------------------------------------------------------------------------------------------
int runQuality(Bench& b, const Args& args, std::vector<ConfigResult>& results, Reference& ref, std::vector<f64>& risCurve) {
    GpuWorld w;
    if (!buildWorld(b, w, args.width, args.height)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    std::printf("scene: %zu emissive triangles in the light tree, %u x %u\n", w.cpu.table.size(), args.width, args.height);
    Rig rig;
    const int rc = initRig(b, w, rig, args.width, args.height);
    if (rc <= 0) {
        destroyRig(rig);
        w.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: ReSTIR / denoiser unavailable\n");
        return rc < 0 ? 1 : hw_bench::kSkip;
    }
    std::printf("ReSTIR kernels: %s\n", rig.restir.kernelLanguage());
    const usize pixels = static_cast<usize>(args.width) * args.height;
    if (!hostBuffer(b, b.readback, static_cast<usize>(sectionBytes(rig) * kSecCount), MemoryUsage::GpuToCpu, "hw_restir.readback")) {
        return 1;
    }
    rg::Graph graph;
    const Camera cam = w.cpu.camera;
    u32 frameIndex = 1u;
    bool ok = true;

    // Visible points: one frame, read back the GPU's surfaces (the reference is evaluated exactly there).
    rig.restir.setSettings(risOnly());
    {
        FrameOptions o{};
        o.frameIndex = frameIndex++;
        o.reset = true;
        o.readback = true;
        o.surfaces = true;
        ok = renderFrame(b, w, rig, graph, cam, o);
    }
    if (!ok) {
        std::fprintf(stderr, "FAIL: first frame\n");
        destroyRig(rig);
        w.gpu.destroy();
        return 1;
    }
    const std::vector<f32> pos(section(b, rig, kSecPos), section(b, rig, kSecPos) + pixels * 4u);
    const std::vector<f32> nrm(section(b, rig, kSecNormal), section(b, rig, kSecNormal) + pixels * 4u);
    const u32 threads = args.threads != 0u ? args.threads : std::max(1u, std::thread::hardware_concurrency());
    std::printf("reference: %u spp per pixel, %u threads ...\n", args.refSpp, threads);
    computeReference(w, diOnly(true), pos, nrm, args.refSpp, threads, ref);
    std::printf("reference: %zu valid pixels of %zu, mean luminance %.5f, %.1f s CPU wall (%u threads), relMSE floor %.2e\n",
                ref.validCount, pixels, ref.mean, ref.seconds, ref.threads, ref.floor);
    expect(ref.validCount > pixels / 2u && ref.mean > 0.0 && std::isfinite(ref.mean), "reference: non-trivial, finite");

    std::vector<f64> l;
    // (a) RIS-only: independent frames.
    {
        ConfigResult r{};
        r.name = "RIS-only (8 cand.)";
        std::vector<ImageMetrics> m;
        std::vector<std::vector<f64>> imgs;
        for (u32 f = 0; f < kRisFrames && ok; ++f) {
            FrameOptions o{};
            o.frameIndex = frameIndex++;
            o.reset = true;
            o.readback = true;
            ok = renderFrame(b, w, rig, graph, cam, o);
            lumImage(section(b, rig, kSecDi), pixels, l);
            m.push_back(measure(ref, l));
            imgs.push_back(l);
        }
        accumulate(r, m);
        r.change = meanChange(ref, imgs);
        r.ok = ok;
        results.push_back(r);
        // N-frame averages (non-overlapping groups).
        for (u32 n = 1; n <= kRisFrames && ok; n *= 2u) {
            f64 acc = 0.0;
            u32 groups = 0;
            for (u32 g = 0; g + n <= kRisFrames; g += n) {
                std::vector<f64> avg(pixels, 0.0);
                for (u32 i = 0; i < n; ++i) {
                    for (usize p = 0; p < pixels; ++p) {
                        avg[p] += imgs[g + i][p];
                    }
                }
                for (f64& v : avg) {
                    v /= static_cast<f64>(n);
                }
                acc += measure(ref, avg).relMse;
                ++groups;
            }
            risCurve.push_back(acc / static_cast<f64>(groups));
        }
    }
    // (b) / (c) ReSTIR DI (+ SVGF), biased then unbiased.
    for (const bool unbiased : {false, true}) {
        rig.restir.setSettings(diOnly(unbiased));
        ConfigResult raw{};
        ConfigResult den{};
        raw.name = unbiased ? "ReSTIR DI unbiased" : "ReSTIR DI biased";
        den.name = unbiased ? "ReSTIR DI unbiased + SVGF" : "ReSTIR DI biased + SVGF";
        std::vector<ImageMetrics> mr;
        std::vector<ImageMetrics> md;
        std::vector<std::vector<f64>> ir;
        std::vector<std::vector<f64>> id;
        for (u32 f = 0; f < kWarmFrames + kMeasureFrames && ok; ++f) {
            FrameOptions o{};
            o.frameIndex = frameIndex++;
            o.reset = f == 0u;
            o.denoise = true;
            o.readback = f >= kWarmFrames;
            ok = renderFrame(b, w, rig, graph, cam, o);
            if (!o.readback || !ok) {
                continue;
            }
            lumImage(section(b, rig, kSecDi), pixels, l);
            mr.push_back(measure(ref, l));
            ir.push_back(l);
            lumImage(section(b, rig, kSecDenoised), pixels, l);
            md.push_back(measure(ref, l));
            id.push_back(l);
        }
        accumulate(raw, mr);
        accumulate(den, md);
        raw.change = meanChange(ref, ir);
        den.change = meanChange(ref, id);
        raw.ok = den.ok = ok;
        results.push_back(raw);
        results.push_back(den);
    }
    expect(ok, "quality frames ok");
    destroyRig(rig);
    w.gpu.destroy();
    return ok ? 0 : 1;
}

u32 argU32(const char* s) { return static_cast<u32>(std::strtoul(s, nullptr, 10)); }

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args args{};
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (std::strcmp(argv[i], "--width") == 0 && more) {
            args.width = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--height") == 0 && more) {
            args.height = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--ref-spp") == 0 && more) {
            args.refSpp = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--threads") == 0 && more) {
            args.threads = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--time-width") == 0 && more) {
            args.timeWidth = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--time-height") == 0 && more) {
            args.timeHeight = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--frames") == 0 && more) {
            args.frames = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--trials") == 0 && more) {
            args.trials = argU32(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-timing") == 0) {
            args.timing = false;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (args.width == 0u || args.height == 0u || args.refSpp == 0u || args.frames == 0u || args.trials == 0u) {
        std::fprintf(stderr, "bad arguments\n");
        return 2;
    }
    int rc = 0;
    std::vector<ConfigResult> results;
    std::vector<f64> risCurve;
    std::vector<TimingResult> timings;
    Reference ref;
    {
        Bench b;
        hw_bench::HwSetupDesc sd{};
        sd.appName = "fuse_rp_hw_bench_restir";
        const int setupRc = hw_bench::setup(b.ctx, sd);
        if (setupRc != 0) {
            return setupRc;
        }
        const RestirCapabilities caps = queryRestirCapabilities(b.ctx.device.get());
        if (!caps.gpu) {
            std::printf("SKIP: T2 gate: %s\n", caps.reason);
            return hw_bench::kSkip;
        }
        const VisCapabilities visCaps = queryVisCapabilities(b.ctx.device.get());
        const ResolveCapabilities resolveCaps = queryResolveCapabilities(b.ctx.device.get());
        if (!visCaps.raster || !resolveCaps.resolve) {
            std::printf("SKIP: capability missing (vis %s, resolve %s)\n", visCaps.rasterReason, resolveCaps.reason);
            return hw_bench::kSkip;
        }
        SamplerDesc smp{};
        smp.name = "hw_restir.sampler";
        b.sampler = b.ctx.bindless.acquireSampler(smp);
        if (!b.sampler.isValid()) {
            std::fprintf(stderr, "FAIL: sampler\n");
            return 1;
        }
        b.samplerHandle = b.ctx.bindless.shaderHandle(b.sampler);
        rc = runQuality(b, args, results, ref, risCurve);
        if (rc == 0 && args.timing) {
            std::printf("timing at %u x %u: 30 warm-up frames, %u trials x %u frames per configuration\n", args.timeWidth, args.timeHeight,
                        args.trials, args.frames);
            rc = runTiming(b, args, timings);
        }
    }
    if (rc == hw_bench::kSkip) {
        return hw_bench::kSkip;
    }

    // --- summary ---------------------------------------------------------------------------------------------
    std::printf("\n=== P7: ReSTIR DI, 10 000 emissive triangles, %u x %u, reference %u spp (%.1f s on %u threads) ===\n", args.width,
                args.height, args.refSpp, ref.seconds, ref.threads);
    std::printf("relMSE = mean (L - Lref)^2 / (Lref^2 + (0.1 mean Lref)^2); reference noise floor %.2e\n", ref.floor);
    std::printf("%-28s %12s %22s %10s %14s\n", "configuration", "relMSE", "(min .. max frame)", "bias", "frame change");
    for (const ConfigResult& r : results) {
        std::printf("%-28s %12.4e   (%.3e .. %.3e) %+9.3f%% ", r.name, r.relMse, r.relMseMin, r.relMseMax, r.bias * 100.0);
        if (r.change >= 0.0) {
            std::printf("%13.3f%%\n", r.change * 100.0);
        } else {
            std::printf("%14s\n", "n/a");
        }
    }
    const ConfigResult* ris = results.size() >= 5u ? &results[0] : nullptr;
    const ConfigResult* restirB = results.size() >= 5u ? &results[1] : nullptr;
    const ConfigResult* denB = results.size() >= 5u ? &results[2] : nullptr;
    const ConfigResult* restirU = results.size() >= 5u ? &results[3] : nullptr;
    const ConfigResult* denU = results.size() >= 5u ? &results[4] : nullptr;
    if (!risCurve.empty()) {
        std::printf("RIS-only N-frame average relMSE:");
        u32 n = 1;
        for (const f64 v : risCurve) {
            std::printf(" N=%u %.3e;", n, v);
            n *= 2u;
        }
        std::printf("\n");
        if (restirB != nullptr && restirU != nullptr) {
            for (const ConfigResult* r : {restirB, restirU, denB, denU}) {
                // log-log interpolation on the 1 / N curve
                f64 frames = -1.0;
                n = 1;
                for (usize i = 0; i < risCurve.size(); ++i, n *= 2u) {
                    if (risCurve[i] <= r->relMse) {
                        if (i == 0u) {
                            frames = 1.0;
                        } else {
                            const f64 a = std::log(risCurve[i - 1u]);
                            const f64 c = std::log(risCurve[i]);
                            const f64 t = (a - std::log(r->relMse)) / std::max(a - c, 1e-12);
                            frames = std::exp(std::log(static_cast<f64>(n / 2u)) + t * std::log(2.0));
                        }
                        break;
                    }
                }
                if (frames > 0.0) {
                    std::printf("  %-28s one frame == ~%.1f averaged RIS-only frames\n", r->name, frames);
                } else {
                    std::printf("  %-28s one frame == more than %u averaged RIS-only frames\n", r->name, kRisFrames);
                }
            }
        }
    }
    if (!timings.empty()) {
        std::printf("GPU time at %u x %u (median of per-frame sums; per-trial median, spread over %u trials):\n", args.timeWidth,
                    args.timeHeight, args.trials);
        for (const TimingResult& t : timings) {
            std::vector<f64> med;
            std::vector<f64> dmed;
            for (usize i = 0; i < t.restir.size(); ++i) {
                med.push_back(t.restir[i].median);
                dmed.push_back(t.denoise[i].median);
            }
            const hw_bench::Stats s = hw_bench::stats(med);
            const hw_bench::Stats d = hw_bench::stats(dmed);
            std::printf("  %-24s restir.* %.3f ms (trials %.3f .. %.3f)", t.name, s.median, s.min, s.max);
            if (d.max > 0.0) {
                std::printf(", SVGF %.3f ms (trials %.3f .. %.3f)", d.median, d.min, d.max);
            }
            std::printf("\n");
            expect(s.median > 0.0 && std::isfinite(s.median), "restir.* GPU time measured");
        }
    }

    // --- gates -----------------------------------------------------------------------------------------------
    expect(results.size() == 5u, "every configuration ran");
    for (const ConfigResult& r : results) {
        expect(r.ok && r.finite, "every configuration's frames ok and finite");
    }
    if (results.size() == 5u) {
        std::printf("gates: relMSE unbiased ReSTIR <= RIS-only / %.1f, biased <= RIS-only / %.1f; denoised relMSE < raw; |bias| "
                    "unbiased (+ SVGF) / RIS-only <= %.0f %%, biased (+ SVGF) <= %.0f %%; denoised frame change < raw and <= %.0f %%\n",
                    kRestirGain, kBiasedGain, kUnbiasedBias * 100.0, kBiasedBias * 100.0, kStableChange * 100.0);
        std::printf("  gain RIS-only / ReSTIR: biased x%.1f, unbiased x%.1f; SVGF gain: biased x%.1f, unbiased x%.1f\n",
                    ris->relMse / std::max(restirB->relMse, 1e-30), ris->relMse / std::max(restirU->relMse, 1e-30),
                    restirB->relMse / std::max(denB->relMse, 1e-30), restirU->relMse / std::max(denU->relMse, 1e-30));
        expect(restirB->relMse * kBiasedGain <= ris->relMse, "biased ReSTIR relMSE at least kBiasedGain x below RIS-only");
        expect(restirU->relMse * kRestirGain <= ris->relMse, "unbiased ReSTIR relMSE at least kRestirGain x below RIS-only");
        expect(denB->relMse < restirB->relMse, "denoised (biased) relMSE below raw");
        expect(denU->relMse < restirU->relMse, "denoised (unbiased) relMSE below raw");
        expect(std::fabs(restirU->bias) <= kUnbiasedBias && std::fabs(denU->bias) <= kUnbiasedBias,
               "unbiased ReSTIR (+ SVGF) |bias| within kUnbiasedBias");
        expect(std::fabs(restirB->bias) <= kBiasedBias && std::fabs(denB->bias) <= kBiasedBias, "biased ReSTIR (+ SVGF) |bias| within kBiasedBias");
        expect(std::fabs(ris->bias) <= kUnbiasedBias, "RIS-only |bias| within kUnbiasedBias");
        expect(denB->change < restirB->change && denU->change < restirU->change, "denoised frame-to-frame change below raw");
        expect(denB->change <= kStableChange && denU->change <= kStableChange, "denoised frame-to-frame change within kStableChange");
    }
    if (rc != 0 || hw_bench::g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", hw_bench::g_failures + (rc != 0 ? 1 : 0));
        return 1;
    }
    std::printf("PASS P7\n");
    return 0;
}

#endif
