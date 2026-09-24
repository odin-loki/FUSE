// WP-6.1 DDGI on Vulkan, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_ddgi_gpu_cpu.cpp.
//
// Scenes are the oracle's analytic boxes (DdgiCpuScene), mirrored into a GpuScene (WP-1.1: one WP-1.2 cube
// mesh, one instance + one material row per box) for the T2 tracer (WP-6.0 BLAS / TLAS, ray query) and into
// the global SDF (sdfSceneFromBoxes: the Compute agent's analytic box primitives) for the T0 tracer. Every
// frame is one render graph: gpu_scene.* -> rt.* -> ddgi.reset (first frame) / raygen / trace / blend
// [-> ddgi.probe] -> read-back copies.
//
//   --mode parity_rq   T2 (ray query), Slang and GLSL volumes side by side, 4 frames (all probes from reset;
//                      an explicit list with duplicates; the rolling schedule; a sun change): every frame
//                      (a) ray set == the oracle's rotation x spherical Fibonacci (angle <= 1e-5 rad);
//                      (b) ray results == the oracle's analytic trace (ddgi_kernel::trace_radiance on the
//                          boxes, previous volume = the GPU's previous atlases) on the GPU's own rays:
//                          distance within 1e-4 (1 + t), radiance within 1e-3 relative, on >= 99% of the
//                          rays (the rest graze an edge: triangle vs slab test, or a shadow ray at an edge);
//                      (c) atlases == the oracle's BlendKernel on the SAME ray results (runBlendReference):
//                          irradiance and distance moments within 1e-5 relative (+1e-7), update counts and
//                          fast-response counts exact;
//                      (d) Slang == GLSL (atlases within the same tolerance)
//   --mode parity_sdf  T0 (global SDF): the same with (b) against gi_gpu::sdf_trace_radiance (the T0 reference,
//                      ddgi_gpu_reference.hpp): distance / radiance within 1e-5 relative on >= 99.9% of rays
//   --mode converge    T2, T0 and the CPU oracle (DdgiCpuVolume::updateProbes) from reset, 24 updates of every
//                      probe with the same rotation sequence: per-probe mean irradiance of T2 vs T0 and of each
//                      vs the oracle within 2% of the volume's mean (documented bound); ddgi.probe at 96 surface
//                      points: T2 vs T0 within 3% of the points' mean
//   --mode leak        thin-wall leak test: a closed room (5 cm walls) in sun + sky, probes on both sides of
//                      the walls; irradiance sampled (ddgi.probe) on the inner / outer faces of the walls, T2 and
//                      T0 against the CPU oracle run on the same frames: ddgi.probe == the oracle's
//                      sample_irradiance on the read-back atlases (1e-5 relative), and the GPU's leak ratio
//                      (inner / outer) == the oracle's within 10% (the port adds no leak). The row's criterion
//                      (inner below epsilon) is NOT met by the oracle's rules (no probe relocation /
//                      classification; the ratio is reported): see the WP-6.1 row's Open list
//   --mode tod         60-step sun sweep (one update per step): temporal flicker (mean second difference of
//                      the sampled irradiance / mean irradiance) below 3% on T2, and within 25% (+0.002) of the
//                      CPU oracle's flicker on the same sweep
//   --mode shade       DDGI as indirect diffuse in the WP-2.1 light.shade: the WP-1.4 / 1.5 G-buffer of the room,
//                      lit by a sun; per lighting kernel language (Slang, GLSL) two ClusteredLighting instances
//                      (LightingFrameDesc::ddgi off / on) in one graph after DdgiGpu::addSamplingUse: the difference
//                      of their f32 dumps == albedo x (1 - metallic) x AO x E / pi with E = the oracle's
//                      sample_irradiance on the read-back atlases at the shade's reconstructed position / normal
//                      (1e-4 relative + 2e-6 of the lit value); coverage identical. (The ddgi = 0 path is the
//                      WP-2.1 / WP-3.2 shade, held by their gates.)
//   --mode zero_alloc  64 steady-state frames (T2 + T0 volumes, rolling schedule, ddgi.probe, sampling use):
//                      0 operator-new calls in DdgiGpu::beginFrame / importInto / addUpdate / addProbe /
//                      addSamplingUse, the ddgi.* pass callbacks and the whole graph build (validated run first)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_vsm_raster_common.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
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
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
[[maybe_unused]] thread_local bool t_inPass = false;
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
using namespace fuse::renderer::gi_gpu;
using namespace fuse::renderer::gpu_scene;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec2;
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
constexpr usize kStagingBytes = 8u * 1024u * 1024u;
constexpr usize kReadbackBytes = 8u * 1024u * 1024u;
constexpr u32 kMaxPoints = 4096u;
constexpr u32 kMaxVolumes = 4u;

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
    bool bindlessReady = false;
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    Buffer points{};
    Buffer pointsOut[kMaxVolumes]{};
    BindlessSlotHandle sampler{};
    u32 samplerHandle = 0;
    bool rt = false;
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
            allocator->destroyBuffer(points);
            for (Buffer& b : pointsOut) {
                allocator->destroyBuffer(b);
            }
        }
        if (device != nullptr && bindlessReady) {
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

bool makeBuffer(Context& ctx, Buffer& b, usize bytes, u32 usage, MemoryUsage memory, const char* name) {
    BufferDesc d{};
    d.size = bytes;
    d.usage = static_cast<BufferUsage>(usage);
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
}

/// 0 ok, kSkip, or 1.
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
    instanceDesc.appName = "fuse_rp_ddgi_gpu";
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
    const DdgiGpuCapabilities caps = queryDdgiGpuCapabilities(ctx.device.get());
    if (!caps.compute) {
        std::printf("SKIP: %s\n", caps.reason);
        return kSkip;
    }
    ctx.rt = caps.rayQuery;
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s; %s; ray query %s\n", ctx.device->info().deviceName.c_str(), ctx.device->info().caps.summary().c_str(),
                ctx.rt ? "yes" : "no");
    ctx.bindlessReady = ctx.bindless.init(*ctx.device, BindlessDesc{});
    if (!ctx.bindlessReady) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    const u32 storage = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress);
    bool ok = makeBuffer(ctx, ctx.staging, kStagingBytes, static_cast<u32>(BufferUsage::TransferSrc), MemoryUsage::CpuToGpu, "rp_ddgi.staging") &&
              makeBuffer(ctx, ctx.readback, kReadbackBytes, static_cast<u32>(BufferUsage::TransferDst) | storage, MemoryUsage::GpuToCpu,
                         "rp_ddgi.readback") &&
              makeBuffer(ctx, ctx.points, kMaxPoints * sizeof(DdgiProbePoint), storage, MemoryUsage::CpuToGpu, "rp_ddgi.points");
    for (Buffer& b : ctx.pointsOut) {
        ok = ok && makeBuffer(ctx, b, kMaxPoints * 16u, storage, MemoryUsage::GpuToCpu, "rp_ddgi.points_out");
    }
    if (!ok || !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: test buffers / UploadQueue\n");
        return 1;
    }
    SamplerDesc sd{};
    sd.name = "rp_ddgi.sampler";
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

// --- scenes ----------------------------------------------------------------------------------------
DdgiCpuSurface surface(f32 r, f32 g, f32 b, Vec3 emissive = {}) {
    DdgiCpuSurface s{};
    s.albedo = {r, g, b};
    s.emissive = emissive;
    return s;
}

/// Room (floor, 3 walls, an open side), a blocker and a pillar holding probes inside them, an emissive
/// panel; sun + sky (the CPU gates' scene).
DdgiCpuScene roomScene(Vec3 sun = Vec3{0.3f, 0.8f, 0.5f}) {
    DdgiCpuScene s;
    s.addBox({-4.f, -0.5f, -4.f}, {4.f, 0.f, 4.f}, surface(0.7f, 0.7f, 0.7f));
    s.addBox({-4.f, 0.f, -4.f}, {-3.8f, 3.f, 4.f}, surface(0.8f, 0.15f, 0.1f));
    s.addBox({3.8f, 0.f, -4.f}, {4.f, 3.f, 4.f}, surface(0.1f, 0.7f, 0.2f));
    s.addBox({-4.f, 0.f, -4.f}, {4.f, 3.f, -3.8f}, surface(0.7f, 0.7f, 0.7f));
    s.addBox({-1.f, 0.f, -0.5f}, {0.5f, 1.2f, 0.7f}, surface(0.7f, 0.7f, 0.7f));
    s.addBox({1.4f, 0.f, 1.f}, {2.2f, 2.1f, 1.6f}, surface(0.8f, 0.15f, 0.1f));
    s.addBox({-2.5f, 2.f, 1.5f}, {-1.5f, 2.1f, 2.5f}, surface(0.2f, 0.2f, 0.2f, {3.f, 2.5f, 2.f}));
    s.sun_direction = sun.normalized();
    s.sun_irradiance = {3.f, 2.8f, 2.5f};
    s.sky_radiance = {0.25f, 0.3f, 0.4f};
    return s;
}

DDGIDesc roomVolume() {
    DDGIDesc d{};
    d.grid_origin = {-3.f, 0.5f, -3.f};
    d.probe_spacing = {1.5f, 1.f, 1.5f};
    d.grid_dims = {5u, 3u, 5u};
    d.rays_per_probe = 64;
    d.probes_per_frame = 75;
    d.irradiance_res = 8;
    d.depth_res = 16;
    d.hysteresis = 0.9f;
    d.max_ray_distance = 12.f;
    return d;
}

/// Thin-wall leak scene: ground, a closed room x, z in [-1.5, 1.5], y in [0, 2] with 5 cm walls and roof.
DdgiCpuScene leakScene() {
    DdgiCpuScene s;
    const DdgiCpuSurface grey = surface(0.75f, 0.75f, 0.75f);
    constexpr f32 t = 0.05f;
    s.addBox({-6.f, -0.5f, -6.f}, {6.f, 0.f, 6.f}, grey);          // ground (also the room's floor)
    s.addBox({-1.5f - t, 0.f, -1.5f - t}, {-1.5f, 2.f + t, 1.5f + t}, grey); // -x wall
    s.addBox({1.5f, 0.f, -1.5f - t}, {1.5f + t, 2.f + t, 1.5f + t}, grey);   // +x wall
    s.addBox({-1.5f, 0.f, -1.5f - t}, {1.5f, 2.f + t, -1.5f}, grey);         // -z wall
    s.addBox({-1.5f, 0.f, 1.5f}, {1.5f, 2.f + t, 1.5f + t}, grey);           // +z wall
    s.addBox({-1.5f, 2.f, -1.5f}, {1.5f, 2.f + t, 1.5f}, grey);              // roof
    s.sun_direction = Vec3{-0.6f, 0.7f, -0.3f}.normalized();
    s.sun_irradiance = {4.f, 4.f, 4.f};
    s.sky_radiance = {0.4f, 0.45f, 0.5f};
    return s;
}

DDGIDesc leakVolume() {
    DDGIDesc d{};
    d.grid_origin = {-2.75f, 0.5f, -2.75f};
    d.probe_spacing = {1.1f, 1.1f, 1.1f};
    d.grid_dims = {6u, 3u, 6u}; // x, z: -2.75, -1.65 | -0.55, 0.55 | 1.65, 2.75 -- walls at |x| = 1.5..1.55
    d.rays_per_probe = 128;
    d.probes_per_frame = 108;
    d.irradiance_res = 8;
    d.depth_res = 16;
    d.hysteresis = 0.9f;
    d.max_ray_distance = 12.f;
    return d;
}

struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    DdgiCpuScene cpu;
    std::vector<fuse::compute::SdfObject> sdf;
    std::vector<DdgiSurface> surfaces;
    std::vector<InstanceHandle> handles;
};

GpuTransform boxTransform(const DdgiCpuBox& b) {
    GpuTransform t{};
    t.rows[0][0] = (b.max.x - b.min.x) * 0.5f;
    t.rows[1][1] = (b.max.y - b.min.y) * 0.5f;
    t.rows[2][2] = (b.max.z - b.min.z) * 0.5f;
    t.rows[0][3] = (b.max.x + b.min.x) * 0.5f;
    t.rows[1][3] = (b.max.y + b.min.y) * 0.5f;
    t.rows[2][3] = (b.max.z + b.min.z) * 0.5f;
    return t;
}

bool buildScene(Context& ctx, Scene& s, const DdgiCpuScene& cpu, bool sunLight = false) {
    s.cpu = cpu;
    sdfSceneFromBoxes(cpu, s.sdf, s.surfaces);
    GpuSceneDesc d{};
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
    s.meshes.resize(1);
    // The WP-1.5 test cube: counter-clockwise seen from outside (DdgiGpuTuning::frontFaceCounterClockwise).
    if (!mr_test::build(mr_test::box(), s.meshes[0]) || s.gpu.addMeshletMesh(s.meshes[0]) != 0u) {
        return false;
    }
    for (u32 i = 0; i < cpu.boxes.size(); ++i) {
        const DdgiCpuBox& b = cpu.boxes[i];
        Material m{};
        m.baseColor = b.surface.albedo;
        m.metallic = 0.f;
        m.roughness = 0.8f;
        m.emissiveColor = b.surface.emissive;
        m.emissiveIntensity = 1.f;
        s.gpu.setMaterial(i, m);
        InstanceDesc id{};
        id.mesh = 0;
        id.material = i;
        id.transform = boxTransform(b);
        s.handles.push_back(s.gpu.addInstance(id));
    }
    if (sunLight) {
        GpuLight sun{};
        sun.type = static_cast<u32>(GpuLightType::Directional);
        sun.direction[0] = -cpu.sun_direction.x;
        sun.direction[1] = -cpu.sun_direction.y;
        sun.direction[2] = -cpu.sun_direction.z;
        sun.intensity = 2.f;
        s.gpu.addLight(sun);
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

// --- volumes ---------------------------------------------------------------------------------------
struct Volume {
    DdgiGpu gpu;
    const char* label = "";
    bool rq = false;
    bool active = false;
    BlendReferenceState prev; ///< the GPU atlases before this frame's update (read back last frame)
    u64 rbOffset = 0;         ///< work buffer copy in ctx.readback
};

bool initVolume(Context& ctx, Scene& s, Volume& v, const DDGIDesc& desc, DdgiTracer tracer, DdgiKernelLanguage language,
                const char* label) {
    DdgiGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.volume = desc;
    d.tracer = tracer;
    d.language = language;
    v.label = label;
    v.rq = tracer == DdgiTracer::RayQuery;
    v.active = v.gpu.init(d);
    if (!v.active) {
        std::printf("  %s: not available (%s)\n", label, v.gpu.reason());
        return false;
    }
    if (!v.rq && !v.gpu.setSdfScene(s.sdf.data(), static_cast<u32>(s.sdf.size()), s.surfaces.data(), static_cast<u32>(s.surfaces.size()))) {
        std::fprintf(stderr, "FAIL: setSdfScene\n");
        v.active = false;
        return false;
    }
    initialVolumeState(desc, d.config, v.prev);
    std::printf("  %s: %s, %s kernels\n", label, v.gpu.tracerName(), v.gpu.kernelLanguage());
    return true;
}

// --- frame -----------------------------------------------------------------------------------------
struct CopyRecord {
    rg::BufferRef src;
    rg::BufferRef dst;
    u64 dstOffset = 0;
    u64 bytes = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    const VkBufferCopy region{0, c.dstOffset, c.bytes};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(c.src)),
                    static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

struct Frame {
    rg::Graph graph;
    CopyRecord copies[kMaxVolumes]{};
    u32 copyCount = 0;
};

struct FrameIo {
    bool readback = true;
    u32 points = 0; ///< ddgi.probe points at ctx.points (per volume output ctx.pointsOut[i])
};

void beginSceneFrame(Context& ctx, Scene& s, rt::AccelerationStructures* rt) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    if (rt != nullptr) {
        rt->beginFrame(ctx.serial);
    }
}

bool beginVolumes(Context& ctx, Scene& s, rt::AccelerationStructures* rt, Volume* const* vols, u32 n, DdgiFrameDesc fd) {
    fd.sunDirection = s.cpu.sun_direction;
    fd.sunIrradiance = s.cpu.sun_irradiance;
    fd.skyRadiance = s.cpu.sky_radiance;
    fd.tlasAddress = rt != nullptr ? rt->tlasAddress() : 0u;
    fd.sceneAddress = s.gpu.headerAddress();
    bool ok = true;
    for (u32 i = 0; i < n; ++i) {
        if (vols[i]->active) {
            ok = vols[i]->gpu.beginFrame(ctx.serial, fd) && ok;
        }
    }
    return ok;
}

/// ddgi.* passes of every volume (+ probe + read-back copies) after the scene / rt imports.
void addVolumes(Context& ctx, Frame& f, const GpuSceneGraphRefs& sceneRefs, const rt::RtGraphRefs& rtRefs, Volume* const* vols, u32 n,
                const FrameIo& io) {
    rg::Graph& graph = f.graph;
    rg::BufferRef points{};
    if (io.points > 0u) {
        points = graph.importBuffer(rg::ImportedBuffer{ctx.points.handle, ctx.points.desc.size, rg::kNoQueue, nullptr, "rp_ddgi.points"});
    }
    const rg::BufferRef rb =
        io.readback ? graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr, "rp_ddgi.readback"})
                    : rg::BufferRef{};
    u64 cursor = 0;
    for (u32 i = 0; i < n; ++i) {
        Volume& v = *vols[i];
        if (!v.active) {
            continue;
        }
        const DdgiGraphRefs refs = v.gpu.importInto(graph);
        expect(v.gpu.addUpdate(graph, refs, &rtRefs, &sceneRefs), "addUpdate");
        if (io.points > 0u) {
            const rg::BufferRef out = graph.importBuffer(
                rg::ImportedBuffer{ctx.pointsOut[i].handle, ctx.pointsOut[i].desc.size, rg::kNoQueue, nullptr, "rp_ddgi.points_out"});
            expect(v.gpu.addProbe(graph, refs, points, ctx.points.deviceAddress, out, ctx.pointsOut[i].deviceAddress, io.points), "addProbe");
            if (io.readback) {
                graph.addPass("readback.points", nullptr, nullptr).use(out, rg::Access::HostRead);
            }
        }
        v.gpu.addSamplingUse(graph, refs, rg::kStageCompute);
        if (io.readback) {
            CopyRecord& c = f.copies[f.copyCount++];
            c = CopyRecord{refs.work, rb, cursor, v.gpu.workBuffer().desc.size};
            v.rbOffset = cursor;
            cursor = (cursor + c.bytes + 255u) & ~u64{255u};
            graph.addPass("readback.copy", &recordCopy, &c)
                .use(refs.work, rg::Access::TransferSrc)
                .use(rb, rg::Access::TransferDst, rg::BufferRange{c.dstOffset, c.bytes});
        }
    }
    expect(cursor <= kReadbackBytes, "read-back buffer large enough");
    if (io.readback) {
        graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
    }
}

bool runFrame(Context& ctx, Scene& s, rt::AccelerationStructures* rt, Volume* const* vols, u32 n, const DdgiFrameDesc& fd, const FrameIo& io,
              Frame& f) {
    beginSceneFrame(ctx, s, rt);
    const GpuSceneCommitStats stats = s.gpu.commit();
    const rt::RtCommitStats rs = rt != nullptr ? rt->commit() : rt::RtCommitStats{};
    ctx.upload.flush();
    if (!beginVolumes(ctx, s, rt, vols, n, fd)) {
        std::fprintf(stderr, "  ddgi beginFrame failed\n");
        return false;
    }
    f.graph.reset();
    f.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(f.graph);
    const rt::RtGraphRefs rtRefs = rt != nullptr ? rt->importInto(f.graph, sceneRefs) : rt::RtGraphRefs{};
    addVolumes(ctx, f, sceneRefs, rtRefs, vols, n, io);
    const rg::ExecuteResult result = ctx.executor->execute(f.graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    s.gpu.collectRetired(ctx.serial);
    if (rt != nullptr) {
        rt->collectRetired(ctx.serial);
    }
    ctx.bindless.collectRetired(ctx.serial);
    return stats.ok && rs.ok && result.ok && waited;
}

int initRt(Context& ctx, Scene& s, rt::AccelerationStructures& rt) {
    if (!ctx.rt) {
        return kSkip;
    }
    rt::AccelerationStructuresDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.scene = &s.gpu;
    d.instanceCapacity = 64;
    d.meshCapacity = 4;
    if (!rt.init(d)) {
        std::printf("  acceleration structures unavailable: %s\n", rt.reason());
        return kSkip;
    }
    return 0;
}

// --- read-back ------------------------------------------------------------------------------------
struct GpuState {
    std::vector<Vec3> dirs;
    std::vector<Vec3> radiance;
    std::vector<f32> distance;
    std::vector<u32> slotStats;
    BlendReferenceState state;
};

void readState(Context& ctx, const Volume& v, GpuState& out) {
    const u8* base = static_cast<const u8*>(ctx.readback.mapped) + v.rbOffset;
    const DdgiWorkLayout& l = v.gpu.layout();
    const DDGIDesc& d = v.gpu.desc().volume;
    const u32 rays = d.rays_per_probe;
    const u32 scheduled = v.gpu.scheduled();
    const u32 probes = v.gpu.probeCount();
    out.dirs.resize(rays);
    for (u32 r = 0; r < rays; ++r) {
        f32 w[4];
        std::memcpy(w, base + l.rayDirs + r * 16u, 16u);
        out.dirs[r] = {w[0], w[1], w[2]};
    }
    out.radiance.resize(static_cast<usize>(scheduled) * rays);
    out.distance.resize(static_cast<usize>(scheduled) * rays);
    for (usize i = 0; i < out.radiance.size(); ++i) {
        f32 w[4];
        std::memcpy(w, base + l.rays + i * 16u, 16u);
        out.radiance[i] = {w[0], w[1], w[2]};
        out.distance[i] = w[3];
    }
    out.slotStats.resize(scheduled);
    std::memcpy(out.slotStats.data(), base + l.slotStats, scheduled * sizeof(u32));
    out.state.irradiance.resize(l.irradianceBytes / sizeof(Vec3));
    std::memcpy(out.state.irradiance.data(), base + l.irradiance, l.irradianceBytes);
    out.state.distance.resize(l.distanceBytes / sizeof(Vec2));
    std::memcpy(out.state.distance.data(), base + l.distance, l.distanceBytes);
    out.state.updateCounts.resize(probes);
    std::memcpy(out.state.updateCounts.data(), base + l.updateCounts, probes * sizeof(u32));
}

f64 relErr(f64 a, f64 b, f64 floor = 1e-6) {
    return std::fabs(a - b) / std::max(std::max(std::fabs(a), std::fabs(b)), floor);
}

struct AtlasDiff {
    f64 maxIrr = 0.0;
    f64 maxDist = 0.0;
    u32 irrBad = 0;
    u32 distBad = 0;
    u32 countBad = 0;
    u32 irrExact = 0;
    u32 irrTotal = 0;
};

/// Atlases within tol relative (+1e-7 absolute); counts exact.
AtlasDiff compareAtlases(const BlendReferenceState& a, const BlendReferenceState& b, f64 tol) {
    AtlasDiff d{};
    for (usize i = 0; i < a.irradiance.size() && i < b.irradiance.size(); ++i) {
        const f32 ga[3] = {a.irradiance[i].x, a.irradiance[i].y, a.irradiance[i].z};
        const f32 gb[3] = {b.irradiance[i].x, b.irradiance[i].y, b.irradiance[i].z};
        for (u32 c = 0; c < 3u; ++c) {
            const f64 diff = std::fabs(static_cast<f64>(ga[c]) - gb[c]);
            d.maxIrr = std::max(d.maxIrr, relErr(ga[c], gb[c]));
            d.irrBad += diff > tol * std::fabs(static_cast<f64>(gb[c])) + 1e-7 ? 1u : 0u;
            d.irrExact += ga[c] == gb[c] ? 1u : 0u;
            ++d.irrTotal;
        }
    }
    for (usize i = 0; i < a.distance.size() && i < b.distance.size(); ++i) {
        const f32 ga[2] = {a.distance[i].x, a.distance[i].y};
        const f32 gb[2] = {b.distance[i].x, b.distance[i].y};
        for (u32 c = 0; c < 2u; ++c) {
            const f64 diff = std::fabs(static_cast<f64>(ga[c]) - gb[c]);
            d.maxDist = std::max(d.maxDist, relErr(ga[c], gb[c]));
            d.distBad += diff > tol * std::fabs(static_cast<f64>(gb[c])) + 1e-7 ? 1u : 0u;
        }
    }
    for (usize i = 0; i < a.updateCounts.size() && i < b.updateCounts.size(); ++i) {
        d.countBad += a.updateCounts[i] != b.updateCounts[i] ? 1u : 0u;
    }
    d.countBad += a.updateCounts.size() != b.updateCounts.size() ? 1u : 0u;
    return d;
}

// --- parity ----------------------------------------------------------------------------------------
constexpr f64 kBlendTol = 1e-5;

struct TraceReport {
    u32 rays = 0;
    u32 outliers = 0;
    f64 maxDist = 0.0; ///< over matching rays
    f64 maxRad = 0.0;
    u32 hits = 0;
    u32 misses = 0;
    u32 backfaces = 0;
};

SdfTraceScene sdfScene(const Scene& s, const DdgiGpu& g, const BlendReferenceState& prev) {
    const DdgiGpuDesc& d = g.desc();
    SdfTraceScene t{};
    t.objects = s.sdf.data();
    t.objectCount = static_cast<u32>(s.sdf.size());
    t.surfaces = s.surfaces.data();
    t.surfaceCount = static_cast<u32>(s.surfaces.size());
    t.sunDirection = s.cpu.sun_direction;
    t.sunIrradiance = s.cpu.sun_irradiance;
    t.skyRadiance = s.cpu.sky_radiance;
    t.maxDistance = d.volume.max_ray_distance;
    t.minDistance = d.tuning.sdfMinDistance;
    t.maxSteps = d.tuning.sdfMaxSteps;
    t.shadowBias = d.tuning.sdfShadowBias;
    t.backfaceDistanceScale = d.config.backface_distance_scale;
    t.multiBounce = d.config.multi_bounce;
    t.volume = volumeView(d.volume, d.config, prev);
    return t;
}

TraceReport checkTrace(const Scene& s, const Volume& v, const GpuState& g, f64 distTol, f64 radTol) {
    TraceReport r{};
    const DdgiGpuDesc& d = v.gpu.desc();
    const u32 rays = d.volume.rays_per_probe;
    ddgi_kernel::TraceParams tp{};
    tp.scene.boxes = s.cpu.boxes.data();
    tp.scene.box_count = static_cast<u32>(s.cpu.boxes.size());
    tp.scene.sun_direction = s.cpu.sun_direction;
    tp.scene.sun_irradiance = s.cpu.sun_irradiance;
    tp.scene.sky_radiance = s.cpu.sky_radiance;
    tp.volume = volumeView(d.volume, d.config, v.prev);
    tp.backface_distance_scale = d.config.backface_distance_scale;
    tp.multi_bounce = d.config.multi_bounce;
    const SdfTraceScene sdf = sdfScene(s, v.gpu, v.prev);
    for (u32 slot = 0; slot < v.gpu.scheduled(); ++slot) {
        const Vec3 origin = ddgi_kernel::probe_world_position(d.volume, v.gpu.schedule()[slot]);
        for (u32 ray = 0; ray < rays; ++ray) {
            const usize i = static_cast<usize>(slot) * rays + ray;
            f32 dist = 0.f;
            const Vec3 ref = v.rq ? ddgi_kernel::trace_radiance(tp, origin, g.dirs[ray], dist)
                                  : sdf_trace_radiance(sdf, origin, g.dirs[ray], dist);
            ++r.rays;
            const bool miss = dist == d.volume.max_ray_distance;
            const bool back = !miss && ref.x == 0.f && ref.y == 0.f && ref.z == 0.f;
            r.misses += miss ? 1u : 0u;
            r.backfaces += back ? 1u : 0u;
            r.hits += !miss && !back ? 1u : 0u;
            const f64 dd = std::fabs(static_cast<f64>(g.distance[i]) - dist);
            const f64 rr = std::max({relErr(g.radiance[i].x, ref.x, 1e-3), relErr(g.radiance[i].y, ref.y, 1e-3),
                                     relErr(g.radiance[i].z, ref.z, 1e-3)});
            if (dd > distTol * (1.0 + dist) || rr > radTol) {
                ++r.outliers;
                continue;
            }
            r.maxDist = std::max(r.maxDist, dd);
            r.maxRad = std::max(r.maxRad, rr);
        }
    }
    return r;
}

/// (a) ray set, (b) trace, (c) blend on the same ray results; then prev <- the GPU atlases.
bool checkVolume(Context& ctx, const Scene& s, Volume& v, u32 frameIndex, GpuState& g, f64 distTol, f64 radTol, f64 minMatch) {
    readState(ctx, v, g);
    const DdgiGpuDesc& d = v.gpu.desc();
    const u32 rays = d.volume.rays_per_probe;
    // (a) ray set.
    const DdgiRayRotation rot = ddgi_cpu::updateRotation(d.config.rotation_seed, frameIndex);
    f64 maxAngle = 0.0;
    for (u32 r = 0; r < rays; ++r) {
        const Vec3 o = rot.apply(ddgi_cpu::sphericalFibonacci(r, rays)).normalized();
        // Chord length in f64 (== the angle for small angles; acos of an f32 dot would bottom out at ~3e-4).
        const f64 dx = static_cast<f64>(o.x) - g.dirs[r].x;
        const f64 dy = static_cast<f64>(o.y) - g.dirs[r].y;
        const f64 dz = static_cast<f64>(o.z) - g.dirs[r].z;
        maxAngle = std::max(maxAngle, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    // (b) trace.
    const TraceReport tr = checkTrace(s, v, g, distTol, radTol);
    // (c) blend on the same ray results.
    BlendReferenceState ref = v.prev;
    ref.fastResponseTexels = 0;
    BlendReferenceInput in{};
    in.volume = &d.volume;
    in.config = &d.config;
    in.schedule = v.gpu.schedule();
    in.scheduled = v.gpu.scheduled();
    in.rayDirs = g.dirs.data();
    in.radiance = g.radiance.data();
    in.distance = g.distance.data();
    in.irradianceTexelDirs = v.gpu.irradianceTexelDirs().data();
    in.distanceTexelDirs = v.gpu.distanceTexelDirs().data();
    const bool ran = runBlendReference(in, ref);
    const AtlasDiff ad = compareAtlases(g.state, ref, kBlendTol);
    u32 fast = 0;
    for (u32 x : g.slotStats) {
        fast += x;
    }
    std::printf("    %-12s frame %u: %u probes; dirs max angle %.2e rad; trace %u rays (%u hit, %u miss, %u backface), %u outliers, "
                "max |dd| %.2e, max rad rel %.2e; blend: irradiance max rel %.2e (%u / %u exact), distance max rel %.2e, "
                "%u / %u / %u beyond tol, fast texels %u / %u\n",
                v.label, frameIndex, v.gpu.scheduled(), maxAngle, tr.rays, tr.hits, tr.misses, tr.backfaces, tr.outliers, tr.maxDist, tr.maxRad,
                ad.maxIrr, ad.irrExact, ad.irrTotal, ad.maxDist, ad.irrBad, ad.distBad, ad.countBad, fast, ref.fastResponseTexels);
    expect(ran, "runBlendReference ran");
    expect(maxAngle <= 1e-5, "(a) GPU ray set == the oracle's rotation x spherical Fibonacci (1e-5 rad)");
    expect(static_cast<f64>(tr.rays - tr.outliers) >= minMatch * tr.rays, "(b) ray results == the CPU trace reference");
    expect(ad.irrBad == 0u && ad.distBad == 0u, "(c) atlases == the oracle's blend on the same ray results");
    expect(ad.countBad == 0u, "(c) update counts == the oracle's");
    expect(fast == ref.fastResponseTexels, "(c) fast-response texel count == the oracle's");
    v.prev = g.state;
    return true;
}

int runParity(Context& ctx, bool rq) {
    if (rq && !ctx.rt) {
        std::printf("SKIP: T2 gate (no ray query on this device)\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, roomScene())) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rt::AccelerationStructures rt;
    if (rq && initRt(ctx, s, rt) != 0) {
        return kSkip;
    }
    const DDGIDesc desc = roomVolume();
    Volume vols[2];
    const DdgiTracer tracer = rq ? DdgiTracer::RayQuery : DdgiTracer::Sdf;
    initVolume(ctx, s, vols[0], desc, tracer, DdgiKernelLanguage::Slang, rq ? "T2 slang" : "T0 slang");
    initVolume(ctx, s, vols[1], desc, tracer, DdgiKernelLanguage::Glsl, rq ? "T2 glsl" : "T0 glsl");
    if (!vols[0].active && !vols[1].active) {
        std::printf("SKIP: no DDGI kernel built\n");
        return kSkip;
    }
    Volume* ptrs[2] = {&vols[0], &vols[1]};
    Frame f;
    // Tolerances of (b): T2 compares triangles (ray query) with the oracle's slab test; T0 compares the same
    // sphere trace in f32 (device sqrt / division rounding may move a step by an ulp).
    const f64 distTol = rq ? 1e-4 : 1e-5;
    const f64 radTol = rq ? 1e-3 : 1e-5;
    const f64 minMatch = rq ? 0.99 : 0.999;
    const u32 explicitList[] = {3, 7, 7, 11, 40, 41, 42, 3, 60, 74, 0, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                                27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 50};
    GpuState g;
    for (u32 frame = 0; frame < 4u; ++frame) {
        DdgiFrameDesc fd{};
        fd.frameIndex = frame;
        if (frame == 1u) {
            fd.probes = explicitList;
            fd.probeCount = static_cast<u32>(sizeof(explicitList) / sizeof(explicitList[0]));
        }
        if (frame == 3u) {
            s.cpu.sun_direction = Vec3{-0.5f, 0.6f, 0.4f}.normalized(); // a lighting change (probe-level detection)
        }
        FrameIo io{};
        if (!runFrame(ctx, s, rq ? &rt : nullptr, ptrs, 2, fd, io, f)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
        if (frame == 1u) {
            expect(vols[0].gpu.stats().duplicatesDropped == 2u && vols[0].gpu.scheduled() == 38u, "explicit schedule deduplicated");
        }
        expect(frame != 0u || vols[0].gpu.stats().reset, "first frame resets the atlases");
        GpuState states[2];
        for (u32 k = 0; k < 2u; ++k) {
            if (vols[k].active) {
                checkVolume(ctx, s, vols[k], frame, states[k], distTol, radTol, minMatch);
            }
        }
        if (vols[0].active && vols[1].active) {
            const AtlasDiff lang = compareAtlases(states[0].state, states[1].state, kBlendTol);
            u32 rayDiff = 0;
            for (usize i = 0; i < states[0].radiance.size(); ++i) {
                rayDiff += (states[0].radiance[i].x != states[1].radiance[i].x || states[0].distance[i] != states[1].distance[i]) ? 1u : 0u;
            }
            std::printf("    slang vs glsl frame %u: atlas max rel %.2e / %.2e (%u / %u exact), %u ray results differ\n", frame, lang.maxIrr,
                        lang.maxDist, lang.irrExact, lang.irrTotal, rayDiff);
            expect(lang.irrBad == 0u && lang.distBad == 0u && lang.countBad == 0u, "(d) Slang == GLSL atlases");
        }
    }
    for (Volume& v : vols) {
        v.gpu.destroy();
    }
    rt.destroy();
    s.gpu.destroy();
    return 0;
}

// --- converge ---------------------------------------------------------------------------------------
std::vector<DdgiProbePoint> roomPoints() {
    std::vector<DdgiProbePoint> pts;
    for (u32 i = 0; i < 48u; ++i) { // floor
        DdgiProbePoint p{};
        p.position[0] = -3.5f + 7.f * static_cast<f32>(i % 8u) / 7.f;
        p.position[1] = 0.f;
        p.position[2] = -3.5f + 7.f * static_cast<f32>(i / 8u) / 5.f;
        p.normal[1] = 1.f;
        pts.push_back(p);
    }
    for (u32 i = 0; i < 24u; ++i) { // left wall (inner face x = -3.8, normal +x)
        DdgiProbePoint p{};
        p.position[0] = -3.8f;
        p.position[1] = 0.3f + 2.4f * static_cast<f32>(i % 4u) / 3.f;
        p.position[2] = -3.5f + 7.f * static_cast<f32>(i / 4u) / 5.f;
        p.normal[0] = 1.f;
        p.normal[1] = 0.f;
        pts.push_back(p);
    }
    for (u32 i = 0; i < 24u; ++i) { // back wall (z = -3.8, normal +z)
        DdgiProbePoint p{};
        p.position[0] = -3.5f + 7.f * static_cast<f32>(i % 6u) / 5.f;
        p.position[1] = 0.3f + 2.4f * static_cast<f32>(i / 6u) / 3.f;
        p.position[2] = -3.8f;
        p.normal[1] = 0.f;
        p.normal[2] = 1.f;
        pts.push_back(p);
    }
    return pts;
}

void uploadPoints(Context& ctx, const std::vector<DdgiProbePoint>& pts) {
    std::memcpy(ctx.points.mapped, pts.data(), pts.size() * sizeof(DdgiProbePoint));
}

Vec3 pointOut(Context& ctx, u32 volume, u32 i) {
    f32 w[4];
    std::memcpy(w, static_cast<const u8*>(ctx.pointsOut[volume].mapped) + i * 16u, 16u);
    return {w[0], w[1], w[2]};
}

f64 luminance(const Vec3& e) {
    return 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z;
}

int runConverge(Context& ctx) {
    if (!ctx.rt) {
        std::printf("SKIP: T2 gate (no ray query on this device)\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, roomScene())) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rt::AccelerationStructures rt;
    if (initRt(ctx, s, rt) != 0) {
        return kSkip;
    }
    const DDGIDesc desc = roomVolume();
    Volume vols[2];
    if (!initVolume(ctx, s, vols[0], desc, DdgiTracer::RayQuery, DdgiKernelLanguage::Auto, "T2") ||
        !initVolume(ctx, s, vols[1], desc, DdgiTracer::Sdf, DdgiKernelLanguage::Auto, "T0")) {
        std::printf("SKIP: DDGI kernels not built\n");
        return kSkip;
    }
    DdgiCpuVolume oracle;
    if (!oracle.init(desc)) {
        std::fprintf(stderr, "FAIL: oracle init\n");
        return 1;
    }
    const std::vector<DdgiProbePoint> pts = roomPoints();
    uploadPoints(ctx, pts);
    Volume* ptrs[2] = {&vols[0], &vols[1]};
    Frame f;
    std::vector<u32> all(oracle.probeCount());
    for (u32 i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    constexpr u32 kFrames = 24;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        DdgiFrameDesc fd{};
        fd.frameIndex = frame;
        FrameIo io{};
        io.points = frame + 1u == kFrames ? static_cast<u32>(pts.size()) : 0u;
        if (!runFrame(ctx, s, &rt, ptrs, 2, fd, io, f)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
        oracle.updateProbes(s.cpu, all.data(), static_cast<u32>(all.size()), frame);
    }
    GpuState g[2];
    readState(ctx, vols[0], g[0]);
    readState(ctx, vols[1], g[1]);
    const u32 ir = desc.irradiance_res;
    auto probeMean = [&](const BlendReferenceState& st, u32 p) {
        f64 sum = 0.0;
        for (u32 y = 1; y <= ir; ++y) {
            for (u32 x = 1; x <= ir; ++x) {
                sum += luminance(st.irradiance[static_cast<usize>(p) * (ir + 2u) * (ir + 2u) + y * (ir + 2u) + x]);
            }
        }
        return sum / (ir * ir);
    };
    f64 volumeMean = 0.0;
    for (u32 p = 0; p < oracle.probeCount(); ++p) {
        volumeMean += luminance(oracle.probeMeanTexel(p));
    }
    volumeMean /= oracle.probeCount();
    f64 t2t0 = 0.0, t2o = 0.0, t0o = 0.0;
    for (u32 p = 0; p < oracle.probeCount(); ++p) {
        const f64 a = probeMean(g[0].state, p);
        const f64 b = probeMean(g[1].state, p);
        const f64 o = luminance(oracle.probeMeanTexel(p));
        t2t0 = std::max(t2t0, std::fabs(a - b) / volumeMean);
        t2o = std::max(t2o, std::fabs(a - o) / volumeMean);
        t0o = std::max(t0o, std::fabs(b - o) / volumeMean);
    }
    f64 pointMean = 0.0, pointDiff = 0.0, pointOracle = 0.0;
    for (u32 i = 0; i < pts.size(); ++i) {
        const Vec3 pos{pts[i].position[0], pts[i].position[1], pts[i].position[2]};
        const Vec3 n{pts[i].normal[0], pts[i].normal[1], pts[i].normal[2]};
        const f64 a = luminance(pointOut(ctx, 0, i));
        const f64 b = luminance(pointOut(ctx, 1, i));
        const f64 o = luminance(oracle.sampleIrradiance(pos, n));
        pointMean += o;
        pointDiff = std::max(pointDiff, std::fabs(a - b));
        pointOracle = std::max(pointOracle, std::max(std::fabs(a - o), std::fabs(b - o)));
    }
    pointMean /= static_cast<f64>(pts.size());
    std::printf("  after %u updates: volume mean luminance %.4f; per-probe max |dE| / mean: T2-T0 %.4f, T2-oracle %.4f, T0-oracle %.4f\n"
                "  %zu surface points (mean E %.4f): max |E_T2 - E_T0| / mean %.3e, max |E_gpu - E_oracle| / mean %.4f\n",
                kFrames, volumeMean, t2t0, t2o, t0o, pts.size(), pointMean, pointDiff / pointMean, pointOracle / pointMean);
    expect(volumeMean > 0.05, "the volume is lit");
    // Bounds (documented in the execution doc): T2 vs T0 differ only on rays grazing an edge (triangles vs the
    // SDF's minDistance) -> per-probe 2%, surface points 0.1%. Against the oracle both GPU paths also see the
    // f32 ray set (<= 4e-7 rad from the oracle's f64 one), which flips a few discrete change-detection
    // decisions (hysteresis 0.9 vs 0.15 for a texel) -> per-probe 2%, surface points 5%.
    expect(t2t0 <= 0.02 && t2o <= 0.02 && t0o <= 0.02, "T2, T0 and the oracle converge to the same probes (2% of the mean)");
    expect(pointDiff <= 1e-3 * pointMean, "sampled irradiance: T2 == T0 within 0.1% of the mean");
    expect(pointOracle <= 0.05 * pointMean, "sampled irradiance: GPU == oracle within 5% of the mean");
    for (Volume& v : vols) {
        v.gpu.destroy();
    }
    rt.destroy();
    s.gpu.destroy();
    return 0;
}

// --- leak -------------------------------------------------------------------------------------------
int runLeak(Context& ctx) {
    Scene s;
    if (!buildScene(ctx, s, leakScene())) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rt::AccelerationStructures rt;
    const bool haveRt = initRt(ctx, s, rt) == 0;
    const DDGIDesc desc = leakVolume();
    Volume vols[2];
    if (haveRt) {
        initVolume(ctx, s, vols[0], desc, DdgiTracer::RayQuery, DdgiKernelLanguage::Auto, "T2");
    }
    initVolume(ctx, s, vols[1], desc, DdgiTracer::Sdf, DdgiKernelLanguage::Auto, "T0");
    if (!vols[1].active) {
        std::printf("SKIP: DDGI kernels not built\n");
        return kSkip;
    }
    // Points on the four walls: the inner face (normal into the room) and the outer face, 3 heights x 5.
    std::vector<DdgiProbePoint> pts;
    std::vector<u8> inner;
    constexpr f32 t = 0.05f;
    for (u32 wall = 0; wall < 4u; ++wall) {
        for (u32 side = 0; side < 2u; ++side) {
            for (u32 k = 0; k < 15u; ++k) {
                const f32 a = -1.2f + 2.4f * static_cast<f32>(k % 5u) / 4.f;
                const f32 y = 0.3f + 1.4f * static_cast<f32>(k / 5u) / 2.f;
                const f32 sgn = wall % 2u == 0u ? -1.f : 1.f;
                const f32 face = side == 0u ? 1.5f : 1.5f + t; // inner / outer face distance from the centre
                const f32 nrm = side == 0u ? -sgn : sgn;
                DdgiProbePoint p{};
                p.position[1] = y;
                p.normal[1] = 0.f;
                if (wall < 2u) {
                    p.position[0] = sgn * face;
                    p.position[2] = a;
                    p.normal[0] = nrm;
                } else {
                    p.position[0] = a;
                    p.position[2] = sgn * face;
                    p.normal[2] = nrm;
                }
                pts.push_back(p);
                inner.push_back(side == 0u ? 1u : 0u);
            }
        }
    }
    uploadPoints(ctx, pts);
    Volume* ptrs[2] = {&vols[0], &vols[1]};
    Frame f;
    constexpr u32 kFrames = 16;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        DdgiFrameDesc fd{};
        fd.frameIndex = frame;
        FrameIo io{};
        io.points = frame + 1u == kFrames ? static_cast<u32>(pts.size()) : 0u;
        if (!runFrame(ctx, s, haveRt ? &rt : nullptr, ptrs, 2, fd, io, f)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
    }
    // The CPU oracle on the same frames (same rotation sequence, the oracle's analytic boxes).
    DdgiCpuVolume oracle;
    oracle.init(desc);
    std::vector<u32> all(oracle.probeCount());
    for (u32 i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    for (u32 frame = 0; frame < kFrames; ++frame) {
        oracle.updateProbes(s.cpu, all.data(), static_cast<u32>(all.size()), frame);
    }
    auto ratio = [&](auto&& sample, f64& outerMean, f64& innerMax) {
        f64 in = 0.0, out = 0.0;
        u32 nIn = 0, nOut = 0;
        innerMax = 0.0;
        for (u32 i = 0; i < pts.size(); ++i) {
            const f64 l = luminance(sample(i));
            if (inner[i] != 0u) {
                in += l;
                innerMax = std::max(innerMax, l);
                ++nIn;
            } else {
                out += l;
                ++nOut;
            }
        }
        outerMean = out / nOut;
        innerMax /= outerMean;
        return (in / nIn) / outerMean;
    };
    f64 oracleOuter = 0.0, oracleMax = 0.0;
    const f64 oracleRatio = ratio(
        [&](u32 i) {
            return oracle.sampleIrradiance({pts[i].position[0], pts[i].position[1], pts[i].position[2]},
                                           {pts[i].normal[0], pts[i].normal[1], pts[i].normal[2]});
        },
        oracleOuter, oracleMax);
    std::printf("  oracle: outer faces mean E %.4f, leak ratio inner / outer mean %.4f, max %.4f\n", oracleOuter, oracleRatio, oracleMax);
    for (u32 k = 0; k < 2u; ++k) {
        Volume& v = vols[k];
        if (!v.active) {
            continue;
        }
        GpuState g;
        readState(ctx, v, g);
        const ddgi_kernel::VolumeView view = volumeView(desc, v.gpu.desc().config, g.state);
        f64 sampleErr = 0.0;
        for (u32 i = 0; i < pts.size(); ++i) {
            const Vec3 e = pointOut(ctx, k, i);
            const Vec3 pos{pts[i].position[0], pts[i].position[1], pts[i].position[2]};
            const Vec3 n{pts[i].normal[0], pts[i].normal[1], pts[i].normal[2]};
            const Vec3 ref = ddgi_kernel::sample_irradiance(view, pos, n);
            sampleErr = std::max({sampleErr, relErr(e.x, ref.x, 1e-4), relErr(e.y, ref.y, 1e-4), relErr(e.z, ref.z, 1e-4)});
        }
        f64 outer = 0.0, innerMax = 0.0;
        const f64 r = ratio([&](u32 i) { return pointOut(ctx, k, i); }, outer, innerMax);
        std::printf("  %s: outer faces mean E %.4f, leak ratio inner / outer mean %.4f, max %.4f; ddgi.probe vs oracle sample on the "
                    "same atlases max rel %.2e\n",
                    v.label, outer, r, innerMax, sampleErr);
        expect(outer > 0.1, "outer faces are lit");
        expect(sampleErr <= 1e-5, "ddgi.probe == the oracle's sample_irradiance on the same atlases");
        expect(std::fabs(r - oracleRatio) <= 0.1 * oracleRatio + 1e-3 && std::fabs(outer - oracleOuter) <= 0.05 * oracleOuter,
               "the GPU volume leaks exactly as much as the oracle's (no leak added by the port)");
        v.gpu.destroy();
    }
    rt.destroy();
    s.gpu.destroy();
    return 0;
}

// --- tod --------------------------------------------------------------------------------------------
int runTod(Context& ctx) {
    if (!ctx.rt) {
        std::printf("SKIP: T2 gate (no ray query on this device)\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, roomScene())) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rt::AccelerationStructures rt;
    if (initRt(ctx, s, rt) != 0) {
        return kSkip;
    }
    DDGIDesc desc = roomVolume();
    desc.hysteresis = 0.97f; // the B5 default
    Volume vol;
    if (!initVolume(ctx, s, vol, desc, DdgiTracer::RayQuery, DdgiKernelLanguage::Auto, "T2")) {
        std::printf("SKIP: DDGI kernels not built\n");
        return kSkip;
    }
    DdgiCpuVolume oracle;
    oracle.init(desc);
    const std::vector<DdgiProbePoint> pts = roomPoints();
    uploadPoints(ctx, pts);
    Volume* ptrs[1] = {&vol};
    Frame f;
    std::vector<u32> all(oracle.probeCount());
    for (u32 i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    constexpr u32 kWarm = 20;
    constexpr u32 kSteps = 60;
    std::vector<std::vector<f64>> gpuE, cpuE;
    for (u32 step = 0; step < kWarm + kSteps; ++step) {
        const u32 k = step < kWarm ? 0u : step - kWarm;
        const f64 phase = 0.25 + 2.3 * static_cast<f64>(k) / kSteps; // sun elevation sweep, east -> west
        s.cpu.sun_direction = Vec3{static_cast<f32>(std::cos(phase) * 0.8), static_cast<f32>(std::sin(phase)), 0.35f}.normalized();
        DdgiFrameDesc fd{};
        fd.frameIndex = step;
        FrameIo io{};
        io.points = static_cast<u32>(pts.size());
        io.readback = false;
        if (!runFrame(ctx, s, &rt, ptrs, 1, fd, io, f)) {
            std::fprintf(stderr, "FAIL: step %u\n", step);
            return 1;
        }
        oracle.updateProbes(s.cpu, all.data(), static_cast<u32>(all.size()), step);
        if (step < kWarm) {
            continue;
        }
        std::vector<f64> ge(pts.size()), ce(pts.size());
        for (u32 i = 0; i < pts.size(); ++i) {
            const Vec3 pos{pts[i].position[0], pts[i].position[1], pts[i].position[2]};
            const Vec3 n{pts[i].normal[0], pts[i].normal[1], pts[i].normal[2]};
            ge[i] = luminance(pointOut(ctx, 0, i));
            ce[i] = luminance(oracle.sampleIrradiance(pos, n));
        }
        gpuE.push_back(ge);
        cpuE.push_back(ce);
    }
    auto flicker = [&](const std::vector<std::vector<f64>>& e) {
        f64 second = 0.0, mean = 0.0;
        u32 n = 0;
        for (usize t = 1; t + 1 < e.size(); ++t) {
            for (usize i = 0; i < e[t].size(); ++i) {
                second += std::fabs(e[t][i] - 0.5 * (e[t - 1][i] + e[t + 1][i]));
                mean += e[t][i];
                ++n;
            }
        }
        return second / std::max(mean, 1e-9);
    };
    const f64 fg = flicker(gpuE);
    const f64 fc = flicker(cpuE);
    std::printf("  %u-step sun sweep (hysteresis %.2f, %u rays): temporal flicker (mean second difference / mean E): GPU %.4f, "
                "oracle %.4f\n",
                kSteps, static_cast<f64>(desc.hysteresis), desc.rays_per_probe, fg, fc);
    expect(fg <= 0.03, "temporal flicker below 3%");
    expect(fg <= fc * 1.25 + 0.002, "GPU flicker within 25% of the oracle's");
    vol.gpu.destroy();
    rt.destroy();
    s.gpu.destroy();
    return 0;
}

// --- shade ------------------------------------------------------------------------------------------
using vsmr_test::Camera;
using vsmr_test::D3;

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

f32 halfAt(const u8* p, usize i) {
    u16 h = 0;
    std::memcpy(&h, p + i * 2u, 2u);
    return GBufferQuantize::halfToFloat(h);
}

struct ImageCopy {
    rg::TextureRef image;
    rg::BufferRef dst;
    u64 dstOffset = 0;
    u32 width = 0;
    u32 height = 0;
    bool depth = false;
};

void recordImageCopy(const rg::PassContext& pc, void* user) {
    const ImageCopy& c = *static_cast<const ImageCopy*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {c.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(c.image)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

int runShade(Context& ctx) {
    using namespace fuse::renderer::culling;
    using namespace fuse::renderer::visbuffer;
    using namespace fuse::renderer::material_resolve;
    using namespace fuse::renderer::lighting_gpu;
    const VisCapabilities visCaps = queryVisCapabilities(ctx.device.get());
    const ResolveCapabilities resolveCaps = queryResolveCapabilities(ctx.device.get());
    const LightingCapabilities lightingCaps = queryLightingCapabilities(ctx.device.get());
    if (!visCaps.raster || !resolveCaps.resolve || !lightingCaps.lighting) {
        std::printf("SKIP: capability missing (vis %s, resolve %s, lighting %s)\n", visCaps.rasterReason, resolveCaps.reason, lightingCaps.reason);
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, roomScene(), true)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rt::AccelerationStructures rt;
    const bool haveRt = initRt(ctx, s, rt) == 0;
    const DDGIDesc desc = roomVolume();
    Volume vol;
    if (!initVolume(ctx, s, vol, desc, haveRt ? DdgiTracer::RayQuery : DdgiTracer::Sdf, DdgiKernelLanguage::Auto, haveRt ? "T2" : "T0")) {
        std::printf("SKIP: DDGI kernels not built\n");
        return kSkip;
    }
    constexpr u32 W = 160, H = 96;
    Camera cam;
    cam.eye = {0.5, 2.6, 5.5};
    cam.at = {-0.5, 0.4, -2.0};
    cam.width = W;
    cam.height = H;
    cam.zNear = 0.1f;
    cam.zFar = 40.f;
    cam.build();
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    ClusteredLighting lighting[4]; // [2 x language + 0] without DDGI, [+ 1] with
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 64;
    VisBufferDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.width = W;
    vd.height = H;
    vd.mode = VisMode::Raster;
    MaterialResolveDesc md{};
    md.device = ctx.device.get();
    md.allocator = ctx.allocator.get();
    md.bindless = &ctx.bindless;
    md.width = W;
    md.height = H;
    bool ok = culler.init(cd) && culler.setResolution(W, H) && vb.init(vd) && resolve.init(md);
    bool language[2] = {};
    for (u32 i = 0; i < 4u; ++i) {
        ClusteredLightingDesc ld{};
        ld.device = ctx.device.get();
        ld.allocator = ctx.allocator.get();
        ld.bindless = &ctx.bindless;
        ld.lightCapacity = 16;
        ld.language = i < 2u ? LightingKernelLanguage::Slang : LightingKernelLanguage::Glsl;
        language[i / 2u] = lighting[i].init(ld);
    }
    if (!language[0] && !language[1]) {
        std::printf("SKIP: no lighting kernel built\n");
        return kSkip;
    }
    if (!ok) {
        std::fprintf(stderr, "FAIL: VB / resolve / lighting init\n");
        return 1;
    }
    Buffer dumps[4]{};
    Buffer gbRead{};
    const u32 storage = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress);
    for (Buffer& b : dumps) {
        ok = ok && makeBuffer(ctx, b, static_cast<usize>(W) * H * 16u, storage, MemoryUsage::GpuToCpu, "rp_ddgi.dump");
    }
    // G-buffer read-back: RT0 (normal + AO, RGBA16F), RT1 (albedo, RGBA8), RT2 (rough / metal, RGBA8), RT4 (depth).
    const u64 px = static_cast<u64>(W) * H;
    const u64 offRt0 = 0u;
    const u64 offRt1 = offRt0 + px * 8u;
    const u64 offRt2 = offRt1 + px * 4u;
    const u64 offRt4 = offRt2 + px * 4u;
    ok = ok && makeBuffer(ctx, gbRead, static_cast<usize>(offRt4 + px * 4u), static_cast<u32>(BufferUsage::TransferDst), MemoryUsage::GpuToCpu,
                          "rp_ddgi.gbuffer_rb");
    if (!ok) {
        std::fprintf(stderr, "FAIL: dump / read-back buffers\n");
        return 1;
    }
    Frame f;
    ImageCopy imageCopies[4];
    constexpr u32 kFrames = 6;
    Volume* ptrs[1] = {&vol};
    std::vector<Vec4> dump[4];
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const bool last = frame + 1u == kFrames;
        beginSceneFrame(ctx, s, haveRt ? &rt : nullptr);
        const GpuSceneCommitStats stats = s.gpu.commit();
        if (haveRt) {
            rt.commit();
        }
        ctx.upload.flush();
        DdgiFrameDesc fd{};
        fd.frameIndex = frame;
        ok = beginVolumes(ctx, s, haveRt ? &rt : nullptr, ptrs, 1, fd) && ok;
        CullFrameDesc cf{};
        std::memcpy(cf.viewProj, cam.viewProj.m, sizeof(cf.viewProj));
        cf.instanceCount = s.gpu.instanceHighWater();
        ok = culler.beginFrame(ctx.serial, cf) && ok;
        ok = vb.beginFrame(ctx.serial, cam.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
        ResolveFrameDesc rf{};
        std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
        std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
        rf.scene = s.gpu.headerHandle();
        rf.vis = vb.visStorageHandle();
        rf.sampler = ctx.samplerHandle;
        ok = resolve.beginFrame(ctx.serial, rf) && ok;
        for (u32 k = 0; k < 4u; ++k) {
            if (!language[k / 2u]) {
                continue;
            }
            LightingFrameDesc lf{};
            lf.camera = clusterCamera(cam);
            lf.scene = s.gpu.headerHandle();
            lf.lightCount = s.gpu.lightHighWater();
            lf.gbuffer = &resolve;
            lf.ddgi = (k & 1u) != 0u ? vol.gpu.volumeAddress() : 0u;
            ok = lighting[k].beginFrame(ctx.serial, lf) && ok;
        }
        if (!ok) {
            std::fprintf(stderr, "FAIL: beginFrame\n");
            return 1;
        }
        rg::Graph& graph = f.graph;
        graph.reset();
        f.copyCount = 0;
        const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
        const rt::RtGraphRefs rtRefs = haveRt ? rt.importInto(graph, sceneRefs) : rt::RtGraphRefs{};
        const CullGraphRefs cull = culler.importInto(graph);
        const VisGraphRefs vis = vb.importInto(graph);
        vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), culler, cull);
        const ResolveGraphRefs gbuffer = resolve.importInto(graph);
        resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
        FrameIo io{};
        io.readback = last;
        addVolumes(ctx, f, sceneRefs, rtRefs, ptrs, 1, io); // ddgi.* + ddgi.sample_use (compute) + work copy
        rg::BufferRef dumpRefs[4]{};
        for (u32 k = 0; k < 4u; ++k) {
            if (!language[k / 2u]) {
                continue;
            }
            const LightingGraphRefs l = lighting[k].importInto(graph);
            lighting[k].addAssignment(graph, l, sceneRefs);
            dumpRefs[k] = graph.importBuffer(rg::ImportedBuffer{dumps[k].handle, dumps[k].desc.size, rg::kNoQueue, nullptr, "rp_ddgi.dump"});
            lighting[k].addShade(graph, l, sceneRefs, gbuffer, dumpRefs[k], dumps[k].deviceAddress);
            graph.addPass("readback.dump", nullptr, nullptr).use(dumpRefs[k], rg::Access::HostRead);
        }
        if (last) {
            const rg::BufferRef rb =
                graph.importBuffer(rg::ImportedBuffer{gbRead.handle, gbRead.desc.size, rg::kNoQueue, nullptr, "rp_ddgi.gbuffer_rb"});
            const u32 sources[4] = {0u, 1u, 2u, 4u};
            const u64 offsets[4] = {offRt0, offRt1, offRt2, offRt4};
            const u64 sizes[4] = {px * 8u, px * 4u, px * 4u, px * 4u};
            for (u32 i = 0; i < 4u; ++i) {
                imageCopies[i] = ImageCopy{gbuffer.gbuffer[sources[i]], rb, offsets[i], W, H, false};
                graph.addPass("readback.gbuffer", &recordImageCopy, &imageCopies[i])
                    .use(gbuffer.gbuffer[sources[i]], rg::Access::TransferSrc)
                    .use(rb, rg::Access::TransferDst, rg::BufferRange{offsets[i], sizes[i]});
            }
            graph.addPass("readback.gbuffer_host", nullptr, nullptr).use(rb, rg::Access::HostRead);
        }
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        culler.collectRetired(ctx.serial);
        vb.collectRetired(ctx.serial);
        resolve.collectRetired(ctx.serial);
        for (ClusteredLighting& l : lighting) {
            l.collectRetired(ctx.serial);
        }
        s.gpu.collectRetired(ctx.serial);
        if (haveRt) {
            rt.collectRetired(ctx.serial);
        }
        ctx.bindless.collectRetired(ctx.serial);
        if (!stats.ok || !result.ok || !waited) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
    }
    for (u32 k = 0; k < 4u; ++k) {
        dump[k].resize(px);
        std::memcpy(dump[k].data(), dumps[k].mapped, px * 16u);
    }
    GpuState g;
    readState(ctx, vol, g);
    const ddgi_kernel::VolumeView view = volumeView(desc, vol.gpu.desc().config, g.state);
    const u8* rb = static_cast<const u8*>(gbRead.mapped);
    const clustered_kernel::CameraView cv = clustered_kernel::make_camera(clusterCamera(cam));
    for (u32 lang = 0; lang < 2u; ++lang) {
        if (!language[lang]) {
            continue;
        }
        u32 shaded = 0, bad = 0, alphaBad = 0, lit = 0;
        f64 maxRel = 0.0, meanIndirect = 0.0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const usize p = static_cast<usize>(y) * W + x;
                const Vec4& a = dump[2u * lang][p];
                const Vec4& b = dump[2u * lang + 1u][p];
                if ((a.w > 0.f) != (b.w > 0.f)) {
                    ++alphaBad;
                    continue;
                }
                if (!(a.w > 0.f)) {
                    continue;
                }
                ++shaded;
                // The shade's surface reconstruction (lighting_gpu::shade_pixel).
                f32 depth = 0.f;
                std::memcpy(&depth, rb + offRt4 + p * 4u, 4u);
                const f32 sx = (static_cast<f32>(x) + 0.5f) * (1.f / static_cast<f32>(W));
                const f32 sy = (static_cast<f32>(y) + 0.5f) * (1.f / static_cast<f32>(H));
                const f32 vdepth = clustered_kernel::view_depth_from_device_depth(depth, cv.near_plane, cv.far_plane, cv.reversed_z);
                const Vec3 pos = clustered_kernel::view_to_world(cv, clustered_kernel::view_position_from_screen(sx, sy, vdepth, cv.tan_x, cv.tan_y));
                const Vec3 n = lighting_gpu::oct_decode_signed(halfAt(rb + offRt0, p * 4u), halfAt(rb + offRt0, p * 4u + 1u));
                const f32 ao = halfAt(rb + offRt0, p * 4u + 3u);
                const Vec3 albedo{rb[offRt1 + p * 4u] / 255.f, rb[offRt1 + p * 4u + 1u] / 255.f, rb[offRt1 + p * 4u + 2u] / 255.f};
                const f32 metallic = rb[offRt2 + p * 4u + 1u] / 255.f;
                const Vec3 e = ddgi_kernel::sample_irradiance(view, pos, n);
                const f32 k = (1.f - std::clamp(metallic, 0.f, 1.f)) * ao * (1.f * (1.f / ddgi_kernel::kPi));
                const Vec3 ref{(albedo.x * e.x) * k, (albedo.y * e.y) * k, (albedo.z * e.z) * k};
                const f32 diff[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
                const f32 rc[3] = {ref.x, ref.y, ref.z};
                const f32 base[3] = {b.x, b.y, b.z};
                bool pixelBad = false;
                for (u32 c = 0; c < 3u; ++c) {
                    const f64 err = std::fabs(static_cast<f64>(diff[c]) - rc[c]);
                    pixelBad = pixelBad || err > 1e-4 * std::fabs(static_cast<f64>(rc[c])) + 2e-6 * std::fabs(static_cast<f64>(base[c])) + 1e-7;
                    if (std::fabs(rc[c]) > 1e-3f) {
                        maxRel = std::max(maxRel, err / std::fabs(static_cast<f64>(rc[c])));
                    }
                }
                bad += pixelBad ? 1u : 0u;
                lit += luminance(ref) > 1e-3 ? 1u : 0u;
                meanIndirect += luminance(ref);
            }
        }
        meanIndirect /= std::max(shaded, 1u);
        std::printf("  %s light.shade with DDGI (%s volume, %u frames): %u shaded pixels, %u with indirect light (mean luminance %.4f); "
                    "(dump_ddgi - dump_plain) vs albedo x (1 - metallic) x AO x E / pi: max rel %.2e, %u beyond tolerance, %u coverage "
                    "mismatches\n",
                    lang == 0u ? "slang" : "glsl", vol.label, kFrames, shaded, lit, meanIndirect, maxRel, bad, alphaBad);
        expect(shaded > px / 2u && lit * 2u > shaded, "the shade covers the frame and DDGI lights it");
        expect(bad == 0u && alphaBad == 0u, "light.shade adds exactly the DDGI indirect diffuse");
    }
    for (Buffer& b : dumps) {
        ctx.allocator->destroyBuffer(b);
    }
    ctx.allocator->destroyBuffer(gbRead);
    for (ClusteredLighting& l : lighting) {
        l.destroy();
    }
    resolve.destroy();
    vb.destroy();
    culler.destroy();
    vol.gpu.destroy();
    rt.destroy();
    s.gpu.destroy();
    return 0;
}

// --- zero_alloc -------------------------------------------------------------------------------------
void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "ddgi.", 5) == 0) {
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
    Scene s;
    if (!buildScene(ctx, s, roomScene())) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rt::AccelerationStructures rt;
    const bool haveRt = initRt(ctx, s, rt) == 0;
    DDGIDesc desc = roomVolume();
    desc.probes_per_frame = 16; // rolling window
    Volume vols[2];
    if (haveRt) {
        initVolume(ctx, s, vols[0], desc, DdgiTracer::RayQuery, DdgiKernelLanguage::Auto, "T2");
    }
    initVolume(ctx, s, vols[1], desc, DdgiTracer::Sdf, DdgiKernelLanguage::Auto, "T0");
    if (!vols[1].active) {
        std::printf("SKIP: DDGI kernels not built\n");
        return kSkip;
    }
    const std::vector<DdgiProbePoint> pts = roomPoints();
    uploadPoints(ctx, pts);
    Volume* ptrs[2] = {&vols[0], &vols[1]};
    Frame f;
    constexpr u32 kWarmup = 8;
    constexpr u32 kTotal = 72;
    unsigned long long counted = 0, callbacks = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s, haveRt ? &rt : nullptr);
        s.gpu.commit();
        if (haveRt) {
            rt.commit();
        }
        ctx.upload.flush();
        s.cpu.sun_direction = Vec3{std::cos(0.05f * static_cast<f32>(frame)), 0.8f, 0.4f}.normalized();
        DdgiFrameDesc fd{};
        fd.frameIndex = frame;
        t_allocations = 0;
        t_count = measure;
        const bool began = beginVolumes(ctx, s, haveRt ? &rt : nullptr, ptrs, 2, fd);
        f.graph.reset();
        f.copyCount = 0;
        const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(f.graph);
        const rt::RtGraphRefs rtRefs = haveRt ? rt.importInto(f.graph, sceneRefs) : rt::RtGraphRefs{};
        FrameIo io{};
        io.readback = false;
        io.points = static_cast<u32>(pts.size());
        addVolumes(ctx, f, sceneRefs, rtRefs, ptrs, 2, io);
        t_count = false;
        unsigned long long frameAllocs = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(f.graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        s.gpu.collectRetired(ctx.serial);
        if (haveRt) {
            rt.collectRetired(ctx.serial);
        }
        ctx.bindless.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            counted += frameAllocs + inCallbacks;
            callbacks += inCallbacks;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s + T0 volumes, 16-probe rolling window, sun moving, ddgi.probe %zu points, sampling use)\n"
                    "  DdgiGpu::beginFrame + graph build (scene / rt imports, importInto, addUpdate, addProbe, addSamplingUse) + ddgi.* "
                    "callbacks: %llu operator-new calls (callbacks %llu)\n",
                    kTotal - kWarmup, haveRt ? "T2" : "(no T2)", pts.size(), counted, callbacks);
        expect(counted == 0u, "DDGI makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u\n", kTotal);
    }
    for (Volume& v : vols) {
        v.gpu.destroy();
    }
    rt.destroy();
    s.gpu.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity_rq";
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
        if (mode == "parity_rq") {
            rc = runParity(ctx, true);
        } else if (mode == "parity_sdf") {
            rc = runParity(ctx, false);
        } else if (mode == "converge") {
            rc = runConverge(ctx);
        } else if (mode == "leak") {
            rc = runLeak(ctx);
        } else if (mode == "tod") {
            rc = runTod(ctx);
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
