#include <fuse/physics/events/collision_events.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testCollisionCallbackDispatch() {
    fuse::physics::CollisionEventSystem system;
    fuse::ecs::EntityID entityA{5, 1};
    fuse::ecs::EntityID entityB{9, 1};

    fuse::u32 callbackHits = 0;
    system.registerCallback(entityA, [&](const fuse::physics::CollisionEvent& event) {
        expectTrue(event.type == fuse::physics::CollisionEventType::Enter, "enter event received");
        ++callbackHits;
    });

    fuse::physics::CollisionEvent enter{};
    enter.type = fuse::physics::CollisionEventType::Enter;
    enter.entityA = entityA;
    enter.entityB = entityB;
    enter.impulse = 3.f;

    system.dispatch({enter});
    expectTrue(callbackHits == 1, "callback invoked once");
    expectTrue(system.dispatchedCount() == 1, "dispatch counter incremented");

    system.unregisterCallback(entityA);
    expectTrue(system.callbackCount() == 0, "callback unregistered");
}

void testTriggerEventType() {
    fuse::physics::CollisionEventSystem system;
    fuse::ecs::EntityID trigger{2, 1};
    bool triggerSeen = false;
    system.registerCallback(trigger, [&](const fuse::physics::CollisionEvent& event) {
        triggerSeen = event.type == fuse::physics::CollisionEventType::Trigger;
    });

    fuse::physics::CollisionEvent triggerEvent{};
    triggerEvent.type = fuse::physics::CollisionEventType::Trigger;
    triggerEvent.entityA = trigger;
    system.dispatch({triggerEvent});
    expectTrue(triggerSeen, "trigger event dispatched");
}

} // namespace

int main() {
    testCollisionCallbackDispatch();
    testTriggerEventType();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
