#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/spatial_query.hpp>
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
    expectTrue(registry.hasFactory("bb.parallel"), "bb.parallel registered");
    expectTrue(registry.hasFactory("bb.inverter"), "bb.inverter registered");
    expectTrue(registry.hasFactory("bb.loop"), "bb.loop registered");
    expectTrue(registry.hasFactory("bb.succeed_always"), "bb.succeed_always registered");
    expectTrue(registry.hasFactory("bb.root"), "bb.root registered");
    expectTrue(registry.hasFactory("bb.condition.distance_less"), "bb.condition.distance_less registered");
    expectTrue(registry.hasFactory("bb.condition.distance_greater"), "bb.condition.distance_greater registered");
    expectTrue(registry.hasFactory("bb.condition.blackboard_get"), "bb.condition.blackboard_get registered");
    expectTrue(registry.hasFactory("bb.action.set_flag"), "bb.action.set_flag registered");
    expectTrue(registry.hasFactory("bb.action.blackboard_set"), "bb.action.blackboard_set registered");
    expectTrue(registry.hasFactory("bb.action.wait"), "bb.action.wait registered");
    expectTrue(registry.hasFactory("bb.action.distance"), "bb.action.distance registered");
    expectTrue(registry.hasFactory("gb.action.move_toward"), "gb.action.move_toward registered");
    expectTrue(registry.hasFactory("bb.condition.allies_in_radius"), "bb.condition.allies_in_radius registered");
    expectTrue(registry.hasFactory("bb.action.nearest_ally"), "bb.action.nearest_ally registered");
    expectTrue(registry.registeredTypeIds().size() >= 17u, "registry exposes built-in type ids");
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

void testWaitLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.wait", 0.f, 0, 3, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "wait leaf loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    std::vector<fuse::u32> waitStarts(tree.nodeCount(), 0);
    fuse::ai::BehaviorEvalContext ctx;
    ctx.waitStartTicks = waitStarts.data();

    for (fuse::u32 tick = 0; tick < 3; ++tick) {
        ctx.tickCount = tick;
        const fuse::ai::BehaviorTickResult result =
            tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
        expectTrue(result.status == fuse::ai::BehaviorStatus::Running, "wait stays running");
    }

    ctx.tickCount = 3;
    const fuse::ai::BehaviorTickResult done =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(done.status == fuse::ai::BehaviorStatus::Success, "wait completes after ticks");
}

void testBlackboardDirectGetSet() {
    fuse::ai::Blackboard board;
    board.resize(2);

    board.setFlag(0, 3, true);
    board.setFlag(1, 1, false);
    expectTrue(board.getFlag(0, 3), "getFlag reads set value");
    expectTrue(!board.getFlag(1, 1), "getFlag reads cleared value");
    expectTrue(!board.getFlag(0, 0), "getFlag returns false for unset slot");

    board.clearFlags(0);
    expectTrue(!board.getFlag(0, 3), "clearFlags resets agent slots");
}

void testBlackboardSetGetLeaves() {
    const std::vector<fuse::ai::NodeLoadSpec> setSpecs = {
        {"bb.action.blackboard_set", 1.f, 2, 1, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> getSpecs = {
        {"bb.condition.blackboard_get", 0.f, 2, 1, {}, {}},
    };

    fuse::ai::BehaviorTree setTree;
    fuse::ai::BehaviorTree getTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(setSpecs, 0, setTree), "blackboard set loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(getSpecs, 0, getTree), "blackboard get loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult setResult =
        setTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(setResult.status == fuse::ai::BehaviorStatus::Success, "blackboard set succeeds");
    expectTrue(setResult.wroteFlag, "blackboard set writes flag");
    expectTrue(setResult.flagIndex == 2u, "blackboard set targets configured flag");
    expectTrue(setResult.flagValue, "blackboard set writes true");

    board.setFlag(0, 2, true);
    const fuse::ai::BehaviorTickResult getResult =
        getTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(getResult.status == fuse::ai::BehaviorStatus::Success, "blackboard get succeeds when flag set");
}

void testDistanceGreaterCondition() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.distance_greater", 5.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "distance greater loads");

    fuse::ai::AgentSnapshot far;
    far.x = 0.f;
    far.y = 0.f;
    far.targetX = 10.f;
    far.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, far, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "distance greater succeeds when far");
}

void testDistanceActionLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.distance", 5.f, 1, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "distance action loads");

    fuse::ai::AgentSnapshot near;
    near.x = 0.f;
    near.y = 0.f;
    near.targetX = 3.f;
    near.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, near, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "distance action succeeds");
    expectTrue(result.wroteFlag, "distance action writes within-range flag");
    expectTrue(result.flagIndex == 1u, "distance action uses configured flag");
    expectTrue(result.flagValue, "distance action marks within threshold");
}

void testWaitViaRuntime() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.wait", 0.f, 0, 2, {}, {}},
        {"bb.action.set_flag", 0.f, 0, 1, {}, {}},
        {"bb.sequence", 0.f, 0, 1, {}, {0, 1}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 2, tree), "wait sequence loads");

    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(tree);
    runtime.addAgent({});

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    fuse::frame::FrameCtx ctx;
    for (fuse::u32 frame = 0; frame < 3; ++frame) {
        ctx.frameIndex = frame;
        runtime.buildSnapshots();
        runtime.evaluate(ctx);
        runtime.commit();
    }

    expectTrue(runtime.blackboard().flag(0, 0), "set_flag runs after wait completes");
    expectTrue(runtime.tickCount() == 3u, "runtime advanced through wait frames");

    scheduler.shutdown();
}

void testSequenceChildStatusAggregation() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
        {"bb.action.set_flag", 0.f, 0, 1, {}, {}},
        {"bb.sequence", 0.f, 0, 1, {}, {0, 1}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 2, tree), "sequence aggregation tree loads");

    fuse::ai::AgentSnapshot far;
    far.x = 0.f;
    far.y = 0.f;
    far.targetX = 20.f;
    far.targetY = 0.f;

    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, far, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "sequence propagates first child failure");
    expectTrue(!result.wroteFlag, "sequence stops before second child on failure");
}

void testSelectorChildStatusAggregation() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
        {"bb.action.set_flag", 0.f, 1, 1, {}, {}},
        {"bb.selector", 0.f, 0, 1, {}, {0, 1}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 2, tree), "selector aggregation tree loads");

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
               "selector falls through to second child success");
    expectTrue(result.wroteFlag && result.flagIndex == 1u,
               "selector returns second child side effects");
}

void testParallelChildStatusAggregation() {
    const std::vector<fuse::ai::NodeLoadSpec> setFlag0 = {
        {"bb.action.set_flag", 0.f, 0, 1, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> setFlag1 = {
        {"bb.action.set_flag", 0.f, 1, 1, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> failCond = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> waitLeaf = {
        {"bb.action.wait", 0.f, 0, 3, {}, {}},
    };

    fuse::ai::BehaviorTree successTree;
    fuse::ai::BehaviorTree failureTree;
    fuse::ai::BehaviorTree runningTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(setFlag0, 0, successTree), "parallel child A loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(setFlag1, 0, successTree), "parallel child B loads");
    successTree.addNode({fuse::ai::NodeKind::Parallel, 0.f, 0, 1, 0, 1});
    successTree.setRoot(2);

    expectTrue(fuse::ai::loadTreeFromSpecs(setFlag0, 0, failureTree), "parallel fail child A loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(failCond, 0, failureTree), "parallel fail child B loads");
    failureTree.addNode({fuse::ai::NodeKind::Parallel, 0.f, 0, 1, 0, 1});
    failureTree.setRoot(2);

    expectTrue(fuse::ai::loadTreeFromSpecs(setFlag0, 0, runningTree), "parallel running child A loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(waitLeaf, 0, runningTree), "parallel running child B loads");
    runningTree.addNode({fuse::ai::NodeKind::Parallel, 0.f, 0, 1, 0, 1});
    runningTree.setRoot(2);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult allSuccess =
        successTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(allSuccess.status == fuse::ai::BehaviorStatus::Success,
               "parallel succeeds when both children succeed");
    expectTrue(allSuccess.wroteFlag && allSuccess.flagIndex == 1u,
               "parallel prefers later child flag write on dual success");

    agent.targetX = 20.f;
    const fuse::ai::BehaviorTickResult anyFailure =
        failureTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(anyFailure.status == fuse::ai::BehaviorStatus::Failure,
               "parallel fails when any child fails");

    agent.targetX = 0.f;
    std::vector<fuse::u32> waitStarts(runningTree.nodeCount(), 0);
    fuse::ai::BehaviorEvalContext ctx;
    ctx.waitStartTicks = waitStarts.data();
    const fuse::ai::BehaviorTickResult anyRunning =
        runningTree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(anyRunning.status == fuse::ai::BehaviorStatus::Running,
               "parallel stays running when any child is running");
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

std::vector<fuse::ai::AllyCandidate> makeTestAllies() {
    return {
        {0, 0.f, 0.f, 1},
        {1, 5.f, 0.f, 1},
        {2, 8.f, 0.f, 1},
        {3, 3.f, 4.f, 1},
    };
}

void testAlliesInRadiusConditionLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.allies_in_radius", 10.f, 0, 2, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "allies in radius condition loads");

    fuse::ai::AgentSnapshot agent;
    agent.x = 0.f;
    agent.y = 0.f;
    agent.teamId = 1;

    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "allies in radius succeeds when minCount met");
}

void testAlliesInRadiusConditionFailsSparse() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.allies_in_radius", 3.f, 0, 2, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "tight radius condition loads");

    fuse::ai::AgentSnapshot agent;
    agent.x = 0.f;
    agent.y = 0.f;
    agent.teamId = 1;

    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "allies in radius fails when not enough allies in tight radius");
}

void testNearestAllyActionLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.nearest_ally", 20.f, 2, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "nearest ally action loads");

    fuse::ai::AgentSnapshot agent;
    agent.x = 0.f;
    agent.y = 0.f;
    agent.teamId = 1;

    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "nearest ally action succeeds");
    expectTrue(result.wroteFlag, "nearest ally action writes flag");
    expectTrue(result.flagIndex == 2u, "nearest ally action targets configured flag");
    expectTrue(result.flagValue, "nearest ally action marks ally found");
}

void testNearestAllyActionFailsBeyondRadius() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.nearest_ally", 4.f, 1, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "nearest ally max-radius action loads");

    fuse::ai::AgentSnapshot agent;
    agent.x = 0.f;
    agent.y = 0.f;
    agent.teamId = 1;

    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "nearest ally action fails when closest ally beyond max radius");
}

void testRuntimeParallelMultiAgentAggregation() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
        {"bb.action.blackboard_set", 1.f, 2, 1, {}, {}},
        {"bb.sequence", 0.f, 0, 1, {}, {0, 1}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 2, tree), "multi-agent patrol tree loads");

    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(tree);

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
    scheduler.initialize(4);

    fuse::frame::FrameCtx ctx;
    ctx.frameIndex = 1;
    runtime.buildSnapshots();
    runtime.evaluate(ctx);
    runtime.commit();

    expectTrue(runtime.lastResults()[0].status == fuse::ai::BehaviorStatus::Success,
               "near agent succeeds in parallel eval");
    expectTrue(runtime.lastResults()[1].status == fuse::ai::BehaviorStatus::Failure,
               "far agent fails in parallel eval");
    expectTrue(runtime.blackboard().getFlag(0, 2), "near agent blackboard flag committed");
    expectTrue(!runtime.blackboard().getFlag(1, 2), "far agent blackboard flag not committed");

    scheduler.shutdown();
}

} // namespace

int run_spatial_query_tests();

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
    testWaitLeaf();
    testBlackboardDirectGetSet();
    testBlackboardSetGetLeaves();
    testSequenceChildStatusAggregation();
    testSelectorChildStatusAggregation();
    testParallelChildStatusAggregation();
    testDistanceGreaterCondition();
    testDistanceActionLeaf();
    testWaitViaRuntime();
    testTextLoader();
    testUaiskTemplateHooks();
    testRuntimeParallelEval();
    testRuntimeParallelMultiAgentAggregation();
    testAlliesInRadiusConditionLeaf();
    testAlliesInRadiusConditionFailsSparse();
    testNearestAllyActionLeaf();
    testNearestAllyActionFailsBeyondRadius();
    g_failures += run_spatial_query_tests();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ai_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ai_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
