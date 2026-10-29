#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/spatial_query.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

constexpr fuse::u32 kFactionBandit = 1u << 0;
constexpr fuse::u32 kFactionWolf = 1u << 1;
constexpr fuse::u32 kFactionMerchant = 1u << 2;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

fuse::ai::AllyCandidate candidate(fuse::u32 index, float x, float y, float z, fuse::u32 team, fuse::u32 factions) {
    fuse::ai::AllyCandidate out;
    out.agentIndex = index;
    out.x = x;
    out.y = y;
    out.z = z;
    out.teamId = team;
    out.factionMask = factions;
    return out;
}

/// Self is agent 0 on team 1 at the origin, hostile to bandits and wolves, friendly to merchants.
fuse::ai::AgentSnapshot guard() {
    fuse::ai::AgentSnapshot agent;
    agent.teamId = 1;
    agent.hostileMask = kFactionBandit | kFactionWolf;
    agent.friendlyMask = kFactionMerchant;
    return agent;
}

std::vector<fuse::ai::AllyCandidate> camp() {
    return {
        candidate(0, 0.f, 0.f, 0.f, 1, kFactionBandit),            // self (mask ignored)
        candidate(1, 1.f, 0.f, 0.f, 1, kFactionBandit),            // same team: never hostile
        candidate(2, 2.f, 0.f, 0.f, 2, kFactionMerchant | kFactionBandit), // friendly wins
        candidate(3, 0.f, 0.f, 4.f, 3, kFactionWolf),              // 4 m straight up (3D)
        candidate(4, 3.f, 0.f, 4.f, 3, kFactionBandit),            // 5 m away
        candidate(5, 0.5f, 0.f, 0.f, 4, 0u),                       // neutral: no faction bits
    };
}

fuse::ai::BehaviorTree loadLeaf(const fuse::ai::NodeLoadSpec& spec, const char* message) {
    fuse::ai::BehaviorTree tree;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), message);
    return tree;
}

fuse::ai::BehaviorTickResult tickLeaf(const fuse::ai::BehaviorTree& tree,
                                      const fuse::ai::AgentSnapshot& agent,
                                      const std::vector<fuse::ai::AllyCandidate>* allies) {
    fuse::ai::Blackboard board;
    board.resize(8);
    fuse::ai::BehaviorEvalContext ctx;
    ctx.allies = allies;
    return tree.tick(0, agent, fuse::ai::BlackboardView(board), ctx);
}

fuse::ai::NodeLoadSpec hostilesSpec(float radius, fuse::u32 minCount) {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.condition.hostiles_in_radius";
    spec.threshold = radius;
    spec.loopCount = minCount;
    return spec;
}

fuse::ai::NodeLoadSpec nearestSpec(float maxRadius) {
    fuse::ai::NodeLoadSpec spec;
    spec.typeId = "bb.action.nearest_hostile";
    spec.threshold = maxRadius;
    spec.flagIndex = 3;
    spec.scalarSlot = 2;
    return spec;
}

void testHostilesInRadiusCondition() {
    const std::vector<fuse::ai::AllyCandidate> allies = camp();
    const fuse::ai::AgentSnapshot agent = guard();
    using fuse::ai::BehaviorStatus;

    expectTrue(tickLeaf(loadLeaf(hostilesSpec(4.f, 1), "hostiles leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Success,
               "wolf 4 m overhead counts (3D distance, inclusive radius)");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(3.9f, 1), "hostiles leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Failure,
               "same team, friendly-masked, and neutral agents nearby are not hostile");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(4.f, 2), "hostiles leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Failure,
               "minCount 2 needs the 5 m bandit too");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(5.f, 2), "hostiles leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Success,
               "minCount 2 met at 5 m");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(5.f, 0), "hostiles leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Success,
               "minCount 0 defaults to 1");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(0.f, 1), "hostiles leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Failure,
               "zero radius fails");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(std::numeric_limits<float>::quiet_NaN(), 1), "hostiles leaf loads"),
                        agent,
                        &allies)
                       .status == BehaviorStatus::Failure,
               "NaN radius fails");

    const std::vector<fuse::ai::AllyCandidate> empty;
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(10.f, 1), "hostiles leaf loads"), agent, &empty).status ==
                   BehaviorStatus::Failure,
               "empty ally context fails");
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(10.f, 1), "hostiles leaf loads"), agent, nullptr).status ==
                   BehaviorStatus::Failure,
               "null ally context fails");

    fuse::ai::AgentSnapshot pacifist = guard();
    pacifist.hostileMask = 0;
    expectTrue(tickLeaf(loadLeaf(hostilesSpec(100.f, 1), "hostiles leaf loads"), pacifist, &allies).status ==
                   BehaviorStatus::Failure,
               "empty hostile mask finds nobody");
}

void testNearestHostileAction() {
    const std::vector<fuse::ai::AllyCandidate> allies = camp();
    const fuse::ai::AgentSnapshot agent = guard();
    using fuse::ai::BehaviorStatus;

    const fuse::ai::BehaviorTickResult unlimited =
        tickLeaf(loadLeaf(nearestSpec(0.f), "nearest hostile leaf loads"), agent, &allies);
    expectTrue(unlimited.status == BehaviorStatus::Success, "nearest hostile found with no cap");
    expectTrue(unlimited.wroteFlag && unlimited.flagIndex == 3u && unlimited.flagValue, "flag slot written");
    expectTrue(unlimited.wroteScalar && unlimited.scalarIndex == 2u && unlimited.scalarValue == 3.f,
               "scalar holds the wolf's agentIndex (nearest in 3D)");

    expectTrue(tickLeaf(loadLeaf(nearestSpec(4.f), "nearest hostile leaf loads"), agent, &allies).status ==
                   BehaviorStatus::Success,
               "max radius is inclusive");
    const fuse::ai::BehaviorTickResult capped =
        tickLeaf(loadLeaf(nearestSpec(3.f), "nearest hostile leaf loads"), agent, &allies);
    expectTrue(capped.status == BehaviorStatus::Failure && !capped.wroteFlag && !capped.wroteScalar,
               "nearest hostile beyond max radius fails without writes");
    expectTrue(tickLeaf(loadLeaf(nearestSpec(std::numeric_limits<float>::quiet_NaN()), "nearest hostile leaf loads"),
                        agent,
                        &allies)
                       .status == BehaviorStatus::Failure,
               "NaN max radius fails");

    fuse::ai::NodeLoadSpec noScalar = nearestSpec(0.f);
    noScalar.scalarSlot = fuse::ai::BehaviorNode::kNoScalarSlot;
    const fuse::ai::BehaviorTickResult flagOnly = tickLeaf(loadLeaf(noScalar, "flag-only leaf loads"), agent, &allies);
    expectTrue(flagOnly.status == BehaviorStatus::Success && flagOnly.wroteFlag && !flagOnly.wroteScalar,
               "no scalar slot writes only the flag");

    expectTrue(tickLeaf(loadLeaf(nearestSpec(0.f), "nearest hostile leaf loads"), agent, nullptr).status ==
                   BehaviorStatus::Failure,
               "null ally context fails");
}

void testRuntimeCarriesFactionBindings() {
    fuse::ai::BehaviorTree tree;
    fuse::ai::NodeLoadSpec spec = nearestSpec(0.f);
    spec.flagIndex = 0;
    spec.scalarSlot = 0;
    expectTrue(fuse::ai::loadTreeFromSpecs({spec}, 0, tree), "runtime hostile tree loads");

    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(tree);

    fuse::ai::AgentBinding sentry{};
    sentry.teamId = 1;
    sentry.hostileMask = kFactionWolf;
    runtime.addAgent(sentry);

    fuse::ai::AgentBinding nearBandit{}; // close, but the sentry only hunts wolves
    nearBandit.x = 1.f;
    nearBandit.teamId = 2;
    nearBandit.factionMask = kFactionBandit;
    runtime.addAgent(nearBandit);

    fuse::ai::AgentBinding wolf{};
    wolf.x = 2.f;
    wolf.z = 2.f;
    wolf.teamId = 3;
    wolf.factionMask = kFactionWolf;
    runtime.addAgent(wolf);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    fuse::frame::FrameCtx ctx;
    ctx.frameIndex = 1;
    runtime.buildSnapshots();
    expectTrue(runtime.snapshots()[0].hostileMask == kFactionWolf, "snapshot carries hostileMask");
    expectTrue(runtime.snapshots()[2].z == 2.f, "snapshot carries z");
    runtime.evaluate(ctx);
    runtime.commit();

    expectTrue(runtime.blackboard().flag(0, 0), "sentry found a hostile");
    expectTrue(runtime.blackboard().scalar(0, 0) == 2.f, "sentry targets the wolf via binding factionMask");
    expectTrue(!runtime.blackboard().flag(1, 0) && !runtime.blackboard().flag(2, 0),
               "agents with empty hostile masks find nobody");

    scheduler.shutdown();
}

} // namespace

int run_hostile_leaf_tests() {
    testHostilesInRadiusCondition();
    testNearestHostileAction();
    testRuntimeCarriesFactionBindings();
    return g_failures;
}
