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
    expectTrue(registry.hasFactory("bb.condition.any_ally_in_radius"), "bb.condition.any_ally_in_radius registered");
    expectTrue(registry.hasFactory("bb.action.allies_count"), "bb.action.allies_count registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_bound"), "bb.guard.blackboard_bound registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_scalar_empty"), "bb.guard.blackboard_scalar_empty registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_flag_empty"), "bb.guard.blackboard_flag_empty registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_empty"), "bb.guard.blackboard_empty registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_agent_valid"), "bb.guard.blackboard_agent_valid registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_flag_set"), "bb.guard.blackboard_flag_set registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_scalar_set"), "bb.guard.blackboard_scalar_set registered");
    expectTrue(registry.hasFactory("bb.guard.spatial_radius_valid"), "bb.guard.spatial_radius_valid registered");
    expectTrue(registry.hasFactory("bb.guard.ally_context"), "bb.guard.ally_context registered");
    expectTrue(registry.registeredTypeIds().size() >= 28u, "registry exposes built-in type ids");
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

void testBlackboardTryGetSetBounds() {
    fuse::ai::Blackboard board;
    board.resize(1);

    bool value = false;
    expectTrue(!board.tryGetFlag(99, 0, value), "tryGetFlag rejects out-of-range agent");
    expectTrue(!board.tryGetFlag(0, fuse::ai::Blackboard::kMaxFlags, value),
               "tryGetFlag rejects out-of-range flag");
    expectTrue(!board.trySetFlag(0, fuse::ai::Blackboard::kMaxFlags, true),
               "trySetFlag rejects out-of-range flag");
    expectTrue(!board.trySetFlag(4, 0, true), "trySetFlag rejects out-of-range agent");

    expectTrue(board.trySetFlag(0, 2, true), "trySetFlag accepts valid slot");
    expectTrue(board.tryGetFlag(0, 2, value) && value, "tryGetFlag reads valid slot");

    fuse::ai::BlackboardView view(board);
    expectTrue(!view.tryGetFlag(4, 0, value), "view tryGetFlag rejects invalid agent");
    expectTrue(view.tryGetFlag(0, 2, value) && value, "view tryGetFlag reads committed slot");
}

void testBlackboardTypedScalars() {
    fuse::ai::Blackboard board;
    board.resize(1);

    expectTrue(board.trySetScalar(0, 1, 3.5f), "trySetScalar accepts valid slot");
    float scalar = 0.f;
    expectTrue(board.tryGetScalar(0, 1, scalar), "tryGetScalar reads valid slot");
    expectTrue(scalar == 3.5f, "scalar value round-trips");

    expectTrue(!board.trySetScalar(0, fuse::ai::Blackboard::kMaxScalars, 1.f),
               "trySetScalar rejects out-of-range slot");
    expectTrue(!board.tryGetScalar(2, 0, scalar), "tryGetScalar rejects out-of-range agent");

    board.clearScalars(0);
    expectTrue(board.scalar(0, 1) == 0.f, "clearScalars resets agent slots");

    fuse::ai::BlackboardView view(board);
    expectTrue(view.scalar(0, 1) == 0.f, "view scalar reads cleared slot");
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

std::vector<fuse::ai::AllyCandidate> makeTestAllies();

fuse::ai::BehaviorTree makeParallelTree(fuse::ai::NodeKind childAKind,
                                      fuse::ai::NodeKind childBKind,
                                      const fuse::ai::ParallelPolicy& policy,
                                      fuse::u32 childAFlag = 0,
                                      fuse::u32 childBFlag = 1) {
    fuse::ai::BehaviorTree tree;
    tree.addNode({childAKind, 0.f, childAFlag, 1, 0, 0});
    tree.addNode({childBKind, 0.f, childBFlag, 1, 0, 0});
    fuse::ai::BehaviorNode parallel;
    parallel.kind = fuse::ai::NodeKind::Parallel;
    parallel.childA = 0;
    parallel.childB = 1;
    parallel.parallelPolicy = policy;
    tree.addNode(std::move(parallel));
    tree.setRoot(2);
    return tree;
}

void testParallelPolicyAllSuccess() {
    const fuse::ai::ParallelPolicy policy;
    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "parallel all-success with default policy");
    expectTrue(result.wroteFlag && result.flagIndex == 1u,
               "parallel all-success prefers later child side effect");
}

void testParallelPolicyThresholdFailTolerance() {
    fuse::ai::ParallelPolicy policy;
    policy.successThreshold = 1;
    policy.failThreshold = 2;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ConditionDistanceLess,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    agent.targetX = 20.f;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "parallel tolerates one failure when fail threshold is 2");
    expectTrue(result.wroteFlag && result.flagIndex == 0u,
               "parallel still returns successful child side effect");
}

void testParallelPolicySuccessThresholdFail() {
    fuse::ai::ParallelPolicy policy;
    policy.successThreshold = 2;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ConditionDistanceLess,
                                                 fuse::ai::NodeKind::ConditionDistanceGreater,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    agent.targetX = 10.f;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "parallel fails when success threshold is not met");
    expectTrue(!result.wroteFlag, "parallel threshold fail has no flag side effects");
}

void testParallelPolicyAbortOnFail() {
    fuse::ai::ParallelPolicy policy;
    policy.abortOnFail = true;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ConditionDistanceLess,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy,
                                                 0,
                                                 3);

    fuse::ai::AgentSnapshot agent;
    agent.targetX = 20.f;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "parallel abort-on-fail returns failure");
    expectTrue(!result.wroteFlag,
               "parallel abort-on-fail skips remaining child side effects");
}

void testParallelPolicyAbortOnSuccess() {
    fuse::ai::ParallelPolicy policy;
    policy.successThreshold = 1;
    policy.abortOnSuccess = true;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy,
                                                 0,
                                                 1);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "parallel abort-on-success returns success when threshold met");
    expectTrue(result.wroteFlag && result.flagIndex == 0u,
               "parallel abort-on-success keeps first child side effect");
}

void testParallelScalarSideEffectMerge() {
    fuse::ai::NodeLoadSpec countSpec;
    countSpec.typeId = "bb.action.allies_count";
    countSpec.threshold = 10.f;
    countSpec.scalarSlot = 0;

    fuse::ai::NodeLoadSpec nearestSpec;
    nearestSpec.typeId = "bb.action.nearest_ally";
    nearestSpec.threshold = 20.f;
    nearestSpec.scalarSlot = 1;

    fuse::ai::NodeLoadSpec parallelSpec;
    parallelSpec.typeId = "bb.parallel";
    parallelSpec.childIndices = {0, 1};

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({countSpec, nearestSpec, parallelSpec}, 2, tree),
               "parallel scalar merge tree loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "parallel scalar merge succeeds when both spatial children succeed");
    expectTrue(result.wroteScalar && result.scalarIndex == 1u,
               "parallel scalar merge prefers later child scalar write");
    expectTrue(result.scalarValue == 1.f, "parallel scalar merge stores nearest ally index");
}

void testParallelPolicyTextLoader() {
    const std::string text = R"(
bb.action.set_flag flag=0
bb.condition.distance_less threshold=5
bb.parallel children=0,1 success=1 fail=2 abort=1
root=2
)";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::loadTreeFromText(text, tree, &error), "parallel policy text loads");
    expectTrue(tree.node(2).parallelPolicy.successThreshold == 1u, "text loader sets success threshold");
    expectTrue(tree.node(2).parallelPolicy.failThreshold == 2u, "text loader sets fail threshold");
    expectTrue(tree.node(2).parallelPolicy.abortOnFail, "text loader sets abort-on-fail");
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

void testSpatialLeavesFailWithoutAlliesContext() {
    const std::vector<fuse::ai::NodeLoadSpec> radiusSpecs = {
        {"bb.condition.allies_in_radius", 10.f, 0, 2, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> nearestSpecs = {
        {"bb.action.nearest_ally", 20.f, 2, 1, {}, {}},
    };

    fuse::ai::BehaviorTree radiusTree;
    fuse::ai::BehaviorTree nearestTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(radiusSpecs, 0, radiusTree), "radius leaf loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(nearestSpecs, 0, nearestTree), "nearest leaf loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult nullAlliesRadius =
        radiusTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(nullAlliesRadius.status == fuse::ai::BehaviorStatus::Failure,
               "allies_in_radius fails when ally context is missing");

    const fuse::ai::BehaviorTickResult nullAlliesNearest =
        nearestTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(nullAlliesNearest.status == fuse::ai::BehaviorStatus::Failure,
               "nearest_ally fails when ally context is missing");
}

void testSpatialLeavesFailWithEmptyAllies() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.allies_in_radius", 10.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "empty-allies radius tree loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies;
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Failure,
               "allies_in_radius fails when ally list is empty");
}

void testEmptyBlackboardViewReadsFalse() {
    fuse::ai::BlackboardView emptyView;
    bool flag = false;
    float scalar = 0.f;
    expectTrue(!emptyView.isBound(), "default blackboard view is not bound");
    expectTrue(!emptyView.tryGetFlag(0, 0, flag), "empty blackboard view rejects flag read");
    expectTrue(!emptyView.tryGetScalar(0, 0, scalar), "empty blackboard view rejects scalar read");
    expectTrue(!emptyView.flag(0, 0), "empty blackboard view flag() returns false");
    expectTrue(emptyView.scalar(0, 0) == 0.f, "empty blackboard view scalar() returns zero");
    expectTrue(emptyView.isScalarEmpty(0, 0), "empty blackboard view treats scalar as empty");
}

void testBlackboardIsEmptyGuard() {
    fuse::ai::Blackboard board;
    expectTrue(board.isEmpty(), "unresized blackboard is empty");

    board.resize(2);
    expectTrue(!board.isEmpty(), "resized blackboard is not empty");
}

void testGuardBlackboardBoundLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.guard.blackboard_bound", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "blackboard bound guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult bound =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(bound.status == fuse::ai::BehaviorStatus::Success,
               "blackboard bound guard succeeds with bound view");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "blackboard bound guard fails with empty view");
}

void testGuardBlackboardScalarEmptyLeaf() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.guard.blackboard_scalar_empty";
    spec.scalarSlot = 1;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "scalar empty guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult emptyScalar =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(emptyScalar.status == fuse::ai::BehaviorStatus::Success,
               "scalar empty guard succeeds when slot is zero");

    board.setScalar(0, 1, 2.f);
    const fuse::ai::BehaviorTickResult setScalar =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(setScalar.status == fuse::ai::BehaviorStatus::Failure,
               "scalar empty guard fails when slot is set");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "scalar empty guard fails with unbound view");
}

void testGuardBlackboardFlagEmptyLeaf() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.guard.blackboard_flag_empty";
    spec.flagIndex = 2;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "flag empty guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult emptyFlag =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(emptyFlag.status == fuse::ai::BehaviorStatus::Success,
               "flag empty guard succeeds when flag is false");

    board.setFlag(0, 2, true);
    const fuse::ai::BehaviorTickResult setFlag =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(setFlag.status == fuse::ai::BehaviorStatus::Failure,
               "flag empty guard fails when flag is set");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "flag empty guard fails with unbound view");
}

void testGuardBlackboardEmptyLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.guard.blackboard_empty", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "blackboard empty guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard emptyBoard;

    const fuse::ai::BehaviorTickResult empty =
        tree.tick(0, agent, fuse::ai::BlackboardView(emptyBoard));
    expectTrue(empty.status == fuse::ai::BehaviorStatus::Success,
               "blackboard empty guard succeeds when agent count is zero");

    fuse::ai::Blackboard board;
    board.resize(1);
    const fuse::ai::BehaviorTickResult populated =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(populated.status == fuse::ai::BehaviorStatus::Failure,
               "blackboard empty guard fails when agents are allocated");
}

void testBlackboardViewAgentAndFlagHelpers() {
    fuse::ai::Blackboard board;
    board.resize(2);
    board.setFlag(1, 0, true);
    board.setScalar(0, 2, 4.f);

    const fuse::ai::BlackboardView view(board);
    expectTrue(view.agentCount() == 2u, "blackboard view reports agent count");
    expectTrue(view.isAgentValid(0), "agent index 0 is valid");
    expectTrue(view.isAgentValid(1), "agent index 1 is valid");
    expectTrue(!view.isAgentValid(2), "agent index 2 is out of range");
    expectTrue(view.isFlagEmpty(0, 0), "unset flag reads as empty");
    expectTrue(!view.isFlagEmpty(1, 0), "set flag is not empty");
    expectTrue(view.isFlagSet(1, 0), "isFlagSet mirrors non-empty flag");
    expectTrue(!view.isFlagSet(0, 0), "isFlagSet false for cleared flag");
    expectTrue(view.isScalarEmpty(0, 1), "unset scalar reads as empty");
    expectTrue(view.isScalarSet(0, 2), "isScalarSet true for non-zero scalar");
    expectTrue(!view.isScalarSet(0, 1), "isScalarSet false for zero scalar");

    const fuse::ai::BlackboardView unbound;
    expectTrue(unbound.agentCount() == 0u, "unbound view reports zero agents");
    expectTrue(!unbound.isAgentValid(0), "unbound view rejects agent index");
    expectTrue(unbound.isFlagEmpty(0, 0), "unbound view treats flag as empty");
    expectTrue(!unbound.isFlagSet(0, 0), "unbound view isFlagSet returns false");
    expectTrue(!unbound.isScalarSet(0, 0), "unbound view isScalarSet returns false");
}

void testGuardBlackboardAgentValidLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.guard.blackboard_agent_valid", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "agent valid guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(2);

    const fuse::ai::BehaviorTickResult valid =
        tree.tick(1, agent, fuse::ai::BlackboardView(board));
    expectTrue(valid.status == fuse::ai::BehaviorStatus::Success,
               "agent valid guard succeeds for in-range agent");

    const fuse::ai::BehaviorTickResult invalid =
        tree.tick(2, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "agent valid guard fails for out-of-range agent");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "agent valid guard fails with unbound view");
}

void testGuardBlackboardFlagSetLeaf() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.guard.blackboard_flag_set";
    spec.flagIndex = 2;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "flag set guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult unset =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(unset.status == fuse::ai::BehaviorStatus::Failure,
               "flag set guard fails when flag is false");

    board.setFlag(0, 2, true);
    const fuse::ai::BehaviorTickResult set =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(set.status == fuse::ai::BehaviorStatus::Success,
               "flag set guard succeeds when flag is true");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "flag set guard fails with unbound view");
}

void testGuardBlackboardScalarSetLeaf() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.guard.blackboard_scalar_set";
    spec.scalarSlot = 1;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "scalar set guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult unset =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(unset.status == fuse::ai::BehaviorStatus::Failure,
               "scalar set guard fails when slot is zero");

    board.setScalar(0, 1, 1.5f);
    const fuse::ai::BehaviorTickResult set =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(set.status == fuse::ai::BehaviorStatus::Success,
               "scalar set guard succeeds when slot is non-zero");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "scalar set guard fails with unbound view");
}

void testGuardSpatialRadiusValidLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> validSpecs = {
        {"bb.guard.spatial_radius_valid", 5.f, 0, 1, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> invalidSpecs = {
        {"bb.guard.spatial_radius_valid", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree validTree;
    fuse::ai::BehaviorTree invalidTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(validSpecs, 0, validTree), "spatial radius guard loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(invalidSpecs, 0, invalidTree), "zero radius guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult valid =
        validTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(valid.status == fuse::ai::BehaviorStatus::Success,
               "spatial radius guard succeeds for positive radius");

    const fuse::ai::BehaviorTickResult invalid =
        invalidTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "spatial radius guard fails for zero radius");
}

void testGuardAllyContextLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.guard.ally_context", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "ally context guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult missing =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(missing.status == fuse::ai::BehaviorStatus::Failure,
               "ally context guard fails without ally list");

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult present =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(present.status == fuse::ai::BehaviorStatus::Success,
               "ally context guard succeeds with non-empty ally list");
}

void testAnyAllyInRadiusConditionLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.any_ally_in_radius", 10.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "any ally in radius condition loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "any ally in radius succeeds when allies are nearby");
}

void testAlliesCountActionLeaf() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.action.allies_count";
    spec.threshold = 10.f;
    spec.flagIndex = 2;
    spec.scalarSlot = 1;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "allies count action loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "allies count action succeeds");
    expectTrue(result.wroteScalar && result.scalarIndex == 1u, "allies count writes scalar slot");
    expectTrue(result.scalarValue == 3.f, "allies count stores three in-radius allies");
    expectTrue(result.wroteFlag && result.flagValue, "allies count sets flag when count > 0");
}

void testParallelRequireBoardGuard() {
    fuse::ai::ParallelPolicy policy;
    policy.requireBoundBlackboard = true;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult bound =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(bound.status == fuse::ai::BehaviorStatus::Success,
               "parallel require-board guard succeeds with bound view");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-board guard fails with empty view");
    expectTrue(!unbound.wroteFlag, "parallel require-board guard skips child side effects");
}

void testParallelRequireValidAgentGuard() {
    fuse::ai::ParallelPolicy policy;
    policy.requireValidAgent = true;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult valid =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(valid.status == fuse::ai::BehaviorStatus::Success,
               "parallel require-agent guard succeeds for valid agent index");

    const fuse::ai::BehaviorTickResult invalid =
        tree.tick(1, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-agent guard fails for out-of-range agent");
    expectTrue(!invalid.wroteFlag, "parallel require-agent guard skips child side effects");
}

void testParallelRequireAlliesGuard() {
    fuse::ai::ParallelPolicy policy;
    policy.requireAllyContext = true;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult missing =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(missing.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-allies guard fails without ally list");

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult present =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(present.status == fuse::ai::BehaviorStatus::Success,
               "parallel require-allies guard succeeds with ally context");
}

void testParallelGuardTextLoader() {
    const std::string text = R"(
bb.action.set_flag flag=0
bb.action.set_flag flag=1
bb.parallel children=0,1 require_board=1 require_allies=1 require_agent=1
root=2
)";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::loadTreeFromText(text, tree, &error), "parallel guard text loads");
    expectTrue(tree.node(2).parallelPolicy.requireBoundBlackboard, "text loader sets require_board");
    expectTrue(tree.node(2).parallelPolicy.requireAllyContext, "text loader sets require_allies");
    expectTrue(tree.node(2).parallelPolicy.requireValidAgent, "text loader sets require_agent");
}

void testNearestAllyWritesScalarSlot() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.action.nearest_ally";
    spec.threshold = 20.f;
    spec.flagIndex = 2;
    spec.scalarSlot = 1;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "nearest ally scalar tree loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success, "nearest ally scalar action succeeds");
    expectTrue(result.wroteScalar, "nearest ally writes scalar slot");
    expectTrue(result.scalarIndex == 1u, "nearest ally targets configured scalar slot");
    expectTrue(result.scalarValue == 1.f, "nearest ally scalar stores ally agent index");
}

void testParallelSpatialChildStatusAggregation() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.allies_in_radius", 10.f, 0, 2, {}, {}},
        {"bb.action.set_flag", 0.f, 3, 1, {}, {}},
        {"bb.parallel", 0.f, 0, 1, {}, {0, 1}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 2, tree), "parallel spatial tree loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult result =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(result.status == fuse::ai::BehaviorStatus::Success,
               "parallel succeeds when spatial condition and action both succeed");
    expectTrue(result.wroteFlag && result.flagIndex == 3u,
               "parallel returns set_flag side effect from second child");
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
    testBlackboardTryGetSetBounds();
    testBlackboardTypedScalars();
    testBlackboardSetGetLeaves();
    testSequenceChildStatusAggregation();
    testSelectorChildStatusAggregation();
    testParallelPolicyAllSuccess();
    testParallelPolicyThresholdFailTolerance();
    testParallelPolicySuccessThresholdFail();
    testParallelPolicyAbortOnFail();
    testParallelPolicyAbortOnSuccess();
    testParallelScalarSideEffectMerge();
    testParallelPolicyTextLoader();
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
    testSpatialLeavesFailWithoutAlliesContext();
    testSpatialLeavesFailWithEmptyAllies();
    testEmptyBlackboardViewReadsFalse();
    testBlackboardIsEmptyGuard();
    testGuardBlackboardBoundLeaf();
    testGuardBlackboardScalarEmptyLeaf();
    testGuardBlackboardFlagEmptyLeaf();
    testGuardBlackboardEmptyLeaf();
    testGuardBlackboardAgentValidLeaf();
    testGuardBlackboardFlagSetLeaf();
    testGuardBlackboardScalarSetLeaf();
    testGuardSpatialRadiusValidLeaf();
    testBlackboardViewAgentAndFlagHelpers();
    testGuardAllyContextLeaf();
    testAnyAllyInRadiusConditionLeaf();
    testAlliesCountActionLeaf();
    testParallelRequireBoardGuard();
    testParallelRequireValidAgentGuard();
    testParallelRequireAlliesGuard();
    testParallelGuardTextLoader();
    testNearestAllyActionLeaf();
    testNearestAllyWritesScalarSlot();
    testNearestAllyActionFailsBeyondRadius();
    testParallelSpatialChildStatusAggregation();
    g_failures += run_spatial_query_tests();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ai_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ai_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
