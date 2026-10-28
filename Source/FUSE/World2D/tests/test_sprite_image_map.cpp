#include <fuse/core/init.hpp>
#include <fuse/world2d/sprite_image_map.hpp>
#include <fuse/world2d/sprite_index.hpp>
#include <fuse/world2d/sprite_sort.hpp>

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

void testSpriteIndexOk() {
    expectTrue(fuse::world2d::sprite_index_ok(0), "sprite_index_ok(0) is true");
    expectTrue(fuse::world2d::sprite_index_ok(1023), "sprite_index_ok(1023) is true");
    expectTrue(!fuse::world2d::sprite_index_ok(-1), "sprite_index_ok(-1) is false");
    expectTrue(!fuse::world2d::sprite_index_ok(1024), "sprite_index_ok(1024) is false");
}

void testSpriteSortKey() {
    expectTrue(fuse::world2d::sprite_sort_key(1, 20) == 100020, "layer 1 y 20 is 100020");
    expectTrue(fuse::world2d::sprite_sort_key(0, -5) == -5, "layer 0 y -5 is -5");
    expectTrue(fuse::world2d::sprite_sort_key(1, 20) > fuse::world2d::sprite_sort_key(0, 20),
               "higher layer sorts after a lower layer");
    expectTrue(fuse::world2d::sprite_sort_key(1000, 0) == 100000000, "layer 1000 is the upper clamp");
    expectTrue(fuse::world2d::sprite_sort_key(1001, 0) == 100000000, "layer above 1000 clamps");
    expectTrue(fuse::world2d::sprite_sort_key(-1000, 0) == -100000000, "layer -1000 is the lower clamp");
    expectTrue(fuse::world2d::sprite_sort_key(-1001, 0) == -100000000, "layer below -1000 clamps");
    expectTrue(fuse::world2d::sprite_sort_key(0, 100000) == 100000, "y 100000 is the upper clamp");
    expectTrue(fuse::world2d::sprite_sort_key(0, 100001) == 100000, "y above 100000 clamps");
    expectTrue(fuse::world2d::sprite_sort_key(0, -100000) == -100000, "y -100000 is the lower clamp");
    expectTrue(fuse::world2d::sprite_sort_key(0, -100001) == -100000, "y below -100000 clamps");
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
    testSpriteIndexOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_world2d_sprite_image_map_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testSpriteSortKey();
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
