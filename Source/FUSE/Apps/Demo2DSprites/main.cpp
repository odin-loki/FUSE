#include "demo_check.hpp"
#include "demo_present.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/world2d/world_2d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#endif

#include <cstdlib>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_2d_sprites: 2D dimension path + SpriteToy module bridge");

    const bool presentGameWindow = fuse::demo::present::requestedFromArgv(argc, argv);
    const std::string projectPath =
        fuse::demo::present::projectPathFromArgv(argc, argv, "Samples/unification/demo_2d_sprites");
    const int frameTarget = presentGameWindow ? 3 : 30;

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.dimensions.enable2D, "project enables 2D");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");

    // GPU path when the build has the Vulkan RHI and a device exists; the PlaceholderRenderer otherwise.
    fuse::hybrid::HybridComposer localComposer;
    fuse::hybrid::HybridComposer* composer = &localComposer;
#if defined(FUSE_HAS_VULKAN_RHI)
    fuse::demo::present::HostPresentUnlock presentUnlock;
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
    bootstrapDesc.scene.width = 160;
    bootstrapDesc.scene.height = 120;
    if (presentGameWindow && fuse::demo::present::gameWindowPresentAvailable()) {
        fuse::demo::present::applyGameWindowToBootstrapDesc(bootstrapDesc, 256, 160, "demo_2d_sprites");
    }
    std::unique_ptr<fuse::hybrid::HybridRendererBootstrap> gpuRuntime =
        fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    if (gpuRuntime != nullptr && gpuRuntime->isReady()) {
        composer = &gpuRuntime->composer();
    }
#endif
    fuse::world2d::World2D world2D;
    const fuse::demo::wiring::World2DBridgeResult bridge =
        fuse::demo::wiring::bridge2DWorldFromProject(project, world2D);
    fuse::demo::check(bridge.ok, "SpriteToy-inspired module bridges into World2D");
    fuse::demo::check(bridge.spriteCount >= 2u, "module bridge spawned multiple sprites");
    fuse::demo::check(bridge.animatedSpriteCount >= 1u, "Hero animated-sprite metadata bridged");
    fuse::demo::check(bridge.physicsEnabled, "module bridge enabled Box2D tick path");
    fuse::demo::check(bridge.physicsBodyCount >= 1u, "physics body attached to hero sprite");

    composer->setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
    composer->attachWorld2D(&world2D);

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < frameTarget; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        world2D.tickGameThread(ctx);
#if defined(FUSE_HAS_VULKAN_RHI)
        if (composer != &localComposer) {
            gpuRuntime->runFrame(ctx);
            continue;
        }
#endif
        composer->tick(ctx);
        composer->render(ctx);
    }

    fuse::demo::check(composer->frameCount() == static_cast<fuse::u32>(frameTarget),
                      presentGameWindow ? "present mode frames ticked" : "30 frames ticked");
    fuse::demo::check(world2D.readSnapshot().sprites().size() >= 2u, "2D snapshot retains bridged sprites");
    {
        fuse::frame::FrameCtx animCtx;
        animCtx.time = 1.f;
        world2D.tickGameThread(animCtx);
        bool heroAtlasFrameActive = false;
        for (const fuse::world2d::SpriteDrawCmd& cmd : world2D.readSnapshot().sprites()) {
            if (cmd.atlasFrameCount >= 8u && cmd.atlasFrame > 0u) {
                heroAtlasFrameActive = true;
            }
        }
        fuse::demo::check(heroAtlasFrameActive, "Hero animated-sprite atlas frame drives GPU tint path");
    }
#if defined(FUSE_HAS_VULKAN_RHI)
    if (composer->gpuSceneActive()) {
        fuse::hybrid::HybridSceneRenderer& gpu = *composer->gpuScene();
        fuse::demo::check(gpu.waitIdle() && composer->gpuSceneFrames() == static_cast<fuse::u32>(frameTarget),
                          "GPU frames rendered");
        const fuse::hybrid::OverlayReadbackCheck overlay = fuse::hybrid::checkOverlayReadback(gpu);
        fuse::demo::check(overlay.valid && gpu.spriteCount() >= 2u && overlay.covered > 0u && overlay.mismatched == 0u,
                          "2D sprites visible in the GPU readback (== CPU sprite layer)");
        fuse::demo::check(composer->renderer().pixelCount() == 0u, "PlaceholderRenderer off on the GPU path");
        fuse::log::info("demo_2d_sprites: GPU path (%u sprites, %u covered pixels)", gpu.spriteCount(), overlay.covered);
        if (presentGameWindow && gpuRuntime != nullptr) {
            fuse::demo::check(fuse::demo::present::realPresentCallCount(*gpuRuntime) >= 1u,
                              "vkQueuePresentKHR via GameWindow present path");
        }
    } else
#endif
    {
        fuse::demo::check(composer->renderer().sample(160, 120) > 0, "2D sprite visible");
        fuse::log::info("demo_2d_sprites: PlaceholderRenderer fallback (no Vulkan device)");
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    if (gpuRuntime != nullptr) {
        gpuRuntime->shutdown();
    }
#endif
    fuse::core::shutdown();
    return fuse::demo::finish("demo_2d_sprites");
}
