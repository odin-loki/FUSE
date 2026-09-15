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

bool entity_less_(const ecs::EntityID& a, const ecs::EntityID& b) {
    if (a.index != b.index) {
        return a.index < b.index;
    }
    return a.generation < b.generation;
}

void sort_entities_(std::vector<ecs::EntityID>& entities) {
    std::sort(entities.begin(), entities.end(), entity_less_);
}

bool entry_higher_priority_(const InterestEntry& a, const InterestEntry& b) {
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    return a.distance_sq < b.distance_sq;
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
    sort_entities_(entities);
}

bool InterestScopeSet::contains(ecs::EntityID entity) const {
    return contains_entity_(entities, entity);
}

bool InterestScopeSet::insert_entity(ecs::EntityID entity) {
    if (contains(entity)) {
        return false;
    }
    entities.push_back(entity);
    sort_entities_(entities);
    return true;
}

bool InterestScopeSet::remove_entity(ecs::EntityID entity) {
    for (u32 i = 0; i < entities.size(); ++i) {
        if (entities[i] == entity) {
            entities.erase(entities.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool InterestScopeSet::equal_to(const InterestScopeSet& other) const {
    if (entities.size() != other.entities.size()) {
        return false;
    }
    for (u32 i = 0; i < entities.size(); ++i) {
        if (entities[i] != other.entities[i]) {
            return false;
        }
    }
    return true;
}

void InterestSetDiff::clear() {
    entered.clear();
    left.clear();
}

bool InterestSetDiff::apply_diff(InterestScopeSet& scope) const {
    if (empty()) {
        return false;
    }

    bool changed = false;
    for (const ecs::EntityID& entity : left) {
        if (scope.remove_entity(entity)) {
            changed = true;
        }
    }
    for (const ecs::EntityID& entity : entered) {
        if (scope.insert_entity(entity)) {
            changed = true;
        }
    }
    return changed;
}

InterestSetDiff make_empty_interest_diff() {
    return InterestSetDiff{};
}

void clear_interest_diff(InterestSetDiff& diff) {
    diff.clear();
}

bool is_empty_interest_diff(const InterestSetDiff& diff) {
    return diff.empty();
}

bool has_scope_enters(const InterestSetDiff& diff) {
    return diff.has_enters();
}

bool has_scope_leaves(const InterestSetDiff& diff) {
    return diff.has_leaves();
}

u32 count_scope_diff_entities(const InterestSetDiff& diff) {
    return static_cast<u32>(diff.entered.size() + diff.left.size());
}

bool diff_interest_scope_sets(const InterestScopeSet& previous, const InterestScopeSet& current,
                              InterestSetDiff& out) {
    out.clear();

    if (previous.equal_to(current)) {
        return false;
    }

    if (previous.empty() && current.empty()) {
        return false;
    }

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

    sort_entities_(out.entered);
    sort_entities_(out.left);
    return !out.empty();
}

u32 count_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                               const std::vector<InterestCandidate>& candidates) {
    if (candidates.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const InterestCandidate& candidate : candidates) {
        const f32 distance_sq = distance_sq_3d(observer, candidate.position);
        if (within_relevance_radius(distance_sq, policy)) {
            ++count;
        }
    }
    return count;
}

u32 count_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                               const std::vector<InterestCandidate>& candidates,
                               const InterestScopeSet& prior_scope) {
    if (candidates.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const InterestCandidate& candidate : candidates) {
        const f32 distance_sq = distance_sq_3d(observer, candidate.position);
        const bool was_in_scope = prior_scope.contains(candidate.entity);
        if (within_relevance_radius(distance_sq, policy, was_in_scope)) {
            ++count;
        }
    }
    return count;
}

u32 filter_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                const std::vector<InterestCandidate>& candidates,
                                std::vector<InterestEntry>& out_entries) {
    out_entries.clear();
    if (candidates.empty()) {
        return 0;
    }

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

    std::sort(out_entries.begin(), out_entries.end(), entry_higher_priority_);
    return static_cast<u32>(out_entries.size());
}

u32 filter_candidates_in_radius(const ecs::vec3& observer, const InterestPolicy& policy,
                                const std::vector<InterestCandidate>& candidates,
                                const InterestScopeSet& prior_scope,
                                std::vector<InterestEntry>& out_entries) {
    out_entries.clear();
    if (candidates.empty()) {
        return 0;
    }

    for (const InterestCandidate& candidate : candidates) {
        const f32 distance_sq = distance_sq_3d(observer, candidate.position);
        const bool was_in_scope = prior_scope.contains(candidate.entity);
        if (!within_relevance_radius(distance_sq, policy, was_in_scope)) {
            continue;
        }

        InterestEntry entry{};
        entry.entity = candidate.entity;
        entry.distance_sq = distance_sq;
        entry.scope = classify_interest(distance_sq, policy, was_in_scope);
        entry.priority = compute_relevance_priority(distance_sq, policy, candidate.priority_boost);
        out_entries.push_back(entry);
    }

    std::sort(out_entries.begin(), out_entries.end(), entry_higher_priority_);
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

bool InterestManager::register_entity(InterestCandidate candidate) {
    for (const InterestCandidate& existing : m_candidates) {
        if (existing.entity == candidate.entity) {
            return false;
        }
    }
    m_candidates.push_back(candidate);
    return true;
}

bool InterestManager::unregister_entity(ecs::EntityID entity) {
    for (u32 i = 0; i < m_candidates.size(); ++i) {
        if (m_candidates[i].entity != entity) {
            continue;
        }

        m_candidates.erase(m_candidates.begin() + static_cast<std::ptrdiff_t>(i));
        if (i < m_entries.size()) {
            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (i < m_was_in_scope.size()) {
            m_was_in_scope.erase(m_was_in_scope.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return true;
    }
    return false;
}

bool InterestManager::is_entity_registered(ecs::EntityID entity) const {
    for (const InterestCandidate& candidate : m_candidates) {
        if (candidate.entity == entity) {
            return true;
        }
    }
    return false;
}

bool InterestManager::update_entity_position(ecs::EntityID entity, ecs::vec3 position) {
    for (InterestCandidate& candidate : m_candidates) {
        if (candidate.entity == entity) {
            candidate.position = position;
            return true;
        }
    }
    return false;
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

bool InterestManager::evaluate_and_diff(InterestSetDiff& out) {
    evaluate();
    return compute_scope_diff(out);
}

bool InterestManager::compute_scope_diff(InterestSetDiff& out) const {
    if (!scope_changed_since_last_evaluate()) {
        out.clear();
        return false;
    }
    return diff_interest_scope_sets(m_previous_scope_set, m_scope_set, out);
}

bool InterestManager::scope_changed_since_last_evaluate() const {
    if (!m_has_scope_snapshot) {
        return false;
    }
    return !m_previous_scope_set.equal_to(m_scope_set);
}

bool InterestManager::is_entity_in_scope(ecs::EntityID entity) const {
    return m_scope_set.contains(entity);
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

u32 InterestManager::count_registered_in_radius() const {
    return count_candidates_in_radius(m_observer, m_policy, m_candidates, m_previous_scope_set);
}

bool InterestManager::has_registered_in_radius() const {
    return count_registered_in_radius() > 0;
}

u32 InterestManager::filter_registered_in_radius(std::vector<InterestEntry>& out_entries) const {
    return filter_candidates_in_radius(m_observer, m_policy, m_candidates, m_previous_scope_set,
                                       out_entries);
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
