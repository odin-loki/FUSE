// WP-7.3 path-tracing mode, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_pathtrace_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): gpu_scene.* -> rt.* (WP-6.0 BLAS / TLAS) -> light-tree slot (WP-7.1) ->
// pathtrace.trace (one pass per PathTracerGpu instance) -> readback. Instances: the T3 variant (VK_KHR_ray_tracing_
// pipeline: raygen / miss / closest hit / any hit, SBT) and the T2 variant (ray-query compute), each in Slang and GLSL.
//
//   --mode converge     Converge scene (test_rp_pathtrace_scene.hpp) at 24 x 16: 64 frames x 64 spp = 4096 spp per
//                       pixel per variant, against the CPU reference (pt_reference.hpp, 4096 spp): per pixel and
//                       channel |gpu - cpu| / sqrt(se_gpu^2 + se_cpu^2) (standard errors from each side's own sum of
//                       squares): <= 4 for >= 99.5 % of the tests, <= 6 for all; per-channel image z < 4. Variants
//                       compared bit for bit (reported: RT pipeline vs ray query, Slang vs GLSL). The last graph's
//                       readback copies wait on RAY_TRACING_SHADER (rg::kStageRayTracing), no barrier into ALL_COMMANDS
//   --mode furnace      Furnace scene: albedo 1 -> every pixel 1 (1e-5); albedo 0.6 -> every sample is 0.6 or 1
//                       (mean and mean square of each pixel consistent with one two-valued mixture, 1e-5); Russian
//                       roulette from bounce 0 with albedo 1: image mean within 5 sigma of 1
//   --mode determinism  fresh instances replay the same 8 frames bit for bit; another seed differs; accumulation
//                       restarts on a camera change
//   --mode denoise      the T3 path tracer at 1 spp without accumulation feeds, through the IDenoiser interface
//                       (DenoiserRegistry), the in-tree SVGF (created by the registry factory with the device) and an NRD
//                       mock backend (selected for RELAX, adds its pass with the plugin's external accesses): 16 frames
//                       validate, SVGF output finite, remodulated (signal x albedo guide) mean within 10 % of the noisy
//                       frame's, RMSE against a 256-spp CPU reference below 0.8 x the 1-spp frame's (surface pixels)
//   --mode zero_alloc   64 steady-state accumulating frames, both backends: 0 operator-new calls in
//                       PathTracerGpu::beginFrame, the pathtrace.* callbacks and the whole graph build
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, device below T2, no kernel built).
#include "test_rp_pathtrace_scene.hpp"

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

#include <fuse/renderer/denoise/denoiser.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/pathtrace/pt_gpu.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rg/sync_model.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
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
using namespace fuse::renderer::pathtrace;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

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
constexpr u32 kVariants = 4; // RT pipeline Slang, RT pipeline GLSL, ray query Slang, ray query GLSL

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
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    Buffer motion{};
    u64 serial = 0;
    PtCapabilities caps{};

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
    instanceDesc.appName = "fuse_rp_pathtrace";
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
    ctx.caps = queryPtCapabilities(ctx.device.get());
    if (!ctx.caps.rayQuery) {
        std::printf("SKIP: T2 gate: %s\n", ctx.caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s; %s\n", ctx.device->info().deviceName.c_str(), ctx.device->info().caps.summary().c_str());
    std::printf("path tracer gate: ray query %s (%s); RT pipeline %s (%s)\n", ctx.caps.rayQuery ? "yes" : "no", ctx.caps.reason,
                ctx.caps.rayTracingPipeline ? "yes" : "no", ctx.caps.pipelineReason);
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
    stagingDesc.name = "rp_pathtrace.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

bool hostBuffer(Context& ctx, Buffer& b, usize bytes, MemoryUsage memory, BufferUsage usage, const char* name) {
    if (b.handle != nullptr && b.desc.size >= bytes) {
        return true;
    }
    if (b.handle != nullptr) {
        ctx.allocator->destroyBuffer(b);
    }
    BufferDesc d{};
    d.size = bytes;
    d.usage = usage;
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
}

// --- scene + rig ------------------------------------------------------------------------------------
struct Rig {
    gpu_scene::GpuScene scene;
    pt_test::World world;
    rt::AccelerationStructures as;
    light_tree::LightTreeGpu tree;
    PathTracerGpu pt[kVariants];
    bool built[kVariants] = {};
    const char* name[kVariants] = {"rt_pipeline/slang", "rt_pipeline/glsl", "ray_query/slang", "ray_query/glsl"};
    bool lights = false;
};

void destroyRig(Rig& rig) {
    for (PathTracerGpu& p : rig.pt) {
        p.destroy();
    }
    rig.tree.destroy();
    rig.as.destroy();
    rig.scene.destroy();
}

/// 1 ok, 0 nothing to run (skip), -1 failure. `mask` selects variants (bit i = variant i).
int initRig(Context& ctx, Rig& rig, pt_test::SceneKind kind, u32 width, u32 height, u32 mask, f32 albedo = 1.f) {
    gpu_scene::GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 16;
    if (!rig.scene.init(d) || !rig.scene.gpuEnabled()) {
        return -1;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
    if (!pt_test::buildWorld(rig.world, rig.scene, kind, width, height, albedo)) {
        return -1;
    }
    const gpu_scene::GpuSceneCommitStats stats = rig.scene.commit();
    ctx.upload.flush();
    if (!stats.ok || !ctx.upload.waitAll()) {
        return -1;
    }
    rt::AccelerationStructuresDesc ad{};
    ad.device = ctx.device.get();
    ad.allocator = ctx.allocator.get();
    ad.upload = &ctx.upload;
    ad.scene = &rig.scene;
    ad.bindless = &ctx.bindless;
    ad.instanceCapacity = 16;
    ad.meshCapacity = 4;
    if (!rig.as.init(ad)) {
        std::printf("  acceleration structures: %s\n", rig.as.reason());
        return 0;
    }
    rig.lights = !rig.world.lights.empty();
    if (rig.lights) {
        light_tree::LightTreeGpuDesc td{};
        td.device = ctx.device.get();
        td.allocator = ctx.allocator.get();
        td.bindless = &ctx.bindless;
        td.initialLights = static_cast<u32>(rig.world.lights.size());
        if (!rig.tree.init(td)) {
            std::printf("  light tree GPU unavailable\n");
            return 0;
        }
    }
    u32 built = 0;
    for (u32 k = 0; k < kVariants; ++k) {
        if ((mask & (1u << k)) == 0u) {
            continue;
        }
        PathTracerGpuDesc pd{};
        pd.device = ctx.device.get();
        pd.allocator = ctx.allocator.get();
        pd.bindless = &ctx.bindless;
        pd.backend = k < 2u ? PtBackend::RayTracingPipeline : PtBackend::RayQuery;
        pd.language = (k % 2u) == 0u ? PtKernelLanguage::Slang : PtKernelLanguage::Glsl;
        pd.width = width;
        pd.height = height;
        const char* lang = (k % 2u) == 0u ? "slang" : "glsl";
        rig.built[k] = rig.pt[k].init(pd) && std::strcmp(rig.pt[k].kernelLanguage(), lang) == 0;
        built += rig.built[k] ? 1u : 0u;
        std::printf("  %-18s %s (%s)\n", rig.name[k], rig.built[k] ? "built" : "not built, skipped", rig.pt[k].reason());
        if (rig.built[k] && k < 2u && k == 0u) {
            const PtSbtLayout& s = rig.pt[k].sbtLayout();
            std::printf("  SBT: handle %u B, record stride %u, raygen %llu B, miss @%llu, hit @%llu (2 records), %llu B\n", s.handleSize,
                        s.recordStride, static_cast<unsigned long long>(s.raygen.size), static_cast<unsigned long long>(s.miss.offset),
                        static_cast<unsigned long long>(s.hit.offset), static_cast<unsigned long long>(s.bytes));
        }
    }
    return built > 0u ? 1 : 0;
}

void setSettings(Rig& rig, const PtSettings& s) {
    for (u32 k = 0; k < kVariants; ++k) {
        if (rig.built[k]) {
            rig.pt[k].setSettings(s);
        }
    }
}

// --- frames --------------------------------------------------------------------------------------------
struct CopyRecord {
    rg::BufferRef src;
    rg::BufferRef dst;
    u64 srcOffset = 0;
    u64 dstOffset = 0;
    u64 bytes = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    const VkBufferCopy region{c.srcOffset, c.dstOffset, c.bytes};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(c.src)),
                    static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

/// Per variant: accum + accumSq (state) and mean (output) read back.
struct Readback {
    u64 state[kVariants] = {};
    u64 output[kVariants] = {};
    u64 end = 0;
};

struct Frame {
    CopyRecord copies[2 * kVariants];
    u32 copyCount = 0;
    Readback layout{};
    bool readback = true;
    u32 frameIndex = 0;
    u32 seed = 0x5EEDu;
    bool reset = false;
    PtCamera camera{};
    PtGraphRefs refs[kVariants]{};
    rg::BufferRef tlas;
    light_tree::LightTreeGraphRefs treeRefs{};
    gpu_scene::GpuSceneGraphRefs sceneRefs{};
};

void beginSceneFrame(Context& ctx, Rig& rig) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    rig.scene.beginFrame(ctx.serial);
    rig.as.beginFrame(ctx.serial);
}

bool beginPtFrames(Context& ctx, Rig& rig, const Frame& f, bool count) {
    bool ok = true;
    PtFrameDesc desc{};
    desc.camera = f.camera;
    desc.width = rig.world.width;
    desc.height = rig.world.height;
    desc.tlasAddress = rig.as.tlasAddress();
    desc.sceneHeader = rig.scene.headerAddress();
    if (rig.lights) {
        desc.lightTreeHeader = rig.tree.headerAddress();
        desc.lights = rig.world.table.data();
        desc.lightCount = static_cast<u32>(rig.world.table.size());
        desc.lightsVersion = rig.world.tree.version();
        desc.emitterMap = rig.world.emitterMap.data();
        desc.emitterMapWords = static_cast<u32>(rig.world.emitterMap.size());
        desc.emitterMapSlots = rig.scene.instanceHighWater();
        desc.emitterMapVersion = 1;
    }
    desc.frameIndex = f.frameIndex;
    desc.seed = f.seed;
    desc.reset = f.reset;
    for (u32 k = 0; k < kVariants; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        t_count = count;
        ok = rig.pt[k].beginFrame(ctx.serial, desc) && ok;
        t_count = false;
    }
    return ok;
}

void buildGraph(Context& ctx, Rig& rig, rg::Graph& graph, Frame& f) {
    graph.reset();
    f.copyCount = 0;
    f.sceneRefs = rig.scene.importInto(graph);
    const rt::RtGraphRefs rtRefs = rig.as.importInto(graph, f.sceneRefs);
    f.tlas = rtRefs.tlas;
    f.treeRefs = rig.lights ? rig.tree.importInto(graph) : light_tree::LightTreeGraphRefs{};
    for (u32 k = 0; k < kVariants; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        f.refs[k] = rig.pt[k].importInto(graph);
        PtGraphInputs in{};
        in.tlas = rtRefs.tlas;
        in.lightTree = f.treeRefs;
        in.scene = f.sceneRefs;
        expect(rig.pt[k].addPasses(graph, f.refs[k], in), "PathTracerGpu::addPasses");
    }
    if (!f.readback) {
        for (u32 k = 0; k < kVariants; ++k) {
            if (rig.built[k]) {
                rg::PassBuilder pass = graph.addPass("pathtrace.use", nullptr, nullptr);
                pass.neverCull();
                pass.use(f.refs[k].output, rg::Access::StorageRead, {}, rg::kStageCompute);
            }
        }
        return;
    }
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, f.layout.end, rg::kNoQueue, nullptr, "rp_pathtrace.readback"});
    for (u32 k = 0; k < kVariants; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const PtBufferLayout& l = rig.pt[k].layout();
        const rg::BufferRef srcs[2] = {f.refs[k].state, f.refs[k].output};
        const u64 bytes[2] = {l.stateBytes, l.outputBytes};
        const u64 dsts[2] = {f.layout.state[k], f.layout.output[k]};
        for (u32 i = 0; i < 2u; ++i) {
            CopyRecord& c = f.copies[f.copyCount++];
            c = CopyRecord{srcs[i], rb, 0u, dsts[i], bytes[i]};
            rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
            pass.use(srcs[i], rg::Access::TransferSrc, rg::BufferRange{0, bytes[i]});
            pass.use(rb, rg::Access::TransferDst, rg::BufferRange{dsts[i], bytes[i]});
        }
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void collect(Context& ctx, Rig& rig) {
    rig.as.collectRetired(ctx.serial);
    if (rig.lights) {
        rig.tree.collectRetired(ctx.serial);
    }
    for (PathTracerGpu& p : rig.pt) {
        if (p.valid()) {
            p.collectRetired(ctx.serial);
        }
    }
    rig.scene.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
}

bool ensureReadback(Context& ctx, Rig& rig, Frame& f) {
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    for (u32 k = 0; k < kVariants; ++k) {
        if (rig.built[k]) {
            f.layout.state[k] = take(rig.pt[k].layout().stateBytes);
            f.layout.output[k] = take(rig.pt[k].layout().outputBytes);
        }
    }
    f.layout.end = cursor;
    return hostBuffer(ctx, ctx.readback, static_cast<usize>(cursor), MemoryUsage::GpuToCpu, BufferUsage::TransferDst, "rp_pathtrace.readback");
}

bool runFrame(Context& ctx, Rig& rig, rg::Graph& graph, Frame& f) {
    beginSceneFrame(ctx, rig);
    const gpu_scene::GpuSceneCommitStats stats = rig.scene.commit();
    const rt::RtCommitStats rtStats = rig.as.commit();
    ctx.upload.flush();
    bool ok = !rig.lights || rig.tree.beginFrame(ctx.serial, rig.world.tree);
    ok = beginPtFrames(ctx, rig, f, false) && ok;
    if (!ok) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    if (f.readback && !ensureReadback(ctx, rig, f)) {
        return false;
    }
    buildGraph(ctx, rig, graph, f);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    collect(ctx, rig);
    return stats.ok && rtStats.ok && result.ok && waited;
}

/// Mean / standard error per pixel and channel from a variant's readback.
struct GpuImage {
    std::vector<f64> mean;     ///< 3 per pixel
    std::vector<f64> variance; ///< 3 per pixel (variance of the mean)
    std::vector<f32> raw;      ///< accum f32x4 per pixel (bitwise comparisons)
    u32 samples = 0;
};

GpuImage readImage(Context& ctx, Rig& rig, const Frame& f, u32 k) {
    GpuImage img;
    const PtBufferLayout& l = rig.pt[k].layout();
    const u64 pixels = static_cast<u64>(l.width) * l.height;
    const u8* base = static_cast<const u8*>(ctx.readback.mapped);
    const f32* acc = reinterpret_cast<const f32*>(base + f.layout.state[k] + l.accum);
    const f32* acc2 = reinterpret_cast<const f32*>(base + f.layout.state[k] + l.accumSq);
    img.mean.resize(pixels * 3u);
    img.variance.resize(pixels * 3u);
    img.raw.assign(acc, acc + pixels * 4u);
    for (u64 p = 0; p < pixels; ++p) {
        const f64 n = acc[p * 4u + 3u];
        img.samples = static_cast<u32>(n);
        for (u32 c = 0; c < 3u; ++c) {
            const f64 m = acc[p * 4u + c] / n;
            const f64 var = n > 1.0 ? std::max(acc2[p * 4u + c] / n - m * m, 0.0) * n / (n - 1.0) : 0.0;
            img.mean[p * 3u + c] = m;
            img.variance[p * 3u + c] = var / n;
        }
    }
    return img;
}

// --- converge -------------------------------------------------------------------------------------------
int runConverge(Context& ctx) {
    constexpr u32 kW = 24;
    constexpr u32 kH = 16;
    constexpr u32 kSpp = 64;
    constexpr u32 kFrames = 64; // 4096 spp
    Rig rig;
    const int rc = initRig(ctx, rig, pt_test::SceneKind::Converge, kW, kH, 0xFu);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: path tracer unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    PtSettings s = pt_test::baseSettings(rig.world);
    s.samplesPerFrame = kSpp;
    setSettings(rig, s);
    rg::Graph graph;
    Frame f;
    f.camera = rig.world.camera;
    bool ok = true;
    for (u32 frame = 0; frame < kFrames && ok; ++frame) {
        f.frameIndex = frame;
        f.readback = frame + 1u == kFrames;
        ok = runFrame(ctx, rig, graph, f);
    }
    expect(ok, "frames ran");
    {
        // The RT-pipeline passes declare their accesses at rg::kStageRayTracing: the readback copies of their state /
        // output wait on RAY_TRACING_SHADER, and no barrier waits at ALL_COMMANDS (the old ExternalRead / ExternalWrite).
        u32 rtVariants = 0;
        for (u32 k = 0; k < 2u; ++k) {
            rtVariants += rig.built[k] ? 1u : 0u;
        }
        u32 fromRt = 0;
        u32 intoRt = 0;
        u32 allCommands = 0;
        for (const rg::BufferBarrier& b : graph.bufferBarriers()) {
            fromRt += (b.srcStages & rg::vkc::kStageRayTracingShader) != 0u ? 1u : 0u;
            intoRt += (b.dstStages & rg::vkc::kStageRayTracingShader) != 0u ? 1u : 0u;
            allCommands += (b.dstStages & rg::vkc::kStageAllCommands) != 0u ? 1u : 0u; // imports start at ALL_COMMANDS (src)
        }
        std::printf("converge: last graph: %u buffer barriers from the ray-tracing stage, %u into it, %u into ALL_COMMANDS\n", fromRt,
                    intoRt, allCommands);
        expect(fromRt >= 2u * rtVariants, "readback copies wait on the RT-pipeline pass at RAY_TRACING_SHADER");
        expect(allCommands == 0u, "no barrier into ALL_COMMANDS (RT-pipeline accesses declared at the ray-tracing stage)");
    }
    std::printf("converge: %u frames x %u spp = %u spp per pixel per variant, %zu emitters (%zu traced + mapped)\n", kFrames, kSpp,
                kFrames * kSpp, rig.world.table.size(), rig.world.emitterRefs.size());
    GpuImage img[kVariants];
    for (u32 k = 0; k < kVariants; ++k) {
        if (rig.built[k]) {
            img[k] = readImage(ctx, rig, f, k);
            expect(img[k].samples == kFrames * kSpp && rig.pt[k].stats().samples == kFrames * kSpp, "accumulated sample count");
        }
    }
    // CPU reference.
    PtReference ref;
    expect(ref.setup(s, rig.world.camera, kW, kH), "reference setup");
    PtReferenceImage cpu;
    expect(ref.render(rig.world.cpu, kFrames * kSpp, 0xC0FFEEull, 4u, cpu), "reference render");
    for (u32 k = 0; k < kVariants; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        u32 tests = 0;
        u32 within4 = 0;
        f64 worst = 0.0;
        f64 diff[3] = {0.0, 0.0, 0.0};
        f64 var[3] = {0.0, 0.0, 0.0};
        f64 meanGpu = 0.0;
        f64 meanCpu = 0.0;
        u32 nonFinite = 0;
        for (usize i = 0; i < cpu.mean.size(); ++i) {
            const f64 g = img[k].mean[i];
            if (!std::isfinite(g)) {
                ++nonFinite;
                continue;
            }
            const f64 v = img[k].variance[i] + cpu.variance[i];
            const f64 d = g - cpu.mean[i];
            diff[i % 3u] += d;
            var[i % 3u] += v;
            meanGpu += g;
            meanCpu += cpu.mean[i];
            ++tests;
            const f64 z = v > 0.0 ? std::fabs(d) / std::sqrt(v) : (std::fabs(d) < 1e-6 ? 0.0 : 1e9);
            within4 += z <= 4.0 ? 1u : 0u;
            worst = std::max(worst, z);
        }
        f64 imageZ = 0.0;
        for (u32 c = 0; c < 3u; ++c) {
            imageZ = std::max(imageZ, std::fabs(diff[c] / std::sqrt(var[c])));
        }
        std::printf("converge %-18s vs CPU reference: %u / %u channel tests within 4 sigma (%.2f %%), worst %.2f sigma, worst "
                    "channel image z %.2f; image mean %.5f vs %.5f; %u non-finite\n",
                    rig.name[k], within4, tests, 100.0 * within4 / std::max(tests, 1u), worst, imageZ, meanGpu / std::max(tests, 1u),
                    meanCpu / std::max(tests, 1u), nonFinite);
        expect(nonFinite == 0u, "finite GPU image");
        expect(within4 * 1000u >= tests * 995u, ">= 99.5 % of the channel tests within 4 sigma");
        expect(worst <= 6.0, "every channel test within 6 sigma");
        expect(imageZ < 4.0, "per-channel image mean within 4 sigma");
    }
    auto bitDiff = [&](u32 a, u32 b) -> long long {
        if (!rig.built[a] || !rig.built[b]) {
            return -1;
        }
        long long n = 0;
        for (usize i = 0; i < img[a].raw.size(); ++i) {
            n += std::memcmp(&img[a].raw[i], &img[b].raw[i], 4u) != 0 ? 1 : 0;
        }
        return n;
    };
    std::printf("converge: accumulation words differing: RT pipeline vs ray query: slang %lld, glsl %lld; slang vs glsl: RT pipeline "
                "%lld, ray query %lld (of %zu)\n",
                static_cast<long long>(bitDiff(0, 2)), static_cast<long long>(bitDiff(1, 3)), static_cast<long long>(bitDiff(0, 1)),
                static_cast<long long>(bitDiff(2, 3)), img[0].raw.size());
    expect(bitDiff(0, 2) <= 0 && bitDiff(1, 3) <= 0, "T3 RT pipeline == T2 ray query bit for bit (same language)");
    destroyRig(rig);
    return 0;
}

// --- furnace --------------------------------------------------------------------------------------------
int runFurnace(Context& ctx) {
    constexpr u32 kW = 16;
    constexpr u32 kH = 12;
    for (const f32 albedo : {1.f, 0.6f}) {
        Rig rig;
        const int rc = initRig(ctx, rig, pt_test::SceneKind::Furnace, kW, kH, 0xFu, albedo);
        if (rc <= 0) {
            destroyRig(rig);
            std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: path tracer unavailable\n");
            return rc < 0 ? 1 : kSkip;
        }
        for (const bool rr : {false, true}) {
            if (rr && albedo != 1.f) {
                continue;
            }
            PtSettings s = pt_test::baseSettings(rig.world);
            s.samplesPerFrame = 32;
            s.maxBounces = 8;
            s.rrStartBounce = rr ? 0u : 3u;
            setSettings(rig, s);
            rg::Graph graph;
            Frame f;
            f.camera = rig.world.camera;
            f.reset = true;
            constexpr u32 kFrames = 8;
            bool ok = true;
            for (u32 frame = 0; frame < kFrames && ok; ++frame) {
                f.frameIndex = frame;
                f.readback = frame + 1u == kFrames;
                ok = runFrame(ctx, rig, graph, f);
                f.reset = false;
            }
            expect(ok, "frames ran");
            for (u32 k = 0; k < kVariants; ++k) {
                if (!rig.built[k]) {
                    continue;
                }
                const GpuImage img = readImage(ctx, rig, f, k);
                const usize pixels = static_cast<usize>(kW) * kH;
                if (!rr) {
                    // Every sample is albedo (box) or 1 (sky): with box fraction q, mean = 1 - q (1 - a) and
                    // E[x^2] = 1 - q (1 - a^2); check E[x^2] predicted from the mean.
                    f64 worst = 0.0;
                    u32 box = 0;
                    u32 sky = 0;
                    for (usize p = 0; p < pixels; ++p) {
                        for (u32 c = 0; c < 3u; ++c) {
                            const f64 n = img.samples;
                            const f64 m = img.mean[p * 3u + c];
                            const f64 var = img.variance[p * 3u + c] * n * (n - 1.0) / n; // population variance
                            const f64 m2 = var + m * m;
                            f64 predicted = m;
                            if (albedo != 1.f) {
                                const f64 q = (1.0 - m) / (1.0 - albedo);
                                predicted = 1.0 - q * (1.0 - static_cast<f64>(albedo) * albedo);
                                worst = std::max(worst, std::fabs(m2 - predicted));
                                worst = std::max(worst, q < -1e-5 || q > 1.0 + 1e-5 ? 1.0 : 0.0);
                                box += (c == 0u && q > 0.999) ? 1u : 0u;
                                sky += (c == 0u && q < 0.001) ? 1u : 0u;
                            } else {
                                worst = std::max(worst, std::fabs(m - 1.0));
                                worst = std::max(worst, std::fabs(m2 - 1.0));
                            }
                        }
                    }
                    std::printf("furnace albedo %.1f %-18s: %u spp, %s max deviation %.2e (%u full-box / %u sky pixels)\n", albedo,
                                rig.name[k], img.samples, albedo == 1.f ? "every pixel == 1:" : "every sample albedo or 1:", worst, box,
                                sky);
                    expect(worst < 2e-5, "furnace exact (every path returns albedo or the sky)");
                    expect(albedo == 1.f || (box > 10u && sky > 10u), "box and sky visible");
                } else {
                    f64 sum = 0.0;
                    f64 var = 0.0;
                    u32 noisy = 0;
                    for (usize p = 0; p < pixels; ++p) {
                        sum += img.mean[p * 3u] - 1.0;
                        var += img.variance[p * 3u];
                        noisy += img.variance[p * 3u] > 0.0 ? 1u : 0u;
                    }
                    const f64 z = var > 0.0 ? sum / std::sqrt(var) : 0.0;
                    std::printf("furnace albedo 1 + RR from bounce 0 %-18s: %u noisy pixels, image z %+.2f\n", rig.name[k], noisy, z);
                    expect(noisy > 0u && std::fabs(z) < 5.0, "furnace with Russian roulette unbiased (5 sigma)");
                }
            }
        }
        destroyRig(rig);
    }
    return 0;
}

// --- determinism ----------------------------------------------------------------------------------------
int runDeterminism(Context& ctx) {
    constexpr u32 kW = 20;
    constexpr u32 kH = 12;
    std::vector<f32> runs[3][kVariants];
    for (u32 run = 0; run < 3u; ++run) {
        Rig rig;
        const int rc = initRig(ctx, rig, pt_test::SceneKind::Converge, kW, kH, 0xFu);
        if (rc <= 0) {
            destroyRig(rig);
            std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: path tracer unavailable\n");
            return rc < 0 ? 1 : kSkip;
        }
        PtSettings s = pt_test::baseSettings(rig.world);
        s.samplesPerFrame = 4;
        setSettings(rig, s);
        rg::Graph graph;
        Frame f;
        f.camera = rig.world.camera;
        f.seed = run == 2u ? 0xBADu : 0x5EEDu;
        bool ok = true;
        for (u32 frame = 0; frame < 8u && ok; ++frame) {
            f.frameIndex = frame;
            f.readback = frame == 7u;
            ok = runFrame(ctx, rig, graph, f);
        }
        expect(ok, "frames ran");
        for (u32 k = 0; k < kVariants; ++k) {
            if (rig.built[k]) {
                runs[run][k] = readImage(ctx, rig, f, k).raw;
                expect(rig.pt[k].sampleCount() == 32u, "8 frames x 4 spp accumulated");
            }
        }
        if (run == 0u) {
            // A camera change restarts the accumulation.
            Frame g = f;
            g.camera.position[0] += 0.05f;
            g.camera.viewProj[12] += 0.01f;
            g.readback = false;
            expect(runFrame(ctx, rig, graph, g), "moved frame");
            bool restarted = true;
            for (u32 k = 0; k < kVariants; ++k) {
                restarted = restarted && (!rig.built[k] || rig.pt[k].sampleCount() == 4u);
            }
            expect(restarted, "camera change restarts the accumulation");
            std::printf("determinism: camera change -> accumulation restarted (4 spp)\n");
        }
        destroyRig(rig);
    }
    for (u32 k = 0; k < kVariants; ++k) {
        if (runs[0][k].empty()) {
            continue;
        }
        const bool same = runs[0][k].size() == runs[1][k].size() &&
                          std::memcmp(runs[0][k].data(), runs[1][k].data(), runs[0][k].size() * 4u) == 0;
        const bool differs = std::memcmp(runs[0][k].data(), runs[2][k].data(), runs[0][k].size() * 4u) != 0;
        std::printf("determinism %-18s: fresh instance replays 8 frames bit for bit: %s; another seed differs: %s\n",
                    k == 0u ? "rt_pipeline/slang" : (k == 1u ? "rt_pipeline/glsl" : (k == 2u ? "ray_query/slang" : "ray_query/glsl")),
                    same ? "yes" : "no", differs ? "yes" : "no");
        expect(same, "deterministic");
        expect(differs, "seed changes the image");
    }
    return 0;
}

// --- denoise (IDenoiser through the registry) ------------------------------------------------------------
class MockNrd final : public denoise::IDenoiser {
public:
    const denoise::DenoiserCaps& caps() const override { return s_caps; }
    bool configure(const denoise::DenoiserSettings& settings) override {
        m_settings = settings;
        return true;
    }
    const denoise::DenoiserSettings& settings() const override { return m_settings; }
    void reset() override {}
    bool beginFrame(u64, const denoise::DenoiseFrameDesc& frame) override {
        m_frame = frame;
        ++frames;
        return frame.signal != 0u && frame.depth != 0u && frame.normal != 0u && frame.motion != 0u;
    }
    denoise::DenoiseGraphRefs importInto(rg::Graph&) override { return {}; }
    void addPasses(rg::Graph& graph, const denoise::DenoiseGraphRefs&, const denoise::DenoiseGraphInputs& in) override {
        // The NRD plugin's pass shape: one pass, external accesses on every bound resource (WP-6.4b NrdDenoiser).
        rg::PassBuilder pass = graph.addPass("denoise.nrd.mock", nullptr, nullptr);
        pass.neverCull();
        pass.use(in.signal, rg::Access::ExternalRead).use(in.motion, rg::Access::ExternalRead);
        ++passes;
    }
    u32 collectRetired(u64) override { return 0; }
    static denoise::DenoiserCaps s_caps;
    u32 frames = 0;
    u32 passes = 0;

private:
    denoise::DenoiserSettings m_settings{};
    denoise::DenoiseFrameDesc m_frame{};
};
denoise::DenoiserCaps MockNrd::s_caps{};

std::unique_ptr<denoise::IDenoiser> makeMockNrd(const denoise::DenoiserCreateInfo&) { return std::make_unique<MockNrd>(); }

int runDenoise(Context& ctx) {
    constexpr u32 kW = 32;
    constexpr u32 kH = 24;
    Rig rig;
    const int rc = initRig(ctx, rig, pt_test::SceneKind::Converge, kW, kH, ctx.caps.rayTracingPipeline ? 0x1u : 0x4u);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: path tracer unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    const u32 k = rig.built[0] ? 0u : 2u;
    PtSettings s = pt_test::baseSettings(rig.world);
    s.accumulate = false;
    s.samplesPerFrame = 1;
    setSettings(rig, s);
    // Registries: the in-tree SVGF (created with the device) and an NRD mock that serves RELAX.
    denoise::DenoiserRegistry registry;
    registry.register_builtin_denoisers();
    MockNrd::s_caps = denoise::DenoiserCaps{};
    MockNrd::s_caps.name = "nrd";
    MockNrd::s_caps.display_name = "NRD (mock)";
    MockNrd::s_caps.methods = denoise::denoiser_method_bit(denoise::DenoiserMethod::Relax);
    MockNrd::s_caps.signals = denoise::denoise_signal_bit(denoise::DenoiseSignal::Gi);
    MockNrd::s_caps.needs_native_frame = true;
    expect(registry.register_backend(MockNrd::s_caps, &makeMockNrd), "mock NRD registered");
    PtReconstructionRequest svgfReq{};
    svgfReq.denoiserMethod = denoise::DenoiserMethod::Svgf;
    const PtReconstruction svgfSel = selectPtReconstruction(nullptr, &registry, svgfReq);
    const PtReconstruction nrdSel = selectPtReconstruction(nullptr, &registry, PtReconstructionRequest{});
    std::printf("denoise: selections: SVGF request -> %s, RELAX request -> %s %s\n", svgfSel.backend, nrdSel.backend,
                denoise::denoiser_method_name(nrdSel.method));
    expect(std::strcmp(svgfSel.backend, "svgf") == 0 && std::strcmp(nrdSel.backend, "nrd") == 0, "registry selections");
    denoise::DenoiserCreateInfo ci{};
    ci.svgf.device = ctx.device.get();
    ci.svgf.allocator = ctx.allocator.get();
    ci.svgf.bindless = &ctx.bindless;
    std::unique_ptr<denoise::IDenoiser> svgf = registry.create(svgfSel.backend, ci);
    std::unique_ptr<denoise::IDenoiser> nrd = registry.create(nrdSel.backend, ci);
    if (svgf == nullptr || nrd == nullptr) {
        destroyRig(rig);
        std::printf("SKIP: denoiser unavailable\n");
        return kSkip;
    }
    denoise::DenoiserSettings ds{};
    ds.signal = denoise::DenoiseSignal::Gi;
    ds.method = svgfSel.method;
    expect(svgf->configure(ds), "svgf configure");
    ds.method = nrdSel.method;
    expect(nrd->configure(ds), "nrd configure");
    const usize pixels = static_cast<usize>(kW) * kH;
    if (!hostBuffer(ctx, ctx.motion, pixels * 8u, MemoryUsage::CpuToGpu,
                    static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress)),
                    "rp_pathtrace.motion")) {
        return 1;
    }
    std::memset(ctx.motion.mapped, 0, pixels * 8u);
    // Readback: the PT output (signal) and the SVGF output.
    rg::Graph graph;
    Frame f;
    f.camera = rig.world.camera;
    constexpr u32 kFrames = 16;
    std::vector<f32> noisy(pixels * 4u);
    std::vector<f32> denoised(pixels * 4u);
    std::vector<f32> albedo(pixels * 4u);
    bool ok = true;
    for (u32 frame = 0; frame < kFrames && ok; ++frame) {
        const bool last = frame + 1u == kFrames;
        beginSceneFrame(ctx, rig);
        ok = rig.scene.commit().ok && rig.as.commit().ok;
        ctx.upload.flush();
        ok = rig.tree.beginFrame(ctx.serial, rig.world.tree) && ok;
        f.frameIndex = frame;
        f.readback = false;
        ok = beginPtFrames(ctx, rig, f, false) && ok;
        ok = svgf->beginFrame(ctx.serial, rig.pt[k].denoiseFrame(ctx.motion.deviceAddress, frame == 0u)) && ok;
        ok = nrd->beginFrame(ctx.serial, rig.pt[k].denoiseFrame(ctx.motion.deviceAddress, frame == 0u)) && ok;
        buildGraph(ctx, rig, graph, f);
        const rg::BufferRef motion =
            graph.importBuffer(rg::ImportedBuffer{ctx.motion.handle, ctx.motion.desc.size, rg::kNoQueue, nullptr, "rp_pathtrace.motion"});
        const denoise::DenoiseGraphInputs in = rig.pt[k].denoiseInputs(f.refs[k], motion);
        const denoise::DenoiseGraphRefs dr = svgf->importInto(graph);
        svgf->addPasses(graph, dr, in);
        const denoise::DenoiseGraphRefs nr = nrd->importInto(graph);
        nrd->addPasses(graph, nr, in);
        CopyRecord copies[3];
        if (last) {
            ok = hostBuffer(ctx, ctx.readback, pixels * 48u + 256u, MemoryUsage::GpuToCpu, BufferUsage::TransferDst, "rp_pathtrace.readback") && ok;
            const rg::BufferRef rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr,
                                                                           "rp_pathtrace.readback"});
            copies[0] = CopyRecord{f.refs[k].output, rb, rig.pt[k].layout().signal, 0u, pixels * 16u};
            copies[1] = CopyRecord{dr.output, rb, 0u, pixels * 16u, pixels * 16u};
            copies[2] = CopyRecord{f.refs[k].output, rb, rig.pt[k].layout().albedo, pixels * 32u, pixels * 16u};
            for (CopyRecord& c : copies) {
                rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
                pass.use(c.src, rg::Access::TransferSrc, rg::BufferRange{c.srcOffset, c.bytes});
                pass.use(rb, rg::Access::TransferDst, rg::BufferRange{c.dstOffset, c.bytes});
            }
            graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
        }
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        ok = result.ok && ctx.executor->waitIdle() && ctx.upload.waitAll() && ok;
        svgf->collectRetired(ctx.serial);
        nrd->collectRetired(ctx.serial);
        collect(ctx, rig);
        if (last && ok) {
            const u8* base = static_cast<const u8*>(ctx.readback.mapped);
            std::memcpy(noisy.data(), base, pixels * 16u);
            std::memcpy(denoised.data(), base + pixels * 16u, pixels * 16u);
            std::memcpy(albedo.data(), base + pixels * 32u, pixels * 16u);
        }
    }
    expect(ok, "denoise frames ran");
    const MockNrd* mock = static_cast<const MockNrd*>(nrd.get());
    expect(mock->frames == kFrames && mock->passes == kFrames, "NRD mock driven every frame through IDenoiser");
    // Converged reference (CPU, 256 spp) to measure the error of the remodulated noisy / denoised images.
    PtReference ref;
    expect(ref.setup(s, rig.world.camera, kW, kH), "reference setup");
    PtReferenceImage truth;
    expect(ref.render(rig.world.cpu, 256, 77, 4u, truth), "reference render");
    // Error over the surface pixels (directly visible emitters, whose value is exact at 1 spp, excluded).
    f64 meanNoisy = 0.0;
    f64 meanDenoised = 0.0;
    f64 errNoisy = 0.0;
    f64 errDenoised = 0.0;
    u32 finite = 0;
    u32 counted = 0;
    for (usize p = 0; p < pixels; ++p) {
        finite += std::isfinite(denoised[p * 4u]) && std::isfinite(denoised[p * 4u + 1u]) && std::isfinite(denoised[p * 4u + 2u]) ? 1u : 0u;
        if (std::max(truth.mean[p * 3u], std::max(truth.mean[p * 3u + 1u], truth.mean[p * 3u + 2u])) > 1.5) {
            continue;
        }
        ++counted;
        for (u32 c = 0; c < 3u; ++c) {
            const f64 a = static_cast<f64>(noisy[p * 4u + c]) * albedo[p * 4u + c];
            const f64 b = static_cast<f64>(denoised[p * 4u + c]) * albedo[p * 4u + c];
            const f64 t = truth.mean[p * 3u + c];
            meanNoisy += a;
            meanDenoised += b;
            errNoisy += (a - t) * (a - t);
            errDenoised += (b - t) * (b - t);
        }
    }
    meanNoisy /= counted * 3.0;
    meanDenoised /= counted * 3.0;
    const f64 rmseNoisy = std::sqrt(errNoisy / (counted * 3.0));
    const f64 rmseDenoised = std::sqrt(errDenoised / (counted * 3.0));
    std::printf("denoise (%s -> IDenoiser svgf + nrd mock, 1 spp / frame, %u frames): finite %u / %zu; %u surface pixels: remodulated "
                "mean %.4f -> %.4f, RMSE vs 256-spp CPU reference %.4f -> %.4f (x%.2f lower)\n",
                rig.name[k], kFrames, finite, pixels, counted, meanNoisy, meanDenoised, rmseNoisy, rmseDenoised, rmseNoisy / rmseDenoised);
    expect(finite == pixels, "denoised output finite");
    expect(counted > pixels / 2u, "surface pixels");
    expect(std::fabs(meanDenoised - meanNoisy) <= 0.1 * meanNoisy, "denoised mean within 10 % of the noisy mean");
    expect(rmseDenoised < 0.8 * rmseNoisy, "denoised error below 0.8 x the 1-spp error");
    svgf.reset();
    nrd.reset();
    destroyRig(rig);
    return 0;
}

// --- zero allocations -------------------------------------------------------------------------------------
void hookBegin(const rg::PassContext&, const char* name, void*) { t_count = std::strncmp(name, "pathtrace.", 10) == 0; }
void hookEnd(const rg::PassContext&, const char*, void*) { t_count = false; }

int runZeroAlloc(Context& ctx, bool countAllocations) {
    constexpr u32 kW = 24;
    constexpr u32 kH = 16;
    Rig rig;
    const int rc = initRig(ctx, rig, pt_test::SceneKind::Converge, kW, kH, 0x5u);
    if (rc <= 0) {
        destroyRig(rig);
        std::printf(rc < 0 ? "FAIL: rig\n" : "SKIP: path tracer unavailable\n");
        return rc < 0 ? 1 : kSkip;
    }
    PtSettings s = pt_test::baseSettings(rig.world);
    s.samplesPerFrame = 1;
    setSettings(rig, s);
    rg::Graph graph;
    Frame f;
    f.camera = rig.world.camera;
    f.readback = false;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long begin = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, rig);
        rig.scene.commit();
        rig.as.commit();
        ctx.upload.flush();
        bool ok = rig.tree.beginFrame(ctx.serial, rig.world.tree);
        f.frameIndex = frame;
        t_allocations = 0;
        ok = beginPtFrames(ctx, rig, f, measure) && ok;
        const unsigned long long inBegin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, rig, graph, f);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        collect(ctx, rig);
        ok = ok && result.ok && waited;
        expect(ok, "frame ok");
        if (measure) {
            begin += inBegin;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    for (u32 k = 0; k < kVariants; ++k) {
        if (rig.built[k]) {
            expect(rig.pt[k].stats().passes == 1u && rig.pt[k].sampleCount() == kTotal, "one pass per frame, samples accumulate");
        }
    }
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state accumulating frames (RT pipeline + ray query, slang)\n"
                    "  PathTracerGpu::beginFrame: %llu operator-new calls; pathtrace.* callbacks: %llu; whole graph build: %llu\n",
                    kTotal - kWarmup, begin, callbacks, build);
        expect(begin == 0u && callbacks == 0u, "path tracer makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the path-tracing passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mode = "converge";
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
        if (mode == "converge") {
            rc = runConverge(ctx);
        } else if (mode == "furnace") {
            rc = runFurnace(ctx);
        } else if (mode == "determinism") {
            rc = runDeterminism(ctx);
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
