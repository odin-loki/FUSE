#include <fuse/core/init.hpp>
#include <fuse/terrain/chunk_grid.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/lod.hpp>
#include <fuse/terrain/queries.hpp>
#include <fuse/terrain/terrain.hpp>

#include <cmath>
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

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

fuse::terrain::TerrainDesc makeTestDesc() {
    fuse::terrain::TerrainDesc desc{};
    desc.resolution = 33;
    desc.world_size = 32.f;
    desc.max_height = 16.f;
    desc.lod_levels = 4;
    desc.chunk_resolution = 16;
    desc.has_svo_caves = true;
    desc.svo_depth = 6;
    return desc;
}

void testHeightfieldSampling() {
    fuse::terrain::Heightfield field{};
    const fuse::terrain::TerrainDesc desc = makeTestDesc();
    field.init(desc);
    field.fill(0.f);
    field.set_height(0, 0, 4.f);
    field.set_height(1, 0, 8.f);
    field.set_height(0, 1, 12.f);
    field.set_height(1, 1, 16.f);

    expectTrue(field.is_initialized(), "heightfield initialized");
    expectNear(field.sample_height(0.f, 0.f), 4.f, 0.01f, "corner height sample");
    expectNear(field.sample_height(1.f, 0.f), 8.f, 0.01f, "edge height sample");
    expectNear(field.sample_height(0.5f, 0.5f), 10.f, 0.01f, "bilinear centre sample");

    field.fill(8.f);
    expectNear(field.sample_normal(4.f, 4.f).y, 1.f, 0.05f, "flat heightfield normal points up");
}

void testLodSelection() {
    const fuse::terrain::TerrainDesc desc = makeTestDesc();
    expectTrue(fuse::terrain::select_lod_level(4.f, desc.lod_levels) == 0, "near camera uses LOD 0");
    expectTrue(fuse::terrain::select_lod_level(64.f, desc.lod_levels) >= 2, "far camera uses coarser LOD");

    const fuse::terrain::LodLevel lod0 = fuse::terrain::make_lod_level(desc, 0);
    const fuse::terrain::LodLevel lod2 = fuse::terrain::make_lod_level(desc, 2);
    expectTrue(lod0.texel_step == 1, "LOD 0 texel step");
    expectTrue(lod2.texel_step == 4, "LOD 2 texel step");
    expectTrue(lod2.world_stride > lod0.world_stride, "coarser LOD has larger stride");
}

void testChunkGridLodUpdate() {
    fuse::terrain::ChunkGrid grid{};
    const fuse::terrain::TerrainDesc desc = makeTestDesc();
    grid.init(desc);

    expectTrue(grid.is_initialized(), "chunk grid initialized");
    expectTrue(grid.chunk_count() > 0, "chunk grid has chunks");

    grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
    expectTrue(grid.visible_chunk_count() > 0, "camera near origin loads chunks");

    grid.update_lod({desc.world_size * 4.f, 0.f, desc.world_size * 4.f}, 0.016f);
    expectTrue(grid.visible_chunk_count() == 0, "camera far away unloads chunks");
}

void testHeightfieldRaycast() {
    fuse::terrain::Heightfield field{};
    const fuse::terrain::TerrainDesc desc = makeTestDesc();
    field.init(desc);
    field.fill(8.f);

    const fuse::terrain::HeightfieldRayHit hit =
        fuse::terrain::raycast_heightfield(field, {0.f, 20.f, 0.f}, {0.f, -1.f, 0.f}, 100.f);
    expectTrue(hit.hit, "ray hits flat heightfield");
    expectNear(hit.position.y, 8.f, 0.2f, "ray hit height");
    expectNear(hit.distance, 12.f, 0.5f, "ray hit distance");
}

void testTerrainFacade() {
    fuse::terrain::Terrain terrain{};
    fuse::terrain::TerrainDesc desc = makeTestDesc();
    terrain.init(desc);
    terrain.generate(42);

    expectTrue(terrain.is_initialized(), "terrain initialized");
    expectTrue(terrain.get_height(0.f, 0.f) >= 0.f, "generated height non-negative");
    expectTrue(terrain.get_height(0.f, 0.f) <= desc.max_height, "generated height within max");

    const fuse::f32 before = terrain.get_height(desc.world_size * 0.25f, desc.world_size * 0.25f);
    terrain.deform({desc.world_size * 0.25f, 0.f, desc.world_size * 0.25f}, 4.f, 2.f);
    const fuse::f32 after = terrain.get_height(desc.world_size * 0.25f, desc.world_size * 0.25f);
    expectTrue(after > before, "deform raises terrain");

    fuse::terrain::vec3 hit{};
    fuse::terrain::vec3 normal{};
    fuse::f32 distance = 0.f;
    expectTrue(terrain.ray_cast({desc.world_size * 0.25f, 20.f, desc.world_size * 0.25f}, {0.f, -1.f, 0.f}, 100.f,
                               hit, normal, distance),
               "terrain ray cast succeeds");
    expectTrue(distance > 0.f, "terrain ray cast distance positive");

    terrain.update_lod({0.f, 0.f, 0.f}, 0.016f);
    std::vector<const fuse::terrain::TerrainChunk*> visible{};
    terrain.get_visible_chunks(visible);
    expectTrue(!visible.empty(), "terrain exposes visible chunks");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHeightfieldSampling();
    testLodSelection();
    testChunkGridLodUpdate();
    testHeightfieldRaycast();
    testTerrainFacade();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_terrain_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_terrain_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
