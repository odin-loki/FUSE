// E03 hybrid GPU frame gates (RE-FI-1 / RE-RUNTIME-3D-RENDER / UNI-U4-1): HybridRendererBootstrap renders World3D's ECS
// scene through the E02 SceneRenderer, World2D sprites and the UI layer through "hybrid.sprite_layer" + the UI stage,
// and RhiContext::submitFrame hands the frame to the swapchain image or the headless target.
//
// Scene (demo_3d_empty style): a floor slab and a lit cube (World3D::spawnMesh, builtin cube), a sun and a point light,
// the World3D camera; one World2D sprite over the cube's corner; the UI quad (HybridComposer's HUD marker).
//
//   --mode cpu         (no Vulkan) spriteQuadFromPlaceholder mapping + spriteLayerTexel "over" math.
//   --mode golden      4 headless frames on Lavapipe with VK_LAYER_KHRONOS_validation + synchronization validation:
//                      the GPU path is active (placeholder off, no RGBA writes), every frame went through the scene
//                      source, 2 instances + 2 lights extracted, the UI stage ran; the read-back frame == the CPU sprite
//                      layer where a quad covers (<= 1 / 255), the cube's centre is lit red-dominant, and the frame
//                      matches Hybrid/tests/golden/hybrid_frame_t0.png (mean FLIP <= 0.02, <= 1% pixels over 4 / 255;
//                      FUSE_UPDATE_GOLDENS=1 rewrites it); 0 validation / sync-validation messages.
//   --mode sprite      "hybrid.sprite_layer" alone: 16 rotated, translucent quads -> RGBA16F read back == the CPU
//                      reference (<= 1 half ulp per channel), 0 validation messages.
//   --mode zero_alloc  8 warm-up + 30 measured frames with the cube and the sprite moving: 0 operator-new calls on the
//                      render thread in HybridRendererBootstrap::runFrame (tick + World3D::render -> SceneRenderer +
//                      sprite layer + RhiContext::submitFrame -> rg::Executor::execute + present path).
//   --mode swapchain   Xvfb: a GameWindow presentable (X11 VkSurfaceKHR), the editor-viewport present unlock
//                      (TrackBHostFeature::EditorViewportPresent), 60 frames blitted into acquired swapchain images and
//                      presented with vkQueuePresentKHR; 0 validation messages.
//
// Exit 77 = skip (stub build, no ICD / validation layer / display).
#include <fuse/core/init.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/hybrid/sprite_layer.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/hybrid/hybrid_scene_renderer.hpp>
#include <fuse/hybrid/sprite_layer_gpu.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/instance.hpp>
#if defined(FUSE_HYBRID_TEST_HAS_HARNESS)
#include "golden.hpp"
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

// --- allocation counter (operator new, render thread only) ----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_NOINLINE
#endif

FUSE_TEST_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (t_count) {
        ++t_allocations;
    }
    return std::malloc(size == 0 ? 1 : size);
}
FUSE_TEST_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
FUSE_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using fuse::f32;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace hybrid = fuse::hybrid;
namespace world3d = fuse::world3d;
namespace world2d = fuse::world2d;
namespace ecs = fuse::ecs;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- CPU mode (every build) -------------------------------------------------------------------------------------------
int runCpu() {
    // Placeholder mapping: the 320 x 240 reference extent is the identity (+ centre offset), others scale by h / 240.
    const hybrid::SpriteQuad a = hybrid::spriteQuadFromPlaceholder(10.f, -20.f, 0.f, 255, 128, 0, 320, 240);
    expect(a.center[0] == 170.f && a.center[1] == 100.f && a.halfExtent[0] == 12.5f && a.color[3] == 1.f,
           "spriteQuadFromPlaceholder: 320 x 240 is the placeholder frame");
    const hybrid::SpriteQuad b = hybrid::spriteQuadFromPlaceholder(10.f, -20.f, 0.f, 255, 128, 0, 640, 480);
    expect(b.center[0] == 340.f && b.center[1] == 200.f && b.halfExtent[0] == 25.f, "placeholder quads scale with height");
    f32 out[4];
    hybrid::spriteLayerTexel(&a, 1, 170, 100, out);
    expect(out[0] == 1.f && std::fabs(out[1] - 128.f / 255.f) < 1e-7f && out[3] == 1.f, "opaque quad covers its centre");
    hybrid::spriteLayerTexel(&a, 1, 190, 100, out);
    expect(out[3] == 0.f, "outside the quad: transparent");
    // Premultiplied "over": 50% white over opaque red = (1, 0.5, 0.5, 1).
    hybrid::SpriteQuad q[2];
    q[0] = hybrid::spriteQuadFromPlaceholder(0.f, 0.f, 0.f, 255, 0, 0, 320, 240);
    q[1] = q[0];
    for (f32& c : q[1].color) {
        c = 0.5f;
    }
    hybrid::spriteLayerTexel(q, 2, 160, 120, out);
    expect(out[0] == 1.f && out[1] == 0.5f && out[2] == 0.5f && out[3] == 1.f, "premultiplied over in quad order");
    // Rotation by 45 degrees: the corner direction reaches half * sqrt(2).
    hybrid::SpriteQuad r = hybrid::spriteQuadFromPlaceholder(0.f, 0.f, 0.785398163f, 255, 255, 255, 320, 240);
    hybrid::spriteLayerTexel(&r, 1, 160 + 16, 120, out);
    expect(out[3] == 1.f, "rotated quad covers along its diagonal");
    hybrid::spriteLayerTexel(&r, 1, 160 + 12, 120 + 12, out);
    expect(out[3] == 0.f, "rotated quad leaves its unrotated corner");
    return 0;
}

#if defined(FUSE_HAS_VULKAN_RHI) && defined(FUSE_VULKAN_BACKEND)

namespace rg = fuse::renderer::rg;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr u32 kWidth = 192;
constexpr u32 kHeight = 144;
constexpr f32 kCubeCenter[3] = {0.3f, 0.5f, -2.f};
constexpr f32 kSpriteX = 30.f; ///< placeholder units: over the cube's upper-right corner
constexpr f32 kSpriteY = -25.f;

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

/// Validation on: the layer + synchronization validation through the layer-settings environment.
int configureValidation(bool validation) {
    if (validation) {
        if (!layerAvailable("VK_LAYER_KHRONOS_validation")) {
            std::printf("SKIP: VK_LAYER_KHRONOS_validation not installed (the gate needs synchronization validation)\n");
            return kSkip;
        }
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        setenv("VK_INSTANCE_LAYERS", "", 1);
    }
    fuse::renderer::resetVulkanValidationCounters();
    return 0;
}

u32 g_printed = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL printMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0 &&
        g_printed < 8u) {
        ++g_printed;
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                     data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

/// Prints the first validation messages (the instance's own messenger only counts them).
struct MessagePrinter {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    void attach(hybrid::HybridRendererBootstrap& runtime) {
        fuse::renderer::RhiContext* rhi = runtime.isReady() ? runtime.rendererBootstrap().rhiContext() : nullptr;
        const fuse::renderer::VulkanInstance* vi = rhi != nullptr ? rhi->bootstrap().instance() : nullptr;
        if (vi == nullptr || !vi->isValid()) {
            return;
        }
        instance = static_cast<VkInstance>(vi->nativeHandle());
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create == nullptr) {
            return;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = printMessage;
        (void)create(instance, &info, nullptr, &messenger);
    }
    /// Before the runtime's shutdown (the instance dies with it).
    void detach() {
        if (messenger == VK_NULL_HANDLE) {
            return;
        }
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) {
            destroy(instance, messenger, nullptr);
        }
        messenger = VK_NULL_HANDLE;
    }
};

u32 validationMessages() {
    const fuse::renderer::VulkanValidationCounters c = fuse::renderer::vulkanValidationCounters();
    if (c.errors + c.warnings != 0u) {
        std::fprintf(stderr, "validation: %u errors, %u warnings; last error: %s\n", c.errors, c.warnings,
                     c.lastError.c_str());
    }
    return c.errors + c.warnings;
}

struct Scene {
    world3d::World3D world3D;
    world2d::World2D world2D;
    fuse::SceneObject2D sprite{"hud_sprite"};
    ecs::EntityID cube{};
};

void buildScene(Scene& s) {
    const f32 floorCenter[3] = {0.f, -0.1f, -2.f};
    const f32 floorHalf[3] = {4.f, 0.1f, 4.f};
    const f32 cubeHalf[3] = {0.5f, 0.5f, 0.5f};
    (void)s.world3D.spawnMesh(world3d::BuiltinMesh::Cube, 0u, floorCenter, floorHalf);
    s.cube = s.world3D.spawnMesh(world3d::BuiltinMesh::Cube, 1u, kCubeCenter, cubeHalf);
    const f32 toSun[3] = {0.45f, 0.8f, 0.4f};
    const f32 sunColor[3] = {1.f, 0.95f, 0.85f};
    (void)s.world3D.spawnDirectionalLight(toSun, sunColor, 2.5f);
    const f32 pointPos[3] = {-0.8f, 1.4f, -1.2f};
    const f32 pointColor[3] = {1.f, 0.7f, 0.4f};
    (void)s.world3D.spawnPointLight(pointPos, pointColor, 4.f, 5.f);
    world3d::RenderCamera3D cam{};
    cam.eye[0] = -0.3f;
    cam.eye[1] = 1.6f;
    cam.eye[2] = 2.5f;
    cam.target[0] = 0.2f;
    cam.target[1] = 0.4f;
    cam.target[2] = -2.f;
    cam.fovY = 1.f;
    cam.farPlane = 60.f;
    cam.valid = true;
    s.world3D.setCamera(cam);
    s.sprite.setPosition(kSpriteX, kSpriteY);
    s.world2D.addSprite(&s.sprite);
}

hybrid::HybridRendererBootstrapDesc bootstrapDesc(bool validation) {
    hybrid::HybridRendererBootstrapDesc d{};
    d.renderer.rhi.bootstrap.instance.enableValidation = validation;
    d.renderer.rhi.bootstrap.instance.appName = "fuse_hybrid_frame_vk";
    d.scene.width = kWidth;
    d.scene.height = kHeight;
    d.projectFlags.enable2D = true;
    d.projectFlags.enable3D = true;
    d.projectFlags.enableUI = true;
    return d;
}

/// Skip unless the GPU scene came up; hard failure when a device exists but the scene path did not start.
int requireGpu(hybrid::HybridRendererBootstrap& runtime) {
    fuse::renderer::RhiContext* rhi = runtime.isReady() ? runtime.rendererBootstrap().rhiContext() : nullptr;
    if (rhi == nullptr || !rhi->bootstrap().status().deviceReady) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    if (!runtime.gpuSceneActive()) {
        std::fprintf(stderr, "FAIL: GPU scene not active on a Vulkan device: %s\n", runtime.composer().gpuSceneStatus());
        return 1;
    }
    return 0;
}

void attach(hybrid::HybridRendererBootstrap& runtime, Scene& s) {
    runtime.composer().attachWorld3D(&s.world3D);
    runtime.composer().attachWorld2D(&s.world2D);
}

/// Pixel of a world point (the World3D camera: right-handed look-at, vertical fov, y down in the image).
bool project(const world3d::RenderCamera3D& c, const f32 p[3], u32 w, u32 h, u32& px, u32& py) {
    f32 f[3] = {c.target[0] - c.eye[0], c.target[1] - c.eye[1], c.target[2] - c.eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f32 rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (f32& v : r) {
        v /= rl;
    }
    const f32 u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const f32 d[3] = {p[0] - c.eye[0], p[1] - c.eye[1], p[2] - c.eye[2]};
    const f32 z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (z <= 0.f) {
        return false;
    }
    const f32 x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    const f32 y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    const f32 t = std::tan(c.fovY * 0.5f);
    const f32 aspect = static_cast<f32>(w) / static_cast<f32>(h);
    const f32 ndcX = x / (z * t * aspect);
    const f32 ndcY = y / (z * t);
    px = static_cast<u32>((ndcX * 0.5f + 0.5f) * static_cast<f32>(w));
    py = static_cast<u32>((0.5f - ndcY * 0.5f) * static_cast<f32>(h));
    return px < w && py < h;
}

int runGolden() {
    if (const int rc = configureValidation(true); rc != 0) {
        return rc;
    }
    fuse::core::initialize();
    Scene scene;
    buildScene(scene);
    auto runtime = hybrid::HybridRendererBootstrap::create(bootstrapDesc(true));
    MessagePrinter printer;
    printer.attach(*runtime);
    attach(*runtime, scene);
    constexpr u32 kFrames = 4;
    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 60.f;
    ctx.frameIndex = 1;
    runtime->runFrame(ctx);
    if (const int rc = requireGpu(*runtime); rc != 0) {
        printer.detach();
        runtime->shutdown();
        fuse::core::shutdown();
        return rc;
    }
    for (u32 f = 1; f < kFrames; ++f) {
        ctx.frameIndex = f + 1u;
        ctx.time = static_cast<f32>(f) * ctx.dt;
        runtime->runFrame(ctx);
    }
    hybrid::HybridComposer& composer = runtime->composer();
    hybrid::HybridSceneRenderer& gpu = *runtime->gpuScene();
    expect(gpu.waitIdle(), "GPU idle");
    expect(!composer.softwarePlaceholderEnabled(), "PlaceholderRenderer off by default with a Vulkan device");
    expect(composer.renderer().pixelCount() == 0u, "no software RGBA writes on the GPU path");
    expect(composer.gpuSceneFrames() == kFrames, "every frame went through the scene frame source");
    expect(runtime->rendererBootstrap().rhiContext()->sceneFramesSubmitted() == kFrames,
           "RhiContext::submitFrame routed every frame to the SceneRenderer path");
    expect(scene.world3D.gpuFramesRendered() == kFrames, "World3D::render drove the SceneRenderer every frame");
    const hybrid::HybridSceneFrameStats& st = gpu.lastFrame();
    expect(st.instances == 2u && st.lights == 2u, "2 instances + 2 lights extracted from the World3D registry");
    expect(st.sprites == 1u && st.uiQuads == 1u && st.uiStageRan, "sprite + UI quad composited by the UI stage");
    expect(st.submitted && st.headless, "headless frame submitted");
    expect(composer.lastCommandList().commandCount() > 0u, "RHI command mirror still recorded");

    const u8* px = gpu.readbackPixels();
    if (px == nullptr) {
        expect(false, "headless readback");
    } else {
        // Overlay: where the CPU sprite layer is opaque the frame is the quad colour.
        u32 covered = 0, bad = 0;
        const hybrid::SpriteQuad* quads = gpu.overlayQuads();
        const u32 quadCount = gpu.overlayCount();
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                f32 ref[4];
                hybrid::spriteLayerTexel(quads, quadCount, x, y, ref);
                if (ref[3] < 1.f) {
                    continue;
                }
                ++covered;
                const u8* p = px + (static_cast<usize>(y) * kWidth + x) * 4u;
                for (u32 c = 0; c < 3u; ++c) {
                    const int want = static_cast<int>(std::lround(ref[c] * 255.f));
                    bad += std::abs(want - static_cast<int>(p[c])) <= 1 ? 0u : 1u;
                }
            }
        }
        std::printf("overlay: %u covered pixels, %u channels off by > 1 / 255\n", covered, bad);
        expect(covered > 200u && bad == 0u, "sprite + UI quads over the 3D frame == CPU sprite layer (<= 1 / 255)");
        // The lit cube: its centre pixel is red-dominant (material 1, base (0.8, 0.3, 0.2)).
        u32 cx = 0, cy = 0;
        if (project(scene.world3D.camera(), kCubeCenter, kWidth, kHeight, cx, cy)) {
            const u8* p = px + (static_cast<usize>(cy) * kWidth + cx) * 4u;
            std::printf("cube centre pixel (%u, %u): %u %u %u\n", cx, cy, p[0], p[1], p[2]);
            expect(p[0] > p[1] && p[0] > p[2] && p[0] > 40u, "lit cube visible (red-dominant centre)");
        } else {
            expect(false, "cube centre projects into the frame");
        }
#if defined(FUSE_HYBRID_TEST_HAS_HARNESS)
        fuse::renderer::harness::ImageRgba8 image(kWidth, kHeight);
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const u8* p = px + (static_cast<usize>(y) * kWidth + x) * 4u;
                u8* d = image.at(x, y);
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
                d[3] = 255u;
            }
        }
        fuse::renderer::harness::GoldenSpec spec{};
        spec.metric = fuse::renderer::harness::GoldenMetric::Flip;
        spec.threshold = 0.02;
        spec.psnrFallbackDb = 38.0;
        spec.pixelTolerance = 4;
        spec.maxDifferingPixels = kWidth * kHeight / 100u;
        fuse::renderer::harness::GoldenStore store(FUSE_HYBRID_GOLDEN_DIR, FUSE_HYBRID_ARTIFACT_DIR);
        const fuse::renderer::harness::GoldenResult r = store.check("hybrid_frame_t0", image, spec);
        std::printf("golden hybrid_frame_t0: %s (%s %.4f, threshold %.4f, PSNR %.2f dB, %u pixels over %u / 255)%s\n",
                    r.passed ? "pass" : "FAIL", fuse::renderer::harness::goldenMetricName(r.metricUsed), r.score,
                    r.threshold, r.psnr, r.differingPixels, spec.pixelTolerance, r.updated ? " [golden updated]" : "");
        if (!r.message.empty()) {
            std::printf("  %s\n", r.message.c_str());
        }
        store.printSummary();
        expect(r.passed || r.updated, "hybrid frame golden within tolerance");
#endif
    }
    printer.detach();
    runtime->shutdown();
    expect(validationMessages() == 0u, "0 validation / synchronization-validation messages");
    fuse::core::shutdown();
    return 0;
}

f32 halfToFloat(u16 h) { return fuse::renderer::GBufferQuantize::halfToFloat(h); }

struct SpriteReadback {
    rg::TextureRef src;
    rg::BufferRef dst;
    u32 width = 0;
    u32 height = 0;
};

void recordSpriteCopy(const rg::PassContext& pc, void* user) {
    const SpriteReadback& c = *static_cast<const SpriteReadback*>(user);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(c.src)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

int runSprite() {
    if (const int rc = configureValidation(true); rc != 0) {
        return rc;
    }
    fuse::core::initialize();
    hybrid::HybridRendererBootstrapDesc desc = bootstrapDesc(true);
    auto runtime = hybrid::HybridRendererBootstrap::create(desc);
    MessagePrinter printer;
    printer.attach(*runtime);
    fuse::frame::FrameCtx ctx{};
    runtime->runFrame(ctx); // creates the GPU scene
    if (const int rc = requireGpu(*runtime); rc != 0) {
        printer.detach();
        runtime->shutdown();
        fuse::core::shutdown();
        return rc;
    }
    hybrid::HybridSceneRenderer& gpu = *runtime->gpuScene();
    constexpr u32 kW = 96;
    constexpr u32 kH = 64;
    constexpr u32 kQuads = 16;
    hybrid::SpriteQuad quads[kQuads];
    u32 seed = 12345u;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<f32>(seed >> 8) / 16777216.f;
    };
    for (hybrid::SpriteQuad& q : quads) {
        q.center[0] = rnd() * kW;
        q.center[1] = rnd() * kH;
        q.halfExtent[0] = 4.f + rnd() * 18.f;
        q.halfExtent[1] = 3.f + rnd() * 12.f;
        const f32 angle = rnd() * 6.2831853f;
        q.cosSin[0] = std::cos(angle);
        q.cosSin[1] = std::sin(angle);
        const f32 a = 0.3f + rnd() * 0.7f;
        q.color[0] = rnd() * a;
        q.color[1] = rnd() * a;
        q.color[2] = rnd() * a;
        q.color[3] = a;
    }
    hybrid::SpriteLayerGpu layer;
    hybrid::SpriteLayerGpuDesc ld{};
    ld.device = gpu.device();
    ld.allocator = gpu.allocator();
    ld.maxQuads = kQuads;
    expect(layer.init(ld), "SpriteLayerGpu::init");
    fuse::renderer::Buffer readback{};
    fuse::renderer::BufferDesc bd{};
    bd.size = static_cast<usize>(kW) * kH * 8u;
    bd.usage = fuse::renderer::BufferUsage::TransferDst;
    bd.memoryUsage = fuse::renderer::MemoryUsage::GpuToCpu;
    bd.name = "hybrid_frame_vk.sprite_readback";
    expect(gpu.allocator()->createBuffer(bd, readback) && readback.mapped != nullptr, "readback buffer");
    u32 bad = 0, checked = 0;
    if (layer.valid() && readback.mapped != nullptr) {
        rg::Graph graph;
        layer.beginFrame(1);
        SpriteReadback copy{};
        copy.src = layer.addPass(graph, quads, kQuads, kW, kH);
        copy.dst = graph.importBuffer(rg::ImportedBuffer{readback.handle, bd.size, rg::kNoQueue, nullptr, "readback"});
        copy.width = kW;
        copy.height = kH;
        graph.addPass("test.sprite_copy", &recordSpriteCopy, &copy)
            .use(copy.src, rg::Access::TransferSrc)
            .use(copy.dst, rg::Access::TransferDst);
        graph.addPass("test.host", nullptr, nullptr).use(copy.dst, rg::Access::HostRead);
        expect(gpu.executor()->execute(graph).ok, "sprite layer graph executes");
        expect(gpu.executor()->waitIdle(), "sprite layer retired");
        const u8* data = static_cast<const u8*>(readback.mapped);
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                f32 ref[4];
                hybrid::spriteLayerTexel(quads, kQuads, x, y, ref);
                for (u32 c = 0; c < 4u; ++c) {
                    u16 h = 0;
                    std::memcpy(&h, data + (static_cast<usize>(y) * kW + x) * 8u + c * 2u, 2u);
                    const f32 got = halfToFloat(h);
                    // <= 1 half ulp: half has 10 mantissa bits.
                    const f32 ulp = (std::max)(std::ldexp(1.f, std::ilogb((std::max)(std::fabs(ref[c]), 6.1e-5f)) - 10), 6e-8f);
                    bad += std::fabs(got - ref[c]) <= ulp ? 0u : 1u;
                    ++checked;
                }
            }
        }
    }
    std::printf("sprite layer: %u channels checked, %u off by > 1 half ulp\n", checked, bad);
    expect(checked == kW * kH * 4u && bad == 0u, "hybrid.sprite_layer == spriteLayerTexel (<= 1 half ulp)");
    gpu.allocator()->destroyBuffer(readback);
    layer.destroy();
    printer.detach();
    runtime->shutdown();
    expect(validationMessages() == 0u, "0 validation / synchronization-validation messages");
    fuse::core::shutdown();
    return 0;
}

void moveCube(Scene& s, u32 frame) {
    ecs::Transform* t = s.world3D.registry().get<ecs::Transform>(s.cube);
    if (t != nullptr) {
        t->local_to_world.data[12] = kCubeCenter[0] + 0.02f * static_cast<f32>(frame % 20u);
    }
    s.sprite.setPosition(kSpriteX + static_cast<f32>(frame % 10u), kSpriteY);
}

int runZeroAlloc() {
    if (const int rc = configureValidation(false); rc != 0) {
        return rc;
    }
    fuse::core::initialize();
    Scene scene;
    buildScene(scene);
    auto runtime = hybrid::HybridRendererBootstrap::create(bootstrapDesc(false));
    attach(*runtime, scene);
    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 60.f;
    runtime->runFrame(ctx);
    if (const int rc = requireGpu(*runtime); rc != 0) {
        runtime->shutdown();
        fuse::core::shutdown();
        return rc;
    }
    constexpr u32 kWarm = 8;
    constexpr u32 kMeasured = 30;
    unsigned long long worst = 0, total = 0;
    for (u32 f = 1; f < kWarm + kMeasured; ++f) {
        moveCube(scene, f);
        ctx.frameIndex = f;
        const bool measure = f >= kWarm;
        t_allocations = 0;
        t_count = measure;
        runtime->runFrame(ctx);
        t_count = false;
        if (measure) {
            total += t_allocations;
            worst = (std::max)(worst, t_allocations);
        }
    }
    expect(runtime->composer().gpuSceneFrames() == kWarm + kMeasured, "every frame on the GPU scene path");
    std::printf("zero_alloc: %u measured frames, %llu operator-new calls (worst frame %llu)\n", kMeasured, total, worst);
    expect(total == 0u, "hybrid GPU frame (tick + 3D + sprites + UI + submit + present) makes no steady-state heap allocations");
    runtime->shutdown();
    fuse::core::shutdown();
    return 0;
}

int runSwapchain() {
#if !defined(FUSE_PLATFORM_WINDOW_X11)
    std::printf("SKIP: X11 window backend not built\n");
    return kSkip;
#else
    if (!fuse::platform::displayServerAvailable()) {
        std::printf("SKIP: no display (run under xvfb-run)\n");
        return kSkip;
    }
    if (const int rc = configureValidation(true); rc != 0) {
        return rc;
    }
    fuse::core::initialize();
    // The editor viewport's present unlock (runtime_viewport.cpp sets it once its surface is real): the game runtime
    // keeps vkQueuePresentKHR behind the compile-time Track B unlock.
    fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, true);
    Scene scene;
    buildScene(scene);
    hybrid::HybridRendererBootstrapDesc desc = bootstrapDesc(true);
    desc.presentable.backend = hybrid::PresentableBackend::GameWindow;
    desc.presentable.window.title = "fuse_hybrid_frame_vk";
    desc.presentable.window.width = 256;
    desc.presentable.window.height = 160;
    desc.presentable.swapchainWidth = 256;
    desc.presentable.swapchainHeight = 160;
    desc.presentable.vsyncMode = fuse::renderer::VsyncMode::Fifo;
    desc.scene.width = 0; // follow the swapchain
    desc.scene.height = 0;
    auto runtime = hybrid::HybridRendererBootstrap::create(desc);
    MessagePrinter printer;
    printer.attach(*runtime);
    int rc = 0;
    const fuse::renderer::VulkanSwapchain* swapchain =
        runtime->isReady() ? runtime->rendererBootstrap().rhiContext()->bootstrap().swapchain() : nullptr;
    if (swapchain == nullptr || swapchain->isHeadless() || !swapchain->hasImages()) {
        std::printf("SKIP: no presentable X11 swapchain (%s)\n",
                    runtime->presentable() != nullptr ? runtime->presentable()->status().message.c_str() : "no presentable");
        rc = kSkip;
    }
    if (rc == 0) {
        attach(*runtime, scene);
        fuse::frame::FrameCtx ctx{};
        ctx.dt = 1.f / 60.f;
        constexpr u32 kFrames = 60;
        for (u32 f = 0; f < kFrames; ++f) {
            moveCube(scene, f);
            ctx.frameIndex = f + 1u;
            runtime->runFrame(ctx);
        }
        rc = requireGpu(*runtime);
        if (rc == 0) {
            const fuse::renderer::PresentPathStatus& ps = runtime->presentPath()->status();
            std::printf("swapchain %ux%u: %u scene frames, %llu presented, %u vkQueuePresentKHR, %llu images acquired\n",
                        swapchain->info().width, swapchain->info().height,
                        static_cast<unsigned>(runtime->composer().gpuSceneFrames()),
                        static_cast<unsigned long long>(ps.presentedFrames), ps.realPresentCallCount,
                        static_cast<unsigned long long>(ps.acquiredImageCount));
            expect(runtime->composer().gpuSceneFrames() == kFrames, "60 frames on the GPU scene path");
            expect(runtime->gpuScene()->width() == swapchain->info().width &&
                       runtime->gpuScene()->height() == swapchain->info().height,
                   "scene renderer sized to the swapchain");
            expect(ps.acquiredImageCount == kFrames && ps.realPresentCallCount == kFrames,
                   "every frame acquired a swapchain image and presented it (vkQueuePresentKHR)");
            expect(runtime->gpuScene()->lastFrame().presentedToSwapchain, "SceneRenderer output blitted into the swapchain image");
        }
    }
    printer.detach();
    runtime->shutdown();
    fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, false);
    if (rc == 0) {
        expect(validationMessages() == 0u, "0 validation / synchronization-validation messages");
    }
    fuse::core::shutdown();
    return rc;
#endif
}

#endif // FUSE_HAS_VULKAN_RHI && FUSE_VULKAN_BACKEND

} // namespace

int main(int argc, char** argv) {
    std::string mode = "cpu";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        }
    }
    int rc = 0;
    if (mode == "cpu") {
        rc = runCpu();
    } else {
#if defined(FUSE_HAS_VULKAN_RHI) && defined(FUSE_VULKAN_BACKEND)
        if (mode == "golden") {
            rc = runGolden();
        } else if (mode == "sprite") {
            rc = runSprite();
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc();
        } else if (mode == "swapchain") {
            rc = runSwapchain();
        } else {
            std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
            return 2;
        }
#else
        std::printf("SKIP: stub build (no Vulkan backend)\n");
        return kSkip;
#endif
    }
    if (rc != 0) {
        return rc;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok (%s)\n", mode.c_str());
    return 0;
}
