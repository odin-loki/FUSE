#include "demo_check.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/fx/afx_mission_hooks.hpp>
#include <fuse/fx/afx_mission_script_vm.hpp>
#include <fuse/fx/afx_template_pack.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/fx/fx_socket.hpp>
#include <fuse/handle.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/object.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_fx: fuse_fx AFX template pack + mission VM + 2D sprite sockets");

    std::string projectPath = "Samples/unification/demo_fx";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.fx, "project enables fuse_fx");
    fuse::demo::check(!project.manifest.defaultWorld2D.empty(), "project declares defaultWorld2D");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");

    fuse::scene::Scene runtimeScene;
    const fuse::demo::wiring::World3DLoadResult missionLoad =
        fuse::demo::wiring::ensure3DWorldFromProject(project, runtimeScene);
    fuse::demo::check(missionLoad.ok, "AFX minimal mission converts and loads");
    fuse::demo::check(missionLoad.materialBindings >= 1u, "AFX mission material refs resolved");

    fuse::world2d::World2D world2D;
    const fuse::demo::wiring::World2DBridgeResult spriteBridge =
        fuse::demo::wiring::bridge2DWorldFromProject(project, world2D);
    fuse::demo::check(spriteBridge.ok, "defaultWorld2D sprite socket world bridges");
    fuse::demo::check(spriteBridge.spriteCount >= 1u, "AFX 2D socket sprites spawned");

    fuse::hybrid::HybridComposer hybridComposer;
    hybridComposer.setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
    hybridComposer.attachWorld2D(&world2D);

    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm missionScriptVm;
    fuse::demo::check(fuse::fx::registerAfxTemplateMissionVm(composer, missionScriptVm),
                      "AFX template mission VM registered");
    fuse::demo::check(missionScriptVm.dispatch("on_ambient_fx", composer), "ambient FX hook dispatched");
    fuse::demo::check(missionScriptVm.dispatch("on_impact_fx", composer), "impact FX hook dispatched");
    fuse::demo::check(missionScriptVm.dispatch("on_spell_cast", composer), "spell cast hook dispatched");

    fuse::fx::FxSocket spriteSocket;
    spriteSocket.kind = fuse::fx::FxSocketKind::Sprite2D;
    spriteSocket.owner = fuse::Handle<fuse::Object>(20u, 1u);
    spriteSocket.effectId = "afx_demo_spark";
    fuse::demo::check(composer.attach(spriteSocket), "AFX spark attached to 2D sprite socket");

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < 5; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        world2D.tickGameThread(ctx);
        hybridComposer.tick(ctx);
        composer.tick(ctx);
        missionScriptVm.dispatchTick(composer, ctx);
    }

    fuse::demo::check(composer.attachmentCount() >= 3u, "FX sockets attached from mission VM + 2D socket");
    fuse::demo::check(composer.effectTimeline().activeCount() >= 1u, "effect timeline active");
    fuse::demo::check(composer.castPipeline().activeCount() >= 1u, "fireball cast active");
    fuse::demo::check(composer.tickCount() == 5u, "FX composer ticked");
    fuse::demo::check(composer.findEffect("afx_demo_spark") != nullptr, "AFX template sample pack registered");
    fuse::demo::check(missionScriptVm.dispatchCount() >= 3u, "AFX mission script VM dispatched hooks");
    fuse::demo::check(composer.particlePoolGpu().syncCount() > 0u, "GPU particle pool synced");
    fuse::demo::check(hybridComposer.frameCount() == 5u, "hybrid composer ticked with 2D FX world");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_fx");
}
