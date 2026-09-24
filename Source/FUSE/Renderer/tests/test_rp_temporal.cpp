// WP-4.1 Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every validation message
// fails the run). CPU gates: test_rp_temporal_cpu.cpp.
//
//   --mode motion      GpuScene (WP-1.1) + two-phase culled visibility buffer (WP-1.3 / WP-1.4) drawn with the
//                      JITTERED matrix (jitter_view_proj, Halton), then "temporal.motion" and the TAAU chain
//                      ("taau.resolve" + "taau.keep" fed by the motion pass's buffers and a render-resolution
//                      colour image). Scene: 4 meshlet meshes (sphere, torus, 2-submesh box, ground plane) x 121
//                      instances (mirrored / squashed ones; a third of them move and rotate every frame), a moving
//                      and turning camera, 7 frames at 192 x 144. Checked every frame >= 1, for each kernel
//                      language built (Slang, GLSL):
//                        analytic   every covered pixel: GPU motion vs the f64 reprojection of the exact point the
//                                   pixel centre sees (the read-back triangle under the draw matrix) < 1e-3 px,
//                                   static and moving instances reported separately; sky pixels vs the f64
//                                   rotation-only reprojection < 1e-3 px; depth == f64 clip w (1e-5 relative)
//                        reference  GPU motion / depth == the CPU kernel "temporal_motion" on the read-back
//                                   visibility image (1e-4 px, 1e-6 relative)
//                        control    the same motion without jitter handling misses the gate (> 0.05 px)
//                        chain      GPU TAAU (motion pass depth / motion in, TaauGpu state machine) == the CPU
//                                   TaauUpscaler (CpuReference) on the read-back buffers: output and history
//   --mode taau        TaauGpu vs TaauUpscaler (CpuReference) on synthetic sequences (tests/test_rp_temporal_common.hpp):
//                      1.5x Catmull-Rom history + masks + camera, 2x Lanczos-2 history without masks / camera /
//                      dilation, 1x bilinear history + static clip; 12 frames each with a history reset, one
//                      TaauGpu across the three resolutions (rebuild path); output + history per frame
//   --mode zero_alloc  80 frames (16 warm-up) of the motion rig: 0 operator-new calls in TemporalMotion / TaauGpu /
//                      VisBuffer / culler beginFrame, the graph build and the temporal.* / taau.* / vis.* / cull.*
//                      pass callbacks (validated run first; validation off for the count)
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// TAAU tolerance (GPU vs CPU kernel), per value e = |gpu - cpu| / max(1, |cpu|) over output RGB and history RGB + w:
//   bulk      e <= 2e-5 for all but 0.1% of the values. The twins execute the same operations in the same order
//             without contraction and Lavapipe rounds add / mul / div / sqrt / floor like the CPU, so the one
//             source of difference is sin() in the Lanczos-2 weights (Vulkan allows 2^-11 absolute; llvmpipe's
//             polynomial is ~1e-7): a weight error dw moves the resolve by <= dw * colour range / sum(w), and the
//             history blend is a contraction, so values stay within a few ulps of the HDR range (measured ~2e-6).
//   outliers  worst e <= 2e-3. clip_to_aabb divides the history offset by the YCoCg box extent, floored at 1e-6:
//             in smooth regions the 3x3 box is almost flat, so the clip factor 1 / maxUnit amplifies a 1e-7 history
//             difference by up to ~1 / extent. That is the kernel's own conditioning, not a twin mismatch: it touches
//             isolated pixels (motion chain on Lavapipe: 0.0015% of the values, worst 1.7e-4; synthetic sequences:
//             none, worst 1.7e-6), and the next frames contract it again.
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_resolve_scene.hpp"
#include "test_rp_temporal_common.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_resolve/resolve_reference.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/taa/taau.hpp>
#include <fuse/renderer/temporal/motion_kernel.hpp>
#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/renderer/temporal/temporal_motion.hpp>
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

// --- allocation counter (operator new) -----------------------------------------------------------------
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
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::temporal;
using namespace fuse::renderer::visbuffer;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
using fuse::u8;
using fuse::usize;
using tm_test::Mat4;

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
constexpr u32 kWidth = 192;
constexpr u32 kHeight = 144;
constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kObjects = 120;
constexpr u32 kFrames = 7;
constexpr f32 kChainRatio = 1.5f;
constexpr u32 kFormatRgba32f = 109u; // VK_FORMAT_R32G32B32A32_SFLOAT
constexpr u32 kFormatR32f = 100u;    // VK_FORMAT_R32_SFLOAT
constexpr u32 kLayoutShaderRead = 5u; // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL

constexpr f64 kTolMotionPx = 1e-3;     // acceptance: analytic reprojection error
constexpr f64 kTolDepthRel = 1e-5;
constexpr f64 kTolKernelPx = 1e-4;     // GPU vs CPU kernel "temporal_motion"
constexpr f64 kTolKernelDepth = 1e-6;
constexpr f64 kTolTaauBulk = 2e-5;      // GPU vs CPU TAAU kernel, every value but kTolTaauOutliers (header comment)
constexpr f64 kTolTaauOutliers = 1e-3;  // fraction of values allowed above kTolTaauBulk
constexpr f64 kTolTaauMax = 2e-3;       // worst value (clip_to_aabb conditioning, header comment)

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

BufferUsage bufferUsage(std::initializer_list<BufferUsage> bits) {
    u32 u = 0;
    for (const BufferUsage b : bits) {
        u |= static_cast<u32>(b);
    }
    return static_cast<BufferUsage>(u);
}

// --- Vulkan context ------------------------------------------------------------------------------------
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
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
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
    instanceDesc.appName = "fuse_rp_temporal";
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
    const TemporalCapabilities caps = queryTemporalCapabilities(ctx.device.get());
    if (!caps.temporal) {
        std::printf("SKIP: temporal passes unsupported: %s\n", caps.reason);
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
    stagingDesc.name = "rp_temporal.staging";
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

// --- host buffers and input images ---------------------------------------------------------------------
struct HostBuffer {
    Buffer buffer{};
    u8 queue = rg::kNoQueue;
    const char* name = "rp_temporal.host";

    bool create(Context& ctx, u64 bytes, BufferUsage usage, MemoryUsage memory, const char* n) {
        name = n;
        BufferDesc d{};
        d.size = static_cast<usize>(bytes);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = n;
        return ctx.allocator->createBuffer(d, buffer) && (memory == MemoryUsage::GpuOnly || buffer.mapped != nullptr);
    }
    void destroy(Context& ctx) {
        if (buffer.handle != nullptr) {
            ctx.allocator->destroyBuffer(buffer);
        }
        buffer = Buffer{};
    }
    rg::BufferRef import(rg::Graph& graph) {
        return graph.importBuffer(rg::ImportedBuffer{buffer.handle, buffer.desc.size, queue, &queue, name});
    }
    template <typename T>
    T* as() const {
        return static_cast<T*>(buffer.mapped);
    }
};

/// A sampled input image (colour RGBA32F or mask R32F) filled by a copy from a host buffer.
struct InputImage {
    Texture tex{};
    BindlessSlotHandle slot{};
    u32 handle = 0;
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    u32 layout = 0;
    u8 queue = rg::kNoQueue;

    bool create(Context& ctx, u32 w, u32 h, u32 fmt, const char* name) {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.format = static_cast<GpuFormat>(fmt);
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        d.name = name;
        if (!ctx.allocator->createImage(d, tex)) {
            return false;
        }
        slot = ctx.bindless.registerTextureSlot(tex, false);
        handle = slot.isValid() ? ctx.bindless.shaderHandle(slot) : 0u;
        format = fmt;
        width = w;
        height = h;
        layout = 0;
        queue = rg::kNoQueue;
        return handle != 0u;
    }
    void destroy(Context& ctx) {
        if (slot.isValid()) {
            ctx.bindless.unregisterSlot(slot);
        }
        if (tex.image != nullptr) {
            ctx.allocator->destroyImage(tex);
        }
        *this = InputImage{};
    }
    rg::TextureRef import(rg::Graph& graph) {
        rg::ImportedImage i{};
        i.image = tex.image;
        i.view = tex.view;
        i.format = format;
        i.width = width;
        i.height = height;
        i.initialLayout = layout;
        i.initialQueue = queue;
        i.layoutTracker = &layout;
        i.queueTracker = &queue;
        i.name = "rp_temporal.input";
        return graph.importImage(i);
    }
};

struct UploadRecord {
    rg::TextureRef image;
    rg::BufferRef src;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const UploadRecord& u = *static_cast<const UploadRecord*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = u.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {u.width, u.height, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(u.src)),
                           static_cast<VkImage>(pc.image(u.image)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

struct CopyRecord {
    enum Kind : u8 { Image, Buffer } kind = Image;
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
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

struct Copies {
    CopyRecord records[8];
    u32 count = 0;
    void reset() { count = 0; }
    void add(rg::Graph& graph, CopyRecord::Kind kind, rg::TextureRef image, rg::BufferRef buffer, rg::BufferRef dst, u64 dstOffset,
             u64 bytes, u32 w = 0, u32 h = 0) {
        CopyRecord& c = records[count++];
        c = CopyRecord{};
        c.kind = kind;
        c.image = image;
        c.buffer = buffer;
        c.dst = dst;
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
        pass.use(dst, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    }
};

/// |gpu - cpu| / max(1, |cpu|) per channel: the worst value, and how many exceed the bulk tolerance.
struct Diff {
    f64 worst = 0.0;
    u64 above = 0;
    u64 values = 0;
    void merge(const Diff& d) {
        worst = std::max(worst, d.worst);
        above += d.above;
        values += d.values;
    }
    f64 fractionAbove() const { return values > 0u ? static_cast<f64>(above) / static_cast<f64>(values) : 0.0; }
};

Diff compareVec4(const f32* gpu, const f32* cpu, u32 count, u32 strideGpu, u32 strideCpu, u32 channels) {
    Diff d{};
    for (u32 i = 0; i < count; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            const f64 g = gpu[i * strideGpu + c];
            const f64 r = cpu[i * strideCpu + c];
            const f64 e = std::isfinite(g) ? std::fabs(g - r) / std::max(1.0, std::fabs(r)) : 1e30;
            d.worst = std::max(d.worst, e);
            d.above += e > kTolTaauBulk ? 1u : 0u;
            ++d.values;
        }
    }
    return d;
}

bool taauWithin(const Diff& d) { return d.worst <= kTolTaauMax && d.fractionAbove() <= kTolTaauOutliers; }

// --- scene ---------------------------------------------------------------------------------------------
struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<ResolveMeshData> data;
    std::vector<resolve_kernel::MeshStreams> streams;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
    std::vector<f32> moverYaw;
};

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

bool buildScene(Context& ctx, Scene& s) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = kObjects + 64u;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    beginSceneFrame(ctx, s);
    const mr_test::SourceMesh sources[4] = {mr_test::uvSphere(16, 24, 1.f), mr_test::torus(24, 12, 1.f, 0.35f), mr_test::box(),
                                            mr_test::plane(16, 60.f, 12.f)};
    s.meshes.resize(4);
    s.data.resize(4);
    for (u32 i = 0; i < 4u; ++i) {
        if (!mr_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
        make_mesh_streams(s.meshes[i], s.data[i]);
        s.streams.push_back(s.data[i].streams);
    }
    std::mt19937 rng(9876);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    InstanceDesc ground{};
    ground.mesh = 3;
    ground.transform = tm_test::place(0.f, -1.8f, -20.f, 1.f, 1.f, 1.f, 0.35f);
    s.handles.push_back(s.gpu.addInstance(ground));
    for (u32 i = 0; i < kObjects; ++i) {
        InstanceDesc id{};
        id.mesh = i % 3u;
        const f32 sc = 0.5f + 0.5f * (u(rng) * 0.5f + 0.5f);
        const f32 mirror = i % 11u == 5u ? -1.f : 1.f;
        const f32 squash = i % 7u == 3u ? 0.6f : 1.f;
        const f32 yaw = u(rng) * 3.f;
        id.transform = tm_test::place(u(rng) * 12.f, -0.6f + 3.f * (u(rng) * 0.5f + 0.5f), -5.f - 26.f * (u(rng) * 0.5f + 0.5f),
                                      sc * mirror, sc * squash, sc, yaw, 0.3f * u(rng));
        s.handles.push_back(s.gpu.addInstance(id));
        if (i % 3u == 1u) {
            s.movers.push_back(static_cast<u32>(s.handles.size() - 1u));
            s.moverYaw.push_back(yaw);
        }
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

/// Moves (translation), turns (yaw) and rescales every `step`-th mover.
void moveObjects(Scene& s, u32 frame, u32 step) {
    for (u32 k = 0; k < s.movers.size(); k += step) {
        const u32 i = s.movers[(k + frame) % s.movers.size()];
        const GpuTransform old = s.gpu.transform(i);
        GpuTransform t = old;
        const f32 a = 0.13f;
        const f32 c = std::cos(a);
        const f32 n = std::sin(a);
        // World yaw about the object's origin (rows' = Ry * rows, 3x3 part) and a 1% growth.
        for (u32 col = 0; col < 3u; ++col) {
            const f32 x = old.rows[0][col];
            const f32 z = old.rows[2][col];
            t.rows[0][col] = (c * x + n * z) * 1.01f;
            t.rows[2][col] = (-n * x + c * z) * 1.01f;
            t.rows[1][col] = old.rows[1][col] * 1.01f;
        }
        t.rows[0][3] += 0.11f;
        t.rows[1][3] -= 0.03f;
        t.rows[2][3] += 0.07f;
        s.gpu.setTransform(s.handles[i], t);
    }
}

Mat4 frameView(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    const f32 eye[3] = {-1.f + 0.3f * t, 1.3f - 0.07f * t, 3.f - 0.25f * t};
    const f32 at[3] = {0.35f * t - 0.5f, -0.5f + 0.02f * t, -16.f};
    return tm_test::lookAt(eye, at);
}

constexpr f32 kFovY = 1.1f;
constexpr f32 kAspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);

Mat4 frameViewProj(u32 frame) { return tm_test::mul(tm_test::perspective(kFovY, kAspect, 0.3f, 120.f), frameView(frame)); }

UpscaleCamera frameCamera(u32 frame) {
    UpscaleCamera c{};
    c.view = tm_test::toMath(frameView(frame));
    c.projection = tm_test::toMath(tm_test::perspective(kFovY, kAspect, 0.3f, 120.f));
    c.vertical_fov_rad = kFovY;
    c.aspect = kAspect;
    c.near_plane = 0.3f;
    c.far_plane = 120.f;
    return c;
}

fuse::math::Vec2 frameJitter(u32 frame) { return upscaleJitterOffset(frame, 8u); }

// --- motion rig ----------------------------------------------------------------------------------------
struct Rig {
    const char* language = nullptr;
    InstanceCuller culler;
    VisBuffer vb;
    TemporalMotion motion;
    TaauGpu taau;
    UpscaleResolution res{};
    InputImage color;
    HostBuffer colorStaging;
    std::vector<fuse::math::Vec3> colorData;
};

/// 1 = ok, 0 = language not built, -1 = failure
int initRig(Context& ctx, Rig& rig, TemporalKernelLanguage language) {
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
    if (!rig.vb.init(vd)) {
        return -1;
    }
    TemporalMotionDesc md{};
    md.device = ctx.device.get();
    md.allocator = ctx.allocator.get();
    md.bindless = &ctx.bindless;
    md.width = kWidth;
    md.height = kHeight;
    md.language = language;
    if (!rig.motion.init(md)) {
        return 0;
    }
    rig.res = makeUpscaleResolution(static_cast<u32>(kWidth * kChainRatio), static_cast<u32>(kHeight * kChainRatio), kChainRatio);
    TaauGpuDesc td{};
    td.device = ctx.device.get();
    td.allocator = ctx.allocator.get();
    td.bindless = &ctx.bindless;
    td.resolution = rig.res;
    td.language = language;
    if (!rig.taau.init(td) || rig.res.render_width != kWidth || rig.res.render_height != kHeight) {
        return -1;
    }
    rig.language = rig.motion.kernelLanguage();
    // Static render-resolution colour (HDR, edges): the chain's colour input.
    rig.colorData.resize(kPixels);
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const f32 checker = ((x / 5u) + (y / 5u)) % 2u == 0u ? 1.f : 0.1f;
            rig.colorData[y * kWidth + x] = {checker * (0.3f + 0.5f * std::sin(0.11f * x)), 0.2f + 0.15f * std::cos(0.07f * y),
                                             (x * 7u + y * 3u) % 61u == 0u ? 9.f : 0.4f * checker};
        }
    }
    if (!rig.color.create(ctx, kWidth, kHeight, kFormatRgba32f, "rp_temporal.color") ||
        !rig.colorStaging.create(ctx, kPixels * 16ull, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_temporal.color_staging")) {
        return -1;
    }
    f32* dst = rig.colorStaging.as<f32>();
    for (u32 i = 0; i < kPixels; ++i) {
        dst[i * 4u + 0u] = rig.colorData[i].x;
        dst[i * 4u + 1u] = rig.colorData[i].y;
        dst[i * 4u + 2u] = rig.colorData[i].z;
        dst[i * 4u + 3u] = 1.f;
    }
    rg::Graph graph;
    UploadRecord up{};
    up.image = rig.color.import(graph);
    up.src = rig.colorStaging.import(graph);
    up.width = kWidth;
    up.height = kHeight;
    graph.addPass("upload.color", &recordUpload, &up).use(up.image, rg::Access::TransferDst).use(up.src, rg::Access::TransferSrc);
    graph.addPass("upload.color_read", nullptr, nullptr)
        .use(up.image, rg::Access::SampledRead, {}, rg::kStageCompute)
        .neverCull();
    return ctx.executor->execute(graph).ok && ctx.executor->waitIdle() ? 1 : -1;
}

void destroyRig(Context& ctx, Rig& rig) {
    rig.taau.destroy();
    rig.motion.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
    rig.color.destroy(ctx);
    rig.colorStaging.destroy(ctx);
}

/// Byte layout of the motion-mode readback buffer.
struct MotionReadback {
    u64 vis = 0, motion = 0, depth = 0, dump = 0, history = 0, end = 0;
};

MotionReadback motionLayout(const UpscaleResolution& res) {
    MotionReadback l{};
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    const u64 dn = static_cast<u64>(res.display_width) * res.display_height;
    l.vis = take(kPixels * 8ull);
    l.motion = take(kPixels * 8ull);
    l.depth = take(kPixels * 4ull);
    l.dump = take(dn * 16u);
    l.history = take(dn * 16u);
    l.end = cursor;
    return l;
}

struct FrameState {
    Copies copies;
    Mat4 vp{};
    Mat4 prevVp{};
    fuse::math::Vec2 jitter{};
    fuse::math::Vec2 prevJitter{};
};

bool beginFrame(Context& ctx, Scene& s, Rig& rig, FrameState& fs, u32 frame, bool reset) {
    f32 draw[16];
    jitter_view_proj(fs.vp.m, fs.jitter.x, fs.jitter.y, kWidth, kHeight, draw);
    CullFrameDesc cf{};
    std::memcpy(cf.viewProj, draw, sizeof(cf.viewProj));
    cf.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, cf);
    ok = rig.vb.beginFrame(ctx.serial, draw, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    MotionFrameDesc mf{};
    std::memcpy(mf.viewProj, fs.vp.m, sizeof(mf.viewProj));
    std::memcpy(mf.prevViewProj, fs.prevVp.m, sizeof(mf.prevViewProj));
    mf.jitter_px = fs.jitter;
    mf.scene = s.gpu.headerHandle();
    mf.vis = rig.vb.visStorageHandle();
    ok = rig.motion.beginFrame(ctx.serial, mf) && ok;
    ok = std::memcmp(rig.motion.drawViewProj(), draw, sizeof(draw)) == 0 && ok;
    TaauGpuFrameDesc tf{};
    tf.resolution = rig.res;
    tf.jitter_px = fs.jitter;
    tf.exposure = 0.8f;
    tf.reset_history = reset;
    tf.camera = frameCamera(frame);
    tf.previous_camera = frameCamera(frame > 0u ? frame - 1u : 0u);
    tf.color = rig.color.handle;
    tf.depth = rig.motion.depthAddress();
    tf.motion = rig.motion.motionAddress();
    ok = rig.taau.beginFrame(ctx.serial, tf) && ok;
    return ok;
}

void buildGraph(Scene& s, Rig& rig, rg::Graph& graph, FrameState& fs, HostBuffer* readback, const MotionReadback& layout) {
    graph.reset();
    fs.copies.reset();
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), rig.culler, cull);
    const MotionGraphRefs m = rig.motion.importInto(graph);
    rig.motion.addMotion(graph, m, vis.vis, sceneRefs);
    const TaauGraphRefs t = rig.taau.importInto(graph);
    TaauGpuInputs in{};
    in.color = rig.color.import(graph);
    in.depth = m.depth;
    in.motion = m.motion;
    const u64 dn = static_cast<u64>(rig.res.display_width) * rig.res.display_height;
    if (readback == nullptr) {
        rig.taau.addResolve(graph, t, in);
        return;
    }
    const rg::BufferRef rb = readback->import(graph);
    rig.taau.addResolve(graph, t, in, rb, readback->buffer.deviceAddress + layout.dump, layout.dump);
    const u32 written = rig.taau.historyIndex();
    fs.copies.add(graph, CopyRecord::Image, vis.vis, {}, rb, layout.vis, kPixels * 8ull, kWidth, kHeight);
    fs.copies.add(graph, CopyRecord::Buffer, {}, m.motion, rb, layout.motion, kPixels * 8ull);
    fs.copies.add(graph, CopyRecord::Buffer, {}, m.depth, rb, layout.depth, kPixels * 4ull);
    fs.copies.add(graph, CopyRecord::Buffer, {}, t.history[written], rb, layout.history, dn * 16u);
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void collect(Context& ctx, Scene& s, Rig& rig) {
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.taau.collectRetired(ctx.serial);
    s.gpu.collectRetired(ctx.serial);
}

// --- motion mode ---------------------------------------------------------------------------------------
struct MotionReport {
    u32 staticPx = 0, movingPx = 0, skyPx = 0;
    f64 staticErr = 0, movingErr = 0, skyErr = 0, depthErr = 0;
    f64 kernelErr = 0, kernelDepthErr = 0;
    f64 naiveErr = 0;
    Diff taauOut{}, taauHistory{};
    u32 statusMismatch = 0;
};

void analyseMotion(Context& ctx, Scene& s, Rig& rig, const FrameState& fs, const u8* rb, const MotionReadback& layout,
                   MotionReport& r) {
    (void)ctx;
    const u32* vis = reinterpret_cast<const u32*>(rb + layout.vis);
    const f32* gm = reinterpret_cast<const f32*>(rb + layout.motion);
    const f32* gd = reinterpret_cast<const f32*>(rb + layout.depth);
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<visbuffer::decode_kernel::MeshPositions> positions;
    for (const resolve_kernel::MeshStreams& st : s.streams) {
        positions.push_back({st.vpos, st.vertexCount});
    }
    // CPU reference kernel on the read-back visibility image.
    std::vector<fuse::math::Vec2> cm(kPixels);
    std::vector<f32> cd(kPixels);
    std::vector<u32> status(kPixels);
    motion_kernel::Params p{};
    p.vis = kernel::make_span(vis, kPixels * 2u);
    p.instances = view.instances;
    p.transforms = view.transforms;
    p.prevTransforms = view.prevTransforms;
    p.meshes = view.meshes;
    p.indices = view.indices;
    p.positions = kernel::make_span(static_cast<const visbuffer::decode_kernel::MeshPositions*>(positions.data()),
                                    static_cast<u32>(positions.size()));
    p.frame = rig.motion.frameConstants();
    p.motion = kernel::make_span(cm.data(), kPixels);
    p.depth = kernel::make_span(cd.data(), kPixels);
    p.status = kernel::make_span(status.data(), kPixels);
    expect(kernel::launch(kernel::Backend::CpuReference, motion_kernel::make_launch(kWidth, kHeight), motion_kernel::Kernel{}, p).ok,
           "CPU motion kernel");
    f32 prevDraw[16];
    jitter_view_proj(fs.prevVp.m, fs.prevJitter.x, fs.prevJitter.y, kWidth, kHeight, prevDraw);
    const f32* draw = rig.motion.drawViewProj();
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const u32 i = y * kWidth + x;
            r.kernelErr = std::max({r.kernelErr, std::fabs(static_cast<f64>(gm[i * 2u]) - cm[i].x) * kWidth,
                                    std::fabs(static_cast<f64>(gm[i * 2u + 1u]) - cm[i].y) * kHeight});
            r.kernelDepthErr = std::max(r.kernelDepthErr, std::fabs(static_cast<f64>(gd[i]) - cd[i]) / std::max(1.0, static_cast<f64>(cd[i])));
            const u32 inst = vis[i * 2u];
            if (inst == kVisInvalid) {
                f64 sky[2];
                if (tm_test::analyticSky(fs.vp.m, fs.prevVp.m, fs.jitter.x, fs.jitter.y, x, y, kWidth, kHeight, sky)) {
                    r.skyErr = std::max({r.skyErr, std::fabs(gm[i * 2u] * static_cast<f64>(kWidth) - sky[0]),
                                         std::fabs(gm[i * 2u + 1u] * static_cast<f64>(kHeight) - sky[1])});
                    ++r.skyPx;
                }
                continue;
            }
            if (status[i] != motion_kernel::kStatusOk) {
                ++r.statusMismatch;
                continue;
            }
            const GpuInstance& gi = view.instances[inst];
            const GpuMesh& mesh = view.meshes[gi.mesh];
            f32 v[3][3];
            for (u32 k = 0; k < 3u; ++k) {
                const u32 vi = static_cast<u32>(static_cast<fuse::s32>(view.indices[mesh.firstIndex + vis[i * 2u + 1u] * 3u + k]) +
                                                mesh.vertexOffset);
                visbuffer::decode_kernel::mesh_position(mesh, s.streams[gi.mesh].vpos, vi, v[k]);
            }
            const GpuTransform& ct = view.transforms[inst];
            const GpuTransform& pt = view.prevTransforms[inst];
            const tm_test::Analytic a = tm_test::analyticMotion(draw, fs.vp.m, fs.prevVp.m, prevDraw, ct, pt, v, x, y, kWidth, kHeight);
            if (!a.ok) {
                ++r.statusMismatch;
                continue;
            }
            const f64 err = std::max(std::fabs(gm[i * 2u] * static_cast<f64>(kWidth) - a.motionPx[0]),
                                     std::fabs(gm[i * 2u + 1u] * static_cast<f64>(kHeight) - a.motionPx[1]));
            const bool moving = std::memcmp(&ct, &pt, sizeof(GpuTransform)) != 0;
            if (moving) {
                r.movingErr = std::max(r.movingErr, err);
                ++r.movingPx;
            } else {
                r.staticErr = std::max(r.staticErr, err);
                ++r.staticPx;
            }
            r.depthErr = std::max(r.depthErr, std::fabs(gd[i] - a.depth) / a.depth);
            const f64 naive[2] = {(x + 0.5) - a.prevDrawPx[0], (y + 0.5) - a.prevDrawPx[1]};
            r.naiveErr = std::max({r.naiveErr, std::fabs(naive[0] - a.motionPx[0]), std::fabs(naive[1] - a.motionPx[1])});
        }
    }
}

/// CPU TAAU on the read-back depth / motion (the exact inputs the GPU consumed).
void analyseChain(TaauUpscaler& cpu, Rig& rig, const FrameState& fs, u32 frame, bool reset, const u8* rb,
                  const MotionReadback& layout, MotionReport& r) {
    UpscaleInputs in{};
    in.resolution = rig.res;
    in.color = rig.colorData.data();
    in.depth = reinterpret_cast<const f32*>(rb + layout.depth);
    in.motion = reinterpret_cast<const fuse::math::Vec2*>(rb + layout.motion);
    in.jitter_px = fs.jitter;
    in.exposure = 0.8f;
    in.reset_history = reset;
    in.camera = frameCamera(frame);
    in.previous_camera = frameCamera(frame > 0u ? frame - 1u : 0u);
    const u32 dn = rig.res.display_width * rig.res.display_height;
    std::vector<fuse::math::Vec3> out(dn);
    expect(cpu.upscale(in, out.data(), kernel::Backend::CpuReference), "CPU TAAU");
    r.taauOut = compareVec4(reinterpret_cast<const f32*>(rb + layout.dump), &out[0].x, dn, 4u, 3u, 3u);
    r.taauHistory = compareVec4(reinterpret_cast<const f32*>(rb + layout.history), &cpu.history()[0].x, dn, 4u, 4u, 4u);
}

int runMotion(Context& ctx) {
    const TemporalKernelLanguage languages[2] = {TemporalKernelLanguage::Slang, TemporalKernelLanguage::Glsl};
    u32 ran = 0;
    for (const TemporalKernelLanguage language : languages) {
        Rig rig;
        const int status = initRig(ctx, rig, language);
        if (status == 0) {
            destroyRig(ctx, rig);
            continue;
        }
        if (status < 0) {
            destroyRig(ctx, rig);
            std::fprintf(stderr, "FAIL: rig init\n");
            return 1;
        }
        ++ran;
        Scene s;
        if (!buildScene(ctx, s)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        const MotionReadback layout = motionLayout(rig.res);
        HostBuffer readback;
        if (!readback.create(ctx, layout.end,
                             bufferUsage({BufferUsage::TransferDst, BufferUsage::Storage, BufferUsage::ShaderDeviceAddress}),
                             MemoryUsage::GpuToCpu, "rp_temporal.readback") ||
            readback.buffer.deviceAddress == 0u) {
            std::fprintf(stderr, "FAIL: readback buffer\n");
            return 1;
        }
        TaauUpscaler cpu;
        rg::Graph graph;
        FrameState fs;
        MotionReport total{};
        for (u32 frame = 0; frame < kFrames; ++frame) {
            beginSceneFrame(ctx, s);
            if (frame > 0u) {
                moveObjects(s, frame, 1u);
            }
            const GpuSceneCommitStats stats = s.gpu.commit();
            ctx.upload.flush();
            fs.prevVp = frame > 0u ? frameViewProj(frame - 1u) : frameViewProj(0u);
            fs.vp = frameViewProj(frame);
            fs.jitter = frameJitter(frame);
            fs.prevJitter = frame > 0u ? frameJitter(frame - 1u) : fs.jitter;
            const bool reset = frame == 0u;
            if (!beginFrame(ctx, s, rig, fs, frame, reset)) {
                std::fprintf(stderr, "FAIL: beginFrame %u\n", frame);
                return 1;
            }
            buildGraph(s, rig, graph, fs, &readback, layout);
            const rg::ExecuteResult result = ctx.executor->execute(graph);
            const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
            collect(ctx, s, rig);
            if (!stats.ok || !result.ok || !waited) {
                std::fprintf(stderr, "FAIL: frame %u execute\n", frame);
                return 1;
            }
            MotionReport r{};
            const u8* rb = readback.as<const u8>();
            if (frame > 0u) {
                analyseMotion(ctx, s, rig, fs, rb, layout, r);
            }
            analyseChain(cpu, rig, fs, frame, reset, rb, layout, r);
            std::printf("  [%s] frame %u: static %u px max %.2e, moving %u px max %.2e, sky %u px max %.2e (px); depth %.1e; "
                        "vs CPU kernel %.1e px / %.1e; no-jitter control %.3f px; TAAU out %.1e history %.1e%s\n",
                        rig.language, frame, r.staticPx, r.staticErr, r.movingPx, r.movingErr, r.skyPx, r.skyErr, r.depthErr,
                        r.kernelErr, r.kernelDepthErr, r.naiveErr, r.taauOut.worst, r.taauHistory.worst,
                        rig.taau.stats().historyUsed ? "" : " (no history)");
            total.staticPx += r.staticPx;
            total.movingPx += r.movingPx;
            total.skyPx += r.skyPx;
            total.statusMismatch += r.statusMismatch;
            total.staticErr = std::max(total.staticErr, r.staticErr);
            total.movingErr = std::max(total.movingErr, r.movingErr);
            total.skyErr = std::max(total.skyErr, r.skyErr);
            total.depthErr = std::max(total.depthErr, r.depthErr);
            total.kernelErr = std::max(total.kernelErr, r.kernelErr);
            total.kernelDepthErr = std::max(total.kernelDepthErr, r.kernelDepthErr);
            total.taauOut.merge(r.taauOut);
            total.taauHistory.merge(r.taauHistory);
            if (frame > 0u) {
                expect(r.naiveErr > 0.05, "negative control: motion without jitter handling fails the gate");
            }
        }
        std::printf("[%s] %u frames: analytic static %u px max %.3g px, moving %u px max %.3g px, sky %u px max %.3g px, "
                    "depth rel %.2g; GPU vs CPU kernel %.2g px, depth %.2g; TAAU chain output max %.2g (%.2g%% > %.0e) history "
                    "max %.2g (%.2g%%)\n",
                    rig.language, kFrames, total.staticPx, total.staticErr, total.movingPx, total.movingErr, total.skyPx,
                    total.skyErr, total.depthErr, total.kernelErr, total.kernelDepthErr, total.taauOut.worst,
                    100.0 * total.taauOut.fractionAbove(), kTolTaauBulk, total.taauHistory.worst,
                    100.0 * total.taauHistory.fractionAbove());
        expect(total.staticPx > 20000u && total.movingPx > 2000u && total.skyPx > 2000u, "coverage (static, moving, sky)");
        expect(total.statusMismatch == 0u, "every covered pixel reconstructs");
        expect(total.staticErr < kTolMotionPx && total.movingErr < kTolMotionPx && total.skyErr < kTolMotionPx,
               "analytic reprojection error < 1e-3 px");
        expect(total.depthErr < kTolDepthRel, "linear depth == analytic clip w");
        expect(total.kernelErr < kTolKernelPx && total.kernelDepthErr < kTolKernelDepth, "GPU == CPU kernel temporal_motion");
        expect(taauWithin(total.taauOut) && taauWithin(total.taauHistory), "TAAU chain == CPU TaauUpscaler within tolerance");
        expect(rig.motion.stats().skyValid, "sky reprojection valid");
        readback.destroy(ctx);
        s.gpu.destroy();
        destroyRig(ctx, rig);
    }
    if (ran == 0u) {
        std::printf("SKIP: no temporal kernels built\n");
        return kSkip;
    }
    return 0;
}

// --- taau mode -----------------------------------------------------------------------------------------
struct TaauInputs {
    InputImage color, reactive, transparency;
    HostBuffer staging, depth, motion, readback;
    UpscaleResolution res{};

    bool create(Context& ctx, const UpscaleResolution& r) {
        res = r;
        const u64 rn = static_cast<u64>(r.render_width) * r.render_height;
        const u64 dn = static_cast<u64>(r.display_width) * r.display_height;
        const BufferUsage storage = bufferUsage({BufferUsage::Storage, BufferUsage::ShaderDeviceAddress, BufferUsage::TransferSrc});
        return color.create(ctx, r.render_width, r.render_height, kFormatRgba32f, "rp_temporal.color") &&
               reactive.create(ctx, r.render_width, r.render_height, kFormatR32f, "rp_temporal.reactive") &&
               transparency.create(ctx, r.render_width, r.render_height, kFormatR32f, "rp_temporal.transparency") &&
               staging.create(ctx, rn * 24u, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_temporal.input_staging") &&
               depth.create(ctx, rn * 4u, storage, MemoryUsage::CpuToGpu, "rp_temporal.depth") &&
               motion.create(ctx, rn * 8u, storage, MemoryUsage::CpuToGpu, "rp_temporal.motion") &&
               readback.create(ctx, dn * 32u,
                               bufferUsage({BufferUsage::TransferDst, BufferUsage::Storage, BufferUsage::ShaderDeviceAddress}),
                               MemoryUsage::GpuToCpu, "rp_temporal.taau_readback") &&
               depth.buffer.deviceAddress != 0u && motion.buffer.deviceAddress != 0u && readback.buffer.deviceAddress != 0u;
    }
    void destroy(Context& ctx) {
        color.destroy(ctx);
        reactive.destroy(ctx);
        transparency.destroy(ctx);
        staging.destroy(ctx);
        depth.destroy(ctx);
        motion.destroy(ctx);
        readback.destroy(ctx);
    }
    void write(const tm_test::TaauFrame& f) {
        const u32 rn = res.render_width * res.render_height;
        f32* s = staging.as<f32>();
        for (u32 i = 0; i < rn; ++i) {
            s[i * 4u + 0u] = f.color[i].x;
            s[i * 4u + 1u] = f.color[i].y;
            s[i * 4u + 2u] = f.color[i].z;
            s[i * 4u + 3u] = 1.f;
        }
        std::memcpy(s + rn * 4u, f.reactive.data(), rn * 4u);
        std::memcpy(s + rn * 5u, f.transparency.data(), rn * 4u);
        std::memcpy(depth.buffer.mapped, f.depth.data(), rn * 4u);
        std::memcpy(motion.buffer.mapped, f.motion.data(), rn * 8u);
    }
};

int runTaau(Context& ctx) {
    struct Config {
        UpscaleResolution res;
        bool camera;
        bool masks;
        taau_kernel::Settings settings;
        const char* label;
    };
    Config configs[3] = {{makeUpscaleResolution(144u, 96u, 1.5f), true, true, {}, "1.5x catmull-rom, masks, camera"},
                         {makeUpscaleResolution(160u, 96u, 2.0f), false, false, {}, "2x lanczos-2, no masks / camera / dilation"},
                         {makeUpscaleResolution(112u, 80u, 1.0f), true, true, {}, "1x bilinear, static clip 0.3"}};
    configs[1].settings.history_filter = static_cast<u32>(taau_kernel::HistoryFilter::Lanczos2);
    configs[1].settings.dilate_motion = 0u;
    configs[2].settings.history_filter = static_cast<u32>(taau_kernel::HistoryFilter::Bilinear);
    configs[2].settings.static_clip_strength = 0.3f;
    const TemporalKernelLanguage languages[2] = {TemporalKernelLanguage::Slang, TemporalKernelLanguage::Glsl};
    u32 ran = 0;
    Diff worstOut{};
    Diff worstHistory{};
    for (const TemporalKernelLanguage language : languages) {
        TaauGpu gpu;
        TaauGpuDesc td{};
        td.device = ctx.device.get();
        td.allocator = ctx.allocator.get();
        td.bindless = &ctx.bindless;
        td.resolution = configs[0].res;
        td.language = language;
        if (!gpu.init(td)) {
            continue;
        }
        ++ran;
        u32 historyFrames = 0;
        for (const Config& cfg : configs) {
            TaauInputs io;
            if (!io.create(ctx, cfg.res)) {
                std::fprintf(stderr, "FAIL: taau inputs\n");
                return 1;
            }
            TaauUpscaler cpu(cfg.settings);
            gpu.settings() = cfg.settings;
            const u32 rn = cfg.res.render_width * cfg.res.render_height;
            const u32 dn = cfg.res.display_width * cfg.res.display_height;
            Diff cfgOut{};
            Diff cfgHistory{};
            for (u32 frame = 0; frame < 12u; ++frame) {
                tm_test::TaauFrame tf = tm_test::makeTaauFrame(cfg.res, frame, cfg.camera, cfg.masks);
                tf.reset = frame == 7u;
                io.write(tf);
                ++ctx.serial;
                ctx.bindless.setFrameSerial(ctx.serial);
                TaauGpuFrameDesc fd{};
                fd.resolution = cfg.res;
                fd.jitter_px = tf.jitter;
                fd.exposure = tf.inputs().exposure;
                fd.reset_history = tf.reset;
                fd.camera = tf.camera;
                fd.previous_camera = tf.previousCamera;
                fd.color = io.color.handle;
                fd.reactive = cfg.masks ? io.reactive.handle : 0u;
                fd.transparency = cfg.masks ? io.transparency.handle : 0u;
                fd.depth = io.depth.buffer.deviceAddress;
                fd.motion = io.motion.buffer.deviceAddress;
                if (!gpu.beginFrame(ctx.serial, fd)) {
                    std::fprintf(stderr, "FAIL: TaauGpu::beginFrame\n");
                    return 1;
                }
                rg::Graph graph;
                UploadRecord ups[3];
                const rg::BufferRef staging = io.staging.import(graph);
                InputImage* images[3] = {&io.color, &io.reactive, &io.transparency};
                const u64 offsets[3] = {0u, rn * 16ull, rn * 20ull};
                TaauGpuInputs in{};
                rg::TextureRef refs[3];
                for (u32 k = 0; k < 3u; ++k) {
                    refs[k] = images[k]->import(graph);
                    ups[k] = UploadRecord{refs[k], staging, offsets[k], cfg.res.render_width, cfg.res.render_height};
                    graph.addPass("upload.input", &recordUpload, &ups[k])
                        .use(refs[k], rg::Access::TransferDst)
                        .use(staging, rg::Access::TransferSrc, rg::BufferRange{offsets[k], k == 0u ? rn * 16ull : rn * 4ull});
                }
                in.color = refs[0];
                if (cfg.masks) {
                    in.reactive = refs[1];
                    in.transparency = refs[2];
                }
                in.depth = io.depth.import(graph);
                in.motion = io.motion.import(graph);
                const TaauGraphRefs t = gpu.importInto(graph);
                const rg::BufferRef rb = io.readback.import(graph);
                gpu.addResolve(graph, t, in, rb, io.readback.buffer.deviceAddress);
                Copies copies;
                copies.add(graph, CopyRecord::Buffer, {}, t.history[gpu.historyIndex()], rb, dn * 16ull, dn * 16ull);
                graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
                if (!cfg.masks) {
                    // The unused mask images still get their upload; declare a consumer so no image is left dangling.
                    graph.addPass("upload.masks_unused", nullptr, nullptr)
                        .use(refs[1], rg::Access::SampledRead, {}, rg::kStageCompute)
                        .use(refs[2], rg::Access::SampledRead, {}, rg::kStageCompute)
                        .neverCull();
                }
                const bool ok = ctx.executor->execute(graph).ok && ctx.executor->waitIdle();
                gpu.collectRetired(ctx.serial);
                if (!ok) {
                    std::fprintf(stderr, "FAIL: execute\n");
                    return 1;
                }
                historyFrames += gpu.stats().historyUsed ? 1u : 0u;
                std::vector<fuse::math::Vec3> ref(dn);
                expect(cpu.upscale(tf.inputs(), ref.data(), kernel::Backend::CpuReference), "CPU TAAU");
                const u8* b = io.readback.as<const u8>();
                const Diff eo = compareVec4(reinterpret_cast<const f32*>(b), &ref[0].x, dn, 4u, 3u, 3u);
                const Diff eh = compareVec4(reinterpret_cast<const f32*>(b + dn * 16ull), &cpu.history()[0].x, dn, 4u, 4u, 4u);
                expect(cpu.lastStats().history_used == gpu.stats().historyUsed, "history validity follows the CPU driver");
                cfgOut.merge(eo);
                cfgHistory.merge(eh);
            }
            std::printf("  [%s] %s (%ux%u -> %ux%u): 12 frames, output max %.3g, history max %.3g (rel)\n",
                        gpu.kernelLanguage(), cfg.label, cfg.res.render_width, cfg.res.render_height, cfg.res.display_width,
                        cfg.res.display_height, cfgOut.worst, cfgHistory.worst);
            worstOut.merge(cfgOut);
            worstHistory.merge(cfgHistory);
            ctx.executor->waitIdle();
            io.destroy(ctx);
        }
        expect(historyFrames == 3u * 10u, "history used on every frame but the first and the reset");
        expect(gpu.stats().rebuilds == 3u, "one rebuild per resolution");
        gpu.destroy();
    }
    if (ran == 0u) {
        std::printf("SKIP: no temporal kernels built\n");
        return kSkip;
    }
    std::printf("taau: GPU vs CPU kernel relative difference: output max %.3g (%llu of %llu values > %.0e), history max %.3g "
                "(%llu of %llu)\n",
                worstOut.worst, static_cast<unsigned long long>(worstOut.above), static_cast<unsigned long long>(worstOut.values),
                kTolTaauBulk, worstHistory.worst, static_cast<unsigned long long>(worstHistory.above),
                static_cast<unsigned long long>(worstHistory.values));
    expect(taauWithin(worstOut) && taauWithin(worstHistory), "TAAU GPU == CPU kernel within tolerance");
    return 0;
}

// --- zero_alloc ----------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "temporal.", 9) == 0 || std::strncmp(name, "taau.", 5) == 0 ||
                            std::strncmp(name, "vis.", 4) == 0 || std::strncmp(name, "cull.", 5) == 0)) {
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
    const int status = initRig(ctx, rig, TemporalKernelLanguage::Auto);
    if (status != 1) {
        destroyRig(ctx, rig);
        if (status == 0) {
            std::printf("SKIP: no temporal kernels built\n");
            return kSkip;
        }
        std::fprintf(stderr, "FAIL: rig init\n");
        return 1;
    }
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    const MotionReadback layout{};
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long temporalSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s);
        moveObjects(s, frame, 10u);
        s.gpu.commit();
        ctx.upload.flush();
        fs.prevVp = frameViewProj(frame == 0u ? 0u : (frame - 1u) % 5u);
        fs.vp = frameViewProj(frame % 5u);
        fs.jitter = frameJitter(frame);
        t_allocations = 0;
        t_count = measure;
        const bool began = beginFrame(ctx, s, rig, fs, frame % 5u, frame == 0u);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(s, rig, graph, fs, nullptr, layout);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        collect(ctx, s, rig);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            temporalSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.motion.stats().motionPasses == 1u && rig.taau.stats().resolvePasses == 1u, "one motion + one TAAU pass per frame");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, ~3%% moved, culled visibility frame + motion + TAAU %ux%u -> %ux%u)\n"
                    "  TemporalMotion / TaauGpu / VisBuffer / culler beginFrame + temporal.* / taau.* / vis.* / cull.* "
                    "callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis, motion, TAAU imports + passes): %llu\n",
                    kTotal - kWarmup, s.gpu.instanceHighWater(), kWidth, kHeight, rig.res.display_width, rig.res.display_height,
                    temporalSide, callbacks, build);
        expect(temporalSide == 0u, "temporal side makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the temporal passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(ctx, rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "motion";
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
        if (mode == "motion") {
            rc = runMotion(ctx);
        } else if (mode == "taau") {
            rc = runTaau(ctx);
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
