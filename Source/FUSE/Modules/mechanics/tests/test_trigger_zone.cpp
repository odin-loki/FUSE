#include <fuse/core/init.hpp>
#include <fuse/mechanics/trigger_zone.hpp>

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

void testTriggerEnterLeave() {
    fuse::mechanics::AxisAlignedBox box{};
    box.minX = 0.f;
    box.minY = 0.f;
    box.minZ = 0.f;
    box.maxX = 10.f;
    box.maxY = 10.f;
    box.maxZ = 10.f;

    fuse::mechanics::TriggerZoneComponent zone("outpost_entry", box);
    fuse::u32 enteredId = 0;
    fuse::u32 leftId = 0;
    zone.setOnEnter([&](fuse::u32 id) { enteredId = id; });
    zone.setOnLeave([&](fuse::u32 id) { leftId = id; });

    zone.testObject(42, 5.f, 5.f, 5.f);
    expectTrue(zone.enterCount() == 1u, "trigger enter fires once");
    expectTrue(enteredId == 42u, "trigger enter records object id");

    zone.testObject(42, 5.f, 5.f, 5.f);
    expectTrue(zone.enterCount() == 1u, "trigger enter does not repeat while inside");

    zone.testObject(42, 20.f, 5.f, 5.f);
    expectTrue(zone.leaveCount() == 1u, "trigger leave fires on exit");
    expectTrue(leftId == 42u, "trigger leave records object id");
}

void testTriggerPeriodicTick() {
    fuse::mechanics::TriggerZoneComponent zone;
    zone.setTickPeriodMs(100);
    fuse::u32 tickFires = 0;
    zone.setOnTick([&]() { ++tickFires; });

    zone.advance(50);
    expectTrue(tickFires == 0u, "trigger tick waits for period");

    zone.advance(150);
    expectTrue(tickFires == 1u, "trigger tick fires after period");
    expectTrue(zone.tickCount() == 1u, "trigger tick count increments");
}

} // namespace

int main() {
    fuse::core::initialize();

    testTriggerEnterLeave();
    testTriggerPeriodicTick();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics trigger zone tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics trigger zone tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
