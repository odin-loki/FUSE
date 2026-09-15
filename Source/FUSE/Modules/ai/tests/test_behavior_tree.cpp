#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <cstdlib>

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

void testRuntimeParallelEval() {
    fuse::ai::BehaviorRuntime runtime;
    runtime.setTree(fuse::ai::BehaviorTree::makePatrolWhenNearTarget());

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
    testRuntimeParallelEval();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ai_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ai_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
