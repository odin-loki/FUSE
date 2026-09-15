#include <fuse/core/init.hpp>
#include <fuse/world_partition/grid_cell.hpp>
#include <fuse/world_partition/streaming_volume.hpp>
#include <fuse/world_partition/world_partition.hpp>

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

void testGridCoordHelpers() {
    const fuse::world_partition::GridCoord coord{2, -1};
    expectTrue(fuse::world_partition::grid_coord_key(coord) != 0u, "grid key is non-zero");

    const fuse::ecs::vec3 world_pos{300.f, 0.f, -50.f};
    const fuse::world_partition::GridCoord mapped =
        fuse::world_partition::world_to_grid(world_pos, 256.f);
    expectTrue(mapped.x == 1 && mapped.y == -1, "world_to_grid maps XZ to cell indices");

    const fuse::spatial::AABB bounds = fuse::world_partition::grid_cell_bounds({0, 0}, 256.f);
    expectNear(bounds.min.x, 0.f, 1e-4f, "cell min x");
    expectNear(bounds.max.x, 256.f, 1e-4f, "cell max x");
}

void testStreamingVolumeHysteresis() {
    fuse::world_partition::StreamingVolume volume{};
    volume.center = {0.f, 0.f, 0.f, 0.f};
    volume.desc.stream_in_radius = 512.f;
    volume.desc.stream_out_radius = 600.f;

    expectTrue(volume.should_load({0, 0}, 256.f), "origin cell should load");
    expectTrue(!volume.should_unload({0, 0}, 256.f), "origin cell should not unload");

    const fuse::world_partition::GridCoord far_cell{10, 0};
    expectTrue(!volume.should_load(far_cell, 256.f), "distant cell should not load");
    expectTrue(volume.should_unload(far_cell, 256.f), "distant cell should unload when resident");
}

bool g_unload_called = false;

void on_test_load(fuse::world_partition::WorldCell& cell) {
    cell.entities.push_back(fuse::ecs::EntityID{1u, 1u});
}

void on_test_unload(fuse::world_partition::WorldCell& /*cell*/) { g_unload_called = true; }

void testWorldPartitionLoadUnloadStubs() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.cell_size = 256.f;
    desc.stream_in_distance = 512.f;
    desc.stream_out_distance = 600.f;
    desc.max_loaded_cells = 8;
    desc.async_loading = false;
    partition.init(desc);

    g_unload_called = false;
    fuse::world_partition::CellLoadCallbacks callbacks{};
    callbacks.on_load = on_test_load;
    callbacks.on_unload = on_test_unload;
    partition.set_callbacks(callbacks);

    const fuse::world_partition::GridCoord origin{0, 0};
    partition.force_load(origin);
    expectTrue(partition.cell_loaded(origin), "force_load marks cell resident");
    expectTrue(partition.cell_residency(origin) == fuse::world_partition::CellResidencyState::Resident,
               "residency is Resident after load");

    const fuse::world_partition::WorldCell* cell = partition.find_cell(origin);
    expectTrue(cell != nullptr && cell->entities.size() == 1u, "load callback populates entities");

    partition.force_unload(origin);
    expectTrue(!partition.cell_loaded(origin), "force_unload clears residency");
    expectTrue(partition.cell_residency(origin) == fuse::world_partition::CellResidencyState::Unloaded,
               "residency returns to Unloaded");
    expectTrue(g_unload_called, "unload callback invoked");

    partition.destroy();
}

void testWorldPartitionStreamingUpdate() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.cell_size = 256.f;
    desc.stream_in_distance = 400.f;
    desc.stream_out_distance = 700.f;
    desc.max_loaded_cells = 16;
    desc.async_loading = true;
    partition.init(desc);

    fuse::ecs::vec3 camera{128.f, 0.f, 128.f, 0.f};
    partition.update(camera);
    expectTrue(partition.loaded_cell_count() >= 1u, "update loads cells near camera");

    const fuse::world_partition::GridCoord camera_cell =
        fuse::world_partition::world_to_grid(camera, desc.cell_size);
    expectTrue(partition.cell_loaded(camera_cell), "camera cell becomes resident");

    fuse::ecs::vec3 far_camera{5000.f, 0.f, 5000.f, 0.f};
    for (int frame = 0; frame < 32; ++frame) {
        partition.update(far_camera);
    }
    expectTrue(partition.loaded_cell_count() <= desc.max_loaded_cells, "resident count respects cap");

    partition.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    testGridCoordHelpers();
    testStreamingVolumeHysteresis();
    testWorldPartitionLoadUnloadStubs();
    testWorldPartitionStreamingUpdate();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world_partition_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world_partition_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
