#include <fuse/adventure/outpost_loader.hpp>
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

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_outpost_loader: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_outpost_loader: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
