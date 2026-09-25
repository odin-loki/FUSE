// Asset W0.7 x WP-1.5: Lavapipe gates of the material resolve's layered bin (VK_LAYER_KHRONOS_validation with
// synchronization validation; every validation message fails the run). CPU gates: test_rp_material_resolve_layered_cpu.cpp.
//
// Every frame is one render graph: GpuScene (WP-1.1) delta upload, VisBuffer::addCulledFrame (WP-1.3 culling +
// WP-1.4 visibility draws), then the WP-1.5 resolve: binned ("resolve.reset" + "resolve.classify" + "resolve.gbuffer",
// 4 feature bins + the layered bin) and uber, with the MaterialLayers resolve table (the 8 W0.7 ball fixtures,
// every texture a mip-mapped bindless image) declared as read. Scene: the WP-1.5 meshes (sphere, torus, 2-submesh
// box, ground plane) x 241 instances; the ground and every other object use layered rows (plain, stochastic,
// triplanar, triplanar + stochastic, moss, snow, wet + detail, everything), the rest the WP-1.5 rows (flat,
// textured, normal-mapped, LOD probe, emissive, AO / emissive / metallic, cloth) and "no material".
//
//   --mode parity      per kernel language (Slang, GLSL), 3 frames of camera + object motion with a trilinear
//                      sampler and 1 with an 8x anisotropic one:
//     identical        binned == uber, bit for bit, on all 7 targets (negative control: one flipped byte)
//     classify         the GPU tile lists of the 4 feature bins and of the layered bin == resolve_reference's
//     reference        every layered pixel (trilinear frames) against the CPU reference: layered_surface on the
//                      read-back visibility image + material_layers::ml_evaluate with the trilinear mip-chain filter
//                      (ml_mips.hpp) over the chain MaterialLayers uploaded, within the documented tolerance
//                      (kTol* below + the LOD bound: the largest change of the CPU result when the LOD moves by
//                      +-kLodSlack mip); material id, shading model, emissive mask exact; emissive radiance of the row
//     plain            a twin scene whose layered rows lost the flag (flat rows), rendered in lockstep: every
//                      non-layered pixel is bit-identical on all 7 targets (whether its tile went to the layered
//                      bin or not), i.e. the layered bin changes nothing for other materials
//     no table         ResolveFrameDesc::layered = 0: layered pixels get ml_evaluate's default surface
//     languages        Slang == GLSL: ids / bins exact, layered pixels within the reference tolerance (reported)
//   --mode golden      the layered scene's G-buffer (albedo x (0.3 + 0.7 N.L)) vs the committed
//                      tests/material_layers/resolve_layered_gbuffer.png (<= 0.5% pixels over 2 LSB, mean <= 0.1 LSB),
//                      and vs the same image from the CPU reference (reported + mean bound); --update-golden rewrites it
//   --mode zero_alloc  64 steady-state frames with layered tiles: 0 operator-new calls in the beginFrames, the graph
//                      build (scene, cull, vis, layers, resolve imports + passes) and the resolve.* / vis.* / cull.*
//                      pass callbacks (validated run first; validation off for the count)
//   --mode plain_hash  prints FNV-1a hashes of the binned and uber G-buffers of the WP-1.5 test scene (no layered
//                      rows, 5 frames, both languages): run against a build without the layered bin and with it to
//                      show the non-layered output is bit-identical (manual check, not a ctest)
//   --backend set | buffer  bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_layers_common.hpp"
#include "test_rp_material_resolve_layered_scene.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/geometry/vertex_codec_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_layers/material_layers.hpp>
#include <fuse/renderer/material_layers/ml_mips.hpp>
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

#ifndef FUSE_RP_ML_FIXTURE_DIR
#define FUSE_RP_ML_FIXTURE_DIR "."
#endif
#ifndef FUSE_RP_MRL_OUTPUT_DIR
#define FUSE_RP_MRL_OUTPUT_DIR "."
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
using namespace fuse::renderer::material_layers;
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
constexpr u32 kTiles = ((kWidth + 7u) / 8u) * ((kHeight + 7u) / 8u);
constexpr u32 kObjects = 240;
constexpr u32 kTargets = kResolveColorAttachments; // RT0..RT5 + material id
constexpr u32 kTexelBytes[kTargets] = {8u, 4u, 4u, 4u, 4u, 8u, 4u};
constexpr const char* kGoldenPng = "resolve_layered_gbuffer.png";

// Tolerances of the layered bin (GPU: textureGrad on the mip-mapped bindless images) against the CPU reference
// (ml_evaluate with ml_trilinear over the same chain). Justification in the Asset W0.7 row of RENDERER-EXECUTION.md;
// a channel passes when |GPU - CPU(LOD)| <= kTol + max(|CPU(LOD +- kLodSlack) - CPU(LOD)|):
//   kLodSlack    Lavapipe's LOD is an approximation of log2(rho) (WP-1.5 measured |dLOD| <= 0.19 mip on the
//                scene-texture path): the CPU result is also evaluated a quarter mip either side and the larger
//                change of each channel is added (on Lavapipe every channel already meets kTol alone, see the
//                "max excess over the plain tolerance" column the gate prints)
//   kTolUnorm8   RGBA8 G-buffer channels (albedo, roughness, metallic): 0.5 LSB output rounding + Lavapipe's
//                fixed-point filter weights (bilinear and mip blend); measured max 0.87 LSB albedo, 2.0 LSB roughness
//   kTolHalf     AO (RGBA16F): the same filter-weight term on the AO texels; measured max 1.6 LSB
//   kTolNormal   RGBA16F oct normal: two quanta (encode_normal_rgba16f < 1e-3 rad each) + the filtered normal-map
//                texels through the tangent / whiteout blends and renormalisations; measured max 0.014 rad
constexpr f32 kLodSlack = 0.25f;
constexpr f64 kTolUnorm8 = 2.5 / 255.0;
constexpr f64 kTolHalf = 2.5 / 255.0;
constexpr f64 kTolNormalRad = 0.02;

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

/// The WP-1.5 gate's camera path.
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

u64 fnv(const u8* p, usize n, u64 h = 1469598103934665603ull) {
    for (usize i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
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
    Buffer texStaging{};
    std::vector<Texture> textures;
    std::vector<BindlessSlotHandle> textureSlots;
    u32 textureHandles[mr_test::kTexCount] = {};
    BindlessSlotHandle samplers[2]{};
    u32 samplerHandles[2] = {}; ///< 0: 8x anisotropic, 1: trilinear (both REPEAT)
    MlLibrary library;
    MlMipChains chains;
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

/// Byte layout of the readback buffer (every section 256-aligned): per scene (0 = layered, 1 = plain twin).
struct ReadbackLayout {
    u64 vis[2] = {};
    u64 sets[2][2][kTargets] = {}; ///< [scene][binned, uber][target]
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
    for (u32 s = 0; s < 2u; ++s) {
        l.vis[s] = take(kPixels * 8ull);
        for (auto& set : l.sets[s]) {
            for (u32 t = 0; t < kTargets; ++t) {
                set[t] = take(static_cast<u64>(kPixels) * kTexelBytes[t]);
            }
        }
    }
    l.binsBytes = ResolveBinLayout::totalBytes(kTiles);
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

/// The WP-1.5 test textures (the non-layered rows' bindless textures) + the two samplers.
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
    sd.name = "rp_mr_layered.tex_staging";
    if (!ctx.allocator->createBuffer(sd, ctx.texStaging) || ctx.texStaging.mapped == nullptr) {
        return false;
    }
    rg::Graph graph;
    std::vector<TexUpload> uploads(data.size());
    const rg::BufferRef staging =
        graph.importBuffer(rg::ImportedBuffer{ctx.texStaging.handle, bytes, rg::kNoQueue, nullptr, "rp_mr_layered.tex_staging"});
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
        td.name = "rp_mr_layered.texture";
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
        im.name = "rp_mr_layered.texture";
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
    SamplerDesc aniso{};
    aniso.minFilter = VK_FILTER_LINEAR;
    aniso.magFilter = VK_FILTER_LINEAR;
    aniso.addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    aniso.anisotropy = true;
    aniso.maxAnisotropy = 8.f;
    aniso.name = "rp_mr_layered.aniso";
    SamplerDesc trilinear = aniso;
    trilinear.anisotropy = false;
    trilinear.maxAnisotropy = 1.f;
    trilinear.name = "rp_mr_layered.trilinear";
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
    instanceDesc.appName = "fuse_rp_material_resolve_layered";
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
    if (!caps.resolve || !queryMaterialLayerCapabilities(ctx.device.get()).layers) {
        std::printf("SKIP: material resolve / layers unsupported: %s\n", caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    std::printf("device: %s, bindless backend: %s\n", ctx.device->info().deviceName.c_str(),
                bindlessBackendName(ctx.bindless.backend()));
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_mr_layered.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = g_layout.end;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_mr_layered.readback";
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
    if (!uploadTextures(ctx)) {
        std::fprintf(stderr, "FAIL: textures\n");
        return 1;
    }
    if (!ml_test::buildLibrary(FUSE_RP_ML_FIXTURE_DIR, ctx.library)) {
        std::fprintf(stderr, "FAIL: layered-material library (fixtures in %s)\n", FUSE_RP_ML_FIXTURE_DIR);
        return 1;
    }
    ml_build_mips(ctx.library, ctx.chains);
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
    std::vector<Material::GPUMaterial> rows;
};

/// `layeredRows`: the scene's rows keep kGpuMaterialLayered (else: the plain twin). `withLayered`: false = the
/// WP-1.5 scene exactly (plain_hash).
bool buildScene(Context& ctx, Scene& s, bool layeredRows, bool withLayered = true) {
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
    s.rows = withLayered ? mrl_test::makeMaterials(ctx.textureHandles) : mr_test::makeMaterials(ctx.textureHandles);
    if (!layeredRows) {
        s.rows = mrl_test::withoutLayered(s.rows);
    }
    for (u32 i = 0; i < s.rows.size(); ++i) {
        s.gpu.setMaterial(i, s.rows[i]);
    }
    std::mt19937 rng(4321);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    InstanceDesc ground{};
    ground.mesh = 3;
    ground.material = withLayered ? mrl_test::kLayeredBase + 3u : mr_test::kMatLodProbe; // triplanar + stochastic
    ground.transform = place(0.f, -1.8f, -20.f, 1.f, 1.f, 1.f, 0.35f);
    s.handles.push_back(s.gpu.addInstance(ground));
    const u32 sphereTorusMaterials[6] = {mr_test::kMatFlat,     mr_test::kMatTextured, mr_test::kMatNormalMapped,
                                         mr_test::kMatEmissive, mr_test::kMatLodProbe, kInvalidIndex};
    for (u32 i = 0; i < kObjects; ++i) {
        InstanceDesc id{};
        id.mesh = i % 3u;
        id.material = id.mesh == 2u ? mr_test::kMatAoEmissiveTex : sphereTorusMaterials[(i / 3u) % 6u];
        if (withLayered && (i / 3u) % 2u == 0u) {
            // Layered rows on every other object; the 2-submesh box takes rows base + k and base + k + 1.
            const u32 k = (i / 6u) % (id.mesh == 2u ? mrl_test::kLayeredRows - 1u : mrl_test::kLayeredRows);
            id.material = mrl_test::kLayeredBase + k;
        }
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

// --- rig ------------------------------------------------------------------------------------------
struct Rig {
    const char* language = nullptr;
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve binned;
    MaterialResolve uber;
    MaterialLayers layers;
};

/// 1 = ok, 0 = language not built, -1 = failure.
int initRig(Context& ctx, Rig& rig, ResolveKernelLanguage language, bool withUber, bool withLayers) {
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
    const MaterialResolveDesc md{ctx.device.get(), ctx.allocator.get(), &ctx.bindless, kWidth, kHeight, language, false};
    if (!rig.binned.init(md)) {
        return 0;
    }
    if (withUber && !rig.uber.init(md)) {
        return -1;
    }
    if (withLayers) {
        MaterialLayersDesc ld{};
        ld.device = ctx.device.get();
        ld.allocator = ctx.allocator.get();
        ld.bindless = &ctx.bindless;
        if (!rig.layers.init(ld) || !rig.layers.setLibrary(ctx.library, &ctx.upload)) {
            return -1;
        }
        ctx.upload.flush();
        if (!ctx.upload.waitAll() || rig.layers.resolveTableHandle() == 0u) {
            return -1;
        }
    }
    rig.language = rig.binned.kernelLanguage();
    return 1;
}

void destroyRig(Rig& rig) {
    rig.binned.destroy();
    rig.uber.destroy();
    rig.layers.destroy();
    rig.vb.destroy();
    rig.culler.destroy();
}

// --- per-frame graph ------------------------------------------------------------------------------
struct CopyRecord {
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
    if (c.buffer.valid()) {
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
    CopyRecord copies[24];
    u32 copyCount = 0;
};

struct FrameOptions {
    Mat4 viewProj{};
    Mat4 prevViewProj{};
    u32 sampler = 0;
    bool layeredTable = true; ///< ResolveFrameDesc::layered = the table (false: 0 and no table ref)
    bool uber = true;
    bool readback = true;
    u32 slot = 0; ///< readback section (0 = layered scene, 1 = plain twin)
};

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

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
    rf.layered = opt.layeredTable ? rig.layers.resolveTableHandle() : 0u;
    ok = rig.binned.beginFrame(ctx.serial, rf) && ok;
    if (opt.uber) {
        ok = rig.uber.beginFrame(ctx.serial, rf) && ok;
    }
    return ok;
}

void buildGraph(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = s.gpu.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const VisGraphRefs vis = rig.vb.importInto(graph);
    rig.vb.addCulledFrame(graph, vis, sceneRefs, s.gpu.headerHandle(), rig.culler, cull);
    const rg::BufferRef table = opt.layeredTable ? rig.layers.importInto(graph).resolveTable : rg::BufferRef{};
    const ResolveGraphRefs binned = rig.binned.importInto(graph);
    rig.binned.addResolve(graph, binned, vis.vis, sceneRefs, ResolvePath::Binned, table);
    ResolveGraphRefs uber{};
    if (opt.uber) {
        uber = rig.uber.importInto(graph);
        rig.uber.addResolve(graph, uber, vis.vis, sceneRefs, ResolvePath::Uber, table);
    }
    if (!opt.readback) {
        return;
    }
    const rg::BufferRef readback =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, g_layout.end, rg::kNoQueue, nullptr, "rp_mr_layered.readback"});
    auto addCopy = [&](rg::TextureRef image, rg::BufferRef buffer, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.image = image;
        c.buffer = buffer;
        c.dst = readback;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        rg::PassBuilder pass = graph.addPass("readback.copy", &recordCopy, &c);
        if (buffer.valid()) {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        } else {
            pass.use(image, rg::Access::TransferSrc);
        }
        pass.use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    addCopy(vis.vis, {}, g_layout.vis[opt.slot], kPixels * 8ull);
    const ResolveGraphRefs* sets[2] = {&binned, &uber};
    for (u32 k = 0; k < (opt.uber ? 2u : 1u); ++k) {
        for (u32 t = 0; t < kTargets; ++t) {
            addCopy(t + 1u < kTargets ? sets[k]->gbuffer[t] : sets[k]->materialId, {}, g_layout.sets[opt.slot][k][t],
                    static_cast<u64>(kPixels) * kTexelBytes[t]);
        }
    }
    if (opt.slot == 0u) {
        addCopy({}, binned.bins, g_layout.bins, g_layout.binsBytes);
    }
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
    rig.vb.collectRetired(ctx.serial);
    rig.binned.collectRetired(ctx.serial);
    rig.uber.collectRetired(ctx.serial);
    rig.layers.collectRetired(ctx.serial);
    s.gpu.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

// --- analysis -------------------------------------------------------------------------------------
const u8* rb(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

/// One G-buffer pixel, decoded.
struct Px {
    f32 n[3] = {0.f, 0.f, 0.f};
    f32 ao = 0.f;
    f32 albedo[3] = {0.f, 0.f, 0.f};
    f32 rough = 0.f, metal = 0.f, mask = 0.f;
    u32 shading = 0;
    f32 emissive[3] = {0.f, 0.f, 0.f};
    u32 material = 0;
};

Px readPx(Context& ctx, u32 slot, u32 set, u32 p) {
    Px r;
    const u8* rt0 = rb(ctx, g_layout.sets[slot][set][0]) + p * 8u;
    const fuse::math::Vec3 n = GBufferEncoding::decodeOctSigned({halfAt(rt0, 0), halfAt(rt0, 1)});
    r.n[0] = n.x;
    r.n[1] = n.y;
    r.n[2] = n.z;
    r.ao = halfAt(rt0, 3);
    const u8* rt1 = rb(ctx, g_layout.sets[slot][set][1]) + p * 4u;
    for (u32 k = 0; k < 3u; ++k) {
        r.albedo[k] = static_cast<f32>(rt1[k]) / 255.f;
    }
    const u8* rt2 = rb(ctx, g_layout.sets[slot][set][2]) + p * 4u;
    r.rough = rt2[0] / 255.f;
    r.metal = rt2[1] / 255.f;
    r.mask = rt2[2] / 255.f;
    r.shading = rt2[3];
    const u8* rt5 = rb(ctx, g_layout.sets[slot][set][5]) + p * 8u;
    for (u32 k = 0; k < 3u; ++k) {
        r.emissive[k] = halfAt(rt5, k);
    }
    std::memcpy(&r.material, rb(ctx, g_layout.sets[slot][set][6]) + p * 4u, 4u);
    return r;
}

f64 angleBetween(const f32 a[3], const f32 b[3]) {
    const f64 la = std::sqrt(static_cast<f64>(a[0]) * a[0] + static_cast<f64>(a[1]) * a[1] + static_cast<f64>(a[2]) * a[2]);
    const f64 lb = std::sqrt(static_cast<f64>(b[0]) * b[0] + static_cast<f64>(b[1]) * b[1] + static_cast<f64>(b[2]) * b[2]);
    if (la <= 0.0 || lb <= 0.0) {
        return 3.14159265358979;
    }
    const f64 c = (static_cast<f64>(a[0]) * b[0] + static_cast<f64>(a[1]) * b[1] + static_cast<f64>(a[2]) * b[2]) / (la * lb);
    return std::acos(std::clamp(c, -1.0, 1.0));
}

f64 clamp01(f64 v) { return std::clamp(v, 0.0, 1.0); }

/// Pixels whose texel differs in any of the 7 targets between two sets.
u32 diffSets(Context& ctx, u32 slotA, u32 setA, u32 slotB, u32 setB, u32* firstDiff = nullptr) {
    u32 n = 0;
    for (u32 p = 0; p < kPixels; ++p) {
        bool diff = false;
        for (u32 t = 0; t < kTargets && !diff; ++t) {
            diff = std::memcmp(rb(ctx, g_layout.sets[slotA][setA][t]) + p * kTexelBytes[t],
                               rb(ctx, g_layout.sets[slotB][setB][t]) + p * kTexelBytes[t], kTexelBytes[t]) != 0;
        }
        if (diff && n == 0u && firstDiff != nullptr) {
            *firstDiff = p;
        }
        n += diff ? 1u : 0u;
    }
    return n;
}

struct LayeredStats {
    u32 layeredPixels = 0;
    u32 bad[6] = {}; ///< albedo, rough, metal, ao, normal, exact (id / shading / emissive)
    f64 maxErr[6] = {};
    f64 maxRawOver[6] = {}; ///< largest error above the plain tolerance (the part the LOD bound covers)
    f64 maxBound[5] = {};
    f64 sumErr[5] = {};
    f64 sumSigned[3] = {}; ///< albedo rgb: GPU - CPU (bias)
    u32 controlBad = 0;    ///< negative control: pixels failing against the CPU reference two mips too coarse
};

const char* const kLayeredChannels[6] = {"albedo", "rough", "metal", "ao", "normal", "exact"};

/// Every layered pixel of `slot`'s binned G-buffer vs the CPU reference on the read-back visibility image.
void compareLayered(Context& ctx, Scene& s, const Mat4& vp, const Mat4& prevVp, u32 slot, u32 set, LayeredStats& st,
                    bool defaultSurface) {
    const u32* vis = reinterpret_cast<const u32*>(rb(ctx, g_layout.vis[slot]));
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<LayeredSurface> surfaces;
    layered_surfaces_reference(view, vp.m, prevVp.m, vis, kWidth, kHeight, surfaces);
    const MlTrilinearFilter f0{&ctx.chains, ctx.library.textures().data(), static_cast<u32>(ctx.library.textures().size()), 0.f};
    const MlTrilinearFilter fLo{&ctx.chains, ctx.library.textures().data(), static_cast<u32>(ctx.library.textures().size()),
                                -kLodSlack};
    const MlTrilinearFilter fHi{&ctx.chains, ctx.library.textures().data(), static_cast<u32>(ctx.library.textures().size()),
                                kLodSlack};
    const MlTrilinearFilter fCtl{&ctx.chains, ctx.library.textures().data(), static_cast<u32>(ctx.library.textures().size()),
                                 2.f};
    const MlView vCtl = ml_trilinear_view(ctx.library, fCtl);
    MlView v0 = ml_trilinear_view(ctx.library, f0);
    MlView vLo = ml_trilinear_view(ctx.library, fLo);
    MlView vHi = ml_trilinear_view(ctx.library, fHi);
    if (defaultSurface) {
        v0.materialCount = vLo.materialCount = vHi.materialCount = 0u; // no table: the default surface
    }
    auto note = [&](u32 c, f64 err, f64 tol, f64 bound) {
        st.maxErr[c] = std::max(st.maxErr[c], err);
        if (c < 5u) {
            st.maxBound[c] = std::max(st.maxBound[c], bound);
            st.sumErr[c] += err;
            st.maxRawOver[c] = std::max(st.maxRawOver[c], err - tol);
        }
        if (!(err <= tol + bound)) {
            ++st.bad[c];
        }
    };
    for (u32 p = 0; p < kPixels; ++p) {
        const LayeredSurface& ls = surfaces[p];
        if (!ls.valid) {
            continue;
        }
        ++st.layeredPixels;
        const MlResult r0 = ml_evaluate(v0, ls.surface, ls.grad);
        const MlResult rl = ml_evaluate(vLo, ls.surface, ls.grad);
        const MlResult rh = ml_evaluate(vHi, ls.surface, ls.grad);
        const Px g = readPx(ctx, slot, set, p);
        if (!defaultSurface) {
            // Negative control: the plain tolerance against a reference whose LOD is two mips off must fail.
            const MlResult rc = ml_evaluate(vCtl, ls.surface, ls.grad);
            bool fails = angleBetween(g.n, rc.normal) > kTolNormalRad;
            for (u32 k = 0; k < 3u; ++k) {
                fails = fails || std::fabs(g.albedo[k] - clamp01(rc.albedo[k])) > kTolUnorm8;
            }
            st.controlBad += fails ? 1u : 0u;
        }
        for (u32 k = 0; k < 3u; ++k) {
            const f64 c = clamp01(r0.albedo[k]);
            const f64 bound = std::max(std::fabs(clamp01(rl.albedo[k]) - c), std::fabs(clamp01(rh.albedo[k]) - c));
            note(0u, std::fabs(g.albedo[k] - c), kTolUnorm8, bound);
            st.sumSigned[k] += g.albedo[k] - c;
        }
        const f64 rb0 = std::max(std::fabs(rl.roughness - r0.roughness), std::fabs(rh.roughness - r0.roughness));
        note(1u, std::fabs(g.rough - r0.roughness), kTolUnorm8, rb0);
        const f64 mb0 = std::max(std::fabs(rl.metallic - r0.metallic), std::fabs(rh.metallic - r0.metallic));
        note(2u, std::fabs(g.metal - r0.metallic), kTolUnorm8, mb0);
        const f64 ab0 = std::max(std::fabs(rl.ao - r0.ao), std::fabs(rh.ao - r0.ao));
        note(3u, std::fabs(g.ao - r0.ao), kTolHalf, ab0);
        const f64 nb0 = std::max(angleBetween(rl.normal, r0.normal), angleBetween(rh.normal, r0.normal));
        note(4u, angleBetween(g.n, r0.normal), kTolNormalRad, nb0);
        const Material::GPUMaterial& row = s.rows[ls.row];
        const fuse::math::Vec3 e = MaterialEval::emissiveRadiance(row);
        const f32 ev[3] = {e.x, e.y, e.z};
        bool exact = g.material == ls.row && g.shading == row.shadingModel &&
                     (g.mask > 0.5f) == (std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z) > 0.001f);
        for (u32 k = 0; k < 3u; ++k) {
            exact = exact && std::fabs(g.emissive[k] - ev[k]) <= 2e-3 * std::max(1.f, ev[k]);
        }
        note(5u, exact ? 0.0 : 1.0, 0.0, 0.0);
    }
}

void printLayered(const char* label, const LayeredStats& st) {
    const f64 n = st.layeredPixels > 0u ? static_cast<f64>(st.layeredPixels) : 1.0;
    std::printf("    %s: %u layered pixels, albedo bias (GPU - CPU) %.2f / %.2f / %.2f LSB; negative control (reference "
                "2 mips too coarse) fails on %u px\n",
                label, st.layeredPixels, st.sumSigned[0] / n * 255.0, st.sumSigned[1] / n * 255.0, st.sumSigned[2] / n * 255.0,
                st.controlBad);
    for (u32 c = 0; c < 6u; ++c) {
        if (c < 5u) {
            std::printf("      %-7s max %.4f (mean %.5f), max LOD bound %.4f, max excess over the plain tolerance %.4f, "
                        "bad %u\n",
                        kLayeredChannels[c], st.maxErr[c], st.sumErr[c] / (c == 0u ? 3.0 * n : n),
                        st.maxBound[c], std::max(0.0, st.maxRawOver[c]), st.bad[c]);
        } else {
            std::printf("      %-7s (material id, shading model, emissive mask + radiance) bad %u\n", kLayeredChannels[c],
                        st.bad[c]);
        }
    }
}

/// GPU tile lists (bin buffer read-back) vs the CPU classification of the read-back visibility image.
u32 compareClassify(Context& ctx, Scene& s, u32 tiles[kBinDrawCount]) {
    const u32* vis = reinterpret_cast<const u32*>(rb(ctx, g_layout.vis[0]));
    const u32* words = reinterpret_cast<const u32*>(rb(ctx, g_layout.bins));
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<u32> tileBins;
    classify_reference(view, vis, kWidth, kHeight, tileBins);
    std::vector<u32> lists[kBinCount];
    tile_lists(tileBins, (kWidth + 7u) / 8u, lists);
    std::vector<u32> layered;
    layered_tile_list(tileBins, (kWidth + 7u) / 8u, layered);
    u32 bad = 0;
    static constexpr u32 kDrawBins[kBinDrawCount] = {kBinEmpty, kBinFlat, kBinTextured, kBinNormalMapped, kBinLayered};
    for (u32 i = 0; i < kBinDrawCount; ++i) {
        const u32 b = kDrawBins[i];
        const u32 argWord = static_cast<u32>(ResolveBinLayout::argsOffset(b, kTiles) / 4u);
        const u32 listWord = static_cast<u32>(ResolveBinLayout::listOffset(b, kTiles) / 4u);
        const u32 count = words[argWord + 1u];
        tiles[i] = count;
        const std::vector<u32>& cpu = b == kBinLayered ? layered : lists[b];
        std::vector<u32> gpu(words + listWord, words + listWord + std::min(count, kTiles));
        std::sort(gpu.begin(), gpu.end());
        bad += words[argWord] == ResolveBinLayout::kVerticesPerTile && gpu == cpu ? 0u : 1u;
    }
    return bad;
}

// --- parity -----------------------------------------------------------------------------------------
int runParity(Context& ctx) {
    constexpr u32 kFrames = 4; // 3 trilinear + 1 anisotropic, then the no-table frame
    std::vector<u8> firstLanguage;
    u32 languagesRun = 0;
    std::vector<u8> firstVis;
    for (const ResolveKernelLanguage language : {ResolveKernelLanguage::Slang, ResolveKernelLanguage::Glsl}) {
        const char* want = language == ResolveKernelLanguage::Slang ? "slang" : "glsl";
        Rig rig;
        Rig plainRig;
        const int rc = initRig(ctx, rig, language, true, true);
        const int rcPlain = rc == 1 ? initRig(ctx, plainRig, language, true, true) : rc;
        if (rc < 0 || rcPlain < 0) {
            std::fprintf(stderr, "FAIL: rig init (%s)\n", want);
            return 1;
        }
        if (rc == 0) {
            std::printf("  language %s: not built, skipped\n", want);
            destroyRig(rig);
            destroyRig(plainRig);
            continue;
        }
        Scene s;
        Scene plain;
        if (!buildScene(ctx, s, true) || !buildScene(ctx, plain, false)) {
            std::fprintf(stderr, "FAIL: scene\n");
            return 1;
        }
        ++languagesRun;
        std::printf("  kernels: %s, %u instances, %zu rows (%u layered), resolve table: %u images, %.1f KiB\n", rig.language,
                    s.gpu.instanceHighWater(), s.rows.size(), mrl_test::kLayeredRows, rig.layers.stats().resolveImages,
                    static_cast<f64>(rig.layers.stats().resolveImageBytes) / 1024.0);
        rg::Graph graph;
        FrameState fs;
        Mat4 prev = frameCamera(0);
        for (u32 frame = 0; frame <= kFrames; ++frame) {
            const bool noTable = frame == kFrames;
            const bool aniso = frame == kFrames - 1u;
            beginSceneFrame(ctx, s);
            beginSceneFrame(ctx, plain);
            if (frame > 0u && !noTable) {
                moveObjects(s, frame, 1u);
                moveObjects(plain, frame, 1u);
            }
            FrameOptions opt{};
            opt.viewProj = frameCamera(noTable ? 0u : frame);
            opt.prevViewProj = prev;
            opt.sampler = ctx.samplerHandles[aniso ? 0u : 1u];
            opt.layeredTable = !noTable;
            if (!runFrame(ctx, s, rig, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: frame %u\n", frame);
                return 1;
            }
            FrameOptions popt = opt;
            popt.slot = 1u;
            popt.layeredTable = true;
            if (!noTable && !runFrame(ctx, plain, plainRig, graph, popt, fs)) {
                std::fprintf(stderr, "FAIL: plain frame %u\n", frame);
                return 1;
            }
            // (1) binned == uber, bit for bit (+ negative control).
            const u32 binnedUber = diffSets(ctx, 0u, 0u, 0u, 1u);
            {
                u8* flip = const_cast<u8*>(rb(ctx, g_layout.sets[0][1][1])) + 4u * (kPixels / 2u) + 1u;
                *flip ^= 0x01u;
                const u32 flipped = diffSets(ctx, 0u, 0u, 0u, 1u);
                *flip ^= 0x01u;
                expect(binnedUber != 0u || flipped == 1u, "negative control: one flipped albedo byte breaks binned == uber");
            }
            // (2) classification.
            u32 tiles[kBinDrawCount] = {};
            const u32 classifyBad = compareClassify(ctx, s, tiles);
            std::printf("  frame %u (%s%s): binned vs uber %u px, tiles e/f/t/n/layered %u/%u/%u/%u/%u, classify mismatches %u\n",
                        frame, noTable ? "no table, " : "", aniso ? "8x aniso" : "trilinear", binnedUber, tiles[0], tiles[1],
                        tiles[2], tiles[3], tiles[4], classifyBad);
            expect(binnedUber == 0u, "binned == uber, bit for bit, on all 7 targets");
            expect(classifyBad == 0u, "GPU tile lists (4 feature bins + layered) == the CPU classification");
            expect(tiles[4] > 50u && tiles[1] + tiles[2] + tiles[3] > 20u, "layered tiles and other tiles every frame");
            // (3) layered pixels vs the CPU reference (trilinear frames), or the default surface (no table).
            if (!aniso) {
                LayeredStats st;
                compareLayered(ctx, s, opt.viewProj, opt.prevViewProj, 0u, 0u, st, noTable);
                printLayered(noTable ? "no table (default surface)" : "layered vs CPU reference", st);
                expect(st.layeredPixels > 3000u, "enough layered pixels");
                expect(noTable || st.controlBad > st.layeredPixels / 20u,
                       "negative control: a reference two mips off fails the plain tolerance on > 5% of the layered pixels");
                for (u32 c = 0; c < 6u; ++c) {
                    expect(st.bad[c] == 0u, noTable ? "no table: layered pixels get the default surface"
                                                    : "layered pixels within the documented tolerance of the CPU reference");
                }
            }
            // (4) the plain twin: every non-layered pixel bit-identical.
            if (!noTable) {
                const u32* visA = reinterpret_cast<const u32*>(rb(ctx, g_layout.vis[0]));
                const u32* visB = reinterpret_cast<const u32*>(rb(ctx, g_layout.vis[1]));
                const u32* words = reinterpret_cast<const u32*>(rb(ctx, g_layout.bins));
                std::vector<u8> layeredTile(kTiles, 0u);
                const u32 listWord = static_cast<u32>(ResolveBinLayout::layeredListOffset(kTiles) / 4u);
                for (u32 i = 0; i < std::min(tiles[4], kTiles); ++i) {
                    const u32 t = words[listWord + i];
                    layeredTile[tile_y(t) * ((kWidth + 7u) / 8u) + tile_x(t)] = 1u;
                }
                u32 visDiff = 0;
                u32 compared = 0;
                u32 inLayeredTiles = 0;
                u32 differ = 0;
                for (u32 p = 0; p < kPixels; ++p) {
                    visDiff += std::memcmp(visA + p * 2u, visB + p * 2u, 8u) != 0 ? 1u : 0u;
                    u32 row = 0;
                    std::memcpy(&row, rb(ctx, g_layout.sets[0][0][6]) + p * 4u, 4u);
                    if (row != kNoMaterial && row < s.rows.size() && gpu_material_layered(s.rows[row])) {
                        continue;
                    }
                    ++compared;
                    const u32 tile = (p / kWidth / 8u) * ((kWidth + 7u) / 8u) + (p % kWidth) / 8u;
                    inLayeredTiles += layeredTile[tile];
                    for (u32 t = 0; t < kTargets; ++t) {
                        if (std::memcmp(rb(ctx, g_layout.sets[0][0][t]) + p * kTexelBytes[t],
                                        rb(ctx, g_layout.sets[1][0][t]) + p * kTexelBytes[t], kTexelBytes[t]) != 0) {
                            ++differ;
                            break;
                        }
                    }
                }
                std::printf("    plain twin: %u non-layered pixels compared (%u of them in layered tiles), %u differ; "
                            "visibility differs on %u px\n",
                            compared, inLayeredTiles, differ, visDiff);
                expect(visDiff == 0u, "plain twin: the same visibility buffer");
                expect(differ == 0u && inLayeredTiles > 100u,
                       "non-layered pixels are bit-identical with and without layered rows (also in layered tiles)");
            }
            // (5) languages: frame 0.
            if (frame == 0u) {
                std::vector<u8> copy;
                for (u32 t = 0; t < kTargets; ++t) {
                    const u8* at = rb(ctx, g_layout.sets[0][0][t]);
                    copy.insert(copy.end(), at, at + static_cast<usize>(kPixels) * kTexelBytes[t]);
                }
                const u8* v = rb(ctx, g_layout.vis[0]);
                if (firstLanguage.empty()) {
                    firstLanguage = std::move(copy);
                    firstVis.assign(v, v + kPixels * 8u);
                } else {
                    u32 differ = 0;
                    u32 idsDiffer = 0;
                    u64 offset = 0;
                    for (u32 t = 0; t < kTargets; ++t) {
                        for (u32 p = 0; p < kPixels; ++p) {
                            const bool d = std::memcmp(firstLanguage.data() + offset + static_cast<u64>(p) * kTexelBytes[t],
                                                       copy.data() + offset + static_cast<u64>(p) * kTexelBytes[t],
                                                       kTexelBytes[t]) != 0;
                            differ += d ? 1u : 0u;
                            idsDiffer += d && t == kTargets - 1u ? 1u : 0u;
                        }
                        offset += static_cast<u64>(kPixels) * kTexelBytes[t];
                    }
                    std::printf("    slang vs glsl (frame 0): %u differing texels (all targets), material ids differ on %u px, "
                                "visibility %s\n",
                                differ, idsDiffer, std::memcmp(firstVis.data(), v, kPixels * 8u) == 0 ? "equal" : "DIFFERS");
                    expect(idsDiffer == 0u && std::memcmp(firstVis.data(), v, kPixels * 8u) == 0,
                           "Slang and GLSL: same visibility and material ids");
                }
            }
            prev = opt.viewProj;
        }
        s.gpu.destroy();
        plain.gpu.destroy();
        destroyRig(rig);
        destroyRig(plainRig);
    }
    if (languagesRun == 0u) {
        std::printf("SKIP: no material-resolve kernels built\n");
        return kSkip;
    }
    return 0;
}

// --- golden -----------------------------------------------------------------------------------------
/// albedo x (0.3 + 0.7 max(N.L, 0)) as sRGB8, background black: a view of the G-buffer that shows every layered
/// feature (albedo layers, normal detail) in one image.
void gbufferImage(const std::vector<f32>& albedo, const std::vector<f32>& normal, const std::vector<u8>& covered,
                  std::vector<u8>& rgba) {
    const f64 L[3] = {0.4 / 1.0770, 0.8 / 1.0770, 0.45 / 1.0770};
    std::vector<MlF4> img(kPixels);
    for (u32 p = 0; p < kPixels; ++p) {
        if (covered[p] == 0u) {
            img[p] = MlF4{0.f, 0.f, 0.f, 1.f};
            continue;
        }
        const f64 ndl = std::max(0.0, normal[p * 3u] * L[0] + normal[p * 3u + 1u] * L[1] + normal[p * 3u + 2u] * L[2]);
        const f64 k = 0.3 + 0.7 * ndl;
        img[p] = MlF4{static_cast<f32>(albedo[p * 3u] * k), static_cast<f32>(albedo[p * 3u + 1u] * k),
                      static_cast<f32>(albedo[p * 3u + 2u] * k), 1.f};
    }
    ml_to_srgb8(img, rgba);
}

int runGolden(Context& ctx, bool update) {
    Rig rig;
    if (initRig(ctx, rig, ResolveKernelLanguage::Auto, false, true) != 1) {
        destroyRig(rig);
        std::printf("SKIP: no material-resolve kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, true)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    beginSceneFrame(ctx, s);
    FrameOptions opt{};
    opt.viewProj = frameCamera(0);
    opt.prevViewProj = opt.viewProj;
    opt.sampler = ctx.samplerHandles[1];
    opt.uber = false;
    if (!runFrame(ctx, s, rig, graph, opt, fs)) {
        std::fprintf(stderr, "FAIL: frame\n");
        return 1;
    }
    std::vector<f32> albedo(kPixels * 3u, 0.f);
    std::vector<f32> normal(kPixels * 3u, 0.f);
    std::vector<f32> refAlbedo(kPixels * 3u, 0.f);
    std::vector<f32> refNormal(kPixels * 3u, 0.f);
    std::vector<u8> covered(kPixels, 0u);
    const u32* vis = reinterpret_cast<const u32*>(rb(ctx, g_layout.vis[0]));
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<LayeredSurface> surfaces;
    layered_surfaces_reference(view, opt.viewProj.m, opt.prevViewProj.m, vis, kWidth, kHeight, surfaces);
    const MlTrilinearFilter f0{&ctx.chains, ctx.library.textures().data(), static_cast<u32>(ctx.library.textures().size()), 0.f};
    const MlView v0 = ml_trilinear_view(ctx.library, f0);
    u32 layered = 0;
    for (u32 p = 0; p < kPixels; ++p) {
        if (vis[p * 2u] == kVisInvalid) {
            continue;
        }
        covered[p] = 1u;
        const Px g = readPx(ctx, 0u, 0u, p);
        for (u32 k = 0; k < 3u; ++k) {
            albedo[p * 3u + k] = g.albedo[k];
            normal[p * 3u + k] = g.n[k];
            refAlbedo[p * 3u + k] = g.albedo[k];
            refNormal[p * 3u + k] = g.n[k];
        }
        if (surfaces[p].valid) {
            ++layered;
            const MlResult r = ml_evaluate(v0, surfaces[p].surface, surfaces[p].grad);
            for (u32 k = 0; k < 3u; ++k) {
                refAlbedo[p * 3u + k] = static_cast<f32>(clamp01(r.albedo[k]));
                refNormal[p * 3u + k] = r.normal[k];
            }
        }
    }
    std::vector<u8> gpuImage;
    std::vector<u8> cpuImage;
    gbufferImage(albedo, normal, covered, gpuImage);
    gbufferImage(refAlbedo, refNormal, covered, cpuImage);
    const ml_test::ImageDiff cpuDiff = ml_test::compareRgba(gpuImage, cpuImage, 2u);
    std::printf("golden: %u covered px, %u layered; GPU vs CPU reference image: %u px > 2 LSB, mean %.3f LSB, worst %u\n",
                static_cast<u32>(std::count(covered.begin(), covered.end(), u8{1})), layered, cpuDiff.over, cpuDiff.mean,
                cpuDiff.worst);
    expect(layered > 3000u, "golden scene has layered pixels");
    expect(cpuDiff.mean <= 1.0, "GPU G-buffer image vs the CPU reference image: mean <= 1 LSB");
    const std::vector<u8> png = ml_png_encode(kWidth, kHeight, gpuImage);
    const std::string out = std::string(FUSE_RP_MRL_OUTPUT_DIR) + "/" + kGoldenPng;
    ml_test::writeBytes(out, png);
    ml_test::writeBytes(std::string(FUSE_RP_MRL_OUTPUT_DIR) + "/resolve_layered_cpu_reference.png",
                        ml_png_encode(kWidth, kHeight, cpuImage));
    const std::string golden = std::string(FUSE_RP_ML_FIXTURE_DIR) + "/" + kGoldenPng;
    if (update) {
        expect(ml_test::writeBytes(golden, png), "write the golden image");
        std::printf("golden: updated %s\n", golden.c_str());
    } else {
        std::vector<u8> bytes;
        std::vector<u8> ref;
        u32 w = 0;
        u32 h = 0;
        const bool read = ml_test::readBytes(golden, bytes) && ml_png_decode(bytes, w, h, ref) && w == kWidth && h == kHeight;
        expect(read, "golden image present (run with --update-golden to create it)");
        if (read) {
            const ml_test::ImageDiff d = ml_test::compareRgba(gpuImage, ref, 2u);
            std::printf("golden: vs %s: %u px > 2 LSB, mean %.4f LSB, worst %u (bit-exact: %s); written %s\n", kGoldenPng,
                        d.over, d.mean, d.worst, gpuImage == ref ? "yes" : "no", out.c_str());
            expect(d.over <= kPixels / 200u && d.mean <= 0.1, "golden image: <= 0.5% pixels over 2 LSB, mean <= 0.1 LSB");
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "resolve.", 8) == 0 || std::strncmp(name, "vis.", 4) == 0 ||
                            std::strncmp(name, "cull.", 5) == 0 || std::strncmp(name, "materials.", 10) == 0)) {
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
    if (initRig(ctx, rig, ResolveKernelLanguage::Auto, false, true) != 1) {
        destroyRig(rig);
        std::printf("SKIP: no material-resolve kernels built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s, true)) {
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
        opt.viewProj = frameCamera(frame % 3u);
        opt.prevViewProj = prev;
        opt.sampler = ctx.samplerHandles[0];
        opt.uber = false;
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
        rig.layers.collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        expect(began && result.ok && waited, "frame ok");
        if (measure) {
            resolveSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, layered + feature bins, binned resolve with the layered table)\n"
                    "  MaterialResolve / VisBuffer / culler beginFrame + resolve.* / vis.* / cull.* pass callbacks: "
                    "%llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis, layers and resolve imports + passes): %llu\n",
                    kTotal - kWarmup, s.gpu.instanceHighWater(), resolveSide, callbacks, build);
        expect(resolveSide == 0u, "resolve side makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the layered table + resolve passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- plain_hash (manual bit-identity check against a build without the layered bin) -----------------
int runPlainHash(Context& ctx) {
    for (const ResolveKernelLanguage language : {ResolveKernelLanguage::Slang, ResolveKernelLanguage::Glsl}) {
        Rig rig;
        if (initRig(ctx, rig, language, true, false) != 1) {
            destroyRig(rig);
            continue;
        }
        Scene s;
        if (!buildScene(ctx, s, false, false)) {
            return 1;
        }
        rg::Graph graph;
        FrameState fs;
        Mat4 prev = frameCamera(0);
        for (u32 frame = 0; frame < 5u; ++frame) {
            beginSceneFrame(ctx, s);
            if (frame > 0u) {
                moveObjects(s, frame, 1u);
            }
            FrameOptions opt{};
            opt.viewProj = frameCamera(frame);
            opt.prevViewProj = prev;
            opt.sampler = ctx.samplerHandles[frame % 2u];
            opt.layeredTable = false;
            if (!runFrame(ctx, s, rig, graph, opt, fs)) {
                return 1;
            }
            u64 hb = 1469598103934665603ull;
            u64 hu = hb;
            for (u32 t = 0; t < kTargets; ++t) {
                hb = fnv(rb(ctx, g_layout.sets[0][0][t]), static_cast<usize>(kPixels) * kTexelBytes[t], hb);
                hu = fnv(rb(ctx, g_layout.sets[0][1][t]), static_cast<usize>(kPixels) * kTexelBytes[t], hu);
            }
            std::printf("plain_hash %s frame %u binned %016llx uber %016llx\n", rig.binned.kernelLanguage(), frame,
                        static_cast<unsigned long long>(hb), static_cast<unsigned long long>(hu));
            prev = opt.viewProj;
        }
        s.gpu.destroy();
        destroyRig(rig);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
    std::string backend = "set";
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--update-golden") == 0) {
            update = true;
        }
    }
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, backend == "buffer", mode != "plain_hash");
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "parity") {
            rc = runParity(ctx);
        } else if (mode == "golden") {
            rc = runGolden(ctx, update);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, false);
        } else if (mode == "plain_hash") {
            return runPlainHash(ctx);
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
