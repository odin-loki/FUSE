#include <fuse/adventure/outpost_id.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_range.hpp>
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

void testOutpostIdOk() {
    expectTrue(fuse::adventure::outpost_id_ok("north_gate"), "north_gate is a valid outpost id");
    expectTrue(!fuse::adventure::outpost_id_ok("North"), "uppercase first letter is invalid");
    expectTrue(!fuse::adventure::outpost_id_ok(""), "empty outpost id is invalid");
    expectTrue(fuse::adventure::outpost_id_ok("a"), "single lowercase letter is valid");
    expectTrue(!fuse::adventure::outpost_id_ok("1gate"), "leading digit is invalid");
    expectTrue(!fuse::adventure::outpost_id_ok("_gate"), "leading underscore is invalid");
    expectTrue(!fuse::adventure::outpost_id_ok("noRth"), "uppercase letter is invalid");
    expectTrue(fuse::adventure::outpost_id_ok("a1234567890123456789012345678901"),
               "32-character outpost id is valid");
    expectTrue(!fuse::adventure::outpost_id_ok("a12345678901234567890123456789012"),
               "33-character outpost id is invalid");
}

void test_outpost_range_3_4_5() {
    const float distance = fuse::adventure::outpost_distance(3.f, 4.f, 0.f, 0.f);
    expectTrue(distance == 5.f, "test_outpost_range_3_4_5 distance is 5");
    expectTrue(fuse::adventure::outpost_in_range(3.f, 4.f, 0.f, 0.f, 5.f),
               "test_outpost_range_3_4_5 radius 5 is in range");
    expectTrue(!fuse::adventure::outpost_in_range(3.f, 4.f, 0.f, 0.f, 4.9f),
               "test_outpost_range_3_4_5 radius 4.9 is not in range");
}

} // namespace

int main() {
    testOutpostIdOk();
    fuse::core::initialize();

    fuse::adventure::OutpostStubContent content;
    std::string error;
    expectTrue(fuse::adventure::loadEmbeddedOutpostStub(content, &error), "embedded outpost stub loads");
    expectTrue(content.scene == "Outpost", "scene name parsed");
    expectTrue(content.conversations.count("outpost_guard") == 1, "guard conversation present");
    expectTrue(content.conversations["outpost_guard"].lines.size() == 2u, "guard has two lines");
    expectTrue(content.doors.count("maintenance_door") == 1, "maintenance door present");
    expectTrue(content.weaponPickups.count("armory_rifle") == 1, "armory rifle present");
    expectTrue(content.placements.count("outpost_guard") == 1, "guard placement parsed");
    expectTrue(content.placements.at("outpost_guard").x == 2.f, "guard placement x parsed");

    test_outpost_range_3_4_5();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_outpost_loader: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_outpost_loader: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
