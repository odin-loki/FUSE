// Screen-space radiance cascades (the WP-6.5 follow-up): Lavapipe gates (VK_LAYER_KHRONOS_validation with
// synchronization validation; every validation message fails the run). CPU gates: test_rp_ssrc_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): a test pass uploads the WP-1.5 G-buffer attachments (RT0 RGBA16F
// signed-octahedral normal + AO, RT1 / RT2 RGBA8, RT4 R32F forward z/w depth) and the WP-2.1 lit image (RGBA16F) of the
// ray-cast SSFX room scene (test_rp_ssfx_gpu_scene.hpp, 97 x 71, odd extent), then SsrcGpu adds its passes, then
// read-back copies (every work-buffer section, the output image). Both kernel languages built run every check.
//
//   --mode parity      ssrc.prepare == ssfx_gpu::prepare_pixel on the uploaded texels (<= 1e-6 relative: unorm8 /
//                      division rounding); ssrc.cascade (cascades 0 and 1, the sections left after the frame) and
//                      ssrc.gather == the CPU reference (SsrcCpuSolver) run on the GPU's own read-back prepared
//                      sections, bit for bit; the RGBA16F output within 1 half ulp of the f32 dump; Slang == GLSL bit
//                      for bit. Configurations: A = the defaults (bilinear fix, s0 1, N0 16, AO over 2 cascades,
//                      compose) on 2 camera positions; B = vanilla, s0 2, sky + the DDGI far field (a synthetic 2 x 2 x 2
//                      volume uploaded as a gi_gpu::DdgiVolumeView + atlases), plane test, depth-proportional slab,
//                      origin bias, AO over every cascade, raw indirect + AO output.
//   --mode zero_alloc  64 steady-state frames (all passes, camera changing): 0 operator-new calls in
//                      SsrcGpu::beginFrame, the ssrc.* pass callbacks and the whole graph build (validated run first;
//                      validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_types.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/renderer/ssrc/ssrc_gpu.hpp>
#include <fuse/renderer/ssrc/ssrc_reference.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

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
using namespace fuse::renderer::ssrc;
using fuse::f32;
using fuse::f64;
using fuse::i32;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace ssfx_gpu = fuse::renderer::ssfx_gpu;

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
constexpr u32 kSectionCount = 7u; ///< SsrcCopySource order

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

/// |image - dump| within one half-float ulp of the dump value (the device's f32 -> f16 store may truncate).
bool withinHalfUlp(f32 image, f32 dump) {
    const u16 bits = GBufferQuantize::floatToHalf(dump);
    const f32 lo = halfToFloat(GBufferQuantize::halfNextDown(bits));
    const f32 hi = halfToFloat(GBufferQuantize::halfNextUp(bits));
    return image >= std::min(lo, hi) && image <= std::max(lo, hi);
}

bool sameBits(f32 a, f32 b) { return std::memcmp(&a, &b, 4u) == 0; }

f64 relErr(f32 a, f32 b, f32 absFloor) {
    return std::fabs(static_cast<f64>(a) - b) / std::max(static_cast<f64>(std::fabs(b)), static_cast<f64>(absFloor));
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
    u64 readbackBytes = 0;
    Texture images[kInputCount]{};
    u32 layouts[kInputCount] = {};
    u8 queues[kInputCount] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
    Buffer staging{};
    Buffer readback{};
    Buffer dump{};
    Buffer ddgi[3]{}; ///< DdgiVolumeView record, irradiance atlas, distance atlas
    u8 stagingQueue = rg::kNoQueue;
    u8 readbackQueue = rg::kNoQueue;
    u8 dumpQueue = rg::kNoQueue;
    u8 ddgiQueues[3] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
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
        if (allocator != nullptr) {
            for (Buffer& b : ddgi) {
                if (b.handle != nullptr) {
                    allocator->destroyBuffer(b);
                }
            }
        }
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
    instanceDesc.appName = "fuse_rp_ssrc_gpu";
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
    const SsrcCapabilities caps = querySsrcCapabilities(ctx.device.get());
    if (!caps.ssrc) {
        std::printf("SKIP: screen-space radiance cascades unsupported: %s\n", caps.reason);
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
const char* const kImageNames[kInputCount] = {"rp_ssrc.rt0", "rp_ssrc.rt1", "rp_ssrc.rt2", "rp_ssrc.rt4", "rp_ssrc.lit"};

u64 align256(u64 v) { return (v + 255u) & ~u64{255u}; }

u64 stagingOffset(u32 w, u32 h, u32 image) {
    u64 at = 0;
    for (u32 i = 0; i < image; ++i) {
        at += align256(static_cast<u64>(w) * h * kTexelBytes[i]);
    }
    return at;
}

/// Read-back layout: the 7 sections (SsrcCopySource order, `sectionBytes` each), then the output image (RGBA16F).
struct ReadbackLayout {
    u64 section[kSectionCount] = {};
    u64 image = 0;
    u64 bytes = 0;
};

ReadbackLayout readbackLayout(const SsrcGpu& ssrc) {
    ReadbackLayout l{};
    u64 at = 0;
    for (u32 s = 0; s < kSectionCount; ++s) {
        l.section[s] = at;
        at += align256(ssrc.copyBytes(static_cast<SsrcCopySource>(s)));
    }
    l.image = at;
    at += align256(static_cast<u64>(ssrc.width()) * ssrc.height() * 8u);
    l.bytes = at;
    return l;
}

/// Read-back bytes a frame of `settings` at w x h needs (ReadbackLayout of the resolved layout).
u64 readbackBytesFor(const SsrcSettings& settings, u32 w, u32 h) {
    SsrcFrameConstants c{};
    SsrcLayoutInfo info{};
    if (!resolve_layout(settings, w, h, c, &info)) {
        return 0u;
    }
    const u64 n = static_cast<u64>(w) * h;
    return 5u * align256(n * 16u) + 2u * align256(info.maxRecords * 8u) + align256(n * 8u);
}

bool createBuffer(Context& ctx, Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(size);
    d.usage = usage;
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
}

bool ensureTargets(Context& ctx, u32 w, u32 h, u64 readbackBytes) {
    if (ctx.w == w && ctx.h == h && ctx.images[0].image != nullptr && ctx.readbackBytes >= readbackBytes) {
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
    const u64 n = static_cast<u64>(w) * h;
    ok = ok && createBuffer(ctx, ctx.staging, stagingOffset(w, h, kInputCount), BufferUsage::TransferSrc,
                            MemoryUsage::CpuToGpu, "rp_ssrc.staging") &&
         createBuffer(ctx, ctx.readback, std::max<u64>(readbackBytes, 256u), BufferUsage::TransferDst,
                      MemoryUsage::GpuToCpu, "rp_ssrc.readback") &&
         createBuffer(ctx, ctx.dump, n * 16u,
                      static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                               static_cast<u32>(BufferUsage::ShaderDeviceAddress)),
                      MemoryUsage::GpuToCpu, "rp_ssrc.dump") &&
         ctx.dump.deviceAddress != 0u;
    ctx.w = w;
    ctx.h = h;
    ctx.readbackBytes = std::max<u64>(readbackBytes, 256u);
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
        const u16 t0[4] = {ssfx_test::halfBits(g.rt0[i].x), ssfx_test::halfBits(g.rt0[i].y),
                           ssfx_test::halfBits(g.rt0[i].z), ssfx_test::halfBits(g.rt0[i].w)};
        std::memcpy(rt0 + i * 8u, t0, 8u);
        const u8 t1[4] = {unorm8Byte(g.rt1[i].x), unorm8Byte(g.rt1[i].y), unorm8Byte(g.rt1[i].z), unorm8Byte(g.rt1[i].w)};
        std::memcpy(rt1 + i * 4u, t1, 4u);
        const u8 t2[4] = {unorm8Byte(g.rt2[i].x), unorm8Byte(g.rt2[i].y), unorm8Byte(g.rt2[i].z), unorm8Byte(g.rt2[i].w)};
        std::memcpy(rt2 + i * 4u, t2, 4u);
        std::memcpy(rt4 + i * 4u, &g.rt4[i], 4u);
        const u16 tl[4] = {ssfx_test::halfBits(g.lit[i].x), ssfx_test::halfBits(g.lit[i].y),
                           ssfx_test::halfBits(g.lit[i].z), ssfx_test::halfBits(g.lit[i].w)};
        std::memcpy(lit + i * 8u, tl, 8u);
    }
}

// --- synthetic DDGI volume (the far-field hook) -----------------------------------------------------------
struct DdgiVolume {
    ddgi_kernel::VolumeView view{};
    std::vector<Vec3> irradiance;
    std::vector<fuse::math::Vec2> distance;
};

/// 2 x 2 x 2 probes around the room, irradiance / depth tiles of 4^2 (+ border) with smooth distinct values;
/// the same arrays feed the CPU VolumeView and the GPU atlases (byte for byte).
void buildDdgi(Context& ctx, DdgiVolume& v) {
    static_assert(sizeof(Vec3) == 12u && sizeof(fuse::math::Vec2) == 8u, "atlas texel layout");
    DDGIDesc desc{};
    desc.grid_origin = Vec3{-4.f, -0.5f, -5.f};
    desc.probe_spacing = Vec3{8.f, 4.f, 12.f};
    desc.grid_dims = DDGIGridDims{2u, 2u, 2u};
    desc.irradiance_res = 4u;
    desc.depth_res = 4u;
    const u32 probes = 8u;
    const u32 it = (desc.irradiance_res + 2u) * (desc.irradiance_res + 2u);
    const u32 dt = (desc.depth_res + 2u) * (desc.depth_res + 2u);
    v.irradiance.resize(static_cast<usize>(probes) * it);
    v.distance.resize(static_cast<usize>(probes) * dt);
    for (u32 p = 0; p < probes; ++p) {
        for (u32 t = 0; t < it; ++t) {
            v.irradiance[static_cast<usize>(p) * it + t] = Vec3{0.2f + 0.1f * static_cast<f32>(p % 2u),
                                                                0.3f + 0.02f * static_cast<f32>(t % 5u),
                                                                0.25f + 0.05f * static_cast<f32>((p + t) % 3u)};
        }
        for (u32 t = 0; t < dt; ++t) {
            const f32 m = 6.f + static_cast<f32>(t % 4u);
            v.distance[static_cast<usize>(p) * dt + t] = fuse::math::Vec2{m, m * m + 0.5f};
        }
    }
    v.view.desc = desc;
    v.view.probe_count = probes;
    v.view.irradiance = v.irradiance.data();
    v.view.distance = v.distance.data();
    v.view.normal_bias = 0.1f;
    v.view.weight_crush_threshold = 0.2f;
    const BufferUsage usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                       static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    const bool ok =
        createBuffer(ctx, ctx.ddgi[1], v.irradiance.size() * sizeof(Vec3), usage, MemoryUsage::CpuToGpu, "rp_ssrc.ddgi_irr") &&
        createBuffer(ctx, ctx.ddgi[2], v.distance.size() * sizeof(fuse::math::Vec2), usage, MemoryUsage::CpuToGpu,
                     "rp_ssrc.ddgi_dist") &&
        createBuffer(ctx, ctx.ddgi[0], sizeof(gi_gpu::DdgiVolumeView), usage, MemoryUsage::CpuToGpu, "rp_ssrc.ddgi_view");
    expect(ok, "DDGI buffers");
    if (!ok) {
        return;
    }
    std::memcpy(ctx.ddgi[1].mapped, v.irradiance.data(), v.irradiance.size() * sizeof(Vec3));
    std::memcpy(ctx.ddgi[2].mapped, v.distance.data(), v.distance.size() * sizeof(fuse::math::Vec2));
    gi_gpu::DdgiVolumeView g{};
    g.origin[0] = desc.grid_origin.x;
    g.origin[1] = desc.grid_origin.y;
    g.origin[2] = desc.grid_origin.z;
    g.probeCount = probes;
    g.spacing[0] = desc.probe_spacing.x;
    g.spacing[1] = desc.probe_spacing.y;
    g.spacing[2] = desc.probe_spacing.z;
    g.irradianceRes = desc.irradiance_res;
    g.dims[0] = desc.grid_dims.x;
    g.dims[1] = desc.grid_dims.y;
    g.dims[2] = desc.grid_dims.z;
    g.depthRes = desc.depth_res;
    g.normalBias = v.view.normal_bias;
    g.weightCrushThreshold = v.view.weight_crush_threshold;
    g.intensity = 1.f;
    g.irradiance = ctx.ddgi[1].deviceAddress;
    g.distance = ctx.ddgi[2].deviceAddress;
    std::memcpy(ctx.ddgi[0].mapped, &g, sizeof(g));
}

// --- per-frame graph ------------------------------------------------------------------------------
struct TestRecord {
    Context* ctx = nullptr;
    rg::BufferRef staging;
    rg::BufferRef readback;
    rg::TextureRef images[kInputCount];
    rg::TextureRef output;
    u64 imageOffset = 0;
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
    region.bufferOffset = r.imageOffset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.ctx->w, r.ctx->h, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(r.output)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VkBuffer>(pc.buffer(r.readback)), 1, &region);
}

/// Imports the test images / buffers, adds the upload, SsrcGpu's passes and (optionally) the read-backs.
void buildGraph(Context& ctx, SsrcGpu& ssrc, rg::Graph& graph, bool readback, bool ddgi) {
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
        rg::ImportedBuffer{ctx.staging.handle, ctx.staging.desc.size, ctx.stagingQueue, &ctx.stagingQueue, "rp_ssrc.staging"});
    rg::PassBuilder up = graph.addPass("test.upload", &recordUpload, &r);
    up.use(r.staging, rg::Access::TransferSrc);
    for (const rg::TextureRef& t : r.images) {
        up.use(t, rg::Access::TransferDst);
    }
    const SsrcGraphRefs refs = ssrc.importInto(graph);
    r.output = refs.output;
    SsrcGraphInputs inputs{};
    inputs.normalAo = r.images[0];
    inputs.albedo = r.images[1];
    inputs.roughMetal = r.images[2];
    inputs.depth = r.images[3];
    inputs.lit = r.images[4];
    if (readback) {
        inputs.dump =
            graph.importBuffer(rg::ImportedBuffer{ctx.dump.handle, ctx.dump.desc.size, ctx.dumpQueue, &ctx.dumpQueue, "rp_ssrc.dump"});
    }
    if (ddgi) {
        const char* names[3] = {"rp_ssrc.ddgi_view", "rp_ssrc.ddgi_irr", "rp_ssrc.ddgi_dist"};
        for (u32 i = 0; i < 3u; ++i) {
            inputs.ddgi[i] = graph.importBuffer(
                rg::ImportedBuffer{ctx.ddgi[i].handle, ctx.ddgi[i].desc.size, ctx.ddgiQueues[i], &ctx.ddgiQueues[i], names[i]});
        }
    }
    ssrc.addPasses(graph, refs, inputs);
    if (readback) {
        const ReadbackLayout l = readbackLayout(ssrc);
        r.imageOffset = l.image;
        r.readback = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, ctx.readbackQueue,
                                                           &ctx.readbackQueue, "rp_ssrc.readback"});
        for (u32 s = 0; s < kSectionCount; ++s) {
            ssrc.addCopy(graph, refs, static_cast<SsrcCopySource>(s), r.readback, l.section[s]);
        }
        graph.addPass("test.readback_image", &recordImageReadback, &r)
            .use(refs.output, rg::Access::TransferSrc)
            .use(r.readback, rg::Access::TransferDst,
                 rg::BufferRange{l.image, static_cast<u64>(ctx.w) * ctx.h * 8u});
    }
}

struct Captured {
    SsrcInputs in;
    std::vector<f32> indirect;
    std::vector<u32> records[2];
    std::vector<f32> dump;
    std::vector<f32> image;
    SsrcFrameConstants constants{};
};

SsrcFrameImages frameImages(Context& ctx, bool dump, bool ddgi) {
    SsrcFrameImages images{};
    images.normalAo = &ctx.images[0];
    images.albedo = &ctx.images[1];
    images.roughMetal = &ctx.images[2];
    images.depth = &ctx.images[3];
    images.lit = &ctx.images[4];
    images.dumpAddress = dump ? ctx.dump.deviceAddress : 0u;
    images.ddgiVolume = ddgi ? ctx.ddgi[0].deviceAddress : 0u;
    return images;
}

bool runFrame(Context& ctx, SsrcGpu& ssrc, rg::Graph& graph, const SsrcSettings& settings,
              const ssfx_gpu::SsfxCameraDesc& camera, const f32 (&ambient)[3], Captured& out) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!ssrc.beginFrame(ctx.serial, settings, camera, ambient, frameImages(ctx, true, settings.ddgi))) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    const ReadbackLayout l = readbackLayout(ssrc);
    if (ctx.readbackBytes < l.bytes) {
        std::fprintf(stderr, "  read-back buffer too small\n");
        return false;
    }
    buildGraph(ctx, ssrc, graph, true, settings.ddgi);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    ssrc.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    const usize n = static_cast<usize>(ctx.w) * ctx.h;
    const u8* base = static_cast<const u8*>(ctx.readback.mapped);
    auto floats = [&](SsrcCopySource s, std::vector<f32>& v) {
        v.resize(n * 4u);
        std::memcpy(v.data(), base + l.section[static_cast<u32>(s)], n * 16u);
    };
    out.in.width = ctx.w;
    out.in.height = ctx.h;
    floats(SsrcCopySource::Geo, out.in.geo);
    floats(SsrcCopySource::Lit, out.in.lit);
    floats(SsrcCopySource::Diffuse, out.in.diffuse);
    floats(SsrcCopySource::Albedo, out.in.albedo);
    floats(SsrcCopySource::Indirect, out.indirect);
    for (u32 s = 0; s < 2u; ++s) {
        const SsrcCopySource src = s == 0u ? SsrcCopySource::Records0 : SsrcCopySource::Records1;
        out.records[s].resize(static_cast<usize>(ssrc.copyBytes(src) / 4u));
        std::memcpy(out.records[s].data(), base + l.section[static_cast<u32>(src)], ssrc.copyBytes(src));
    }
    out.dump.resize(n * 4u);
    std::memcpy(out.dump.data(), ctx.dump.mapped, n * 16u);
    out.image.resize(n * 4u);
    for (usize i = 0; i < n * 4u; ++i) {
        u16 h = 0;
        std::memcpy(&h, base + l.image + i * 2u, 2u);
        out.image[i] = halfToFloat(h);
    }
    out.constants = ssrc.constants();
    return true;
}

struct Lang {
    SsrcKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{SsrcKernelLanguage::Slang, "slang"}, {SsrcKernelLanguage::Glsl, "glsl"}};

bool initSsrc(Context& ctx, SsrcGpu& ssrc, SsrcKernelLanguage language) {
    SsrcGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    return ssrc.init(d);
}

// --- parity ---------------------------------------------------------------------------------------------------
struct Config {
    const char* name;
    SsrcSettings settings;
};

Config configA() {
    Config c{"A: defaults (bilinear fix, s0 1, N0 16, AO 2 cascades, compose)", {}};
    c.settings.compose = true;
    return c;
}

Config configB() {
    Config c{"B: vanilla, s0 2, sky + DDGI far field, plane test, slab slope, bias, AO all", {}};
    c.settings.bilinearFix = false;
    c.settings.probeSpacing0 = 2u;
    c.settings.sky[0] = 0.3f;
    c.settings.sky[1] = 0.4f;
    c.settings.sky[2] = 0.6f;
    c.settings.ddgi = true;
    c.settings.ddgiScale = 0.5f;
    c.settings.planeTolerance = 0.02f;
    c.settings.thickness = 0.2f;
    c.settings.thicknessSlope = 0.05f;
    c.settings.originBias = 0.002f;
    c.settings.aoCascades = 0u;
    c.settings.intensity = 1.25f;
    return c;
}

u32 cascadeWords(const SsrcFrameConstants& c, u32 ci) {
    return c.probesX[ci] * c.probesY[ci] * c.dirRes[ci] * c.dirRes[ci] * 2u;
}

void checkFrame(const char* lang, const Config& cfg, const ssfx_test::RoomScene& room, const Captured& gpu,
                const DdgiVolume* ddgi) {
    const usize n = static_cast<usize>(gpu.in.width) * gpu.in.height;
    const SsrcFrameConstants& c = gpu.constants;
    // prepare vs prepare_pixel (WP-6.3's oracle) on the uploaded texels.
    ssfx_gpu::SsfxGpuSettings ss{};
    ssfx_gpu::SsfxFrameConstants sc{};
    expect(ssfx_gpu::resolve_constants(ss, ssfx_test::cameraDesc(room.camera), room.ambient, gpu.in.width, gpu.in.height, sc),
           "ssfx constants");
    ssfx_gpu::SsfxPreparedFrame prep;
    ssfx_test::prepareCpu(sc, room.g, prep);
    SsrcInputs cpuIn;
    inputsFromPrepared(prep, cpuIn);
    f64 prepErr = 0.0;
    u32 prepExact = 0;
    for (usize i = 0; i < n * 4u; ++i) {
        const f64 e = std::max({relErr(gpu.in.geo[i], cpuIn.geo[i], 1e-6f), relErr(gpu.in.lit[i], cpuIn.lit[i], 1e-6f),
                                relErr(gpu.in.diffuse[i], cpuIn.diffuse[i], 1e-6f),
                                relErr(gpu.in.albedo[i], cpuIn.albedo[i], 1e-6f)});
        prepErr = std::max(prepErr, e);
        prepExact += e == 0.0 ? 1u : 0u;
    }
    std::printf("  [%s] %s\n  [%s]   prepare vs prepare_pixel: %u / %zu values bit-identical, max rel err %.3g\n", lang,
                cfg.name, lang, prepExact, n * 4u, prepErr);
    expect(prepErr <= 1e-6, "ssrc.prepare == prepare_pixel within 1e-6 (unorm8 / division rounding)");

    // The CPU reference on the GPU's own prepared sections.
    SsrcCpuSolver solver;
    std::vector<f32> ind;
    std::vector<f32> img;
    SsrcStats stats{};
    expect(solver.solve(c, gpu.in, ddgi != nullptr ? &ddgi->view : nullptr, ind, img, &stats), "CPU solve");
    const u32 checkCascades = std::min(2u, c.cascades);
    for (u32 ci = 0; ci < checkCascades; ++ci) {
        const u32 words = cascadeWords(c, ci);
        const std::vector<u32>& cpu = solver.records(ci);
        const std::vector<u32>& dev = gpu.records[ci];
        u32 exact = 0;
        u32 nearHalf = 0;
        for (u32 i = 0; i < words; ++i) {
            if (cpu[i] == dev[i]) {
                ++exact;
                ++nearHalf;
                continue;
            }
            bool within = true;
            for (u32 hlf = 0; hlf < 2u; ++hlf) {
                const i32 a = static_cast<i32>((cpu[i] >> (16u * hlf)) & 0xFFFFu);
                const i32 b = static_cast<i32>((dev[i] >> (16u * hlf)) & 0xFFFFu);
                within = within && std::abs(a - b) <= 1;
            }
            nearHalf += within ? 1u : 0u;
        }
        std::printf("  [%s]   ssrc.cascade %u (%u records, %u dirs, spacing %.0f px): %u / %u words bit-identical "
                    "(%u within 1 f16 ulp)\n",
                    lang, ci, words / 2u, c.dirRes[ci] * c.dirRes[ci], static_cast<f64>(c.spacing[ci]), exact, words,
                    nearHalf);
        if (ddgi == nullptr) {
            expect(exact == words, "ssrc.cascade == ssrc_cascade_record bit for bit");
        } else {
            // The DDGI sampler (WP-6.1) agrees with its CPU oracle to the rounding of sqrt / division.
            expect(nearHalf == words && static_cast<f64>(exact) >= 0.999 * words,
                   "ssrc.cascade with the DDGI far field: >= 99.9% bit-identical, all within 1 f16 ulp");
        }
    }
    u32 indExact = 0;
    u32 dumpExact = 0;
    f64 indErr = 0.0;
    u32 imageOk = 0;
    for (usize i = 0; i < n; ++i) {
        bool same = true;
        bool dsame = true;
        for (u32 ch = 0; ch < 4u; ++ch) {
            same = same && sameBits(gpu.indirect[i * 4u + ch], ind[i * 4u + ch]);
            dsame = dsame && sameBits(gpu.dump[i * 4u + ch], img[i * 4u + ch]);
            indErr = std::max(indErr, relErr(gpu.indirect[i * 4u + ch], ind[i * 4u + ch], 1e-4f));
        }
        indExact += same ? 1u : 0u;
        dumpExact += dsame ? 1u : 0u;
        bool ok = true;
        for (u32 ch = 0; ch < 4u; ++ch) {
            ok = ok && withinHalfUlp(gpu.image[i * 4u + ch], gpu.dump[i * 4u + ch]);
        }
        imageOk += ok ? 1u : 0u;
    }
    f64 lit = 0.0;
    for (usize i = 0; i < n; ++i) {
        lit += (gpu.indirect[i * 4u] + gpu.indirect[i * 4u + 1u] + gpu.indirect[i * 4u + 2u]) / 3.0;
    }
    std::printf("  [%s]   ssrc.gather: indirect %u / %zu pixels bit-identical (max rel %.3g, mean indirect %.4f), "
                "output value %u / %zu bit-identical, RGBA16F image within 1 half ulp on %u / %zu; CPU cost %.0f "
                "samples/px\n",
                lang, indExact, n, indErr, lit / static_cast<f64>(n), dumpExact, n, imageOk, n,
                static_cast<f64>(stats.samples) / static_cast<f64>(n));
    if (ddgi == nullptr) {
        expect(indExact == n && dumpExact == n, "ssrc.gather == ssrc_gather_pixel bit for bit");
    } else {
        expect(indErr <= 2e-3, "ssrc.gather with the DDGI far field within 2e-3 relative");
    }
    expect(imageOk == n, "RGBA16F output == the f32 value converted to half (<= 1 half ulp)");
    expect(lit > 1e-3, "the frame has indirect light");
    if (ddgi != nullptr) {
        // The far-field hook contributes: the same frame without the volume (sky only) is darker.
        std::vector<f32> skyOnly;
        std::vector<f32> skyImg;
        expect(solver.solve(c, gpu.in, nullptr, skyOnly, skyImg), "CPU solve without the volume");
        f64 skyLit = 0.0;
        for (usize i = 0; i < n; ++i) {
            skyLit += (skyOnly[i * 4u] + skyOnly[i * 4u + 1u] + skyOnly[i * 4u + 2u]) / 3.0;
        }
        std::printf("  [%s]   DDGI far field: mean indirect %.4f with the volume vs %.4f sky only\n", lang,
                    lit / static_cast<f64>(n), skyLit / static_cast<f64>(n));
        expect(lit - skyLit > 0.01 * static_cast<f64>(n), "the DDGI far field adds light");
    }
}

int runParity(Context& ctx) {
    constexpr u32 kW = 97;
    constexpr u32 kH = 71;
    if (!ensureTargets(ctx, kW, kH,
                       std::max(readbackBytesFor(configA().settings, kW, kH), readbackBytesFor(configB().settings, kW, kH)))) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    DdgiVolume ddgi;
    buildDdgi(ctx, ddgi);
    u32 built = 0;
    std::vector<Captured> perLang[2];
    struct Run {
        Config cfg;
        u32 cameraFrame;
    };
    const Run runs[3] = {{configA(), 0u}, {configA(), 2u}, {configB(), 1u}};
    for (u32 l = 0; l < 2u; ++l) {
        SsrcGpu ssrc;
        if (!initSsrc(ctx, ssrc, kLangs[l].language)) {
            std::printf("  [%s] kernels not built: skipped\n", kLangs[l].name);
            continue;
        }
        ++built;
        rg::Graph graph;
        for (const Run& run : runs) {
            ssfx_test::RoomScene room;
            ssfx_test::buildRoom(room, kW, kH, run.cameraFrame);
            writeStaging(ctx, room.g);
            Captured cap;
            expect(runFrame(ctx, ssrc, graph, run.cfg.settings, ssfx_test::cameraDesc(room.camera), room.ambient, cap),
                   "frame");
            std::printf("  [%s] camera %u: %u passes (%u cascades)\n", kLangs[l].name, run.cameraFrame, ssrc.stats().passes,
                        ssrc.stats().cascades);
            checkFrame(kLangs[l].name, run.cfg, room, cap, run.cfg.settings.ddgi ? &ddgi : nullptr);
            perLang[l].push_back(std::move(cap));
        }
        expect(ssrc.stats().outputRebuilds == 1u, "no output rebuild at a constant extent");
        ssrc.destroy();
    }
    if (built == 0u) {
        std::printf("SKIP: no radiance-cascades kernel built\n");
        return kSkip;
    }
    if (built == 2u && perLang[0].size() == perLang[1].size()) {
        u32 same = 0;
        u32 total = 0;
        for (usize f = 0; f < perLang[0].size(); ++f) {
            const Captured& a = perLang[0][f];
            const Captured& b = perLang[1][f];
            for (u32 s = 0; s < 2u; ++s) {
                const u32 words = cascadeWords(a.constants, s);
                for (u32 i = 0; i < words; ++i) {
                    same += a.records[s][i] == b.records[s][i] ? 1u : 0u;
                    ++total;
                }
            }
            for (usize i = 0; i < a.dump.size(); ++i) {
                same += sameBits(a.dump[i], b.dump[i]) ? 1u : 0u;
                ++total;
            }
        }
        std::printf("  slang vs glsl (%zu frames): %u / %u record words + output values bit-identical\n",
                    perLang[0].size(), same, total);
        expect(same == total, "Slang == GLSL bit for bit");
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "ssrc.", 5) == 0) {
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
    SsrcGpu ssrc;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initSsrc(ctx, ssrc, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no radiance-cascades kernel built\n");
        return kSkip;
    }
    constexpr u32 kW = 48;
    constexpr u32 kH = 32;
    if (!ensureTargets(ctx, kW, kH, 0u)) {
        std::fprintf(stderr, "FAIL: targets\n");
        return 1;
    }
    std::vector<ssfx_test::RoomScene> rooms(4);
    for (u32 i = 0; i < 4u; ++i) {
        ssfx_test::buildRoom(rooms[i], kW, kH, i);
    }
    SsrcSettings settings{};
    settings.compose = true;
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
        settings.sky[0] = 0.1f * static_cast<f32>(frame % 3u);
        const ssfx_gpu::SsfxCameraDesc camera = ssfx_test::cameraDesc(room.camera);
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        const SsrcFrameImages images = frameImages(ctx, false, false);
        t_allocations = 0;
        t_count = measure;
        const bool begun = ssrc.beginFrame(ctx.serial, settings, camera, room.ambient, images);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, ssrc, graph, false, false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        ssrc.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(ssrc.stats().cascades == ssrc.constants().cascades && ssrc.stats().passes == ssrc.constants().cascades + 2u,
               "every pass runs");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            workRebuilds = ssrc.stats().workRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(ssrc.stats().workRebuilds == workRebuilds && ssrc.stats().outputRebuilds == 1u,
           "no work / output rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; prepare, %u cascades, gather + compose; camera changing)\n"
                    "  SsrcGpu::beginFrame + ssrc.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (test upload + ssrc imports and passes): %llu\n",
                    kTotal - kWarmup, ssrc.kernelLanguage(), ssrc.constants().cascades, side, callbacks, build);
        expect(side == 0u, "the radiance-cascades passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the radiance-cascades passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    ssrc.kernelLanguage());
    }
    ssrc.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
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
        if (mode == "parity") {
            rc = runParity(ctx);
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
