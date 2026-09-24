// WP-1.5 material-resolve Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_material_resolve_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, VisBuffer::addCulledFrame
// (WP-1.3 two-phase culling + WP-1.4 visibility draws), then
//   forward   "resolve.forward": the forward G-buffer reference (no-culling draws, rasteriser-interpolated
//             attributes, hardware UV derivatives, the same material code and write_gbuffer packing)
//   binned    "resolve.reset" + "resolve.classify" + "resolve.gbuffer" (4 bins, vkCmdDrawIndirect each)
//   uber      "resolve.gbuffer" (one full-screen pass)
//   dump      "resolve.attributes" (the reconstructed attributes per pixel)
// and read-back copies. Scene: 4 meshlet meshes with UVs / normals / tangents (sphere, torus, a
// 2-submesh box, a ground plane) x 241 instances, 7 materials (flat, textured, normal-mapped, the LOD
// probe, emissive, AO + emissive + metallic textures with clear coat, a normal-mapped cloth box side)
// + instances without a material, 7 mip-mapped bindless textures; 5 frames of camera + object motion,
// alternating an anisotropic (8x) and a trilinear sampler. Checked every frame, for each kernel
// language built (Slang, GLSL):
//   ids        the forward reference's (instance, triangle) == the visibility buffer's on every pixel
//              (raster; atomic: >= 99.5%, the unorm24 target breaks near-ties differently) and the
//              material ids are equal wherever the ids are
//   gbuffer    resolve vs forward on every pixel with equal ids (tolerances, see kTol* below): shading
//              model, emissive mask, alpha exact; depth <= 1e-5; normal angle <= 4e-3 rad (two RGBA16F
//              oct quanta) + the normal map's one-mip bound; albedo / roughness / metallic <= 2 codes
//              + each texture's one-mip bound (adjacentMipBound: largest change of a trilinear sample
//              when the LOD moves by one level); AO / emissive likewise in half precision; velocity
//              <= 1/64 px + 2e-3 |v| (RG16F)
//   lod        LOD probe material (every mip level constant L / 16): |LOD_resolve - LOD_forward| <= 1
//              mip on every pixel (+ the 8-bit read-back quantum); the probe spans >= 3 mips
//   identical  binned == uber, bit for bit, on all 7 targets (negative control: one flipped byte)
//   reference  the GPU attribute dump == the CPU kernel "material_resolve_attributes" on the read-back
//              visibility image (flags / material / bin exact; b, depth <= 2e-5; derivatives, UVs,
//              normal, tangent relative 1e-4 of their scale; velocity 1e-3 px)
//   classify   the GPU tile lists (bin counts + sorted tiles) == "material_resolve_classify"
// and Slang == GLSL on the resolve output (reported; ids, material ids and bins must be equal).
//
//   --mode parity          raster visibility buffer
//   --mode parity_atomic   64-bit atomic visibility buffer (skip without 64-bit atomics)
//   --mode zero_alloc      64 steady-state frames (binned): 0 operator-new calls in
//                          MaterialResolve::beginFrame, the graph build and the resolve.* / vis.* /
//                          cull.* pass callbacks (validated run first; validation off for the count)
//   --backend set | buffer bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/geometry/vertex_codec_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/material_resolve/resolve_reference.hpp>
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
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::visbuffer;
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
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kObjects = 240;
constexpr u32 kFrames = 5;
constexpr u32 kTargets = kResolveColorAttachments; // RT0..RT5 + material id
constexpr u32 kTexelBytes[kTargets] = {8u, 4u, 4u, 4u, 4u, 8u, 4u};

// Tolerances of resolve vs forward (see the header comment and the WP-1.5 row).
constexpr f64 kTolDepth = 1e-5;       // D32 z/w from the rasteriser vs the decoded z/w (WP-1.4: <= 4.8e-7)
constexpr f64 kTolNormalRad = 4e-3;   // two RGBA16F oct quanta (encode_normal_rgba16f keeps each < 1e-3 rad)
constexpr f64 kTolUnorm8 = 2.0 / 255.0;
constexpr f64 kTolHalfRel = 2e-3;     // RGBA16F: 2^-10 relative + interpolation
constexpr f64 kTolVelocityAbs = 1.0 / 64.0;
constexpr f64 kTolVelocityRel = 2e-3;

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

Mat4 frameCamera(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    const f32 eye[3] = {-1.f + 0.35f * t, 1.4f - 0.08f * t, 3.f - 0.2f * t};
    const f32 at[3] = {0.3f * t - 0.5f, -0.6f, -16.f};
    return mul(perspective(1.1f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.3f, 120.f), lookAt(eye, at));
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

f32 halfAt(const u8* p, u32 i) {
    u16 h = 0;
    std::memcpy(&h, p + i * 2u, 2u);
    return fuse::renderer::geometry::vertex_codec::half_to_float(h);
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
    Buffer attributes{};
    Buffer texStaging{};
    std::vector<Texture> textures;
    std::vector<BindlessSlotHandle> textureSlots;
    u32 textureHandles[mr_test::kTexCount] = {};
    u32 textureSizes[mr_test::kTexCount] = {};
    std::vector<f32> levelBounds[mr_test::kTexCount];
    BindlessSlotHandle samplers[2]{};
    u32 samplerHandles[2] = {};
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (device != nullptr) {
            for (BindlessSlotHandle& s : textureSlots) {
                bindless.unregisterSlot(s);
            }
            for (BindlessSlotHandle& s : samplers) {
                if (s.isValid()) {
                    bindless.releaseSampler(s);
                }
            }
        }
        if (allocator != nullptr) {
            for (Texture& t : textures) {
                allocator->destroyImage(t);
            }
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
            allocator->destroyBuffer(attributes);
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

/// Byte layout of the readback buffer (every section 256-aligned).
struct ReadbackLayout {
    u64 vis = 0;
    u64 sets[3][kTargets] = {}; ///< binned, uber, forward
    u64 forwardIds = 0;
    u64 attributes = 0;
    u64 bins = 0;
    u64 binsBytes = 0;
    u64 end = 0;
};

ReadbackLayout makeLayout() {
    ReadbackLayout l{};
    u64 cursor = 0;
    auto take = [&](u64 bytes) {
        const u64 at = cursor;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        return at;
    };
    l.vis = take(kPixels * 8ull);
    for (auto& set : l.sets) {
        for (u32 t = 0; t < kTargets; ++t) {
            set[t] = take(static_cast<u64>(kPixels) * kTexelBytes[t]);
        }
    }
    l.forwardIds = take(kPixels * 8ull);
    l.attributes = take(kPixels * sizeof(ResolveAttributeTexel));
    const u32 tiles = ((kWidth + 7u) / 8u) * ((kHeight + 7u) / 8u);
    l.binsBytes = ResolveBinLayout::bytes(tiles);
    l.bins = take(l.binsBytes);
    l.end = cursor;
    return l;
}
const ReadbackLayout g_layout = makeLayout();

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
    sd.name = "rp_material_resolve.tex_staging";
    if (!ctx.allocator->createBuffer(sd, ctx.texStaging) || ctx.texStaging.mapped == nullptr) {
        return false;
    }
    rg::Graph graph;
    std::vector<TexUpload> uploads(data.size());
    const rg::BufferRef staging = graph.importBuffer(rg::ImportedBuffer{ctx.texStaging.handle, bytes, rg::kNoQueue, nullptr,
                                                                        "rp_material_resolve.tex_staging"});
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
        td.name = "rp_material_resolve.texture";
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
        im.name = "rp_material_resolve.texture";
        u.image = graph.importImage(im);
        u.staging = staging;
        graph.addPass("upload.texture", &recordTexUpload, &u)
            .use(u.image, rg::Access::TransferDst)
            .use(staging, rg::Access::TransferSrc);
        ctx.levelBounds[i] = mr_test::adjacentMipBounds(t);
        ctx.textureSizes[i] = t.width;
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
    SamplerDesc aniso{};
    aniso.minFilter = VK_FILTER_LINEAR;
    aniso.magFilter = VK_FILTER_LINEAR;
    aniso.addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    aniso.anisotropy = true;
    aniso.maxAnisotropy = 8.f;
    aniso.name = "rp_material_resolve.aniso";
    SamplerDesc trilinear = aniso;
    trilinear.anisotropy = false;
    trilinear.maxAnisotropy = 1.f;
    trilinear.name = "rp_material_resolve.trilinear";
    ctx.samplers[0] = ctx.bindless.acquireSampler(aniso);
    ctx.samplers[1] = ctx.bindless.acquireSampler(trilinear);
    for (u32 i = 0; i < 2u; ++i) {
        if (!ctx.samplers[i].isValid()) {
            return false;
        }
        ctx.samplerHandles[i] = ctx.bindless.shaderHandle(ctx.samplers[i]);
    }
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
    instanceDesc.appName = "fuse_rp_material_resolve";
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
    const ResolveCapabilities caps = queryResolveCapabilities(ctx.device.get());
    if (!caps.resolve || !caps.forward) {
        std::printf("SKIP: material resolve unsupported: %s\n", caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s (anisotropy %s, max %.0f)\n", ctx.device->info().deviceName.c_str(),
                ctx.device->info().samplerAnisotropy ? "yes" : "no", ctx.device->info().maxSamplerAnisotropy);
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
    stagingDesc.name = "rp_material_resolve.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = g_layout.end;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_material_resolve.readback";
    BufferDesc attrDesc{};
    attrDesc.size = kPixels * sizeof(ResolveAttributeTexel);
    attrDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::TransferSrc) |
                                              static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    attrDesc.memoryUsage = MemoryUsage::GpuOnly;
    attrDesc.name = "rp_material_resolve.attributes";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr ||
        !ctx.allocator->createBuffer(attrDesc, ctx.attributes) || ctx.attributes.deviceAddress == 0u) {
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
    if (!uploadTextures(ctx)) {
        std::fprintf(stderr, "FAIL: textures\n");
        return 1;
    }
    std::printf("one-mip bounds per level (albedo, roughness, normal, lod probe, emissive, ao, metallic):\n");
    for (const std::vector<f32>& b : ctx.levelBounds) {
        std::printf("   ");
        for (const f32 v : b) {
            std::printf(" %.4f", v);
        }
        std::printf("\n");
    }
    return 0;
}

// --- scene ----------------------------------------------------------------------------------------
struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<ResolveMeshData> data;
    std::vector<resolve_kernel::MeshStreams> streams;
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
    std::vector<Material::GPUMaterial> materials;
};

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
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
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
    s.materials = mr_test::makeMaterials(ctx.textureHandles);
    for (u32 i = 0; i < s.materials.size(); ++i) {
        s.gpu.setMaterial(i, s.materials[i]);
    }
    std::mt19937 rng(4321);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    InstanceDesc ground{};
    ground.mesh = 3;
    ground.material = mr_test::kMatLodProbe;
    ground.transform = place(0.f, -1.8f, -20.f, 1.f, 1.f, 1.f, 0.35f);
    s.handles.push_back(s.gpu.addInstance(ground));
    const u32 sphereTorusMaterials[6] = {mr_test::kMatFlat,     mr_test::kMatTextured, mr_test::kMatNormalMapped,
                                         mr_test::kMatEmissive, mr_test::kMatLodProbe, kInvalidIndex};
    for (u32 i = 0; i < kObjects; ++i) {
        InstanceDesc id{};
        id.mesh = i % 3u;
        id.material = id.mesh == 2u ? mr_test::kMatAoEmissiveTex : sphereTorusMaterials[(i / 3u) % 6u];
        const f32 sc = 0.4f + 0.5f * (u(rng) * 0.5f + 0.5f);
        const f32 mirror = i % 11u == 5u ? -1.f : 1.f;
        const f32 squash = i % 7u == 3u ? 0.6f : 1.f;
        id.transform = place(u(rng) * 14.f, -0.6f + 3.f * (u(rng) * 0.5f + 0.5f), -6.f - 30.f * (u(rng) * 0.5f + 0.5f),
                             sc * mirror, sc * squash, sc, u(rng) * 3.f);
        s.handles.push_back(s.gpu.addInstance(id));
        if (i % 5u == 2u) {
            s.movers.push_back(static_cast<u32>(s.handles.size() - 1u));
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
        t.rows[0][3] += 0.09f;
        t.rows[1][3] -= 0.04f;
        s.gpu.setTransform(s.handles[i], t);
    }
}

// --- per-language rig -----------------------------------------------------------------------------
struct Rig {
    const char* language = nullptr;
    InstanceCuller culler;
    InstanceCuller refCuller;
    VisBuffer vb;
    MaterialResolve binned;
    MaterialResolve uber;
    MaterialResolve forward;
};

/// 1 = ok, 0 = language not built, -1 = failure
int initRig(Context& ctx, Rig& rig, VisMode mode, ResolveKernelLanguage language, bool withReference) {
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = kObjects + 64u;
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
    if (!rig.vb.init(vd)) {
        return -1;
    }
    MaterialResolveDesc md{};
    md.device = ctx.device.get();
    md.allocator = ctx.allocator.get();
    md.bindless = &ctx.bindless;
    md.width = kWidth;
    md.height = kHeight;
    md.language = language;
    if (!rig.binned.init(md)) {
        return 0;
    }
    if (withReference) {
        md.forward = true;
        if (!rig.uber.init(MaterialResolveDesc{md.device, md.allocator, md.bindless, kWidth, kHeight, language, false}) ||
            !rig.forward.init(md)) {
            return -1;
        }
    }
    rig.language = rig.binned.kernelLanguage();
    return 1;
}

void destroyRig(Rig& rig) {
    rig.binned.destroy();
    rig.uber.destroy();
    rig.forward.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
    rig.refCuller.destroy();
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
    CopyRecord copies[40];
    u32 copyCount = 0;
};

struct FrameOptions {
    Mat4 viewProj{};
    Mat4 prevViewProj{};
    u32 sampler = 0;
    bool withReference = true;
    bool readback = true;
};

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const FrameOptions& opt) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, opt.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = s.gpu.instanceHighWater();
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    ok = rig.vb.beginFrame(ctx.serial, opt.viewProj.m, s.gpu.headerHandle(), s.gpu.instanceHighWater()) && ok;
    ResolveFrameDesc rf{};
    std::memcpy(rf.viewProj, opt.viewProj.m, sizeof(rf.viewProj));
    std::memcpy(rf.prevViewProj, opt.prevViewProj.m, sizeof(rf.prevViewProj));
    rf.scene = s.gpu.headerHandle();
    rf.vis = rig.vb.visStorageHandle();
    rf.sampler = opt.sampler;
    ok = rig.binned.beginFrame(ctx.serial, rf) && ok;
    if (opt.withReference) {
        CullFrameDesc all = frame;
        all.frustum = false;
        all.occlusion = false;
        ok = rig.refCuller.beginFrame(ctx.serial, all) && ok;
        ok = rig.uber.beginFrame(ctx.serial, rf) && ok;
        ok = rig.forward.beginFrame(ctx.serial, rf) && ok;
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
    rig.vb.addCulledFrame(graph, vis, sceneRefs, sceneHandle, rig.culler, cull);
    const ResolveGraphRefs binned = rig.binned.importInto(graph);
    rig.binned.addResolve(graph, binned, vis.vis, sceneRefs, ResolvePath::Binned);
    if (!opt.withReference) {
        return;
    }
    const CullGraphRefs refCull = rig.refCuller.importInto(graph);
    const ResolveGraphRefs uber = rig.uber.importInto(graph);
    const ResolveGraphRefs forward = rig.forward.importInto(graph);
    rig.uber.addResolve(graph, uber, vis.vis, sceneRefs, ResolvePath::Uber);
    rig.refCuller.addPhase1(graph, refCull, sceneRefs, sceneHandle);
    rig.forward.addForward(graph, forward, sceneRefs, rig.refCuller, refCull, CullPhase::Phase1, true);
    const rg::BufferRef attr = graph.importBuffer(
        rg::ImportedBuffer{ctx.attributes.handle, ctx.attributes.desc.size, rg::kNoQueue, nullptr, "rp_material_resolve.attributes"});
    rig.binned.addAttributeDump(graph, binned, vis.vis, sceneRefs, attr, ctx.attributes.deviceAddress);
    if (!opt.readback) {
        return;
    }
    const rg::BufferRef readback = graph.importBuffer(
        rg::ImportedBuffer{ctx.readback.handle, g_layout.end, rg::kNoQueue, nullptr, "rp_material_resolve.readback"});
    auto addCopy = [&](CopyRecord::Kind kind, rg::TextureRef image, rg::BufferRef buffer, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.kind = kind;
        c.image = image;
        c.buffer = buffer;
        c.dst = readback;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
        if (kind == CopyRecord::Buffer) {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        } else {
            pass.use(image, rg::Access::TransferSrc);
        }
        pass.use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    addCopy(CopyRecord::Image, vis.vis, {}, g_layout.vis, kPixels * 8ull);
    const ResolveGraphRefs* sets[3] = {&binned, &uber, &forward};
    for (u32 k = 0; k < 3u; ++k) {
        for (u32 t = 0; t < kTargets; ++t) {
            addCopy(CopyRecord::Image, t + 1u < kTargets ? sets[k]->gbuffer[t] : sets[k]->materialId, {}, g_layout.sets[k][t],
                    static_cast<u64>(kPixels) * kTexelBytes[t]);
        }
    }
    addCopy(CopyRecord::Image, forward.forwardIds, {}, g_layout.forwardIds, kPixels * 8ull);
    addCopy(CopyRecord::Buffer, {}, attr, g_layout.attributes, kPixels * sizeof(ResolveAttributeTexel));
    addCopy(CopyRecord::Buffer, {}, binned.bins, g_layout.bins, g_layout.binsBytes);
    graph.addPass("readback.host", nullptr, nullptr).use(readback, rg::Access::HostRead);
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
    rig.binned.collectRetired(ctx.serial);
    rig.uber.collectRetired(ctx.serial);
    rig.forward.collectRetired(ctx.serial);
    s.gpu.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

// --- analysis -------------------------------------------------------------------------------------
const u8* rb(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

/// One G-buffer pixel, decoded.
struct Px {
    f32 n[3] = {0.f, 0.f, 0.f};
    f32 ao = 0.f;
    f32 albedo[4] = {0.f, 0.f, 0.f, 0.f};
    f32 rough = 0.f, metal = 0.f, mask = 0.f, shading = 0.f;
    f32 vel[2] = {0.f, 0.f};
    f32 depth = 0.f;
    f32 emissive[3] = {0.f, 0.f, 0.f};
    u32 material = 0;
};

Px readPx(Context& ctx, u32 set, u32 p) {
    Px r;
    const u8* rt0 = rb(ctx, g_layout.sets[set][0]) + p * 8u;
    const fuse::math::Vec3 n = GBufferEncoding::decodeOctSigned({halfAt(rt0, 0), halfAt(rt0, 1)});
    r.n[0] = n.x;
    r.n[1] = n.y;
    r.n[2] = n.z;
    r.ao = halfAt(rt0, 3);
    const u8* rt1 = rb(ctx, g_layout.sets[set][1]) + p * 4u;
    for (u32 k = 0; k < 4u; ++k) {
        r.albedo[k] = static_cast<f32>(rt1[k]) / 255.f;
    }
    const u8* rt2 = rb(ctx, g_layout.sets[set][2]) + p * 4u;
    r.rough = rt2[0] / 255.f;
    r.metal = rt2[1] / 255.f;
    r.mask = rt2[2] / 255.f;
    r.shading = static_cast<f32>(rt2[3]);
    const u8* rt3 = rb(ctx, g_layout.sets[set][3]) + p * 4u;
    r.vel[0] = halfAt(rt3, 0);
    r.vel[1] = halfAt(rt3, 1);
    std::memcpy(&r.depth, rb(ctx, g_layout.sets[set][4]) + p * 4u, 4u);
    const u8* rt5 = rb(ctx, g_layout.sets[set][5]) + p * 8u;
    for (u32 k = 0; k < 3u; ++k) {
        r.emissive[k] = halfAt(rt5, k);
    }
    std::memcpy(&r.material, rb(ctx, g_layout.sets[set][6]) + p * 4u, 4u);
    return r;
}

struct FrameReport {
    u32 covered = 0;
    u32 compared = 0;
    u32 idDiff = 0;
    u32 materialDiff = 0;
    u32 binnedUberDiff = 0;
    u32 bad[10] = {}; ///< depth, normal, albedo, alpha, rough, metal, mask/shading, ao, emissive, velocity
    f64 maxErr[10] = {};
    u32 lodPixels = 0;
    f64 lodMaxDiff = 0.0;
    f64 lodSumDiff = 0.0;
    f64 lodMin = 1e9, lodMax = -1e9;
    u32 lodBad = 0;
    u32 attrBad = 0;
    f64 attrMaxErr[6] = {}; ///< b/depth, derivatives, uv/duv, normal/tangent, velocity (relative)
    u32 classifyBad = 0;
    u32 binTiles[kBinCount] = {};
    std::vector<u8> binnedCopy; ///< every binned target (Slang vs GLSL)
};

enum Channel : u32 { kDepth, kNormal, kAlbedo, kAlpha, kRough, kMetal, kMaskShading, kAo, kEmissive, kVelocity, kChannels };
const char* const kChannelNames[kChannels] = {"depth", "normal", "albedo", "alpha",    "rough",
                                              "metal", "mask/sm", "ao",    "emissive", "velocity"};

void note(FrameReport& r, Channel c, f64 err, f64 tol) {
    r.maxErr[c] = std::max(r.maxErr[c], err);
    if (!(err <= tol)) {
        ++r.bad[c];
    }
}

f64 relErr(f64 a, f64 b, f64 scale) { return std::fabs(a - b) / std::max(scale, 1e-30); }

void analyse(Context& ctx, Scene& s, const Mat4& vp, const Mat4& prevVp, bool aniso, FrameReport& r) {
    const u32* vis = reinterpret_cast<const u32*>(rb(ctx, g_layout.vis));
    const u32* fids = reinterpret_cast<const u32*>(rb(ctx, g_layout.forwardIds));
    const ResolveAttributeTexel* gpu = reinterpret_cast<const ResolveAttributeTexel*>(rb(ctx, g_layout.attributes));
    // (1) binned == uber, bit for bit.
    for (u32 t = 0; t < kTargets; ++t) {
        const u8* a = rb(ctx, g_layout.sets[0][t]);
        const u8* b = rb(ctx, g_layout.sets[1][t]);
        for (u32 p = 0; p < kPixels; ++p) {
            r.binnedUberDiff += std::memcmp(a + p * kTexelBytes[t], b + p * kTexelBytes[t], kTexelBytes[t]) != 0 ? 1u : 0u;
        }
    }
    {
        std::vector<u8> copy(rb(ctx, g_layout.sets[1][1]), rb(ctx, g_layout.sets[1][1]) + kPixels * 4u);
        copy[4u * (kPixels / 2u) + 1u] ^= 0x01u;
        expect(std::memcmp(copy.data(), rb(ctx, g_layout.sets[0][1]), copy.size()) != 0,
               "negative control: one flipped albedo byte breaks binned == uber");
    }
    r.binnedCopy.clear();
    for (u32 t = 0; t < kTargets; ++t) {
        r.binnedCopy.insert(r.binnedCopy.end(), rb(ctx, g_layout.sets[0][t]),
                            rb(ctx, g_layout.sets[0][t]) + static_cast<usize>(kPixels) * kTexelBytes[t]);
    }
    // (2) resolve (binned) vs forward.
    for (u32 p = 0; p < kPixels; ++p) {
        const bool fwdCovered = fids[p * 2u] != kVisInvalid;
        const bool visCovered = vis[p * 2u] != kVisInvalid;
        const bool same = fids[p * 2u] == vis[p * 2u] && (!visCovered || fids[p * 2u + 1u] == vis[p * 2u + 1u]);
        if (!same) {
            ++r.idDiff;
            continue;
        }
        const Px g = readPx(ctx, 0, p);
        const Px f = readPx(ctx, 2, p);
        if (g.material != f.material) {
            ++r.materialDiff;
            continue;
        }
        if (!fwdCovered) {
            // Empty: both write the clear values (depth 1, everything else 0).
            note(r, kDepth, std::fabs(g.depth - f.depth), 0.0);
            note(r, kAlbedo, std::fabs(g.albedo[0] - f.albedo[0]), 0.0);
            continue;
        }
        ++r.covered;
        ++r.compared;
        const bool hasMat = g.material != kNoMaterial;
        const Material::GPUMaterial m = hasMat ? s.materials[g.material] : Material::GPUMaterial{};
        // Texture term of the tolerance: the one-mip bound of the levels around this pixel's LOD
        // (isotropic LOD of the resolve's analytic footprint; an 8x anisotropic sampler may pick up to
        // 3 levels finer).
        const ResolveAttributeTexel& at = gpu[p];
        auto texBound = [&](u32 handle) {
            for (u32 i = 0; i < mr_test::kTexCount; ++i) {
                if (hasMat && handle == ctx.textureHandles[i]) {
                    const f32 size = static_cast<f32>(ctx.textureSizes[i]);
                    const f32 lod = resolve_kernel::isotropic_lod(at.duvdx, at.duvdy, size, size);
                    return static_cast<f64>(mr_test::lodWindowBound(ctx.levelBounds[i], aniso ? lod - 3.f : lod, lod));
                }
            }
            return 0.0;
        };
        note(r, kDepth, std::fabs(static_cast<f64>(g.depth) - f.depth), kTolDepth);
        const f64 dotn = std::clamp(static_cast<f64>(g.n[0]) * f.n[0] + static_cast<f64>(g.n[1]) * f.n[1] +
                                        static_cast<f64>(g.n[2]) * f.n[2],
                                    -1.0, 1.0);
        note(r, kNormal, std::acos(dotn), kTolNormalRad + 3.0 * texBound(m.normalTexIdx) * m.normalStrength);
        const bool probe = hasMat && m.baseColorTexIdx == ctx.textureHandles[mr_test::kTexLodProbe];
        f64 albedoErr = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            albedoErr = std::max(albedoErr, std::fabs(static_cast<f64>(g.albedo[k]) - f.albedo[k]));
        }
        if (probe) {
            const f64 lg = g.albedo[0] * 16.0;
            const f64 lf = f.albedo[0] * 16.0;
            const f64 d = std::fabs(lg - lf);
            ++r.lodPixels;
            r.lodMaxDiff = std::max(r.lodMaxDiff, d);
            r.lodSumDiff += d;
            r.lodMin = std::min(r.lodMin, lg);
            r.lodMax = std::max(r.lodMax, lg);
            r.lodBad += d <= 1.0 + 16.0 / 255.0 ? 0u : 1u;
        } else {
            const f64 baseMax = hasMat ? std::max({m.baseColor.x, m.baseColor.y, m.baseColor.z}) : 1.0;
            note(r, kAlbedo, albedoErr, kTolUnorm8 + texBound(m.baseColorTexIdx) * baseMax);
        }
        note(r, kAlpha, std::fabs(g.albedo[3] - f.albedo[3]), 0.0);
        note(r, kRough, std::fabs(g.rough - f.rough), kTolUnorm8 + texBound(m.roughnessTexIdx) * m.roughnessEmissive.x);
        note(r, kMetal, std::fabs(g.metal - f.metal), kTolUnorm8 + texBound(m.metallicTexIdx) * m.baseColor.w);
        note(r, kMaskShading, std::fabs(g.mask - f.mask) + std::fabs(g.shading - f.shading), 0.0);
        note(r, kAo, std::fabs(g.ao - f.ao), kTolHalfRel + texBound(m.aoTexIdx));
        f64 emErr = 0.0, emScale = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            emErr = std::max(emErr, std::fabs(static_cast<f64>(g.emissive[k]) - f.emissive[k]));
            emScale = std::max(emScale, static_cast<f64>(std::fabs(f.emissive[k])));
        }
        const f64 emConst = hasMat ? std::max({m.roughnessEmissive.y, m.roughnessEmissive.z, m.roughnessEmissive.w}) *
                                         std::max(m.emissiveIntensity, 0.f)
                                   : 0.0;
        note(r, kEmissive, emErr, kTolHalfRel * emScale + 1e-6 + texBound(m.emissiveTexIdx) * emConst);
        const f64 vmag = std::max(std::fabs(f.vel[0]), std::fabs(f.vel[1]));
        note(r, kVelocity, std::max(std::fabs(g.vel[0] - f.vel[0]), std::fabs(g.vel[1] - f.vel[1])),
             kTolVelocityAbs + kTolVelocityRel * vmag);
    }
    // (3) attribute dump vs the CPU kernel.
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<ResolveAttributeTexel> cpu;
    attributes_reference(view, vp.m, prevVp.m, vis, kWidth, kHeight, cpu);
    for (u32 p = 0; p < kPixels; ++p) {
        const ResolveAttributeTexel& a = gpu[p];
        const ResolveAttributeTexel& c = cpu[p];
        if (a.flags != c.flags || a.material != c.material || a.bin != c.bin) {
            ++r.attrBad;
            continue;
        }
        if (c.flags != kAttrOk) {
            continue;
        }
        const f64 e0 = std::max({std::fabs(static_cast<f64>(a.b1) - c.b1), std::fabs(static_cast<f64>(a.b2) - c.b2),
                                 std::fabs(static_cast<f64>(a.depth) - c.depth)});
        const f64 ds = std::max({std::fabs(c.db1dx), std::fabs(c.db1dy), std::fabs(c.db2dx), std::fabs(c.db2dy), 1e-12f});
        const f64 e1 = std::max({relErr(a.db1dx, c.db1dx, ds), relErr(a.db1dy, c.db1dy, ds), relErr(a.db2dx, c.db2dx, ds),
                                 relErr(a.db2dy, c.db2dy, ds)});
        const f64 us = std::max({std::fabs(c.uv[0]), std::fabs(c.uv[1]), 1.f});
        const f64 dus = std::max({std::fabs(c.duvdx[0]), std::fabs(c.duvdx[1]), std::fabs(c.duvdy[0]), std::fabs(c.duvdy[1]),
                                  1e-12f});
        const f64 e2 = std::max({relErr(a.uv[0], c.uv[0], us), relErr(a.uv[1], c.uv[1], us), relErr(a.duvdx[0], c.duvdx[0], dus),
                                 relErr(a.duvdx[1], c.duvdx[1], dus), relErr(a.duvdy[0], c.duvdy[0], dus),
                                 relErr(a.duvdy[1], c.duvdy[1], dus)});
        f64 ns = 1e-12, ts = 1e-12, e3 = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            ns = std::max(ns, static_cast<f64>(std::fabs(c.normal[k])));
            ts = std::max(ts, static_cast<f64>(std::fabs(c.tangent[k])));
        }
        for (u32 k = 0; k < 3u; ++k) {
            e3 = std::max({e3, relErr(a.normal[k], c.normal[k], ns), relErr(a.tangent[k], c.tangent[k], ts)});
        }
        const f64 e4 = std::max(std::fabs(a.velocity[0] - c.velocity[0]), std::fabs(a.velocity[1] - c.velocity[1]));
        const bool signOk = a.tangentSign == c.tangentSign;
        r.attrMaxErr[0] = std::max(r.attrMaxErr[0], e0);
        r.attrMaxErr[1] = std::max(r.attrMaxErr[1], e1);
        r.attrMaxErr[2] = std::max(r.attrMaxErr[2], e2);
        r.attrMaxErr[3] = std::max(r.attrMaxErr[3], e3);
        r.attrMaxErr[4] = std::max(r.attrMaxErr[4], e4);
        r.attrBad += (e0 <= 2e-5 && e1 <= 1e-4 && e2 <= 1e-4 && e3 <= 1e-4 && e4 <= 1e-3 && signOk) ? 0u : 1u;
    }
    // (4) classification.
    std::vector<u32> tileBins;
    classify_reference(view, vis, kWidth, kHeight, tileBins);
    std::vector<u32> lists[kBinCount];
    const u32 tilesX = (kWidth + 7u) / 8u;
    tile_lists(tileBins, tilesX, lists);
    const u32* bins = reinterpret_cast<const u32*>(rb(ctx, g_layout.bins));
    const u32 capacity = static_cast<u32>(tileBins.size());
    for (u32 b = 0; b < kBinCount; ++b) {
        const u32 count = bins[b * 4u + 1u];
        r.binTiles[b] = count;
        if (bins[b * 4u] != ResolveBinLayout::kVerticesPerTile || count != lists[b].size()) {
            ++r.classifyBad;
            continue;
        }
        std::vector<u32> gpuList(bins + ResolveBinLayout::kListOffset / 4u + b * capacity,
                                 bins + ResolveBinLayout::kListOffset / 4u + b * capacity + count);
        std::sort(gpuList.begin(), gpuList.end());
        r.classifyBad += gpuList == lists[b] ? 0u : 1u;
    }
}

int runParity(Context& ctx, VisMode mode) {
    const bool atomic = mode == VisMode::Atomic64;
    std::vector<FrameReport> last;
    std::vector<const char*> languages;
    for (const ResolveKernelLanguage language : {ResolveKernelLanguage::Slang, ResolveKernelLanguage::Glsl}) {
        const char* want = language == ResolveKernelLanguage::Slang ? "slang" : "glsl";
        Rig rig;
        const int rc = initRig(ctx, rig, mode, language, true);
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init (%s)\n", want);
            return 1;
        }
        if (rc == 0) {
            std::printf("  language %s: not built, skipped\n", want);
            destroyRig(rig);
            continue;
        }
        Scene s;
        if (!buildScene(ctx, s)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        std::printf("  kernels: %s (vis %s, %s), %u instances, %zu materials, %u textures\n", rig.language,
                    rig.vb.kernelLanguage(), atomic ? "atomic64" : "raster", s.gpu.instanceHighWater(), s.materials.size(),
                    mr_test::kTexCount);
        std::printf("  frame smp covered id-diff mat-diff bin!=uber attr-bad cls-bad | tiles e/f/t/n | lod px max|d| "
                    "mean|d| range\n");
        rg::Graph graph;
        FrameState fs;
        FrameReport r;
        Mat4 prev = frameCamera(0);
        FrameReport total;
        for (u32 frame = 0; frame < kFrames; ++frame) {
            beginSceneFrame(ctx, s);
            if (frame > 0u) {
                moveObjects(s, frame, 1u);
            }
            FrameOptions opt{};
            opt.viewProj = frameCamera(frame);
            opt.prevViewProj = prev;
            opt.sampler = ctx.samplerHandles[frame % 2u];
            if (!runFrame(ctx, s, rig, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            r = FrameReport{};
            analyse(ctx, s, opt.viewProj, opt.prevViewProj, frame % 2u == 0u, r);
            prev = opt.viewProj;
            std::printf("  %5u %3s %7u %7u %8u %9u %8u %7u | %u/%u/%u/%u | %5u %6.3f %7.4f [%.2f, %.2f]\n", frame,
                        frame % 2u == 0u ? "ani" : "tri", r.covered, r.idDiff, r.materialDiff, r.binnedUberDiff, r.attrBad,
                        r.classifyBad, r.binTiles[0], r.binTiles[1], r.binTiles[2], r.binTiles[3], r.lodPixels, r.lodMaxDiff,
                        r.lodPixels > 0u ? r.lodSumDiff / r.lodPixels : 0.0, r.lodMin, r.lodMax);
            std::printf("        max err:");
            for (u32 c = 0; c < kChannels; ++c) {
                std::printf(" %s %.3g%s", kChannelNames[c], r.maxErr[c], r.bad[c] != 0u ? "(!)" : "");
                total.bad[c] += r.bad[c];
                total.maxErr[c] = std::max(total.maxErr[c], r.maxErr[c]);
            }
            std::printf("\n        attributes vs CPU: b/depth %.3g, db %.3g, uv %.3g, n/t %.3g, velocity %.3g px\n",
                        r.attrMaxErr[0], r.attrMaxErr[1], r.attrMaxErr[2], r.attrMaxErr[3], r.attrMaxErr[4]);
            expect(r.covered > kPixels / 3u, "the scene covers the view");
            if (atomic) {
                expect(r.idDiff <= kPixels / 200u, "atomic: forward ids == visibility ids on >= 99.5% of the pixels");
            } else {
                expect(r.idDiff == 0u, "forward (instance, triangle) == visibility buffer on every pixel");
            }
            expect(r.materialDiff == 0u, "material ids equal wherever the ids are");
            expect(r.binnedUberDiff == 0u, "binned == uber, bit for bit");
            for (u32 c = 0; c < kChannels; ++c) {
                if (r.bad[c] != 0u) {
                    std::fprintf(stderr, "  channel %s: %u pixels over tolerance\n", kChannelNames[c], r.bad[c]);
                }
            }
            u32 anyBad = 0;
            for (u32 c = 0; c < kChannels; ++c) {
                anyBad += r.bad[c];
            }
            expect(anyBad == 0u, "resolve G-buffer == forward G-buffer within the stated tolerances");
            expect(r.lodPixels > 500u && r.lodBad == 0u, "LOD within one mip of the forward reference on every probe pixel");
            expect(r.lodMax - r.lodMin >= 3.0, "the LOD probe spans at least 3 mips");
            expect(r.attrBad == 0u, "GPU attributes == CPU reference kernel");
            expect(r.classifyBad == 0u, "GPU tile lists == CPU classification");
            expect(r.binTiles[0] > 0u && r.binTiles[1] > 0u && r.binTiles[2] > 0u && r.binTiles[3] > 0u,
                   "every bin receives tiles");
        }
        last.push_back(std::move(r));
        languages.push_back(rig.language);
        s.gpu.destroy();
        destroyRig(rig);
    }
    if (last.empty()) {
        std::printf("SKIP: no material-resolve kernels built\n");
        return kSkip;
    }
    if (last.size() == 2u) {
        const std::vector<u8>& a = last[0].binnedCopy;
        const std::vector<u8>& b = last[1].binnedCopy;
        u64 offset = 0;
        std::printf("  %s vs %s (last frame, binned):", languages[0], languages[1]);
        for (u32 t = 0; t < kTargets; ++t) {
            u32 diff = 0;
            for (u32 p = 0; p < kPixels; ++p) {
                diff += std::memcmp(a.data() + offset + p * kTexelBytes[t], b.data() + offset + p * kTexelBytes[t],
                                    kTexelBytes[t]) != 0
                            ? 1u
                            : 0u;
            }
            std::printf(" rt%u %u px", t, diff);
            if (t == kTargets - 1u) {
                expect(diff == 0u, "Slang == GLSL material ids");
            }
            offset += static_cast<u64>(kPixels) * kTexelBytes[t];
        }
        std::printf(" differ\n");
    }
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "resolve.", 8) == 0 || std::strncmp(name, "vis.", 4) == 0 ||
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
    Rig rig;
    if (initRig(ctx, rig, VisMode::Raster, ResolveKernelLanguage::Auto, false) != 1) {
        destroyRig(rig);
        std::printf("SKIP: no material-resolve kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long resolveSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    Mat4 prev = frameCamera(0);
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s);
        moveObjects(s, frame, 10u);
        s.gpu.commit();
        ctx.upload.flush();
        FrameOptions opt{};
        opt.viewProj = frameCamera(frame % 5u);
        opt.prevViewProj = prev;
        opt.sampler = ctx.samplerHandles[0];
        opt.withReference = false;
        opt.readback = false;
        prev = opt.viewProj;
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
        rig.binned.collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            resolveSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.binned.stats().resolvePasses == 1u, "one resolve pass per frame");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, ~2%% moved, binned resolve)\n"
                    "  MaterialResolve / VisBuffer / culler beginFrame + resolve.* / vis.* / cull.* pass callbacks: "
                    "%llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis and resolve imports + passes): %llu\n",
                    kTotal - kWarmup, s.gpu.instanceHighWater(), resolveSide, callbacks, build);
        expect(resolveSide == 0u, "resolve side makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the resolve passes makes no steady-state heap allocations");
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
            rc = runParity(ctx, VisMode::Raster);
        } else if (mode == "parity_atomic") {
            const VisCapabilities caps = queryVisCapabilities(ctx.device.get());
            if (!caps.atomicBuffer && !caps.atomicImage) {
                std::printf("SKIP: no 64-bit atomics (%s)\n", caps.atomicReason);
                return kSkip;
            }
            rc = runParity(ctx, VisMode::Atomic64);
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
