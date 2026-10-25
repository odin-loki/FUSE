#include "demo_check.hpp"
#include "demo_present.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/hybrid_timeline_drive.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/cinematics/cue_preview.hpp>
#include <fuse/cinematics/vactor_bridge.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#endif

#include <cstdlib>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_timeline: fuse_cinematics outpost intro asset + hybrid drive");

    const std::string projectPath =
        fuse::demo::present::projectPathFromArgv(argc, argv, "Samples/unification/demo_timeline");

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.cinematics, "project enables fuse_cinematics");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");

    fuse::scene::Scene runtimeScene;
    const fuse::demo::wiring::World3DLoadResult worldLoad =
        fuse::demo::wiring::ensure3DWorldFromProject(project, runtimeScene);
    fuse::demo::check(worldLoad.ok, "verve intro mission converts and loads");

    fuse::cinematics::Timeline timeline;
    std::string assetError;
    if (!fuse::cinematics::load_outpost_intro_30s_from_asset(timeline, &assetError)) {
        timeline = fuse::cinematics::make_outpost_intro_30s_stub();
    }
    timeline.playhead().set_duration_ms(30'000);
    timeline.play();

    fuse::SceneObject3D actor("timeline_actor");
    actor.setPosition(0.f, 0.f);
    actor.setZ(0.f);
    fuse::cinematics::VActorBridge vactorBridge;
    vactorBridge.bind("timeline_actor", &actor);
    vactorBridge.apply_shapebase_mount_chain("timeline_actor", {"vehicle_seat"}, 5.f);

    fuse::cinematics::TimelineMs timelineBefore = 0;
    for (int frame = 0; frame < 60; ++frame) {
        timelineBefore = timeline.playhead().time_ms();
        timeline.advance(500);
        fuse::cinematics::drain_actor_cues(timeline, vactorBridge, timelineBefore);
        vactorBridge.sync_bound_objects();
        vactorBridge.sync_motion_from_timeline(timeline);
    }

    const fuse::cinematics::HybridTimelineSample drive =
        fuse::cinematics::sample_hybrid_timeline_drive(timeline);
    const auto cuePreview = fuse::cinematics::preview_cues_at(timeline, 2'500);

    fuse::demo::check(timeline.groups().size() >= 1u, "timeline groups loaded from asset or stub");
    fuse::demo::check(timeline.playhead().time_ms() > 1'000, "playhead advanced");
    fuse::demo::check(drive.spriteX > 0.f || drive.clearG > 0.f, "hybrid timeline drive sampled");
    fuse::demo::check(vactorBridge.mountCount() >= 1u, "VActor mount cue applied");
    fuse::demo::check(!cuePreview.empty(), "cue preview collected timeline events");

#if defined(FUSE_HAS_VULKAN_RHI)
    const bool presentGameWindow = fuse::demo::present::requestedFromArgv(argc, argv);
    if (presentGameWindow) {
        fuse::demo::present::HostPresentUnlock presentUnlock;
        fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
        bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
        bootstrapDesc.scene.width = 320;
        bootstrapDesc.scene.height = 240;
        if (fuse::demo::present::gameWindowPresentAvailable()) {
            fuse::demo::present::applyGameWindowToBootstrapDesc(bootstrapDesc, 320, 240, "demo_timeline");
        }
        std::unique_ptr<fuse::hybrid::HybridRendererBootstrap> gpuRuntime =
            fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
        fuse::demo::check(gpuRuntime != nullptr && gpuRuntime->isReady(), "present bootstrap ready");
        fuse::hybrid::HybridComposer& composer = gpuRuntime->composer();
        fuse::world3d::World3D world3D;
        world3D.addObject(&actor);
        world3D.setClearColor(drive.clearR, drive.clearG, drive.clearB);
        composer.setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
        composer.attachWorld3D(&world3D);

        fuse::frame::FrameCtx ctx;
        for (int frame = 0; frame < 3; ++frame) {
            ctx.dt = 1.f / 60.f;
            ctx.frameIndex = static_cast<fuse::u32>(frame);
            vactorBridge.sync_bound_objects();
            gpuRuntime->runFrame(ctx);
        }
        fuse::demo::check(fuse::demo::present::realPresentCallCount(*gpuRuntime) >= 1u,
                          "vkQueuePresentKHR via GameWindow present path");
        gpuRuntime->shutdown();
    }
#endif

    fuse::core::shutdown();
    return fuse::demo::finish("demo_timeline");
}
