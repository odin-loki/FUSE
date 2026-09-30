#include "demo_check.hpp"
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

    std::string projectPath = "Samples/unification/demo_2d_sprites";
    if (argc > 1) {
        projectPath = argv[1];
    }

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
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
    bootstrapDesc.scene.width = 160;
    bootstrapDesc.scene.height = 120;
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
    for (int frame = 0; frame < 30; ++frame) {
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

    fuse::demo::check(composer->frameCount() == 30u, "30 frames ticked");
    fuse::demo::check(world2D.readSnapshot().sprites().size() >= 2u, "2D snapshot retains bridged sprites");
#if defined(FUSE_HAS_VULKAN_RHI)
    if (composer->gpuSceneActive()) {
        fuse::hybrid::HybridSceneRenderer& gpu = *composer->gpuScene();
        fuse::demo::check(gpu.waitIdle() && composer->gpuSceneFrames() == 30u, "30 GPU frames rendered");
        const fuse::hybrid::OverlayReadbackCheck overlay = fuse::hybrid::checkOverlayReadback(gpu);
        fuse::demo::check(overlay.valid && gpu.spriteCount() >= 2u && overlay.covered > 0u && overlay.mismatched == 0u,
                          "2D sprites visible in the GPU readback (== CPU sprite layer)");
        fuse::demo::check(composer->renderer().pixelCount() == 0u, "PlaceholderRenderer off on the GPU path");
        fuse::log::info("demo_2d_sprites: GPU path (%u sprites, %u covered pixels)", gpu.spriteCount(), overlay.covered);
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
