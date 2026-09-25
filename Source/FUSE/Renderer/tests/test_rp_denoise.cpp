// WP-6.4 in-tree denoiser (SVGF / A-SVGF) Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_denoise_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): the synthetic noisy sequence (svgf_synthetic.hpp) is written to
// host-visible input buffers (signal, UV motion, linear depth, normal, A-SVGF gradient samples, the WP-4.1 /
// WP-1.5 layouts) and, for the G-buffer path, uploaded into an RGBA16F RT0 image by a test pass; SvgfDenoiser
// adds its passes; read-back copies take the state / work / output buffers and the output image. Both kernel
// languages built (Slang, GLSL) run every check.
//
//   --mode passes      each GPU pass against its CPU reference (svgf_kernel.hpp) on the GPU's own inputs of that
//                      pass (keepIntermediates: every a-trous / gradient iteration read back), 6 frames x 3
//                      configurations (61 x 37, odd extent):
//                        gi          RGB, f32x4 normal buffer, SVGF
//                        shadow      scalar, G-buffer RT0 image normal (signed oct, RGBA16F), A-SVGF
//                        reflection  RGB, normal buffer, A-SVGF, 4 a-trous iterations, history tap 1, 2 gradient its
//                      tolerance (per value): |gpu - cpu| <= 1e-4 |cpu| + 1e-5 max|cpu| (see kRelTol); the colour
//                      history == the tap iteration's result bit for bit; the RGBA16F image == the output buffer
//                      converted to half (a bracketing half: the conversion's rounding is implementation-defined). The whole chain (SvgfReference fed the same frames) is reported too.
//   --mode quality     the WP-6.4 acceptance on the GPU: variance reduction factor and bias against the converged
//                      reference (ensemble of 16 sequences x 16 frames, 128 x 96) for shadow (RT0 image normals) /
//                      reflection / GI, dynamic (moving shadow, A-SVGF) and static lighting (SVGF), with the
//                      thresholds of test_rp_denoise_cpu.cpp; disocclusion (history rejection) on the GPU; the A-SVGF
//                      lag test (GI x 0.25 at frame 10).
//   --mode zero_alloc  64 steady-state frames (A-SVGF GI denoiser + shadow denoiser on the RT0 image): 0
//                      operator-new calls in beginFrame, the denoise.* pass callbacks and the whole graph build
//                      (validated run first; validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/denoise/denoise_types.hpp>
#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/renderer/denoise/svgf_kernel.hpp>
#include <fuse/renderer/denoise/svgf_reference.hpp>
#include <fuse/renderer/denoise/svgf_synthetic.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

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
using namespace fuse::renderer::denoise;
namespace kernel = fuse::kernel;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using svgf_kernel::F2;
using svgf_kernel::F4;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

/// Per-pass parity tolerance: |gpu - cpu| <= kRelTol |cpu| + kAbsTol max|cpu| (per component, over the image).
/// Both sides run the same f32 operation sequence (GLSL `precise`, Slang -fp-mode precise, no contraction), so
/// +, -, * and the comparisons are bit-identical; only exp / sqrt / division may differ. Vulkan allows exp 3 + 2|x|
/// ULP (|x| <= ~20 for weights that matter: <= 43 ULP ~ 5e-6 relative), sqrt / inversesqrt 2-3 ULP, division
/// 2.5 ULP. A normalised weighted sum of positive terms keeps the weights' relative error (<= ~1e-5 through 25
/// taps); the variance's squared weights double it; m2 - m1^2 can cancel, hence the small absolute term scaled
/// by the image's magnitude. 1e-4 leaves a 10x margin over that budget.
constexpr f64 kRelTol = 1e-4;
constexpr f64 kAbsTol = 1e-5;

/// Same scenarios and numbers as test_rp_denoise_cpu.cpp kQuality (the WP-6.4 acceptance), applied to the GPU.
struct QualityThreshold {
    DenoiseSignal signal;
    bool dynamic; ///< moving shadow + A-SVGF, else static lighting + SVGF
    f64 minVrf;
    f64 maxRelBias;
};
constexpr u32 kQualityWidth = 128u;
constexpr u32 kQualityHeight = 96u;
constexpr QualityThreshold kQuality[] = {
    {DenoiseSignal::Shadow, true, 30.0, 0.08},     {DenoiseSignal::Shadow, false, 60.0, 0.05},
    {DenoiseSignal::Reflection, true, 60.0, 0.09}, {DenoiseSignal::Reflection, false, 60.0, 0.08},
    {DenoiseSignal::Gi, true, 60.0, 0.10},         {DenoiseSignal::Gi, false, 60.0, 0.09},
};

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

const char* signalName(DenoiseSignal s) {
    return s == DenoiseSignal::Shadow ? "shadow" : (s == DenoiseSignal::Reflection ? "reflection" : "gi");
}

// --- half floats (portable, round to nearest even) ------------------------------------------------
u16 halfBits(f32 value) {
    u32 x = 0;
    std::memcpy(&x, &value, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 absBits = x & 0x7FFFFFFFu;
    if (absBits >= 0x7F800000u) {
        return static_cast<u16>(sign | 0x7C00u | (absBits > 0x7F800000u ? 0x200u : 0u));
    }
    const int exp = static_cast<int>(absBits >> 23) - 127 + 15;
    u32 mant = absBits & 0x7FFFFFu;
    if (exp >= 31) {
        return static_cast<u16>(sign | 0x7C00u);
    }
    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<u16>(sign);
        }
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - exp);
        u32 half = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1u);
        const u32 halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half & 1u) != 0u)) {
            ++half;
        }
        return static_cast<u16>(sign | half);
    }
    u32 half = (static_cast<u32>(exp) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) {
        ++half;
    }
    return static_cast<u16>(sign | half);
}

f32 halfToFloat(u16 h) {
    const u32 sign = (static_cast<u32>(h) & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 bits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            int e = -1;
            do {
                ++e;
                mant <<= 1u;
            } while ((mant & 0x400u) == 0u);
            bits = sign | (static_cast<u32>(127 - 15 - e) << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127u - 15u) << 23) | (mant << 13);
    }
    f32 f = 0.f;
    std::memcpy(&f, &bits, 4);
    return f;
}

/// The stored half is one of the two halves bracketing `value` (Vulkan leaves the rounding of the f32 -> 16-bit
/// float conversion of a storage-image write to the implementation: Lavapipe truncates, the host rounds to
/// nearest even).
bool bracketingHalf(f32 value, u16 stored) {
    const f32 h = halfToFloat(stored);
    const f32 a = std::fabs(value);
    const f32 ulp = a < 6.103515625e-05f ? 5.9604644775390625e-08f : std::ldexp(1.f, std::ilogb(a) - 10);
    return std::fabs(h - value) < ulp || h == value;
}

// --- Vulkan context -------------------------------------------------------------------------------
enum InputBuffer : u32 { kInSignal = 0, kInMotion, kInDepth, kInNormal, kInGradient, kInCount };

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
    // Two input sets (two denoisers in one frame), each: signal / motion / depth / normal / gradient.
    Buffer inputs[2][kInCount]{};
    u8 inputQueues[2][kInCount] = {};
    Texture rt0{};          ///< RGBA16F G-buffer RT0 (signed oct normal in xy)
    u32 rt0Layout = 0;
    u8 rt0Queue = rg::kNoQueue;
    Buffer staging{};       ///< RT0 upload
    u8 stagingQueue = rg::kNoQueue;
    Buffer readback{};
    u8 readbackQueue = rg::kNoQueue;
    u64 serial = 0;

    void destroyTargets() {
        if (allocator == nullptr) {
            return;
        }
        for (auto& set : inputs) {
            for (Buffer& b : set) {
                if (b.handle != nullptr) {
                    allocator->destroyBuffer(b);
                }
                b = Buffer{};
            }
        }
        for (Buffer* b : {&staging, &readback}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
            *b = Buffer{};
        }
        if (rt0.image != nullptr) {
            allocator->destroyImage(rt0);
        }
        rt0 = Texture{};
        rt0Layout = 0;
        rt0Queue = stagingQueue = readbackQueue = rg::kNoQueue;
        for (auto& set : inputQueues) {
            for (u8& q : set) {
                q = rg::kNoQueue;
            }
        }
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
    instanceDesc.appName = "fuse_rp_denoise";
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
    const DenoiseCapabilities caps = queryDenoiseCapabilities(ctx.device.get());
    if (!caps.denoise) {
        std::printf("SKIP: denoiser unsupported: %s\n", caps.reason);
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

/// Read-back sections of one frame (sizes of the denoiser's buffers + the RGBA16F image).
struct ReadbackLayout {
    u64 state = 0;
    u64 work = 0;
    u64 output = 0;
    u64 image = 0;
    u64 end = 0;
};

ReadbackLayout readbackLayout(const SvgfBufferLayout& l) {
    ReadbackLayout r{};
    auto align = [](u64 v) { return (v + 255u) & ~u64{255u}; };
    r.state = 0;
    r.work = align(r.state + l.stateBytes);
    r.output = align(r.work + l.workBytes);
    r.image = align(r.output + l.outputBytes);
    r.end = r.image + static_cast<u64>(l.width) * l.height * 8u;
    return r;
}

bool ensureTargets(Context& ctx, u32 w, u32 h) {
    if (ctx.w == w && ctx.h == h && ctx.rt0.image != nullptr) {
        return true;
    }
    vkDeviceWaitIdle(ctx.vkDevice);
    ctx.destroyTargets();
    const u64 n = static_cast<u64>(w) * h;
    const u64 ns = static_cast<u64>(strata(w)) * strata(h);
    auto buffer = [&](Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = static_cast<usize>(size);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = name;
        return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
    };
    const BufferUsage inUsage =
        static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    const u64 sizes[kInCount] = {n * 16u, n * 8u, n * 4u, n * 16u, ns * 16u};
    const char* names[kInCount] = {"rp_denoise.signal", "rp_denoise.motion", "rp_denoise.depth", "rp_denoise.normal",
                                   "rp_denoise.gradient"};
    bool ok = true;
    for (auto& set : ctx.inputs) {
        for (u32 i = 0; i < kInCount; ++i) {
            ok = ok && buffer(set[i], sizes[i], inUsage, MemoryUsage::CpuToGpu, names[i]) && set[i].deviceAddress != 0u;
        }
    }
    const SvgfBufferLayout big = SvgfBufferLayout::compute(w, h, true);
    ok = ok && buffer(ctx.staging, n * 8u, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_denoise.staging") &&
         buffer(ctx.readback, readbackLayout(big).end, BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "rp_denoise.readback");
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.format = GpuFormat::R16G16B16A16Sfloat;
    d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
    d.name = "rp_denoise.rt0";
    ok = ok && ctx.allocator->createImage(d, ctx.rt0);
    ctx.w = w;
    ctx.h = h;
    return ok;
}

/// Host writes of one input set (+ the RT0 staging when `image`).
void writeInputs(Context& ctx, u32 set, const SyntheticFrame& f, bool image) {
    const usize n = static_cast<usize>(f.width) * f.height;
    Buffer* in = ctx.inputs[set];
    std::memcpy(in[kInSignal].mapped, f.signal.data(), f.signal.size() * sizeof(f32));
    std::memcpy(in[kInMotion].mapped, f.motion.data(), n * sizeof(F2));
    std::memcpy(in[kInDepth].mapped, f.depth.data(), n * sizeof(f32));
    std::memcpy(in[kInNormal].mapped, f.normals.data(), n * sizeof(F4));
    std::memcpy(in[kInGradient].mapped, f.gradient.data(), f.gradient.size() * sizeof(F4));
    if (image) {
        u16* s = static_cast<u16*>(ctx.staging.mapped);
        for (usize i = 0; i < n; ++i) {
            s[i * 4u + 0u] = halfBits(f.normalOct[i].x);
            s[i * 4u + 1u] = halfBits(f.normalOct[i].y);
            s[i * 4u + 2u] = halfBits(0.f);
            s[i * 4u + 3u] = halfBits(1.f);
        }
    }
}

DenoiseFrameDesc frameDesc(Context& ctx, u32 set, bool image) {
    DenoiseFrameDesc d{};
    d.width = ctx.w;
    d.height = ctx.h;
    d.signal = ctx.inputs[set][kInSignal].deviceAddress;
    d.motion = ctx.inputs[set][kInMotion].deviceAddress;
    d.depth = ctx.inputs[set][kInDepth].deviceAddress;
    d.normal = image ? 0u : ctx.inputs[set][kInNormal].deviceAddress;
    d.normalImage = image ? &ctx.rt0 : nullptr;
    d.gradient = ctx.inputs[set][kInGradient].deviceAddress;
    return d;
}

// --- per-frame graph ------------------------------------------------------------------------------
struct TestRecord {
    Context* ctx = nullptr;
    rg::BufferRef staging;
    rg::TextureRef rt0;
    rg::BufferRef readback;
    DenoiseGraphRefs refs{};
    ReadbackLayout rb{};
    const SvgfBufferLayout* layout = nullptr;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const TestRecord& r = *static_cast<const TestRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.ctx->w, r.ctx->h, 1};
    vkCmdCopyBufferToImage(cmd, static_cast<VkBuffer>(pc.buffer(r.staging)), static_cast<VkImage>(pc.image(r.rt0)),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void recordReadback(const rg::PassContext& pc, void* user) {
    const TestRecord& r = *static_cast<const TestRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const VkBuffer dst = static_cast<VkBuffer>(pc.buffer(r.readback));
    const VkBufferCopy state{0, r.rb.state, r.layout->stateBytes};
    const VkBufferCopy work{0, r.rb.work, r.layout->workBytes};
    const VkBufferCopy output{0, r.rb.output, r.layout->outputBytes};
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(r.refs.state)), dst, 1, &state);
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(r.refs.work)), dst, 1, &work);
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(r.refs.output)), dst, 1, &output);
    VkBufferImageCopy region{};
    region.bufferOffset = r.rb.image;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.ctx->w, r.ctx->h, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(r.refs.outputImage)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                           1, &region);
}

/// What one frame read back.
struct Captured {
    std::vector<u8> state;
    std::vector<u8> work;
    std::vector<F4> output;
    std::vector<u16> image;
};

std::vector<F4> section(const std::vector<u8>& bytes, u64 offset, usize count) {
    std::vector<F4> v(count);
    std::memcpy(v.data(), bytes.data() + offset, count * sizeof(F4));
    return v;
}
std::vector<F2> section2(const std::vector<u8>& bytes, u64 offset, usize count) {
    std::vector<F2> v(count);
    std::memcpy(v.data(), bytes.data() + offset, count * sizeof(F2));
    return v;
}

TestRecord g_records[2]{};
rg::BufferRef g_inputRefs[2][kInCount]{};

/// Imports input set `set` (and the RT0 image + its upload when `image`) into the graph.
DenoiseGraphInputs importInputs(Context& ctx, rg::Graph& graph, u32 set, bool image, TestRecord& r) {
    const char* names[kInCount] = {"rp_denoise.signal", "rp_denoise.motion", "rp_denoise.depth", "rp_denoise.normal",
                                   "rp_denoise.gradient"};
    for (u32 i = 0; i < kInCount; ++i) {
        Buffer& b = ctx.inputs[set][i];
        g_inputRefs[set][i] = graph.importBuffer(rg::ImportedBuffer{b.handle, b.desc.size, ctx.inputQueues[set][i],
                                                                    &ctx.inputQueues[set][i], names[i]});
    }
    DenoiseGraphInputs in{};
    in.signal = g_inputRefs[set][kInSignal];
    in.motion = g_inputRefs[set][kInMotion];
    in.depth = g_inputRefs[set][kInDepth];
    in.gradient = g_inputRefs[set][kInGradient];
    if (image) {
        r.staging = graph.importBuffer(
            rg::ImportedBuffer{ctx.staging.handle, ctx.staging.desc.size, ctx.stagingQueue, &ctx.stagingQueue, "rp_denoise.staging"});
        rg::ImportedImage im{};
        im.image = ctx.rt0.image;
        im.view = ctx.rt0.view;
        im.format = static_cast<u32>(GpuFormat::R16G16B16A16Sfloat);
        im.width = ctx.w;
        im.height = ctx.h;
        im.initialLayout = ctx.rt0Layout;
        im.initialQueue = ctx.rt0Queue;
        im.layoutTracker = &ctx.rt0Layout;
        im.queueTracker = &ctx.rt0Queue;
        im.name = "rp_denoise.rt0";
        r.rt0 = graph.importImage(im);
        graph.addPass("test.upload_rt0", &recordUpload, &r).use(r.staging, rg::Access::TransferSrc).use(r.rt0, rg::Access::TransferDst);
        in.normalImage = r.rt0;
    } else {
        in.normal = g_inputRefs[set][kInNormal];
    }
    return in;
}

/// One frame of one denoiser (input set 0). `out` != nullptr: read everything back.
bool runFrame(Context& ctx, SvgfDenoiser& dn, rg::Graph& graph, const SyntheticFrame& f, bool image, Captured* out) {
    writeInputs(ctx, 0u, f, image);
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!dn.beginFrame(ctx.serial, frameDesc(ctx, 0u, image))) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    graph.reset();
    TestRecord& r = g_records[0];
    r = TestRecord{};
    r.ctx = &ctx;
    const DenoiseGraphInputs inputs = importInputs(ctx, graph, 0u, image, r);
    r.refs = dn.importInto(graph);
    dn.addPasses(graph, r.refs, inputs);
    r.layout = &dn.layout();
    r.rb = readbackLayout(dn.layout());
    if (out != nullptr) {
        r.readback = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, ctx.readbackQueue,
                                                           &ctx.readbackQueue, "rp_denoise.readback"});
        graph.addPass("test.readback", &recordReadback, &r)
            .use(r.refs.state, rg::Access::TransferSrc)
            .use(r.refs.work, rg::Access::TransferSrc)
            .use(r.refs.output, rg::Access::TransferSrc)
            .use(r.refs.outputImage, rg::Access::TransferSrc)
            .use(r.readback, rg::Access::TransferDst, rg::BufferRange{0, r.rb.end});
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    dn.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    if (out != nullptr) {
        const u8* base = static_cast<const u8*>(ctx.readback.mapped);
        const SvgfBufferLayout& l = dn.layout();
        const usize n = static_cast<usize>(l.width) * l.height;
        out->state.assign(base + r.rb.state, base + r.rb.state + l.stateBytes);
        out->work.assign(base + r.rb.work, base + r.rb.work + l.workBytes);
        out->output.resize(n);
        std::memcpy(out->output.data(), base + r.rb.output, n * sizeof(F4));
        out->image.resize(n * 4u);
        std::memcpy(out->image.data(), base + r.rb.image, n * 8u);
    }
    return true;
}

// --- comparisons ----------------------------------------------------------------------------------
struct Compare {
    u32 values = 0;
    u32 mismatches = 0;
    u32 exact = 0;
    f64 worst = 0.0; ///< max |gpu - cpu| / (kRelTol |cpu| + kAbsTol max|cpu|)
};

void compareInto(Compare& c, const f32* gpu, const f32* cpu, usize count, u32 components, u32 stride) {
    for (u32 k = 0; k < components; ++k) {
        f64 m = 0.0;
        for (usize i = 0; i < count; ++i) {
            m = std::max(m, std::fabs(static_cast<f64>(cpu[i * stride + k])));
        }
        for (usize i = 0; i < count; ++i) {
            const f64 g = gpu[i * stride + k];
            const f64 r = cpu[i * stride + k];
            const f64 tol = kRelTol * std::fabs(r) + kAbsTol * m;
            const f64 e = std::fabs(g - r);
            ++c.values;
            c.exact += std::memcmp(&gpu[i * stride + k], &cpu[i * stride + k], 4u) == 0 ? 1u : 0u;
            if (!(e <= tol) && !(e == 0.0)) {
                ++c.mismatches;
            }
            c.worst = std::max(c.worst, tol > 0.0 ? e / tol : (e > 0.0 ? 1e30 : 0.0));
        }
    }
}
void compare4(Compare& c, const std::vector<F4>& gpu, const std::vector<F4>& cpu) {
    compareInto(c, &gpu[0].x, &cpu[0].x, gpu.size(), 4u, 4u);
}
void compare2(Compare& c, const std::vector<F2>& gpu, const std::vector<F2>& cpu) {
    compareInto(c, &gpu[0].x, &cpu[0].x, gpu.size(), 2u, 2u);
}

template <typename T>
kernel::Span<const T> cs(const std::vector<T>& v) {
    return kernel::Span<const T>{v.data(), static_cast<u32>(v.size())};
}
template <typename T>
kernel::Span<T> ms(std::vector<T>& v) {
    return kernel::Span<T>{v.data(), static_cast<u32>(v.size())};
}

enum PassId : u32 { kPGuide = 0, kPGradPrep, kPGradAtrous, kPTemporal, kPVariance, kPAtrous, kPCount };
const char* kPassNames[kPCount] = {"denoise.guide", "denoise.gradient.prepare", "denoise.gradient.atrous",
                                   "denoise.temporal", "denoise.variance", "denoise.atrous"};

/// Every pass of the frame just run, on the GPU's own inputs of that pass.
void checkPasses(const SvgfDenoiser& dn, const SyntheticFrame& f, bool image, const Captured& cap, Compare (&cmp)[kPCount],
                 u32& historyMismatch, u32& imageMismatch) {
    using namespace svgf_kernel;
    const DenoiseFrameConstants& c = dn.constants();
    const SvgfBufferLayout& l = dn.layout();
    const usize n = static_cast<usize>(l.width) * l.height;
    const usize ns = static_cast<usize>(l.strataW) * l.strataH;
    const u32 cur = dn.parity();
    const u32 prev = cur ^ 1u;
    const std::vector<F4> guideCur = section(cap.state, l.guide[cur], n);
    const std::vector<F4> guidePrev = section(cap.state, l.guide[prev], n);
    const std::vector<F4> histCur = section(cap.state, l.hist[cur], n);
    const std::vector<F4> histPrev = section(cap.state, l.hist[prev], n);
    const std::vector<F4> momCur = section(cap.state, l.mom[cur], n);
    const std::vector<F4> momPrev = section(cap.state, l.mom[prev], n);
    const std::vector<F2> gradZ = section2(cap.work, l.gradZ, n);
    const std::vector<F4> accum = section(cap.work, l.accum, n);
    const std::vector<F4> variance = section(cap.work, l.variance, n);
    const kernel::Backend be = kernel::Backend::CpuReference;

    // guide
    {
        std::vector<F4> g(n);
        std::vector<F2> gz(n);
        GuideParams p{};
        p.c = c;
        p.depth = cs(f.depth);
        p.normals = image ? cs(f.normalOct) : cs(f.normals);
        p.guide = ms(g);
        p.gradZ = ms(gz);
        kernel::launch(be, make_launch(kNameGuide, l.width, l.height), GuideKernel{}, p);
        compare4(cmp[kPGuide], guideCur, g);
        compare2(cmp[kPGuide], gradZ, gz);
    }
    // gradients
    if ((c.flags & kDenoiseFlagGradients) != 0u) {
        std::vector<F4> g(ns);
        GradientParams p{};
        p.c = c;
        p.guide = cs(guideCur);
        p.src = cs(f.gradient);
        p.dst = ms(g);
        p.mode = kDenoiseGradientPrepare;
        kernel::launch(be, make_launch(kNameGradientPrepare, l.strataW, l.strataH), GradientKernel{}, p);
        compare4(cmp[kPGradPrep], section(cap.work, l.gradient[0], ns), g);
        for (u32 k = 0; k < c.gradientIterations; ++k) {
            const std::vector<F4> src = section(cap.work, l.gradient[k], ns);
            p.src = cs(src);
            p.mode = kDenoiseGradientAtrous;
            p.step = 1u << k;
            kernel::launch(be, make_launch(kNameGradientAtrous, l.strataW, l.strataH), GradientKernel{}, p);
            compare4(cmp[kPGradAtrous], section(cap.work, l.gradient[k + 1u], ns), g);
        }
    }
    // temporal
    const std::vector<F4> lambda = section(cap.work, l.gradient[c.gradientIterations], ns);
    {
        std::vector<F4> a(n);
        std::vector<F4> m(n);
        TemporalParams p{};
        p.c = c;
        p.signal = cs(f.signal);
        p.motion = cs(f.motion);
        p.guideCur = cs(guideCur);
        p.guidePrev = cs(guidePrev);
        p.gradZ = cs(gradZ);
        p.histPrev = cs(histPrev);
        p.momPrev = cs(momPrev);
        p.lambda = cs(lambda);
        p.accum = ms(a);
        p.momCur = ms(m);
        kernel::launch(be, make_launch(kNameTemporal, l.width, l.height), TemporalKernel{}, p);
        compare4(cmp[kPTemporal], accum, a);
        compare4(cmp[kPTemporal], momCur, m);
    }
    // variance
    {
        std::vector<F4> v(n);
        VarianceParams p{};
        p.c = c;
        p.guide = cs(guideCur);
        p.gradZ = cs(gradZ);
        p.accum = cs(accum);
        p.moments = cs(momCur);
        p.dst = ms(v);
        kernel::launch(be, make_launch(kNameVariance, l.width, l.height), VarianceKernel{}, p);
        compare4(cmp[kPVariance], variance, v);
    }
    // a-trous
    for (u32 k = 0; k < c.atrousIterations; ++k) {
        const bool last = k + 1u == c.atrousIterations;
        const std::vector<F4> src = k == 0u ? variance : section(cap.work, l.atrous[k - 1u], n);
        const std::vector<F4> gpu = last ? cap.output : section(cap.work, l.atrous[k], n);
        std::vector<F4> o(n);
        AtrousParams p{};
        p.c = c;
        p.guide = cs(guideCur);
        p.gradZ = cs(gradZ);
        p.src = cs(src);
        p.dst = ms(o);
        p.step = 1u << k;
        kernel::launch(be, make_launch(kNameAtrous, l.width, l.height), AtrousKernel{}, p);
        compare4(cmp[kPAtrous], gpu, o);
        if (k == c.historyTap) {
            for (usize i = 0; i < n; ++i) {
                const F4 want{gpu[i].x, gpu[i].y, gpu[i].z, 0.f};
                historyMismatch += std::memcmp(&want, &histCur[i], sizeof(F4)) != 0 ? 1u : 0u;
            }
        }
    }
    // output image == output buffer rounded to half (scalar: (v, v, v, var))
    const bool scalar = (c.flags & kDenoiseFlagScalar) != 0u;
    for (usize i = 0; i < n; ++i) {
        const F4& o = cap.output[i];
        const f32 want[4] = {o.x, scalar ? o.x : o.y, scalar ? o.x : o.z, o.w};
        for (u32 k = 0; k < 4u; ++k) {
            imageMismatch += bracketingHalf(want[k], cap.image[i * 4u + k]) ? 0u : 1u;
        }
    }
}

// --- passes ----------------------------------------------------------------------------------------
struct Lang {
    DenoiseKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{DenoiseKernelLanguage::Slang, "slang"}, {DenoiseKernelLanguage::Glsl, "glsl"}};

struct PassConfig {
    const char* name;
    DenoiseSignal signal;
    bool image;
    bool gradients;
    u32 atrous;
    u32 historyTap;
    u32 gradientIterations;
};
constexpr PassConfig kPassConfigs[] = {
    {"gi (normal buffer, SVGF)", DenoiseSignal::Gi, false, false, 5u, 0u, 3u},
    {"shadow (RT0 image, A-SVGF)", DenoiseSignal::Shadow, true, true, 5u, 0u, 3u},
    {"reflection (normal buffer, A-SVGF, 4 it, tap 1)", DenoiseSignal::Reflection, false, true, 4u, 1u, 2u},
};

int runPasses(Context& ctx) {
    constexpr u32 kW = 61;
    constexpr u32 kH = 37;
    constexpr u32 kFrames = 6;
    if (!ensureTargets(ctx, kW, kH)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    u32 languages = 0;
    std::vector<std::vector<F4>> firstLangOutputs;
    rg::Graph graph;
    for (const Lang& lang : kLangs) {
        SvgfDenoiser dn;
        SvgfDenoiserDesc desc{};
        desc.device = ctx.device.get();
        desc.allocator = ctx.allocator.get();
        desc.bindless = &ctx.bindless;
        desc.language = lang.language;
        desc.keepIntermediates = true;
        if (!dn.init(desc)) {
            std::printf("passes: %s kernels not built, skipped\n", lang.name);
            continue;
        }
        ++languages;
        u32 cfgIndex = 0;
        for (const PassConfig& cfg : kPassConfigs) {
            SvgfSettings s = svgf_preset(cfg.signal);
            s.gradients = cfg.gradients;
            s.atrousIterations = cfg.atrous;
            s.historyTap = cfg.historyTap;
            s.gradientIterations = cfg.gradientIterations;
            dn.setSettings(s);
            dn.reset();
            SyntheticDesc sd{};
            sd.width = kW;
            sd.height = kH;
            sd.signal = cfg.signal;
            sd.lightStepFrame = 4u;
            sd.lightStepScale = 0.5f;
            SvgfReference chain;
            chain.init(kW, kH, s);
            Compare cmp[kPCount]{};
            u32 historyMismatch = 0;
            u32 imageMismatch = 0;
            f64 chainWorst = 0.0;
            u32 historyFrames = 0;
            SyntheticFrame f;
            Captured cap;
            for (u32 t = 0; t < kFrames; ++t) {
                synthetic_frame(sd, t, 41u + cfgIndex, f);
                if (!runFrame(ctx, dn, graph, f, cfg.image, &cap)) {
                    return 1;
                }
                historyFrames += dn.stats().history ? 1u : 0u;
                checkPasses(dn, f, cfg.image, cap, cmp, historyMismatch, imageMismatch);
                SvgfReferenceInputs in{};
                in.signal = f.signal.data();
                in.motion = f.motion.data();
                in.depth = f.depth.data();
                in.normals = cfg.image ? f.normalOct.data() : f.normals.data();
                in.normalOct = cfg.image;
                in.gradient = cfg.gradients ? f.gradient.data() : nullptr;
                chain.runFrame(in);
                for (usize i = 0; i < cap.output.size(); ++i) {
                    const f32* g = &cap.output[i].x;
                    const f32* r = &chain.output()[i].x;
                    for (u32 k = 0; k < 4u; ++k) {
                        chainWorst = std::max(chainWorst, std::fabs(static_cast<f64>(g[k]) - r[k]) / (std::fabs(static_cast<f64>(r[k])) + 1e-3));
                    }
                }
                if (languages == 1u) {
                    firstLangOutputs.push_back(cap.output);
                } else {
                    const std::vector<F4>& other = firstLangOutputs[cfgIndex * kFrames + t];
                    if (std::memcmp(other.data(), cap.output.data(), other.size() * sizeof(F4)) != 0 && t + 1u == kFrames) {
                        std::printf("  (slang and glsl outputs differ in bits; each is checked against the CPU)\n");
                    }
                }
            }
            std::printf("passes [%s] %s: %u frames (%u with history)\n", lang.name, cfg.name, kFrames, historyFrames);
            for (u32 p = 0; p < kPCount; ++p) {
                if (cmp[p].values == 0u) {
                    continue;
                }
                std::printf("  %-26s %8u values, %8u bit-exact, %u over tolerance, worst %.3f of tolerance\n", kPassNames[p],
                            cmp[p].values, cmp[p].exact, cmp[p].mismatches, cmp[p].worst);
                expect(cmp[p].mismatches == 0u, "GPU pass == CPU reference within tolerance");
            }
            std::printf("  history tap == colour history (bits): %s; RGBA16F image == output as a bracketing half: %s; whole chain "
                        "vs SvgfReference: max |d| / (|ref| + 1e-3) = %.2e\n",
                        historyMismatch == 0u ? "yes" : "NO", imageMismatch == 0u ? "yes" : "NO", chainWorst);
            expect(historyMismatch == 0u, "the history tap's result is the colour history");
            expect(chainWorst <= 1e-3, "whole GPU chain == SvgfReference within 1e-3 (relative, 6 frames)");
            expect(imageMismatch == 0u, "output image == output buffer converted to half (bracketing half)");
            expect(historyFrames == kFrames - 1u, "every frame after the first reprojects");
            expect(cmp[kPTemporal].values > 0u && cmp[kPAtrous].values > 0u, "passes checked");
            if (cfg.gradients) {
                expect(cmp[kPGradPrep].values > 0u && cmp[kPGradAtrous].values > 0u, "gradient passes checked");
            }
            ++cfgIndex;
        }
        dn.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no denoise kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- quality ---------------------------------------------------------------------------------------
bool initDenoiser(Context& ctx, SvgfDenoiser& dn, bool keep = false) {
    SvgfDenoiserDesc desc{};
    desc.device = ctx.device.get();
    desc.allocator = ctx.allocator.get();
    desc.bindless = &ctx.bindless;
    desc.keepIntermediates = keep;
    return dn.init(desc);
}

int runQuality(Context& ctx) {
    constexpr u32 kFrames = 16;
    constexpr u32 kRealisations = 16;
    if (!ensureTargets(ctx, kQualityWidth, kQualityHeight)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    SvgfDenoiser dn;
    if (!initDenoiser(ctx, dn)) {
        std::printf("SKIP: no denoise kernel built\n");
        return kSkip;
    }
    std::printf("quality: %s kernels, %u x %u, ensemble of %u sequences x %u frames\n", dn.kernelLanguage(), kQualityWidth,
                kQualityHeight, kRealisations, kFrames);
    rg::Graph graph;
    usize n = static_cast<usize>(kQualityWidth) * kQualityHeight;
    Captured cap;
    SyntheticFrame f, prev;
    for (const QualityThreshold& q : kQuality) {
        SyntheticDesc sd{};
        sd.width = kQualityWidth;
        sd.height = kQualityHeight;
        sd.signal = q.signal;
        if (!q.dynamic) {
            sd.objectSpeedPx = -sd.panPx;
        }
        SvgfSettings s = svgf_preset(q.signal);
        s.gradients = q.dynamic;
        dn.setSettings(s);
        LuminanceEnsemble in, out;
        in.init(n);
        out.init(n);
        for (u32 r = 0; r < kRealisations; ++r) {
            dn.reset();
            for (u32 t = 0; t < kFrames; ++t) {
                synthetic_frame(sd, t, 1000u + r * 7919u, f);
                const bool last = t + 1u == kFrames;
                // Shadows read the normal from the G-buffer RT0 image, the RGB signals from the f32x4 buffer.
                if (!runFrame(ctx, dn, graph, f, q.signal == DenoiseSignal::Shadow, last ? &cap : nullptr)) {
                    return 1;
                }
            }
            in.add(f.signal.data(), f.stride);
            out.add(cap.output, q.signal == DenoiseSignal::Shadow);
        }
        std::vector<u8> surface(n);
        for (usize i = 0; i < n; ++i) {
            surface[i] = f.surface[i] != kSurfSky ? 1u : 0u;
        }
        const QualityReport rep = evaluate_quality(in, out, f.truth, f.stride, surface);
        std::printf("quality %-10s %-7s (%s): VRF %.1f (input var %.4f -> %.6f), RMS bias %.2f%% of mean %.3f (input %.2f%%), "
                    "RMSE %.4f -> %.4f, %u px  [VRF >= %.0f, bias <= %.0f%%]\n",
                    signalName(q.signal), q.dynamic ? "dynamic" : "static", q.dynamic ? "A-SVGF" : "SVGF", rep.vrf,
                    rep.inputVariance, rep.outputVariance, 100.0 * rep.relRmsBias, rep.meanTruth, 100.0 * rep.inputRelRmsBias,
                    rep.inputRmse, rep.outputRmse, rep.pixels, q.minVrf, 100.0 * q.maxRelBias);
        expect(rep.vrf >= q.minVrf, "GPU variance reduction factor meets the WP-6.4 threshold");
        expect(rep.relRmsBias <= q.maxRelBias, "GPU bias against the converged reference within the WP-6.4 threshold");
    }

    // The disocclusion and lag checks run at 64 x 48 (the CPU gates' extent).
    if (!ensureTargets(ctx, 64u, 48u)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    n = static_cast<usize>(64u) * 48u;
    // Disocclusion on the GPU: temporal result + moments of each frame.
    {
        SvgfDenoiser keep;
        if (!initDenoiser(ctx, keep, true)) {
            return 1;
        }
        keep.setSettings(svgf_preset(DenoiseSignal::Gi));
        SyntheticDesc sd{};
        sd.objectSpeedPx = 2.25f;
        std::vector<u8> disocc, stable;
        u32 total = 0, restart = 0, exact = 0, stableTotal = 0, kept = 0;
        for (u32 t = 0; t < 10u; ++t) {
            std::swap(prev, f);
            synthetic_frame(sd, t, 11u, f);
            if (!runFrame(ctx, keep, graph, f, false, &cap)) {
                return 1;
            }
            if (t == 0u) {
                continue;
            }
            const SvgfBufferLayout& l = keep.layout();
            const std::vector<F4> mom = section(cap.state, l.mom[keep.parity()], n);
            const std::vector<F4> acc = section(cap.work, l.accum, n);
            synthetic_disocclusion(prev, f, disocc, stable);
            for (usize i = 0; i < n; ++i) {
                if (disocc[i] != 0u) {
                    ++total;
                    restart += mom[i].z == 1.f ? 1u : 0u;
                    exact += acc[i].x == f.signal[i * 4u] && acc[i].y == f.signal[i * 4u + 1u] && acc[i].z == f.signal[i * 4u + 2u] ? 1u : 0u;
                }
                if (stable[i] != 0u) {
                    ++stableTotal;
                    kept += mom[i].z > 1.f ? 1u : 0u;
                }
            }
        }
        std::printf("disocclusion (GPU): %u disoccluded pixels over 9 frames: %u restart (length 1), %u temporal result == new "
                    "sample; %u / %u stable pixels keep history\n",
                    total, restart, exact, kept, stableTotal);
        expect(total >= 100u && restart == total && exact == total, "GPU: every disoccluded pixel rejects its history");
        expect(kept == stableTotal && stableTotal > 1000u, "GPU: stable pixels keep their history");
        keep.destroy();
    }

    // A-SVGF lag on the GPU.
    f64 relErr[2] = {0.0, 0.0};
    for (const bool gradients : {false, true}) {
        SvgfSettings s = svgf_preset(DenoiseSignal::Gi);
        s.gradients = gradients;
        dn.setSettings(s);
        SyntheticDesc sd{};
        sd.lightStepFrame = 10u;
        sd.lightStepScale = 0.25f;
        f64 err = 0.0;
        f64 truth = 0.0;
        for (u32 r = 0; r < 4u; ++r) {
            dn.reset();
            for (u32 t = 0; t < 14u; ++t) {
                synthetic_frame(sd, t, 500u + r * 31u, f);
                const bool after = t >= 10u;
                if (!runFrame(ctx, dn, graph, f, false, after ? &cap : nullptr)) {
                    return 1;
                }
                for (usize i = 0; i < n && after; ++i) {
                    if (f.surface[i] == kSurfSky) {
                        continue;
                    }
                    const F4& o = cap.output[i];
                    const f64 lt = synthetic_luminance(&f.truth[i * 4u], 4u);
                    err += std::fabs(0.2126 * o.x + 0.7152 * o.y + 0.0722 * o.z - lt);
                    truth += lt;
                }
            }
        }
        relErr[gradients ? 1 : 0] = err / truth;
    }
    std::printf("asvgf (GPU): light x 0.25 at frame 10, mean relative error over frames 10..13: SVGF %.3f, A-SVGF %.3f\n",
                relErr[0], relErr[1]);
    expect(relErr[1] <= 0.5 * relErr[0], "GPU: A-SVGF halves the temporal lag error after a lighting change");
    dn.destroy();
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "denoise.", 8) == 0) {
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
    constexpr u32 kW = 64;
    constexpr u32 kH = 40;
    if (!ensureTargets(ctx, kW, kH)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    SvgfDenoiser gi, shadow;
    if (!initDenoiser(ctx, gi) || !initDenoiser(ctx, shadow)) {
        std::printf("SKIP: no denoise kernel built\n");
        return kSkip;
    }
    SvgfSettings gs = svgf_preset(DenoiseSignal::Gi);
    gs.gradients = true;
    gi.setSettings(gs);
    shadow.setSettings(svgf_preset(DenoiseSignal::Shadow));
    SyntheticDesc giDesc{};
    giDesc.width = kW;
    giDesc.height = kH;
    SyntheticDesc shDesc = giDesc;
    shDesc.signal = DenoiseSignal::Shadow;
    // Pre-generated content (the generator allocates; it is not under test).
    std::vector<SyntheticFrame> giFrames(4), shFrames(4);
    for (u32 i = 0; i < 4u; ++i) {
        synthetic_frame(giDesc, i + 1u, 5u, giFrames[i]);
        synthetic_frame(shDesc, i + 1u, 6u, shFrames[i]);
    }
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        writeInputs(ctx, 0u, giFrames[frame % 4u], false);
        writeInputs(ctx, 1u, shFrames[frame % 4u], true);
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        DenoiseFrameDesc sdesc = frameDesc(ctx, 1u, true);
        sdesc.reset = frame % 37u == 0u;
        const bool begun = gi.beginFrame(ctx.serial, frameDesc(ctx, 0u, false)) && shadow.beginFrame(ctx.serial, sdesc);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        graph.reset();
        g_records[0] = TestRecord{};
        g_records[0].ctx = &ctx;
        g_records[1] = TestRecord{};
        g_records[1].ctx = &ctx;
        const DenoiseGraphInputs giIn = importInputs(ctx, graph, 0u, false, g_records[0]);
        const DenoiseGraphInputs shIn = importInputs(ctx, graph, 1u, true, g_records[1]);
        const DenoiseGraphRefs giRefs = gi.importInto(graph);
        gi.addPasses(graph, giRefs, giIn);
        const DenoiseGraphRefs shRefs = shadow.importInto(graph);
        shadow.addPasses(graph, shRefs, shIn);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        gi.collectRetired(ctx.serial);
        shadow.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(gi.stats().passes == 4u + gs.gradientIterations + gs.atrousIterations && shadow.stats().passes == 3u + 5u,
               "every pass runs in steady state");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = gi.stats().stateRebuilds + shadow.stats().stateRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(gi.stats().stateRebuilds + shadow.stats().stateRebuilds == rebuilds && rebuilds == 2u,
           "no resource rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; A-SVGF GI on a normal buffer + shadow on the RT0 image, content\n"
                    "  changing, periodic resets)\n"
                    "  SvgfDenoiser::beginFrame + denoise.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (test imports / upload + denoiser imports and passes): %llu\n",
                    kTotal - kWarmup, gi.kernelLanguage(), side, callbacks, build);
        expect(side == 0u, "the denoiser makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the denoiser passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal, gi.kernelLanguage());
    }
    gi.destroy();
    shadow.destroy();
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
        } else if (mode == "quality") {
            rc = runQuality(ctx);
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
