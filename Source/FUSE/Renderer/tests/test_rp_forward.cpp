// WP-2.3 forward transparency Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_forward_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, VisBuffer::addCulledFrame
// (WP-1.3 + WP-1.4, Raster), the binned material resolve (WP-1.5), ClusteredLighting (WP-2.1:
// assignment + light.shade with its f32 dump), then for each shader language built (Slang, GLSL) one
// ForwardTransparency (forward.copy + forward.draw with its per-layer dump) and read-back copies.
//
// Scene: ground plane, a back wall and a small box (opaque), three spheres (one mirrored, one partly
// behind the small box) lit by 3 lights (point, spot, directional) + ambient, 16x9x24 clusters, 256x192.
//
//   --mode twin     alpha-1 twin: frame A draws the spheres opaque (deferred), frame B flags them
//                   kInstanceTransparent (forward, opacity 1), same camera and lights. Checked:
//                     coverage   the pixels whose visible forward layer is a sphere == the pixels of
//                                frame A's G-buffer with a sphere material (depth test against the
//                                opaque depth included: the small box hides part of a sphere in both);
//                     surface    the visible layer's quantised surface (dump) vs frame A's G-buffer:
//                                depth <= 1e-5, normal <= 4e-3 rad (the WP-1.5 forward-vs-resolve
//                                interpolation tolerances), albedo / roughness / metallic / emissive exact;
//                     shading    every forward layer == lighting_gpu::ShadeKernel (the deferred shade's
//                                CPU reference) run on the layer's surface and the GPU cluster lists,
//                                within the WP-2.1 tolerance (1e-4 relative + 1e-6);
//                     twin       forward radiance vs frame A's deferred radiance, per pixel within
//                                2 (1e-4 |ref| + 1e-6) + |K(forward surface) - K(deferred surface)|
//                                (K = the CPU shade kernel: the only inputs that differ are the depth
//                                and normal interpolation bounded above); max relative error reported;
//                     image      frame B's composite == frame A's lit image bit for bit off the spheres,
//                                and == the forward radiance rounded to half on them (alpha 1 replaces).
//   --mode blend    4 frames of opacities in (0, 1) (and 0 / 1), two camera positions that change the
//                   back-to-front order, then the small box hidden: the RGBA16F composite == a CPU
//                   composite of the lit image and the dumped layers in the sorted order with the
//                   same premultiplied over rounded to half per layer (<= 1 half ulp per layer); every
//                   layer's shading == the CPU kernel; the draw order == back to front; the depth test
//                   (layer coverage with the box == without it, minus the pixels where the box's depth is
//                   nearer).
//   --mode media    participating media on the transparent fragments (ForwardFrameDesc::atmosphereAddress / fogAddress,
//                   kForwardFlagAerial | kForwardFlagFog): a synthetic WP-8.2 atmosphere (test_rp_sky_source.hpp) and a
//                   synthetic WP-8.1 froxel volume (resolveFogConstants of the frame's camera + a patterned integrated
//                   volume) in one host-visible buffer, declared through ForwardGraphRefs::atmosphere / fog. The RGBA16F
//                   composite == a CPU composite of the lit image and the dumped layers (radiance before the media)
//                   with each layer's radiance through at_aerial (screen uv, |P - camera|) x T + S x E and then
//                   fog_kernel::sample_integrated (screen point, view depth) x T + S (the CPU twins on the same data),
//                   premultiplied over rounded to half per layer (<= 1 half ulp per layer + 2e-4 relative); pixels
//                   without a transparent layer keep the lit image; the media change > 1000 pixels.
//   --mode zero_alloc  64 steady-state frames (spheres moving, opacities changing): 0 operator-new calls
//                   in ForwardTransparency::beginFrame, the forward.* pass callbacks and the whole graph
//                   build (validated run first; validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no shader built).
#include "test_rp_material_resolve_scene.hpp"
#include "test_rp_sky_source.hpp"

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/forward/forward_reference.hpp>
#include <fuse/renderer/forward/forward_transparency.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_kernel.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_reference.hpp>
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
using namespace fuse::renderer::forward;
using namespace fuse::renderer::gpu_scene;
using namespace fuse::renderer::lighting_gpu;
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::visbuffer;
using fuse::f32;
using fuse::f64;
using fuse::i32;
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
constexpr usize kStagingBytes = 8u * 1024u * 1024u;
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kLayers = 3; ///< transparent spheres = max draws per frame
constexpr f32 kFovY = 1.0f;
constexpr f32 kNear = 0.3f;
constexpr f32 kFar = 60.f;
constexpr f32 kAmbient[3] = {0.04f, 0.045f, 0.05f};
constexpr u32 kLanguages = 2; // Slang, GLSL

// Shading tolerance (GPU vs the CPU shade kernel on the same surface, light table and lists): the WP-2.1
// gate's, same justification (same f32 expressions in the same order, no contraction; device sqrt /
// division / log rounding of a few ulp per term).
constexpr f64 kTolRel = 1e-4;
constexpr f64 kTolAbs = 1e-6;
// Surface tolerances of the forward fragment vs the resolved G-buffer texel (WP-1.5, forward raster vs
// visibility-buffer resolve): the rasteriser's attribute interpolation vs the analytic barycentrics.
constexpr f64 kTolDepth = 1e-5;
constexpr f64 kTolNormal = 4e-3; // rad: two RGBA16F oct quanta

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

Mat4 lookAt(const Vec3& eye, const Vec3& at) {
    const Vec3 f = (at - eye).normalized();
    const Vec3 s = fuse::math::cross(f, Vec3{0.f, 1.f, 0.f}).normalized();
    const Vec3 u = fuse::math::cross(s, f);
    Mat4 v{};
    v.m[0] = s.x;
    v.m[4] = s.y;
    v.m[8] = s.z;
    v.m[1] = u.x;
    v.m[5] = u.y;
    v.m[9] = u.z;
    v.m[2] = -f.x;
    v.m[6] = -f.y;
    v.m[10] = -f.z;
    v.m[12] = -s.dot(eye);
    v.m[13] = -u.dot(eye);
    v.m[14] = f.dot(eye);
    v.m[15] = 1.f;
    return v;
}

struct FrameCamera {
    Mat4 viewProj{};
    ClusterCameraDesc cluster{};
};

FrameCamera makeCamera(const Vec3& eye, const Vec3& at) {
    FrameCamera c{};
    c.viewProj = mul(perspective(kFovY, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), kNear, kFar), lookAt(eye, at));
    c.cluster.position = eye;
    c.cluster.forward = at - eye;
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

f32 toHalf(f32 v) { return GBufferQuantize::toHalf(v); }

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
    Buffer shadeDump{};
    Buffer layerDumps[kLanguages]{};
    Buffer media{}; ///< --mode media: synthetic atmosphere + fog constants + integrated volume
    BindlessSlotHandle sampler{};
    u32 samplerHandle = 0;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (device != nullptr && sampler.isValid()) {
            bindless.releaseSampler(sampler);
        }
        if (allocator != nullptr) {
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
            allocator->destroyBuffer(shadeDump);
            for (Buffer& b : layerDumps) {
                allocator->destroyBuffer(b);
            }
            allocator->destroyBuffer(media);
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
    instanceDesc.appName = "fuse_rp_forward";
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
    const ForwardCapabilities forwardCaps = queryForwardCapabilities(ctx.device.get());
    if (!forwardCaps.forward) {
        std::printf("SKIP: forward transparency unsupported: %s\n", forwardCaps.reason);
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
    stagingDesc.name = "rp_forward.staging";
    const BufferUsage dumpUsage =
        static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    BufferDesc shadeDesc{};
    shadeDesc.size = static_cast<usize>(kPixels) * 16u;
    shadeDesc.usage = dumpUsage;
    shadeDesc.memoryUsage = MemoryUsage::GpuToCpu;
    shadeDesc.name = "rp_forward.shade_dump";
    BufferDesc layerDesc = shadeDesc;
    layerDesc.size = static_cast<usize>(kPixels) * kLayers * sizeof(ForwardDumpTexel);
    layerDesc.name = "rp_forward.layer_dump";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(shadeDesc, ctx.shadeDump) || ctx.shadeDump.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: buffers\n");
        return 1;
    }
    for (Buffer& b : ctx.layerDumps) {
        if (!ctx.allocator->createBuffer(layerDesc, b) || b.mapped == nullptr || b.deviceAddress == 0u) {
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
    SamplerDesc trilinear{};
    trilinear.minFilter = VK_FILTER_LINEAR;
    trilinear.magFilter = VK_FILTER_LINEAR;
    trilinear.addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    trilinear.name = "rp_forward.trilinear";
    ctx.sampler = ctx.bindless.acquireSampler(trilinear);
    if (!ctx.sampler.isValid()) {
        std::fprintf(stderr, "FAIL: sampler\n");
        return 1;
    }
    ctx.samplerHandle = ctx.bindless.shaderHandle(ctx.sampler);
    return 0;
}

// --- scene ----------------------------------------------------------------------------------------
// The box mesh has two submeshes (material index 0 and 1): a box instance uses rows material and material + 1.
enum MaterialSlot : u32 {
    kMatGround = 0,
    kMatWall = 1,  ///< + kMatWall + 1
    kMatBox = 3,   ///< + kMatBox + 1
    kMatSphere0 = 5,
    kMatSphere1,
    kMatSphere2,
    kMatCount,
};
enum Object : u32 { kGround = 0, kWall, kBox, kSphere0, kSphere1, kSphere2, kObjectCount };
constexpr u32 kSphereObjects[kLayers] = {kSphere0, kSphere1, kSphere2};

struct Scene {
    GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    InstanceHandle handles[kObjectCount]{};
    GpuTransform base[kObjectCount]{};
    std::vector<LightHandle> lights;
};

fuse::renderer::Material::GPUMaterial flatMaterial(f32 r, f32 g, f32 b, f32 metal, f32 rough) {
    fuse::renderer::Material::GPUMaterial m{};
    m.baseColor = {r, g, b, metal};
    m.roughnessEmissive = {rough, 0.f, 0.f, 0.f};
    return m;
}

constexpr u32 kOpaqueFlags = kInstanceVisible | kInstanceCastShadow | kInstanceReceiveShadow;
constexpr u32 kTransparentInstanceFlags = kInstanceVisible | kInstanceTransparent;

bool buildScene(Context& ctx, Scene& s) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = 64u;
    if (!s.gpu.init(d) || !s.gpu.gpuEnabled()) {
        return false;
    }
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
    const mr_test::SourceMesh sources[3] = {mr_test::uvSphere(24, 32, 1.f), mr_test::box(), mr_test::plane(8, 40.f, 4.f)};
    s.meshes.resize(3);
    for (u32 i = 0; i < 3u; ++i) {
        if (!mr_test::build(sources[i], s.meshes[i]) || s.gpu.addMeshletMesh(s.meshes[i]) != i) {
            return false;
        }
    }
    fuse::renderer::Material::GPUMaterial materials[kMatCount] = {
        flatMaterial(0.6f, 0.6f, 0.55f, 0.f, 0.8f),   flatMaterial(0.35f, 0.45f, 0.72f, 0.f, 0.6f),
        flatMaterial(0.45f, 0.35f, 0.72f, 0.f, 0.6f), flatMaterial(0.8f, 0.75f, 0.32f, 0.f, 0.52f),
        flatMaterial(0.32f, 0.75f, 0.8f, 0.f, 0.52f), flatMaterial(0.88f, 0.35f, 0.2f, 0.f, 0.35f),
        flatMaterial(0.95f, 0.8f, 0.45f, 1.f, 0.32f), flatMaterial(0.2f, 0.75f, 0.4f, 0.f, 0.55f)};
    // No channel sits on an unorm8 rounding tie (x * 255 = k + 0.5), where the attachment's conversion
    // and the forward pass's emulation may legitimately round differently.
    // Sphere 2 also emits a little (the emissive channel goes through the forward path too).
    materials[kMatSphere2].roughnessEmissive = {0.55f, 0.2f, 0.5f, 0.1f};
    materials[kMatSphere2].emissiveIntensity = 1.5f;
    for (u32 i = 0; i < kMatCount; ++i) {
        s.gpu.setMaterial(i, materials[i]);
    }
    struct Obj {
        u32 mesh;
        u32 material;
        GpuTransform t;
    };
    const Obj objects[kObjectCount] = {
        {2u, kMatGround, place(0.f, -1.3f, -10.f, 1.f, 1.f, 1.f, 0.2f)},
        {1u, kMatWall, place(0.f, 0.5f, -9.f, 5.f, 2.5f, 0.3f, 0.f)},
        {1u, kMatBox, place(1.35f, -0.55f, -1.4f, 0.45f, 0.45f, 0.45f, 0.5f)},
        {0u, kMatSphere0, place(0.1f, 0.25f, -4.8f, 1.25f, 1.25f, 1.25f, 0.3f)},
        {0u, kMatSphere1, place(-1.05f, -0.1f, -3.2f, 0.8f, -0.8f, 0.8f, 1.1f)}, // mirrored (det < 0)
        {0u, kMatSphere2, place(1.15f, -0.3f, -2.3f, 0.7f, 0.7f, 0.7f, -0.4f)},  // partly behind the box
    };
    for (u32 i = 0; i < kObjectCount; ++i) {
        InstanceDesc id{};
        id.mesh = objects[i].mesh;
        id.material = objects[i].material;
        id.transform = objects[i].t;
        id.flags = kOpaqueFlags;
        s.handles[i] = s.gpu.addInstance(id);
        s.base[i] = objects[i].t;
    }
    // 3 lights: a point light, a spot light and a directional light.
    GpuLight point{};
    point.type = static_cast<u32>(GpuLightType::Point);
    point.position[0] = -2.2f;
    point.position[1] = 1.8f;
    point.position[2] = -1.5f;
    point.range = 9.f;
    point.color[0] = 1.f;
    point.color[1] = 0.85f;
    point.color[2] = 0.7f;
    point.intensity = 9.f;
    s.lights.push_back(s.gpu.addLight(point));
    GpuLight spot{};
    spot.type = static_cast<u32>(GpuLightType::Spot);
    spot.position[0] = 2.5f;
    spot.position[1] = 2.8f;
    spot.position[2] = -1.f;
    const Vec3 sd = (Vec3{0.3f, 0.f, -3.5f} - Vec3{2.5f, 2.8f, -1.f}).normalized();
    spot.direction[0] = sd.x;
    spot.direction[1] = sd.y;
    spot.direction[2] = sd.z;
    spot.range = 12.f;
    spot.cosInner = 0.93f;
    spot.cosOuter = 0.8f;
    spot.color[0] = 0.6f;
    spot.color[1] = 0.75f;
    spot.color[2] = 1.f;
    spot.intensity = 30.f;
    s.lights.push_back(s.gpu.addLight(spot));
    GpuLight sun{};
    sun.type = static_cast<u32>(GpuLightType::Directional);
    const Vec3 dd = Vec3{0.4f, -1.f, -0.3f}.normalized();
    sun.direction[0] = dd.x;
    sun.direction[1] = dd.y;
    sun.direction[2] = dd.z;
    sun.color[0] = 1.f;
    sun.color[1] = 0.95f;
    sun.color[2] = 0.85f;
    sun.intensity = 1.2f;
    s.lights.push_back(s.gpu.addLight(sun));
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    return stats.ok && ctx.upload.waitAll();
}

// --- rig ------------------------------------------------------------------------------------------
struct Rig {
    InstanceCuller culler;
    VisBuffer vb;
    MaterialResolve resolve;
    ClusteredLighting lighting;
    ForwardTransparency forward[kLanguages];
    bool built[kLanguages] = {};
    const char* language[kLanguages] = {"slang", "glsl"};
};

int initRig(Context& ctx, Rig& rig) {
    InstanceCullerDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.instanceCapacity = 64u;
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
    ClusteredLightingDesc ld{};
    ld.device = ctx.device.get();
    ld.allocator = ctx.allocator.get();
    ld.bindless = &ctx.bindless;
    ld.lightCapacity = 16u;
    if (!rig.lighting.init(ld)) {
        return 0;
    }
    u32 built = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        ForwardTransparencyDesc fd{};
        fd.device = ctx.device.get();
        fd.allocator = ctx.allocator.get();
        fd.bindless = &ctx.bindless;
        fd.maxDraws = kLayers;
        fd.instanceCapacity = 64u;
        // mr_test meshes wind clockwise seen from outside; the projection flips y, so their front faces
        // are clockwise in framebuffer space.
        fd.frontFaceCounterClockwise = false;
        fd.language = k == 0u ? ForwardKernelLanguage::Slang : ForwardKernelLanguage::Glsl;
        rig.built[k] = rig.forward[k].init(fd);
        built += rig.built[k] ? 1u : 0u;
        std::printf("  forward shaders %s: %s\n", rig.language[k], rig.built[k] ? "built" : "not built, skipped");
    }
    return built > 0u ? 1 : 0;
}

void destroyRig(Rig& rig) {
    for (ForwardTransparency& f : rig.forward) {
        f.destroy();
    }
    rig.lighting.destroy();
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

/// G-buffer attachments read back (GBufferAttachment order) and their texel bytes, + the material id.
constexpr u32 kGBufferRead[5] = {0u, 1u, 2u, 4u, 5u};
constexpr u32 kGBufferBytes[5] = {8u, 4u, 4u, 4u, 8u};

struct ReadbackLayout {
    u64 gbuffer[5] = {};
    u64 materialId = 0;
    u64 lists = 0;
    u64 listsBytes = 0;
    u64 lit = 0;
    u64 color[kLanguages] = {};
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
    l.materialId = take(static_cast<u64>(kPixels) * 4u);
    l.listsBytes = listsBytes;
    l.lists = take(listsBytes);
    l.lit = take(static_cast<u64>(kPixels) * 8u);
    for (u32 k = 0; k < kLanguages; ++k) {
        l.color[k] = take(static_cast<u64>(kPixels) * 8u);
    }
    l.end = cursor;
    return l;
}

struct FrameState {
    CopyRecord copies[16];
    u32 copyCount = 0;
    ReadbackLayout layout{};
};

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
    readbackDesc.name = "rp_forward.readback";
    return ctx.allocator->createBuffer(readbackDesc, ctx.readback) && ctx.readback.mapped != nullptr;
}

/// --mode media: the synthetic media of the frame (null otherwise).
struct Media {
    fuse::renderer::test_sky::SyntheticSky sky;
    volumetric_gpu::FogFrameConstants fog{};
    std::vector<Vec4> integrated;
    u64 fogAddress = 0;
};
const Media* g_media = nullptr;

bool beginFrame(Context& ctx, Scene& s, Rig& rig, const FrameCamera& cam, bool dumps) {
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
    ok = rig.lighting.beginFrame(ctx.serial, lf) && ok;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        ForwardFrameDesc fd{};
        std::memcpy(fd.viewProj, cam.viewProj.m, sizeof(fd.viewProj));
        fd.scene = &s.gpu;
        fd.lighting = &rig.lighting;
        fd.sampler = ctx.samplerHandle;
        fd.dumpAddress = dumps ? ctx.layerDumps[k].deviceAddress : 0u;
        if (g_media != nullptr) {
            fd.atmosphereAddress = g_media->sky.address();
            fd.fogAddress = g_media->fogAddress;
        }
        if (dumps) {
            std::memset(ctx.layerDumps[k].mapped, 0, ctx.layerDumps[k].desc.size);
        }
        ok = rig.forward[k].beginFrame(ctx.serial, fd) && ok;
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
    // The uber path (WP-1.5: bit-identical to the binned one). With this scene (flat materials only) the
    // binned path's G-buffer stays at the first frame's content on Lavapipe (see the WP-2.3 row, open
    // issues); the forward pass does not depend on the path.
    rig.resolve.addResolve(graph, gbuffer, vis.vis, sceneRefs, ResolvePath::Uber);
    const LightingGraphRefs lighting = rig.lighting.importInto(graph);
    rig.lighting.addAssignment(graph, lighting, sceneRefs);
    rg::BufferRef shadeDump{};
    if (readback) {
        shadeDump = graph.importBuffer(
            rg::ImportedBuffer{ctx.shadeDump.handle, ctx.shadeDump.desc.size, rg::kNoQueue, nullptr, "rp_forward.shade_dump"});
    }
    rig.lighting.addShade(graph, lighting, sceneRefs, gbuffer, shadeDump, ctx.shadeDump.deviceAddress);
    ForwardGraphRefs forward[kLanguages]{};
    rg::BufferRef layerDumps[kLanguages]{};
    const rg::BufferRef media = g_media != nullptr ? graph.importBuffer(rg::ImportedBuffer{ctx.media.handle, ctx.media.desc.size, rg::kNoQueue,
                                                                                           nullptr, "rp_forward.media"})
                                                   : rg::BufferRef{};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        forward[k] = rig.forward[k].importInto(graph);
        forward[k].atmosphere = media; // one buffer holds both media
        forward[k].fog = media;
        if (readback) {
            layerDumps[k] = graph.importBuffer(rg::ImportedBuffer{ctx.layerDumps[k].handle, ctx.layerDumps[k].desc.size,
                                                                  rg::kNoQueue, nullptr, "rp_forward.layer_dump"});
        }
        rig.forward[k].addForward(graph, forward[k], sceneRefs, lighting, vis.depth, layerDumps[k]);
    }
    if (!readback) {
        return;
    }
    const rg::BufferRef rb = graph.importBuffer(
        rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_forward.readback"});
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
    addCopy(CopyRecord::Image, gbuffer.materialId, {}, fs.layout.materialId, static_cast<u64>(kPixels) * 4u);
    addCopy(CopyRecord::Buffer, {}, lighting.lists, fs.layout.lists, fs.layout.listsBytes);
    addCopy(CopyRecord::Image, lighting.output, {}, fs.layout.lit, static_cast<u64>(kPixels) * 8u);
    graph.addPass("readback.shade_dump", nullptr, nullptr).use(shadeDump, rg::Access::HostRead);
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        addCopy(CopyRecord::Image, forward[k].color, {}, fs.layout.color[k], static_cast<u64>(kPixels) * 8u);
        if (rig.forward[k].drawCount() > 0u) {
            graph.addPass("readback.layer_dump", nullptr, nullptr).use(layerDumps[k], rg::Access::HostRead);
        }
    }
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void beginSceneFrame(Context& ctx, Scene& s) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    s.gpu.beginFrame(ctx.serial);
}

bool runFrame(Context& ctx, Scene& s, Rig& rig, rg::Graph& graph, const FrameCamera& cam, FrameState& fs, bool readback) {
    const GpuSceneCommitStats stats = s.gpu.commit();
    ctx.upload.flush();
    if (!beginFrame(ctx, s, rig, cam, readback)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    buildGraph(ctx, s, rig, graph, fs, readback);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.culler.collectRetired(ctx.serial);
    rig.vb.collectRetired(ctx.serial);
    rig.resolve.collectRetired(ctx.serial);
    rig.lighting.collectRetired(ctx.serial);
    for (ForwardTransparency& f : rig.forward) {
        f.collectRetired(ctx.serial);
    }
    s.gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

// --- read-back decoding ---------------------------------------------------------------------------
const u8* rb(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

struct Captured {
    std::vector<f32> depth;
    std::vector<Vec4> rt0, rt1, rt2, rt5;
    std::vector<u32> material;
    std::vector<Vec4> lit;            ///< lighting output (RGBA16F)
    std::vector<Vec4> shade;          ///< light.shade f32 dump
    std::vector<Vec4> color[kLanguages]; ///< forward composite (RGBA16F)
    std::vector<ForwardDumpTexel> layers[kLanguages];
    std::vector<SortedDraw> draws[kLanguages];
    ClusterGridSoA grid{};
    std::vector<u32> directional;
};

Vec4 half4(const u8* p, usize pixel) {
    return {halfAt(p, pixel * 4u), halfAt(p, pixel * 4u + 1u), halfAt(p, pixel * 4u + 2u), halfAt(p, pixel * 4u + 3u)};
}

void capture(Context& ctx, const Rig& rig, const FrameState& fs, Captured& c) {
    c.depth.resize(kPixels);
    c.rt0.resize(kPixels);
    c.rt1.resize(kPixels);
    c.rt2.resize(kPixels);
    c.rt5.resize(kPixels);
    c.material.resize(kPixels);
    c.lit.resize(kPixels);
    c.shade.resize(kPixels);
    const u8* rt0 = rb(ctx, fs.layout.gbuffer[0]);
    const u8* rt1 = rb(ctx, fs.layout.gbuffer[1]);
    const u8* rt2 = rb(ctx, fs.layout.gbuffer[2]);
    const u8* rt4 = rb(ctx, fs.layout.gbuffer[3]);
    const u8* rt5 = rb(ctx, fs.layout.gbuffer[4]);
    const u8* lit = rb(ctx, fs.layout.lit);
    std::memcpy(c.material.data(), rb(ctx, fs.layout.materialId), static_cast<usize>(kPixels) * 4u);
    std::memcpy(c.shade.data(), ctx.shadeDump.mapped, static_cast<usize>(kPixels) * sizeof(Vec4));
    for (usize p = 0; p < kPixels; ++p) {
        std::memcpy(&c.depth[p], rt4 + p * 4u, 4u);
        c.rt0[p] = half4(rt0, p);
        c.rt5[p] = half4(rt5, p);
        c.rt1[p] = {rt1[p * 4u] / 255.f, rt1[p * 4u + 1u] / 255.f, rt1[p * 4u + 2u] / 255.f, rt1[p * 4u + 3u] / 255.f};
        c.rt2[p] = {rt2[p * 4u] / 255.f, rt2[p * 4u + 1u] / 255.f, rt2[p * 4u + 2u] / 255.f, rt2[p * 4u + 3u] / 255.f};
        c.lit[p] = half4(lit, p);
    }
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const u8* color = rb(ctx, fs.layout.color[k]);
        c.color[k].resize(kPixels);
        for (usize p = 0; p < kPixels; ++p) {
            c.color[k][p] = half4(color, p);
        }
        const u32 draws = rig.forward[k].drawCount();
        c.layers[k].resize(static_cast<usize>(draws) * kPixels);
        std::memcpy(c.layers[k].data(), ctx.layerDumps[k].mapped, c.layers[k].size() * sizeof(ForwardDumpTexel));
        c.draws[k].assign(rig.forward[k].draws(), rig.forward[k].draws() + draws);
    }
    // The GPU cluster lists of the frame (the forward pass and light.shade both read these).
    const LightingBufferLayout& L = rig.lighting.layout();
    const u8* lists = rb(ctx, fs.layout.lists);
    LightListHeader header{};
    std::memcpy(&header, lists + L.header, sizeof(header));
    const u32 clusters = rig.lighting.clusterDesc().clusterCount();
    c.grid.grid.resize(clusters);
    std::memcpy(c.grid.grid.data(), lists + L.grid, static_cast<usize>(clusters) * sizeof(ClusterGridEntry));
    c.grid.lightList.resize(std::min(header.totalEntries, clusters * rig.lighting.capacity()));
    std::memcpy(c.grid.lightList.data(), lists + L.lightList, c.grid.lightList.size() * 4u);
    c.directional.resize(std::min(header.directionalCount, rig.lighting.lightCapacity()));
    std::memcpy(c.directional.data(), lists + L.directional, c.directional.size() * 4u);
}

/// lighting_gpu::ShadeKernel (the deferred shade's CPU reference) over a "G-buffer".
std::vector<Vec4> cpuShade(const Scene& s, const Rig& rig, const FrameCamera& cam, const Captured& c, const std::vector<f32>& depth,
                           const std::vector<Vec4>& rt0, const std::vector<Vec4>& rt1, const std::vector<Vec4>& rt2,
                           const std::vector<Vec4>& rt5) {
    std::vector<GpuLight> table(s.gpu.lightHighWater());
    for (u32 i = 0; i < table.size(); ++i) {
        table[i] = s.gpu.light(i);
    }
    ShadeReferenceDesc sd{};
    sd.width = kWidth;
    sd.height = kHeight;
    sd.desc = rig.lighting.clusterDesc();
    sd.camera = cam.cluster;
    sd.ambient = {kAmbient[0], kAmbient[1], kAmbient[2]};
    sd.gbuffer = GBufferTexels{depth.data(), rt0.data(), rt1.data(), rt2.data(), rt5.data()};
    sd.lights = table.data();
    sd.lightCount = static_cast<u32>(table.size());
    sd.grid = &c.grid;
    sd.directional = &c.directional;
    sd.brdfLut = rig.lighting.brdfLut(); // WP-2.2 compensated BRDF when the lighting has its LUT
    std::vector<Vec4> out;
    shadeReferenceFrame(sd, out);
    return out;
}

/// One forward layer as a G-buffer (uncovered pixels: depth 1 = empty).
struct LayerGBuffer {
    std::vector<f32> depth;
    std::vector<Vec4> rt0, rt1, rt2, rt5;
};

LayerGBuffer layerGBuffer(const ForwardDumpTexel* layer) {
    LayerGBuffer g;
    g.depth.assign(kPixels, 1.f);
    g.rt0.resize(kPixels);
    g.rt1.resize(kPixels);
    g.rt2.resize(kPixels);
    g.rt5.resize(kPixels);
    for (usize p = 0; p < kPixels; ++p) {
        const ForwardDumpTexel& t = layer[p];
        if (t.covered == 0u) {
            continue;
        }
        g.depth[p] = t.depth;
        g.rt0[p] = {t.rt0[0], t.rt0[1], t.rt0[2], t.rt0[3]};
        g.rt1[p] = {t.rt1[0], t.rt1[1], t.rt1[2], t.rt1[3]};
        g.rt2[p] = {t.rt2[0], t.rt2[1], t.rt2[2], t.rt2[3]};
        g.rt5[p] = {t.rt5[0], t.rt5[1], t.rt5[2], t.rt5[3]};
    }
    return g;
}

bool within(f64 gpu, f64 ref, f64 extra = 0.0) { return std::fabs(gpu - ref) <= kTolRel * std::fabs(ref) + kTolAbs + extra; }

/// Every covered texel of every layer == the CPU shade kernel on the layer's own surface.
struct LayerShadeReport {
    u32 covered = 0;
    u32 bad = 0;
    f64 maxRel = 0.0;
    std::vector<std::vector<Vec4>> refs; ///< per layer
};

LayerShadeReport checkLayerShading(const Scene& s, const Rig& rig, const FrameCamera& cam, const Captured& c, u32 k) {
    LayerShadeReport r{};
    const usize layers = c.draws[k].size();
    for (usize d = 0; d < layers; ++d) {
        const ForwardDumpTexel* layer = c.layers[k].data() + d * kPixels;
        const LayerGBuffer g = layerGBuffer(layer);
        r.refs.push_back(cpuShade(s, rig, cam, c, g.depth, g.rt0, g.rt1, g.rt2, g.rt5));
        const std::vector<Vec4>& ref = r.refs.back();
        for (usize p = 0; p < kPixels; ++p) {
            const ForwardDumpTexel& t = layer[p];
            if (t.covered == 0u) {
                continue;
            }
            ++r.covered;
            bool bad = !(ref[p].w > 0.f) || t.slot != c.draws[k][d].slot || t.alpha != c.draws[k][d].opacity;
            const f32 rc[3] = {ref[p].x, ref[p].y, ref[p].z};
            for (u32 ch = 0; ch < 3u; ++ch) {
                bad = bad || !within(t.radiance[ch], rc[ch]);
                if (std::fabs(rc[ch]) > 1e-3) {
                    r.maxRel = std::max(r.maxRel, std::fabs(static_cast<f64>(t.radiance[ch]) - rc[ch]) / std::fabs(rc[ch]));
                }
            }
            r.bad += bad ? 1u : 0u;
        }
    }
    return r;
}

bool isSphereMaterial(u32 m) { return m == kMatSphere0 || m == kMatSphere1 || m == kMatSphere2; }

f64 normalAngle(const Vec4& a, const Vec4& b) {
    const Vec3 na = oct_decode_signed(a.x, a.y);
    const Vec3 nb = oct_decode_signed(b.x, b.y);
    return std::atan2(fuse::math::cross(na, nb).length(), static_cast<f64>(na.dot(nb)));
}

bool sameGrid(const ClusterGridSoA& a, const ClusterGridSoA& b) {
    if (a.grid.size() != b.grid.size() || a.lightList != b.lightList) {
        return false;
    }
    for (usize i = 0; i < a.grid.size(); ++i) {
        if (a.grid[i].offset != b.grid[i].offset || a.grid[i].count != b.grid[i].count) {
            return false;
        }
    }
    return true;
}

void setSpheres(Scene& s, u32 flags) {
    for (const u32 o : kSphereObjects) {
        s.gpu.setInstanceFlags(s.handles[o], flags);
    }
}

// --- twin -----------------------------------------------------------------------------------------
int runTwin(Context& ctx) {
    Rig rig;
    const int rc = initRig(ctx, rig);
    if (rc <= 0) {
        destroyRig(rig);
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init\n");
            return 1;
        }
        std::printf("SKIP: resolve / lighting / forward shaders not built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    FrameState fs;
    fs.layout = makeLayout(rig.lighting.layout().listsBytes);
    if (!ensureReadback(ctx, fs.layout)) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    rg::Graph graph;
    const FrameCamera cams[2] = {makeCamera(Vec3{0.2f, 0.9f, 2.2f}, Vec3{0.f, 0.f, -5.f}),
                                 makeCamera(Vec3{-1.6f, 1.4f, 1.4f}, Vec3{0.3f, -0.2f, -5.f})};
    for (u32 view = 0; view < 2u; ++view) {
        const FrameCamera& cam = cams[view];
        // Frame A: the spheres opaque (visibility buffer, resolve, deferred shade).
        beginSceneFrame(ctx, s);
        setSpheres(s, kOpaqueFlags);
        if (!runFrame(ctx, s, rig, graph, cam, fs, true)) {
            std::fprintf(stderr, "FAIL: frame A\n");
            return 1;
        }
        Captured a{};
        capture(ctx, rig, fs, a);
        // Frame B: the same spheres transparent at opacity 1 (forward pass).
        beginSceneFrame(ctx, s);
        setSpheres(s, kTransparentInstanceFlags);
        if (!runFrame(ctx, s, rig, graph, cam, fs, true)) {
            std::fprintf(stderr, "FAIL: frame B\n");
            return 1;
        }
        Captured b{};
        capture(ctx, rig, fs, b);
        expect(sameGrid(a.grid, b.grid) && a.directional == b.directional, "same cluster lists in both frames");
        // CPU shade of frame A's G-buffer (the deferred side of the surface term).
        const std::vector<Vec4> refDeferred = cpuShade(s, rig, cam, a, a.depth, a.rt0, a.rt1, a.rt2, a.rt5);
        u32 spherePixels = 0;
        u32 deferredSphereInB = 0;
        for (usize p = 0; p < kPixels; ++p) {
            spherePixels += isSphereMaterial(a.material[p]) ? 1u : 0u;
            deferredSphereInB += isSphereMaterial(b.material[p]) ? 1u : 0u;
        }
        expect(deferredSphereInB == 0u, "transparent instances never reach the visibility buffer / G-buffer");
        std::printf("  view %u: %u sphere pixels in the deferred frame, %zu light-list entries\n", view, spherePixels,
                    a.grid.lightList.size());
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            const std::vector<SortedDraw>& draws = b.draws[k];
            expect(draws.size() == kLayers, "every sphere drawn");
            bool order = true;
            for (usize d = 1; d < draws.size(); ++d) {
                order = order && draws[d - 1].key >= draws[d].key;
            }
            expect(order, "draws back to front");
            const LayerShadeReport shading = checkLayerShading(s, rig, cam, b, k);
            // Visible layer per pixel = the last covering draw (alpha 1: it replaces the others).
            u32 coverageBad = 0, surfaceBad = 0, twinBad = 0, imageBad = 0, offBad = 0, visible = 0;
            f64 maxDepth = 0.0, maxNormal = 0.0, maxTwinRel = 0.0, maxSurfaceTerm = 0.0;
            for (usize p = 0; p < kPixels; ++p) {
                i32 top = -1;
                for (usize d = 0; d < draws.size(); ++d) {
                    top = b.layers[k][d * kPixels + p].covered != 0u ? static_cast<i32>(d) : top;
                }
                const bool sphereA = isSphereMaterial(a.material[p]);
                if ((top >= 0) != sphereA) {
                    ++coverageBad;
                    continue;
                }
                const Vec4& outB = b.color[k][p];
                if (top < 0) {
                    // Off the spheres: the composite is frame A's lit image, bit for bit.
                    offBad += std::memcmp(&outB, &a.lit[p], sizeof(Vec4)) != 0 ? 1u : 0u;
                    continue;
                }
                ++visible;
                const ForwardDumpTexel& t = b.layers[k][static_cast<usize>(top) * kPixels + p];
                // Surface: the forward fragment vs the resolved texel.
                const f64 dz = std::fabs(static_cast<f64>(t.depth) - a.depth[p]);
                const f64 dn = normalAngle(Vec4{t.rt0[0], t.rt0[1], t.rt0[2], t.rt0[3]}, a.rt0[p]);
                maxDepth = std::max(maxDepth, dz);
                maxNormal = std::max(maxNormal, dn);
                bool sbad = dz > kTolDepth || dn > kTolNormal || t.rt0[3] != a.rt0[p].w;
                const Vec4 fwd[3] = {{t.rt1[0], t.rt1[1], t.rt1[2], t.rt1[3]},
                                     {t.rt2[0], t.rt2[1], t.rt2[2], t.rt2[3]},
                                     {t.rt5[0], t.rt5[1], t.rt5[2], t.rt5[3]}};
                const Vec4 def[3] = {a.rt1[p], a.rt2[p], a.rt5[p]};
                // RT1 / RT2 (unorm8) exact; RT5 (RGBA16F emissive) within one half ulp: the attachment's
                // f32 -> f16 conversion may truncate (Lavapipe does) where packHalf2x16 rounds to nearest.
                for (u32 i = 0; i < 2u; ++i) {
                    sbad = sbad || std::memcmp(&fwd[i], &def[i], sizeof(Vec4)) != 0;
                }
                const f32 fe[3] = {fwd[2].x, fwd[2].y, fwd[2].z};
                const f32 de[3] = {def[2].x, def[2].y, def[2].z};
                for (u32 ch = 0; ch < 3u; ++ch) {
                    sbad = sbad || !(std::fabs(static_cast<f64>(fe[ch]) - de[ch]) <= std::fabs(static_cast<f64>(de[ch])) / 1024.0 + 6e-8);
                }
                surfaceBad += sbad ? 1u : 0u;
                // Twin: forward radiance vs the deferred radiance of the opaque twin.
                const Vec4& ref = a.shade[p];
                const Vec4& kf = shading.refs[static_cast<usize>(top)][p];
                const Vec4& kd = refDeferred[p];
                const f32 fr[3] = {t.radiance[0], t.radiance[1], t.radiance[2]};
                const f32 dr[3] = {ref.x, ref.y, ref.z};
                const f32 kfr[3] = {kf.x, kf.y, kf.z};
                const f32 kdr[3] = {kd.x, kd.y, kd.z};
                bool tbad = !(ref.w > 0.f);
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const f64 surfaceTerm = std::fabs(static_cast<f64>(kfr[ch]) - kdr[ch]);
                    maxSurfaceTerm = std::max(maxSurfaceTerm, surfaceTerm / std::max(1e-3, std::fabs(static_cast<f64>(dr[ch]))));
                    const f64 diff = std::fabs(static_cast<f64>(fr[ch]) - dr[ch]);
                    tbad = tbad || !(diff <= 2.0 * (kTolRel * std::fabs(static_cast<f64>(dr[ch])) + kTolAbs) + surfaceTerm);
                    if (std::fabs(dr[ch]) > 1e-3) {
                        maxTwinRel = std::max(maxTwinRel, diff / std::fabs(static_cast<f64>(dr[ch])));
                    }
                    // The composite at alpha 1 is the forward radiance rounded to half.
                    const f32 o[3] = {outB.x, outB.y, outB.z};
                    // The attachment's f32 -> f16 conversion may round either way (Vulkan: RTE or RTZ;
                    // Lavapipe truncates): within one half ulp of the f32 radiance.
                    imageBad += std::fabs(static_cast<f64>(o[ch]) - fr[ch]) <= std::fabs(static_cast<f64>(fr[ch])) / 1024.0 + 6e-8 ? 0u : 1u;
                }
                imageBad += outB.w != 1.f ? 1u : 0u;
                twinBad += tbad ? 1u : 0u;
            }
            std::printf("    %s: layers %zu covered %u, shade-vs-kernel bad %u (max rel %.2e) | visible %u coverage-bad %u "
                        "surface-bad %u (max depth %.2e, normal %.2e rad) twin-bad %u (max rel %.2e, surface term %.2e) "
                        "image-bad %u off-sphere-bad %u\n",
                        rig.language[k], draws.size(), shading.covered, shading.bad, shading.maxRel, visible, coverageBad,
                        surfaceBad, maxDepth, maxNormal, twinBad, maxTwinRel, maxSurfaceTerm, imageBad, offBad);
            expect(shading.covered > 0u && shading.bad == 0u, "forward layers == the deferred shade kernel on their surface");
            expect(visible == spherePixels && coverageBad == 0u, "forward coverage == the opaque twin's coverage");
            expect(surfaceBad == 0u, "forward surface == the twin's G-buffer within the interpolation tolerances");
            expect(twinBad == 0u, "forward radiance == the deferred twin within the justified tolerance");
            expect(imageBad == 0u && offBad == 0u, "alpha-1 composite == lit image off the spheres, forward radiance on them");
        }
        if (rig.built[0] && rig.built[1]) {
            u32 diff = 0;
            for (usize p = 0; p < kPixels; ++p) {
                diff += std::memcmp(&b.color[0][p], &b.color[1][p], sizeof(Vec4)) != 0 ? 1u : 0u;
            }
            std::printf("    slang vs glsl composite: %u px differ\n", diff);
        }
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- blend ----------------------------------------------------------------------------------------
int runBlend(Context& ctx) {
    Rig rig;
    const int rc = initRig(ctx, rig);
    if (rc <= 0) {
        destroyRig(rig);
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init\n");
            return 1;
        }
        std::printf("SKIP: resolve / lighting / forward shaders not built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    FrameState fs;
    fs.layout = makeLayout(rig.lighting.layout().listsBytes);
    if (!ensureReadback(ctx, fs.layout)) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    rg::Graph graph;
    struct Step {
        f32 opacity[kLayers];
        u32 camera;
        bool boxHidden;
    };
    const Step steps[5] = {{{0.35f, 0.6f, 0.8f}, 0u, false},
                           {{1.f, 0.5f, 0.f}, 0u, false},
                           {{0.7f, 0.25f, 0.5f}, 1u, false},
                           {{0.35f, 0.6f, 0.8f}, 0u, true},
                           {{0.9f, 0.9f, 0.15f}, 1u, false}};
    const FrameCamera cams[2] = {makeCamera(Vec3{0.2f, 0.9f, 2.2f}, Vec3{0.f, 0.f, -5.f}),
                                 // From the far left the mirrored sphere becomes the farthest one.
                                 makeCamera(Vec3{-7.5f, 1.2f, -7.f}, Vec3{0.4f, -0.3f, -3.f})};
    std::vector<ForwardDumpTexel> withBox[kLanguages];
    std::vector<f32> boxDepth;
    std::vector<SortedDraw> withBoxDraws[kLanguages];
    u32 orders[2][kLayers] = {};
    for (u32 step = 0; step < 5u; ++step) {
        const Step& st = steps[step];
        const FrameCamera& cam = cams[st.camera];
        beginSceneFrame(ctx, s);
        setSpheres(s, kTransparentInstanceFlags);
        s.gpu.setInstanceFlags(s.handles[kBox], st.boxHidden ? 0u : kOpaqueFlags);
        for (u32 i = 0; i < kLayers; ++i) {
            for (ForwardTransparency& f : rig.forward) {
                if (f.valid()) {
                    f.setOpacity(s.handles[kSphereObjects[i]], st.opacity[i]);
                }
            }
        }
        if (!runFrame(ctx, s, rig, graph, cam, fs, true)) {
            std::fprintf(stderr, "FAIL: blend frame %u\n", step);
            return 1;
        }
        Captured c{};
        capture(ctx, rig, fs, c);
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            const std::vector<SortedDraw>& draws = c.draws[k];
            // Order: back to front by the clip w of each sphere's centre (brute force here).
            bool order = draws.size() == kLayers;
            f32 prevW = 1e30f;
            for (usize d = 0; order && d < draws.size(); ++d) {
                const GpuTransform& t = s.gpu.transform(draws[d].slot);
                const f32 w = forward::clip_row_dot(cam.viewProj.m, 3u, Vec3{t.rows[0][3], t.rows[1][3], t.rows[2][3]});
                order = w <= prevW;
                prevW = w;
                if (step < 3u && st.camera < 2u) {
                    orders[st.camera][d] = draws[d].slot;
                }
            }
            expect(order, "draws sorted back to front");
            const LayerShadeReport shading = checkLayerShading(s, rig, cam, c, k);
            // CPU composite: the lit image, then every layer in draw order, premultiplied over, rounded to
            // half after each layer (the RGBA16F attachment). Tolerance per channel: n layers x |x| / 1024 (>= one
            // half ulp of x) + the subnormal step: each blend rounds its f32 result to half once, either way
            // (Vulkan allows RTE or RTZ; Lavapipe truncates, the emulation rounds to nearest), and (1 - a) < 1
            // keeps earlier rounding differences from growing.
            u32 compositeBad = 0, layered[kLayers + 1u] = {};
            f64 maxUlps = 0.0;
            for (usize p = 0; p < kPixels; ++p) {
                Vec4 dst = c.lit[p];
                u32 n = 0;
                for (usize d = 0; d < draws.size(); ++d) {
                    const ForwardDumpTexel& t = c.layers[k][d * kPixels + p];
                    if (t.covered == 0u) {
                        continue;
                    }
                    ++n;
                    const Vec4 o = blend_over(Vec3{t.radiance[0], t.radiance[1], t.radiance[2]}, t.alpha, dst);
                    dst = {toHalf(o.x), toHalf(o.y), toHalf(o.z), toHalf(o.w)};
                }
                ++layered[n];
                const Vec4& g = c.color[k][p];
                const f32 gv[4] = {g.x, g.y, g.z, g.w};
                const f32 cv[4] = {dst.x, dst.y, dst.z, dst.w};
                for (u32 ch = 0; ch < 4u; ++ch) {
                    const f64 ulp = std::max(std::fabs(static_cast<f64>(cv[ch])), std::fabs(static_cast<f64>(gv[ch]))) / 1024.0 + 6e-8;
                    const f64 diff = std::fabs(static_cast<f64>(gv[ch]) - cv[ch]);
                    maxUlps = std::max(maxUlps, diff / ulp);
                    compositeBad += diff <= std::max(1u, n) * ulp ? 0u : 1u;
                }
            }
            std::printf("  step %u (%s, opacities %.2f %.2f %.2f%s) %s: order", step, st.camera == 0u ? "front" : "left",
                        st.opacity[0], st.opacity[1], st.opacity[2], st.boxHidden ? ", box hidden" : "", rig.language[k]);
            for (const SortedDraw& d : draws) {
                std::printf(" %u", d.slot);
            }
            std::printf(" | px with 0/1/2/3 layers %u/%u/%u/%u | shade-bad %u (max rel %.2e) | composite-bad %u (max %.2f half ulp)\n",
                        layered[0], layered[1], layered[2], layered[3], shading.bad, shading.maxRel, compositeBad, maxUlps);
            expect(layered[2] > 0u && layered[1] > 0u, "overlapping transparent layers");
            expect(shading.bad == 0u, "every layer == the deferred shade kernel on its surface");
            expect(compositeBad == 0u, "RGBA16F composite == CPU composite of the sorted layers");
            // Depth test: the box hides part of sphere 2 (step 0 vs step 3, same camera and opacities).
            if (step == 0u) {
                withBox[k] = c.layers[k];
                withBoxDraws[k] = draws;
                boxDepth = c.depth;
            } else if (step == 3u) {
                u32 hidden = 0, extra = 0, missing = 0;
                for (usize d = 0; d < draws.size(); ++d) {
                    expect(draws[d].slot == withBoxDraws[k][d].slot, "same order with and without the box");
                    for (usize p = 0; p < kPixels; ++p) {
                        const ForwardDumpTexel& without = c.layers[k][d * kPixels + p];
                        const ForwardDumpTexel& with = withBox[k][d * kPixels + p];
                        if (with.covered != 0u && without.covered == 0u) {
                            ++missing;
                        } else if (with.covered == 0u && without.covered != 0u) {
                            // Hidden with the box: the opaque depth there must be nearer than the fragment.
                            ++hidden;
                            extra += boxDepth[p] < without.depth ? 0u : 1u;
                        }
                    }
                }
                std::printf("    depth test: %u layer texels hidden by the box (%u not explained by a nearer opaque depth), "
                            "%u appear only with the box\n",
                            hidden, extra, missing);
                expect(hidden > 50u && extra == 0u && missing == 0u, "depth test against the opaque depth");
            }
        }
    }
    bool changed = false;
    for (u32 d = 0; d < kLayers; ++d) {
        changed = changed || orders[0][d] != orders[1][d];
    }
    expect(changed, "the two cameras sort the spheres differently");
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- media ----------------------------------------------------------------------------------------
int runMedia(Context& ctx) {
    Rig rig;
    const int rc = initRig(ctx, rig);
    if (rc <= 0) {
        destroyRig(rig);
        if (rc < 0) {
            std::fprintf(stderr, "FAIL: rig init\n");
            return 1;
        }
        std::printf("SKIP: resolve / lighting / forward shaders not built\n");
        return kSkip;
    }
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    const FrameCamera cam = makeCamera(Vec3{0.2f, 0.9f, 2.2f}, Vec3{0.f, 0.f, -5.f});
    Media media;
    const Vec3 fwd = cam.cluster.forward.normalized();
    const Vec3 right = fuse::math::cross(fwd, Vec3{0.f, 1.f, 0.f}).normalized();
    const Vec3 up = fuse::math::cross(right, fwd);
    const f32 tanY = std::tan(kFovY * 0.5f);
    if (!media.sky.build(cam.cluster.position, Vec3{0.3f, 0.8f, 0.2f}, fwd, right, up, tanY * static_cast<f32>(kWidth) / kHeight, tanY,
                         12.f)) {
        std::fprintf(stderr, "FAIL: synthetic sky\n");
        return 1;
    }
    volumetric_gpu::FroxelFogSettings fs{};
    fs.gridX = 16;
    fs.gridY = 12;
    fs.gridZ = 24;
    fs.farPlane = 20.f;
    if (!volumetric_gpu::resolveFogConstants(fs, cam.cluster, nullptr, false, 0u, kWidth, kHeight, media.fog)) {
        std::fprintf(stderr, "FAIL: fog constants\n");
        return 1;
    }
    const u32 gx = media.fog.gridX, gy = media.fog.gridY, gz = media.fog.gridZ;
    media.integrated.resize(static_cast<usize>(gx) * gy * gz);
    for (u32 y = 0; y < gy; ++y) {
        for (u32 x = 0; x < gx; ++x) {
            for (u32 z = 0; z < gz; ++z) {
                const f32 k = static_cast<f32>(z + 1u) / static_cast<f32>(gz);
                const f32 u = static_cast<f32>(x) / static_cast<f32>(gx);
                const f32 v = static_cast<f32>(y) / static_cast<f32>(gy);
                media.integrated[volumetric_gpu::fog_kernel::froxel_index(media.fog, x, y, z)] =
                    Vec4{0.05f * k * (1.f + u), 0.04f * k * (1.f + v), 0.06f * k, std::exp(-0.8f * k * (1.f + 0.3f * u))};
            }
        }
    }
    const u64 skyBytes = (media.sky.bytes() + 255u) & ~u64{255u};
    const u64 fogBytes = (sizeof(volumetric_gpu::FogFrameConstants) + 255u) & ~u64{255u};
    BufferDesc bd{};
    bd.size = static_cast<usize>(skyBytes + fogBytes + media.integrated.size() * sizeof(Vec4));
    bd.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    bd.memoryUsage = MemoryUsage::CpuToGpu;
    bd.name = "rp_forward.media";
    if (!ctx.allocator->createBuffer(bd, ctx.media) || ctx.media.mapped == nullptr || ctx.media.deviceAddress == 0u) {
        std::fprintf(stderr, "FAIL: media buffer\n");
        return 1;
    }
    u8* mapped = static_cast<u8*>(ctx.media.mapped);
    media.sky.write(mapped, ctx.media.deviceAddress);
    media.fog.integrated = ctx.media.deviceAddress + skyBytes + fogBytes;
    media.fogAddress = ctx.media.deviceAddress + skyBytes;
    std::memcpy(mapped + skyBytes, &media.fog, sizeof(media.fog));
    std::memcpy(mapped + skyBytes + fogBytes, media.integrated.data(), media.integrated.size() * sizeof(Vec4));
    FrameState fstate;
    fstate.layout = makeLayout(rig.lighting.layout().listsBytes);
    if (!ensureReadback(ctx, fstate.layout)) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    rg::Graph graph;
    const f32 opacity[kLayers] = {0.35f, 0.6f, 0.8f};
    Captured c{};
    Captured plain{};
    for (u32 pass = 0; pass < 2u; ++pass) { // without, then with the media
        g_media = pass == 1u ? &media : nullptr;
        beginSceneFrame(ctx, s);
        setSpheres(s, kTransparentInstanceFlags);
        for (u32 i = 0; i < kLayers; ++i) {
            for (ForwardTransparency& f : rig.forward) {
                if (f.valid()) {
                    f.setOpacity(s.handles[kSphereObjects[i]], opacity[i]);
                }
            }
        }
        const bool ok = runFrame(ctx, s, rig, graph, cam, fstate, true);
        g_media = nullptr;
        if (!ok) {
            std::fprintf(stderr, "FAIL: media frame %u\n", pass);
            return 1;
        }
        capture(ctx, rig, fstate, pass == 1u ? c : plain);
    }
    const clustered_kernel::CameraView view = clustered_kernel::make_camera(cam.cluster);
    const Vec3 E = media.sky.illuminance();
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        const std::vector<SortedDraw>& draws = c.draws[k];
        u32 compositeBad = 0, changed = 0, bareBad = 0, covered = 0;
        f64 maxUlps = 0.0;
        for (u32 py = 0; py < kHeight; ++py) {
            for (u32 px = 0; px < kWidth; ++px) {
                const usize p = static_cast<usize>(py) * kWidth + px;
                const f32 sx = (static_cast<f32>(px) + 0.5f) * (1.f / static_cast<f32>(kWidth));
                const f32 sy = (static_cast<f32>(py) + 0.5f) * (1.f / static_cast<f32>(kHeight));
                Vec4 dst = c.lit[p];
                u32 n = 0;
                for (usize d = 0; d < draws.size(); ++d) {
                    const ForwardDumpTexel& t = c.layers[k][d * kPixels + p];
                    if (t.covered == 0u) {
                        continue;
                    }
                    ++n;
                    const f32 vd = clustered_kernel::view_depth_from_device_depth(t.depth, view.near_plane, view.far_plane, false);
                    const Vec3 pos =
                        clustered_kernel::view_to_world(view, clustered_kernel::view_position_from_screen(sx, sy, vd, view.tan_x, view.tan_y));
                    const Vec3 toCam = view.position - pos;
                    Vec3 L{t.radiance[0], t.radiance[1], t.radiance[2]};
                    Vec3 sc{}, tr{};
                    media.sky.aerial(sx, sy, std::sqrt(toCam.dot(toCam)), sc, tr);
                    L = Vec3{L.x * tr.x + sc.x * E.x, L.y * tr.y + sc.y * E.y, L.z * tr.z + sc.z * E.z};
                    const Vec4 fog = volumetric_gpu::fog_kernel::sample_integrated(media.fog, media.integrated.data(), sx, sy, vd);
                    L = Vec3{L.x * fog.w + fog.x, L.y * fog.w + fog.y, L.z * fog.w + fog.z};
                    const Vec4 o = blend_over(L, t.alpha, dst);
                    dst = {toHalf(o.x), toHalf(o.y), toHalf(o.z), toHalf(o.w)};
                }
                const Vec4& g = c.color[k][p];
                covered += n > 0u ? 1u : 0u;
                if (n == 0u) {
                    bareBad += std::memcmp(&g, &c.lit[p], sizeof(Vec4)) == 0 ? 0u : 1u;
                    continue;
                }
                changed += std::memcmp(&g, &plain.color[k][p], sizeof(Vec4)) != 0 ? 1u : 0u;
                const f32 gv[4] = {g.x, g.y, g.z, g.w};
                const f32 cv[4] = {dst.x, dst.y, dst.z, dst.w};
                for (u32 ch = 0; ch < 4u; ++ch) {
                    const f64 ulp = std::max(std::fabs(static_cast<f64>(cv[ch])), std::fabs(static_cast<f64>(gv[ch]))) / 1024.0 + 6e-8;
                    const f64 diff = std::fabs(static_cast<f64>(gv[ch]) - cv[ch]);
                    maxUlps = std::max(maxUlps, diff / ulp);
                    compositeBad += diff <= n * ulp + 2e-4 * std::fabs(static_cast<f64>(cv[ch])) ? 0u : 1u;
                }
            }
        }
        std::printf("  media %s: %u px with transparent layers (%u changed by the media), composite-bad %u (max %.2f half ulp), "
                    "bare px != lit %u\n",
                    rig.language[k], covered, changed, compositeBad, maxUlps, bareBad);
        expect(covered > 1000u && changed * 10u >= covered * 9u, "the media change the transparent pixels");
        expect(compositeBad == 0u, "RGBA16F composite == CPU composite of the fogged / aerial-perspective layers");
        expect(bareBad == 0u, "pixels without a transparent layer keep the lit image (media only on transparent fragments)");
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "forward.", 8) == 0) {
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
    if (initRig(ctx, rig) != 1) {
        destroyRig(rig);
        std::printf("SKIP: resolve / lighting / forward shaders not built\n");
        return kSkip;
    }
    if (rig.built[0] && rig.built[1]) {
        rig.forward[1].destroy();
        rig.built[1] = false;
    }
    const u32 k = rig.built[0] ? 0u : 1u;
    Scene s;
    if (!buildScene(ctx, s)) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long forwardSide = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    const FrameCamera cam = makeCamera(Vec3{0.2f, 0.9f, 2.2f}, Vec3{0.f, 0.f, -5.f});
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        beginSceneFrame(ctx, s);
        if (frame == 0u) {
            setSpheres(s, kTransparentInstanceFlags);
        }
        for (u32 i = 0; i < kLayers; ++i) {
            const u32 o = kSphereObjects[i];
            GpuTransform t = s.base[o];
            t.rows[0][3] += 0.4f * std::sin(0.3f * static_cast<f32>(frame + 7u * i));
            t.rows[2][3] += 0.6f * std::cos(0.2f * static_cast<f32>(frame + 5u * i));
            s.gpu.setTransform(s.handles[o], t);
            rig.forward[k].setOpacity(s.handles[o], 0.2f + 0.7f * static_cast<f32>((frame + i) % 8u) / 7.f);
        }
        s.gpu.commit();
        ctx.upload.flush();
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
        ok = rig.lighting.beginFrame(ctx.serial, lf) && ok;
        ForwardFrameDesc fd{};
        std::memcpy(fd.viewProj, cam.viewProj.m, sizeof(fd.viewProj));
        fd.scene = &s.gpu;
        fd.lighting = &rig.lighting;
        fd.sampler = ctx.samplerHandle;
        t_allocations = 0;
        t_count = measure;
        ok = rig.forward[k].beginFrame(ctx.serial, fd) && ok;
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
        rig.lighting.collectRetired(ctx.serial);
        rig.forward[k].collectRetired(ctx.serial);
        s.gpu.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(ok && result.ok && waited, "frame ok");
        expect(rig.forward[k].drawCount() == kLayers, "three transparent draws per frame");
        if (measure) {
            forwardSide += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rig.forward[k].stats().passes == 1u && rig.forward[k].stats().targetRebuilds == 1u,
           "one forward pass per frame, no target rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (3 transparent spheres moving, opacities changing, culled VB + resolve + "
                    "lighting + forward)\n"
                    "  ForwardTransparency::beginFrame + forward.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene, cull, vis, resolve, lighting and forward imports + passes): %llu\n",
                    kTotal - kWarmup, forwardSide, callbacks, build);
        expect(forwardSide == 0u, "forward transparency makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the forward passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    s.gpu.destroy();
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "twin";
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
        if (mode == "twin") {
            rc = runTwin(ctx);
        } else if (mode == "blend") {
            rc = runBlend(ctx);
        } else if (mode == "media") {
            rc = runMedia(ctx);
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
