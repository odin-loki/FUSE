#include <fuse/core/init.hpp>
#include <fuse/mechanics/health_component.hpp>
#include <fuse/mechanics/health_ratio.hpp>
#include <fuse/mechanics/hp_cap.hpp>

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

void testHealthDamageAndHeal() {
    fuse::mechanics::HealthComponent health("actor", 100);
    expectTrue(health.attach(), "health component attaches");

    expectTrue(health.applyDamage(30) == 30, "damage applies");
    expectTrue(health.currentHealth() == 70, "health reduced");
    expectTrue(health.isAlive(), "still alive");

    expectTrue(health.heal(20) == 20, "heal applies");
    expectTrue(health.currentHealth() == 90, "health restored");

    expectTrue(health.applyDamage(200) == 90, "overkill clamps");
    expectTrue(!health.isAlive(), "actor dies");
    expectTrue(health.heal(10) == 0, "dead actor ignores heal");
}

void testHealthInterfaceLookup() {
    fuse::mechanics::Component root("enemy");
    fuse::mechanics::HealthComponent health("vitals", 50);
    expectTrue(root.addComponent(&health), "root accepts health");
    expectTrue(root.attach(), "root attaches");

    auto* iface = root.getInterface<fuse::mechanics::HealthProviderInterface>("mechanics", "health");
    expectTrue(iface != nullptr, "health interface resolves");
    expectTrue(iface->maxHealth() == 50, "interface reads max health");
    expectTrue(iface->applyDamage(10) == 10, "interface applies damage");
    expectTrue(iface->currentHealth() == 40, "interface reads current health");
}

void testHpCapOk() {
    expectTrue(fuse::mechanics::hp_cap_ok(1), "hp_cap_ok(1) is true");
    expectTrue(fuse::mechanics::hp_cap_ok(100000), "hp_cap_ok(100000) is true");
    expectTrue(!fuse::mechanics::hp_cap_ok(0), "hp_cap_ok(0) is false");
    expectTrue(!fuse::mechanics::hp_cap_ok(100001), "hp_cap_ok(100001) is false");
}

void testHealthRatio() {
    expectTrue(fuse::mechanics::health_ratio(5.f, 10.f) == 0.5f, "health_ratio(5, 10) == 0.5");
    expectTrue(fuse::mechanics::health_ratio(12.f, 10.f) == 1.f, "health_ratio(12, 10) == 1");
    expectTrue(fuse::mechanics::health_ratio(1.f, 0.f) == 0.f, "health_ratio(1, 0) == 0");
    expectTrue(fuse::mechanics::health_ratio(0.f, 10.f) == 0.f, "zero current returns 0");
    expectTrue(fuse::mechanics::health_ratio(-4.f, 10.f) == 0.f, "negative current counts as 0");
    expectTrue(fuse::mechanics::health_ratio(5.f, -1.f) == 0.f, "negative max health returns 0");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(fuse::mechanics::health_ratio(nan, 10.f) == 0.f, "non-finite current returns 0");
    expectTrue(fuse::mechanics::health_ratio(5.f, nan) == 0.f, "non-finite max health returns 0");
    expectTrue(fuse::mechanics::health_ratio(inf, 10.f) == 0.f, "infinite current returns 0");
    expectTrue(fuse::mechanics::health_ratio(5.f, -inf) == 0.f, "infinite max health returns 0");
}

} // namespace

int main() {
    testHpCapOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_health_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testHealthRatio();
    fuse::core::initialize();
    testHealthDamageAndHeal();
    testHealthInterfaceLookup();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_health_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_health_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
