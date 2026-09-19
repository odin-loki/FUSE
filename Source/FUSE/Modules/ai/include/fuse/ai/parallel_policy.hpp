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

} // namespace fuse::ai
