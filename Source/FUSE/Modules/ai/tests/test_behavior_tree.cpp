#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/uaisk_template_hooks.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testPatrolTreeNearTarget() {
    fuse::ai::BehaviorTree tree = fuse::ai::BehaviorTree::makePatrolWhenNearTarget();
    fuse::ai::Blackboard board;
    board.resize(1);

    fuse::ai::AgentSnapshot near;
    near.x = 0.f;
    near.y = 0.f;
    near.targetX = 3.f;
    near.targetY = 0.f;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, near, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "near agent succeeds");
    expectTrue(result.wroteFlag, "near agent sets patrol flag");
}

void testPatrolTreeFarTarget() {
    fuse::ai::BehaviorTree tree = fuse::ai::BehaviorTree::makePatrolWhenNearTarget();
    fuse::ai::Blackboard board;
    board.resize(1);

    fuse::ai::AgentSnapshot far;
    far.x = 0.f;
    far.y = 0.f;
    far.targetX = 10.f;
    far.targetY = 0.f;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, far, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure, "far agent fails sequence");
    expectTrue(!result.wroteFlag, "far agent does not set patrol flag");
}

void testRegistryPatrolParity() {
    const fuse::ai::BehaviorTree manual = fuse::ai::BehaviorTree::makePatrolWhenNearTarget();
    const fuse::ai::BehaviorTree loaded = fuse::ai::BehaviorTree::makePatrolWhenNearTargetFromRegistry();

    expectTrue(manual.nodeCount() == loaded.nodeCount(), "registry tree node count matches manual");
    expectTrue(manual.rootIndex() == loaded.rootIndex(), "registry tree root matches manual");

    fuse::ai::AgentSnapshot near;
    near.x = 0.f;
    near.y = 0.f;
    near.targetX = 2.f;
    near.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult manualResult =
        manual.tick(0, near, fuse::ai::BlackboardView(board));
    const fuse::ai::BehaviorTickResult loadedResult =
        loaded.tick(0, near, fuse::ai::BlackboardView(board));

    expectTrue(manualResult.status == loadedResult.status, "registry patrol status parity");
    expectTrue(manualResult.wroteFlag == loadedResult.wroteFlag, "registry patrol flag parity");
}

void testRegistryBuiltinNodes() {
    fuse::ai::NodeRegistry& registry = fuse::ai::NodeRegistry::instance();
    expectTrue(registry.hasFactory("bb.sequence"), "bb.sequence registered");
    expectTrue(registry.hasFactory("bb.selector"), "bb.selector registered");
    expectTrue(registry.hasFactory("bb.inverter"), "bb.inverter registered");
    expectTrue(registry.hasFactory("bb.loop"), "bb.loop registered");
    expectTrue(registry.hasFactory("bb.succeed_always"), "bb.succeed_always registered");
    expectTrue(registry.hasFactory("bb.root"), "bb.root registered");
    expectTrue(registry.hasFactory("gb.action.move_toward"), "gb.action.move_toward registered");
    expectTrue(registry.registeredTypeIds().size() >= 9u, "registry exposes built-in type ids");
}

void testInverterDecorator() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
        {"bb.inverter", 0.f, 0, 1, {}, {0}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 1, tree), "inverter tree loads");

    fuse::ai::AgentSnapshot near;
    near.x = 0.f;
    near.y = 0.f;
    near.targetX = 1.f;
    near.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, near, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "inverter flips near distance success to failure");
}

void testLoopDecorator() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.set_flag", 0.f, 0, 1, {}, {}},
        {"bb.loop", 0.f, 0, 3, {}, {0}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 1, tree), "loop tree loads");
    expectTrue(tree.node(1).loopCount == 3u, "loop count preserved from spec");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "loop completes");
    expectTrue(result.wroteFlag, "loop child sets flag");
}

void testSucceedAlwaysDecorator() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
        {"bb.succeed_always", 0.f, 0, 1, {}, {0}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 1, tree), "succeed_always tree loads");

    fuse::ai::AgentSnapshot far;
    far.x = 0.f;
    far.y = 0.f;
    far.targetX = 20.f;
    far.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, far, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "succeed_always forces success on failing child");
}

void testGuideBotMoveTowardLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"gb.action.move_toward", 1.f, 2, 1, {"aiMovement.cs"}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "move toward leaf loads");
    expectTrue(tree.node(0).scriptHook == "aiMovement.cs", "UAISK hook preserved on leaf");

    fuse::ai::AgentSnapshot agent;
    agent.x = 0.f;
    agent.y = 0.f;
    agent.targetX = 5.f;
    agent.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(4);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "move toward succeeds when far");
    expectTrue(result.flagIndex == 2u, "move toward writes configured flag");
}

void testTextLoader() {
    const std::string text = R"(
# patrol via text loader
bb.condition.distance_less threshold=5
bb.action.set_flag flag=0 hook=aiActions.cs
bb.sequence children=0,1
root=2
)";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::loadTreeFromText(text, tree, &error), "text loader succeeds");
    expectTrue(tree.nodeCount() == 3u, "text loader parsed three nodes");

    fuse::ai::AgentSnapshot near;
    near.x = 0.f;
    near.y = 0.f;
    near.targetX = 4.f;
    near.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, near, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "text-loaded patrol succeeds near");
}

void testUaiskTemplateHooks() {
    expectTrue(fuse::ai::uaisk::templateHookCount >= 4u, "UAISK template hooks documented");
    expectTrue(fuse::ai::uaisk::kTemplateHooks[0].fuseRegistryTypeId == std::string_view("bb.selector"),
               "UAISK aiBehaviors maps to selector");
}

void testRuntimeParallelEval() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(fuse::ai::BehaviorTree::makePatrolWhenNearTargetFromRegistry());

    fuse::ai::AgentBinding near{};
    near.x = 0.f;
    near.y = 0.f;
    near.targetX = 2.f;
    near.targetY = 0.f;

    fuse::ai::AgentBinding far{};
    far.x = 0.f;
    far.y = 0.f;
    far.targetX = 12.f;
    far.targetY = 0.f;

    runtime.addAgent(near);
    runtime.addAgent(far);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    fuse::frame::FrameCtx ctx;
    ctx.frameIndex = 1;
    runtime.buildSnapshots();
    runtime.evaluate(ctx);
    runtime.commit();

    expectTrue(runtime.blackboard().flag(0, 0), "near agent patrol flag committed");
    expectTrue(!runtime.blackboard().flag(1, 0), "far agent patrol flag not committed");
    expectTrue(runtime.tickCount() == 1u, "runtime tick count advanced");

    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testPatrolTreeNearTarget();
    testPatrolTreeFarTarget();
    testRegistryPatrolParity();
    testRegistryBuiltinNodes();
    testInverterDecorator();
    testLoopDecorator();
    testSucceedAlwaysDecorator();
    testGuideBotMoveTowardLeaf();
    testTextLoader();
    testUaiskTemplateHooks();
    testRuntimeParallelEval();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ai_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ai_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
