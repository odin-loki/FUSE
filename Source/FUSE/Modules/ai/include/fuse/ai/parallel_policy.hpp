#pragma once

#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/types.hpp>

namespace fuse::ai {

/// Effective success threshold for `bb.parallel` — 0 means all active children must succeed.
[[nodiscard]] u32 effective_success_threshold(const ParallelPolicy& policy, u32 activeChildCount);

/// Effective fail tolerance — 0 defaults to 1.
[[nodiscard]] u32 effective_fail_threshold(const ParallelPolicy& policy);

/// True when optional parallel preconditions (bound board, ally context, valid agent) are met.
[[nodiscard]] bool parallel_preconditions_satisfied(const ParallelPolicy& policy,
                                                    u32 agentIndex,
                                                    const BlackboardView& board,
                                                    const BehaviorEvalContext& ctx);

/// True when parallel policy thresholds are within sane bounds for the given child count.
[[nodiscard]] bool is_parallel_policy_valid(const ParallelPolicy& policy, u32 activeChildCount);

/// Clamp thresholds to `[1, activeChildCount]` when non-zero; leaves zero success as "all children".
[[nodiscard]] ParallelPolicy normalize_parallel_policy(const ParallelPolicy& policy,
                                                       u32 activeChildCount);

/// Child-success rule for a parallel node.
namespace success_policy {

enum class ParallelPolicy { RequireAll, RequireOne };

/// `childCount <= 0` is failure. `successCount < 0` counts as zero.
/// RequireAll is `successCount >= childCount`. RequireOne is `successCount > 0`.
[[nodiscard]] inline bool parallel_succeeded(int successCount, int childCount, ParallelPolicy policy) {
    if (childCount <= 0) {
        return false;
    }
    const int successes = successCount < 0 ? 0 : successCount;
    if (policy == ParallelPolicy::RequireAll) {
        return successes >= childCount;
    }
    return successes > 0;
}

} // namespace success_policy

using success_policy::parallel_succeeded;

} // namespace fuse::ai
