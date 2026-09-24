// WP-3.1 virtual shadow map Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_vsm_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, VisBuffer::addCulledFrame
// (WP-1.3 cull + WP-1.4 raster depth), then for each kernel language built (Slang, GLSL) one
// VirtualShadowMap::addFrame over the visibility buffer's depth and the GPU scene:
//   vsm.reset [+ vsm.init] + vsm.mark        page marking from depth
//   [vsm.bounds_clear] + vsm.invalidate      static-page invalidation from GPU-scene bounds changes
//   vsm.update + vsm.alloc + vsm.render      page table, physical pool (age-bucketed LRU), render list
//   vsm.clear                                clears the render list's physical pages (dispatch indirect)
// and read-back copies of the depth, the page table, the physical metadata and the work buffer. The CPU
// side replays the frame on the read-back depth and the scene's CPU mirror:
//   marking      markReference (vsm_kernel.hpp) == the GPU request bits, bit for bit
//   invalidation VsmInvalidationReference (bounds kernel + footprints) -> invalidation mask
//   allocation   core_logic VsmPagePool::update (the page model) on the same masks: page table and owners /
//                lastUsed equal word for word, every statistic equal, render list equal as a set
// and Slang == GLSL (every word).
//
//   --mode parity      Scene: ground + 72 objects, 256 x 192, 16 levels (4 m level 0), 256 physical pages,
//                      8 frames: camera moves (window scrolls), objects move, one removed and re-added, the
//                      light rotates once, mark radius 1.5 texels. Frame 0 also reads the pool back: every
//                      page of the render list holds the clear value
//   --mode cache       static scene: frames 1..3 render 0 pages; one object moves: only pages under its old /
//                      new footprint re-render (subset of the CPU invalidation mask, > 0), the rest stay
//                      cached; static again: 0 pages; the object is removed: its footprint re-renders
//   --mode evict       16 physical pages, 8 levels, a moving camera: evictions and failed needs every frame,
//                      still equal to the model
//   --mode zero_alloc  64 steady-state frames (objects moving): 0 operator-new calls in
//                      VirtualShadowMap::beginFrame, the vsm.* pass callbacks and the whole graph build
//                      (validated run first; validation off for the count)
//   --backend set | buffer bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_resolve_scene.hpp"

#include <bit>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/shadow/vsm/virtual_shadow_map.hpp>
#include <fuse/renderer/shadow/vsm/vsm_clipmap.hpp>
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
using namespace fuse::renderer::visbuffer;
using namespace fuse::renderer::vsm;
namespace core_logic = fuse::core_logic;
using fuse::f32;
using fuse::f64;
using fuse::s32;
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
constexpr u32 kObjects = 72;
constexpr f32 kFovY = 1.1f;
constexpr f32 kNear = 0.3f;
constexpr f32 kFar = 150.f;
constexpr u32 kLanguages = 2; // Slang, GLSL

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

// --- math (column-major, Vulkan clip space, forward depth) -----------------------------------------
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

/// Inverse in double, rounded to float.
Mat4 inverse(const Mat4& in) {
    f64 a[4][8];
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            a[r][c] = in.m[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (u32 c = 0; c < 4; ++c) {
        u32 piv = c;
        for (u32 r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) {
                piv = r;
            }
        }
        for (u32 k = 0; k < 8; ++k) {
            std::swap(a[c][k], a[piv][k]);
        }
        const f64 d = a[c][c];
        for (u32 k = 0; k < 8; ++k) {
            a[c][k] /= d;
        }
        for (u32 r = 0; r < 4; ++r) {
            if (r != c) {
                const f64 f = a[r][c];
                for (u32 k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    Mat4 out{};
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            out.m[c * 4 + r] = static_cast<f32>(a[r][c + 4]);
        }
    }
    return out;
}

struct FrameCamera {
    Mat4 viewProj{};
    Mat4 invViewProj{};
    f32 eye[3] = {};
};

FrameCamera frameCamera(f32 t) {
    const f32 eye[3] = {-1.f + 0.35f * t, 2.4f - 0.05f * t, 3.f - 0.3f * t};
    const f32 at[3] = {0.3f * t - 0.5f, -0.6f, -16.f};
    FrameCamera c{};
    c.viewProj = mul(perspective(kFovY, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), kNear, kFar), lookAt(eye, at));
    c.invViewProj = inverse(c.viewProj);
    std::memcpy(c.eye, eye, sizeof(eye));
    return c;
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
    instanceDesc.appName = "fuse_rp_vsm";
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
    if (!visCaps.raster) {
        std::printf("SKIP: visibility buffer unsupported: %s\n", visCaps.rasterReason);
        return kSkip;
    }
    const VsmCapabilities vsmCaps = queryVsmCapabilities(ctx.device.get());
    if (!vsmCaps.vsm) {
        std::printf("SKIP: VSM unsupported: %s\n", vsmCaps.reason);
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
    stagingDesc.name = "rp_vsm.staging";
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

// --- scene ----------------------------------------------------------------------------------------
struct Scene {
    GpuScene gpu;
    std::vector<fuse::renderer::geometry::MeshletMesh> meshes;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
    InstanceDesc removedDesc{};
    bool removed = false;
};

bool buildScene(Context& ctx, Scene& s) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 32; // grows during the build: the VSM bounds buffer follows
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    const mr_test::SourceMesh sources[4] = {mr_test::uvSphere(12, 16, 1.f), mr_test::torus(16, 8, 1.f, 0.35f), mr_test::box(),
                                            mr_test::plane(8, 80.f, 8.f)};
    s.meshes.resize(4);
    for (u32 i = 0; i < 4u; ++i) {
        if (!mr_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    std::mt19937 rng(4321);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    InstanceDesc ground{};
    ground.mesh = 3;
    ground.transform = place(0.f, -1.8f, -20.f, 1.f, 1.f, 1.f, 0.35f);
    s.handles.push_back(s.gpu.addInstance(ground));
    for (u32 i = 0; i < kObjects; ++i) {
        InstanceDesc id{};
        id.mesh = i % 3u;
        const f32 sc = 0.4f + 0.5f * (u(rng) * 0.5f + 0.5f);
        id.transform = place(u(rng) * 14.f, -0.6f + 3.f * (u(rng) * 0.5f + 0.5f), -4.f - 40.f * (u(rng) * 0.5f + 0.5f), sc, sc,
                             sc, u(rng) * 3.f);
        if (i % 9u == 4u) {
            id.flags &= ~kInstanceCastShadow; // receivers only: never invalidate
        }
        s.handles.push_back(s.gpu.addInstance(id));
        if (i % 6u == 1u) {
            s.movers.push_back(static_cast<u32>(s.handles.size() - 1u));
        }
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

void moveObject(Scene& s, u32 slot, f32 dx, f32 dz) {
    GpuTransform t = s.gpu.transform(slot);
    t.rows[0][3] += dx;
    t.rows[2][3] += dz;
    s.gpu.setTransform(s.handles[slot], t);
}

// --- rig ------------------------------------------------------------------------------------------
struct RigDesc {
    u32 levels = 16;
    f32 extent = 4.f;
    u32 poolX = 16;
    u32 poolY = 16;
    f32 markRadius = 1.5f;
    f32 texelsPerPixel = 4.f; ///< finer than the 256 x 192 screen: more pages and page boundaries
};

struct Rig {
    InstanceCuller culler;
    VisBuffer vb;
    VirtualShadowMap vsm[kLanguages];
    bool built[kLanguages] = {};
    const char* language[kLanguages] = {"slang", "glsl"};
};

int initRig(Context& ctx, Rig& rig, const RigDesc& rd) {
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = kObjects + 64u;
    if (!rig.culler.init(cd) || !rig.culler.setResolution(kWidth, kHeight)) {
        return -1;
    }
    VisBufferDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.width = kWidth;
    vd.height = kHeight;
    vd.mode = VisMode::Raster;
    if (!rig.vb.init(vd)) {
        return -1;
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
        d.instanceCapacity = 16; // grows at the first frame (bounds buffer rebuild path)
        d.language = k == 0u ? VsmKernelLanguage::Slang : VsmKernelLanguage::Glsl;
        rig.built[k] = rig.vsm[k].init(d);
        built += rig.built[k] ? 1u : 0u;
        std::printf("  vsm kernels %s: %s\n", rig.language[k], rig.built[k] ? "built" : "not built, skipped");
    }
    return built > 0u ? 1 : 0;
}

void destroyRig(Rig& rig) {
    for (VirtualShadowMap& v : rig.vsm) {
        v.destroy();
    }
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

struct ReadbackLayout {
    u64 depth = 0;
    u64 pageTable[kLanguages] = {};
    u64 physMeta[kLanguages] = {};
    u64 work[kLanguages] = {};
    u64 pool = 0; ///< language 0's pool, when requested
    u64 end = 0;
};

ReadbackLayout makeLayout(const Rig& rig, bool pool) {
    ReadbackLayout l{};
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    l.depth = take(static_cast<u64>(kPixels) * 4u);
    for (u32 k = 0; k < kLanguages; ++k) {
        const VirtualShadowMap& v = rig.vsm[rig.built[k] ? k : (k ^ 1u)];
        l.pageTable[k] = take(v.pageTableBuffer().desc.size);
        l.physMeta[k] = take(v.physMetaBuffer().desc.size);
        l.work[k] = take(v.workBuffer().desc.size);
    }
    if (pool) {
        const VirtualShadowMap& v = rig.vsm[rig.built[0] ? 0u : 1u];
        l.pool = take(static_cast<u64>(v.poolImage().desc.width) * v.poolImage().desc.height * 4u);
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
    d.name = "rp_vsm.readback";
    return ctx.allocator->createBuffer(d, ctx.readback) && ctx.readback.mapped != nullptr;
}

struct FrameState {
    CopyRecord copies[16];
    u32 copyCount = 0;
    ReadbackLayout layout{};
};

VsmFrameDesc vsmFrame(const Scene& s, const Rig& rig, const FrameCamera& cam, const f32 light[3]) {
    VsmFrameDesc f{};
    std::memcpy(f.view.lightDirection, light, sizeof(f.view.lightDirection));
    std::memcpy(f.view.cameraPosition, cam.eye, sizeof(f.view.cameraPosition));
    std::memcpy(f.view.invViewProj, cam.invViewProj.m, sizeof(f.view.invViewProj));
    f.view.depthWidth = kWidth;
    f.view.depthHeight = kHeight;
    f.view.pixelSpread = 2.f * std::tan(kFovY * 0.5f) / static_cast<f32>(kHeight);
    f.depthHandle = rig.vb.depthSampledHandle();
    f.scene = s.gpu.headerHandle();
    f.instanceCount = s.gpu.instanceHighWater();
    return f;
}

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const FrameCamera& cam, const f32 light[3], bool vsmToo = true) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, cam.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    if (vsmToo) {
        const VsmFrameDesc f = vsmFrame(s, rig, cam, light);
        for (u32 k = 0; k < kLanguages; ++k) {
            if (rig.built[k]) {
                ok = rig.vsm[k].beginFrame(ctx.serial, f) && ok;
            }
        }
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, FrameState& fs, bool readback, bool pool) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), rig.culler, cull);
    VsmGraphRefs vsm[kLanguages]{};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            vsm[k] = rig.vsm[k].importInto(graph);
            rig.vsm[k].addFrame(graph, vsm[k], vis.depth, sceneRefs, true);
        }
    }
    if (!readback) {
        return;
    }
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_vsm.readback"});
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
    addCopy(CopyRecord::Depth, vis.depth, {}, fs.layout.depth, static_cast<u64>(kPixels) * 4u, kWidth, kHeight);
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const VirtualShadowMap& v = rig.vsm[k];
        addCopy(CopyRecord::Buffer, {}, vsm[k].pageTable, fs.layout.pageTable[k], v.pageTableBuffer().desc.size, 0, 0);
        addCopy(CopyRecord::Buffer, {}, vsm[k].physMeta, fs.layout.physMeta[k], v.physMetaBuffer().desc.size, 0, 0);
        addCopy(CopyRecord::Buffer, {}, vsm[k].work, fs.layout.work[k], v.workBuffer().desc.size, 0, 0);
    }
    if (pool) {
        const u32 k = rig.built[0] ? 0u : 1u;
        const Texture& img = rig.vsm[k].poolImage();
        addCopy(CopyRecord::Image, vsm[k].pool, {}, fs.layout.pool, static_cast<u64>(img.desc.width) * img.desc.height * 4u,
                img.desc.width, img.desc.height);
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

bool runFrame(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameCamera& cam, const f32 light[3], FrameState& fs,
              bool readback, bool pool) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rig, cam, light)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    if (readback) {
        fs.layout = makeLayout(rig, pool);
        if (!ensureReadback(ctx, fs.layout)) {
            return false;
        }
    }
    buildGraph(ctx, s, rig, graph, fs, readback, pool);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    for (VirtualShadowMap& v : rig.vsm) {
        v.collectRetired(ctx.serial);
    }
    s.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

// --- CPU replay -----------------------------------------------------------------------------------
const u8* rbAt(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

struct GpuVsm {
    std::vector<u32> pte;
    std::vector<u32> meta;
    std::vector<u32> work;
};

void readVsm(Context& ctx, const FrameState& fs, const VirtualShadowMap& v, u32 k, GpuVsm& out) {
    out.pte.resize(v.pageTableBuffer().desc.size / 4u);
    out.meta.resize(v.physMetaBuffer().desc.size / 4u);
    out.work.resize(v.workBuffer().desc.size / 4u);
    std::memcpy(out.pte.data(), rbAt(ctx, fs.layout.pageTable[k]), out.pte.size() * 4u);
    std::memcpy(out.meta.data(), rbAt(ctx, fs.layout.physMeta[k]), out.meta.size() * 4u);
    std::memcpy(out.work.data(), rbAt(ctx, fs.layout.work[k]), out.work.size() * 4u);
}

/// The CPU side of one VSM (per language): references + page model.
struct Replay {
    VsmInvalidationReference inval;
    std::unique_ptr<VsmPageModel> model{new VsmPageModel()};
    std::vector<u32> request, invalid, scratch;
    std::vector<u32> prevPte; ///< model page table before this frame's update
    u32 boundsRebuilds = 0;
    core_logic::VsmPageStats stats{};
};

struct FrameReport {
    u32 requestBits = 0;
    bool markEqual = false;
    bool pteEqual = false;
    bool metaEqual = false;
    bool statsEqual = false;
    bool listEqual = false;
    u32 changedSlots = 0;
    u32 invalidBits = 0;
    core_logic::VsmPageStats gpu{};
    std::vector<u32> renderList; ///< GPU render list, sorted virtual pages
};

u32 popcount(const std::vector<u32>& w) {
    u32 n = 0;
    for (const u32 v : w) {
        n += static_cast<u32>(std::popcount(v));
    }
    return n;
}

void replay(Context& ctx, Scene& s, const VirtualShadowMap& v, const FrameState& fs, u32 k, Replay& r, FrameReport& rep,
            GpuVsm& g) {
    const VsmFrameConstants& c = v.constants();
    readVsm(ctx, fs, v, k, g);
    std::vector<f32> depth(kPixels);
    std::memcpy(depth.data(), rbAt(ctx, fs.layout.depth), static_cast<usize>(kPixels) * 4u);
    // Marking.
    r.request.assign(c.requestWords, 0u);
    markReference(c, depth.data(), kWidth, kHeight, r.request.data(), fuse::kernel::Backend::CpuParallel, r.scratch);
    const u32* gpuRequest = g.work.data() + c.offRequest;
    rep.markEqual = std::equal(r.request.begin(), r.request.end(), gpuRequest);
    rep.requestBits = popcount(r.request);
    // Invalidation (mirror the GPU's bounds history restarts).
    if (v.stats().boundsRebuilds != r.boundsRebuilds) {
        r.inval.reset();
        r.boundsRebuilds = v.stats().boundsRebuilds;
    }
    const u32 n = s.gpu.instanceHighWater();
    std::vector<GpuInstance> inst(n);
    std::vector<GpuTransform> xf(n);
    for (u32 i = 0; i < n; ++i) {
        inst[i] = s.gpu.instance(i);
        xf[i] = s.gpu.transform(i);
    }
    std::vector<GpuMesh> meshes(s.gpu.meshCount());
    for (u32 i = 0; i < meshes.size(); ++i) {
        meshes[i] = s.gpu.mesh(i);
    }
    r.invalid.assign(c.requestWords, 0u);
    rep.changedSlots = r.inval.run(c, inst.data(), xf.data(), n, meshes.data(), static_cast<u32>(meshes.size()), r.invalid.data());
    rep.invalidBits = popcount(r.invalid);
    // Allocation: the model on the CPU masks.
    r.prevPte.resize(c.virtualPages);
    for (u32 i = 0; i < c.virtualPages; ++i) {
        r.prevPte[i] = r.model->pte(i);
    }
    const core_logic::ClStatus st = r.model->update(frameInput(c), r.request.data(), r.invalid.data(), r.stats);
    expect(st == core_logic::ClStatus::Ok, "model update");
    rep.pteEqual = true;
    for (u32 i = 0; i < c.virtualPages; ++i) {
        rep.pteEqual = rep.pteEqual && g.pte[i] == r.model->pte(i);
    }
    rep.metaEqual = true;
    for (u32 p = 0; p < c.physPages; ++p) {
        const u32 owner = g.meta[p * 2u];
        rep.metaEqual = rep.metaEqual && owner == r.model->owner(p) &&
                        (owner == kPageNone || g.meta[p * 2u + 1u] == r.model->last_used(p));
    }
    core_logic::VsmPageStats& gs = rep.gpu;
    gs.toRender = g.work[kCounterRenderCount];
    gs.requested = g.work[kCounterRequested];
    gs.alreadyMapped = g.work[kCounterAlreadyMapped];
    gs.needed = g.work[kCounterNeeded];
    gs.candidates = g.work[kCounterCandidates];
    gs.allocated = g.work[kCounterAllocated];
    gs.evicted = g.work[kCounterEvicted];
    gs.failed = g.work[kCounterFailed];
    gs.invalidated = g.work[kCounterInvalidated];
    gs.scrolled = g.work[kCounterScrolled];
    rep.statsEqual = std::memcmp(&gs, &r.stats, sizeof(gs)) == 0 && g.work[1] == 1u && g.work[2] == 1u;
    rep.renderList.clear();
    bool physOk = true;
    for (u32 i = 0; i < gs.toRender && i < c.physPages; ++i) {
        const u32 vp = g.work[c.offRenderList + i * 2u];
        const u32 pp = g.work[c.offRenderList + i * 2u + 1u];
        rep.renderList.push_back(vp);
        physOk = physOk && vp < c.virtualPages && (r.model->pte(vp) & kPtePhysMask) == pp;
    }
    std::sort(rep.renderList.begin(), rep.renderList.end());
    std::vector<u32> modelList;
    for (u32 i = 0; i < r.model->render_count(); ++i) {
        modelList.push_back(r.model->render_page(i));
    }
    rep.listEqual = physOk && modelList == rep.renderList;
}

void printStats(const char* tag, const core_logic::VsmPageStats& s) {
    std::printf("%s req %u mapped %u need %u cand %u alloc %u evict %u fail %u render %u inval %u scroll %u\n", tag, s.requested,
                s.alreadyMapped, s.needed, s.candidates, s.allocated, s.evicted, s.failed, s.toRender, s.invalidated, s.scrolled);
}

/// Checks one frame for every language; returns the reports.
void checkFrame(Context& ctx, Scene& s, Rig& rig, const FrameState& fs, Replay replays[kLanguages], FrameReport reps[kLanguages],
                u32 frame) {
    GpuVsm g[kLanguages];
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        FrameReport& rep = reps[k];
        replay(ctx, s, rig.vsm[k], fs, k, replays[k], rep, g[k]);
        char tag[64];
        std::snprintf(tag, sizeof(tag), "  frame %u %-5s bits %4u slots %3u inval-bits %5u |", frame, rig.language[k],
                      rep.requestBits, rep.changedSlots, rep.invalidBits);
        printStats(tag, rep.gpu);
        expect(rep.markEqual, "GPU request bits == markReference on the same depth");
        expect(rep.pteEqual, "GPU page table == the page model");
        expect(rep.metaEqual, "GPU physical owners / lastUsed == the page model");
        expect(rep.statsEqual, "GPU statistics == the page model");
        expect(rep.listEqual, "GPU render list == the page model's (as a set)");
        if (!rep.statsEqual) {
            printStats("    model:", replays[k].stats);
        }
    }
    if (rig.built[0] && rig.built[1]) {
        const u32 vp = rig.vsm[0].constants().virtualPages;
        const bool same = std::equal(g[0].pte.begin(), g[0].pte.begin() + vp, g[1].pte.begin()) && g[0].meta == g[1].meta &&
                          reps[0].renderList == reps[1].renderList && std::memcmp(&reps[0].gpu, &reps[1].gpu, sizeof(reps[0].gpu)) == 0;
        expect(same, "Slang == GLSL (page table, physical metadata, statistics, render list)");
    }
}

int prepare(Context& ctx, Scene& s, Rig& rig, const RigDesc& rd, Replay replays[kLanguages]) {
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    const int rigRc = initRig(ctx, rig, rd);
    if (rigRc < 0) {
        std::fprintf(stderr, "FAIL: rig init\n");
        return 1;
    }
    if (rigRc == 0) {
        std::printf("SKIP: no VSM kernel built\n");
        destroyRig(rig);
        return kSkip;
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            expect(replays[k].model->reset(rd.levels, rd.poolX * rd.poolY) == core_logic::ClStatus::Ok, "model reset");
        }
    }
    return 0;
}

const f32 kSun[3] = {0.35f, -1.f, -0.25f};
const f32 kSun2[3] = {0.3f, -1.f, -0.3f};

// --- modes ----------------------------------------------------------------------------------------
int runParity(Context& ctx) {
    Scene s;
    Rig rig;
    Replay replays[kLanguages];
    const RigDesc rd{};
    const int rc = prepare(ctx, s, rig, rd, replays);
    if (rc != 0) {
        return rc;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kFrames = 8;
    u32 totalRendered = 0, totalScrolled = 0, totalInvalidated = 0;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        beginSceneFrame(ctx, s);
        if (frame > 0u) {
            for (u32 k = 0; k < s.movers.size(); ++k) {
                moveObject(s, s.movers[(k + frame) % s.movers.size()], 0.07f, -0.05f);
            }
        }
        if (frame == 3u) {
            s.removedDesc.mesh = 1;
            s.removedDesc.transform = s.gpu.transform(5);
            s.gpu.removeInstance(s.handles[5]);
        } else if (frame == 5u) {
            s.handles[5] = s.gpu.addInstance(s.removedDesc);
        }
        const FrameCamera cam = frameCamera(0.6f * static_cast<f32>(frame));
        const f32* light = frame >= 6u ? kSun2 : kSun;
        if (!runFrame(ctx, s, rig, graph, cam, light, fs, true, frame == 0u)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        FrameReport reps[kLanguages];
        checkFrame(ctx, s, rig, fs, replays, reps, frame);
        const u32 k0 = rig.built[0] ? 0u : 1u;
        totalRendered += reps[k0].gpu.toRender;
        totalScrolled += reps[k0].gpu.scrolled;
        totalInvalidated += reps[k0].gpu.invalidated;
        if (frame == 0u) {
            expect(reps[k0].gpu.toRender == reps[k0].gpu.requested && reps[k0].gpu.failed == 0u && reps[k0].gpu.requested > 30u,
                   "frame 0 renders every requested page");
            // vsm.clear: every render-list page of the pool holds the clear value.
            const VirtualShadowMap& v = rig.vsm[k0];
            const u32 poolW = v.poolImage().desc.width;
            const u32* pool = reinterpret_cast<const u32*>(rbAt(ctx, fs.layout.pool));
            bool cleared = true;
            for (u32 i = 0; i < reps[k0].gpu.toRender; ++i) {
                const u32 p = replays[k0].model->pte(reps[k0].renderList[i]) & kPtePhysMask;
                const u32 px = (p % rd.poolX) * kPageTexels;
                const u32 py = (p / rd.poolX) * kPageTexels;
                for (u32 y = 0; y < kPageTexels && cleared; y += 7u) {
                    for (u32 x = 0; x < kPageTexels; x += 5u) {
                        cleared = cleared && pool[(py + y) * poolW + px + x] == 0x3F800000u;
                    }
                    cleared = cleared && pool[(py + y) * poolW + px + kPageTexels - 1u] == 0x3F800000u;
                }
                cleared = cleared && pool[(py + kPageTexels - 1u) * poolW + px + kPageTexels - 1u] == 0x3F800000u;
            }
            expect(cleared, "vsm.clear (dispatch indirect) cleared every page of the render list");
        }
        if (frame == 6u) {
            expect(reps[k0].gpu.toRender == reps[k0].gpu.requested - reps[k0].gpu.failed, "light rotation re-renders every page");
        }
    }
    std::printf("parity: %u frames, %u page renders, %u scrolled, %u invalidated by bounds changes\n", kFrames, totalRendered,
                totalScrolled, totalInvalidated);
    expect(totalScrolled > 0u && totalInvalidated > 0u, "scrolling and invalidation happened");
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            expect(rig.vsm[k].stats().boundsRebuilds >= 2u, "the bounds buffer grew (rebuild path exercised)");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

int runCache(Context& ctx) {
    Scene s;
    Rig rig;
    Replay replays[kLanguages];
    const RigDesc rd{};
    const int rc = prepare(ctx, s, rig, rd, replays);
    if (rc != 0) {
        return rc;
    }
    rg::Graph graph;
    FrameState fs;
    const FrameCamera cam = frameCamera(1.f);
    const u32 k0 = rig.built[0] ? 0u : 1u;
    // The shadow-casting mover nearest the camera (receiver-only movers never invalidate anything).
    u32 mover = 0;
    f32 nearest = -1e30f;
    for (const u32 m : s.movers) {
        if ((s.gpu.instance(m).flags & kInstanceCastShadow) != 0u && s.gpu.transform(m).rows[2][3] > nearest) {
            nearest = s.gpu.transform(m).rows[2][3];
            mover = m;
        }
    }
    expect(mover != 0u, "a shadow-casting mover exists");
    for (u32 frame = 0; frame < 9; ++frame) {
        beginSceneFrame(ctx, s);
        if (frame == 4u) {
            moveObject(s, mover, 0.3f, 0.f);
        } else if (frame == 7u) {
            s.gpu.removeInstance(s.handles[mover]);
        }
        if (!runFrame(ctx, s, rig, graph, cam, kSun, fs, true, false)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        FrameReport reps[kLanguages];
        checkFrame(ctx, s, rig, fs, replays, reps, frame);
        const FrameReport& r = reps[k0];
        if (frame == 0u) {
            expect(r.gpu.toRender == r.gpu.requested && r.gpu.toRender > 30u, "frame 0 renders every requested page");
        } else if (frame == 4u || frame == 7u) {
            // Only pages under the object's old / new footprint re-render (plus pages the frame requests for
            // the first time: the object also moved on screen); every other requested page stays cached.
            const Replay& rp = replays[k0];
            auto bit = [](const std::vector<u32>& w, u32 v) { return ((w[v / 32u] >> (v % 32u)) & 1u) != 0u; };
            u32 underFootprint = 0, fresh = 0, stray = 0, requestedInvalid = 0, missed = 0;
            for (const u32 v : r.renderList) {
                if (bit(rp.invalid, v)) {
                    ++underFootprint;
                } else if ((rp.prevPte[v] & kPteMapped) == 0u) {
                    ++fresh;
                } else {
                    ++stray;
                }
            }
            for (u32 v = 0; v < rig.vsm[k0].constants().virtualPages; ++v) {
                if (bit(rp.request, v) && bit(rp.invalid, v)) {
                    ++requestedInvalid;
                    missed += std::binary_search(r.renderList.begin(), r.renderList.end(), v) ? 0u : 1u;
                }
            }
            std::printf("  frame %u: object %s: %u pages re-rendered of %u requested (%u under its footprint, %u requested for "
                        "the first time)\n",
                        frame, frame == 4u ? "moved" : "removed", r.gpu.toRender, r.gpu.requested, underFootprint, fresh);
            expect(r.changedSlots == 1u, "exactly one slot changed");
            expect(stray == 0u && underFootprint > 0u && fresh <= r.gpu.allocated,
                   "re-rendered pages are under the moved object's footprint or new this frame");
            expect(missed == 0u && underFootprint == requestedInvalid, "every requested page under the footprint re-renders");
            expect(r.gpu.toRender * 2u < r.gpu.requested, "most pages stay cached");
        } else {
            expect(r.gpu.toRender == 0u && r.gpu.allocated == 0u && r.gpu.invalidated == 0u,
                   "static frame: 0 pages rendered (static page cache)");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

int runEvict(Context& ctx) {
    Scene s;
    Rig rig;
    Replay replays[kLanguages];
    RigDesc rd{};
    rd.levels = 8;
    rd.poolX = 4;
    rd.poolY = 4;
    rd.markRadius = 0.f;
    const int rc = prepare(ctx, s, rig, rd, replays);
    if (rc != 0) {
        return rc;
    }
    rg::Graph graph;
    FrameState fs;
    const u32 k0 = rig.built[0] ? 0u : 1u;
    u32 evicted = 0, failed = 0;
    for (u32 frame = 0; frame < 6; ++frame) {
        beginSceneFrame(ctx, s);
        moveObject(s, s.movers[frame % s.movers.size()], 0.1f, 0.f);
        const FrameCamera cam = frameCamera(1.5f * static_cast<f32>(frame));
        if (!runFrame(ctx, s, rig, graph, cam, kSun, fs, true, false)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            destroyRig(rig);
            return 1;
        }
        FrameReport reps[kLanguages];
        checkFrame(ctx, s, rig, fs, replays, reps, frame);
        evicted += reps[k0].gpu.evicted;
        failed += reps[k0].gpu.failed;
    }
    std::printf("evict: 16 physical pages, %u evictions, %u failed needs over 6 frames\n", evicted, failed);
    expect(evicted > 0u && failed > 0u, "the small pool evicts and fails");
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// Pass-callback allocation counting (rg::PassHooks).
void hookBegin(const rg::PassContext&, const char* name, void*) { t_count = std::strncmp(name, "vsm.", 4) == 0; }
void hookEnd(const rg::PassContext&, const char*, void*) { t_count = false; }

int runZeroAlloc(Context& ctx, bool countAllocations) {
    Scene s;
    Rig rig;
    Replay replays[kLanguages];
    const RigDesc rd{};
    const int rc = prepare(ctx, s, rig, rd, replays);
    if (rc != 0) {
        return rc;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long vsmSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    const u32 k0 = rig.built[0] ? 0u : 1u;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s);
        moveObject(s, s.movers[frame % s.movers.size()], frame % 2u == 0u ? 0.05f : -0.05f, 0.f);
        s.gpu.commit();
        ctx.upload.flush();
        const FrameCamera cam = frameCamera(static_cast<f32>(frame % 4u) * 0.2f);
        bool ok = beginFrame(ctx, s, rig, cam, kSun, false);
        const VsmFrameDesc f = vsmFrame(s, rig, cam, kSun);
        t_allocations = 0;
        t_count = measure;
        for (u32 k = 0; k < kLanguages; ++k) {
            if (rig.built[k]) {
                ok = rig.vsm[k].beginFrame(ctx.serial, f) && ok;
            }
        }
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, s, rig, graph, fs, false, false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rig.culler.collectRetired(ctx.serial);
        rig.vb.collectRetired(ctx.serial);
        for (VirtualShadowMap& v : rig.vsm) {
            v.collectRetired(ctx.serial);
        }
        s.gpu.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(ok && result.ok && waited, "frame ok");
        if (measure) {
            vsmSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.vsm[k0].stats().passes == 7u, "vsm.reset, mark, invalidate, update, alloc, render, clear (+ nothing else) in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, objects moving, culled VB depth + 2 VSMs)\n"
                    "  VirtualShadowMap::beginFrame + vsm.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis and vsm imports + passes): %llu\n",
                    kTotal - kWarmup, s.gpu.instanceHighWater(), vsmSide, callbacks, build);
        expect(vsmSide == 0u, "the VSM makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the VSM passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
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
        if (mode == "parity") {
            rc = runParity(ctx);
        } else if (mode == "cache") {
            rc = runCache(ctx);
        } else if (mode == "evict") {
            rc = runEvict(ctx);
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
