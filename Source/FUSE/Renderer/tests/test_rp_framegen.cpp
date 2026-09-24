// WP-4.4 Lavapipe gates for FSR 3.1 frame generation (FrameGenGpu) under VK_LAYER_KHRONOS_validation with
// synchronization validation (every validation message fails the run). CPU gates: test_rp_framegen_cpu.cpp,
// test_rp_latency_cpu.cpp.
//
// Scene: a smooth periodic colour pattern (sRGB-encoded display values in [0.1, 0.9], periods >= 15 px) translating
// by v display pixels per frame (a camera pan over a plane at 10 m): source N = P(p - v N), display 256 x 144, render
// (depth / motion) 192 x 108, UV motion v / display size everywhere. The analytic interpolated frame between N - 1
// and N is P(p - v (N - 0.5)).
//
//   --mode midpoint      16 frames, v = (3, 1): per frame N >= 1 the interpolated frame is compared with the analytic
//                        midpoint over the interior (12 px border): RMSE, PSNR, and the fitted time s* (argmin over s of
//                        RMSE(out, P(p - v (N - 1 + s)))); gates: s* within 0.05 of 0.5, RMSE <= 0.25 x the best
//                        endpoint (showing N - 1 or N), PSNR >= 35 dB, finite. Also fg.convert == fg_device_depth /
//                        the UV motion bit for bit (read back), and the real frame + no UI == the source.
//   --mode ui            the same with a UI texture (opaque panel, translucent panel, a cursor moving 20 px / frame):
//                        presentInterpolated == fg_composite(interpolated, UI) and presentReal == fg_composite(source, UI)
//                        (within 1 half ulp: llvmpipe truncates f32 -> f16 stores); no cursor ghost at its previous
//                        position; the HUD-less interpolation under the opaque panel still lands at the midpoint (the UI
//                        never enters the interpolation).
//   --mode reset         camera cut (pattern A -> B) with reset: the interpolated frame is exactly the current frame (no
//                        mixing with the old scene); the next frame interpolates at the midpoint again; invalidate()
//                        behaves like reset; a cut without reset is reported (the SDK's scene-change detection).
//   --mode determinism   the same 10-frame sequence through two fresh FrameGenGpu instances and through the GLSL twins
//                        of the FUSE passes: interpolated and composited outputs bit-identical.
//   --mode optical_flow  integer translation (4, 2): the level-0 optical-flow vectors of interior blocks equal -v (the
//                        block's position in the previous frame) — the search variant the device runs (portable on
//                        Lavapipe's 8-lane subgroups).
//   --mode zero_alloc    80 frames (16 warm-up): 0 operator-new calls in FrameGenGpu::beginFrame, the graph build
//                        (imports + passes) and the fg.* pass callbacks (validated run first; validation off for the
//                        count), 0 layout conflicts.
//   --mode latency_probe create_latency_provider on Lavapipe: no VK_NV_low_latency2 / VK_AMD_anti_lag, no NVIDIA
//                        provider -> the None provider, with the reasons.
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, passes not built).
#include <fuse/renderer/framegen/fg_gpu.hpp>
#include <fuse/renderer/present/latency/latency_provider.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// --- allocation counter (operator new) -----------------------------------------------------------------
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
using namespace fuse::renderer::framegen;
namespace present = fuse::renderer::present;
using fuse::f32;
using fuse::f64;
using fuse::i32;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec2;
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
constexpr u32 kDisplayW = 256;
constexpr u32 kDisplayH = 144;
constexpr u32 kRenderW = 192;
constexpr u32 kRenderH = 108;
constexpr u32 kFormatRgba16f = 97u;
constexpr u32 kBorder = 12u;

// Gates (measured values in the WP-4.4 row of docs/unification/RENDERER-EXECUTION.md).
constexpr f64 kGateMidpointTime = 0.05;   ///< |s* - 0.5|
constexpr f64 kGateRmseVsEndpoint = 0.25; ///< interpolated RMSE <= this x the best endpoint's
constexpr f64 kGatePsnr = 35.0;           ///< dB vs the analytic midpoint

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n", data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
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

// --- Vulkan context ------------------------------------------------------------------------------------
struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    u64 serial = 0;
    FgCapabilities caps{};

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

int setup(Context& ctx, bool validation) {
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
    instanceDesc.appName = "fuse_rp_framegen";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
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
    ctx.caps = queryFgCapabilities(ctx.device.get(), kFormatRgba16f);
    if (!ctx.caps.supported) {
        std::printf("SKIP: frame generation unsupported: %s\n", ctx.caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s, compute subgroups %u..%u (vendored search %s)\n", ctx.device->info().deviceName.c_str(), ctx.caps.subgroupMin,
                ctx.caps.subgroupMax, ctx.caps.vendoredSearch ? "usable" : "not usable: portable search");
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

// --- half floats --------------------------------------------------------------------------------------------
u16 toHalf(f32 value) {
    u32 x;
    std::memcpy(&x, &value, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 exp = static_cast<i32>((x >> 23) & 0xffu) - 127 + 15;
    u32 mant = x & 0x7fffffu;
    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<u16>(sign);
        }
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - exp);
        u32 h = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1u);
        const u32 half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (h & 1u) != 0u)) {
            ++h;
        }
        return static_cast<u16>(sign | h);
    }
    if (exp >= 31) {
        return static_cast<u16>(sign | 0x7c00u);
    }
    u32 h = (static_cast<u32>(exp) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u) != 0u)) {
        ++h;
    }
    return static_cast<u16>(sign | h);
}

f32 fromHalf(u16 h) {
    const u32 sign = (static_cast<u32>(h) & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1fu;
    const u32 mant = h & 0x3ffu;
    u32 x;
    if (exp == 0u) {
        if (mant == 0u) {
            x = sign;
        } else {
            f32 v = static_cast<f32>(mant) / 1024.f / 16384.f;
            return (sign != 0u) ? -v : v;
        }
    } else if (exp == 31u) {
        x = sign | 0x7f800000u | (mant << 13);
    } else {
        x = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
    }
    f32 f;
    std::memcpy(&f, &x, 4);
    return f;
}

/// |a - b| in half ulps of b (both representable halves).
u32 halfUlps(u16 a, u16 b) {
    auto key = [](u16 v) -> i32 { return (v & 0x8000u) != 0u ? -static_cast<i32>(v & 0x7fffu) : static_cast<i32>(v); };
    return static_cast<u32>(std::abs(key(a) - key(b)));
}

// --- scene --------------------------------------------------------------------------------------------------
struct Pattern {
    f32 phase = 0.f;
    f32 value(f32 u, f32 v, u32 c) const {
        constexpr f32 kTau = 6.28318530718f;
        const f32 cc = static_cast<f32>(c);
        return 0.5f + 0.18f * std::sin(kTau * (u / 19.f + 0.13f * cc + phase)) * std::cos(kTau * (v / 15.f - 0.07f * cc)) +
               0.12f * std::sin(kTau * ((0.6f * u + v) / 27.f) + cc + 3.f * phase);
    }
    /// Display image of the pattern shifted by `offset` px (content at p shows P(p - offset)).
    void image(Vec2 offset, std::vector<Vec4>& out) const {
        out.resize(static_cast<usize>(kDisplayW) * kDisplayH);
        for (u32 y = 0; y < kDisplayH; ++y) {
            for (u32 x = 0; x < kDisplayW; ++x) {
                const f32 u = static_cast<f32>(x) + 0.5f - offset.x, v = static_cast<f32>(y) + 0.5f - offset.y;
                out[y * kDisplayW + x] = Vec4(value(u, v, 0), value(u, v, 1), value(u, v, 2), 1.f);
            }
        }
    }
};

struct Ui {
    bool enabled = false;
    u32 cursorX = 0, cursorY = 0;
    /// Premultiplied UI RGBA.
    Vec4 at(u32 x, u32 y) const {
        if (!enabled) {
            return Vec4(0.f, 0.f, 0.f, 0.f);
        }
        if (x >= cursorX && x < cursorX + 6u && y >= cursorY && y < cursorY + 6u) {
            return Vec4(1.f, 1.f, 1.f, 1.f);
        }
        if (x >= 8u && x < 72u && y >= 8u && y < 28u) {
            return Vec4(0.9f, 0.2f, 0.1f, 1.f);
        }
        if (x >= 180u && x < 248u && y >= 110u && y < 136u) {
            return Vec4(0.05f, 0.3f, 0.4f, 0.5f);
        }
        return Vec4(0.f, 0.f, 0.f, 0.f);
    }
};

// --- GPU harness --------------------------------------------------------------------------------------------
struct HostBuffer {
    Buffer buffer{};
    u8 queue = rg::kNoQueue;
    bool create(Context& ctx, u64 bytes, BufferUsage usage, MemoryUsage memory, const char* n) {
        BufferDesc d{};
        d.size = static_cast<usize>(bytes);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = n;
        return ctx.allocator->createBuffer(d, buffer) && buffer.mapped != nullptr;
    }
    void destroy(Context& ctx) {
        if (buffer.handle != nullptr) {
            ctx.allocator->destroyBuffer(buffer);
        }
        buffer = Buffer{};
    }
    rg::BufferRef import(rg::Graph& graph, const char* name) {
        return graph.importBuffer(rg::ImportedBuffer{buffer.handle, buffer.desc.size, queue, &queue, name});
    }
};

struct InputImage {
    Texture tex{};
    u32 layout = 0;
    u8 queue = rg::kNoQueue;
    bool create(Context& ctx, const char* name) {
        TextureDesc d{};
        d.width = kDisplayW;
        d.height = kDisplayH;
        d.format = static_cast<GpuFormat>(kFormatRgba16f);
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst) |
                                          static_cast<u32>(ImageUsage::TransferSrc));
        d.name = name;
        return ctx.allocator->createImage(d, tex);
    }
    void destroy(Context& ctx) {
        if (tex.image != nullptr) {
            ctx.allocator->destroyImage(tex);
        }
        tex = Texture{};
    }
    rg::TextureRef import(rg::Graph& graph, const char* name) {
        rg::ImportedImage i{};
        i.image = tex.image;
        i.view = tex.view;
        i.format = kFormatRgba16f;
        i.width = kDisplayW;
        i.height = kDisplayH;
        i.initialLayout = layout;
        i.initialQueue = queue;
        i.layoutTracker = &layout;
        i.queueTracker = &queue;
        i.name = name;
        return graph.importImage(i);
    }
};

struct CopyRecord {
    rg::TextureRef image;
    rg::BufferRef buffer;
    u64 offset = 0;
    u32 width = 0, height = 0;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const CopyRecord& u = *static_cast<const CopyRecord*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = u.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {u.width, u.height, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(u.buffer)),
                           static_cast<VkImage>(pc.image(u.image)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void recordReadback(const rg::PassContext& pc, void* user) {
    const CopyRecord& u = *static_cast<const CopyRecord*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = u.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {u.width, u.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(u.image)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(u.buffer)), 1, &region);
}

thread_local bool t_inPass = false;
void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "fg.", 3) == 0) {
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

/// Everything one frame reads back.
struct FrameOut {
    std::vector<u16> interpolated;        ///< RGBA16F display (FSR 3.1 output, HUD-less)
    std::vector<u16> presentInterpolated; ///< RGBA16F display
    std::vector<u16> presentReal;         ///< RGBA16F display
    std::vector<u16> source;              ///< the uploaded source (halves)
    std::vector<u16> ui;                  ///< the uploaded UI (halves)
    std::vector<f32> inputDepth;          ///< R32F render (fg.convert output)
    std::vector<f32> inputMotion;         ///< RG32F render
    std::vector<std::int16_t> ofVector;      ///< RG16_SINT, level-0 optical flow
    bool fiReset = false;
};

struct FrameInput {
    const Pattern* pattern = nullptr;
    Vec2 offset{};    ///< content offset of this frame (display px)
    Vec2 motionPx{};  ///< display px, current - previous
    bool reset = false;
    Ui ui{};
    u64 frameId = 0;
};

class Runner {
public:
    explicit Runner(Context& c) : ctx(c) {}
    ~Runner() { destroy(); }

    bool init(FgKernelLanguage language = FgKernelLanguage::Auto, FgSearchVariant search = FgSearchVariant::Auto) {
        const u64 displayBytes = static_cast<u64>(kDisplayW) * kDisplayH * 8u;
        const u64 rn = static_cast<u64>(kRenderW) * kRenderH;
        const BufferUsage storage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                             static_cast<u32>(BufferUsage::TransferSrc));
        FgSizes sizes{};
        fg_compute_sizes(kDisplayW, kDisplayH, kRenderW, kRenderH, sizes);
        ofBlocks = sizes.ofW[0] * sizes.ofH[0];
        ofW = sizes.ofW[0];
        ofH = sizes.ofH[0];
        readbackBytes = 3u * displayBytes + rn * 12u + ofBlocks * 4u;
        if (!source.create(ctx, "rp_fg.source") || !ui.create(ctx, "rp_fg.ui") ||
            !staging.create(ctx, 2u * displayBytes, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_fg.staging") ||
            !depth.create(ctx, rn * 4u, storage, MemoryUsage::CpuToGpu, "rp_fg.depth") ||
            !motion.create(ctx, rn * 8u, storage, MemoryUsage::CpuToGpu, "rp_fg.motion") ||
            !readback.create(ctx, readbackBytes, BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "rp_fg.readback")) {
            std::fprintf(stderr, "FAIL: harness resources\n");
            return false;
        }
        FgGpuDesc d{};
        d.device = ctx.device.get();
        d.allocator = ctx.allocator.get();
        d.displayWidth = kDisplayW;
        d.displayHeight = kDisplayH;
        d.maxRenderWidth = kRenderW;
        d.maxRenderHeight = kRenderH;
        d.sourceFormat = kFormatRgba16f;
        d.language = language;
        d.search = search;
        if (!fg.init(d)) {
            std::fprintf(stderr, "FAIL: FrameGenGpu init\n");
            return false;
        }
        return true;
    }

    void destroy() {
        if (ctx.executor != nullptr) {
            ctx.executor->waitIdle();
        }
        fg.destroy();
        source.destroy(ctx);
        ui.destroy(ctx);
        staging.destroy(ctx);
        depth.destroy(ctx);
        motion.destroy(ctx);
        readback.destroy(ctx);
    }

    /// Uploads the frame (when `upload`), runs FrameGenGpu, reads back when `out` != null.
    bool frame(const FrameInput& in, FrameOut* out, bool upload = true, bool invalidate = false) {
        const u32 pixels = kDisplayW * kDisplayH;
        const u64 displayBytes = static_cast<u64>(pixels) * 8u;
        const u32 rn = kRenderW * kRenderH;
        if (upload) {
            in.pattern->image(in.offset, scratch);
            u16* s = static_cast<u16*>(staging.buffer.mapped);
            for (u32 i = 0; i < pixels; ++i) {
                s[i * 4u + 0u] = toHalf(scratch[i].x);
                s[i * 4u + 1u] = toHalf(scratch[i].y);
                s[i * 4u + 2u] = toHalf(scratch[i].z);
                s[i * 4u + 3u] = toHalf(1.f);
            }
            u16* u = s + static_cast<usize>(pixels) * 4u;
            for (u32 y = 0; y < kDisplayH; ++y) {
                for (u32 x = 0; x < kDisplayW; ++x) {
                    const Vec4 c = in.ui.at(x, y);
                    u16* t = u + (static_cast<usize>(y) * kDisplayW + x) * 4u;
                    t[0] = toHalf(c.x);
                    t[1] = toHalf(c.y);
                    t[2] = toHalf(c.z);
                    t[3] = toHalf(c.w);
                }
            }
            f32* dp = static_cast<f32*>(depth.buffer.mapped);
            f32* mp = static_cast<f32*>(motion.buffer.mapped);
            for (u32 i = 0; i < rn; ++i) {
                dp[i] = 10.f;
                mp[i * 2u + 0u] = in.motionPx.x / static_cast<f32>(kDisplayW);
                mp[i * 2u + 1u] = in.motionPx.y / static_cast<f32>(kDisplayH);
            }
        }
        ++ctx.serial;
        if (invalidate) {
            fg.invalidate();
        }
        FgGpuFrameDesc f{};
        f.renderWidth = kRenderW;
        f.renderHeight = kRenderH;
        f.jitter_px = Vec2(0.f, 0.f);
        f.near_plane = 0.1f;
        f.far_plane = 1000.f;
        f.vertical_fov_rad = 0.9f;
        f.frame_time_ms = 16.667f;
        f.reset = in.reset;
        f.frame_id = in.frameId;
        t_allocations = 0;
        t_count = measuring;
        const bool began = fg.beginFrame(ctx.serial, f);
        t_count = false;
        allocBegin += measuring ? t_allocations : 0u;

        graph.reset();
        const rg::BufferRef stagingRef = staging.import(graph, "rp_fg.staging");
        const rg::TextureRef srcRef = source.import(graph, "rp_fg.source");
        const rg::TextureRef uiRef = ui.import(graph, "rp_fg.ui");
        if (upload) {
            ups[0] = CopyRecord{srcRef, stagingRef, 0u, kDisplayW, kDisplayH};
            ups[1] = CopyRecord{uiRef, stagingRef, displayBytes, kDisplayW, kDisplayH};
            for (u32 k = 0; k < 2u; ++k) {
                graph.addPass("upload.input", &recordUpload, &ups[k])
                    .use(ups[k].image, rg::Access::TransferDst)
                    .use(stagingRef, rg::Access::TransferSrc, rg::BufferRange{ups[k].offset, displayBytes});
            }
        }
        const rg::BufferRef depthRef = depth.import(graph, "rp_fg.depth");
        const rg::BufferRef motionRef = motion.import(graph, "rp_fg.motion");
        t_allocations = 0;
        t_count = measuring;
        const FgGraphRefs refs = fg.importInto(graph);
        FgGpuInputs inputs{};
        inputs.source = srcRef;
        inputs.ui = in.ui.enabled ? uiRef : rg::TextureRef{};
        inputs.depth = depthRef;
        inputs.motion = motionRef;
        fg.addPasses(graph, refs, inputs);
        t_count = false;
        allocBuild += measuring ? t_allocations : 0u;
        if (out != nullptr) {
            const rg::BufferRef rb = readback.import(graph, "rp_fg.readback");
            const rg::TextureRef imgs[6] = {refs.interpolated, refs.presentInterpolated, refs.presentReal, refs.images[kSlotInputDepth],
                                            refs.images[kSlotInputMotion], refs.images[kSlotOfVector]};
            const u64 offsets[6] = {0u, displayBytes, 2u * displayBytes, 3u * displayBytes, 3u * displayBytes + rn * 4ull,
                                    3u * displayBytes + rn * 12ull};
            const u64 sizes[6] = {displayBytes, displayBytes, displayBytes, rn * 4ull, rn * 8ull, ofBlocks * 4ull};
            const u32 ws[6] = {kDisplayW, kDisplayW, kDisplayW, kRenderW, kRenderW, ofW};
            const u32 hs[6] = {kDisplayH, kDisplayH, kDisplayH, kRenderH, kRenderH, ofH};
            for (u32 k = 0; k < 6u; ++k) {
                rbs[k] = CopyRecord{imgs[k], rb, offsets[k], ws[k], hs[k]};
                graph.addPass("readback.copy", &recordReadback, &rbs[k])
                    .use(imgs[k], rg::Access::TransferSrc)
                    .use(rb, rg::Access::TransferDst, rg::BufferRange{offsets[k], sizes[k]});
            }
            graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
        }
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        allocCallbacks += measuring ? t_allocations : 0u;
        const bool waited = ctx.executor->waitIdle();
        conflicts += graph.stats().layoutConflicts;
        if (!began || !result.ok || !waited) {
            std::fprintf(stderr, "FAIL: frame %llu (began %d, executed %d)\n", static_cast<unsigned long long>(in.frameId), began ? 1 : 0,
                         result.ok ? 1 : 0);
            return false;
        }
        if (out != nullptr) {
            const u8* base = static_cast<const u8*>(readback.buffer.mapped);
            auto grab16 = [&](std::vector<u16>& v, u64 offset, usize count) {
                v.resize(count);
                std::memcpy(v.data(), base + offset, count * 2u);
            };
            grab16(out->interpolated, 0u, static_cast<usize>(pixels) * 4u);
            grab16(out->presentInterpolated, displayBytes, static_cast<usize>(pixels) * 4u);
            grab16(out->presentReal, 2u * displayBytes, static_cast<usize>(pixels) * 4u);
            out->inputDepth.resize(rn);
            std::memcpy(out->inputDepth.data(), base + 3u * displayBytes, rn * 4u);
            out->inputMotion.resize(static_cast<usize>(rn) * 2u);
            std::memcpy(out->inputMotion.data(), base + 3u * displayBytes + rn * 4ull, rn * 8u);
            out->ofVector.resize(static_cast<usize>(ofBlocks) * 2u);
            std::memcpy(out->ofVector.data(), base + 3u * displayBytes + rn * 12ull, ofBlocks * 4u);
            const u16* s = static_cast<const u16*>(staging.buffer.mapped);
            out->source.assign(s, s + static_cast<usize>(pixels) * 4u);
            out->ui.assign(s + static_cast<usize>(pixels) * 4u, s + static_cast<usize>(pixels) * 8u);
            out->fiReset = fg.stats().fiReset;
        }
        return true;
    }

    Context& ctx;
    FrameGenGpu fg;
    InputImage source, ui;
    HostBuffer staging, depth, motion, readback;
    rg::Graph graph;
    CopyRecord ups[2]{};
    CopyRecord rbs[6]{};
    std::vector<Vec4> scratch;
    u64 readbackBytes = 0;
    u32 ofBlocks = 0, ofW = 0, ofH = 0;
    bool measuring = false;
    unsigned long long allocBegin = 0, allocBuild = 0, allocCallbacks = 0;
    u32 conflicts = 0;
};

// --- metrics ----------------------------------------------------------------------------------------------------
bool interior(u32 x, u32 y) { return x >= kBorder && y >= kBorder && x + kBorder < kDisplayW && y + kBorder < kDisplayH; }

/// RMSE (RGB, interior, optional mask) of a half image against the pattern at `offset`.
f64 rmseVs(const std::vector<u16>& img, const Pattern& p, Vec2 offset, const Ui* skipUi = nullptr, bool onlyUnderOpaquePanel = false) {
    f64 sum = 0.0;
    u64 n = 0;
    for (u32 y = 0; y < kDisplayH; ++y) {
        for (u32 x = 0; x < kDisplayW; ++x) {
            if (!interior(x, y)) {
                continue;
            }
            if (skipUi != nullptr) {
                const bool underPanel = x >= 8u && x < 72u && y >= 8u && y < 28u;
                if (onlyUnderOpaquePanel != underPanel) {
                    continue;
                }
            }
            const f32 u = static_cast<f32>(x) + 0.5f - offset.x, v = static_cast<f32>(y) + 0.5f - offset.y;
            const usize i = (static_cast<usize>(y) * kDisplayW + x) * 4u;
            for (u32 c = 0; c < 3u; ++c) {
                const f64 d = static_cast<f64>(fromHalf(img[i + c])) - static_cast<f64>(p.value(u, v, c));
                sum += d * d;
                ++n;
            }
        }
    }
    return n > 0u ? std::sqrt(sum / static_cast<f64>(n)) : 0.0;
}

/// argmin over s in [0, 1] of RMSE(img, P at prev + s (cur - prev)), parabolic refinement.
f64 fitTime(const std::vector<u16>& img, const Pattern& p, Vec2 prev, Vec2 cur, const Ui* skipUi = nullptr) {
    constexpr u32 kSteps = 50;
    f64 best = 1e30;
    u32 bestK = 0;
    f64 err[kSteps + 1];
    for (u32 k = 0; k <= kSteps; ++k) {
        const f32 s = static_cast<f32>(k) / kSteps;
        err[k] = rmseVs(img, p, Vec2(prev.x + s * (cur.x - prev.x), prev.y + s * (cur.y - prev.y)), skipUi, false);
        if (err[k] < best) {
            best = err[k];
            bestK = k;
        }
    }
    f64 s = static_cast<f64>(bestK) / kSteps;
    if (bestK > 0u && bestK < kSteps) {
        const f64 a = err[bestK - 1u], b = err[bestK], c = err[bestK + 1u];
        const f64 den = a - 2.0 * b + c;
        if (den > 0.0) {
            s += 0.5 * (a - c) / den / kSteps;
        }
    }
    return s;
}

bool finiteImage(const std::vector<u16>& img) {
    for (const u16 h : img) {
        if (((h >> 10) & 0x1fu) == 0x1fu) {
            return false;
        }
    }
    return true;
}

f64 psnr(f64 rmse) { return rmse > 0.0 ? 20.0 * std::log10(1.0 / rmse) : 200.0; }

// --- modes ---------------------------------------------------------------------------------------------------------
Vec2 offsetAt(Vec2 v, u32 frame) { return Vec2(v.x * static_cast<f32>(frame), v.y * static_cast<f32>(frame)); }

int runMidpoint(Context& ctx, bool withUi) {
    Runner r(ctx);
    if (!r.init()) {
        return 1;
    }
    const Pattern pattern{};
    const Vec2 v(3.f, 1.f);
    constexpr u32 kFrames = 16;
    f64 sumInterp = 0.0, sumEndpoint = 0.0, worstS = 0.0, worstPsnr = 200.0, worstUnderPanel = 0.0;
    u32 analysed = 0, ghostPixels = 0, compositeMismatch = 0, realMismatch = 0, uiExact = 0, uiOpaque = 0;
    std::printf("%s: FrameGenGpu kernels %s, %s search; display %ux%u, render %ux%u, v = (%.0f, %.0f) px / frame\n",
                withUi ? "ui" : "midpoint", r.fg.kernelLanguage(), r.fg.portableSearch() ? "portable" : "vendored", kDisplayW, kDisplayH,
                kRenderW, kRenderH, v.x, v.y);
    for (u32 n = 0; n < kFrames; ++n) {
        FrameInput in{};
        in.pattern = &pattern;
        in.offset = offsetAt(v, n);
        in.motionPx = v;
        in.frameId = n;
        in.ui.enabled = withUi;
        in.ui.cursorX = 20u + 20u * n % 200u;
        in.ui.cursorY = 60u;
        FrameOut out{};
        if (!r.frame(in, &out)) {
            return 1;
        }
        expect(finiteImage(out.interpolated) && finiteImage(out.presentInterpolated), "outputs finite");
        if (n == 0u) {
            expect(out.fiReset, "frame 0 resets");
            // fg.convert vs the CPU twin (bit for bit).
            u32 depthMismatch = 0, motionMismatch = 0;
            for (u32 i = 0; i < kRenderW * kRenderH; ++i) {
                const f32 ref = fg_device_depth(10.f, 0.1f, 1000.f);
                depthMismatch += std::memcmp(&out.inputDepth[i], &ref, 4) != 0 ? 1u : 0u;
                const f32 mx = v.x / static_cast<f32>(kDisplayW), my = v.y / static_cast<f32>(kDisplayH);
                motionMismatch += (out.inputMotion[i * 2u] != mx || out.inputMotion[i * 2u + 1u] != my) ? 1u : 0u;
            }
            expect(depthMismatch == 0u && motionMismatch == 0u, "fg.convert == fg_device_depth / UV motion bit for bit");
            continue;
        }
        const Vec2 prev = offsetAt(v, n - 1u), cur = offsetAt(v, n);
        const Vec2 mid((prev.x + cur.x) * 0.5f, (prev.y + cur.y) * 0.5f);
        const Ui* skip = withUi ? &in.ui : nullptr;
        const f64 eInterp = rmseVs(out.interpolated, pattern, mid, skip);
        // Endpoint baselines: showing frame N - 1 or frame N instead of the interpolated frame.
        std::vector<Vec4> img;
        const f64 eShowPrev = [&] {
            f64 sum = 0.0;
            u64 cnt = 0;
            pattern.image(prev, img);
            for (u32 y = 0; y < kDisplayH; ++y) {
                for (u32 x = 0; x < kDisplayW; ++x) {
                    if (!interior(x, y)) {
                        continue;
                    }
                    const f32 u = static_cast<f32>(x) + 0.5f - mid.x, vv = static_cast<f32>(y) + 0.5f - mid.y;
                    const Vec4 c = img[static_cast<usize>(y) * kDisplayW + x];
                    const f32 ch[3] = {c.x, c.y, c.z};
                    for (u32 k = 0; k < 3u; ++k) {
                        const f64 d = ch[k] - pattern.value(u, vv, k);
                        sum += d * d;
                        ++cnt;
                    }
                }
            }
            return std::sqrt(sum / static_cast<f64>(cnt));
        }();
        const f64 eShowCur = rmseVs(out.source, pattern, mid, skip);
        const f64 endpoint = std::min(eShowPrev, eShowCur);
        const f64 s = fitTime(out.interpolated, pattern, prev, cur, skip);
        const f64 p = psnr(eInterp);
        std::printf("  frame %2u: interpolated RMSE %.5f (PSNR %.2f dB), showing N-1 %.5f, showing N %.5f, fitted time s* = %.3f%s\n", n,
                    eInterp, p, eShowPrev, eShowCur, s, out.fiReset ? " (reset)" : "");
        expect(!out.fiReset, "no reset after frame 0");
        expect(std::fabs(s - 0.5) <= kGateMidpointTime, "interpolated frame lands at the midpoint");
        expect(eInterp <= kGateRmseVsEndpoint * endpoint, "interpolated error <= 0.25 x the best endpoint");
        expect(p >= kGatePsnr, "PSNR vs the analytic midpoint");
        sumInterp += eInterp;
        sumEndpoint += endpoint;
        worstS = std::max(worstS, std::fabs(s - 0.5));
        worstPsnr = std::min(worstPsnr, p);
        ++analysed;
        // UI / composition checks.
        const u32 pixels = kDisplayW * kDisplayH;
        for (u32 i = 0; i < pixels; ++i) {
            const Vec4 ui(fromHalf(out.ui[i * 4u]), fromHalf(out.ui[i * 4u + 1u]), fromHalf(out.ui[i * 4u + 2u]), fromHalf(out.ui[i * 4u + 3u]));
            const Vec4 fi(fromHalf(out.interpolated[i * 4u]), fromHalf(out.interpolated[i * 4u + 1u]), fromHalf(out.interpolated[i * 4u + 2u]), 1.f);
            const Vec4 sr(fromHalf(out.source[i * 4u]), fromHalf(out.source[i * 4u + 1u]), fromHalf(out.source[i * 4u + 2u]), 1.f);
            const Vec4 ci = fg_composite(fi, ui, withUi);
            const Vec4 cr = fg_composite(sr, ui, withUi);
            const f32 ciArr[3] = {ci.x, ci.y, ci.z}, crArr[3] = {cr.x, cr.y, cr.z};
            for (u32 c = 0; c < 3u; ++c) {
                compositeMismatch += halfUlps(out.presentInterpolated[i * 4u + c], toHalf(ciArr[c])) > 1u ? 1u : 0u;
                realMismatch += halfUlps(out.presentReal[i * 4u + c], toHalf(crArr[c])) > 1u ? 1u : 0u;
            }
            if (withUi && ui.w == 1.f) {
                ++uiOpaque;
                uiExact += (out.presentInterpolated[i * 4u] == out.ui[i * 4u] && out.presentInterpolated[i * 4u + 1u] == out.ui[i * 4u + 1u] &&
                            out.presentInterpolated[i * 4u + 2u] == out.ui[i * 4u + 2u])
                               ? 1u
                               : 0u;
            }
        }
        if (withUi) {
            // The previous frame's cursor must not appear in the interpolated presentation (no UI ghost).
            const u32 px = 20u + 20u * (n - 1u) % 200u, py = 60u;
            for (u32 y = py; y < py + 6u; ++y) {
                for (u32 x = px; x < px + 6u; ++x) {
                    if (in.ui.at(x, y).w != 0.f) {
                        continue; // overlaps this frame's UI
                    }
                    const usize i = (static_cast<usize>(y) * kDisplayW + x) * 4u;
                    for (u32 c = 0; c < 3u; ++c) {
                        ghostPixels += out.presentInterpolated[i + c] != out.interpolated[i + c] ? 1u : 0u;
                    }
                }
            }
            worstUnderPanel = std::max(worstUnderPanel, rmseVs(out.interpolated, pattern, mid, &in.ui, true));
        }
    }
    std::printf("  mean interpolated RMSE %.5f vs best endpoint %.5f (ratio %.3f); worst |s* - 0.5| %.3f; worst PSNR %.2f dB\n",
                sumInterp / analysed, sumEndpoint / analysed, sumInterp / sumEndpoint, worstS, worstPsnr);
    expect(compositeMismatch == 0u, "presentInterpolated == fg_composite(interpolated, UI) within 1 half ulp");
    expect(realMismatch == 0u, "presentReal == fg_composite(source, UI) within 1 half ulp");
    if (withUi) {
        std::printf("  UI: %u opaque UI pixel samples, %u exact in the interpolated presentation; previous-cursor ghost channels %u; "
                    "HUD-less RMSE under the opaque panel (worst) %.5f\n",
                    uiOpaque, uiExact, ghostPixels, worstUnderPanel);
        expect(uiExact == uiOpaque, "opaque UI shows exactly in the interpolated presentation");
        expect(ghostPixels == 0u, "no UI ghost at the previous cursor position");
        expect(worstUnderPanel <= 2.0 * sumInterp / analysed + 1e-3, "the interpolation under the UI is unaffected by the UI");
    }
    expect(r.conflicts == 0u, "no layout conflicts");
    return 0;
}

int runReset(Context& ctx) {
    Runner r(ctx);
    if (!r.init()) {
        return 1;
    }
    Pattern a{};
    Pattern b{};
    b.phase = 0.37f;
    Pattern c{};
    c.phase = 0.71f;
    const Vec2 v(3.f, 1.f);
    u32 frame = 0;
    FrameOut out{};
    auto run = [&](const Pattern& p, u32 local, bool reset, bool invalidate, FrameOut& o) {
        FrameInput in{};
        in.pattern = &p;
        in.offset = offsetAt(v, local);
        in.motionPx = v;
        in.frameId = frame++;
        in.reset = reset;
        return r.frame(in, &o, true, invalidate);
    };
    for (u32 n = 0; n < 6; ++n) {
        if (!run(a, n, false, false, out)) {
            return 1;
        }
    }
    // Cut to B with reset.
    if (!run(b, 0, true, false, out)) {
        return 1;
    }
    bool exact = out.fiReset;
    for (usize i = 0; exact && i < out.interpolated.size(); i += 4u) {
        exact = out.interpolated[i] == out.source[i] && out.interpolated[i + 1u] == out.source[i + 1u] &&
                out.interpolated[i + 2u] == out.source[i + 2u];
    }
    const f64 eCut = rmseVs(out.interpolated, b, Vec2(0.f, 0.f));
    std::printf("reset: cut A -> B with reset: interpolated == current frame bit for bit: %s (RMSE vs B %.6f)\n", exact ? "yes" : "NO", eCut);
    expect(exact, "reset: the interpolated frame is exactly the current frame");
    if (!run(b, 1, false, false, out)) {
        return 1;
    }
    const Vec2 mid(v.x * 0.5f, v.y * 0.5f);
    const f64 eAfter = rmseVs(out.interpolated, b, mid);
    const f64 sAfter = fitTime(out.interpolated, b, Vec2(0.f, 0.f), v);
    std::printf("reset: next frame interpolates B at s* = %.3f (RMSE %.5f)\n", sAfter, eAfter);
    expect(!out.fiReset && std::fabs(sAfter - 0.5) <= kGateMidpointTime && psnr(eAfter) >= kGatePsnr, "reset: next frame back at the midpoint");
    // invalidate() == reset.
    if (!run(b, 2, false, true, out)) {
        return 1;
    }
    bool inv = out.fiReset;
    for (usize i = 0; inv && i < out.interpolated.size(); i += 4u) {
        inv = out.interpolated[i] == out.source[i] && out.interpolated[i + 1u] == out.source[i + 1u] && out.interpolated[i + 2u] == out.source[i + 2u];
    }
    expect(inv, "invalidate(): the interpolated frame is the current frame");
    for (u32 n = 3; n < 6; ++n) {
        if (!run(b, n, false, false, out)) {
            return 1;
        }
    }
    // Cut to C without reset: report what the SDK's scene-change detection / the interpolation does.
    if (!run(c, 0, false, false, out)) {
        return 1;
    }
    const f64 eNoReset = rmseVs(out.interpolated, c, Vec2(0.f, 0.f));
    std::printf("reset: cut B -> C WITHOUT reset: interpolated RMSE vs C %.5f (%s)\n", eNoReset,
                eNoReset < 1e-6 ? "scene-change detection reset the interpolation" : "the frame mixes both scenes");
    expect(eCut < 1e-2, "reset output is the new scene");
    expect(r.conflicts == 0u, "no layout conflicts");
    return 0;
}

int runDeterminism(Context& ctx) {
    const Pattern pattern{};
    const Vec2 v(3.f, 1.f);
    constexpr u32 kFrames = 10;
    std::vector<std::vector<u16>> ref;
    const FgKernelLanguage langs[3] = {FgKernelLanguage::Auto, FgKernelLanguage::Auto, FgKernelLanguage::Glsl};
    u32 identical = 0, compared = 0;
    for (u32 run = 0; run < 3u; ++run) {
        Runner r(ctx);
        if (!r.init(langs[run])) {
            if (run == 2u) {
                std::printf("determinism: GLSL twins not built, skipped\n");
                break;
            }
            return 1;
        }
        Ui ui{};
        ui.enabled = true;
        for (u32 n = 0; n < kFrames; ++n) {
            FrameInput in{};
            in.pattern = &pattern;
            in.offset = offsetAt(v, n);
            in.motionPx = v;
            in.frameId = n;
            ui.cursorX = 30u + 7u * n;
            ui.cursorY = 90u;
            in.ui = ui;
            FrameOut out{};
            if (!r.frame(in, &out)) {
                return 1;
            }
            std::vector<u16> both = out.interpolated;
            both.insert(both.end(), out.presentInterpolated.begin(), out.presentInterpolated.end());
            both.insert(both.end(), out.presentReal.begin(), out.presentReal.end());
            if (run == 0u) {
                ref.push_back(both);
            } else {
                ++compared;
                identical += both == ref[n] ? 1u : 0u;
            }
        }
        std::printf("determinism: run %u (%s FUSE passes) done\n", run, r.fg.kernelLanguage());
    }
    std::printf("determinism: %u / %u frames bit-identical to the first run\n", identical, compared);
    expect(compared >= kFrames && identical == compared, "outputs bit-identical across instances and FUSE-pass languages");
    return 0;
}

int runOpticalFlow(Context& ctx) {
    Runner r(ctx);
    if (!r.init()) {
        return 1;
    }
    const Pattern pattern{};
    const Vec2 v(4.f, 2.f);
    u32 good = 0, total = 0;
    for (u32 n = 0; n < 6; ++n) {
        FrameInput in{};
        in.pattern = &pattern;
        in.offset = offsetAt(v, n);
        in.motionPx = v;
        in.frameId = n;
        FrameOut out{};
        if (!r.frame(in, &out)) {
            return 1;
        }
        if (n < 2u) {
            continue;
        }
        u32 frameGood = 0, frameTotal = 0;
        i32 hx[3] = {}, hy[3] = {};
        for (u32 by = 2; by + 2 < r.ofH; ++by) {
            for (u32 bx = 2; bx + 2 < r.ofW; ++bx) {
                const i32 x = out.ofVector[(by * r.ofW + bx) * 2u];
                const i32 y = out.ofVector[(by * r.ofW + bx) * 2u + 1u];
                ++frameTotal;
                frameGood += (x == -static_cast<i32>(v.x) && y == -static_cast<i32>(v.y)) ? 1u : 0u;
                hx[0] += x == -4 ? 1 : 0;
                hx[1] += x == 4 ? 1 : 0;
                hx[2] += x == 0 ? 1 : 0;
                hy[0] += y == -2 ? 1 : 0;
                hy[1] += y == 2 ? 1 : 0;
                hy[2] += y == 0 ? 1 : 0;
            }
        }
        std::printf("optical_flow: frame %u: %u / %u interior blocks = (%d, %d) [x: -4 %d, +4 %d, 0 %d; y: -2 %d, +2 %d, 0 %d]\n", n, frameGood,
                    frameTotal, -static_cast<i32>(v.x), -static_cast<i32>(v.y), hx[0], hx[1], hx[2], hy[0], hy[1], hy[2]);
        good += frameGood;
        total += frameTotal;
    }
    std::printf("optical_flow (%s search): %u / %u = %.1f%% interior blocks exact\n", r.fg.portableSearch() ? "portable" : "vendored", good, total,
                100.0 * good / std::max(total, 1u));
    expect(total > 0u && good >= total * 9u / 10u, ">= 90% of interior optical-flow blocks = -v");
    return 0;
}

int runZeroAlloc(Context& ctx, bool countAllocations) {
    Runner r(ctx);
    if (!r.init()) {
        return 1;
    }
    const Pattern pattern{};
    const Vec2 v(3.f, 1.f);
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 n = 0; n < kTotal; ++n) {
        FrameInput in{};
        in.pattern = &pattern;
        in.offset = offsetAt(v, n % 8u);
        in.motionPx = v;
        in.frameId = n;
        in.ui.enabled = (n & 1u) != 0u;
        in.ui.cursorX = 40u;
        in.ui.cursorY = 40u;
        in.reset = n == 40u; // a reset frame inside the measured window (different plan length)
        r.measuring = countAllocations && n >= kWarmup;
        const bool upload = n < 2u || (n % 5u) == 0u;
        if (!r.frame(in, nullptr, upload)) {
            return 1;
        }
    }
    r.measuring = false;
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(r.conflicts == 0u, "no layout conflicts in the declared accesses");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u fg.* passes / frame, UI every other frame, one reset frame): "
                    "beginFrame %llu, graph build (imports + passes) %llu, fg.* callbacks %llu operator-new calls\n",
                    kTotal - kWarmup, r.fg.stats().passes, r.allocBegin, r.allocBuild, r.allocCallbacks);
        expect(r.allocBegin == 0u && r.allocBuild == 0u && r.allocCallbacks == 0u, "frame generation makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok, %u layout conflicts\n", kTotal, r.conflicts);
    }
    return 0;
}

int runLatencyProbe(Context& ctx) {
    present::LatencyProviderSources sources{};
    sources.device = ctx.device.get();
    present::LatencyProviderHandle h = present::create_latency_provider(present::LatencyPreference::Auto, sources);
    std::printf("latency_probe: backend %s — %s\n", h.provider != nullptr ? present::latency_backend_name(h.provider->backend()) : "(null)",
                h.reason.c_str());
    expect(h.provider != nullptr && h.provider->backend() == present::LatencyBackend::None && !h.provider->pacing(),
           "Lavapipe: no latency extension -> None");
    std::string reason;
    present::VkAntiLagBinding antiLag;
    expect(!antiLag.init(*ctx.device, reason) && reason.find("not enabled") != std::string::npos, "VK_AMD_anti_lag reported absent");
    present::VkLowLatency2Binding ll2;
    expect(!ll2.init(*ctx.device, nullptr, reason) && reason.find("not enabled") != std::string::npos, "VK_NV_low_latency2 reported absent");
    h = present::create_latency_provider(present::LatencyPreference::NvLowLatency2, sources);
    expect(h.provider->backend() == present::LatencyBackend::None, "explicit low_latency2 unavailable -> None");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "midpoint";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        }
    }
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, true);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "midpoint") {
            rc = runMidpoint(ctx, false);
        } else if (mode == "ui") {
            rc = runMidpoint(ctx, true);
        } else if (mode == "reset") {
            rc = runReset(ctx);
        } else if (mode == "determinism") {
            rc = runDeterminism(ctx);
        } else if (mode == "optical_flow") {
            rc = runOpticalFlow(ctx);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, false);
        } else if (mode == "latency_probe") {
            rc = runLatencyProbe(ctx);
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
        const int setupRc = setup(counted, false);
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
    std::printf("PASS %s\n", mode.c_str());
    return 0;
}

#endif
