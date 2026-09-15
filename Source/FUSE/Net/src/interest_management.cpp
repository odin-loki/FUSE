#include <fuse/net/interest_management.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::net {

namespace {

constexpr f32 kDefaultUnloadScale = 1.25f;
constexpr f32 kMaxPriority = 1.f;

f32 clamp_radius_(f32 radius) {
    return radius > 0.f ? radius : 0.f;
}

f32 radius_sq_(f32 radius) {
    const f32 clamped = clamp_radius_(radius);
    return clamped * clamped;
}

} // namespace

f32 distance_sq_3d(const ecs::vec3& a, const ecs::vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

f32 effective_relevance_radius(const InterestPolicy& policy) {
    return clamp_radius_(policy.relevance_radius);
}

f32 effective_unload_radius(const InterestPolicy& policy) {
    if (policy.unload_radius > 0.f) {
        return policy.unload_radius;
    }
    return effective_relevance_radius(policy) * kDefaultUnloadScale;
}

f32 effective_always_relevant_radius(const InterestPolicy& policy) {
    const f32 always = clamp_radius_(policy.always_relevant_radius);
    const f32 relevance = effective_relevance_radius(policy);
    return always > relevance ? relevance : always;
}

InterestScope classify_interest(f32 distance_sq, const InterestPolicy& policy, bool was_in_scope) {
    const f32 always_sq = radius_sq_(effective_always_relevant_radius(policy));
    const f32 relevance_sq = radius_sq_(effective_relevance_radius(policy));
    const f32 unload_sq = radius_sq_(effective_unload_radius(policy));

    if (distance_sq <= always_sq) {
        return InterestScope::AlwaysRelevant;
    }
    if (distance_sq <= relevance_sq) {
        return InterestScope::InScope;
    }
    if (was_in_scope && distance_sq <= unload_sq) {
        return InterestScope::InScope;
    }
    return InterestScope::OutOfScope;
}

bool within_relevance_radius(f32 distance_sq, const InterestPolicy& policy, bool was_in_scope) {
    return classify_interest(distance_sq, policy, was_in_scope) != InterestScope::OutOfScope;
}

namespace {

bool contains_entity_(const std::vector<ecs::EntityID>& entities, ecs::EntityID entity) {
    for (const ecs::EntityID& candidate : entities) {
        if (candidate == entity) {
            return true;
        }
    }
    return false;
}

} // namespace

void InterestScopeSet::clear() {
    entities.clear();
}

void InterestScopeSet::build_from_entries(const std::vector<InterestEntry>& entries) {
    entities.clear();
    for (const InterestEntry& entry : entries) {
        if (entry.scope != InterestScope::OutOfScope) {
            entities.push_back(entry.entity);
        }
    }
}

void diff_interest_scope_sets(const InterestScopeSet& previous, const InterestScopeSet& current,
                              InterestSetDiff& out) {
    out.entered.clear();
    out.left.clear();

    for (const ecs::EntityID& entity : current.entities) {
        if (!contains_entity_(previous.entities, entity)) {
            out.entered.push_back(entity);
        }
    }

    for (const ecs::EntityID& entity : previous.entities) {
        if (!contains_entity_(current.entities, entity)) {
            out.left.push_back(entity);
        }
    }
}

u32 filter_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                const std::vector<InterestCandidate>& candidates,
                                std::vector<InterestEntry>& out_entries) {
    out_entries.clear();
    for (const InterestCandidate& candidate : candidates) {
        const f32 distance_sq = distance_sq_3d(observer, candidate.position);
        if (!within_relevance_radius(distance_sq, policy)) {
            continue;
        }

        InterestEntry entry{};
        entry.entity = candidate.entity;
        entry.distance_sq = distance_sq;
        entry.scope = classify_interest(distance_sq, policy);
        entry.priority = compute_relevance_priority(distance_sq, policy, candidate.priority_boost);
        out_entries.push_back(entry);
    }

    return static_cast<u32>(out_entries.size());
}

f32 compute_relevance_priority(f32 distance_sq, const InterestPolicy& policy, f32 priority_boost) {
    const InterestScope scope = classify_interest(distance_sq, policy, false);
    if (scope == InterestScope::OutOfScope) {
        return 0.f;
    }
    if (scope == InterestScope::AlwaysRelevant) {
        return kMaxPriority + priority_boost;
    }

    const f32 relevance_sq = radius_sq_(effective_relevance_radius(policy));
    if (relevance_sq <= 0.f) {
        return priority_boost;
    }

    const f32 normalized = (relevance_sq - distance_sq) / relevance_sq;
    return std::max(0.f, normalized) + priority_boost;
}

void InterestManager::set_policy(InterestPolicy policy) {
    m_policy = policy;
}

void InterestManager::set_observer_position(ecs::vec3 position) {
    m_observer = position;
}

void InterestManager::register_entity(InterestCandidate candidate) {
    m_candidates.push_back(candidate);
}

void InterestManager::clear_entities() {
    m_candidates.clear();
    m_entries.clear();
    m_was_in_scope.clear();
    m_scope_set.clear();
    m_previous_scope_set.clear();
    m_has_scope_snapshot = false;
}

void InterestManager::evaluate() {
    if (m_entries.size() != m_candidates.size()) {
        m_entries.assign(m_candidates.size(), InterestEntry{});
        m_was_in_scope.assign(m_candidates.size(), false);
    }

    for (u32 i = 0; i < m_candidates.size(); ++i) {
        const InterestCandidate& candidate = m_candidates[i];
        const f32 distance_sq = distance_sq_3d(m_observer, candidate.position);
        const InterestScope scope =
            classify_interest(distance_sq, m_policy, m_was_in_scope[i]);

        InterestEntry& entry = m_entries[i];
        entry.entity = candidate.entity;
        entry.distance_sq = distance_sq;
        entry.scope = scope;
        entry.priority = compute_relevance_priority(distance_sq, m_policy, candidate.priority_boost);
        m_was_in_scope[i] = scope != InterestScope::OutOfScope;
    }

    if (m_has_scope_snapshot) {
        m_previous_scope_set = m_scope_set;
    }
    m_scope_set.build_from_entries(m_entries);
    if (!m_has_scope_snapshot) {
        m_previous_scope_set = m_scope_set;
        m_has_scope_snapshot = true;
    }
}

void InterestManager::compute_scope_diff(InterestSetDiff& out) const {
    diff_interest_scope_sets(m_previous_scope_set, m_scope_set, out);
}

u32 InterestManager::in_scope_count() const {
    u32 count = 0;
    for (const InterestEntry& entry : m_entries) {
        if (entry.scope != InterestScope::OutOfScope) {
            ++count;
        }
    }
    return count;
}

bool InterestPriorityQueue::higher_priority_(const InterestEntry& a, const InterestEntry& b) {
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    return a.distance_sq < b.distance_sq;
}

void InterestPriorityQueue::push(InterestEntry entry) {
    if (entry.scope == InterestScope::OutOfScope || entry.priority <= 0.f) {
        return;
    }

    m_heap.push_back(entry);
    heapify_up_(static_cast<u32>(m_heap.size() - 1));
}

bool InterestPriorityQueue::pop(InterestEntry& out) {
    if (m_heap.empty()) {
        return false;
    }

    out = m_heap.front();
    if (m_heap.size() == 1) {
        m_heap.clear();
        return true;
    }

    m_heap.front() = m_heap.back();
    m_heap.pop_back();
    heapify_down_(0);
    return true;
}

void InterestPriorityQueue::clear() {
    m_heap.clear();
}

void InterestPriorityQueue::build_from_manager(const InterestManager& manager) {
    clear();
    for (const InterestEntry& entry : manager.entries()) {
        push(entry);
    }
}

void InterestPriorityQueue::heapify_up_(u32 index) {
    while (index > 0) {
        const u32 parent = (index - 1) / 2;
        if (!higher_priority_(m_heap[index], m_heap[parent])) {
            break;
        }
        std::swap(m_heap[index], m_heap[parent]);
        index = parent;
    }
}

void InterestPriorityQueue::heapify_down_(u32 index) {
    const u32 count = static_cast<u32>(m_heap.size());
    while (true) {
        const u32 left = index * 2 + 1;
        const u32 right = left + 1;
        u32 best = index;

        if (left < count && higher_priority_(m_heap[left], m_heap[best])) {
            best = left;
        }
        if (right < count && higher_priority_(m_heap[right], m_heap[best])) {
            best = right;
        }
        if (best == index) {
            break;
        }

        std::swap(m_heap[index], m_heap[best]);
        index = best;
    }
}

} // namespace fuse::net
