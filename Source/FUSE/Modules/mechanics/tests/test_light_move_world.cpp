#include <fuse/mechanics/light_component.hpp>
#include <fuse/mechanics/move_component.hpp>
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

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_light_move_world: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_light_move_world: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
