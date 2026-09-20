#include "demo_check.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/uaisk_script_import.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_ai_bt: fuse_ai BT on hybrid 2D/3D agents + UAISK profile");

    std::string projectPath = "Samples/unification/demo_ai_bt";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.ai, "project enables fuse_ai");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");

    fuse::scene::Scene runtimeScene;
    const fuse::demo::wiring::World3DLoadResult missionLoad =
        fuse::demo::wiring::ensure3DWorldFromProject(project, runtimeScene);
    fuse::demo::check(missionLoad.ok, "BehaviorTestbed.mis converts and loads");

    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3DRuntime;
    fuse::SceneObject3D patrolObject3D("patrol_agent");
    patrolObject3D.setPosition(-2.f, 0.f);
    patrolObject3D.setZ(0.f);
    world3DRuntime.addObject(&patrolObject3D);

    const fuse::demo::wiring::World2DBridgeResult spriteBridge =
        fuse::demo::wiring::bridge2DWorldFromProject(project, world2D);
    fuse::demo::check(spriteBridge.ok, "sprite agent module bridges into World2D");

    composer.setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3DRuntime);

    fuse::ai::BehaviorRuntime aiRuntime;
    aiRuntime.registerTreeProfile(0, fuse::ai::BehaviorTree::makePatrolWhenNearTarget());
    fuse::ai::uaisk::registerPatrolSquadProfile(aiRuntime);

    fuse::ai::AgentBinding agent2D{};
    agent2D.x = 0.f;
    agent2D.y = 0.f;
    agent2D.targetX = 2.f;
    agent2D.targetY = 0.f;
    agent2D.treeProfileId = 0;
    aiRuntime.addAgent(agent2D);

    fuse::ai::AgentBinding agent3D{};
    agent3D.x = -2.f;
    agent3D.y = 0.f;
    agent3D.targetX = 12.f;
    agent3D.targetY = 0.f;
    agent3D.treeProfileId = 1;
    aiRuntime.addAgent(agent3D);

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < 10; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        aiRuntime.buildSnapshots();
        aiRuntime.evaluate(ctx);
        aiRuntime.commit();
        composer.tick(ctx);
    }

    fuse::demo::check(aiRuntime.tickCount() == 10u, "BT ticked each frame");
    fuse::demo::check(aiRuntime.treeProfileCount() >= 2u, "per-agent tree profiles registered");
    fuse::demo::check(aiRuntime.blackboard().flag(0, 0), "near 2D agent sets patrol flag");
    fuse::demo::check(!aiRuntime.blackboard().flag(1, 0), "far 3D agent does not set patrol flag");
    fuse::demo::check(missionLoad.entityCount >= 2u, "converted behavior testbed has patrol agents");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_ai_bt");
}
