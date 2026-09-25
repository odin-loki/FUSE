// WP-9.2 3D Gaussian splatting Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). CPU gates: test_rp_gsplat_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): a test pass uploads the visibility-buffer depth (R32F, the WP-1.4
// export depth format, forward z/w) from a staging buffer, GsplatRenderer adds its passes (the radix sort runs
// inline inside gsplat.sort), then read-back copies of every section. The CPU reference
// (gsplat_reference.hpp) is the oracle; both kernel languages built (Slang, GLSL) run every check.
//
//   --mode passes       2000 splats (SH degree 3), 157 x 117 (partial tiles), 2 cameras, occluder over the left
//                       half at mid-scene depth:
//                         preprocess  == gs_preprocess_splat (radius / tile rect / view z bit for bit, the rest
//                                        within 2e-6 relative)
//                         scan        offsets and counters == the reference's
//                         emit + sort keys / values (whole capacity, incl. padding) == the reference's stable sort
//                         ranges      == the reference's
//                         raster      == gs_raster_pixel on the GPU's own preprocess / sort output (isolated)
//                         image       end to end vs the CPU reference: PSNR >= 60 dB
//   --mode composite    depth composite: an occluder in front of every splat -> (0, 0, 0, T = 1) everywhere; one
//                       behind every splat == no depth image (bit for bit); the half-image occluder leaves the
//                       uncovered half bit-identical to the no-depth frame and matches the reference where covered
//   --mode determinism  the same frame 4 times (another camera in between, then on a re-initialised renderer):
//                       output, keys and values bit-identical
//   --mode zero_alloc   48 steady-state frames with a moving camera: 0 operator-new calls in beginFrame, the graph
//                       build and the gsplat.* pass callbacks (incl. the inline radix sort; validation off for the
//                       count, a validated run first)
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/gsplat/gsplat.hpp>
#include <fuse/renderer/gsplat/gsplat_reference.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include "test_rp_gsplat_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::gsplat;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kSections = 7u; ///< GsCopySource count
constexpr u32 kMaxSplats = 4096u;
constexpr u32 kCapacity = 1u << 16;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                     data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

u64 align256(u64 v) { return (v + 255u) & ~u64{255u}; }

// --- Vulkan context -------------------------------------------------------------------------------
struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    BindlessDescriptors bindless;
    u32 w = 0;
    u32 h = 0;
    Texture depth{};
    u32 depthLayout = 0;
    u8 depthQueue = rg::kNoQueue;
    Buffer staging{};
    Buffer readback{};
    u8 stagingQueue = rg::kNoQueue;
    u8 readbackQueue = rg::kNoQueue;
    u64 serial = 0;

    void destroyTargets() {
        if (allocator == nullptr) {
            return;
        }
        if (depth.image != nullptr) {
            allocator->destroyImage(depth);
        }
        depth = Texture{};
        for (Buffer* b : {&staging, &readback}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
            *b = Buffer{};
        }
        depthLayout = 0;
        depthQueue = stagingQueue = readbackQueue = rg::kNoQueue;
    }

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        destroyTargets();
        if (device != nullptr) {
            bindless.collectRetired(~0ull);
            bindless.destroy(*device);
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

int setup(Context& ctx, bool descriptorBuffer, bool validation) {
    if (validation) {
        if (!layerAvailable(kValidationLayer)) {
            std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
            return kSkip;
        }
        setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        setenv("VK_INSTANCE_LAYERS", "", 1);
    }
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_gsplat";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr && validation) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    if (createMessenger != nullptr) {
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    if (descriptorBuffer && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const GsCapabilities caps = queryGsplatCapabilities(ctx.device.get());
    if (!caps.gsplat) {
        std::printf("SKIP: splat passes unsupported: %s\n", caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    std::printf("bindless backend: %s\n", bindlessBackendName(ctx.bindless.backend()));
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

/// Read-back offset of a section (GsCopySource order) for the renderer's current sizes.
u64 readbackOffset(const GsplatRenderer& gs, u32 section) {
    u64 at = 0;
    for (u32 i = 0; i < section; ++i) {
        at += align256(gs.copyBytes(static_cast<GsCopySource>(i)));
    }
    return at;
}

u64 readbackBytes(u32 w, u32 h) {
    const u64 tiles = u64{(w + kGsTile - 1u) / kGsTile} * ((h + kGsTile - 1u) / kGsTile);
    return align256(u64{kMaxSplats} * 64u) + align256(u64{kMaxSplats} * 4u) + 256u + align256(u64{kCapacity} * 8u) +
           align256(u64{kCapacity} * 4u) + align256(tiles * 8u) + align256(u64{w} * h * 16u);
}

bool ensureTargets(Context& ctx, u32 w, u32 h) {
    if (ctx.w == w && ctx.h == h && ctx.depth.image != nullptr) {
        return true;
    }
    vkDeviceWaitIdle(ctx.vkDevice);
    ctx.destroyTargets();
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.format = GpuFormat::R32Sfloat; // WP-1.4 kVisExportDepthFormat
    d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
    d.name = "rp_gsplat.vis_depth";
    bool ok = ctx.allocator->createImage(d, ctx.depth);
    auto buffer = [&](Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
        BufferDesc bd{};
        bd.size = static_cast<usize>(size);
        bd.usage = usage;
        bd.memoryUsage = memory;
        bd.name = name;
        return ctx.allocator->createBuffer(bd, b) && b.mapped != nullptr;
    };
    ok = ok && buffer(ctx.staging, u64{w} * h * 4u, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_gsplat.staging") &&
         buffer(ctx.readback, readbackBytes(w, h), BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "rp_gsplat.readback");
    ctx.w = w;
    ctx.h = h;
    return ok;
}

// --- per-frame graph ------------------------------------------------------------------------------
struct TestRecord {
    Context* ctx = nullptr;
    rg::BufferRef staging;
    rg::TextureRef depth;
};
TestRecord g_record{};

void recordUpload(const rg::PassContext& pc, void* user) {
    const TestRecord& r = *static_cast<const TestRecord*>(user);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.ctx->w, r.ctx->h, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(r.staging)),
                           static_cast<VkImage>(pc.image(r.depth)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void buildGraph(Context& ctx, GsplatRenderer& gs, rg::Graph& graph, bool useDepth, bool readback) {
    graph.reset();
    TestRecord& r = g_record;
    r = TestRecord{};
    r.ctx = &ctx;
    if (useDepth) {
        rg::ImportedImage im{};
        im.image = ctx.depth.image;
        im.view = ctx.depth.view;
        im.format = static_cast<u32>(GpuFormat::R32Sfloat);
        im.width = ctx.w;
        im.height = ctx.h;
        im.initialLayout = ctx.depthLayout;
        im.initialQueue = ctx.depthQueue;
        im.layoutTracker = &ctx.depthLayout;
        im.queueTracker = &ctx.depthQueue;
        im.name = "rp_gsplat.vis_depth";
        r.depth = graph.importImage(im);
        r.staging = graph.importBuffer(
            rg::ImportedBuffer{ctx.staging.handle, ctx.staging.desc.size, ctx.stagingQueue, &ctx.stagingQueue, "rp_gsplat.staging"});
        graph.addPass("test.upload", &recordUpload, &r)
            .use(r.staging, rg::Access::TransferSrc)
            .use(r.depth, rg::Access::TransferDst);
    }
    const GsGraphRefs refs = gs.importInto(graph);
    gs.addPasses(graph, refs, r.depth);
    if (readback) {
        const rg::BufferRef rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size,
                                                                       ctx.readbackQueue, &ctx.readbackQueue, "rp_gsplat.readback"});
        for (u32 s = 0; s < kSections; ++s) {
            gs.addCopy(graph, refs, static_cast<GsCopySource>(s), rb, readbackOffset(gs, s));
        }
    }
}

struct Captured {
    std::vector<GsProjected> projected;
    std::vector<u32> offsets;
    u32 counters[4] = {};
    std::vector<u64> keys;
    std::vector<u32> values;
    std::vector<u32> ranges;
    std::vector<f32> image;
};

template <typename T>
void readSection(const Context& ctx, const GsplatRenderer& gs, GsCopySource s, std::vector<T>& out) {
    const u64 bytes = gs.copyBytes(s);
    out.resize(static_cast<usize>(bytes / sizeof(T)));
    std::memcpy(out.data(), static_cast<const u8*>(ctx.readback.mapped) + readbackOffset(gs, static_cast<u32>(s)),
                static_cast<usize>(bytes));
}

/// One frame (depth: the uploaded composite depth or null). Returns false on a failed frame.
bool runFrame(Context& ctx, GsplatRenderer& gs, rg::Graph& graph, const GsCamera& camera, const GsSettings& settings,
              const std::vector<f32>* depth, Captured* out) {
    if (!ensureTargets(ctx, ctx.w, ctx.h)) {
        return false;
    }
    if (depth != nullptr) {
        std::memcpy(ctx.staging.mapped, depth->data(), depth->size() * 4u);
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!gs.beginFrame(ctx.serial, camera, settings, ctx.w, ctx.h, depth != nullptr ? &ctx.depth : nullptr)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    buildGraph(ctx, gs, graph, depth != nullptr, out != nullptr);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    gs.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    if (out != nullptr) {
        readSection(ctx, gs, GsCopySource::Projected, out->projected);
        readSection(ctx, gs, GsCopySource::Offsets, out->offsets);
        std::memcpy(out->counters, static_cast<const u8*>(ctx.readback.mapped) + readbackOffset(gs, 2u), 16u);
        readSection(ctx, gs, GsCopySource::Keys, out->keys);
        readSection(ctx, gs, GsCopySource::Values, out->values);
        readSection(ctx, gs, GsCopySource::Ranges, out->ranges);
        readSection(ctx, gs, GsCopySource::Output, out->image);
    }
    return true;
}

struct Lang {
    GsKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{GsKernelLanguage::Slang, "slang"}, {GsKernelLanguage::Glsl, "glsl"}};

bool initRenderer(Context& ctx, GsplatRenderer& gs, GsKernelLanguage language, const GsAsset& asset) {
    GsplatDesc desc{};
    desc.device = ctx.device.get();
    desc.allocator = ctx.allocator.get();
    desc.bindless = &ctx.bindless;
    desc.language = language;
    desc.maxSplats = kMaxSplats;
    desc.maxEntries = kCapacity;
    return gs.init(desc) && gs.setSplats(asset.splats.data(), static_cast<u32>(asset.splats.size()), asset.shDegree);
}

f32 relErr(f32 a, f32 b) { return std::fabs(a - b) / std::max(1e-6f, std::max(std::fabs(a), std::fabs(b))); }

// --- passes ----------------------------------------------------------------------------------------
int runPasses(Context& ctx) {
    const GsAsset asset = gs_test::makeScene(2000, 3, 21u);
    ctx.w = 157;
    ctx.h = 117;
    if (!ensureTargets(ctx, ctx.w, ctx.h)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        GsplatRenderer gs;
        if (!initRenderer(ctx, gs, lang.language, asset)) {
            std::printf("  [%s] kernels not built: skipped\n", lang.name);
            continue;
        }
        ++built;
        rg::Graph graph;
        for (const f32 angle : {0.35f, 2.2f}) {
            const GsCamera cam = gs_test::orbitCamera(ctx.w, ctx.h, angle);
            const std::vector<f32> depth = gs_test::occluderDepth(cam, ctx.w, ctx.h, gs_test::kOrbit, 0.5f);
            Captured gpu;
            if (!runFrame(ctx, gs, graph, cam, GsSettings{}, &depth, &gpu)) {
                expect(false, "frame ran");
                continue;
            }
            const GsFrameConstants& f = gs.constants();
            GsReferenceFrame ref;
            gs_render_reference(asset.splats.data(), f, depth.data(), ref);
            // preprocess
            u32 intMismatch = 0;
            u32 visible = 0;
            f32 maxRel = 0.f;
            for (usize i = 0; i < ref.projected.size(); ++i) {
                const GsProjected& a = gpu.projected[i];
                const GsProjected& b = ref.projected[i];
                bool same = a.radius == b.radius && std::memcmp(a.rect, b.rect, sizeof(a.rect)) == 0 && a.viewZ == b.viewZ;
                intMismatch += same ? 0u : 1u;
                visible += b.radius > 0u ? 1u : 0u;
                const f32 fa[] = {a.mean[0], a.mean[1], a.depth, a.conic[0], a.conic[1], a.conic[2], a.opacity,
                                  a.color[0], a.color[1], a.color[2]};
                const f32 fb[] = {b.mean[0], b.mean[1], b.depth, b.conic[0], b.conic[1], b.conic[2], b.opacity,
                                  b.color[0], b.color[1], b.color[2]};
                for (u32 k = 0; k < 10u; ++k) {
                    maxRel = std::max(maxRel, relErr(fa[k], fb[k]));
                }
            }
            // scan / emit / sort / ranges
            const bool offsetsOk = gpu.offsets == ref.offsets;
            const bool countersOk = gpu.counters[0] == ref.entries && gpu.counters[1] == ref.dropped;
            const bool keysOk = gpu.keys == ref.keys;
            const bool valuesOk = gpu.values == ref.values;
            const bool rangesOk = gpu.ranges == ref.ranges;
            // raster, isolated: the reference raster over the GPU's own preprocess / sort output.
            f64 rasterMax = 0.0;
            u32 rasterOff = 0;
            std::vector<f32> iso(gpu.image.size());
            for (u32 y = 0; y < ctx.h; ++y) {
                for (u32 x = 0; x < ctx.w; ++x) {
                    const u32 tile = (y / kGsTile) * f.tilesX + x / kGsTile;
                    const usize p = static_cast<usize>(y) * ctx.w + x;
                    f32 px[4];
                    gs_raster_pixel(gpu.projected.data(), gpu.values.data(), gpu.ranges[tile * 2u],
                                    gpu.ranges[tile * 2u + 1u], x, y, depth[p], f, px);
                    f64 d = 0.0;
                    for (u32 c = 0; c < 4u; ++c) {
                        iso[p * 4u + c] = px[c];
                        d = std::max(d, f64(std::fabs(px[c] - gpu.image[p * 4u + c])));
                    }
                    rasterMax = std::max(rasterMax, d);
                    rasterOff += d > 1e-4 ? 1u : 0u;
                }
            }
            const usize pixels = static_cast<usize>(ctx.w) * ctx.h;
            const f64 psnrIso = gs_psnr(gpu.image.data(), iso.data(), pixels);
            const f64 psnr = gs_psnr(gpu.image.data(), ref.image.data(), pixels);
            f64 meanT = 0.0;
            for (usize p = 0; p < pixels; ++p) {
                meanT += gpu.image[p * 4u + 3u];
            }
            meanT /= f64(pixels);
            std::printf("  [%s] angle %.2f: %u / %zu visible, %u entries (capacity %u), key bits %u (%u radix passes), "
                        "mean T %.3f\n",
                        lang.name, f64(angle), visible, ref.projected.size(), gpu.counters[0], f.capacity,
                        gs.stats().keyBits, gs.stats().sortPasses, meanT);
            std::printf("    preprocess: %u integer / view-z mismatches, max rel float err %.3g; offsets %s, counters %s, "
                        "keys %s, values %s, ranges %s\n",
                        intMismatch, f64(maxRel), offsetsOk ? "==" : "!=", countersOk ? "==" : "!=", keysOk ? "==" : "!=",
                        valuesOk ? "==" : "!=", rangesOk ? "==" : "!=");
            std::printf("    raster (isolated): max |diff| %.3g, %u px > 1e-4, PSNR %.1f dB; image vs CPU reference: "
                        "PSNR %.1f dB\n",
                        rasterMax, rasterOff, psnrIso, psnr);
            expect(visible > 1000u && gpu.counters[0] > 4000u, "scene is non-trivial");
            expect(intMismatch == 0u, "preprocess radius / tile rect / view z == reference");
            expect(maxRel <= 2e-6f, "preprocess floats within 2e-6 relative of the reference");
            expect(offsetsOk && countersOk, "scan offsets / counters == reference");
            expect(keysOk && valuesOk, "emit + radix sort keys / values == reference stable sort");
            expect(rangesOk, "tile ranges == reference");
            expect(rasterOff <= static_cast<u32>(pixels / 1000u) && psnrIso >= 80.0, "raster == gs_raster_pixel (isolated)");
            expect(psnr >= 60.0, "splat image vs CPU reference PSNR >= 60 dB");
        }
        gs.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no splat kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- composite -------------------------------------------------------------------------------------
int runComposite(Context& ctx) {
    const GsAsset asset = gs_test::makeScene(1500, 2, 5u);
    ctx.w = 128;
    ctx.h = 96;
    if (!ensureTargets(ctx, ctx.w, ctx.h)) {
        return 1;
    }
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        GsplatRenderer gs;
        if (!initRenderer(ctx, gs, lang.language, asset)) {
            continue;
        }
        ++built;
        rg::Graph graph;
        const GsCamera cam = gs_test::orbitCamera(ctx.w, ctx.h, 0.8f);
        const usize pixels = static_cast<usize>(ctx.w) * ctx.h;
        Captured none, front, behind, half;
        const std::vector<f32> dFront = gs_test::occluderDepth(cam, ctx.w, ctx.h, 0.5f, 1.f);
        const std::vector<f32> dBehind = gs_test::occluderDepth(cam, ctx.w, ctx.h, 60.f, 1.f);
        const std::vector<f32> dHalf = gs_test::occluderDepth(cam, ctx.w, ctx.h, gs_test::kOrbit, 0.5f);
        bool ok = runFrame(ctx, gs, graph, cam, GsSettings{}, nullptr, &none) &&
                  runFrame(ctx, gs, graph, cam, GsSettings{}, &dFront, &front) &&
                  runFrame(ctx, gs, graph, cam, GsSettings{}, &dBehind, &behind) &&
                  runFrame(ctx, gs, graph, cam, GsSettings{}, &dHalf, &half);
        expect(ok, "composite frames ran");
        if (!ok) {
            continue;
        }
        bool frontEmpty = true;
        for (usize p = 0; p < pixels; ++p) {
            frontEmpty = frontEmpty && front.image[p * 4u] == 0.f && front.image[p * 4u + 1u] == 0.f &&
                         front.image[p * 4u + 2u] == 0.f && front.image[p * 4u + 3u] == 1.f;
        }
        const bool behindSame = behind.image == none.image;
        u32 uncoveredDiff = 0;
        u32 covered = 0;
        u32 hidden = 0;
        for (usize p = 0; p < pixels; ++p) {
            if (dHalf[p] == 1.f) {
                uncoveredDiff += std::memcmp(&half.image[p * 4u], &none.image[p * 4u], 16u) != 0 ? 1u : 0u;
            } else {
                ++covered;
                hidden += half.image[p * 4u + 3u] > none.image[p * 4u + 3u] ? 1u : 0u;
            }
        }
        GsReferenceFrame ref;
        gs_render_reference(asset.splats.data(), gs.constants(), dHalf.data(), ref);
        const f64 psnr = gs_psnr(half.image.data(), ref.image.data(), pixels);
        std::printf("  [%s] occluder in front: %s; behind == no depth: %s; half occluder: %u uncovered px differ, "
                    "%u / %u covered px more transparent, PSNR vs reference %.1f dB\n",
                    lang.name, frontEmpty ? "empty" : "NOT EMPTY", behindSame ? "yes" : "NO", uncoveredDiff, hidden, covered,
                    psnr);
        expect(frontEmpty, "occluder in front of every splat -> (0, 0, 0, T = 1)");
        expect(behindSame, "occluder behind every splat == no depth image (bit for bit)");
        expect(uncoveredDiff == 0u, "uncovered half bit-identical to the no-depth frame");
        expect(hidden > covered / 4u, "the occluder hides the splats behind it");
        expect(psnr >= 60.0, "composited image vs CPU reference PSNR >= 60 dB");
        gs.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no splat kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- determinism -----------------------------------------------------------------------------------
int runDeterminism(Context& ctx) {
    const GsAsset asset = gs_test::makeScene(2500, 3, 99u);
    ctx.w = 144;
    ctx.h = 100;
    if (!ensureTargets(ctx, ctx.w, ctx.h)) {
        return 1;
    }
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        GsplatRenderer gs;
        if (!initRenderer(ctx, gs, lang.language, asset)) {
            continue;
        }
        ++built;
        rg::Graph graph;
        const GsCamera camA = gs_test::orbitCamera(ctx.w, ctx.h, 1.1f);
        const GsCamera camB = gs_test::orbitCamera(ctx.w, ctx.h, -0.7f);
        const std::vector<f32> depth = gs_test::occluderDepth(camA, ctx.w, ctx.h, gs_test::kOrbit, 0.4f);
        Captured a0, b, a1, a2, a3;
        bool ok = runFrame(ctx, gs, graph, camA, GsSettings{}, &depth, &a0) &&
                  runFrame(ctx, gs, graph, camB, GsSettings{}, &depth, &b) &&
                  runFrame(ctx, gs, graph, camA, GsSettings{}, &depth, &a1) &&
                  runFrame(ctx, gs, graph, camA, GsSettings{}, &depth, &a2);
        gs.destroy();
        ok = ok && initRenderer(ctx, gs, lang.language, asset) && runFrame(ctx, gs, graph, camA, GsSettings{}, &depth, &a3);
        expect(ok, "determinism frames ran");
        if (!ok) {
            continue;
        }
        bool same = true;
        for (const Captured* c : {&a1, &a2, &a3}) {
            same = same && c->image == a0.image && c->keys == a0.keys && c->values == a0.values && c->ranges == a0.ranges;
        }
        std::printf("  [%s] 4 runs of one frame (another camera between, a fresh renderer last): %s; other camera "
                    "differs: %s\n",
                    lang.name, same ? "bit-identical" : "DIFFER", b.image != a0.image ? "yes" : "no");
        expect(same, "output, keys, values and ranges are bit-identical across runs");
        expect(b.image != a0.image, "the second camera renders a different image");
        gs.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no splat kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "gsplat.", 7) == 0) {
        t_inPass = true;
        t_count = true;
    }
}

void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, bool countAllocations) {
    const GsAsset asset = gs_test::makeScene(1500, 3, 3u);
    ctx.w = 96;
    ctx.h = 64;
    if (!ensureTargets(ctx, ctx.w, ctx.h)) {
        return 1;
    }
    GsplatRenderer gs;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initRenderer(ctx, gs, lang.language, asset)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no splat kernel built\n");
        return kSkip;
    }
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 64;
    unsigned long long side = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    const GsCamera first = gs_test::orbitCamera(ctx.w, ctx.h, 0.f);
    const std::vector<f32> depth = gs_test::occluderDepth(first, ctx.w, ctx.h, gs_test::kOrbit, 0.5f);
    std::memcpy(ctx.staging.mapped, depth.data(), depth.size() * 4u);
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        const GsCamera cam = gs_test::orbitCamera(ctx.w, ctx.h, 0.05f * static_cast<f32>(frame));
        GsSettings settings{};
        settings.shDegree = frame % 4u;
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        const bool begun = gs.beginFrame(ctx.serial, cam, settings, ctx.w, ctx.h, &ctx.depth);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, gs, graph, true, false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        gs.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(gs.stats().passes == 6u, "every pass runs");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = gs.stats().imageRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(gs.stats().imageRebuilds == rebuilds && rebuilds == 1u, "no buffer rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; 1500 splats, SH degree cycling 0..3, camera orbiting,\n"
                    "  depth composite)\n"
                    "  GsplatRenderer::beginFrame + gsplat.* pass callbacks (incl. the inline radix sort): %llu "
                    "operator-new calls (callbacks %llu)\n"
                    "  whole graph build (test upload + gsplat imports and passes): %llu\n",
                    kTotal - kWarmup, gs.kernelLanguage(), side, callbacks, build);
        expect(side == 0u, "the splat passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the splat passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    gs.kernelLanguage());
    }
    gs.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "passes";
    std::string backend = "set";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            backend = argv[i + 1];
        }
    }
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, backend == "buffer", true);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "passes") {
            rc = runPasses(ctx);
        } else if (mode == "composite") {
            rc = runComposite(ctx);
        } else if (mode == "determinism") {
            rc = runDeterminism(ctx);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, false);
        } else {
            std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
            return 2;
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (mode == "zero_alloc" && rc == 0) {
        Context counted;
        const int setupRc = setup(counted, backend == "buffer", false);
        if (setupRc != 0) {
            return setupRc;
        }
        rc = runZeroAlloc(counted, true);
    }
    std::printf("validation messages: %u\n", g_messages);
    if (rc != 0 || g_failures != 0 || g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %d failure(s), %u validation message(s)\n", g_failures + rc, g_messages);
        return 1;
    }
    std::printf("PASS %s (%s)\n", mode.c_str(), backend.c_str());
    return 0;
}

#endif
