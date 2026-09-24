// WP-6.3 screen-space fallback Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). CPU gates: test_rp_ssfx_gpu_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): a test pass uploads the WP-1.5 G-buffer attachments the passes
// read (RT0 RGBA16F signed-octahedral normal + AO, RT1 / RT2 RGBA8, RT4 R32F forward z/w depth) and the WP-2.1
// lit image (RGBA16F) from a staging buffer, then SsfxGpu adds its passes, then read-back copies (every
// work-buffer section, the output image). The scenes are ray cast on the CPU (test_rp_ssfx_gpu_scene.hpp), so
// the texels are known exactly. Both kernel languages built (Slang, GLSL) run every check.
//
// Oracles: each GPU pass against its single-source CPU kernel, run on the GPU's own read-back inputs (so a
// gate isolates one pass):
//   --mode passes  the room scene (97 x 71, odd extent; 2 camera positions; 2 settings: GTAO + SSR with
//                  roughness gate and contact hardening + 2-bounce SSGI, then jittered GTAO with falloff +
//                  multi-bounce + mirror SSR without contact hardening + 1-bounce SSGI):
//                    ssfx.prepare  == ssfx_gpu::prepare_pixel on the uploaded texels
//                    ssfx.gtao     == fuse::ssfx::gtao_kernel (bit for bit: IEEE-exact operations only)
//                    ssfx.ssr      == fuse::ssfx::ssr_kernel (same hits; colour bit for bit; confidence within
//                                     1e-5 relative: contact hardening's pow is a device approximation)
//                    ssfx.ssgi     == computeSsgiCpu (all bounces), statistical gate (below)
//                    ssfx.compose  == ssfx_gpu::compose_pixel on the GPU's effect outputs (bit for bit)
//                  plus the whole chain == reference_frame, and the RGBA16F output == the f32 dump converted to half
//                  (<= 1 half ulp: the device conversion may truncate).
//   --mode analytic GTAO through the whole GPU path (engine G-buffer -> prepare -> gtao) on a plane (exactly 1)
//                  and at the crease of 90 / 120 degree wedges ((1 - cos alpha) / 2), == the CPU kernel.
//   --mode zero_alloc  64 steady-state frames (every pass, 2 SSGI bounces, camera moving): 0 operator-new calls
//                  in SsfxGpu::beginFrame, the ssfx.* pass callbacks and the whole graph build (validated run
//                  first; validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// SSGI tolerance: the gather directions use cos / sin of a hashed angle (the oracle's libm; the device's
// cos / sin differ by a few ulp, Vulkan allows 2^-11 absolute), so a perturbed ray can step onto a neighbouring
// depth pixel near a silhouette and hit another surface: one of sample_sqrt^2 rays changes. The gate: at least
// 99% of pixels within 1e-4 relative (+ 1e-5 absolute) and the frame mean within 1e-3 relative.
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/ssfx/gtao_kernel.hpp>

#include "test_rp_ssfx_gpu_scene.hpp"

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

// GCC may inline the replacement operators into callers and then report a false
// -Wmismatched-new-delete at -O2; replacement functions must not be inline anyway.
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
using namespace fuse::renderer::ssfx_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kInputCount = 5u; ///< RT0, RT1, RT2, RT4, lit
constexpr u32 kSectionCount = 8u;

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

f32 halfToFloat(u16 h) { return GBufferQuantize::halfToFloat(h); }
f32 roundHalf(f32 v) { return GBufferQuantize::toHalf(v); }

/// |image - dump| within one half-float ulp of the dump value (the device's f32 -> f16 store conversion may
/// truncate instead of rounding to nearest: Vulkan leaves the rounding mode of the conversion open).
bool withinHalfUlp(f32 image, f32 dump) {
    const u16 bits = GBufferQuantize::floatToHalf(dump);
    const f32 lo = halfToFloat(GBufferQuantize::halfNextDown(bits));
    const f32 hi = halfToFloat(GBufferQuantize::halfNextUp(bits));
    return image >= std::min(lo, hi) && image <= std::max(lo, hi);
}

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
    Texture images[kInputCount]{};
    u32 layouts[kInputCount] = {};
    u8 queues[kInputCount] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
    Buffer staging{};
    Buffer readback{};
    Buffer dump{};
    u8 stagingQueue = rg::kNoQueue;
    u8 readbackQueue = rg::kNoQueue;
    u8 dumpQueue = rg::kNoQueue;
    u64 serial = 0;

    void destroyTargets() {
        if (allocator == nullptr) {
            return;
        }
        for (Texture& t : images) {
            if (t.image != nullptr) {
                allocator->destroyImage(t);
            }
            t = Texture{};
        }
        for (Buffer* b : {&staging, &readback, &dump}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
            *b = Buffer{};
        }
        for (u32 i = 0; i < kInputCount; ++i) {
            layouts[i] = 0u;
            queues[i] = rg::kNoQueue;
        }
        stagingQueue = readbackQueue = dumpQueue = rg::kNoQueue;
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
    instanceDesc.appName = "fuse_rp_ssfx_gpu";
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
    const SsfxCapabilities caps = querySsfxCapabilities(ctx.device.get());
    if (!caps.ssfx) {
        std::printf("SKIP: screen-space passes unsupported: %s\n", caps.reason);
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

// Input order: RT0, RT1, RT2, RT4, lit.
constexpr GpuFormat kFormats[kInputCount] = {GpuFormat::R16G16B16A16Sfloat, GpuFormat::R8G8B8A8Unorm,
                                             GpuFormat::R8G8B8A8Unorm, GpuFormat::R32Sfloat,
                                             GpuFormat::R16G16B16A16Sfloat};
constexpr u64 kTexelBytes[kInputCount] = {8u, 4u, 4u, 4u, 8u};
const char* const kImageNames[kInputCount] = {"rp_ssfx.rt0", "rp_ssfx.rt1", "rp_ssfx.rt2", "rp_ssfx.rt4", "rp_ssfx.lit"};

u64 align256(u64 v) { return (v + 255u) & ~u64{255u}; }

u64 stagingOffset(u32 w, u32 h, u32 image) {
    u64 at = 0;
    for (u32 i = 0; i < image; ++i) {
        at += align256(static_cast<u64>(w) * h * kTexelBytes[i]);
    }
    return at;
}

/// Read-back: the 8 sections (SsfxCopySource order) then the output image (RGBA16F).
u64 readbackOffset(u32 w, u32 h, u32 section) {
    const u64 n = static_cast<u64>(w) * h;
    u64 at = 0;
    for (u32 i = 0; i < section; ++i) {
        at += align256(i == static_cast<u32>(SsfxCopySource::Ao) ? n * 4u : n * 16u);
    }
    return at;
}

bool ensureTargets(Context& ctx, u32 w, u32 h) {
    if (ctx.w == w && ctx.h == h && ctx.images[0].image != nullptr) {
        return true;
    }
    vkDeviceWaitIdle(ctx.vkDevice);
    ctx.destroyTargets();
    bool ok = true;
    for (u32 i = 0; i < kInputCount; ++i) {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.format = kFormats[i];
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        d.name = kImageNames[i];
        ok = ok && ctx.allocator->createImage(d, ctx.images[i]);
    }
    auto buffer = [&](Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = static_cast<usize>(size);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = name;
        return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
    };
    const u64 n = static_cast<u64>(w) * h;
    ok = ok && buffer(ctx.staging, stagingOffset(w, h, kInputCount), BufferUsage::TransferSrc, MemoryUsage::CpuToGpu,
                      "rp_ssfx.staging") &&
         buffer(ctx.readback, readbackOffset(w, h, kSectionCount) + n * 8u, BufferUsage::TransferDst,
                MemoryUsage::GpuToCpu, "rp_ssfx.readback") &&
         buffer(ctx.dump, n * 16u,
                static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                         static_cast<u32>(BufferUsage::ShaderDeviceAddress)),
                MemoryUsage::GpuToCpu, "rp_ssfx.dump") &&
         ctx.dump.deviceAddress != 0u;
    ctx.w = w;
    ctx.h = h;
    return ok;
}

u8 unorm8Byte(f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

void writeStaging(Context& ctx, const ssfx_test::GBuffer& g) {
    u8* base = static_cast<u8*>(ctx.staging.mapped);
    const usize n = static_cast<usize>(g.width) * g.height;
    u8* rt0 = base + stagingOffset(g.width, g.height, 0);
    u8* rt1 = base + stagingOffset(g.width, g.height, 1);
    u8* rt2 = base + stagingOffset(g.width, g.height, 2);
    u8* rt4 = base + stagingOffset(g.width, g.height, 3);
    u8* lit = base + stagingOffset(g.width, g.height, 4);
    for (usize i = 0; i < n; ++i) {
        const u16 t0[4] = {ssfx_test::halfBits(g.rt0[i].x), ssfx_test::halfBits(g.rt0[i].y), ssfx_test::halfBits(g.rt0[i].z),
                           ssfx_test::halfBits(g.rt0[i].w)};
        std::memcpy(rt0 + i * 8u, t0, 8u);
        const u8 t1[4] = {unorm8Byte(g.rt1[i].x), unorm8Byte(g.rt1[i].y), unorm8Byte(g.rt1[i].z), unorm8Byte(g.rt1[i].w)};
        std::memcpy(rt1 + i * 4u, t1, 4u);
        const u8 t2[4] = {unorm8Byte(g.rt2[i].x), unorm8Byte(g.rt2[i].y), unorm8Byte(g.rt2[i].z), unorm8Byte(g.rt2[i].w)};
        std::memcpy(rt2 + i * 4u, t2, 4u);
        std::memcpy(rt4 + i * 4u, &g.rt4[i], 4u);
        const u16 tl[4] = {ssfx_test::halfBits(g.lit[i].x), ssfx_test::halfBits(g.lit[i].y), ssfx_test::halfBits(g.lit[i].z),
                           ssfx_test::halfBits(g.lit[i].w)};
        std::memcpy(lit + i * 8u, tl, 8u);
    }
}

// --- per-frame graph ------------------------------------------------------------------------------
struct TestRecord {
    Context* ctx = nullptr;
    rg::BufferRef staging;
    rg::BufferRef readback;
    rg::TextureRef images[kInputCount];
    rg::TextureRef output;
};

TestRecord g_record{};

void recordUpload(const rg::PassContext& pc, void* user) {
    const TestRecord& r = *static_cast<const TestRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    for (u32 i = 0; i < kInputCount; ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset = stagingOffset(r.ctx->w, r.ctx->h, i);
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {r.ctx->w, r.ctx->h, 1};
        vkCmdCopyBufferToImage(cmd, static_cast<VkBuffer>(pc.buffer(r.staging)), static_cast<VkImage>(pc.image(r.images[i])),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
}

void recordImageReadback(const rg::PassContext& pc, void* user) {
    const TestRecord& r = *static_cast<const TestRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    VkBufferImageCopy region{};
    region.bufferOffset = readbackOffset(r.ctx->w, r.ctx->h, kSectionCount);
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.ctx->w, r.ctx->h, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(r.output)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VkBuffer>(pc.buffer(r.readback)), 1, &region);
}

/// Imports the test images / buffers, adds the upload, SsfxGpu's passes and (optionally) the read-backs.
void buildGraph(Context& ctx, SsfxGpu& ssfx, rg::Graph& graph, bool readback) {
    graph.reset();
    TestRecord& r = g_record;
    r = TestRecord{};
    r.ctx = &ctx;
    for (u32 i = 0; i < kInputCount; ++i) {
        rg::ImportedImage im{};
        im.image = ctx.images[i].image;
        im.view = ctx.images[i].view;
        im.format = static_cast<u32>(kFormats[i]);
        im.width = ctx.w;
        im.height = ctx.h;
        im.initialLayout = ctx.layouts[i];
        im.initialQueue = ctx.queues[i];
        im.layoutTracker = &ctx.layouts[i];
        im.queueTracker = &ctx.queues[i];
        im.name = kImageNames[i];
        r.images[i] = graph.importImage(im);
    }
    r.staging = graph.importBuffer(
        rg::ImportedBuffer{ctx.staging.handle, ctx.staging.desc.size, ctx.stagingQueue, &ctx.stagingQueue, "rp_ssfx.staging"});
    rg::PassBuilder up = graph.addPass("test.upload", &recordUpload, &r);
    up.use(r.staging, rg::Access::TransferSrc);
    for (const rg::TextureRef& t : r.images) {
        up.use(t, rg::Access::TransferDst);
    }
    const SsfxGraphRefs refs = ssfx.importInto(graph);
    r.output = refs.output;
    SsfxGraphInputs inputs{};
    inputs.normalAo = r.images[0];
    inputs.albedo = r.images[1];
    inputs.roughMetal = r.images[2];
    inputs.depth = r.images[3];
    inputs.lit = r.images[4];
    if (readback) {
        inputs.dump =
            graph.importBuffer(rg::ImportedBuffer{ctx.dump.handle, ctx.dump.desc.size, ctx.dumpQueue, &ctx.dumpQueue, "rp_ssfx.dump"});
    }
    ssfx.addPasses(graph, refs, inputs);
    if (readback) {
        r.readback = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, ctx.readbackQueue,
                                                           &ctx.readbackQueue, "rp_ssfx.readback"});
        for (u32 s = 0; s < kSectionCount; ++s) {
            ssfx.addCopy(graph, refs, static_cast<SsfxCopySource>(s), r.readback, readbackOffset(ctx.w, ctx.h, s));
        }
        graph.addPass("test.readback_image", &recordImageReadback, &r)
            .use(refs.output, rg::Access::TransferSrc)
            .use(r.readback, rg::Access::TransferDst,
                 rg::BufferRange{readbackOffset(ctx.w, ctx.h, kSectionCount), static_cast<u64>(ctx.w) * ctx.h * 8u});
    }
}

struct Captured {
    SsfxPreparedFrame prepared;
    std::vector<f32> ao;
    std::vector<Vec4> ssr;
    std::vector<Vec3> gi;
    std::vector<Vec4> dump;
    std::vector<Vec4> image;
};

SsfxFrameImages frameImages(Context& ctx, bool dump) {
    SsfxFrameImages images{};
    images.normalAo = &ctx.images[0];
    images.albedo = &ctx.images[1];
    images.roughMetal = &ctx.images[2];
    images.depth = &ctx.images[3];
    images.lit = &ctx.images[4];
    images.dumpAddress = dump ? ctx.dump.deviceAddress : 0u;
    return images;
}

bool runFrame(Context& ctx, SsfxGpu& ssfx, rg::Graph& graph, const SsfxGpuSettings& settings, const SsfxCameraDesc& camera,
              const f32 (&ambient)[3], Captured& out) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!ssfx.beginFrame(ctx.serial, settings, camera, ambient, frameImages(ctx, true))) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    buildGraph(ctx, ssfx, graph, true);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    ssfx.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    const usize n = static_cast<usize>(ctx.w) * ctx.h;
    const u8* base = static_cast<const u8*>(ctx.readback.mapped);
    auto texels = [&](SsfxCopySource s, usize i) {
        f32 v[4];
        std::memcpy(v, base + readbackOffset(ctx.w, ctx.h, static_cast<u32>(s)) + i * 16u, 16u);
        return Vec4{v[0], v[1], v[2], v[3]};
    };
    SsfxPreparedFrame& p = out.prepared;
    p.width = ctx.w;
    p.height = ctx.h;
    p.prepared.resize(n);
    p.normal.resize(n);
    p.radiance.resize(n);
    p.litAlpha.resize(n);
    p.albedo.resize(n);
    p.diffuse.resize(n);
    out.ao.resize(n);
    out.ssr.resize(n);
    out.gi.resize(n);
    out.dump.resize(n);
    out.image.resize(n);
    const f32* dumpF = static_cast<const f32*>(ctx.dump.mapped);
    const u8* image = base + readbackOffset(ctx.w, ctx.h, kSectionCount);
    for (usize i = 0; i < n; ++i) {
        p.prepared[i] = texels(SsfxCopySource::Prepared, i);
        const Vec4 nrm = texels(SsfxCopySource::Normals, i);
        p.normal[i] = Vec3{nrm.x, nrm.y, nrm.z};
        const Vec4 rad = texels(SsfxCopySource::Radiance, i);
        p.radiance[i] = Vec3{rad.x, rad.y, rad.z};
        p.litAlpha[i] = rad.w;
        const Vec4 alb = texels(SsfxCopySource::Albedo, i);
        p.albedo[i] = Vec3{alb.x, alb.y, alb.z};
        const Vec4 dif = texels(SsfxCopySource::Diffuse, i);
        p.diffuse[i] = Vec3{dif.x, dif.y, dif.z};
        std::memcpy(&out.ao[i], base + readbackOffset(ctx.w, ctx.h, static_cast<u32>(SsfxCopySource::Ao)) + i * 4u, 4u);
        out.ssr[i] = texels(SsfxCopySource::Ssr, i);
        const Vec4 gi = texels(SsfxCopySource::Gi, i);
        out.gi[i] = Vec3{gi.x, gi.y, gi.z};
        out.dump[i] = Vec4{dumpF[i * 4u], dumpF[i * 4u + 1u], dumpF[i * 4u + 2u], dumpF[i * 4u + 3u]};
        u16 hbits[4];
        std::memcpy(hbits, image + i * 8u, 8u);
        out.image[i] = Vec4{halfToFloat(hbits[0]), halfToFloat(hbits[1]), halfToFloat(hbits[2]), halfToFloat(hbits[3])};
    }
    return true;
}

struct Lang {
    SsfxKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{SsfxKernelLanguage::Slang, "slang"}, {SsfxKernelLanguage::Glsl, "glsl"}};

bool initSsfx(Context& ctx, SsfxGpu& ssfx, SsfxKernelLanguage language) {
    SsfxGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    return ssfx.init(d);
}

// --- comparisons ----------------------------------------------------------------------------------
bool sameBits(f32 a, f32 b) { return std::memcmp(&a, &b, 4u) == 0; }

f64 relErr(f32 a, f32 b, f32 absFloor) {
    return std::fabs(static_cast<f64>(a) - b) / std::max(static_cast<f64>(std::fabs(b)), static_cast<f64>(absFloor));
}

// --- passes ----------------------------------------------------------------------------------------
struct Config {
    const char* name;
    SsfxGpuSettings settings;
};

Config configA() {
    Config c{"gtao + ssr(roughness, contact) + ssgi x2", {}};
    c.settings.gtao.radius = 0.8f;
    c.settings.gtao.slices = 6;
    c.settings.gtao.steps = 10;
    c.settings.ssr_params.max_steps = 96;
    c.settings.ssgi_params.sample_sqrt = 3;
    c.settings.ssgi_params.bounces = 2;
    c.settings.ssgi_params.max_steps = 48;
    return c;
}

Config configB() {
    Config c{"jittered gtao (falloff, multi-bounce) + mirror ssr + ssgi x1", {}};
    c.settings.gtao.radius = 1.2f;
    c.settings.gtao.falloff = 0.3f;
    c.settings.gtao.jitter = true;
    c.settings.gtao.frame = 5;
    c.settings.gtao.slices = 4;
    c.settings.multiBounce = true;
    c.settings.ssrRoughness = false;
    c.settings.contact.enabled = false;
    c.settings.ssr_params.thickness = 0.3f;
    c.settings.ssgi_params.sample_sqrt = 2;
    c.settings.ssgi_params.bounces = 1;
    c.settings.ssgi_params.intensity = 1.5f;
    return c;
}

/// All pass checks of one frame. Returns the GPU capture for cross-language comparison.
void checkFrame(const char* lang, const Config& cfg, const ssfx_test::RoomScene& room, const SsfxGpu& ssfx,
                const Captured& gpu) {
    const usize n = gpu.ao.size();
    const SsfxFrameConstants& c = ssfx.constants();
    // prepare
    SsfxPreparedFrame cpuPrep;
    ssfx_test::prepareCpu(c, room.g, cpuPrep);
    f64 prepErr = 0.0;
    u32 prepExact = 0;
    for (usize i = 0; i < n; ++i) {
        const Vec4& a = gpu.prepared.prepared[i];
        const Vec4& b = cpuPrep.prepared[i];
        const f64 e = std::max({relErr(a.x, b.x, 1e-6f), relErr(a.y, b.y, 1e-6f), relErr(a.z, b.z, 1e-6f),
                                relErr(a.w, b.w, 1e-6f),
                                static_cast<f64>((gpu.prepared.normal[i] - cpuPrep.normal[i]).length()),
                                (gpu.prepared.radiance[i] - cpuPrep.radiance[i]).length() /
                                    std::max(1e-6, static_cast<f64>(cpuPrep.radiance[i].length())),
                                static_cast<f64>((gpu.prepared.albedo[i] - cpuPrep.albedo[i]).length()),
                                static_cast<f64>((gpu.prepared.diffuse[i] - cpuPrep.diffuse[i]).length())});
        prepErr = std::max(prepErr, e);
        prepExact += e == 0.0 ? 1u : 0u;
    }
    std::printf("  [%s] %s: prepare vs prepare_pixel: %u / %zu pixels bit-identical, max rel err %.3g\n", lang, cfg.name,
                prepExact, n, prepErr);
    expect(prepErr <= 1e-6, "ssfx.prepare == prepare_pixel within 1e-6 (unorm8 / division rounding)");

    // Effects on the GPU's own prepared inputs.
    SsfxReferenceFrame ref;
    expect(reference_frame(cfg.settings, c, gpu.prepared, ref), "reference_frame");
    u32 aoExact = 0;
    f64 aoErr = 0.0;
    u32 occluded = 0;
    for (usize i = 0; i < n; ++i) {
        aoExact += sameBits(gpu.ao[i], ref.ao[i]) ? 1u : 0u;
        aoErr = std::max(aoErr, std::fabs(static_cast<f64>(gpu.ao[i]) - ref.ao[i]));
        occluded += ref.ao[i] < 0.9f ? 1u : 0u;
    }
    std::printf("  [%s] %s: ssfx.gtao vs gtao_kernel: %u / %zu bit-identical, max |diff| %.3g (%u occluded pixels)\n", lang,
                cfg.name, aoExact, n, aoErr, occluded);
    expect(occluded > 100u, "the frame has occlusion");
    expect(aoErr <= 1e-6, "ssfx.gtao == gtao_kernel (IEEE-exact operations; 1e-6 covers host contraction)");

    u32 hitMismatch = 0;
    u32 hits = 0;
    u32 colourExact = 0;
    f64 confErr = 0.0;
    for (usize i = 0; i < n; ++i) {
        const Vec4& a = gpu.ssr[i];
        const Vec4& b = ref.ssr[i];
        const bool ha = a.w > 0.f;
        const bool hb = b.w > 0.f;
        hits += hb ? 1u : 0u;
        if (ha != hb) {
            ++hitMismatch;
            continue;
        }
        if (!hb) {
            continue;
        }
        colourExact += sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z) ? 1u : 0u;
        confErr = std::max(confErr, relErr(a.w, b.w, 1e-6f));
    }
    std::printf("  [%s] %s: ssfx.ssr vs ssr_kernel: %u hits, %u hit mismatches, %u / %u colours bit-identical, max "
                "confidence rel err %.3g\n",
                lang, cfg.name, hits, hitMismatch, colourExact, hits, confErr);
    expect(hits > 200u, "the frame has SSR hits");
    expect(hitMismatch == 0u && colourExact == hits, "ssfx.ssr: same hits and hit colours as ssr_kernel");
    expect(confErr <= 1e-5, "ssfx.ssr confidence within 1e-5 relative (device pow in contact hardening)");

    u32 giOk = 0;
    f64 giMax = 0.0;
    f64 sumGpu = 0.0;
    f64 sumCpu = 0.0;
    u32 giLit = 0;
    u32 giExact = 0;
    for (usize i = 0; i < n; ++i) {
        const Vec3 d = gpu.gi[i] - ref.gi[i];
        const f64 e = d.length() / (std::max(1e-6, static_cast<f64>(ref.gi[i].length())));
        const f64 ea = d.length();
        giOk += (e <= 1e-4 || ea <= 1e-5) ? 1u : 0u;
        giExact += ea == 0.0 ? 1u : 0u;
        giMax = std::max(giMax, e);
        sumGpu += gpu.gi[i].x + gpu.gi[i].y + gpu.gi[i].z;
        sumCpu += ref.gi[i].x + ref.gi[i].y + ref.gi[i].z;
        giLit += ref.gi[i].length() > 0.f ? 1u : 0u;
    }
    const f64 meanRel = std::fabs(sumGpu - sumCpu) / std::max(1e-9, sumCpu);
    std::printf("  [%s] %s: ssfx.ssgi (%u bounce(s)) vs computeSsgiCpu: %u lit pixels, %u / %zu within 1e-4 rel "
                "(%u bit-identical), worst rel %.3g, frame mean rel diff %.3g\n",
                lang, cfg.name, ssfx.stats().ssgiBounces, giLit, giOk, n, giExact, giMax, meanRel);
    expect(giLit > 200u, "the frame has indirect light");
    expect(static_cast<f64>(giOk) >= 0.99 * static_cast<f64>(n), "ssfx.ssgi: >= 99% of pixels within 1e-4 relative");
    expect(meanRel <= 1e-3, "ssfx.ssgi: frame mean within 1e-3 relative");

    // compose on the GPU's effect outputs: bit for bit.
    u32 composeExact = 0;
    f64 composeErr = 0.0;
    f64 chainErr = 0.0;
    u32 imageOk = 0;
    u32 imageNearest = 0;
    for (u32 y = 0; y < gpu.prepared.height; ++y) {
        for (u32 x = 0; x < gpu.prepared.width; ++x) {
            const usize i = static_cast<usize>(y) * gpu.prepared.width + x;
            const Vec4 e = compose_pixel(c, x, y, gpu.prepared.prepared[i], gpu.prepared.normal[i], gpu.prepared.albedo[i],
                                         gpu.prepared.radiance[i], gpu.prepared.litAlpha[i], gpu.ao[i], gpu.ssr[i],
                                         gpu.gi[i]);
            const Vec4& g = gpu.dump[i];
            const bool same = sameBits(e.x, g.x) && sameBits(e.y, g.y) && sameBits(e.z, g.z) && sameBits(e.w, g.w);
            composeExact += same ? 1u : 0u;
            composeErr = std::max({composeErr, relErr(g.x, e.x, 1e-6f), relErr(g.y, e.y, 1e-6f), relErr(g.z, e.z, 1e-6f)});
            const Vec4& r = ref.composed[i];
            chainErr = std::max({chainErr, relErr(g.x, r.x, 1e-3f), relErr(g.y, r.y, 1e-3f), relErr(g.z, r.z, 1e-3f)});
            const Vec4& im = gpu.image[i];
            imageNearest += im.x == roundHalf(g.x) && im.y == roundHalf(g.y) && im.z == roundHalf(g.z) &&
                                    im.w == roundHalf(g.w)
                                ? 1u
                                : 0u;
            imageOk += withinHalfUlp(im.x, g.x) && withinHalfUlp(im.y, g.y) && withinHalfUlp(im.z, g.z) &&
                               withinHalfUlp(im.w, g.w)
                           ? 1u
                           : 0u;
        }
    }
    std::printf("  [%s] %s: ssfx.compose vs compose_pixel: %u / %zu bit-identical (max rel %.3g); whole chain vs "
                "reference_frame max rel %.3g; RGBA16F image within 1 half ulp of the dump on %u / %zu pixels (%u "
                "round-to-nearest)\n",
                lang, cfg.name, composeExact, n, composeErr, chainErr, imageOk, n, imageNearest);
    expect(composeErr <= 1e-6, "ssfx.compose == compose_pixel");
    expect(imageOk == n, "RGBA16F output == the f32 dump converted to half (<= 1 half ulp)");
    (void)room;
}

int runPasses(Context& ctx) {
    constexpr u32 kW = 97;
    constexpr u32 kH = 71;
    if (!ensureTargets(ctx, kW, kH)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    u32 built = 0;
    std::vector<Captured> perLang[2];
    const Config configs[2] = {configA(), configB()};
    for (u32 l = 0; l < 2u; ++l) {
        SsfxGpu ssfx;
        if (!initSsfx(ctx, ssfx, kLangs[l].language)) {
            std::printf("  [%s] kernels not built: skipped\n", kLangs[l].name);
            continue;
        }
        ++built;
        rg::Graph graph;
        for (u32 frame = 0; frame < 2u; ++frame) {
            ssfx_test::RoomScene room;
            ssfx_test::buildRoom(room, kW, kH, frame * 2u);
            writeStaging(ctx, room.g);
            for (const Config& cfg : configs) {
                Captured cap;
                expect(runFrame(ctx, ssfx, graph, cfg.settings, ssfx_test::cameraDesc(room.camera), room.ambient, cap),
                       "frame");
                std::printf("  [%s] frame %u, %s: %u passes\n", kLangs[l].name, frame, cfg.name, ssfx.stats().passes);
                checkFrame(kLangs[l].name, cfg, room, ssfx, cap);
                perLang[l].push_back(std::move(cap));
            }
        }
        expect(ssfx.stats().outputRebuilds == 1u && ssfx.stats().workRebuilds == 1u, "no rebuild at a constant extent");
        ssfx.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no screen-space kernel built\n");
        return kSkip;
    }
    if (built == 2u && perLang[0].size() == perLang[1].size()) {
        u32 aoSame = 0;
        u32 ssrSame = 0;
        u32 dumpSame = 0;
        usize total = 0;
        for (usize f = 0; f < perLang[0].size(); ++f) {
            const Captured& a = perLang[0][f];
            const Captured& b = perLang[1][f];
            for (usize i = 0; i < a.ao.size(); ++i) {
                aoSame += sameBits(a.ao[i], b.ao[i]) ? 1u : 0u;
                ssrSame += std::memcmp(&a.ssr[i], &b.ssr[i], sizeof(Vec4)) == 0 ? 1u : 0u;
                dumpSame += std::memcmp(&a.dump[i], &b.dump[i], sizeof(Vec4)) == 0 ? 1u : 0u;
                ++total;
            }
        }
        std::printf("  slang vs glsl (%zu pixels over 4 frames): GTAO %u, SSR %u, composed %u bit-identical\n", total,
                    aoSame, ssrSame, dumpSame);
        expect(aoSame == total, "Slang GTAO == GLSL GTAO bit for bit");
    }
    return 0;
}

// --- analytic ------------------------------------------------------------------------------------------
int runAnalytic(Context& ctx) {
    constexpr u32 kSize = 160;
    if (!ensureTargets(ctx, kSize, kSize)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        SsfxGpu ssfx;
        if (!initSsfx(ctx, ssfx, lang.language)) {
            std::printf("  [%s] kernels not built: skipped\n", lang.name);
            continue;
        }
        ++built;
        rg::Graph graph;
        // Plane (oblique view): AO 1.
        {
            ssfx_test::WedgeScene plane;
            ssfx_test::buildWedge(plane, kSize, kSize, 180.f, 70.f, 3.f, 20.f, 1.5f);
            writeStaging(ctx, plane.g);
            SsfxGpuSettings s{};
            s.ssr = false;
            s.ssgi = false;
            s.gtao.radius = 2.f;
            s.gtao.slices = 8;
            s.gtao.steps = 16;
            Captured cap;
            const f32 ambient[3] = {0.2f, 0.2f, 0.2f};
            expect(runFrame(ctx, ssfx, graph, s, ssfx_test::cameraDesc(plane.camera), ambient, cap), "plane frame");
            f32 worst = 0.f;
            u32 count = 0;
            for (usize i = 0; i < cap.ao.size(); ++i) {
                if (plane.face[i] != 0u) {
                    worst = std::max(worst, std::fabs(1.f - cap.ao[i]));
                    ++count;
                }
            }
            std::printf("  [%s] plane (oblique, RT0 / RT4 through ssfx.prepare): %u pixels, max |1 - AO| = %.3g\n", lang.name,
                        count, static_cast<f64>(worst));
            expect(count > 10000u && worst <= 1e-5f, "GPU GTAO: a plane gives AO 1");
        }
        for (const f32 alpha : {90.f, 120.f}) {
            ssfx_test::WedgeScene w;
            ssfx_test::buildWedge(w, kSize, kSize, alpha, 90.f, 3.f, 25.f);
            writeStaging(ctx, w.g);
            SsfxGpuSettings s{};
            s.ssr = false;
            s.ssgi = false;
            s.gtao.radius = 1000.f;
            s.gtao.max_radius_px = 512.f;
            s.gtao.slices = 32;
            s.gtao.steps = 160;
            Captured cap;
            const f32 ambient[3] = {0.2f, 0.2f, 0.2f};
            expect(runFrame(ctx, ssfx, graph, s, ssfx_test::cameraDesc(w.camera), ambient, cap), "wedge frame");
            SsfxReferenceFrame ref;
            expect(reference_frame(s, ssfx.constants(), cap.prepared, ref), "wedge reference");
            const f64 foot = 2.0 * 3.0 / kSize;
            f64 nearSum = 0.0;
            u32 nearCount = 0;
            f64 diff = 0.0;
            for (usize i = 0; i < cap.ao.size(); ++i) {
                diff = std::max(diff, std::fabs(static_cast<f64>(cap.ao[i]) - ref.ao[i]));
                if (w.face[i] != 0u && w.creaseDistance[i] < foot) {
                    nearSum += cap.ao[i];
                    ++nearCount;
                }
            }
            const f64 nearMean = nearCount > 0u ? nearSum / nearCount : 0.0;
            const f64 expected = ssfx_test::wedgeVisibility(alpha);
            std::printf("  [%s] wedge %.0f deg: analytic %.4f, GPU GTAO within 1 footprint of the crease %.4f (%u px); "
                        "GPU vs CPU kernel max |diff| %.3g\n",
                        lang.name, static_cast<f64>(alpha), expected, nearMean, nearCount, diff);
            expect(nearCount >= 128u && std::fabs(nearMean - expected) <= 0.025,
                   "GPU GTAO at the crease within 0.025 of (1 - cos alpha) / 2");
            expect(diff <= 1e-6, "GPU GTAO == CPU gtao_kernel on the wedge");
        }
        ssfx.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no screen-space kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "ssfx.", 5) == 0) {
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
    SsfxGpu ssfx;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initSsfx(ctx, ssfx, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no screen-space kernel built\n");
        return kSkip;
    }
    constexpr u32 kW = 48;
    constexpr u32 kH = 32;
    if (!ensureTargets(ctx, kW, kH)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    std::vector<ssfx_test::RoomScene> rooms(4);
    for (u32 i = 0; i < 4u; ++i) {
        ssfx_test::buildRoom(rooms[i], kW, kH, i);
    }
    Config cfg = configA();
    cfg.settings.ssgi_params.sample_sqrt = 2;
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 workRebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        const ssfx_test::RoomScene& room = rooms[frame % 4u];
        writeStaging(ctx, room.g);
        cfg.settings.gtao.frame = frame;
        cfg.settings.gtao.jitter = (frame % 2u) == 1u;
        const SsfxCameraDesc camera = ssfx_test::cameraDesc(room.camera);
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        const SsfxFrameImages images = frameImages(ctx, false);
        t_allocations = 0;
        t_count = measure;
        const bool begun = ssfx.beginFrame(ctx.serial, cfg.settings, camera, room.ambient, images);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, ssfx, graph, false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        ssfx.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(ssfx.stats().aoRan && ssfx.stats().ssrRan && ssfx.stats().ssgiBounces == 2u, "every pass runs");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            workRebuilds = ssfx.stats().workRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(ssfx.stats().workRebuilds == workRebuilds && ssfx.stats().outputRebuilds == 1u,
           "no work / output rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; prepare, GTAO (jitter alternating), SSR, 2 SSGI bounces,\n"
                    "  compose; camera changing)\n"
                    "  SsfxGpu::beginFrame + ssfx.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (test upload + ssfx imports and passes): %llu\n",
                    kTotal - kWarmup, ssfx.kernelLanguage(), side, callbacks, build);
        expect(side == 0u, "the screen-space passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the screen-space passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    ssfx.kernelLanguage());
    }
    ssfx.destroy();
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
        } else if (mode == "analytic") {
            rc = runAnalytic(ctx);
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
