#include <fuse/physics/events/collision_events.hpp>
#include <fuse/physics/forces/impulse_scale.hpp>
#include <fuse/physics/forces/knockback.hpp>
#include <fuse/physics/melee_arc.hpp>
#include <fuse/physics/narrowphase/contact_count.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testMeleeArcOk() {
    expectTrue(fuse::physics::melee_arc_ok(1.f), "melee arc 1 is valid");
    expectTrue(fuse::physics::melee_arc_ok(180.f), "melee arc 180 is valid");
    expectTrue(!fuse::physics::melee_arc_ok(0.f), "melee arc 0 is invalid");
    expectTrue(!fuse::physics::melee_arc_ok(180.1f), "melee arc 180.1 is invalid");
    expectTrue(!fuse::physics::melee_arc_ok(std::numeric_limits<float>::quiet_NaN()),
               "melee arc NaN is invalid");
}

void testContactCountOk() {
    expectTrue(fuse::physics::contact_count_ok(0), "contact count 0 is valid");
    expectTrue(fuse::physics::contact_count_ok(64), "contact count 64 is valid");
    expectTrue(!fuse::physics::contact_count_ok(-1), "contact count -1 is invalid");
    expectTrue(!fuse::physics::contact_count_ok(65), "contact count 65 is invalid");
}

void testScaleImpulse() {
    expectTrue(fuse::physics::scale_impulse(10.f, 0.5f) == 5.f, "impulse 10 scaled by 0.5 is 5");
    expectTrue(fuse::physics::scale_impulse(-2.f, 1.f) == 0.f, "negative impulse counts as 0");
    expectTrue(fuse::physics::scale_impulse(2.f, 8.f) == 8.f, "scale above 4 clamps so 2 * 4 is 8");
    expectTrue(fuse::physics::scale_impulse(4.f, -1.f) == 0.f, "negative scale clamps to 0");
    expectTrue(fuse::physics::scale_impulse(std::numeric_limits<float>::quiet_NaN(), 1.f) == 0.f,
               "non-finite impulse returns 0");
    expectTrue(fuse::physics::scale_impulse(3.f, std::numeric_limits<float>::infinity()) == 0.f,
               "non-finite scale returns 0");
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

void testKnockbackDeltaV() {
    const fuse::physics::vec3 delta =
        fuse::physics::knockback_delta_v(fuse::physics::vec3{1.f, 0.f, 0.f}, 10.f, 2.f);
    expectTrue(delta.x == 5.f && delta.y == 0.f && delta.z == 0.f,
               "unit +X impulse 10 mass 2 yields delta-v (5,0,0)");

    const fuse::physics::vec3 zeroMass =
        fuse::physics::knockback_delta_v(fuse::physics::vec3{1.f, 0.f, 0.f}, 10.f, 0.f);
    expectTrue(zeroMass.x == 0.f && zeroMass.y == 0.f && zeroMass.z == 0.f, "mass 0 yields zero knockback");
}

} // namespace

int main() {
    testMeleeArcOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }
    testContactCountOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }
    testScaleImpulse();
    testCollisionCallbackDispatch();
    testTriggerEventType();
    testKnockbackDeltaV();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
