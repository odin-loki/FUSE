// WP-2.1 GPU clustered lighting Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_clustered_gpu_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, VisBuffer::addCulledFrame
// (WP-1.3 + WP-1.4), the binned material resolve (WP-1.5) into the G-buffer, then for each kernel
// language built (Slang, GLSL) one ClusteredLighting:
//   light.bounds + light.bin + light.cull + light.scan + light.compact   (cluster light lists)
//   light.shade (+ an f32 radiance dump)                                   (deferred shading)
// and read-back copies. Scene: the WP-1.5 scene (241 instances, 4 meshlet meshes, 7 materials, 7
// textures) lit by 4,096 scene light slots (points, then spots, 2 directional lights, lights without a
// range, after the spots; WP-2.2: 100 rectangle / disk area lights between the points and the spots and
// a sun disk, shaded with the compensated BRDF from the BRDF LUT), 16x9x24 clusters, 256x192; 4 frames of camera / object / light motion, point lights removed
// (free slots) and re-added. Checked every frame, for each language:
//   lists      GPU grid (offset, count) + flat list == the B5 oracle, the real ClusteredLightCuller,
//              on the scene's point / spot lights (translated to scene slots; points sit below spots,
//              so the translation is monotone and lists must be equal entry for entry); header totals
//              == the culler's stats (entries, clusters at capacity, dropped); directional list ==
//              the directional slots; oracleLightGrid (the adapter the CPU gates use) == the culler
//   shade      the GPU f32 dump == lighting_gpu::ShadeKernel (clustered_gpu_kernel.hpp) run on the
//              read-back G-buffer texels, the scene light table and the oracle grid: the same pixels are
//              shaded (alpha) and every channel within kTolRel * |ref| + kTolAbs (1e-4, 1e-6; see below)
//   output     the RGBA16F target == the f32 dump rounded to half (<= 1 half ulp)
// and Slang == GLSL (lists bit for bit, dump reported).
//
//   --mode parity_4096     the scene above, capacity 256
//   --mode overflow        capacity 24 + a dense light cluster: clusters overflow, dropped counts and the
//                          kept lowest slots == the oracle
//   --mode zero_alloc      64 steady-state frames (lights moving): 0 operator-new calls in
//                          ClusteredLighting::beginFrame, the light.* pass callbacks and the whole graph
//                          build (validated run first; validation off for the count)
//   --backend set | buffer bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
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
using namespace fuse::renderer::lighting_gpu;
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::visbuffer;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
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
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kObjects = 240;
constexpr u32 kLightSlots = 4096;
constexpr u32 kFrames = 4;
constexpr f32 kFovY = 1.1f;
constexpr f32 kNear = 0.3f;
constexpr f32 kFar = 120.f;
constexpr f32 kAmbient[3] = {0.03f, 0.035f, 0.045f};
constexpr u32 kLanguages = 2; // Slang, GLSL

// Shade tolerance (GPU f32 vs the CPU reference kernel on the same G-buffer texels, light table and
// grid): both evaluate the same f32 expressions in the same order with no contraction (`precise` /
// -fp-mode precise), so the difference is only the device's rounding of sqrt / division / log (Vulkan
// allows x / y and 1 / sqrt 2.5 ulp, sqrt as 1 / inversesqrt, log 3 ulp): a few ulp (~3e-7) per term
// through ~20 dependent operations of one light (~5e-6 relative), and the sum of up to ~260 positive
// terms (capacity 256 + directional lights) keeps that relative bound. A pixel on a slice boundary may
// pick the neighbouring cluster through the log's rounding; the lights that differ between the two
// lists barely reach the pixel (window (1 - (d/r)^4)^2 ~ 0 at d ~ r), far below the absolute term.
// kTolRel = 1e-4 (20x the per-term bound; the plan's §7 asks 1e-3) + kTolAbs = 1e-6 radiance units
// (lit pixels are 1e-2..1e2). Measured on Lavapipe: max relative 3.1e-6 (WP-2.1 lobe). WP-2.2 (the
// compensated BRDF from the LUT, 100 LTC area lights, a sun disk; same tolerance): the worst pixel sits
// at 0.95 of the bound (relative 1.2e-4 on a 0.04 pixel next to two disks): an area light's form factor
// is ~2e-6 of the light's full response sensitive to the device's sqrt / division ulps (N.V, the LUT
// coordinate, the edge / boundary sums), measured per light by fuse_rp_ltc_vk_probe_*.
constexpr f64 kTolRel = 1e-4;
constexpr f64 kTolAbs = 1e-6;

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

// --- math (column-major, Vulkan clip space, forward depth: the WP-1.5 gate's camera) ---------------
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

struct FrameCamera {
    Mat4 viewProj{};
    ClusterCameraDesc cluster{};
};

FrameCamera frameCamera(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    const f32 eye[3] = {-1.f + 0.35f * t, 1.4f - 0.08f * t, 3.f - 0.2f * t};
    const f32 at[3] = {0.3f * t - 0.5f, -0.6f, -16.f};
    FrameCamera c{};
    c.viewProj = mul(perspective(kFovY, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), kNear, kFar), lookAt(eye, at));
    c.cluster.position = {eye[0], eye[1], eye[2]};
    c.cluster.forward = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    c.cluster.up = {0.f, 1.f, 0.f};
    c.cluster.nearPlane = kNear;
    c.cluster.farPlane = kFar;
    c.cluster.screenWidth = kWidth;
    c.cluster.screenHeight = kHeight;
    c.cluster.fovYRadians = kFovY;
    c.cluster.reversedZ = false; // WP-1.5 RT4 = forward z / w
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
    ResourceManager resources; ///< for the oracle ClusteredLightCuller's buffers
    bool resourcesReady = false;
    Buffer staging{};
    Buffer readback{};
    Buffer dumps[kLanguages]{};
    Buffer texStaging{};
    std::vector<Texture> textures;
    std::vector<BindlessSlotHandle> textureSlots;
    u32 textureHandles[mr_test::kTexCount] = {};
    BindlessSlotHandle sampler{};
    u32 samplerHandle = 0;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (resourcesReady) {
            resources.destroy();
        }
        if (device != nullptr) {
            for (BindlessSlotHandle& s : textureSlots) {
                bindless.unregisterSlot(s);
            }
            if (sampler.isValid()) {
                bindless.releaseSampler(sampler);
            }
        }
        if (allocator != nullptr) {
            for (Texture& t : textures) {
                allocator->destroyImage(t);
            }
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
            for (Buffer& b : dumps) {
                allocator->destroyBuffer(b);
            }
            allocator->destroyBuffer(texStaging);
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

/// G-buffer attachments read back for the CPU reference (GBufferAttachment order) and their texel bytes.
constexpr u32 kGBufferRead[5] = {0u, 1u, 2u, 4u, 5u};
constexpr u32 kGBufferBytes[5] = {8u, 4u, 4u, 4u, 8u};

/// Byte layout of the readback buffer (every section 256-aligned).
struct ReadbackLayout {
    u64 gbuffer[5] = {};
    u64 lists[kLanguages] = {};
    u64 listsBytes = 0;
    u64 output[kLanguages] = {};
    u64 end = 0;
};

ReadbackLayout makeLayout(u64 listsBytes) {
    ReadbackLayout l{};
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    for (u32 i = 0; i < 5u; ++i) {
        l.gbuffer[i] = take(static_cast<u64>(kPixels) * kGBufferBytes[i]);
    }
    l.listsBytes = listsBytes;
    for (u32 k = 0; k < kLanguages; ++k) {
        l.lists[k] = take(listsBytes);
        l.output[k] = take(static_cast<u64>(kPixels) * 8u);
    }
    l.end = cursor;
    return l;
}

struct TexUpload {
    rg::TextureRef image;
    rg::BufferRef staging;
    u32 levels = 0;
    u32 size = 0;
    u64 offsets[16] = {};
};

void recordTexUpload(const rg::PassContext& pc, void* user) {
    const TexUpload& t = *static_cast<const TexUpload*>(user);
    VkBufferImageCopy regions[16]{};
    for (u32 l = 0; l < t.levels; ++l) {
        regions[l].bufferOffset = t.offsets[l];
        regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        const u32 s = std::max(1u, t.size >> l);
        regions[l].imageExtent = {s, s, 1};
    }
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(t.staging)),
                           static_cast<VkImage>(pc.image(t.image)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t.levels, regions);
}

bool uploadTextures(Context& ctx) {
    const std::vector<mr_test::TextureData> data = mr_test::makeTextures();
    u64 bytes = 0;
    for (const mr_test::TextureData& t : data) {
        for (const std::vector<f32>& l : t.levels) {
            bytes += l.size();
        }
    }
    BufferDesc sd{};
    sd.size = bytes;
    sd.usage = BufferUsage::TransferSrc;
    sd.memoryUsage = MemoryUsage::CpuToGpu;
    sd.name = "rp_clustered_gpu.tex_staging";
    if (!ctx.allocator->createBuffer(sd, ctx.texStaging) || ctx.texStaging.mapped == nullptr) {
        return false;
    }
    rg::Graph graph;
    std::vector<TexUpload> uploads(data.size());
    const rg::BufferRef staging = graph.importBuffer(
        rg::ImportedBuffer{ctx.texStaging.handle, bytes, rg::kNoQueue, nullptr, "rp_clustered_gpu.tex_staging"});
    u8* dst = static_cast<u8*>(ctx.texStaging.mapped);
    u64 cursor = 0;
    ctx.textures.resize(data.size());
    for (u32 i = 0; i < data.size(); ++i) {
        const mr_test::TextureData& t = data[i];
        TextureDesc td{};
        td.width = t.width;
        td.height = t.height;
        td.mipLevels = static_cast<u32>(t.levels.size());
        td.format = GpuFormat::R8G8B8A8Unorm;
        td.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        td.name = "rp_clustered_gpu.texture";
        if (!ctx.allocator->createImage(td, ctx.textures[i])) {
            return false;
        }
        TexUpload& u = uploads[i];
        u.levels = td.mipLevels;
        u.size = t.width;
        for (u32 l = 0; l < u.levels; ++l) {
            u.offsets[l] = cursor;
            for (const f32 v : t.levels[l]) {
                dst[cursor++] = static_cast<u8>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
            }
        }
        rg::ImportedImage im{};
        im.image = ctx.textures[i].image;
        im.view = ctx.textures[i].view;
        im.format = static_cast<u32>(GpuFormat::R8G8B8A8Unorm);
        im.width = t.width;
        im.height = t.height;
        im.mipLevels = td.mipLevels;
        im.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        im.name = "rp_clustered_gpu.texture";
        u.image = graph.importImage(im);
        u.staging = staging;
        graph.addPass("upload.texture", &recordTexUpload, &u)
            .use(u.image, rg::Access::TransferDst)
            .use(staging, rg::Access::TransferSrc);
    }
    if (!ctx.executor->execute(graph).ok || !ctx.executor->waitIdle()) {
        return false;
    }
    for (u32 i = 0; i < data.size(); ++i) {
        ctx.textureSlots.push_back(ctx.bindless.registerTextureSlot(ctx.textures[i], false));
        if (!ctx.textureSlots.back().isValid()) {
            return false;
        }
        ctx.textureHandles[i] = ctx.bindless.shaderHandle(ctx.textureSlots.back());
    }
    SamplerDesc trilinear{};
    trilinear.minFilter = VK_FILTER_LINEAR;
    trilinear.magFilter = VK_FILTER_LINEAR;
    trilinear.addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    trilinear.name = "rp_clustered_gpu.trilinear";
    ctx.sampler = ctx.bindless.acquireSampler(trilinear);
    if (!ctx.sampler.isValid()) {
        return false;
    }
    ctx.samplerHandle = ctx.bindless.shaderHandle(ctx.sampler);
    return true;
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
    instanceDesc.appName = "fuse_rp_clustered_gpu";
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
    const ResolveCapabilities resolveCaps = queryResolveCapabilities(ctx.device.get());
    if (!resolveCaps.resolve) {
        std::printf("SKIP: material resolve unsupported: %s\n", resolveCaps.reason);
        return kSkip;
    }
    const LightingCapabilities lightingCaps = queryLightingCapabilities(ctx.device.get());
    if (!lightingCaps.lighting) {
        std::printf("SKIP: clustered lighting unsupported: %s\n", lightingCaps.reason);
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
    ResourceManager::Desc rmDesc{};
    rmDesc.stagingRingBytes = 64u * 1024u;
    ctx.resourcesReady = ctx.resources.init(*ctx.device, ctx.bindless, rmDesc);
    if (!ctx.resourcesReady) {
        std::fprintf(stderr, "FAIL: ResourceManager (oracle culler)\n");
        return 1;
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_clustered_gpu.staging";
    BufferDesc dumpDesc{};
    dumpDesc.size = static_cast<usize>(kPixels) * 16u;
    dumpDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    dumpDesc.memoryUsage = MemoryUsage::GpuToCpu;
    dumpDesc.name = "rp_clustered_gpu.dump";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: buffers\n");
        return 1;
    }
    for (Buffer& b : ctx.dumps) {
        if (!ctx.allocator->createBuffer(dumpDesc, b) || b.mapped == nullptr || b.deviceAddress == 0u) {
            std::fprintf(stderr, "FAIL: dump buffers\n");
            return 1;
        }
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
    if (!uploadTextures(ctx)) {
        std::fprintf(stderr, "FAIL: textures\n");
        return 1;
    }
    return 0;
}

bool ensureReadback(Context& ctx, const ReadbackLayout& layout) {
    if (ctx.readback.handle != nullptr && ctx.readback.desc.size >= layout.end) {
        return true;
    }
    if (ctx.readback.handle != nullptr) {
        ctx.allocator->destroyBuffer(ctx.readback);
    }
    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<usize>(layout.end);
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_clustered_gpu.readback";
    return ctx.allocator->createBuffer(readbackDesc, ctx.readback) && ctx.readback.mapped != nullptr;
}

// --- scene ----------------------------------------------------------------------------------------
struct LightScene {
    u32 points = 0;
    u32 areas = 0; ///< WP-2.2 rectangle / disk lights (after the points: the oracle clusters them as points)
    u32 spots = 0;
    f32 rangeScale = 1.f;
    bool dense = false; ///< overflow mode: a dense ball of lights in front of the camera
};

struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
    std::vector<LightHandle> lights;
    std::vector<u32> pointSlots; ///< slots of point lights (for removal / re-add)
    std::vector<LightHandle> removed;
    std::mt19937 rng{2024};
};

GpuLight makePoint(std::mt19937& rng, const LightScene& ls) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    GpuLight l{};
    l.type = static_cast<u32>(GpuLightType::Point);
    if (ls.dense) {
        l.position[0] = u(rng) * 2.f;
        l.position[1] = u(rng) * 1.2f;
        l.position[2] = -9.f + u(rng) * 2.f;
    } else {
        l.position[0] = u(rng) * 15.f;
        l.position[1] = -1.6f + 4.2f * (u(rng) * 0.5f + 0.5f);
        l.position[2] = -3.f - 34.f * (u(rng) * 0.5f + 0.5f);
    }
    l.range = (0.5f + 2.0f * (u(rng) * 0.5f + 0.5f)) * ls.rangeScale;
    l.color[0] = 0.6f + 0.4f * (u(rng) * 0.5f + 0.5f);
    l.color[1] = 0.5f + 0.5f * (u(rng) * 0.5f + 0.5f);
    l.color[2] = 0.4f + 0.6f * (u(rng) * 0.5f + 0.5f);
    l.intensity = 1.5f + 3.f * (u(rng) * 0.5f + 0.5f);
    return l;
}

GpuLight makeSpot(std::mt19937& rng, const LightScene& ls) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    GpuLight l = makePoint(rng, ls);
    l.type = static_cast<u32>(GpuLightType::Spot);
    l.range = (1.5f + 2.5f * (u(rng) * 0.5f + 0.5f)) * ls.rangeScale;
    const Vec3 d = Vec3{u(rng) * 0.6f, -1.f, u(rng) * 0.6f}.normalized();
    l.direction[0] = d.x;
    l.direction[1] = d.y;
    l.direction[2] = d.z;
    l.cosInner = 0.93f + 0.05f * (u(rng) * 0.5f + 0.5f);
    l.cosOuter = l.cosInner - 0.15f;
    l.intensity *= 2.f;
    return l;
}

/// WP-2.2 area light (rectangle for even `i`, disk for odd), facing down-ish, points' placement.
GpuLight makeArea(std::mt19937& rng, const LightScene& ls, u32 i) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    const GpuLight p = makePoint(rng, ls);
    ltc::AreaLightDesc d{};
    d.center = {p.position[0], p.position[1], p.position[2]};
    d.normal = Vec3{0.4f * u(rng), -1.f, 0.4f * u(rng)}.normalized();
    d.tangent = Vec3{u(rng), u(rng), u(rng)};
    d.halfWidth = 0.15f + 0.35f * (u(rng) * 0.5f + 0.5f);
    d.halfHeight = i % 4u == 1u ? d.halfWidth : 0.15f + 0.35f * (u(rng) * 0.5f + 0.5f);
    d.color = {p.color[0], p.color[1], p.color[2]};
    d.intensity = 2.f * p.intensity;
    d.range = p.range * 1.5f;
    return i % 2u == 0u ? ltc::makeRectLight(d) : ltc::makeDiskLight(d);
}

bool buildScene(Context& ctx, Scene& s, const LightScene& ls) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = kObjects + 64u;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    const mr_test::SourceMesh sources[4] = {mr_test::uvSphere(16, 24, 1.f), mr_test::torus(24, 12, 1.f, 0.35f), mr_test::box(),
                                            mr_test::plane(16, 60.f, 12.f)};
    s.meshes.resize(4);
    for (u32 i = 0; i < 4u; ++i) {
        if (!mr_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    const std::vector<Material::GPUMaterial> materials = mr_test::makeMaterials(ctx.textureHandles);
    for (u32 i = 0; i < materials.size(); ++i) {
        s.gpu.setMaterial(i, materials[i]);
    }
    std::mt19937 rng(4321);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    InstanceDesc ground{};
    ground.mesh = 3;
    ground.material = mr_test::kMatFlat;
    ground.transform = place(0.f, -1.8f, -20.f, 1.f, 1.f, 1.f, 0.35f);
    s.handles.push_back(s.gpu.addInstance(ground));
    const u32 materialsCycle[6] = {mr_test::kMatFlat,     mr_test::kMatTextured, mr_test::kMatNormalMapped,
                                   mr_test::kMatEmissive, mr_test::kMatLodProbe, kInvalidIndex};
    for (u32 i = 0; i < kObjects; ++i) {
        InstanceDesc id{};
        id.mesh = i % 3u;
        id.material = id.mesh == 2u ? mr_test::kMatAoEmissiveTex : materialsCycle[(i / 3u) % 6u];
        const f32 sc = 0.4f + 0.5f * (u(rng) * 0.5f + 0.5f);
        id.transform = place(u(rng) * 14.f, -0.6f + 3.f * (u(rng) * 0.5f + 0.5f), -6.f - 30.f * (u(rng) * 0.5f + 0.5f), sc, sc,
                             sc, u(rng) * 3.f);
        s.handles.push_back(s.gpu.addInstance(id));
        if (i % 5u == 2u) {
            s.movers.push_back(static_cast<u32>(s.handles.size() - 1u));
        }
    }
    // Lights: points, then (WP-2.2) area lights, then spots (slot order), then 2 directional lights (the
    // first a 0.27-degree sun disk when the scene has area lights), then lights without a range;
    // the table is padded to kLightSlots with more points' worth of no-range slots.
    for (u32 i = 0; i < ls.points; ++i) {
        s.lights.push_back(s.gpu.addLight(makePoint(s.rng, ls)));
        s.pointSlots.push_back(static_cast<u32>(s.lights.size() - 1u));
    }
    for (u32 i = 0; i < ls.areas; ++i) {
        s.lights.push_back(s.gpu.addLight(makeArea(s.rng, ls, i)));
    }
    for (u32 i = 0; i < ls.spots; ++i) {
        s.lights.push_back(s.gpu.addLight(makeSpot(s.rng, ls)));
    }
    GpuLight sun{};
    sun.type = static_cast<u32>(GpuLightType::Directional);
    const Vec3 sd = Vec3{0.35f, -1.f, -0.25f}.normalized();
    sun.direction[0] = sd.x;
    sun.direction[1] = sd.y;
    sun.direction[2] = sd.z;
    sun.color[0] = 1.f;
    sun.color[1] = 0.95f;
    sun.color[2] = 0.85f;
    sun.intensity = 0.6f;
    GpuLight sky = sun;
    if (ls.areas > 0u) {
        ltc::setSunAngularRadius(sun, 0.0047f); // WP-2.2: the sun as a disk (0.27 degrees)
    }
    s.lights.push_back(s.gpu.addLight(sun));
    sky.direction[0] = -0.2f;
    sky.direction[1] = -0.3f;
    sky.direction[2] = 0.93f;
    sky.intensity = 0.15f;
    s.lights.push_back(s.gpu.addLight(sky));
    while (s.lights.size() < kLightSlots) {
        // Spot lights without a range (after the spots, so slot order stays points-then-spots): they
        // occupy no cluster and contribute nothing.
        GpuLight none = makeSpot(s.rng, ls);
        none.range = s.lights.size() % 2u == 0u ? 0.f : -1.f;
        s.lights.push_back(s.gpu.addLight(none));
    }
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

/// Frame motion: objects, ~64 lights jitter; frame 2 removes 16 point lights, frame 3 re-adds them
/// (into the freed point slots: points stay below spots).
void animate(Scene& s, u32 frame, const LightScene& ls, u32 lightStep) {
    for (u32 k = 0; k < s.movers.size(); ++k) {
        const u32 i = s.movers[(k + frame) % s.movers.size()];
        GpuTransform t = s.gpu.transform(i);
        t.rows[0][3] += 0.09f;
        t.rows[1][3] -= 0.04f;
        s.gpu.setTransform(s.handles[i], t);
    }
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 k = frame % lightStep; k < ls.points + ls.areas + ls.spots; k += lightStep) {
        if (!s.gpu.lightAlive(s.lights[k])) {
            continue;
        }
        GpuLight l = s.gpu.light(k);
        l.position[0] += 0.2f * u(s.rng);
        l.position[1] += 0.1f * u(s.rng);
        l.position[2] += 0.2f * u(s.rng);
        s.gpu.setLight(s.lights[k], l);
    }
    if (frame == 2u) {
        for (u32 k = 0; k < 16u && k < s.pointSlots.size(); ++k) {
            const u32 slot = s.pointSlots[(k * 97u) % s.pointSlots.size()];
            if (s.gpu.removeLight(s.lights[slot])) {
                s.removed.push_back(s.lights[slot]);
            }
        }
    } else if (frame == 3u) {
        for (usize k = 0; k < s.removed.size(); ++k) {
            const LightHandle h = s.gpu.addLight(makePoint(s.rng, ls));
            if (h.valid() && h.slot < s.lights.size()) {
                s.lights[h.slot] = h;
            }
        }
        s.removed.clear();
    }
}

// --- rig ------------------------------------------------------------------------------------------
struct Rig {
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    ClusteredLighting lighting[kLanguages];
    bool built[kLanguages] = {};
    const char* language[kLanguages] = {"slang", "glsl"};
};

int initRig(Context& ctx, Rig& rig, const ClusterDesc& clusters) {
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
    MaterialResolveDesc md{};
    md.device = ctx.device.get();
    md.allocator = ctx.allocator.get();
    md.bindless = &ctx.bindless;
    md.width = kWidth;
    md.height = kHeight;
    if (!rig.resolve.init(md)) {
        return 0;
    }
    u32 built = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        ClusteredLightingDesc ld{};
        ld.device = ctx.device.get();
        ld.allocator = ctx.allocator.get();
        ld.bindless = &ctx.bindless;
        ld.clusters = clusters;
        ld.lightCapacity = kLightSlots;
        ld.language = k == 0u ? LightingKernelLanguage::Slang : LightingKernelLanguage::Glsl;
        rig.built[k] = rig.lighting[k].init(ld);
        built += rig.built[k] ? 1u : 0u;
        std::printf("  lighting kernels %s: %s\n", rig.language[k], rig.built[k] ? "built" : "not built, skipped");
    }
    return built > 0u ? 1 : 0;
}

void destroyRig(Rig& rig) {
    for (ClusteredLighting& l : rig.lighting) {
        l.destroy();
    }
    rig.resolve.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
    enum Kind : u8 { Image, Buffer } kind = Image;
    rg::TextureRef image;
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
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

struct FrameState {
    CopyRecord copies[16];
    u32 copyCount = 0;
    ReadbackLayout layout{};
};

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const FrameCamera& cam) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, cam.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    ResolveFrameDesc rf{};
    std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
    std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
    rf.scene = s.gpu.headerHandle();
    rf.vis = rig.vb.visStorageHandle();
    rf.sampler = ctx.samplerHandle;
    ok = rig.resolve.beginFrame(ctx.serial, rf) && ok;
    LightingFrameDesc lf{};
    lf.camera = cam.cluster;
    lf.scene = s.gpu.headerHandle();
    lf.lightCount = s.gpu.lightHighWater();
    std::memcpy(lf.ambient, kAmbient, sizeof(lf.ambient));
    lf.gbuffer = &rig.resolve;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            ok = rig.lighting[k].beginFrame(ctx.serial, lf) && ok;
        }
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, FrameState& fs, bool readback) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), rig.culler, cull);
    const ResolveGraphRefs gbuffer = rig.resolve.importInto(graph);
    rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Binned);
    LightingGraphRefs lighting[kLanguages]{};
    rg::BufferRef dumps[kLanguages]{};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        lighting[k] = rig.lighting[k].importInto(graph);
        rig.lighting[k].addAssignment(graph, lighting[k], sceneRefs);
        if (readback) {
            dumps[k] = graph.importBuffer(rg::ImportedBuffer{ctx.dumps[k].handle, ctx.dumps[k].desc.size, rg::kNoQueue, nullptr,
                                                             "rp_clustered_gpu.dump"});
        }
        rig.lighting[k].addShade(graph, lighting[k], sceneRefs, gbuffer, dumps[k], ctx.dumps[k].deviceAddress);
    }
    if (!readback) {
        return;
    }
    const rg::BufferRef rb = graph.importBuffer(
        rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_clustered_gpu.readback"});
    auto addCopy = [&](CopyRecord::Kind kind, rg::TextureRef image, rg::BufferRef buffer, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.kind = kind;
        c.image = image;
        c.buffer = buffer;
        c.dst = rb;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
        if (kind == CopyRecord::Buffer) {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        } else {
            pass.use(image, rg::Access::TransferSrc);
        }
        pass.use(rb, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    for (u32 i = 0; i < 5u; ++i) {
        addCopy(CopyRecord::Image, gbuffer.gbuffer[kGBufferRead[i]], {}, fs.layout.gbuffer[i],
                static_cast<u64>(kPixels) * kGBufferBytes[i]);
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        addCopy(CopyRecord::Buffer, {}, lighting[k].lists, fs.layout.lists[k], fs.layout.listsBytes);
        addCopy(CopyRecord::Image, lighting[k].output, {}, fs.layout.output[k], static_cast<u64>(kPixels) * 8u);
        graph.addPass("readback.dump", nullptr, nullptr).use(dumps[k], rg::Access::HostRead);
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

bool runFrame(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameCamera& cam, FrameState& fs, bool readback) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rig, cam)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    buildGraph(ctx, s, rig, graph, fs, readback);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.resolve.collectRetired(ctx.serial);
    for (ClusteredLighting& l : rig.lighting) {
        l.collectRetired(ctx.serial);
    }
    s.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

// --- analysis -------------------------------------------------------------------------------------
const u8* rb(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

struct GpuLists {
    LightListHeader header{};
    ClusterGridSoA grid{};
    std::vector<u32> directional;
};

void readLists(Context& ctx, const FrameState& fs, const ClusteredLighting& l, u32 k, u32 clusters, GpuLists& out) {
    const LightingBufferLayout& layout = l.layout();
    const u8* base = rb(ctx, fs.layout.lists[k]);
    std::memcpy(&out.header, base + layout.header, sizeof(out.header));
    out.grid.grid.resize(clusters);
    std::memcpy(out.grid.grid.data(), base + layout.grid, static_cast<usize>(clusters) * sizeof(ClusterGridEntry));
    const u32 total = std::min<u32>(out.header.totalEntries, clusters * l.capacity());
    out.grid.lightList.resize(total);
    std::memcpy(out.grid.lightList.data(), base + layout.lightList, static_cast<usize>(total) * 4u);
    out.directional.resize(std::min(out.header.directionalCount, l.lightCapacity()));
    std::memcpy(out.directional.data(), base + layout.directional, out.directional.size() * 4u);
}

bool sameGrid(const ClusterGridSoA& a, const ClusterGridSoA& b, u32* firstBad = nullptr) {
    if (a.grid.size() != b.grid.size() || a.lightList.size() != b.lightList.size()) {
        return false;
    }
    for (usize i = 0; i < a.grid.size(); ++i) {
        if (a.grid[i].offset != b.grid[i].offset || a.grid[i].count != b.grid[i].count) {
            if (firstBad != nullptr) {
                *firstBad = static_cast<u32>(i);
            }
            return false;
        }
    }
    return a.lightList == b.lightList;
}

struct DecodedGBuffer {
    std::vector<f32> depth;
    std::vector<Vec4> rt0, rt1, rt2, rt5;
};

void decodeGBuffer(Context& ctx, const FrameState& fs, DecodedGBuffer& g) {
    g.depth.resize(kPixels);
    g.rt0.resize(kPixels);
    g.rt1.resize(kPixels);
    g.rt2.resize(kPixels);
    g.rt5.resize(kPixels);
    const u8* rt0 = rb(ctx, fs.layout.gbuffer[0]);
    const u8* rt1 = rb(ctx, fs.layout.gbuffer[1]);
    const u8* rt2 = rb(ctx, fs.layout.gbuffer[2]);
    const u8* rt4 = rb(ctx, fs.layout.gbuffer[3]);
    const u8* rt5 = rb(ctx, fs.layout.gbuffer[4]);
    for (usize p = 0; p < kPixels; ++p) {
        std::memcpy(&g.depth[p], rt4 + p * 4u, 4u);
        g.rt0[p] = {halfAt(rt0, p * 4u), halfAt(rt0, p * 4u + 1u), halfAt(rt0, p * 4u + 2u), halfAt(rt0, p * 4u + 3u)};
        g.rt5[p] = {halfAt(rt5, p * 4u), halfAt(rt5, p * 4u + 1u), halfAt(rt5, p * 4u + 2u), halfAt(rt5, p * 4u + 3u)};
        g.rt1[p] = {rt1[p * 4u] / 255.f, rt1[p * 4u + 1u] / 255.f, rt1[p * 4u + 2u] / 255.f, rt1[p * 4u + 3u] / 255.f};
        g.rt2[p] = {rt2[p * 4u] / 255.f, rt2[p * 4u + 1u] / 255.f, rt2[p * 4u + 2u] / 255.f, rt2[p * 4u + 3u] / 255.f};
    }
}

struct LanguageReport {
    bool listsEqual = false;
    bool totalsEqual = false;
    bool directionalEqual = false;
    u32 shadeBad = 0;
    u32 alphaBad = 0;
    u32 outputBad = 0;
    f64 maxRel = 0.0;
    f64 maxAbs = 0.0;
    u32 shaded = 0;
    GpuLists lists{};
    std::vector<Vec4> dump;
};

struct FrameReport {
    u32 covered = 0;
    u32 liveLights = 0;
    u32 clustered = 0;
    u64 entries = 0;
    u32 nonEmpty = 0;
    u32 atCapacity = 0;
    u32 dropped = 0;
    u32 maxPerCluster = 0;
    bool adapterEqual = false;
    f64 frameMax = 0.0;
    LanguageReport lang[kLanguages];
};

void analyse(Context& ctx, Scene& s, Rig& rig, const FrameState& fs, const FrameCamera& cam, const ClusterDesc& clusters,
             FrameReport& r) {
    // Oracle: the real ClusteredLightCuller on the scene's point / spot lights.
    const u32 slots = s.gpu.lightHighWater();
    std::vector<GpuLight> table(slots);
    for (u32 i = 0; i < slots; ++i) {
        table[i] = s.gpu.light(i);
        r.liveLights += table[i].type != 0u ? 1u : 0u;
    }
    OracleLights oracle{};
    makeOracleLights(table.data(), slots, oracle);
    expect(oracle.monotone, "scene keeps point slots below spot slots (monotone translation)");
    r.clustered = static_cast<u32>(oracle.points.size() + oracle.spots.size());
    ClusteredLightCuller culler;
    culler.init(clusters, ctx.resources);
    expect(culler.isReady(), "oracle culler ready");
    culler.cullLights(oracle.points, oracle.spots, cam.cluster);
    ClusterGridSoA expected{};
    translateToSlots(culler.gridSoA(), oracle, expected);
    ClusterGridSoA adapter{};
    oracleLightGrid(clusters, cam.cluster, oracle, adapter);
    ClusterGridSoA adapterSlots{};
    translateToSlots(adapter, oracle, adapterSlots);
    r.adapterEqual = sameGrid(adapterSlots, expected);
    const ClusteredLightCullerStats stats = culler.stats();
    culler.destroy();
    r.entries = expected.lightList.size();
    r.atCapacity = stats.clustersAtCapacity;
    r.dropped = stats.lightsDroppedOverflow;
    for (const ClusterGridEntry& e : expected.grid) {
        r.nonEmpty += e.count != 0u ? 1u : 0u;
        r.maxPerCluster = std::max(r.maxPerCluster, e.count);
    }

    DecodedGBuffer g{};
    decodeGBuffer(ctx, fs, g);
    for (const f32 d : g.depth) {
        r.covered += d < 1.f ? 1u : 0u;
    }
    ShadeReferenceDesc sd{};
    sd.width = kWidth;
    sd.height = kHeight;
    sd.desc = clusters;
    sd.camera = cam.cluster;
    sd.ambient = {kAmbient[0], kAmbient[1], kAmbient[2]};
    sd.gbuffer = GBufferTexels{g.depth.data(), g.rt0.data(), g.rt1.data(), g.rt2.data(), g.rt5.data()};
    sd.lights = table.data();
    sd.lightCount = slots;
    sd.grid = &expected;
    sd.directional = &oracle.directional;
    sd.brdfLut = rig.lighting[rig.built[0] ? 0u : 1u].brdfLut(); // WP-2.2: the compensated BRDF + area lights
    std::vector<Vec4> ref;
    shadeReferenceFrame(sd, ref);
    for (const Vec4& v : ref) {
        r.frameMax = std::max({r.frameMax, static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)});
    }

    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        LanguageReport& L = r.lang[k];
        readLists(ctx, fs, rig.lighting[k], k, clusters.clusterCount(), L.lists);
        u32 firstBad = ~0u;
        L.listsEqual = sameGrid(L.lists.grid, expected, &firstBad);
        if (!L.listsEqual) {
            std::fprintf(stderr, "  %s: lists differ (entries gpu %zu cpu %zu, first bad cluster %u)\n", rig.language[k],
                         L.lists.grid.lightList.size(), expected.lightList.size(), firstBad);
        }
        L.totalsEqual = L.lists.header.totalEntries == expected.lightList.size() &&
                        L.lists.header.clustersAtCapacity == stats.clustersAtCapacity &&
                        L.lists.header.droppedTotal == stats.lightsDroppedOverflow &&
                        L.lists.header.nonEmptyClusters == r.nonEmpty && L.lists.header.lightCount == slots &&
                        L.lists.header.clusterCount == clusters.clusterCount();
        L.directionalEqual = L.lists.directional == oracle.directional;
        L.dump.resize(kPixels);
        std::memcpy(L.dump.data(), ctx.dumps[k].mapped, static_cast<usize>(kPixels) * sizeof(Vec4));
        const u8* out = rb(ctx, fs.layout.output[k]);
        for (usize p = 0; p < kPixels; ++p) {
            const Vec4& gv = L.dump[p];
            const Vec4& cv = ref[p];
            L.shaded += gv.w > 0.f ? 1u : 0u;
            if ((gv.w > 0.f) != (cv.w > 0.f)) {
                ++L.alphaBad;
                continue;
            }
            const f32 gc[3] = {gv.x, gv.y, gv.z};
            const f32 cc[3] = {cv.x, cv.y, cv.z};
            bool bad = false;
            for (u32 c = 0; c < 3u; ++c) {
                const f64 diff = std::fabs(static_cast<f64>(gc[c]) - cc[c]);
                const f64 mag = std::fabs(static_cast<f64>(cc[c]));
                L.maxAbs = std::max(L.maxAbs, diff);
                if (mag > 1e-3) {
                    L.maxRel = std::max(L.maxRel, diff / mag);
                }
                bad = bad || !(diff <= kTolRel * mag + kTolAbs);
                // RGBA16F output == the dump rounded to half (<= 1 half ulp: 2^-10 relative, or the
                // subnormal spacing 2^-24).
                const f64 h = halfAt(out, p * 4u + c);
                const f64 hd = std::fabs(h - gc[c]);
                if (!(hd <= std::fabs(static_cast<f64>(gc[c])) * (1.0 / 1024.0) + 6e-8)) {
                    ++L.outputBad;
                }
            }
            L.shadeBad += bad ? 1u : 0u;
        }
    }
}

int runParity(Context& ctx, const ClusterDesc& clusters, const LightScene& ls, const char* label) {
    Rig rig;
    const int rc = initRig(ctx, rig, clusters);
    if (rc < 0) {
        std::fprintf(stderr, "FAIL: rig init\n");
        return 1;
    }
    if (rc == 0) {
        destroyRig(rig);
        std::printf("SKIP: no clustered-lighting kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, ls)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    FrameState fs;
    const u32 lang = rig.built[0] ? 0u : 1u;
    fs.layout = makeLayout(rig.lighting[lang].layout().listsBytes);
    if (!ensureReadback(ctx, fs.layout)) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    std::printf("  %s: %u light slots (%u points, %u area, %u spots, 2 directional), %ux%ux%u clusters, capacity %u, %ux%u\n",
                label, s.gpu.lightHighWater(), ls.points, ls.areas, ls.spots, clusters.tilesX, clusters.tilesY, clusters.slicesZ,
                rig.lighting[lang].capacity(), kWidth, kHeight);
    std::printf("  frame live clustered covered | entries non-empty max/cl at-cap dropped | lang lists totals dir "
                "shaded alpha-bad shade-bad max-rel max-abs out16-bad\n");
    rg::Graph graph;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        beginSceneFrame(ctx, s);
        if (frame > 0u) {
            animate(s, frame, ls, 64u);
        }
        const FrameCamera cam = frameCamera(frame);
        if (!runFrame(ctx, s, rig, graph, cam, fs, true)) {
            std::fprintf(stderr, "FAIL: frame %u\n", frame);
            return 1;
        }
        FrameReport r{};
        analyse(ctx, s, rig, fs, cam, clusters, r);
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            const LanguageReport& L = r.lang[k];
            std::printf("  %5u %4u %9u %7u | %7llu %9u %6u %6u %7u | %5s %5s %6s %3s %6u %9u %9u %.2e %.2e %9u\n", frame,
                        r.liveLights, r.clustered, r.covered, static_cast<unsigned long long>(r.entries), r.nonEmpty,
                        r.maxPerCluster, r.atCapacity, r.dropped, rig.language[k], L.listsEqual ? "==" : "DIFF",
                        L.totalsEqual ? "==" : "DIFF", L.directionalEqual ? "==" : "DIFF", L.shaded, L.alphaBad, L.shadeBad,
                        L.maxRel, L.maxAbs, L.outputBad);
            expect(L.listsEqual, "GPU light lists == ClusteredLightCuller (grid offsets / counts and flat list)");
            expect(L.totalsEqual, "GPU list totals == the culler's stats");
            expect(L.directionalEqual, "GPU directional list == directional slots");
            expect(L.alphaBad == 0u, "GPU and CPU shade the same pixels");
            expect(L.shadeBad == 0u, "GPU shade == CPU reference within kTolRel * |ref| + kTolAbs");
            expect(L.outputBad == 0u, "RGBA16F output == f32 dump rounded to half");
        }
        std::printf("        frame max radiance %.3g\n", r.frameMax);
        expect(r.adapterEqual, "oracleLightGrid (CPU gates' adapter) == ClusteredLightCuller");
        expect(r.covered > kPixels / 3u, "the scene covers the view");
        if (!ls.dense) {
            expect(r.entries > 5000u && r.nonEmpty > clusters.clusterCount() / 8u, "the lights fill the grid");
        } else {
            expect(r.atCapacity > 0u && r.dropped > 0u, "overflow mode: clusters overflow");
        }
        if (rig.built[0] && rig.built[1]) {
            u32 dumpDiff = 0;
            f64 dumpMax = 0.0;
            for (usize p = 0; p < kPixels; ++p) {
                const Vec4& a = r.lang[0].dump[p];
                const Vec4& b = r.lang[1].dump[p];
                dumpDiff += std::memcmp(&a, &b, sizeof(Vec4)) != 0 ? 1u : 0u;
                dumpMax = std::max({dumpMax, std::fabs(static_cast<f64>(a.x) - b.x), std::fabs(static_cast<f64>(a.y) - b.y),
                                    std::fabs(static_cast<f64>(a.z) - b.z)});
            }
            const bool listsSame = sameGrid(r.lang[0].lists.grid, r.lang[1].lists.grid);
            std::printf("        slang vs glsl: lists %s, shade %u px differ (max abs %.2e)\n", listsSame ? "==" : "DIFF",
                        dumpDiff, dumpMax);
            expect(listsSame, "Slang == GLSL light lists");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "light.", 6) == 0) {
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
    ClusterDesc clusters{};
    if (initRig(ctx, rig, clusters) != 1) {
        destroyRig(rig);
        std::printf("SKIP: no clustered-lighting kernels built\n");
        return kSkip;
    }
    // One language is enough for the allocation count.
    if (rig.built[0] && rig.built[1]) {
        rig.lighting[1].destroy();
        rig.built[1] = false;
    }
    Scene s;
    LightScene ls{};
    ls.points = 2600;
    ls.spots = 1400;
    if (!buildScene(ctx, s, ls)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long lightingSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s);
        animate(s, frame % 2u, ls, 64u); // frames 0/1 only: lights and objects move, no add / remove
        s.gpu.commit();
        ctx.upload.flush();
        const FrameCamera cam = frameCamera(frame % 4u);
        // Everything but the lighting's beginFrame, then the lighting's beginFrame counted alone.
        CullFrameDesc cf{};
        std::memcpy(cf.viewProj, cam.viewProj.m, sizeof(cf.viewProj));
        cf.instanceCount = s.gpu.instanceHighWater();
        bool ok = rig.culler.beginFrame(ctx.serial, cf);
        ok = rig.vb.beginFrame(ctx.serial, cam.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
        ResolveFrameDesc rf{};
        std::memcpy(rf.viewProj, cam.viewProj.m, sizeof(rf.viewProj));
        std::memcpy(rf.prevViewProj, cam.viewProj.m, sizeof(rf.prevViewProj));
        rf.scene = s.gpu.headerHandle();
        rf.vis = rig.vb.visStorageHandle();
        rf.sampler = ctx.samplerHandle;
        ok = rig.resolve.beginFrame(ctx.serial, rf) && ok;
        LightingFrameDesc lf{};
        lf.camera = cam.cluster;
        lf.scene = s.gpu.headerHandle();
        lf.lightCount = s.gpu.lightHighWater();
        std::memcpy(lf.ambient, kAmbient, sizeof(lf.ambient));
        lf.gbuffer = &rig.resolve;
        t_allocations = 0;
        t_count = measure;
        ok = rig.lighting[0].beginFrame(ctx.serial, lf) && ok;
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, s, rig, graph, fs, false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rig.culler.collectRetired(ctx.serial);
        rig.vb.collectRetired(ctx.serial);
        rig.resolve.collectRetired(ctx.serial);
        rig.lighting[0].collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        expect(ok && result.ok && waited, "frame ok");
        if (measure) {
            lightingSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.lighting[0].stats().assignmentPasses == 1u && rig.lighting[0].stats().shadePasses == 1u,
           "one assignment and one shade per frame");
    expect(rig.lighting[0].stats().bufferRebuilds == 1u && rig.lighting[0].stats().gbufferBinds == 1u,
           "no buffer growth / G-buffer rebinding in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u light slots, ~1.5%% moving, culled VB + binned resolve + lighting)\n"
                    "  ClusteredLighting::beginFrame + light.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis, resolve and lighting imports + passes): %llu\n",
                    kTotal - kWarmup, s.gpu.lightHighWater(), lightingSide, callbacks, build);
        expect(lightingSide == 0u, "lighting makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the lighting passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity_4096";
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
        if (mode == "parity_4096") {
            LightScene ls{};
            ls.points = 2500;
            ls.areas = 100; // WP-2.2: 50 rectangles + 50 disks (clustered like points)
            ls.spots = 1400;
            rc = runParity(ctx, ClusterDesc{}, ls, "parity_4096");
        } else if (mode == "overflow") {
            ClusterDesc clusters{};
            clusters.maxLightsPerCluster = 24;
            LightScene ls{};
            ls.points = 2000;
            ls.spots = 1000;
            ls.dense = true;
            ls.rangeScale = 0.8f;
            rc = runParity(ctx, clusters, ls, "overflow");
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
