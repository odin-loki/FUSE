// WP-1.4 visibility-buffer Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_visbuffer_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, then
// VisBuffer::addCulledFrame: InstanceCuller (WP-1.3) phase 1, the phase-1 visibility draw
// (vkCmdDrawIndexedIndirectCount over the scene index buffer), [export], Hi-Z, phase 2, the phase-2
// draw, [export], Hi-Z; then the full-frame decode ("vis.decode") and read-back copies. A second
// culler with frustum and occlusion off draws every instance into a second VisBuffer (the no-culling
// reference image). Scene: 3 meshlet meshes (sphere, torus, box; WP-1.2 builder) x 600 instances
// behind 3 occluder slabs, 6 frames of camera and object motion.
//
//   --mode raster          R32G32_UINT + D32 target (T0 default)
//   --mode atomic_image    64-bit atomicMin target, R64_UINT image (skip without image int64 atomics)
//   --mode atomic_buffer   64-bit atomicMin target, u64 storage buffer (skip without buffer int64 atomics)
//     checked every frame, for each kernel language built (Slang, GLSL):
//       ids       every robust pixel of the CPU raster reference (raster_reference, f64 per pixel
//                 centre; >= 60% of the pixels) has exactly the reference (instance, triangle); a
//                 corrupted copy of the read-back is caught (negative control)
//       decode    the GPU decode == the CPU decode kernel on the read-back image (flags equal,
//                 depth / barycentrics within 1e-5), and is consistent with the depth: raster
//                 |decode - D32| <= 1e-4 on covered pixels, depth == 1 exactly where empty; atomic
//                 floor(q) - 1e-4 <= decode <= exported depth + 1e-4
//       export    (atomic) exported R32G32 / R32F == vis64_unpack / vis64_export_depth of the raw
//                 64-bit words, bit for bit
//       culling   the culled image == the no-culling image (0 differing pixels); phase-1 and phase-2
//                 draws both occur over the run
//     and Slang == GLSL: identical visibility images, decode within 1e-5.
//   --mode zero_alloc      64 steady-state frames (raster, 1% of the instances moved): 0 operator-new
//                          calls in VisBuffer::beginFrame + the graph build + the vis.* / cull.* pass
//                          callbacks (validated run first; validation off for the count).
//   --backend set | buffer bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
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
constexpr u32 kFrames = 6;

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

Mat4 camera(f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az) {
    const f32 eye[3] = {ex, ey, ez};
    const f32 at[3] = {ax, ay, az};
    return mul(perspective(1.1f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 120.f), lookAt(eye, at));
}

Mat4 frameCamera(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    return camera(-1.2f + 0.45f * t, 0.6f - 0.1f * t, 3.f, 0.4f * t - 1.f, 0.f, -25.f);
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
    Buffer decode{};
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
            allocator->destroyBuffer(decode);
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

/// Byte layout of the readback buffer.
struct ReadbackLayout {
    static constexpr u64 vis = 0;
    static constexpr u64 depth = vis + kPixels * 8ull;
    static constexpr u64 raw64 = depth + kPixels * 4ull;
    static constexpr u64 decode = raw64 + kPixels * 8ull;
    static constexpr u64 refVis = decode + kPixels * 16ull;
    static constexpr u64 counts = refVis + kPixels * 8ull;
    static constexpr u64 end = counts + kCountWords * 4ull;
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
    instanceDesc.appName = "fuse_rp_visbuffer";
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
    const VisCapabilities caps = queryVisCapabilities(ctx.device.get());
    if (!caps.raster) {
        std::printf("SKIP: visibility buffer unsupported: %s\n", caps.rasterReason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s; atomic targets: buffer %s, image %s (%s)\n", ctx.device->info().deviceName.c_str(),
                caps.atomicBuffer ? "yes" : "no", caps.atomicImage ? "yes" : "no", caps.atomicReason);
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
    stagingDesc.name = "rp_visbuffer.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = ReadbackLayout::end;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_visbuffer.readback";
    BufferDesc decodeDesc{};
    decodeDesc.size = kPixels * sizeof(VisDecodeTexel);
    decodeDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                static_cast<u32>(BufferUsage::TransferSrc) |
                                                static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    decodeDesc.memoryUsage = MemoryUsage::GpuOnly;
    decodeDesc.name = "rp_visbuffer.decode";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr ||
        !ctx.allocator->createBuffer(decodeDesc, ctx.decode) || ctx.decode.deviceAddress == 0u) {
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
};

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
    const vis_test::SourceMesh sources[3] = {vis_test::uvSphere(16, 24, 1.f), vis_test::torus(24, 12, 1.f, 0.35f),
                                             vis_test::box()};
    s.meshes.resize(3);
    for (u32 i = 0; i < 3u; ++i) {
        if (!vis_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    for (const geometry::MeshletMesh& m : s.meshes) {
        s.positions.push_back(decode_kernel::MeshPositions{m.positions.data(), m.vertex_count()});
    }
    std::mt19937 rng(1234);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    // Occluder slabs (boxes) in front of the field.
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

// --- per-language rig -----------------------------------------------------------------------------
struct Rig {
    const char* language = nullptr;
    InstanceCuller culler;
    InstanceCuller refCuller;
    VisBuffer vb;
    VisBuffer refVb;
};

/// 1 = ok, 0 = language not built (Auto fell back), -1 = failure
int initRig(Context& ctx, Rig& rig, VisMode mode, VisAtomicTarget target, VisKernelLanguage language) {
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
    vd.language = language;
    if (!rig.vb.init(vd)) {
        return 0;
    }
    if (!rig.refVb.init(vd)) {
        return -1;
    }
    rig.language = rig.vb.kernelLanguage();
    return 1;
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
    enum Kind : u8 { Image, Buffer } kind = Image;
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
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {c.aspect, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

struct FrameState {
    CopyRecord copies[8];
    u32 copyCount = 0;
};

struct FrameOptions {
    Mat4 viewProj{};
    bool withReference = true;
    bool readback = true;
    bool decode = true;
};

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const FrameOptions& opt) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, opt.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, opt.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    if (opt.withReference) {
        CullFrameDesc all = frame;
        all.frustum = false;
        all.occlusion = false;
        ok = rig.refCuller.beginFrame(ctx.serial, all) && ok;
        ok = rig.refVb.beginFrame(ctx.serial, opt.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    const u32 sceneHandle = s.gpu.headerHandle();
    rg::BufferRef readback{};
    rg::BufferRef decode{};
    if (opt.readback) {
        readback = graph.importBuffer(
            rg::ImportedBuffer{ctx.readback.handle, ReadbackLayout::end, rg::kNoQueue, nullptr, "rp_visbuffer.readback"});
    }
    if (opt.decode) {
        decode = graph.importBuffer(
            rg::ImportedBuffer{ctx.decode.handle, ctx.decode.desc.size, rg::kNoQueue, nullptr, "rp_visbuffer.decode"});
    }
    auto addCopy = [&](const char* name, CopyRecord::Kind kind, rg::TextureRef image, u32 aspect, rg::BufferRef buffer,
                       u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.kind = kind;
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
        pass.use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    if (opt.withReference) {
        const CullGraphRefs refCull = rig.refCuller.importInto(graph);
        const VisGraphRefs refVis = rig.refVb.importInto(graph);
        rig.refCuller.addPhase1(graph, refCull, sceneRefs, sceneHandle);
        rig.refVb.addDraw(graph, refVis, sceneRefs, rig.refCuller, refCull, CullPhase::Phase1, true);
        rig.refVb.addExport(graph, refVis);
        if (opt.readback) {
            addCopy("readback.ref_vis", CopyRecord::Image, refVis.vis, VK_IMAGE_ASPECT_COLOR_BIT, {}, ReadbackLayout::refVis,
                    kPixels * 8ull);
        }
    }
    rig.vb.addCulledFrame(graph, vis, sceneRefs, sceneHandle, rig.culler, cull);
    if (opt.decode) {
        rig.vb.addDecode(graph, vis, sceneRefs, decode, ctx.decode.deviceAddress);
    }
    if (opt.readback) {
        const bool atomic = rig.vb.mode() == VisMode::Atomic64;
        addCopy("readback.vis", CopyRecord::Image, vis.vis, VK_IMAGE_ASPECT_COLOR_BIT, {}, ReadbackLayout::vis, kPixels * 8ull);
        addCopy("readback.depth", CopyRecord::Image, vis.depth, atomic ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
                {}, ReadbackLayout::depth, kPixels * 4ull);
        if (vis.image64.valid()) {
            addCopy("readback.raw64", CopyRecord::Image, vis.image64, VK_IMAGE_ASPECT_COLOR_BIT, {}, ReadbackLayout::raw64,
                    kPixels * 8ull);
        }
        if (vis.buffer64.valid()) {
            addCopy("readback.raw64", CopyRecord::Buffer, {}, 0u, vis.buffer64, ReadbackLayout::raw64, kPixels * 8ull);
        }
        if (opt.decode) {
            addCopy("readback.decode", CopyRecord::Buffer, {}, 0u, decode, ReadbackLayout::decode, kPixels * 16ull);
        }
        addCopy("readback.counts", CopyRecord::Buffer, {}, 0u, cull.counts, ReadbackLayout::counts, kCountWords * 4ull);
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

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

// --- analysis -------------------------------------------------------------------------------------
template <typename T>
const T* rb(Context& ctx, u64 offset) {
    return reinterpret_cast<const T*>(static_cast<const u8*>(ctx.readback.mapped) + offset);
}

/// Robust reference pixels whose GPU sample differs.
u32 idMismatches(const std::vector<RasterRefPixel>& ref, const u32* vis, u32* robust, u32* robustCovered = nullptr) {
    u32 bad = 0;
    u32 n = 0;
    u32 nc = 0;
    for (u32 p = 0; p < kPixels; ++p) {
        if (ref[p].robust == 0u) {
            continue;
        }
        ++n;
        nc += ref[p].instance != kVisInvalid;
        const VisSample s = vis_raster_unpack(vis[p * 2u], vis[p * 2u + 1u]);
        bad += s.instance != ref[p].instance || s.triangle != ref[p].triangle;
    }
    if (robust != nullptr) {
        *robust = n;
    }
    if (robustCovered != nullptr) {
        *robustCovered = nc;
    }
    return bad;
}

struct FrameReport {
    u32 robust = 0;
    u32 robustCovered = 0;
    u32 refCovered = 0;
    u32 idBad = 0;
    u32 covered = 0;
    u32 decodeBad = 0;
    u32 depthBad = 0;
    u32 exportBad = 0;
    u32 cullDiff = 0;
    u32 phase1 = 0;
    u32 phase2 = 0;
    f64 maxDecodeErr = 0.0;
    f64 maxDepthErr = 0.0;
    std::vector<u32> vis;
    std::vector<VisDecodeTexel> decode;
};

void analyse(Context& ctx, Scene& s, Rig& rig, const Mat4& vp, FrameReport& r) {
    const bool atomic = rig.vb.mode() == VisMode::Atomic64;
    const u32* vis = rb<u32>(ctx, ReadbackLayout::vis);
    r.vis.assign(vis, vis + kPixels * 2u);
    const VisDecodeTexel* gpuDecode = rb<VisDecodeTexel>(ctx, ReadbackLayout::decode);
    r.decode.assign(gpuDecode, gpuDecode + kPixels);
    const f32* depth = rb<f32>(ctx, ReadbackLayout::depth);
    const VisSceneView view = vis_scene_view(s.gpu, s.positions);
    // (1) ids against the CPU raster reference.
    std::vector<RasterRefPixel> ref;
    RasterRefStats st{};
    raster_reference(view, vp.m, kWidth, kHeight, RasterRefOptions{}, ref, &st);
    r.idBad = idMismatches(ref, vis, &r.robust, &r.robustCovered);
    r.refCovered = st.covered;
    // Negative control: corrupt one robust covered pixel's triangle.
    std::vector<u32> corrupted(r.vis);
    for (u32 p = 0; p < kPixels; ++p) {
        if (ref[p].robust != 0u && ref[p].instance != kVisInvalid) {
            corrupted[p * 2u + 1u] ^= 1u;
            break;
        }
    }
    expect(idMismatches(ref, corrupted.data(), nullptr) == r.idBad + 1u, "negative control: a corrupted id is caught");
    // (2) GPU decode == CPU decode kernel; consistent with the depth.
    std::vector<VisDecodeTexel> cpu;
    decode_reference(view, vp.m, vis, kWidth, kHeight, cpu);
    for (u32 p = 0; p < kPixels; ++p) {
        const VisDecodeTexel& g = gpuDecode[p];
        const VisDecodeTexel& c = cpu[p];
        const f64 err = std::max({std::fabs(static_cast<f64>(g.depth) - c.depth), std::fabs(static_cast<f64>(g.b1) - c.b1),
                                  std::fabs(static_cast<f64>(g.b2) - c.b2)});
        if (g.flags != c.flags || !(err <= 1e-5)) {
            ++r.decodeBad;
        } else {
            r.maxDecodeErr = std::max(r.maxDecodeErr, err);
        }
        const bool covered = vis[p * 2u] != kVisInvalid;
        r.covered += covered;
        if (covered != (g.flags == kVisDecodeOk)) {
            ++r.depthBad;
            continue;
        }
        if (!covered) {
            r.depthBad += depth[p] != 1.f;
            continue;
        }
        if (atomic) {
            const u64 word = rb<u64>(ctx, ReadbackLayout::raw64)[p];
            const f64 lo = vis64_depth_floor(vis64_depth_bits(word));
            const bool ok = g.depth >= lo - 1e-4 && g.depth <= depth[p] + 1e-4;
            r.depthBad += ok ? 0u : 1u;
            r.maxDepthErr = std::max(r.maxDepthErr, std::fabs(g.depth - lo));
        } else {
            const f64 e = std::fabs(static_cast<f64>(g.depth) - depth[p]);
            r.depthBad += e <= 1e-4 ? 0u : 1u;
            r.maxDepthErr = std::max(r.maxDepthErr, e);
        }
    }
    // (3) export == unpack of the raw words.
    if (atomic) {
        const u64* raw = rb<u64>(ctx, ReadbackLayout::raw64);
        for (u32 p = 0; p < kPixels; ++p) {
            const VisSample e = vis64_unpack(raw[p]);
            const f32 d = vis64_export_depth(raw[p]);
            r.exportBad += (e.instance != vis[p * 2u] || e.triangle != vis[p * 2u + 1u] ||
                            std::memcmp(&d, &depth[p], sizeof(f32)) != 0)
                               ? 1u
                               : 0u;
        }
    }
    // (4) culled image == no-culling image.
    const u32* refVis = rb<u32>(ctx, ReadbackLayout::refVis);
    for (u32 p = 0; p < kPixels * 2u; p += 2u) {
        r.cullDiff += (refVis[p] != vis[p] || (vis[p] != kVisInvalid && refVis[p + 1u] != vis[p + 1u])) ? 1u : 0u;
    }
    const u32* counts = rb<u32>(ctx, ReadbackLayout::counts);
    r.phase1 = counts[kCountPhase1Draws];
    r.phase2 = counts[kCountPhase2Draws];
}

int runIds(Context& ctx, VisMode mode, VisAtomicTarget target) {
    struct LanguageResult {
        const char* language;
        FrameReport last;
    };
    std::vector<LanguageResult> results;
    for (const VisKernelLanguage language : {VisKernelLanguage::Slang, VisKernelLanguage::Glsl}) {
        const char* want = language == VisKernelLanguage::Slang ? "slang" : "glsl";
        Rig rig;
        const int rc = initRig(ctx, rig, mode, target, language);
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init (%s)\n", want);
            return 1;
        }
        if (rc == 0) {
            std::printf("  language %s: not built, skipped\n", want);
            continue;
        }
        Scene s;
        if (!buildScene(ctx, s, kInstances)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        std::printf("  kernels: %s, mode %s%s, %u instances, %u indices\n", rig.language,
                    mode == VisMode::Raster ? "raster" : "atomic64",
                    mode == VisMode::Raster ? "" : (rig.vb.atomicTarget() == VisAtomicTarget::Image ? " (image)" : " (buffer)"),
                    kInstances, s.gpu.indexCount());
        std::printf("  frame  phase1 phase2 covered  robust rob-cov  id-bad dec-bad dep-bad exp-bad cull-diff  max|dec|  max|dep|\n");
        rg::Graph graph;
        FrameState fs;
        u32 phase1 = 0, phase2 = 0;
        FrameReport r;
        for (u32 frame = 0; frame < kFrames; ++frame) {
            beginSceneFrame(ctx, s);
            if (frame > 0u) {
                moveObjects(s, frame, 1u);
            }
            FrameOptions opt{};
            opt.viewProj = frameCamera(frame);
            if (!runFrame(ctx, s, rig, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            r = FrameReport{};
            analyse(ctx, s, rig, opt.viewProj, r);
            phase1 += r.phase1;
            phase2 += r.phase2;
            std::printf("  %5u %7u %6u %7u %7u %7u %7u %7u %7u %7u %9u %9.2g %9.2g\n", frame, r.phase1, r.phase2, r.covered,
                        r.robust, r.robustCovered, r.idBad, r.decodeBad, r.depthBad, r.exportBad, r.cullDiff, r.maxDecodeErr, r.maxDepthErr);
            expect(r.robust >= kPixels * 6u / 10u, "at least 60% of the pixels are robust in the CPU reference");
            expect(r.covered > kPixels / 4u, "the scene covers the view");
            expect(r.robustCovered >= r.refCovered * 8u / 10u, "at least 80% of the covered pixels are robust");
            expect(r.idBad == 0u, "instance / triangle ids == CPU raster reference on every robust pixel");
            expect(r.decodeBad == 0u, "GPU decode == CPU decode kernel");
            expect(r.depthBad == 0u, "decode consistent with the depth");
            expect(r.exportBad == 0u, "export == unpack of the 64-bit words");
            expect(r.cullDiff == 0u, "culled image == no-culling image");
        }
        expect(phase1 > 0u && phase2 > 0u, "both cull phases fed the visibility buffer");
        std::printf("  %s: %u phase-1 + %u phase-2 draws over %u frames\n", rig.language, phase1, phase2, kFrames);
        results.push_back(LanguageResult{rig.language, std::move(r)});
        s.gpu.destroy();
        rig.vb.destroy();
        rig.refVb.destroy();
        rig.culler.destroy();
        rig.refCuller.destroy();
    }
    if (results.empty()) {
        std::printf("SKIP: no visibility-buffer kernels built\n");
        return kSkip;
    }
    if (results.size() == 2u) {
        const FrameReport& a = results[0].last;
        const FrameReport& b = results[1].last;
        f64 maxErr = 0.0;
        bool flags = true;
        for (u32 p = 0; p < kPixels; ++p) {
            flags = flags && a.decode[p].flags == b.decode[p].flags;
            maxErr = std::max({maxErr, std::fabs(static_cast<f64>(a.decode[p].depth) - b.decode[p].depth),
                               std::fabs(static_cast<f64>(a.decode[p].b1) - b.decode[p].b1),
                               std::fabs(static_cast<f64>(a.decode[p].b2) - b.decode[p].b2)});
        }
        expect(a.vis == b.vis, "Slang == GLSL visibility images");
        expect(flags && maxErr <= 1e-5, "Slang == GLSL decode (1e-5)");
        std::printf("  slang vs glsl: visibility %s, decode max |diff| %.3g\n", a.vis == b.vis ? "identical" : "DIFFERENT",
                    maxErr);
    }
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "vis.", 4) == 0 || std::strncmp(name, "cull.", 5) == 0)) {
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
    if (initRig(ctx, rig, VisMode::Raster, VisAtomicTarget::Auto, VisKernelLanguage::Auto) != 1) {
        std::printf("SKIP: no visibility-buffer kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, kInstances)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long visSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool last = frame + 1u == kTotal;
        const bool measure = countAllocations && frame >= kWarmup && !last;
        beginSceneFrame(ctx, s);
        moveObjects(s, frame, 10u); // ~1% of the instances
        s.gpu.commit();
        ctx.upload.flush();
        FrameOptions opt{};
        opt.viewProj = frameCamera(frame % 6u);
        opt.withReference = last;
        opt.readback = last;
        opt.decode = last;
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
        rig.vb.collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            visSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    FrameReport r;
    analyse(ctx, s, rig, frameCamera((kTotal - 1u) % 6u), r);
    expect(r.idBad == 0u && r.decodeBad == 0u && r.depthBad == 0u && r.cullDiff == 0u, "last frame verified");
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, ~1%% moved)\n"
                    "  VisBuffer::beginFrame + culler + vis.* / cull.* pass callbacks: %llu operator-new calls "
                    "(callbacks %llu)\n"
                    "  whole graph build (scene, cull and vis imports + passes): %llu\n",
                    kTotal - kWarmup - 1u, kInstances, visSide, callbacks, build);
        expect(visSide == 0u, "visibility-buffer side makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the vis passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: last frame %u covered, %u robust, 0 mismatches\n",
                    r.covered, r.robust);
    }
    s.gpu.destroy();
    rig.vb.destroy();
    rig.refVb.destroy();
    rig.culler.destroy();
    rig.refCuller.destroy();
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
        const int setupRc = setup(ctx, backend == "buffer", true);
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
