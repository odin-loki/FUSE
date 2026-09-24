// WP-6.2 ray-traced shadows and reflections, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_rt_effects_cpu.cpp.
//
// Scene: ground plane, 7 boxes (a floating slab, a back wall, a shadow-only caster, a visible non-caster, an
// emissive box) and a mirror sphere as WP-1.1 GPU-scene meshlet meshes; a sun, a point, a spot, a rectangle and a
// disk light. Every frame is one render graph (WP-0.3): gpu_scene.* -> rt.* (WP-6.0 BLAS / TLAS) -> cull.* /
// vis.* (WP-1.3 / 1.4) -> resolve.* (WP-1.5 G-buffer) -> rt.shadows + rt.reflections (both kernel languages)
// [-> light.* with the RT visibility] -> readback. The CPU side traces with the WP-6.0 reference
// (rt::RtReferenceScene, two-level fuse::spatial::BVH, f64 triangles) through RtEffectsReference.
//
//   --mode hard        punctual sun + point + spot, 1 ray each, and single-sample reflections (mirror sphere,
//                      glossy / rough surfaces) over 3 frames with camera and object motion (TLAS refits).
//                      Per ray (the kernels dump the exact ray + committed hit of every (light, pixel)):
//                        * ray generation == the CPU mirror from the read-back G-buffer texels
//                          (rt_effects_kernel.hpp, same Owen-Sobol (u, v) bit for bit)
//                        * robust rays (rt::RtReferenceScene::traceClassified, 1e-4 barycentric band): hit / miss and
//                          (instance, primitive) == the CPU BVH, so visibility == the CPU reference EXACTLY
//                        * hit distance output == the dumped t, and |t - t_cpu| <= rt::rtTTolerance(t)
//                        * reflection radiance == RtEffectsReference::shadeHit at the GPU's hit (1e-3 relative)
//                        * Slang == GLSL
//   --mode converge    area lights (rectangle, disk) + a sun disk, 4 samples each, and 2-sample glossy
//                      reflections, static view at 160 x 120, 64 frames accumulated on the host (256 shadow / 128
//                      reflection samples per pixel) == the offline reference (RtEffectsReference: stratified f64
//                      sampling, 1024 shadow / 576 reflection samples) within a statistical tolerance (kZ below); the mean
//                      occluder / hit distances agree; the error of one frame is several times the converged one
//   --mode shade       light.shade with LightingFrameDesc::rtShadows == the CPU shade kernel with the RT visibility
//                      (ShadeReferenceDesc::rtShadow over the read-back visibility), WP-2.1 tolerance
//   --mode zero_alloc  64 steady-state frames (moving box, moving camera): 0 operator-new calls in
//                      RtEffects::beginFrame, the rt.* pass callbacks and the whole graph build
//   --mode caps_gate   a device capped below T2 (VulkanDeviceDesc::maxTier = T1): RtEffects::init fails with the
//                      reason; uncapped: succeeds
//
// Exit 77 = skip (stub build, no ICD / validation layer, device below T2, no kernel built).
#include "test_rp_vsm_raster_common.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/renderer/rt_effects/rt_effects.hpp>
#include <fuse/renderer/rt_effects/rt_effects_kernel.hpp>
#include <fuse/renderer/rt_effects/rt_effects_reference.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>
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
#include <thread>
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

using namespace vsmr_test;
using namespace fuse::renderer;
using namespace fuse::renderer::culling;
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::lighting_gpu;
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::rt;
using namespace fuse::renderer::rt_effects;
using namespace fuse::renderer::visbuffer;
using fuse::u16;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
using V3 = RtfxVec3<f64>;
using F3 = RtfxVec3<f32>;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kLanguages = 2; // Slang, GLSL
constexpr f64 kEdgeEpsilon = 1.0e-4;
constexpr f32 kAmbient[3] = {0.03f, 0.035f, 0.045f};
constexpr f32 kSky[3] = {0.35f, 0.5f, 0.8f};
// Shade tolerance: the WP-2.1 gate's (GPU f32 shade vs its CPU kernel), relative to the unshadowed radiance (see
// runShade); the RT visibility is read back and fed to the CPU kernel bit for bit, so it adds nothing.
constexpr f64 kTolRel = 1e-4;
constexpr f64 kTolAbs = 1e-6;

enum LightSlot : u32 { kSun = 0, kPoint = 1, kSpot = 2, kRect = 3, kDisk = 4 };

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

f32 halfAt(const u8* p, usize i) {
    u16 h = 0;
    std::memcpy(&h, p + i * 2u, 2u);
    return GBufferQuantize::halfToFloat(h);
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
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    Buffer shadowDump[kLanguages]{};
    Buffer reflDump[kLanguages]{};
    Buffer shadeDump[kLanguages]{};
    Buffer plainDump[kLanguages]{};
    BindlessSlotHandle sampler{};
    u32 samplerHandle = 0;
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
            for (u32 k = 0; k < kLanguages; ++k) {
                allocator->destroyBuffer(shadowDump[k]);
                allocator->destroyBuffer(reflDump[k]);
                allocator->destroyBuffer(shadeDump[k]);
                allocator->destroyBuffer(plainDump[k]);
            }
        }
        if (device != nullptr) {
            if (sampler.isValid()) {
                bindless.releaseSampler(sampler);
            }
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

bool hostBuffer(Context& ctx, Buffer& b, usize bytes, MemoryUsage memory, const char* name) {
    if (b.handle != nullptr && b.desc.size >= bytes) {
        return true;
    }
    if (b.handle != nullptr) {
        ctx.allocator->destroyBuffer(b);
    }
    BufferDesc d{};
    d.size = bytes;
    d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                       static_cast<u32>(BufferUsage::TransferDst));
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr && b.deviceAddress != 0u;
}

/// 0 ok, kSkip, or 1. `maxTier`: caps_gate.
int setup(Context& ctx, bool validation, RenderTier maxTier = kMaxRenderTier, bool requireRt = true) {
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
    instanceDesc.appName = "fuse_rp_rt_effects";
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
    VulkanDeviceDesc deviceDesc{};
    deviceDesc.maxTier = maxTier;
    ctx.device = VulkanDevice::create(*ctx.instance, deviceDesc);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    const RtCapabilities caps = queryRtCapabilities(ctx.device.get());
    if (requireRt && !caps.usable) {
        std::printf("SKIP: T2 gate: %s\n", caps.reason);
        return kSkip;
    }
    if (requireRt) {
        const VisCapabilities visCaps = queryVisCapabilities(ctx.device.get());
        const ResolveCapabilities resolveCaps = queryResolveCapabilities(ctx.device.get());
        const LightingCapabilities lightingCaps = queryLightingCapabilities(ctx.device.get());
        if (!visCaps.raster || !resolveCaps.resolve || !lightingCaps.lighting) {
            std::printf("SKIP: capability missing (vis %s, resolve %s, lighting %s)\n", visCaps.rasterReason, resolveCaps.reason,
                        lightingCaps.reason);
            return kSkip;
        }
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s; %s\n", ctx.device->info().deviceName.c_str(), ctx.device->info().caps.summary().c_str());
    if (!ctx.bindless.init(*ctx.device, BindlessDesc{})) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_rt_effects.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    SamplerDesc sd{};
    sd.name = "rp_rt_effects.sampler";
    ctx.sampler = ctx.bindless.acquireSampler(sd);
    if (!ctx.sampler.isValid()) {
        std::fprintf(stderr, "FAIL: sampler\n");
        return 1;
    }
    ctx.samplerHandle = ctx.bindless.shaderHandle(ctx.sampler);
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

// --- scene ----------------------------------------------------------------------------------------
struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<Material::GPUMaterial> materials;
    std::vector<InstanceHandle> handles;
    std::vector<Box> boxes;
    RtReferenceScene bvh;
};

enum MaterialRow : u32 { kMatGround = 0, kMatGrey = 2, kMatMirror = 4, kMatGlossy = 6, kMatEmissive = 8, kMatCount = 10 };

Material::GPUMaterial material(f32 r, f32 g, f32 b, f32 metallic, f32 roughness) {
    Material::GPUMaterial m{};
    m.baseColor = {r, g, b, metallic};
    m.roughnessEmissive = {roughness, 0.f, 0.f, 0.f};
    return m;
}

GpuTransform sphereTransform(f32 x, f32 y, f32 z, f32 r) {
    GpuTransform t{};
    t.rows[0][0] = r;
    t.rows[1][1] = r;
    t.rows[2][2] = r;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

/// `soft`: the sun gets an angular radius (converge); the material roughness set: rough (converge) or mixed.
bool buildScene(Context& ctx, Scene& s, bool soft) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 64;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    if (!buildMeshes(s.meshes, 40.f)) {
        return false;
    }
    s.meshes.emplace_back();
    if (!mr_test::build(mr_test::uvSphere(16, 24, 1.f), s.meshes.back())) {
        return false;
    }
    for (u32 i = 0; i < s.meshes.size(); ++i) {
        if (s.gpu.addMeshletMesh(s.meshes[i]) != i || !s.bvh.setMeshFromScene(s.gpu, i, s.meshes[i].positions.data())) {
            return false;
        }
    }
    // Rows in pairs: the box's second submesh reads row + 1 in the G-buffer (WP-1.5); hits read the base row.
    s.materials.resize(kMatCount);
    s.materials[kMatGround] = material(0.7f, 0.68f, 0.62f, 0.f, soft ? 0.35f : 0.45f);
    s.materials[kMatGrey] = material(0.55f, 0.6f, 0.65f, 0.f, 0.6f);
    s.materials[kMatMirror] = material(0.95f, 0.93f, 0.9f, 1.f, soft ? 0.12f : 0.f);
    s.materials[kMatGlossy] = material(0.8f, 0.3f, 0.2f, 0.f, 0.2f);
    s.materials[kMatEmissive] = material(0.2f, 0.2f, 0.2f, 0.f, 0.5f);
    s.materials[kMatEmissive].roughnessEmissive = {0.5f, 1.f, 0.8f, 0.3f};
    s.materials[kMatEmissive].emissiveIntensity = 3.f;
    for (u32 i = 0; i < kMatCount; i += 2u) {
        s.materials[i + 1u] = s.materials[i];
    }
    for (u32 i = 0; i < kMatCount; ++i) {
        s.gpu.setMaterial(i, s.materials[i]);
    }
    InstanceDesc ground{};
    ground.mesh = 0;
    ground.material = kMatGround;
    s.handles.push_back(s.gpu.addInstance(ground));
    struct B {
        Box box;
        u32 material;
        u32 flags;
    };
    const u32 normal = kInstanceVisible | kInstanceCastShadow | kInstanceReceiveShadow;
    const B boxes[7] = {
        {{{-3.0, 0.0, -8.0}, {-1.8, 1.6, -6.8}}, kMatGrey, normal},
        {{{0.5, 0.0, -10.0}, {1.7, 2.2, -8.8}}, kMatGlossy, normal},
        {{{2.2, 0.0, -6.5}, {2.8, 0.8, -5.9}}, kMatEmissive, normal},
        {{{-1.0, 1.8, -6.0}, {0.4, 2.0, -4.6}}, kMatGrey, normal},        // floating slab
        {{{-4.5, 0.0, -12.0}, {4.5, 3.0, -11.6}}, kMatGlossy, normal},    // back wall
        {{{1.2, 1.2, -4.5}, {1.8, 1.8, -3.9}}, kMatGrey, kInstanceCastShadow}, // shadow-only caster
        {{{-3.5, 0.0, -4.5}, {-3.0, 0.5, -4.0}}, kMatGrey, kInstanceVisible},  // visible non-caster
    };
    for (const B& b : boxes) {
        InstanceDesc id{};
        id.mesh = 1;
        id.material = b.material;
        id.flags = b.flags;
        id.transform = World::boxTransform(b.box);
        s.handles.push_back(s.gpu.addInstance(id));
        s.boxes.push_back(b.box);
    }
    InstanceDesc sphere{};
    sphere.mesh = 2;
    sphere.material = kMatMirror;
    sphere.transform = sphereTransform(-0.6f, 1.1f, -8.4f, 1.1f);
    s.handles.push_back(s.gpu.addInstance(sphere));

    GpuLight sun{};
    sun.type = static_cast<u32>(GpuLightType::Directional);
    const f32 sd[3] = {0.35f, -1.f, -0.45f};
    const f32 sl = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
    for (u32 c = 0; c < 3u; ++c) {
        sun.direction[c] = sd[c] / sl;
    }
    sun.intensity = 2.5f;
    if (soft) {
        ltc::setSunAngularRadius(sun, 0.1f);
    }
    GpuLight point{};
    point.type = static_cast<u32>(GpuLightType::Point);
    point.position[0] = -2.f;
    point.position[1] = 3.f;
    point.position[2] = -4.f;
    point.range = 12.f;
    point.intensity = 12.f;
    GpuLight spot{};
    spot.type = static_cast<u32>(GpuLightType::Spot);
    spot.position[0] = 2.f;
    spot.position[1] = 4.5f;
    spot.position[2] = -7.f;
    spot.direction[0] = -0.2f;
    spot.direction[1] = -1.f;
    spot.direction[2] = 0.05f;
    spot.range = 10.f;
    spot.cosInner = 0.92f;
    spot.cosOuter = 0.8f;
    spot.intensity = 25.f;
    ltc::AreaLightDesc rect{};
    rect.center = {-0.5f, 4.2f, -6.5f};
    rect.normal = {0.f, -1.f, 0.f};
    rect.tangent = {1.f, 0.f, 0.2f};
    rect.halfWidth = 1.1f;
    rect.halfHeight = 0.5f;
    rect.intensity = 6.f;
    rect.range = 14.f;
    ltc::AreaLightDesc disk{};
    disk.center = {1.8f, 3.4f, -4.5f};
    disk.normal = {-0.3f, -1.f, 0.1f};
    disk.halfWidth = 0.6f;
    disk.halfHeight = 0.6f;
    disk.intensity = 6.f;
    disk.range = 12.f;
    for (const GpuLight& l : {sun, point, spot, ltc::makeRectLight(rect), ltc::makeDiskLight(disk)}) {
        s.gpu.addLight(l);
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    s.bvh.setInstances(s.gpu);
    return stats.ok && ctx.upload.waitAll();
}

Camera makeCamera(u32 width, u32 height, u32 frame) {
    Camera c;
    const f64 t = frame;
    c.eye = {0.3 + 0.15 * t, 3.2 - 0.05 * t, 3.0 - 0.2 * t};
    c.at = {0.1 * t, 0.6, -8.0};
    c.width = width;
    c.height = height;
    c.zNear = 0.2f;
    c.zFar = 80.f;
    c.build();
    return c;
}

ClusterCameraDesc clusterCamera(const Camera& c) {
    ClusterCameraDesc d{};
    d.position = {static_cast<f32>(c.eye.x), static_cast<f32>(c.eye.y), static_cast<f32>(c.eye.z)};
    d.forward = {static_cast<f32>(c.at.x - c.eye.x), static_cast<f32>(c.at.y - c.eye.y), static_cast<f32>(c.at.z - c.eye.z)};
    d.up = {0.f, 1.f, 0.f};
    d.nearPlane = c.zNear;
    d.farPlane = c.zFar;
    d.screenWidth = c.width;
    d.screenHeight = c.height;
    d.fovYRadians = c.fovY;
    d.reversedZ = false;
    return d;
}

void moveBox(Scene& s, u32 box, f64 dx) {
    Box b = s.boxes[box];
    b.lo.x += dx;
    b.hi.x += dx;
    s.boxes[box] = b;
    s.gpu.setTransform(s.handles[1u + box], World::boxTransform(b));
}

// --- rig ------------------------------------------------------------------------------------------
struct Rig {
    u32 width = 0;
    u32 height = 0;
    bool shade = false;
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    AccelerationStructures as;
    RtEffects fx[kLanguages];
    ClusteredLighting lighting[kLanguages];
    ClusteredLighting plain[kLanguages]; ///< shade mode: the same shade without RT shadows (baseline error)
    bool built[kLanguages] = {};
    const char* language[kLanguages] = {"slang", "glsl"};
};

/// 1 ok, 0 no kernel, -1 failure.
int initRig(Context& ctx, Scene& s, Rig& rig, u32 width, u32 height, bool shade) {
    rig.width = width;
    rig.height = height;
    rig.shade = shade;
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 64;
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
    ad.scene = &s.gpu;
    ad.instanceCapacity = 64;
    ad.meshCapacity = 16;
    if (!rig.as.init(ad)) {
        std::printf("  acceleration structures: %s\n", rig.as.reason());
        return 0;
    }
    u32 built = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        RtEffectsDesc fd{};
        fd.device = ctx.device.get();
        fd.allocator = ctx.allocator.get();
        fd.bindless = &ctx.bindless;
        fd.width = width;
        fd.height = height;
        fd.language = k == 0u ? RtEffectsKernelLanguage::Slang : RtEffectsKernelLanguage::Glsl;
        rig.built[k] = rig.fx[k].init(fd) && std::strcmp(rig.fx[k].kernelLanguage(), rig.language[k]) == 0;
        if (rig.built[k] && shade) {
            ClusteredLightingDesc ld{};
            ld.device = ctx.device.get();
            ld.allocator = ctx.allocator.get();
            ld.bindless = &ctx.bindless;
            ld.lightCapacity = 64;
            ld.language = k == 0u ? LightingKernelLanguage::Slang : LightingKernelLanguage::Glsl;
            rig.built[k] = rig.lighting[k].init(ld) && rig.plain[k].init(ld);
        }
        built += rig.built[k] ? 1u : 0u;
        std::printf("  %s kernels: %s (%s)\n", rig.language[k], rig.built[k] ? "built" : "not built, skipped", rig.fx[k].reason());
    }
    const usize pixels = static_cast<usize>(width) * height;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!hostBuffer(ctx, ctx.shadowDump[k], pixels * kRtfxMaxShadowLights * sizeof(RtfxRayRecord), MemoryUsage::GpuToCpu,
                        "rp_rt_effects.shadow_dump") ||
            !hostBuffer(ctx, ctx.reflDump[k], pixels * sizeof(RtfxRayRecord), MemoryUsage::GpuToCpu, "rp_rt_effects.refl_dump") ||
            !hostBuffer(ctx, ctx.shadeDump[k], pixels * 16u, MemoryUsage::GpuToCpu, "rp_rt_effects.shade_dump") ||
            !hostBuffer(ctx, ctx.plainDump[k], pixels * 16u, MemoryUsage::GpuToCpu, "rp_rt_effects.plain_dump")) {
            return -1;
        }
    }
    return built > 0u ? 1 : 0;
}

void destroyRig(Rig& rig) {
    for (u32 k = 0; k < kLanguages; ++k) {
        rig.lighting[k].destroy();
        rig.plain[k].destroy();
        rig.fx[k].destroy();
    }
    rig.as.destroy();
    rig.resolve.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
    bool image = false;
    rg::TextureRef src;
    rg::BufferRef buffer;
    rg::BufferRef dst;
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
        const VkBufferCopy region{0, c.dstOffset, c.bytes};
        vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.buffer)), dst, 1, &region);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.src)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

/// G-buffer attachments read back (GBufferAttachment order): RT0, RT1, RT2, RT4, RT5.
constexpr u32 kGBufferRead[5] = {0u, 1u, 2u, 4u, 5u};
constexpr u32 kGBufferBytes[5] = {8u, 4u, 4u, 4u, 8u};

struct ReadbackLayout {
    u64 gbuffer[5] = {};
    u64 out[kLanguages] = {};
    u64 lists[kLanguages] = {};
    u64 end = 0;
};

struct FrameOptions {
    bool readback = true;
    bool dumps = true;
    bool shade = false;
    bool soft = false;       ///< converge: area lights + sun disk, 4 samples; else punctual, 1 sample
    u32 frameIndex = 0;
    u32 reflectionSamples = 1;
};

struct FrameState {
    CopyRecord copies[16];
    u32 copyCount = 0;
    ReadbackLayout layout{};
};

ReadbackLayout makeLayout(const Rig& rig) {
    ReadbackLayout l{};
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    const u64 pixels = static_cast<u64>(rig.width) * rig.height;
    for (u32 i = 0; i < 5u; ++i) {
        l.gbuffer[i] = take(pixels * kGBufferBytes[i]);
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        l.out[k] = take(RtEffectsOutputLayout::compute(rig.width, rig.height).bytes);
        if (rig.shade && rig.built[k]) {
            l.lists[k] = take(rig.lighting[k].listsBuffer().desc.size);
        }
    }
    l.end = cursor;
    return l;
}

bool ensureReadback(Context& ctx, const ReadbackLayout& layout) {
    if (ctx.readback.handle != nullptr && ctx.readback.desc.size >= layout.end) {
        return true;
    }
    if (ctx.readback.handle != nullptr) {
        ctx.allocator->destroyBuffer(ctx.readback);
    }
    BufferDesc d{};
    d.size = static_cast<usize>(layout.end);
    d.usage = BufferUsage::TransferDst;
    d.memoryUsage = MemoryUsage::GpuToCpu;
    d.name = "rp_rt_effects.readback";
    return ctx.allocator->createBuffer(d, ctx.readback) && ctx.readback.mapped != nullptr;
}

RtEffectsFrameDesc makeFrameDesc(const Scene& s, const Rig& rig, const Camera& cam, const FrameOptions& o) {
    RtEffectsFrameDesc d{};
    std::memcpy(d.viewProj, cam.viewProj.m, sizeof(d.viewProj));
    d.cameraPosition[0] = static_cast<f32>(cam.eye.x);
    d.cameraPosition[1] = static_cast<f32>(cam.eye.y);
    d.cameraPosition[2] = static_cast<f32>(cam.eye.z);
    d.scene = s.gpu.headerHandle();
    d.gbuffer = &rig.resolve;
    d.tlasAddress = rig.as.tlasAddress();
    const u32 slots[3] = {o.soft ? static_cast<u32>(kRect) : static_cast<u32>(kSun), o.soft ? static_cast<u32>(kDisk) : static_cast<u32>(kPoint),
                          o.soft ? static_cast<u32>(kSun) : static_cast<u32>(kSpot)};
    for (u32 i = 0; i < 3u; ++i) {
        d.shadowLights[i] = RtShadowLightDesc{slots[i], s.gpu.light(slots[i]), o.soft ? 4u : 1u};
    }
    d.shadowLightCount = 3;
    d.hitLights[0] = RtHitLightDesc{kSun, s.gpu.light(kSun), true};
    d.hitLightCount = 1;
    d.reflectionSamples = o.reflectionSamples;
    d.frameIndex = o.frameIndex;
    std::memcpy(d.ambient, kAmbient, sizeof(d.ambient));
    std::memcpy(d.sky, kSky, sizeof(d.sky));
    return d;
}

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const Camera& cam, const FrameOptions& o, bool countFx = false) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, cam.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    ResolveFrameDesc rf{};
    std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
    std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
    rf.scene = s.gpu.headerHandle();
    rf.vis = rig.vb.visStorageHandle();
    rf.sampler = ctx.samplerHandle;
    ok = rig.resolve.beginFrame(ctx.serial, rf) && ok;
    const RtEffectsFrameDesc desc = makeFrameDesc(s, rig, cam, o);
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        t_count = countFx;
        ok = rig.fx[k].beginFrame(ctx.serial, desc) && ok;
        t_count = false;
        if (o.shade) {
            LightingFrameDesc lf{};
            lf.camera = clusterCamera(cam);
            lf.scene = s.gpu.headerHandle();
            lf.lightCount = s.gpu.lightHighWater();
            std::memcpy(lf.ambient, kAmbient, sizeof(lf.ambient));
            lf.gbuffer = &rig.resolve;
            lf.rtShadows = rig.fx[k].shadowViewAddress();
            ok = rig.lighting[k].beginFrame(ctx.serial, lf) && ok;
            lf.rtShadows = 0;
            ok = rig.plain[k].beginFrame(ctx.serial, lf) && ok;
        }
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, FrameState& fs, const FrameOptions& o) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const RtGraphRefs rtRefs = rig.as.importInto(graph, sceneRefs);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), rig.culler, cull);
    const ResolveGraphRefs gbuffer = rig.resolve.importInto(graph);
    rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
    RtEffectsGraphRefs fx[kLanguages]{};
    LightingGraphRefs lighting[kLanguages]{};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        fx[k] = rig.fx[k].importInto(graph);
        RtEffectsDump sd{};
        RtEffectsDump rd{};
        if (o.dumps) {
            sd.buffer = graph.importBuffer(rg::ImportedBuffer{ctx.shadowDump[k].handle, ctx.shadowDump[k].desc.size, rg::kNoQueue, nullptr,
                                                              "rp_rt_effects.shadow_dump"});
            sd.address = ctx.shadowDump[k].deviceAddress;
            rd.buffer = graph.importBuffer(
                rg::ImportedBuffer{ctx.reflDump[k].handle, ctx.reflDump[k].desc.size, rg::kNoQueue, nullptr, "rp_rt_effects.refl_dump"});
            rd.address = ctx.reflDump[k].deviceAddress;
        }
        expect(rig.fx[k].addShadows(graph, fx[k], rtRefs.tlas, gbuffer, sd), "addShadows");
        expect(rig.fx[k].addReflections(graph, fx[k], rtRefs.tlas, gbuffer, sceneRefs, rd), "addReflections");
        if (o.dumps && o.readback) {
            graph.addPass("readback.dumps", nullptr, nullptr).use(sd.buffer, rg::Access::HostRead).use(rd.buffer, rg::Access::HostRead);
        }
        if (o.shade) {
            lighting[k] = rig.lighting[k].importInto(graph);
            rig.lighting[k].addAssignment(graph, lighting[k], sceneRefs);
            rig.fx[k].addSamplingUse(graph, fx[k], rg::kStageCompute);
            const rg::BufferRef dump = o.readback ? graph.importBuffer(rg::ImportedBuffer{ctx.shadeDump[k].handle, ctx.shadeDump[k].desc.size,
                                                                                          rg::kNoQueue, nullptr, "rp_rt_effects.shade_dump"})
                                                  : rg::BufferRef{};
            rig.lighting[k].addShade(graph, lighting[k], sceneRefs, gbuffer, dump, o.readback ? ctx.shadeDump[k].deviceAddress : 0u);
            const LightingGraphRefs plainRefs = rig.plain[k].importInto(graph);
            rig.plain[k].addAssignment(graph, plainRefs, sceneRefs);
            const rg::BufferRef plainDump = o.readback ? graph.importBuffer(rg::ImportedBuffer{ctx.plainDump[k].handle, ctx.plainDump[k].desc.size,
                                                                                               rg::kNoQueue, nullptr, "rp_rt_effects.plain_dump"})
                                                       : rg::BufferRef{};
            rig.plain[k].addShade(graph, plainRefs, sceneRefs, gbuffer, plainDump, o.readback ? ctx.plainDump[k].deviceAddress : 0u);
            if (o.readback) {
                graph.addPass("readback.shade", nullptr, nullptr).use(dump, rg::Access::HostRead).use(plainDump, rg::Access::HostRead);
            }
        } else {
            // Where light.shade would read the visibility (declaration only).
            rig.fx[k].addSamplingUse(graph, fx[k], rg::kStageCompute);
        }
    }
    if (!o.readback) {
        return;
    }
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_rt_effects.readback"});
    auto addCopy = [&](bool image, rg::TextureRef src, rg::BufferRef buffer, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.image = image;
        c.src = src;
        c.buffer = buffer;
        c.dst = rb;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        c.width = rig.width;
        c.height = rig.height;
        rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
        if (image) {
            pass.use(src, rg::Access::TransferSrc);
        } else {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        }
        pass.use(rb, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    const u64 pixels = static_cast<u64>(rig.width) * rig.height;
    for (u32 i = 0; i < 5u; ++i) {
        addCopy(true, gbuffer.gbuffer[kGBufferRead[i]], {}, fs.layout.gbuffer[i], pixels * kGBufferBytes[i]);
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        addCopy(false, {}, fx[k].output, fs.layout.out[k], rig.fx[k].outputLayout().bytes);
        if (o.shade) {
            addCopy(false, {}, lighting[k].lists, fs.layout.lists[k], rig.lighting[k].listsBuffer().desc.size);
        }
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void beginSceneFrame(Context& ctx, Scene& s, Rig& rig) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    rig.as.beginFrame(ctx.serial);
}

void collect(Context& ctx, Scene& s, Rig& rig) {
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.resolve.collectRetired(ctx.serial);
    rig.as.collectRetired(ctx.serial);
    for (u32 k = 0; k < kLanguages; ++k) {
        rig.fx[k].collectRetired(ctx.serial);
        rig.lighting[k].collectRetired(ctx.serial);
        rig.plain[k].collectRetired(ctx.serial);
    }
    s.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
}

bool runFrame(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const Camera& cam, const FrameOptions& o, FrameState& fs) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    const RtCommitStats rtStats = rig.as.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rig, cam, o)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    if (o.readback) {
        fs.layout = makeLayout(rig);
        if (!ensureReadback(ctx, fs.layout)) {
            return false;
        }
    }
    buildGraph(ctx, s, rig, graph, fs, o);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    collect(ctx, s, rig);
    return stats.ok && rtStats.ok && result.ok && waited;
}

// --- CPU side -------------------------------------------------------------------------------------
const u8* rbAt(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

struct GBufferCpu {
    std::vector<f32> depth;
    std::vector<Vec4> rt0, rt1, rt2, rt5;
};

void decodeGBuffer(Context& ctx, const FrameState& fs, u32 pixels, GBufferCpu& g) {
    g.depth.resize(pixels);
    g.rt0.resize(pixels);
    g.rt1.resize(pixels);
    g.rt2.resize(pixels);
    g.rt5.resize(pixels);
    const u8* rt0 = rbAt(ctx, fs.layout.gbuffer[0]);
    const u8* rt1 = rbAt(ctx, fs.layout.gbuffer[1]);
    const u8* rt2 = rbAt(ctx, fs.layout.gbuffer[2]);
    const u8* rt4 = rbAt(ctx, fs.layout.gbuffer[3]);
    const u8* rt5 = rbAt(ctx, fs.layout.gbuffer[4]);
    for (usize p = 0; p < pixels; ++p) {
        std::memcpy(&g.depth[p], rt4 + p * 4u, 4u);
        g.rt0[p] = {halfAt(rt0, p * 4u), halfAt(rt0, p * 4u + 1u), halfAt(rt0, p * 4u + 2u), halfAt(rt0, p * 4u + 3u)};
        g.rt5[p] = {halfAt(rt5, p * 4u), halfAt(rt5, p * 4u + 1u), halfAt(rt5, p * 4u + 2u), halfAt(rt5, p * 4u + 3u)};
        g.rt1[p] = {rt1[p * 4u] / 255.f, rt1[p * 4u + 1u] / 255.f, rt1[p * 4u + 2u] / 255.f, rt1[p * 4u + 3u] / 255.f};
        g.rt2[p] = {rt2[p * 4u] / 255.f, rt2[p * 4u + 1u] / 255.f, rt2[p * 4u + 2u] / 255.f, rt2[p * 4u + 3u] / 255.f};
    }
}

struct Outputs {
    std::vector<f32> visibility;  ///< [c][pixel]
    std::vector<f32> hitDistance; ///< [c][pixel]
    std::vector<RtfxReflectionTexel> reflection;
};

void readOutputs(Context& ctx, const FrameState& fs, const Rig& rig, u32 k, Outputs& o) {
    const u32 pixels = rig.width * rig.height;
    const RtEffectsOutputLayout& l = rig.fx[k].outputLayout();
    const u8* base = rbAt(ctx, fs.layout.out[k]);
    o.visibility.resize(static_cast<usize>(pixels) * kRtfxMaxShadowLights);
    o.hitDistance.resize(o.visibility.size());
    o.reflection.resize(pixels);
    std::memcpy(o.visibility.data(), base + l.visibility, o.visibility.size() * 4u);
    std::memcpy(o.hitDistance.data(), base + l.hitDistance, o.hitDistance.size() * 4u);
    std::memcpy(o.reflection.data(), base + l.reflection, o.reflection.size() * sizeof(RtfxReflectionTexel));
}

RtProbeRay toProbe(const RtfxRayRecord& r) {
    RtProbeRay ray{};
    std::memcpy(ray.origin, r.origin, sizeof(ray.origin));
    std::memcpy(ray.direction, r.direction, sizeof(ray.direction));
    ray.tMin = r.tMin;
    ray.tMax = r.tMax;
    return ray;
}

f64 dist3(const f32 a[3], const F3& b) {
    const f64 dx = static_cast<f64>(a[0]) - b.x, dy = static_cast<f64>(a[1]) - b.y, dz = static_cast<f64>(a[2]) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// The CPU mirror of a pixel's surface (rt_effects_kernel.hpp in f32 from the read-back texels).
struct MirrorSurface {
    bool valid = false;
    F3 position{};
    F3 normal{};
    F3 origin{};
    F3 view{};
    f32 roughness = 0.f;
};

MirrorSurface mirrorSurface(const RtfxFrameConstants& c, const GBufferCpu& g, u32 x, u32 y) {
    MirrorSurface m{};
    const usize p = static_cast<usize>(y) * c.view.width + x;
    const f32 depth = g.depth[p];
    if (!(depth < 1.f) || !std::isfinite(depth)) {
        return m;
    }
    m.valid = true;
    m.position = rtfxReconstruct<f32>(c.invViewProj, x, y, c.invWidth, c.invHeight, depth);
    m.normal = rtfxOctDecode<f32>(g.rt0[p].x, g.rt0[p].y);
    const F3 camera = rtfxLoad<f32>(c.cameraPosition);
    m.origin = rtfxRayOrigin<f32>(m.position, m.normal, camera, c.normalBias, c.viewBias);
    m.view = rtfxNormalize(rtfxSub(camera, m.position), m.normal);
    m.roughness = g.rt2[p].x;
    return m;
}

// --- hard ---------------------------------------------------------------------------------------------
struct HardStats {
    u32 rays = 0;
    u32 sky = 0;
    u32 hits = 0;
    u32 robust = 0;
    u32 robustMismatch = 0;
    u32 visBad = 0;       ///< visibility != (hit ? 0 : 1)
    u32 hitDistBad = 0;   ///< output hit distance != the dumped t
    u32 tBad = 0;
    f64 maxDt = 0.0;
    f64 maxOrigin = 0.0;  ///< |GPU origin - CPU mirror| / (1 + |origin|)
    f64 maxDir = 0.0;
    u32 genBad = 0;
    u32 validMismatch = 0; ///< reflection: below-horizon decision differs from the mirror
    u32 radianceChecked = 0;
    u32 radianceBad = 0;
    u32 secondaryAmbiguous = 0;
    f64 maxRadianceRel = 0.0;
    u32 mirrorPixels = 0;
};

void checkShadowRays(const Scene& s, const Rig& rig, u32 k, const GBufferCpu& g, const Outputs& out, const RtfxRayRecord* dump,
                     HardStats& st) {
    const RtfxFrameConstants& c = rig.fx[k].frameConstants();
    const u32 W = rig.width, H = rig.height, pixels = W * H;
    for (u32 ch = 0; ch < c.view.count; ++ch) {
        const RtfxShadowLight& light = c.shadowLights[ch];
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const u32 p = y * W + x;
                const RtfxRayRecord& r = dump[ch * pixels + p];
                const f32 vis = out.visibility[ch * pixels + p];
                const f32 hd = out.hitDistance[ch * pixels + p];
                const MirrorSurface m = mirrorSurface(c, g, x, y);
                if (!m.valid) {
                    ++st.sky;
                    st.visBad += (vis != 1.f || hd != kRtfxNoHit || r.flags != 0u) ? 1u : 0u;
                    continue;
                }
                ++st.rays;
                // Ray generation == the CPU mirror.
                const f64 dOrigin = dist3(r.origin, m.origin) / (1.0 + std::sqrt(static_cast<f64>(m.origin.x) * m.origin.x +
                                                                                 static_cast<f64>(m.origin.y) * m.origin.y +
                                                                                 static_cast<f64>(m.origin.z) * m.origin.z));
                u32 ux = 0, uy = 0;
                rtfxSample2(c.frameIndex * light.samples + 0u, rtfxPixelSeed(x, y, ch, c.seed), ux, uy);
                F3 dir{};
                f32 tMax = 0.f;
                const F3 gpuOrigin{r.origin[0], r.origin[1], r.origin[2]};
                rtfxShadowRay<f32>(light, gpuOrigin, rtfxUnit(ux), rtfxUnit(uy), c.farDistance, dir, tMax);
                const f64 dDir = dist3(r.direction, dir);
                st.maxOrigin = std::max(st.maxOrigin, dOrigin);
                st.maxDir = std::max(st.maxDir, dDir);
                st.genBad += (dOrigin > 1e-4 || dDir > 1e-5 || std::fabs(r.tMax - tMax) > 1e-5f * (1.f + tMax)) ? 1u : 0u;
                // Parity with the CPU BVH.
                const bool gHit = (r.flags & kRtfxRayHit) != 0u;
                st.hits += gHit ? 1u : 0u;
                st.visBad += vis != (gHit ? 0.f : 1.f) ? 1u : 0u;
                st.hitDistBad += (gHit ? hd != r.t : hd != kRtfxNoHit) ? 1u : 0u;
                const RtRefClassified cl = rig.as.ready() ? s.bvh.traceClassified(toProbe(r), c.shadowCullMask, kEdgeEpsilon) : RtRefClassified{};
                const bool same = gHit == cl.hit.hit && (!gHit || (r.instance == cl.hit.instance && r.primitive == cl.hit.primitive));
                st.robust += cl.robust ? 1u : 0u;
                if (cl.robust && !same) {
                    ++st.robustMismatch;
                    if (st.robustMismatch <= 5u) {
                        std::fprintf(stderr, "  shadow light %u pixel (%u, %u): gpu %d inst %u prim %u t %.6f, cpu %d inst %u prim %u t %.6f\n",
                                     ch, x, y, gHit ? 1 : 0, r.instance, r.primitive, static_cast<f64>(r.t), cl.hit.hit ? 1 : 0,
                                     cl.hit.instance, cl.hit.primitive, cl.hit.t);
                    }
                }
                if (same && gHit) {
                    const f64 dt = std::fabs(static_cast<f64>(r.t) - cl.hit.t);
                    st.maxDt = std::max(st.maxDt, dt);
                    st.tBad += dt > rtTTolerance(cl.hit.t) ? 1u : 0u;
                }
            }
        }
    }
}

void checkReflectionRays(const Scene& s, const Rig& rig, u32 k, const GBufferCpu& g, const Outputs& out, const RtfxRayRecord* dump,
                         HardStats& st) {
    const RtfxFrameConstants& c = rig.fx[k].frameConstants();
    const RtEffectsReference ref(s.bvh, s.gpu, s.materials.data(), static_cast<u32>(s.materials.size()));
    const u32 W = rig.width, H = rig.height;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const u32 p = y * W + x;
            const RtfxRayRecord& r = dump[p];
            const RtfxReflectionTexel& t = out.reflection[p];
            const MirrorSurface m = mirrorSurface(c, g, x, y);
            if (!m.valid) {
                ++st.sky;
                st.visBad += (t.radiance[0] != 0.f || t.hitDistance != kRtfxNoHit || r.flags != 0u) ? 1u : 0u;
                continue;
            }
            const bool mirror = !(m.roughness >= c.mirrorRoughness);
            st.mirrorPixels += mirror ? 1u : 0u;
            u32 ux = 0, uy = 0;
            rtfxSample2(c.frameIndex * (mirror ? 1u : c.reflectionSamples), rtfxPixelSeed(x, y, kRtfxStreamReflection, c.seed), ux, uy);
            F3 dir{};
            const bool valid = rtfxReflectionRay<f32>(m.normal, m.view, m.roughness, c.mirrorRoughness, rtfxUnit(ux), rtfxUnit(uy), dir);
            const bool gValid = (r.flags & kRtfxRayTraced) != 0u;
            if (valid != gValid) {
                ++st.validMismatch; // a direction within float noise of the horizon
                continue;
            }
            if (!valid) {
                st.visBad += (t.radiance[0] != 0.f || t.radiance[1] != 0.f || t.radiance[2] != 0.f || t.hitDistance != kRtfxNoHit) ? 1u : 0u;
                continue;
            }
            ++st.rays;
            const f64 dOrigin = dist3(r.origin, m.origin) /
                                (1.0 + std::sqrt(static_cast<f64>(m.origin.x) * m.origin.x + static_cast<f64>(m.origin.y) * m.origin.y +
                                                 static_cast<f64>(m.origin.z) * m.origin.z));
            const f64 dDir = dist3(r.direction, dir);
            st.maxOrigin = std::max(st.maxOrigin, dOrigin);
            st.maxDir = std::max(st.maxDir, dDir);
            // The G-buffer normal / view differ from the GPU's by float rounding only; glossy lobes amplify it.
            st.genBad += (dOrigin > 1e-4 || dDir > 2e-4) ? 1u : 0u;
            const bool gHit = (r.flags & kRtfxRayHit) != 0u;
            st.hits += gHit ? 1u : 0u;
            st.hitDistBad += (gHit ? t.hitDistance != r.t : t.hitDistance != kRtfxNoHit) ? 1u : 0u;
            const RtRefClassified cl = s.bvh.traceClassified(toProbe(r), c.reflectionCullMask, kEdgeEpsilon);
            const bool same = gHit == cl.hit.hit && (!gHit || (r.instance == cl.hit.instance && r.primitive == cl.hit.primitive));
            st.robust += cl.robust ? 1u : 0u;
            if (cl.robust && !same) {
                ++st.robustMismatch;
                if (st.robustMismatch <= 5u) {
                    std::fprintf(stderr, "  reflection pixel (%u, %u): gpu %d inst %u prim %u t %.6f, cpu %d inst %u prim %u t %.6f\n", x, y,
                                 gHit ? 1 : 0, r.instance, r.primitive, static_cast<f64>(r.t), cl.hit.hit ? 1 : 0, cl.hit.instance,
                                 cl.hit.primitive, cl.hit.t);
                }
            }
            if (!same || !cl.robust) {
                continue;
            }
            if (gHit) {
                const f64 dt = std::fabs(static_cast<f64>(r.t) - cl.hit.t);
                st.maxDt = std::max(st.maxDt, dt);
                st.tBad += dt > rtTTolerance(cl.hit.t) ? 1u : 0u;
            }
            // Radiance == the reference's hit shading at the GPU's ray and hit (f32 vs f64: 1e-3 relative).
            RtRefHit h = cl.hit;
            h.t = gHit ? static_cast<f64>(r.t) : -1.0;
            bool secondaryRobust = true;
            const V3 rad = ref.shadeHit(c, V3{r.origin[0], r.origin[1], r.origin[2]}, V3{r.direction[0], r.direction[1], r.direction[2]}, h,
                                        &secondaryRobust);
            if (!secondaryRobust) {
                ++st.secondaryAmbiguous;
                continue;
            }
            ++st.radianceChecked;
            const f64 ref3[3] = {rad.x, rad.y, rad.z};
            bool bad = false;
            for (u32 ch = 0; ch < 3u; ++ch) {
                const f64 d = std::fabs(static_cast<f64>(t.radiance[ch]) - ref3[ch]);
                st.maxRadianceRel = std::max(st.maxRadianceRel, d / (std::fabs(ref3[ch]) + 1e-3));
                bad = bad || d > 1e-3 * std::fabs(ref3[ch]) + 1e-4;
            }
            st.radianceBad += bad ? 1u : 0u;
        }
    }
}

void printHard(const char* tag, const HardStats& st) {
    std::printf("  %-28s %u rays (%u sky), %u hits, %u robust (%.2f%%), %u robust mismatches, vis / output bad %u, hit-distance output "
                "bad %u, t outside tol %u (max |dt| %.2e), ray-gen max |do| %.2e |dd| %.2e (%u beyond tol)",
                tag, st.rays, st.sky, st.hits, st.robust, st.rays > 0u ? 100.0 * st.robust / st.rays : 0.0, st.robustMismatch, st.visBad,
                st.hitDistBad, st.tBad, st.maxDt, st.maxOrigin, st.maxDir, st.genBad);
    if (st.radianceChecked + st.secondaryAmbiguous > 0u) {
        std::printf(", %u mirror px, horizon mismatches %u, radiance checked %u (bad %u, max rel %.2e, %u secondary ambiguous)",
                    st.mirrorPixels, st.validMismatch, st.radianceChecked, st.radianceBad, st.maxRadianceRel, st.secondaryAmbiguous);
    }
    std::printf("\n");
}

void expectHard(const HardStats& st, bool reflection) {
    expect(st.rays > 1000u && st.hits > 50u && st.hits < st.rays, "rays both hit and miss");
    expect(st.robustMismatch == 0u, "robust rays: GPU hit / miss and ids == the CPU BVH reference (exact)");
    expect(st.robust * 100u >= st.rays * 95u, ">= 95% robust rays");
    expect(st.visBad == 0u, "outputs == the dumped ray results (visibility 0 / 1, sky pixels)");
    expect(st.hitDistBad == 0u, "hit-distance output == the traced t");
    expect(st.tBad == 0u, "hit distance == the CPU reference within rtTTolerance");
    expect(st.genBad == 0u, "ray generation == the CPU mirror of the kernel");
    if (reflection) {
        expect(st.validMismatch * 1000u <= st.rays + 1000u, "horizon decisions agree (<= 0.1%)");
        expect(st.radianceChecked > 500u && st.radianceBad == 0u, "reflection radiance == the reference hit shading");
        expect(st.secondaryAmbiguous * 50u <= st.radianceChecked, "ambiguous secondary shadow rays stay below 2%");
        expect(st.mirrorPixels > 100u, "mirror pixels on screen");
    }
}

int runHard(Context& ctx) {
    Scene s;
    if (!buildScene(ctx, s, false)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(ctx, s, rig, 128, 96, false);
    if (rc <= 0) {
        destroyRig(rig);
        s.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: rt effects unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    rg::Graph graph;
    FrameState fs;
    const u32 pixels = rig.width * rig.height;
    for (u32 frame = 0; frame < 3u; ++frame) {
        beginSceneFrame(ctx, s, rig);
        if (frame > 0u) {
            moveBox(s, 0, 0.25);
            moveBox(s, 3, -0.2);
        }
        const Camera cam = makeCamera(rig.width, rig.height, frame);
        FrameOptions o{};
        o.frameIndex = frame;
        if (!runFrame(ctx, s, rig, graph, cam, o, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        if (frame > 0u) {
            s.bvh.updateInstances(s.gpu);
        }
        GBufferCpu g{};
        decodeGBuffer(ctx, fs, pixels, g);
        Outputs outs[kLanguages];
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            readOutputs(ctx, fs, rig, k, outs[k]);
            HardStats sh{};
            checkShadowRays(s, rig, k, g, outs[k], static_cast<const RtfxRayRecord*>(ctx.shadowDump[k].mapped), sh);
            char tag[64];
            std::snprintf(tag, sizeof(tag), "frame %u %s shadows", frame, rig.language[k]);
            printHard(tag, sh);
            expectHard(sh, false);
            HardStats rf{};
            checkReflectionRays(s, rig, k, g, outs[k], static_cast<const RtfxRayRecord*>(ctx.reflDump[k].mapped), rf);
            std::snprintf(tag, sizeof(tag), "frame %u %s reflections", frame, rig.language[k]);
            printHard(tag, rf);
            expectHard(rf, true);
        }
        if (rig.built[0] && rig.built[1]) {
            u32 visDiff = 0, reflDiff = 0;
            f64 maxRefl = 0.0;
            for (usize i = 0; i < static_cast<usize>(pixels) * 3u; ++i) {
                visDiff += outs[0].visibility[i] != outs[1].visibility[i] ? 1u : 0u;
            }
            for (u32 p = 0; p < pixels; ++p) {
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const f64 a = outs[0].reflection[p].radiance[ch], b = outs[1].reflection[p].radiance[ch];
                    const f64 d = std::fabs(a - b) / (std::fabs(a) + 1e-3);
                    maxRefl = std::max(maxRefl, d);
                    reflDiff += d > 1e-3 ? 1u : 0u;
                }
            }
            std::printf("  frame %u Slang vs GLSL: %u visibility values differ, %u reflection channels beyond 1e-3 (max rel %.2e)\n", frame,
                        visDiff, reflDiff, maxRefl);
            // Both equal the CPU reference on every robust ray; the rest (edge bands) may differ.
            expect(visDiff * 100u <= pixels * 3u, "Slang == GLSL visibility (differences only on non-robust rays)");
            expect(reflDiff * 100u <= pixels * 3u, "Slang == GLSL reflections (differences only on non-robust rays)");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- converge ----------------------------------------------------------------------------------------------
// Statistical tolerance. Per pixel, the GPU mean over N samples and the reference mean over M stratified samples
// estimate the same integral; with per-sample variance s^2 (the larger of the reference's sample variance and the
// GPU's per-frame variance x samples per frame, floored by the Bernoulli variance of the pooled mean for
// visibility), the difference has standard deviation at most
// sigma = sqrt(s^2 / N + s^2 / M) for independent sampling (Owen-scrambled Sobol and stratification only
// reduce it). Gate: at most 0.1% of the values beyond kZ sigma + kFloat (Gaussian: 6e-5 beyond 4 sigma),
// the mean signed difference (bias) within 4 sigma of the mean, and the converged RMS error well below one frame's.
constexpr f64 kZ = 4.0;
constexpr f64 kFloat = 2e-3; ///< f32 kernel vs f64 reference (ray origins, directions, hit shading)
constexpr u32 kConvergeFrames = 64;
constexpr u32 kShadowSqrt = 32;     ///< reference: 1024 samples per (pixel, light)
constexpr u32 kReflectionSqrt = 24; ///< reference: 576 samples per pixel

struct ConvergeStats {
    u32 pixels = 0;
    u32 outliers = 0;
    f64 sumDiff = 0.0;
    f64 sumVar = 0.0;
    f64 sumAbs = 0.0;
    f64 maxAbs = 0.0;
    f64 rmsConverged = 0.0;
    f64 rmsOneFrame = 0.0;
    u32 hitChecked = 0;
    u32 hitOutliers = 0;
    u32 partial = 0; ///< reference strictly between 0 and 1 (penumbra) / non-trivial radiance
};

/// Per-sample variance estimated from the GPU's per-frame means (sum / sum of squares over kConvergeFrames frames
/// of `samples` samples each): Var(frame mean) x samples (exact for independent samples, an upper bound for the
/// stratified Owen-Sobol frames). Rare heavy-tail events (a GGX tail sample reaching the emissive box, a direction
/// grazing the horizon) that the reference's own 576 samples may miss enter the tolerance through it.
f64 perSampleVariance(f64 sum, f64 sumSq, u32 samples) {
    const f64 n = kConvergeFrames;
    const f64 mean = sum / n;
    return std::max(0.0, (sumSq - n * mean * mean) / (n - 1.0)) * samples;
}

void accumulate(ConvergeStats& st, f64 gpu, f64 ref, f64 var, u32 n, u32 m, f64 oneFrame, u32 frameSamples) {
    const f64 sigma = std::sqrt(var / n + var / m);
    const f64 d = gpu - ref;
    ++st.pixels;
    st.outliers += std::fabs(d) > kZ * sigma + kFloat * (1.0 + std::fabs(ref)) ? 1u : 0u;
    st.sumDiff += d;
    st.sumVar += var / n + var / m;
    st.sumAbs += std::fabs(d);
    st.maxAbs = std::max(st.maxAbs, std::fabs(d));
    st.rmsConverged += d * d;
    st.rmsOneFrame += (oneFrame - ref) * (oneFrame - ref);
    (void)frameSamples;
}

void printConverge(const char* tag, const ConvergeStats& st) {
    const f64 n = std::max<u32>(st.pixels, 1u);
    std::printf("  %-34s %u values (%u non-trivial): %u beyond %.0f sigma (%.3f%%), mean |d| %.2e, max |d| %.3f, bias %.2e (4 sigma "
                "%.2e), RMS converged %.2e vs one frame %.2e; hit distance %u checked, %u beyond\n",
                tag, st.pixels, st.partial, st.outliers, kZ, 100.0 * st.outliers / n, st.sumAbs / n, st.maxAbs, st.sumDiff / n,
                4.0 * std::sqrt(st.sumVar) / n, std::sqrt(st.rmsConverged / n), std::sqrt(st.rmsOneFrame / n), st.hitChecked, st.hitOutliers);
}

void expectConverge(const ConvergeStats& st) {
    const f64 n = std::max<u32>(st.pixels, 1u);
    expect(st.pixels > 1000u && st.partial > 100u, "enough non-trivial pixels");
    expect(st.outliers * 1000u <= st.pixels, "converged result == the offline reference (<= 0.1% beyond 4 sigma)");
    expect(std::fabs(st.sumDiff / n) <= 4.0 * std::sqrt(st.sumVar) / n + 1e-3, "no bias against the offline reference");
    expect(std::sqrt(st.rmsConverged / n) * 3.0 < std::sqrt(st.rmsOneFrame / n), "the many-frame result converges (RMS error 3x below one frame)");
    expect(st.hitOutliers * 100u <= st.hitChecked + 10u, "mean occluder / hit distances == the reference");
}

int runConverge(Context& ctx) {
    Scene s;
    if (!buildScene(ctx, s, true)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(ctx, s, rig, 160, 120, false);
    if (rc <= 0) {
        destroyRig(rig);
        s.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: rt effects unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    const u32 W = rig.width, H = rig.height, pixels = W * H;
    const Camera cam = makeCamera(W, H, 0);
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kShadowSamples = 4, kReflSamples = 2;
    // Host accumulation per language: visibility sums, weighted hit-distance sums, radiance sums.
    std::vector<f64> visSum[kLanguages], visSq[kLanguages], hdSum[kLanguages], hdWeight[kLanguages], radSum[kLanguages],
        radSq[kLanguages], rhdSum[kLanguages], rhdWeight[kLanguages];
    Outputs first[kLanguages];
    GBufferCpu g0{};
    u32 gbufferChanged = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        visSum[k].assign(static_cast<usize>(pixels) * 3u, 0.0);
        visSq[k].assign(visSum[k].size(), 0.0);
        hdSum[k].assign(visSum[k].size(), 0.0);
        hdWeight[k].assign(visSum[k].size(), 0.0);
        radSum[k].assign(static_cast<usize>(pixels) * 3u, 0.0);
        radSq[k].assign(radSum[k].size(), 0.0);
        rhdSum[k].assign(pixels, 0.0);
        rhdWeight[k].assign(pixels, 0.0);
    }
    for (u32 frame = 0; frame < kConvergeFrames; ++frame) {
        beginSceneFrame(ctx, s, rig);
        FrameOptions o{};
        o.soft = true;
        o.frameIndex = frame;
        o.reflectionSamples = kReflSamples;
        o.dumps = false;
        if (!runFrame(ctx, s, rig, graph, cam, o, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        GBufferCpu g{};
        decodeGBuffer(ctx, fs, pixels, g);
        if (frame == 0u) {
            g0 = g;
        } else {
            gbufferChanged += std::memcmp(g.depth.data(), g0.depth.data(), g.depth.size() * 4u) != 0 ? 1u : 0u;
        }
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            Outputs out;
            readOutputs(ctx, fs, rig, k, out);
            for (usize i = 0; i < visSum[k].size(); ++i) {
                visSum[k][i] += out.visibility[i];
                visSq[k][i] += static_cast<f64>(out.visibility[i]) * out.visibility[i];
                const f64 occluded = (1.0 - out.visibility[i]) * kShadowSamples;
                if (out.hitDistance[i] >= 0.f) {
                    hdSum[k][i] += out.hitDistance[i] * occluded;
                    hdWeight[k][i] += occluded;
                }
            }
            for (u32 p = 0; p < pixels; ++p) {
                for (u32 ch = 0; ch < 3u; ++ch) {
                    radSum[k][p * 3u + ch] += out.reflection[p].radiance[ch];
                    radSq[k][p * 3u + ch] += static_cast<f64>(out.reflection[p].radiance[ch]) * out.reflection[p].radiance[ch];
                }
                if (out.reflection[p].hitDistance >= 0.f) {
                    rhdSum[k][p] += out.reflection[p].hitDistance;
                    rhdWeight[k][p] += 1.0;
                }
            }
            if (frame == 0u) {
                first[k] = out;
            }
        }
    }
    expect(gbufferChanged == 0u, "static view: the G-buffer is identical every frame");
    // Offline reference over the read-back surfaces (threads over rows).
    const u32 k0 = rig.built[0] ? 0u : 1u;
    const RtfxFrameConstants& c = rig.fx[k0].frameConstants();
    const RtEffectsReference ref(s.bvh, s.gpu, s.materials.data(), static_cast<u32>(s.materials.size()));
    std::vector<RtfxEstimate> refShadow(static_cast<usize>(pixels) * 3u), refRefl(pixels);
    {
        const u32 threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (u32 t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                for (u32 y = t; y < H; y += threads) {
                    for (u32 x = 0; x < W; ++x) {
                        const u32 p = y * W + x;
                        const RtfxSurface surf =
                            RtEffectsReference::surfaceFromGBuffer(c, x, y, g0.depth[p], g0.rt0[p].x, g0.rt0[p].y, g0.rt2[p].x);
                        for (u32 ch = 0; ch < 3u; ++ch) {
                            refShadow[ch * pixels + p] = ref.shadow(c, c.shadowLights[ch], surf, kShadowSqrt, 0x9E37u + 131u * p + ch);
                        }
                        refRefl[p] = ref.reflection(c, surf, kReflectionSqrt, 0x51EDu + 977u * p);
                    }
                }
            });
        }
        for (std::thread& th : pool) {
            th.join();
        }
    }
    const char* lightNames[3] = {"rectangle", "disk", "sun disk"};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        for (u32 ch = 0; ch < 3u; ++ch) {
            ConvergeStats st{};
            for (u32 p = 0; p < pixels; ++p) {
                const RtfxEstimate& e = refShadow[ch * pixels + p];
                if (!(g0.depth[p] < 1.f)) {
                    continue;
                }
                const usize i = static_cast<usize>(ch) * pixels + p;
                const u32 n = kConvergeFrames * kShadowSamples;
                const f64 gpu = visSum[k][i] / kConvergeFrames;
                const f64 pooled = (gpu * n + e.mean[0] * e.samples) / (n + e.samples);
                const f64 var = std::max({e.variance[0], pooled * (1.0 - pooled) + 1.0 / (n + e.samples),
                                          perSampleVariance(visSum[k][i], visSq[k][i], kShadowSamples)});
                accumulate(st, gpu, e.mean[0], var, n, e.samples, first[k].visibility[i], kShadowSamples);
                st.partial += e.mean[0] > 0.0 && e.mean[0] < 1.0 ? 1u : 0u;
                if (hdWeight[k][i] >= 32.0 && e.hits >= 32u) {
                    const f64 mean = hdSum[k][i] / hdWeight[k][i];
                    const f64 tvar = std::max(e.hitDistanceVariance, 1e-6);
                    ++st.hitChecked;
                    st.hitOutliers += std::fabs(mean - e.hitDistance) > kZ * std::sqrt(tvar / hdWeight[k][i] + tvar / e.hits) + 1e-3 ? 1u : 0u;
                }
            }
            char tag[64];
            std::snprintf(tag, sizeof(tag), "%s %s shadows", rig.language[k], lightNames[ch]);
            printConverge(tag, st);
            expectConverge(st);
        }
        ConvergeStats st{};
        for (u32 p = 0; p < pixels; ++p) {
            if (!(g0.depth[p] < 1.f)) {
                continue;
            }
            const RtfxEstimate& e = refRefl[p];
            const bool mirror = !(g0.rt2[p].x >= c.mirrorRoughness);
            const u32 n = kConvergeFrames * (mirror ? 1u : kReflSamples);
            for (u32 ch = 0; ch < 3u; ++ch) {
                const f64 gpu = radSum[k][p * 3u + ch] / kConvergeFrames;
                const f64 var = std::max(e.variance[ch], perSampleVariance(radSum[k][p * 3u + ch], radSq[k][p * 3u + ch],
                                                                           mirror ? 1u : kReflSamples)) +
                                1e-6;
                accumulate(st, gpu, e.mean[ch], var, n, e.samples, first[k].reflection[p].radiance[ch], 1u);
                st.partial += (ch == 0u && e.variance[0] > 1e-6) ? 1u : 0u;
            }
            if (rhdWeight[k][p] >= 16.0 && e.hits >= 16u) {
                // Frame means of the hit distance, averaged over the frames that hit (weights per frame).
                const f64 mean = rhdSum[k][p] / rhdWeight[k][p];
                const f64 tvar = std::max(e.hitDistanceVariance, 1e-6);
                ++st.hitChecked;
                st.hitOutliers += std::fabs(mean - e.hitDistance) > kZ * std::sqrt(tvar / rhdWeight[k][p] + tvar / e.hits) + 0.05 * e.hitDistance
                                      ? 1u
                                      : 0u;
            }
        }
        char tag[64];
        std::snprintf(tag, sizeof(tag), "%s reflections (rgb)", rig.language[k]);
        printConverge(tag, st);
        expectConverge(st);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- shade ------------------------------------------------------------------------------------------------
struct RtHook {
    const f32* visibility = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 count = 0;
    u32 slots[kRtfxMaxShadowLights] = {};
    static f32 fn(const void* user, u32 slot, u32 px, u32 py) {
        const RtHook& h = *static_cast<const RtHook*>(user);
        if (px >= h.width || py >= h.height) {
            return -1.f;
        }
        for (u32 c = 0; c < h.count; ++c) {
            if (h.slots[c] == slot) {
                return h.visibility[static_cast<usize>(c) * h.width * h.height + static_cast<usize>(py) * h.width + px];
            }
        }
        return -1.f;
    }
};

int runShade(Context& ctx) {
    Scene s;
    if (!buildScene(ctx, s, false)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(ctx, s, rig, 128, 96, true);
    if (rc <= 0) {
        destroyRig(rig);
        s.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: rt effects unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    const u32 W = rig.width, H = rig.height, pixels = W * H;
    rg::Graph graph;
    FrameState fs;
    for (u32 frame = 0; frame < 2u; ++frame) {
        beginSceneFrame(ctx, s, rig);
        if (frame == 1u) {
            moveBox(s, 1, 0.3);
        }
        const Camera cam = makeCamera(W, H, frame);
        FrameOptions o{};
        o.shade = true;
        o.soft = frame == 1u; // frame 1: fractional visibility (area lights + sun disk, 4 samples)
        o.frameIndex = frame;
        if (!runFrame(ctx, s, rig, graph, cam, o, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        GBufferCpu g{};
        decodeGBuffer(ctx, fs, pixels, g);
        std::vector<GpuLight> table(s.gpu.lightHighWater());
        for (u32 i = 0; i < table.size(); ++i) {
            table[i] = s.gpu.light(i);
        }
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            const ClusteredLighting& L = rig.lighting[k];
            const LightingBufferLayout& lay = L.layout();
            const u8* base = rbAt(ctx, fs.layout.lists[k]);
            LightListHeader header{};
            std::memcpy(&header, base + lay.header, sizeof(header));
            ClusterGridSoA grid{};
            const u32 clusters = L.clusterDesc().clusterCount();
            grid.grid.resize(clusters);
            std::memcpy(grid.grid.data(), base + lay.grid, static_cast<usize>(clusters) * sizeof(ClusterGridEntry));
            grid.lightList.resize(std::min<u32>(header.totalEntries, clusters * L.capacity()));
            std::memcpy(grid.lightList.data(), base + lay.lightList, grid.lightList.size() * 4u);
            std::vector<u32> directional(std::min(header.directionalCount, L.lightCapacity()));
            std::memcpy(directional.data(), base + lay.directional, directional.size() * 4u);
            Outputs out;
            readOutputs(ctx, fs, rig, k, out);
            const RtfxFrameConstants& c = rig.fx[k].frameConstants();
            RtHook hook{};
            hook.visibility = out.visibility.data();
            hook.width = W;
            hook.height = H;
            hook.count = c.view.count;
            std::memcpy(hook.slots, c.view.slots, sizeof(hook.slots));
            ShadeReferenceDesc sd{};
            sd.width = W;
            sd.height = H;
            sd.desc = L.clusterDesc();
            sd.camera = clusterCamera(cam);
            sd.ambient = {kAmbient[0], kAmbient[1], kAmbient[2]};
            sd.gbuffer = GBufferTexels{g.depth.data(), g.rt0.data(), g.rt1.data(), g.rt2.data(), g.rt5.data()};
            sd.lights = table.data();
            sd.lightCount = static_cast<u32>(table.size());
            sd.grid = &grid;
            sd.directional = &directional;
            sd.brdfLut = L.brdfLut();
            sd.rtShadow = &RtHook::fn;
            sd.rtShadowUser = &hook;
            std::vector<Vec4> ref;
            shadeReferenceFrame(sd, ref);
            ShadeReferenceDesc unshadowed = sd;
            unshadowed.rtShadow = nullptr;
            std::vector<Vec4> plain;
            shadeReferenceFrame(unshadowed, plain);
            const Vec4* gpu = static_cast<const Vec4*>(ctx.shadeDump[k].mapped);
            const Vec4* gpuPlain = static_cast<const Vec4*>(ctx.plainDump[k].mapped);
            u32 shaded = 0, bad = 0, alphaBad = 0, darker = 0, partial = 0, baseline = 0;
            f64 maxRel = 0.0;
            for (u32 p = 0; p < pixels; ++p) {
                if ((gpu[p].w > 0.f) != (ref[p].w > 0.f)) {
                    ++alphaBad;
                    continue;
                }
                if (!(ref[p].w > 0.f)) {
                    continue;
                }
                ++shaded;
                const f32 gc[3] = {gpu[p].x, gpu[p].y, gpu[p].z};
                const f32 cc[3] = {ref[p].x, ref[p].y, ref[p].z};
                const f32 pc[3] = {plain[p].x, plain[p].y, plain[p].z};
                const f32 gp[3] = {gpuPlain[p].x, gpuPlain[p].y, gpuPlain[p].z};
                bool isBad = false;
                bool baselineBeyond = false;
                for (u32 ch = 0; ch < 3u; ++ch) {
                    // The shade's device-vs-host error is per light (sqrt / division ulps of each light's response,
                    // the LTC form factors most); the visibility scales each light by <= 1 exactly, so the error
                    // bound of the UNSHADOWED sum bounds the shadowed one (relative to the shadowed sum it would
                    // grow wherever shadows remove the large terms).
                    // Plus the pixel's baseline: the same shade WITHOUT RT shadows vs its CPU kernel (the WP-2.2 area
                    // lights' thin margin: LTC form factors carry a few 1e-6 of a light's response in device ulps).
                    const f64 diff = std::fabs(static_cast<f64>(gc[ch]) - cc[ch]);
                    const f64 mag = std::max(std::fabs(static_cast<f64>(cc[ch])), std::fabs(static_cast<f64>(pc[ch])));
                    const f64 base = std::fabs(static_cast<f64>(gp[ch]) - pc[ch]);
                    if (mag > 1e-3) {
                        maxRel = std::max(maxRel, diff / mag);
                    }
                    baselineBeyond = baselineBeyond || !(base <= kTolRel * mag + kTolAbs);
                    isBad = isBad || !(diff <= kTolRel * mag + kTolAbs + 2.0 * base);
                }
                bad += isBad ? 1u : 0u;
                baseline += baselineBeyond ? 1u : 0u;
                darker += ref[p].x < plain[p].x - 1e-4f ? 1u : 0u;
                for (u32 ch = 0; ch < c.view.count; ++ch) {
                    const f32 v = out.visibility[static_cast<usize>(ch) * pixels + p];
                    partial += v > 0.f && v < 1.f ? 1u : 0u;
                }
            }
            std::printf("  frame %u %-5s light.shade + RT shadows (%s): %u shaded, %u bad, %u alpha, max rel %.2e; %u pixels darkened by RT "
                        "shadows, %u fractional visibilities; %u pixels whose unshadowed baseline exceeds 1e-4 (WP-2.2 margin)\n",
                        frame, rig.language[k], o.soft ? "soft" : "hard", shaded, bad, alphaBad, maxRel, darker, partial, baseline);
            expect(shaded > 5000u && bad == 0u && alphaBad == 0u, "light.shade with RT shadows == the CPU shade kernel (1e-4 |unshadowed| + 1e-6 + 2 x the pixel's unshadowed error)");
            expect(darker * 10u > shaded, "the RT shadows darken the shade");
            if (o.soft) {
                expect(partial > 200u, "soft RT shadows give fractional visibility");
            }
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------------
void hookBegin(const rg::PassContext&, const char* name, void*) { t_count = std::strncmp(name, "rt.", 3) == 0; }
void hookEnd(const rg::PassContext&, const char*, void*) { t_count = false; }

int runZeroAlloc(Context& ctx, bool countAllocations) {
    Scene s;
    if (!buildScene(ctx, s, true)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(ctx, s, rig, 96, 72, false);
    if (rc <= 0) {
        destroyRig(rig);
        s.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: rt effects unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long begin = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s, rig);
        moveBox(s, 0, frame % 2u == 0u ? 0.05 : -0.05);
        s.gpu.commit();
        rig.as.commit();
        ctx.upload.flush();
        const Camera cam = makeCamera(rig.width, rig.height, frame % 3u);
        FrameOptions o{};
        o.readback = false;
        o.dumps = false;
        o.soft = true;
        o.frameIndex = frame;
        o.reflectionSamples = 2;
        t_allocations = 0;
        bool ok = beginFrame(ctx, s, rig, cam, o, measure);
        const unsigned long long inBegin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, s, rig, graph, fs, o);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        collect(ctx, s, rig);
        ok = ok && result.ok && waited;
        expect(ok, "frame ok");
        if (measure) {
            begin += inBegin;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    const u32 k0 = rig.built[0] ? 0u : 1u;
    expect(rig.fx[k0].stats().shadowPasses == 1u && rig.fx[k0].stats().reflectionPasses == 1u, "rt.shadows + rt.reflections per frame");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (moving box + TLAS refit, moving camera, 3 soft lights x 4 rays, 2 reflection rays)\n"
                    "  RtEffects::beginFrame: %llu operator-new calls; rt.* pass callbacks: %llu; whole graph build: %llu\n",
                    kTotal - kWarmup, begin, callbacks, build);
        expect(begin == 0u && callbacks == 0u, "the RT effects make no steady-state heap allocations");
        expect(build == 0u, "graph build with the RT effects passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- caps_gate -------------------------------------------------------------------------------------------
int runCapsGate() {
    int rc = 0;
    {
        Context capped;
        const int setupRc = setup(capped, true, RenderTier::T1, false);
        if (setupRc != 0) {
            return setupRc;
        }
        RtEffects fx;
        RtEffectsDesc d{};
        d.device = capped.device.get();
        d.allocator = capped.allocator.get();
        d.bindless = &capped.bindless;
        const bool ok = fx.init(d);
        std::printf("  maxTier T1: init %s (%s)\n", ok ? "succeeded" : "refused", fx.reason());
        expect(!ok && std::strcmp(fx.reason(), "ok") != 0, "a device capped below T2 refuses the RT effects with a reason");
        rg::Graph graph;
        expect(!fx.addShadows(graph, RtEffectsGraphRefs{}, rg::BufferRef{}, ResolveGraphRefs{}), "a refused RtEffects records nothing");
    }
    {
        Context full;
        const int setupRc = setup(full, true);
        if (setupRc != 0) {
            return setupRc;
        }
        RtEffects fx;
        RtEffectsDesc d{};
        d.device = full.device.get();
        d.allocator = full.allocator.get();
        d.bindless = &full.bindless;
        const bool ok = fx.init(d);
        std::printf("  uncapped: init %s (%s, %s kernels)\n", ok ? "succeeded" : "refused", fx.reason(), fx.kernelLanguage());
        expect(ok, "an uncapped T2 device accepts the RT effects");
        fx.destroy();
    }
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mode = "hard";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        }
    }
    int rc = 0;
    if (mode == "caps_gate") {
        rc = runCapsGate();
    } else {
        Context ctx;
        const int setupRc = setup(ctx, true);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "hard") {
            rc = runHard(ctx);
        } else if (mode == "converge") {
            rc = runConverge(ctx);
        } else if (mode == "shade") {
            rc = runShade(ctx);
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
