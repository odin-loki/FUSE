#include <fuse/ai/child_count.hpp>
#include <fuse/ai/parallel_policy.hpp>
#include <fuse/ai/spatial_query.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testChildCountOk() {
    expectTrue(fuse::ai::child_count_ok(1), "child_count_ok(1) is true");
    expectTrue(!fuse::ai::child_count_ok(0), "child_count_ok(0) is false");
    expectTrue(fuse::ai::child_count_ok(16), "child_count_ok(16) is true");
    expectTrue(!fuse::ai::child_count_ok(17), "child_count_ok(17) is false");
}

void testEffectiveThresholds() {
    fuse::ai::ParallelPolicy policy;
    expectTrue(fuse::ai::effective_success_threshold(policy, 2) == 2u,
               "zero success threshold means all children");
    expectTrue(fuse::ai::effective_fail_threshold(policy) == 1u,
               "zero fail threshold defaults to one");

    policy.successThreshold = 1;
    policy.failThreshold = 2;
    expectTrue(fuse::ai::effective_success_threshold(policy, 2) == 1u,
               "explicit success threshold preserved");
    expectTrue(fuse::ai::effective_fail_threshold(policy) == 2u,
               "explicit fail threshold preserved");
}

void testParallelPreconditions() {
    fuse::ai::ParallelPolicy policy;
    fuse::ai::Blackboard board;
    board.resize(1);
    const fuse::ai::BlackboardView bound(board);
    const fuse::ai::BlackboardView unbound;

    fuse::ai::BehaviorEvalContext ctx;
    const std::vector<fuse::ai::AllyCandidate> allies = {{0, 0.f, 0.f, 1}};
    ctx.allies = &allies;

    expectTrue(fuse::ai::parallel_preconditions_satisfied(policy, 0, bound, ctx),
               "default policy preconditions pass");

    policy.requireBoundBlackboard = true;
    expectTrue(!fuse::ai::parallel_preconditions_satisfied(policy, 0, unbound, ctx),
               "require board fails on unbound view");
    expectTrue(fuse::ai::parallel_preconditions_satisfied(policy, 0, bound, ctx),
               "require board passes on bound view");

    policy.requireAllyContext = true;
    fuse::ai::BehaviorEvalContext emptyCtx;
    expectTrue(!fuse::ai::parallel_preconditions_satisfied(policy, 0, bound, emptyCtx),
               "require allies fails without context");

    policy.requireValidAgent = true;
    expectTrue(fuse::ai::parallel_preconditions_satisfied(policy, 0, bound, ctx),
               "require agent passes for valid index");
    expectTrue(!fuse::ai::parallel_preconditions_satisfied(policy, 2, bound, ctx),
               "require agent fails for out-of-range index");
}

void testNormalizeParallelPolicy() {
    fuse::ai::ParallelPolicy policy;
    policy.successThreshold = 5;
    policy.failThreshold = 9;

    const fuse::ai::ParallelPolicy normalized = fuse::ai::normalize_parallel_policy(policy, 2);
    expectTrue(normalized.successThreshold == 2u, "success threshold clamped to child count");
    expectTrue(normalized.failThreshold == 2u, "fail threshold clamped to child count");
    expectTrue(normalized.abortOnFail == policy.abortOnFail, "normalize preserves abort flags");
}

void testParallelPolicyValidity() {
    fuse::ai::ParallelPolicy policy;
    expectTrue(fuse::ai::is_parallel_policy_valid(policy, 2), "default policy valid for two children");
    expectTrue(!fuse::ai::is_parallel_policy_valid(policy, 0), "policy invalid with zero children");

    policy.successThreshold = 3;
    expectTrue(!fuse::ai::is_parallel_policy_valid(policy, 2),
               "success threshold above child count is invalid");
}

void testParallelSucceeded() {
    using fuse::ai::success_policy::ParallelPolicy;
    using fuse::ai::parallel_succeeded;
    expectTrue(parallel_succeeded(2, 2, ParallelPolicy::RequireAll),
               "2 of 2 with RequireAll succeeds");
    expectTrue(!parallel_succeeded(1, 2, ParallelPolicy::RequireAll),
               "1 of 2 with RequireAll fails");
    expectTrue(parallel_succeeded(1, 2, ParallelPolicy::RequireOne),
               "1 of 2 with RequireOne succeeds");
}

} // namespace

int run_parallel_policy_tests() {
    testChildCountOk();
    testEffectiveThresholds();
    testParallelPreconditions();
    testNormalizeParallelPolicy();
    testParallelPolicyValidity();
    testParallelSucceeded();
    return g_failures;
}
