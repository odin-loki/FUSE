#include <fuse/math/morton.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/svo.hpp>
#include <fuse/scene/svo_depth.hpp>

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

void testSvoSetGetRoundTrip() {
    fuse::scene::SVO svo;
    fuse::scene::SVODesc desc{};
    desc.rootSize = 64.f;
    desc.maxDepth = 4;
    svo.init(desc);

    svo.set({1, 2, 3}, 42u);
    svo.set({4, 5, 6}, 7u);

    expectTrue(svo.get({1, 2, 3}) == 42u, "SVO set/get round-trip first voxel");
    expectTrue(svo.get({4, 5, 6}) == 7u, "SVO set/get round-trip second voxel");
    expectTrue(svo.get({0, 0, 0}) == 0u, "unset voxel reads zero");
    expectTrue(svo.voxelCount() == 2u, "voxel count tracks inserts");
    expectTrue(svo.nodeCount() >= 1u, "node pool allocated");
}

void testSvoFill() {
    fuse::scene::SVO svo;
    fuse::scene::SVODesc desc{};
    desc.rootSize = 32.f;
    desc.maxDepth = 3;
    svo.init(desc);

    svo.fill({0, 0, 0}, {1, 1, 1}, 9u);
    expectTrue(svo.get({0, 0, 0}) == 9u, "fill writes corner voxel");
    expectTrue(svo.get({1, 1, 1}) == 9u, "fill writes opposite corner voxel");
    expectTrue(svo.voxelCount() == 8u, "fill writes 2^3 voxels");
}

void testSvoCarveRemovesSolid() {
    fuse::scene::SVO svo;
    fuse::scene::SVODesc desc{};
    desc.rootSize = 32.f;
    desc.maxDepth = 3;
    svo.init(desc);

    svo.fill({0, 0, 0}, {7, 7, 7}, 1u);
    const fuse::scene::vec3 center = svo.desc().origin + fuse::scene::vec3(18.f, 18.f, 18.f);
    svo.carve(center, 6.f);

    expectTrue(svo.get({4, 4, 4}) == 0u, "carve clears voxels near sphere center");
    expectTrue(svo.get({7, 7, 7}) == 1u, "carve preserves distant solid voxels");
}

void testSvoSdfQuery() {
    fuse::scene::SVO svo;
    fuse::scene::SVODesc desc{};
    desc.rootSize = 32.f;
    desc.maxDepth = 3;
    svo.init(desc);

    svo.set({2, 2, 2}, 1u);
    const fuse::scene::vec3 sample = svo.desc().origin + fuse::scene::vec3(10.f, 10.f, 10.f);
    const fuse::scene::f32 sdf = svo.sdfQuery(sample);

    expectTrue(sdf < svo.desc().rootSize, "SDF query returns finite distance near solid voxel");
}

void testSvoRayCastHitsSolid() {
    fuse::scene::SVO svo;
    fuse::scene::SVODesc desc{};
    desc.rootSize = 32.f;
    desc.maxDepth = 3;
    svo.init(desc);

    svo.set({3, 3, 3}, 1u);
    const fuse::scene::vec3 origin = svo.desc().origin + fuse::scene::vec3(0.f, 14.f, 14.f);
    const fuse::scene::vec3 direction{1.f, 0.f, 0.f};

    fuse::scene::ivec3 hitVoxel{};
    fuse::scene::vec3 hitNormal{};
    fuse::scene::f32 hitDistance = 0.f;
    const bool hit = svo.rayCast(origin, direction, 64.f, hitVoxel, hitNormal, hitDistance);

    expectTrue(hit, "SVO ray cast hits solid voxel");
    expectTrue(hitVoxel.x == 3 && hitVoxel.y == 3 && hitVoxel.z == 3, "ray cast returns hit voxel coord");
    expectTrue(hitDistance > 0.f, "ray cast reports positive distance");
}

void testMorton2() {
    expectTrue(fuse::math::morton2(1u, 0u) == 1u, "morton2(1, 0) is 1");
    expectTrue(fuse::math::morton2(0u, 1u) == 2u, "morton2(0, 1) is 2");
    const auto code = fuse::math::morton2(3u, 1u);
    expectTrue(fuse::math::morton2_x(code) == 3u, "morton2_x round-trips x of morton2(3, 1)");
    expectTrue(fuse::math::morton2_y(code) == 1u, "morton2_y round-trips y of morton2(3, 1)");
}

void testSvoDepthOk() {
    expectTrue(fuse::scene::svo_depth_ok(1), "svo depth 1");
    expectTrue(fuse::scene::svo_depth_ok(12), "svo depth 12");
    expectTrue(!fuse::scene::svo_depth_ok(0), "svo depth 0 rejected");
    expectTrue(!fuse::scene::svo_depth_ok(13), "svo depth 13 rejected");
}

} // namespace

int main() {
    testSvoDepthOk();
    testSvoSetGetRoundTrip();
    testSvoFill();
    testSvoCarveRemovesSolid();
    testSvoSdfQuery();
    testSvoRayCastHitsSolid();
    testMorton2();

    if (g_failures == 0) {
        std::printf("fuse scene svo tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse scene svo tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
