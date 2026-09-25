// Frame composer Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation / sync-validation message fails the run). CPU gates: test_rp_frame_cpu.cpp.
//
// Scene (the WP-6.1 / WP-3.2 box-world helpers): the WP-1.5 test cube (mr_test::box) instanced as a ground slab,
// a back wall, three boxes, a floating slab and a small emissive block (DdgiCpuScene boxes, so the T0 DDGI global
// SDF is the same world: gi_gpu::sdfSceneFromBoxes), a sun (directional, slot 0) + a point + a spot light, and 24
// gaussian splats in front of the wall. Render 96 x 64, display 144 x 96 (1.5x upscale).
//
//   --mode full         8 frames with every stage the tier allows (splats included; TAAU frames 0-3, FSR 3 frames
//                       4-7 at T2 / TAAU at T0) under validation + sync validation: the graph executes, every
//                       planned stage ran, 0 messages, the output is finite and not black.
//   --mode toggles      one reset frame per configuration (render-resolution scene colour, upscaler / post off),
//                       each toggle changes the image only where expected:
//                         fog off == the fog-on frame's pre-fog image (bit for bit), fog on changes pixels
//                         sky on vs off: only background pixels (RT4 depth == 1) change, all of them
//                         aerial perspective on vs off: only geometry pixels change
//                         SSFX / VSM / DDGI on vs off: only geometry pixels change; SSFX off == the lit image
//                         clouds on vs off: only background pixels change
//                         splats on vs off: only pixels a splat covers (splat T < 1) change
//                         T2: RT shadows and the denoiser on vs off: only geometry pixels change
//                       and a repeated configuration reproduces its bits (the reset frame is self-contained).
//   --mode determinism  two independent runs (scene + composer rebuilt) of 6 frames: final output and scene
//                       colour bit-identical.
//   --mode zero_alloc   64 steady-state frames (camera moving, every stage on): 0 operator-new calls in
//                       FrameComposer::beginFrame, the whole graph build (reset + addFrame) and every pass callback
//                       (validated run first; validation off for the count: the layer allocates through operator new).
//   --mode golden       4 frames (T0: TAAU, T2: FSR 3), post on: the display image (8-bit) against
//                       Tests/golden/renderer/frame_composer_t<N>.png. Tolerance (kGoldenSpec): mean FLIP <= 0.02
//                       (PSNR >= 38 dB without the metrics library) and at most 1% of the pixels with a channel
//                       more than 4 / 255 off: the frame chains ~20 packages whose f32 math may move by an ulp
//                       across llvmpipe releases; bit-exactness on one build is gated by --mode determinism.
//   --mode parity       the composer's own kernel vs its CPU reference (frame_types.hpp) on a live frame:
//                       frame.gather background == its input image, distance == frame_view_distance (1e-5 rel),
//                       frame.resolve == half(frame_resolve_texel(clouds result, splat texel)) (<= 1 half ulp),
//                       T2: frame.shadow_pack == frame_pack_visibility(denoised) bit for bit.
//   --tier t0 | t2      T2 skips (77) without the T2 gate.
//   --language auto | slang | glsl   the composer kernel's language (auto = Slang when built, else GLSL).
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, kernel not built).
#include "harness/golden.hpp"
#include "harness/image_io.hpp"
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

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
using namespace fuse::renderer::frame;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
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
constexpr usize kStagingBytes = 8u * 1024u * 1024u;
constexpr u32 kRenderW = 96;
constexpr u32 kRenderH = 64;
constexpr u32 kDisplayW = 144;
constexpr u32 kDisplayH = 96;
constexpr u32 kSunSlot = 0;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0 ||
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
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
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

int setup(Context& ctx, bool validation, bool t2) {
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
    instanceDesc.appName = "fuse_rp_frame";
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
    if (t2 && !rt::queryRtCapabilities(ctx.device.get()).usable) {
        std::printf("SKIP: device below the T2 gate\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    BindlessDesc bdesc{};
    // Descriptor-set backend: descriptor-free compute pipelines (DDGI, FSR 3) after a bindless pass on the
    // descriptor-buffer backend hit VUID-vkCmdDispatch-None-08117 on Lavapipe (known quirk).
    bdesc.backend = BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_frame.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    BufferDesc readbackDesc{};
    readbackDesc.size = 8u * 1024u * 1024u;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_frame.readback";
    if (!ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: readback\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    return 0;
}

// --- scene ------------------------------------------------------------------------------------------
gpu_scene::GpuTransform boxTransform(const DdgiCpuBox& b) {
    gpu_scene::GpuTransform t{};
    t.rows[0][0] = (b.max.x - b.min.x) * 0.5f;
    t.rows[1][1] = (b.max.y - b.min.y) * 0.5f;
    t.rows[2][2] = (b.max.z - b.min.z) * 0.5f;
    t.rows[0][3] = (b.max.x + b.min.x) * 0.5f;
    t.rows[1][3] = (b.max.y + b.min.y) * 0.5f;
    t.rows[2][3] = (b.max.z + b.min.z) * 0.5f;
    return t;
}

constexpr f32 kToSun[3] = {0.45f, 0.8f, 0.4f};

struct Scene {
    gpu_scene::GpuScene gpu;
    geometry::MeshletMesh box;
    DdgiCpuScene cpu;
    std::vector<fuse::compute::SdfObject> sdf;
    std::vector<gi_gpu::DdgiSurface> surfaces;
    std::vector<gsplat::GsSplat> splats;
};

DdgiCpuScene makeWorld() {
    DdgiCpuScene w{};
    auto add = [&](Vec3 lo, Vec3 hi, Vec3 albedo, Vec3 emissive = {}) {
        DdgiCpuSurface s{};
        s.albedo = albedo;
        s.emissive = emissive;
        w.addBox(lo, hi, s);
    };
    add({-8.f, -0.2f, -12.f}, {8.f, 0.f, 4.f}, {0.6f, 0.6f, 0.55f});       // ground
    add({-6.f, 0.f, -8.f}, {6.f, 3.f, -7.6f}, {0.7f, 0.65f, 0.6f});        // back wall
    add({-2.2f, 0.f, -3.5f}, {-1.f, 1.2f, -2.3f}, {0.8f, 0.25f, 0.2f});    // red box
    add({0.6f, 0.f, -4.5f}, {2.f, 2.2f, -3.1f}, {0.25f, 0.35f, 0.8f});     // blue tower
    add({-0.8f, 1.6f, -2.6f}, {0.6f, 1.8f, -1.6f}, {0.7f, 0.7f, 0.7f});    // floating slab
    add({1.f, 0.f, -1.6f}, {1.4f, 0.4f, -1.2f}, {0.2f, 0.2f, 0.2f}, {2.f, 1.6f, 0.8f}); // emissive block
    const f32 l = std::sqrt(kToSun[0] * kToSun[0] + kToSun[1] * kToSun[1] + kToSun[2] * kToSun[2]);
    w.sun_direction = {kToSun[0] / l, kToSun[1] / l, kToSun[2] / l};
    w.sun_irradiance = {2.5f, 2.4f, 2.2f};
    w.sky_radiance = {0.05f, 0.07f, 0.1f};
    return w;
}

void makeSplats(std::vector<gsplat::GsSplat>& out) {
    out.clear();
    for (u32 i = 0; i < 24u; ++i) {
        gsplat::GsSplat s{};
        const f32 a = static_cast<f32>(i) * 0.7f;
        s.position[0] = -3.2f + 0.28f * static_cast<f32>(i);
        s.position[1] = 0.9f + 0.35f * std::sin(a);
        s.position[2] = -5.5f + 0.3f * std::cos(a);
        s.opacity = 0.85f;
        s.scale[0] = 0.12f;
        s.scale[1] = 0.08f + 0.04f * static_cast<f32>(i % 3u);
        s.scale[2] = 0.12f;
        const f32 c0 = 0.28209479f;
        const f32 col[3] = {0.9f - 0.03f * static_cast<f32>(i), 0.3f + 0.02f * static_cast<f32>(i), 0.6f};
        for (u32 k = 0; k < 3u; ++k) {
            s.sh[k] = (col[k] - 0.5f) / c0;
        }
        out.push_back(s);
    }
}

bool buildScene(Context& ctx, Scene& s) {
    s.cpu = makeWorld();
    gi_gpu::sdfSceneFromBoxes(s.cpu, s.sdf, s.surfaces);
    makeSplats(s.splats);
    gpu_scene::GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 64;
    d.meshCapacity = 4;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    if (!mr_test::build(mr_test::box(), s.box) || s.gpu.addMeshletMesh(s.box) != 0u) {
        return false;
    }
    for (u32 i = 0; i < s.cpu.boxes.size(); ++i) {
        const DdgiCpuBox& b = s.cpu.boxes[i];
        Material m{};
        m.baseColor = b.surface.albedo;
        m.metallic = i == 3u ? 0.6f : 0.f;
        m.roughness = i == 3u ? 0.25f : 0.7f;
        m.emissiveColor = b.surface.emissive;
        m.emissiveIntensity = 1.f;
        s.gpu.setMaterial(i, m);
        gpu_scene::InstanceDesc id{};
        id.mesh = 0;
        id.material = i;
        id.transform = boxTransform(b);
        s.gpu.addInstance(id);
    }
    gpu_scene::GpuLight sun{};
    sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    sun.direction[0] = -s.cpu.sun_direction.x;
    sun.direction[1] = -s.cpu.sun_direction.y;
    sun.direction[2] = -s.cpu.sun_direction.z;
    sun.color[0] = 1.f;
    sun.color[1] = 0.95f;
    sun.color[2] = 0.85f;
    sun.intensity = 2.5f;
    if (s.gpu.addLight(sun).slot != kSunSlot) {
        return false;
    }
    gpu_scene::GpuLight point{};
    point.type = static_cast<u32>(gpu_scene::GpuLightType::Point);
    point.position[0] = 0.2f;
    point.position[1] = 1.1f;
    point.position[2] = -1.2f;
    point.range = 4.f;
    point.color[0] = 1.f;
    point.color[1] = 0.7f;
    point.color[2] = 0.4f;
    point.intensity = 3.f;
    s.gpu.addLight(point);
    gpu_scene::GpuLight spot = point;
    spot.type = static_cast<u32>(gpu_scene::GpuLightType::Spot);
    spot.position[0] = -1.6f;
    spot.position[1] = 2.6f;
    spot.position[2] = -1.5f;
    spot.direction[0] = 0.f;
    spot.direction[1] = -1.f;
    spot.direction[2] = -0.2f;
    spot.range = 5.f;
    spot.cosInner = 0.95f;
    spot.cosOuter = 0.8f;
    spot.color[0] = 0.5f;
    spot.color[1] = 0.8f;
    spot.color[2] = 1.f;
    spot.intensity = 6.f;
    s.gpu.addLight(spot);
    const gpu_scene::GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

// --- composer ---------------------------------------------------------------------------------------
FrameKernelLanguage g_language = FrameKernelLanguage::Auto;

FrameComposerDesc composerDesc(Context& ctx, Scene& s, FrameTier tier) {
    FrameComposerDesc d{};
    d.language = g_language;
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.upload = &ctx.upload;
    d.scene = &s.gpu;
    d.tier = tier;
    d.renderWidth = kRenderW;
    d.renderHeight = kRenderH;
    d.displayWidth = kDisplayW;
    d.displayHeight = kDisplayH;
    d.instanceCapacity = 64;
    d.meshCapacity = 4;
    d.lightCapacity = 16;
    d.vsmClipmap.levels = 12;
    d.vsmClipmap.firstLevelExtent = 4.f;
    d.vsmClipmap.markRadiusTexels = 2.5f;
    d.vsmClipmap.texelsPerPixel = 2.f;
    d.vsmPoolPagesX = 16;
    d.vsmPoolPagesY = 16;
    DDGIDesc& v = d.ddgiVolume;
    v.grid_origin = {-6.f, 0.5f, -9.f};
    v.probe_spacing = {3.f, 1.5f, 3.25f};
    v.grid_dims = {5u, 3u, 5u};
    v.rays_per_probe = 64;
    v.probes_per_frame = 75;
    v.irradiance_res = 6;
    v.depth_res = 8;
    v.hysteresis = 0.9f;
    v.max_ray_distance = 20.f;
    d.maxSplats = 64;
    d.maxSplatEntries = 1u << 14;
    return d;
}

FrameSettings baseSettings() {
    FrameSettings fs{};
    fs.vsmFilter.pcfRadius = 1;
    fs.ssfxSettings.ssgi_params.sample_sqrt = 2;
    // Atmosphere: default Earth medium, smaller LUTs (Lavapipe).
    fs.atmosphere.sizes.transWidth = 64;
    fs.atmosphere.sizes.transHeight = 32;
    fs.atmosphere.sizes.skyWidth = 64;
    fs.atmosphere.sizes.skyHeight = 48;
    fs.atmosphere.sizes.apWidth = 16;
    fs.atmosphere.sizes.apHeight = 16;
    fs.atmosphere.sizes.apDepth = 16;
    fs.atmosphere.sampling.transSteps = 32;
    fs.atmosphere.sampling.msDirSqrt = 4;
    fs.atmosphere.sampling.skySteps = 16;
    fs.atmosphere.ozoneAbsorption = {0.650e-6f, 1.881e-6f, 0.085e-6f};
    // Fog: small grid, a visible medium.
    fs.fogSettings.gridX = 24;
    fs.fogSettings.gridY = 16;
    fs.fogSettings.gridZ = 24;
    fs.fogSettings.farPlane = 30.f;
    fs.fogSettings.medium.density = 0.04f;
    fs.fogSettings.ambient[0] = 0.02f;
    fs.fogSettings.ambient[1] = 0.025f;
    fs.fogSettings.ambient[2] = 0.03f;
    // Clouds: small noise volumes, render-resolution reconstruction.
    clouds::CloudSettings& c = fs.cloudSettings;
    c.noise.shapeSize = 32;
    c.noise.detailSize = 16;
    c.noise.weatherSize = 32;
    c.resolution.width = kRenderW;
    c.resolution.height = kRenderH;
    c.resolution.block = 2;
    c.sampling.primarySteps = 24;
    c.sampling.lightSteps = 3;
    c.medium.coverageBias = 0.2f;
    fs.splats = true;
    return fs;
}

FrameDesc frameDesc(u64 serial, u32 frame, bool reset) {
    FrameDesc d{};
    d.serial = serial;
    d.frameIndex = frame;
    const f32 t = static_cast<f32>(frame);
    d.camera.eye[0] = -0.3f + 0.05f * t;
    d.camera.eye[1] = 1.7f;
    d.camera.eye[2] = 4.5f - 0.04f * t;
    d.camera.target[0] = 0.f;
    d.camera.target[1] = 1.0f;
    d.camera.target[2] = -3.f;
    d.camera.fovY = 1.0f;
    d.camera.nearPlane = 0.1f;
    d.camera.farPlane = 60.f;
    d.sun.slot = kSunSlot;
    std::memcpy(d.sun.toSun, kToSun, sizeof(kToSun));
    d.sun.illuminance[0] = 3.f;
    d.sun.illuminance[1] = 3.f;
    d.sun.illuminance[2] = 3.f;
    d.resetHistory = reset;
    return d;
}

// --- per-frame graph + readback ----------------------------------------------------------------------
struct CopyRecord {
    bool image = true;
    rg::TextureRef src;
    rg::BufferRef buffer;
    rg::BufferRef dst;
    u64 srcOffset = 0;
    u64 dstOffset = 0;
    u64 bytes = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const VkBuffer dst = static_cast<VkBuffer>(pc.buffer(c.dst));
    if (!c.image) {
        const VkBufferCopy region{c.srcOffset, c.dstOffset, c.bytes};
        vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.buffer)), dst, 1, &region);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.src)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

/// What a frame reads back (offsets into ctx.readback; 0 bytes = not read).
enum Readback : u32 {
    kRbDepth = 0,  ///< RT4 R32F
    kRbLit,        ///< RGBA16F render
    kRbSsfx,
    kRbSky,
    kRbFog,
    kRbSceneColor,
    kRbOutput,     ///< RGBA16F display (or render when no upscaler)
    kRbSplats,     ///< f32x4 render
    kRbBackground, ///< f32x4 render
    kRbDistance,   ///< f32 render
    kRbClouds,     ///< f32x4 render
    kRbDenoised,   ///< f32x4 render (T2)
    kRbVisibility, ///< f32 render (T2)
    kRbCount,
};

struct FrameIo {
    CopyRecord copies[kRbCount]{};
    u32 copyCount = 0;
    u64 offset[kRbCount] = {};
    u64 bytes[kRbCount] = {};
    bool readback = false;
};

void buildFrame(Context& ctx, FrameComposer& composer, rg::Graph& graph, FrameIo& io, FrameGraphOutputs& outs) {
    graph.reset();
    outs = composer.addFrame(graph);
    io.copyCount = 0;
    for (u64& b : io.bytes) {
        b = 0;
    }
    if (!io.readback) {
        return;
    }
    const rg::BufferRef rb = graph.importBuffer(
        rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr, "rp_frame.readback"});
    u64 cursor = 0;
    const u64 n = static_cast<u64>(kRenderW) * kRenderH;
    auto add = [&](Readback what, bool image, rg::TextureRef src, rg::BufferRef buffer, u64 srcOffset, u64 bytes, u32 w, u32 h) {
        if ((image && !src.valid()) || (!image && !buffer.valid())) {
            return;
        }
        CopyRecord& c = io.copies[io.copyCount++];
        c = CopyRecord{};
        c.image = image;
        c.src = src;
        c.buffer = buffer;
        c.dst = rb;
        c.srcOffset = srcOffset;
        c.dstOffset = cursor;
        c.bytes = bytes;
        c.width = w;
        c.height = h;
        io.offset[what] = cursor;
        io.bytes[what] = bytes;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
        if (image) {
            pass.use(src, rg::Access::TransferSrc);
        } else {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{srcOffset, bytes});
        }
        pass.use(rb, rg::Access::TransferDst, rg::BufferRange{c.dstOffset, bytes});
    };
    add(kRbDepth, true, outs.gbuffer.gbuffer[4], {}, 0, n * 4u, kRenderW, kRenderH);
    add(kRbLit, true, outs.lit, {}, 0, n * 8u, kRenderW, kRenderH);
    add(kRbSsfx, true, outs.ssfx, {}, 0, n * 8u, kRenderW, kRenderH);
    add(kRbSky, true, outs.sky, {}, 0, n * 8u, kRenderW, kRenderH);
    add(kRbFog, true, outs.fog, {}, 0, n * 8u, kRenderW, kRenderH);
    add(kRbSceneColor, true, outs.sceneColor, {}, 0, n * 8u, kRenderW, kRenderH);
    const u32 ow = composer.outputWidth();
    const u32 oh = composer.outputHeight();
    add(kRbOutput, true, outs.output, {}, 0, static_cast<u64>(ow) * oh * 8u, ow, oh);
    add(kRbSplats, false, {}, outs.splats, composer.splatOutputOffset(), n * 16u, 0, 0);
    add(kRbBackground, false, {}, outs.background, 0, n * 16u, 0, 0);
    add(kRbDistance, false, {}, outs.distance, 0, n * 4u, 0, 0);
    add(kRbClouds, false, {}, outs.clouds, composer.cloudsResultOffset(), n * 16u, 0, 0);
    add(kRbDenoised, false, {}, outs.denoised, 0, n * 16u, 0, 0);
    add(kRbVisibility, false, {}, outs.visibility, 0, n * 4u, 0, 0);
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

struct Frame {
    std::vector<u8> bytes[kRbCount];
    u32 ran = 0;
    u32 outW = 0;
    u32 outH = 0;
};

bool runFrame(Context& ctx, Scene& s, FrameComposer& composer, rg::Graph& graph, const FrameDesc& fd, const FrameSettings& fs,
              bool readback, Frame* frame) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    composer.beginSceneFrame(ctx.serial);
    const gpu_scene::GpuSceneCommitStats stats = s.gpu.commit();
    const bool committed = composer.commitScene();
    ctx.upload.flush();
    FrameDesc d = fd;
    d.serial = ctx.serial;
    if (!composer.beginFrame(d, fs)) {
        std::fprintf(stderr, "  FrameComposer::beginFrame failed\n");
        return false;
    }
    FrameIo io{};
    io.readback = readback;
    FrameGraphOutputs outs{};
    buildFrame(ctx, composer, graph, io, outs);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    composer.collectRetired(ctx.serial);
    s.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (frame != nullptr) {
        frame->ran = composer.stats().ran;
        frame->outW = composer.outputWidth();
        frame->outH = composer.outputHeight();
        for (u32 i = 0; i < kRbCount; ++i) {
            frame->bytes[i].clear();
            if (readback && io.bytes[i] != 0u) {
                const u8* p = static_cast<const u8*>(ctx.readback.mapped) + io.offset[i];
                frame->bytes[i].assign(p, p + io.bytes[i]);
            }
        }
    }
    if (!result.ok) {
        std::fprintf(stderr, "  graph execution failed\n");
    }
    return stats.ok && committed && result.ok && waited;
}

// --- analysis helpers --------------------------------------------------------------------------------
f32 depthAt(const Frame& f, u32 p) {
    f32 d = 0.f;
    std::memcpy(&d, f.bytes[kRbDepth].data() + p * 4u, 4u);
    return d;
}

bool pixelEqual(const std::vector<u8>& a, const std::vector<u8>& b, u32 p, u32 stride) {
    return std::memcmp(a.data() + p * stride, b.data() + p * stride, stride) == 0;
}

struct Diff {
    u32 geometryChanged = 0;
    u32 backgroundChanged = 0;
    u32 geometry = 0;
    u32 background = 0;
};

/// Pixels of `what` (RGBA16F render images) that differ between a and b, split by a's RT4 depth.
Diff diffImages(const Frame& a, const Frame& b, Readback what) {
    Diff d{};
    const u32 n = kRenderW * kRenderH;
    if (a.bytes[what].size() != n * 8u || b.bytes[what].size() != n * 8u || a.bytes[kRbDepth].size() != n * 4u) {
        d.geometryChanged = d.backgroundChanged = ~0u;
        return d;
    }
    for (u32 p = 0; p < n; ++p) {
        const bool bg = !(depthAt(a, p) < 1.f);
        const bool same = pixelEqual(a.bytes[what], b.bytes[what], p, 8u);
        if (bg) {
            ++d.background;
            d.backgroundChanged += same ? 0u : 1u;
        } else {
            ++d.geometry;
            d.geometryChanged += same ? 0u : 1u;
        }
    }
    return d;
}

bool sameBytes(const std::vector<u8>& a, const std::vector<u8>& b) { return !a.empty() && a == b; }

/// RGB of two RGBA16F images bit for bit (frame.resolve writes alpha 1; the stages keep the lit image's coverage).
bool sameRgb(const std::vector<u8>& a, const std::vector<u8>& b) {
    if (a.empty() || a.size() != b.size()) {
        return false;
    }
    for (usize p = 0; p < a.size(); p += 8u) {
        if (std::memcmp(a.data() + p, b.data() + p, 6u) != 0) {
            return false;
        }
    }
    return true;
}

bool finiteAndLit(const std::vector<u8>& img, f64& mean) {
    const usize count = img.size() / 2u;
    f64 sum = 0.0;
    for (usize i = 0; i < count; ++i) {
        u16 h = 0;
        std::memcpy(&h, img.data() + i * 2u, 2u);
        const f32 v = halfToFloat(h);
        if (!std::isfinite(v)) {
            return false;
        }
        if (i % 4u != 3u) {
            sum += v;
        }
    }
    mean = count > 0 ? sum / (static_cast<f64>(count) * 0.75) : 0.0;
    return mean > 1e-3;
}

const char* tierName(FrameTier t) { return t == FrameTier::T2 ? "t2" : "t0"; }

// --- rig: scene + composer ------------------------------------------------------------------------------
struct Rig {
    Scene scene;
    FrameComposer composer;
    rg::Graph graph;
    ~Rig() { composer.destroy(); }
};

int initRig(Context& ctx, Rig& rig, FrameTier tier) {
    if (!buildScene(ctx, rig.scene)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    if (!rig.composer.init(composerDesc(ctx, rig.scene, tier))) {
        std::printf("SKIP: FrameComposer::init: %s\n", rig.composer.reason());
        return kSkip;
    }
    if (g_language != FrameKernelLanguage::Auto &&
        std::strcmp(rig.composer.kernelLanguage(), g_language == FrameKernelLanguage::Glsl ? "glsl" : "slang") != 0) {
        std::printf("SKIP: requested kernel language not built\n");
        return kSkip;
    }
    if (tier == FrameTier::T0 &&
        !rig.composer.setSdfScene(rig.scene.sdf.data(), static_cast<u32>(rig.scene.sdf.size()), rig.scene.surfaces.data(),
                                  static_cast<u32>(rig.scene.surfaces.size()))) {
        std::fprintf(stderr, "FAIL: setSdfScene\n");
        return 1;
    }
    if (!rig.composer.setSplats(rig.scene.splats.data(), static_cast<u32>(rig.scene.splats.size()), 0u)) {
        std::fprintf(stderr, "FAIL: setSplats\n");
        return 1;
    }
    const u32 avail = rig.composer.available();
    std::printf("composer (%s kernel, tier %s): available 0x%05x (first missing: %s)\n", rig.composer.kernelLanguage(),
                tierName(tier), avail, rig.composer.reason());
    const u32 required = kStageScene | kStageVsm | kStageDdgi | kStageLighting | kStageSsfx | kStageAtmosphere | kStageSky |
                         kStageFog | kStageClouds | kStageSplats | kStageResolve | kStageTaau | kStagePost |
                         (tier == FrameTier::T2 ? (kStageTlas | kStageRtShadows | kStageDenoise | kStageFsr3) : 0u);
    expect((avail & required) == required, "every stage of the tier is available on Lavapipe");
    return 0;
}

u32 plannedStages(FrameTier tier, const FrameSettings& fs) {
    u32 p = kStageScene | kStageLighting | kStageResolve;
    if (tier == FrameTier::T2) {
        p |= kStageTlas;
        if (fs.rtShadows) {
            p |= kStageRtShadows | (fs.denoise ? kStageDenoise : 0u);
        }
    }
    p |= fs.vsm ? kStageVsm : 0u;
    p |= fs.ddgi ? kStageDdgi : 0u;
    p |= fs.ssfx ? kStageSsfx : 0u;
    p |= (fs.sky || fs.aerialPerspective) ? (kStageSky | kStageAtmosphere) : 0u;
    p |= fs.fog ? kStageFog : 0u;
    p |= fs.clouds ? (kStageClouds | kStageAtmosphere) : 0u;
    p |= fs.splats ? kStageSplats : 0u;
    p |= fs.upscaler == FrameUpscaler::Taau ? kStageTaau : (fs.upscaler == FrameUpscaler::Fsr3 ? kStageFsr3 : 0u);
    p |= fs.post ? kStagePost : 0u;
    return p;
}

// --- modes ------------------------------------------------------------------------------------------------
int runFull(Context& ctx, FrameTier tier) {
    Rig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    FrameSettings fs = baseSettings();
    for (u32 frame = 0; frame < 8u; ++frame) {
        fs.upscaler = (tier == FrameTier::T2 && frame >= 4u) ? FrameUpscaler::Fsr3 : FrameUpscaler::Taau;
        Frame out{};
        const bool ok = runFrame(ctx, rig.scene, rig.composer, rig.graph, frameDesc(0, frame, frame == 0u || frame == 4u), fs,
                                 frame == 3u || frame == 7u, &out);
        expect(ok, "frame executes");
        const u32 planned = plannedStages(tier, fs);
        if (out.ran != planned) {
            std::fprintf(stderr, "  frame %u: ran 0x%05x, planned 0x%05x\n", frame, out.ran, planned);
        }
        expect(out.ran == planned, "every planned stage ran");
        if (frame == 3u || frame == 7u) {
            f64 mean = 0.0;
            expect(out.outW == kDisplayW && out.outH == kDisplayH, "output at display resolution");
            expect(finiteAndLit(out.bytes[kRbOutput], mean), "output finite and not black");
            f64 sceneMean = 0.0;
            expect(finiteAndLit(out.bytes[kRbSceneColor], sceneMean), "scene colour finite and not black");
            std::printf("  frame %u (%s): output mean %.4f, scene colour mean %.4f, %u composer passes\n", frame,
                        fs.upscaler == FrameUpscaler::Fsr3 ? "fsr3" : "taau", mean, sceneMean, rig.composer.stats().composerPasses);
        }
    }
    std::printf("full %s: 8 frames, validation messages %u\n", tierName(tier), g_messages);
    return 0;
}

struct Toggle {
    const char* name;
    FrameSettings settings;
};

int runToggles(Context& ctx, FrameTier tier) {
    Rig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    FrameSettings base = baseSettings();
    base.upscaler = FrameUpscaler::None;
    base.post = false;
    base.clouds = false;
    base.splats = false;
    // FroxelFog keeps its own jitter sequence index (not reset by FogFrameDesc::resetHistory): unjittered fog makes
    // a reset frame a function of the settings alone.
    base.fogSettings.jitter = false;
    auto run = [&](const FrameSettings& fs, Frame& out) {
        const bool ok = runFrame(ctx, rig.scene, rig.composer, rig.graph, frameDesc(0, 2u, true), fs, true, &out);
        expect(ok, "toggle frame executes");
        expect(out.ran == plannedStages(tier, fs), "toggle frame ran its planned stages");
    };
    auto report = [&](const char* what, const Diff& d) {
        std::printf("  %-34s geometry %5u / %5u changed, background %5u / %5u changed\n", what, d.geometryChanged, d.geometry,
                    d.backgroundChanged, d.background);
    };
    // Reference frame (fog, sky, aerial, SSFX, VSM, DDGI on; clouds / splats off) and its repeat.
    Frame ref{}, again{};
    run(base, ref);
    run(base, again);
    expect(sameBytes(ref.bytes[kRbSceneColor], again.bytes[kRbSceneColor]), "a repeated reset frame reproduces its bits");
    for (const Readback r : {kRbDepth, kRbLit, kRbSsfx, kRbSky, kRbFog, kRbSceneColor}) {
        if (!sameBytes(ref.bytes[r], again.bytes[r])) {
            const Diff x = diffImages(ref, again, r == kRbDepth ? kRbLit : r);
            std::printf("  repeat differs at readback %u (geometry %u, background %u changed)\n", static_cast<u32>(r), x.geometryChanged,
                        x.backgroundChanged);
        }
    }
    Diff d = diffImages(ref, ref, kRbSceneColor);
    expect(d.geometry > 0u && d.background > 0u, "the view shows geometry and background");
    std::printf("toggles %s: %u geometry / %u background pixels\n", tierName(tier), d.geometry, d.background);

    // fog
    {
        FrameSettings fs = base;
        fs.fog = false;
        Frame off{};
        run(fs, off);
        expect(sameRgb(off.bytes[kRbSceneColor], ref.bytes[kRbSky]), "fog off == the fog-on frame's pre-fog image");
        d = diffImages(ref, ref, kRbSceneColor);
        const Diff fog = diffImages(ref, off, kRbSceneColor);
        report("fog on vs off", fog);
        expect(fog.geometryChanged > fog.geometry / 2u, "fog changes the geometry pixels");
    }
    // sky (aerial and fog off)
    {
        FrameSettings on = base;
        on.fog = false;
        on.aerialPerspective = false;
        FrameSettings off = on;
        off.sky = false;
        Frame a{}, b{};
        run(on, a);
        run(off, b);
        const Diff sky = diffImages(a, b, kRbSceneColor);
        report("sky on vs off", sky);
        expect(sky.geometryChanged == 0u, "sky on changes no geometry pixel");
        expect(sky.backgroundChanged == sky.background, "sky on writes every background pixel");
        expect(sameRgb(b.bytes[kRbSceneColor], b.bytes[kRbSsfx]), "sky / aerial / fog off: scene colour == the SSFX image");
        // aerial perspective (sky on, fog off)
        FrameSettings aerial = on;
        aerial.aerialPerspective = true;
        Frame c{};
        run(aerial, c);
        const Diff ap = diffImages(c, a, kRbSceneColor);
        report("aerial perspective on vs off", ap);
        expect(ap.backgroundChanged == 0u, "aerial perspective changes no background pixel");
        expect(ap.geometryChanged > 0u, "aerial perspective changes geometry pixels");
        // clouds (sky on, aerial off, fog off)
        FrameSettings cl = on;
        cl.clouds = true;
        Frame e{};
        run(cl, e);
        const Diff clouds = diffImages(e, a, kRbSceneColor);
        report("clouds on vs off", clouds);
        expect(clouds.geometryChanged == 0u, "clouds change no geometry pixel");
        expect(clouds.backgroundChanged > 0u, "clouds change background pixels");
        // splats (sky on, aerial off, fog off)
        FrameSettings sp = on;
        sp.splats = true;
        Frame g{};
        run(sp, g);
        u32 covered = 0, changed = 0, changedOutside = 0;
        const u32 n = kRenderW * kRenderH;
        if (g.bytes[kRbSplats].size() == n * 16u) {
            for (u32 p = 0; p < n; ++p) {
                f32 t = 1.f;
                std::memcpy(&t, g.bytes[kRbSplats].data() + p * 16u + 12u, 4u);
                const bool cov = t < 1.f;
                const bool diff = !pixelEqual(g.bytes[kRbSceneColor], a.bytes[kRbSceneColor], p, 8u);
                covered += cov ? 1u : 0u;
                changed += diff ? 1u : 0u;
                changedOutside += (diff && !cov) ? 1u : 0u;
            }
        }
        std::printf("  %-34s %u pixels covered, %u changed, %u changed outside the splats\n", "splats on vs off", covered, changed,
                    changedOutside);
        expect(covered > 0u && changed > 0u && changedOutside == 0u, "splats change only the pixels they cover");
    }
    // screen-space effects (sky / aerial / fog off: scene colour == SSFX output, or the lit image)
    FrameSettings plain = base;
    plain.fog = false;
    plain.sky = false;
    plain.aerialPerspective = false;
    Frame p0{};
    run(plain, p0);
    {
        FrameSettings fs = plain;
        fs.ssfx = false;
        Frame off{};
        run(fs, off);
        expect(sameRgb(off.bytes[kRbSceneColor], p0.bytes[kRbLit]), "SSFX off: scene colour == the lit image");
        const Diff sx = diffImages(p0, off, kRbSceneColor);
        report("SSFX on vs off", sx);
        expect(sx.backgroundChanged == 0u && sx.geometryChanged > 0u, "SSFX changes geometry pixels only");
    }
    auto geometryOnly = [&](const char* name, FrameSettings fs) {
        Frame off{};
        run(fs, off);
        const Diff x = diffImages(p0, off, kRbSceneColor);
        report(name, x);
        expect(x.backgroundChanged == 0u, "the toggle changes no background pixel");
        expect(x.geometryChanged > 0u, "the toggle changes geometry pixels");
    };
    if (tier == FrameTier::T0) {
        FrameSettings fs = plain;
        fs.vsm = false;
        geometryOnly("VSM on vs off", fs);
    } else {
        // T2: the sun (the only VSM-shadowed light) takes its visibility from the RT shadows, so the VSM toggle
        // is measured with RT shadows off (and then must not change anything with them on).
        FrameSettings on = plain;
        on.rtShadows = false;
        FrameSettings off = on;
        off.vsm = false;
        Frame a{}, b{};
        run(on, a);
        run(off, b);
        const Diff x = diffImages(a, b, kRbSceneColor);
        report("VSM on vs off (RT shadows off)", x);
        expect(x.backgroundChanged == 0u && x.geometryChanged > 0u, "VSM changes geometry pixels only");
        FrameSettings rtOnly = plain;
        rtOnly.vsm = false;
        Frame c{};
        run(rtOnly, c);
        const Diff y = diffImages(p0, c, kRbSceneColor);
        report("VSM on vs off (RT shadows on)", y);
        expect(y.backgroundChanged == 0u && y.geometryChanged == 0u, "RT-shadowed sun: the VSM toggle changes nothing");
    }
    {
        FrameSettings fs = plain;
        fs.ddgi = false;
        geometryOnly("DDGI on vs off", fs);
    }
    if (tier == FrameTier::T2) {
        FrameSettings fs = plain;
        fs.rtShadows = false;
        geometryOnly("RT shadows on vs off", fs);
        fs = plain;
        fs.denoise = false;
        geometryOnly("denoiser on vs off", fs);
    }
    std::printf("toggles %s: validation messages %u\n", tierName(tier), g_messages);
    return 0;
}

bool runSequence(Context& ctx, FrameTier tier, u32 frames, Frame& last) {
    Rig rig;
    if (initRig(ctx, rig, tier) != 0) {
        return false;
    }
    FrameSettings fs = baseSettings();
    fs.upscaler = tier == FrameTier::T2 ? FrameUpscaler::Fsr3 : FrameUpscaler::Taau;
    bool ok = true;
    for (u32 frame = 0; frame < frames; ++frame) {
        ok = runFrame(ctx, rig.scene, rig.composer, rig.graph, frameDesc(0, frame, frame == 0u), fs, frame + 1u == frames, &last) && ok;
    }
    return ok;
}

int runDeterminism(Context& ctx, FrameTier tier) {
    Frame a{}, b{};
    const bool okA = runSequence(ctx, tier, 6u, a);
    const bool okB = runSequence(ctx, tier, 6u, b);
    expect(okA && okB, "both runs execute");
    const bool output = sameBytes(a.bytes[kRbOutput], b.bytes[kRbOutput]);
    const bool scene = sameBytes(a.bytes[kRbSceneColor], b.bytes[kRbSceneColor]);
    std::printf("determinism %s: output %s, scene colour %s (%zu / %zu bytes)\n", tierName(tier), output ? "identical" : "DIFFERS",
                scene ? "identical" : "DIFFERS", a.bytes[kRbOutput].size(), a.bytes[kRbSceneColor].size());
    expect(output && scene, "two runs are bit-identical");
    return 0;
}

thread_local bool t_inPass = false;
void hookBegin(const rg::PassContext&, const char*, void*) {
    t_inPass = true;
    t_count = true;
}
void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, FrameTier tier, bool count) {
    Rig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    FrameSettings fs = baseSettings();
    fs.upscaler = tier == FrameTier::T2 ? FrameUpscaler::Fsr3 : FrameUpscaler::Taau;
    constexpr u32 kWarmup = 8;
    const u32 total = count ? kWarmup + 64u : 12u;
    unsigned long long begin = 0, build = 0, callbacks = 0;
    if (count) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 imageRebuilds = 0, sampledBinds = 0;
    for (u32 frame = 0; frame < total; ++frame) {
        const bool measure = count && frame >= kWarmup;
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        rig.scene.gpu.beginFrame(ctx.serial);
        rig.composer.beginSceneFrame(ctx.serial);
        const bool committed = rig.scene.gpu.commit().ok && rig.composer.commitScene();
        ctx.upload.flush();
        const FrameDesc fd = frameDesc(ctx.serial, frame, frame == 0u);
        t_allocations = 0;
        t_count = measure;
        const bool begun = rig.composer.beginFrame(fd, fs);
        t_count = false;
        const unsigned long long b = t_allocations;
        t_allocations = 0;
        t_count = measure;
        rig.graph.reset();
        (void)rig.composer.addFrame(rig.graph);
        t_count = false;
        const unsigned long long g = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(rig.graph);
        const unsigned long long c = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rig.composer.collectRetired(ctx.serial);
        rig.scene.gpu.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(committed && begun && result.ok && waited, "zero_alloc frame ok");
        expect(rig.composer.stats().ran == plannedStages(tier, fs), "every stage runs");
        if (measure) {
            begin += b;
            build += g;
            callbacks += c;
        }
        if (frame + 1u == kWarmup) {
            imageRebuilds = rig.composer.stats().imageRebuilds;
            sampledBinds = rig.composer.stats().sampledBinds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (count) {
        std::printf("zero_alloc %s (validation layer off: it is C++ and allocates through operator new):\n"
                    "  64 steady-state frames, every stage (%s upscaler), camera moving\n"
                    "  FrameComposer::beginFrame: %llu operator-new calls\n"
                    "  whole graph build (reset + addFrame, every package's imports and passes): %llu\n"
                    "  pass callbacks (every pass of the frame): %llu\n",
                    tierName(tier), tier == FrameTier::T2 ? "fsr3" : "taau", begin, build, callbacks);
        expect(begin == 0u, "FrameComposer::beginFrame makes no steady-state heap allocations");
        expect(build == 0u, "the composed graph build makes no steady-state heap allocations");
        expect(callbacks == 0u, "the composed frame's pass callbacks make no steady-state heap allocations");
        expect(rig.composer.stats().imageRebuilds == imageRebuilds && rig.composer.stats().sampledBinds == sampledBinds,
               "no composer resource / sampled-slot rebuild in steady state");
    } else {
        std::printf("zero_alloc %s: %u validated frames ok, validation messages %u\n", tierName(tier), total, g_messages);
    }
    return 0;
}

f32 toUnorm(f32 v) { return std::min(1.f, std::max(0.f, v)); }

int runGolden(Context& ctx, FrameTier tier) {
    Frame last{};
    if (!runSequence(ctx, tier, 4u, last)) {
        expect(false, "golden sequence executes");
        return 0;
    }
    if (last.bytes[kRbOutput].size() != static_cast<usize>(last.outW) * last.outH * 8u) {
        expect(false, "output read back");
        return 0;
    }
    harness::ImageRgba8 image(last.outW, last.outH);
    for (u32 y = 0; y < last.outH; ++y) {
        for (u32 x = 0; x < last.outW; ++x) {
            const usize p = static_cast<usize>(y) * last.outW + x;
            u8* dst = image.at(x, y);
            for (u32 k = 0; k < 3u; ++k) {
                u16 h = 0;
                std::memcpy(&h, last.bytes[kRbOutput].data() + p * 8u + k * 2u, 2u);
                dst[k] = static_cast<u8>(std::lround(toUnorm(halfToFloat(h)) * 255.f));
            }
            dst[3] = 255u;
        }
    }
    harness::GoldenSpec spec{};
    spec.metric = harness::GoldenMetric::Flip;
    spec.threshold = 0.02;
    spec.psnrFallbackDb = 38.0;
    spec.pixelTolerance = 4;
    spec.maxDifferingPixels = last.outW * last.outH / 100u;
    harness::GoldenStore store(FUSE_RP_GOLDEN_DIR, FUSE_RP_HARNESS_ARTIFACT_DIR);
    const std::string name = std::string("frame_composer_") + tierName(tier);
    const harness::GoldenResult r = store.check(name, image, spec);
    std::printf("golden %s: %s (%s %.4f, threshold %.4f, PSNR %.2f dB, %u pixels over %u / 255, max channel diff %u)%s\n",
                name.c_str(), r.passed ? "pass" : "FAIL", harness::goldenMetricName(r.metricUsed), r.score, r.threshold, r.psnr,
                r.differingPixels, spec.pixelTolerance, r.maxChannelDiff, r.updated ? " [golden updated]" : "");
    if (!r.message.empty()) {
        std::printf("  %s\n", r.message.c_str());
    }
    store.printSummary();
    expect(r.passed || r.updated, "golden image within tolerance");
    return 0;
}

int runParity(Context& ctx, FrameTier tier) {
    Rig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    FrameSettings fs = baseSettings();
    fs.upscaler = FrameUpscaler::None;
    fs.post = false;
    Frame f{};
    for (u32 frame = 0; frame < 2u; ++frame) {
        expect(runFrame(ctx, rig.scene, rig.composer, rig.graph, frameDesc(0, frame, frame == 0u), fs, frame == 1u, &f),
               "parity frame executes");
    }
    const FrameConstants& c = rig.composer.constants();
    const u32 n = kRenderW * kRenderH;
    if (f.bytes[kRbFog].size() != n * 8u || f.bytes[kRbBackground].size() != n * 16u || f.bytes[kRbDistance].size() != n * 4u ||
        f.bytes[kRbClouds].size() != n * 16u || f.bytes[kRbSplats].size() != n * 16u || f.bytes[kRbSceneColor].size() != n * 8u) {
        expect(false, "parity inputs read back");
        return 0;
    }
    u32 bgBad = 0, distBad = 0, resolveBad = 0;
    f64 worstDist = 0.0;
    for (u32 y = 0; y < kRenderH; ++y) {
        for (u32 x = 0; x < kRenderW; ++x) {
            const u32 p = y * kRenderW + x;
            f32 bg[4];
            std::memcpy(bg, f.bytes[kRbBackground].data() + p * 16u, 16u);
            for (u32 k = 0; k < 4u; ++k) {
                u16 h = 0;
                std::memcpy(&h, f.bytes[kRbFog].data() + p * 8u + k * 2u, 2u);
                bgBad += bg[k] == halfToFloat(h) ? 0u : 1u;
            }
            f32 dist = 0.f;
            std::memcpy(&dist, f.bytes[kRbDistance].data() + p * 4u, 4u);
            const f32 ref = frame_view_distance(c, x, y, depthAt(f, p));
            const f64 rel = std::fabs(static_cast<f64>(dist) - ref) / std::max(1e-6, static_cast<f64>(ref));
            worstDist = std::max(worstDist, rel);
            distBad += rel <= 1e-5 ? 0u : 1u;
            f32 base[4], splat[4], out[4];
            std::memcpy(base, f.bytes[kRbClouds].data() + p * 16u, 16u);
            std::memcpy(splat, f.bytes[kRbSplats].data() + p * 16u, 16u);
            frame_resolve_texel(c, base, splat, out);
            for (u32 k = 0; k < 4u; ++k) {
                u16 h = 0;
                std::memcpy(&h, f.bytes[kRbSceneColor].data() + p * 8u + k * 2u, 2u);
                const f32 g = halfToFloat(h);
                const f32 ulp = std::max(std::fabs(out[k]) * (1.f / 1024.f), 6.1e-5f);
                resolveBad += std::fabs(g - out[k]) <= ulp ? 0u : 1u;
            }
        }
    }
    u32 packBad = 0;
    u32 packChecked = 0;
    if (tier == FrameTier::T2) {
        if (f.bytes[kRbDenoised].size() != n * 16u || f.bytes[kRbVisibility].size() != n * 4u) {
            expect(false, "shadow pack inputs read back");
        } else {
            for (u32 p = 0; p < n; ++p) {
                f32 dn[4], v = 0.f;
                std::memcpy(dn, f.bytes[kRbDenoised].data() + p * 16u, 16u);
                std::memcpy(&v, f.bytes[kRbVisibility].data() + p * 4u, 4u);
                packBad += v == frame_pack_visibility(dn) ? 0u : 1u;
                ++packChecked;
            }
        }
    }
    std::printf("parity %s: shadow pack %u / %u bad\n", tierName(tier), packBad, packChecked);
    expect(packBad == 0u, "frame.shadow_pack == frame_pack_visibility (bit for bit)");
    std::printf("parity %s (%s kernel): gather background %u bad channels, distance %u bad (worst rel %.3g), resolve %u bad "
                "channels\n",
                tierName(tier), rig.composer.kernelLanguage(), bgBad, distBad, worstDist, resolveBad);
    expect(bgBad == 0u, "frame.gather background == its input image");
    expect(distBad == 0u, "frame.gather distance == frame_view_distance (1e-5 relative)");
    expect(resolveBad == 0u, "frame.resolve == half(frame_resolve_texel) (<= 1 half ulp)");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "full";
    std::string tierArg = "t0";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--tier") == 0) {
            tierArg = argv[i + 1];
        } else if (std::strcmp(argv[i], "--language") == 0) {
            g_language = std::strcmp(argv[i + 1], "glsl") == 0    ? FrameKernelLanguage::Glsl
                         : std::strcmp(argv[i + 1], "slang") == 0 ? FrameKernelLanguage::Slang
                                                                  : FrameKernelLanguage::Auto;
        }
    }
    const FrameTier tier = tierArg == "t2" ? FrameTier::T2 : FrameTier::T0;
    int rc = 0;
    {
        Context ctx;
        rc = setup(ctx, true, tier == FrameTier::T2);
        if (rc != 0) {
            return rc;
        }
        if (mode == "full") {
            rc = runFull(ctx, tier);
        } else if (mode == "toggles") {
            rc = runToggles(ctx, tier);
        } else if (mode == "determinism") {
            rc = runDeterminism(ctx, tier);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, tier, false);
        } else if (mode == "golden") {
            rc = runGolden(ctx, tier);
        } else if (mode == "parity") {
            rc = runParity(ctx, tier);
        } else {
            std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
            return 2;
        }
    }
    if (rc == 0 && mode == "zero_alloc") {
        Context ctx;
        rc = setup(ctx, false, tier == FrameTier::T2);
        if (rc == 0) {
            rc = runZeroAlloc(ctx, tier, true);
        }
    }
    if (rc != 0) {
        return rc;
    }
    expect(g_messages == 0u, "0 validation / synchronization-validation messages");
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s), %u validation messages\n", g_failures, g_messages);
        return 1;
    }
    std::printf("ok (%s, %s)\n", mode.c_str(), tierName(tier));
    return 0;
}

#endif
