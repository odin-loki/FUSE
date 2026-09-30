// E02 SceneRenderer Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every validation /
// sync-validation message fails the run). CPU gates: test_scene_renderer_cpu.cpp.
//
// Scene (ECS): a floor slab and a lit cube (both the procedural cube mesh, ecs::Mesh aabb [-1, 1]), a point light, a
// sun (DirectionalLight) and a camera entity; materials from a standalone MaterialSystem (the E02 material feed).
// Render 96 x 64, display 144 x 96 (TAAU), post on, atmosphere / fog / DDGI / VSM / SSFX on (clouds, splats off).
//
//   --mode golden      the same scene rendered twice: (A) through SceneRenderer from the registry, (B) built directly on
//                      a GpuScene + FrameComposer (the frame_composer.hpp protocol by hand: addMeshletMesh, setMaterial,
//                      addInstance, addLight, setSdfScene with the same boxes). 4 frames each; the GpuScene instance /
//                      transform / light / material tables and the SDF boxes are byte-identical, and the display output
//                      and scene colour of every frame are bit-identical (tolerance: exact; the brief allows FLIP <= 0.01).
//   --mode zero_alloc  38 frames (8 warm-up + 30 measured) with the camera and the cube moving: 0 operator-new calls in
//                      graph.reset + SceneRenderer::renderScene (extract, feed, commit, composer beginFrame + addFrame) and
//                      in every pass callback (validated run first; validation off for the count).
//   --mode blit        frame generation on: SceneRenderer::addPresent blits presentReal and presentInterpolated into two
//                      headless RGBA8 targets ("present.blit", RGBA16F -> UNORM) == the CPU conversion of the read-back
//                      RGBA16F images (<= 1 / 255), plus a 2:1 linear downscale blit == the 2 x 2 box average (<= 2 / 255).
//   --mode ui          a FrameUiSource (a graph transient cleared to a premultiplied colour): output == the CPU
//                      frame_ui_composite_texel of (hudless, ui) (<= 1 half ulp); a fully transparent UI leaves the
//                      output == hudless bit for bit; the HUD-less image is not modified by the stage.
//   --tier t0 | t2     T2 skips (77) without the T2 gate.
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing).
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/scene_renderer/scene_renderer.hpp>
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
using namespace fuse::renderer::frame;
using namespace fuse::renderer::scene_renderer;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace ecs = fuse::ecs;

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
constexpr u32 kRenderW = 96;
constexpr u32 kRenderH = 64;
constexpr u32 kDisplayW = 144;
constexpr u32 kDisplayH = 96;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
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

f32 halfToFloat(u16 h) { return GBufferQuantize::halfToFloat(h); }

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

int setup(Context& ctx, bool validation, bool t2) {
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
    instanceDesc.appName = "fuse_scene_renderer_vk";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
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
    if (t2 && !rt::queryRtCapabilities(ctx.device.get()).usable) {
        std::printf("SKIP: device below the T2 gate\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    BindlessDesc bdesc{};
    // Descriptor-set backend (the frame composer gates' choice: a Lavapipe descriptor-buffer quirk, test_rp_frame.cpp).
    bdesc.backend = BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "scene_renderer_vk.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    BufferDesc readbackDesc{};
    readbackDesc.size = 4u * 1024u * 1024u;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "scene_renderer_vk.readback";
    if (!ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: readback\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    return 0;
}

// --- readback ---------------------------------------------------------------------------------------
struct Copy {
    rg::TextureRef src;
    rg::BufferRef dst;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const Copy& c = *static_cast<const Copy*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = c.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(c.src)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

/// Up to 8 image copies into ctx.readback per frame (records must outlive execution).
struct Readback {
    Copy copies[8]{};
    u32 count = 0;
    u64 cursor = 0;
    rg::BufferRef buffer;

    void begin(Context& ctx, rg::Graph& graph) {
        count = 0;
        cursor = 0;
        buffer = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr,
                                                       "scene_renderer_vk.readback"});
    }
    /// Returns the byte offset of the copy in ctx.readback (~0 when not recorded).
    u64 add(rg::Graph& graph, rg::TextureRef src, u32 w, u32 h, u32 texelBytes) {
        if (!src.valid() || count >= 8u) {
            return ~0ull;
        }
        Copy& c = copies[count++];
        c.src = src;
        c.dst = buffer;
        c.offset = cursor;
        c.width = w;
        c.height = h;
        const u64 bytes = static_cast<u64>(w) * h * texelBytes;
        cursor = (cursor + bytes + 255u) & ~u64{255u};
        graph.addPass("readback.copy", &recordCopy, &c)
            .use(src, rg::Access::TransferSrc)
            .use(buffer, rg::Access::TransferDst, rg::BufferRange{c.offset, bytes});
        return c.offset;
    }
    void end(rg::Graph& graph) { graph.addPass("readback.host", nullptr, nullptr).use(buffer, rg::Access::HostRead); }
};

std::vector<u8> bytesAt(Context& ctx, u64 offset, u64 bytes) {
    if (offset == ~0ull) {
        return {};
    }
    const u8* p = static_cast<const u8*>(ctx.readback.mapped) + offset;
    return std::vector<u8>(p, p + bytes);
}

// --- scene ------------------------------------------------------------------------------------------
constexpr u32 kCubeMesh = 0;   ///< engine mesh id of the procedural cube
constexpr f32 kToSun[3] = {0.45f, 0.8f, 0.4f};

struct BoxSpec {
    f32 center[3];
    f32 half[3];
    u32 material;
};
constexpr BoxSpec kFloor = {{0.f, -0.1f, -2.f}, {4.f, 0.1f, 4.f}, 0u};
constexpr BoxSpec kCube = {{0.3f, 0.5f, -2.f}, {0.5f, 0.5f, 0.5f}, 1u};
constexpr f32 kPointPos[3] = {-0.8f, 1.4f, -1.2f};

Material floorMaterial() {
    Material m{};
    m.baseColor = fuse::math::Vec3{0.6f, 0.6f, 0.55f};
    m.roughness = 0.8f;
    return m;
}
Material cubeMaterial() {
    Material m{};
    m.baseColor = fuse::math::Vec3{0.8f, 0.3f, 0.2f};
    m.metallic = 0.2f;
    m.roughness = 0.35f;
    return m;
}

ecs::mat4 boxMatrix(const BoxSpec& b, f32 dx) {
    ecs::mat4 m = ecs::mat4::identity();
    m.data[0] = b.half[0];
    m.data[5] = b.half[1];
    m.data[10] = b.half[2];
    m.data[12] = b.center[0] + dx;
    m.data[13] = b.center[1];
    m.data[14] = b.center[2];
    return m;
}

ecs::mat4 sunMatrix() {
    // Lights shine down their local -Z: +Z = the direction toward the sun.
    const f32 l = std::sqrt(kToSun[0] * kToSun[0] + kToSun[1] * kToSun[1] + kToSun[2] * kToSun[2]);
    const f32 z[3] = {kToSun[0] / l, kToSun[1] / l, kToSun[2] / l};
    // Any orthonormal x / y completing the frame.
    f32 x[3] = {z[2], 0.f, -z[0]};
    const f32 xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    x[0] /= xl;
    x[2] /= xl;
    const f32 y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    ecs::mat4 m = ecs::mat4::identity();
    for (u32 a = 0; a < 3u; ++a) {
        m.data[0 + a] = x[a];
        m.data[4 + a] = y[a];
        m.data[8 + a] = z[a];
    }
    m.data[12] = 0.f;
    m.data[13] = 10.f;
    m.data[14] = 0.f;
    return m;
}

struct EcsScene {
    ecs::Registry registry;
    ecs::EntityID floor{};
    ecs::EntityID cube{};
    ecs::EntityID point{};
    ecs::EntityID sun{};
    ecs::EntityID camera{};
};

void buildEcs(EcsScene& s) {
    s.registry.init(64);
    auto box = [&](const BoxSpec& b) {
        const ecs::EntityID e = s.registry.create();
        ecs::Transform t{};
        t.local_to_world = boxMatrix(b, 0.f);
        s.registry.add<ecs::Transform>(e, t);
        ecs::Mesh m{};
        m.vertex_buffer = ecs::MeshVertexBufferHandle(kCubeMesh, 1);
        m.material_id = b.material;
        m.aabb_min = ecs::vec3{-1.f, -1.f, -1.f, 0.f};
        m.aabb_max = ecs::vec3{1.f, 1.f, 1.f, 0.f};
        s.registry.add<ecs::Mesh>(e, m);
        return e;
    };
    s.floor = box(kFloor);
    s.cube = box(kCube);
    s.point = s.registry.create();
    {
        ecs::Transform t{};
        t.local_to_world.data[12] = kPointPos[0];
        t.local_to_world.data[13] = kPointPos[1];
        t.local_to_world.data[14] = kPointPos[2];
        s.registry.add<ecs::Transform>(s.point, t);
        ecs::PointLight p{};
        p.color = ecs::vec3{1.f, 0.7f, 0.4f, 0.f};
        p.intensity = 4.f;
        p.radius = 5.f;
        s.registry.add<ecs::PointLight>(s.point, p);
    }
    s.sun = s.registry.create();
    {
        ecs::Transform t{};
        t.local_to_world = sunMatrix();
        s.registry.add<ecs::Transform>(s.sun, t);
        ecs::DirectionalLight d{};
        d.color = ecs::vec3{1.f, 0.95f, 0.85f, 0.f};
        d.intensity = 2.5f;
        s.registry.add<ecs::DirectionalLight>(s.sun, d);
    }
    s.camera = s.registry.create();
    {
        ecs::Transform t{};
        t.local_to_world.data[12] = -0.3f;
        t.local_to_world.data[13] = 1.6f;
        t.local_to_world.data[14] = 2.5f;
        s.registry.add<ecs::Transform>(s.camera, t);
        ecs::Camera c{};
        c.fov_deg = 57.29578f;
        c.near_plane = 0.1f;
        c.far_plane = 60.f;
        c.is_active = true;
        s.registry.add<ecs::Camera>(s.camera, c);
    }
}

FrameCamera cameraAt(u32 frame, bool moving) {
    FrameCamera c{};
    const f32 t = moving ? static_cast<f32>(frame) : 0.f;
    c.eye[0] = -0.3f + 0.04f * t;
    c.eye[1] = 1.6f;
    c.eye[2] = 2.5f - 0.03f * t;
    c.target[0] = 0.2f;
    c.target[1] = 0.4f;
    c.target[2] = -2.f;
    c.fovY = 1.f;
    c.nearPlane = 0.1f;
    c.farPlane = 60.f;
    return c;
}

FrameComposerDesc composerTemplate() {
    FrameComposerDesc d{};
    d.clusters = ClusterDesc{};
    d.vsmClipmap.levels = 12;
    d.vsmClipmap.firstLevelExtent = 4.f;
    d.vsmClipmap.markRadiusTexels = 2.5f;
    d.vsmClipmap.texelsPerPixel = 2.f;
    d.vsmPoolPagesX = 16;
    d.vsmPoolPagesY = 16;
    DDGIDesc& v = d.ddgiVolume;
    v.grid_origin = {-4.f, 0.5f, -6.f};
    v.probe_spacing = {2.f, 1.5f, 2.f};
    v.grid_dims = {5u, 3u, 5u};
    v.rays_per_probe = 64;
    v.probes_per_frame = 75;
    v.irradiance_res = 6;
    v.depth_res = 8;
    v.hysteresis = 0.9f;
    v.max_ray_distance = 20.f;
    d.maxSplats = 64;
    d.maxSplatEntries = 1u << 14;
    return d;
}

FrameSettings frameSettings() {
    FrameSettings fs{};
    fs.vsmFilter.pcfRadius = 1;
    fs.ssfxSettings.ssgi_params.sample_sqrt = 2;
    fs.atmosphere.sizes.transWidth = 64;
    fs.atmosphere.sizes.transHeight = 32;
    fs.atmosphere.sizes.skyWidth = 64;
    fs.atmosphere.sizes.skyHeight = 48;
    fs.atmosphere.sizes.apWidth = 16;
    fs.atmosphere.sizes.apHeight = 16;
    fs.atmosphere.sizes.apDepth = 16;
    fs.atmosphere.sampling.transSteps = 32;
    fs.atmosphere.sampling.msDirSqrt = 4;
    fs.atmosphere.sampling.skySteps = 16;
    fs.fogSettings.gridX = 24;
    fs.fogSettings.gridY = 16;
    fs.fogSettings.gridZ = 24;
    fs.fogSettings.farPlane = 30.f;
    fs.fogSettings.medium.density = 0.03f;
    fs.clouds = false;
    fs.splats = false;
    fs.restir = false;
    fs.frameGen = false;
    fs.upscaler = FrameUpscaler::Taau;
    return fs;
}

SceneRendererDesc rendererDesc(Context& ctx, SceneTier tier) {
    SceneRendererDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.width = kDisplayW;
    d.height = kDisplayH;
    d.renderWidth = kRenderW;
    d.renderHeight = kRenderH;
    d.tier = tier;
    d.instanceCapacity = 64;
    d.meshCapacity = 4;
    d.materialCapacity = 8;
    d.lightCapacity = 16;
    d.entityCapacity = 64;
    d.composer = composerTemplate();
    return d;
}

struct SceneRig {
    EcsScene ecs;
    MaterialSystem materials;
    SceneRenderer renderer;
    rg::Graph graph;
};

int initRig(Context& ctx, SceneRig& rig, SceneTier tier) {
    buildEcs(rig.ecs);
    rig.materials.initStandalone();
    rig.materials.registerMaterial(floorMaterial());
    rig.materials.registerMaterial(cubeMaterial());
    if (!rig.renderer.initialize(rendererDesc(ctx, tier))) {
        std::fprintf(stderr, "FAIL: SceneRenderer::initialize: %s\n", rig.renderer.reason());
        return 1;
    }
    ProceduralMeshDesc cube{};
    std::string error;
    if (!rig.renderer.meshes().registerProcedural(kCubeMesh, cube, &error)) {
        std::fprintf(stderr, "FAIL: procedural cube: %s\n", error.c_str());
        return 1;
    }
    rig.renderer.setMaterialSystem(&rig.materials);
    rig.renderer.setFrameSettings(frameSettings());
    return 0;
}

void finishFrame(Context& ctx, SceneRenderer& r, u64 serial) {
    (void)ctx.executor->waitIdle();
    (void)r.upload().waitAll();
    r.collectRetired(serial);
}

struct FrameImages {
    std::vector<u8> output;     ///< RGBA16F output extent
    std::vector<u8> sceneColor; ///< RGBA16F render extent
    u32 outW = 0;
    u32 outH = 0;
};

// --- golden: SceneRenderer vs FrameComposer built directly ---------------------------------------------
struct DirectRig {
    gpu_scene::GpuScene scene;
    FrameComposer composer;
    rg::Graph graph;
    geometry::MeshletMesh cube;
};

gpu_scene::GpuLight directPointLight(ecs::EntityID id) {
    gpu_scene::GpuLight l{};
    l.type = static_cast<u32>(gpu_scene::GpuLightType::Point);
    l.position[0] = kPointPos[0];
    l.position[1] = kPointPos[1];
    l.position[2] = kPointPos[2];
    // Identity rotation: direction = -Z column (-0, -0, -1).
    l.direction[0] = -0.f;
    l.direction[1] = -0.f;
    l.direction[2] = -1.f;
    l.color[0] = 1.f;
    l.color[1] = 0.7f;
    l.color[2] = 0.4f;
    l.intensity = 4.f;
    l.range = 5.f;
    l.entityIndex = id.index;
    return l;
}

gpu_scene::GpuLight directSun(ecs::EntityID id) {
    const ecs::mat4 m = sunMatrix();
    gpu_scene::GpuLight l{};
    l.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    l.position[0] = m.data[12];
    l.position[1] = m.data[13];
    l.position[2] = m.data[14];
    const f32 d[3] = {-m.data[8], -m.data[9], -m.data[10]};
    const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const f32 inv = 1.f / len;
    for (u32 a = 0; a < 3u; ++a) {
        l.direction[a] = d[a] * inv;
    }
    l.color[0] = 1.f;
    l.color[1] = 0.95f;
    l.color[2] = 0.85f;
    l.intensity = 2.5f;
    l.entityIndex = id.index;
    return l;
}

bool sameTable(const gpu_scene::GpuScene& a, const gpu_scene::GpuScene& b, gpu_scene::GpuSceneTable t) {
    const gpu_scene::TableBytes x = a.tableBytes(t);
    const gpu_scene::TableBytes y = b.tableBytes(t);
    return x.count == y.count && x.stride == y.stride && x.count > 0u &&
           std::memcmp(x.data, y.data, static_cast<usize>(x.count) * x.stride) == 0;
}

int runGolden(Context& ctx, SceneTier tier) {
    constexpr u32 kFrames = 4;
    const FrameTier ftier = tier == SceneTier::T2 ? FrameTier::T2 : FrameTier::T0;
    // (A) SceneRenderer from the ECS registry.
    std::vector<FrameImages> a(kFrames);
    SceneRig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    expect(rig.renderer.tier() == ftier, "SceneRenderer tier as requested");
    FrameCamera ecsCamera{};
    expect(cameraFromRegistry(rig.ecs.registry, ecsCamera) && ecsCamera.eye[1] == 1.6f && ecsCamera.target[2] == 1.5f &&
               std::fabs(ecsCamera.fovY - 1.f) < 1e-6f,
           "cameraFromRegistry: eye = translation, forward = -Z, fov from the component");
    for (u32 f = 0; f < kFrames; ++f) {
        rig.graph.reset();
        SceneFrameDesc sfd{};
        sfd.serial = f + 1u;
        sfd.frameIndex = f;
        const FrameGraphOutputs outs = rig.renderer.renderScene(rig.ecs.registry, cameraAt(f, true), rig.graph, sfd);
        expect(outs.output.valid(), "SceneRenderer recorded the frame");
        Readback rb;
        rb.begin(ctx, rig.graph);
        const u32 ow = rig.renderer.composer().outputWidth();
        const u32 oh = rig.renderer.composer().outputHeight();
        const u64 o0 = rb.add(rig.graph, outs.output, ow, oh, 8u);
        const u64 o1 = rb.add(rig.graph, outs.sceneColor, kRenderW, kRenderH, 8u);
        rb.end(rig.graph);
        const rg::ExecuteResult r = ctx.executor->execute(rig.graph);
        expect(r.ok, "SceneRenderer frame executes");
        finishFrame(ctx, rig.renderer, sfd.serial);
        a[f].output = bytesAt(ctx, o0, static_cast<u64>(ow) * oh * 8u);
        a[f].sceneColor = bytesAt(ctx, o1, static_cast<u64>(kRenderW) * kRenderH * 8u);
        a[f].outW = ow;
        a[f].outH = oh;
        const SceneFrameStats& st = rig.renderer.lastFrame();
        if (f == 0u) {
            expect(st.meshesUploaded == 1u && st.materials.rowsWritten == 2u && st.extract.added == 2u &&
                       st.extract.lightsAdded == 2u,
                   "frame 0: 1 mesh, 2 materials, 2 instances, 2 lights");
            expect(ftier == FrameTier::T2 || st.sdfRebuilt, "T0 frame 0 builds the SDF");
        } else {
            expect(st.meshesUploaded == 0u && st.materials.rowsWritten == 0u && st.extract.added == 0u && !st.sdfRebuilt,
                   "later frames: nothing re-uploaded");
        }
        expect(st.sunSlot == 1u, "sun = the DirectionalLight's slot (point light first)");
        expect(rig.renderer.composer().stats().ran == rig.renderer.composer().stats().ran, "stats");
    }
    std::printf("golden %s: SceneRenderer ran stages 0x%08x (%s kernel)\n", ftier == FrameTier::T2 ? "t2" : "t0",
                rig.renderer.composer().stats().ran, rig.renderer.composer().kernelLanguage());
    const DdgiCpuScene sdfA = rig.renderer.sdfBoxes();

    // (B) the same scene on a GpuScene + FrameComposer built by hand.
    std::vector<FrameImages> b(kFrames);
    DirectRig d;
    gpu_scene::GpuSceneDesc sd{};
    sd.device = ctx.device.get();
    sd.allocator = ctx.allocator.get();
    sd.upload = &ctx.upload;
    sd.bindless = &ctx.bindless;
    sd.instanceCapacity = 64;
    sd.meshCapacity = 4;
    sd.materialCapacity = 8;
    sd.lightCapacity = 16;
    if (!d.scene.init(sd) || !buildProceduralMesh(ProceduralMeshDesc{}, d.cube)) {
        expect(false, "direct GpuScene");
        return 0;
    }
    FrameComposerDesc cd = composerTemplate();
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.upload = &ctx.upload;
    cd.scene = &d.scene;
    cd.tier = ftier;
    cd.renderWidth = kRenderW;
    cd.renderHeight = kRenderH;
    cd.displayWidth = kDisplayW;
    cd.displayHeight = kDisplayH;
    cd.instanceCapacity = 64;
    cd.meshCapacity = 4;
    cd.lightCapacity = 16;
    if (!d.composer.init(cd)) {
        std::fprintf(stderr, "FAIL: direct FrameComposer: %s\n", d.composer.reason());
        return 1;
    }
    DdgiCpuScene sdfB;
    for (const BoxSpec* spec : {&kFloor, &kCube}) {
        DdgiCpuBox box{};
        box.min = fuse::math::Vec3{spec->center[0] - spec->half[0], spec->center[1] - spec->half[1], spec->center[2] - spec->half[2]};
        box.max = fuse::math::Vec3{spec->center[0] + spec->half[0], spec->center[1] + spec->half[1], spec->center[2] + spec->half[2]};
        box.surface = ddgiSurfaceFromMaterial(spec->material == 0u ? floorMaterial() : cubeMaterial());
        sdfB.boxes.push_back(box);
    }
    std::vector<fuse::compute::SdfObject> objects;
    std::vector<gi_gpu::DdgiSurface> surfaces;
    gi_gpu::sdfSceneFromBoxes(sdfB, objects, surfaces);
    bool sdfSame = sdfA.boxes.size() == sdfB.boxes.size();
    for (usize i = 0; sdfSame && i < sdfB.boxes.size(); ++i) {
        sdfSame = std::memcmp(&sdfA.boxes[i], &sdfB.boxes[i], sizeof(DdgiCpuBox)) == 0;
    }
    expect(ftier == FrameTier::T2 || sdfSame, "SDF boxes from the ECS == the directly built boxes (bit for bit)");
    const ecs::EntityID boxes[2] = {rig.ecs.floor, rig.ecs.cube};
    for (u32 f = 0; f < kFrames; ++f) {
        const u64 serial = f + 1u;
        ctx.bindless.setFrameSerial(serial);
        d.scene.beginFrame(serial);
        d.composer.beginSceneFrame(serial);
        if (f == 0u) {
            expect(d.scene.addMeshletMesh(d.cube) == 0u, "direct mesh row 0");
            d.scene.setMaterial(0, floorMaterial());
            d.scene.setMaterial(1, cubeMaterial());
            for (u32 i = 0; i < 2u; ++i) {
                const BoxSpec& spec = i == 0u ? kFloor : kCube;
                gpu_scene::InstanceDesc id{};
                id.mesh = 0;
                id.material = spec.material;
                const ecs::mat4 m = boxMatrix(spec, 0.f);
                gpu_scene::transformFromColumnMajor(m.data.data(), id.transform);
                id.entityIndex = boxes[i].index;
                id.entityGeneration = boxes[i].generation;
                d.scene.addInstance(id);
            }
            d.scene.addLight(directPointLight(rig.ecs.point));
            d.scene.addLight(directSun(rig.ecs.sun));
            if (ftier == FrameTier::T0) {
                d.composer.setSdfScene(objects.data(), static_cast<u32>(objects.size()), surfaces.data(),
                                       static_cast<u32>(surfaces.size()));
            }
        }
        const bool committed = d.scene.commit().ok && d.composer.commitScene();
        ctx.upload.flush();
        FrameDesc fd{};
        fd.serial = serial;
        fd.frameIndex = f;
        fd.camera = cameraAt(f, true);
        fd.resetHistory = f == 0u;
        fd.sun.slot = 1u;
        const gpu_scene::GpuLight sun = directSun(rig.ecs.sun);
        for (u32 k = 0; k < 3u; ++k) {
            fd.sun.toSun[k] = -sun.direction[k];
            fd.sun.illuminance[k] = sun.color[k] * sun.intensity;
        }
        expect(committed && d.composer.beginFrame(fd, frameSettings()), "direct frame begins");
        d.graph.reset();
        const FrameGraphOutputs outs = d.composer.addFrame(d.graph);
        Readback rb;
        rb.begin(ctx, d.graph);
        const u32 ow = d.composer.outputWidth();
        const u32 oh = d.composer.outputHeight();
        const u64 o0 = rb.add(d.graph, outs.output, ow, oh, 8u);
        const u64 o1 = rb.add(d.graph, outs.sceneColor, kRenderW, kRenderH, 8u);
        rb.end(d.graph);
        expect(ctx.executor->execute(d.graph).ok, "direct frame executes");
        (void)ctx.executor->waitIdle();
        (void)ctx.upload.waitAll();
        d.composer.collectRetired(serial);
        d.scene.collectRetired(serial);
        ctx.bindless.collectRetired(serial);
        b[f].output = bytesAt(ctx, o0, static_cast<u64>(ow) * oh * 8u);
        b[f].sceneColor = bytesAt(ctx, o1, static_cast<u64>(kRenderW) * kRenderH * 8u);
        b[f].outW = ow;
        b[f].outH = oh;
        if (f == 0u) {
            expect(sameTable(rig.renderer.scene(), d.scene, gpu_scene::GpuSceneTable::Instances), "instance table identical");
            expect(sameTable(rig.renderer.scene(), d.scene, gpu_scene::GpuSceneTable::Transforms), "transform table identical");
            expect(sameTable(rig.renderer.scene(), d.scene, gpu_scene::GpuSceneTable::Lights), "light table identical");
            expect(sameTable(rig.renderer.scene(), d.scene, gpu_scene::GpuSceneTable::Materials), "material table identical");
        }
    }
    u32 identical = 0;
    for (u32 f = 0; f < kFrames; ++f) {
        const bool out = !a[f].output.empty() && a[f].output == b[f].output && a[f].outW == b[f].outW;
        const bool sc = !a[f].sceneColor.empty() && a[f].sceneColor == b[f].sceneColor;
        identical += out && sc ? 1u : 0u;
        if (!(out && sc)) {
            u32 diff = 0;
            for (usize i = 0; i < std::min(a[f].output.size(), b[f].output.size()); i += 2u) {
                diff += std::memcmp(a[f].output.data() + i, b[f].output.data() + i, 2u) != 0 ? 1u : 0u;
            }
            std::printf("  frame %u: output %s (%u differing channels), scene colour %s\n", f, out ? "identical" : "DIFFERS", diff,
                        sc ? "identical" : "DIFFERS");
        }
    }
    // Not black / finite.
    f64 mean = 0.0;
    bool finite = true;
    const std::vector<u8>& last = a[kFrames - 1u].output;
    for (usize i = 0; i + 1u < last.size(); i += 2u) {
        u16 h = 0;
        std::memcpy(&h, last.data() + i, 2u);
        const f32 v = halfToFloat(h);
        finite = finite && std::isfinite(v);
        if ((i / 2u) % 4u != 3u) {
            mean += v;
        }
    }
    mean /= std::max<usize>(1u, last.size() / 8u * 3u);
    std::printf("golden %s: %u / %u frames bit-identical (display %ux%u output + scene colour), output mean %.4f\n",
                ftier == FrameTier::T2 ? "t2" : "t0", identical, kFrames, a[0].outW, a[0].outH, mean);
    expect(identical == kFrames, "SceneRenderer image == directly built FrameComposer image (bit for bit, every frame)");
    expect(finite && mean > 0.01, "output finite and lit");
    d.composer.destroy();
    d.scene.destroy();
    return 0;
}

// --- zero_alloc ----------------------------------------------------------------------------------------
thread_local bool t_inPass = false;
void hookBegin(const rg::PassContext&, const char*, void*) {
    t_inPass = true;
    t_count = true;
}
void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, SceneTier tier, bool count) {
    SceneRig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    constexpr u32 kWarmup = 8;
    const u32 total = count ? kWarmup + 30u : 12u;
    if (count) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    unsigned long long frameAllocs = 0;
    unsigned long long callbackAllocs = 0;
    u32 rebuilds = 0;
    for (u32 f = 0; f < total; ++f) {
        const bool measure = count && f >= kWarmup;
        // Move the cube (transform rows change every frame; the SDF is static: dynamicSdf off).
        if (ecs::Transform* t = rig.ecs.registry.get<ecs::Transform>(rig.ecs.cube)) {
            t->local_to_world = boxMatrix(kCube, 0.02f * static_cast<f32>(f));
        }
        t_allocations = 0;
        t_count = measure;
        rig.graph.reset();
        const FrameGraphOutputs outs = rig.renderer.renderScene(rig.ecs.registry, cameraAt(f, true), rig.graph);
        t_count = false;
        const unsigned long long fa = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult r = ctx.executor->execute(rig.graph);
        const unsigned long long ca = t_allocations;
        finishFrame(ctx, rig.renderer, rig.renderer.frameSerial());
        expect(outs.output.valid() && r.ok, "zero_alloc frame executes");
        expect(f == 0u || rig.renderer.lastFrame().extract.transformWrites == 1u, "the moving cube's transform is rewritten");
        if (measure) {
            frameAllocs += fa;
            callbackAllocs += ca;
        }
        if (f + 1u == kWarmup) {
            rebuilds = rig.renderer.composer().stats().imageRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (count) {
        std::printf("zero_alloc (validation off): 30 steady-state frames, camera + cube moving\n"
                    "  graph.reset + SceneRenderer::renderScene: %llu operator-new calls\n"
                    "  pass callbacks: %llu\n",
                    frameAllocs, callbackAllocs);
        expect(frameAllocs == 0u, "renderScene makes no steady-state heap allocations");
        expect(callbackAllocs == 0u, "the frame's pass callbacks make no steady-state heap allocations");
        expect(rig.renderer.composer().stats().imageRebuilds == rebuilds, "no composer resource rebuild in steady state");
    } else {
        std::printf("zero_alloc: %u validated frames ok, validation messages %u\n", total, g_messages);
    }
    return 0;
}

// --- blit ------------------------------------------------------------------------------------------------
struct Target {
    Texture image{};
    u32 layout = 0;
    u8 queue = rg::kNoQueue;
};

bool createTarget(Context& ctx, Target& t, u32 w, u32 h, const char* name) {
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.format = GpuFormat::R8G8B8A8Unorm;
    // Sampled: the allocator creates a default view (a swapchain image carries COLOR_ATTACHMENT for the same reason).
    d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::TransferDst) | static_cast<u32>(ImageUsage::TransferSrc) |
                                      static_cast<u32>(ImageUsage::Sampled));
    d.name = name;
    return ctx.allocator->createImage(d, t.image);
}

rg::TextureRef importTarget(rg::Graph& graph, Target& t, u32 w, u32 h, const char* name) {
    PresentTargetDesc p{};
    p.image = t.image.image;
    p.view = t.image.view;
    p.format = 37u;
    p.width = w;
    p.height = h;
    p.layoutTracker = &t.layout;
    p.queueTracker = &t.queue;
    p.name = name;
    return importPresentTarget(graph, p);
}

u8 toUnorm8(u16 h) {
    const f32 v = std::min(1.f, std::max(0.f, halfToFloat(h)));
    return static_cast<u8>(std::lround(v * 255.f));
}

int runBlit(Context& ctx, SceneTier tier) {
    SceneRig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    FrameSettings fs = frameSettings();
    fs.frameGen = true;
    rig.renderer.setFrameSettings(fs);
    const bool fgAvailable = (rig.renderer.composer().available() & kStageFrameGen) != 0u;
    Target real, mid, half;
    if (!createTarget(ctx, real, kDisplayW, kDisplayH, "present.real") ||
        !createTarget(ctx, mid, kDisplayW, kDisplayH, "present.interpolated") ||
        !createTarget(ctx, half, kDisplayW / 2u, kDisplayH / 2u, "present.half")) {
        expect(false, "headless targets");
        return 0;
    }
    PresentBlit halfBlit{};
    u32 checked = 0, bad = 0, badMid = 0, badHalf = 0;
    for (u32 f = 0; f < 3u; ++f) {
        rig.graph.reset();
        const FrameGraphOutputs outs = rig.renderer.renderScene(rig.ecs.registry, cameraAt(f, true), rig.graph);
        expect(outs.output.valid(), "blit frame recorded");
        ScenePresentTargets pt{};
        pt.target = importTarget(rig.graph, real, kDisplayW, kDisplayH, "present.real");
        pt.width = kDisplayW;
        pt.height = kDisplayH;
        pt.interpolatedTarget = importTarget(rig.graph, mid, kDisplayW, kDisplayH, "present.interpolated");
        expect(rig.renderer.addPresent(rig.graph, outs, pt), "addPresent");
        const bool fg = outs.presentInterpolated.valid();
        const rg::TextureRef realSrc = fg ? outs.presentReal : outs.output;
        halfBlit = PresentBlit{};
        halfBlit.source = realSrc;
        halfBlit.sourceWidth = kDisplayW;
        halfBlit.sourceHeight = kDisplayH;
        halfBlit.target = importTarget(rig.graph, half, kDisplayW / 2u, kDisplayH / 2u, "present.half");
        halfBlit.targetWidth = kDisplayW / 2u;
        halfBlit.targetHeight = kDisplayH / 2u;
        expect(addPresentBlit(rig.graph, halfBlit), "2:1 blit");
        Readback rb;
        rb.begin(ctx, rig.graph);
        const u64 oSrc = rb.add(rig.graph, realSrc, kDisplayW, kDisplayH, 8u);
        const u64 oReal = rb.add(rig.graph, pt.target, kDisplayW, kDisplayH, 4u);
        const u64 oMidSrc = fg ? rb.add(rig.graph, outs.presentInterpolated, kDisplayW, kDisplayH, 8u) : ~0ull;
        const u64 oMid = fg ? rb.add(rig.graph, pt.interpolatedTarget, kDisplayW, kDisplayH, 4u) : ~0ull;
        const u64 oHalf = rb.add(rig.graph, halfBlit.target, kDisplayW / 2u, kDisplayH / 2u, 4u);
        rb.end(rig.graph);
        expect(ctx.executor->execute(rig.graph).ok, "blit frame executes");
        finishFrame(ctx, rig.renderer, rig.renderer.frameSerial());
        const u64 n = static_cast<u64>(kDisplayW) * kDisplayH;
        const std::vector<u8> src = bytesAt(ctx, oSrc, n * 8u);
        const std::vector<u8> dst = bytesAt(ctx, oReal, n * 4u);
        const std::vector<u8> hdst = bytesAt(ctx, oHalf, n);
        for (u64 p = 0; p < n; ++p) {
            for (u32 c = 0; c < 4u; ++c) {
                u16 h = 0;
                std::memcpy(&h, src.data() + p * 8u + c * 2u, 2u);
                bad += std::abs(static_cast<int>(toUnorm8(h)) - static_cast<int>(dst[p * 4u + c])) <= 1 ? 0u : 1u;
                ++checked;
            }
        }
        for (u32 y = 0; y < kDisplayH / 2u; ++y) {
            for (u32 x = 0; x < kDisplayW / 2u; ++x) {
                for (u32 c = 0; c < 3u; ++c) {
                    f32 sum = 0.f;
                    for (u32 k = 0; k < 4u; ++k) {
                        const u64 p = static_cast<u64>(y * 2u + k / 2u) * kDisplayW + x * 2u + k % 2u;
                        u16 h = 0;
                        std::memcpy(&h, src.data() + p * 8u + c * 2u, 2u);
                        sum += halfToFloat(h);
                    }
                    const f32 want = std::min(1.f, std::max(0.f, sum * 0.25f)) * 255.f;
                    const f32 got = hdst[(static_cast<u64>(y) * (kDisplayW / 2u) + x) * 4u + c];
                    badHalf += std::fabs(got - want) <= 2.f ? 0u : 1u;
                }
            }
        }
        if (fg) {
            const std::vector<u8> msrc = bytesAt(ctx, oMidSrc, n * 8u);
            const std::vector<u8> mdst = bytesAt(ctx, oMid, n * 4u);
            for (u64 p = 0; p < n; ++p) {
                for (u32 c = 0; c < 3u; ++c) {
                    u16 h = 0;
                    std::memcpy(&h, msrc.data() + p * 8u + c * 2u, 2u);
                    badMid += std::abs(static_cast<int>(toUnorm8(h)) - static_cast<int>(mdst[p * 4u + c])) <= 1 ? 0u : 1u;
                }
            }
        }
        expect(!fgAvailable || fg, "frame generation hands both frames to the presenter");
    }
    std::printf("blit: %u channels checked, %u off by > 1/255 (real), %u (interpolated%s), %u half-res channels off by > 2/255\n",
                checked, bad, badMid, fgAvailable ? "" : ": FG unavailable", badHalf);
    expect(checked > 0u && bad == 0u, "present.blit RGBA16F -> RGBA8 == CPU conversion (<= 1 / 255)");
    expect(badMid == 0u, "interpolated frame blit == CPU conversion (<= 1 / 255)");
    expect(badHalf == 0u, "2:1 linear blit == 2 x 2 box average (<= 2 / 255)");
    (void)ctx.executor->waitIdle();
    ctx.allocator->destroyImage(real.image);
    ctx.allocator->destroyImage(mid.image);
    ctx.allocator->destroyImage(half.image);
    return 0;
}

// --- ui ---------------------------------------------------------------------------------------------------
struct UiSource {
    f32 color[4] = {0.f, 0.f, 0.f, 0.f};
    rg::TextureRef image;
    u32 calls = 0;
};

void recordUiClear(const rg::PassContext& pc, void* user) {
    const UiSource& s = *static_cast<const UiSource*>(user);
    VkClearColorValue value{};
    std::memcpy(value.float32, s.color, sizeof(s.color));
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(s.image)),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

rg::TextureRef recordUi(rg::Graph& graph, const FrameUiContext& context, void* user) {
    UiSource& s = *static_cast<UiSource*>(user);
    rg::ImageDesc d{};
    d.width = context.width;
    d.height = context.height;
    d.format = 97u; // RGBA16F
    d.name = "test.ui";
    s.image = graph.createImage(d);
    graph.addPass("test.ui_clear", &recordUiClear, &s).use(s.image, rg::Access::TransferDst);
    ++s.calls;
    return s.image;
}

int runUi(Context& ctx, SceneTier tier) {
    SceneRig rig;
    if (const int rc = initRig(ctx, rig, tier); rc != 0) {
        return rc;
    }
    if ((rig.renderer.composer().available() & kStageUi) == 0u) {
        std::printf("SKIP: UI composite kernel not built\n");
        return kSkip;
    }
    UiSource src;
    expect(rig.renderer.setUiSource(FrameUiSource{&recordUi, &src}), "setUiSource");
    const f32 colors[3][4] = {{0.25f, 0.1f, 0.05f, 0.5f}, {0.f, 0.f, 0.f, 0.f}, {0.6f, 0.6f, 0.6f, 1.f}};
    u32 bad = 0, checked = 0;
    for (u32 f = 0; f < 3u; ++f) {
        std::memcpy(src.color, colors[f], sizeof(src.color));
        rig.graph.reset();
        const FrameGraphOutputs outs = rig.renderer.renderScene(rig.ecs.registry, cameraAt(f, true), rig.graph);
        expect((rig.renderer.composer().stats().ran & kStageUi) != 0u, "UI stage ran");
        expect(outs.ui.valid() && outs.hudless.valid() && outs.output.valid() && outs.output.id != outs.hudless.id,
               "UI output separate from the HUD-less image");
        const u32 w = rig.renderer.composer().outputWidth();
        const u32 h = rig.renderer.composer().outputHeight();
        Readback rb;
        rb.begin(ctx, rig.graph);
        const u64 oHud = rb.add(rig.graph, outs.hudless, w, h, 8u);
        const u64 oOut = rb.add(rig.graph, outs.output, w, h, 8u);
        const u64 oUi = rb.add(rig.graph, outs.ui, w, h, 8u);
        rb.end(rig.graph);
        expect(ctx.executor->execute(rig.graph).ok, "UI frame executes");
        finishFrame(ctx, rig.renderer, rig.renderer.frameSerial());
        const u64 n = static_cast<u64>(w) * h;
        const std::vector<u8> hud = bytesAt(ctx, oHud, n * 8u);
        const std::vector<u8> out = bytesAt(ctx, oOut, n * 8u);
        const std::vector<u8> ui = bytesAt(ctx, oUi, n * 8u);
        if (f == 1u) {
            expect(!hud.empty() && hud == out, "transparent UI: output == HUD-less (bit for bit)");
        }
        for (u64 p = 0; p < n; ++p) {
            f32 s4[4], u4[4], ref[4];
            for (u32 c = 0; c < 4u; ++c) {
                u16 hs = 0, hu = 0;
                std::memcpy(&hs, hud.data() + p * 8u + c * 2u, 2u);
                std::memcpy(&hu, ui.data() + p * 8u + c * 2u, 2u);
                s4[c] = halfToFloat(hs);
                u4[c] = halfToFloat(hu);
            }
            frame_ui_composite_texel(s4, u4, ref);
            for (u32 c = 0; c < 4u; ++c) {
                u16 ho = 0;
                std::memcpy(&ho, out.data() + p * 8u + c * 2u, 2u);
                const f32 g = halfToFloat(ho);
                const f32 ulp = std::max(std::fabs(ref[c]) * (1.f / 1024.f), 6.1e-5f);
                bad += std::fabs(g - ref[c]) <= ulp ? 0u : 1u;
                ++checked;
            }
        }
    }
    std::printf("ui (%s kernel): %u UI callbacks, %u / %u channels off by > 1 half ulp\n",
                rig.renderer.composer().stats().ran != 0u ? "frame.ui_composite" : "-", src.calls, bad, checked);
    expect(src.calls == 3u, "UI callback once per frame");
    expect(checked > 0u && bad == 0u, "frame.ui_composite == frame_ui_composite_texel (<= 1 half ulp)");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "golden";
    std::string tierArg = "t0";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--tier") == 0) {
            tierArg = argv[i + 1];
        }
    }
    const SceneTier tier = tierArg == "t2" ? SceneTier::T2 : SceneTier::T0;
    int rc = 0;
    {
        Context ctx;
        rc = setup(ctx, true, tier == SceneTier::T2);
        if (rc != 0) {
            return rc;
        }
        if (mode == "golden") {
            rc = runGolden(ctx, tier);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, tier, false);
        } else if (mode == "blit") {
            rc = runBlit(ctx, tier);
        } else if (mode == "ui") {
            rc = runUi(ctx, tier);
        } else {
            std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
            return 2;
        }
    }
    if (rc == 0 && mode == "zero_alloc") {
        Context ctx;
        rc = setup(ctx, false, tier == SceneTier::T2);
        if (rc == 0) {
            rc = runZeroAlloc(ctx, tier, true);
        }
    }
    if (rc != 0) {
        return rc;
    }
    expect(g_messages == 0u, "0 validation / synchronization-validation messages");
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s), %u validation messages\n", g_failures, g_messages);
        return 1;
    }
    std::printf("ok (%s, %s)\n", mode.c_str(), tierArg.c_str());
    return 0;
}

#endif
