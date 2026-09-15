#include "demo_check.hpp"

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_ai_bt: fuse_ai behavior tree on 2D + 3D agent stubs");

    std::string projectPath = "Samples/unification/demo_ai_bt";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.ai, "project enables fuse_ai");

    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(fuse::ai::BehaviorTree::makePatrolWhenNearTarget());

    fuse::ai::AgentBinding agent2D{};
    agent2D.x = 0.f;
    agent2D.y = 0.f;
    agent2D.targetX = 2.f;
    agent2D.targetY = 0.f;
    runtime.addAgent(agent2D);

    fuse::ai::AgentBinding agent3D{};
    agent3D.x = 0.f;
    agent3D.y = 0.f;
    agent3D.targetX = 12.f;
    agent3D.targetY = 0.f;
    runtime.addAgent(agent3D);

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < 10; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        runtime.buildSnapshots();
        runtime.evaluate(ctx);
        runtime.commit();
    }

    fuse::demo::check(runtime.tickCount() == 10u, "BT ticked each frame");
    fuse::demo::check(runtime.blackboard().flag(0, 0), "near 2D agent sets patrol flag");
    fuse::demo::check(!runtime.blackboard().flag(1, 0), "far 3D agent does not set patrol flag");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_ai_bt");
}
