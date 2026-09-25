// WP-4.5 GPU post stack Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). CPU gates: test_rp_post_gpu_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): a test pass uploads the inputs (RGBA16F HDR image in the
// WP-2.1 lit-image format, R32F linear depth, RG16F velocity) from a staging buffer, then PostStackGpu
// adds its passes, then read-back copies (output image, the display transform's input, the bloom image,
// the exposure state). The CPU oracle gets the same (half-rounded) inputs. Both kernel languages built
// (Slang, GLSL) run every check.
//
//   --mode passes   each pass against its CPU reference, one pass enabled at a time (61 x 37, odd extent):
//                     bloom         display input == renderer::bloom_composite, bloom image ==
//                                   renderer::bloom_image, relative 1/1024 (7-level pyramid)
//                     dof           == renderer::dof_pass (relative 1/1024)
//                     motion blur   == renderer::motion_blur_pass, with and without depth (relative 1/1024)
//                     exposure      5 frames of changing content: histogram == LuminanceHistogram (at
//                                   most count / 1024 samples in another bin), adapted EV ==
//                                   AutoExposure::updateFromHistogram within 1/1024 EV; EMA variant; reset
//                     tonemap       ACES / Filmic / Reinhard / Neutral / AgX x (no curve, filmic, Reinhard,
//                                   ACES curve), display == display_reference (absolute 1/1024)
//                     grade         PostStack lift / contrast / saturation / gamma / gain, vignette, grain
//                     LUT           a baked look grade LUT (look vignette + grain), == the Look kernels
//                   plus the RGBA16F output image == the f32 dump rounded to half.
//   --mode stack    the full PostStack chain (bloom, DoF, motion blur, auto exposure, filmic curve, grade,
//                   vignette, grain), 4 frames: display == CPU (spatial chain, LuminanceHistogram +
//                   AutoExposure::updateFromHistogram, PostStack::processFrame) within 1/1024.
//   --mode look     "no-op Look is bit-identical to PostStack": a no-op Look driven through the Look node
//                   API (PostStackGpu::beginLookFrame) produces the same bits (dump and image) as the
//                   PostStack it feeds (apply_look_to_post_stack -> settings_from_post_stack), for each
//                   operator; both == PostStack::processFrame within 1/1024. The default look (DoF, motion
//                   blur, bloom, LUT grade, vignette, grain) == the CPU LookPostChain within 1/1024.
//   --mode zero_alloc  64 steady-state frames (full stack incl. auto exposure; content, seed, look / stack
//                   alternating): 0 operator-new calls in PostStackGpu::beginFrame / beginLookFrame, the
//                   post.* pass callbacks and the whole graph build (validated run first; validation off
//                   for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/look_post_chain.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/postprocess/gpu/post_gpu_reference.hpp>
#include <fuse/renderer/postprocess/gpu/post_stack_gpu.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
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
using namespace fuse::renderer::post_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec2;
using fuse::math::Vec3;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr f32 kTol = 1.f / 1024.f; ///< the WP-4.5 acceptance: each pass within 1/1024 of its CPU reference
constexpr u32 kPassW = 61;
constexpr u32 kPassH = 37;

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

f32 roundHalf(f32 v) { return halfToFloat(halfBits(v)); }

u32 hashU32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
f32 rnd(u32 seed) { return static_cast<f32>(hashU32(seed) & 0xFFFFFFu) / 16777216.f; }

// --- inputs ---------------------------------------------------------------------------------------
struct Inputs {
    u32 w = 0;
    u32 h = 0;
    std::vector<Vec3> hdr;     ///< half-rounded (the GPU image holds exactly these)
    std::vector<f32> depth;    ///< metres (R32F: exact)
    std::vector<Vec2> velocity;///< half-rounded pixels / frame
};

/// Scene-like frame: sky gradient, a lit "object" block in front (near depth) moving right, emissive
/// highlights, a dark region. `frame` shifts content (exposure adaptation, zero-alloc churn).
Inputs makeInputs(u32 w, u32 h, u32 frame, f32 brightness = 1.f) {
    Inputs in;
    in.w = w;
    in.h = h;
    const usize n = static_cast<usize>(w) * h;
    in.hdr.resize(n);
    in.depth.resize(n);
    in.velocity.resize(n);
    const u32 bx0 = (w / 5u + frame * 2u) % w;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
            Vec3 c{0.08f + 0.5f * fx, 0.1f + 0.3f * (1.f - fy), 0.25f + 0.2f * fy};
            c = c * (0.6f + 0.8f * rnd(i * 7u + frame * 131u + 1u));
            f32 depth = 12.f + 20.f * fy + 3.f * rnd(i + 900u);
            Vec2 vel{0.f, 0.f};
            const bool object = x >= bx0 && x < bx0 + w / 3u && y > h / 4u && y < (3u * h) / 4u;
            if (object) {
                c = Vec3{0.6f, 0.35f, 0.2f} * (0.7f + 0.6f * rnd(i + 17u));
                depth = 2.5f + 0.4f * rnd(i + 23u);
                vel = Vec2{9.f + 2.f * rnd(i + 3u), 1.5f * (rnd(i + 5u) - 0.5f)};
            } else if (y < h / 5u) {
                vel = Vec2{-1.25f + 0.5f * rnd(i + 44u), 0.f};
            }
            if ((x * 7u + y * 3u + frame) % 29u == 0u) {
                c = c * (15.f + 30.f * rnd(i + 11u)); // highlights
            }
            if (x < w / 6u && y > (4u * h) / 5u) {
                c = c * 0.02f; // dark corner
            }
            c = c * brightness;
            in.hdr[i] = Vec3{roundHalf(c.x), roundHalf(c.y), roundHalf(c.z)};
            in.depth[i] = depth;
            in.velocity[i] = Vec2{roundHalf(vel.x), roundHalf(vel.y)};
        }
    }
    return in;
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
    // Inputs (recreated when the extent changes) and read-back buffers.
    u32 w = 0;
    u32 h = 0;
    Texture hdr{};
    Texture depth{};
    Texture velocity{};
    u32 layouts[3] = {0u, 0u, 0u};
    u8 queues[3] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
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
        for (Texture* t : {&hdr, &depth, &velocity}) {
            if (t->image != nullptr) {
                allocator->destroyImage(*t);
            }
            *t = Texture{};
        }
        for (Buffer* b : {&staging, &readback, &dump}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
            *b = Buffer{};
        }
        for (u32 i = 0; i < 3u; ++i) {
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
    instanceDesc.appName = "fuse_rp_post_gpu";
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
    const PostCapabilities caps = queryPostCapabilities(ctx.device.get());
    if (!caps.post) {
        std::printf("SKIP: GPU post stack unsupported: %s\n", caps.reason);
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

// Read-back layout: output image (RGBA16F), display input, bloom (f32x4 each), exposure state.
struct Readback {
    u64 image = 0;
    u64 displayInput = 0;
    u64 bloom = 0;
    u64 state = 0;
    u64 end = 0;
};

Readback readbackLayout(u32 w, u32 h) {
    Readback r{};
    const u64 n = static_cast<u64>(w) * h;
    r.image = 0;
    r.displayInput = (n * 8u + 255u) & ~u64{255u};
    r.bloom = r.displayInput + ((n * 16u + 255u) & ~u64{255u});
    r.state = r.bloom + ((n * 16u + 255u) & ~u64{255u});
    r.end = r.state + sizeof(PostExposureState);
    return r;
}

// Staging layout: hdr (RGBA16F), depth (R32F), velocity (RG16F).
struct Staging {
    u64 hdr = 0;
    u64 depth = 0;
    u64 velocity = 0;
    u64 end = 0;
};

Staging stagingLayout(u32 w, u32 h) {
    Staging s{};
    const u64 n = static_cast<u64>(w) * h;
    s.hdr = 0;
    s.depth = (n * 8u + 255u) & ~u64{255u};
    s.velocity = s.depth + ((n * 4u + 255u) & ~u64{255u});
    s.end = s.velocity + n * 4u;
    return s;
}

bool ensureTargets(Context& ctx, u32 w, u32 h) {
    if (ctx.w == w && ctx.h == h && ctx.hdr.image != nullptr) {
        return true;
    }
    vkDeviceWaitIdle(ctx.vkDevice);
    ctx.destroyTargets();
    auto image = [&](Texture& t, GpuFormat format, const char* name) {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.format = format;
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst) |
                                          static_cast<u32>(ImageUsage::Storage));
        d.name = name;
        if (format != GpuFormat::R16G16B16A16Sfloat) {
            d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        }
        return ctx.allocator->createImage(d, t);
    };
    auto buffer = [&](Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = static_cast<usize>(size);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = name;
        return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
    };
    const u64 n = static_cast<u64>(w) * h;
    const bool ok =
        image(ctx.hdr, GpuFormat::R16G16B16A16Sfloat, "rp_post.hdr") && image(ctx.depth, GpuFormat::R32Sfloat, "rp_post.depth") &&
        image(ctx.velocity, GpuFormat::R16G16Sfloat, "rp_post.velocity") &&
        buffer(ctx.staging, stagingLayout(w, h).end, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_post.staging") &&
        buffer(ctx.readback, readbackLayout(w, h).end, BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "rp_post.readback") &&
        buffer(ctx.dump, n * 16u,
               static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress)),
               MemoryUsage::GpuToCpu, "rp_post.dump") &&
        ctx.dump.deviceAddress != 0u;
    ctx.w = w;
    ctx.h = h;
    return ok;
}

void writeStaging(Context& ctx, const Inputs& in) {
    const Staging s = stagingLayout(in.w, in.h);
    u8* base = static_cast<u8*>(ctx.staging.mapped);
    for (usize i = 0; i < in.hdr.size(); ++i) {
        const u16 texel[4] = {halfBits(in.hdr[i].x), halfBits(in.hdr[i].y), halfBits(in.hdr[i].z), halfBits(1.f)};
        std::memcpy(base + s.hdr + i * 8u, texel, 8u);
        std::memcpy(base + s.depth + i * 4u, &in.depth[i], 4u);
        const u16 v[2] = {halfBits(in.velocity[i].x), halfBits(in.velocity[i].y)};
        std::memcpy(base + s.velocity + i * 4u, v, 4u);
    }
}

// --- per-frame graph ------------------------------------------------------------------------------
struct TestRecord {
    Context* ctx = nullptr;
    rg::BufferRef staging;
    rg::BufferRef readback;
    rg::TextureRef images[3];
    rg::TextureRef output;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const TestRecord& r = *static_cast<const TestRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const Staging s = stagingLayout(r.ctx->w, r.ctx->h);
    const u64 offsets[3] = {s.hdr, s.depth, s.velocity};
    for (u32 i = 0; i < 3u; ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset = offsets[i];
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
    region.bufferOffset = readbackLayout(r.ctx->w, r.ctx->h).image;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.ctx->w, r.ctx->h, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(r.output)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VkBuffer>(pc.buffer(r.readback)), 1, &region);
}

struct Frame {
    const PostGpuSettings* settings = nullptr;
    const look::LookEffectGraph* graph = nullptr;
    const look::LookResolved* look = nullptr;
    u64 seed = 0;
    f32 dt = 1.f / 60.f;
    bool depth = true;
    bool velocity = true;
    bool readback = true;
};

struct Captured {
    std::vector<Vec3> dump;
    std::vector<Vec3> image;
    std::vector<Vec3> displayInput;
    std::vector<Vec3> bloom;
    PostExposureState state{};
    std::vector<f32> dumpRaw;  ///< the dump's exact bits (bitwise comparisons)
    std::vector<u16> imageRaw;
};

TestRecord g_record{};

bool runFrame(Context& ctx, PostStackGpu& post, rg::Graph& graph, const Frame& f, Captured* out, bool upload = true) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    PostFrameImages images{};
    images.hdr = &ctx.hdr;
    images.depth = f.depth ? &ctx.depth : nullptr;
    images.velocity = f.velocity ? &ctx.velocity : nullptr;
    images.dumpAddress = f.readback ? ctx.dump.deviceAddress : 0u;
    bool ok = false;
    if (f.graph != nullptr) {
        ok = post.beginLookFrame(ctx.serial, *f.graph, *f.look, f.seed, f.dt, images);
    } else {
        ok = post.beginFrame(ctx.serial, *f.settings, images);
    }
    if (!ok) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    graph.reset();
    TestRecord& r = g_record;
    r = TestRecord{};
    r.ctx = &ctx;
    Texture* textures[3] = {&ctx.hdr, &ctx.depth, &ctx.velocity};
    const u32 formats[3] = {static_cast<u32>(GpuFormat::R16G16B16A16Sfloat), static_cast<u32>(GpuFormat::R32Sfloat),
                            static_cast<u32>(GpuFormat::R16G16Sfloat)};
    const char* names[3] = {"rp_post.hdr", "rp_post.depth", "rp_post.velocity"};
    for (u32 i = 0; i < 3u; ++i) {
        rg::ImportedImage im{};
        im.image = textures[i]->image;
        im.view = textures[i]->view;
        im.format = formats[i];
        im.width = ctx.w;
        im.height = ctx.h;
        im.initialLayout = ctx.layouts[i];
        im.initialQueue = ctx.queues[i];
        im.layoutTracker = &ctx.layouts[i];
        im.queueTracker = &ctx.queues[i];
        im.name = names[i];
        r.images[i] = graph.importImage(im);
    }
    r.staging = graph.importBuffer(
        rg::ImportedBuffer{ctx.staging.handle, ctx.staging.desc.size, ctx.stagingQueue, &ctx.stagingQueue, "rp_post.staging"});
    r.readback = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, ctx.readbackQueue,
                                                       &ctx.readbackQueue, "rp_post.readback"});
    const rg::BufferRef dump =
        graph.importBuffer(rg::ImportedBuffer{ctx.dump.handle, ctx.dump.desc.size, ctx.dumpQueue, &ctx.dumpQueue, "rp_post.dump"});
    if (upload) {
        rg::PassBuilder up = graph.addPass("test.upload", &recordUpload, &r);
        up.use(r.staging, rg::Access::TransferSrc);
        for (const rg::TextureRef& t : r.images) {
            up.use(t, rg::Access::TransferDst);
        }
    }
    const PostGraphRefs refs = post.importInto(graph);
    r.output = refs.output;
    PostGraphInputs inputs{};
    inputs.hdr = r.images[0];
    inputs.depth = f.depth ? r.images[1] : rg::TextureRef{};
    inputs.velocity = f.velocity ? r.images[2] : rg::TextureRef{};
    inputs.dump = f.readback ? dump : rg::BufferRef{};
    post.addPasses(graph, refs, inputs);
    const Readback rb = readbackLayout(ctx.w, ctx.h);
    if (f.readback) {
        graph.addPass("test.readback_image", &recordImageReadback, &r)
            .use(refs.output, rg::Access::TransferSrc)
            .use(r.readback, rg::Access::TransferDst, rg::BufferRange{rb.image, static_cast<u64>(ctx.w) * ctx.h * 8u});
        post.addCopy(graph, refs, PostCopySource::DisplayInput, r.readback, rb.displayInput);
        post.addCopy(graph, refs, PostCopySource::Bloom, r.readback, rb.bloom);
        post.addCopy(graph, refs, PostCopySource::ExposureState, r.readback, rb.state);
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    post.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    if (out != nullptr && f.readback) {
        const usize n = static_cast<usize>(ctx.w) * ctx.h;
        const u8* base = static_cast<const u8*>(ctx.readback.mapped);
        const f32* dumpF = static_cast<const f32*>(ctx.dump.mapped);
        out->dump.resize(n);
        out->image.resize(n);
        out->displayInput.resize(n);
        out->bloom.resize(n);
        out->dumpRaw.assign(dumpF, dumpF + n * 4u);
        out->imageRaw.resize(n * 4u);
        std::memcpy(out->imageRaw.data(), base + rb.image, n * 8u);
        for (usize i = 0; i < n; ++i) {
            out->dump[i] = Vec3{dumpF[i * 4u], dumpF[i * 4u + 1u], dumpF[i * 4u + 2u]};
            out->image[i] = Vec3{halfToFloat(out->imageRaw[i * 4u]), halfToFloat(out->imageRaw[i * 4u + 1u]),
                                 halfToFloat(out->imageRaw[i * 4u + 2u])};
            f32 v[4];
            std::memcpy(v, base + rb.displayInput + i * 16u, 16u);
            out->displayInput[i] = Vec3{v[0], v[1], v[2]};
            std::memcpy(v, base + rb.bloom + i * 16u, 16u);
            out->bloom[i] = Vec3{v[0], v[1], v[2]};
        }
        std::memcpy(&out->state, base + rb.state, sizeof(PostExposureState));
    }
    return true;
}

// --- comparisons ----------------------------------------------------------------------------------
struct Error {
    f64 worst = 0.0;  ///< worst |gpu - cpu| / max(1, |cpu|) (hdr) or |gpu - cpu| (display)
    u32 over = 0;     ///< channels over the tolerance
};

Error compareHdr(const std::vector<Vec3>& gpu, const std::vector<Vec3>& cpu) {
    Error e{};
    for (usize i = 0; i < cpu.size() && i < gpu.size(); ++i) {
        const f32 g[3] = {gpu[i].x, gpu[i].y, gpu[i].z};
        const f32 c[3] = {cpu[i].x, cpu[i].y, cpu[i].z};
        for (u32 k = 0; k < 3u; ++k) {
            const f64 d = std::fabs(static_cast<f64>(g[k]) - c[k]) / std::max(1.0, std::fabs(static_cast<f64>(c[k])));
            e.worst = std::max(e.worst, d);
            e.over += d > kTol || !std::isfinite(g[k]) ? 1u : 0u;
        }
    }
    if (gpu.size() != cpu.size()) {
        e.over += 1u;
    }
    return e;
}

Error compareDisplay(const std::vector<Vec3>& gpu, const std::vector<Vec3>& cpu) {
    Error e{};
    for (usize i = 0; i < cpu.size() && i < gpu.size(); ++i) {
        const f32 g[3] = {gpu[i].x, gpu[i].y, gpu[i].z};
        const f32 c[3] = {cpu[i].x, cpu[i].y, cpu[i].z};
        for (u32 k = 0; k < 3u; ++k) {
            const f64 d = std::fabs(static_cast<f64>(g[k]) - c[k]);
            e.worst = std::max(e.worst, d);
            e.over += d > kTol || !std::isfinite(g[k]) ? 1u : 0u;
        }
    }
    if (gpu.size() != cpu.size()) {
        e.over += 1u;
    }
    return e;
}

bool report(const char* lang, const char* what, const Error& e) {
    std::printf("  [%s] %-44s max err %.3e (tolerance %.3e) %s\n", lang, what, e.worst, static_cast<f64>(kTol),
                e.over == 0u ? "ok" : "OVER");
    return e.over == 0u;
}

/// The output image is the dump converted to half (RGBA16F storage): one of the two halves bracketing
/// the f32 value (the conversion's rounding mode is the implementation's; Lavapipe does not round to
/// nearest even), so it is also within 1/1024 of the display result.
bool bracketingHalf(f32 value, f32 stored) {
    const f32 a = std::fabs(value);
    const f32 ulp = a < 6.103515625e-05f ? 5.9604644775390625e-08f : std::ldexp(1.f, std::ilogb(a) - 10);
    return std::fabs(stored - value) < ulp || stored == value;
}

void checkImage(const char* lang, const Captured& c) {
    u32 mismatches = 0;
    for (usize i = 0; i < c.dump.size(); ++i) {
        const f32 d[3] = {c.dump[i].x, c.dump[i].y, c.dump[i].z};
        const f32 m[3] = {c.image[i].x, c.image[i].y, c.image[i].z};
        for (u32 k = 0; k < 3u; ++k) {
            mismatches += bracketingHalf(d[k], m[k]) && std::fabs(d[k] - m[k]) <= kTol ? 0u : 1u;
        }
    }
    if (mismatches != 0u) {
        std::printf("  [%s] output image vs dump: %u channel(s) not a bracketing half\n", lang, mismatches);
    }
    expect(mismatches == 0u, "RGBA16F output == the f32 display result converted to half (bracketing half)");
}

std::vector<Vec3> displayReference(const PostGpuSettings& s, const std::vector<Vec3>& in, f32 scale, u32 w, u32 h,
                                   const look::Lut3D* lut) {
    std::vector<Vec3> out(in.size());
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            out[y * w + x] = display_reference(s, in[y * w + x], scale, x, y, w, h, lut);
        }
    }
    return out;
}

/// Settings with everything off: Neutral tone map, no exposure, linear output.
PostGpuSettings neutralSettings() {
    PostGpuSettings s{};
    s.toneMapper = ToneMapper::Neutral;
    s.outputSrgb = false;
    return s;
}

struct Lang {
    PostKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{PostKernelLanguage::Slang, "slang"}, {PostKernelLanguage::Glsl, "glsl"}};

bool initPost(Context& ctx, PostStackGpu& post, PostKernelLanguage language) {
    PostStackGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    return post.init(d);
}

// --- passes ----------------------------------------------------------------------------------------
void passesBloom(Context& ctx, PostStackGpu& post, rg::Graph& graph, const char* lang, const Inputs& in) {
    PostGpuSettings s = neutralSettings();
    s.bloom = true;
    s.bloomParams.intensity = 0.35f;
    s.bloomParams.threshold = 0.9f;
    s.bloomTint = {1.f, 1.f, 1.f};
    Frame f{};
    f.settings = &s;
    Captured c;
    expect(runFrame(ctx, post, graph, f, &c), "bloom frame");
    std::vector<Vec3> bloom;
    bloom_image(in.hdr.data(), in.w, in.h, s.bloomParams, bloom);
    std::vector<Vec3> composite;
    bloom_composite(in.hdr.data(), in.w, in.h, s.bloomParams, composite);
    std::printf("  [%s] bloom: %u pyramid levels, %u passes\n", lang, post.stats().bloomLevels, post.stats().passes);
    expect(post.stats().bloomLevels == bloom_level_count(in.w, in.h, s.bloomParams), "bloom level count");
    expect(report(lang, "bloom image vs bloom_image", compareHdr(c.bloom, bloom)), "bloom image within 1/1024");
    expect(report(lang, "bloom composite vs bloom_composite", compareHdr(c.displayInput, composite)),
           "bloom composite within 1/1024");
    checkImage(lang, c);
    // Tinted (Look) composite: hdr + bloom * intensity * tint.
    s.bloomTint = {1.2f, 0.8f, 0.5f};
    expect(runFrame(ctx, post, graph, f, &c), "tinted bloom frame");
    std::vector<Vec3> tinted;
    spatial_reference(s, in.hdr.data(), nullptr, nullptr, in.w, in.h, tinted);
    expect(report(lang, "tinted bloom vs spatial_reference", compareHdr(c.displayInput, tinted)), "tinted bloom");
}

void passesDof(Context& ctx, PostStackGpu& post, rg::Graph& graph, const char* lang, const Inputs& in) {
    PostGpuSettings s = neutralSettings();
    s.dof = true;
    s.dofParams.focal_distance = 14.f;
    s.dofParams.f_stop = 1.4f;
    s.dofParams.max_coc_radius_px = 7.f;
    for (const bool nearBlur : {true, false}) {
        s.dofParams.near_blur = nearBlur;
        Frame f{};
        f.settings = &s;
        Captured c;
        expect(runFrame(ctx, post, graph, f, &c), "dof frame");
        std::vector<Vec3> ref;
        dof_pass(in.hdr.data(), in.depth.data(), in.w, in.h, s.dofParams, ref);
        expect(post.stats().dofRan, "dof ran");
        expect(report(lang, nearBlur ? "dof vs dof_pass (near blur)" : "dof vs dof_pass (far only)",
                      compareHdr(c.displayInput, ref)),
               "dof within 1/1024");
    }
    // Without a depth input DoF is skipped (the oracle's rule).
    Frame f{};
    f.settings = &s;
    f.depth = false;
    Captured c;
    expect(runFrame(ctx, post, graph, f, &c) && !post.stats().dofRan, "dof skipped without depth");
    expect(compareHdr(c.displayInput, in.hdr).worst == 0.0, "no depth: display input == input");
}

void passesMotionBlur(Context& ctx, PostStackGpu& post, rg::Graph& graph, const char* lang, const Inputs& in) {
    PostGpuSettings s = neutralSettings();
    s.motionBlur = true;
    s.motionBlurParams.max_blur_px = 12.f;
    for (const bool depth : {true, false}) {
        Frame f{};
        f.settings = &s;
        f.depth = depth;
        Captured c;
        expect(runFrame(ctx, post, graph, f, &c), "motion blur frame");
        std::vector<Vec3> ref;
        motion_blur_pass(in.hdr.data(), in.velocity.data(), depth ? in.depth.data() : nullptr, in.w, in.h,
                         s.motionBlurParams, ref);
        u32 changed = 0;
        for (usize i = 0; i < ref.size(); ++i) {
            changed += ref[i].x != in.hdr[i].x ? 1u : 0u;
        }
        std::printf("  [%s] motion blur: %u of %u pixels blurred by the oracle, tile %u px\n", lang, changed,
                    in.w * in.h, motion_blur_tile_size(s.motionBlurParams));
        expect(changed > 100u, "the test frame exercises the motion blur");
        expect(report(lang, depth ? "motion blur vs motion_blur_pass (depth)" : "motion blur vs motion_blur_pass (no depth)",
                      compareHdr(c.displayInput, ref)),
               "motion blur within 1/1024");
    }
}

void passesExposure(Context& ctx, PostStackGpu& post, rg::Graph& graph, const char* lang) {
    for (const bool ema : {false, true}) {
        PostGpuSettings s = neutralSettings();
        s.autoExposure = true;
        s.autoExposureParams.use_ema_adaptation = ema;
        s.exposureEv = 0.25f;
        s.deltaSeconds = 0.1f;
        AutoExposure cpu{};
        cpu.init();
        cpu.setParams(s.autoExposureParams);
        post.resetExposure(0.f);
        f64 worstEv = 0.0;
        u32 worstBins = 0;
        for (u32 frame = 0; frame < 6u; ++frame) {
            // Brightness steps (dark -> bright -> dark) so the adaptation moves both ways.
            const f32 brightness = frame < 2u ? 0.05f : (frame < 4u ? 20.f : 1.f);
            const Inputs in = makeInputs(kPassW, kPassH, frame, brightness);
            writeStaging(ctx, in);
            if (frame == 5u) {
                post.resetExposure(1.5f); // scene cut
                cpu.resetToEv(1.5f);
            }
            Frame f{};
            f.settings = &s;
            Captured c;
            expect(runFrame(ctx, post, graph, f, &c), "exposure frame");
            std::vector<u32> bins;
            histogram_reference(in.hdr.data(), kPassW * kPassH, s.histogram, bins);
            LuminanceHistogram hist;
            hist.init(s.histogram);
            for (const Vec3& v : in.hdr) {
                hist.accumulate(v);
            }
            const f32 ev = cpu.updateFromHistogram(hist, s.deltaSeconds);
            u32 moved = 0;
            for (u32 b = 0; b < kPostHistMaxBins; ++b) {
                const u32 ref = b < bins.size() ? bins[b] : 0u;
                moved += c.state.bins[b] > ref ? c.state.bins[b] - ref : 0u;
            }
            worstBins = std::max(worstBins, moved);
            worstEv = std::max(worstEv, std::fabs(static_cast<f64>(c.state.currentEv) - ev));
            expect(c.state.sampleCount == kPassW * kPassH, "histogram sample count");
            expect(std::fabs(c.state.meteredLuminance - hist.meteringLuminance()) <=
                       kTol * std::max(1.f, hist.meteringLuminance()),
                   "metered luminance == LuminanceHistogram::meteringLuminance");
            // The display uses the adapted EV: compare with the reference at the CPU's EV.
            const std::vector<Vec3> ref =
                displayReference(s, in.hdr, std::pow(2.f, s.exposureEv - ev), kPassW, kPassH, nullptr);
            expect(compareDisplay(c.dump, ref).over == 0u, "auto-exposed display within 1/1024");
            if (frame == 5u) {
                expect(c.state.frames == 1u, "reset restarts the adaptation state");
            }
        }
        std::printf("  [%s] exposure (%s): 6 frames, worst |EV - AutoExposure| %.3e, worst bin moves %u of %u\n", lang,
                    ema ? "EMA" : "linear", worstEv, worstBins, kPassW * kPassH);
        expect(worstEv <= kTol, "adapted EV within 1/1024 of AutoExposure::updateFromHistogram");
        expect(worstBins * 1024u <= kPassW * kPassH, "histogram within count / 1024 of LuminanceHistogram");
    }
}

void passesDisplay(Context& ctx, PostStackGpu& post, rg::Graph& graph, const char* lang, const Inputs& in) {
    Error worst{};
    u32 cases = 0;
    const ToneMapper ops[5] = {ToneMapper::ACES, ToneMapper::Filmic, ToneMapper::Reinhard, ToneMapper::Neutral, ToneMapper::AgX};
    for (const ToneMapper op : ops) {
        for (u32 curve = 0; curve < 4u; ++curve) {
            PostGpuSettings s = neutralSettings();
            s.toneMapper = op;
            s.outputSrgb = true;
            s.exposureEv = tone_mapper_mid_grey_calibration_ev(op) + 0.3f;
            if (curve == 1u) {
                TonemapCurveParams overrides{};
                overrides.toe_strength = 0.05f;
                overrides.gamma = 1.1f;
                s.curveParams = make_filmic_curve_params(overrides);
            } else if (curve == 2u) {
                s.curveParams = make_reinhard_curve_params({6.f, 0.5f});
            } else if (curve == 3u) {
                s.curveParams = make_aces_curve_params({1.1f, 0.4f});
            }
            s.curve = curve != 0u;
            Frame f{};
            f.settings = &s;
            Captured c;
            expect(runFrame(ctx, post, graph, f, &c), "display frame");
            const Error e = compareDisplay(c.dump, displayReference(s, in.hdr, std::pow(2.f, s.exposureEv), in.w, in.h, nullptr));
            worst.worst = std::max(worst.worst, e.worst);
            worst.over += e.over;
            checkImage(lang, c);
            ++cases;
        }
    }
    std::printf("  [%s] tonemap: %u cases (ACES / Filmic / Reinhard / Neutral / AgX x 4 curves)\n", lang, cases);
    expect(report(lang, "tonemap + curve vs display_reference", worst), "tonemap within 1/1024");

    // PostStack grade stages, vignette, grain.
    PostStack stack;
    stack.init({in.w, in.h});
    ColorGradeParams g = stack.colorGrade().params();
    g.lift = {0.02f, -0.01f, 0.03f};
    g.contrast = 1.2f;
    g.saturation = 0.7f;
    g.gamma = {1.1f, 0.9f, 1.f};
    g.gain = {1.05f, 1.f, 0.95f};
    g.vignette = 0.4f;
    g.film_grain = 0.04f;
    stack.setColorGradeParams(g);
    PostGpuSettings s = settings_from_post_stack(stack, 0x1234567890ABCDEFull);
    s.bloom = s.dof = s.motionBlur = s.autoExposure = false;
    Frame f{};
    f.settings = &s;
    Captured c;
    expect(runFrame(ctx, post, graph, f, &c), "grade frame");
    expect(report(lang, "grade + vignette + grain vs display_reference",
                  compareDisplay(c.dump, displayReference(s, in.hdr, std::pow(2.f, s.exposureEv), in.w, in.h, nullptr))),
           "grade within 1/1024");
    checkImage(lang, c);

    // Look LUT + look vignette + look grain.
    look::LookParamBlock b = look::look_default_params();
    b.set(look::LookParam::GradeSaturation, 1.4f);
    b.set(look::LookParam::GradeTemperatureK, 4800.f);
    b.set(look::LookParam::GradeContrast, 1.2f);
    b.set(look::LookParam::VignetteRoundness, 0.6f);
    b.set(look::LookParam::VignetteColor, 0.1f, 0);
    b.set(look::LookParam::GrainResponse, 0.5f);
    const look::LookResolved look = look::look_resolve(b);
    look::Lut3D lut;
    look::lut_generate_grade(look, 32, lut);
    post.setGradeLut(&lut);
    PostGpuSettings ls{};
    expect(settings_from_look(look::LookEffectGraph::makeDefault(), look, 77u, 0.f, ls, nullptr), "look settings");
    ls.bloom = ls.dof = ls.motionBlur = ls.autoExposure = false;
    f.settings = &ls;
    expect(runFrame(ctx, post, graph, f, &c) && post.stats().lutApplied, "LUT frame");
    expect(report(lang, "LUT + look vignette + grain vs Look kernels",
                  compareDisplay(c.dump, displayReference(ls, in.hdr, std::pow(2.f, ls.exposureEv), in.w, in.h, &lut))),
           "LUT within 1/1024");
    checkImage(lang, c);
    post.setGradeLut(nullptr);
}

int runPasses(Context& ctx) {
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        PostStackGpu post;
        if (!initPost(ctx, post, lang.language)) {
            std::printf("  [%s] kernels not built: skipped\n", lang.name);
            continue;
        }
        ++built;
        rg::Graph graph;
        const Inputs in = makeInputs(kPassW, kPassH, 0);
        if (!ensureTargets(ctx, kPassW, kPassH)) {
            std::fprintf(stderr, "FAIL: targets\n");
            return 1;
        }
        writeStaging(ctx, in);
        passesBloom(ctx, post, graph, lang.name, in);
        passesDof(ctx, post, graph, lang.name, in);
        passesMotionBlur(ctx, post, graph, lang.name, in);
        passesDisplay(ctx, post, graph, lang.name, in);
        passesExposure(ctx, post, graph, lang.name);
        post.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no post kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- stack -----------------------------------------------------------------------------------------
void configureFullStack(PostStack& stack) {
    BloomParams bloom = stack.bloom().params();
    bloom.intensity = 0.2f;
    stack.setBloomParams(bloom);
    DOFParams dof = stack.dofParams();
    dof.enabled = true;
    dof.focal_distance = 14.f;
    dof.max_coc_radius_px = 6.f;
    stack.setDofParams(dof);
    MotionBlurParams mb = stack.motionBlurParams();
    mb.enabled = true;
    mb.max_blur_px = 10.f;
    stack.setMotionBlurParams(mb);
    AutoExposureParams ae = stack.autoExposure().params();
    ae.enabled = true;
    stack.setAutoExposureParams(ae);
    TonemapCurveParams overrides{};
    overrides.shoulder_strength = 0.1f;
    stack.setTonemapCurveParams(make_filmic_curve_params(overrides));
    ColorGradeParams g = stack.colorGrade().params();
    g.tone_mapper = ToneMapper::AgX;
    g.exposure = 0.5f;
    g.lift = {0.01f, 0.f, -0.01f};
    g.saturation = 1.1f;
    g.gain = {1.02f, 1.f, 0.98f};
    g.vignette = 0.3f;
    g.film_grain = 0.02f;
    stack.setColorGradeParams(g);
}

int runStack(Context& ctx) {
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        PostStackGpu post;
        if (!initPost(ctx, post, lang.language)) {
            continue;
        }
        ++built;
        rg::Graph graph;
        const u32 w = 96;
        const u32 h = 54;
        if (!ensureTargets(ctx, w, h)) {
            std::fprintf(stderr, "FAIL: targets\n");
            return 1;
        }
        PostStack stack;
        stack.init({w, h});
        configureFullStack(stack);
        post.resetExposure(0.f);
        stack.resetAutoExposureTo(0.f);
        const f32 dt = 0.05f;
        Error worst{};
        f64 worstEv = 0.0;
        for (u32 frame = 0; frame < 4u; ++frame) {
            const Inputs in = makeInputs(w, h, frame, frame < 2u ? 3.f : 0.3f);
            writeStaging(ctx, in);
            const u64 seed = 1000u + frame;
            const PostGpuSettings s = settings_from_post_stack(stack, seed, dt);
            Frame f{};
            f.settings = &s;
            Captured c;
            expect(runFrame(ctx, post, graph, f, &c), "stack frame");
            expect(post.stats().bloomRan && post.stats().dofRan && post.stats().motionBlurRan && post.stats().exposureRan,
                   "every pass ran");
            // CPU oracle: the spatial chain, the histogram adaptation, then PostStack::processFrame.
            std::vector<Vec3> spatial;
            spatial_reference(s, in.hdr.data(), in.depth.data(), in.velocity.data(), w, h, spatial);
            LuminanceHistogram hist;
            hist.init(s.histogram);
            for (const Vec3& v : spatial) {
                hist.accumulate(v);
            }
            stack.updateAutoExposureFromHistogram(hist, dt);
            PostFrameInput pin{};
            pin.hdr = in.hdr.data();
            pin.linear_depth_m = in.depth.data();
            pin.velocity_px = in.velocity.data();
            pin.width = w;
            pin.height = h;
            pin.frame_seed = seed;
            std::vector<Vec3> oracle;
            expect(stack.processFrame(pin, oracle), "PostStack::processFrame");
            const Error e = compareDisplay(c.dump, oracle);
            worst.worst = std::max(worst.worst, e.worst);
            worst.over += e.over;
            worstEv = std::max(worstEv, std::fabs(static_cast<f64>(c.state.currentEv) - stack.autoExposure().currentEv()));
            const Error spatialErr = compareHdr(c.displayInput, spatial);
            expect(spatialErr.over == 0u, "spatial chain within 1/1024");
            checkImage(lang.name, c);
            std::printf("  [%s] frame %u: %u passes, EV gpu %.4f cpu %.4f, display max err %.3e, spatial max err %.3e\n",
                        lang.name, frame, post.stats().passes, static_cast<f64>(c.state.currentEv),
                        static_cast<f64>(stack.autoExposure().currentEv()), e.worst, spatialErr.worst);
        }
        expect(report(lang.name, "full PostStack chain vs processFrame", worst), "full stack within 1/1024");
        expect(worstEv <= kTol, "adapted EV == PostStack auto exposure");
        post.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no post kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- look ------------------------------------------------------------------------------------------
look::LookResolved noopLook(look::LookToneMapOperator op) {
    look::LookParamBlock b = look::look_default_params();
    using P = look::LookParam;
    for (P p : {P::AoEnabled, P::DofEnabled, P::MbEnabled, P::BloomEnabled, P::DirtEnabled, P::FlareEnabled,
                P::ExpAutoEnabled, P::GradeEnabled, P::SharpenEnabled, P::CaEnabled, P::VignetteEnabled,
                P::GrainEnabled}) {
        b.set(p, 0.f);
    }
    look::LookResolved r = look::look_resolve(b);
    r.tonemap.op = op;
    return r;
}

int runLook(Context& ctx) {
    u32 built = 0;
    for (const Lang& lang : kLangs) {
        PostStackGpu post;
        if (!initPost(ctx, post, lang.language)) {
            continue;
        }
        ++built;
        rg::Graph graph;
        const u32 w = 80;
        const u32 h = 48;
        if (!ensureTargets(ctx, w, h)) {
            std::fprintf(stderr, "FAIL: targets\n");
            return 1;
        }
        const Inputs in = makeInputs(w, h, 3);
        writeStaging(ctx, in);
        const look::LookEffectGraph lookGraph = look::LookEffectGraph::makeDefault();

        // (1) No-op Look through the Look nodes vs the PostStack it feeds: bit-identical.
        for (const look::LookToneMapOperator op : {look::LookToneMapOperator::Aces, look::LookToneMapOperator::Filmic,
                                                   look::LookToneMapOperator::Reinhard, look::LookToneMapOperator::Neutral}) {
            const look::LookResolved noop = noopLook(op);
            Frame lf{};
            lf.graph = &lookGraph;
            lf.look = &noop;
            lf.seed = 42u;
            Captured viaLook;
            expect(runFrame(ctx, post, graph, lf, &viaLook), "no-op look frame");
            const u32 lookPasses = post.stats().passes;
            expect(post.stats().lookUnsupported == 0u, "no-op look uses no unsupported node");
            PostStack stack;
            stack.init({w, h});
            look::apply_look_to_post_stack(noop, stack);
            const PostGpuSettings s = settings_from_post_stack(stack, 42u);
            Frame sf{};
            sf.settings = &s;
            Captured viaStack;
            expect(runFrame(ctx, post, graph, sf, &viaStack), "PostStack frame");
            const bool dumpSame = viaLook.dumpRaw.size() == viaStack.dumpRaw.size() &&
                                  std::memcmp(viaLook.dumpRaw.data(), viaStack.dumpRaw.data(), viaLook.dumpRaw.size() * 4u) == 0;
            const bool imageSame = viaLook.imageRaw.size() == viaStack.imageRaw.size() &&
                                   std::memcmp(viaLook.imageRaw.data(), viaStack.imageRaw.data(), viaLook.imageRaw.size() * 2u) == 0;
            std::printf("  [%s] no-op Look (%s): %u passes, dump %s, image %s vs PostStack\n", lang.name,
                        look::kLookToneMapOperatorNames[static_cast<u32>(op)], lookPasses,
                        dumpSame ? "bit-identical" : "DIFFERENT", imageSame ? "bit-identical" : "DIFFERENT");
            expect(dumpSame && imageSame, "no-op Look is bit-identical to PostStack (GPU)");
            expect(lookPasses == post.stats().passes, "same passes");
            PostFrameInput pin{};
            pin.hdr = in.hdr.data();
            pin.linear_depth_m = in.depth.data();
            pin.velocity_px = in.velocity.data();
            pin.width = w;
            pin.height = h;
            pin.frame_seed = 42u;
            std::vector<Vec3> oracle;
            expect(stack.processFrame(pin, oracle), "processFrame");
            expect(report(lang.name, "no-op Look vs PostStack::processFrame", compareDisplay(viaLook.dump, oracle)),
                   "no-op Look within 1/1024 of PostStack::processFrame");
        }

        // (2) The default look (DoF on) vs the CPU LookPostChain (auto exposure off: the CPU chain meters a
        //     strided mean, the GPU a histogram).
        look::LookParamBlock b = look::look_default_params();
        b.set(look::LookParam::DofEnabled, 1.f);
        b.set(look::LookParam::DofFocusDistanceM, 14.f);
        b.set(look::LookParam::DofMaxCocRadiusPx, 6.f);
        b.set(look::LookParam::MbMaxBlurPx, 10.f);
        b.set(look::LookParam::ExpAutoEnabled, 0.f);
        b.set(look::LookParam::ExpBiasEv, 0.3f);
        b.set(look::LookParam::BloomIntensity, 0.15f);
        b.set(look::LookParam::BloomTint, 1.1f, 0);
        b.set(look::LookParam::GradeSaturation, 1.2f);
        b.set(look::LookParam::GradeTemperatureK, 5600.f);
        b.set(look::LookParam::GrainResponse, 0.3f);
        const look::LookResolved look = look::look_resolve(b);
        look::Lut3D lut;
        look::lut_generate_grade(look, 32, lut);
        post.setGradeLut(&lut);
        Frame lf{};
        lf.graph = &lookGraph;
        lf.look = &look;
        lf.seed = 9u;
        Captured c;
        expect(runFrame(ctx, post, graph, lf, &c), "look frame");
        expect(post.stats().dofRan && post.stats().motionBlurRan && post.stats().bloomRan && post.stats().lutApplied,
               "look drives DoF, motion blur, bloom and the LUT");
        look::LookPostChain chain;
        expect(chain.init({w, h, look::LookOutputEncoding::Srgb, 0.f, fuse::kernel::Backend::CpuReference}), "chain init");
        look::LookChainInput cin{};
        cin.hdr = in.hdr.data();
        cin.linear_depth_m = in.depth.data();
        cin.velocity_px = in.velocity.data();
        cin.frame_seed = 9u;
        std::vector<Vec3> oracle(static_cast<usize>(w) * h);
        expect(chain.process(cin, lookGraph, look, lut, oracle.data()), "LookPostChain::process");
        expect(report(lang.name, "default look vs LookPostChain", compareDisplay(c.dump, oracle)),
               "look within 1/1024 of LookPostChain");
        checkImage(lang.name, c);
        post.setGradeLut(nullptr);
        post.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no post kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "post.", 5) == 0) {
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
    PostStackGpu post;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initPost(ctx, post, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no post kernel built\n");
        return kSkip;
    }
    const u32 w = 64;
    const u32 h = 40;
    if (!ensureTargets(ctx, w, h)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    PostStack stack;
    stack.init({w, h});
    configureFullStack(stack);
    look::LookParamBlock b = look::look_default_params();
    b.set(look::LookParam::DofEnabled, 1.f);
    const look::LookResolved look = look::look_resolve(b);
    const look::LookEffectGraph lookGraph = look::LookEffectGraph::makeDefault();
    look::Lut3D lut;
    look::lut_generate_grade(look, 32, lut);
    post.setGradeLut(&lut);
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long postSide = 0, callbacks = 0, build = 0;
    std::vector<Inputs> contents;
    for (u32 i = 0; i < 4u; ++i) {
        contents.push_back(makeInputs(w, h, i, 0.5f + static_cast<f32>(i)));
    }
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    PostGpuSettings s{};
    u32 workRebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        writeStaging(ctx, contents[frame % 4u]);
        const bool viaLook = (frame % 2u) == 1u;
        s = settings_from_post_stack(stack, 500u + frame, 1.f / 60.f);
        Frame f{};
        f.settings = &s;
        if (viaLook) {
            f.settings = nullptr;
            f.graph = &lookGraph;
            f.look = &look;
            f.seed = 700u + frame;
        }
        f.readback = false;
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        PostFrameImages images{};
        images.hdr = &ctx.hdr;
        images.depth = &ctx.depth;
        images.velocity = &ctx.velocity;
        t_allocations = 0;
        t_count = measure;
        const bool begun = viaLook ? post.beginLookFrame(ctx.serial, lookGraph, look, f.seed, f.dt, images)
                                   : post.beginFrame(ctx.serial, s, images);
        t_count = false;
        const unsigned long long begin = t_allocations;
        // Graph build (test passes + post passes) under the counter.
        t_allocations = 0;
        t_count = measure;
        graph.reset();
        TestRecord& r = g_record;
        r = TestRecord{};
        r.ctx = &ctx;
        Texture* textures[3] = {&ctx.hdr, &ctx.depth, &ctx.velocity};
        const u32 formats[3] = {static_cast<u32>(GpuFormat::R16G16B16A16Sfloat), static_cast<u32>(GpuFormat::R32Sfloat),
                                static_cast<u32>(GpuFormat::R16G16Sfloat)};
        for (u32 i = 0; i < 3u; ++i) {
            rg::ImportedImage im{};
            im.image = textures[i]->image;
            im.view = textures[i]->view;
            im.format = formats[i];
            im.width = w;
            im.height = h;
            im.initialLayout = ctx.layouts[i];
            im.initialQueue = ctx.queues[i];
            im.layoutTracker = &ctx.layouts[i];
            im.queueTracker = &ctx.queues[i];
            r.images[i] = graph.importImage(im);
        }
        r.staging = graph.importBuffer(
            rg::ImportedBuffer{ctx.staging.handle, ctx.staging.desc.size, ctx.stagingQueue, &ctx.stagingQueue, "rp_post.staging"});
        rg::PassBuilder up = graph.addPass("test.upload", &recordUpload, &r);
        up.use(r.staging, rg::Access::TransferSrc);
        for (const rg::TextureRef& t : r.images) {
            up.use(t, rg::Access::TransferDst);
        }
        const PostGraphRefs refs = post.importInto(graph);
        post.addPasses(graph, refs, PostGraphInputs{r.images[0], r.images[1], r.images[2], {}});
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        post.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(post.stats().bloomRan && post.stats().dofRan && post.stats().motionBlurRan && post.stats().exposureRan,
               "every pass runs in steady state");
        if (measure) {
            postSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            workRebuilds = post.stats().workRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(post.stats().workRebuilds == workRebuilds && post.stats().outputRebuilds == 1u && post.stats().lutRebuilds == 1u,
           "no work / output / LUT rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; PostStack and Look frames alternating; bloom, DoF, motion blur,\n"
                    "  auto exposure, LUT; content and seeds changing)\n"
                    "  PostStackGpu::beginFrame / beginLookFrame + post.* pass callbacks: %llu operator-new calls "
                    "(callbacks %llu)\n"
                    "  whole graph build (test upload + post imports and passes): %llu\n",
                    kTotal - kWarmup, post.kernelLanguage(), postSide, callbacks, build);
        expect(postSide == 0u, "the GPU post stack makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the post passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    post.kernelLanguage());
    }
    post.destroy();
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
        } else if (mode == "stack") {
            rc = runStack(ctx);
        } else if (mode == "look") {
            rc = runLook(ctx);
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
