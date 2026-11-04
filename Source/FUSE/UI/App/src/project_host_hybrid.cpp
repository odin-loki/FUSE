// Engine game side for hybrid projects (see project_host_hybrid.hpp).
#include "fuse/app/project_host_hybrid.hpp"

#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_scene_renderer.hpp>
#include <fuse/hybrid/sprite_layer.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#endif

#include <cstdio>
#include <filesystem>
#include <memory>

namespace fuse::app {

namespace {
struct HybridSide {
    world3d::World3D world3D; // worlds outlive the composer that points at them
    world2d::World2D world2D;
    hybrid::HybridComposer composer;
    frame::FrameCtx ctx;
};

void* create(const project::ProjectManifest& m, std::string& error) {
    auto* g = new HybridSide();
    g->composer.setProjectFlags(project::toDimensionFlags(m.dimensions));
    const std::filesystem::path root = m.projectRoot.empty() ? std::filesystem::path(".") : std::filesystem::path(m.projectRoot);
    std::error_code ec;
    if (m.dimensions.enable3D) {
        if (!m.defaultWorld3D.empty() && std::filesystem::exists(root / m.defaultWorld3D, ec)) {
            const auto r = g->world3D.loadWorldFromFuselevel((root / m.defaultWorld3D).string());
            if (!r.ok) error = "defaultWorld3D: " + r.note; // reported; the empty world still renders
        }
        g->world3D.setPhysicsEnabled(true);
        g->composer.attachWorld3D(&g->world3D);
    }
    if (m.dimensions.enable2D) {
        if (!m.defaultWorld2D.empty() && std::filesystem::exists(root / m.defaultWorld2D, ec))
            (void)g->world2D.loadWorldFromFuselevel((root / m.defaultWorld2D).string());
        g->composer.attachWorld2D(&g->world2D);
    }
    return g;
}

void tick(HybridSide& g, f32 dt) {
    g.ctx.dt = dt;
    g.ctx.time += dt;
    g.composer.tick(g.ctx);
}

const unsigned char* frame(void* side, f32 dt, u32& width, u32& height) {
    auto& g = *static_cast<HybridSide*>(side);
    tick(g, dt);
    g.composer.render(g.ctx);
    ++g.ctx.frameIndex;
    const hybrid::PlaceholderRenderer& fb = g.composer.renderer();
    width = fb.width();
    height = fb.height();
    return width && height ? fb.pixels() : nullptr;
}

void destroy(void* side) { delete static_cast<HybridSide*>(side); }
void* handle(void* side) { return &static_cast<HybridSide*>(side)->composer; }

#if defined(FUSE_HAS_VULKAN_RHI)
// The GPU viewport: a device created at start (the window presenters adopt it) and the scene renderer on it, created
// with the first frame (after the job scheduler is up). The world renders into the renderer's headless RGBA8 target.
struct GpuSide {
    std::unique_ptr<renderer::VulkanInstance> instance;
    std::unique_ptr<renderer::VulkanDevice> device;
    std::unique_ptr<hybrid::HybridSceneRenderer> scene;
    bool failed = false;
};

void* gpuCreate(bool validation, std::string& error) {
    auto gpu = std::make_unique<GpuSide>();
    // surface extensions so a swapchain presenter can adopt the device (skipped by the loader when missing)
    const char* exts[] = {"VK_KHR_surface",
#if defined(_WIN32)
                          "VK_KHR_win32_surface"
#else
                          "VK_KHR_xlib_surface"
#endif
    };
    renderer::VulkanInstanceDesc id{};
    id.appName = "fuse_hybrid_viewport";
    id.enableValidation = validation;
    id.extraExtensions = exts;
    id.extraExtensionCount = 2;
    gpu->instance = renderer::VulkanInstance::create(id);
    if (!gpu->instance || !gpu->instance->isValid()) {
        error = "no Vulkan instance" + (gpu->instance ? ": " + gpu->instance->info().message : std::string());
        return nullptr;
    }
    renderer::VulkanDeviceDesc dd{};
    dd.preferDiscreteGpu = true;
    gpu->device = renderer::VulkanDevice::create(*gpu->instance, dd);
    if (!gpu->device || !gpu->device->isValid()) {
        error = "no Vulkan device" + (gpu->device ? ": " + gpu->device->info().message : std::string());
        return nullptr;
    }
    return gpu.release();
}

VulkanDeviceHandles gpuDevice(void* p) {
    const auto& gpu = *static_cast<GpuSide*>(p);
    return {gpu.device->instanceHandle(), gpu.device->nativePhysicalDevice(), gpu.device->nativeHandle(),
            gpu.device->queues().graphics, gpu.device->queues().graphicsFamily};
}

bool gpuFrame(void* side, void* p, f32 dt, u32 width, u32 height, bool readback, GameSideGpuFrame& out) {
    auto& g = *static_cast<HybridSide*>(side);
    auto& gpu = *static_cast<GpuSide*>(p);
    if (gpu.failed) return false;
    if (!gpu.scene) {
        auto scene = std::make_unique<hybrid::HybridSceneRenderer>();
        hybrid::HybridSceneRendererDesc sd{};
        sd.width = width;
        sd.height = height;
        sd.sdfProxyMeshes = true;
        sd.headlessReadback = true; // the copy is cheap; CPU-presented windows and gates read it
        if (!scene->initialize(*gpu.device, sd)) {
            std::fprintf(stderr, "hybrid GPU viewport: %s (software frames)\n", scene->reason());
            gpu.failed = true;
            return false;
        }
        gpu.scene = std::move(scene);
    }
    hybrid::HybridSceneRenderer& scene = *gpu.scene;
    if (width != scene.width() || height != scene.height()) {
        // a presenter on this device may still sample the old target in a submitted UI pass
        gpu.device->waitIdle();
        if (!scene.resize(width, height)) return false;
    }
    tick(g, dt);
    // the composer's GPU frame without its RhiContext: 2D sprites as overlay quads, then the 3D world
    scene.beginOverlay();
    const hybrid::DimensionFlags& flags = g.composer.projectFlags();
    if (flags.enable2D) {
        for (const world2d::SpriteDrawCmd& cmd : g.world2D.readSnapshot().sprites()) {
            if (!cmd.visible) continue;
            u8 r = 255, gr = 200, b = 64;
            if (cmd.atlasColorFromImageMap) {
                r = cmd.atlasR;
                gr = cmd.atlasG;
                b = cmd.atlasB;
            } else {
                world2d::spriteAtlasPlaybackTint(cmd.atlasFrame, cmd.atlasFrameCount, r, gr, b);
            }
            (void)scene.addSprite(hybrid::spriteQuadFromPlaceholder(cmd.x, cmd.y, cmd.rotation, r, gr, b, width, height,
                                                                    cmd.atlasFrame, cmd.atlasFrameCount));
        }
    }
    bool recorded = false;
    if (flags.enable3D) {
        g.world3D.setRenderer(&scene);
        g.world3D.render(g.ctx);
        recorded = scene.frameRecorded();
    }
    if (!recorded && !scene.renderEmpty(g.ctx)) return false;
    if (!scene.submitHeadless()) return false;
    ++g.ctx.frameIndex;
    const hybrid::HybridSceneRenderer::HeadlessTarget t = scene.headlessTarget();
    out.texture = {t.image, t.view, t.layout};
    out.width = scene.width();
    out.height = scene.height();
    out.pixels = readback && scene.waitIdle() ? scene.readbackPixels() : nullptr;
    return true;
}

const unsigned char* gpuReadback(void* p, u32& width, u32& height) {
    auto& gpu = *static_cast<GpuSide*>(p);
    if (!gpu.scene || gpu.scene->framesSubmitted() == 0 || !gpu.scene->waitIdle()) return nullptr;
    width = gpu.scene->width();
    height = gpu.scene->height();
    return gpu.scene->readbackPixels();
}

void gpuDestroy(void* p) {
    auto* gpu = static_cast<GpuSide*>(p);
    if (gpu->scene) gpu->scene->shutdown();
    gpu->scene.reset();
    gpu->device.reset();
    gpu->instance.reset();
    delete gpu;
}
#endif
} // namespace

GameSideBackend hybridGameSide() {
    GameSideBackend b{&create, &frame, &destroy, &handle};
#if defined(FUSE_HAS_VULKAN_RHI)
    b.gpuCreate = &gpuCreate;
    b.gpuDevice = &gpuDevice;
    b.gpuFrame = &gpuFrame;
    b.gpuDestroy = &gpuDestroy;
    b.gpuReadback = &gpuReadback;
#endif
    return b;
}

} // namespace fuse::app
