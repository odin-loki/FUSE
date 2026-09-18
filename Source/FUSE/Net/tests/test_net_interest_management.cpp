#include <fuse/ecs/entity.hpp>
#include <fuse/net/interest_management.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

namespace {

fuse::ecs::EntityID make_entity(fuse::u32 index) {
    fuse::ecs::EntityID entity{};
    entity.index = index;
    entity.generation = 1;
    return entity;
}

} // namespace

void run_interest_management_tests() {
    fuse::net::InterestPolicy policy{};
    policy.relevance_radius = 100.f;
    policy.always_relevant_radius = 10.f;

    expectNear(fuse::net::effective_relevance_radius(policy), 100.f, 1e-4f,
               "effective relevance radius uses policy value");
    expectNear(fuse::net::effective_unload_radius(policy), 125.f, 1e-4f,
               "default unload radius is 1.25x relevance");
    expectNear(fuse::net::effective_always_relevant_radius(policy), 10.f, 1e-4f,
               "always-relevant radius clamped below relevance");

    policy.unload_radius = 200.f;
    expectNear(fuse::net::effective_unload_radius(policy), 200.f, 1e-4f,
               "explicit unload radius overrides default scale");

    const fuse::ecs::vec3 origin{0.f, 0.f, 0.f, 0.f};
    const fuse::ecs::vec3 near_pos{5.f, 0.f, 0.f, 0.f};
    const fuse::ecs::vec3 mid_pos{50.f, 0.f, 0.f, 0.f};
    const fuse::ecs::vec3 far_pos{150.f, 0.f, 0.f, 0.f};

    const f32 near_sq = fuse::net::distance_sq_3d(origin, near_pos);
    const f32 mid_sq = fuse::net::distance_sq_3d(origin, mid_pos);
    const f32 far_sq = fuse::net::distance_sq_3d(origin, far_pos);

    policy.unload_radius = 0.f;
    expectTrue(fuse::net::classify_interest(near_sq, policy) ==
                   fuse::net::InterestScope::AlwaysRelevant,
               "observer-near entity is always relevant");
    expectTrue(fuse::net::classify_interest(mid_sq, policy) == fuse::net::InterestScope::InScope,
               "mid-range entity is in scope");
    expectTrue(fuse::net::classify_interest(far_sq, policy) == fuse::net::InterestScope::OutOfScope,
               "far entity is out of scope");

    const f32 hysteresis_sq = fuse::net::distance_sq_3d(origin, {110.f, 0.f, 0.f, 0.f});
    expectTrue(fuse::net::classify_interest(hysteresis_sq, policy, false) ==
                   fuse::net::InterestScope::OutOfScope,
               "beyond relevance without prior scope stays out");
    expectTrue(fuse::net::classify_interest(hysteresis_sq, policy, true) ==
                   fuse::net::InterestScope::InScope,
               "hysteresis keeps entity in scope until unload radius");

    const f32 priority_near = fuse::net::compute_relevance_priority(near_sq, policy);
    const f32 priority_mid = fuse::net::compute_relevance_priority(mid_sq, policy);
    const f32 priority_far = fuse::net::compute_relevance_priority(far_sq, policy);
    expectTrue(priority_near > priority_mid, "closer entities get higher priority");
    expectTrue(priority_mid > priority_far, "mid priority beats out-of-scope zero");
    expectNear(priority_far, 0.f, 1e-4f, "out-of-scope priority is zero");

    fuse::net::InterestManager manager;
    manager.set_policy(policy);
    manager.set_observer_position(origin);

    manager.register_entity({make_entity(1), near_pos, 0.f});
    manager.register_entity({make_entity(2), mid_pos, 0.1f});
    manager.register_entity({make_entity(3), far_pos, 0.f});
    manager.evaluate();

    expectTrue(manager.in_scope_count() == 2u, "manager counts in-scope entities");
    expectTrue(manager.entries()[0].scope == fuse::net::InterestScope::AlwaysRelevant,
               "first candidate classified always relevant");
    expectTrue(manager.entries()[1].priority > manager.entries()[2].priority,
               "in-scope entry outranks out-of-scope entry");

    fuse::net::InterestPriorityQueue queue;
    queue.build_from_manager(manager);
    expectTrue(queue.size() == manager.in_scope_count(), "queue size matches in-scope count");

    fuse::net::InterestEntry first{};
    fuse::net::InterestEntry second{};
    expectTrue(queue.pop(first), "queue pop succeeds");
    expectTrue(queue.pop(second), "queue pop succeeds twice");
    expectTrue(first.priority >= second.priority, "queue pops highest priority first");
    expectTrue(first.entity.index != second.entity.index, "queue pops distinct entities");
    expectTrue(queue.empty(), "queue drains after two in-scope entities");

    manager.set_observer_position({200.f, 0.f, 0.f, 0.f});
    manager.evaluate();
    expectTrue(manager.in_scope_count() == 1u,
               "moving observer drops distant entities unless hysteresis applies");

    manager.clear_entities();
    manager.set_observer_position(origin);
    manager.register_entity({make_entity(10), {95.f, 0.f, 0.f, 0.f}, 0.f});
    manager.evaluate();
    expectTrue(manager.in_scope_count() == 1u, "entity within relevance radius is scoped");

    manager.register_entity({make_entity(11), {110.f, 0.f, 0.f, 0.f}, 0.f});
    manager.evaluate();
    expectTrue(manager.entries()[0].scope == fuse::net::InterestScope::InScope,
               "hysteresis keeps entity in scope past relevance radius");
    expectTrue(manager.entries()[1].scope == fuse::net::InterestScope::OutOfScope,
               "never-scoped entity stays out beyond relevance radius");

    queue.clear();
    fuse::net::InterestEntry low{};
    low.entity = make_entity(20);
    low.priority = 0.2f;
    low.scope = fuse::net::InterestScope::InScope;
    fuse::net::InterestEntry high = low;
    high.entity = make_entity(21);
    high.priority = 0.9f;
    fuse::net::InterestEntry mid = low;
    mid.entity = make_entity(22);
    mid.priority = 0.5f;

    queue.push(low);
    queue.push(high);
    queue.push(mid);
    expectTrue(queue.size() == 3u, "manual pushes retained");

    fuse::net::InterestEntry popped{};
    expectTrue(queue.pop(popped), "manual pop succeeds");
    expectTrue(popped.entity.index == 21u, "highest manual priority popped first");
    expectTrue(queue.pop(popped), "second pop succeeds");
    expectTrue(popped.entity.index == 22u, "mid priority popped second");
    expectTrue(queue.pop(popped), "third pop succeeds");
    expectTrue(popped.entity.index == 20u, "lowest priority popped last");
    expectTrue(queue.empty(), "queue empty after drain");

    fuse::net::InterestEntry ignored{};
    ignored.scope = fuse::net::InterestScope::OutOfScope;
    ignored.priority = 1.f;
    queue.push(ignored);
    expectTrue(queue.empty(), "out-of-scope entries are not enqueued");

    // --- radius filter stub ---
    policy.relevance_radius = 50.f;
    policy.always_relevant_radius = 5.f;
    policy.unload_radius = 0.f;

    std::vector<fuse::net::InterestCandidate> candidates;
    candidates.push_back({make_entity(30), {10.f, 0.f, 0.f, 0.f}, 0.f});
    candidates.push_back({make_entity(31), {80.f, 0.f, 0.f, 0.f}, 0.f});
    candidates.push_back({make_entity(32), {3.f, 0.f, 0.f, 0.f}, 0.2f});

    std::vector<fuse::net::InterestEntry> filtered;
    const fuse::u32 filtered_count =
        fuse::net::filter_candidates_in_radius(origin, policy, candidates, filtered);
    expectTrue(filtered_count == 2u, "radius filter keeps in-scope candidates only");
    expectTrue(filtered.size() == 2u, "radius filter output size matches count");
    expectTrue(filtered[0].entity.index == 30u || filtered[0].entity.index == 32u,
               "radius filter retains near entities");
    expectTrue(!fuse::net::within_relevance_radius(
                   fuse::net::distance_sq_3d(origin, {80.f, 0.f, 0.f, 0.f}), policy),
               "within_relevance_radius rejects far entities");

    std::vector<fuse::net::InterestCandidate> empty_candidates;
    std::vector<fuse::net::InterestEntry> empty_filtered;
    expectTrue(fuse::net::filter_candidates_in_radius(origin, policy, empty_candidates,
                                                      empty_filtered) == 0u,
               "radius filter returns zero for empty candidate list");
    expectTrue(empty_filtered.empty(), "radius filter clears output on empty input");

    // --- enter/leave set diff ---
    fuse::net::InterestManager diff_manager;
    diff_manager.set_policy(policy);
    diff_manager.set_observer_position(origin);
    diff_manager.register_entity({make_entity(40), {10.f, 0.f, 0.f, 0.f}, 0.f});
    diff_manager.register_entity({make_entity(41), {20.f, 0.f, 0.f, 0.f}, 0.f});
    diff_manager.evaluate();
    expectTrue(diff_manager.scope_set().size() == 2u, "initial scope set has two entities");

    fuse::net::InterestSetDiff first_diff{};
    diff_manager.compute_scope_diff(first_diff);
    expectTrue(first_diff.entered.empty() && first_diff.left.empty(),
               "first evaluation produces empty enter/leave diff");

    diff_manager.set_observer_position({200.f, 0.f, 0.f, 0.f});
    diff_manager.evaluate();
    fuse::net::InterestSetDiff move_diff{};
    diff_manager.compute_scope_diff(move_diff);
    expectTrue(move_diff.entered.empty(), "moving away does not enter new entities");
    expectTrue(move_diff.left.size() == 2u, "moving away leaves prior in-scope entities");

    diff_manager.clear_entities();
    diff_manager.set_observer_position(origin);
    diff_manager.register_entity({make_entity(50), {10.f, 0.f, 0.f, 0.f}, 0.f});
    diff_manager.evaluate();
    diff_manager.register_entity({make_entity(51), {15.f, 0.f, 0.f, 0.f}, 0.f});
    diff_manager.evaluate();
    fuse::net::InterestSetDiff add_diff{};
    diff_manager.compute_scope_diff(add_diff);
    expectTrue(add_diff.entered.size() == 1u, "new in-scope entity appears in entered set");
    expectTrue(add_diff.entered[0].index == 51u, "entered set identifies new entity");
    expectTrue(add_diff.left.empty(), "adding entity does not leave prior scope");

    fuse::net::InterestScopeSet manual_prev;
    fuse::net::InterestScopeSet manual_curr;
    fuse::net::InterestSetDiff manual_diff{};
    manual_prev.build_from_entries(diff_manager.entries());
    diff_manager.set_observer_position({200.f, 0.f, 0.f, 0.f});
    diff_manager.evaluate();
    manual_curr.build_from_entries(diff_manager.entries());
    fuse::net::diff_interest_scope_sets(manual_prev, manual_curr, manual_diff);
    expectTrue(manual_diff.left.size() == 2u, "manual diff detects entities leaving scope");
    expectTrue(manual_diff.entered.empty(), "manual diff has no enters when observer moves away");

    // --- priority order drain (explicit) ---
    fuse::net::InterestPriorityQueue order_queue;
    expectTrue(order_queue.empty(), "new priority queue starts empty");
    expectTrue(!order_queue.pop(popped), "pop on empty queue returns false");

    fuse::net::InterestEntry tie_near{};
    tie_near.entity = make_entity(60);
    tie_near.distance_sq = 10.f;
    tie_near.priority = 0.5f;
    tie_near.scope = fuse::net::InterestScope::InScope;
    fuse::net::InterestEntry tie_far = tie_near;
    tie_far.entity = make_entity(61);
    tie_far.distance_sq = 50.f;
    order_queue.push(tie_far);
    order_queue.push(tie_near);

    fuse::net::InterestEntry tie_first{};
    fuse::net::InterestEntry tie_second{};
    expectTrue(order_queue.pop(tie_first), "priority tie-break pop succeeds");
    expectTrue(order_queue.pop(tie_second), "priority tie-break second pop succeeds");
    expectTrue(tie_first.entity.index == 60u, "equal priority prefers closer entity");
    expectTrue(order_queue.empty(), "queue empty after tie-break drain");

    // --- empty scope set ---
    fuse::net::InterestManager empty_manager;
    empty_manager.set_policy(policy);
    empty_manager.set_observer_position(origin);
    empty_manager.register_entity({make_entity(70), {500.f, 0.f, 0.f, 0.f}, 0.f});
    empty_manager.register_entity({make_entity(71), {600.f, 0.f, 0.f, 0.f}, 0.f});
    empty_manager.evaluate();
    expectTrue(empty_manager.scope_set().empty(), "all far entities produce empty scope set");
    expectTrue(empty_manager.in_scope_count() == 0u, "empty scope has zero in-scope count");
    expectTrue(!empty_manager.is_entity_in_scope(make_entity(70)),
               "is_entity_in_scope returns false for out-of-scope entity");

    fuse::net::InterestSetDiff empty_first_diff{};
    empty_manager.compute_scope_diff(empty_first_diff);
    expectTrue(empty_first_diff.empty(), "first empty-scope evaluation diff is empty");

    empty_manager.set_observer_position({490.f, 0.f, 0.f, 0.f});
    empty_manager.evaluate();
    fuse::net::InterestSetDiff empty_to_scope_diff{};
    empty_manager.compute_scope_diff(empty_to_scope_diff);
    expectTrue(empty_to_scope_diff.entered.size() == 1u,
               "moving observer into range enters one entity");
    expectTrue(empty_to_scope_diff.entered[0].index == 70u,
               "entered entity is nearest to new observer position");
    expectTrue(empty_to_scope_diff.left.empty(), "empty-to-scoped diff has no leaves");
    expectTrue(empty_manager.scope_changed_since_last_evaluate(),
               "scope_changed_since_last_evaluate detects enter");

    // --- scope set contains + sorted diff output ---
    fuse::net::InterestScopeSet scope_a;
    fuse::net::InterestScopeSet scope_b;
    scope_a.entities = {make_entity(3), make_entity(1), make_entity(2)};
    scope_b.entities = {make_entity(4), make_entity(2)};

    fuse::net::InterestSetDiff sorted_diff{};
    fuse::net::diff_interest_scope_sets(scope_a, scope_b, sorted_diff);
    expectTrue(sorted_diff.entered.size() == 1u && sorted_diff.entered[0].index == 4u,
               "diff entered set is sorted and contains new entity");
    expectTrue(sorted_diff.left.size() == 2u && sorted_diff.left[0].index == 1u &&
                   sorted_diff.left[1].index == 3u,
               "diff left set is sorted by entity index");

    expectTrue(scope_b.contains(make_entity(2)), "scope set contains reports in-scope entity");
    expectTrue(!scope_b.contains(make_entity(99)), "scope set contains rejects unknown entity");

    // --- update_entity_position enter/leave ---
    fuse::net::InterestManager update_manager;
    update_manager.set_policy(policy);
    update_manager.set_observer_position(origin);
    const fuse::ecs::EntityID moving_entity = make_entity(80);
    update_manager.register_entity({moving_entity, {200.f, 0.f, 0.f, 0.f}, 0.f});
    update_manager.evaluate();
    expectTrue(update_manager.scope_set().empty(), "entity starts out of scope");

    expectTrue(update_manager.update_entity_position(moving_entity, {10.f, 0.f, 0.f, 0.f}),
               "update_entity_position succeeds for registered entity");
    expectTrue(!update_manager.update_entity_position(make_entity(999), {0.f, 0.f, 0.f, 0.f}),
               "update_entity_position rejects unknown entity");
    update_manager.evaluate();
    fuse::net::InterestSetDiff position_diff{};
    update_manager.compute_scope_diff(position_diff);
    expectTrue(position_diff.entered.size() == 1u && position_diff.entered[0] == moving_entity,
               "moving entity into radius enters scope");
    expectTrue(update_manager.is_entity_in_scope(moving_entity),
               "is_entity_in_scope true after position update");

    expectTrue(update_manager.update_entity_position(moving_entity, {500.f, 0.f, 0.f, 0.f}),
               "update_entity_position can move entity back out");
    update_manager.evaluate();
    fuse::net::InterestSetDiff leave_diff{};
    update_manager.compute_scope_diff(leave_diff);
    expectTrue(leave_diff.left.size() == 1u && leave_diff.left[0] == moving_entity,
               "moving entity out of radius leaves scope");
    expectTrue(leave_diff.entered.empty(), "leave-only diff has no enters");

    // --- radius filter priority ordering + always-relevant scope ---
    std::vector<fuse::net::InterestCandidate> priority_candidates;
    priority_candidates.push_back({make_entity(90), {40.f, 0.f, 0.f, 0.f}, 0.f});
    priority_candidates.push_back({make_entity(91), {2.f, 0.f, 0.f, 0.f}, 0.5f});
    priority_candidates.push_back({make_entity(92), {4.f, 0.f, 0.f, 0.f}, 0.f});

    std::vector<fuse::net::InterestEntry> priority_filtered;
    const fuse::u32 priority_filtered_count =
        fuse::net::filter_candidates_in_radius(origin, policy, priority_candidates, priority_filtered);
    expectTrue(priority_filtered_count == 3u, "radius filter keeps all in-range candidates");
    expectTrue(priority_filtered.size() == 3u, "radius filter output size matches count");
    expectTrue(priority_filtered[0].entity.index == 91u,
               "radius filter sorts highest priority first");
    expectTrue(priority_filtered[0].scope == fuse::net::InterestScope::AlwaysRelevant,
               "radius filter marks inner-radius entities always relevant");
    expectTrue(priority_filtered[0].priority >= priority_filtered[1].priority,
               "radius filter output is priority descending");

    // --- count_candidates_in_radius ---
    expectTrue(fuse::net::count_candidates_in_radius(origin, policy, candidates) == filtered_count,
               "count_candidates_in_radius matches filter output size");
    expectTrue(fuse::net::count_candidates_in_radius(origin, policy, empty_candidates) == 0u,
               "count on empty candidate list is zero");

    fuse::net::InterestPolicy tight_policy{};
    tight_policy.relevance_radius = 6.f;
    tight_policy.always_relevant_radius = 5.f;
    std::vector<fuse::net::InterestCandidate> tight_candidates;
    tight_candidates.push_back({make_entity(100), {2.f, 0.f, 0.f, 0.f}, 0.f});
    tight_candidates.push_back({make_entity(101), {10.f, 0.f, 0.f, 0.f}, 0.f});
    expectTrue(fuse::net::count_candidates_in_radius(origin, tight_policy, tight_candidates) == 1u,
               "tight relevance radius keeps only in-sphere candidates");
    std::vector<fuse::net::InterestEntry> tight_filtered;
    expectTrue(fuse::net::filter_candidates_in_radius(origin, tight_policy, tight_candidates,
                                                      tight_filtered) == 1u,
               "tight relevance radius filter matches count");
    expectTrue(tight_filtered[0].entity.index == 100u,
               "tight relevance filter keeps nearest candidate");
    expectTrue(tight_filtered[0].scope == fuse::net::InterestScope::AlwaysRelevant,
               "tight relevance filter marks inner entity always relevant");

    // --- InterestSetDiff::apply_to ---
    fuse::net::InterestScopeSet applied_scope;
    applied_scope.entities = {make_entity(1), make_entity(2), make_entity(3)};
    fuse::net::InterestSetDiff patch_diff{};
    patch_diff.entered = {make_entity(4)};
    patch_diff.left = {make_entity(2)};
    patch_diff.apply_diff(applied_scope);
    expectTrue(applied_scope.size() == 3u, "apply_to preserves net scope size");
    expectTrue(applied_scope.contains(make_entity(1)), "apply_to keeps unchanged entities");
    expectTrue(applied_scope.contains(make_entity(3)), "apply_to keeps unchanged entities");
    expectTrue(applied_scope.contains(make_entity(4)), "apply_to inserts entered entity");
    expectTrue(!applied_scope.contains(make_entity(2)), "apply_to removes left entity");

    fuse::net::InterestSetDiff noop_diff{};
    const fuse::net::InterestScopeSet before_noop = applied_scope;
    noop_diff.apply_to(applied_scope);
    expectTrue(applied_scope.equal_to(before_noop), "empty diff apply_to is a no-op");

    // --- evaluate_and_diff ---
    fuse::net::InterestManager eval_diff_manager;
    eval_diff_manager.set_policy(policy);
    eval_diff_manager.set_observer_position(origin);
    eval_diff_manager.register_entity({make_entity(110), {10.f, 0.f, 0.f, 0.f}, 0.f});
    fuse::net::InterestSetDiff eval_first{};
    expectTrue(!eval_diff_manager.evaluate_and_diff(eval_first),
               "evaluate_and_diff first pass is empty");
    expectTrue(eval_first.empty(), "evaluate_and_diff first pass clears diff output");

    eval_diff_manager.register_entity({make_entity(111), {12.f, 0.f, 0.f, 0.f}, 0.f});
    fuse::net::InterestSetDiff eval_second{};
    expectTrue(eval_diff_manager.evaluate_and_diff(eval_second),
               "evaluate_and_diff reports scope change on entity add");
    expectTrue(eval_second.entered.size() == 1u && eval_second.entered[0].index == 111u,
               "evaluate_and_diff reports newly scoped entity");

    fuse::net::InterestScopeSet replicated_scope;
    replicated_scope.entities = {make_entity(110)};
    eval_second.apply_to(replicated_scope);
    expectTrue(replicated_scope.contains(make_entity(111)),
               "apply_to replicates enter from evaluate_and_diff");
    expectTrue(replicated_scope.size() == 2u, "apply_to grows replicated scope on enter");

    // --- empty scope transitions ---
    fuse::net::InterestManager transition_manager;
    transition_manager.set_policy(policy);
    transition_manager.set_observer_position(origin);
    transition_manager.register_entity({make_entity(120), {500.f, 0.f, 0.f, 0.f}, 0.f});
    fuse::net::InterestSetDiff transition_first{};
    expectTrue(!transition_manager.evaluate_and_diff(transition_first),
               "first empty-scope evaluate_and_diff returns false");
    fuse::net::InterestSetDiff transition_second{};
    expectTrue(!transition_manager.evaluate_and_diff(transition_second),
               "empty-to-empty evaluate_and_diff returns false");
    expectTrue(transition_manager.scope_set().empty(), "repeated empty-scope evaluate stays empty");
    expectTrue(transition_second.empty(), "empty-to-empty evaluate_and_diff stays empty");

    transition_manager.clear_entities();
    transition_manager.register_entity({make_entity(121), {10.f, 0.f, 0.f, 0.f}, 0.f});
    fuse::net::InterestSetDiff after_clear{};
    expectTrue(!transition_manager.evaluate_and_diff(after_clear),
               "first evaluate after clear_entities returns false");
    expectTrue(after_clear.empty(), "first evaluate after clear_entities has empty diff");
    expectTrue(transition_manager.scope_set().contains(make_entity(121)),
               "scope repopulates after clear_entities");

    // --- is_empty_interest_diff + count_scope_diff_entities ---
    fuse::net::InterestSetDiff empty_diff_check{};
    expectTrue(fuse::net::is_empty_interest_diff(empty_diff_check),
               "is_empty_interest_diff reports empty diff");
    expectTrue(fuse::net::count_scope_diff_entities(empty_diff_check) == 0u,
               "count_scope_diff_entities is zero for empty diff");

    fuse::net::InterestSetDiff mixed_diff{};
    mixed_diff.entered = {make_entity(1), make_entity(2)};
    mixed_diff.left = {make_entity(3)};
    expectTrue(!fuse::net::is_empty_interest_diff(mixed_diff),
               "is_empty_interest_diff rejects non-empty diff");
    expectTrue(fuse::net::count_scope_diff_entities(mixed_diff) == 3u,
               "count_scope_diff_entities sums entered and left");

    // --- empty-set diff guard ---
    fuse::net::InterestScopeSet empty_prev;
    fuse::net::InterestScopeSet empty_curr;
    fuse::net::InterestSetDiff both_empty_diff{};
    fuse::net::diff_interest_scope_sets(empty_prev, empty_curr, both_empty_diff);
    expectTrue(both_empty_diff.empty(), "diff of two empty scope sets is empty");

    // --- evaluate_and_diff return value + previous_scope_set ---
    fuse::net::InterestManager return_manager;
    return_manager.set_policy(policy);
    return_manager.set_observer_position(origin);
    return_manager.register_entity({make_entity(130), {10.f, 0.f, 0.f, 0.f}, 0.f});
    fuse::net::InterestSetDiff return_first{};
    expectTrue(!return_manager.evaluate_and_diff(return_first),
               "evaluate_and_diff returns false on first empty diff");
    expectTrue(return_manager.scope_set().contains(make_entity(130)),
               "first evaluate_and_diff populates scope set");
    expectTrue(return_manager.previous_scope_set().equal_to(return_manager.scope_set()),
               "previous_scope_set matches scope on first evaluation");

    return_manager.register_entity({make_entity(131), {12.f, 0.f, 0.f, 0.f}, 0.f});
    fuse::net::InterestSetDiff return_second{};
    expectTrue(return_manager.evaluate_and_diff(return_second),
               "evaluate_and_diff returns true when scope changes");
    expectTrue(return_second.entered.size() == 1u && return_second.entered[0].index == 131u,
               "evaluate_and_diff return path still fills entered set");

    // --- hysteresis-aware radius count/filter ---
    fuse::net::InterestPolicy hysteresis_policy{};
    hysteresis_policy.relevance_radius = 50.f;
    hysteresis_policy.always_relevant_radius = 5.f;
    hysteresis_policy.unload_radius = 0.f;

    std::vector<fuse::net::InterestCandidate> hysteresis_candidates;
    hysteresis_candidates.push_back({make_entity(140), {40.f, 0.f, 0.f, 0.f}, 0.f});
    hysteresis_candidates.push_back({make_entity(141), {55.f, 0.f, 0.f, 0.f}, 0.f});

    expectTrue(fuse::net::count_candidates_in_radius(origin, hysteresis_policy, hysteresis_candidates) == 1u,
               "radius count without hysteresis drops beyond relevance");
    fuse::net::InterestScopeSet prior_scope;
    prior_scope.entities = {make_entity(141)};
    expectTrue(fuse::net::count_candidates_in_radius(origin, hysteresis_policy, hysteresis_candidates,
                                                     prior_scope) == 2u,
               "radius count with prior scope keeps hysteresis entity");

    std::vector<fuse::net::InterestEntry> hysteresis_filtered;
    const fuse::u32 hysteresis_filtered_count = fuse::net::filter_candidates_in_radius(
        origin, hysteresis_policy, hysteresis_candidates, prior_scope, hysteresis_filtered);
    expectTrue(hysteresis_filtered_count == 2u,
               "hysteresis radius filter count matches prior-scope count");
    expectTrue(hysteresis_filtered.size() == 2u,
               "hysteresis radius filter output size matches count");

    bool saw_hysteresis_entity = false;
    for (const fuse::net::InterestEntry& entry : hysteresis_filtered) {
        if (entry.entity.index == 141u) {
            saw_hysteresis_entity = true;
            expectTrue(entry.scope == fuse::net::InterestScope::InScope,
                       "hysteresis filter marks retained entity in scope");
        }
    }
    expectTrue(saw_hysteresis_entity, "hysteresis filter retains prior-scope entity");

    // --- apply_diff alias + empty guard ---
    fuse::net::InterestScopeSet alias_scope;
    alias_scope.entities = {make_entity(1), make_entity(2)};
    fuse::net::InterestSetDiff alias_diff{};
    alias_diff.entered = {make_entity(3)};
    alias_diff.left = {make_entity(1)};
    alias_diff.apply_diff(alias_scope);
    expectTrue(alias_scope.size() == 2u, "apply_diff preserves net scope size");
    expectTrue(alias_scope.contains(make_entity(2)), "apply_diff keeps unchanged entity");
    expectTrue(alias_scope.contains(make_entity(3)), "apply_diff inserts entered entity");
    expectTrue(!alias_scope.contains(make_entity(1)), "apply_diff removes left entity");

    fuse::net::InterestSetDiff guarded_empty_diff{};
    const fuse::net::InterestScopeSet alias_before_guard = alias_scope;
    guarded_empty_diff.apply_diff(alias_scope);
    expectTrue(alias_scope.equal_to(alias_before_guard), "apply_diff no-op on empty diff");
    expectTrue(!guarded_empty_diff.apply_diff(alias_scope),
               "apply_diff returns false on empty diff");

    // --- diff_interest_scope_sets bool return + equal-set guard ---
    fuse::net::InterestScopeSet equal_a;
    fuse::net::InterestScopeSet equal_b;
    equal_a.entities = {make_entity(1), make_entity(2)};
    equal_b.entities = {make_entity(2), make_entity(1)};
    fuse::net::InterestSetDiff equal_diff{};
    expectTrue(!fuse::net::diff_interest_scope_sets(equal_a, equal_b, equal_diff),
               "diff_interest_scope_sets returns false for equal scope sets");
    expectTrue(equal_diff.empty(), "equal-set diff guard clears output");

    fuse::net::InterestScopeSet diff_prev;
    fuse::net::InterestScopeSet diff_curr;
    diff_prev.entities = {make_entity(1)};
    diff_curr.entities = {make_entity(1), make_entity(2)};
    fuse::net::InterestSetDiff bool_diff{};
    expectTrue(fuse::net::diff_interest_scope_sets(diff_prev, diff_curr, bool_diff),
               "diff_interest_scope_sets returns true when scope grows");
    expectTrue(bool_diff.has_enters() && !bool_diff.has_leaves(),
               "has_scope_enters/has_scope_leaves classify enter-only diff");
    expectTrue(fuse::net::has_scope_enters(bool_diff), "has_scope_enters free helper");
    expectTrue(!fuse::net::has_scope_leaves(bool_diff), "has_scope_leaves rejects enter-only diff");

    fuse::net::InterestSetDiff leave_only_diff{};
    leave_only_diff.left = {make_entity(5)};
    expectTrue(!leave_only_diff.has_enters(), "has_enters false for leave-only diff");
    expectTrue(leave_only_diff.has_leaves(), "has_leaves true for leave-only diff");
    expectTrue(fuse::net::has_scope_leaves(leave_only_diff), "has_scope_leaves free helper");

    // --- compute_scope_diff bool return ---
    fuse::net::InterestManager scope_diff_manager;
    scope_diff_manager.set_policy(policy);
    scope_diff_manager.set_observer_position(origin);
    scope_diff_manager.register_entity({make_entity(150), {10.f, 0.f, 0.f, 0.f}, 0.f});
    scope_diff_manager.evaluate();
    fuse::net::InterestSetDiff unchanged_diff{};
    expectTrue(!scope_diff_manager.compute_scope_diff(unchanged_diff),
               "compute_scope_diff returns false when scope unchanged");
    expectTrue(unchanged_diff.empty(), "unchanged compute_scope_diff clears output");

    scope_diff_manager.register_entity({make_entity(151), {12.f, 0.f, 0.f, 0.f}, 0.f});
    scope_diff_manager.evaluate();
    fuse::net::InterestSetDiff changed_diff{};
    expectTrue(scope_diff_manager.compute_scope_diff(changed_diff),
               "compute_scope_diff returns true when scope changed");
    expectTrue(changed_diff.has_enters(), "changed compute_scope_diff fills entered set");

    // --- registered radius filter/count on manager ---
    fuse::net::InterestManager registered_manager;
    registered_manager.set_policy(hysteresis_policy);
    registered_manager.set_observer_position(origin);
    registered_manager.register_entity({make_entity(160), {40.f, 0.f, 0.f, 0.f}, 0.f});
    registered_manager.register_entity({make_entity(161), {55.f, 0.f, 0.f, 0.f}, 0.f});
    registered_manager.evaluate();

    expectTrue(registered_manager.count_registered_in_radius() == 1u,
               "count_registered_in_radius without prior hysteresis keeps relevance only");
    std::vector<fuse::net::InterestEntry> registered_filtered;
    expectTrue(registered_manager.filter_registered_in_radius(registered_filtered) == 1u,
               "filter_registered_in_radius matches count without hysteresis");
    expectTrue(registered_filtered.size() == 1u,
               "filter_registered_in_radius output size matches count");
    expectTrue(registered_filtered[0].entity.index == 160u,
               "filter_registered_in_radius keeps in-relevance entity");

    fuse::net::InterestManager hysteresis_manager;
    hysteresis_manager.set_policy(hysteresis_policy);
    hysteresis_manager.set_observer_position(origin);
    hysteresis_manager.register_entity({make_entity(162), {40.f, 0.f, 0.f, 0.f}, 0.f});
    hysteresis_manager.register_entity({make_entity(163), {45.f, 0.f, 0.f, 0.f}, 0.f});
    hysteresis_manager.evaluate();
    expectTrue(hysteresis_manager.update_entity_position(make_entity(163), {55.f, 0.f, 0.f, 0.f}),
               "update moves entity past relevance while prior scope retains it");
    hysteresis_manager.evaluate();
    expectTrue(hysteresis_manager.in_scope_count() == 2u,
               "evaluate keeps hysteresis entity in scope after position update");
    expectTrue(hysteresis_manager.count_registered_in_radius() == 2u,
               "count_registered_in_radius uses prior scope hysteresis after evaluate");
    std::vector<fuse::net::InterestEntry> hysteresis_registered;
    expectTrue(hysteresis_manager.filter_registered_in_radius(hysteresis_registered) == 2u,
               "filter_registered_in_radius uses prior scope hysteresis after evaluate");

    bool saw_registered_hysteresis = false;
    for (const fuse::net::InterestEntry& entry : hysteresis_registered) {
        if (entry.entity.index == 163u) {
            saw_registered_hysteresis = true;
        }
    }
    expectTrue(saw_registered_hysteresis,
               "filter_registered_in_radius retains hysteresis entity from prior scope");

    // --- InterestSetDiff::clear ---
    fuse::net::InterestSetDiff clearable_diff{};
    clearable_diff.entered = {make_entity(1)};
    clearable_diff.left = {make_entity(2)};
    clearable_diff.clear();
    expectTrue(clearable_diff.empty(), "InterestSetDiff::clear empties entered and left");
    expectTrue(!fuse::net::has_scope_enters(clearable_diff),
               "cleared diff has no enters via helper");

    // --- clear_interest_diff free helper ---
    fuse::net::InterestSetDiff helper_clear_diff{};
    helper_clear_diff.entered = {make_entity(7)};
    helper_clear_diff.left = {make_entity(8)};
    fuse::net::clear_interest_diff(helper_clear_diff);
    expectTrue(helper_clear_diff.empty(), "clear_interest_diff empties diff");
    expectTrue(fuse::net::is_empty_interest_diff(helper_clear_diff),
               "clear_interest_diff leaves is_empty_interest_diff true");

    // --- can_apply_interest_diff preflight guard ---
    fuse::net::InterestScopeSet preflight_scope;
    preflight_scope.entities = {make_entity(1), make_entity(2)};
    fuse::net::InterestSetDiff preflight_diff{};
    expectTrue(!fuse::net::can_apply_interest_diff(preflight_diff, preflight_scope),
               "can_apply_interest_diff rejects empty diff");

    preflight_diff.entered = {make_entity(3)};
    expectTrue(fuse::net::can_apply_interest_diff(preflight_diff, preflight_scope),
               "can_apply_interest_diff accepts enter for absent entity");
    preflight_diff.left = {make_entity(1)};
    expectTrue(fuse::net::can_apply_interest_diff(preflight_diff, preflight_scope),
               "can_apply_interest_diff accepts leave for present entity");

    fuse::net::InterestSetDiff redundant_diff{};
    redundant_diff.entered = {make_entity(1)};
    redundant_diff.left = {make_entity(99)};
    expectTrue(!fuse::net::can_apply_interest_diff(redundant_diff, preflight_scope),
               "can_apply_interest_diff rejects redundant non-empty diff");
    expectTrue(!redundant_diff.apply_diff(preflight_scope),
               "apply_diff returns false when diff is redundant");
    fuse::net::InterestScopeSet unchanged_scope;
    unchanged_scope.entities = {make_entity(1), make_entity(2)};
    expectTrue(preflight_scope.equal_to(unchanged_scope),
               "redundant apply_diff leaves scope unchanged");

    // --- preflight_interest_diff_apply struct guards (B7.4 deepen follow-up) ---
    fuse::net::InterestSetDiff empty_apply_diff{};
    const fuse::net::InterestDiffApplyPreflight empty_apply_preflight =
        fuse::net::preflight_interest_diff_apply(empty_apply_diff, preflight_scope);
    expectTrue(empty_apply_preflight.empty_diff, "preflight marks empty diff");
    expectTrue(!empty_apply_preflight.has_pending_enters, "empty diff has no pending enters");
    expectTrue(!empty_apply_preflight.has_pending_leaves, "empty diff has no pending leaves");
    expectTrue(empty_apply_preflight.should_skip(), "empty diff preflight should_skip");

    fuse::net::InterestSetDiff enter_only_diff{};
    enter_only_diff.entered = {make_entity(3)};
    const fuse::net::InterestDiffApplyPreflight enter_preflight =
        fuse::net::preflight_interest_diff_apply(enter_only_diff, preflight_scope);
    expectTrue(!enter_preflight.empty_diff, "enter-only preflight sees non-empty diff");
    expectTrue(enter_preflight.has_pending_enters, "enter-only preflight marks pending enters");
    expectTrue(!enter_preflight.has_pending_leaves, "enter-only preflight has no pending leaves");
    expectTrue(!enter_preflight.redundant_diff, "enter-only preflight is not redundant");
    expectTrue(enter_preflight.can_apply(), "enter-only preflight can_apply");
    expectTrue(!fuse::net::should_skip_interest_diff_apply(enter_only_diff, preflight_scope),
               "should_skip rejects enter-only diff");

    fuse::net::InterestSetDiff leave_only_apply_diff{};
    leave_only_apply_diff.left = {make_entity(2)};
    const fuse::net::InterestDiffApplyPreflight leave_preflight =
        fuse::net::preflight_interest_diff_apply(leave_only_apply_diff, preflight_scope);
    expectTrue(leave_preflight.has_pending_leaves, "leave-only preflight marks pending leaves");
    expectTrue(!leave_preflight.has_pending_enters, "leave-only preflight has no pending enters");
    expectTrue(leave_preflight.can_apply(), "leave-only preflight can_apply");

    const fuse::net::InterestDiffApplyPreflight redundant_preflight =
        fuse::net::preflight_interest_diff_apply(redundant_diff, preflight_scope);
    expectTrue(redundant_preflight.redundant_diff, "preflight marks redundant diff");
    expectTrue(!redundant_preflight.has_pending_enters, "redundant preflight has no pending enters");
    expectTrue(!redundant_preflight.has_pending_leaves, "redundant preflight has no pending leaves");
    expectTrue(!redundant_preflight.can_apply(), "redundant preflight cannot apply");
    expectTrue(fuse::net::should_skip_interest_diff_apply(redundant_diff, preflight_scope),
               "should_skip accepts redundant diff");

    // --- apply_to bool return ---
    fuse::net::InterestScopeSet apply_to_scope;
    apply_to_scope.entities = {make_entity(1)};
    fuse::net::InterestSetDiff apply_to_diff{};
    apply_to_diff.entered = {make_entity(2)};
    expectTrue(apply_to_diff.apply_to(apply_to_scope), "apply_to returns true when scope changes");
    expectTrue(apply_to_scope.contains(make_entity(2)), "apply_to inserts entered entity");

    fuse::net::InterestSetDiff apply_to_empty{};
    expectTrue(!apply_to_empty.apply_to(apply_to_scope), "apply_to returns false on empty diff");

    // --- InterestScopeSet insert/remove bool guards ---
    fuse::net::InterestScopeSet insert_scope;
    expectTrue(insert_scope.insert_entity(make_entity(5)), "insert_entity returns true on first insert");
    expectTrue(!insert_scope.insert_entity(make_entity(5)),
               "insert_entity returns false on duplicate insert");
    expectTrue(insert_scope.remove_entity(make_entity(5)), "remove_entity returns true when present");
    expectTrue(!insert_scope.remove_entity(make_entity(5)),
               "remove_entity returns false when absent");

    // --- is_entity_registered + has_any_registered_in_radius ---
    fuse::net::InterestManager registration_manager;
    registration_manager.set_policy(policy);
    registration_manager.set_observer_position(origin);
    const fuse::ecs::EntityID registered_entity = make_entity(170);
    registration_manager.register_entity({registered_entity, {10.f, 0.f, 0.f, 0.f}, 0.f});
    expectTrue(registration_manager.is_entity_registered(registered_entity),
               "is_entity_registered reports registered entity");
    expectTrue(!registration_manager.is_entity_registered(make_entity(999)),
               "is_entity_registered rejects unknown entity");

    registration_manager.evaluate();
    expectTrue(registration_manager.has_any_registered_in_radius(),
               "has_any_registered_in_radius true when entity in scope");
    expectTrue(registration_manager.count_registered_in_radius() > 0u,
               "count_registered_in_radius positive when has_any is true");

    registration_manager.set_observer_position({500.f, 0.f, 0.f, 0.f});
    registration_manager.evaluate();
    expectTrue(!registration_manager.has_any_registered_in_radius(),
               "has_any_registered_in_radius false when all entities out of scope");
    expectTrue(registration_manager.count_registered_in_radius() == 0u,
               "count_registered_in_radius zero when has_any is false");

    // --- register_entity / unregister_entity bool returns ---
    fuse::net::InterestManager register_manager;
    register_manager.set_policy(policy);
    register_manager.set_observer_position(origin);
    const fuse::ecs::EntityID reg_entity = make_entity(180);
    expectTrue(register_manager.register_entity({reg_entity, {10.f, 0.f, 0.f, 0.f}, 0.f}),
               "register_entity returns true on first insert");
    expectTrue(!register_manager.register_entity({reg_entity, {12.f, 0.f, 0.f, 0.f}, 0.f}),
               "register_entity returns false on duplicate entity");
    expectTrue(register_manager.candidates().size() == 1u,
               "duplicate register_entity does not grow candidate list");

    expectTrue(!register_manager.unregister_entity(make_entity(999)),
               "unregister_entity returns false for unknown entity");
    expectTrue(register_manager.unregister_entity(reg_entity),
               "unregister_entity returns true when entity removed");
    expectTrue(!register_manager.is_entity_registered(reg_entity),
               "unregistered entity no longer reported as registered");
    expectTrue(register_manager.candidates().empty(), "unregister_entity clears candidate list");

    register_manager.register_entity({make_entity(181), {10.f, 0.f, 0.f, 0.f}, 0.f});
    register_manager.evaluate();
    expectTrue(register_manager.unregister_entity(make_entity(181)),
               "unregister_entity succeeds after evaluate");
    register_manager.evaluate();
    expectTrue(register_manager.scope_set().empty(),
               "scope set clears after unregister and re-evaluate");

    // --- apply_and_clear / apply_interest_diff helpers ---
    fuse::net::InterestScopeSet apply_clear_scope;
    apply_clear_scope.entities = {make_entity(1), make_entity(2)};
    fuse::net::InterestSetDiff apply_clear_diff{};
    apply_clear_diff.entered = {make_entity(3)};
    apply_clear_diff.left = {make_entity(1)};
    expectTrue(apply_clear_diff.apply_and_clear(apply_clear_scope),
               "apply_and_clear returns true when scope changes");
    expectTrue(apply_clear_diff.empty(), "apply_and_clear clears diff payload");
    expectTrue(apply_clear_scope.contains(make_entity(3)), "apply_and_clear inserts entered entity");
    expectTrue(!apply_clear_scope.contains(make_entity(1)), "apply_and_clear removes left entity");

    fuse::net::InterestScopeSet alias_clear_scope;
    alias_clear_scope.entities = {make_entity(5)};
    fuse::net::InterestSetDiff alias_clear_diff{};
    alias_clear_diff.entered = {make_entity(6)};
    expectTrue(alias_clear_diff.apply_to_and_clear(alias_clear_scope),
               "apply_to_and_clear returns true when scope changes");
    expectTrue(alias_clear_diff.empty(), "apply_to_and_clear clears diff payload");
    expectTrue(alias_clear_scope.contains(make_entity(6)), "apply_to_and_clear inserts entered entity");

    fuse::net::InterestScopeSet free_apply_scope;
    free_apply_scope.entities = {make_entity(7)};
    fuse::net::InterestSetDiff free_apply_diff{};
    free_apply_diff.entered = {make_entity(8)};
    expectTrue(fuse::net::apply_interest_diff(free_apply_diff, free_apply_scope),
               "apply_interest_diff returns true when scope changes");
    expectTrue(free_apply_diff.empty(), "apply_interest_diff clears diff");
    expectTrue(free_apply_scope.contains(make_entity(8)), "apply_interest_diff inserts entered entity");

    fuse::net::InterestSetDiff noop_clear_diff{};
    const fuse::net::InterestScopeSet noop_before = free_apply_scope;
    expectTrue(!noop_clear_diff.apply_and_clear(free_apply_scope),
               "apply_and_clear returns false on empty diff");
    expectTrue(free_apply_scope.equal_to(noop_before), "apply_and_clear no-op leaves scope unchanged");

    // --- relevance_radii_disabled early-out ---
    fuse::net::InterestPolicy disabled_policy{};
    disabled_policy.relevance_radius = 0.f;
    disabled_policy.always_relevant_radius = 0.f;
    expectTrue(fuse::net::relevance_radii_disabled(disabled_policy),
               "relevance_radii_disabled true when both radii are zero");

    std::vector<fuse::net::InterestCandidate> disabled_candidates;
    disabled_candidates.push_back({make_entity(190), {10.f, 0.f, 0.f, 0.f}, 0.f});
    disabled_candidates.push_back({make_entity(191), {20.f, 0.f, 0.f, 0.f}, 0.f});
    expectTrue(fuse::net::count_candidates_in_radius(origin, disabled_policy, disabled_candidates) == 0u,
               "count_candidates_in_radius early-outs when radii disabled");
    std::vector<fuse::net::InterestEntry> disabled_filtered;
    expectTrue(fuse::net::filter_candidates_in_radius(origin, disabled_policy, disabled_candidates,
                                                      disabled_filtered) == 0u,
               "filter_candidates_in_radius early-outs when radii disabled");
    expectTrue(disabled_filtered.empty(), "disabled radii filter leaves output empty");

    fuse::net::InterestScopeSet disabled_prior;
    disabled_prior.entities = {make_entity(191)};
    expectTrue(fuse::net::count_candidates_in_radius(origin, disabled_policy, disabled_candidates,
                                                     disabled_prior) == 0u,
               "hysteresis count early-outs when radii disabled and prior scope empty");
    std::vector<fuse::net::InterestEntry> disabled_hysteresis_filtered;
    expectTrue(fuse::net::filter_candidates_in_radius(origin, disabled_policy, disabled_candidates,
                                                      disabled_prior,
                                                      disabled_hysteresis_filtered) == 0u,
               "hysteresis filter early-outs when radii disabled and prior scope empty");

    fuse::net::InterestPolicy tight_disabled_policy{};
    tight_disabled_policy.relevance_radius = -1.f;
    tight_disabled_policy.always_relevant_radius = -1.f;
    expectTrue(fuse::net::relevance_radii_disabled(tight_disabled_policy),
               "relevance_radii_disabled treats negative radii as zero");
    expectTrue(fuse::net::count_candidates_in_radius(origin, tight_disabled_policy,
                                                     disabled_candidates) == 0u,
               "negative radii count path early-outs");

    // --- preflight_radius_filter struct guards (B7.4 deepen follow-up) ---
    const fuse::net::RadiusFilterPreflight empty_radius_preflight =
        fuse::net::preflight_radius_filter(policy, empty_candidates);
    expectTrue(empty_radius_preflight.candidates_empty, "radius preflight marks empty candidates");
    expectTrue(!empty_radius_preflight.radii_disabled, "radius preflight radii enabled for default policy");
    expectTrue(empty_radius_preflight.should_skip(), "empty candidates radius preflight should_skip");
    expectTrue(fuse::net::should_skip_radius_filter(policy, empty_candidates),
               "should_skip_radius_filter on empty candidates");

    const fuse::net::RadiusFilterPreflight active_radius_preflight =
        fuse::net::preflight_radius_filter(policy, candidates);
    expectTrue(!active_radius_preflight.candidates_empty, "radius preflight sees candidates");
    expectTrue(!active_radius_preflight.radii_disabled, "radius preflight radii enabled");
    expectTrue(active_radius_preflight.can_filter(), "active radius preflight can_filter");
    expectTrue(!fuse::net::should_skip_radius_filter(policy, candidates),
               "should_skip rejects active radius filter");

    const fuse::net::RadiusFilterPreflight disabled_radius_preflight =
        fuse::net::preflight_radius_filter(disabled_policy, disabled_candidates);
    expectTrue(disabled_radius_preflight.radii_disabled, "radius preflight marks disabled radii");
    expectTrue(!disabled_radius_preflight.candidates_empty, "disabled radii still has candidates");
    expectTrue(disabled_radius_preflight.prior_scope_empty, "simple preflight prior scope empty");
    expectTrue(!disabled_radius_preflight.hysteresis_retention, "simple preflight no hysteresis retention");
    expectTrue(!disabled_radius_preflight.can_filter(), "disabled radii preflight cannot filter");
    expectTrue(fuse::net::should_skip_radius_filter(disabled_policy, disabled_candidates),
               "should_skip accepts disabled radii without prior scope");

    const fuse::net::RadiusFilterPreflight hysteresis_radius_preflight =
        fuse::net::preflight_radius_filter(disabled_policy, disabled_candidates, disabled_prior);
    expectTrue(hysteresis_radius_preflight.radii_disabled, "hysteresis preflight marks disabled radii");
    expectTrue(!hysteresis_radius_preflight.prior_scope_empty, "hysteresis preflight sees prior scope");
    expectTrue(hysteresis_radius_preflight.hysteresis_retention,
               "hysteresis preflight retains prior-scope entities");
    expectTrue(hysteresis_radius_preflight.can_filter(), "hysteresis preflight can_filter with prior scope");
    expectTrue(!fuse::net::should_skip_radius_filter(disabled_policy, disabled_candidates, disabled_prior),
               "should_skip rejects hysteresis retention path");

    fuse::net::InterestManager preflight_manager;
    preflight_manager.set_policy(hysteresis_policy);
    preflight_manager.set_observer_position(origin);
    preflight_manager.register_entity({make_entity(200), {40.f, 0.f, 0.f, 0.f}, 0.f});
    preflight_manager.evaluate();
    const fuse::net::RadiusFilterPreflight registered_preflight =
        preflight_manager.preflight_registered_radius_filter();
    expectTrue(!registered_preflight.candidates_empty, "registered preflight sees candidates");
    expectTrue(!registered_preflight.radii_disabled, "registered preflight radii enabled");
    expectTrue(registered_preflight.can_filter(), "registered preflight can_filter");
    expectTrue(!preflight_manager.preflight_registered_radius_filter().should_skip(),
               "registered should_skip false when filter active");
}

} // namespace fuse::net::tests
