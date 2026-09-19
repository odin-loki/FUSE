#include <fuse/core/init.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

void testWorld2DLoadWorldBridge() {
    const std::string module = writeTempFile(
        "/tmp/fuse_world2d_bridge.cs",
        R"(module "SpriteToy";
new SceneToy() {
  new SpritePlayer(Player) {
    position = "3 4";
  };
};)");
    const std::string fuselevel = "/tmp/fuse_world2d_bridge.fuselevel";

    const fuse::project::ConvertResult converted =
        fuse::project::convertT2DModuleToFuselevel(module, fuselevel);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "t2d convert ok for bridge");

    fuse::world2d::World2D world;
    world.setFuselevelPath(fuselevel);
    const fuse::dimension::WorldHandle handle(2u, 1u);
    world.loadWorld(handle);

    const fuse::world2d::FuselevelLoadResult& stats = world.lastFuselevelLoad();
    expectTrue(stats.ok, "loadWorld bridge populated fuselevel");
    expectTrue(stats.entityCount >= 2u, "bridge loaded scene nodes");
    expectTrue(world.activeWorld() == handle, "active world handle stored");
}

} // namespace

int main() {
    fuse::core::initialize();
    testWorld2DLoadWorldBridge();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world2d_fuselevel_bridge_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world2d_fuselevel_bridge_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
