// WP-8.1 froxel fog Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_volumetric_gpu_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): the GPU scene's light table (WP-1.1, delta upload), the WP-2.1
// light assignment (light.bounds / bin / cull / scan / compact -> cluster lists), a test pass uploading the
// fog.apply inputs (RT4 R32F forward z/w depth, RGBA16F lit image; synthetic, test_rp_volumetric_gpu_scene.hpp),
// then one FroxelFog per kernel language built (Slang, GLSL) reading the lighting frame / lists / light table
// through BDA: fog.inject, fog.temporal, fog.integrate, fog.apply; then read-back copies (every work-buffer
// section, the lists buffer, the output image, the apply dump).
//
// Oracles: each GPU pass against its CPU reference (froxel_fog_kernel.hpp) run on the GPU's own read-back
// inputs, so a gate isolates one pass:
//   --mode passes     32 x 18 x 48 froxels (2 x 2 x 2 per 16 x 9 x 24 cluster), 96 x 64 apply extent, the
//                     courtyard scene (4 point, 2 spot lights, a sun, a free slot, a no-range spot; height fog +
//                     a sphere and a box volume), 6 frames of camera motion with jitter and reprojection, a light
//                     moving and one removed / re-added:
//                       lists         GPU cluster lists == the oracle (oracleLightGrid, WP-2.1's gate) entry for
//                                     entry
//                       fog.inject    == inject_froxel with the GPU lists
//                       fog.temporal  == temporal_froxel on the read-back current + previous history; the
//                                     previous history == last frame's output bit for bit (ping-pong)
//                       fog.integrate == integrate_column on the read-back history
//                       fog.apply     == apply_pixel on the read-back volume (dump), RGBA16F == dump within one
//                                     half ulp
//                     plus Slang == GLSL within tolerance.
//   --mode analytic   homogeneous medium, ambient only: transmittance e^{-sigma |ray| d} and in-scattering
//                     albedo L (1 - T) at every slice boundary; fog.apply on a constant-depth image vs the closed
//                     form (interpolation tolerance)
//   --mode converge   static camera, 256 frames: the distance to the frame one jitter period earlier falls by
//                     1e4 (geometric convergence), the temporal variance of the integrated fog over the last period
//                     is below epsilon and the period mean == the CPU jitter-period mean
//   --mode ghosting   moving camera, 96 frames, reprojected history and a same-froxel control side by side:
//                     ghosting (relative L1 to the froxel mean) and trail energy below thresholds, reprojection at
//                     least halves the control's ghosting
//   --mode shadow     WP-3.2 VSM shadow lookup (64 x 36 x 64 froxels): a box under the shadowed spot light
//                     (VsmShadows local page, PCF; the GPU scene's ground + box), VsmShadows::addSamplingUse before the fog,
//                     FogFrameDesc::shadows: fog.inject == inject_froxel whose shadow hook returns vsm.probe's
//                     visibility at the same points (the GPU's fuse_vsm_shadow at the fog's sample points), and
//                     the visibility == an analytic ray / box test from each froxel sample to the light (0 wrong
//                     away from the penumbra); the shadow removes the spot's in-scattering in the box's shadow
//   --mode zero_alloc 80 frames (16 warm-up, camera moving): 0 operator-new calls in FroxelFog::beginFrame, the
//                     fog.* pass callbacks and the whole graph build (validated run first; validation off for the
//                     count)
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Tolerances (parity): the kernels are line-for-line twins of the CPU reference in f32 with no contraction; the
// differences are the device exp / sqrt / division (Vulkan allows a few ulp) and the device log of the cluster
// slice lookup (a different cluster near a boundary changes no contribution: every light touching the sample is
// in both clusters' lists). Gate: |gpu - cpu| <= 2e-5 |cpu| + 1e-9 (+ 1e-7 x the frame maximum where a
// trilinear / division rounding moves a weight: temporal) on every channel.
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_reference.hpp>

#include "test_rp_volumetric_gpu_scene.hpp"
#include "test_rp_vsm_raster_common.hpp" // WP-3.2 analytic box world + GpuScene meshes (shadow mode)

#include <fuse/renderer/shadow/vsm_raster/vsm_shadows.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
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
using namespace fuse::renderer::volumetric_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace fk = fuse::renderer::volumetric_gpu::fog_kernel;
using lighting_gpu::ClusteredLighting;
using lighting_gpu::ClusteredLightingDesc;
using lighting_gpu::LightingFrameDesc;
using lighting_gpu::LightingGraphRefs;
using lighting_gpu::LightListHeader;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kW = 96;
constexpr u32 kH = 64;
constexpr u32 kFogs = 2u;       ///< fog instances per frame (two languages, or reprojected + control)
constexpr u32 kSections = 4u;   ///< Current, HistoryPrev, History, Integrated
constexpr usize kStagingBytes = 4u * 1024u * 1024u;

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

bool withinHalfUlp(f32 image, f32 dump) {
    const u16 bits = GBufferQuantize::floatToHalf(dump);
    const f32 lo = halfToFloat(GBufferQuantize::halfNextDown(bits));
    const f32 hi = halfToFloat(GBufferQuantize::halfNextUp(bits));
    return image >= std::min(lo, hi) && image <= std::max(lo, hi);
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
    UploadQueue upload;
    Buffer staging{};    ///< UploadQueue staging (GPU scene)
    Buffer imgStaging{}; ///< depth + lit texels
    Buffer readback{};
    Buffer dumps[kFogs]{};
    Buffer probeIn{};    ///< shadow mode: VsmProbeInput per froxel
    Buffer probeOut{};   ///< shadow mode: VsmProbeOutput per froxel
    Texture images[2]{}; ///< RT4 (R32F), lit (RGBA16F)
    u32 layouts[2] = {0u, 0u};
    u8 queues[2] = {rg::kNoQueue, rg::kNoQueue};
    u8 imgStagingQueue = rg::kNoQueue;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            for (Texture& t : images) {
                if (t.image != nullptr) {
                    allocator->destroyImage(t);
                }
            }
            for (Buffer* b : {&staging, &imgStaging, &readback, &dumps[0], &dumps[1], &probeIn, &probeOut}) {
                if (b->handle != nullptr) {
                    allocator->destroyBuffer(*b);
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

bool makeBuffer(Context& ctx, Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(size);
    d.usage = usage;
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
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
    instanceDesc.appName = "fuse_rp_volumetric_gpu";
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
    const FogCapabilities caps = queryFogCapabilities(ctx.device.get());
    if (!caps.fog || !lighting_gpu::queryLightingCapabilities(ctx.device.get()).lighting) {
        std::printf("SKIP: froxel fog / clustered lighting unsupported: %s\n", caps.reason);
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
    const BufferUsage storageBda =
        static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    bool ok = makeBuffer(ctx, ctx.staging, kStagingBytes, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_fog.staging") &&
              makeBuffer(ctx, ctx.imgStaging, static_cast<u64>(kW) * kH * 16u, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu,
                         "rp_fog.img_staging");
    for (Buffer& d : ctx.dumps) {
        ok = ok && makeBuffer(ctx, d, static_cast<u64>(kW) * kH * 16u, storageBda, MemoryUsage::GpuToCpu, "rp_fog.dump") &&
             d.deviceAddress != 0u;
    }
    const GpuFormat formats[2] = {GpuFormat::R32Sfloat, GpuFormat::R16G16B16A16Sfloat};
    const char* names[2] = {"rp_fog.rt4", "rp_fog.lit"};
    for (u32 i = 0; i < 2u; ++i) {
        TextureDesc d{};
        d.width = kW;
        d.height = kH;
        d.format = formats[i];
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        d.name = names[i];
        ok = ok && ctx.allocator->createImage(d, ctx.images[i]);
    }
    if (!ok || !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: buffers / images / UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

/// Writes the apply inputs into the image staging buffer; returns the CPU copies the reference reads (the
/// lit image as the GPU sees it: rounded to half).
void writeImages(Context& ctx, std::vector<f32>& depth, std::vector<Vec4>& lit) {
    fog_test::images(kW, kH, depth, lit);
    u8* base = static_cast<u8*>(ctx.imgStaging.mapped);
    const usize n = static_cast<usize>(kW) * kH;
    std::memcpy(base, depth.data(), n * 4u);
    u8* litBytes = base + align256(n * 4u);
    for (usize i = 0; i < n; ++i) {
        const u16 h[4] = {GBufferQuantize::floatToHalf(lit[i].x), GBufferQuantize::floatToHalf(lit[i].y),
                          GBufferQuantize::floatToHalf(lit[i].z), GBufferQuantize::floatToHalf(lit[i].w)};
        std::memcpy(litBytes + i * 8u, h, 8u);
        lit[i] = Vec4{halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]), halfToFloat(h[3])};
    }
}

// --- rig -------------------------------------------------------------------------------------------
struct Lang {
    FogKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{FogKernelLanguage::Slang, "slang"}, {FogKernelLanguage::Glsl, "glsl"}};

struct Rig {
    gpu_scene::GpuScene scene;
    std::vector<gpu_scene::LightHandle> lights;
    ClusteredLighting lighting;
    FroxelFog fog[kFogs];
    bool built[kFogs] = {false, false};
    const char* label[kFogs] = {"", ""};
    FroxelFogSettings settings[kFogs];
    bool apply = true;
    bool readback = true;
    bool withLights = true;
    // shadow mode (WP-3.2): a box world, one shadowed spot light, probes at the fog's sample points
    vsm::VsmShadows* shadows = nullptr;
    vsmr_test::World world;
    std::vector<geometry::MeshletMesh> meshes;
    vsm::VsmLocalLightDesc local{};
    u32 probeCount = 0;
};

bool initRig(Context& ctx, Rig& rig, bool twoLanguages, bool withLights, bool boxWorld = false) {
    gpu_scene::GpuSceneDesc sd{};
    sd.device = ctx.device.get();
    sd.allocator = ctx.allocator.get();
    sd.upload = &ctx.upload;
    sd.bindless = &ctx.bindless;
    sd.lightCapacity = 64;
    if (!rig.scene.init(sd) || !rig.scene.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
    for (const gpu_scene::GpuLight& l : fog_test::lights()) {
        rig.lights.push_back(rig.scene.addLight(l));
    }
    if (boxWorld) {
        // Ground + a slab under the spot light of slot 4 (fog_test::lights: at (0, 5, -8), pointing down).
        rig.world.groundSize = 80.f;
        rig.world.boxes.push_back(vsmr_test::Box{{-0.7, 2.4, -8.7}, {0.7, 2.8, -7.3}});
        if (!vsmr_test::buildMeshes(rig.meshes, rig.world.groundSize)) {
            return false;
        }
        for (u32 i = 0; i < rig.meshes.size(); ++i) {
            if (rig.scene.addMeshletMesh(rig.meshes[i]) != i) {
                return false;
            }
        }
        if (!vsmr_test::addInstances(rig.scene, rig.world)) {
            return false;
        }
    }
    rig.scene.commit();
    ctx.upload.flush();
    if (!ctx.upload.waitAll()) {
        return false;
    }
    ClusteredLightingDesc ld{};
    ld.device = ctx.device.get();
    ld.allocator = ctx.allocator.get();
    ld.bindless = &ctx.bindless;
    ld.clusters = fog_test::clusters();
    ld.lightCapacity = 64;
    ld.energyCompensation = false;
    if (!rig.lighting.init(ld)) {
        return false;
    }
    rig.withLights = withLights;
    for (u32 k = 0; k < kFogs; ++k) {
        FroxelFogDesc fd{};
        fd.device = ctx.device.get();
        fd.allocator = ctx.allocator.get();
        fd.bindless = &ctx.bindless;
        fd.language = twoLanguages ? kLangs[k].language : FogKernelLanguage::Auto;
        rig.built[k] = rig.fog[k].init(fd);
        rig.label[k] = twoLanguages ? kLangs[k].name : (k == 0u ? "reprojected" : "same-froxel");
        rig.settings[k] = fog_test::settings();
    }
    return rig.built[0] || rig.built[1];
}

void destroyRig(Context& ctx, Rig& rig) {
    vkDeviceWaitIdle(ctx.vkDevice);
    for (FroxelFog& f : rig.fog) {
        f.destroy();
    }
    rig.lighting.destroy();
    rig.scene.destroy();
}

// Read-back layout: per fog, 4 sections + the output image; then the lists buffer.
struct Layout {
    u64 section[kFogs][kSections] = {};
    u64 image[kFogs] = {};
    u64 lists = 0;
    u64 listsBytes = 0;
    u64 end = 0;
};

Layout makeLayout(const Rig& rig) {
    Layout l{};
    u64 at = 0;
    for (u32 k = 0; k < kFogs; ++k) {
        const u64 bytes = rig.built[k] ? rig.fog[k].copyBytes() : 0u;
        for (u32 s = 0; s < kSections; ++s) {
            l.section[k][s] = at;
            at = align256(at + std::max<u64>(bytes, 16u));
        }
        l.image[k] = at;
        at = align256(at + static_cast<u64>(kW) * kH * 8u);
    }
    l.lists = at;
    l.listsBytes = rig.lighting.listsBuffer().desc.size;
    l.end = align256(at + l.listsBytes);
    return l;
}

struct CopyRecord {
    rg::BufferRef src;
    rg::TextureRef image;
    rg::BufferRef dst;
    u64 dstOffset = 0;
    u64 bytes = 0;
};

struct GraphRecord {
    Context* ctx = nullptr;
    rg::BufferRef imgStaging;
    rg::TextureRef images[2];
    CopyRecord copies[8];
    u32 copyCount = 0;
};
GraphRecord g_record{};

void recordUpload(const rg::PassContext& pc, void* user) {
    const GraphRecord& r = *static_cast<const GraphRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const u64 offsets[2] = {0u, align256(static_cast<u64>(kW) * kH * 4u)};
    for (u32 i = 0; i < 2u; ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset = offsets[i];
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kW, kH, 1};
        vkCmdCopyBufferToImage(cmd, static_cast<VkBuffer>(pc.buffer(r.imgStaging)), static_cast<VkImage>(pc.image(r.images[i])),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
}

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    if (c.image.valid()) {
        VkBufferImageCopy region{};
        region.bufferOffset = c.dstOffset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kW, kH, 1};
        vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
        return;
    }
    const VkBufferCopy region{0u, c.dstOffset, c.bytes};
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.src)), static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

void buildGraph(Context& ctx, Rig& rig, rg::Graph& graph, const Layout& layout) {
    graph.reset();
    GraphRecord& r = g_record;
    r.ctx = &ctx;
    r.copyCount = 0;
    const gpu_scene::GpuSceneGraphRefs sceneRefs = rig.scene.importInto(graph);
    const LightingGraphRefs lighting = rig.lighting.importInto(graph);
    rig.lighting.addAssignment(graph, lighting, sceneRefs);
    const char* names[2] = {"rp_fog.rt4", "rp_fog.lit"};
    const u32 formats[2] = {static_cast<u32>(GpuFormat::R32Sfloat), static_cast<u32>(GpuFormat::R16G16B16A16Sfloat)};
    for (u32 i = 0; i < 2u; ++i) {
        rg::ImportedImage im{};
        im.image = ctx.images[i].image;
        im.view = ctx.images[i].view;
        im.format = formats[i];
        im.width = kW;
        im.height = kH;
        im.initialLayout = ctx.layouts[i];
        im.initialQueue = ctx.queues[i];
        im.layoutTracker = &ctx.layouts[i];
        im.queueTracker = &ctx.queues[i];
        im.name = names[i];
        r.images[i] = graph.importImage(im);
    }
    r.imgStaging = graph.importBuffer(rg::ImportedBuffer{ctx.imgStaging.handle, ctx.imgStaging.desc.size, ctx.imgStagingQueue,
                                                         &ctx.imgStagingQueue, "rp_fog.img_staging"});
    graph.addPass("test.upload", &recordUpload, &r)
        .use(r.imgStaging, rg::Access::TransferSrc)
        .use(r.images[0], rg::Access::TransferDst)
        .use(r.images[1], rg::Access::TransferDst);
    vsm::VsmShadowGraphRefs shadowRefs{};
    if (rig.shadows != nullptr) {
        shadowRefs = rig.shadows->importInto(graph);
        rig.shadows->addRaster(graph, shadowRefs, nullptr, sceneRefs);
        rig.shadows->addSamplingUse(graph, shadowRefs, nullptr, rg::kStageCompute); // before fog.inject
    }
    rg::BufferRef rb{};
    if (rig.readback) {
        rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, layout.end, rg::kNoQueue, nullptr, "rp_fog.readback"});
    }
    for (u32 k = 0; k < kFogs; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const FogGraphRefs refs = rig.fog[k].importInto(graph);
        FogGraphInputs in{};
        in.lights = lighting.lists;
        in.scene = &sceneRefs;
        in.depth = r.images[0];
        in.lit = r.images[1];
        if (rig.readback) {
            in.dump = graph.importBuffer(
                rg::ImportedBuffer{ctx.dumps[k].handle, ctx.dumps[k].desc.size, rg::kNoQueue, nullptr, "rp_fog.dump"});
        }
        rig.fog[k].addPasses(graph, refs, in);
        if (!rig.readback) {
            continue;
        }
        const FogCopySource sources[kSections] = {FogCopySource::Current, FogCopySource::HistoryPrev, FogCopySource::History,
                                                  FogCopySource::Integrated};
        for (u32 s = 0; s < kSections; ++s) {
            rig.fog[k].addCopy(graph, refs, sources[s], rb, layout.section[k][s]);
        }
        if (refs.output.valid()) {
            CopyRecord& c = r.copies[r.copyCount++];
            c = CopyRecord{};
            c.image = refs.output;
            c.dst = rb;
            c.dstOffset = layout.image[k];
            graph.addPass("test.readback_image", &recordCopy, &c)
                .use(refs.output, rg::Access::TransferSrc)
                .use(rb, rg::Access::TransferDst, rg::BufferRange{layout.image[k], static_cast<u64>(kW) * kH * 8u});
        }
        graph.addPass("test.dump_host", nullptr, nullptr).use(in.dump, rg::Access::HostRead);
    }
    if (rig.shadows != nullptr && rig.probeCount > 0u) {
        const rg::BufferRef in =
            graph.importBuffer(rg::ImportedBuffer{ctx.probeIn.handle, ctx.probeIn.desc.size, rg::kNoQueue, nullptr, "rp_fog.probe_in"});
        const rg::BufferRef out = graph.importBuffer(
            rg::ImportedBuffer{ctx.probeOut.handle, ctx.probeOut.desc.size, rg::kNoQueue, nullptr, "rp_fog.probe_out"});
        rig.shadows->addProbe(graph, shadowRefs, nullptr, in, ctx.probeIn.deviceAddress, out, ctx.probeOut.deviceAddress,
                              rig.probeCount);
        graph.addPass("test.probe_host", nullptr, nullptr).use(out, rg::Access::HostRead);
    }
    if (rig.readback) {
        CopyRecord& c = r.copies[r.copyCount++];
        c = CopyRecord{};
        c.src = lighting.lists;
        c.dst = rb;
        c.dstOffset = layout.lists;
        c.bytes = layout.listsBytes;
        graph.addPass("test.readback_lists", &recordCopy, &c)
            .use(lighting.lists, rg::Access::TransferSrc, rg::BufferRange{0, layout.listsBytes})
            .use(rb, rg::Access::TransferDst, rg::BufferRange{layout.lists, layout.listsBytes});
        graph.addPass("test.readback_host", nullptr, nullptr).use(rb, rg::Access::HostRead);
    }
}

bool ensureReadback(Context& ctx, const Layout& layout) {
    if (ctx.readback.handle != nullptr && ctx.readback.desc.size >= layout.end) {
        return true;
    }
    if (ctx.readback.handle != nullptr) {
        vkDeviceWaitIdle(ctx.vkDevice);
        ctx.allocator->destroyBuffer(ctx.readback);
        ctx.readback = Buffer{};
    }
    return makeBuffer(ctx, ctx.readback, layout.end, BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "rp_fog.readback");
}

/// One frame: scene commit, lighting + fog constants, graph, execute.
bool runFrame(Context& ctx, Rig& rig, rg::Graph& graph, const ClusterCameraDesc& cam, Layout& layout) {
    rig.scene.commit();
    ctx.upload.flush();
    LightingFrameDesc lf{};
    lf.camera = cam;
    lf.scene = rig.scene.headerHandle();
    lf.lightCount = rig.scene.lightHighWater();
    bool ok = rig.lighting.beginFrame(ctx.serial, lf);
    if (rig.shadows != nullptr) {
        vsm::VsmShadowFrameDesc sf{};
        sf.scene = rig.scene.headerHandle();
        sf.instanceCount = rig.scene.instanceHighWater();
        sf.locals = &rig.local;
        sf.localCount = 1;
        ok = rig.shadows->beginFrame(ctx.serial, sf) && ok;
    }
    for (u32 k = 0; k < kFogs; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        FogFrameDesc fd{};
        fd.camera = cam;
        fd.shadows = rig.shadows != nullptr ? rig.shadows->shadowConstantsAddress() : 0u;
        fd.lighting = rig.withLights ? rig.lighting.frameConstantsAddress() : 0u;
        fd.depth = &ctx.images[0];
        fd.lit = &ctx.images[1];
        fd.dumpAddress = rig.readback ? ctx.dumps[k].deviceAddress : 0u;
        ok = rig.fog[k].beginFrame(ctx.serial, rig.settings[k], fd) && ok;
    }
    if (!ok) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    if (rig.shadows != nullptr && rig.built[0]) {
        // Probes at this frame's fog sample points (the jittered froxel points fog.inject evaluates).
        const FogFrameConstants& c = rig.fog[0].constants();
        vsm::VsmProbeInput* in = static_cast<vsm::VsmProbeInput*>(ctx.probeIn.mapped);
        rig.probeCount = c.froxelCount;
        for (u32 y = 0; y < c.gridY; ++y) {
            for (u32 x = 0; x < c.gridX; ++x) {
                for (u32 z = 0; z < c.gridZ; ++z) {
                    const Vec3 p = fk::froxel_point(c, x, y, z, c.jitter[0], c.jitter[1], c.jitter[2]);
                    vsm::VsmProbeInput& q = in[fk::froxel_index(c, x, y, z)];
                    q = vsm::VsmProbeInput{};
                    q.position[0] = p.x;
                    q.position[1] = p.y;
                    q.position[2] = p.z;
                    q.slot = rig.local.slot;
                    q.normal[0] = q.normal[1] = q.normal[2] = 0.f; // the fog's lookup: no normal offset
                }
            }
        }
    }
    layout = makeLayout(rig);
    if (rig.readback && !ensureReadback(ctx, layout)) {
        std::fprintf(stderr, "  readback buffer\n");
        return false;
    }
    buildGraph(ctx, rig, graph, layout);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    for (FroxelFog& f : rig.fog) {
        f.collectRetired(ctx.serial);
    }
    rig.lighting.collectRetired(ctx.serial);
    rig.scene.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    return true;
}

void beginSceneFrame(Context& ctx, Rig& rig) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
}

// --- read-back ---------------------------------------------------------------------------------------
const u8* rb(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

std::vector<Vec4> readTexels(Context& ctx, u64 offset, u32 count) {
    std::vector<Vec4> v(count);
    std::memcpy(v.data(), rb(ctx, offset), static_cast<usize>(count) * 16u);
    return v;
}

void readLists(Context& ctx, const Rig& rig, const Layout& layout, FogLightLists& out) {
    const lighting_gpu::LightingBufferLayout& l = rig.lighting.layout();
    const u8* base = rb(ctx, layout.lists);
    LightListHeader header{};
    std::memcpy(&header, base + l.header, sizeof(header));
    const u32 clusters = rig.lighting.clusterDesc().clusterCount();
    out.grid.resize(static_cast<usize>(clusters) * 2u);
    std::memcpy(out.grid.data(), base + l.grid, out.grid.size() * 4u);
    out.lightList.resize(std::min<u32>(header.totalEntries, clusters * rig.lighting.capacity()));
    std::memcpy(out.lightList.data(), base + l.lightList, out.lightList.size() * 4u);
    out.directional.resize(std::min(header.directionalCount, rig.lighting.lightCapacity()));
    std::memcpy(out.directional.data(), base + l.directional, out.directional.size() * 4u);
}

std::vector<gpu_scene::GpuLight> sceneLights(const Rig& rig) {
    std::vector<gpu_scene::GpuLight> v(rig.scene.lightHighWater());
    for (u32 i = 0; i < v.size(); ++i) {
        v[i] = rig.scene.light(i);
    }
    return v;
}

// --- comparisons ---------------------------------------------------------------------------------------
struct Compare {
    u64 values = 0;
    u64 exact = 0;
    u64 bad = 0;
    f64 maxRel = 0.0;
    void add(f32 gpu, f32 ref, f64 tolRel, f64 tolAbs) {
        ++values;
        if (std::memcmp(&gpu, &ref, 4u) == 0) {
            ++exact;
            return;
        }
        const f64 d = std::fabs(static_cast<f64>(gpu) - ref);
        const f64 mag = std::fabs(static_cast<f64>(ref));
        maxRel = std::max(maxRel, d / std::max(mag, 1e-12));
        if (!(d <= tolRel * mag + tolAbs)) {
            if (bad < 5u) {
                std::fprintf(stderr, "    mismatch gpu %.9g ref %.9g\n", gpu, ref);
            }
            ++bad;
        }
    }
    void add4(const Vec4& gpu, const Vec4& ref, f64 tolRel, f64 tolAbs) {
        add(gpu.x, ref.x, tolRel, tolAbs);
        add(gpu.y, ref.y, tolRel, tolAbs);
        add(gpu.z, ref.z, tolRel, tolAbs);
        add(gpu.w, ref.w, tolRel, tolAbs);
    }
};

f64 maxAbs(const std::vector<Vec4>& v) {
    f64 m = 0.0;
    for (const Vec4& x : v) {
        m = std::max({m, static_cast<f64>(std::fabs(x.x)), static_cast<f64>(std::fabs(x.y)), static_cast<f64>(std::fabs(x.z))});
    }
    return m;
}

constexpr f64 kTolRel = 2e-5;
constexpr f64 kTolAbs = 1e-9;

// --- passes ------------------------------------------------------------------------------------------
int runPasses(Context& ctx) {
    Rig rig;
    if (!initRig(ctx, rig, true, true)) {
        std::printf("SKIP: no froxel fog kernel built\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    std::vector<f32> depth;
    std::vector<Vec4> lit;
    writeImages(ctx, depth, lit);
    rg::Graph graph;
    std::vector<Vec4> lastHistory[kFogs];
    constexpr u32 kFrames = 6;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        beginSceneFrame(ctx, rig);
        // Light motion: frame 2 moves the first point light, frame 3 removes the second spot (a free slot),
        // frame 4 re-adds it (into the freed slot).
        if (frame == 2u) {
            gpu_scene::GpuLight l = rig.scene.light(0);
            l.position[0] += 1.5f;
            l.position[2] -= 1.f;
            rig.scene.setLight(rig.lights[0], l);
        } else if (frame == 3u) {
            rig.scene.removeLight(rig.lights[5]);
        } else if (frame == 4u) {
            rig.lights[5] = rig.scene.addLight(fog_test::lights()[5]);
        }
        for (u32 k = 0; k < kFogs; ++k) {
            rig.settings[k].jitter = frame != 1u; // one unjittered frame
        }
        const ClusterCameraDesc cam = fog_test::camera(frame * 3u, 1, kW, kH);
        Layout layout{};
        if (!runFrame(ctx, rig, graph, cam, layout)) {
            destroyRig(ctx, rig);
            return 1;
        }
        const std::vector<gpu_scene::GpuLight> lights = sceneLights(rig);
        FogLightLists gpuLists;
        readLists(ctx, rig, layout, gpuLists);
        FogLightLists oracle;
        oracleFogLights(fog_test::clusters(), cam, lights.data(), static_cast<u32>(lights.size()), oracle);
        const bool listsEqual =
            gpuLists.grid == oracle.grid && gpuLists.lightList == oracle.lightList && gpuLists.directional == oracle.directional;
        expect(listsEqual, "GPU cluster lists == the oracle (entry for entry)");
        const lighting_gpu::LightingFrameConstants lightingView =
            makeLightingView(fog_test::clusters(), cam, static_cast<u32>(lights.size()));
        const fk::LightView view = makeLightView(&lightingView, lights.data(), gpuLists);
        std::vector<Vec4> integrated[kFogs];
        std::printf("frame %u: %zu list entries (%s oracle), %zu directional\n", frame, gpuLists.lightList.size(),
                    listsEqual ? "==" : "!=", gpuLists.directional.size());
        for (u32 k = 0; k < kFogs; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            FogFrameConstants c = rig.fog[k].constants();
            const u32 n = c.froxelCount;
            const std::vector<Vec4> cur = readTexels(ctx, layout.section[k][0], n);
            const std::vector<Vec4> prev = readTexels(ctx, layout.section[k][1], n);
            const std::vector<Vec4> hist = readTexels(ctx, layout.section[k][2], n);
            integrated[k] = readTexels(ctx, layout.section[k][3], n);
            // inject
            std::vector<Vec4> refCur;
            injectReference(c, view, refCur);
            Compare ci;
            for (u32 i = 0; i < n; ++i) {
                ci.add4(cur[i], refCur[i], kTolRel, kTolAbs);
            }
            // temporal (on the GPU's own inputs); the previous history is last frame's output
            std::vector<Vec4> refHist;
            temporalReference(c, cur, prev, refHist);
            Compare ct;
            const f64 histMax = maxAbs(hist);
            for (u32 i = 0; i < n; ++i) {
                ct.add4(hist[i], refHist[i], kTolRel, kTolAbs + 1e-7 * histMax);
            }
            bool pingPong = true;
            if (frame > 0u) {
                pingPong = lastHistory[k].size() == n && std::memcmp(lastHistory[k].data(), prev.data(), n * 16u) == 0;
            }
            lastHistory[k] = hist;
            // integrate
            std::vector<Vec4> refInt;
            integrateReference(c, hist, refInt);
            Compare cg;
            for (u32 i = 0; i < n; ++i) {
                cg.add4(integrated[k][i], refInt[i], kTolRel, kTolAbs);
            }
            // apply (dump) + the RGBA16F image
            std::vector<Vec4> refApply;
            applyReference(c, integrated[k], depth, lit, refApply);
            const f32* dumpF = static_cast<const f32*>(ctx.dumps[k].mapped);
            const u8* image = rb(ctx, layout.image[k]);
            Compare ca;
            u32 halfBad = 0;
            for (u32 i = 0; i < kW * kH; ++i) {
                const Vec4 d{dumpF[i * 4u], dumpF[i * 4u + 1u], dumpF[i * 4u + 2u], dumpF[i * 4u + 3u]};
                ca.add4(d, refApply[i], kTolRel, kTolAbs);
                u16 h[4];
                std::memcpy(h, image + i * 8u, 8u);
                const f32 dv[4] = {d.x, d.y, d.z, d.w};
                for (u32 ch = 0; ch < 4u; ++ch) {
                    halfBad += withinHalfUlp(halfToFloat(h[ch]), dv[ch]) ? 0u : 1u;
                }
            }
            std::printf("  %-5s history %s | inject exact %llu/%llu max rel %.2e bad %llu | temporal exact %llu/%llu max rel "
                        "%.2e bad %llu | integrate exact %llu/%llu max rel %.2e bad %llu | apply exact %llu/%llu max rel %.2e "
                        "bad %llu | half-ulp bad %u | ping-pong %s\n",
                        rig.label[k], (c.flags & kFogFlagHistory) != 0u ? "on " : "off",
                        static_cast<unsigned long long>(ci.exact), static_cast<unsigned long long>(ci.values), ci.maxRel,
                        static_cast<unsigned long long>(ci.bad), static_cast<unsigned long long>(ct.exact),
                        static_cast<unsigned long long>(ct.values), ct.maxRel, static_cast<unsigned long long>(ct.bad),
                        static_cast<unsigned long long>(cg.exact), static_cast<unsigned long long>(cg.values), cg.maxRel,
                        static_cast<unsigned long long>(cg.bad), static_cast<unsigned long long>(ca.exact),
                        static_cast<unsigned long long>(ca.values), ca.maxRel, static_cast<unsigned long long>(ca.bad), halfBad,
                        pingPong ? "ok" : "BROKEN");
            expect(ci.bad == 0u, "fog.inject == inject_froxel");
            expect(ct.bad == 0u, "fog.temporal == temporal_froxel");
            expect(cg.bad == 0u, "fog.integrate == integrate_column");
            expect(ca.bad == 0u, "fog.apply == apply_pixel");
            expect(halfBad == 0u, "RGBA16F output == dump within one half ulp");
            expect(pingPong, "the previous history == last frame's temporal output (bit for bit)");
            expect(frame == 0u || (c.flags & kFogFlagHistory) != 0u, "history used after the first frame");
        }
        if (rig.built[0] && rig.built[1]) {
            Compare cl;
            for (usize i = 0; i < integrated[0].size(); ++i) {
                cl.add4(integrated[0][i], integrated[1][i], 1e-4, 1e-8);
            }
            std::printf("  slang vs glsl integrated: exact %llu/%llu max rel %.2e bad %llu\n",
                        static_cast<unsigned long long>(cl.exact), static_cast<unsigned long long>(cl.values), cl.maxRel,
                        static_cast<unsigned long long>(cl.bad));
            expect(cl.bad == 0u, "Slang == GLSL (integrated volume within 1e-4 relative)");
        }
    }
    destroyRig(ctx, rig);
    return 0;
}

// --- analytic ----------------------------------------------------------------------------------------
int runAnalytic(Context& ctx) {
    Rig rig;
    if (!initRig(ctx, rig, true, false)) {
        std::printf("SKIP: no froxel fog kernel built\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    // Constant-depth image: every pixel at view depth 13 (the apply check), lit = 0.5.
    std::vector<f32> depth(static_cast<usize>(kW) * kH, fog_test::deviceDepth(13.f));
    std::vector<Vec4> lit(static_cast<usize>(kW) * kH, Vec4{0.5f, 0.5f, 0.5f, 1.f});
    {
        u8* base = static_cast<u8*>(ctx.imgStaging.mapped);
        std::memcpy(base, depth.data(), depth.size() * 4u);
        u8* litBytes = base + align256(depth.size() * 4u);
        const u16 h = GBufferQuantize::floatToHalf(0.5f);
        const u16 one = GBufferQuantize::floatToHalf(1.f);
        for (usize i = 0; i < lit.size(); ++i) {
            const u16 t[4] = {h, h, h, one};
            std::memcpy(litBytes + i * 8u, t, 8u);
        }
    }
    rg::Graph graph;
    const ClusterCameraDesc cam = fog_test::camera(0, 0, kW, kH);
    for (f32 sigma : {0.0005f, 0.02f, 0.3f}) {
        for (u32 k = 0; k < kFogs; ++k) {
            FroxelFogSettings& s = rig.settings[k];
            s = fog_test::settings();
            s.volumeCount = 0;
            s.medium.density = sigma;
            s.medium.height_falloff = 0.f;
            s.temporal = false;
        }
        beginSceneFrame(ctx, rig);
        Layout layout{};
        if (!runFrame(ctx, rig, graph, cam, layout)) {
            destroyRig(ctx, rig);
            return 1;
        }
        for (u32 k = 0; k < kFogs; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            const FogFrameConstants& c = rig.fog[k].constants();
            const std::vector<Vec4> integ = readTexels(ctx, layout.section[k][3], c.froxelCount);
            f64 maxT = 0.0;
            f64 maxL = 0.0;
            for (u32 y = 0; y < c.gridY; ++y) {
                for (u32 x = 0; x < c.gridX; ++x) {
                    const f64 scale = fk::column_ray_scale(c, x, y);
                    for (u32 z = 0; z < c.gridZ; ++z) {
                        const Vec4 v = integ[fk::froxel_index(c, x, y, z)];
                        const f64 T = std::exp(-static_cast<f64>(sigma) * scale * c.sliceDepth[z + 1u]);
                        const f64 L = static_cast<f64>(c.albedo[0]) * c.ambient[0] * (1.0 - T);
                        maxT = std::max(maxT, std::fabs(v.w - T) / T);
                        maxL = std::max(maxL, std::fabs(v.x - L) / std::max(L, 1e-12));
                    }
                }
            }
            // Apply at view depth 13: lit T + L along each pixel's ray (closed form) vs the dump.
            const f32* dumpF = static_cast<const f32*>(ctx.dumps[k].mapped);
            f64 maxApply = 0.0;
            for (u32 py = 0; py < kH; ++py) {
                for (u32 px = 0; px < kW; ++px) {
                    const f64 sx = (px + 0.5) / kW;
                    const f64 sy = (py + 0.5) / kH;
                    const f64 rx = (sx * 2.0 - 1.0) * c.tanX;
                    const f64 ry = (1.0 - sy * 2.0) * c.tanY;
                    const f64 d = static_cast<f64>(fk::view_depth(c, depth[py * kW + px])) * std::sqrt(rx * rx + ry * ry + 1.0);
                    const f64 T = std::exp(-static_cast<f64>(sigma) * d);
                    const f64 ref = 0.5 * T + static_cast<f64>(c.albedo[0]) * c.ambient[0] * (1.0 - T);
                    maxApply = std::max(maxApply, std::fabs(dumpF[(py * kW + px) * 4u] - ref) / ref);
                }
            }
            std::printf("analytic %s sigma=%.4f: transmittance max rel %.2e, in-scattering max rel %.2e, apply at depth 13 "
                        "max rel %.2e\n",
                        rig.label[k], sigma, maxT, maxL, maxApply);
            expect(maxT < 2e-5, "homogeneous transmittance == e^{-sigma d} (2e-5 relative)");
            expect(maxL < 2e-4, "homogeneous in-scattering == albedo ambient (1 - T) (2e-4 relative)");
            // Linear interpolation of e^{-sigma |ray| d} between slice boundaries errs by up to (sigma dz |ray|)^2 / 8 of
            // T (3% of T for sigma = 0.3 in the ~1.7-unit slice at depth 13) plus the bilinear blend of neighbouring
            // columns' ray lengths.
            expect(maxApply < 1e-2, "fog.apply == lit T + L (closed form; 1e-2 relative: linear depth interpolation)");
        }
    }
    destroyRig(ctx, rig);
    return 0;
}

// --- converge ----------------------------------------------------------------------------------------
int runConverge(Context& ctx) {
    Rig rig;
    if (!initRig(ctx, rig, true, true)) {
        std::printf("SKIP: no froxel fog kernel built\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    std::vector<f32> depth;
    std::vector<Vec4> lit;
    writeImages(ctx, depth, lit);
    rg::Graph graph;
    const ClusterCameraDesc cam = fog_test::camera(0, 0, kW, kH);
    constexpr u32 kFrames = 256;
    std::vector<std::vector<Vec4>> ring[kFogs];
    std::vector<f64> phase[kFogs];
    fog_test::VarianceWindow window[kFogs];
    fog_test::VarianceWindow visible[kFogs];
    FogLightLists lists;
    for (u32 k = 0; k < kFogs; ++k) {
        ring[k].resize(kFogJitterPeriod);
    }
    for (u32 frame = 0; frame < kFrames; ++frame) {
        beginSceneFrame(ctx, rig);
        Layout layout{};
        if (!runFrame(ctx, rig, graph, cam, layout)) {
            destroyRig(ctx, rig);
            return 1;
        }
        readLists(ctx, rig, layout, lists);
        for (u32 k = 0; k < kFogs; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            const u32 n = rig.fog[k].constants().froxelCount;
            const std::vector<Vec4> h = readTexels(ctx, layout.section[k][2], n);
            std::vector<Vec4>& slot = ring[k][frame % kFogJitterPeriod];
            if (frame >= kFogJitterPeriod) {
                phase[k].push_back(fog_test::relL1(h, slot));
            }
            slot = h;
            if (frame >= kFrames - kFogJitterPeriod) {
                window[k].add(h);
                visible[k].add(readTexels(ctx, layout.section[k][3], n));
            }
        }
    }
    const std::vector<gpu_scene::GpuLight> lights = sceneLights(rig);
    const lighting_gpu::LightingFrameConstants lightingView =
        makeLightingView(fog_test::clusters(), cam, static_cast<u32>(lights.size()));
    const fk::LightView view = makeLightView(&lightingView, lights.data(), lists);
    for (u32 k = 0; k < kFogs; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        FogFrameConstants c = rig.fog[k].constants();
        c.flags |= kFogFlagLights;
        std::vector<Vec4> periodMean;
        jitterPeriodMeanReference(c, view, periodMean);
        const f64 toPeriod = fog_test::relL1(window[k].mean(), periodMean);
        std::printf("converge %s: distance to the frame one jitter period earlier %.2e (frame 16) -> %.2e (frame 64) -> "
                    "%.2e (frame %u); relative std over the last period: froxels %.2e, integrated in-scattering %.2e; period "
                    "mean vs the CPU jitter-period mean %.2e\n",
                    rig.label[k], phase[k][0], phase[k][48], phase[k].back(), kFrames - 1u, window[k].relativeStd(),
                    visible[k].relativeStd(), toPeriod);
        expect(phase[k].back() < 1e-4 * phase[k][0], "static camera: converges (period-to-period distance falls by 1e4)");
        expect(visible[k].relativeStd() < 1e-2, "static camera: temporal variance of the integrated fog below 1e-2");
        expect(window[k].relativeStd() < 4e-2, "static camera: temporal variance of the froxels below 4e-2");
        expect(toPeriod < 1e-4, "static camera: the history converges to the jitter-period mean (1e-4)");
    }
    destroyRig(ctx, rig);
    return 0;
}

// --- ghosting ----------------------------------------------------------------------------------------
int runGhosting(Context& ctx) {
    Rig rig;
    if (!initRig(ctx, rig, false, true) || !rig.built[0] || !rig.built[1]) {
        std::printf("SKIP: no froxel fog kernel built\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    rig.settings[1].reproject = false; // the control: same-froxel history
    std::vector<f32> depth;
    std::vector<Vec4> lit;
    writeImages(ctx, depth, lit);
    rg::Graph graph;
    constexpr u32 kFrames = 96;
    f64 metric[kFogs] = {0.0, 0.0};
    f64 trail[kFogs] = {0.0, 0.0};
    u32 samples = 0;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        beginSceneFrame(ctx, rig);
        const ClusterCameraDesc cam = fog_test::camera(frame, 1, kW, kH);
        Layout layout{};
        if (!runFrame(ctx, rig, graph, cam, layout)) {
            destroyRig(ctx, rig);
            return 1;
        }
        if (frame < 48u || frame % 8u != 0u) {
            continue;
        }
        FogLightLists lists;
        readLists(ctx, rig, layout, lists);
        const std::vector<gpu_scene::GpuLight> lights = sceneLights(rig);
        const lighting_gpu::LightingFrameConstants lightingView =
            makeLightingView(fog_test::clusters(), cam, static_cast<u32>(lights.size()));
        const fk::LightView view = makeLightView(&lightingView, lights.data(), lists);
        std::vector<Vec4> truth;
        froxelAverageReference(rig.fog[0].constants(), view, 2, truth);
        for (u32 k = 0; k < kFogs; ++k) {
            const std::vector<Vec4> h = readTexels(ctx, layout.section[k][2], rig.fog[k].constants().froxelCount);
            metric[k] += fog_test::relL1(h, truth);
            trail[k] += fog_test::trailEnergy(h, truth);
        }
        ++samples;
    }
    for (u32 k = 0; k < kFogs; ++k) {
        metric[k] /= samples;
        trail[k] /= samples;
    }
    std::printf("ghosting (%s kernels, %u samples): relative L1 to the froxel mean %.4f (reprojected) / %.4f (same-froxel "
                "history); trail energy %.4f / %.4f\n",
                rig.fog[0].kernelLanguage(), samples, metric[0], metric[1], trail[0], trail[1]);
    expect(metric[0] < 0.08, "moving camera: ghosting metric (relative L1 to the froxel mean) below 0.08");
    expect(trail[0] < 0.04, "moving camera: trail energy below 0.04");
    expect(metric[0] < 0.5 * metric[1], "reprojection at least halves the ghosting of a same-froxel history");
    destroyRig(ctx, rig);
    return 0;
}


// --- shadow ------------------------------------------------------------------------------------------
struct ProbeLookup {
    std::unordered_map<u64, f32> visibility; ///< key: position bits hashed (x, y, z)
    u32 slot = vsm::kNoSlot;
    mutable u32 missing = 0;
};

u64 positionKey(const Vec3& p) {
    u32 b[3];
    std::memcpy(&b[0], &p.x, 4u);
    std::memcpy(&b[1], &p.y, 4u);
    std::memcpy(&b[2], &p.z, 4u);
    return (static_cast<u64>(b[0]) * 0x9E3779B97F4A7C15ull) ^ (static_cast<u64>(b[1]) << 21u) ^ (static_cast<u64>(b[2]) << 42u) ^
           b[2];
}

f32 probeShadow(const void* user, u32 slot, const Vec3& p) {
    const ProbeLookup& l = *static_cast<const ProbeLookup*>(user);
    if (slot != l.slot) {
        return 1.f; // not shadowed by the VSM (fuse_vsm_shadow returns 1)
    }
    const auto it = l.visibility.find(positionKey(p));
    if (it == l.visibility.end()) {
        ++l.missing;
        return 1.f;
    }
    return it->second;
}

int runShadow(Context& ctx) {
    if (!vsm::queryVsmCapabilities(ctx.device.get()).vsm) {
        std::printf("SKIP: VSM unsupported\n");
        return kSkip;
    }
    Rig rig;
    if (!initRig(ctx, rig, true, true, true)) {
        std::printf("SKIP: no froxel fog kernel built\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    vsm::VsmShadows shadows;
    vsm::VsmShadowsDesc sd{};
    sd.device = ctx.device.get();
    sd.allocator = ctx.allocator.get();
    sd.bindless = &ctx.bindless;
    const u32 spotSlot = 4u;
    const gpu_scene::GpuLight spot = rig.scene.light(spotSlot);
    if (!shadows.init(sd) || !vsm::makeLocalLight(spotSlot, spot, rig.local)) {
        std::printf("SKIP: VsmShadows unavailable\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    rig.shadows = &shadows;
    for (FroxelFogSettings& fs : rig.settings) { // a finer grid: more samples around the box's shadow
        fs.gridX = 64;
        fs.gridY = 36;
        fs.gridZ = 64;
    }
    const u32 maxProbes = 64u * 36u * 64u;
    const BufferUsage storageBda =
        static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    if (!makeBuffer(ctx, ctx.probeIn, maxProbes * sizeof(vsm::VsmProbeInput), storageBda, MemoryUsage::CpuToGpu, "rp_fog.probe_in") ||
        !makeBuffer(ctx, ctx.probeOut, maxProbes * sizeof(vsm::VsmProbeOutput), storageBda, MemoryUsage::GpuToCpu,
                    "rp_fog.probe_out")) {
        std::fprintf(stderr, "FAIL: probe buffers\n");
        shadows.destroy();
        destroyRig(ctx, rig);
        return 1;
    }
    std::vector<f32> depth;
    std::vector<Vec4> lit;
    writeImages(ctx, depth, lit);
    rg::Graph graph;
    const vsmr_test::D3 lightPos{spot.position[0], spot.position[1], spot.position[2]};
    for (u32 frame = 0; frame < 3u; ++frame) {
        beginSceneFrame(ctx, rig);
        const ClusterCameraDesc cam = fog_test::camera(frame * 4u, 1, kW, kH);
        Layout layout{};
        if (!runFrame(ctx, rig, graph, cam, layout)) {
            shadows.destroy();
            destroyRig(ctx, rig);
            return 1;
        }
        const std::vector<gpu_scene::GpuLight> lights = sceneLights(rig);
        FogLightLists gpuLists;
        readLists(ctx, rig, layout, gpuLists);
        const lighting_gpu::LightingFrameConstants lightingView =
            makeLightingView(fog_test::clusters(), cam, static_cast<u32>(lights.size()));
        const FogFrameConstants& c0 = rig.fog[0].constants();
        // Probe results keyed by the sample point (the CPU recomputes the same points bit for bit).
        ProbeLookup lookup;
        lookup.slot = spotSlot;
        const vsm::VsmProbeInput* in = static_cast<const vsm::VsmProbeInput*>(ctx.probeIn.mapped);
        const vsm::VsmProbeOutput* out = static_cast<const vsm::VsmProbeOutput*>(ctx.probeOut.mapped);
        u32 shadowedSamples = 0;
        u32 boxShadowed = 0;
        u32 inCone = 0;
        u32 wrong = 0;
        u32 classified = 0;
        for (u32 i = 0; i < rig.probeCount; ++i) {
            const Vec3 p{in[i].position[0], in[i].position[1], in[i].position[2]};
            lookup.visibility[positionKey(p)] = out[i].visibility;
            // Analytic occlusion of the segment p -> light by the box, away from the penumbra and the box.
            const vsmr_test::D3 dp{p.x, p.y, p.z};
            const vsmr_test::D3 toLight = lightPos - dp;
            const f64 dist = std::sqrt(vsmr_test::dot(toLight, toLight));
            const Vec3 l{static_cast<f32>(toLight.x / dist), static_cast<f32>(toLight.y / dist), static_cast<f32>(toLight.z / dist)};
            const Vec3 axis = fk::safe_normalize(fk::v3(spot.direction), Vec3{0.f, 0.f, -1.f});
            if (!(dist < spot.range) || fk::spot_cone(-fk::dot3(l, axis), spot.cosInner, spot.cosOuter) <= 0.f) {
                continue;
            }
            ++inCone;
            shadowedSamples += out[i].visibility < 0.5f ? 1u : 0u;
            // Occluders: the box, and the ground plane y = 0 (the light is above it: every sample below is shadowed).
            const f64 m = 0.12;
            auto occludedAt = [&](const vsmr_test::D3& q) {
                const vsmr_test::D3 t = lightPos - q;
                const f64 len = std::sqrt(vsmr_test::dot(t, t));
                return q.y < 0.0 || rig.world.occluded(q, vsmr_test::normalize(t), len);
            };
            const bool occluded = occludedAt(dp);
            bool stable = std::fabs(dp.y) > m; // not at the ground (receiver / bias region)
            const vsmr_test::D3 offsets[6] = {{m, 0, 0}, {-m, 0, 0}, {0, m, 0}, {0, -m, 0}, {0, 0, m}, {0, 0, -m}};
            for (const vsmr_test::D3& o : offsets) {
                stable = stable && occludedAt(dp + o) == occluded;
            }
            for (const vsmr_test::Box& b : rig.world.boxes) {
                stable = stable && !(dp.x > b.lo.x - m && dp.x < b.hi.x + m && dp.y > b.lo.y - m && dp.y < b.hi.y + m &&
                                     dp.z > b.lo.z - m && dp.z < b.hi.z + m);
            }
            if (!stable) {
                continue;
            }
            ++classified;
            boxShadowed += occluded && dp.y > 0.0 ? 1u : 0u;
            const bool bad = occluded ? out[i].visibility > 0.05f : out[i].visibility < 0.95f;
            wrong += bad ? 1u : 0u;
        }
        const fk::LightView plain = makeLightView(&lightingView, lights.data(), gpuLists);
        fk::LightView view = plain;
        view.shadow = &probeShadow;
        view.shadowUser = &lookup;
        FogFrameConstants unshadowed = c0;
        unshadowed.flags &= ~static_cast<u32>(kFogFlagShadows);
        std::vector<Vec4> refNoShadow;
        injectReference(unshadowed, plain, refNoShadow);
        u32 darker = 0;
        for (u32 k = 0; k < kFogs; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            FogFrameConstants c = rig.fog[k].constants();
            expect((c.flags & kFogFlagShadows) != 0u && c.shadows != 0u, "shadows enabled in the fog constants");
            const std::vector<Vec4> cur = readTexels(ctx, layout.section[k][0], c.froxelCount);
            std::vector<Vec4> ref;
            injectReference(c, view, ref);
            Compare ci;
            darker = 0;
            for (u32 i = 0; i < c.froxelCount; ++i) {
                ci.add4(cur[i], ref[i], kTolRel, kTolAbs);
                darker += ref[i].x < refNoShadow[i].x * 0.999f ? 1u : 0u;
            }
            std::printf("  shadow %s frame %u: inject exact %llu/%llu max rel %.2e bad %llu (probe misses %u)\n", rig.label[k], frame,
                        static_cast<unsigned long long>(ci.exact), static_cast<unsigned long long>(ci.values), ci.maxRel,
                        static_cast<unsigned long long>(ci.bad), lookup.missing);
            expect(ci.bad == 0u && lookup.missing == 0u, "fog.inject with VSM shadows == inject_froxel x probe visibility");
        }
        std::printf("shadow frame %u: %u samples in the spot cone, %u shadowed (visibility < 0.5), %u froxels darker than "
                    "unshadowed; analytic ray / box + ground test: %u classified (%u in the box's shadow above the "
                    "ground), %u wrong\n",
                    frame, inCone, shadowedSamples, darker, classified, boxShadowed, wrong);
        expect(wrong == 0u, "VSM visibility at the fog samples == analytic occlusion (away from the penumbra)");
        expect(shadowedSamples > 50u && darker > 50u && boxShadowed > 30u, "the box casts a volumetric shadow (samples and froxels)");
        expect(classified > inCone / 2u, "most cone samples are classified");
    }
    vkDeviceWaitIdle(ctx.vkDevice);
    rig.shadows = nullptr;
    shadows.destroy();
    destroyRig(ctx, rig);
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "fog.", 4) == 0) {
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
    Rig rig;
    if (!initRig(ctx, rig, true, true)) {
        std::printf("SKIP: no froxel fog kernel built\n");
        destroyRig(ctx, rig);
        return kSkip;
    }
    rig.readback = false;
    std::vector<f32> depth;
    std::vector<Vec4> lit;
    writeImages(ctx, depth, lit);
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
        beginSceneFrame(ctx, rig);
        rig.scene.commit();
        ctx.upload.flush();
        const ClusterCameraDesc cam = fog_test::camera(frame, 1, kW, kH);
        LightingFrameDesc lf{};
        lf.camera = cam;
        lf.scene = rig.scene.headerHandle();
        lf.lightCount = rig.scene.lightHighWater();
        bool begun = rig.lighting.beginFrame(ctx.serial, lf);
        t_allocations = 0;
        t_count = measure;
        for (u32 k = 0; k < kFogs; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            FogFrameDesc fd{};
            fd.camera = cam;
            fd.lighting = rig.lighting.frameConstantsAddress();
            fd.depth = &ctx.images[0];
            fd.lit = &ctx.images[1];
            begun = rig.fog[k].beginFrame(ctx.serial, rig.settings[k], fd) && begun;
        }
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, rig, graph, Layout{});
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        for (FroxelFog& f : rig.fog) {
            f.collectRetired(ctx.serial);
        }
        rig.lighting.collectRetired(ctx.serial);
        rig.scene.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(rig.fog[0].stats().passes == 4u || !rig.built[0], "every fog pass runs");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = rig.fog[0].stats().workRebuilds + rig.fog[1].stats().workRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.fog[0].stats().workRebuilds + rig.fog[1].stats().workRebuilds == rebuilds, "no work rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s + %s kernels; inject, temporal (reprojected), integrate, apply; camera "
                    "moving)\n"
                    "  FroxelFog::beginFrame + fog.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene + light assignment + test upload + fog imports and passes): %llu\n",
                    kTotal - kWarmup, rig.built[0] ? rig.fog[0].kernelLanguage() : "-",
                    rig.built[1] ? rig.fog[1].kernelLanguage() : "-", side, callbacks, build);
        expect(side == 0u, "the fog passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the fog passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    destroyRig(ctx, rig);
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
        } else if (mode == "converge") {
            rc = runConverge(ctx);
        } else if (mode == "ghosting") {
            rc = runGhosting(ctx);
        } else if (mode == "shadow") {
            rc = runShadow(ctx);
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
