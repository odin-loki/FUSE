#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::net {

enum class InterestScope : u8 {
    OutOfScope,
    InScope,
    AlwaysRelevant,
};

/// AOI policy — relevance vs unload hysteresis mirrors B7.5 terrain streaming radii.
struct InterestPolicy {
    f32 relevance_radius = 128.f;
    f32 unload_radius = 0.f;          // 0 = relevance_radius * 1.25
    f32 always_relevant_radius = 8.f;   // inner sphere with max replication priority
};

/// One entity considered for ghosting / authoritative replication (B7.4 stub).
struct InterestCandidate {
    ecs::EntityID entity = ecs::EntityID::null();
    ecs::vec3 position{};
    f32 priority_boost = 0.f;
};

/// Scoped entity with computed replication priority.
struct InterestEntry {
    ecs::EntityID entity = ecs::EntityID::null();
    f32 distance_sq = 0.f;
    f32 priority = 0.f;
    InterestScope scope = InterestScope::OutOfScope;
};

[[nodiscard]] f32 distance_sq_3d(const ecs::vec3& a, const ecs::vec3& b);
[[nodiscard]] f32 effective_relevance_radius(const InterestPolicy& policy);
[[nodiscard]] f32 effective_unload_radius(const InterestPolicy& policy);
[[nodiscard]] f32 effective_always_relevant_radius(const InterestPolicy& policy);
[[nodiscard]] InterestScope classify_interest(f32 distance_sq, const InterestPolicy& policy,
                                                bool was_in_scope = false);
[[nodiscard]] f32 compute_relevance_priority(f32 distance_sq, const InterestPolicy& policy,
                                             f32 priority_boost = 0.f);
[[nodiscard]] bool within_relevance_radius(f32 distance_sq, const InterestPolicy& policy,
                                           bool was_in_scope = false);

/// In-scope entity set snapshot — used for enter/leave diff between evaluations.
struct InterestScopeSet {
    std::vector<ecs::EntityID> entities;

    void clear();
    void build_from_entries(const std::vector<InterestEntry>& entries);
    [[nodiscard]] bool contains(ecs::EntityID entity) const;
    /// Sorted insert — returns false when the entity is already present.
    [[nodiscard]] bool insert_entity(ecs::EntityID entity);
    /// Remove one entity — returns false when the entity is not present.
    [[nodiscard]] bool remove_entity(ecs::EntityID entity);
    [[nodiscard]] bool equal_to(const InterestScopeSet& other) const;
    [[nodiscard]] bool empty() const { return entities.empty(); }
    [[nodiscard]] u32 size() const { return static_cast<u32>(entities.size()); }
};

/// Preflight for applying an interest scope diff without mutating scope (B7.4 deepen follow-up).
struct InterestDiffPreflight {
    bool empty_diff = true;
    bool has_enters = false;
    bool has_leaves = false;
    /// True when at least one enter/leave would modify `scope`.
    bool would_change_scope = false;

    [[nodiscard]] bool can_apply() const { return !empty_diff && would_change_scope; }

    /// True when apply can proceed normally or via the empty/redundant-diff fast path (B7.4 deepen follow-up).
    [[nodiscard]] bool can_apply_or_skip() const {
        return empty_diff || would_change_scope || (!empty_diff && !would_change_scope);
    }
};

/// Preflight for radius filter/count early-out checks (B7.4 deepen follow-up).
struct RadiusFilterPreflight {
    bool radii_disabled = false;
    bool empty_candidates = true;
    bool prior_scope_empty = true;
    /// True when filter/count would return zero without scanning candidates.
    bool early_out = true;

    [[nodiscard]] bool can_filter() const { return !early_out; }

    /// True when filter/count can proceed or is a no-op early-out (B7.4 deepen follow-up).
    [[nodiscard]] bool can_filter_or_skip() const { return can_filter() || early_out; }
};

/// Entities that entered or left scope between two snapshots.
struct InterestSetDiff {
    std::vector<ecs::EntityID> entered;
    std::vector<ecs::EntityID> left;

    [[nodiscard]] bool empty() const { return entered.empty() && left.empty(); }

    void clear();

    /// Apply enter/leave to a scope snapshot (ghost manager incremental update stub).
    /// Returns true when at least one entity was inserted or removed.
    [[nodiscard]] bool apply_diff(InterestScopeSet& scope) const;
    /// Alias for `apply_diff` — returns true when the scope snapshot changed.
    [[nodiscard]] bool apply_to(InterestScopeSet& scope) const;
    /// Apply enter/leave and clear this diff — returns true when scope changed.
    [[nodiscard]] bool apply_and_clear(InterestScopeSet& scope);
    /// Alias for `apply_and_clear`.
    [[nodiscard]] bool apply_to_and_clear(InterestScopeSet& scope);

    [[nodiscard]] bool has_enters() const { return !entered.empty(); }
    [[nodiscard]] bool has_leaves() const { return !left.empty(); }
};

[[nodiscard]] bool is_empty_interest_diff(const InterestSetDiff& diff);
[[nodiscard]] bool has_scope_enters(const InterestSetDiff& diff);
[[nodiscard]] bool has_scope_leaves(const InterestSetDiff& diff);
[[nodiscard]] u32 count_scope_diff_entities(const InterestSetDiff& diff);
void clear_interest_diff(InterestSetDiff& diff);
/// Apply `diff` to `scope` and clear `diff`. Returns true when scope changed.
[[nodiscard]] bool apply_interest_diff(InterestSetDiff& diff, InterestScopeSet& scope);
/// True when `diff` is non-empty and at least one enter/leave would modify `scope`.
[[nodiscard]] bool can_apply_interest_diff(const InterestSetDiff& diff, const InterestScopeSet& scope);
/// Preflight diff apply without mutating scope (B7.4 deepen follow-up).
[[nodiscard]] InterestDiffPreflight preflight_interest_diff(const InterestSetDiff& diff,
                                                              const InterestScopeSet& scope);
/// True when diff apply should be skipped (empty or redundant non-empty diff).
[[nodiscard]] bool should_skip_interest_diff_apply(const InterestSetDiff& diff, const InterestScopeSet& scope);
/// True when relevance and always-relevant radii are both zero (radius filter early-out).
[[nodiscard]] bool relevance_radii_disabled(const InterestPolicy& policy);
/// Preflight radius filter/count without building entry rows (B7.4 deepen follow-up).
[[nodiscard]] RadiusFilterPreflight preflight_radius_filter(const InterestPolicy& policy,
                                                            const std::vector<InterestCandidate>& candidates);
/// Preflight radius filter/count with prior scope hysteresis (B7.4 deepen follow-up).
[[nodiscard]] RadiusFilterPreflight preflight_radius_filter(const InterestPolicy& policy,
                                                            const std::vector<InterestCandidate>& candidates,
                                                            const InterestScopeSet& prior_scope);
/// True when radius filter/count would early-out without scanning candidates.
[[nodiscard]] bool should_skip_radius_filter(const InterestPolicy& policy,
                                             const std::vector<InterestCandidate>& candidates);
/// True when hysteresis radius filter/count would early-out without scanning candidates.
[[nodiscard]] bool should_skip_radius_filter(const InterestPolicy& policy,
                                             const std::vector<InterestCandidate>& candidates,
                                             const InterestScopeSet& prior_scope);

/// Returns true when `out` is non-empty.
[[nodiscard]] bool diff_interest_scope_sets(const InterestScopeSet& previous, const InterestScopeSet& current,
                                            InterestSetDiff& out);

/// Count in-scope candidates without building entry rows (no hysteresis).
[[nodiscard]] u32 count_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                               const std::vector<InterestCandidate>& candidates);

/// Count in-scope candidates using prior scope for unload hysteresis.
[[nodiscard]] u32 count_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                               const std::vector<InterestCandidate>& candidates,
                                               const InterestScopeSet& prior_scope);

/// Radius filter stub — collects in-scope entries for an observer (no hysteresis).
[[nodiscard]] u32 filter_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                                const std::vector<InterestCandidate>& candidates,
                                                std::vector<InterestEntry>& out_entries);

/// Radius filter with prior scope for unload hysteresis.
[[nodiscard]] u32 filter_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                                const std::vector<InterestCandidate>& candidates,
                                                const InterestScopeSet& prior_scope,
                                                std::vector<InterestEntry>& out_entries);

/// Server-side interest manager — evaluates AOI for an observer against registered entities.
class InterestManager {
public:
    void set_policy(InterestPolicy policy);
    [[nodiscard]] const InterestPolicy& policy() const { return m_policy; }

    void set_observer_position(ecs::vec3 position);
    [[nodiscard]] ecs::vec3 observer_position() const { return m_observer; }

    /// Returns false when `candidate.entity` is already registered.
    [[nodiscard]] bool register_entity(InterestCandidate candidate);
    /// Remove a registered entity — returns false when the entity is not registered.
    [[nodiscard]] bool unregister_entity(ecs::EntityID entity);
    /// True when `entity` is present in the registered candidate list.
    [[nodiscard]] bool is_entity_registered(ecs::EntityID entity) const;
    /// Update a registered entity position. Returns false when the entity is not registered.
    [[nodiscard]] bool update_entity_position(ecs::EntityID entity, ecs::vec3 position);
    void clear_entities();

    /// Recompute scope/priority for all registered entities.
    void evaluate();
    /// Recompute scope and fill `out` with enter/leave vs the previous evaluation.
    /// Returns true when `out` is non-empty.
    [[nodiscard]] bool evaluate_and_diff(InterestSetDiff& out);

    [[nodiscard]] const std::vector<InterestCandidate>& candidates() const { return m_candidates; }
    [[nodiscard]] const std::vector<InterestEntry>& entries() const { return m_entries; }
    [[nodiscard]] u32 in_scope_count() const;
    /// Query scope for one entity after the latest `evaluate()` call.
    [[nodiscard]] bool is_entity_in_scope(ecs::EntityID entity) const;

    /// Count registered candidates in scope for the current observer (uses prior scope hysteresis).
    [[nodiscard]] u32 count_registered_in_radius() const;
    /// True when at least one registered candidate is in scope for the current observer.
    [[nodiscard]] bool has_any_registered_in_radius() const;
    /// Filter registered candidates in scope for the current observer (uses prior scope hysteresis).
    [[nodiscard]] u32 filter_registered_in_radius(std::vector<InterestEntry>& out_entries) const;
    /// Preflight registered radius filter/count without building entry rows (B7.4 deepen follow-up).
    [[nodiscard]] RadiusFilterPreflight preflight_registered_in_radius() const;

    /// Snapshot of in-scope entities from the last `evaluate()` call.
    [[nodiscard]] const InterestScopeSet& scope_set() const { return m_scope_set; }
    /// In-scope entities from the evaluation before the latest `evaluate()` call.
    [[nodiscard]] const InterestScopeSet& previous_scope_set() const { return m_previous_scope_set; }

    /// Diff current scope against the previous evaluation's scope set.
    /// Returns true when `out` is non-empty.
    [[nodiscard]] bool compute_scope_diff(InterestSetDiff& out) const;
    /// True when the last two `evaluate()` calls produced different in-scope sets.
    [[nodiscard]] bool scope_changed_since_last_evaluate() const;

private:
    InterestPolicy m_policy{};
    ecs::vec3 m_observer{};
    std::vector<InterestCandidate> m_candidates;
    std::vector<InterestEntry> m_entries;
    std::vector<bool> m_was_in_scope;
    InterestScopeSet m_scope_set;
    InterestScopeSet m_previous_scope_set;
    bool m_has_scope_snapshot = false;
};

/// Max-priority replication queue — highest priority popped first (Torque ghost ordering stub).
class InterestPriorityQueue {
public:
    void push(InterestEntry entry);
    [[nodiscard]] bool pop(InterestEntry& out);
    void clear();
    [[nodiscard]] u32 size() const { return static_cast<u32>(m_heap.size()); }
    [[nodiscard]] bool empty() const { return m_heap.empty(); }

    /// Enqueue all in-scope entries from a manager evaluation.
    void build_from_manager(const InterestManager& manager);

private:
    std::vector<InterestEntry> m_heap;

    static bool higher_priority_(const InterestEntry& a, const InterestEntry& b);
    void heapify_up_(u32 index);
    void heapify_down_(u32 index);
};

} // namespace fuse::net
