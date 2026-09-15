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
}

} // namespace fuse::net::tests
