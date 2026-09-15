#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/terrain/chunk_grid.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/lod.hpp>
#include <fuse/terrain/lod_residency_budget.hpp>
#include <fuse/terrain/lod_residency_queue.hpp>
#include <fuse/terrain/lod_residency_set.hpp>
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

void testMorphFactorClamp() {
    expectNear(fuse::terrain::clamp_morph_factor(-0.5f), 0.f, 0.001f, "negative morph clamps to zero");
    expectNear(fuse::terrain::clamp_morph_factor(0.f), 0.f, 0.001f, "zero morph unchanged");
    expectNear(fuse::terrain::clamp_morph_factor(0.42f), 0.42f, 0.001f, "in-range morph unchanged");
    expectNear(fuse::terrain::clamp_morph_factor(1.f), 1.f, 0.001f, "unity morph unchanged");
    expectNear(fuse::terrain::clamp_morph_factor(1.5f), 1.f, 0.001f, "above-one morph clamps to one");

    const fuse::terrain::LodResidencyMorphSnapshot snapshot =
        fuse::terrain::capture_morph_snapshot(2, 2.f);
    expectEq(snapshot.lod, 2u, "snapshot preserves lod");
    expectNear(snapshot.morph_factor, 1.f, 0.001f, "snapshot clamps morph factor");
}

void testAdjacentLodPair() {
    const fuse::terrain::TerrainDesc desc = makeTestDesc();

    const fuse::terrain::LodTransition transition = fuse::terrain::compute_lod_transition(7.f, desc.lod_levels);
    const fuse::terrain::AdjacentLodPair pair = fuse::terrain::make_adjacent_lod_pair(transition, desc.lod_levels);
    expectEq(pair.fine_lod, transition.lod, "adjacent pair fine lod matches transition");
    expectEq(pair.coarse_lod, pair.fine_lod + 1, "adjacent pair coarse lod is fine + 1");
    expectTrue(pair.morph_factor >= 0.f && pair.morph_factor <= 1.f, "adjacent pair morph clamped");

    const fuse::terrain::LodTransition maxTransition =
        fuse::terrain::compute_lod_transition(1000.f, desc.lod_levels);
    const fuse::terrain::AdjacentLodPair maxPair =
        fuse::terrain::make_adjacent_lod_pair(maxTransition, desc.lod_levels);
    expectEq(maxPair.fine_lod, desc.lod_levels - 1, "max ring clamps fine lod");
    expectEq(maxPair.coarse_lod, desc.lod_levels - 1, "max ring coarse lod stays at finest available");

    const fuse::f32 base_stride = desc.world_size / static_cast<fuse::f32>(desc.chunk_resolution);
    const fuse::terrain::vec3 original{base_stride * 1.5f, 3.f, base_stride * 2.5f};
    const fuse::terrain::vec3 blended = fuse::terrain::blend_morph_between_lods(original, pair, base_stride);
    const fuse::terrain::vec3 morphed =
        fuse::terrain::morph_vertex_position(original, pair.coarse_lod, pair.morph_factor, base_stride);
    expectNear(blended.x, morphed.x, 0.01f, "blend matches coarse morph at same factor");
    expectNear(blended.z, morphed.z, 0.01f, "blend matches coarse morph Z");
}

void testCollectEvictionCandidatesOrdering() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(residency.add(1u, 100.f), "add near chunk");
    expectTrue(residency.add(3u, 500.f), "add mid chunk");
    expectTrue(residency.add(5u, 900.f), "add far chunk");

    const auto all = residency.collect_eviction_candidates();
    expectEq(static_cast<fuse::u32>(all.size()), 3u, "collect all candidates");
    expectTrue(all[0] == 5u && all[1] == 3u && all[2] == 1u, "candidates sorted farthest-first");

    const auto top_two = residency.collect_eviction_candidates(2u);
    expectEq(static_cast<fuse::u32>(top_two.size()), 2u, "collect limits candidate count");
    expectTrue(top_two[0] == 5u && top_two[1] == 3u, "limited list keeps eviction order");

    residency.clear();
    expectTrue(residency.collect_eviction_candidates().empty(), "empty residency has no candidates");
}

void testResidencyHelperStubs() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(fuse::terrain::try_add_resident(residency, 2u, 50.f), "try_add_resident accepts valid focus");
    expectTrue(!fuse::terrain::try_add_resident(residency, 3u, -1.f), "try_add_resident rejects negative focus");
    expectTrue(residency.contains(2u), "stub add tracks resident chunk");
    expectTrue(fuse::terrain::try_remove_resident(residency, 2u), "try_remove_resident evicts chunk");
    expectTrue(!residency.contains(2u), "stub remove clears resident chunk");
}

void testIncomingOutranksResident() {
    const fuse::f32 load_radius = 24.f;
    expectTrue(fuse::terrain::incoming_outranks_resident(20.f, load_radius, 10.f),
               "closer incoming outranks farther resident");
    expectTrue(!fuse::terrain::incoming_outranks_resident(5.f, load_radius, 10.f),
               "farther incoming does not outrank nearer resident");
    expectTrue(!fuse::terrain::incoming_outranks_resident(0.f, load_radius, 100.f),
               "zero incoming priority never outranks");
}

void testChunkGridResidentCapEviction() {
    fuse::terrain::ChunkGrid grid{};
    fuse::terrain::TerrainDesc desc = makeTestDesc();
    desc.async_loading = false;
    desc.max_resident_chunks = 2;
    desc.load_radius = 28.f; // origin reaches three 16 m chunks; cap keeps two resident
    grid.init(desc);

    grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
    expectEq(grid.resident_chunk_count(), 2u, "cap limits initial residents");
    expectEq(grid.budget_counters().budget_evictions, 0u, "no evictions before cap pressure");

    // Move focus toward the far corner so a nearer chunk outranks an existing resident.
    grid.update_lod({30.f, 0.f, 30.f}, 0.016f);
    expectEq(grid.budget_counters().budget_evictions, 1u, "cap pressure evicts farthest resident");
    expectEq(grid.budget_counters().rejected_loads, 0u, "successful eviction avoids rejection");
    expectEq(grid.resident_chunk_count(), 2u, "resident count stays at cap after eviction load");

    grid.destroy();
}

void testLodResidencySetAddRemove() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(residency.empty(), "new residency set is empty");
    expectEq(residency.pick_eviction_candidate(), fuse::terrain::kInvalidChunkIndex,
             "empty set has no eviction candidate");

    expectTrue(residency.add(1u, 100.f), "add near chunk");
    expectTrue(residency.add(5u, 900.f), "add far chunk");
    expectEq(residency.size(), 2u, "two resident chunks tracked");
    expectTrue(residency.contains(1u) && residency.contains(5u), "contains resident chunks");
    expectNear(residency.focus_distance_for(5u), 900.f, 1e-4f, "focus distance stored for far chunk");

    expectTrue(residency.update_focus_distance(1u, 50.f), "update near focus distance");
    expectEq(residency.pick_eviction_candidate(), 5u, "farthest focus distance evicts first");

    expectTrue(residency.remove(1u), "remove near chunk");
    expectEq(residency.size(), 1u, "size drops after remove");
    expectTrue(!residency.contains(1u), "removed chunk no longer resident");
    expectEq(residency.pick_eviction_candidate(), 5u, "remaining chunk is eviction candidate");

    const auto candidates = residency.collect_eviction_candidates();
    expectEq(static_cast<fuse::u32>(candidates.size()), 1u, "one eviction candidate remains");
    expectEq(candidates[0], 5u, "eviction candidate matches remaining chunk");

    residency.clear();
    expectTrue(residency.empty(), "clear empties residency set");
}

void testLodClampHelpers() {
    expectEq(fuse::terrain::clamp_lod_level(99u, 4u), 3u, "lod clamps to max ring");
    expectEq(fuse::terrain::clamp_lod_level(0u, 0u), 0u, "zero max lod levels returns zero");
    expectEq(fuse::terrain::clamp_lod_level(2u, 8u), 2u, "in-range lod unchanged");

    expectTrue(fuse::terrain::resident_cap_unlimited(0u), "zero resident cap is unlimited");
    expectEq(fuse::terrain::resident_chunk_headroom(4u, 2u), 2u, "resident headroom subtracts count");
    expectEq(fuse::terrain::resident_chunk_headroom(2u, 4u), 0u, "over-cap headroom is zero");
    expectTrue(fuse::terrain::can_accept_resident_chunk(4u, 3u), "under cap accepts chunk");
    expectTrue(!fuse::terrain::can_accept_resident_chunk(4u, 4u), "at cap rejects chunk");
    expectEq(fuse::terrain::effective_tick_budget(8u, 3u), 3u, "tick budget clamps to cap");
    expectEq(fuse::terrain::clamp_pending_submits(5u, 2u), 2u, "pending submits clamp to cap");
}

void testLodSkirtStubs() {
    const fuse::terrain::LodSkirtParams raw{12.f, 0u};
    const fuse::terrain::LodSkirtParams clamped = fuse::terrain::clamp_skirt_params(raw, 8.f);
    expectNear(clamped.depth, 8.f, 0.001f, "skirt depth clamps to max");
    expectEq(clamped.segments, 1u, "zero segments clamp to one");

    expectEq(fuse::terrain::compute_skirt_vertex_strip_count(5u, 2u), 10u, "strip count multiplies edge verts");
    expectEq(fuse::terrain::compute_skirt_vertex_strip_count(0u, 2u), 0u, "zero edge verts yields zero strips");

    const fuse::terrain::TerrainDesc desc = makeTestDesc();
    const fuse::terrain::LodMeshVertexCounts defaultSkirts =
        fuse::terrain::compute_lod_mesh_vertex_counts(desc.chunk_resolution, 0, desc.lod_levels);
    const fuse::terrain::LodMeshVertexCounts extraSegments =
        fuse::terrain::compute_lod_mesh_vertex_counts(desc.chunk_resolution, 0, desc.lod_levels, true, 0u,
                                                      {4.f, 2u});
    expectTrue(extraSegments.skirt_vertices > defaultSkirts.skirt_vertices,
               "extra skirt segments increase skirt vertex count");
}

void testEmptyTerrainResidency() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(residency.collect_eviction_candidates().empty(), "empty residency has no candidates");
    expectTrue(!residency.remove(0u), "remove on empty set fails");
    expectTrue(!residency.add(0u, -1.f), "negative focus distance rejected");

    fuse::terrain::ChunkGrid grid{};
    expectEq(grid.chunk_count(), 0u, "uninitialized grid has no chunks");
    expectTrue(grid.residency_set().empty(), "uninitialized grid has empty residency set");

    fuse::terrain::TerrainDesc desc = makeTestDesc();
    desc.async_loading = false;
    grid.init(desc);
    grid.update_lod({desc.world_size * 8.f, 0.f, desc.world_size * 8.f}, 0.016f);
    expectEq(grid.resident_chunk_count(), 0u, "far camera keeps terrain empty");
    expectTrue(grid.residency_set().empty(), "no residents tracked when camera is far");
    grid.destroy();
    expectTrue(grid.residency_set().empty(), "destroy clears residency set");

    const fuse::terrain::LodTransition transition = fuse::terrain::compute_lod_transition(4.f, 0u);
    expectEq(transition.lod, 0u, "zero max lod levels keeps lod at zero");
    expectNear(transition.morph_factor, 0.f, 0.001f, "zero max lod levels has no morph");
}

void testLodMeshVertexCounts() {
    const fuse::terrain::TerrainDesc desc = makeTestDesc();

    const fuse::terrain::LodMeshVertexCounts lod0 =
        fuse::terrain::compute_lod_mesh_vertex_counts(desc.chunk_resolution, 0, desc.lod_levels);
    expectEq(lod0.grid_vertices, (desc.chunk_resolution + 1) * (desc.chunk_resolution + 1),
             "LOD 0 grid vertex count");
    expectEq(lod0.skirt_vertices, 4u * (desc.chunk_resolution + 1), "LOD 0 skirt vertex count");
    expectEq(lod0.seam_vertices, 0u, "LOD 0 has no seam verts without neighbor delta");
    expectEq(lod0.total_vertices, lod0.grid_vertices + lod0.skirt_vertices + lod0.seam_vertices,
             "LOD 0 total vertex count");

    const fuse::terrain::LodMeshVertexCounts lod1 =
        fuse::terrain::compute_lod_mesh_vertex_counts(desc.chunk_resolution, 1, desc.lod_levels);
    const fuse::u32 lod1_edge = desc.chunk_resolution / 2u + 1u;
    expectEq(lod1.grid_vertices, lod1_edge * lod1_edge, "LOD 1 grid vertex count halves stride");
    expectTrue(lod1.total_vertices < lod0.total_vertices, "coarser LOD has fewer total vertices");

    const fuse::terrain::LodMeshVertexCounts withSeam =
        fuse::terrain::compute_lod_mesh_vertex_counts(desc.chunk_resolution, 0, desc.lod_levels, true, 1u);
    expectTrue(withSeam.seam_vertices > 0u, "neighbor LOD delta adds seam vertices");
    expectEq(withSeam.total_vertices, withSeam.grid_vertices + withSeam.skirt_vertices + withSeam.seam_vertices,
             "seam total includes all buckets");
}

void testResidencyMorphSync() {
    fuse::terrain::TerrainChunk chunk{};
    chunk.lod = 1;
    chunk.morph_factor = 0.75f;
    chunk.loaded = true;
    chunk.residency = fuse::terrain::ChunkResidencyState::Resident;

    const fuse::terrain::LodResidencyMorphSnapshot snapshot =
        fuse::terrain::capture_morph_snapshot(chunk.lod, chunk.morph_factor);
    chunk.morph_factor = 0.f;
    fuse::terrain::sync_morph_after_residency(chunk, snapshot);
    expectNear(chunk.morph_factor, 0.75f, 0.001f, "morph restored when lod matches snapshot");
    expectTrue(chunk.dirty, "restored morph marks chunk dirty");

    chunk.lod = 2;
    chunk.morph_factor = 0.f;
    fuse::terrain::sync_morph_after_residency(chunk, snapshot);
    expectNear(chunk.morph_factor, 0.f, 0.001f, "morph not applied when lod diverged during async I/O");
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

void testAdjacentLodMorphBlend() {
    const fuse::terrain::AdjacentLodPair raw{0, 1, 1.5f};
    const fuse::terrain::AdjacentLodPair clamped = fuse::terrain::clamp_adjacent_lod_pair(raw);
    expectNear(clamped.morph_factor, 1.f, 0.001f, "adjacent pair morph clamps above one");

    expectNear(fuse::terrain::blend_adjacent_lod_morph(0.f, 1.f, 0.5f), 0.5f, 0.001f,
               "adjacent morph blend interpolates");
    expectNear(fuse::terrain::blend_adjacent_lod_morph(0.2f, 0.8f, 2.f), 0.8f, 0.001f,
               "adjacent morph blend clamps blend factor");
    expectNear(fuse::terrain::blend_adjacent_lod_morph(-0.5f, 1.5f, 0.25f), 0.25f, 0.001f,
               "adjacent morph blend clamps endpoints");
}

void testResidencyPriorityPromoteDemote() {
    expectNear(fuse::terrain::promote_residency_priority(4.f, 6.f), 6.f, 0.001f, "promote keeps higher priority");
    expectNear(fuse::terrain::promote_residency_priority(8.f, 3.f), 8.f, 0.001f, "promote keeps existing when higher");
    expectNear(fuse::terrain::demote_residency_priority(10.f, 0.5f), 5.f, 0.001f, "demote scales priority");
    expectNear(fuse::terrain::demote_residency_priority(10.f, 1.5f), 10.f, 0.001f, "demote scale clamps above one");
    expectNear(fuse::terrain::demote_residency_priority(10.f, -0.5f), 0.f, 0.001f, "demote scale clamps below zero");
}

void testLodResidencyQueueEnqueuePromoteDemote() {
    fuse::terrain::LodResidencyQueue queue;

    fuse::terrain::LodResidencyRequest first{};
    first.chunk_index = 3;
    first.kind = fuse::terrain::LodResidencyRequestKind::Load;
    first.priority = 4.f;
    expectTrue(queue.enqueue(first), "enqueue accepts first request");
    expectEq(queue.pending_enqueue_count(), 1u, "one pending request after enqueue");

    fuse::terrain::LodResidencyRequest promote{};
    promote.chunk_index = 3;
    promote.kind = fuse::terrain::LodResidencyRequestKind::Load;
    promote.priority = 9.f;
    expectTrue(queue.enqueue(promote), "duplicate enqueue promotes priority");
    expectEq(queue.pending_enqueue_count(), 1u, "duplicate enqueue does not grow pending list");

    expectTrue(queue.demote(3, fuse::terrain::LodResidencyRequestKind::Load, 0.5f),
               "demote finds pending request");
    expectTrue(!queue.demote(99, fuse::terrain::LodResidencyRequestKind::Load, 0.5f),
               "demote misses unknown chunk");
}

void testLodResidencyQueueDrainOrdering() {
    withScheduler(2, [] {
        fuse::terrain::LodResidencyQueue queue;

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::terrain::LodResidencyRequest request{};
            request.chunk_index = i;
            request.kind = fuse::terrain::LodResidencyRequestKind::Load;
            request.priority = static_cast<fuse::f32>(i);
            expectTrue(queue.submit(request, [](fuse::u32, fuse::terrain::LodResidencyRequestKind) { return true; }),
                       "priority submit succeeds");
        }

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 3u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::terrain::CompletedLodResidencyRequest> completed;
        expectEq(queue.drain_completed(completed), 3u, "drain returns all completed requests");
        expectTrue(completed[0].priority >= completed[1].priority &&
                       completed[1].priority >= completed[2].priority,
                   "drain orders highest priority first");
        expectEq(completed[0].chunk_index, 2u, "highest-priority completion is first");
    });
}

void testLodResidencyQueueBudgetReject() {
    withScheduler(1, [] {
        fuse::terrain::LodResidencyQueue queue;
        queue.set_max_pending_submits(1);
        std::atomic<bool> gate_open{false};

        fuse::terrain::LodResidencyRequest blocking{};
        blocking.chunk_index = 0;
        blocking.kind = fuse::terrain::LodResidencyRequestKind::Load;
        blocking.priority = 1.f;
        expectTrue(queue.submit(blocking, [&](fuse::u32, fuse::terrain::LodResidencyRequestKind) {
            while (!gate_open.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }), "first submit accepted");

        fuse::terrain::LodResidencyRequest overflow{};
        overflow.chunk_index = 1;
        overflow.kind = fuse::terrain::LodResidencyRequestKind::Load;
        expectTrue(!queue.submit(overflow, [](fuse::u32, fuse::terrain::LodResidencyRequestKind) { return true; }),
                   "second submit rejected while pending cap reached");

        gate_open.store(true);
        for (int attempt = 0; attempt < 100 && queue.completed_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::terrain::CompletedLodResidencyRequest> completed;
        queue.drain_completed(completed);
        expectEq(completed.size(), 1u, "blocked request completes after gate opens");
    });
}

void testLodResidencyQueueFlushBudget() {
    withScheduler(1, [] {
        fuse::terrain::LodResidencyQueue queue;
        queue.set_max_pending_submits(2);

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::terrain::LodResidencyRequest request{};
            request.chunk_index = i;
            request.kind = fuse::terrain::LodResidencyRequestKind::Load;
            request.priority = static_cast<fuse::f32>(i);
            expectTrue(queue.enqueue(request), "enqueue pending request");
        }

        expectEq(queue.flush(2, [](fuse::u32, fuse::terrain::LodResidencyRequestKind) { return true; }), 2u,
               "flush respects budget cap");
        expectEq(queue.pending_enqueue_count(), 1u, "flush leaves lower-priority pending request");

        for (int attempt = 0; attempt < 100 && queue.completed_count() < 2u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::terrain::CompletedLodResidencyRequest> completed;
        queue.drain_completed(completed);
        expectEq(completed.size(), 2u, "flush submits complete");
        expectEq(completed[0].chunk_index, 2u, "flush submits highest priority first");
    });
}

void testLodResidencyQueueStub() {
    withScheduler(1, [] {
        fuse::terrain::LodResidencyQueue queue;
        std::atomic<bool> worker_ran{false};

        fuse::terrain::LodResidencyRequest request{};
        request.chunk_index = 5;
        request.kind = fuse::terrain::LodResidencyRequestKind::Load;
        request.priority = 12.f;
        request.morph_snapshot = fuse::terrain::capture_morph_snapshot(1, 0.5f);

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
        expectEq(completed[0].morph_snapshot.lod, 1u, "completed request carries morph snapshot lod");
        expectNear(completed[0].morph_snapshot.morph_factor, 0.5f, 0.001f,
                   "completed request carries morph snapshot factor");
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
    expectEq(grid.residency_set().size(), grid.resident_chunk_count(), "residency set tracks loaded chunks");

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
    testMorphFactorClamp();
    testAdjacentLodPair();
    testAdjacentLodMorphBlend();
    testCollectEvictionCandidatesOrdering();
    testResidencyHelperStubs();
    testIncomingOutranksResident();
    testLodResidencySetAddRemove();
    testLodClampHelpers();
    testLodSkirtStubs();
    testEmptyTerrainResidency();
    testLodMeshVertexCounts();
    testResidencyMorphSync();
    testResidencyPriorityPromoteDemote();
    testVertexMorphSnapsToGrid();
    testChunkResidencyStateHelpers();
    testLodResidencyQueueEnqueuePromoteDemote();
    testLodResidencyQueueStub();
    testLodResidencyQueueDrainOrdering();
    testLodResidencyQueueBudgetReject();
    testLodResidencyQueueFlushBudget();
    testChunkGridLodTransitions();
    testChunkGridResidentCapEviction();
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
