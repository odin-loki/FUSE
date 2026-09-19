#include <fuse/core/init.hpp>
#include <fuse/mechanics/health_component.hpp>

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

} // namespace

int main() {
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
