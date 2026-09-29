// Vulkan half of the B5 per-pass frame benchmark (fuse_b5_frame_bench; see test_b5_frame_bench.cpp). The passes the
// tree runs through Vulkan, timed with the engine's GPU profiler (GpuProfiler: TOP_OF_PIPE / BOTTOM_OF_PIPE
// timestamp queries per zone — the profiler's GPU pass breakdown is this measurement):
//
//   gbuffer   B5.2 GBufferRasterPass: 6 MRT targets + D32 at width x height, `draws` quads front-to-back (plus a
//             full-screen background) with per-draw push constants; one zone around the recorded pass
//             (VulkanBootstrap + FrameManager + submitGraphicsQueue, the fuse_b5_rhi_gbuffer_pass path).
//   taa       WP-4.1 TaauGpu at 1x (render == display == width x height): the "taau.*" render-graph passes.
//   post      WP-4.5 PostStackGpu: bloom + depth of field + motion blur + ACES display transform — the "post.*"
//             render-graph passes, zones from GpuProfiler::passHooks on the rg::Executor.
//
// Inputs are synthetic (the benchmark frame's surfaces, uploaded once); timings are reported, not asserted.

#include "b5_frame_bench.hpp"

#include <fuse/math/mat.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/deferred/gbuffer_raster_pass.hpp>
#include <fuse/renderer/postprocess/gpu/post_stack_gpu.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/gpu_profiler.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace b5bench {

#if !defined(FUSE_VULKAN_BACKEND)

bool vulkanBench(const BenchConfig& /*cfg*/, const Frame& /*frame*/, PassResult& gbuffer, PassResult& taa,
                 PassResult& post, std::string& deviceName) {
    deviceName = "none (Vulkan backend not built)";
    for (PassResult* r : {&gbuffer, &taa, &post}) {
        r->note = "Vulkan backend not built";
    }
    return false;
}

#else

namespace {

using namespace fuse::renderer;
using fuse::u16;
using fuse::u64;
using fuse::u8;

u16 halfBits(f32 value) {
    return GBufferQuantize::floatToHalf(value);
}

/// Sum of the latest resolved frame's zones whose name starts with `prefix`, in ms (-1: frame not resolved).
f64 zoneSumMs(const GpuProfiler& gpu, const char* prefix) {
    const GpuFrameTimings& frame = gpu.latest();
    if (!frame.valid) {
        return -1.0;
    }
    u64 ns = 0u;
    bool any = false;
    const usize length = std::strlen(prefix);
    for (const GpuZoneTiming& zone : frame.zones) {
        if (std::strncmp(zone.name, prefix, length) == 0 && zone.depth == 0u) {
            ns += zone.durationNs();
            any = true;
        }
    }
    return any ? static_cast<f64>(ns) * 1e-6 : -1.0;
}

// ---------------------------------------------------------------------------------------------
// G-buffer raster pass
// ---------------------------------------------------------------------------------------------

bool benchGBuffer(const BenchConfig& cfg, PassResult& out, std::string& deviceName) {
#if !defined(FUSE_B5_RHI_SHADERS_BUILT)
    out.note = "raster shaders not built (glslangValidator missing)";
    (void)cfg;
    (void)deviceName;
    return true;
#else
    VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.instance.appName = "fuse_b5_frame_bench";
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady || bootstrap->frameManager() == nullptr ||
        !bootstrap->frameManager()->isReady()) {
        out.note = "no Vulkan device";
        return false;
    }
    VulkanDevice& device = *bootstrap->device();
    FrameManager& frames = *bootstrap->frameManager();
    deviceName = device.info().deviceName;
    auto gpu = GpuProfiler::create(device);
    if (gpu == nullptr || !gpu->isValid() || !gpu->timestampsSupported(rg::QueueClass::Graphics)) {
        out.note = "no timestamp queries on the graphics queue";
        return true;
    }
    {
        BindlessDescriptors bindless;
        bindless.init(device, BindlessDesc{});
        ResourceManager resources;
        GBuffer gbuffer;
        GBufferDesc gdesc{};
        gdesc.width = cfg.width;
        gdesc.height = cfg.height;
        GBufferRasterPass pass;
        const std::string vert = std::string(FUSE_B5_RHI_SHADER_DIR) + "/gbuffer.vert.spv";
        const std::string frag = std::string(FUSE_B5_RHI_SHADER_DIR) + "/gbuffer.frag.spv";
        GBufferRasterPassDesc passDesc{};
        passDesc.vertexSpirvPath = vert.c_str();
        passDesc.fragmentSpirvPath = frag.c_str();
        const bool ready = resources.init(device, bindless) && gbuffer.init(resources, gdesc) &&
                           pass.init(device, resources, gbuffer, passDesc);
        if (!ready) {
            out.failed = true;
            out.note = "G-buffer pass init failed: " + pass.stats().message;
        } else {
            // One full-screen background quad + `draws` object quads (~3x overdraw), sorted front to back.
            const u32 quads = cfg.draws + 1u;
            std::vector<f32> vertices;
            vertices.reserve(static_cast<usize>(quads) * 18u);
            std::vector<GBufferDraw> draws(quads);
            std::mt19937 rng(20260929u);
            std::uniform_real_distribution<f32> u01(0.f, 1.f);
            const f32 side = std::min(1.f, 2.f * std::sqrt(3.f / static_cast<f32>(std::max(cfg.draws, 1u))));
            for (u32 q = 0; q < quads; ++q) {
                const bool background = q == 0u;
                const f32 s = background ? 2.f : side * (0.6f + 0.8f * u01(rng));
                const f32 x0 = background ? -1.f : -1.f + (2.f - s) * u01(rng);
                const f32 y0 = background ? -1.f : -1.f + (2.f - s) * u01(rng);
                const f32 quad[18] = {x0,     y0,     0.f, x0 + s, y0,     0.f, x0 + s, y0 + s, 0.f,
                                      x0,     y0,     0.f, x0 + s, y0 + s, 0.f, x0,     y0 + s, 0.f};
                vertices.insert(vertices.end(), quad, quad + 18);
                GBufferDraw& d = draws[q];
                d.vertexCount = 6u;
                d.firstVertex = q * 6u;
                d.push.albedo[0] = u01(rng);
                d.push.albedo[1] = u01(rng);
                d.push.albedo[2] = u01(rng);
                const Vec3 n = Vec3{u01(rng) - 0.5f, 1.f, u01(rng) - 0.5f}.normalized();
                d.push.normal[0] = n.x;
                d.push.normal[1] = n.y;
                d.push.normal[2] = n.z;
                d.push.surface[0] = u01(rng);                                // roughness
                d.push.surface[1] = u01(rng) < 0.3f ? 1.f : 0.f;             // metallic
                d.push.surface[2] = background ? 0.999f : 0.05f + 0.9f * u01(rng); // NDC depth
                d.push.surface[3] = 1.f;
            }
            std::sort(draws.begin(), draws.end(),
                      [](const GBufferDraw& a, const GBufferDraw& b) { return a.push.surface[2] < b.push.surface[2]; });
            BufferDesc vbDesc{};
            vbDesc.size = vertices.size() * sizeof(f32);
            vbDesc.usage = BufferUsage::Vertex;
            vbDesc.memoryUsage = MemoryUsage::CpuToGpu;
            vbDesc.name = "fuse.b5_bench.gbuffer_quads";
            const BufferHandle vb = resources.createBuffer(vbDesc);
            Buffer* vertexBuffer = resources.getBuffer(vb);
            if (vertexBuffer == nullptr || vertexBuffer->mapped == nullptr) {
                out.failed = true;
                out.note = "vertex buffer";
            } else {
                std::memcpy(vertexBuffer->mapped, vertices.data(), vbDesc.size);
                const u32 total = cfg.warmup + cfg.iterations;
                for (u32 i = 0; i < total && !out.failed; ++i) {
                    frames.signalTickComplete();
                    frames.beginFrame(i);
                    if (!resetFrameSlotCommandPool(device, frames)) {
                        out.failed = true;
                        out.note = "command pool reset";
                        break;
                    }
                    auto cmd = static_cast<VkCommandBuffer>(frames.currentCommandBuffer());
                    VkCommandBufferBeginInfo beginInfo{};
                    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    gpu->beginFrame();
                    bool ok = vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
                    const u32 zone = gpu->beginZone(cmd, "gbuffer");
                    ok = ok && pass.record(cmd, vertexBuffer->handle, draws.data(), quads);
                    gpu->endZone(cmd, zone);
                    ok = ok && vkEndCommandBuffer(cmd) == VK_SUCCESS;
                    GraphicsQueueSubmitDesc submit{};
                    submit.device = &device;
                    submit.frameManager = &frames;
                    submit.commandsAlreadyRecorded = true;
                    ok = ok && submitGraphicsQueue(submit).ok;
                    frames.endFrame();
                    gpu->endFrame();
                    device.waitIdle();
                    gpu->resolve();
                    if (!ok) {
                        out.failed = true;
                        out.note = "G-buffer frame record / submit failed";
                        break;
                    }
                    const f64 ms = zoneSumMs(*gpu, "gbuffer");
                    if (i >= cfg.warmup && ms >= 0.0) {
                        out.ms.push_back(ms);
                    }
                }
                if (!out.failed) {
                    out.ran = !out.ms.empty();
                    char note[160];
                    std::snprintf(note, sizeof(note), "%u draws + background, 6 MRT + D32, push constants per draw",
                                  cfg.draws);
                    out.note = out.ran ? note : "no resolved GPU zones";
                }
            }
            resources.destroyBuffer(vb);
        }
        device.waitIdle();
        gpu.reset();
        pass.destroy();
        gbuffer.destroy();
        resources.destroy();
        bindless.destroy(device);
    }
    bootstrap.reset();
    return true;
#endif
}

// ---------------------------------------------------------------------------------------------
// TAA (TAAU 1x) + GPU post stack on the render graph
// ---------------------------------------------------------------------------------------------

struct RgContext {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    std::unique_ptr<GpuProfiler> gpu;
    BindlessDescriptors bindless;
    bool bindlessReady = false;

    ~RgContext() {
        if (device != nullptr) {
            device->waitIdle();
        }
        gpu.reset();
        executor.reset();
        if (bindlessReady && device != nullptr) {
            bindless.collectRetired(~0ull);
            bindless.destroy(*device);
        }
        allocator.reset();
        device.reset();
        instance.reset();
    }
};

/// A persistent image imported into every frame's graph (layout / queue tracked across frames).
struct BenchImage {
    Texture tex{};
    BindlessSlotHandle slot{};
    u32 handle = 0;
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    u32 layout = 0;
    u8 queue = rg::kNoQueue;
    const char* name = "b5_bench.image";

    bool create(RgContext& ctx, u32 w, u32 h, GpuFormat fmt, const char* n, bool bindlessSlot) {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.format = fmt;
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        d.name = n;
        name = n;
        if (!ctx.allocator->createImage(d, tex)) {
            return false;
        }
        format = static_cast<u32>(fmt);
        width = w;
        height = h;
        if (bindlessSlot) {
            slot = ctx.bindless.registerTextureSlot(tex, false);
            handle = slot.isValid() ? ctx.bindless.shaderHandle(slot) : 0u;
            return handle != 0u;
        }
        return true;
    }
    void destroy(RgContext& ctx) {
        if (slot.isValid()) {
            ctx.bindless.unregisterSlot(slot);
        }
        if (tex.image != nullptr) {
            ctx.allocator->destroyImage(tex);
        }
        *this = BenchImage{};
    }
    rg::TextureRef import(rg::Graph& graph) {
        rg::ImportedImage i{};
        i.image = tex.image;
        i.view = tex.view;
        i.format = format;
        i.width = width;
        i.height = height;
        i.initialLayout = layout;
        i.initialQueue = queue;
        i.layoutTracker = &layout;
        i.queueTracker = &queue;
        i.name = name;
        return graph.importImage(i);
    }
};

struct BenchBuffer {
    Buffer buffer{};
    u8 queue = rg::kNoQueue;
    const char* name = "b5_bench.buffer";

    bool create(RgContext& ctx, u64 bytes, BufferUsage usage, const char* n) {
        name = n;
        BufferDesc d{};
        d.size = static_cast<usize>(bytes);
        d.usage = usage;
        d.memoryUsage = MemoryUsage::CpuToGpu;
        d.name = n;
        return ctx.allocator->createBuffer(d, buffer) && buffer.mapped != nullptr;
    }
    void destroy(RgContext& ctx) {
        if (buffer.handle != nullptr) {
            ctx.allocator->destroyBuffer(buffer);
        }
        buffer = Buffer{};
    }
    rg::BufferRef import(rg::Graph& graph) {
        return graph.importBuffer(rg::ImportedBuffer{buffer.handle, buffer.desc.size, queue, &queue, name});
    }
};

struct UploadRecord {
    rg::TextureRef image;
    rg::BufferRef staging;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const UploadRecord& r = *static_cast<const UploadRecord*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = r.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.width, r.height, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(r.staging)),
                           static_cast<VkImage>(pc.image(r.image)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

UpscaleCamera upscaleCamera(const Frame& f) {
    UpscaleCamera c{};
    const Vec3 target = f.view.position - f.view.back;
    c.view = fuse::math::lookAt(f.view.position, target, f.view.up);
    const f32 aspect = static_cast<f32>(f.width) / static_cast<f32>(f.height);
    c.projection = fuse::math::perspective(kFovYRadians * 180.f / 3.14159265f, aspect, kNearPlane, kFarPlane);
    c.position = f.view.position;
    c.vertical_fov_rad = kFovYRadians;
    c.aspect = aspect;
    c.near_plane = kNearPlane;
    c.far_plane = kFarPlane;
    return c;
}

bool benchTemporalPost(const BenchConfig& cfg, const Frame& frame, PassResult& taa, PassResult& post,
                       std::string& deviceName) {
    RgContext ctx;
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_b5_frame_bench";
    instanceDesc.enableValidation = false;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        taa.note = post.note = "no Vulkan instance";
        return false;
    }
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        taa.note = post.note = "no Vulkan device";
        return false;
    }
    if (deviceName.empty()) {
        deviceName = ctx.device->info().deviceName;
    }
    const temporal::TemporalCapabilities tcaps = temporal::queryTemporalCapabilities(ctx.device.get());
    const post_gpu::PostCapabilities pcaps = post_gpu::queryPostCapabilities(ctx.device.get());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    ctx.bindlessReady = ctx.allocator != nullptr && ctx.allocator->isValid() && ctx.bindless.init(*ctx.device, BindlessDesc{});
    if (ctx.bindlessReady) {
        ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    }
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        taa.failed = post.failed = true;
        taa.note = post.note = "allocator / bindless / rg::Executor init failed";
        return true;
    }
    GpuProfilerDesc pdesc{};
    pdesc.name = "b5_bench.gpu";
    ctx.gpu = GpuProfiler::create(*ctx.device, pdesc);
    if (ctx.gpu == nullptr || !ctx.gpu->isValid()) {
        taa.note = post.note = "GpuProfiler unavailable";
        return true;
    }
    ctx.executor->setPassHooks(ctx.gpu->passHooks());

    const u32 w = cfg.width;
    const u32 h = cfg.height;
    const u64 n = static_cast<u64>(w) * h;
    // Inputs: TAAU colour (RGBA16F, bindless) + depth / motion (BDA buffers); post HDR (RGBA16F), depth (R32F),
    // velocity (RG16F). One staging buffer uploads the four images on the first frame.
    BenchImage color;
    BenchImage hdr;
    BenchImage depthTex;
    BenchImage velocity;
    BenchBuffer staging;
    BenchBuffer taaDepth;
    BenchBuffer taaMotion;
    // TransferSrc: TAAU copies this frame's depth / motion into its previous-frame buffers.
    const BufferUsage bda = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                     static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                                     static_cast<u32>(BufferUsage::TransferSrc));
    const u64 offColor = 0u;
    const u64 offHdr = n * 8u;
    const u64 offDepth = n * 16u;
    const u64 offVelocity = n * 20u;
    const bool created =
        color.create(ctx, w, h, GpuFormat::R16G16B16A16Sfloat, "b5_bench.taa_color", true) &&
        hdr.create(ctx, w, h, GpuFormat::R16G16B16A16Sfloat, "b5_bench.post_hdr", false) &&
        depthTex.create(ctx, w, h, GpuFormat::R32Sfloat, "b5_bench.post_depth", false) &&
        velocity.create(ctx, w, h, GpuFormat::R16G16Sfloat, "b5_bench.post_velocity", false) &&
        staging.create(ctx, n * 24u, BufferUsage::TransferSrc, "b5_bench.staging") &&
        taaDepth.create(ctx, n * 4u, bda, "b5_bench.taa_depth") && taaMotion.create(ctx, n * 8u, bda, "b5_bench.taa_motion") &&
        taaDepth.buffer.deviceAddress != 0u && taaMotion.buffer.deviceAddress != 0u;
    bool ok = created;
    if (!created) {
        taa.failed = post.failed = true;
        taa.note = post.note = "input images / buffers";
    } else {
        // HDR colour from the frame (sky 0.3..1.2, bright spots for bloom), linear depth, a small camera-pan velocity.
        u8* base = static_cast<u8*>(staging.buffer.mapped);
        f32* depthOut = static_cast<f32*>(taaDepth.buffer.mapped);
        f32* motionOut = static_cast<f32*>(taaMotion.buffer.mapped);
        const usize fw = frame.width;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const usize src = static_cast<usize>(std::min(y * frame.height / h, frame.height - 1u)) * fw +
                                  std::min(x * frame.width / w, frame.width - 1u);
                const usize i = static_cast<usize>(y) * w + x;
                const Vec3 c = frame.sceneColor[src] * ((x * 7u + y * 13u) % 97u == 0u ? 8.f : 1.f);
                const u16 texel[4] = {halfBits(c.x), halfBits(c.y), halfBits(c.z), halfBits(1.f)};
                std::memcpy(base + offColor + i * 8u, texel, 8u);
                std::memcpy(base + offHdr + i * 8u, texel, 8u);
                const f32 viewDepth = frame.viewDepth[src] > 0.f ? frame.viewDepth[src] : kFarPlane;
                std::memcpy(base + offDepth + i * 4u, &viewDepth, 4u);
                const u16 v[2] = {halfBits(1.5f), halfBits(-0.5f)};
                std::memcpy(base + offVelocity + i * 4u, v, 4u);
                depthOut[i] = frame.deviceDepth[src];
                motionOut[i * 2u + 0u] = 1.5f;
                motionOut[i * 2u + 1u] = -0.5f;
            }
        }
    }

    temporal::TaauGpu taau;
    bool taauReady = false;
    if (ok && tcaps.temporal) {
        temporal::TaauGpuDesc td{};
        td.device = ctx.device.get();
        td.allocator = ctx.allocator.get();
        td.bindless = &ctx.bindless;
        td.resolution = UpscaleResolution{w, h, w, h};
        taauReady = taau.init(td);
        if (!taauReady) {
            taa.failed = true;
            taa.note = "TaauGpu::init failed";
        }
    } else if (ok) {
        taa.note = std::string("TAAU unsupported: ") + tcaps.reason;
    }
    post_gpu::PostStackGpu stack;
    bool postReady = false;
    if (ok && pcaps.post) {
        post_gpu::PostStackGpuDesc pd{};
        pd.device = ctx.device.get();
        pd.allocator = ctx.allocator.get();
        pd.bindless = &ctx.bindless;
        postReady = stack.init(pd);
        if (!postReady) {
            post.failed = true;
            post.note = "PostStackGpu::init failed";
        }
    } else if (ok) {
        post.note = std::string("GPU post stack unsupported: ") + pcaps.reason;
    }

    post_gpu::PostGpuSettings settings{};
    settings.bloom = true;
    settings.bloomParams.threshold = 1.f;
    settings.bloomParams.intensity = 0.3f;
    settings.dof = true;
    settings.dofParams.focal_distance = 12.f;
    settings.dofParams.f_stop = 2.8f;
    settings.dofParams.max_coc_radius_px = 8.f;
    settings.motionBlur = true;
    settings.motionBlurParams.max_blur_px = 12.f;
    settings.toneMapper = ToneMapper::ACES;

    const UpscaleCamera camera = upscaleCamera(frame);
    u64 serial = 0u;
    UploadRecord uploads[4];
    const u32 total = cfg.warmup + cfg.iterations;
    for (u32 i = 0; ok && (taauReady || postReady) && i < total; ++i) {
        ++serial;
        ctx.bindless.setFrameSerial(serial);
        if (taauReady) {
            temporal::TaauGpuFrameDesc fd{};
            fd.resolution = UpscaleResolution{w, h, w, h};
            fd.jitter_px = {((i * 5u) % 8u) / 8.f - 0.4375f, ((i * 3u) % 8u) / 8.f - 0.4375f};
            fd.reset_history = i == 0u;
            fd.camera = camera;
            fd.previous_camera = camera;
            fd.color = color.handle;
            fd.depth = taaDepth.buffer.deviceAddress;
            fd.motion = taaMotion.buffer.deviceAddress;
            if (!taau.beginFrame(serial, fd)) {
                taa.failed = true;
                taa.note = "TaauGpu::beginFrame failed";
                taauReady = false;
            }
        }
        if (postReady) {
            post_gpu::PostFrameImages images{};
            images.hdr = &hdr.tex;
            images.depth = &depthTex.tex;
            images.velocity = &velocity.tex;
            if (!stack.beginFrame(serial, settings, images)) {
                post.failed = true;
                post.note = "PostStackGpu::beginFrame failed";
                postReady = false;
            }
        }
        if (!taauReady && !postReady) {
            break;
        }
        rg::Graph graph;
        const rg::TextureRef colorRef = color.import(graph);
        const rg::TextureRef hdrRef = hdr.import(graph);
        const rg::TextureRef depthRef = depthTex.import(graph);
        const rg::TextureRef velocityRef = velocity.import(graph);
        if (i == 0u) {
            const rg::BufferRef stagingRef = staging.import(graph);
            const rg::TextureRef targets[4] = {colorRef, hdrRef, depthRef, velocityRef};
            const u64 offsets[4] = {offColor, offHdr, offDepth, offVelocity};
            const u64 sizes[4] = {n * 8u, n * 8u, n * 4u, n * 4u};
            for (u32 k = 0; k < 4u; ++k) {
                uploads[k] = UploadRecord{targets[k], stagingRef, offsets[k], w, h};
                graph.addPass("bench.upload", &recordUpload, &uploads[k])
                    .use(targets[k], rg::Access::TransferDst)
                    .use(stagingRef, rg::Access::TransferSrc, rg::BufferRange{offsets[k], sizes[k]});
            }
        }
        rg::TextureRef taaOut{};
        rg::TextureRef postOut{};
        if (taauReady) {
            temporal::TaauGpuInputs in{};
            in.color = colorRef;
            in.depth = taaDepth.import(graph);
            in.motion = taaMotion.import(graph);
            const temporal::TaauGraphRefs refs = taau.importInto(graph);
            taau.addResolve(graph, refs, in);
            taaOut = refs.output;
        }
        if (postReady) {
            const post_gpu::PostGraphRefs refs = stack.importInto(graph);
            post_gpu::PostGraphInputs inputs{};
            inputs.hdr = hdrRef;
            inputs.depth = depthRef;
            inputs.velocity = velocityRef;
            stack.addPasses(graph, refs, inputs);
            postOut = refs.output;
        }
        // A consumer after both chains, so nothing they write is culled (and every uploaded input has a reader).
        rg::PassBuilder keep = graph.addPass("bench.keep", nullptr, nullptr);
        keep.use(taauReady ? taaOut : colorRef, rg::Access::SampledRead, {}, rg::kStageCompute);
        if (postReady) {
            keep.use(postOut, rg::Access::SampledRead, {}, rg::kStageCompute);
        } else {
            keep.use(hdrRef, rg::Access::SampledRead, {}, rg::kStageCompute)
                .use(depthRef, rg::Access::SampledRead, {}, rg::kStageCompute)
                .use(velocityRef, rg::Access::SampledRead, {}, rg::kStageCompute);
        }
        keep.neverCull();
        ctx.gpu->beginFrame();
        const bool executed = ctx.executor->execute(graph).ok;
        ctx.gpu->endFrame();
        const bool waited = ctx.executor->waitIdle();
        ctx.gpu->resolve();
        if (taauReady) {
            taau.collectRetired(serial);
        }
        if (postReady) {
            stack.collectRetired(serial);
        }
        ctx.bindless.collectRetired(serial);
        if (!executed || !waited) {
            taa.failed = taauReady;
            post.failed = postReady;
            taa.note = post.note = "render graph execute failed: " + ctx.executor->message();
            ok = false;
            break;
        }
        if (i >= cfg.warmup) {
            const f64 taaMs = zoneSumMs(*ctx.gpu, "taau.");
            const f64 postMs = zoneSumMs(*ctx.gpu, "post.");
            if (taauReady && taaMs >= 0.0) {
                taa.ms.push_back(taaMs);
            }
            if (postReady && postMs >= 0.0) {
                post.ms.push_back(postMs);
            }
        }
    }
    if (taauReady && !taa.failed) {
        taa.ran = !taa.ms.empty();
        taa.note = taa.ran ? std::string("taau.* passes, 1x (") + taau.kernelLanguage() + ")" : "no resolved GPU zones";
    }
    if (postReady && !post.failed) {
        post.ran = !post.ms.empty();
        char note[200];
        std::snprintf(note, sizeof(note), "post.* passes: bloom (%u levels) + DoF + motion blur + ACES display (%s)",
                      stack.stats().bloomLevels, stack.kernelLanguage());
        post.note = post.ran ? note : "no resolved GPU zones";
    }
    ctx.device->waitIdle();
    if (taauReady || taau.valid()) {
        taau.destroy();
    }
    if (postReady || stack.valid()) {
        stack.destroy();
    }
    color.destroy(ctx);
    hdr.destroy(ctx);
    depthTex.destroy(ctx);
    velocity.destroy(ctx);
    staging.destroy(ctx);
    taaDepth.destroy(ctx);
    taaMotion.destroy(ctx);
    return true;
}

} // namespace

bool vulkanBench(const BenchConfig& cfg, const Frame& frame, PassResult& gbuffer, PassResult& taa, PassResult& post,
                 std::string& deviceName) {
    const bool haveDevice = benchGBuffer(cfg, gbuffer, deviceName);
    const bool haveRg = benchTemporalPost(cfg, frame, taa, post, deviceName);
    return haveDevice || haveRg;
}

#endif

} // namespace b5bench
