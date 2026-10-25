#include <fuse/core/init.hpp>
#include <fuse/world2d/sprite_image_map.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testBundledHeroSheetSample() {
    fuse::world2d::SpriteImageMapSearchRoots roots;
    roots.repositoryRoot = FUSE_SOURCE_DIR;
    fuse::u8 r0 = 0;
    fuse::u8 g0 = 0;
    fuse::u8 b0 = 0;
    fuse::u8 r1 = 0;
    fuse::u8 g1 = 0;
    fuse::u8 b1 = 0;
    expectTrue(fuse::world2d::sampleSpriteImageMapFrame(roots, "HeroSheet.png", 0u, 8u, r0, g0, b0),
               "HeroSheet frame 0 samples");
    expectTrue(fuse::world2d::sampleSpriteImageMapFrame(roots, "HeroSheet.png", 1u, 8u, r1, g1, b1),
               "HeroSheet frame 1 samples");
    expectTrue(r0 != r1 || g0 != g1 || b0 != b1, "adjacent atlas frames differ when PNG present or procedural");
}

} // namespace

int main() {
    fuse::core::initialize();
    testBundledHeroSheetSample();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world2d_sprite_image_map_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world2d_sprite_image_map_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
