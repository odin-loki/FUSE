// WP-6.0 acceleration structures, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_rt_cpu.cpp.
//
// Scene (tiny, Lavapipe traces on the CPU): 4 WP-1.2 meshlet meshes in one GpuScene (WP-1.1: sphere,
// torus, box, and a deformable sphere) plus one bounds-only mesh without a scene index range (no BLAS),
// 56 instance slots: visible, shadow-only, transparent, one freed slot, one instance of the bounds-only
// mesh. Every frame is one render graph (WP-0.3): gpu_scene.* -> rt.* (compact, decode, BLAS build /
// refit, compaction query, instance packing, TLAS build / update) -> rt.probe (ray-query compute, two
// dispatches: cull mask all / shadow-only) -> host readback.
//
// Parity (every checked frame, per kernel language built): each probe ray against the CPU reference
// (rt_reference.hpp: two-level fuse::spatial::BVH, double-precision triangles):
//   * robust rays (the answer does not change when every triangle grows / shrinks by 1e-4 in
//     barycentric space; >= 95% of the rays) have the same (instance, primitive) and hit / miss state;
//   * every ray whose ids match has |t_gpu - t_cpu| <= 1e-5 + 1e-4 * t and barycentrics within 1e-3;
//   * custom index == instance index (== GPU-scene slot), geometry index 0.
//
//   --mode parity       GPU instance packing: build frame + two motion frames; the GPU-packed instance
//                       buffer == packRtInstance on the CPU, byte for byte; Slang == GLSL hit ids
//   --mode cpu_packing  CPU fallback packing through the UploadQueue: same parity, same frames
//   --mode refit        moving instances (TLAS UPDATE) and a deforming mesh (BLAS UPDATE in place) over
//                       8 frames, forced TLAS rebuild after maxTlasUpdates; the reference refits too;
//                       parity every frame
//   --mode compaction   static BLASes queried (deformable never): compacted size <= build size; the BLASes
//                       that shrink are copied (COMPACT), memory drops, the TLAS is rebuilt on the new
//                       addresses; forced copies (every static BLAS through COMPACT, for exact-size
//                       builders such as lavapipe, whose compacted size == build size) keep parity;
//                       compaction off keeps the build size
//   --mode zero_alloc   64 steady-state frames (motion + deformation + probe): 0 operator-new calls in
//                       rt.beginFrame / commit / importInto / probe.addPass and the rt.* pass callbacks
//                       (validated run first; validation off for the count)
//   --mode caps_gate    T2 gate on real devices: FUSE_RENDER_TIER_MAX=T1 and VulkanDeviceDesc::maxTier=T0
//                       reject (init false with the reason, no AS / ray query enabled), uncapped accepts
//
// Exit 77 = skip (stub build, no ICD / validation layer, device below T2, no kernel built).
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/rt_probe.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/renderer/vk/allocator.hpp>
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
#include <random>
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
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::rt;
using fuse::f32;
using fuse::f64;
using fuse::u16;
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
constexpr u32 kRays = 2048;          ///< per dispatch
constexpr u32 kDispatches = 2;       ///< cull mask all, shadow-only
constexpr u32 kDeformMesh = 3;
constexpr f64 kEdgeEpsilon = 1.0e-4;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
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
    UploadQueue upload;
    Buffer staging{};
    Buffer rays{};     ///< RtProbeRay[kRays * kDispatches], host-written
    Buffer hits{};     ///< RtProbeHit[kRays * kDispatches], host-read
    Buffer readback{}; ///< instance buffer copy
    Buffer deform{};   ///< deformed positions of kDeformMesh (AS build input)
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(deform);
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(hits);
            allocator->destroyBuffer(rays);
            allocator->destroyBuffer(staging);
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

/// 0 ok, kSkip, or 1. `maxTier` caps the device (caps_gate); `requireRt` skips below T2.
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
    instanceDesc.appName = "fuse_rp_rt";
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
    VulkanDeviceDesc deviceDesc{};
    deviceDesc.maxTier = maxTier;
    ctx.device = VulkanDevice::create(*ctx.instance, deviceDesc);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    const RtCapabilities caps = queryRtCapabilities(ctx.device.get());
    if (requireRt && !caps.usable) {
        std::printf("SKIP: T2 gate: %s (%s)\n", caps.reason, ctx.device->info().caps.summary().c_str());
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s; %s\n", ctx.device->info().deviceName.c_str(), ctx.device->info().caps.summary().c_str());
    if (!requireRt) {
        return 0;
    }
    auto make = [&](Buffer& out, usize size, u32 usage, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = size;
        d.usage = static_cast<BufferUsage>(usage);
        d.memoryUsage = memory;
        d.name = name;
        return ctx.allocator->createBuffer(d, out);
    };
    const u32 storage = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress);
    const usize rayBytes = static_cast<usize>(kRays) * kDispatches * sizeof(RtProbeRay);
    const usize hitBytes = static_cast<usize>(kRays) * kDispatches * sizeof(RtProbeHit);
    if (!make(ctx.staging, kStagingBytes, static_cast<u32>(BufferUsage::TransferSrc), MemoryUsage::CpuToGpu, "rp_rt.staging") ||
        !make(ctx.rays, rayBytes, storage, MemoryUsage::CpuToGpu, "rp_rt.rays") ||
        !make(ctx.hits, hitBytes, storage, MemoryUsage::GpuToCpu, "rp_rt.hits") ||
        !make(ctx.readback, 1u << 20, static_cast<u32>(BufferUsage::TransferDst), MemoryUsage::GpuToCpu, "rp_rt.readback") ||
        !make(ctx.deform, 1u << 20,
              storage | static_cast<u32>(BufferUsage::TransferDst) | static_cast<u32>(BufferUsage::AccelerationStructureBuildInput),
              MemoryUsage::GpuOnly, "rp_rt.deform") ||
        ctx.staging.mapped == nullptr || ctx.rays.mapped == nullptr || ctx.hits.mapped == nullptr || ctx.readback.mapped == nullptr ||
        ctx.rays.deviceAddress == 0u || ctx.hits.deviceAddress == 0u || ctx.deform.deviceAddress == 0u) {
        std::fprintf(stderr, "FAIL: test buffers\n");
        return 1;
    }
    if (!ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

// --- scene ----------------------------------------------------------------------------------------
GpuTransform place(f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz, f32 yaw) {
    GpuTransform t{};
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    t.rows[0][0] = c * sx;
    t.rows[0][2] = s * sz;
    t.rows[1][1] = sy;
    t.rows[2][0] = -s * sx;
    t.rows[2][2] = c * sz;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers; ///< indices into handles
    std::vector<f32> restPositions; ///< decoded kDeformMesh positions
    std::vector<f32> deformed;
    RtReferenceScene ref;
};

bool buildScene(Context& ctx, Scene& s) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.instanceCapacity = 64;
    d.meshCapacity = 16;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    s.gpu.beginFrame(ctx.serial);
    const vis_test::SourceMesh sources[4] = {vis_test::uvSphere(12, 16, 1.f), vis_test::torus(24, 12, 1.f, 0.35f), vis_test::box(),
                                             vis_test::uvSphere(8, 12, 0.8f)};
    s.meshes.resize(4);
    for (u32 i = 0; i < 4u; ++i) {
        if (!vis_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    GpuMesh boundsOnly{}; // mesh 4: no index range -> no BLAS; its instances are inactive
    boundsOnly.vertexCount = 3;
    boundsOnly.triangleCount = 1;
    boundsOnly.boundsRadius = 1.f;
    if (s.gpu.addMesh(boundsOnly) != 4u) {
        return false;
    }
    for (u32 i = 0; i < 4u; ++i) {
        if (!s.ref.setMeshFromScene(s.gpu, i, s.meshes[i].positions.data())) {
            return false;
        }
    }
    s.restPositions = s.ref.meshPositions(kDeformMesh);
    std::mt19937 rng(6060);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 i = 0; i < 56u; ++i) {
        InstanceDesc id{};
        id.mesh = i % 4u;
        if (i % 11u == 5u) {
            id.flags = kInstanceCastShadow; // shadow only
        } else if (i % 13u == 7u) {
            id.flags = kInstanceVisible | kInstanceCastShadow | kInstanceTransparent;
        }
        if (i == 50u) {
            id.mesh = 4u; // bounds-only mesh
        }
        const f32 sc = 0.4f + 0.3f * (u(rng) * 0.5f + 0.5f);
        id.transform = place(u(rng) * 7.f, u(rng) * 3.5f, -11.f + u(rng) * 5.f, sc, sc * (0.8f + 0.2f * u(rng)), sc, u(rng) * 3.f);
        s.handles.push_back(s.gpu.addInstance(id));
        if (i % 3u == 1u) {
            s.movers.push_back(i);
        }
    }
    s.gpu.removeInstance(s.handles[20]); // a free slot inside the high-water mark
    s.movers.erase(std::remove(s.movers.begin(), s.movers.end(), 20u), s.movers.end());
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

void moveObjects(Scene& s, u32 frame) {
    for (u32 k = 0; k < s.movers.size(); ++k) {
        const u32 i = s.movers[k];
        GpuTransform t = s.gpu.transform(i);
        t.rows[0][3] += 0.11f * std::sin(static_cast<f32>(frame + k));
        t.rows[1][3] += 0.08f * std::cos(0.9f * static_cast<f32>(frame) + static_cast<f32>(k)); // oscillates: stays in view
        t.rows[2][3] += 0.03f * std::cos(static_cast<f32>(frame * 3u + k));
        s.gpu.setTransform(s.handles[i], t);
    }
}

/// New positions of kDeformMesh (same topology), uploaded into ctx.deform; the reference refits.
bool deform(Context& ctx, Scene& s, AccelerationStructures& rt, u32 frame) {
    s.deformed = s.restPositions;
    const f32 phase = 0.7f * static_cast<f32>(frame);
    for (usize v = 0; v < s.deformed.size(); v += 3u) {
        const f32 y = s.restPositions[v + 1];
        const f32 k = 1.f + 0.25f * std::sin(phase + 3.f * y);
        s.deformed[v + 0] *= k;
        s.deformed[v + 2] *= k;
        s.deformed[v + 1] += 0.1f * std::cos(phase);
    }
    const usize bytes = s.deformed.size() * sizeof(f32);
    usize ring = 0;
    if (!ctx.upload.stage(s.deformed.data(), bytes, ring) || !ctx.upload.recordBufferCopy(ctx.deform.handle, ring, 0, bytes)) {
        return false;
    }
    return rt.deformMesh(kDeformMesh, ctx.deform) &&
           s.ref.updateMeshPositions(kDeformMesh, s.deformed.data(), static_cast<u32>(s.deformed.size() / 3u));
}

void makeRays(Context& ctx, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    RtProbeRay* rays = static_cast<RtProbeRay*>(ctx.rays.mapped);
    for (u32 i = 0; i < kRays * kDispatches; ++i) {
        RtProbeRay& r = rays[i];
        r.origin[0] = u(rng) * 3.f;
        r.origin[1] = u(rng) * 2.f;
        r.origin[2] = 3.f + u(rng);
        const f32 target[3] = {u(rng) * 7.5f, u(rng) * 4.f, -11.f + u(rng) * 5.5f};
        f32 d[3] = {target[0] - r.origin[0], target[1] - r.origin[1], target[2] - r.origin[2]};
        const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (u32 c = 0; c < 3u; ++c) {
            r.direction[c] = d[c] / len;
        }
        r.tMin = (i % 5u == 0u) ? 0.5f + 8.f * (u(rng) * 0.5f + 0.5f) : 0.f; // some rays start inside the volume
        r.tMax = (i % 7u == 0u) ? 9.f + 4.f * u(rng) : 100.f;                  // some rays end inside it
    }
}

// --- frame ----------------------------------------------------------------------------------------
struct CopyJob {
    void* src = nullptr;
    void* dst = nullptr;
    u64 size = 0;
};

void recordCopy(const rg::PassContext& context, void* user) {
    const CopyJob& job = *static_cast<const CopyJob*>(user);
    if (job.size == 0u) {
        return;
    }
    VkBufferCopy region{0, 0, job.size};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(context.commandBuffer), static_cast<VkBuffer>(job.src),
                    static_cast<VkBuffer>(job.dst), 1, &region);
}

struct FrameIo {
    rg::Graph graph;
    CopyJob copy{};
    bool readInstances = false;
    bool probe = true;
    RtCommitStats stats{};
};

void beginSceneFrame(Context& ctx, Scene& s, AccelerationStructures& rt) {
    ++ctx.serial;
    s.gpu.beginFrame(ctx.serial);
    rt.beginFrame(ctx.serial);
}

/// Graph of one frame (after scene.commit / rt.commit / upload.flush).
void buildGraph(Context& ctx, Scene& s, AccelerationStructures& rt, RtProbe& probe, FrameIo& io) {
    rg::Graph& graph = io.graph;
    graph.reset();
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const RtGraphRefs rtRefs = rt.importInto(graph, sceneRefs);
    probe.beginFrame();
    rg::BufferRef hits{};
    if (io.probe) {
        const rg::BufferRef rays =
            graph.importBuffer(rg::ImportedBuffer{ctx.rays.handle, ctx.rays.desc.size, rg::kNoQueue, nullptr, "rp_rt.rays"});
        hits = graph.importBuffer(rg::ImportedBuffer{ctx.hits.handle, ctx.hits.desc.size, rg::kNoQueue, nullptr, "rp_rt.hits"});
        for (u32 d = 0; d < kDispatches; ++d) {
            RtProbeDispatch pd{};
            pd.tlas = rtRefs.tlas;
            pd.tlasAddress = rt.tlasAddress();
            pd.rays = rays;
            pd.raysAddress = ctx.rays.deviceAddress + static_cast<u64>(d) * kRays * sizeof(RtProbeRay);
            pd.hits = hits;
            pd.hitsAddress = ctx.hits.deviceAddress + static_cast<u64>(d) * kRays * sizeof(RtProbeHit);
            pd.count = kRays;
            pd.cullMask = d == 0u ? kRtMaskAll : kRtMaskShadow;
            expect(probe.addPass(graph, pd), "probe.addPass");
        }
    }
    rg::BufferRef readback{};
    if (io.readInstances) {
        readback = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr, "rp_rt.rb"});
        io.copy = CopyJob{rt.instanceBuffer().handle, ctx.readback.handle,
                          std::min<u64>(static_cast<u64>(rt.tlasInstanceCount()) * sizeof(AsInstance), ctx.readback.desc.size)};
        graph.addPass("test.copy_instances", &recordCopy, &io.copy)
            .use(rtRefs.instances, rg::Access::TransferSrc)
            .use(readback, rg::Access::TransferDst);
    }
    if (hits.valid() || readback.valid()) {
        rg::PassBuilder host = graph.addPass("test.host", nullptr, nullptr);
        if (hits.valid()) {
            host.use(hits, rg::Access::HostRead);
        }
        if (readback.valid()) {
            host.use(readback, rg::Access::HostRead);
        }
    }
}

bool runFrame(Context& ctx, Scene& s, AccelerationStructures& rt, RtProbe& probe, FrameIo& io) {
    const GpuSceneCommitStats sceneStats = s.gpu.commit();
    io.stats = rt.commit();
    ctx.upload.flush();
    buildGraph(ctx, s, rt, probe, io);
    const rg::ExecuteResult result = ctx.executor->execute(io.graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    s.gpu.collectRetired(ctx.serial);
    rt.collectRetired(ctx.serial);
    return sceneStats.ok && io.stats.ok && result.ok && waited;
}

// --- parity -----------------------------------------------------------------------------------------
struct Parity {
    u32 rays = 0;
    u32 hits = 0;
    u32 robust = 0;
    u32 robustMismatch = 0;
    u32 idMatch = 0;
    u32 tBad = 0;
    u32 baryBad = 0;
    u32 customBad = 0;
    f64 maxTError = 0.0;
};

Parity checkParity(Context& ctx, const Scene& s, const char* label) {
    Parity p{};
    const RtProbeRay* rays = static_cast<const RtProbeRay*>(ctx.rays.mapped);
    const RtProbeHit* hits = static_cast<const RtProbeHit*>(ctx.hits.mapped);
    for (u32 i = 0; i < kRays * kDispatches; ++i) {
        const u32 mask = i < kRays ? kRtMaskAll : kRtMaskShadow;
        const RtRefClassified c = s.ref.traceClassified(rays[i], mask, kEdgeEpsilon);
        const RtProbeHit& g = hits[i];
        const bool gHit = (g.flags & kRtHit) != 0u;
        const bool same = gHit == c.hit.hit && (!gHit || (g.instance == c.hit.instance && g.primitive == c.hit.primitive));
        ++p.rays;
        p.hits += gHit ? 1u : 0u;
        p.robust += c.robust ? 1u : 0u;
        if (c.robust && !same) {
            ++p.robustMismatch;
            if (p.robustMismatch <= 5u) {
                std::fprintf(stderr, "  %s ray %u: gpu (%d inst %u prim %u t %.6f) cpu (%d inst %u prim %u t %.6f)\n", label, i, gHit ? 1 : 0,
                             g.instance, g.primitive, static_cast<f64>(g.t), c.hit.hit ? 1 : 0, c.hit.instance, c.hit.primitive, c.hit.t);
            }
        }
        if (same) {
            ++p.idMatch;
            if (gHit) {
                const f64 err = std::abs(static_cast<f64>(g.t) - c.hit.t);
                p.maxTError = std::max(p.maxTError, err);
                p.tBad += err > rtTTolerance(c.hit.t) ? 1u : 0u;
                p.baryBad += (std::abs(static_cast<f64>(g.u) - c.hit.u) > 1e-3 || std::abs(static_cast<f64>(g.v) - c.hit.v) > 1e-3) ? 1u : 0u;
                p.customBad += (g.customIndex != g.instance || g.geometry != 0u) ? 1u : 0u;
            }
        }
    }
    std::printf("  %-26s %u rays, %u hits, %u robust, %u robust mismatches, %u id matches, %u t outside tol "
                "(max |dt| %.3g), %u bary, %u custom\n",
                label, p.rays, p.hits, p.robust, p.robustMismatch, p.idMatch, p.tBad, p.maxTError, p.baryBad, p.customBad);
    expect(p.robustMismatch == 0u, "robust rays: GPU hit ids == CPU BVH reference");
    expect(p.tBad == 0u, "t within tolerance on every matching hit");
    expect(p.baryBad == 0u, "barycentrics within 1e-3");
    expect(p.customBad == 0u, "custom index == instance index, geometry 0");
    expect(p.robust * 100u >= p.rays * 95u, ">= 95% robust rays");
    expect(p.hits * 5u >= p.rays && p.hits * 20u <= p.rays * 19u, "rays both hit and miss");
    return p;
}

bool checkPacking(Context& ctx, const Scene& s, const AccelerationStructures& rt) {
    const AsInstance* gpu = static_cast<const AsInstance*>(ctx.readback.mapped);
    const u32 n = rt.tlasInstanceCount();
    u32 diff = 0, active = 0;
    for (u32 i = 0; i < n; ++i) {
        const AsInstance cpu =
            packRtInstance(s.gpu.instance(i), s.gpu.transform(i), i, rt.blasAddresses(), rt.blasCount(), rt.inactiveBlasAddress(),
                           rt.inactiveMask());
        diff += std::memcmp(&cpu, &gpu[i], sizeof(AsInstance)) != 0 ? 1u : 0u;
        active += ((cpu.customIndexMask >> 24) & kRtMaskAll) != 0u ? 1u : 0u;
    }
    std::printf("  instance buffer: %u slots (%u active), %u differ from packRtInstance\n", n, active, diff);
    expect(diff == 0u, "instance buffer == packRtInstance (byte for byte)");
    expect(active > 0u && active < n, "active and inactive slots");
    return diff == 0u;
}

// --- modes ------------------------------------------------------------------------------------------
struct Rt {
    AccelerationStructures as;
    RtProbe probe;
};

int initRt(Context& ctx, Scene& s, Rt& rt, RtInstancePacking packing, RtKernelLanguage language, bool compaction = true,
           u32 maxTlasUpdates = 64, bool forceCompactionCopy = false) {
    RtMeshOptions deformable{};
    deformable.deformable = true;
    rt.as.setMeshOptions(kDeformMesh, deformable);
    AccelerationStructuresDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.scene = &s.gpu;
    d.packing = packing;
    d.language = language;
    d.compaction = compaction;
    d.maxTlasUpdates = maxTlasUpdates;
    d.forceCompactionCopy = forceCompactionCopy;
    d.instanceCapacity = 64;
    d.meshCapacity = 16;
    if (!rt.as.init(d)) {
        std::printf("  rt init: %s\n", rt.as.reason());
        return kSkip;
    }
    if (!rt.probe.init(ctx.device.get(), language)) {
        std::printf("  probe init: %s\n", rt.probe.reason());
        return kSkip;
    }
    return 0;
}

int runParity(Context& ctx, RtInstancePacking packing) {
    const bool gpuPacking = packing != RtInstancePacking::Cpu;
    std::vector<std::vector<RtProbeHit>> runs;
    std::vector<const char*> names;
    for (RtKernelLanguage language : {RtKernelLanguage::Slang, RtKernelLanguage::Glsl}) {
        Scene s;
        if (!buildScene(ctx, s)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        s.ref.setInstances(s.gpu);
        Rt rt;
        const int rc = initRt(ctx, s, rt, packing, language);
        if (rc == kSkip) {
            continue;
        }
        if (std::strcmp(rt.as.kernelLanguage(), rt.probe.kernelLanguage()) != 0 ||
            std::strcmp(rt.as.kernelLanguage(), language == RtKernelLanguage::Slang ? "slang" : "glsl") != 0) {
            continue; // Auto fell back to the other language: that run is covered by its own iteration
        }
        std::printf("kernels: %s, packing: %s, dead slots: %s\n", rt.as.kernelLanguage(), rt.as.packingName(),
                    rt.as.capabilities().inactiveInstanceQuirk ? "placeholder BLAS + kRtMaskDead (driver quirk)" : "reference 0");
        expect(std::strcmp(rt.as.packingName(), gpuPacking ? "gpu" : "cpu") == 0, "packing path");
        makeRays(ctx, 99);
        FrameIo io;
        for (u32 frame = 0; frame < 3u; ++frame) {
            beginSceneFrame(ctx, s, rt.as);
            if (frame > 0u) {
                moveObjects(s, frame);
            }
            io.readInstances = true;
            if (!runFrame(ctx, s, rt.as, rt.probe, io)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            if (frame == 0u) {
                expect(io.stats.blasBuilds == 4u && io.stats.tlas == TlasBuildMode::Build, "frame 0 builds 4 BLASes + the TLAS");
                expect(rt.as.blas(4).state == BlasState::Unsupported, "bounds-only mesh has no BLAS");
            } else {
                expect(io.stats.tlas != TlasBuildMode::None, "motion rebuilds / refits the TLAS");
                s.ref.updateInstances(s.gpu);
            }
            expect(io.stats.cpuPacked == !gpuPacking, "CPU packing only on the fallback path");
            char label[64];
            std::snprintf(label, sizeof(label), "%s frame %u (%s)", rt.as.kernelLanguage(), frame,
                          io.stats.tlas == TlasBuildMode::Build ? "build" : io.stats.tlas == TlasBuildMode::Update ? "update" : "none");
            checkParity(ctx, s, label);
            checkPacking(ctx, s, rt.as);
        }
        const RtProbeHit* hits = static_cast<const RtProbeHit*>(ctx.hits.mapped);
        runs.emplace_back(hits, hits + kRays * kDispatches);
        names.push_back(rt.as.kernelLanguage());
        rt.probe.destroy();
        rt.as.destroy();
        s.gpu.destroy();
    }
    if (runs.empty()) {
        std::printf("SKIP: no rt kernels built\n");
        return kSkip;
    }
    if (runs.size() == 2u) {
        u32 idDiff = 0, bitDiff = 0;
        for (u32 i = 0; i < runs[0].size(); ++i) {
            const RtProbeHit& a = runs[0][i];
            const RtProbeHit& b = runs[1][i];
            idDiff += (a.flags != b.flags || a.instance != b.instance || a.primitive != b.primitive) ? 1u : 0u;
            bitDiff += std::memcmp(&a, &b, sizeof(a)) != 0 ? 1u : 0u;
        }
        std::printf("  %s vs %s: %u id differences, %u bitwise differences\n", names[0], names[1], idDiff, bitDiff);
        expect(idDiff == 0u, "Slang == GLSL hit ids");
    }
    return 0;
}

int runRefit(Context& ctx) {
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    s.ref.setInstances(s.gpu);
    Rt rt;
    constexpr u32 kMaxUpdates = 3;
    if (initRt(ctx, s, rt, RtInstancePacking::Auto, RtKernelLanguage::Auto, true, kMaxUpdates) != 0) {
        std::printf("SKIP: rt unavailable\n");
        return kSkip;
    }
    makeRays(ctx, 1234);
    FrameIo io;
    u32 tlasUpdates = 0, tlasBuilds = 0, blasUpdates = 0, refRefits = 0;
    for (u32 frame = 0; frame < 8u; ++frame) {
        beginSceneFrame(ctx, s, rt.as);
        if (frame > 0u) {
            moveObjects(s, frame);
            if (!deform(ctx, s, rt.as, frame)) {
                std::fprintf(stderr, "FAIL: deform\n");
                return 1;
            }
        }
        if (!runFrame(ctx, s, rt.as, rt.probe, io)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
        if (frame > 0u) {
            refRefits += s.ref.updateInstances(s.gpu) ? 1u : 0u;
            tlasUpdates += io.stats.tlas == TlasBuildMode::Update ? 1u : 0u;
            tlasBuilds += io.stats.tlas == TlasBuildMode::Build ? 1u : 0u;
            blasUpdates += io.stats.blasUpdates;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "frame %u (TLAS %s, %u BLAS refit)", frame,
                      io.stats.tlas == TlasBuildMode::Build ? "build" : io.stats.tlas == TlasBuildMode::Update ? "update" : "none",
                      io.stats.blasUpdates);
        checkParity(ctx, s, label);
    }
    std::printf("refit: %u TLAS updates, %u forced TLAS rebuilds, %u BLAS refits (deformable mesh: %u refits recorded), "
                "reference top-level refits %u\n",
                tlasUpdates, tlasBuilds, blasUpdates, rt.as.blas(kDeformMesh).updates, refRefits);
    // frames 1..7 move + deform: 3 updates, then a forced rebuild (maxTlasUpdates = 3), and so on.
    expect(tlasUpdates == 6u && tlasBuilds == 1u, "TLAS refit 3x then rebuilt (maxTlasUpdates)");
    expect(blasUpdates == 7u && rt.as.blas(kDeformMesh).updates == 7u, "deformable BLAS refit in place every frame");
    expect(rt.as.blas(kDeformMesh).deformable && rt.as.blas(kDeformMesh).state == BlasState::Built, "deformable BLAS never compacted");
    expect(refRefits == 7u, "reference refits (same slot set)");
    rt.probe.destroy();
    rt.as.destroy();
    s.gpu.destroy();
    return 0;
}

int runCompaction(Context& ctx) {
    // Pass 0: compaction as shipped. Pass 1: forced copies (the copy / repoint / TLAS-rebuild path on
    // drivers whose compacted size equals the build size). Pass 2: compaction off.
    for (u32 pass = 0; pass < 3u; ++pass) {
        const bool compaction = pass != 2u;
        const bool force = pass == 1u;
        const char* name = pass == 0u ? "on" : pass == 1u ? "forced copy" : "off";
        Scene s;
        if (!buildScene(ctx, s)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        s.ref.setInstances(s.gpu);
        Rt rt;
        if (initRt(ctx, s, rt, RtInstancePacking::Auto, RtKernelLanguage::Auto, compaction, 64, force) != 0) {
            std::printf("SKIP: rt unavailable\n");
            return kSkip;
        }
        makeRays(ctx, 777);
        FrameIo io;
        RtMemoryStats before{};
        u32 copies = 0;
        for (u32 frame = 0; frame < 3u; ++frame) {
            beginSceneFrame(ctx, s, rt.as);
            if (!runFrame(ctx, s, rt.as, rt.probe, io)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            if (frame == 0u) {
                before = rt.as.memory();
                expect(io.stats.compactionQueries == (compaction ? 3u : 0u), "compaction queries for the 3 static BLASes only");
            }
            copies += io.stats.blasCompactions;
            if (io.stats.blasCompactions > 0u) {
                expect(io.stats.tlas == TlasBuildMode::Build, "TLAS rebuilt on the compacted BLAS addresses");
            }
            char label[64];
            std::snprintf(label, sizeof(label), "compaction %s frame %u", name, frame);
            checkParity(ctx, s, label);
        }
        const RtMemoryStats after = rt.as.memory();
        std::printf("  compaction %s: BLAS bytes %llu -> %llu (build size %llu), %u of %u compacted, %u copies\n", name,
                    static_cast<unsigned long long>(before.blasBytes), static_cast<unsigned long long>(after.blasBytes),
                    static_cast<unsigned long long>(after.blasBuildBytes), after.compactedCount, after.blasCount, copies);
        u32 smaller = 0;
        for (u32 m = 0; m < 4u; ++m) {
            const BlasInfo& b = rt.as.blas(m);
            std::printf("    mesh %u: %u tris, build %llu, compacted query %llu, now %llu (%s%s)\n", m, b.triangles,
                        static_cast<unsigned long long>(b.buildSize), static_cast<unsigned long long>(b.compactedSize),
                        static_cast<unsigned long long>(b.size), b.state == BlasState::Compacted ? "compacted" : "build size",
                        b.deformable ? ", deformable" : "");
            if (m < 3u && compaction) {
                expect(b.compactedSize != 0u && b.compactedSize <= b.buildSize, "compacted size queried and <= build size");
                smaller += b.compactedSize < b.buildSize ? 1u : 0u;
            }
            if (m == kDeformMesh) {
                expect(b.compactedSize == 0u && b.state != BlasState::Compacted, "deformable BLAS is never compacted");
            }
        }
        if (pass == 0u) {
            expect(copies == smaller && after.compactedCount == smaller, "exactly the BLASes that shrink are copied");
            if (smaller > 0u) {
                expect(after.blasBytes < before.blasBytes, "compaction reduces BLAS memory");
                std::printf("  compaction saves %.1f%% of BLAS memory\n",
                            100.0 * static_cast<f64>(before.blasBytes - after.blasBytes) / static_cast<f64>(before.blasBytes));
            } else {
                std::printf("  NOTE: the driver's compacted sizes equal its build sizes (exact-size builder): nothing to "
                            "reclaim here; the size reduction is only measurable on drivers that over-allocate builds\n");
            }
        } else if (pass == 1u) {
            expect(copies == 3u && after.compactedCount == 3u, "forced: every static BLAS copied through COMPACT");
            expect(after.blasBytes <= before.blasBytes, "compaction never grows BLAS memory");
        } else {
            expect(copies == 0u && after.compactedCount == 0u && after.blasBytes == before.blasBytes, "compaction off keeps the build size");
        }
        rt.probe.destroy();
        rt.as.destroy();
        s.gpu.destroy();
    }
    return 0;
}

// --- zero_alloc -------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "rt.", 3) == 0) {
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
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    s.ref.setInstances(s.gpu);
    Rt rt;
    if (initRt(ctx, s, rt, RtInstancePacking::Auto, RtKernelLanguage::Auto) != 0) {
        std::printf("SKIP: rt unavailable\n");
        return kSkip;
    }
    makeRays(ctx, 4242);
    FrameIo io;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long rtSide = 0, callbacks = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 updates = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool last = frame + 1u == kTotal;
        const bool measure = countAllocations && frame >= kWarmup && !last;
        ++ctx.serial;
        s.gpu.beginFrame(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        rt.as.beginFrame(ctx.serial);
        t_count = false;
        unsigned long long frameAllocs = t_allocations;
        if (frame > 0u) {
            moveObjects(s, frame);
            if (!deform(ctx, s, rt.as, frame)) {
                std::fprintf(stderr, "FAIL: deform\n");
                return 1;
            }
        }
        s.gpu.commit();
        t_allocations = 0;
        t_count = measure;
        io.stats = rt.as.commit();
        t_count = false;
        frameAllocs += t_allocations;
        ctx.upload.flush();
        // Graph build: the rt part (importInto + probe.addPass) is counted; the test's own imports
        // and the scene import are measured together with it (the graph reuses its capacity).
        io.probe = true;
        io.readInstances = false;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, s, rt.as, rt.probe, io);
        t_count = false;
        frameAllocs += t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(io.graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        s.gpu.collectRetired(ctx.serial);
        rt.as.collectRetired(ctx.serial);
        expect(io.stats.ok && result.ok && waited, "frame ok");
        updates += io.stats.blasUpdates;
        if (measure) {
            rtSide += frameAllocs + inCallbacks;
            callbacks += inCallbacks;
        }
        if (last) {
            s.ref.updateInstances(s.gpu);
            checkParity(ctx, s, "zero_alloc last frame");
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances moving, 1 mesh deforming, %u BLAS refits, probe %u rays)\n"
                    "  rt.beginFrame / commit / graph build (importInto + probe.addPass) + rt.* callbacks: %llu operator-new "
                    "calls (callbacks %llu)\n",
                    kTotal - kWarmup - 1u, static_cast<u32>(s.movers.size()), updates, kRays * kDispatches, rtSide, callbacks);
        expect(rtSide == 0u, "acceleration structures make no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u BLAS refits\n", updates);
    }
    rt.probe.destroy();
    rt.as.destroy();
    s.gpu.destroy();
    return 0;
}

// --- caps_gate --------------------------------------------------------------------------------------
int runCapsGate() {
    struct Case {
        const char* label;
        const char* env; ///< FUSE_RENDER_TIER_MAX ("" = unset)
        RenderTier maxTier;
        bool expectUsable;
    };
    const Case cases[] = {{"uncapped", "", kMaxRenderTier, true},
                          {"FUSE_RENDER_TIER_MAX=T1", "T1", kMaxRenderTier, false},
                          {"VulkanDeviceDesc::maxTier=T0", "", RenderTier::T0, false}};
    bool sawT2 = false;
    for (const Case& c : cases) {
        setenv("FUSE_RENDER_TIER_MAX", c.env, 1);
        Context ctx;
        const int rc = setup(ctx, true, c.maxTier, false);
        if (rc != 0) {
            setenv("FUSE_RENDER_TIER_MAX", "", 1);
            return rc;
        }
        const RendererCaps& caps = ctx.device->info().caps;
        const RtCapabilities rtCaps = queryRtCapabilities(ctx.device.get());
        if (c.expectUsable && !caps.supports(RenderFeature::RayQuery)) {
            std::printf("SKIP: the device does not support ray queries (hardware %s)\n", renderTierName(caps.hardwareTier));
            setenv("FUSE_RENDER_TIER_MAX", "", 1);
            return kSkip;
        }
        sawT2 = sawT2 || rtCaps.usable;
        AccelerationStructures as;
        RtProbe probe;
        GpuScene scene;
        GpuSceneDesc sd{};
        sd.device = ctx.device.get();
        sd.allocator = ctx.allocator.get();
        UploadQueue upload;
        Buffer staging{};
        BufferDesc bd{};
        bd.size = 1u << 20;
        bd.usage = BufferUsage::TransferSrc;
        bd.memoryUsage = MemoryUsage::CpuToGpu;
        ctx.allocator->createBuffer(bd, staging);
        upload.init(ctx.device.get(), staging.handle, staging.mapped, bd.size);
        sd.upload = &upload;
        scene.init(sd);
        AccelerationStructuresDesc d{};
        d.device = ctx.device.get();
        d.allocator = ctx.allocator.get();
        d.upload = &upload;
        d.scene = &scene;
        const bool asOk = as.init(d);
        const bool probeOk = probe.init(ctx.device.get());
        std::printf("  %-30s tier %s (hw %s): AS %d ray query %d -> gate %s (%s, fallback %s); init AS %d probe %d%s%s\n", c.label,
                    renderTierName(caps.tier), renderTierName(caps.hardwareTier), caps.accelerationStructure ? 1 : 0,
                    caps.rayQuery ? 1 : 0, rtCaps.usable ? "usable" : "rejected", rtCaps.reason, rtCaps.fallback, asOk ? 1 : 0,
                    probeOk ? 1 : 0, asOk ? "" : ": ", asOk ? "" : as.reason());
        expect(rtCaps.usable == c.expectUsable, "T2 gate decision");
        expect(asOk == c.expectUsable && probeOk == c.expectUsable, "init follows the gate");
        expect(caps.accelerationStructure == c.expectUsable && caps.rayQuery == c.expectUsable,
               "AS / ray query enabled exactly when the tier allows it");
        if (!c.expectUsable) {
            expect(std::strcmp(rtCaps.fallback, "global-sdf") == 0, "T0 fallback named");
            expect(as.commit().ok == false && as.tlasAddress() == 0u, "a rejected builder does nothing");
        }
        probe.destroy();
        as.destroy();
        scene.destroy();
        upload.destroy();
        ctx.allocator->destroyBuffer(staging);
    }
    setenv("FUSE_RENDER_TIER_MAX", "", 1);
    expect(sawT2, "an uncapped Lavapipe device passes the T2 gate");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
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
        if (mode == "parity") {
            rc = runParity(ctx, RtInstancePacking::Gpu);
        } else if (mode == "cpu_packing") {
            rc = runParity(ctx, RtInstancePacking::Cpu);
        } else if (mode == "refit") {
            rc = runRefit(ctx);
        } else if (mode == "compaction") {
            rc = runCompaction(ctx);
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
