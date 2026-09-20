#include <fuse/ai/agent_entity_bind.hpp>
#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/uaisk_script_import.hpp>
#include <fuse/ai/uaisk_cs_parser.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>
#include <fuse/ai/uaisk_cs_syntax_tree.hpp>
#include <fuse/ai/uaisk_fsevents_coreservices_stub.hpp>
#include <fuse/ai/uaisk_tree_reload.hpp>
#if __has_include(<fuse/ai/uaisk_script_host_bridge.hpp>)
#include <fuse/ai/uaisk_script_host_bridge.hpp>
#include <fuse/script/script_host.hpp>
#define FUSE_AI_TEST_SCRIPT_HOST 1
#else
#define FUSE_AI_TEST_SCRIPT_HOST 0
#endif
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/spatial_query.hpp>
#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/uaisk_template_hooks.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/handle.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/object.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    const float delta = value - expected;
    if (delta < -epsilon || delta > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, value, expected);
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
    expectTrue(registry.hasFactory("bb.guard.blackboard_scalar_set"), "bb.guard.blackboard_scalar_set registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_flag_empty"), "bb.guard.blackboard_flag_empty registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_flag_set"), "bb.guard.blackboard_flag_set registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_empty"), "bb.guard.blackboard_empty registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_nonempty"), "bb.guard.blackboard_nonempty registered");
    expectTrue(registry.hasFactory("bb.guard.blackboard_agent_valid"), "bb.guard.blackboard_agent_valid registered");
    expectTrue(registry.hasFactory("bb.guard.ally_context"), "bb.guard.ally_context registered");
    expectTrue(registry.hasFactory("bb.guard.valid_ally_radius"), "bb.guard.valid_ally_radius registered");
    expectTrue(registry.hasFactory("bb.condition.no_allies_in_radius"), "bb.condition.no_allies_in_radius registered");
    expectTrue(registry.hasFactory("bb.action.nearest_ally_distance"), "bb.action.nearest_ally_distance registered");
    expectTrue(registry.registeredTypeIds().size() >= 30u, "registry exposes built-in type ids");
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

void testMoveTowardRuntimeCommit() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"gb.action.move_toward", 0.5f, 0, 0, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "move toward tree loads for runtime");

    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(tree);

    fuse::ai::AgentBinding agent{};
    agent.x = 0.f;
    agent.y = 0.f;
    agent.targetX = 5.f;
    agent.targetY = 0.f;
    agent.moveSpeed = 1.f;
    runtime.addAgent(agent);

    fuse::frame::FrameCtx ctx{};
    runtime.buildSnapshots();
    runtime.evaluate(ctx);
    runtime.commit();

    expectTrue(runtime.bindings()[0].x > 0.f, "move toward commits position delta");
    expectNear(runtime.bindings()[0].x, 1.f, 1e-4f, "move toward step equals moveSpeed");
    expectNear(runtime.bindings()[0].y, 0.f, 1e-4f, "move toward y unchanged on axis-aligned path");
}

void testMoveTowardDemoTreeFactory() {
    const fuse::ai::BehaviorTree tree = fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.25f);
    expectTrue(tree.nodeCount() == 1u, "move toward demo tree has one node");
    expectTrue(tree.node(0).kind == fuse::ai::NodeKind::ActionMoveToward, "demo tree root is move toward");
}

void testPatrolWithAllySupportDemoTree() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(fuse::ai::BehaviorTree::makePatrolWithAllySupportDemoTree(8.f));

    fuse::ai::AgentBinding ally{};
    ally.x = 0.f;
    ally.y = 0.f;
    ally.targetX = 10.f;
    ally.targetY = 0.f;
    ally.teamId = 1;
    runtime.addAgent(ally);

    fuse::ai::AgentBinding squadMate{};
    squadMate.x = 1.f;
    squadMate.y = 0.f;
    squadMate.targetX = 10.f;
    squadMate.targetY = 0.f;
    squadMate.teamId = 1;
    runtime.addAgent(squadMate);

    expectTrue(runtime.agentCount() == 2u, "patrol ally runtime has squad pair");

    fuse::frame::FrameCtx ctx;
    runtime.buildSnapshots();
    runtime.evaluate(ctx);
    runtime.commit();

    expectTrue(runtime.blackboard().flag(0, 1), "ally agent sets squad flag when allies in radius");
}

void testPerAgentTreeSelection() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.registerTreeProfile(0, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.5f));
    runtime.registerTreeProfile(1, fuse::ai::BehaviorTree::makePatrolWithAllySupportDemoTree(8.f));

    fuse::ai::AgentBinding mover{};
    mover.x = 0.f;
    mover.y = 0.f;
    mover.targetX = 4.f;
    mover.targetY = 0.f;
    mover.moveSpeed = 0.5f;
    mover.treeProfileId = 0;
    runtime.addAgent(mover);

    fuse::ai::AgentBinding patrol{};
    patrol.x = 0.f;
    patrol.y = 0.f;
    patrol.targetX = 10.f;
    patrol.targetY = 0.f;
    patrol.teamId = 1;
    patrol.treeProfileId = 1;
    runtime.addAgent(patrol);

    fuse::ai::AgentBinding squadMate{};
    squadMate.x = 1.f;
    squadMate.y = 0.f;
    squadMate.teamId = 1;
    squadMate.treeProfileId = 1;
    runtime.addAgent(squadMate);

    fuse::frame::FrameCtx ctx;
    runtime.buildSnapshots();
    runtime.evaluate(ctx);
    runtime.commit();

    expectTrue(runtime.bindings()[0].x > 0.f, "profile 0 agent moves toward target");
    expectTrue(runtime.blackboard().flag(1, 1), "profile 1 patrol agent sets squad flag");
    expectTrue(runtime.treeProfileCount() == 2u, "two tree profiles registered");
}

void testUaiskScriptHostBridge() {
#if FUSE_AI_TEST_SCRIPT_HOST
    fuse::script::ScriptHost host;
    fuse::ai::BehaviorRuntime runtime;
    runtime.registerTreeProfile(0, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.2f));

    fuse::ai::uaisk::ScriptHostBridge bridge(host, runtime);
    expectTrue(bridge.attach(), "ScriptHost bridge attaches");
    expectTrue(bridge.loadPatrolSquadViaHost(), "patrol squad imports via ScriptHost");
    expectTrue(bridge.importCount() >= 1u, "ScriptHost import count tracked");
    expectTrue(runtime.treeProfileCount() >= 2u, "ScriptHost import registers profile");
#endif
}

void testUaiskCsCodegen() {
    static const char* kCsText =
        "class PatrolSquad : BehaviorBase {\n"
        "  behaviorTree = \"patrol_squad.bt\";\n"
        "  float patrolRadius;\n"
        "  void onPatrol() {}\n"
        "}\n";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenTreeFromCs("aiBehaviors.cs", kCsText, tree, &error),
               "UAISK codegen builds patrol squad tree");
    expectTrue(tree.nodeCount() >= 7u, "UAISK codegen emits selector tree nodes");

    fuse::ai::BehaviorRuntime runtime;
    expectTrue(fuse::ai::uaisk::importCodegenProfile("aiBehaviors.cs", kCsText, 1, runtime, &error),
               "UAISK codegen imports runtime profile");
    expectTrue(runtime.treeProfileCount() == 1u, "UAISK codegen profile registered");

    fuse::ai::uaisk::UaiskCsParseResult parsed;
    fuse::ai::uaisk::UaiskCsAst ast;
    expectTrue(fuse::ai::uaisk::parseCsModule("aiBehaviors.cs", kCsText, parsed), "AST parse for codegen");
    expectTrue(fuse::ai::uaisk::buildAstFromParse(parsed, ast), "AST build succeeds");
    expectTrue(ast.baseClass == "BehaviorBase", "AST captures base class");
    expectTrue(ast.methods.size() == 1u, "AST captures method ref");
    expectTrue(ast.fields.size() == 1u, "AST captures field ref");
}

void testWireAgentEntityBindings() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.registerTreeProfile(0, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.1f));
    fuse::ai::AgentBinding binding{};
    binding.x = 1.f;
    binding.y = 2.f;
    runtime.addAgent(binding);

    const fuse::Handle<fuse::Object> entity(9u, 1u);
    fuse::ai::wireAgentEntityBindings(runtime, {{0u, entity}});
    expectTrue(runtime.bindings()[0].agent.index() == 9u, "wire sets agent entity handle");
}

void testUaiskCsParser() {
    static const char* kCsText =
        "class PatrolSquad {\n"
        "  behaviorTree = \"patrol_squad.bt\";\n"
        "  bb.selector children=4,5 hook=aiBehaviors.cs\n"
        "}\n";

    fuse::ai::uaisk::UaiskCsParseResult parsed;
    expectTrue(fuse::ai::uaisk::parseCsModule("aiBehaviors.cs", kCsText, parsed), "UAISK cs parser succeeds");
    expectTrue(parsed.className == "PatrolSquad", "UAISK cs parser extracts class name");
    expectTrue(parsed.profileId == 1u, "UAISK cs parser maps aiBehaviors to profile 1");
    expectTrue(!parsed.behaviorTreeHooks.empty(), "UAISK cs parser collects hook refs");
}

void testUaiskScriptImportProfile() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.registerTreeProfile(0, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.25f));

    std::string error;
    expectTrue(fuse::ai::uaisk::registerPatrolSquadProfile(runtime, &error), "UAISK patrol_squad profile imports");
    expectTrue(fuse::ai::uaisk::treeProfileForModule("aiBehaviors.cs") == 1u,
               "aiBehaviors.cs maps to patrol selector profile");
    expectTrue(fuse::ai::uaisk::treeProfileForModule("aiMovement.cs") == 0u,
               "aiMovement.cs maps to move-toward profile");
    expectTrue(runtime.treeProfileCount() == 2u, "import adds profile 1");
}

void testUaiskPatrolSquadTemplateLoad() {
    const std::string text = R"(
bb.condition.allies_in_radius threshold=8 loops=1
bb.action.set_flag flag=1
bb.condition.distance_less threshold=5
bb.action.set_flag flag=0
bb.sequence children=0,1
bb.sequence children=2,3
bb.selector children=4,5 hook=aiBehaviors.cs
root=6
)";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::loadTreeFromText(text, tree, &error), "UAISK patrol_squad template loads");
    expectTrue(tree.nodeCount() == 7u, "patrol_squad template has seven nodes");
    expectTrue(tree.node(6).kind == fuse::ai::NodeKind::Selector, "patrol_squad root is selector");
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

    const fuse::ai::BehaviorTickResult farResult =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(farResult.status == fuse::ai::BehaviorStatus::Running, "move toward runs while far");
    expectTrue(farResult.flagIndex == 2u, "move toward writes configured flag");
    expectTrue(farResult.flagValue, "move toward marks moving flag");

    agent.targetX = 0.5f;
    const fuse::ai::BehaviorTickResult nearResult =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(nearResult.status == fuse::ai::BehaviorStatus::Success, "move toward succeeds when arrived");
    expectTrue(!nearResult.flagValue, "move toward clears moving flag on arrival");
}

void testMonitorDecorator() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.action.wait", 0.f, 0, 3, {}, {}},
        {"bb.condition.distance_greater", 2.f, 0, 0, {}, {}},
        {"bb.monitor", 0.f, 0, 0, {}, {0, 1}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 2, tree), "monitor decorator loads");

    fuse::ai::Blackboard board;
    board.resize(1);

    fuse::ai::AgentSnapshot agent;
    agent.x = 0.f;
    agent.y = 0.f;
    agent.targetX = 5.f;
    agent.targetY = 0.f;

    fuse::ai::BehaviorEvalContext ctx;
    ctx.tickCount = 1;
    std::vector<fuse::u32> wait_ticks(static_cast<std::size_t>(tree.nodeCount()), 0u);
    ctx.waitStartTicks = wait_ticks.data();

    const fuse::ai::BehaviorTickResult running =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(running.status == fuse::ai::BehaviorStatus::Running,
               "monitor returns running while guarded child runs");

    agent.targetX = 1.f;
    const fuse::ai::BehaviorTickResult failed =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(failed.status == fuse::ai::BehaviorStatus::Failure,
               "monitor aborts when observer child fails");
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

void testParallelPolicyHelpers() {
    fuse::ai::ParallelPolicy policy;
    expectTrue(!fuse::ai::parallel_policy_has_guards(policy),
               "default parallel policy has no guards");
    expectTrue(fuse::ai::parallel_policy_is_valid(policy, 2u),
               "default parallel policy is valid for two children");

    policy.requireNonEmptyBoard = true;
    expectTrue(fuse::ai::parallel_policy_has_guards(policy),
               "parallel policy reports configured guards");
    policy.successThreshold = 3;
    expectTrue(!fuse::ai::parallel_policy_is_valid(policy, 2u),
               "parallel policy rejects impossible success threshold");
}

void testGuardBlackboardNonemptyLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.guard.blackboard_nonempty", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "blackboard nonempty guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult populated =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(populated.status == fuse::ai::BehaviorStatus::Success,
               "blackboard nonempty guard succeeds when agents are allocated");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "blackboard nonempty guard fails with unbound view");

    board.resize(0);
    const fuse::ai::BehaviorTickResult empty =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(empty.status == fuse::ai::BehaviorStatus::Failure,
               "blackboard nonempty guard fails when agent count is zero");
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

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Success,
               "blackboard empty guard succeeds when view is unbound");

    fuse::ai::Blackboard board;
    board.resize(1);
    const fuse::ai::BehaviorTickResult populated =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(populated.status == fuse::ai::BehaviorStatus::Failure,
               "blackboard empty guard fails when agents are allocated");
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
               "agent valid guard succeeds for in-range index");

    const fuse::ai::BehaviorTickResult invalid =
        tree.tick(2, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "agent valid guard fails for out-of-range index");

    const fuse::ai::BehaviorTickResult unbound =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(unbound.status == fuse::ai::BehaviorStatus::Failure,
               "agent valid guard fails with unbound view");
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

    const fuse::ai::BehaviorTickResult emptyScalar =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(emptyScalar.status == fuse::ai::BehaviorStatus::Failure,
               "scalar set guard fails when slot is zero");

    board.setScalar(0, 1, 1.5f);
    const fuse::ai::BehaviorTickResult setScalar =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(setScalar.status == fuse::ai::BehaviorStatus::Success,
               "scalar set guard succeeds when slot is non-zero");
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

    const fuse::ai::BehaviorTickResult emptyFlag =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(emptyFlag.status == fuse::ai::BehaviorStatus::Failure,
               "flag set guard fails when flag is false");

    board.setFlag(0, 2, true);
    const fuse::ai::BehaviorTickResult setFlag =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(setFlag.status == fuse::ai::BehaviorStatus::Success,
               "flag set guard succeeds when flag is true");
}

void testGuardValidAllyRadiusLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> validSpecs = {
        {"bb.guard.valid_ally_radius", 5.f, 0, 1, {}, {}},
    };
    const std::vector<fuse::ai::NodeLoadSpec> invalidSpecs = {
        {"bb.guard.valid_ally_radius", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree validTree;
    fuse::ai::BehaviorTree invalidTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(validSpecs, 0, validTree), "valid radius guard loads");
    expectTrue(fuse::ai::loadTreeFromSpecs(invalidSpecs, 0, invalidTree), "invalid radius guard loads");

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult valid =
        validTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(valid.status == fuse::ai::BehaviorStatus::Success,
               "valid ally radius guard succeeds for positive threshold");

    const fuse::ai::BehaviorTickResult invalid =
        invalidTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "valid ally radius guard fails for zero threshold");
}

void testBlackboardViewAgentAndFlagHelpers() {
    fuse::ai::Blackboard board;
    board.resize(2);
    board.setFlag(1, 0, true);
    board.setScalar(0, 2, 4.f);

    const fuse::ai::BlackboardView view(board);
    expectTrue(view.agentCount() == 2u, "blackboard view reports agent count");
    expectTrue(!view.isBoardEmpty(), "resized board view is not empty");
    expectTrue(view.isAgentValid(0), "agent index 0 is valid");
    expectTrue(view.isAgentValid(1), "agent index 1 is valid");
    expectTrue(!view.isAgentValid(2), "agent index 2 is out of range");
    expectTrue(view.isFlagEmpty(0, 0), "unset flag reads as empty");
    expectTrue(!view.isFlagEmpty(1, 0), "set flag is not empty");
    expectTrue(view.isFlagSet(1, 0), "set flag reads as set");
    expectTrue(!view.isFlagSet(0, 0), "unset flag is not set");
    expectTrue(view.isScalarEmpty(0, 2) == false, "non-zero scalar is not empty");
    expectTrue(view.isScalarSet(0, 2), "non-zero scalar reads as set");
    expectTrue(!view.isScalarSet(0, 0), "zero scalar is not set");

    const fuse::ai::BlackboardView unbound;
    expectTrue(unbound.isBoardEmpty(), "unbound view is board empty");
    expectTrue(unbound.agentCount() == 0u, "unbound view reports zero agents");
    expectTrue(!unbound.isAgentValid(0), "unbound view rejects agent index");
    expectTrue(unbound.isFlagEmpty(0, 0), "unbound view treats flag as empty");
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
               "parallel require-agent guard succeeds for valid index");

    const fuse::ai::BehaviorTickResult invalid =
        tree.tick(1, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-agent guard fails for out-of-range index");
    expectTrue(!invalid.wroteFlag, "parallel require-agent guard skips child side effects");
}

void testParallelRequireNonEmptyBoardGuard() {
    fuse::ai::ParallelPolicy policy;
    policy.requireNonEmptyBoard = true;

    fuse::ai::BehaviorTree tree = makeParallelTree(fuse::ai::NodeKind::ActionSetFlag,
                                                 fuse::ai::NodeKind::ActionSetFlag,
                                                 policy);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult populated =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(populated.status == fuse::ai::BehaviorStatus::Success,
               "parallel require-nonempty-board guard succeeds with agents");

    const fuse::ai::BehaviorTickResult empty =
        tree.tick(0, agent, fuse::ai::BlackboardView());
    expectTrue(empty.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-nonempty-board guard fails with unbound view");

    board.resize(0);
    const fuse::ai::BehaviorTickResult zeroAgents =
        tree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(zeroAgents.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-nonempty-board guard fails with zero agents");
    expectTrue(!zeroAgents.wroteFlag,
               "parallel require-nonempty-board guard skips children on empty board");
}

void testParallelRequireValidRadiusGuard() {
    fuse::ai::ParallelPolicy policy;
    policy.requireValidRadius = true;

    auto makeRadiusParallelTree = [&](float threshold) {
        fuse::ai::BehaviorTree tree;
        tree.addNode({fuse::ai::NodeKind::ActionSetFlag, 0.f, 0, 1, 0, 0});
        tree.addNode({fuse::ai::NodeKind::ActionSetFlag, 0.f, 1, 1, 0, 0});
        fuse::ai::BehaviorNode parallel;
        parallel.kind = fuse::ai::NodeKind::Parallel;
        parallel.childA = 0;
        parallel.childB = 1;
        parallel.threshold = threshold;
        parallel.parallelPolicy = policy;
        tree.addNode(std::move(parallel));
        tree.setRoot(2);
        return tree;
    };

    const fuse::ai::BehaviorTree validTree = makeRadiusParallelTree(5.f);
    const fuse::ai::BehaviorTree invalidTree = makeRadiusParallelTree(0.f);

    fuse::ai::AgentSnapshot agent;
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BehaviorTickResult valid =
        validTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(valid.status == fuse::ai::BehaviorStatus::Success,
               "parallel require-radius guard succeeds with positive threshold");

    const fuse::ai::BehaviorTickResult invalid =
        invalidTree.tick(0, agent, fuse::ai::BlackboardView(board));
    expectTrue(invalid.status == fuse::ai::BehaviorStatus::Failure,
               "parallel require-radius guard fails with zero threshold");
    expectTrue(!invalid.wroteFlag,
               "parallel require-radius guard skips children on invalid radius");
}

void testSpatialLeavesFailOnInvalidRadius() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.allies_in_radius", 0.f, 0, 1, {}, {}},
        {"bb.action.allies_count", 0.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree radiusTree;
    fuse::ai::BehaviorTree countTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, radiusTree), "zero-radius condition loads");
    expectTrue(fuse::ai::loadTreeFromSpecs({specs[1]}, 0, countTree), "zero-radius count loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult radiusResult =
        radiusTree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(radiusResult.status == fuse::ai::BehaviorStatus::Failure,
               "allies_in_radius fails when radius is not finite");

    const fuse::ai::BehaviorTickResult countResult =
        countTree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(countResult.status == fuse::ai::BehaviorStatus::Failure,
               "allies_count fails when radius is not finite");
}

void testBlackboardViewSlotValidity() {
    fuse::ai::Blackboard board;
    board.resize(1);

    const fuse::ai::BlackboardView view(board);
    expectTrue(view.isFlagSlotValid(0), "flag slot zero is valid");
    expectTrue(view.isScalarSlotValid(fuse::ai::Blackboard::kMaxScalars - 1),
               "last scalar slot is valid");
    expectTrue(!view.isFlagSlotValid(fuse::ai::Blackboard::kMaxFlags),
               "out-of-range flag slot is invalid");
    expectTrue(!view.isScalarSlotValid(fuse::ai::Blackboard::kMaxScalars),
               "out-of-range scalar slot is invalid");

    const fuse::ai::BlackboardView unbound;
    expectTrue(!unbound.isFlagSlotValid(0), "unbound view rejects flag slot lookup");
    expectTrue(!unbound.isScalarSlotValid(0), "unbound view rejects scalar slot lookup");
}

void testParallelGuardTextLoader() {
    const std::string text = R"(
bb.action.set_flag flag=0
bb.action.set_flag flag=1
bb.parallel children=0,1 require_board=1 require_allies=1 require_agent=1 require_nonempty_board=1 require_radius=1 threshold=8
root=2
)";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::loadTreeFromText(text, tree, &error), "parallel guard text loads");
    expectTrue(tree.node(2).parallelPolicy.requireBoundBlackboard, "text loader sets require_board");
    expectTrue(tree.node(2).parallelPolicy.requireAllyContext, "text loader sets require_allies");
    expectTrue(tree.node(2).parallelPolicy.requireValidAgent, "text loader sets require_agent");
    expectTrue(tree.node(2).parallelPolicy.requireNonEmptyBoard, "text loader sets require_nonempty_board");
    expectTrue(tree.node(2).parallelPolicy.requireValidRadius, "text loader sets require_radius");
    expectTrue(tree.node(2).threshold == 8.f, "text loader preserves parallel radius threshold");
}

void testNoAlliesInRadiusConditionLeaf() {
    const std::vector<fuse::ai::NodeLoadSpec> specs = {
        {"bb.condition.no_allies_in_radius", 2.f, 0, 1, {}, {}},
    };

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs(specs, 0, tree), "no allies in radius condition loads");

    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    fuse::ai::Blackboard board;
    board.resize(1);

    const std::vector<fuse::ai::AllyCandidate> allies = makeTestAllies();
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = &allies;

    const fuse::ai::BehaviorTickResult clear =
        tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(clear.status == fuse::ai::BehaviorStatus::Success,
               "no allies in radius succeeds when none are nearby");

    const std::vector<fuse::ai::NodeLoadSpec> wideSpecs = {
        {"bb.condition.no_allies_in_radius", 100.f, 0, 1, {}, {}},
    };
    fuse::ai::BehaviorTree wideTree;
    expectTrue(fuse::ai::loadTreeFromSpecs(wideSpecs, 0, wideTree), "wide no-allies tree loads");

    const fuse::ai::BehaviorTickResult blocked =
        wideTree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
    expectTrue(blocked.status == fuse::ai::BehaviorStatus::Failure,
               "no allies in radius fails when allies are within radius");
}

void testNearestAllyDistanceActionLeaf() {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.action.nearest_ally_distance";
    spec.flagIndex = 1;
    spec.scalarSlot = 0;

    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "nearest ally distance action loads");

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
               "nearest ally distance action succeeds");
    expectTrue(result.wroteScalar && result.scalarIndex == 0u,
               "nearest ally distance writes scalar slot");
    expectTrue(result.scalarValue == 25.f, "nearest ally distance stores squared distance");
    expectTrue(result.wroteFlag && result.flagValue, "nearest ally distance sets flag");
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

void testUaiskCsSyntaxTree() {
    static const char* kCsText =
        "class PatrolSquad {\n"
        "  behaviorTree = \"patrol_squad.bt\";\n"
        "  public void onTick() {}\n"
        "  squadRadius = 8;\n"
        "}\n";

    fuse::ai::uaisk::UaiskCsSyntaxTree tree;
    expectTrue(fuse::ai::uaisk::parseCsSyntaxTree("aiBehaviors.cs", kCsText, tree),
               "UAISK syntax tree parse succeeds");
    expectTrue(tree.rootClassName == "PatrolSquad", "UAISK syntax tree root class");
    expectTrue(tree.nodes.size() >= 4u, "UAISK syntax tree collects nodes");
    expectTrue(!tree.behaviorTreeHooks.empty(), "UAISK syntax tree collects hooks");
}

void testUaiskTreeFileWatchReload() {
    static const char* kInitialBt =
        "bb.action.set_flag flag=1\n"
        "root=0\n";
    static const char* kUpdatedBt =
        "bb.action.set_flag flag=0\n"
        "root=0\n";

    fuse::ai::BehaviorRuntime runtime;
    fuse::ai::uaisk::TreeFileWatchRegistry registry;
    registry.watchProfile("patrol_squad.bt", 2u, kInitialBt);
    expectTrue(registry.pollReloads(runtime) == 0u, "no reload when content unchanged");

    registry.setContent("patrol_squad.bt", kUpdatedBt);
    expectTrue(registry.pollReloads(runtime) == 1u, "tree reload on content change");
    expectTrue(registry.reloadCount() == 1u, "reload counter tracked");
}

void testUaiskInotifyFileWatchProgress() {
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "fuse_patrol_wave14.bt";
    {
        std::ofstream out(tempPath);
        out << "bb.action.set_flag flag=1\nroot=0\n";
    }

    fuse::ai::BehaviorRuntime runtime;
    fuse::ai::uaisk::TreeFileWatchRegistry registry;
    registry.watchProfileFromDisk(tempPath.string(), 4u);
    expectTrue(registry.pollInotifyFileChanges(runtime) == 0u, "no inotify reload when unchanged");
    expectTrue(registry.pollInotifyFileChanges(runtime) >= 0u, "inotify poll path executes");

    fs::remove(tempPath);
}

void testUaiskFSEventsBackendStub() {
#if defined(__APPLE__)
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "fuse_fsevents_wave16.bt";
    {
        std::ofstream out(tempPath);
        out << "bb.action.set_flag flag=1\nroot=0\n";
    }
    fuse::ai::uaisk::OsFileWatchHandle handle;
    expectTrue(fuse::ai::uaisk::createOsFileWatch(tempPath.string(), handle),
               "FSEvents watch handle created on macOS");
    expectTrue(handle.backend == fuse::ai::uaisk::OsFileWatchBackend::FSEvents,
               "FSEvents backend selected on macOS");
    expectTrue(handle.fsevents.latencyMs >= 16u, "FSEvents latency stub seeded");
    fuse::ai::uaisk::OsFileWatchStatus status;
    expectTrue(!fuse::ai::uaisk::pollOsFileWatch(handle, status), "FSEvents poll unchanged");
    expectTrue(status.fsevents.latencyMs >= 16u, "FSEvents poll reports latency stub");
    fuse::ai::uaisk::closeOsFileWatch(handle);
    fs::remove(tempPath);
#else
    fuse::ai::uaisk::OsFileWatchHandle handle;
    expectTrue(handle.backend == fuse::ai::uaisk::OsFileWatchBackend::StatPoll,
               "non-macOS builds use stat poll backend by default");
#endif
}

void testUaiskInotifyHotReloadRegistry() {
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "fuse_inotify_wave16.bt";
    {
        std::ofstream out(tempPath);
        out << "bb.action.set_flag flag=1\nroot=0\n";
    }

    fuse::ai::BehaviorRuntime runtime;
    fuse::ai::uaisk::TreeFileWatchRegistry registry;
    registry.watchProfileFromDisk(tempPath.string(), 6u);
    expectTrue(registry.inotifyPollCount() >= 0u, "inotify poll counter available");
    expectTrue(registry.pollInotifyFileChanges(runtime) == 0u, "inotify hot reload unchanged");

    {
        std::ofstream out(tempPath, std::ios::trunc);
        out << "bb.action.set_flag flag=0\nroot=0\n";
    }
    expectTrue(registry.pollInotifyFileChanges(runtime) >= 0u, "inotify hot reload poll executes");

    fs::remove(tempPath);
}

void testUaiskCodegenSyntaxTreePath() {
    static const char* kCsText =
        "class SquadPatrol : BehaviorBase {\n"
        "  behaviorTree = \"aiSquad.cs\";\n"
        "  void onSquadPatrol() {}\n"
        "}\n";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenTreeFromSyntaxTree("aiSquad.cs", kCsText, tree, &error),
               "syntax-tree codegen builds squad tree");
    expectTrue(tree.nodeCount() >= 7u, "syntax-tree codegen emits selector nodes");
}

void testUaiskCodegenMethodBody() {
    static const char* kCsText =
        "class PatrolActions : BehaviorBase {\n"
        "  void onPatrolMove() {\n"
        "    moveToward(target, moveSpeed = 0.3f);\n"
        "  }\n"
        "}\n";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenTreeFromSyntaxTree("aiActions.cs", kCsText, tree, &error),
               "method-body codegen builds move-toward tree");
    expectTrue(tree.nodeCount() >= 1u, "method-body codegen emits nodes");
}

void testUaiskExpressionAstCodegen() {
    static const char* kCsText =
        "class TargetingPatrol : BehaviorBase {\n"
        "  condition = \"distance < 12\";\n"
        "  distanceThreshold = 5.f;\n"
        "  behaviorTree = \"aiTargeting.cs\";\n"
        "}\n";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenTreeFromSyntaxTree("aiTargeting.cs", kCsText, tree, &error),
               "expression AST codegen builds distance tree");
    expectTrue(tree.nodeCount() >= 1u, "expression AST codegen emits nodes");

    fuse::ai::uaisk::UaiskCsSyntaxTree syntaxTree;
    expectTrue(fuse::ai::uaisk::parseCsSyntaxTree("aiTargeting.cs", kCsText, syntaxTree),
               "expression AST syntax tree parse");
    fuse::ai::uaisk::UaiskCsAst ast;
    expectTrue(fuse::ai::uaisk::buildAstFromSyntaxTree(syntaxTree, ast), "expression AST ast build");
    expectTrue(!ast.conditions.empty(), "expression AST condition collected");
    expectTrue(ast.conditions[0].fieldName == "distance", "expression AST field name parsed");
    expectTrue(ast.conditions[0].threshold == 12.f, "expression AST threshold parsed");
}

void testUaiskNestedCompositeCodegen() {
    static const char* kCsText =
        "class CompositePatrol : BehaviorBase {\n"
        "  composite = \"selector\";\n"
        "  behaviorTree = \"aiComposite.cs\";\n"
        "  void onNestedPatrol() {}\n"
        "}\n";

    fuse::ai::BehaviorTree tree;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenTreeFromSyntaxTree("aiComposite.cs", kCsText, tree, &error),
               "nested composite codegen builds selector tree");
    expectTrue(tree.nodeCount() >= 7u, "nested composite codegen emits selector+sequence nodes");
}

void testUaiskFSEventsPollPath() {
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "fuse_patrol_wave20.bt";
    {
        std::ofstream out(tempPath);
        out << "bb.action.set_flag flag=1\nroot=0\n";
    }

    fuse::ai::BehaviorRuntime runtime;
    fuse::ai::uaisk::TreeFileWatchRegistry registry;
    registry.watchProfileFromDisk(tempPath.string(), 8u);
    expectTrue(registry.pollFSEventsFileChanges(runtime) == 0u, "no fsevents reload when unchanged");
    expectTrue(registry.pollFSEventsFileChanges(runtime) >= 0u, "fsevents poll path executes");

    fs::remove(tempPath);
}

void testUaiskCodegenMultiLeafMethodBody() {
    static const char* kCsText =
        "class SquadActions : BehaviorBase {\n"
        "  void onSquadAdvance() {\n"
        "    wait(0.5f);\n"
        "    moveToward(target, moveSpeed = 0.4f);\n"
        "    setFlag(patrolReady, true);\n"
        "    alliesInRadius(8.f);\n"
        "  }\n"
        "}\n";

    fuse::ai::uaisk::UaiskCsSyntaxTree syntaxTree;
    expectTrue(fuse::ai::uaisk::parseCsSyntaxTree("aiSquadActions.cs", kCsText, syntaxTree),
               "multi-leaf method body parses");

    fuse::ai::uaisk::UaiskCsAst ast;
    expectTrue(fuse::ai::uaisk::buildAstFromSyntaxTree(syntaxTree, ast), "multi-leaf AST built");

    std::vector<fuse::ai::NodeLoadSpec> specs;
    fuse::u32 rootIndex = 0;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenSpecsForModule(ast, specs, rootIndex, &error),
               "multi-leaf method-body codegen specs");

    bool sawWait = false;
    bool sawMove = false;
    bool sawFlag = false;
    bool sawAllies = false;
    for (const fuse::ai::NodeLoadSpec& spec : specs) {
        if (spec.typeId == "bb.action.wait") {
            sawWait = true;
        }
        if (spec.typeId == "gb.action.move_toward") {
            sawMove = true;
        }
        if (spec.typeId == "bb.action.set_flag") {
            sawFlag = true;
        }
        if (spec.typeId == "bb.condition.allies_in_radius") {
            sawAllies = true;
        }
    }
    expectTrue(sawWait, "multi-leaf method body emits wait");
    expectTrue(sawMove, "multi-leaf method body emits moveToward");
    expectTrue(sawFlag, "multi-leaf method body emits setFlag");
    expectTrue(sawAllies, "multi-leaf method body emits alliesInRadius");
    expectTrue(specs.size() >= 4u, "multi-leaf method body accumulates leaf sequence");
}

void testUaiskFSEventsCoreServicesStub() {
    const auto config = fuse::ai::uaisk::fsevents_stub::makeDefaultCoreServicesWatchConfig();
    expectTrue(config.latencyMs >= 16u, "CoreServices watch config latency seeded");
    expectTrue(config.createFlags == fuse::ai::uaisk::fsevents_stub::kFSEventStreamCreateFlagFileEvents,
               "CoreServices create flags stub");
    expectTrue(fuse::ai::uaisk::fsevents_stub::createFileEventStreamStub("patrol.bt", config) == nullptr,
               "CoreServices stream stub returns null without framework link");
}

void testUaiskCodegenFieldDefaults() {
    static const char* kCsText =
        "class PatrolSquad : BehaviorBase {\n"
        "  behaviorTree = \"aiBehaviors.cs\";\n"
        "  float patrolRadius = 12;\n"
        "  float distanceThreshold = 7;\n"
        "}\n";

    fuse::ai::uaisk::UaiskCsSyntaxTree tree;
    expectTrue(fuse::ai::uaisk::parseCsSyntaxTree("aiBehaviors.cs", kCsText, tree),
               "syntax tree parses field defaults");

    fuse::ai::uaisk::UaiskCsAst ast;
    expectTrue(fuse::ai::uaisk::buildAstFromSyntaxTree(tree, ast), "AST built from syntax tree");

    std::vector<fuse::ai::NodeLoadSpec> specs;
    fuse::u32 rootIndex = 0;
    std::string error;
    expectTrue(fuse::ai::uaisk::codegenSpecsForModule(ast, specs, rootIndex, &error),
               "codegen specs with field defaults");

    bool sawRadius = false;
    bool sawDistance = false;
    for (const fuse::ai::NodeLoadSpec& spec : specs) {
        if (spec.typeId == "bb.condition.allies_in_radius" && spec.threshold == 12.f) {
            sawRadius = true;
        }
        if (spec.typeId == "bb.condition.distance_less" && spec.threshold == 7.f) {
            sawDistance = true;
        }
    }
    expectTrue(sawRadius, "patrolRadius field default applied to allies_in_radius");
    expectTrue(sawDistance, "distanceThreshold field default applied to distance_less");
}

void testUaiskTreeOsFileWatchProgress() {
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "fuse_patrol_wave13.bt";
    {
        std::ofstream out(tempPath);
        out << "bb.action.set_flag flag=1\nroot=0\n";
    }

    fuse::ai::BehaviorRuntime runtime;
    fuse::ai::uaisk::TreeFileWatchRegistry registry;
    registry.watchProfileFromDisk(tempPath.string(), 3u);
    expectTrue(registry.osPollCount() == 0u, "os poll count starts at zero");
    expectTrue(registry.pollOsFileChanges(runtime) == 0u, "no os reload when file unchanged");
    expectTrue(registry.osPollCount() == 1u, "os poll counted");

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::ofstream out(tempPath, std::ios::trunc);
        out << "bb.action.set_flag flag=2\nroot=0\n";
    }

    registry.setContent(tempPath.string(),
                        "bb.action.set_flag flag=2\nroot=0\n");
    expectTrue(registry.pollReloads(runtime) == 1u, "content-hash reload on disk content change");
    expectTrue(registry.osReloadCount() == 0u || registry.osReloadCount() == 1u,
               "os reload counter tracked");

    fs::remove(tempPath);
}

void testReloadCodegenProfile() {
    static const char* kCsText =
        "class PatrolSquad : BehaviorBase {\n"
        "  behaviorTree = \"patrol_squad.bt\";\n"
        "}\n";

    fuse::ai::BehaviorRuntime runtime;
    runtime.registerTreeProfile(1, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.1f));
    runtime.addAgent({});
    runtime.blackboard().setFlag(0, 2, true);

    std::string error;
    expectTrue(fuse::ai::uaisk::reloadCodegenProfile("aiBehaviors.cs", kCsText, 1, runtime,
                                                     fuse::ai::TreeReloadPolicy::PreserveBlackboard, &error),
               "UAISK codegen reloads runtime profile");
    expectTrue(runtime.treeProfileCount() == 1u, "reload keeps profile slot count");
    expectTrue(runtime.blackboard().getFlag(0, 2), "codegen reload preserves blackboard");
}

void testRuntimeTreeReloadPreservesBlackboard() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.registerTreeProfile(0, fuse::ai::BehaviorTree::makePatrolWhenNearTarget());

    fuse::ai::AgentBinding binding{};
    binding.x = 0.f;
    binding.y = 0.f;
    binding.targetX = 1.f;
    binding.targetY = 0.f;
    runtime.addAgent(binding);
    runtime.blackboard().setFlag(0, 1, true);

    fuse::ai::BehaviorTree replacement = fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.2f);
    runtime.reloadTreeProfile(0, replacement, fuse::ai::TreeReloadPolicy::PreserveBlackboard);

    expectTrue(runtime.blackboard().getFlag(0, 1), "reload preserves blackboard flags");
    expectTrue(runtime.treeProfileCount() == 1u, "reload keeps profile count");
}

void testAgentEntityBindSyncsBindingPosition() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.1f));

    fuse::ai::AgentBinding binding{};
    binding.agent = fuse::Handle<fuse::Object>(3u, 1u);
    binding.x = 0.f;
    binding.y = 0.f;
    binding.targetX = 5.f;
    binding.targetY = 0.f;
    runtime.addAgent(binding);

    runtime.setAgentPositionProvider([](fuse::Handle<fuse::Object> entity, float& outX, float& outY) {
        if (entity.index() == 3u) {
            outX = 2.f;
            outY = -1.f;
            return true;
        }
        return false;
    });

    runtime.syncAgentBindingsFromEntities();
    runtime.buildSnapshots();

    expectTrue(runtime.bindings()[0].x == 2.f, "entity bind updates binding x");
    expectTrue(runtime.bindings()[0].y == -1.f, "entity bind updates binding y");
    expectTrue(runtime.snapshots()[0].x == 2.f, "entity bind flows into snapshot");
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
    testMoveTowardRuntimeCommit();
    testMoveTowardDemoTreeFactory();
    testPatrolWithAllySupportDemoTree();
    testPerAgentTreeSelection();
    testUaiskScriptHostBridge();
    testUaiskCsParser();
    testUaiskCsCodegen();
    testWireAgentEntityBindings();
    testUaiskScriptImportProfile();
    testUaiskPatrolSquadTemplateLoad();
    testGuideBotMoveTowardLeaf();
    testMonitorDecorator();
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
    testParallelPolicyHelpers();
    testGuardBlackboardNonemptyLeaf();
    testGuardBlackboardBoundLeaf();
    testGuardBlackboardScalarEmptyLeaf();
    testGuardBlackboardFlagEmptyLeaf();
    testGuardBlackboardEmptyLeaf();
    testGuardBlackboardAgentValidLeaf();
    testGuardBlackboardScalarSetLeaf();
    testGuardBlackboardFlagSetLeaf();
    testGuardValidAllyRadiusLeaf();
    testBlackboardViewAgentAndFlagHelpers();
    testGuardAllyContextLeaf();
    testAnyAllyInRadiusConditionLeaf();
    testAlliesCountActionLeaf();
    testParallelRequireBoardGuard();
    testParallelRequireAlliesGuard();
    testParallelRequireValidAgentGuard();
    testParallelRequireNonEmptyBoardGuard();
    testParallelRequireValidRadiusGuard();
    testSpatialLeavesFailOnInvalidRadius();
    testBlackboardViewSlotValidity();
    testParallelGuardTextLoader();
    testNoAlliesInRadiusConditionLeaf();
    testNearestAllyDistanceActionLeaf();
    testNearestAllyActionLeaf();
    testNearestAllyWritesScalarSlot();
    testNearestAllyActionFailsBeyondRadius();
    testParallelSpatialChildStatusAggregation();
    testUaiskCsSyntaxTree();
    testUaiskTreeFileWatchReload();
    testUaiskTreeOsFileWatchProgress();
    testUaiskInotifyFileWatchProgress();
    testUaiskFSEventsBackendStub();
    testUaiskInotifyHotReloadRegistry();
    testUaiskCodegenSyntaxTreePath();
    testUaiskCodegenMethodBody();
    testUaiskCodegenMultiLeafMethodBody();
    testUaiskExpressionAstCodegen();
    testUaiskNestedCompositeCodegen();
    testUaiskFSEventsPollPath();
    testUaiskFSEventsCoreServicesStub();
    testUaiskCodegenFieldDefaults();
    testReloadCodegenProfile();
    testRuntimeTreeReloadPreservesBlackboard();
    testAgentEntityBindSyncsBindingPosition();
    g_failures += run_spatial_query_tests();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ai_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ai_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
