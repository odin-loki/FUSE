#include <fuse/mechanics/camera_component.hpp>
#include <fuse/mechanics/follow_component.hpp>
#include <fuse/mechanics/light_component.hpp>
#include <fuse/mechanics/move_component.hpp>
#include <fuse/mechanics/animate_component.hpp>
#include <fuse/mechanics/waypoint_component.hpp>
#include <fuse/mechanics/look_at_component.hpp>
#include <fuse/mechanics/radio_component.hpp>
#include <fuse/mechanics/path_component.hpp>
#include <fuse/mechanics/timer_component.hpp>
#include <fuse/mechanics/broadphase_world_stub.hpp>
#include <fuse/core/init.hpp>

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

} // namespace

int main() {
    fuse::core::initialize();

    fuse::mechanics::LightComponent light("outpost_lamp", 2.f);
    light.enable();
    expectTrue(light.enabled(), "light component enabled");
    expectTrue(light.enableCount() == 1u, "light enable counted");
    light.disable();
    expectTrue(light.disableCount() == 1u, "light disable counted");

    fuse::mechanics::MoveComponent mover("agent_move", 2.f);
    mover.setTarget(4.f, 0.f, 0.f);
    mover.advance(1.f);
    expectTrue(mover.tickCount() == 1u, "move component tick counted");
    expectTrue(mover.x() > 0.f, "move component advances toward target");

    fuse::mechanics::BroadphaseWorldStub world;
    fuse::mechanics::BroadphaseWorldBody character{};
    character.objectId = 1;
    character.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    character.x = 0.f;
    world.addBody(character);

    fuse::mechanics::BroadphaseWorldBody trigger{};
    trigger.objectId = 2;
    trigger.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Trigger);
    trigger.x = 1.f;
    world.addBody(trigger);

    expectTrue(world.queryOverlaps(fuse::mechanics::BroadphaseProxyFilter::Character,
                                   fuse::mechanics::BroadphaseProxyFilter::Trigger) >= 1u,
               "broadphase world stub finds character/trigger overlap");

    fuse::mechanics::BroadphaseWorldStub layerWorld;
    fuse::mechanics::BroadphaseWorldBody layerA{};
    layerA.objectId = 10;
    layerA.proxy =
        fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    layerA.collisionLayer = 1u;
    layerA.x = 0.f;
    layerWorld.addBody(layerA);

    fuse::mechanics::BroadphaseWorldBody layerB{};
    layerB.objectId = 11;
    layerB.proxy =
        fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    layerB.collisionLayer = 8u;
    layerB.collisionMask = 8u;
    layerB.x = 0.f;
    layerWorld.addBody(layerB);
    expectTrue(layerWorld.queryOverlaps(fuse::mechanics::BroadphaseProxyFilter::Character,
                                        fuse::mechanics::BroadphaseProxyFilter::Character) == 0u,
               "collision layer mask filters broadphase character overlap");

    fuse::mechanics::CameraComponent camera("outpost_cam", 75.f);
    camera.activate();
    expectTrue(camera.active(), "camera component active");
    expectTrue(camera.activateCount() == 1u, "camera activate counted");

    fuse::mechanics::FollowComponent follower("agent_follow", 3.f);
    follower.setPosition(0.f, 0.f, 0.f);
    follower.setTargetObjectId(1);
    follower.advanceToward(3.f, 0.f, 0.f, 1.f);
    expectTrue(follower.tickCount() == 1u, "follow component tick counted");
    expectTrue(follower.x() > 0.f, "follow component advances");

    expectTrue(world.removeBody(2), "broadphase world removes body");
    expectTrue(world.bodyCount() == 1u, "broadphase world body count after remove");
    expectTrue(world.queryAabbOverlaps(-1.f, -1.f, -1.f, 2.f, 2.f, 2.f) >= 1u,
               "broadphase world aabb query finds body");
    expectTrue(world.queryRaycastStub(0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 5.f) >= 1u,
               "broadphase world raycast stub finds body");

    fuse::mechanics::PathComponent path("outpost_patrol");
    path.setPosition(0.f, 0.f, 0.f);
    path.addWaypoint(4.f, 0.f, 0.f);
    path.advanceAlongPath(2.f, 1.f);
    expectTrue(path.tickCount() == 1u, "path component tick counted");
    expectTrue(path.x() > 0.f, "path component advances along waypoints");

    fuse::mechanics::TimerComponent timer("outpost_timer", 0.5f);
    fuse::u32 callbackCount = 0;
    timer.setOnFire([&callbackCount]() { ++callbackCount; });
    timer.tick(1.1f);
    expectTrue(timer.fireCount() == 2u, "timer component fires periodically");
    expectTrue(callbackCount == 2u, "timer onFire callback invoked");

    fuse::mechanics::PathComponent loopPath("loop_patrol");
    loopPath.setPosition(0.f, 0.f, 0.f);
    loopPath.addWaypoint(2.f, 0.f, 0.f);
    loopPath.setLoop(true);
    loopPath.advanceAlongPath(4.f, 1.f);
    expectTrue(loopPath.loopCount() >= 1u, "path component loops waypoints");

    fuse::mechanics::AnimateComponent animate("lever_anim", 0.25f);
    animate.advance(0.5f);
    expectTrue(animate.cycleCount() >= 1u, "animate component completes cycle");

    expectTrue(world.queryRaycastStubFiltered(0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 5.f,
                                              fuse::mechanics::BroadphaseProxyFilter::Character) >= 1u,
               "filtered raycast finds character body");

    fuse::mechanics::WaypointComponent waypoint("patrol_wp", 2.f, 0.f, 0.f);
    waypoint.markVisited();
    expectTrue(waypoint.visitCount() == 1u, "waypoint component visit counted");

    fuse::mechanics::LookAtComponent lookAt("guard_look", 90.f);
    lookAt.setTarget(4.f, 0.f);
    lookAt.advanceTowardTarget(0.f, 0.f, 1.f);
    expectTrue(lookAt.tickCount() == 1u, "look-at component tick counted");

    expectTrue(world.queryDbvtOverlaps(-1.f, -1.f, -1.f, 2.f, 2.f, 2.f) >= 1u,
               "dbvt overlap query finds body");

    fuse::mechanics::RadioComponent radio("outpost_radio", "patrol");
    radio.startBroadcast();
    expectTrue(radio.broadcasting(), "radio component broadcasting");
    expectTrue(radio.broadcastCount() == 1u, "radio broadcast counted");
    expectTrue(radio.channel() == "patrol", "radio channel stored");
    radio.stopBroadcast();
    expectTrue(!radio.broadcasting(), "radio broadcast stopped");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_light_move_world: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_light_move_world: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
