#include <fuse/physics/phase4_test_registry.hpp>

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

void testCatalogAndCategories() {
    const auto& catalog = fuse::physics::Phase4TestRegistry::catalog();
    expectTrue(!catalog.empty(), "phase 4 catalog non-empty");
    expectTrue(fuse::physics::Phase4TestRegistry::countByCategory(
                   fuse::physics::Phase4TestCategory::Destruction) >= 1u,
               "destruction category present");
    expectTrue(fuse::physics::Phase4TestRegistry::automatedCount() >= 1u, "automated tests registered");
}

void testAutomatedSmoke() {
    expectTrue(fuse::physics::Phase4TestRegistry::runAutomatedSmoke(), "automated smoke passes");
}

} // namespace

int main() {
    testCatalogAndCategories();
    testAutomatedSmoke();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
