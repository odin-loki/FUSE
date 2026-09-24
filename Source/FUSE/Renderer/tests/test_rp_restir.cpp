// WP-7.2 ReSTIR DI and GI, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_restir_cpu.cpp.
//
// Scene (test_rp_restir_scene.hpp): ground + 4 boxes as WP-1.1 GPU-scene meshlet meshes (the WP-6.0 BLAS / TLAS),
// lit by a ceiling panel of 10 000 emissive triangles in the WP-7.1 light tree (LightTreeGpu ring slot). Every frame
// is one render graph (WP-0.3): gpu_scene.* -> rt.* -> cull.* / vis.* -> resolve.* (WP-1.5 G-buffer) ->
// light_tree slot -> restir.* (both kernel languages, one RestirGpu each) -> readback. The CPU side traces with the
// WP-6.0 reference (rt::RtReferenceScene, f64 triangles) and runs the single-source kernel (restir_kernel.hpp).
//
//   --mode passes      64 x 48, two sequences (unbiased Talbot MIS, then biased 1 / M) of 3 frames each with a camera
//                      move (WP-4.1-convention UV motion uploaded per frame) and keepIntermediates: EVERY pass is
//                      recomputed on the CPU from the GPU's own read-back inputs of that pass and must match BIT FOR
//                      BIT on every pixel whose rays are robust (rt::RtReferenceScene::traceClassified, 1e-4
//                      barycentric band; the GI hit (instance, primitive) must equal the CPU's, the hit point
//                      within rt::rtTTolerance across the surface (|dt| |cos|), and the CPU continues from the GPU's t): restir.prepare (albedo within
//                      1 ulp: unorm8), restir.di.initial / .temporal / .spatial x2, restir.gi.initial / .temporal /
//                      .spatial x2, restir.shade (signals, depth, histories); Slang == GLSL on every pixel
//   --mode denoise     the DI / GI signals, surface normals and depth feed the WP-6.4 SvgfDenoiser (GI preset) as its
//                      input layout, 16 frames: the chain runs under validation, the denoised output is finite, its
//                      mean within 10 % of the noisy input's and its pixel-to-pixel variation at most half the input's
//   --mode zero_alloc  64 steady-state frames (both chains, unbiased, 2 spatial iterations, camera alternating with
//                      motion): 0 operator-new calls in RestirGpu::beginFrame, the restir.* pass callbacks and the whole
//                      graph build
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, device below T2, no kernel built).
#include "test_rp_restir_scene.hpp"

namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

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

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/restir/restir_gpu.hpp>
#include <fuse/renderer/restir/restir_kernel.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::culling;
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::restir;
using namespace fuse::renderer::rt;
using namespace fuse::renderer::visbuffer;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using vsmr_test::Box;
using vsmr_test::Camera;
using vsmr_test::D3;

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
constexpr u32 kPanelX = 50; // 50 x 100 cells -> 10 000 emissive triangles
constexpr u32 kPanelZ = 100;

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

u32 bitsOf(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, 4u);
    return b;
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
    Buffer motion{};
    Buffer dump[kLanguages]{};
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
            allocator->destroyBuffer(motion);
            for (Buffer& b : dump) {
                allocator->destroyBuffer(b);
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

/// 0 ok, kSkip, or 1.
int setup(Context& ctx, bool validation, bool descriptorBuffer) {
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
    instanceDesc.appName = "fuse_rp_restir";
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
    if (descriptorBuffer && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const RestirCapabilities caps = queryRestirCapabilities(ctx.device.get());
    if (!caps.gpu) {
        std::printf("SKIP: T2 gate: %s\n", caps.reason);
        return kSkip;
    }
    const VisCapabilities visCaps = queryVisCapabilities(ctx.device.get());
    const ResolveCapabilities resolveCaps = queryResolveCapabilities(ctx.device.get());
    if (!visCaps.raster || !resolveCaps.resolve) {
        std::printf("SKIP: capability missing (vis %s, resolve %s)\n", visCaps.rasterReason, resolveCaps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s; %s\n", ctx.device->info().deviceName.c_str(), ctx.device->info().caps.summary().c_str());
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
    stagingDesc.name = "rp_restir.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    SamplerDesc sd{};
    sd.name = "rp_restir.sampler";
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
struct GpuWorld {
    restir_test::Scene cpu; ///< geometry, albedos, light list / table / tree, camera
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<Material::GPUMaterial> materials;
    RtReferenceScene bvh;
};

Material::GPUMaterial material(f32 r, f32 g, f32 b) {
    Material::GPUMaterial m{};
    m.baseColor = {r, g, b, 0.f};
    m.roughnessEmissive = {0.8f, 0.f, 0.f, 0.f};
    return m;
}

bool buildWorld(Context& ctx, GpuWorld& w, u32 width, u32 height) {
    if (!restir_test::makeScene(w.cpu, width, height, kPanelX, kPanelZ)) {
        return false;
    }
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 16;
    if (!w.gpu.init(d) || !w.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    w.gpu.beginFrame(ctx.serial);
    if (!vsmr_test::buildMeshes(w.meshes, restir_test::kGroundSize)) {
        return false;
    }
    for (u32 i = 0; i < w.meshes.size(); ++i) {
        if (w.gpu.addMeshletMesh(w.meshes[i]) != i || !w.bvh.setMeshFromScene(w.gpu, i, w.meshes[i].positions.data())) {
            return false;
        }
    }
    // Rows in pairs: a box's second submesh reads row + 1 in the G-buffer (WP-1.5); GI hits read the base row.
    const u32 rows = 2u * (1u + static_cast<u32>(w.cpu.world.boxes.size()));
    w.materials.resize(rows);
    w.materials[0] = material(restir_test::kGroundAlbedo[0], restir_test::kGroundAlbedo[1], restir_test::kGroundAlbedo[2]);
    for (u32 b = 0; b < w.cpu.world.boxes.size(); ++b) {
        w.materials[2u + 2u * b] = material(w.cpu.boxAlbedo[b][0], w.cpu.boxAlbedo[b][1], w.cpu.boxAlbedo[b][2]);
    }
    for (u32 i = 0; i < rows; i += 2u) {
        w.materials[i + 1u] = w.materials[i];
    }
    for (u32 i = 0; i < rows; ++i) {
        w.gpu.setMaterial(i, w.materials[i]);
    }
    InstanceDesc ground{};
    ground.mesh = 0;
    ground.material = 0;
    if (!w.gpu.addInstance(ground).valid()) {
        return false;
    }
    for (u32 b = 0; b < w.cpu.world.boxes.size(); ++b) {
        InstanceDesc id{};
        id.mesh = 1;
        id.material = 2u + 2u * b;
        id.transform = vsmr_test::World::boxTransform(w.cpu.world.boxes[b]);
        if (!w.gpu.addInstance(id).valid()) {
            return false;
        }
    }
    const GpuSceneCommitStats stats = w.gpu.commit();
    ctx.upload.flush();
    w.bvh.setInstances(w.gpu);
    return stats.ok && ctx.upload.waitAll();
}

Camera frameCamera(const GpuWorld& w, u32 step) {
    Camera c = w.cpu.camera;
    c.eye.x += 0.12 * step;
    c.eye.z -= 0.05 * step;
    c.at.x += 0.04 * step;
    c.build();
    return c;
}

RestirCamera toRestirCamera(const Camera& c) { return restir_test::restirCamera(c); }

/// UV motion (current - previous) of every pixel of `cur` against `prev` from the analytic world (WP-4.1 convention);
/// sky pixels get the camera rotation only (the direction at infinity).
void computeMotion(const GpuWorld& w, const Camera& cur, const Camera& prev, f32* out) {
    for (u32 y = 0; y < cur.height; ++y) {
        for (u32 x = 0; x < cur.width; ++x) {
            D3 o{};
            D3 d{};
            cur.ray(x, y, o, d);
            f64 t = 0.0;
            D3 p{};
            if (w.cpu.world.cast(o, d, 0.0, 1e3, t) != vsmr_test::World::kMiss) {
                p = o + d * t;
            } else {
                p = o + d * 1e4;
            }
            const f32* m = prev.viewProj.m;
            const f64 cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
            const f64 cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            const f64 cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            const f64 pu = (cx / cw + 1.0) * 0.5;
            const f64 pv = (cy / cw + 1.0) * 0.5;
            const usize i = static_cast<usize>(y) * cur.width + x;
            out[i * 2u] = static_cast<f32>((x + 0.5) / cur.width - pu);
            out[i * 2u + 1u] = static_cast<f32>((y + 0.5) / cur.height - pv);
        }
    }
}

// --- rig ------------------------------------------------------------------------------------------
struct Rig {
    u32 width = 0;
    u32 height = 0;
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    AccelerationStructures as;
    light_tree::LightTreeGpu tree;
    RestirGpu restir[kLanguages];
    bool built[kLanguages] = {};
    const char* language[kLanguages] = {"slang", "glsl"};
};

/// 1 ok, 0 no kernel, -1 failure.
int initRig(Context& ctx, GpuWorld& w, Rig& rig, u32 width, u32 height, bool keep, u32 languages = kLanguages) {
    rig.width = width;
    rig.height = height;
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 16;
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
    ad.scene = &w.gpu;
    ad.instanceCapacity = 16;
    ad.meshCapacity = 4;
    if (!rig.as.init(ad)) {
        std::printf("  acceleration structures: %s\n", rig.as.reason());
        return 0;
    }
    light_tree::LightTreeGpuDesc td{};
    td.device = ctx.device.get();
    td.allocator = ctx.allocator.get();
    td.bindless = &ctx.bindless;
    td.initialLights = static_cast<u32>(w.cpu.lights.size());
    if (!rig.tree.init(td)) {
        std::printf("  light tree GPU unavailable\n");
        return 0;
    }
    u32 built = 0;
    for (u32 k = 0; k < languages; ++k) {
        RestirGpuDesc rd{};
        rd.device = ctx.device.get();
        rd.allocator = ctx.allocator.get();
        rd.bindless = &ctx.bindless;
        rd.width = width;
        rd.height = height;
        rd.keepIntermediates = keep;
        rd.initialLights = static_cast<u32>(w.cpu.table.size());
        rd.language = k == 0u ? RestirKernelLanguage::Slang : RestirKernelLanguage::Glsl;
        rig.built[k] = rig.restir[k].init(rd) && std::strcmp(rig.restir[k].kernelLanguage(), rig.language[k]) == 0;
        built += rig.built[k] ? 1u : 0u;
        std::printf("  %s kernels: %s (%s)\n", rig.language[k], rig.built[k] ? "built" : "not built, skipped", rig.restir[k].reason());
    }
    return built > 0u ? 1 : 0;
}

void destroyRig(Rig& rig) {
    for (RestirGpu& r : rig.restir) {
        r.destroy();
    }
    rig.tree.destroy();
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
    u64 srcOffset = 0;
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

/// G-buffer attachments read back: RT0 (RGBA16F), RT1 (RGBA8), RT4 (R32F).
constexpr u32 kGBufferRead[3] = {0u, 1u, 4u};
constexpr u32 kGBufferBytes[3] = {8u, 4u, 4u};

struct ReadbackLayout {
    u64 gbuffer[3] = {};
    u64 state[kLanguages] = {};
    u64 work[kLanguages] = {};
    u64 output[kLanguages] = {};
    u64 end = 0;
};

struct FrameState {
    CopyRecord copies[16];
    u32 copyCount = 0;
    ReadbackLayout layout{};
    bool readback = true;
    bool dumps = true;
    bool motion = true;
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
    for (u32 i = 0; i < 3u; ++i) {
        l.gbuffer[i] = take(pixels * kGBufferBytes[i]);
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const RestirBufferLayout& b = rig.restir[k].layout();
        l.state[k] = take(b.stateBytes);
        l.work[k] = take(b.workBytes);
        l.output[k] = take(b.outputBytes);
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
    d.name = "rp_restir.readback";
    return ctx.allocator->createBuffer(d, ctx.readback) && ctx.readback.mapped != nullptr;
}

bool beginFrame(Context& ctx, GpuWorld& w, Rig& rig, const Camera& cam, u32 frameIndex, bool reset, const FrameState& fs,
                bool countRestir = false) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, cam.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = w.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, w.gpu.headerHandle(), w.gpu.instanceHighWater()) && ok;
    ResolveFrameDesc rf{};
    std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
    std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
    rf.scene = w.gpu.headerHandle();
    rf.vis = rig.vb.visStorageHandle();
    rf.sampler = ctx.samplerHandle;
    ok = rig.resolve.beginFrame(ctx.serial, rf) && ok;
    ok = rig.tree.beginFrame(ctx.serial, w.cpu.tree) && ok;
    RestirFrameDesc desc{};
    desc.camera = toRestirCamera(cam);
    desc.gbuffer = &rig.resolve;
    desc.tlasAddress = rig.as.tlasAddress();
    desc.lightTreeHeader = rig.tree.headerAddress();
    desc.sceneHeader = w.gpu.headerAddress();
    desc.lights = w.cpu.table.data();
    desc.lightCount = static_cast<u32>(w.cpu.table.size());
    desc.lightsVersion = w.cpu.tree.version();
    desc.motion = fs.motion ? ctx.motion.deviceAddress : 0u;
    desc.frameIndex = frameIndex;
    desc.seed = 0xBEEFu;
    desc.reset = reset;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        t_count = countRestir;
        ok = rig.restir[k].beginFrame(ctx.serial, desc) && ok;
        t_count = false;
    }
    return ok;
}

void buildGraph(Context& ctx, GpuWorld& w, Rig& rig, rg::Graph& graph, FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = w.gpu.importInto(graph);
    const RtGraphRefs rtRefs = rig.as.importInto(graph, sceneRefs);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, w.gpu.headerHandle(), rig.culler, cull);
    const ResolveGraphRefs gbuffer = rig.resolve.importInto(graph);
    rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
    const light_tree::LightTreeGraphRefs treeRefs = rig.tree.importInto(graph);
    const rg::BufferRef motion =
        fs.motion ? graph.importBuffer(rg::ImportedBuffer{ctx.motion.handle, ctx.motion.desc.size, rg::kNoQueue, nullptr, "rp_restir.motion"})
                  : rg::BufferRef{};
    RestirGraphRefs refs[kLanguages]{};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        refs[k] = rig.restir[k].importInto(graph);
        RestirGraphInputs in{};
        in.tlas = rtRefs.tlas;
        in.lightTree = treeRefs;
        in.gbuffer = &gbuffer;
        in.scene = sceneRefs;
        in.motion = motion;
        if (fs.dumps) {
            in.dump = graph.importBuffer(rg::ImportedBuffer{ctx.dump[k].handle, ctx.dump[k].desc.size, rg::kNoQueue, nullptr, "rp_restir.dump"});
            in.dumpAddress = ctx.dump[k].deviceAddress;
        }
        expect(rig.restir[k].addPasses(graph, refs[k], in), "RestirGpu::addPasses");
        if (fs.dumps && fs.readback) {
            graph.addPass("readback.dump", nullptr, nullptr).use(in.dump, rg::Access::HostRead);
        }
    }
    if (!fs.readback) {
        // Where a consumer (lighting composite / WP-6.4) would read the signals.
        for (u32 k = 0; k < kLanguages; ++k) {
            if (rig.built[k]) {
                rg::PassBuilder pass = graph.addPass("restir.use", nullptr, nullptr);
                pass.neverCull();
                pass.use(refs[k].output, rg::Access::StorageRead, {}, rg::kStageCompute);
            }
        }
        return;
    }
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_restir.readback"});
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
    for (u32 i = 0; i < 3u; ++i) {
        addCopy(true, gbuffer.gbuffer[kGBufferRead[i]], {}, fs.layout.gbuffer[i], pixels * kGBufferBytes[i]);
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const RestirBufferLayout& b = rig.restir[k].layout();
        addCopy(false, {}, refs[k].state, fs.layout.state[k], b.stateBytes);
        addCopy(false, {}, refs[k].work, fs.layout.work[k], b.workBytes);
        addCopy(false, {}, refs[k].output, fs.layout.output[k], b.outputBytes);
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void beginSceneFrame(Context& ctx, GpuWorld& w, Rig& rig) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    w.gpu.beginFrame(ctx.serial);
    rig.as.beginFrame(ctx.serial);
}

void collect(Context& ctx, GpuWorld& w, Rig& rig) {
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.resolve.collectRetired(ctx.serial);
    rig.as.collectRetired(ctx.serial);
    rig.tree.collectRetired(ctx.serial);
    for (RestirGpu& r : rig.restir) {
        r.collectRetired(ctx.serial);
    }
    w.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
}

bool runFrame(Context& ctx, GpuWorld& w, Rig& rig, rg::Graph& graph, const Camera& cam, u32 frameIndex, bool reset, FrameState& fs) {
    beginSceneFrame(ctx, w, rig);
    const GpuSceneCommitStats stats = w.gpu.commit();
    const RtCommitStats rtStats = rig.as.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, w, rig, cam, frameIndex, reset, fs)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    if (fs.readback) {
        fs.layout = makeLayout(rig);
        if (!ensureReadback(ctx, fs.layout)) {
            return false;
        }
    }
    buildGraph(ctx, w, rig, graph, fs);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    collect(ctx, w, rig);
    return stats.ok && rtStats.ok && result.ok && waited;
}

const u8* rbAt(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

// --- CPU mirror ------------------------------------------------------------------------------------
/// One language's read-back buffers of a frame.
struct Readback {
    std::vector<RestirSurfaceF> surf[2]; ///< [0] this frame, [1] previous frame
    std::vector<f32> albedo;             ///< this frame, 4 per pixel
    std::vector<RestirDiReservoir> diStage[kRestirStages];
    std::vector<RestirGiReservoir> giStage[kRestirStages];
    std::vector<RestirDiReservoir> diHistory[2]; ///< [0] written this frame, [1] previous
    std::vector<RestirGiReservoir> giHistory[2];
    std::vector<f32> diSignal;
    std::vector<f32> giSignal;
    std::vector<f32> depth;
    std::vector<RestirGiHitRecord> hits;
};

template <typename T>
void copyOut(const u8* base, u64 offset, usize count, std::vector<T>& out) {
    out.resize(count);
    std::memcpy(out.data(), base + offset, count * sizeof(T));
}

void readLanguage(Context& ctx, const FrameState& fs, const Rig& rig, u32 k, Readback& r) {
    const RestirGpu& g = rig.restir[k];
    const RestirBufferLayout& l = g.layout();
    const usize pixels = static_cast<usize>(rig.width) * rig.height;
    const u8* state = rbAt(ctx, fs.layout.state[k]);
    const u8* work = rbAt(ctx, fs.layout.work[k]);
    const u8* output = rbAt(ctx, fs.layout.output[k]);
    for (u32 i = 0; i < 2u; ++i) {
        const u32 slot = i == 0u ? g.slot() : g.slot() ^ 1u;
        std::vector<f32> pos;
        std::vector<f32> nrm;
        copyOut(state, l.surfPos[slot], pixels * 4u, pos);
        copyOut(state, l.surfNormal[slot], pixels * 4u, nrm);
        r.surf[i].resize(pixels);
        for (usize p = 0; p < pixels; ++p) {
            r.surf[i][p].p = RV3{pos[p * 4u], pos[p * 4u + 1u], pos[p * 4u + 2u]};
            r.surf[i][p].depth = pos[p * 4u + 3u];
            r.surf[i][p].n = RV3{nrm[p * 4u], nrm[p * 4u + 1u], nrm[p * 4u + 2u]};
        }
        copyOut(state, l.diHistory[slot], pixels, r.diHistory[i]);
        copyOut(state, l.giHistory[slot], pixels, r.giHistory[i]);
    }
    copyOut(state, l.surfAlbedo[g.slot()], pixels * 4u, r.albedo);
    for (u32 s = 0; s < kRestirStages; ++s) {
        copyOut(work, l.diStage[s], pixels, r.diStage[s]);
        copyOut(work, l.giStage[s], pixels, r.giStage[s]);
    }
    copyOut(output, l.diSignal, pixels * 4u, r.diSignal);
    copyOut(output, l.giSignal, pixels * 4u, r.giSignal);
    copyOut(output, l.depth, pixels, r.depth);
    r.hits.resize(pixels);
    std::memcpy(r.hits.data(), ctx.dump[k].mapped, pixels * sizeof(RestirGiHitRecord));
}

/// The kernel environment over one language's read-back buffers and the WP-6.0 CPU BVH (robust classification).
struct MirrorEnv {
    const GpuWorld* w = nullptr;
    const Readback* rb = nullptr;
    const f32* motionData = nullptr;
    light_tree::LightTreeView treeView{};
    u32 cullMask = 0;
    const RestirDiReservoir* diSrc = nullptr;
    const RestirGiReservoir* giSrc = nullptr;
    u32 pixel = 0;              ///< gi.initial: the pixel whose GPU hit record answers traceHit
    mutable bool robust = true; ///< every ray of the current pixel classified robust
    mutable bool hitMismatch = false;
    mutable f64 worstT = 0.0;   ///< |t_gpu - t_cpu| / rtTTolerance(t)
    mutable u64 rays = 0;

    RestirSurfaceF surface(u32 slot, u32 p) const { return rb->surf[slot][p]; }
    RestirDiReservoir diSource(u32 p) const { return diSrc[p]; }
    RestirDiReservoir diHistory(u32 p) const { return rb->diHistory[1][p]; }
    RestirGiReservoir giSource(u32 p) const { return giSrc[p]; }
    RestirGiReservoir giHistory(u32 p) const { return rb->giHistory[1][p]; }
    void motion(u32 p, f32& mx, f32& my) const {
        mx = motionData[p * 2u];
        my = motionData[p * 2u + 1u];
    }
    const light_tree::LightTreeView& tree() const { return treeView; }
    const RestirLight& light(u32 i) const { return w->cpu.table[i]; }

    RtRefClassified classify(const RV3& o, const RV3& d, f32 tMin, f32 tMax) const {
        RtProbeRay ray{};
        ray.origin[0] = o.x;
        ray.origin[1] = o.y;
        ray.origin[2] = o.z;
        ray.direction[0] = d.x;
        ray.direction[1] = d.y;
        ray.direction[2] = d.z;
        ray.tMin = tMin;
        ray.tMax = tMax;
        ++rays;
        const RtRefClassified c = w->bvh.traceClassified(ray, cullMask, kEdgeEpsilon);
        robust = robust && c.robust;
        return c;
    }
    bool occluded(const RV3& o, const RV3& d, f32 tMax) const { return classify(o, d, 0.f, tMax).hit.hit; }

    /// The GPU's committed hit (checked against the CPU BVH), continued with the mirror of rs_trace_hit's attributes.
    bool traceHit(const RV3& o, const RV3& d, f32 tMin, f32 tMax, RestirHitF& hit) const {
        const RtRefClassified c = classify(o, d, tMin, tMax);
        const RestirGiHitRecord& rec = rb->hits[pixel];
        const bool gpuHit = (rec.flags & kRestirHitHit) != 0u;
        if (c.robust && (gpuHit != c.hit.hit || (gpuHit && (rec.instance != c.hit.instance || rec.primitive != c.hit.primitive)))) {
            hitMismatch = true;
        }
        if (!gpuHit) {
            return false;
        }
        hit.t = rec.t;
        hit.instance = rec.instance;
        hit.primitive = rec.primitive;
        const GpuInstance& inst = w->gpu.instance(rec.instance);
        const GpuTransform& xf = w->gpu.transform(rec.instance);
        const GpuMesh& mesh = w->gpu.mesh(inst.mesh);
        const u16* vpos = w->meshes[inst.mesh].positions.data();
        RV3 v[3];
        for (u32 k = 0; k < 3u; ++k) {
            const u32 vi = w->gpu.indexData()[mesh.firstIndex + 3u * rec.primitive + k];
            f32 local[3];
            rtDecodePosition(mesh, vpos, vi, local);
            f32 world[3];
            for (u32 r = 0; r < 3u; ++r) {
                const f32 a = xf.rows[r][0] * local[0];
                const f32 b = xf.rows[r][1] * local[1];
                const f32 ab = a + b;
                const f32 cz = xf.rows[r][2] * local[2];
                const f32 abc = ab + cz;
                world[r] = abc + xf.rows[r][3];
            }
            v[k] = RV3{world[0], world[1], world[2]};
        }
        hit.normal = rsCross(rsSub(v[1], v[0]), rsSub(v[2], v[0]));
        if (c.hit.hit && rec.instance == c.hit.instance && rec.primitive == c.hit.primitive) {
            // Distance of the two hit points from each other ACROSS the surface: |dt| |cos| (a grazing ray's t
            // is ill-conditioned along the surface; the CPU continues from the GPU's t anyway).
            const RV3 n = rsNormalize(hit.normal, RV3{0.f, 1.f, 0.f});
            const f64 cosine = std::fabs(static_cast<f64>(rsDot(n, d)));
            worstT = std::max(worstT, std::fabs(static_cast<f64>(rec.t) - c.hit.t) * cosine / rtTTolerance(c.hit.t));
        }
        hit.albedo = RV3{0.8f, 0.8f, 0.8f};
        if (inst.material < w->materials.size()) {
            const Material::GPUMaterial& m = w->materials[inst.material];
            hit.albedo = RV3{m.baseColor.x, m.baseColor.y, m.baseColor.z};
        }
        return true;
    }
};

bool sameDi(const RestirDiReservoir& a, const RestirDiReservoir& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
bool sameGi(const RestirGiReservoir& a, const RestirGiReservoir& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

struct PassStats {
    const char* name = "";
    u32 pixels = 0;   ///< compared
    u32 exact = 0;    ///< bit-identical
    u32 nonRobust = 0;///< a ray of the pixel within the edge band (excluded from the exact gate)
    u32 mismatch = 0; ///< robust but different: a failure
    u32 hitMismatch = 0;
    f64 worstT = 0.0;
};

void report(const PassStats& s) {
    std::printf("    %-20s %5u px: %5u bit-identical, %3u non-robust, %u robust mismatches%s\n", s.name, s.pixels, s.exact, s.nonRobust,
                s.mismatch, s.hitMismatch > 0u ? " (GI hit id mismatch!)" : "");
}

void check(const PassStats& s) {
    expect(s.mismatch == 0u && s.hitMismatch == 0u, "pass == CPU kernel bit for bit on every robust pixel");
    expect(s.pixels > 0u && s.nonRobust * 50u <= s.pixels, "non-robust pixels <= 2 %");
}

/// Recomputes every pass of language k's frame on the CPU from the GPU's own inputs.
void checkPasses(Context& ctx, const GpuWorld& w, const Rig& rig, u32 k, const FrameState& fs, const Readback& r, const f32* motion,
                 u32 frame, u32 totals[3]) {
    const RestirGpu& g = rig.restir[k];
    const RestirFrameConstants& c = g.frameConstants();
    const RestirSettings& s = g.settings();
    const u32 W = rig.width;
    const u32 H = rig.height;
    const usize pixels = static_cast<usize>(W) * H;
    MirrorEnv env{};
    env.w = &w;
    env.rb = &r;
    env.motionData = motion;
    env.treeView = w.cpu.tree.view();
    env.cullMask = c.cullMask;
    std::printf("  %s frame %u (%s, history %s):\n", rig.language[k], frame, s.unbiased ? "unbiased" : "biased",
                (c.flags & kRestirFlagHistory) != 0u ? "yes" : "no");

    // restir.prepare
    {
        PassStats st{};
        st.name = "restir.prepare";
        const u8* rt0 = rbAt(ctx, fs.layout.gbuffer[0]);
        const u8* rt1 = rbAt(ctx, fs.layout.gbuffer[1]);
        const u8* rt4 = rbAt(ctx, fs.layout.gbuffer[2]);
        f64 worstAlbedo = 0.0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const usize p = static_cast<usize>(y) * W + x;
                f32 depth = 0.f;
                std::memcpy(&depth, rt4 + p * 4u, 4u);
                RestirSurfaceF out{};
                const bool ok = rsPrepare(c, x, y, depth, halfAt(rt0, p * 4u), halfAt(rt0, p * 4u + 1u), out);
                const RestirSurfaceF& gpu = r.surf[0][p];
                const bool same = bitsOf(out.p.x) == bitsOf(gpu.p.x) && bitsOf(out.p.y) == bitsOf(gpu.p.y) &&
                                  bitsOf(out.p.z) == bitsOf(gpu.p.z) && bitsOf(out.depth) == bitsOf(gpu.depth) &&
                                  bitsOf(out.n.x) == bitsOf(gpu.n.x) && bitsOf(out.n.y) == bitsOf(gpu.n.y) && bitsOf(out.n.z) == bitsOf(gpu.n.z);
                for (u32 ch = 0; ch < 3u && ok; ++ch) {
                    const f32 ref = static_cast<f32>(rt1[p * 4u + ch]) / 255.f;
                    worstAlbedo = std::max(worstAlbedo, std::fabs(static_cast<f64>(r.albedo[p * 4u + ch]) - ref));
                }
                ++st.pixels;
                st.exact += same ? 1u : 0u;
                st.mismatch += same ? 0u : 1u;
            }
        }
        report(st);
        check(st);
        expect(worstAlbedo <= 1e-7, "prepare: albedo == RT1 / 255 within 1e-7");
    }
    auto diPass = [&](const char* name, u32 stage, u32 mode, u32 iteration, u32 srcStage) {
        PassStats st{};
        st.name = name;
        env.diSrc = r.diStage[srcStage].data();
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const usize p = static_cast<usize>(y) * W + x;
                env.robust = true;
                const RestirDiReservoir cpu = stage == 0u ? rsDiInitial(env, c, x, y) : rsDiReuse(env, c, x, y, mode, iteration);
                ++st.pixels;
                if (sameDi(cpu, r.diStage[stage][p])) {
                    ++st.exact;
                } else if (!env.robust) {
                    ++st.nonRobust;
                } else {
                    ++st.mismatch;
                    if (st.mismatch <= 3u) {
                        const RestirDiReservoir& gp = r.diStage[stage][p];
                        std::fprintf(stderr, "      (%u,%u) cpu light %u W %.9g M %g | gpu light %u W %.9g M %g\n", x, y, cpu.light, cpu.W,
                                     cpu.M, gp.light, gp.W, gp.M);
                    }
                }
            }
        }
        report(st);
        check(st);
        totals[0] += st.exact;
        totals[1] += st.nonRobust;
    };
    auto giPass = [&](const char* name, u32 stage, u32 mode, u32 iteration, u32 srcStage) {
        PassStats st{};
        st.name = name;
        env.giSrc = r.giStage[srcStage].data();
        env.worstT = 0.0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const usize p = static_cast<usize>(y) * W + x;
                env.robust = true;
                env.hitMismatch = false;
                env.pixel = static_cast<u32>(p);
                RestirGiHitRecord rec{};
                const RestirGiReservoir cpu =
                    stage == 0u ? rsGiInitial(env, c, x, y, &rec) : rsGiReuse(env, c, x, y, mode, iteration);
                ++st.pixels;
                bool same = sameGi(cpu, r.giStage[stage][p]);
                if (stage == 0u) {
                    const RestirGiHitRecord& g0 = r.hits[p];
                    same = same && bitsOf(rec.direction[0]) == bitsOf(g0.direction[0]) &&
                           bitsOf(rec.direction[1]) == bitsOf(g0.direction[1]) && bitsOf(rec.direction[2]) == bitsOf(g0.direction[2]) &&
                           rec.flags == g0.flags;
                    st.hitMismatch += env.hitMismatch ? 1u : 0u;
                }
                if (same) {
                    ++st.exact;
                } else if (!env.robust) {
                    ++st.nonRobust;
                } else {
                    ++st.mismatch;
                    if (st.mismatch <= 3u) {
                        const RestirGiReservoir& gp = r.giStage[stage][p];
                        std::fprintf(stderr, "      (%u,%u) cpu W %.9g M %g L %.9g | gpu W %.9g M %g L %.9g\n", x, y, cpu.W, cpu.M,
                                     cpu.radiance[0], gp.W, gp.M, gp.radiance[0]);
                    }
                }
            }
        }
        st.worstT = env.worstT;
        report(st);
        check(st);
        if (stage == 0u) {
            std::printf("      GI hit: worst |t_gpu - t_cpu| |cos| = %.3f x rtTTolerance\n", st.worstT);
            expect(st.worstT <= 1.0, "GI hit point within rt::rtTTolerance across the surface");
        }
        totals[0] += st.exact;
        totals[1] += st.nonRobust;
    };
    u32 lastDi = 0u;
    if (s.di) {
        diPass("restir.di.initial", 0u, 0u, 0u, 0u);
        if (s.diTemporal) {
            diPass("restir.di.temporal", 1u, kRestirModeTemporal, 0u, 0u);
            lastDi = 1u;
        }
        for (u32 i = 0; i < s.diSpatialIterations; ++i) {
            diPass(i == 0u ? "restir.di.spatial 0" : "restir.di.spatial 1+", 2u + i, kRestirModeSpatial, i, lastDi);
            lastDi = 2u + i;
        }
        expect(g.finalDiStage() == lastDi, "final DI stage");
    }
    u32 lastGi = 0u;
    if (s.gi) {
        giPass("restir.gi.initial", 0u, 0u, 0u, 0u);
        if (s.giTemporal) {
            giPass("restir.gi.temporal", 1u, kRestirModeTemporal, 0u, 0u);
            lastGi = 1u;
        }
        for (u32 i = 0; i < s.giSpatialIterations; ++i) {
            giPass(i == 0u ? "restir.gi.spatial 0" : "restir.gi.spatial 1+", 2u + i, kRestirModeSpatial, i, lastGi);
            lastGi = 2u + i;
        }
    }
    // restir.shade
    {
        PassStats st{};
        st.name = "restir.shade";
        f64 sumDi = 0.0;
        f64 sumGi = 0.0;
        u32 lit = 0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const usize p = static_cast<usize>(y) * W + x;
                env.robust = true;
                f32 di[4];
                f32 gi[4];
                f32 depth = 0.f;
                rsShade(env, c, x, y, s.di ? &r.diStage[lastDi][p] : nullptr, s.gi ? &r.giStage[lastGi][p] : nullptr, di, gi, depth);
                bool same = std::memcmp(di, &r.diSignal[p * 4u], 16u) == 0 && std::memcmp(gi, &r.giSignal[p * 4u], 16u) == 0 &&
                            bitsOf(depth) == bitsOf(r.depth[p]);
                same = same && sameDi(r.diHistory[0][p], s.di ? r.diStage[lastDi][p] : RestirDiReservoir{}) &&
                       sameGi(r.giHistory[0][p], s.gi ? r.giStage[lastGi][p] : RestirGiReservoir{});
                ++st.pixels;
                if (same) {
                    ++st.exact;
                } else if (!env.robust) {
                    ++st.nonRobust;
                } else {
                    ++st.mismatch;
                }
                sumDi += r.diSignal[p * 4u] + r.diSignal[p * 4u + 1u] + r.diSignal[p * 4u + 2u];
                sumGi += r.giSignal[p * 4u] + r.giSignal[p * 4u + 1u] + r.giSignal[p * 4u + 2u];
                lit += r.diSignal[p * 4u] > 0.f ? 1u : 0u;
            }
        }
        report(st);
        check(st);
        std::printf("      signals: mean DI %.4f, mean GI %.4f (rgb sum per pixel), %u / %zu pixels with direct light\n", sumDi / pixels,
                    sumGi / pixels, lit, pixels);
        expect(sumDi > 0.0 && sumGi > 0.0 && lit > pixels / 4u, "non-trivial DI / GI signals");
        totals[0] += st.exact;
        totals[1] += st.nonRobust;
    }
    totals[2] += static_cast<u32>(env.rays);
}

int runPasses(Context& ctx) {
    constexpr u32 kW = 64;
    constexpr u32 kH = 48;
    GpuWorld w;
    if (!buildWorld(ctx, w, kW, kH)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    std::printf("scene: %zu emissive triangles (light tree %u nodes, depth %u), %zu boxes\n", w.cpu.lights.size(), w.cpu.tree.stats().nodes,
                w.cpu.tree.stats().maxDepth, w.cpu.world.boxes.size());
    Rig rig;
    const int rc = initRig(ctx, w, rig, kW, kH, true);
    if (rc <= 0) {
        destroyRig(rig);
        w.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: ReSTIR unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    const usize pixels = static_cast<usize>(kW) * kH;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!hostBuffer(ctx, ctx.dump[k], pixels * sizeof(RestirGiHitRecord), MemoryUsage::GpuToCpu, "rp_restir.dump")) {
            return 1;
        }
    }
    if (!hostBuffer(ctx, ctx.motion, pixels * 8u, MemoryUsage::CpuToGpu, "rp_restir.motion")) {
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    u32 totals[3] = {0u, 0u, 0u};
    u32 frameIndex = 0;
    for (const bool unbiased : {true, false}) {
        RestirSettings settings{};
        settings.unbiased = unbiased;
        settings.diCandidates = 4;
        settings.diSpatialIterations = 2;
        settings.diNeighbors = 4;
        settings.diRadius = 4.f;
        settings.giSpatialIterations = 2;
        settings.giNeighbors = 3;
        settings.giRadius = 4.f;
        for (RestirGpu& r : rig.restir) {
            r.setSettings(settings);
        }
        Camera prev = frameCamera(w, 0);
        for (u32 f = 0; f < 3u; ++f) {
            const Camera cam = frameCamera(w, f);
            std::vector<f32> motion(pixels * 2u, 0.f);
            computeMotion(w, cam, prev, motion.data());
            std::memcpy(ctx.motion.mapped, motion.data(), motion.size() * 4u);
            prev = cam;
            fs.readback = true;
            fs.dumps = true;
            fs.motion = true;
            if (!runFrame(ctx, w, rig, graph, cam, frameIndex, f == 0u, fs)) {
                std::fprintf(stderr, "FAIL: frame\n");
                return 1;
            }
            Readback rb[kLanguages];
            for (u32 k = 0; k < kLanguages; ++k) {
                if (!rig.built[k]) {
                    continue;
                }
                readLanguage(ctx, fs, rig, k, rb[k]);
                checkPasses(ctx, w, rig, k, fs, rb[k], motion.data(), frameIndex, totals);
            }
            if (rig.built[0] && rig.built[1]) {
                bool same = rb[0].diSignal == rb[1].diSignal && rb[0].giSignal == rb[1].giSignal && rb[0].depth == rb[1].depth;
                for (u32 s = 0; s < kRestirStages && same; ++s) {
                    same = std::memcmp(rb[0].diStage[s].data(), rb[1].diStage[s].data(), pixels * sizeof(RestirDiReservoir)) == 0 &&
                           std::memcmp(rb[0].giStage[s].data(), rb[1].giStage[s].data(), pixels * sizeof(RestirGiReservoir)) == 0;
                }
                std::printf("  Slang == GLSL (every stage, signals, depth): %s\n", same ? "bit for bit" : "DIFFERENT");
                expect(same, "Slang == GLSL bit for bit");
            }
            ++frameIndex;
        }
    }
    std::printf("passes: %u pixel-passes bit-identical, %u non-robust (excluded), %u CPU rays traced\n", totals[0], totals[1], totals[2]);
    w.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- denoise ---------------------------------------------------------------------------------------
int runDenoise(Context& ctx) {
    constexpr u32 kW = 64;
    constexpr u32 kH = 48;
    GpuWorld w;
    if (!buildWorld(ctx, w, kW, kH)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(ctx, w, rig, kW, kH, false, 1u);
    if (rc <= 0) {
        destroyRig(rig);
        w.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: ReSTIR unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    const usize pixels = static_cast<usize>(kW) * kH;
    if (!hostBuffer(ctx, ctx.motion, pixels * 8u, MemoryUsage::CpuToGpu, "rp_restir.motion") ||
        !hostBuffer(ctx, ctx.dump[0], pixels * 16u, MemoryUsage::GpuToCpu, "rp_restir.denoised")) {
        return 1;
    }
    std::memset(ctx.motion.mapped, 0, pixels * 8u);
    denoise::SvgfDenoiser dn;
    denoise::SvgfDenoiserDesc dd{};
    dd.device = ctx.device.get();
    dd.allocator = ctx.allocator.get();
    dd.bindless = &ctx.bindless;
    if (!dn.init(dd)) {
        destroyRig(rig);
        w.gpu.destroy();
        std::printf("SKIP: denoiser unavailable\n");
        return kSkip;
    }
    dn.setSettings(denoise::svgf_preset(denoise::DenoiseSignal::Gi));
    RestirSettings settings{};
    settings.diCandidates = 4;
    for (RestirGpu& r : rig.restir) {
        r.setSettings(settings);
    }
    rg::Graph graph;
    FrameState fs;
    fs.readback = false;
    fs.dumps = false;
    fs.motion = true;
    const Camera cam = frameCamera(w, 0);
    Buffer rawCopy{};
    if (!hostBuffer(ctx, rawCopy, pixels * 16u, MemoryUsage::GpuToCpu, "rp_restir.raw")) {
        return 1;
    }
    bool ok = true;
    f64 meanOut = 0.0;
    f64 meanIn = 0.0;
    f64 gradOut = 0.0; ///< sum of |x(p + 1) - x(p)|: pixel-to-pixel noise
    f64 gradIn = 0.0;
    bool finite = true;
    for (u32 frame = 0; frame < 16u && ok; ++frame) {
        beginSceneFrame(ctx, w, rig);
        w.gpu.commit();
        rig.as.commit();
        ctx.upload.flush();
        ok = beginFrame(ctx, w, rig, cam, frame, frame == 0u, fs);
        denoise::DenoiseFrameDesc df{};
        df.width = kW;
        df.height = kH;
        df.signal = rig.restir[0].giSignalAddress();
        df.motion = ctx.motion.deviceAddress;
        df.depth = rig.restir[0].depthAddress();
        df.normal = rig.restir[0].normalAddress();
        df.reset = frame == 0u;
        ok = ok && dn.beginFrame(ctx.serial, df);
        // graph: the rig's frame + the denoiser on the ReSTIR outputs + readback of both
        graph.reset();
        const GpuSceneGraphRefs sceneRefs = w.gpu.importInto(graph);
        const RtGraphRefs rtRefs = rig.as.importInto(graph, sceneRefs);
        const CullGraphRefs cull = rig.culler.importInto(graph);
        const VisGraphRefs vis = rig.vb.importInto(graph);
        rig.vb.addCulledFrame(graph, vis, sceneRefs, w.gpu.headerHandle(), rig.culler, cull);
        const ResolveGraphRefs gbuffer = rig.resolve.importInto(graph);
        rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
        const light_tree::LightTreeGraphRefs treeRefs = rig.tree.importInto(graph);
        const rg::BufferRef motion =
            graph.importBuffer(rg::ImportedBuffer{ctx.motion.handle, ctx.motion.desc.size, rg::kNoQueue, nullptr, "rp_restir.motion"});
        const RestirGraphRefs refs = rig.restir[0].importInto(graph);
        RestirGraphInputs in{};
        in.tlas = rtRefs.tlas;
        in.lightTree = treeRefs;
        in.gbuffer = &gbuffer;
        in.scene = sceneRefs;
        in.motion = motion;
        ok = ok && rig.restir[0].addPasses(graph, refs, in);
        const denoise::DenoiseGraphRefs dref = dn.importInto(graph);
        denoise::DenoiseGraphInputs din{};
        din.signal = refs.output;
        din.motion = motion;
        din.depth = refs.output;
        din.normal = refs.state;
        dn.addPasses(graph, dref, din);
        CopyRecord copies[2]{};
        const rg::BufferRef out = graph.importBuffer(
            rg::ImportedBuffer{ctx.dump[0].handle, ctx.dump[0].desc.size, rg::kNoQueue, nullptr, "rp_restir.denoised"});
        const rg::BufferRef raw = graph.importBuffer(rg::ImportedBuffer{rawCopy.handle, rawCopy.desc.size, rg::kNoQueue, nullptr, "rp_restir.raw"});
        copies[0] = CopyRecord{false, {}, dref.output, out, 0u, pixels * 16u, kW, kH, 0u};
        graph.addPass("readback.denoised", &recordCopy, &copies[0])
            .use(dref.output, rg::Access::TransferSrc, rg::BufferRange{0, pixels * 16u})
            .use(out, rg::Access::TransferDst, rg::BufferRange{0, pixels * 16u});
        const u64 giOffset = rig.restir[0].layout().giSignal;
        copies[1] = CopyRecord{false, {}, refs.output, raw, 0u, pixels * 16u, kW, kH, giOffset};
        graph.addPass("readback.raw", &recordCopy, &copies[1])
            .use(refs.output, rg::Access::TransferSrc, rg::BufferRange{giOffset, pixels * 16u})
            .use(raw, rg::Access::TransferDst, rg::BufferRange{0, pixels * 16u});
        graph.addPass("readback.host", nullptr, nullptr).use(out, rg::Access::HostRead).use(raw, rg::Access::HostRead);
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        ok = ok && result.ok && ctx.executor->waitIdle() && ctx.upload.waitAll();
        collect(ctx, w, rig);
        dn.collectRetired(ctx.serial);
        if (frame == 15u) {
            const f32* o = static_cast<const f32*>(ctx.dump[0].mapped);
            const f32* i = static_cast<const f32*>(rawCopy.mapped);
            for (usize p = 0; p < pixels; ++p) {
                for (u32 ch = 0; ch < 3u; ++ch) {
                    finite = finite && std::isfinite(o[p * 4u + ch]);
                    meanOut += o[p * 4u + ch];
                    meanIn += i[p * 4u + ch];
                    if ((p + 1u) % kW != 0u) {
                        gradOut += std::fabs(o[(p + 1u) * 4u + ch] - o[p * 4u + ch]);
                        gradIn += std::fabs(i[(p + 1u) * 4u + ch] - i[p * 4u + ch]);
                    }
                }
            }
        }
    }
    expect(ok, "frames ok");
    meanOut /= static_cast<f64>(pixels);
    meanIn /= static_cast<f64>(pixels);
    std::printf("denoise: 16 frames ReSTIR GI -> SVGF (GI preset): denoised mean %.5f vs noisy mean %.5f, finite %s; pixel-to-pixel "
                "variation %.4f vs %.4f (x%.1f smoother)\n",
                meanOut, meanIn, finite ? "yes" : "no", gradOut, gradIn, gradIn / std::max(gradOut, 1e-30));
    expect(finite && meanIn > 0.0 && std::fabs(meanOut - meanIn) <= 0.1 * meanIn, "denoised output finite, mean within 10 %");
    expect(gradOut * 2.0 < gradIn, "the denoiser smooths the ReSTIR signal (pixel-to-pixel variation at least halved)");
    dn.destroy();
    ctx.allocator->destroyBuffer(rawCopy);
    w.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------------
void hookBegin(const rg::PassContext&, const char* name, void*) { t_count = std::strncmp(name, "restir.", 7) == 0; }
void hookEnd(const rg::PassContext&, const char*, void*) { t_count = false; }

int runZeroAlloc(Context& ctx, bool countAllocations) {
    constexpr u32 kW = 48;
    constexpr u32 kH = 32;
    GpuWorld w;
    if (!buildWorld(ctx, w, kW, kH)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    Rig rig;
    const int rc = initRig(ctx, w, rig, kW, kH, false, 1u);
    if (rc <= 0) {
        destroyRig(rig);
        w.gpu.destroy();
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: ReSTIR unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    const usize pixels = static_cast<usize>(kW) * kH;
    if (!hostBuffer(ctx, ctx.motion, pixels * 8u, MemoryUsage::CpuToGpu, "rp_restir.motion")) {
        return 1;
    }
    RestirSettings settings{};
    settings.unbiased = true;
    settings.diSpatialIterations = 2;
    settings.giSpatialIterations = 2;
    rig.restir[0].setSettings(settings);
    rg::Graph graph;
    FrameState fs;
    fs.readback = false;
    fs.dumps = false;
    fs.motion = true;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long begin = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    std::vector<f32> motion[2];
    for (u32 i = 0; i < 2u; ++i) {
        motion[i].assign(pixels * 2u, 0.f);
        computeMotion(w, frameCamera(w, i), frameCamera(w, i ^ 1u), motion[i].data());
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, w, rig);
        w.gpu.commit();
        rig.as.commit();
        ctx.upload.flush();
        const Camera cam = frameCamera(w, frame % 2u);
        std::memcpy(ctx.motion.mapped, motion[frame % 2u].data(), pixels * 8u);
        t_allocations = 0;
        bool ok = beginFrame(ctx, w, rig, cam, frame, frame == 0u, fs, measure);
        const unsigned long long inBegin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, w, rig, graph, fs);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        collect(ctx, w, rig);
        ok = ok && result.ok && waited;
        expect(ok, "frame ok");
        if (measure) {
            begin += inBegin;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.restir[0].stats().passes == 2u + 4u + 4u && rig.restir[0].stats().history, "every restir.* pass per frame, history on");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (DI + GI, unbiased, 2 spatial iterations each, camera alternating with motion)\n"
                    "  RestirGpu::beginFrame: %llu operator-new calls; restir.* pass callbacks: %llu; whole graph build: %llu\n",
                    kTotal - kWarmup, begin, callbacks, build);
        expect(begin == 0u && callbacks == 0u, "ReSTIR makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the ReSTIR passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    w.gpu.destroy();
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mode = "passes";
    std::string backend = "set";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            backend = argv[i + 1];
        }
    }
    const bool buffer = backend == "buffer";
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, true, buffer);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "passes") {
            rc = runPasses(ctx);
        } else if (mode == "denoise") {
            rc = runDenoise(ctx);
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
        const int setupRc = setup(counted, false, buffer);
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
