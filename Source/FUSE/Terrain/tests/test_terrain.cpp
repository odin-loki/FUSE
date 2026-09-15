#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/terrain/chunk_grid.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/lod.hpp>
#include <fuse/terrain/lod_residency_queue.hpp>
#include <fuse/terrain/queries.hpp>
#include <fuse/terrain/terrain.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

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

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
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

void testLodTransitionMorphBand() {
    const fuse::terrain::TerrainDesc desc = makeTestDesc();

    const fuse::terrain::LodTransition nearTransition = fuse::terrain::compute_lod_transition(2.f, desc.lod_levels);
    expectTrue(nearTransition.lod == 0, "near camera stays at LOD 0");
    expectNear(nearTransition.morph_factor, 0.f, 0.01f, "near camera has no morph");

    const fuse::terrain::LodTransition midRingTransition = fuse::terrain::compute_lod_transition(6.5f, desc.lod_levels);
    expectTrue(midRingTransition.lod == 0, "mid-ring still at LOD 0");
    expectTrue(midRingTransition.morph_factor > 0.f && midRingTransition.morph_factor < 0.5f,
               "mid-ring morph factor ramps gradually");

    const fuse::terrain::LodTransition edgeTransition = fuse::terrain::compute_lod_transition(7.5f, desc.lod_levels);
    expectTrue(edgeTransition.lod == 0, "ring edge still at LOD 0");
    expectTrue(edgeTransition.morph_factor > 0.5f, "ring edge morph factor active");

    const fuse::terrain::LodTransition ringBoundary = fuse::terrain::compute_lod_transition(8.f, desc.lod_levels);
    expectTrue(ringBoundary.lod == 1, "ring boundary advances to LOD 1");
    expectNear(ringBoundary.morph_factor, 0.f, 0.01f, "ring boundary resets morph factor");

    const fuse::terrain::LodTransition farTransition = fuse::terrain::compute_lod_transition(32.f, desc.lod_levels);
    expectTrue(farTransition.lod >= 2, "far camera uses coarser LOD ring");
    expectTrue(farTransition.morph_factor >= 0.f && farTransition.morph_factor <= 1.f, "morph factor clamped");
}

void testVertexMorphSnapsToGrid() {
    const fuse::terrain::TerrainDesc desc = makeTestDesc();
    const fuse::terrain::LodLevel lod1 = fuse::terrain::make_lod_level(desc, 1);
    const fuse::f32 base_stride = desc.world_size / static_cast<fuse::f32>(desc.chunk_resolution);

    const fuse::terrain::vec3 original{base_stride * 1.5f, 4.f, base_stride * 2.5f};
    const fuse::terrain::vec3 morphed =
        fuse::terrain::morph_vertex_position(original, 1, 1.f, base_stride);

    expectNear(morphed.x, base_stride * 2.f, 0.01f, "morph snaps X to coarser grid");
    expectNear(morphed.z, base_stride * 2.f, 0.01f, "morph snaps Z to coarser grid");
    expectNear(morphed.y, original.y, 0.01f, "morph preserves Y (height deferred to GPU)");

    const fuse::terrain::vec3 halfMorphed =
        fuse::terrain::morph_vertex_position(original, 1, 0.5f, base_stride);
    const fuse::f32 snapped_x = base_stride * 2.f;
    const fuse::f32 snapped_z = base_stride * 2.f;
    expectTrue(halfMorphed.x > std::min(original.x, snapped_x) && halfMorphed.x < std::max(original.x, snapped_x),
               "half morph interpolates X");
    expectTrue(halfMorphed.z > std::min(original.z, snapped_z) && halfMorphed.z < std::max(original.z, snapped_z),
               "half morph interpolates Z");

    const fuse::terrain::vec3 unchanged = fuse::terrain::morph_vertex_position(original, 0, 1.f, base_stride);
    expectNear(unchanged.x, original.x, 0.01f, "LOD 0 skips morph");
    expectNear(unchanged.z, original.z, 0.01f, "LOD 0 skips morph");

    const fuse::terrain::vec3 zeroMorph =
        fuse::terrain::morph_vertex_position(original, 2, 0.f, base_stride);
    expectNear(zeroMorph.x, original.x, 0.01f, "zero morph factor leaves position unchanged");
}

void testChunkResidencyStateHelpers() {
    using fuse::terrain::ChunkResidencyState;
    expectTrue(fuse::terrain::is_loading_state(ChunkResidencyState::QueuedLoad),
               "QueuedLoad is loading state");
    expectTrue(fuse::terrain::is_loading_state(ChunkResidencyState::Loading), "Loading is loading state");
    expectTrue(fuse::terrain::is_unloading_state(ChunkResidencyState::QueuedUnload),
               "QueuedUnload is unloading state");
    expectTrue(fuse::terrain::is_unloading_state(ChunkResidencyState::Unloading),
               "Unloading is unloading state");
    expectTrue(fuse::terrain::is_transitional_state(ChunkResidencyState::Loading),
               "Loading is transitional");
    expectTrue(fuse::terrain::is_queued_state(ChunkResidencyState::QueuedLoad), "QueuedLoad is queued");
    expectTrue(fuse::terrain::is_resident_state(ChunkResidencyState::Resident), "Resident is resident");
}

void testLodResidencyQueueStub() {
    withScheduler(1, [] {
        fuse::terrain::LodResidencyQueue queue;
        std::atomic<bool> worker_ran{false};

        fuse::terrain::LodResidencyRequest request{};
        request.chunk_index = 5;
        request.kind = fuse::terrain::LodResidencyRequestKind::Load;
        request.priority = 12.f;

        const bool submitted = queue.submit(request, [&](fuse::u32 chunk_index,
                                                         fuse::terrain::LodResidencyRequestKind kind) {
            worker_ran.store(chunk_index == 5 && kind == fuse::terrain::LodResidencyRequestKind::Load);
            return true;
        });
        expectTrue(submitted, "queue submits to JobScheduler");

        for (int attempt = 0; attempt < 100 && queue.completed_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::terrain::CompletedLodResidencyRequest> completed;
        expectEq(queue.drain_completed(completed), 1u, "drain returns completed request");
        expectTrue(worker_ran.load(), "worker stub ran on scheduler thread");
        expectTrue(completed[0].success, "completed request reports success");
        expectEq(queue.in_flight_count(), 0u, "in-flight count returns to zero");
    });
}

void testChunkGridLodTransitions() {
    fuse::terrain::ChunkGrid grid{};
    fuse::terrain::TerrainDesc desc = makeTestDesc();
    desc.async_loading = false;
    grid.init(desc);

    grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
    expectTrue(grid.visible_chunk_count() > 0, "camera near terrain loads chunks");
    expectTrue(grid.resident_chunk_count() == grid.visible_chunk_count(), "resident count matches visible");

    bool hasMorphingChunk = false;
    for (fuse::u32 i = 0; i < grid.chunk_count(); ++i) {
        const fuse::terrain::TerrainChunk& chunk = grid.chunk(i);
        if (chunk.morph_factor > 0.f) {
            hasMorphingChunk = true;
        }
        expectTrue(chunk.morph_factor >= 0.f && chunk.morph_factor <= 1.f, "chunk morph factor in range");
        if (chunk.loaded) {
            expectTrue(fuse::terrain::is_resident_state(chunk.residency), "loaded chunk is Resident");
        }
    }
    grid.update_lod({8.f, 0.f, 1.f}, 0.016f);
    for (fuse::u32 i = 0; i < grid.chunk_count(); ++i) {
        const fuse::terrain::TerrainChunk& chunk = grid.chunk(i);
        if (chunk.morph_factor > 0.f) {
            hasMorphingChunk = true;
        }
    }
    expectTrue(hasMorphingChunk, "some chunks have active morph factor in transition band");

    grid.update_lod({desc.world_size * 4.f, 0.f, desc.world_size * 4.f}, 0.016f);
    expectTrue(grid.visible_chunk_count() == 0, "camera far away unloads chunks");
    expectEq(grid.resident_chunk_count(), 0u, "no resident chunks when camera is far");
}

void testChunkGridAsyncResidency() {
    withScheduler(1, [] {
        fuse::terrain::ChunkGrid grid{};
        fuse::terrain::TerrainDesc desc = makeTestDesc();
        desc.async_loading = true;
        desc.max_async_in_flight = 2;
        grid.init(desc);

        for (int frame = 0; frame < 64 && grid.resident_chunk_count() == 0u; ++frame) {
            grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
            grid.drain_completed_requests();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        expectTrue(grid.resident_chunk_count() >= 1u, "async load completes near camera");
        expectEq(grid.in_flight_request_count(), 0u, "no in-flight requests after completion");

        for (int frame = 0; frame < 64 && grid.resident_chunk_count() > 0u; ++frame) {
            grid.update_lod({desc.world_size * 4.f, 0.f, desc.world_size * 4.f}, 0.016f);
            grid.drain_completed_requests();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        expectEq(grid.resident_chunk_count(), 0u, "async unload returns to Unloaded");
        grid.destroy();
    });
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
    desc.async_loading = false;
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
    for (const fuse::terrain::TerrainChunk* chunk : visible) {
        expectTrue(fuse::terrain::is_resident_state(chunk->residency), "visible chunk is Resident");
    }

    terrain.update_lod({8.f, 0.f, 1.f}, 0.016f);
    terrain.get_visible_chunks(visible);
    bool terrainHasMorph = false;
    for (const fuse::terrain::TerrainChunk* chunk : visible) {
        if (chunk->morph_factor > 0.f) {
            terrainHasMorph = true;
        }
    }
    expectTrue(terrainHasMorph, "terrain visible chunks carry morph factors");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHeightfieldSampling();
    testLodSelection();
    testLodTransitionMorphBand();
    testVertexMorphSnapsToGrid();
    testChunkResidencyStateHelpers();
    testLodResidencyQueueStub();
    testChunkGridLodTransitions();
    testChunkGridAsyncResidency();
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
