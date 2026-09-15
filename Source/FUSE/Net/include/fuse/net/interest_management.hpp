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

/// Server-side interest manager — evaluates AOI for an observer against registered entities.
class InterestManager {
public:
    void set_policy(InterestPolicy policy);
    [[nodiscard]] const InterestPolicy& policy() const { return m_policy; }

    void set_observer_position(ecs::vec3 position);
    [[nodiscard]] ecs::vec3 observer_position() const { return m_observer; }

    void register_entity(InterestCandidate candidate);
    void clear_entities();

    /// Recompute scope/priority for all registered entities.
    void evaluate();

    [[nodiscard]] const std::vector<InterestEntry>& entries() const { return m_entries; }
    [[nodiscard]] u32 in_scope_count() const;

private:
    InterestPolicy m_policy{};
    ecs::vec3 m_observer{};
    std::vector<InterestCandidate> m_candidates;
    std::vector<InterestEntry> m_entries;
    std::vector<bool> m_was_in_scope;
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
