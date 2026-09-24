// WP-3.2 virtual shadow maps, page rendering / filtering / local lights: Lavapipe gates
// (VK_LAYER_KHRONOS_validation with synchronization validation; every validation message fails the
// run). CPU gates: test_rp_vsm_raster_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, VisBuffer::addCulledFrame
// (WP-1.3 + WP-1.4 raster depth), [the binned material resolve (WP-1.5)], then for each kernel language
// built (Slang, GLSL) one WP-3.1 VirtualShadowMap::addFrame (marking ... vsm.render + vsm.clear) and
// one VsmShadows::addRaster (vsm.raster_reset, vsm.raster = dispatch indirect on the render list,
// vsm.local_dirty / list / clear / raster), [ClusteredLighting (WP-2.1) light.shade and the WP-2.3
// forward pass sampling the shadows], [vsm.probe] and read-back copies. The CPU side keeps its own pool
// and local atlas: every frame VsmRasterReference renders the GPU's render lists into them, so cached
// pages carry the CPU's content from the frame that rendered them.
//
//   raster (every mode, every frame, every language): each render-list page of the GPU pool == the CPU
//          reference page (float bits; a depth may differ by <= 1e-6 d units, coverage never), and every
//          mapped page (cached ones included) == the CPU pool; the same for the local atlas; the pages
//          vsm.raster / vsm.local_raster executed == the render lists (exactly the render list is drawn);
//          Slang == GLSL (pools and atlases bit for bit)
//   --mode seam     the B5 CSM seam gate (fuse_b5_shadows_gates) ported: ground + a wall through every
//                   clipmap level, 320 x 180; vsm.probe at every ground pixel (hard and PCF 1) == the analytic
//                   ray / box occlusion, 0 mismatches, on both sides of every level boundary; frame 1 is
//                   static (renders 0 pages) and probes each receiver on the next coarser level: equal where
//                   that page is mapped
//   --mode shade    ground, 16 boxes, a transparent sphere; a directional light (VSM), a spot and a point
//                   light (local pages) and an unshadowed point light; 3 frames (PCF 1, PCSS, hard; the
//                   camera and a box move): the light.shade f32 dump == shadeShadowedReference (the WP-2.1
//                   CPU shade with the CPU-rendered pages and the CPU filters) within the WP-2.1 tolerance
//                   (1e-4 |ref| + 1e-6); the forward layer of the transparent sphere == the same reference
//                   on its surface; pixels whose shadow comparison lies within 1e-6 of its threshold are
//                   reported (ambiguous) and excluded, at most 0.1 %; the shadows are not vacuous
//   --mode local    spot + two point lights over boxes: vsm.probe on the ground == the analytic ray / box
//                   occlusion towards the light (hard), then static frames render 0 local pages, a box moved
//                   inside the spot's range re-renders exactly its page, one next to a point light its 6 faces
//   --mode cache    the shade scene without shading: static frames render 0 directional and 0 local pages
//                   and leave the pool / atlas bit-identical; a moved box re-renders only pages under its old /
//                   new footprint and the lights whose range it touches, still == the CPU pool
//   --mode zero_alloc  64 steady-state frames (a box moving, local lights, sampling uses): 0 operator-new
//                   calls in VsmShadows::beginFrame, the vsm.* pass callbacks and the whole graph build
//                   (validated run first; validation off for the count)
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_vsm_raster_common.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/forward/forward_transparency.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/shadow/vsm/virtual_shadow_map.hpp>
#include <fuse/renderer/shadow/vsm/vsm_kernel.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_reference.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_shadows.hpp>
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
using namespace fuse::renderer::visbuffer;
using namespace fuse::renderer::vsm;
using fuse::u16;
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
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kLanguages = 2; // Slang, GLSL
constexpr u32 kMaxProbes = 320u * 180u;
constexpr f32 kAmbient[3] = {0.03f, 0.035f, 0.045f};
// Shade tolerance: the WP-2.1 gate's (GPU f32 shade vs its CPU reference kernel; see
// test_rp_clustered_gpu.cpp). The shadow visibilities are bit-exact between the two (same f32 math,
// same pages), so the shadows add nothing to it.
constexpr f64 kTolRel = 1e-4;
constexpr f64 kTolAbs = 1e-6;
constexpr f64 kRasterTol = 1e-6; ///< page depth, d units (Vulkan allows 2.5 ulp division)

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
    Buffer probeIn{};
    Buffer probeOut[kLanguages]{};
    Buffer dumps[kLanguages]{};
    Buffer fdumps[kLanguages]{};
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
            allocator->destroyBuffer(probeIn);
            for (u32 k = 0; k < kLanguages; ++k) {
                allocator->destroyBuffer(probeOut[k]);
                allocator->destroyBuffer(dumps[k]);
                allocator->destroyBuffer(fdumps[k]);
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
    BufferDesc d{};
    d.size = bytes;
    d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr && b.deviceAddress != 0u;
}

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
    instanceDesc.appName = "fuse_rp_vsm_raster";
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
    const VisCapabilities visCaps = queryVisCapabilities(ctx.device.get());
    const VsmCapabilities vsmCaps = queryVsmCapabilities(ctx.device.get());
    const ResolveCapabilities resolveCaps = queryResolveCapabilities(ctx.device.get());
    const LightingCapabilities lightingCaps = queryLightingCapabilities(ctx.device.get());
    const forward::ForwardCapabilities forwardCaps = forward::queryForwardCapabilities(ctx.device.get());
    if (!visCaps.raster || !vsmCaps.vsm || !resolveCaps.resolve || !lightingCaps.lighting || !forwardCaps.forward) {
        std::printf("SKIP: capability missing (vis %s, vsm %s, resolve %s, lighting %s, forward %s)\n", visCaps.rasterReason,
                    vsmCaps.reason, resolveCaps.reason, lightingCaps.reason, forwardCaps.reason);
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
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_vsm_raster.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    bool ok = hostBuffer(ctx, ctx.probeIn, kMaxProbes * sizeof(VsmProbeInput), MemoryUsage::CpuToGpu, "rp_vsm_raster.probe_in");
    for (u32 k = 0; k < kLanguages; ++k) {
        ok = ok && hostBuffer(ctx, ctx.probeOut[k], kMaxProbes * sizeof(VsmProbeOutput), MemoryUsage::GpuToCpu, "rp_vsm_raster.probe_out");
        ok = ok && hostBuffer(ctx, ctx.dumps[k], kMaxProbes * 16u, MemoryUsage::GpuToCpu, "rp_vsm_raster.dump");
        ok = ok && hostBuffer(ctx, ctx.fdumps[k], kMaxProbes * sizeof(forward::ForwardDumpTexel), MemoryUsage::GpuToCpu,
                              "rp_vsm_raster.forward_dump");
    }
    SamplerDesc sd{};
    sd.name = "rp_vsm_raster.sampler";
    ctx.sampler = ctx.bindless.acquireSampler(sd);
    if (!ok || !ctx.sampler.isValid()) {
        std::fprintf(stderr, "FAIL: host buffers / sampler\n");
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
enum LightSlot : u32 { kSun = 0, kSpot = 1, kPointA = 2, kPointB = 3, kPlain = 4, kLightCount = 5 };

struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<decode_kernel::MeshPositions> positions;
    World world;
    InstanceHandle transparent{};
    std::vector<LightHandle> lights;

    VisSceneView view() const { return vis_scene_view(gpu, positions); }
    InstanceHandle boxHandle(u32 i) const {
        InstanceHandle h{};
        h.slot = world.boxSlots[i];
        h.generation = gpu.instance(h.slot).generation;
        return h;
    }
    void moveBox(u32 i, const Box& b) {
        world.boxes[i] = b;
        gpu.setTransform(boxHandle(i), World::boxTransform(b));
    }
};

struct SceneOptions {
    bool lights = false;
    bool transparent = false;
    D3 sun = seamSunTravel();
};

bool buildScene(Context& ctx, Scene& s, const World& world, const SceneOptions& o) {
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
    s.world = world;
    if (!buildMeshes(s.meshes, world.groundSize)) {
        return false;
    }
    s.meshes.emplace_back();
    if (!mr_test::build(mr_test::uvSphere(16, 24, 1.f), s.meshes.back())) {
        return false;
    }
    for (u32 i = 0; i < s.meshes.size(); ++i) {
        if (s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
        s.positions.push_back(decode_kernel::MeshPositions{s.meshes[i].positions.data(), s.meshes[i].vertex_count()});
    }
    Material::GPUMaterial m{};
    m.baseColor = {0.75f, 0.7f, 0.62f, 0.f};
    m.roughnessEmissive = {0.55f, 0.f, 0.f, 0.f};
    s.gpu.setMaterial(0, m);
    if (!addInstances(s.gpu, s.world, 0u)) {
        return false;
    }
    if (o.transparent) {
        InstanceDesc id{};
        id.mesh = 2;
        id.material = 0;
        id.flags |= kInstanceTransparent;
        id.transform = World::boxTransform(Box{{-2.4, 0.3, -6.6}, {-1.2, 1.5, -5.4}});
        s.transparent = s.gpu.addInstance(id);
    }
    if (o.lights) {
        GpuLight sun{};
        sun.type = static_cast<u32>(GpuLightType::Directional);
        sun.direction[0] = static_cast<f32>(o.sun.x);
        sun.direction[1] = static_cast<f32>(o.sun.y);
        sun.direction[2] = static_cast<f32>(o.sun.z);
        sun.intensity = 2.5f;
        GpuLight spot{};
        spot.type = static_cast<u32>(GpuLightType::Spot);
        spot.position[0] = 1.5f;
        spot.position[1] = 4.5f;
        spot.position[2] = -9.f;
        spot.direction[0] = -0.15f;
        spot.direction[1] = -1.f;
        spot.direction[2] = 0.1f;
        spot.range = 7.f;
        spot.cosInner = 0.9f;
        spot.cosOuter = 0.75f;
        spot.intensity = 30.f;
        spot.color[2] = 0.8f;
        GpuLight pa{};
        pa.type = static_cast<u32>(GpuLightType::Point);
        pa.position[0] = -5.f;
        pa.position[1] = 1.6f;
        pa.position[2] = -3.f;
        pa.range = 4.f;
        pa.intensity = 6.f;
        pa.color[0] = 0.9f;
        GpuLight pb = pa;
        pb.position[0] = 6.f;
        pb.position[2] = -16.f;
        pb.range = 4.f;
        GpuLight plain = pa;
        plain.position[0] = 0.f;
        plain.position[1] = 2.5f;
        plain.position[2] = -3.f;
        plain.range = 5.f;
        plain.intensity = 3.f;
        for (const GpuLight& l : {sun, spot, pa, pb, plain}) {
            s.lights.push_back(s.gpu.addLight(l));
        }
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

World shadeWorld() {
    World w;
    w.groundSize = 60.f;
    // 14: inside the spot's range only; 15: inside point light A's only (the cache / local gates move them).
    const Box boxes[16] = {{{-0.5, 0.0, -30.0}, {0.5, 3.0, -2.0}},  {{1.0, 0.0, -9.5}, {2.2, 1.4, -8.2}},
                           {{-4.0, 0.0, -6.0}, {-3.3, 2.4, -4.2}},  {{-2.5, 0.8, -12.0}, {-1.5, 1.2, -7.0}},
                           {{3.0, 0.0, -5.0}, {3.6, 0.6, -4.4}},    {{4.8, 0.0, -15.0}, {5.3, 3.0, -14.5}},
                           {{6.2, 0.0, -13.0}, {7.0, 0.9, -12.0}},  {{-6.0, 0.0, -18.0}, {-4.5, 2.0, -16.5}},
                           {{2.5, 1.5, -11.0}, {3.5, 1.7, -8.0}},   {{-1.2, 0.0, -4.0}, {-0.9, 1.9, -3.7}},
                           {{0.8, 0.0, -20.0}, {2.8, 1.0, -19.0}},  {{-3.5, 0.0, -24.0}, {-2.5, 5.0, -23.0}},
                           {{4.0, 2.2, -7.5}, {4.6, 2.5, -6.8}},    {{-2.2, 0.0, -9.0}, {-1.8, 0.4, -8.6}},
                           {{1.5, 0.0, -10.0}, {2.1, 0.8, -9.4}},   {{-4.2, 0.0, -3.3}, {-3.6, 1.0, -2.7}}};
    for (const Box& b : boxes) {
        w.boxes.push_back(b);
    }
    return w;
}

Camera shadeCamera(u32 frame) {
    Camera c;
    const f64 t = frame;
    c.eye = {-1.0 + 0.3 * t, 3.2 - 0.1 * t, 3.5 - 0.25 * t};
    c.at = {0.4 * t, 0.0, -14.0};
    c.width = 256;
    c.height = 192;
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

// --- rig ------------------------------------------------------------------------------------------
struct RigDesc {
    u32 width = 320;
    u32 height = 180;
    u32 levels = 16;
    f32 extent = 4.f;
    u32 poolX = 16;
    u32 poolY = 16;
    f32 markRadius = 2.5f;
    f32 texelsPerPixel = 2.f;
    bool shade = false;   ///< material resolve + lighting + forward
    u32 localX = 8;
    u32 localY = 2;
};

struct Rig {
    RigDesc desc{};
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    VirtualShadowMap vsm[kLanguages];
    VsmShadows shadows[kLanguages];
    ClusteredLighting lighting[kLanguages];
    forward::ForwardTransparency forward[kLanguages];
    bool built[kLanguages] = {};
    const char* language[kLanguages] = {"slang", "glsl"};
};

int initRig(Context& ctx, Rig& rig, const RigDesc& rd) {
    rig.desc = rd;
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 64;
    if (!rig.culler.init(cd) || !rig.culler.setResolution(rd.width, rd.height)) {
        return -1;
    }
    VisBufferDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.width = rd.width;
    vd.height = rd.height;
    vd.mode = VisMode::Raster;
    if (!rig.vb.init(vd)) {
        return -1;
    }
    if (rd.shade) {
        MaterialResolveDesc md{};
        md.device = ctx.device.get();
        md.allocator = ctx.allocator.get();
        md.bindless = &ctx.bindless;
        md.width = rd.width;
        md.height = rd.height;
        if (!rig.resolve.init(md)) {
            return -1;
        }
    }
    u32 built = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        VirtualShadowMapDesc d{};
        d.device = ctx.device.get();
        d.allocator = ctx.allocator.get();
        d.bindless = &ctx.bindless;
        d.clipmap.levels = rd.levels;
        d.clipmap.firstLevelExtent = rd.extent;
        d.clipmap.markRadiusTexels = rd.markRadius;
        d.clipmap.texelsPerPixel = rd.texelsPerPixel;
        d.poolPagesX = rd.poolX;
        d.poolPagesY = rd.poolY;
        d.instanceCapacity = 64;
        d.language = k == 0u ? VsmKernelLanguage::Slang : VsmKernelLanguage::Glsl;
        VsmShadowsDesc sd{};
        sd.device = ctx.device.get();
        sd.allocator = ctx.allocator.get();
        sd.bindless = &ctx.bindless;
        sd.localPagesX = rd.localX;
        sd.localPagesY = rd.localY;
        sd.language = d.language;
        rig.built[k] = rig.vsm[k].init(d) && rig.shadows[k].init(sd);
        if (rig.built[k] && rd.shade) {
            ClusteredLightingDesc ld{};
            ld.device = ctx.device.get();
            ld.allocator = ctx.allocator.get();
            ld.bindless = &ctx.bindless;
            ld.lightCapacity = 64;
            ld.language = k == 0u ? LightingKernelLanguage::Slang : LightingKernelLanguage::Glsl;
            forward::ForwardTransparencyDesc fd{};
            fd.device = ctx.device.get();
            fd.allocator = ctx.allocator.get();
            fd.bindless = &ctx.bindless;
            fd.width = rd.width;
            fd.height = rd.height;
            fd.instanceCapacity = 64;
            fd.language = k == 0u ? forward::ForwardKernelLanguage::Slang : forward::ForwardKernelLanguage::Glsl;
            rig.built[k] = rig.lighting[k].init(ld) && rig.forward[k].init(fd);
        }
        built += rig.built[k] ? 1u : 0u;
        std::printf("  %s kernels: %s\n", rig.language[k], rig.built[k] ? "built" : "not built, skipped");
    }
    return built > 0u ? 1 : 0;
}

void destroyRig(Rig& rig) {
    for (u32 k = 0; k < kLanguages; ++k) {
        rig.forward[k].destroy();
        rig.lighting[k].destroy();
        rig.shadows[k].destroy();
        rig.vsm[k].destroy();
    }
    rig.resolve.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
    enum Kind : u8 { Image, Depth, Buffer } kind = Image;
    rg::TextureRef image;
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
    if (c.kind == CopyRecord::Buffer) {
        const VkBufferCopy region{0, c.dstOffset, c.bytes};
        vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.buffer)), dst, 1, &region);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {c.kind == CopyRecord::Depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                                           : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

constexpr u32 kGBufferRead[5] = {0u, 1u, 2u, 4u, 5u};
constexpr u32 kGBufferBytes[5] = {8u, 4u, 4u, 4u, 8u};

struct ReadbackLayout {
    u64 depth = 0;
    u64 gbuffer[5] = {};
    u64 pageTable[kLanguages] = {};
    u64 vsmWork[kLanguages] = {};
    u64 pool[kLanguages] = {};
    u64 work[kLanguages] = {};
    u64 local[kLanguages] = {};
    u64 lists[kLanguages] = {};
    u64 end = 0;
};

struct FrameOptions {
    bool readback = true;
    bool shade = false;
    u32 probes = 0;          ///< vsm.probe points (ctx.probeIn)
    VsmFilterDesc filter{};
    bool locals = false;     ///< the scene's spot / point lights get local shadows
};

struct FrameState {
    CopyRecord copies[48];
    u32 copyCount = 0;
    ReadbackLayout layout{};
    VsmLocalLightDesc locals[3]{};
    u32 localCount = 0;
};

ReadbackLayout makeLayout(const Rig& rig, bool shade) {
    ReadbackLayout l{};
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    const u64 pixels = static_cast<u64>(rig.desc.width) * rig.desc.height;
    l.depth = take(pixels * 4u);
    if (shade) {
        for (u32 i = 0; i < 5u; ++i) {
            l.gbuffer[i] = take(pixels * kGBufferBytes[i]);
        }
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        const u32 src = rig.built[k] ? k : (k ^ 1u);
        const VirtualShadowMap& v = rig.vsm[src];
        const VsmShadows& s = rig.shadows[src];
        l.pageTable[k] = take(v.pageTableBuffer().desc.size);
        l.vsmWork[k] = take(v.workBuffer().desc.size);
        l.pool[k] = take(static_cast<u64>(v.poolImage().desc.width) * v.poolImage().desc.height * 4u);
        l.work[k] = take(s.workBuffer().desc.size);
        l.local[k] = take(static_cast<u64>(s.localImage().desc.width) * s.localImage().desc.height * 4u);
        if (shade) {
            l.lists[k] = take(rig.lighting[src].listsBuffer().desc.size);
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
    d.name = "rp_vsm_raster.readback";
    return ctx.allocator->createBuffer(d, ctx.readback) && ctx.readback.mapped != nullptr;
}

void collectLocals(const Scene& s, FrameState& fs) {
    fs.localCount = 0;
    for (u32 slot : {static_cast<u32>(kSpot), static_cast<u32>(kPointA), static_cast<u32>(kPointB)}) {
        if (slot < s.gpu.lightHighWater() && makeLocalLight(slot, s.gpu.light(slot), fs.locals[fs.localCount])) {
            fs.locals[fs.localCount].nearPlane = 0.05f;
            fs.locals[fs.localCount].lightSize = 0.15f;
            ++fs.localCount;
        }
    }
}

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const Camera& cam, const D3& sun, const FrameOptions& o, FrameState& fs,
                bool countVsm = false) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, cam.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    if (o.shade) {
        ResolveFrameDesc rf{};
        std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
        std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
        rf.scene = s.gpu.headerHandle();
        rf.vis = rig.vb.visStorageHandle();
        rf.sampler = ctx.samplerHandle;
        ok = rig.resolve.beginFrame(ctx.serial, rf) && ok;
    }
    VsmFrameDesc vf{};
    vf.view.lightDirection[0] = static_cast<f32>(sun.x);
    vf.view.lightDirection[1] = static_cast<f32>(sun.y);
    vf.view.lightDirection[2] = static_cast<f32>(sun.z);
    vf.view.cameraPosition[0] = static_cast<f32>(cam.eye.x);
    vf.view.cameraPosition[1] = static_cast<f32>(cam.eye.y);
    vf.view.cameraPosition[2] = static_cast<f32>(cam.eye.z);
    std::memcpy(vf.view.invViewProj, cam.invViewProjF.m, sizeof(vf.view.invViewProj));
    vf.view.depthWidth = cam.width;
    vf.view.depthHeight = cam.height;
    vf.view.pixelSpread = cam.pixelSpread();
    vf.depthHandle = rig.vb.depthSampledHandle();
    vf.scene = s.gpu.headerHandle();
    vf.instanceCount = s.gpu.instanceHighWater();
    if (o.locals) {
        collectLocals(s, fs);
    } else {
        fs.localCount = 0;
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        ok = rig.vsm[k].beginFrame(ctx.serial, vf) && ok;
        VsmShadowFrameDesc sf{};
        sf.vsm = &rig.vsm[k];
        sf.directionalSlot = kSun;
        sf.scene = s.gpu.headerHandle();
        sf.instanceCount = s.gpu.instanceHighWater();
        sf.locals = fs.locals;
        sf.localCount = fs.localCount;
        sf.filter = o.filter;
        t_count = countVsm;
        ok = rig.shadows[k].beginFrame(ctx.serial, sf) && ok;
        t_count = false;
        if (o.shade) {
            LightingFrameDesc lf{};
            lf.camera = clusterCamera(cam);
            lf.scene = s.gpu.headerHandle();
            lf.lightCount = s.gpu.lightHighWater();
            std::memcpy(lf.ambient, kAmbient, sizeof(lf.ambient));
            lf.gbuffer = &rig.resolve;
            lf.shadows = rig.shadows[k].shadowConstantsAddress();
            ok = rig.lighting[k].beginFrame(ctx.serial, lf) && ok;
            forward::ForwardFrameDesc ff{};
            std::memcpy(ff.viewProj, cam.viewProj.m, sizeof(ff.viewProj));
            ff.scene = &s.gpu;
            ff.lighting = &rig.lighting[k];
            ff.sampler = ctx.samplerHandle;
            ff.dumpAddress = ctx.fdumps[k].deviceAddress;
            ok = rig.forward[k].beginFrame(ctx.serial, ff) && ok;
        }
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, FrameState& fs, const FrameOptions& o) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), rig.culler, cull);
    ResolveGraphRefs gbuffer{};
    if (o.shade) {
        gbuffer = rig.resolve.importInto(graph);
        rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
    }
    VsmGraphRefs vsm[kLanguages]{};
    VsmShadowGraphRefs sh[kLanguages]{};
    LightingGraphRefs lighting[kLanguages]{};
    rg::BufferRef probeIn{};
    if (o.probes > 0u) {
        probeIn = graph.importBuffer(rg::ImportedBuffer{ctx.probeIn.handle, ctx.probeIn.desc.size, rg::kNoQueue, nullptr, "rp_vsm_raster.probe_in"});
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        vsm[k] = rig.vsm[k].importInto(graph);
        rig.vsm[k].addFrame(graph, vsm[k], vis.depth, sceneRefs, true);
        sh[k] = rig.shadows[k].importInto(graph);
        rig.shadows[k].addRaster(graph, sh[k], &vsm[k], sceneRefs);
        if (o.probes > 0u) {
            const rg::BufferRef out = graph.importBuffer(
                rg::ImportedBuffer{ctx.probeOut[k].handle, ctx.probeOut[k].desc.size, rg::kNoQueue, nullptr, "rp_vsm_raster.probe_out"});
            rig.shadows[k].addProbe(graph, sh[k], &vsm[k], probeIn, ctx.probeIn.deviceAddress, out, ctx.probeOut[k].deviceAddress,
                                    o.probes);
            if (o.readback) {
                graph.addPass("readback.probe", nullptr, nullptr).use(out, rg::Access::HostRead);
            }
        }
        if (!o.shade) {
            // Where light.shade / the forward pass would sample (declarations only).
            rig.shadows[k].addSamplingUse(graph, sh[k], &vsm[k], rg::kStageCompute);
            rig.shadows[k].addSamplingUse(graph, sh[k], &vsm[k], rg::kStageFragment);
        }
        if (o.shade) {
            lighting[k] = rig.lighting[k].importInto(graph);
            rig.lighting[k].addAssignment(graph, lighting[k], sceneRefs);
            const rg::BufferRef dump = o.readback ? graph.importBuffer(rg::ImportedBuffer{ctx.dumps[k].handle, ctx.dumps[k].desc.size,
                                                                                          rg::kNoQueue, nullptr, "rp_vsm_raster.dump"})
                                                  : rg::BufferRef{};
            rig.shadows[k].addSamplingUse(graph, sh[k], &vsm[k], rg::kStageCompute);
            rig.lighting[k].addShade(graph, lighting[k], sceneRefs, gbuffer, dump, o.readback ? ctx.dumps[k].deviceAddress : 0u);
            const forward::ForwardGraphRefs fw = rig.forward[k].importInto(graph);
            const rg::BufferRef fdump = o.readback ? graph.importBuffer(rg::ImportedBuffer{ctx.fdumps[k].handle, ctx.fdumps[k].desc.size,
                                                                                           rg::kNoQueue, nullptr, "rp_vsm_raster.fdump"})
                                                   : rg::BufferRef{};
            rig.shadows[k].addSamplingUse(graph, sh[k], &vsm[k], rg::kStageFragment);
            rig.forward[k].addForward(graph, fw, sceneRefs, lighting[k], vis.depth, fdump);
            if (o.readback) {
                graph.addPass("readback.dump", nullptr, nullptr).use(dump, rg::Access::HostRead);
                graph.addPass("readback.fdump", nullptr, nullptr).use(fdump, rg::Access::HostRead);
            }
        }
    }
    if (!o.readback) {
        return;
    }
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_vsm_raster.readback"});
    auto addCopy = [&](CopyRecord::Kind kind, rg::TextureRef image, rg::BufferRef buffer, u64 dstOffset, u64 bytes, u32 w, u32 h) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.kind = kind;
        c.image = image;
        c.buffer = buffer;
        c.dst = rb;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        c.width = w;
        c.height = h;
        rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
        if (kind == CopyRecord::Buffer) {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        } else {
            pass.use(image, rg::Access::TransferSrc);
        }
        pass.use(rb, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    const u32 W = rig.desc.width;
    const u32 H = rig.desc.height;
    addCopy(CopyRecord::Depth, vis.depth, {}, fs.layout.depth, static_cast<u64>(W) * H * 4u, W, H);
    if (o.shade) {
        for (u32 i = 0; i < 5u; ++i) {
            addCopy(CopyRecord::Image, gbuffer.gbuffer[kGBufferRead[i]], {}, fs.layout.gbuffer[i], static_cast<u64>(W) * H * kGBufferBytes[i],
                    W, H);
        }
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const VirtualShadowMap& v = rig.vsm[k];
        const VsmShadows& sw = rig.shadows[k];
        addCopy(CopyRecord::Buffer, {}, vsm[k].pageTable, fs.layout.pageTable[k], v.pageTableBuffer().desc.size, 0, 0);
        addCopy(CopyRecord::Buffer, {}, vsm[k].work, fs.layout.vsmWork[k], v.workBuffer().desc.size, 0, 0);
        const Texture& pool = v.poolImage();
        addCopy(CopyRecord::Image, vsm[k].pool, {}, fs.layout.pool[k], static_cast<u64>(pool.desc.width) * pool.desc.height * 4u,
                pool.desc.width, pool.desc.height);
        addCopy(CopyRecord::Buffer, {}, sh[k].work, fs.layout.work[k], sw.workBuffer().desc.size, 0, 0);
        const Texture& local = sw.localImage();
        addCopy(CopyRecord::Image, sh[k].local, {}, fs.layout.local[k], static_cast<u64>(local.desc.width) * local.desc.height * 4u,
                local.desc.width, local.desc.height);
        if (o.shade) {
            addCopy(CopyRecord::Buffer, {}, lighting[k].lists, fs.layout.lists[k], rig.lighting[k].listsBuffer().desc.size, 0, 0);
        }
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

void collect(Context& ctx, Scene& s, Rig& rig) {
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.resolve.collectRetired(ctx.serial);
    for (u32 k = 0; k < kLanguages; ++k) {
        rig.vsm[k].collectRetired(ctx.serial);
        rig.lighting[k].collectRetired(ctx.serial);
        rig.forward[k].collectRetired(ctx.serial);
    }
    s.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
}

bool runFrame(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const Camera& cam, const D3& sun, const FrameOptions& o,
              FrameState& fs) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rig, cam, sun, o, fs)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    if (o.readback) {
        fs.layout = makeLayout(rig, o.shade);
        if (!ensureReadback(ctx, fs.layout)) {
            return false;
        }
    }
    buildGraph(ctx, s, rig, graph, fs, o);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    collect(ctx, s, rig);
    return stats.ok && result.ok && waited;
}

// --- CPU side -------------------------------------------------------------------------------------
const u8* rbAt(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

/// The CPU mirror of one language: its pool / atlas (rendered by the reference) and last frame's GPU state.
struct Mirror {
    std::vector<u32> pool, local, pte, gpuPool, gpuLocal, vsmWork, work;
    VsmRasterReference raster;
    std::vector<u32> list, localList;
};

struct RasterReport {
    u32 dirPages = 0;          ///< render-list length
    u32 dirExecuted = 0;       ///< vsm.raster workgroups (kWordDirPages)
    u32 localPages = 0;
    u32 localExecuted = 0;
    u64 texels = 0;            ///< texels compared (rendered pages)
    u64 exact = 0;             ///< of those, bit-identical
    u64 bad = 0;               ///< depth off by > kRasterTol or coverage differs
    u64 cachedBad = 0;         ///< mapped pages (cached included) != the CPU pool
    u32 mapped = 0;
    u64 localTexels = 0, localExact = 0, localBad = 0;
    u32 dirtyMask = 0;
};

void comparePage(const u32* gpu, const u32* cpu, u32 width, u32 page, u32 pagesX, u64& texels, u64& exact, u64& bad) {
    const u32 ox = (page % pagesX) * kPageTexels;
    const u32 oy = (page / pagesX) * kPageTexels;
    for (u32 y = 0; y < kPageTexels; ++y) {
        for (u32 x = 0; x < kPageTexels; ++x) {
            const usize i = static_cast<usize>(oy + y) * width + ox + x;
            ++texels;
            if (gpu[i] == cpu[i]) {
                ++exact;
                continue;
            }
            const f32 a = raster_math::float_of(gpu[i]);
            const f32 b = raster_math::float_of(cpu[i]);
            if ((a < 1.f) != (b < 1.f) || std::fabs(static_cast<f64>(a) - b) > kRasterTol) {
                ++bad;
            }
        }
    }
}

/// Reads one language's VSM state back, renders its render lists on the CPU and compares.
RasterReport checkRaster(Context& ctx, Scene& s, Rig& rig, const FrameState& fs, u32 k, Mirror& m) {
    RasterReport r{};
    const VirtualShadowMap& v = rig.vsm[k];
    const VsmShadows& sw = rig.shadows[k];
    const VsmFrameConstants& c = v.constants();
    const u32 poolW = v.poolImage().desc.width;
    const u32 poolH = v.poolImage().desc.height;
    const u32 localW = sw.localImage().desc.width;
    const u32 localH = sw.localImage().desc.height;
    if (m.pool.empty()) {
        m.pool.assign(static_cast<usize>(poolW) * poolH, kDepthClearBits);
        m.local.assign(static_cast<usize>(localW) * localH, kDepthClearBits);
    }
    m.pte.resize(v.pageTableBuffer().desc.size / 4u);
    m.vsmWork.resize(v.workBuffer().desc.size / 4u);
    m.gpuPool.resize(static_cast<usize>(poolW) * poolH);
    m.work.resize(sw.workBuffer().desc.size / 4u);
    m.gpuLocal.resize(static_cast<usize>(localW) * localH);
    std::memcpy(m.pte.data(), rbAt(ctx, fs.layout.pageTable[k]), m.pte.size() * 4u);
    std::memcpy(m.vsmWork.data(), rbAt(ctx, fs.layout.vsmWork[k]), m.vsmWork.size() * 4u);
    std::memcpy(m.gpuPool.data(), rbAt(ctx, fs.layout.pool[k]), m.gpuPool.size() * 4u);
    std::memcpy(m.work.data(), rbAt(ctx, fs.layout.work[k]), m.work.size() * 4u);
    std::memcpy(m.gpuLocal.data(), rbAt(ctx, fs.layout.local[k]), m.gpuLocal.size() * 4u);
    // Directional: exactly the render list.
    r.dirPages = std::min(m.vsmWork[kCounterRenderCount], c.physPages);
    r.dirExecuted = m.work[kWordDirPages];
    m.list.assign(m.vsmWork.begin() + c.offRenderList, m.vsmWork.begin() + c.offRenderList + r.dirPages * 2u);
    m.raster.prepare(s.view(), &c);
    m.raster.renderPages(c, m.list.data(), r.dirPages, m.pool.data(), poolW);
    for (u32 i = 0; i < r.dirPages; ++i) {
        comparePage(m.gpuPool.data(), m.pool.data(), poolW, m.list[i * 2u + 1u], c.poolPagesX, r.texels, r.exact, r.bad);
    }
    // Every mapped page, cached ones included, == the CPU pool.
    for (u32 i = 0; i < c.virtualPages; ++i) {
        if ((m.pte[i] & kPteMapped) == 0u) {
            continue;
        }
        ++r.mapped;
        u64 t = 0, e = 0, b = 0;
        comparePage(m.gpuPool.data(), m.pool.data(), poolW, m.pte[i] & kPtePhysMask, c.poolPagesX, t, e, b);
        r.cachedBad += b;
    }
    // Local pages.
    const VsmShadowConstants& sc = sw.constants();
    r.localPages = std::min(m.work[kWordLocalDispatchX], kMaxLocalPages);
    r.localExecuted = m.work[kWordLocalPages];
    r.dirtyMask = m.work[kWordDirtyMask];
    m.localList.assign(m.work.begin() + kWordLocalList, m.work.begin() + kWordLocalList + r.localPages * 2u);
    m.raster.renderLocal(sc, m.localList.data(), r.localPages, m.local.data(), localW);
    for (u32 i = 0; i < r.localPages; ++i) {
        comparePage(m.gpuLocal.data(), m.local.data(), localW, m.localList[i * 2u + 1u], sc.localPagesX, r.localTexels, r.localExact,
                    r.localBad);
    }
    // Every assigned local page == the CPU atlas.
    for (u32 l = 0; l < sc.localCount; ++l) {
        for (u32 f = 0; f < sc.local[l].faces; ++f) {
            u64 t = 0, e = 0, b = 0;
            comparePage(m.gpuLocal.data(), m.local.data(), localW, sc.local[l].page[f], sc.localPagesX, t, e, b);
            r.cachedBad += b;
        }
    }
    return r;
}

void printRaster(const char* tag, const RasterReport& r) {
    std::printf("%s dir pages %u (executed %u), %llu texels, %llu bit-exact, %llu bad; mapped %u, cached-bad %llu; local pages %u "
                "(executed %u, dirty 0x%x), %llu texels, %llu bit-exact, %llu bad\n",
                tag, r.dirPages, r.dirExecuted, static_cast<unsigned long long>(r.texels), static_cast<unsigned long long>(r.exact),
                static_cast<unsigned long long>(r.bad), r.mapped, static_cast<unsigned long long>(r.cachedBad), r.localPages,
                r.localExecuted, r.dirtyMask, static_cast<unsigned long long>(r.localTexels),
                static_cast<unsigned long long>(r.localExact), static_cast<unsigned long long>(r.localBad));
}

void expectRaster(const RasterReport& r) {
    expect(r.dirExecuted == r.dirPages, "vsm.raster renders exactly the render list's pages");
    expect(r.localExecuted == r.localPages, "vsm.local_raster renders exactly the local render list");
    expect(r.bad == 0u && r.localBad == 0u, "rendered pages == the CPU reference (coverage exact, depth <= 1e-6)");
    expect(r.cachedBad == 0u, "every mapped page (cached included) == the CPU pool / atlas");
}

/// Checks every language's raster; Slang == GLSL.
void checkRasterAll(Context& ctx, Scene& s, Rig& rig, const FrameState& fs, Mirror mirrors[kLanguages], RasterReport reports[kLanguages],
                    u32 frame) {
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        reports[k] = checkRaster(ctx, s, rig, fs, k, mirrors[k]);
        char tag[64];
        std::snprintf(tag, sizeof(tag), "  frame %u %-5s", frame, rig.language[k]);
        printRaster(tag, reports[k]);
        expectRaster(reports[k]);
    }
    if (rig.built[0] && rig.built[1]) {
        expect(mirrors[0].gpuPool == mirrors[1].gpuPool && mirrors[0].gpuLocal == mirrors[1].gpuLocal,
               "Slang == GLSL (pool and local atlas bit for bit)");
    }
}

u32 firstBuilt(const Rig& rig) { return rig.built[0] ? 0u : 1u; }

// --- seam ------------------------------------------------------------------------------------------
struct Probe {
    D3 p{};
    usize pixel = 0;
};

int runSeam(Context& ctx) {
    Scene s;
    const World world = seamWorld();
    if (!buildScene(ctx, s, world, SceneOptions{})) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const RigDesc rd{};
    const int rc = initRig(ctx, rig, rd);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: no kernels built\n");
        return rc < 0 ? 1 : kSkip;
    }
    const Camera cam = seamCamera();
    const D3 sun = seamSunTravel();
    const D3 toLight = sun * -1.0;
    // Receivers: every pixel whose ray hits the ground (the analytic point; the rasterised ground is the
    // same plane).
    std::vector<Probe> probes;
    for (u32 y = 0; y < cam.height; ++y) {
        for (u32 x = 0; x < cam.width; ++x) {
            D3 o, d;
            cam.ray(x, y, o, d);
            f64 t = 0.0;
            if (world.cast(o, d, 0.0, 1e9, t) == World::kGround) {
                Probe p{};
                p.p = o + d * t;
                p.p.y = 0.0;
                p.pixel = static_cast<usize>(y) * cam.width + x;
                probes.push_back(p);
            }
        }
    }
    const u32 count = static_cast<u32>(std::min<usize>(probes.size(), kMaxProbes));
    rg::Graph graph;
    FrameState fs;
    Mirror mirrors[kLanguages];
    const struct {
        u32 filter;
        u32 radius;
        const char* name;
    } modes[2] = {{kFilterHard, 0u, "hard"}, {kFilterPcf, 1u, "pcf1"}};
    u32 frame = 0;
    for (const auto& mode : modes) {
        std::vector<s32> levels[kLanguages];
        std::vector<f32> auto_[kLanguages];
        for (u32 pass = 0; pass < 2u; ++pass) {
            VsmProbeInput* in = static_cast<VsmProbeInput*>(ctx.probeIn.mapped);
            for (u32 i = 0; i < count; ++i) {
                in[i] = VsmProbeInput{};
                in[i].position[0] = static_cast<f32>(probes[i].p.x);
                in[i].position[1] = 0.f;
                in[i].position[2] = static_cast<f32>(probes[i].p.z);
                in[i].slot = kSun;
                in[i].level = pass == 0u || levels[firstBuilt(rig)].empty() ? -1 : levels[firstBuilt(rig)][i] + 1;
            }
            beginSceneFrame(ctx, s);
            FrameOptions o{};
            o.probes = count;
            o.filter.filter = mode.filter;
            o.filter.pcfRadius = mode.radius;
            o.filter.normalOffset = 1.5f;
            o.filter.depthBias = 1.f;
            if (!runFrame(ctx, s, rig, graph, cam, sun, o, fs)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                destroyRig(rig);
                return 1;
            }
            RasterReport reports[kLanguages];
            checkRasterAll(ctx, s, rig, fs, mirrors, reports, frame);
            if (frame > 0u) {
                expect(reports[firstBuilt(rig)].dirPages == 0u && reports[firstBuilt(rig)].dirExecuted == 0u,
                       "a cached (static) frame renders 0 pages");
            }
            ++frame;
            for (u32 k = 0; k < kLanguages; ++k) {
                if (!rig.built[k]) {
                    continue;
                }
                const VsmProbeOutput* out = static_cast<const VsmProbeOutput*>(ctx.probeOut[k].mapped);
                const VsmFrameConstants& c = rig.vsm[k].constants();
                if (pass == 0u) {
                    levels[k].assign(count, -1);
                    auto_[k].assign(count, 1.f);
                }
                u32 checked = 0, mismatches = 0, unmapped = 0, boundary = 0, boundaryBad = 0, coarse = 0, coarseBad = 0, both = 0;
                std::vector<s32> pixLevel(static_cast<usize>(cam.width) * cam.height, -1);
                std::vector<signed char> pixTruth(pixLevel.size(), -1);
                std::vector<unsigned char> pixWrong(pixLevel.size(), 0u);
                for (u32 i = 0; i < count; ++i) {
                    const VsmProbeOutput& r = out[i];
                    if (pass == 0u) {
                        levels[k][i] = r.level;
                        auto_[k][i] = r.visibility;
                    }
                    const s32 level = levels[k][i];
                    if (level < 0) {
                        ++unmapped;
                        continue;
                    }
                    const s32 coarser = std::min<s32>(level + 1, static_cast<s32>(c.levels) - 1);
                    const f64 margin = (3.0 + mode.radius) * c.level[coarser].pageWorld / 128.0;
                    if (!analyticClear(world, probes[i].p, toLight, margin)) {
                        continue;
                    }
                    const bool truth = world.occluded(probes[i].p, toLight);
                    const f32 vis = pass == 0u ? r.visibility : auto_[k][i];
                    if (pass == 0u) {
                        ++checked;
                        const bool wrong = (vis < 0.5f) != truth || !(vis == 0.f || vis == 1.f);
                        mismatches += wrong ? 1u : 0u;
                        pixLevel[probes[i].pixel] = level;
                        pixTruth[probes[i].pixel] = truth ? 1 : 0;
                        pixWrong[probes[i].pixel] = wrong ? 1u : 0u;
                    } else if (r.visibility >= 0.f && level + 1 < static_cast<s32>(c.levels)) {
                        ++coarse;
                        coarseBad += (r.visibility < 0.5f) != truth || !(r.visibility == 0.f || r.visibility == 1.f) ? 1u : 0u;
                    }
                }
                if (pass == 0u) {
                    u32 sides[16][2][2] = {};
                    for (u32 y = 0; y < cam.height; ++y) {
                        for (u32 x = 0; x < cam.width; ++x) {
                            const usize i = static_cast<usize>(y) * cam.width + x;
                            if (pixLevel[i] < 0) {
                                continue;
                            }
                            s32 other = -1;
                            const s32 dx[4] = {1, -1, 0, 0};
                            const s32 dy[4] = {0, 0, 1, -1};
                            for (u32 n = 0; n < 4u; ++n) {
                                const s32 nx = static_cast<s32>(x) + dx[n];
                                const s32 ny = static_cast<s32>(y) + dy[n];
                                if (nx < 0 || ny < 0 || nx >= static_cast<s32>(cam.width) || ny >= static_cast<s32>(cam.height)) {
                                    continue;
                                }
                                const s32 l = pixLevel[static_cast<usize>(ny) * cam.width + static_cast<usize>(nx)];
                                if (l >= 0 && l != pixLevel[i]) {
                                    other = l;
                                }
                            }
                            if (other < 0) {
                                continue;
                            }
                            ++boundary;
                            boundaryBad += pixWrong[i];
                            const s32 b = std::min(pixLevel[i], other);
                            if (b < 16) {
                                sides[b][pixLevel[i] == b ? 0 : 1][pixTruth[i] == 1 ? 1 : 0]++;
                            }
                        }
                    }
                    for (u32 b = 0; b < 16u; ++b) {
                        both += sides[b][0][0] > 0u && sides[b][0][1] > 0u && sides[b][1][0] > 0u && sides[b][1][1] > 0u ? 1u : 0u;
                    }
                    std::printf("  seam %s %-5s: %u receivers checked, %u mismatches; %u at level boundaries (%u mismatches), %u "
                                "boundaries lit + shadowed on both sides; %u unmapped\n",
                                mode.name, rig.language[k], checked, mismatches, boundary, boundaryBad, both, unmapped);
                    expect(checked > 10000u && boundary > 200u, "the seam scene samples every level boundary");
                    expect(mismatches == 0u && boundaryBad == 0u, "seam: 0 mismatches across clipmap boundaries (GPU VSM == analytic)");
                    expect(both >= 2u, "the wall's shadow edge crosses the level boundaries");
                    expect(unmapped == 0u, "every receiver finds a mapped page");
                } else {
                    std::printf("  seam %s %-5s: coarser level %u receivers checked where mapped, %u mismatches\n", mode.name,
                                rig.language[k], coarse, coarseBad);
                    expect(coarse > 100u && coarseBad == 0u, "the next coarser level agrees where its page is mapped");
                }
            }
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- shade -----------------------------------------------------------------------------------------
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

struct ShadeStats {
    u32 shaded = 0;
    u32 bad = 0;
    u32 alphaBad = 0;
    u32 ambiguous = 0;
    f64 maxRel = 0.0;
    u32 shadowed[3] = {}; ///< pixels with visibility < 1 for the sun, the spot, point A
    u32 partial = 0;      ///< pixels with a fractional visibility
};

/// Compares a GPU radiance image with the shadowed CPU reference on the same surface texels.
void compareShade(const GBufferTexels& tex, const std::vector<Vec4>& gpu, const std::vector<Vec4>& ref, const ShadowReferenceSource& src,
                  const ClusterCameraDesc& camDesc, u32 W, u32 H, ShadeStats& st, const std::vector<u8>* only = nullptr) {
    const clustered_kernel::CameraView cv = clustered_kernel::make_camera(camDesc);
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const usize p = static_cast<usize>(y) * W + x;
            if (only != nullptr && (*only)[p] == 0u) {
                continue;
            }
            const Vec4& g = gpu[p];
            const Vec4& c = ref[p];
            if ((g.w > 0.f) != (c.w > 0.f)) {
                ++st.alphaBad;
                continue;
            }
            if (!(c.w > 0.f)) {
                continue;
            }
            ++st.shaded;
            // The receiver as the shade reconstructs it; ambiguity of every shadowed light's comparisons.
            const f32 sx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(W);
            const f32 sy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(H);
            const f32 vd = clustered_kernel::view_depth_from_device_depth(tex.depth[p], cv.near_plane, cv.far_plane, cv.reversed_z);
            const Vec3 pos = clustered_kernel::view_to_world(cv, clustered_kernel::view_position_from_screen(sx, sy, vd, cv.tan_x, cv.tan_y));
            const Vec3 n = oct_decode_signed(tex.normalAo[p].x, tex.normalAo[p].y);
            bool ambiguous = false;
            const u32 slots[3] = {kSun, kSpot, kPointA};
            for (u32 i = 0; i < 3u; ++i) {
                const raster_math::SampleResult r = shadowVisibility(src, slots[i], pos, n);
                ambiguous = ambiguous || r.margin < raster_math::kAmbiguity;
                bool inRange = true; // local lights: count receivers inside the range sphere only
                if (i > 0u) {
                    const VsmLocalLight& e = src.shadow->local[i - 1u];
                    const Vec3 d{pos.x - e.position[0], pos.y - e.position[1], pos.z - e.position[2]};
                    inRange = d.dot(d) < e.range * e.range;
                }
                st.shadowed[i] += inRange && r.visibility < 1.f ? 1u : 0u;
                st.partial += inRange && r.visibility > 0.f && r.visibility < 1.f ? 1u : 0u;
            }
            const f32 gc[3] = {g.x, g.y, g.z};
            const f32 cc[3] = {c.x, c.y, c.z};
            bool bad = false;
            for (u32 ch = 0; ch < 3u; ++ch) {
                const f64 diff = std::fabs(static_cast<f64>(gc[ch]) - cc[ch]);
                const f64 mag = std::fabs(static_cast<f64>(cc[ch]));
                if (mag > 1e-3) {
                    st.maxRel = std::max(st.maxRel, diff / mag);
                }
                bad = bad || !(diff <= kTolRel * mag + kTolAbs);
            }
            if (bad && ambiguous) {
                ++st.ambiguous;
            } else if (bad) {
                ++st.bad;
            }
        }
    }
}

int runShade(Context& ctx) {
    Scene s;
    SceneOptions so{};
    so.lights = true;
    so.transparent = true;
    so.sun = normalize(D3{0.45, -1.0, -0.35});
    if (!buildScene(ctx, s, shadeWorld(), so)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    RigDesc rd{};
    rd.width = 256;
    rd.height = 192;
    rd.shade = true;
    rd.texelsPerPixel = 2.f;
    const int rc = initRig(ctx, rig, rd);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: no kernels built\n");
        return rc < 0 ? 1 : kSkip;
    }
    rg::Graph graph;
    FrameState fs;
    Mirror mirrors[kLanguages];
    const u32 W = rd.width;
    const u32 H = rd.height;
    const u32 pixels = W * H;
    const u32 filters[3] = {kFilterPcf, kFilterPcss, kFilterHard};
    for (u32 frame = 0; frame < 3u; ++frame) {
        beginSceneFrame(ctx, s);
        if (frame == 1u) {
            Box b = s.world.boxes[1];
            b.lo.x += 0.3;
            b.hi.x += 0.3;
            s.moveBox(1, b);
        }
        const Camera cam = shadeCamera(frame);
        for (u32 k = 0; k < kLanguages; ++k) {
            std::memset(ctx.fdumps[k].mapped, 0, ctx.fdumps[k].desc.size); // the forward pass writes covered texels only
        }
        FrameOptions o{};
        o.shade = true;
        o.locals = true;
        o.filter.filter = filters[frame];
        o.filter.pcfRadius = 1;
        o.filter.sunTanAngle = 0.03f;
        o.filter.pcssMaxRadius = 4;
        if (!runFrame(ctx, s, rig, graph, cam, so.sun, o, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        RasterReport reports[kLanguages];
        checkRasterAll(ctx, s, rig, fs, mirrors, reports, frame);
        GBufferCpu g{};
        decodeGBuffer(ctx, fs, pixels, g);
        std::vector<GpuLight> table(s.gpu.lightHighWater());
        for (u32 i = 0; i < table.size(); ++i) {
            table[i] = s.gpu.light(i);
        }
        std::vector<Vec4> dumps[kLanguages];
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
            ShadowReferenceSource src{};
            src.shadow = &rig.shadows[k].constants();
            src.vsm = &rig.vsm[k].constants();
            src.store.pageTable = mirrors[k].pte.data();
            src.store.pool = mirrors[k].pool.data();
            src.store.poolWidth = rig.vsm[k].poolImage().desc.width;
            src.store.local = mirrors[k].local.data();
            src.store.localWidth = rig.shadows[k].localImage().desc.width;
            std::vector<Vec4> ref;
            shadeShadowedReference(sd, src, ref);
            dumps[k].resize(pixels);
            std::memcpy(dumps[k].data(), ctx.dumps[k].mapped, static_cast<usize>(pixels) * sizeof(Vec4));
            ShadeStats st{};
            compareShade(sd.gbuffer, dumps[k], ref, src, sd.camera, W, H, st);
            std::printf("  frame %u %-5s shade (%s): %u shaded, %u bad, %u alpha, %u ambiguous (excluded), max rel %.2e; shadowed "
                        "sun %u / spot %u / point %u, fractional %u\n",
                        frame, rig.language[k], filters[frame] == kFilterPcf ? "pcf1" : (filters[frame] == kFilterPcss ? "pcss" : "hard"),
                        st.shaded, st.bad, st.alphaBad, st.ambiguous, st.maxRel, st.shadowed[0], st.shadowed[1], st.shadowed[2],
                        st.partial);
            expect(st.bad == 0u && st.alphaBad == 0u, "light.shade with shadows == the CPU reference (1e-4 |ref| + 1e-6)");
            expect(st.ambiguous * 1000u <= st.shaded, "ambiguous shadow comparisons stay below 0.1 %");
            expect(st.shadowed[0] * 20u > st.shaded && st.shadowed[1] > 0u && st.shadowed[2] > 0u,
                   "the sun, the spot and the point light cast shadows on screen");
            if (filters[frame] != kFilterHard) {
                expect(st.partial > 0u, "the soft filters produce fractional visibility");
            }
            // Forward: the transparent sphere's layer (the only transparent draw).
            const forward::ForwardDumpTexel* fd = static_cast<const forward::ForwardDumpTexel*>(ctx.fdumps[k].mapped);
            const u32 draws = rig.forward[k].drawCount();
            if (draws >= 1u) {
                GBufferCpu fg{};
                fg.depth.assign(pixels, 1.f);
                fg.rt0.assign(pixels, Vec4{});
                fg.rt1.assign(pixels, Vec4{});
                fg.rt2.assign(pixels, Vec4{});
                fg.rt5.assign(pixels, Vec4{});
                std::vector<Vec4> fgpu(pixels, Vec4{});
                std::vector<u8> covered(pixels, 0u);
                for (u32 p = 0; p < pixels; ++p) {
                    const forward::ForwardDumpTexel& t = fd[p];
                    if (t.covered == 0u) {
                        continue;
                    }
                    covered[p] = 1u;
                    fg.depth[p] = t.depth;
                    fg.rt0[p] = {t.rt0[0], t.rt0[1], t.rt0[2], t.rt0[3]};
                    fg.rt1[p] = {t.rt1[0], t.rt1[1], t.rt1[2], t.rt1[3]};
                    fg.rt2[p] = {t.rt2[0], t.rt2[1], t.rt2[2], t.rt2[3]};
                    fg.rt5[p] = {t.rt5[0], t.rt5[1], t.rt5[2], t.rt5[3]};
                    fgpu[p] = {t.radiance[0], t.radiance[1], t.radiance[2], 1.f};
                }
                ShadeReferenceDesc fsd = sd;
                fsd.gbuffer = GBufferTexels{fg.depth.data(), fg.rt0.data(), fg.rt1.data(), fg.rt2.data(), fg.rt5.data()};
                std::vector<Vec4> fref;
                shadeShadowedReference(fsd, src, fref);
                ShadeStats fst{};
                compareShade(fsd.gbuffer, fgpu, fref, src, sd.camera, W, H, fst, &covered);
                std::printf("  frame %u %-5s forward layer: %u shaded, %u bad, %u ambiguous, max rel %.2e; shadowed sun %u\n", frame,
                            rig.language[k], fst.shaded, fst.bad, fst.ambiguous, fst.maxRel, fst.shadowed[0]);
                expect(fst.shaded > 100u && fst.bad == 0u && fst.alphaBad == 0u, "forward pass with shadows == the CPU reference");
                expect(fst.ambiguous * 100u <= fst.shaded, "forward: ambiguous comparisons stay rare");
            } else {
                expect(false, "the transparent sphere is drawn by the forward pass");
            }
        }
        if (rig.built[0] && rig.built[1]) {
            u32 differ = 0;
            for (u32 p = 0; p < pixels; ++p) {
                const f64 d = std::fabs(static_cast<f64>(dumps[0][p].x) - dumps[1][p].x) + std::fabs(static_cast<f64>(dumps[0][p].y) - dumps[1][p].y) +
                              std::fabs(static_cast<f64>(dumps[0][p].z) - dumps[1][p].z);
                differ += d > 2e-4 * (std::fabs(static_cast<f64>(dumps[0][p].x)) + 1e-3) ? 1u : 0u;
            }
            std::printf("  frame %u Slang vs GLSL shade: %u pixels differ beyond 2 x tolerance\n", frame, differ);
            expect(differ == 0u, "Slang == GLSL (shade within tolerance)");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- local ------------------------------------------------------------------------------------------
int runLocal(Context& ctx) {
    Scene s;
    SceneOptions so{};
    so.lights = true;
    so.sun = normalize(D3{0.45, -1.0, -0.35});
    if (!buildScene(ctx, s, shadeWorld(), so)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    RigDesc rd{};
    rd.width = 256;
    rd.height = 192;
    const int rc = initRig(ctx, rig, rd);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: no kernels built\n");
        return rc < 0 ? 1 : kSkip;
    }
    rg::Graph graph;
    FrameState fs;
    Mirror mirrors[kLanguages];
    const Camera cam = shadeCamera(0);
    // Probes: ground points around each local light (analytic answers away from shadow edges).
    struct LocalProbe {
        D3 p{};
        u32 slot = 0;
    };
    std::vector<LocalProbe> probes;
    const u32 slots[3] = {kSpot, kPointA, kPointB};
    for (u32 li = 0; li < 3u; ++li) {
        const GpuLight& l = s.gpu.light(slots[li]);
        for (f64 dz = -l.range; dz <= l.range; dz += 0.08) {
            for (f64 dx = -l.range; dx <= l.range; dx += 0.08) {
                LocalProbe p{};
                p.p = {l.position[0] + dx, 0.0, l.position[2] + dz};
                p.slot = slots[li];
                if (probes.size() < kMaxProbes && std::fabs(p.p.x) < 29.0 && std::fabs(p.p.z) < 29.0) {
                    probes.push_back(p);
                }
            }
        }
    }
    const u32 count = static_cast<u32>(probes.size());
    VsmProbeInput* in = static_cast<VsmProbeInput*>(ctx.probeIn.mapped);
    for (u32 i = 0; i < count; ++i) {
        in[i] = VsmProbeInput{};
        in[i].position[0] = static_cast<f32>(probes[i].p.x);
        in[i].position[2] = static_cast<f32>(probes[i].p.z);
        in[i].slot = probes[i].slot;
    }
    for (u32 frame = 0; frame < 6u; ++frame) {
        beginSceneFrame(ctx, s);
        if (frame == 3u) {
            Box b = s.world.boxes[14]; // inside the spot's range only
            b.lo.z -= 0.2;
            b.hi.z -= 0.2;
            s.moveBox(14, b);
        } else if (frame == 5u) {
            Box b = s.world.boxes[15]; // inside point light A's range only
            b.lo.x -= 0.2;
            b.hi.x -= 0.2;
            s.moveBox(15, b);
        }
        FrameOptions o{};
        o.locals = true;
        o.probes = frame == 0u ? count : 0u;
        o.filter.filter = kFilterHard;
        if (!runFrame(ctx, s, rig, graph, cam, so.sun, o, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        RasterReport reports[kLanguages];
        checkRasterAll(ctx, s, rig, fs, mirrors, reports, frame);
        const RasterReport& r = reports[firstBuilt(rig)];
        if (frame == 0u) {
            expect(r.localPages == 13u, "frame 0 renders the spot's page and both point lights' 6 faces");
            for (u32 k = 0; k < kLanguages; ++k) {
                if (!rig.built[k]) {
                    continue;
                }
                const VsmProbeOutput* out = static_cast<const VsmProbeOutput*>(ctx.probeOut[k].mapped);
                const VsmShadowConstants& sc = rig.shadows[k].constants();
                u32 checked[3] = {}, bad[3] = {}, shadowed[3] = {};
                for (u32 i = 0; i < count; ++i) {
                    const u32 li = probes[i].slot == kSpot ? 0u : (probes[i].slot == kPointA ? 1u : 2u);
                    const VsmLocalLight& e = sc.local[li];
                    const D3 lp{e.position[0], e.position[1], e.position[2]};
                    const D3 rel = probes[i].p - lp;
                    const f64 dist = std::sqrt(dot(rel, rel));
                    if (dist > e.range * 0.95 || dist < 0.3) {
                        continue;
                    }
                    if (e.type == kLocalSpot) {
                        const D3 f{e.forward[0], e.forward[1], e.forward[2]};
                        const f64 cz = dot(rel, f);
                        const D3 rr{e.right[0], e.right[1], e.right[2]};
                        const D3 uu{e.up[0], e.up[1], e.up[2]};
                        if (!(cz > 0.1) || std::fabs(dot(rel, rr) / cz) * e.invTanHalf > 0.95 ||
                            std::fabs(dot(rel, uu) / cz) * e.invTanHalf > 0.95) {
                            continue; // outside the page frustum (with a margin)
                        }
                    }
                    // Analytic: segment receiver -> light against the boxes, stable within the margin.
                    const f64 texel = 2.0 * dist / (e.invTanHalf * 128.0);
                    const f64 margin = 4.0 * texel + 0.01;
                    auto occ = [&](const D3& q) {
                        const D3 d = lp - q;
                        const f64 len = std::sqrt(dot(d, d));
                        return s.world.occluded(q, d * (1.0 / len), len);
                    };
                    const bool truth = occ(probes[i].p);
                    bool stable = true;
                    const D3 offs[4] = {{margin, 0, 0}, {-margin, 0, 0}, {0, 0, margin}, {0, 0, -margin}};
                    for (const D3& d : offs) {
                        stable = stable && occ(probes[i].p + d) == truth;
                    }
                    bool nearBox = false;
                    for (const Box& b : s.world.boxes) {
                        nearBox = nearBox || (probes[i].p.x > b.lo.x - margin && probes[i].p.x < b.hi.x + margin &&
                                              probes[i].p.z > b.lo.z - margin && probes[i].p.z < b.hi.z + margin);
                    }
                    if (!stable || nearBox) {
                        continue;
                    }
                    ++checked[li];
                    shadowed[li] += truth ? 1u : 0u;
                    bad[li] += (out[i].visibility < 0.5f) != truth ? 1u : 0u;
                }
                std::printf("  local probes %-5s: spot %u checked (%u shadowed) %u wrong; point A %u (%u) %u; point B %u (%u) %u\n",
                            rig.language[k], checked[0], shadowed[0], bad[0], checked[1], shadowed[1], bad[1], checked[2], shadowed[2],
                            bad[2]);
                for (u32 li = 0; li < 3u; ++li) {
                    expect(checked[li] > 500u && shadowed[li] > 20u, "local light probes cover lit and shadowed ground");
                    expect(bad[li] == 0u, "local shadows == the analytic occlusion towards the light (spot page / cube faces)");
                }
            }
        } else if (frame == 3u) {
            expect(r.localPages == 1u && r.localExecuted == 1u, "a box moved inside the spot's range re-renders exactly its page");
        } else if (frame == 5u) {
            expect(r.localPages == 6u && r.localExecuted == 6u, "a box moved next to a point light re-renders its 6 faces");
        } else {
            expect(r.localPages == 0u && r.localExecuted == 0u && r.dirPages == 0u, "static frame: 0 local and 0 directional pages");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- cache -----------------------------------------------------------------------------------------
int runCache(Context& ctx) {
    Scene s;
    SceneOptions so{};
    so.lights = true;
    so.sun = normalize(D3{0.45, -1.0, -0.35});
    if (!buildScene(ctx, s, shadeWorld(), so)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    RigDesc rd{};
    rd.width = 256;
    rd.height = 192;
    const int rc = initRig(ctx, rig, rd);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: no kernels built\n");
        return rc < 0 ? 1 : kSkip;
    }
    rg::Graph graph;
    FrameState fs;
    Mirror mirrors[kLanguages];
    const Camera cam = shadeCamera(1);
    std::vector<u32> prevPool, prevLocal;
    for (u32 frame = 0; frame < 7u; ++frame) {
        beginSceneFrame(ctx, s);
        if (frame == 4u) {
            Box b = s.world.boxes[15];
            b.lo.x += 0.25;
            b.hi.x += 0.25;
            s.moveBox(15, b);
        }
        FrameOptions o{};
        o.locals = true;
        if (!runFrame(ctx, s, rig, graph, cam, so.sun, o, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        RasterReport reports[kLanguages];
        checkRasterAll(ctx, s, rig, fs, mirrors, reports, frame);
        const u32 k0 = firstBuilt(rig);
        const RasterReport& r = reports[k0];
        if (frame == 0u) {
            expect(r.dirPages > 30u && r.localPages == 13u, "frame 0 renders every requested page and every local page");
        } else if (frame == 4u) {
            std::printf("  frame 4: moved box: %u directional pages, %u local pages re-rendered (dirty 0x%x)\n", r.dirPages, r.localPages,
                        r.dirtyMask);
            expect(r.dirPages > 0u && r.dirPages * 4u < r.mapped, "a moved box re-renders only the pages under its footprint");
            expect(r.localPages == 6u, "only point light A (whose range the box touches) re-renders");
        } else {
            expect(r.dirPages == 0u && r.dirExecuted == 0u && r.localPages == 0u && r.localExecuted == 0u,
                   "cached frame: 0 directional and 0 local pages rendered when nothing moves");
            if (frame != 5u) {
                expect(mirrors[k0].gpuPool == prevPool && mirrors[k0].gpuLocal == prevLocal,
                       "a cached frame leaves the pool and the local atlas bit-identical");
            }
        }
        prevPool = mirrors[k0].gpuPool;
        prevLocal = mirrors[k0].gpuLocal;
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- zero_alloc -------------------------------------------------------------------------------------
void hookBegin(const rg::PassContext&, const char* name, void*) { t_count = std::strncmp(name, "vsm.", 4) == 0; }
void hookEnd(const rg::PassContext&, const char*, void*) { t_count = false; }

int runZeroAlloc(Context& ctx, bool countAllocations) {
    Scene s;
    SceneOptions so{};
    so.lights = true;
    so.sun = normalize(D3{0.45, -1.0, -0.35});
    if (!buildScene(ctx, s, shadeWorld(), so)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    RigDesc rd{};
    rd.width = 256;
    rd.height = 192;
    const int rc = initRig(ctx, rig, rd);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: no kernels built\n");
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
        beginSceneFrame(ctx, s);
        Box b = s.world.boxes[4];
        const f64 step = frame % 2u == 0u ? 0.05 : -0.05;
        b.lo.x += step;
        b.hi.x += step;
        s.moveBox(4, b);
        s.gpu.commit();
        ctx.upload.flush();
        const Camera cam = shadeCamera(frame % 3u);
        FrameOptions o{};
        o.readback = false;
        o.locals = true;
        t_allocations = 0;
        bool ok = beginFrame(ctx, s, rig, cam, so.sun, o, fs, measure);
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
    const u32 k0 = firstBuilt(rig);
    expect(rig.shadows[k0].stats().passes == 6u, "vsm.raster_reset, raster, local_dirty, local_list, local_clear, local_raster");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, a box moving, 3 local lights, 2 VSMs + 2 VsmShadows)\n"
                    "  VsmShadows::beginFrame: %llu operator-new calls; vsm.* pass callbacks: %llu; whole graph build: %llu\n",
                    kTotal - kWarmup, s.gpu.instanceHighWater(), begin, callbacks, build);
        expect(begin == 0u && callbacks == 0u, "the VSM raster makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the VSM raster passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mode = "seam";
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
        if (mode == "seam") {
            rc = runSeam(ctx);
        } else if (mode == "shade") {
            rc = runShade(ctx);
        } else if (mode == "local") {
            rc = runLocal(ctx);
        } else if (mode == "cache") {
            rc = runCache(ctx);
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
