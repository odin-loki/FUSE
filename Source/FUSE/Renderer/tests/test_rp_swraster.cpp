// WP-5.4 compute software rasteriser, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_swraster_cpu.cpp.
//
// Scene: micro-triangle heavy (dense spheres / tori far away, 1-3 px triangles), a few mid-distance
// and near instances (HW by size / extent), one crossing the near plane (HW clip) and 3 occluder slabs,
// 256x192, 5 frames of camera + object motion. Every frame is one render graph (WP-0.3) with five rigs
// on the SAME GpuScene (WP-1.1), each an InstanceCuller (WP-1.3) + an Atomic64 VisBuffer (WP-1.4):
//   P  SwRasterizer ForceSoftware, HW draws skipped   (parity: only software words in the target)
//   S  SwRasterizer ForceSoftware                    (every SW-safe cluster in software)
//   H  SwRasterizer ForceHardware                    (the same clusters in hardware)
//   M  SwRasterizer Classify (by triangle size)       (the mixed frame; + vis.decode)
//   R  VisBuffer::addCulledFrame                     (the WP-1.4 indirect path, whole meshes)
//
//   --mode atomic_image    R64_UINT image target (skip without image int64 atomics)
//   --mode atomic_buffer   u64 buffer target (skip without buffer int64 atomics)
//     checked every frame, for each kernel language built (Slang, GLSL):
//       parity    P: per-meshlet SwResult of every classify record == the CPU reference kernel
//                 (swraster_kernel.hpp) on the read-back records, exactly; the SW list == the meshlets
//                 classified Software; the 64-bit target == the CPU reference rasteriser over the
//                 read-back SW lists BIT FOR BIT; demotions equal (0)
//       sw_vs_hw  S vs H (same cluster set, SW vs HW raster): (instance, triangle) agree on >= 99.9%
//                 of all pixels and of covered pixels; every disagreement is classified (exact edge hit
//                 = a centre on an edge of either snapped triangle, near edge = within 2 sub-pixels,
//                 near depth = both cover the centre within 8 depth quanta) and none is unexplained
//       mixed     M draws both SW and HW clusters; its exported image decodes (GPU vis.decode == CPU
//                 decode kernel) with every covered pixel Ok, decoded depth within 64 quanta of the
//                 word's and barycentrics inside the triangle up to the snapping tolerance; M agrees
//                 with H and with the WP-1.4 path R on >= 99.9% of pixels; H vs R reported
//       counters  0 overflow, 0 skipped, 0 oversize, 0 demoted in every rig
//     and Slang == GLSL (identical 64-bit targets of every rig).
//   --mode zero_alloc      64 steady-state frames (rig M, ~1% moved): 0 operator-new calls in
//                          SwRasterizer / VisBuffer / culler beginFrame + the graph build + the
//                          swraster.* / vis.* / cull.* pass callbacks (validated run first; validation
//                          off for the count)
//   --backend set | buffer bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/swraster/swraster.hpp>
#include <fuse/renderer/swraster/swraster_reference.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>
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
using namespace fuse::renderer::culling;
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::swraster;
using namespace fuse::renderer::visbuffer;
using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::s64;
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
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kFrames = 5;
constexpr u32 kCapacity = 16384;  ///< clusters / records per region
constexpr u32 kRecordsRead = 1024; ///< classify records read back per region (the scene needs far fewer)
constexpr u64 kReadbackBytes = 12u * 1024u * 1024u;

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

// --- math (column-major, Vulkan clip space, forward depth) ----------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

Mat4 lookAt(const f32 eye[3], const f32 at[3]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& v : s) {
        v /= sl;
    }
    const f32 u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -f[0];
    v.m[6] = -f[1];
    v.m[10] = -f[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    v.m[15] = 1.f;
    return v;
}

Mat4 frameView(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    const f32 eye[3] = {-0.8f + 0.35f * t, 0.3f - 0.08f * t, 2.f};
    const f32 at[3] = {0.3f * t - 0.6f, 0.f, -30.f};
    return mul(perspective(1.1f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 150.f), lookAt(eye, at));
}

GpuTransform place(f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz, f32 yaw) {
    GpuTransform t{};
    const f32 c = std::cos(yaw);
    const f32 n = std::sin(yaw);
    t.rows[0][0] = c * sx;
    t.rows[0][2] = n * sz;
    t.rows[1][1] = sy;
    t.rows[2][0] = -n * sx;
    t.rows[2][2] = c * sz;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
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
    Buffer decodeOut{};
    u8 decodeQueue = rg::kNoQueue;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(decodeOut);
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
    instanceDesc.appName = "fuse_rp_swraster";
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
    ctx.device = VulkanDevice::create(*ctx.instance, deviceDesc);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    if (descriptorBuffer && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const SwRasterCapabilities sw = querySwRasterCapabilities(ctx.device.get());
    if (!sw.usable) {
        std::printf("SKIP: software rasteriser unsupported: %s\n", sw.reason);
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
    stagingDesc.name = "rp_swraster.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = kReadbackBytes;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_swraster.readback";
    BufferDesc decodeDesc{};
    decodeDesc.size = kPixels * sizeof(VisDecodeTexel);
    decodeDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::TransferSrc) |
                                                static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    decodeDesc.memoryUsage = MemoryUsage::GpuOnly;
    decodeDesc.name = "rp_swraster.decode";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr ||
        !ctx.allocator->createBuffer(decodeDesc, ctx.decodeOut) || ctx.decodeOut.deviceAddress == 0u) {
        std::fprintf(stderr, "FAIL: buffers\n");
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
struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<decode_kernel::MeshPositions> positions;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
    SwSceneStorage storage;
};

bool buildScene(Context& ctx, Scene& s) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 1024;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    const vis_test::SourceMesh sources[3] = {vis_test::uvSphere(20, 32, 1.f), vis_test::torus(32, 16, 1.f, 0.35f), vis_test::box()};
    s.meshes.resize(3);
    for (u32 i = 0; i < 3u; ++i) {
        if (!vis_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    for (const geometry::MeshletMesh& m : s.meshes) {
        s.positions.push_back(decode_kernel::MeshPositions{m.positions.data(), m.vertex_count()});
    }
    std::mt19937 rng(5454);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    auto add = [&](u32 mesh, const GpuTransform& xf, bool mover) {
        InstanceDesc id{};
        id.mesh = mesh;
        id.transform = xf;
        s.handles.push_back(s.gpu.addInstance(id));
        if (mover) {
            s.movers.push_back(static_cast<u32>(s.handles.size() - 1u));
        }
    };
    for (u32 w = 0; w < 3u; ++w) { // occluder slabs
        add(2, place(-7.f + 7.f * static_cast<f32>(w), -0.5f + u(rng), -13.f, 2.2f, 1.6f, 0.25f, 0.f), false);
    }
    add(0, place(-1.2f, -0.4f, -2.5f, 0.7f, 0.7f, 0.7f, 0.3f), true);   // near: HW extent
    add(1, place(0.25f, -0.25f, 1.45f, 0.3f, 0.3f, 0.3f, 0.5f), false); // crosses the near plane: HW clip
    for (u32 i = 0; i < 12u; ++i) {                                      // mid distance: SW (dense) / HW size (boxes)
        add(i % 3u, place(u(rng) * 6.f, u(rng) * 3.f, -6.f - 5.f * (u(rng) * 0.5f + 0.5f), 0.5f, 0.5f, 0.5f, u(rng) * 3.f),
            i % 3u == 0u);
    }
    for (u32 i = 0; i < 360u; ++i) { // far: micro triangles
        const f32 z = -18.f - 40.f * (u(rng) * 0.5f + 0.5f);
        const f32 sc = 0.5f + 0.5f * (u(rng) * 0.5f + 0.5f);
        add(i % 2u, place(u(rng) * 0.5f * -z, u(rng) * 0.36f * -z, z, sc, sc * (0.8f + 0.2f * u(rng)), sc, u(rng) * 3.f), i % 9u == 4u);
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    s.storage.build(s.gpu, s.meshes);
    return stats.ok && ctx.upload.waitAll();
}

void moveObjects(Scene& s, u32 step) {
    for (u32 k = 0; k < s.movers.size(); k += step) {
        const u32 i = s.movers[k];
        GpuTransform t = s.gpu.transform(i);
        t.rows[0][3] += 0.09f;
        t.rows[1][3] -= 0.04f;
        s.gpu.setTransform(s.handles[i], t);
    }
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

// --- rigs -----------------------------------------------------------------------------------------
enum RigId : u32 { kRigP = 0, kRigS, kRigH, kRigM, kRigR, kRigCount };
const char* const kRigNames[kRigCount] = {"P", "S", "H", "M", "R"};

struct Rig {
    InstanceCuller culler;
    VisBuffer vb;
    SwRasterizer sw;
    bool hasSw = false;
    SwRasterMode mode = SwRasterMode::Classify;
    bool skipHardware = false;
    bool results = false;

    void destroy() {
        sw.destroy();
        vb.destroy();
        culler.destroy();
    }
};

/// 1 = ok, 0 = language not built, -1 = failure
int initRig(Context& ctx, Rig& rig, VisAtomicTarget target, SwRasterKernelLanguage language, bool hasSw, SwRasterMode mode,
            bool skipHardware, bool results) {
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 1024;
    if (!rig.culler.init(cd) || !rig.culler.setResolution(kWidth, kHeight)) {
        return -1;
    }
    VisBufferDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.width = kWidth;
    vd.height = kHeight;
    vd.mode = VisMode::Atomic64;
    vd.atomicTarget = target;
    if (!rig.vb.init(vd)) {
        return -1;
    }
    rig.hasSw = hasSw;
    rig.mode = mode;
    rig.skipHardware = skipHardware;
    rig.results = results;
    if (!hasSw) {
        return 1;
    }
    SwRasterDesc sd{};
    sd.device = ctx.device.get();
    sd.allocator = ctx.allocator.get();
    sd.bindless = &ctx.bindless;
    sd.vis = &rig.vb;
    sd.language = language;
    sd.clusterCapacity = kCapacity;
    sd.results = results;
    return rig.sw.init(sd) ? 1 : 0;
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
    bool image = false;
    rg::TextureRef src;
    rg::BufferRef buffer;
    rg::BufferRef dst;
    u64 srcOffset = 0;
    u64 dstOffset = 0;
    u64 bytes = 0;
    u32 texelBytes = 8;
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
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.src)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

/// Readback offsets of one rig (0 = not read).
struct RigReadback {
    u64 raw64 = 0, counts = 0, groups[2] = {0, 0}, sw[2] = {0, 0}, results[2] = {0, 0}, vis = 0, decode = 0;
};

struct FrameState {
    CopyRecord copies[48];
    u32 copyCount = 0;
    RigReadback rb[kRigCount];
    u64 end = 0;
};

u64 take(u64& cursor, u64 bytes) {
    const u64 at = cursor;
    cursor += (bytes + 255u) & ~u64{255};
    return at;
}

void makeLayout(FrameState& fs, bool full) {
    u64 o = 0;
    for (u32 r = 0; r < kRigCount; ++r) {
        RigReadback& b = fs.rb[r];
        b = RigReadback{};
        if (!full && r != kRigM) {
            continue;
        }
        b.raw64 = take(o, kPixels * 8ull);
        if (r != kRigR) {
            b.counts = take(o, kSwCountWords * 4ull);
        }
        if (r == kRigP || r == kRigM) {
            for (u32 g = 0; g < 2u; ++g) {
                b.groups[g] = take(o, kRecordsRead * sizeof(SwGroup));
                b.sw[g] = take(o, static_cast<u64>(kCapacity) * sizeof(SwCluster));
                b.results[g] = take(o, kRecordsRead * kSwGroupSize * 4ull);
            }
        }
        if (r == kRigM) {
            b.vis = take(o, kPixels * 8ull);
            b.decode = take(o, kPixels * sizeof(VisDecodeTexel));
        }
    }
    fs.end = o;
}

struct FrameOptions {
    Mat4 viewProj{};
    bool readback = true;
    bool full = true; ///< all five rigs (else only M)
};

bool beginFrame(Context& ctx, Scene& s, Rig* rigs, const FrameOptions& opt) {
    bool ok = true;
    for (u32 r = 0; r < kRigCount; ++r) {
        if (!opt.full && r != kRigM) {
            continue;
        }
        Rig& rig = rigs[r];
        CullFrameDesc frame{};
        std::memcpy(frame.viewProj, opt.viewProj.m, sizeof(frame.viewProj));
        frame.instanceCount = s.gpu.instanceHighWater();
        ok = rig.culler.beginFrame(ctx.serial, frame) && ok;
        ok = rig.vb.beginFrame(ctx.serial, opt.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
        if (rig.hasSw) {
            SwRasterFrameDesc sf{};
            std::memcpy(sf.viewProj, opt.viewProj.m, sizeof(sf.viewProj));
            sf.sceneHandle = s.gpu.headerHandle();
            sf.mode = rig.mode;
            sf.skipHardware = rig.skipHardware;
            sf.results = rig.results;
            ok = rig.sw.beginFrame(ctx.serial, sf, rig.culler) && ok;
        }
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig* rigs, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const u32 sceneHandle = s.gpu.headerHandle();
    rg::BufferRef readback{};
    if (opt.readback) {
        readback = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, fs.end, rg::kNoQueue, nullptr, "rp_swraster.readback"});
    }
    auto copyBuffer = [&](const char* name, rg::BufferRef src, u64 srcOffset, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.buffer = src;
        c.dst = readback;
        c.srcOffset = srcOffset;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        graph.addPass(name, &recordCopy, &c)
            .use(src, rg::Access::TransferSrc, rg::BufferRange{srcOffset, bytes})
            .use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    auto copyImage = [&](const char* name, rg::TextureRef src, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.image = true;
        c.src = src;
        c.dst = readback;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        graph.addPass(name, &recordCopy, &c)
            .use(src, rg::Access::TransferSrc)
            .use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    for (u32 r = 0; r < kRigCount; ++r) {
        if (!opt.full && r != kRigM) {
            continue;
        }
        Rig& rig = rigs[r];
        const CullGraphRefs cull = rig.culler.importInto(graph);
        const VisGraphRefs vis = rig.vb.importInto(graph);
        SwRasterGraphRefs sw{};
        if (rig.hasSw) {
            sw = rig.sw.importInto(graph);
            rig.sw.addCulledFrame(graph, sw, vis, sceneRefs, sceneHandle, rig.culler, cull);
        } else {
            rig.vb.addCulledFrame(graph, vis, sceneRefs, sceneHandle, rig.culler, cull);
        }
        rg::BufferRef decode{};
        if (r == kRigM) {
            decode = graph.importBuffer(
                rg::ImportedBuffer{ctx.decodeOut.handle, ctx.decodeOut.desc.size, ctx.decodeQueue, &ctx.decodeQueue, "rp_swraster.decode"});
            rig.vb.addDecode(graph, vis, sceneRefs, decode, ctx.decodeOut.deviceAddress);
        }
        if (!opt.readback) {
            continue;
        }
        const RigReadback& b = fs.rb[r];
        if (vis.image64.valid()) {
            copyImage("readback.raw64", vis.image64, b.raw64, kPixels * 8ull);
        } else {
            copyBuffer("readback.raw64", vis.buffer64, 0u, b.raw64, kPixels * 8ull);
        }
        if (rig.hasSw) {
            copyBuffer("readback.counts", sw.counts, 0u, b.counts, kSwCountWords * 4ull);
        }
        if (r == kRigP || r == kRigM) {
            for (u32 g = 0; g < 2u; ++g) {
                copyBuffer("readback.groups", sw.groups, static_cast<u64>(g) * kCapacity * sizeof(SwGroup), b.groups[g],
                           kRecordsRead * sizeof(SwGroup));
                copyBuffer("readback.sw", sw.software, static_cast<u64>(g) * kCapacity * sizeof(SwCluster), b.sw[g],
                           static_cast<u64>(kCapacity) * sizeof(SwCluster));
                if (!sw.results.valid()) {
                    continue; // zero_alloc's rig M has no results buffer
                }
                copyBuffer("readback.results", sw.results, static_cast<u64>(g) * kCapacity * kSwGroupSize * 4ull, b.results[g],
                           kRecordsRead * kSwGroupSize * 4ull);
            }
        }
        if (r == kRigM) {
            copyImage("readback.vis", vis.vis, b.vis, kPixels * 8ull);
            copyBuffer("readback.decode", decode, 0u, b.decode, kPixels * sizeof(VisDecodeTexel));
        }
    }
    if (opt.readback) {
        graph.addPass("readback.host", nullptr, nullptr).use(readback, rg::Access::HostRead);
    }
}

bool runFrame(Context& ctx, Scene& s, Rig* rigs, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rigs, opt)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    buildGraph(ctx, s, rigs, graph, opt, fs);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    for (u32 r = 0; r < kRigCount; ++r) {
        rigs[r].culler.collectRetired(ctx.serial);
        rigs[r].vb.collectRetired(ctx.serial);
    }
    s.gpu.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

// --- analysis -------------------------------------------------------------------------------------
template <typename T>
const T* rb(Context& ctx, u64 offset) {
    return reinterpret_cast<const T*>(static_cast<const u8*>(ctx.readback.mapped) + offset);
}

/// Snapped SW vertices of a mesh triangle (the scene index layout: MTRI order == triangle id).
bool triangleVertices(const Scene& s, const f32 viewProj[16], u32 instance, u32 triangle, sw_kernel::SwVertex v[3]) {
    if (instance >= s.gpu.instanceHighWater()) {
        return false;
    }
    const GpuInstance& inst = s.gpu.instance(instance);
    const GpuMesh& mesh = s.gpu.mesh(inst.mesh);
    if (triangle >= mesh.indexCount / 3u) {
        return false;
    }
    const u32* indices = s.gpu.indexData();
    for (u32 k = 0; k < 3u; ++k) {
        const u32 vertex = static_cast<u32>(static_cast<s32>(indices[mesh.firstIndex + triangle * 3u + k]) + mesh.vertexOffset);
        f32 p[3];
        decode_kernel::mesh_position(mesh, s.positions[inst.mesh].vpos, vertex, p);
        v[k] = sw_kernel::project_vertex(decode_kernel::clip_position(s.gpu.transform(instance), viewProj, p), kWidth, kHeight);
        if (v[k].valid == 0u) {
            return false;
        }
    }
    return true;
}

/// Pixel centre vs a snapped triangle: min over edges of |E| / max(|dx|, |dy|) (sub-pixels, L-inf
/// distance bound), whether an edge function is exactly 0, coverage (top-left) and the integer depth.
struct CentreTest {
    bool ok = false;
    bool covered = false;
    bool exact = false;
    f64 distance = 1e30;
    u64 depth = 0; ///< 32-bit fixed-point depth (z >> 8 = the word's depth)
};

CentreTest testCentre(const sw_kernel::SwVertex in[3], u32 px, u32 py) {
    CentreTest t{};
    sw_kernel::SwVertex v[3] = {in[0], in[1], in[2]};
    s64 area = static_cast<s64>(v[1].x - v[0].x) * (v[2].y - v[0].y) - static_cast<s64>(v[1].y - v[0].y) * (v[2].x - v[0].x);
    if (area == 0) {
        return t;
    }
    if (area < 0) {
        std::swap(v[1], v[2]);
        area = -area;
    }
    t.ok = true;
    const s64 x = static_cast<s64>(px) * 256 + 128;
    const s64 y = static_cast<s64>(py) * 256 + 128;
    s64 e[3];
    bool in3 = true;
    for (u32 i = 0; i < 3u; ++i) {
        const sw_kernel::SwVertex& a = v[(i + 1u) % 3u];
        const sw_kernel::SwVertex& b = v[(i + 2u) % 3u];
        const s64 dx = b.x - a.x;
        const s64 dy = b.y - a.y;
        e[i] = dx * (y - a.y) - dy * (x - a.x);
        const s64 m = std::max(std::llabs(dx), std::llabs(dy));
        t.distance = std::min(t.distance, static_cast<f64>(std::llabs(e[i])) / static_cast<f64>(m));
        t.exact = t.exact || e[i] == 0;
        const s64 bias = sw_kernel::edge_bias(static_cast<s32>(dx), static_cast<s32>(dy));
        in3 = in3 && e[i] + bias >= 0;
    }
    t.covered = in3;
    if (in3) {
        t.depth = (static_cast<u64>(e[0]) * v[0].z + static_cast<u64>(e[1]) * v[1].z + static_cast<u64>(e[2]) * v[2].z) /
                  static_cast<u64>(area);
    }
    return t;
}

/// Distance (px) from the centre of pixel (px, py) to the exact f64 screen triangle (0 inside) and the
/// triangle's vertex depth range (z / w).
f64 centreDistance(const Scene& s, const f32 viewProj[16], u32 instance, u32 triangle, u32 px, u32 py, f64& zMin, f64& zMax) {
    const GpuInstance& inst = s.gpu.instance(instance);
    const GpuMesh& mesh = s.gpu.mesh(inst.mesh);
    const u32* indices = s.gpu.indexData();
    f64 x[3], y[3];
    for (u32 k = 0; k < 3u; ++k) {
        const u32 vertex = static_cast<u32>(static_cast<s32>(indices[mesh.firstIndex + triangle * 3u + k]) + mesh.vertexOffset);
        f32 p[3];
        decode_kernel::mesh_position(mesh, s.positions[inst.mesh].vpos, vertex, p);
        const decode_kernel::Clip c = decode_kernel::clip_position(s.gpu.transform(instance), viewProj, p);
        x[k] = (static_cast<f64>(c.x) / c.w * 0.5 + 0.5) * kWidth;
        y[k] = (static_cast<f64>(c.y) / c.w * 0.5 + 0.5) * kHeight;
        const f64 z = static_cast<f64>(c.z) / c.w;
        zMin = k == 0u ? z : std::min(zMin, z);
        zMax = k == 0u ? z : std::max(zMax, z);
    }
    const f64 cx = px + 0.5, cy = py + 0.5;
    f64 e[3];
    for (u32 i = 0; i < 3u; ++i) {
        const u32 a = (i + 1u) % 3u, b = (i + 2u) % 3u;
        e[i] = (x[b] - x[a]) * (cy - y[a]) - (y[b] - y[a]) * (cx - x[a]);
    }
    if ((e[0] >= 0.0 && e[1] >= 0.0 && e[2] >= 0.0) || (e[0] <= 0.0 && e[1] <= 0.0 && e[2] <= 0.0)) {
        return 0.0;
    }
    f64 best = 1e30;
    for (u32 i = 0; i < 3u; ++i) {
        const u32 a = i, b = (i + 1u) % 3u;
        const f64 dx = x[b] - x[a], dy = y[b] - y[a];
        const f64 len2 = dx * dx + dy * dy;
        f64 t = len2 > 0.0 ? ((cx - x[a]) * dx + (cy - y[a]) * dy) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const f64 qx = x[a] + t * dx - cx, qy = y[a] + t * dy - cy;
        best = std::min(best, std::sqrt(qx * qx + qy * qy));
    }
    return best;
}

struct Agreement {
    u32 differ = 0;
    u32 covered = 0; ///< pixels covered in either image
    u32 exactEdge = 0;
    u32 nearEdge = 0;
    u32 nearDepth = 0;
    u32 other = 0;
    u32 depthDiff = 0; ///< same id, different depth quantum
    u32 depthMax = 0;
    u32 exactHits = 0; ///< covered centres lying exactly on an edge of a's snapped winner (tie-break exercised)
};

Agreement compareIds(const Scene& s, const f32 viewProj[16], const u64* a, const u64* b) {
    Agreement g{};
    for (u32 p = 0; p < kPixels; ++p) {
        const VisSample sa = vis64_unpack(a[p]);
        const VisSample sb = vis64_unpack(b[p]);
        g.covered += (sa.instance != kVisInvalid || sb.instance != kVisInvalid) ? 1u : 0u;
        if (sa.instance != kVisInvalid) {
            sw_kernel::SwVertex v[3];
            if (triangleVertices(s, viewProj, sa.instance, sa.triangle, v)) {
                const CentreTest t = testCentre(v, p % kWidth, p / kWidth);
                g.exactHits += t.ok && t.exact ? 1u : 0u;
            }
        }
        if (sa.instance == sb.instance && sa.triangle == sb.triangle) {
            if (a[p] != b[p]) {
                ++g.depthDiff;
                const u32 qa = vis64_depth_bits(a[p]), qb = vis64_depth_bits(b[p]);
                g.depthMax = std::max(g.depthMax, qa > qb ? qa - qb : qb - qa);
            }
            continue;
        }
        ++g.differ;
        const u32 px = p % kWidth, py = p / kWidth;
        sw_kernel::SwVertex va[3], vb[3];
        const bool oka = sa.instance != kVisInvalid && triangleVertices(s, viewProj, sa.instance, sa.triangle, va);
        const bool okb = sb.instance != kVisInvalid && triangleVertices(s, viewProj, sb.instance, sb.triangle, vb);
        const CentreTest ta = oka ? testCentre(va, px, py) : CentreTest{};
        const CentreTest tb = okb ? testCentre(vb, px, py) : CentreTest{};
        if ((ta.ok && ta.exact) || (tb.ok && tb.exact)) {
            ++g.exactEdge;
        } else if ((ta.ok && ta.distance <= 2.0) || (tb.ok && tb.distance <= 2.0)) {
            ++g.nearEdge;
        } else if (ta.covered && tb.covered &&
                   (ta.depth > tb.depth ? ta.depth - tb.depth : tb.depth - ta.depth) <= 8u * 256u) {
            ++g.nearDepth;
        } else {
            if (g.other < 4u) {
                std::fprintf(stderr, "  pixel (%u, %u): (%u, %u) d %u vs (%u, %u) d %u; edge distances %.2f / %.2f sub-px\n", px, py,
                             sa.instance, sa.triangle, vis64_depth_bits(a[p]), sb.instance, sb.triangle, vis64_depth_bits(b[p]),
                             ta.distance, tb.distance);
            }
            ++g.other;
        }
    }
    return g;
}

f64 agreePercent(u32 differ, u32 total) { return total == 0u ? 100.0 : 100.0 * static_cast<f64>(total - differ) / total; }

struct ClassifyParity {
    u32 records[2] = {};
    u32 swCount[2] = {};
    u32 resultBad = 0;
    u32 listBad = 0;
    u32 slots = 0;
    u32 hist[kSwResultCount] = {};
    std::vector<SwCluster> swList; ///< the read-back SW lists of both regions
};

/// A rig's per-meshlet SwResult of every read-back classify record == the CPU reference kernel, and
/// its SW lists == the meshlets classified Software.
ClassifyParity classifyParity(Context& ctx, Scene& s, const RigReadback& b, const SwRasterConstants& constants) {
    ClassifyParity r{};
    const u32* counts = rb<u32>(ctx, b.counts);
    for (u32 g = 0; g < 2u; ++g) {
        r.records[g] = counts[g == 0u ? kSwCountClassify0 : kSwCountClassify1];
        r.swCount[g] = counts[g == 0u ? kSwCountSoftware0 : kSwCountSoftware1];
        expect(r.records[g] <= kRecordsRead, "records fit the read-back window");
        const u32 n = std::min(r.records[g], kRecordsRead);
        const SwGroup* groups = rb<SwGroup>(ctx, b.groups[g]);
        std::vector<u32> cpu;
        swraster_classify_reference(s.storage.view, constants, {groups, n}, cpu, fuse::kernel::Backend::CpuParallel);
        const u32* gpu = rb<u32>(ctx, b.results[g]);
        std::vector<SwCluster> expected;
        for (u32 i = 0; i < n * kSwGroupSize; ++i) {
            ++r.slots;
            ++r.hist[gpu[i] < kSwResultCount ? gpu[i] : 0u];
            if (gpu[i] != cpu[i]) {
                if (r.resultBad < 4u) {
                    std::fprintf(stderr, "  region %u record %u lane %u: GPU %u, CPU %u\n", g, i / kSwGroupSize, i % kSwGroupSize, gpu[i],
                                 cpu[i]);
                }
                ++r.resultBad;
            }
            if (cpu[i] == kSwResultSoftware) {
                expected.push_back(SwCluster{groups[i / kSwGroupSize].instance, groups[i / kSwGroupSize].firstMeshlet + i % kSwGroupSize});
            }
        }
        const SwCluster* list = rb<SwCluster>(ctx, b.sw[g]);
        std::vector<SwCluster> got(list, list + std::min(r.swCount[g], kCapacity));
        auto less = [](const SwCluster& x, const SwCluster& y) {
            return x.instance != y.instance ? x.instance < y.instance : x.meshlet < y.meshlet;
        };
        std::sort(got.begin(), got.end(), less);
        std::sort(expected.begin(), expected.end(), less);
        const bool same = got.size() == expected.size() &&
                          std::equal(got.begin(), got.end(), expected.begin(), [](const SwCluster& x, const SwCluster& y) {
                              return x.instance == y.instance && x.meshlet == y.meshlet;
                          });
        r.listBad += same ? 0u : 1u;
        r.swList.insert(r.swList.end(), got.begin(), got.end());
    }
    return r;
}

struct RunImages {
    std::vector<u64> words[kRigCount];
};

int runGate(Context& ctx, VisAtomicTarget target) {
    std::vector<RunImages> runs;
    std::vector<const char*> languages;
    for (const SwRasterKernelLanguage language : {SwRasterKernelLanguage::Slang, SwRasterKernelLanguage::Glsl}) {
        const char* want = language == SwRasterKernelLanguage::Slang ? "slang" : "glsl";
        Rig rigs[kRigCount];
        const SwRasterMode modes[kRigCount] = {SwRasterMode::ForceSoftware, SwRasterMode::ForceSoftware, SwRasterMode::ForceHardware,
                                               SwRasterMode::Classify, SwRasterMode::Classify};
        int rc = 1;
        for (u32 r = 0; r < kRigCount && rc == 1; ++r) {
            rc = initRig(ctx, rigs[r], target, language, r != kRigR, modes[r], r == kRigP, r == kRigP || r == kRigM);
        }
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init (%s)\n", want);
            return 1;
        }
        if (rc == 0) {
            std::printf("  language %s: not built, skipped\n", want);
            for (Rig& r : rigs) {
                r.destroy();
            }
            continue;
        }
        Scene s;
        if (!buildScene(ctx, s)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        std::printf("  kernels: %s, target %s, %u instances, meshlets per mesh (%zu, %zu, %zu), capacity %u\n",
                    rigs[kRigM].sw.kernelLanguage(), rigs[kRigM].vb.atomicTarget() == VisAtomicTarget::Image ? "image" : "buffer",
                    s.gpu.instanceHighWater(), s.meshes[0].meshlets.size(), s.meshes[1].meshlets.size(), s.meshes[2].meshlets.size(),
                    rigs[kRigM].sw.capacity());
        rg::Graph graph;
        FrameState fs;
        makeLayout(fs, true);
        if (fs.end > kReadbackBytes) {
            std::fprintf(stderr, "FAIL: readback layout too large (%llu)\n", static_cast<unsigned long long>(fs.end));
            return 1;
        }
        RunImages images;
        u32 totalSw = 0, totalHw = 0;
        u32 mixedHist[kSwResultCount] = {};
        for (u32 frame = 0; frame < kFrames; ++frame) {
            beginSceneFrame(ctx, s);
            if (frame > 0u) {
                moveObjects(s, 1u);
            }
            FrameOptions opt{};
            opt.viewProj = frameView(frame);
            if (!runFrame(ctx, s, rigs, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            s.storage.build(s.gpu, s.meshes);
            const f32* vp = opt.viewProj.m;
            // (1) parity: P (and M's classification) == the CPU reference.
            const RigReadback& bp = fs.rb[kRigP];
            const u32* pc = rb<u32>(ctx, bp.counts);
            const SwRasterConstants& constantsP = rigs[kRigP].sw.constants();
            ClassifyParity cp = classifyParity(ctx, s, bp, rigs[kRigP].sw.constants());
            const ClassifyParity cm = classifyParity(ctx, s, fs.rb[kRigM], rigs[kRigM].sw.constants());
            const u32* records = cp.records;
            const u32* swCount = cp.swCount;
            const u32 resultBad = cp.resultBad + cm.resultBad;
            const u32 listBad = cp.listBad + cm.listBad;
            const u32 slots = cp.slots;
            const u32* hist = cp.hist;
            std::vector<SwCluster>& swList = cp.swList;
            for (u32 k = 0; k < kSwResultCount; ++k) {
                mixedHist[k] += cm.hist[k];
            }
            std::vector<u64> cpuWords(kPixels, kVis64Clear);
            std::vector<SwCluster> demoted;
            SwRasterReferenceStats rst{};
            swraster_raster_reference(s.storage.view, constantsP, {swList.data(), static_cast<u32>(swList.size())}, cpuWords, demoted,
                                      &rst, fuse::kernel::Backend::CpuParallel);
            const u64* pWords = rb<u64>(ctx, bp.raw64);
            u32 rasterBad = 0, pCovered = 0;
            for (u32 p = 0; p < kPixels; ++p) {
                pCovered += vis64_valid(pWords[p]) ? 1u : 0u;
                if (pWords[p] != cpuWords[p]) {
                    if (rasterBad < 4u) {
                        std::fprintf(stderr, "  pixel (%u, %u): GPU %016llx, CPU %016llx\n", p % kWidth, p / kWidth,
                                     static_cast<unsigned long long>(pWords[p]), static_cast<unsigned long long>(cpuWords[p]));
                    }
                    ++rasterBad;
                }
            }
            // (2) SW vs HW on the same cluster set.
            const u64* sWords = rb<u64>(ctx, fs.rb[kRigS].raw64);
            const u64* hWords = rb<u64>(ctx, fs.rb[kRigH].raw64);
            const u64* mWords = rb<u64>(ctx, fs.rb[kRigM].raw64);
            const u64* rWords = rb<u64>(ctx, fs.rb[kRigR].raw64);
            const Agreement sh = compareIds(s, vp, sWords, hWords);
            const Agreement mh = compareIds(s, vp, mWords, hWords);
            const Agreement mr = compareIds(s, vp, mWords, rWords);
            const Agreement hr = compareIds(s, vp, hWords, rWords);
            // (3) mixed frame: counts, decode.
            const u32* mc = rb<u32>(ctx, fs.rb[kRigM].counts);
            const u32 mSw = mc[kSwCountSoftware0] + mc[kSwCountSoftware1];
            const u32 mHw = mc[kSwCountHardware0 + 1u] + mc[kSwCountHardware1 + 1u];
            totalSw += mSw;
            totalHw += mHw;
            const u32* mVis = rb<u32>(ctx, fs.rb[kRigM].vis);
            const VisDecodeTexel* gpuDecode = rb<VisDecodeTexel>(ctx, fs.rb[kRigM].decode);
            std::vector<VisDecodeTexel> cpuDecode;
            decode_reference(vis_scene_view(s.gpu, s.positions), vp, mVis, kWidth, kHeight, cpuDecode);
            u32 decodeFlagBad = 0, decodeNotOk = 0, depthOff = 0, farOff = 0, idBad = 0, interior = 0, rangeOff = 0;
            f64 decodeMaxDiff = 0.0, depthMaxQ = 0.0, distMax = 0.0;
            for (u32 p = 0; p < kPixels; ++p) {
                const VisSample sm = vis64_unpack(mWords[p]);
                idBad += (sm.instance != mVis[p * 2u] || sm.triangle != mVis[p * 2u + 1u]) ? 1u : 0u;
                const VisDecodeTexel& g = gpuDecode[p];
                const VisDecodeTexel& c = cpuDecode[p];
                decodeFlagBad += g.flags != c.flags ? 1u : 0u;
                decodeMaxDiff = std::max({decodeMaxDiff, std::fabs(static_cast<f64>(g.depth) - c.depth),
                                          std::fabs(static_cast<f64>(g.b1) - c.b1), std::fabs(static_cast<f64>(g.b2) - c.b2)});
                if (sm.instance == kVisInvalid) {
                    continue;
                }
                if (c.flags != kVisDecodeOk) {
                    ++decodeNotOk;
                    continue;
                }
                // The decoded triangle covers the centre up to vertex snapping (<= 1/512 px per axis and
                // vertex): its exact (f64) screen triangle is within 1/256 px of the centre.
                f64 zMin = 0.0, zMax = 0.0;
                const f64 dist = centreDistance(s, vp, sm.instance, sm.triangle, p % kWidth, p / kWidth, zMin, zMax);
                distMax = std::max(distMax, dist);
                farOff += dist > 1.0 / 256.0 ? 1u : 0u;
                // The word's depth is an interpolation over the (snapped) triangle: inside the triangle's
                // exact vertex depth range, widened by that range for centres on the snapped boundary.
                const f64 q = static_cast<f64>(vis64_depth_bits(mWords[p]));
                const f64 lo = std::floor(zMin * 16777216.0), hi = std::floor(zMax * 16777216.0);
                rangeOff += (q < lo - (hi - lo) - 1.0 || q > hi + (hi - lo) + 1.0) ? 1u : 0u;
                // Where the centre is well inside the exact triangle: decode (f32 plane through the exact
                // vertices) vs the word (SW: plane through the snapped vertices) - statistic.
                const f64 b0 = 1.0 - c.b1 - c.b2;
                if (std::min({b0, static_cast<f64>(c.b1), static_cast<f64>(c.b2)}) >= 0.05) {
                    ++interior;
                    const f64 dq = static_cast<f64>(c.depth) * 16777216.0 - q;
                    depthMaxQ = std::max(depthMaxQ, std::fabs(dq - 0.5));
                    depthOff += std::fabs(dq - 0.5) > 64.0 ? 1u : 0u;
                }
            }
            u32 counterBad = 0;
            for (u32 r = 0; r < kRigR; ++r) {
                const u32* c = rb<u32>(ctx, fs.rb[r].counts);
                counterBad += c[kSwCountOverflow] + c[kSwCountSkipped] + c[kSwCountOversize] + c[kSwCountDemoted];
            }
            std::printf("  frame %u  parity: records %u+%u, slots %u (culled %u, SW %u, HW size %u / extent %u / clip %u), result-bad %u, "
                        "list-bad %u, SW clusters %u+%u, %u triangles, %u px, raster-bad %u, demoted GPU %u / CPU %u\n",
                        frame, records[0], records[1], slots, hist[kSwResultCulled], hist[kSwResultSoftware],
                        hist[kSwResultHardwareSize], hist[kSwResultHardwareExtent], hist[kSwResultHardwareClip], resultBad, listBad,
                        swCount[0], swCount[1], rst.triangles, pCovered, rasterBad, pc[kSwCountDemoted], rst.demoted);
            auto line = [&](const char* name, const Agreement& a) {
                std::printf("           %s: %u px differ of %u covered (%.4f%% of covered, %.4f%% of all agree): exact-edge %u, "
                            "near-edge %u, near-depth %u, other %u; same id depth diff %u px (max %u quanta); %u centres exactly on "
                            "an edge of the first image's winner\n",
                            name, a.differ, a.covered, agreePercent(a.differ, a.covered), agreePercent(a.differ, kPixels), a.exactEdge,
                            a.nearEdge, a.nearDepth, a.other, a.depthDiff, a.depthMax, a.exactHits);
            };
            line("SW vs HW   ", sh);
            line("mixed vs HW", mh);
            line("mixed vs 1.4", mr);
            line("HW vs 1.4  ", hr);
            std::printf("           mixed: %u SW + %u HW clusters; decode: flags-bad %u (GPU-CPU max diff %.3g), not-ok %u, id-bad %u, "
                        "centre-to-triangle max %.5f px (off %u), depth outside the triangle's range %u, interior decode-vs-word depth "
                        "max %.1f quanta, > 64 quanta on %u of %u px; counters-bad %u\n",
                        mSw, mHw, decodeFlagBad, decodeMaxDiff, decodeNotOk, idBad, distMax, farOff, rangeOff, depthMaxQ, depthOff,
                        interior, counterBad);
            expect(resultBad == 0u, "per-meshlet classification == CPU reference kernel (P and M)");
            expect(listBad == 0u, "SW list == the meshlets classified Software");
            expect(rasterBad == 0u, "SW raster (GPU) == CPU reference rasteriser bit for bit");
            expect(pc[kSwCountDemoted] == rst.demoted, "demotions GPU == CPU");
            expect(pCovered > kPixels / 20u && swCount[0] + swCount[1] > 1000u, "the SW path covers a real part of the frame");
            expect(sh.differ * 1000u <= kPixels && sh.differ * 1000u <= sh.covered, "SW vs HW agree on >= 99.9% of pixels");
            expect(sh.other == 0u, "every SW / HW disagreement is an edge or depth near-tie");
            expect(mh.differ * 1000u <= kPixels && mr.differ * 1000u <= kPixels, "mixed agrees with HW and the WP-1.4 path on >= 99.9%");
            expect(mSw > 0u && mHw > 0u, "the mixed frame has SW and HW clusters");
            expect(decodeFlagBad == 0u && decodeMaxDiff <= 1e-6, "GPU decode == CPU decode kernel");
            expect(idBad == 0u, "exported visibility image == the 64-bit words");
            expect(decodeNotOk == 0u && farOff == 0u && rangeOff == 0u,
                   "mixed frame decodes: ids valid, the decoded triangle covers the centre, depth within its range");
            expect(depthOff * 200u <= interior, "decode-vs-word depth within 64 quanta on >= 99.5% of interior pixels");
            expect(counterBad == 0u, "no overflow / skipped / oversize / demoted cluster");
            if (frame + 1u == kFrames) {
                for (u32 r = 0; r < kRigCount; ++r) {
                    const u64* w = rb<u64>(ctx, fs.rb[r].raw64);
                    images.words[r].assign(w, w + kPixels);
                }
            }
        }
        expect(totalSw > 0u && totalHw > 0u, "SW and HW clusters over the run");
        std::printf("  mixed (Classify) over the run: culled %u, SW %u, HW size %u, HW extent %u, HW clip %u\n",
                    mixedHist[kSwResultCulled], mixedHist[kSwResultSoftware], mixedHist[kSwResultHardwareSize],
                    mixedHist[kSwResultHardwareExtent], mixedHist[kSwResultHardwareClip]);
        expect(mixedHist[kSwResultSoftware] > 0u && mixedHist[kSwResultHardwareSize] > 0u && mixedHist[kSwResultHardwareExtent] > 0u &&
                   mixedHist[kSwResultHardwareClip] > 0u,
               "the mixed frames classify clusters SW, HW by size, by extent and by clip");
        runs.push_back(std::move(images));
        languages.push_back(rigs[kRigM].sw.kernelLanguage());
        s.gpu.destroy();
        for (Rig& r : rigs) {
            r.destroy();
        }
    }
    if (runs.empty()) {
        std::printf("SKIP: no software rasteriser kernels built\n");
        return kSkip;
    }
    if (runs.size() == 2u) {
        bool same = true;
        for (u32 r = 0; r < kRigCount; ++r) {
            same = same && runs[0].words[r] == runs[1].words[r];
        }
        expect(same, "Slang == GLSL 64-bit targets (every rig)");
        std::printf("  %s vs %s: 64-bit targets %s\n", languages[0], languages[1], same ? "identical" : "DIFFERENT");
    }
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "swraster.", 9) == 0 || std::strncmp(name, "vis.", 4) == 0 ||
                            std::strncmp(name, "cull.", 5) == 0)) {
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
    const VisCapabilities caps = queryVisCapabilities(ctx.device.get());
    Rig rigs[kRigCount];
    if (initRig(ctx, rigs[kRigM], caps.atomicImage ? VisAtomicTarget::Image : VisAtomicTarget::Buffer, SwRasterKernelLanguage::Auto,
                true, SwRasterMode::Classify, false, false) != 1) {
        std::printf("SKIP: no software rasteriser kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    makeLayout(fs, false);
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long frameSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 lastCovered = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool last = frame + 1u == kTotal;
        const bool measure = countAllocations && frame >= kWarmup && !last;
        beginSceneFrame(ctx, s);
        if ((frame & 7u) == 0u) {
            moveObjects(s, 4u); // ~1% of the instances
        }
        s.gpu.commit();
        ctx.upload.flush();
        FrameOptions opt{};
        opt.viewProj = frameView(frame % kFrames);
        opt.full = false;
        opt.readback = last;
        t_allocations = 0;
        t_count = measure;
        const bool began = beginFrame(ctx, s, rigs, opt);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, s, rigs, graph, opt, fs);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rigs[kRigM].culler.collectRetired(ctx.serial);
        rigs[kRigM].vb.collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            frameSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (last) {
            const u64* w = rb<u64>(ctx, fs.rb[kRigM].raw64);
            for (u32 p = 0; p < kPixels; ++p) {
                lastCovered += vis64_valid(w[p]) ? 1u : 0u;
            }
        }
    }
    expect(lastCovered > kPixels / 10u, "last frame covers the view");
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, ~1%% moved)\n"
                    "  SwRasterizer/VisBuffer/culler beginFrame + swraster.* / vis.* / cull.* pass callbacks: %llu operator-new "
                    "calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis and swraster imports + passes): %llu\n",
                    kTotal - kWarmup - 1u, s.gpu.instanceHighWater(), frameSide, callbacks, build);
        expect(frameSide == 0u, "software rasteriser makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the swraster passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: last frame %u covered pixels\n", lastCovered);
    }
    s.gpu.destroy();
    for (Rig& r : rigs) {
        r.destroy();
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "atomic_buffer";
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
        const VisCapabilities caps = queryVisCapabilities(ctx.device.get());
        if (mode == "atomic_image") {
            if (!caps.atomicImage) {
                std::printf("SKIP: no R64_UINT image atomics (%s)\n", caps.atomicReason);
                return kSkip;
            }
            rc = runGate(ctx, VisAtomicTarget::Image);
        } else if (mode == "atomic_buffer") {
            if (!caps.atomicBuffer) {
                std::printf("SKIP: no 64-bit buffer atomics (%s)\n", caps.atomicReason);
                return kSkip;
            }
            rc = runGate(ctx, VisAtomicTarget::Buffer);
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
