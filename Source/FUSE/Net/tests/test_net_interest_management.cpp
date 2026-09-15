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
}

} // namespace fuse::net::tests
