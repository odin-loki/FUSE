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

void testEvictionCandidateTieBreak() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(!residency.has_eviction_candidate(), "empty set has no eviction candidate");
    expectTrue(residency.add(2u, 100.f), "add lower-index chunk");
    expectTrue(residency.add(8u, 100.f), "add higher-index chunk at same distance");
    expectTrue(residency.has_eviction_candidate(), "non-empty set has eviction candidate");
    expectEq(residency.pick_eviction_candidate(), 8u, "equal focus distance breaks tie by chunk index");

    const auto candidates = residency.collect_eviction_candidates();
    expectEq(candidates[0], 8u, "tie-break prefers higher chunk index first");
    expectEq(candidates[1], 2u, "tie-break orders lower chunk index second");
}

void testResidencyHelperStubs() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(fuse::terrain::try_add_resident(residency, 2u, 50.f), "try_add_resident accepts valid focus");
    expectTrue(!fuse::terrain::try_add_resident(residency, 3u, -1.f), "try_add_resident rejects negative focus");
    expectTrue(!fuse::terrain::try_add_resident(residency, fuse::terrain::kInvalidChunkIndex, 10.f),
               "try_add_resident rejects invalid chunk index");
    expectTrue(residency.contains(2u), "stub add tracks resident chunk");
    expectTrue(fuse::terrain::try_remove_resident(residency, 2u), "try_remove_resident evicts chunk");
    expectTrue(!residency.contains(2u), "stub remove clears resident chunk");
    expectTrue(!fuse::terrain::try_remove_resident(residency, fuse::terrain::kInvalidChunkIndex),
               "try_remove_resident rejects invalid chunk index");

    expectTrue(fuse::terrain::apply_residency_on_load_complete(residency, 4u, 12.f, true),
               "apply on successful load registers resident");
    expectTrue(residency.contains(4u), "load-complete stub tracks resident chunk");
    expectTrue(!fuse::terrain::apply_residency_on_load_complete(residency, 5u, -1.f, true),
               "load-complete stub rejects invalid focus");
    expectTrue(!fuse::terrain::apply_residency_on_load_complete(residency, fuse::terrain::kInvalidChunkIndex, 8.f,
                                                                true),
               "load-complete stub rejects invalid chunk index");
    expectTrue(!fuse::terrain::apply_residency_on_load_complete(residency, 6u, 8.f, false),
               "failed load does not register resident");

    expectTrue(fuse::terrain::apply_residency_on_unload_complete(residency, 4u, true),
               "apply on successful unload clears resident");
    expectTrue(!residency.contains(4u), "unload-complete stub removes resident chunk");
    expectTrue(!fuse::terrain::apply_residency_on_unload_complete(residency, 99u, true),
               "unload-complete stub misses unknown chunk");
    expectTrue(!fuse::terrain::apply_residency_on_unload_complete(residency, fuse::terrain::kInvalidChunkIndex, true),
               "unload-complete stub rejects invalid chunk index");
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

void testIncomingOutranksEviction() {
    expectTrue(!fuse::terrain::incoming_outranks_eviction(0.f, 100.f),
               "zero incoming priority does not outrank eviction score");
    expectTrue(fuse::terrain::incoming_outranks_eviction(150.f, 100.f),
               "higher incoming priority outranks resident score");
    expectTrue(!fuse::terrain::incoming_outranks_eviction(50.f, 100.f),
               "lower incoming priority blocked from evicting farther resident");
    expectTrue(fuse::terrain::can_evict_for_incoming(20.f, 100.f, 24.f,
                                                     fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               "can evict when incoming outranks under distance policy");
    expectTrue(!fuse::terrain::can_evict_for_incoming(5.f, 10.f, 24.f,
                                                      fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               "cannot evict when incoming does not outrank");
    expectTrue(fuse::terrain::can_evict_for_incoming(0.f, 100.f, 24.f,
                                                     fuse::terrain::LodEvictionPolicy::Lru),
               "LRU policy ignores incoming outrank check");
    expectTrue(!fuse::terrain::can_evict_for_incoming(20.f, 0.f, 24.f,
                                                      fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               "zero eviction score blocks distance-policy eviction");
}

void testPickBudgetEvictionCandidate() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(residency.add(1u, 10.f), "add near chunk");
    expectTrue(residency.add(3u, 15.f), "add mid chunk");
    expectTrue(residency.add(5u, 20.f), "add far chunk");

    const auto candidates = residency.collect_eviction_candidates();
    const fuse::f32 load_radius = 24.f;
    fuse::f32 score = -1.f;
    const fuse::u32 blocked = fuse::terrain::pick_budget_eviction_candidate(
        candidates,
        [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 1.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus, score);
    expectEq(blocked, fuse::terrain::kInvalidChunkIndex, "weak incoming blocked from evicting any resident");
    expectNear(score, -1.f, 1e-4f, "blocked pick leaves score unset");

    const fuse::u32 picked = fuse::terrain::pick_budget_eviction_candidate(
        candidates,
        [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 20.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus, score);
    expectEq(picked, 5u, "eligible incoming evicts farthest resident first");
    expectNear(score, 20.f, 1e-4f, "picked candidate score recorded");
}

void testPickBudgetEvictionCandidateFromSet() {
    fuse::terrain::LodResidencySet residency;
    fuse::f32 score = -1.f;
    const fuse::f32 load_radius = 24.f;
    const fuse::u32 empty_pick = fuse::terrain::pick_budget_eviction_candidate_from_set(
        residency,
        [&](fuse::u32) { return 0.f; }, 100.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus, score);
    expectEq(empty_pick, fuse::terrain::kInvalidChunkIndex, "empty residency guarded pick returns invalid index");
    expectNear(score, -1.f, 1e-4f, "empty residency leaves score unset");

    expectTrue(residency.add(5u, 900.f), "add far chunk");
    const fuse::u32 picked = fuse::terrain::pick_budget_eviction_candidate_from_set(
        residency,
        [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 950.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus, score);
    expectEq(picked, 5u, "guarded set pick returns eligible candidate");
    expectNear(score, 900.f, 1e-4f, "guarded set pick records candidate score");
}

void testHasBudgetEvictionCandidate() {
    fuse::terrain::LodResidencySet residency;
    const fuse::f32 load_radius = 24.f;
    expectTrue(!fuse::terrain::has_budget_eviction_candidate(
                   residency,
                   [&](fuse::u32) { return 0.f; }, 100.f, load_radius,
                   fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               "empty residency has no budget eviction candidate");

    expectTrue(residency.add(1u, 10.f), "add near chunk");
    expectTrue(residency.add(5u, 20.f), "add far chunk");

    expectTrue(!fuse::terrain::has_budget_eviction_candidate(
                   residency,
                   [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 1.f,
                   load_radius, fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               "weak incoming yields no eligible budget candidate");
    expectTrue(fuse::terrain::has_budget_eviction_candidate(
                   residency,
                   [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 20.f,
                   load_radius, fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               "strong incoming finds eligible budget candidate");
}

void testCanAttemptBudgetEviction() {
    expectTrue(!fuse::terrain::can_attempt_budget_eviction(2u, 2u, false),
               "cap pressure without eviction candidate cannot attempt eviction");
    expectTrue(fuse::terrain::can_attempt_budget_eviction(2u, 2u, true),
               "cap pressure with eviction candidate can attempt eviction");
    expectTrue(!fuse::terrain::can_attempt_budget_eviction(4u, 2u, true),
               "under cap does not attempt budget eviction");
    expectTrue(!fuse::terrain::can_attempt_budget_eviction(0u, 100u, false),
               "unlimited cap never attempts budget eviction");
}

void testInvalidChunkIndexBudgetCandidateFilter() {
    const std::vector<fuse::u32> candidates = {fuse::terrain::kInvalidChunkIndex, 3u, 1u};
    const fuse::f32 load_radius = 24.f;
    fuse::f32 score = -1.f;
    const fuse::u32 picked = fuse::terrain::pick_budget_eviction_candidate(
        candidates, [](fuse::u32 chunk_index) { return chunk_index == 3u ? 10.f : 100.f; }, 5.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus, score);
    expectEq(picked, 1u, "invalid chunk index skipped in budget candidate pick");
    expectNear(score, 100.f, 1e-4f, "picked score ignores invalid index entries");

    const auto eligible = fuse::terrain::collect_budget_eviction_candidates(
        candidates, [](fuse::u32 chunk_index) { return chunk_index == 3u ? 10.f : 100.f; }, 5.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus);
    expectEq(static_cast<fuse::u32>(eligible.size()), 1u, "invalid chunk index skipped in budget candidate collect");
    expectEq(eligible[0], 1u, "eligible list ignores invalid index entries");
}

void testPickEvictionCandidateGuarded() {
    fuse::terrain::LodResidencySet residency;
    expectEq(fuse::terrain::pick_eviction_candidate_guarded(residency), fuse::terrain::kInvalidChunkIndex,
             "empty set guarded pick returns invalid index");
    expectTrue(residency.add(4u, 200.f), "add resident chunk");
    expectEq(fuse::terrain::pick_eviction_candidate_guarded(residency), 4u,
             "guarded pick returns eviction candidate when set non-empty");
}

void testResidencyGuardedRemoveAndPresence() {
    fuse::terrain::LodResidencySet residency;
    const fuse::u32 resident = 2u;

    expectTrue(!fuse::terrain::has_residency_guarded(residency), "has_residency_guarded false on empty set");
    expectTrue(!fuse::terrain::remove_resident_guarded(residency, resident),
               "remove guard false when chunk absent");
    expectTrue(!fuse::terrain::remove_resident_guarded(residency, fuse::terrain::kInvalidChunkIndex),
               "remove guard rejects invalid chunk index");

    expectTrue(residency.add(resident, 100.f), "add resident for guarded remove");
    expectTrue(fuse::terrain::has_residency_guarded(residency),
               "has_residency_guarded true when set non-empty");
    expectTrue(fuse::terrain::remove_resident_guarded(residency, resident),
               "remove guard evicts resident chunk");
    expectTrue(!fuse::terrain::has_residency_guarded(residency),
               "has_residency_guarded false after guarded remove");
    expectTrue(!fuse::terrain::contains_resident_guarded(residency, resident),
               "contains guard false after guarded remove");
}

void testResidencyContainsClearGuards() {
    fuse::terrain::LodResidencySet residency;
    const fuse::u32 resident = 2u;
    const fuse::u32 missing = 9u;

    expectTrue(!fuse::terrain::contains_resident_guarded(residency, resident),
               "contains guard false on empty set");
    expectTrue(!fuse::terrain::contains_resident_guarded(residency, fuse::terrain::kInvalidChunkIndex),
               "contains guard rejects invalid chunk index");
    expectTrue(!fuse::terrain::clear_residency_guarded(residency),
               "clear guard returns false when already empty");

    expectTrue(residency.add(resident, 100.f), "add resident for guard tests");
    expectTrue(fuse::terrain::contains_resident_guarded(residency, resident),
               "contains guard true for resident chunk");
    expectTrue(!fuse::terrain::contains_resident_guarded(residency, missing),
               "contains guard false for absent chunk");
    expectTrue(!fuse::terrain::contains_resident_guarded(residency, fuse::terrain::kInvalidChunkIndex),
               "contains guard false for invalid chunk even when set non-empty");

    expectTrue(fuse::terrain::clear_residency_guarded(residency), "clear guard succeeds when set non-empty");
    expectTrue(residency.empty(), "clear guard empties residency set");
    expectTrue(!fuse::terrain::clear_residency_guarded(residency),
               "clear guard returns false on second clear");
}

void testCollectBudgetEvictionCandidates() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(residency.collect_eviction_candidates().empty(), "empty residency has no candidates");

    expectTrue(residency.add(1u, 10.f), "add near chunk");
    expectTrue(residency.add(3u, 15.f), "add mid chunk");
    expectTrue(residency.add(5u, 20.f), "add far chunk");

    const auto candidates = residency.collect_eviction_candidates();
    const fuse::f32 load_radius = 24.f;
    const auto blocked = fuse::terrain::collect_budget_eviction_candidates(
        candidates,
        [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 1.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus);
    expectTrue(blocked.empty(), "weak incoming yields no eligible budget eviction candidates");

    const auto eligible = fuse::terrain::collect_budget_eviction_candidates(
        candidates,
        [&](fuse::u32 chunk_index) { return residency.focus_distance_for(chunk_index); }, 20.f, load_radius,
        fuse::terrain::LodEvictionPolicy::DistanceFromFocus);
    expectEq(static_cast<fuse::u32>(eligible.size()), 3u, "strong incoming keeps all residents eligible");
    expectEq(eligible[0], 5u, "eligible list preserves farthest-first order");
    expectEq(eligible[1], 3u, "eligible list preserves mid ordering");
    expectEq(eligible[2], 1u, "eligible list preserves nearest ordering");

    expectTrue(fuse::terrain::collect_budget_eviction_candidates({}, [](fuse::u32) { return 0.f; }, 10.f,
                                                                 load_radius,
                                                                 fuse::terrain::LodEvictionPolicy::DistanceFromFocus)
                   .empty(),
               "empty candidate list early-outs to empty eligible set");
}

void testAsyncInFlightBudgetGuards() {
    expectTrue(fuse::terrain::can_submit_async_load(0u, 0u), "zero async cap allows submit");
    expectTrue(fuse::terrain::can_submit_async_load(3u, 4u), "under async cap allows submit");
    expectTrue(!fuse::terrain::can_submit_async_load(4u, 4u), "at async cap blocks submit");
    expectTrue(fuse::terrain::is_at_async_in_flight_cap(4u, 4u), "at async cap reports cap reached");
    expectTrue(!fuse::terrain::is_at_async_in_flight_cap(3u, 4u), "under async cap is not at cap");
    expectEq(fuse::terrain::async_in_flight_headroom(0u, 100u), ~0u, "zero async cap has unlimited headroom");
    expectEq(fuse::terrain::async_in_flight_headroom(4u, 2u), 2u, "async headroom subtracts in-flight count");
    expectEq(fuse::terrain::async_in_flight_headroom(4u, 6u), 0u, "over-cap async headroom is zero");
}

void testPendingSubmitGuards() {
    expectTrue(!fuse::terrain::would_exceed_pending_submits(0u, 0u, 0u),
               "zero pending cap never exceeds");
    expectTrue(!fuse::terrain::would_exceed_pending_submits(1u, 0u, 2u),
               "under pending cap does not exceed");
    expectTrue(fuse::terrain::would_exceed_pending_submits(1u, 1u, 2u),
               "at pending cap exceeds");
    expectTrue(fuse::terrain::can_submit_residency_request(1u, 0u, 2u),
               "can submit when pending headroom remains");
    expectTrue(!fuse::terrain::can_submit_residency_request(1u, 1u, 2u),
               "cannot submit when pending cap reached");
}

void testRankBudgetUnloadPriority() {
    expectNear(fuse::terrain::rank_budget_unload_priority(2.f, 8.f, 5.f, 3.f), 8.f, 1e-4f,
               "rank budget picks unload rank when higher");
    expectNear(fuse::terrain::rank_budget_unload_priority(2.f, 3.f, 5.f, 9.f), 9.f, 1e-4f,
               "rank budget picks budget score when higher");
    expectNear(fuse::terrain::eviction_unload_priority(2.f, 3.f, 900.f, 0.f, 0u, 10u,
                                                     fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               900.f, 1e-4f, "eviction unload priority merges budget focus distance");
    expectNear(fuse::terrain::eviction_unload_priority(2.f, 3.f, 0.f, 50.f, 2u, 10u,
                                                     fuse::terrain::LodEvictionPolicy::Lru),
               8.f, 1e-4f, "eviction unload priority uses LRU budget score");
}

void testBudgetEvictionScore() {
    expectNear(fuse::terrain::budget_eviction_score(900.f, 0.f, 0u, 10u,
                                                  fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               900.f, 1e-4f, "budget score prefers focus distance");
    expectNear(fuse::terrain::budget_eviction_score(-1.f, 50.f, 0u, 10u,
                                                  fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               50.f, 1e-4f, "budget score falls back to unload priority");
    expectNear(fuse::terrain::budget_eviction_score(100.f, 0.f, 2u, 10u,
                                                  fuse::terrain::LodEvictionPolicy::Lru),
               8.f, 1e-4f, "budget score uses LRU age");
    expectNear(fuse::terrain::eviction_score_for(250.f, 3u, 10u,
                                               fuse::terrain::LodEvictionPolicy::DistanceFromFocus),
               250.f, 1e-4f, "eviction score uses focus distance");
}

void testResidentCapIncomingGuards() {
    expectTrue(!fuse::terrain::would_exceed_resident_cap(0u, 100u, 1u),
               "unlimited cap never exceeds with incoming");
    expectTrue(!fuse::terrain::would_exceed_resident_cap(4u, 2u, 0u),
               "zero incoming never exceeds cap");
    expectTrue(!fuse::terrain::would_exceed_resident_cap(4u, 3u, 1u),
               "incoming fits within headroom");
    expectTrue(fuse::terrain::would_exceed_resident_cap(4u, 4u, 1u),
               "incoming exceeds when at cap");
    expectTrue(fuse::terrain::needs_budget_eviction_for_incoming(2u, 2u, 1u),
               "needs eviction when incoming would exceed cap");

    fuse::terrain::LodResidencyBudget budget{};
    budget.max_loads_per_tick = 3u;
    expectEq(fuse::terrain::clamp_loads_per_tick(8u, budget), 3u, "clamp loads per tick to budget cap");
    expectEq(fuse::terrain::clamp_eviction_batch(5u, 2u), 2u, "eviction batch clamps to headroom");
    expectEq(fuse::terrain::clamp_eviction_batch(1u, 4u), 1u, "eviction batch unchanged under headroom");
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
    expectEq(grid.budget_counters().eviction_skipped, 0u, "no skipped evictions before cap pressure");

    // Move focus toward the far corner so a nearer chunk outranks an existing resident.
    grid.update_lod({30.f, 0.f, 30.f}, 0.016f);
    expectEq(grid.budget_counters().budget_evictions, 1u, "cap pressure evicts farthest resident");
    expectEq(grid.budget_counters().rejected_loads, 0u, "successful eviction avoids rejection");
    expectEq(grid.budget_counters().eviction_skipped, 0u, "successful eviction avoids skip counter");
    expectEq(grid.resident_chunk_count(), 2u, "resident count stays at cap after eviction load");

    grid.destroy();
}

void testChunkGridEmptyResidencyEarlyOut() {
    fuse::terrain::ChunkGrid grid{};
    fuse::terrain::TerrainDesc desc = makeTestDesc();
    desc.async_loading = false;
    desc.max_resident_chunks = 1;
    desc.load_radius = 28.f;
    grid.init(desc);

    expectTrue(grid.residency_set().empty(), "grid starts with empty residency set");
    expectEq(grid.budget_counters().eviction_skipped, 0u, "no eviction skips before cap pressure");

    grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
    expectEq(grid.resident_chunk_count(), 1u, "single resident fills cap");
    expectTrue(!grid.residency_set().empty(), "resident tracked in residency set");

    grid.destroy();
    expectTrue(grid.residency_set().empty(), "destroy clears residency set");
    expectEq(grid.resident_chunk_count(), 0u, "destroy clears resident chunks");
}

void testChunkGridEmptyResidencyBudgetReject() {
    fuse::terrain::LodResidencySet residency;
    expectTrue(residency.empty(), "fresh residency set is empty");
    expectTrue(!fuse::terrain::can_attempt_budget_eviction(1u, 1u, residency.has_eviction_candidate()),
               "empty residency cannot attempt budget eviction at cap");

    fuse::terrain::ChunkGrid grid{};
    fuse::terrain::TerrainDesc desc = makeTestDesc();
    desc.async_loading = false;
    desc.max_resident_chunks = 1;
    desc.load_radius = 28.f;
    grid.init(desc);

    expectTrue(grid.residency_set().empty(), "grid starts with empty residency");
    expectEq(grid.budget_counters().eviction_skipped, 0u, "empty grid has no eviction skips");

    const fuse::u32 skipped_before = grid.budget_counters().eviction_skipped;
    grid.update_lod({desc.world_size * 4.f, 0.f, desc.world_size * 4.f}, 0.016f);
    expectEq(grid.resident_chunk_count(), 0u, "far camera keeps zero residents");
    expectTrue(grid.residency_set().empty(), "far camera keeps residency set empty");
    expectTrue(grid.budget_counters().eviction_skipped == skipped_before,
               "empty residency does not increment eviction_skipped");

    grid.destroy();
}

void testChunkGridEvictionSkippedWhenIncomingDoesNotOutrank() {
    fuse::terrain::ChunkGrid grid{};
    fuse::terrain::TerrainDesc desc = makeTestDesc();
    desc.async_loading = false;
    desc.max_resident_chunks = 1;
    desc.load_radius = 28.f;
    grid.init(desc);

    const fuse::f32 far = desc.world_size * 4.f;
    const fuse::terrain::vec3 focus{24.f, 0.f, 8.f}; // centre of chunk (1,0)

    grid.update_lod(focus, 0.016f);
    expectEq(grid.resident_chunk_count(), 1u, "single resident at cap");

    grid.update_lod({far, 0.f, far}, 0.016f);
    grid.update_lod(focus, 0.016f);
    expectEq(grid.resident_chunk_count(), 1u, "refocus restores single resident");

    // Pending loads were cleared while far; refocus again queues farther chunks as Unloaded.
    grid.update_lod(focus, 0.016f);
    expectTrue(grid.budget_counters().eviction_skipped >= 1u,
               "farther chunks increment skip counter when nearer resident holds cap");
    expectTrue(grid.budget_counters().rejected_loads >= 1u,
               "farther chunks reject load when they cannot outrank resident");
    expectEq(grid.resident_chunk_count(), 1u, "resident count unchanged when eviction skipped");

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
    expectTrue(fuse::terrain::needs_budget_eviction(4u, 4u), "at cap needs budget eviction");
    expectTrue(!fuse::terrain::needs_budget_eviction(4u, 3u), "under cap does not need eviction");
    expectTrue(!fuse::terrain::needs_budget_eviction(0u, 100u), "unlimited cap never needs eviction");
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
    expectNear(queue.pending_priority_for(3, fuse::terrain::LodResidencyRequestKind::Load), 4.5f, 1e-4f,
               "pending priority query returns demoted value");
    expectNear(queue.pending_priority_for(99, fuse::terrain::LodResidencyRequestKind::Load), -1.f, 1e-4f,
               "missing pending priority returns -1");
}

void testLodResidencyQueuePendingEnqueueGuards() {
    fuse::terrain::LodResidencyQueue queue;
    expectTrue(!fuse::terrain::has_pending_enqueue(queue), "has_pending_enqueue false on empty queue");
    expectTrue(!queue.has_pending_enqueue(), "member has_pending_enqueue false on empty queue");
    expectNear(fuse::terrain::peek_highest_pending_priority(queue), -1.f, 1e-4f,
               "peek priority returns -1 on empty queue");
    expectTrue(queue.empty(), "empty queue reports empty");

    fuse::terrain::LodResidencyRequest request{};
    request.chunk_index = 4;
    request.kind = fuse::terrain::LodResidencyRequestKind::Load;
    request.priority = 6.f;
    expectTrue(queue.enqueue(request), "enqueue for pending guard tests");
    expectTrue(fuse::terrain::has_pending_enqueue(queue), "has_pending_enqueue true when pending non-empty");
    expectNear(fuse::terrain::peek_highest_pending_priority(queue), 6.f, 1e-4f,
               "peek priority returns highest pending priority");

    fuse::terrain::LodResidencyRequest peeked{};
    expectTrue(queue.peek_pending(peeked), "peek_pending succeeds when pending non-empty");
    expectTrue(peeked.chunk_index == 4u && peeked.priority == 6.f, "peek_pending copies highest-priority request");
    expectEq(queue.pending_enqueue_count(), 1u, "peek_pending leaves pending queue unchanged");
}

void testLodResidencyQueueDequeueIfGuard() {
    fuse::terrain::LodResidencyQueue queue;

    fuse::terrain::LodResidencyRequest low{};
    low.chunk_index = 0;
    low.kind = fuse::terrain::LodResidencyRequestKind::Load;
    low.priority = 2.f;

    fuse::terrain::LodResidencyRequest high{};
    high.chunk_index = 1;
    high.kind = fuse::terrain::LodResidencyRequestKind::Unload;
    high.priority = 7.f;

    expectTrue(queue.enqueue(low), "enqueue low-priority load");
    expectTrue(queue.enqueue(high), "enqueue high-priority unload");

    fuse::terrain::LodResidencyRequest out{};
    expectTrue(!fuse::terrain::try_dequeue_pending_if(queue, 10.f, out),
               "dequeue_if false when highest priority below threshold");
    expectEq(queue.pending_enqueue_count(), 2u, "failed dequeue_if leaves pending queue unchanged");

    expectTrue(fuse::terrain::try_dequeue_pending_if(queue, 7.f, out),
               "dequeue_if succeeds at exact priority threshold");
    expectTrue(out.chunk_index == 1u && out.kind == fuse::terrain::LodResidencyRequestKind::Unload,
               "dequeue_if removes highest-priority unload");
    expectEq(queue.pending_enqueue_count(), 1u, "dequeue_if shrinks pending queue");

    expectTrue(fuse::terrain::try_dequeue_pending_if(queue, 1.f, out),
               "dequeue_if drains remaining pending request");
    expectEq(queue.pending_enqueue_count(), 0u, "dequeue_if drains pending queue");
    expectTrue(!fuse::terrain::has_pending_enqueue(queue), "has_pending_enqueue false after drain");
}

void testLodResidencyQueueDequeueHelpers() {
    fuse::terrain::LodResidencyQueue queue;

    fuse::terrain::LodResidencyRequest out{};
    expectTrue(!fuse::terrain::try_dequeue_pending(queue, out), "try_dequeue_pending false on empty queue");
    expectTrue(!fuse::terrain::peek_highest_pending(queue, out), "peek_highest_pending false on empty queue");

    fuse::terrain::LodResidencyRequest low{};
    low.chunk_index = 0;
    low.kind = fuse::terrain::LodResidencyRequestKind::Load;
    low.priority = 1.f;

    fuse::terrain::LodResidencyRequest high{};
    high.chunk_index = 1;
    high.kind = fuse::terrain::LodResidencyRequestKind::Unload;
    high.priority = 8.f;

    expectTrue(queue.enqueue(low), "enqueue low-priority load");
    expectTrue(queue.enqueue(high), "enqueue high-priority unload");
    expectEq(queue.pending_enqueue_count(), 2u, "two requests pending for helper tests");

    fuse::terrain::LodResidencyRequest peeked{};
    expectTrue(fuse::terrain::peek_highest_pending(queue, peeked),
               "peek_highest_pending succeeds when pending non-empty");
    expectTrue(peeked.kind == fuse::terrain::LodResidencyRequestKind::Unload && peeked.chunk_index == 1u,
               "peek returns highest-priority unload without removing");
    expectEq(queue.pending_enqueue_count(), 2u, "peek leaves pending queue unchanged");

    fuse::terrain::LodResidencyRequest dequeued{};
    expectTrue(fuse::terrain::try_dequeue_pending(queue, dequeued),
               "try_dequeue_pending removes highest-priority request");
    expectTrue(dequeued.chunk_index == 1u && dequeued.kind == fuse::terrain::LodResidencyRequestKind::Unload,
               "dequeue helper removes highest-priority unload");
    expectEq(queue.pending_enqueue_count(), 1u, "dequeue helper shrinks pending queue");

    expectTrue(fuse::terrain::peek_highest_pending(queue, peeked), "peek remaining pending request");
    expectTrue(peeked.chunk_index == 0u && peeked.priority == 1.f, "peek sees surviving low-priority load");

    expectTrue(fuse::terrain::try_dequeue_pending(queue, dequeued),
               "try_dequeue_pending drains final pending request");
    expectEq(queue.pending_enqueue_count(), 0u, "dequeue helpers drain pending queue");
    expectTrue(!fuse::terrain::peek_highest_pending(queue, peeked), "peek false after pending queue drained");
}

void testLodResidencyQueueSubmitGuard() {
    withScheduler(1, [] {
        fuse::terrain::LodResidencyQueue queue;
        queue.set_max_pending_submits(1);
        expectTrue(fuse::terrain::can_submit_residency_request(queue),
                   "submit guard allows first pending slot");

        fuse::terrain::LodResidencyRequest blocking{};
        blocking.chunk_index = 0;
        blocking.kind = fuse::terrain::LodResidencyRequestKind::Load;
        blocking.priority = 1.f;
        std::atomic<bool> gate_open{false};
        expectTrue(queue.submit(blocking, [&](fuse::u32, fuse::terrain::LodResidencyRequestKind) {
            while (!gate_open.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }), "first submit accepted");
        expectTrue(!fuse::terrain::can_submit_residency_request(queue),
                   "submit guard blocks when pending cap reached");

        gate_open.store(true);
        for (int attempt = 0; attempt < 100 && queue.completed_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::terrain::CompletedLodResidencyRequest> completed;
        queue.drain_completed(completed);
        expectEq(completed.size(), 1u, "blocked request completes after gate opens");
        expectTrue(fuse::terrain::can_submit_residency_request(queue),
                   "submit guard reopens after drain");
    });
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

void testChunkGridAsyncInFlightCapGuard() {
    withScheduler(1, [] {
        fuse::terrain::ChunkGrid grid{};
        fuse::terrain::TerrainDesc desc = makeTestDesc();
        desc.async_loading = true;
        desc.max_async_in_flight = 1;
        desc.load_radius = 28.f;
        grid.init(desc);

        grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
        expectTrue(grid.in_flight_request_count() <= desc.max_async_in_flight,
                   "initial async submit respects in-flight cap");
        expectTrue(!fuse::terrain::can_submit_async_load(desc.max_async_in_flight, desc.max_async_in_flight),
                   "at async cap blocks additional submits");

        for (int frame = 0; frame < 64 && grid.resident_chunk_count() == 0u; ++frame) {
            grid.drain_completed_requests();
            grid.update_lod({0.f, 0.f, 0.f}, 0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        expectTrue(grid.resident_chunk_count() >= 1u, "in-flight cap carryover still completes loads");
        expectEq(grid.in_flight_request_count(), 0u, "in-flight count returns to zero after drain");

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
    testEvictionCandidateTieBreak();
    testResidencyHelperStubs();
    testIncomingOutranksResident();
    testIncomingOutranksEviction();
    testPickBudgetEvictionCandidate();
    testPickBudgetEvictionCandidateFromSet();
    testHasBudgetEvictionCandidate();
    testCanAttemptBudgetEviction();
    testInvalidChunkIndexBudgetCandidateFilter();
    testPickEvictionCandidateGuarded();
    testResidencyGuardedRemoveAndPresence();
    testResidencyContainsClearGuards();
    testCollectBudgetEvictionCandidates();
    testAsyncInFlightBudgetGuards();
    testPendingSubmitGuards();
    testRankBudgetUnloadPriority();
    testBudgetEvictionScore();
    testResidentCapIncomingGuards();
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
    testLodResidencyQueuePendingEnqueueGuards();
    testLodResidencyQueueDequeueIfGuard();
    testLodResidencyQueueDequeueHelpers();
    testLodResidencyQueueSubmitGuard();
    testLodResidencyQueueStub();
    testLodResidencyQueueDrainOrdering();
    testLodResidencyQueueBudgetReject();
    testLodResidencyQueueFlushBudget();
    testChunkGridLodTransitions();
    testChunkGridResidentCapEviction();
    testChunkGridEmptyResidencyEarlyOut();
    testChunkGridEmptyResidencyBudgetReject();
    testChunkGridEvictionSkippedWhenIncomingDoesNotOutrank();
    testChunkGridAsyncResidency();
    testChunkGridAsyncInFlightCapGuard();
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
