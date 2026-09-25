// WP-5.1 mesh-shader path, Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_meshlet_path_cpu.cpp.
//
// Every frame is one render graph (WP-0.3) with two rigs on the SAME GpuScene (WP-1.1):
//   mesh rig      InstanceCuller (WP-1.3) + VisBuffer (WP-1.4 targets) + MeshletPath (mesh shaders):
//                 cull.phase1, meshlet.reset/expand1/phase1, [vis.export], cull.hiz, (Hi-Z read-back),
//                 cull.phase2, meshlet.expand2/phase2, [vis.export], cull.hiz, (Hi-Z read-back)
//   indirect rig  a second culler + VisBuffer running VisBuffer::addCulledFrame (the WP-1.4 path)
// Scene: the WP-1.4 gate's (3 closed meshlet meshes - sphere, torus, box - x 600 instances behind 3
// non-uniformly scaled occluder slabs, 256x192), 8 frames of camera + object motion, backface cone
// culling on in odd frames.
//
//   --mode raster          R32G32_UINT + D32 target
//   --mode atomic_image    64-bit atomicMin target, R64_UINT image (skip without image int64 atomics)
//   --mode atomic_buffer   64-bit atomicMin target, u64 buffer (skip without buffer int64 atomics)
//     checked every frame, for each kernel language built (Slang, GLSL):
//       ids       mesh-path visibility image == indirect-path image on every pixel (covered or not),
//                 bit for bit, except near depth ties: pixels whose two candidate surfaces lie within
//                 kDepthUlps (8 f32 ulp, CPU decode) of each other, where Lavapipe's pipeline-dependent
//                 depth rounding (below) decides; they are counted and bounded (<= 0.05% of covered
//                 pixels); every other mismatch fails. Depth of the same (instance, triangle): within
//                 kDepthUlps (D32 raster / exported R32F); atomic: the raw 64-bit words of the same id
//                 within kDepthUlps depth quanta
//       cull      per-meshlet results (every record of both regions) == the CPU reference kernel
//                 (meshlet_cull_kernel.hpp) on the read-back task-group records, last frame's and this
//                 frame's read-back Hi-Z, except meshlets on a test boundary (strict / lenient bracket);
//                 the culler's phase-1 / phase-2 draw counts equal between the rigs
//       counters  0 overflow, 0 skipped instances, 0 oversize meshlets; over the run meshlets are
//                 drawn early, late (deferred then disoccluded) and in phase 2, and meshlets are
//                 frustum-, cone- and Hi-Z-culled
//     and Slang == GLSL (identical images).
//   --mode masked_t0       device created with maxTier = T0 (mesh shaders masked): MeshletPath Auto
//                          selects the WP-1.4 fallback (reason names the tier cap), MeshShader mode
//                          fails, 3 frames through MeshletPath::addCulledFrame == VisBuffer's own
//                          (a second culler + VisBuffer), bit for bit
//   --mode zero_alloc      64 steady-state frames (raster, ~1% moved): 0 operator-new calls in
//                          MeshletPath::beginFrame + the graph build + the meshlet.* / vis.* / cull.*
//                          pass callbacks (validated run first; validation off for the count)
//   --backend set | buffer bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/renderer/culling/cull_reference.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/meshlet/meshlet_path.hpp>
#include <fuse/renderer/meshlet/meshlet_reference.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
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
using namespace fuse::renderer::meshlet;
using namespace fuse::renderer::visbuffer;
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
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kInstances = 600;
constexpr u32 kFrames = 8;
constexpr u32 kGroupCapacity = 2048;
/// Depth tolerance between the paths for the same (instance, triangle), in f32 ulps: Lavapipe sets up
/// vertex- and mesh-pipeline triangles on different code paths, and the same fragment's depth differs
/// by up to 4 ulp on ~3% of the covered pixels (observed). A pixel whose two candidate surfaces are
/// within this distance is a near tie: either path may keep either one.
constexpr u32 kDepthUlps = 8;

u32 ulpDistance(f32 a, f32 b) {
    u32 x = 0, y = 0;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x > y ? x - y : y - x; // both depths are in [0, 1]: same sign
}

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

struct View {
    Mat4 viewProj{};
    f32 eye[3] = {0.f, 0.f, 0.f};
};

View frameView(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    View v{};
    v.eye[0] = -1.2f + 0.45f * t;
    v.eye[1] = 0.6f - 0.1f * t;
    v.eye[2] = 3.f;
    const f32 at[3] = {0.4f * t - 1.f, 0.f, -25.f};
    v.viewProj = mul(perspective(1.1f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 120.f), lookAt(v.eye, at));
    return v;
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

/// Byte layout of the readback buffer (Hi-Z mips appended at run time).
struct ReadbackLayout {
    u64 vis = 0, depth = 0, raw64 = 0, refVis = 0, refDepth = 0, refRaw64 = 0, counts = 0, cullCounts = 0,
        refCullCounts = 0, groups = 0, results = 0, hiz1 = 0, hiz2 = 0, end = 0;
    u64 hizMip[kMaxHizMips] = {};
};

ReadbackLayout makeLayout(u32 hizDim, u32 hizMips) {
    ReadbackLayout l{};
    u64 o = 0;
    auto take = [&](u64 bytes) {
        const u64 at = o;
        o += (bytes + 255u) & ~u64{255};
        return at;
    };
    l.vis = take(kPixels * 8ull);
    l.depth = take(kPixels * 4ull);
    l.raw64 = take(kPixels * 8ull);
    l.refVis = take(kPixels * 8ull);
    l.refDepth = take(kPixels * 4ull);
    l.refRaw64 = take(kPixels * 8ull);
    l.counts = take(kMeshletCountWords * 4ull);
    l.cullCounts = take(kCountWords * 4ull);
    l.refCullCounts = take(kCountWords * 4ull);
    l.groups = take(2ull * kGroupCapacity * sizeof(MeshletGroup));
    l.results = take(2ull * kGroupCapacity * kMeshletTaskGroup * 4ull);
    u64 hizBytes = 0;
    for (u32 m = 0; m < hizMips; ++m) {
        l.hizMip[m] = hizBytes;
        const u64 dim = std::max<u64>(1u, hizDim >> m);
        hizBytes += dim * dim * 4u;
    }
    l.hiz1 = take(hizBytes);
    l.hiz2 = take(hizBytes);
    l.end = o;
    return l;
}

constexpr u64 kReadbackBytes = 8u * 1024u * 1024u;

int setup(Context& ctx, bool descriptorBuffer, bool validation, RenderTier maxTier = kMaxRenderTier) {
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
    instanceDesc.appName = "fuse_rp_meshlet_path";
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
    if (descriptorBuffer && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const VisCapabilities vis = queryVisCapabilities(ctx.device.get());
    if (!vis.raster) {
        std::printf("SKIP: visibility buffer unsupported: %s\n", vis.rasterReason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    const MeshletCapabilities mesh = queryMeshletCapabilities(ctx.device.get());
    std::printf("device: %s; %s; mesh path: %s (%s; max task groups %u, mesh outputs %u v / %u p)\n",
                ctx.device->info().deviceName.c_str(), ctx.device->info().caps.summary().c_str(), mesh.meshPath ? "yes" : "no",
                mesh.reason, mesh.maxTaskGroups, mesh.maxMeshOutputVertices, mesh.maxMeshOutputPrimitives);
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
    stagingDesc.name = "rp_meshlet_path.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = kReadbackBytes;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_meshlet_path.readback";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr) {
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

// --- scene (the WP-1.4 gate's) --------------------------------------------------------------------
struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<decode_kernel::MeshPositions> positions;
    std::vector<fuse::kernel::Span<const geometry::MeshletRecord>> meshlets;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
};

/// Makes a closed mesh's winding outward (positive signed volume): the WP-1.2 cone assumes the side
/// its winding faces is the visible one, and vis_test::uvSphere is wound inward.
vis_test::SourceMesh outward(vis_test::SourceMesh m) {
    f64 volume = 0.0;
    for (usize t = 0; t + 2u < m.indices.size(); t += 3u) {
        const f32* a = &m.positions[m.indices[t] * 3u];
        const f32* b = &m.positions[m.indices[t + 1u] * 3u];
        const f32* c = &m.positions[m.indices[t + 2u] * 3u];
        volume += static_cast<f64>(a[0]) * (static_cast<f64>(b[1]) * c[2] - static_cast<f64>(b[2]) * c[1]) +
                  static_cast<f64>(a[1]) * (static_cast<f64>(b[2]) * c[0] - static_cast<f64>(b[0]) * c[2]) +
                  static_cast<f64>(a[2]) * (static_cast<f64>(b[0]) * c[1] - static_cast<f64>(b[1]) * c[0]);
    }
    if (volume < 0.0) {
        for (usize t = 0; t + 2u < m.indices.size(); t += 3u) {
            std::swap(m.indices[t + 1u], m.indices[t + 2u]);
        }
    }
    return m;
}

bool buildScene(Context& ctx, Scene& s, u32 count) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = count + 64u;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    const vis_test::SourceMesh sources[3] = {outward(vis_test::uvSphere(16, 24, 1.f)),
                                             outward(vis_test::torus(24, 12, 1.f, 0.35f)), outward(vis_test::box())};
    s.meshes.resize(3);
    for (u32 i = 0; i < 3u; ++i) {
        if (!vis_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    for (const geometry::MeshletMesh& m : s.meshes) {
        s.positions.push_back(decode_kernel::MeshPositions{m.positions.data(), m.vertex_count()});
        s.meshlets.push_back({m.meshlets.data(), static_cast<u32>(m.meshlets.size())});
    }
    std::mt19937 rng(1234);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 w = 0; w < 3u; ++w) {
        InstanceDesc id{};
        id.mesh = 2;
        id.transform = place(-7.f + 7.f * static_cast<f32>(w), -0.5f + u(rng), -11.f, 2.4f, 2.6f, 0.25f, 0.f);
        s.handles.push_back(s.gpu.addInstance(id));
    }
    for (u32 i = static_cast<u32>(s.handles.size()); i < count; ++i) {
        InstanceDesc id{};
        id.mesh = i % 3u;
        const f32 sc = 0.35f + 0.4f * (u(rng) * 0.5f + 0.5f);
        id.transform = place(u(rng) * 16.f, u(rng) * 7.f, -14.f - 26.f * (u(rng) * 0.5f + 0.5f), sc, sc, sc, u(rng) * 3.f);
        if (i % 97u == 11u) {
            id.flags &= ~static_cast<u32>(kInstanceVisible);
        }
        s.handles.push_back(s.gpu.addInstance(id));
        if (i % 9u == 4u) {
            s.movers.push_back(i);
        }
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

void moveObjects(Scene& s, u32 frame, u32 step) {
    for (u32 k = 0; k < s.movers.size(); k += step) {
        const u32 i = s.movers[(k + frame) % s.movers.size()];
        GpuTransform t = s.gpu.transform(i);
        t.rows[0][3] += 0.07f;
        t.rows[1][3] -= 0.03f;
        s.gpu.setTransform(s.handles[i], t);
    }
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

// --- rig ------------------------------------------------------------------------------------------
struct Rig {
    InstanceCuller culler;    ///< mesh rig
    VisBuffer vb;
    MeshletPath path;
    InstanceCuller refCuller; ///< WP-1.4 indirect rig
    VisBuffer refVb;
    const char* language = "none";

    void destroy() {
        path.destroy();
        vb.destroy();
        refVb.destroy();
        culler.destroy();
        refCuller.destroy();
    }
};

/// 1 = ok, 0 = language not built, -1 = failure
int initRig(Context& ctx, Rig& rig, VisMode mode, VisAtomicTarget target, MeshletKernelLanguage language,
            MeshletPathMode pathMode, bool results) {
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = kInstances + 64u;
    if (!rig.culler.init(cd) || !rig.refCuller.init(cd) || !rig.culler.setResolution(kWidth, kHeight) ||
        !rig.refCuller.setResolution(kWidth, kHeight)) {
        return -1;
    }
    VisBufferDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.width = kWidth;
    vd.height = kHeight;
    vd.mode = mode;
    vd.atomicTarget = target;
    if (!rig.vb.init(vd) || !rig.refVb.init(vd)) {
        return -1;
    }
    MeshletPathDesc md{};
    md.device = ctx.device.get();
    md.allocator = ctx.allocator.get();
    md.bindless = &ctx.bindless;
    md.vis = &rig.vb;
    md.mode = pathMode;
    md.language = language;
    md.groupCapacity = kGroupCapacity;
    md.results = results;
    if (!rig.path.init(md)) {
        return pathMode == MeshletPathMode::MeshShader ? 0 : -1;
    }
    rig.language = rig.path.kernelLanguage();
    return 1;
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
    enum Kind : u8 { Image, Buffer, Hiz } kind = Image;
    const InstanceCuller* culler = nullptr;
    const ReadbackLayout* layout = nullptr;
    rg::TextureRef image;
    u32 aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    rg::BufferRef buffer;
    rg::BufferRef dst;
    u64 dstOffset = 0;
    u64 bytes = 0;
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
    if (c.kind == CopyRecord::Hiz) {
        VkBufferImageCopy regions[kMaxHizMips]{};
        const u32 mips = c.culler->hizMipCount();
        for (u32 m = 0; m < mips; ++m) {
            const u32 dim = std::max(1u, c.culler->hizDim() >> m);
            regions[m].bufferOffset = c.dstOffset + c.layout->hizMip[m];
            regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            regions[m].imageExtent = {dim, dim, 1};
        }
        vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, mips,
                               regions);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {c.aspect, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

struct FrameState {
    CopyRecord copies[20];
    u32 copyCount = 0;
    ReadbackLayout layout{};
};

struct FrameOptions {
    View view{};
    bool cone = false;
    bool results = true;
    bool withReference = true;
    bool readback = true;
};

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const FrameOptions& opt) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, opt.view.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, opt.view.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    MeshletFrameDesc mf{};
    std::memcpy(mf.viewProj, opt.view.viewProj.m, sizeof(mf.viewProj));
    std::memcpy(mf.camera, opt.view.eye, sizeof(mf.camera));
    mf.sceneHandle = s.gpu.headerHandle();
    mf.cone = opt.cone;
    mf.results = opt.results;
    ok = rig.path.beginFrame(ctx.serial, mf, rig.culler) && ok;
    if (opt.withReference) {
        ok = rig.refCuller.beginFrame(ctx.serial, frame) && ok;
        ok = rig.refVb.beginFrame(ctx.serial, opt.view.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    const MeshletGraphRefs mesh = rig.path.importInto(graph);
    const u32 sceneHandle = s.gpu.headerHandle();
    rg::BufferRef readback{};
    if (opt.readback) {
        readback = graph.importBuffer(
            rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_meshlet_path.readback"});
    }
    auto addCopy = [&](const char* name, CopyRecord::Kind kind, rg::TextureRef image, u32 aspect, rg::BufferRef buffer,
                       u64 dstOffset, u64 bytes, const InstanceCuller* culler) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.kind = kind;
        c.culler = culler;
        c.layout = &fs.layout;
        c.image = image;
        c.aspect = aspect;
        c.buffer = buffer;
        c.dst = readback;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        rg::PassBuilder pass = graph.addPass(name, &recordCopy, &c);
        if (kind == CopyRecord::Buffer) {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        } else {
            pass.use(image, rg::Access::TransferSrc);
        }
        pass.use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, kind == CopyRecord::Hiz ? fs.layout.hiz2 - fs.layout.hiz1 : bytes});
    };
    const bool atomic = rig.vb.mode() == VisMode::Atomic64;
    const bool meshPath = rig.path.usesMeshShaders();
    if (opt.readback && meshPath) {
        // Mesh rig piece by piece: the phase-1 Hi-Z is read back between the phases.
        rig.culler.addPhase1(graph, cull, sceneRefs, sceneHandle);
        rig.path.addDraw(graph, mesh, vis, sceneRefs, rig.culler, cull, CullPhase::Phase1);
        rig.vb.addExport(graph, vis);
        rig.culler.addHizBuild(graph, cull, vis.depth, rig.vb.depthSampledHandle());
        addCopy("readback.hiz1", CopyRecord::Hiz, cull.hiz, 0u, {}, fs.layout.hiz1, 0u, &rig.culler);
        rig.culler.addPhase2(graph, cull, sceneRefs, sceneHandle);
        rig.path.addDraw(graph, mesh, vis, sceneRefs, rig.culler, cull, CullPhase::Phase2);
        rig.vb.addExport(graph, vis);
        rig.culler.addHizBuild(graph, cull, vis.depth, rig.vb.depthSampledHandle());
    } else {
        rig.path.addCulledFrame(graph, mesh, vis, sceneRefs, sceneHandle, rig.culler, cull);
    }
    if (opt.withReference) {
        const CullGraphRefs refCull = rig.refCuller.importInto(graph);
        const VisGraphRefs refVis = rig.refVb.importInto(graph);
        rig.refVb.addCulledFrame(graph, refVis, sceneRefs, sceneHandle, rig.refCuller, refCull);
        if (opt.readback) {
            addCopy("readback.ref_vis", CopyRecord::Image, refVis.vis, VK_IMAGE_ASPECT_COLOR_BIT, {}, fs.layout.refVis,
                    kPixels * 8ull, nullptr);
            addCopy("readback.ref_depth", CopyRecord::Image, refVis.depth,
                    atomic ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, {}, fs.layout.refDepth, kPixels * 4ull,
                    nullptr);
            if (refVis.image64.valid()) {
                addCopy("readback.ref_raw64", CopyRecord::Image, refVis.image64, VK_IMAGE_ASPECT_COLOR_BIT, {},
                        fs.layout.refRaw64, kPixels * 8ull, nullptr);
            }
            if (refVis.buffer64.valid()) {
                addCopy("readback.ref_raw64", CopyRecord::Buffer, {}, 0u, refVis.buffer64, fs.layout.refRaw64, kPixels * 8ull,
                        nullptr);
            }
            addCopy("readback.ref_cull_counts", CopyRecord::Buffer, {}, 0u, refCull.counts, fs.layout.refCullCounts,
                    kCountWords * 4ull, nullptr);
        }
    }
    if (opt.readback) {
        addCopy("readback.vis", CopyRecord::Image, vis.vis, VK_IMAGE_ASPECT_COLOR_BIT, {}, fs.layout.vis, kPixels * 8ull, nullptr);
        addCopy("readback.depth", CopyRecord::Image, vis.depth, atomic ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, {},
                fs.layout.depth, kPixels * 4ull, nullptr);
        if (vis.image64.valid()) {
            addCopy("readback.raw64", CopyRecord::Image, vis.image64, VK_IMAGE_ASPECT_COLOR_BIT, {}, fs.layout.raw64,
                    kPixels * 8ull, nullptr);
        }
        if (vis.buffer64.valid()) {
            addCopy("readback.raw64", CopyRecord::Buffer, {}, 0u, vis.buffer64, fs.layout.raw64, kPixels * 8ull, nullptr);
        }
        addCopy("readback.cull_counts", CopyRecord::Buffer, {}, 0u, cull.counts, fs.layout.cullCounts, kCountWords * 4ull,
                nullptr);
        if (meshPath) {
            addCopy("readback.hiz2", CopyRecord::Hiz, cull.hiz, 0u, {}, fs.layout.hiz2, 0u, &rig.culler);
            addCopy("readback.counts", CopyRecord::Buffer, {}, 0u, mesh.counts, fs.layout.counts, kMeshletCountWords * 4ull,
                    nullptr);
            addCopy("readback.groups", CopyRecord::Buffer, {}, 0u, mesh.groups, fs.layout.groups, rig.path.groupsBytes(), nullptr);
            if (mesh.results.valid()) {
                addCopy("readback.results", CopyRecord::Buffer, {}, 0u, mesh.results, fs.layout.results,
                        rig.path.resultsBytes(), nullptr);
            }
        }
        graph.addPass("readback.host", nullptr, nullptr).use(readback, rg::Access::HostRead);
    }
}

bool runFrame(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rig, opt)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    buildGraph(ctx, s, rig, graph, opt, fs);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.culler.collectRetired(ctx.serial);
    rig.refCuller.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.refVb.collectRetired(ctx.serial);
    s.gpu.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

// --- analysis -------------------------------------------------------------------------------------
template <typename T>
const T* rb(Context& ctx, u64 offset) {
    return reinterpret_cast<const T*>(static_cast<const u8*>(ctx.readback.mapped) + offset);
}

void copyPyramid(Context& ctx, const InstanceCuller& culler, u64 base, const ReadbackLayout& l, HizPyramid& out) {
    out.dim0 = culler.hizDim();
    out.mipCount = culler.hizMipCount();
    for (u32 m = 0; m < out.mipCount; ++m) {
        const u32 dim = std::max(1u, out.dim0 >> m);
        out.levels[m].resize(static_cast<usize>(dim) * dim);
        std::memcpy(out.levels[m].data(), rb<u8>(ctx, base + l.hizMip[m]), out.levels[m].size() * 4u);
    }
}

struct FrameReport {
    u32 covered = 0;
    u32 idBad = 0;   ///< pixels whose (instance, triangle) differ from the indirect path
    u32 ties = 0;    ///< ... of which near depth ties (the two surfaces within kDepthUlps at the pixel)
    u32 depthDiff = 0;   ///< pixels whose depth bits differ
    u32 depthMaxUlp = 0; ///< largest difference among pixels with the same id
    u32 rawDiff = 0;     ///< atomic words that differ ...
    u32 rawBad = 0;      ///< ... with the same id by more than kDepthUlps depth quanta
    u32 cullCountBad = 0;
    u32 slots = 0;
    u32 ambiguous = 0;
    u32 resultBad = 0;
    u32 hist[8] = {};
    u32 late = 0; ///< region-0 meshlets deferred by the early pass and drawn by the late pass
    u32 records[2] = {};
    u32 counters[kMeshletCountWords] = {};
    std::vector<u32> vis;
};

void analyse(Context& ctx, Scene& s, Rig& rig, const FrameState& fs, const View& view, bool prevValid,
             const HizPyramid& prevHiz, FrameReport& r) {
    const ReadbackLayout& l = fs.layout;
    const bool atomic = rig.vb.mode() == VisMode::Atomic64;
    const u32* vis = rb<u32>(ctx, l.vis);
    const u32* ref = rb<u32>(ctx, l.refVis);
    r.vis.assign(vis, vis + kPixels * 2u);
    // (1) ids: mesh path == indirect path on every pixel.
    std::vector<u32> mismatched;
    for (u32 p = 0; p < kPixels; ++p) {
        r.covered += vis[p * 2u] != kVisInvalid ? 1u : 0u;
        if (vis[p * 2u] != ref[p * 2u] || vis[p * 2u + 1u] != ref[p * 2u + 1u]) {
            mismatched.push_back(p);
        }
    }
    r.idBad = static_cast<u32>(mismatched.size());
    if (!mismatched.empty()) {
        // Classify: an exact depth tie between the two samples (draw-order dependent, meshlet_path.hpp).
        const VisSceneView sv = vis_scene_view(s.gpu, s.positions);
        std::vector<VisDecodeTexel> a, b;
        decode_reference(sv, view.viewProj.m, vis, kWidth, kHeight, a);
        decode_reference(sv, view.viewProj.m, ref, kWidth, kHeight, b);
        for (u32 p : mismatched) {
            r.ties += (a[p].flags == kVisDecodeOk && b[p].flags == kVisDecodeOk && a[p].depth >= 0.f && b[p].depth >= 0.f &&
                       ulpDistance(a[p].depth, b[p].depth) <= kDepthUlps)
                          ? 1u
                          : 0u;
        }
        for (usize i = 0; i < mismatched.size() && i < 4u; ++i) {
            const u32 p = mismatched[i];
            std::fprintf(stderr, "  pixel (%u, %u): mesh (%u, %u) d %.9g, indirect (%u, %u) d %.9g\n", p % kWidth, p / kWidth,
                         vis[p * 2u], vis[p * 2u + 1u], static_cast<f64>(a[p].depth), ref[p * 2u], ref[p * 2u + 1u],
                         static_cast<f64>(b[p].depth));
        }
    }
    // Depth: D32 (raster) or the exported R32F (atomic). Reported in ulps (see the Lavapipe note in
    // the header comment); pixels with the same id and a different depth are counted.
    {
        const u32* d = rb<u32>(ctx, l.depth);
        const u32* e = rb<u32>(ctx, l.refDepth);
        for (u32 p = 0; p < kPixels; ++p) {
            if (d[p] != e[p]) {
                ++r.depthDiff;
                const u32 ulps = d[p] > e[p] ? d[p] - e[p] : e[p] - d[p];
                const bool sameId = vis[p * 2u] == ref[p * 2u] && vis[p * 2u + 1u] == ref[p * 2u + 1u];
                if (sameId) {
                    r.depthMaxUlp = std::max(r.depthMaxUlp, ulps);
                }
            }
        }
    }
    if (atomic) {
        const u64* raw = rb<u64>(ctx, l.raw64);
        const u64* refRaw = rb<u64>(ctx, l.refRaw64);
        for (u32 p = 0; p < kPixels; ++p) {
            if (raw[p] == refRaw[p]) {
                continue;
            }
            ++r.rawDiff;
            const u32 qa = vis64_depth_bits(raw[p]);
            const u32 qb = vis64_depth_bits(refRaw[p]);
            // A different id is an id mismatch (classified above); the same id may differ in depth by
            // kDepthUlps quanta (the 2^-24 quantum is the f32 ulp in [0.5, 1)).
            const bool sameId = (raw[p] & 0xFFFFFFFFFFull) == (refRaw[p] & 0xFFFFFFFFFFull);
            r.rawBad += !sameId || (qa > qb ? qa - qb : qb - qa) <= kDepthUlps ? 0u : 1u;
        }
    }
    const u32* cc = rb<u32>(ctx, l.cullCounts);
    const u32* rc = rb<u32>(ctx, l.refCullCounts);
    r.cullCountBad = (cc[kCountPhase1Draws] != rc[kCountPhase1Draws] || cc[kCountPhase2Draws] != rc[kCountPhase2Draws]) ? 1u : 0u;
    // Per-meshlet parity needs last frame's pyramid whenever the early pass used it.
    if (!rig.path.usesMeshShaders() || (!prevValid && (rig.path.constants().cull.flags & kCullHistoryValid) != 0u)) {
        return;
    }
    // (2) per-meshlet results == the CPU reference kernel.
    std::memcpy(r.counters, rb<u32>(ctx, l.counts), sizeof(r.counters));
    HizPyramid hiz1;
    copyPyramid(ctx, rig.culler, l.hiz1, l, hiz1);
    const MeshletGroup* groups = rb<MeshletGroup>(ctx, l.groups);
    const u32* results = rb<u32>(ctx, l.results);
    const u32 cap = rig.path.groupCapacity();
    MeshletReferenceInput in{};
    in.scene = scene_spans(s.gpu);
    in.meshlets = {s.meshlets.data(), static_cast<u32>(s.meshlets.size())};
    in.constants = &rig.path.constants();
    in.prevHiz = prevValid ? prevHiz.view() : culling::cull_kernel::HizLevels{};
    in.hiz = hiz1.view();
    for (u32 region = 0; region < 2u; ++region) {
        const u32 n = std::min(r.counters[region == 0u ? kMeshletCountTasks0 : kMeshletCountTasks1], cap);
        r.records[region] = n;
        in.groups = {groups + static_cast<usize>(region) * cap, n};
        in.region = region;
        MeshletParityReference pr;
        meshlet_cull_reference_parity(in, pr, fuse::kernel::Backend::CpuParallel);
        for (u32 i = 0; i < n * kMeshletTaskGroup; ++i) {
            const u32 gpu = results[static_cast<usize>(region) * cap * kMeshletTaskGroup + i];
            ++r.hist[gpu < 8u ? gpu : 0u];
            r.late += region == 0u && gpu == kMeshletResultPhase2Drawn ? 1u : 0u;
            ++r.slots;
            if (pr.ambiguous[i] != 0u) {
                ++r.ambiguous;
                continue;
            }
            if (gpu != pr.results[i]) {
                if (r.resultBad < 4u) {
                    const MeshletGroup g = in.groups[i / kMeshletTaskGroup];
                    std::fprintf(stderr, "  region %u record %u lane %u (instance %u, meshlet %u): GPU %u, CPU %u\n", region,
                                 i / kMeshletTaskGroup, i % kMeshletTaskGroup, g.instance, g.firstMeshlet + i % kMeshletTaskGroup,
                                 gpu, pr.results[i]);
                }
                ++r.resultBad;
            }
        }
    }
}

int runIds(Context& ctx, VisMode mode, VisAtomicTarget target) {
    if (!queryMeshletCapabilities(ctx.device.get()).meshPath) {
        std::printf("SKIP: no mesh-shader path on this device (%s); the T0 fallback is covered by --mode masked_t0\n",
                    queryMeshletCapabilities(ctx.device.get()).reason);
        return kSkip;
    }
    std::vector<std::vector<u32>> images;
    std::vector<const char*> languages;
    for (const MeshletKernelLanguage language : {MeshletKernelLanguage::Slang, MeshletKernelLanguage::Glsl}) {
        const char* want = language == MeshletKernelLanguage::Slang ? "slang" : "glsl";
        Rig rig;
        const int rc = initRig(ctx, rig, mode, target, language, MeshletPathMode::MeshShader, true);
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init (%s)\n", want);
            return 1;
        }
        if (rc == 0) {
            std::printf("  language %s: not built, skipped\n", want);
            rig.destroy();
            continue;
        }
        Scene s;
        if (!buildScene(ctx, s, kInstances)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        std::printf("  kernels: %s, mode %s%s, %u instances, meshlets per mesh (%zu, %zu, %zu), capacity %u records\n",
                    rig.language, mode == VisMode::Raster ? "raster" : "atomic64",
                    mode == VisMode::Raster ? "" : (rig.vb.atomicTarget() == VisAtomicTarget::Image ? " (image)" : " (buffer)"),
                    kInstances, s.meshes[0].meshlets.size(), s.meshes[1].meshlets.size(), s.meshes[2].meshlets.size(),
                    rig.path.groupCapacity());
        std::printf("  frame cone covered id-bad ties dep-diff ulp raw-diff raw-bad cnt-bad | rec0 rec1   slots  ambig res-bad | "
                    "frust  cone    p1 p2(late+ph2) occl | ovf skip big\n");
        rg::Graph graph;
        FrameState fs;
        fs.layout = makeLayout(rig.culler.hizDim(), rig.culler.hizMipCount());
        if (fs.layout.end > kReadbackBytes) {
            std::fprintf(stderr, "FAIL: readback layout too large\n");
            return 1;
        }
        HizPyramid prevHiz;
        bool prevValid = false;
        u32 total[8] = {};
        u32 late = 0;
        FrameReport r;
        for (u32 frame = 0; frame < kFrames; ++frame) {
            beginSceneFrame(ctx, s);
            if (frame > 0u) {
                moveObjects(s, frame, 1u);
            }
            FrameOptions opt{};
            opt.view = frameView(frame);
            opt.cone = (frame & 1u) != 0u;
            if (!runFrame(ctx, s, rig, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            r = FrameReport{};
            analyse(ctx, s, rig, fs, opt.view, prevValid && rig.culler.historyValid(), prevHiz, r);
            for (u32 k = 0; k < 8u; ++k) {
                total[k] += r.hist[k];
            }
            late += r.late;
            std::printf("  %5u %4s %7u %6u %4u %8u %3u %8u %7u %7u | %4u %4u %7u %6u %7u | %5u %5u %5u %5u %5u | %3u %4u %3u\n",
                        frame, opt.cone ? "on" : "off", r.covered, r.idBad, r.ties, r.depthDiff, r.depthMaxUlp, r.rawDiff, r.rawBad,
                        r.cullCountBad, r.records[0],
                        r.records[1], r.slots, r.ambiguous, r.resultBad, r.hist[kMeshletResultFrustumCulled],
                        r.hist[kMeshletResultConeCulled], r.hist[kMeshletResultPhase1Drawn], r.hist[kMeshletResultPhase2Drawn],
                        r.hist[kMeshletResultOccluded], r.counters[kMeshletCountOverflow], r.counters[kMeshletCountSkipped],
                        r.counters[kMeshletCountOversize]);
            expect(r.covered > kPixels / 4u, "the scene covers the view");
            expect(r.idBad == r.ties, "every (instance, triangle) difference is a near depth tie (<= 8 ulp)");
            expect(r.ties * 2000u <= r.covered, "near-tie flips on at most 0.05% of the covered pixels");
            expect(r.depthMaxUlp <= kDepthUlps, "mesh-path depth within 1 ulp of the indirect path's (same id)");
            expect(r.rawBad == 0u, "mesh-path 64-bit words == indirect path's up to kDepthUlps depth quanta (same id)");
            expect(r.cullCountBad == 0u, "both rigs' cullers made the same instance decisions");
            expect(r.resultBad == 0u, "per-meshlet results == CPU reference kernel");
            expect(r.ambiguous * 100u <= r.slots, "at most 1% of the meshlets on a test boundary");
            expect(r.counters[kMeshletCountOverflow] == 0u && r.counters[kMeshletCountSkipped] == 0u &&
                       r.counters[kMeshletCountOversize] == 0u,
                   "no overflow, skipped instance or oversize meshlet");
            expect(r.hist[kMeshletResultDeferred] == 0u, "no meshlet left deferred after the late pass");
            copyPyramid(ctx, rig.culler, fs.layout.hiz2, fs.layout, prevHiz);
            prevValid = true;
        }
        std::printf("  %s over %u frames: frustum %u, cone %u, early %u, late %u, phase 2 %u, occluded %u\n", rig.language,
                    kFrames, total[kMeshletResultFrustumCulled], total[kMeshletResultConeCulled], total[kMeshletResultPhase1Drawn],
                    late, total[kMeshletResultPhase2Drawn] - late, total[kMeshletResultOccluded]);
        expect(late > 0u, "the late pass drew deferred (disoccluded) meshlets");
        expect(total[kMeshletResultFrustumCulled] > 0u && total[kMeshletResultConeCulled] > 0u &&
                   total[kMeshletResultPhase1Drawn] > 0u && total[kMeshletResultPhase2Drawn] > 0u &&
                   total[kMeshletResultOccluded] > 0u,
               "meshlets frustum-, cone- and Hi-Z-culled and drawn in both phases over the run");
        images.push_back(r.vis);
        languages.push_back(rig.language);
        s.gpu.destroy();
        rig.destroy();
    }
    if (images.empty()) {
        std::printf("SKIP: no mesh-shader path kernels built\n");
        return kSkip;
    }
    if (images.size() == 2u) {
        expect(images[0] == images[1], "Slang == GLSL visibility images");
        std::printf("  %s vs %s: visibility %s\n", languages[0], languages[1], images[0] == images[1] ? "identical" : "DIFFERENT");
    }
    return 0;
}

// --- masked T0 ------------------------------------------------------------------------------------
int runMaskedT0(Context& ctx) {
    const RendererCaps& caps = ctx.device->info().caps;
    std::printf("masked device: %s\n", caps.summary().c_str());
    expect(!caps.meshShader && !caps.taskShader, "maxTier T0 masks the mesh / task shader features");
    {
        Rig probe;
        expect(initRig(ctx, probe, VisMode::Raster, VisAtomicTarget::Auto, MeshletKernelLanguage::Auto, MeshletPathMode::MeshShader,
                       false) == 0,
               "MeshShader mode fails on a T0 device");
        probe.destroy();
    }
    Rig rig;
    if (initRig(ctx, rig, VisMode::Raster, VisAtomicTarget::Auto, MeshletKernelLanguage::Auto, MeshletPathMode::Auto, false) != 1) {
        std::fprintf(stderr, "FAIL: rig init\n");
        return 1;
    }
    std::printf("  MeshletPath (Auto): %s, reason: %s\n", rig.path.usesMeshShaders() ? "mesh shaders" : "WP-1.4 indirect fallback",
                rig.path.fallbackReason());
    expect(!rig.path.usesMeshShaders(), "Auto selects the fallback on T0");
    expect(std::strstr(rig.path.fallbackReason(), "tier") != nullptr, "fallback reason names the tier");
    Scene s;
    if (!buildScene(ctx, s, kInstances)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    fs.layout = makeLayout(rig.culler.hizDim(), rig.culler.hizMipCount());
    HizPyramid none;
    for (u32 frame = 0; frame < 3u; ++frame) {
        beginSceneFrame(ctx, s);
        moveObjects(s, frame, 1u);
        FrameOptions opt{};
        opt.view = frameView(frame);
        opt.results = false;
        if (!runFrame(ctx, s, rig, graph, opt, fs)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
        FrameReport r;
        analyse(ctx, s, rig, fs, opt.view, false, none, r);
        std::printf("  frame %u: %u covered, %u id mismatches vs VisBuffer::addCulledFrame, depth %s, draw passes %u\n", frame,
                    r.covered, r.idBad, r.depthDiff == 0u ? "identical" : "DIFFERENT", rig.path.stats().drawPasses);
        expect(r.covered > kPixels / 4u && r.idBad == 0u && r.depthDiff == 0u && r.cullCountBad == 0u,
               "fallback frame == the WP-1.4 path bit for bit");
        expect(rig.path.stats().drawPasses == 2u, "fallback records the two vis.phase draws");
    }
    s.gpu.destroy();
    rig.destroy();
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "meshlet.", 8) == 0 || std::strncmp(name, "vis.", 4) == 0 ||
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
    if (!queryMeshletCapabilities(ctx.device.get()).meshPath) {
        std::printf("SKIP: no mesh-shader path on this device\n");
        return kSkip;
    }
    Rig rig;
    if (initRig(ctx, rig, VisMode::Raster, VisAtomicTarget::Auto, MeshletKernelLanguage::Auto, MeshletPathMode::MeshShader, true) !=
        1) {
        std::printf("SKIP: no mesh-shader path kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, kInstances)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    fs.layout = makeLayout(rig.culler.hizDim(), rig.culler.hizMipCount());
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long pathSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    HizPyramid none;
    FrameReport r;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool last = frame + 1u == kTotal;
        const bool measure = countAllocations && frame >= kWarmup && !last;
        beginSceneFrame(ctx, s);
        moveObjects(s, frame, 10u); // ~1% of the instances
        s.gpu.commit();
        ctx.upload.flush();
        FrameOptions opt{};
        opt.view = frameView(frame % 8u);
        opt.cone = (frame & 1u) != 0u;
        opt.withReference = last;
        opt.readback = last;
        opt.results = last;
        t_allocations = 0;
        t_count = measure;
        const bool began = beginFrame(ctx, s, rig, opt);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, s, rig, graph, opt, fs);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rig.culler.collectRetired(ctx.serial);
        rig.refCuller.collectRetired(ctx.serial);
        rig.vb.collectRetired(ctx.serial);
        rig.refVb.collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            pathSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (last) {
            analyse(ctx, s, rig, fs, opt.view, false, none, r);
        }
    }
    // The reference rig starts cold in the last frame (no history) while the mesh rig has 79 frames of
    // history: the final images must still agree (occlusion is conservative).
    expect(r.idBad == 0u && r.depthMaxUlp <= kDepthUlps, "last frame: mesh path == indirect path");
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, ~1%% moved)\n"
                    "  MeshletPath/VisBuffer/culler beginFrame + meshlet.* / vis.* / cull.* pass callbacks: %llu operator-new "
                    "calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis and meshlet imports + passes): %llu\n",
                    kTotal - kWarmup - 1u, kInstances, pathSide, callbacks, build);
        expect(pathSide == 0u, "mesh path makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the meshlet passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: last frame %u covered, %u id mismatches\n", r.covered,
                    r.idBad);
    }
    s.gpu.destroy();
    rig.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "raster";
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
        const int setupRc =
            setup(ctx, backend == "buffer", true, mode == "masked_t0" ? RenderTier::T0 : kMaxRenderTier);
        if (setupRc != 0) {
            return setupRc;
        }
        const VisCapabilities caps = queryVisCapabilities(ctx.device.get());
        if (mode == "raster") {
            rc = runIds(ctx, VisMode::Raster, VisAtomicTarget::Auto);
        } else if (mode == "atomic_image") {
            if (!caps.atomicImage) {
                std::printf("SKIP: no R64_UINT image atomics (%s)\n", caps.atomicReason);
                return kSkip;
            }
            rc = runIds(ctx, VisMode::Atomic64, VisAtomicTarget::Image);
        } else if (mode == "atomic_buffer") {
            if (!caps.atomicBuffer) {
                std::printf("SKIP: no 64-bit buffer atomics (%s)\n", caps.atomicReason);
                return kSkip;
            }
            rc = runIds(ctx, VisMode::Atomic64, VisAtomicTarget::Buffer);
        } else if (mode == "masked_t0") {
            rc = runMaskedT0(ctx);
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
