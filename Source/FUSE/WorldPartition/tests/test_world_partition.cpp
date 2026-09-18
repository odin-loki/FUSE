#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/world_partition/grid_cell.hpp>
#include <fuse/world_partition/residency_set.hpp>
#include <fuse/world_partition/streaming_budget.hpp>
#include <fuse/world_partition/streaming_request_queue.hpp>
#include <fuse/world_partition/streaming_volume.hpp>
#include <fuse/world_partition/world_partition.hpp>

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

void testResidencyStateHelpers() {
    using fuse::world_partition::CellResidencyState;
    expectTrue(fuse::world_partition::is_loading_state(CellResidencyState::QueuedLoad),
               "QueuedLoad is loading state");
    expectTrue(fuse::world_partition::is_loading_state(CellResidencyState::Loading),
               "Loading is loading state");
    expectTrue(fuse::world_partition::is_unloading_state(CellResidencyState::QueuedUnload),
               "QueuedUnload is unloading state");
    expectTrue(fuse::world_partition::is_unloading_state(CellResidencyState::Unloading),
               "Unloading is unloading state");
    expectTrue(fuse::world_partition::is_transitional_state(CellResidencyState::Loading),
               "Loading is transitional");
    expectTrue(fuse::world_partition::is_queued_state(CellResidencyState::QueuedLoad),
               "QueuedLoad is queued");
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

    const fuse::f32 near_priority = volume.unload_priority_for({1, 0}, 256.f);
    const fuse::f32 far_priority = volume.unload_priority_for({10, 0}, 256.f);
    expectTrue(far_priority > near_priority, "farther cells receive higher unload priority");
    expectTrue(near_priority == 0.f, "cells inside stream-out radius have zero unload priority");
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
    desc.budget.max_async_in_flight = 1;
    desc.async_loading = true;
    partition.init(desc);

    fuse::ecs::vec3 camera{128.f, 0.f, 128.f, 0.f};
    for (int frame = 0; frame < 32 && partition.loaded_cell_count() == 0u; ++frame) {
        partition.update(camera);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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

void testStreamingRequestQueueStub() {
    withScheduler(1, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        std::atomic<bool> worker_ran{false};

        fuse::world_partition::StreamingRequest request{};
        request.coord = {3, 4};
        request.kind = fuse::world_partition::StreamingRequestKind::Load;
        request.priority = 10.f;

        const bool submitted = queue.submit(request, [&](fuse::world_partition::GridCoord coord,
                                                         fuse::world_partition::StreamingRequestKind kind) {
            worker_ran.store(coord.x == 3 && coord.y == 4 &&
                             kind == fuse::world_partition::StreamingRequestKind::Load);
            return true;
        });
        expectTrue(submitted, "queue submits to JobScheduler");

        for (int attempt = 0; attempt < 100 && queue.completed_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 1u, "drain returns completed request");
        expectTrue(worker_ran.load(), "worker stub ran on scheduler thread");
        expectTrue(completed[0].success, "completed request reports success");
        expectEq(queue.in_flight_count(), 0u, "in-flight count returns to zero");
    });
}

void testStreamingBudgetCaps() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.stream_in_distance = 800.f;
    desc.budget.max_loads_per_tick = 1;
    desc.max_loaded_cells = 16;
    partition.init(desc);

    const fuse::ecs::vec3 camera{128.f, 0.f, 128.f, 0.f};
    partition.update(camera);

    expectEq(partition.loaded_cell_count(), 1u, "load budget limits synchronous loads per tick");
    expectTrue(partition.queued_load_count() >= 1u, "remaining loads stay queued");

    partition.update(camera);
    expectTrue(partition.loaded_cell_count() >= 2u, "next tick processes another load within budget");

    partition.destroy();
}

void testUnloadPriorityOrdering() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.stream_out_distance = 300.f;
    desc.budget.max_unloads_per_tick = 1;
    desc.max_loaded_cells = 8;
    partition.init(desc);

    const fuse::world_partition::GridCoord near_cell{1, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};
    partition.force_load(near_cell);
    partition.force_load(far_cell);
    expectTrue(partition.cell_loaded(near_cell) && partition.cell_loaded(far_cell),
               "both cells resident before eviction");

    const fuse::ecs::vec3 camera{0.f, 0.f, 0.f, 0.f};
    partition.update(camera);

    expectTrue(partition.cell_residency(far_cell) == fuse::world_partition::CellResidencyState::Unloaded,
               "farther cell evicts first within unload budget");
    expectTrue(partition.cell_residency(near_cell) == fuse::world_partition::CellResidencyState::QueuedUnload,
               "nearer cell stays queued until the next unload tick");

    partition.destroy();
}

void testStreamingRequestQueueMultipleSubmits() {
    withScheduler(2, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        std::atomic<fuse::u32> worker_count{0};

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::world_partition::StreamingRequest request{};
            request.coord = {static_cast<fuse::s32>(i), 0};
            request.kind = fuse::world_partition::StreamingRequestKind::Load;
            request.priority = static_cast<fuse::f32>(i);
            expectTrue(queue.submit(request, [&](fuse::world_partition::GridCoord, fuse::world_partition::StreamingRequestKind) {
                worker_count.fetch_add(1u);
                return true;
            }), "batch submit succeeds");
        }

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 3u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 3u, "drain returns all completed requests");
        expectEq(worker_count.load(), 3u, "all worker stubs executed");
        expectEq(queue.in_flight_count(), 0u, "in-flight count returns to zero after drain");
    });
}

void testStreamingRequestQueueInFlightTracking() {
    withScheduler(1, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        std::atomic<bool> gate_open{false};

        fuse::world_partition::StreamingRequest request{};
        request.coord = {9, 9};
        request.kind = fuse::world_partition::StreamingRequestKind::Unload;

        expectTrue(queue.submit(request, [&](fuse::world_partition::GridCoord, fuse::world_partition::StreamingRequestKind) {
            while (!gate_open.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }), "blocking submit tracks in-flight");

        for (int attempt = 0; attempt < 50 && queue.in_flight_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expectEq(queue.in_flight_count(), 1u, "in-flight count rises while worker runs");

        gate_open.store(true);
        for (int attempt = 0; attempt < 100 && queue.completed_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 1u, "blocking job completes after gate opens");
        expectTrue(completed[0].kind == fuse::world_partition::StreamingRequestKind::Unload,
                   "completed request preserves unload kind");
    });
}

void testResidentCellBudgetReject() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.max_loaded_cells = 2;
    desc.default_cell_bytes = 1024u;
    partition.init(desc);

    const fuse::world_partition::GridCoord a{0, 0};
    const fuse::world_partition::GridCoord b{1, 0};
    const fuse::world_partition::GridCoord c{2, 0};

    partition.force_load(a);
    partition.force_load(b);
    expectEq(partition.loaded_cell_count(), 2u, "two cells resident at cap");

    partition.force_load(c);
    expectEq(partition.budget_counters().budget_evictions, 1u, "cell cap triggers budget eviction");
    expectEq(partition.budget_counters().rejected_loads, 0u, "eviction frees slot for forced load");
    expectTrue(partition.cell_loaded(c), "incoming cell becomes resident after eviction");
    expectEq(partition.loaded_cell_count(), 2u, "resident count stays at cap");

    partition.destroy();
}

void testByteBudgetClamp() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.max_loaded_cells = 16;
    desc.default_cell_bytes = 1024u;
    desc.budget.max_resident_bytes = 2048u;
    partition.init(desc);

    const fuse::world_partition::GridCoord a{0, 0};
    const fuse::world_partition::GridCoord b{1, 0};
    const fuse::world_partition::GridCoord c{2, 0};

    partition.force_load(a);
    partition.force_load(b);
    expectEq(partition.resident_byte_count(), 2048u, "byte budget fills at two cells");

    partition.force_load(c);
    expectEq(partition.budget_counters().budget_evictions, 1u, "byte cap triggers budget eviction");
    expectEq(partition.budget_counters().bytes_evicted, 1024u, "byte eviction tracks freed footprint");
    expectEq(partition.rejected_load_count(), 0u, "byte eviction accepts incoming cell");
    expectTrue(partition.cell_loaded(c), "third cell loads after byte-budget eviction");
    expectEq(partition.resident_byte_count(), 2048u, "byte budget remains clamped after swap");

    partition.destroy();
}

void testLruEvictionOrdering() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.max_loaded_cells = 2;
    desc.stream_in_distance = 1.f;
    desc.stream_out_distance = 10000.f;
    desc.eviction_policy = fuse::world_partition::EvictionPolicy::Lru;
    partition.init(desc);

    const fuse::world_partition::GridCoord old_cell{0, 0};
    const fuse::world_partition::GridCoord recent_cell{1, 0};
    const fuse::world_partition::GridCoord incoming{2, 0};

    partition.force_load(old_cell);
    partition.update({0.f, 0.f, 0.f, 0.f});
    partition.force_load(old_cell);
    partition.update({0.f, 0.f, 0.f, 0.f});
    partition.force_load(recent_cell);

    expectTrue(partition.cell_loaded(old_cell) && partition.cell_loaded(recent_cell),
               "both cells resident before LRU eviction");

    partition.force_load(incoming);

    expectTrue(partition.cell_residency(old_cell) == fuse::world_partition::CellResidencyState::Unloaded,
               "oldest touched cell evicts under LRU policy");
    expectTrue(partition.cell_loaded(recent_cell), "recently touched cell remains resident");

    partition.destroy();
}

void testStreamingRequestQueueDrainOrdering() {
    withScheduler(2, [] {
        fuse::world_partition::StreamingRequestQueue queue;

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::world_partition::StreamingRequest request{};
            request.coord = {static_cast<fuse::s32>(i), 0};
            request.kind = fuse::world_partition::StreamingRequestKind::Load;
            request.priority = static_cast<fuse::f32>(i);
            expectTrue(queue.submit(request, [](fuse::world_partition::GridCoord,
                                                fuse::world_partition::StreamingRequestKind) { return true; }),
                       "priority submit succeeds");
        }

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 3u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 3u, "drain returns all completed requests");
        expectTrue(completed[0].priority >= completed[1].priority &&
                       completed[1].priority >= completed[2].priority,
                   "drain orders highest priority first");
        expectTrue(completed[0].coord.x == 2, "highest-priority completion is first");
    });
}

void testStreamingRequestQueuePendingReject() {
    withScheduler(1, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        queue.set_max_pending_submits(1);
        std::atomic<bool> gate_open{false};

        fuse::world_partition::StreamingRequest blocking{};
        blocking.coord = {0, 0};
        blocking.kind = fuse::world_partition::StreamingRequestKind::Load;
        blocking.priority = 1.f;
        expectTrue(queue.submit(blocking, [&](fuse::world_partition::GridCoord,
                                              fuse::world_partition::StreamingRequestKind) {
            while (!gate_open.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }), "first submit accepted");

        fuse::world_partition::StreamingRequest overflow{};
        overflow.coord = {1, 0};
        overflow.kind = fuse::world_partition::StreamingRequestKind::Load;
        expectTrue(!queue.submit(overflow, [](fuse::world_partition::GridCoord,
                                              fuse::world_partition::StreamingRequestKind) { return true; }),
                   "second submit rejected while pending cap reached");

        gate_open.store(true);
        for (int attempt = 0; attempt < 100 && queue.completed_count() == 0u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        queue.drain_completed(completed);
        expectEq(completed.size(), 1u, "blocked request completes after gate opens");
    });
}

void testStreamingRequestQueueEmptyDrain() {
    fuse::world_partition::StreamingRequestQueue queue;
    expectTrue(queue.empty(), "fresh queue is empty");
    expectEq(queue.in_flight_count(), 0u, "fresh queue has zero in-flight");
    expectEq(queue.completed_count(), 0u, "fresh queue has zero completed");

    std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
    expectEq(queue.drain_completed(completed), 0u, "drain on empty queue returns zero");
    expectTrue(completed.empty(), "empty drain leaves output vector empty");
    expectTrue(queue.empty(), "queue remains empty after drain");
}

void testStreamingRequestQueueFifoOrdering() {
    withScheduler(1, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        std::atomic<fuse::u32> worker_count{0};

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::world_partition::StreamingRequest request{};
            request.coord = {static_cast<fuse::s32>(i), 0};
            request.kind = fuse::world_partition::StreamingRequestKind::Load;
            request.priority = 5.f;
            expectTrue(queue.submit(request, [&](fuse::world_partition::GridCoord,
                                                 fuse::world_partition::StreamingRequestKind) {
                worker_count.fetch_add(1u);
                return true;
            }), "equal-priority submit succeeds");
        }

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 3u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 3u, "drain returns all equal-priority requests");
        expectEq(worker_count.load(), 3u, "all equal-priority workers executed");
        expectTrue(completed[0].coord.x == 0 && completed[1].coord.x == 1 && completed[2].coord.x == 2,
                   "equal-priority drain preserves FIFO submit order");
        expectTrue(completed[0].submit_sequence < completed[1].submit_sequence &&
                       completed[1].submit_sequence < completed[2].submit_sequence,
                   "submit sequence increases in FIFO order");
    });
}

void testStreamingBudgetHelperFunctions() {
    expectTrue(fuse::world_partition::byte_budget_unlimited(0u), "zero byte cap is unlimited");
    expectEq(fuse::world_partition::bytes_remaining(2048u, 1024u), 1024u, "bytes remaining subtracts resident");
    expectEq(fuse::world_partition::bytes_remaining(2048u, 3000u), 0u, "bytes remaining clamps at zero");
    expectEq(fuse::world_partition::resident_cell_headroom(4u, 2u), 2u, "cell headroom subtracts resident count");
    expectEq(fuse::world_partition::resident_cell_headroom(2u, 2u), 0u, "cell headroom zero at cap");
    expectTrue(fuse::world_partition::is_at_cell_cap(2u, 2u), "cell cap predicate");
    expectTrue(fuse::world_partition::is_at_byte_cap(2048u, 2048u), "byte cap predicate");
    expectEq(fuse::world_partition::clamp_incoming_bytes(512u, 1024u), 512u, "clamp incoming to remaining");
    expectEq(fuse::world_partition::clamp_incoming_bytes(~0ull, 1024u), 1024u, "unlimited budget does not clamp");
    expectEq(fuse::world_partition::clamp_pending_submits(5u, 2u), 2u, "clamp pending submits to cap");
    expectEq(fuse::world_partition::clamp_pending_submits(5u, 0u), 5u, "zero pending cap is unlimited");
    expectTrue(fuse::world_partition::needs_budget_eviction(2u, 2u, 2048u, 1024u, 1024u),
               "needs eviction when cell or byte cap would be exceeded");
    expectTrue(!fuse::world_partition::needs_budget_eviction(4u, 2u, 0u, 1024u, 1024u),
               "no eviction when headroom remains");
}

void testEffectiveUnloadPriority() {
    expectNear(fuse::world_partition::effective_unload_priority(0.f, 0.f), 0.f, 1e-4f, "zero priorities");
    expectNear(fuse::world_partition::effective_unload_priority(10.f, 3.f), 10.f, 1e-4f, "streaming priority wins");
    expectNear(fuse::world_partition::effective_unload_priority(2.f, 8.f), 8.f, 1e-4f, "stored priority wins");
}

void testRankUnloadPriorityStub() {
    expectNear(fuse::world_partition::rank_unload_priority_stub(0.f, 0.f, 0.f), 0.f, 1e-4f, "all-zero rank");
    expectNear(fuse::world_partition::rank_unload_priority_stub(2.f, 8.f, 5.f), 8.f, 1e-4f, "rank picks max component");
    expectNear(fuse::world_partition::rank_unload_priority_stub(1.f, 3.f, 9.f), 9.f, 1e-4f, "focus distance can dominate");
}

void testRankUnloadPriority() {
    expectNear(fuse::world_partition::rank_unload_priority(0.f, 0.f, 0.f), 0.f, 1e-4f, "rank unload all-zero");
    expectNear(fuse::world_partition::rank_unload_priority(2.f, 8.f, 5.f), 8.f, 1e-4f, "rank unload picks max");
    expectNear(fuse::world_partition::rank_unload_priority_stub(4.f, 1.f, 6.f),
               fuse::world_partition::rank_unload_priority(4.f, 1.f, 6.f), 1e-4f,
               "stub alias matches rank_unload_priority");
}

void testRankBudgetUnloadPriority() {
    expectNear(fuse::world_partition::rank_budget_unload_priority(2.f, 8.f, 5.f, 3.f), 8.f, 1e-4f,
               "rank budget picks unload rank when higher");
    expectNear(fuse::world_partition::rank_budget_unload_priority(2.f, 3.f, 5.f, 9.f), 9.f, 1e-4f,
               "rank budget picks budget score when higher");
    expectNear(fuse::world_partition::rank_budget_unload_priority_stub(2.f, 3.f, 5.f, 9.f),
               fuse::world_partition::rank_budget_unload_priority(2.f, 3.f, 5.f, 9.f), 1e-4f,
               "rank budget stub alias matches rank_budget_unload_priority");
    expectNear(fuse::world_partition::eviction_unload_priority(2.f, 3.f, 900.f, 0.f, 0u, 10u,
                                                              fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               900.f, 1e-4f, "eviction unload priority merges budget focus distance");
    expectNear(fuse::world_partition::eviction_unload_priority(2.f, 3.f, 0.f, 50.f, 2u, 10u,
                                                              fuse::world_partition::EvictionPolicy::Lru),
               8.f, 1e-4f, "eviction unload priority uses LRU budget score");
}

void testCanAttemptBudgetEviction() {
    expectTrue(!fuse::world_partition::can_attempt_budget_eviction(2u, 2u, 2048u, 1024u, 1024u, false),
               "cannot attempt eviction without residency candidate");
    expectTrue(fuse::world_partition::can_attempt_budget_eviction(2u, 2u, 2048u, 1024u, 1024u, true),
               "can attempt eviction when pressure and candidate exist");
    expectTrue(!fuse::world_partition::can_attempt_budget_eviction(4u, 2u, 0u, 1024u, 1024u, true),
               "cannot attempt eviction when headroom remains");
}

void testCanAttemptBudgetEvictionFromSet() {
    fuse::world_partition::ResidencySet residency;
    expectTrue(!fuse::world_partition::can_attempt_budget_eviction_from_set(2u, 2u, 2048u, 1024u, 1024u,
                                                                            residency),
               "empty residency cannot attempt budget eviction at cap");
    expectTrue(!fuse::world_partition::can_attempt_budget_eviction_from_set(4u, 2u, 0u, 1024u, 1024u, residency),
               "empty residency cannot attempt when headroom remains");

    residency.add({5, 0}, 900.f);
    expectTrue(fuse::world_partition::can_attempt_budget_eviction_from_set(2u, 2u, 2048u, 1024u, 1024u, residency),
               "non-empty residency can attempt when pressure and candidate exist");
}

void testEmptyResidencyEvictionSkipGuard() {
    expectTrue(!fuse::world_partition::should_record_eviction_skipped_on_empty_residency(false, false),
               "no skip when budget pressure absent");
    expectTrue(!fuse::world_partition::should_record_eviction_skipped_on_empty_residency(false, true),
               "no skip when budget pressure absent even with candidate");
    expectTrue(fuse::world_partition::should_record_eviction_skipped_on_empty_residency(true, false),
               "skip when pressure exists but residency empty");
    expectTrue(!fuse::world_partition::should_record_eviction_skipped_on_empty_residency(true, true),
               "no skip when pressure and candidate both exist");
}

void testResidencyEvictionCandidateGuards() {
    fuse::world_partition::ResidencySet residency;
    expectTrue(!fuse::world_partition::has_residency_eviction_candidate_guarded(residency),
               "empty residency guarded candidate check is false");

    const fuse::world_partition::GridCoord cell{2, 2};
    residency.add(cell, 100.f);
    expectTrue(fuse::world_partition::has_residency_eviction_candidate_guarded(residency),
               "non-empty residency guarded candidate check is true");
    expectNear(fuse::world_partition::focus_distance_for_guarded(residency, cell), 100.f, 1e-4f,
               "guarded focus distance returns stored value");
    expectNear(fuse::world_partition::focus_distance_for_guarded(residency, {9, 9}), -1.f, 1e-4f,
               "guarded focus distance returns -1 for absent coord");
    expectNear(fuse::world_partition::focus_distance_for_guarded(residency,
                                                                 fuse::world_partition::kInvalidGridCoord),
               -1.f, 1e-4f, "guarded focus distance rejects invalid coord");
}

void testIsBudgetEvictionScoreEligible() {
    expectTrue(!fuse::world_partition::is_budget_eviction_score_eligible(-1.f),
               "negative score is not eligible");
    expectTrue(!fuse::world_partition::is_budget_eviction_score_eligible(0.f), "zero score is not eligible");
    expectTrue(fuse::world_partition::is_budget_eviction_score_eligible(0.01f),
               "positive score is eligible");
}

void testIsValidUnloadRank() {
    expectTrue(!fuse::world_partition::is_valid_unload_rank(0.f), "zero unload rank is invalid");
    expectTrue(!fuse::world_partition::is_valid_unload_rank(-5.f), "negative unload rank is invalid");
    expectTrue(fuse::world_partition::is_valid_unload_rank(0.01f), "positive unload rank is valid");
}

void testEvictionUnloadPriorityGuarded() {
    expectNear(fuse::world_partition::eviction_unload_priority_guarded(0.f, 0.f, 900.f, 0.f, 0u, 10u,
                                                                      fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               900.f, 1e-4f, "guarded unload rank preserves positive budget score");
    expectNear(fuse::world_partition::eviction_unload_priority_guarded(0.f, 0.f, -1.f, 0.f, 0u, 10u,
                                                                      fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               0.f, 1e-4f, "guarded unload rank rejects ineligible budget score");
    expectNear(fuse::world_partition::eviction_unload_priority_guarded(2.f, 3.f, 0.f, 50.f, 2u, 10u,
                                                                      fuse::world_partition::EvictionPolicy::Lru),
               8.f, 1e-4f, "guarded unload rank uses LRU budget score when eligible");
}

void testInvalidGridCoordSentinel() {
    expectTrue(!fuse::world_partition::is_valid_grid_coord(fuse::world_partition::kInvalidGridCoord),
               "invalid sentinel coord is not valid");
    expectTrue(fuse::world_partition::is_valid_grid_coord({0, 0}), "origin coord is valid");
    expectTrue(!fuse::world_partition::is_valid_grid_coord({-1, 0}), "negative x is invalid");
}

void testPickEvictionCandidateGuarded() {
    fuse::world_partition::ResidencySet residency;
    expectTrue(fuse::world_partition::pick_eviction_candidate_guarded(residency) ==
                   fuse::world_partition::kInvalidGridCoord,
               "empty set guarded pick returns invalid coord");

    const fuse::world_partition::GridCoord far_cell{5, 0};
    expectTrue(residency.add(far_cell, 900.f), "add far cell");
    expectTrue(fuse::world_partition::pick_eviction_candidate_guarded(residency) == far_cell,
               "guarded pick returns eviction candidate when set non-empty");
}

void testCollectBudgetEvictionCandidates() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord near_cell{1, 0};
    const fuse::world_partition::GridCoord mid_cell{3, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};
    residency.add(near_cell, 100.f);
    residency.add(mid_cell, 500.f);
    residency.add(far_cell, 900.f);

    const auto candidates = residency.collect_eviction_candidates();
    const auto blocked = fuse::world_partition::collect_budget_eviction_candidates(
        candidates,
        [&](fuse::world_partition::GridCoord coord) {
            return residency.focus_distance_for(coord);
        },
        50.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus);
    expectTrue(blocked.empty(), "weak incoming yields no eligible budget eviction candidates");

    const auto eligible = fuse::world_partition::collect_budget_eviction_candidates(
        candidates,
        [&](fuse::world_partition::GridCoord coord) {
            return residency.focus_distance_for(coord);
        },
        600.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus);
    expectEq(static_cast<fuse::u32>(eligible.size()), 2u, "eligible list skips blocked farthest resident");
    expectTrue(eligible[0] == mid_cell && eligible[1] == near_cell,
               "eligible candidates preserve farthest-first order");
}

void testResidentCapIncomingGuards() {
    expectTrue(!fuse::world_partition::would_exceed_cell_cap(4u, 3u, 1u),
               "incoming fits within cell headroom");
    expectTrue(fuse::world_partition::would_exceed_cell_cap(4u, 4u, 1u),
               "incoming exceeds when at cell cap");
    expectTrue(!fuse::world_partition::would_exceed_cell_cap(4u, 4u, 0u),
               "zero incoming never exceeds cell cap");
    expectTrue(fuse::world_partition::needs_budget_eviction_for_incoming(2u, 2u, 2048u, 1024u, 1024u),
               "needs eviction when incoming would exceed cell or byte cap");
    expectTrue(!fuse::world_partition::needs_budget_eviction_for_incoming(4u, 2u, 0u, 1024u, 1024u),
               "no eviction when headroom remains for incoming");
    expectEq(fuse::world_partition::clamp_eviction_batch(5u, 2u), 2u, "eviction batch clamps to headroom");
    expectEq(fuse::world_partition::clamp_eviction_batch(1u, 4u), 1u, "eviction batch unchanged under headroom");
}

void testIncomingOutranksEviction() {
    expectTrue(!fuse::world_partition::incoming_outranks_eviction(0.f, 100.f),
               "zero incoming priority does not outrank");
    expectTrue(fuse::world_partition::incoming_outranks_eviction(150.f, 100.f),
               "higher incoming priority outranks resident score");
    expectTrue(!fuse::world_partition::incoming_outranks_eviction(50.f, 100.f),
               "lower incoming priority blocked from evicting farther resident");
    expectTrue(fuse::world_partition::can_evict_for_incoming(150.f, 100.f,
                                                             fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               "can evict when incoming outranks under distance policy");
    expectTrue(!fuse::world_partition::can_evict_for_incoming(50.f, 100.f,
                                                              fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               "cannot evict when incoming does not outrank");
    expectTrue(fuse::world_partition::can_evict_for_incoming(0.f, 100.f,
                                                              fuse::world_partition::EvictionPolicy::Lru),
               "LRU policy ignores incoming outrank check");
}

void testPickBudgetEvictionCandidate() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord near_cell{1, 0};
    const fuse::world_partition::GridCoord mid_cell{3, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};
    residency.add(near_cell, 100.f);
    residency.add(mid_cell, 500.f);
    residency.add(far_cell, 900.f);

    const auto candidates = residency.collect_eviction_candidates();
    fuse::f32 score = -1.f;
    const fuse::world_partition::GridCoord blocked =
        fuse::world_partition::pick_budget_eviction_candidate(
            candidates,
            [&](fuse::world_partition::GridCoord coord) {
                return residency.focus_distance_for(coord);
            },
            50.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus, score);
    expectTrue(blocked == fuse::world_partition::kInvalidGridCoord,
               "farthest blocked when incoming too weak");
    expectNear(score, -1.f, 1e-4f, "blocked pick leaves score unset");

    const fuse::world_partition::GridCoord picked =
        fuse::world_partition::pick_budget_eviction_candidate(
            candidates,
            [&](fuse::world_partition::GridCoord coord) {
                return residency.focus_distance_for(coord);
            },
            600.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus, score);
    expectTrue(picked == mid_cell, "skips blocked farthest and picks next eligible candidate");
    expectNear(score, 500.f, 1e-4f, "picked candidate score recorded");
}

void testPickBudgetEvictionCandidateFromSet() {
    fuse::world_partition::ResidencySet residency;
    fuse::f32 score = -1.f;
    const fuse::world_partition::GridCoord empty_pick =
        fuse::world_partition::pick_budget_eviction_candidate_from_set(
            residency,
            [&](fuse::world_partition::GridCoord) { return 0.f; }, 100.f,
            fuse::world_partition::EvictionPolicy::DistanceFromFocus, score);
    expectTrue(empty_pick == fuse::world_partition::kInvalidGridCoord,
               "empty residency guarded pick returns invalid coord");
    expectNear(score, -1.f, 1e-4f, "empty residency leaves score unset");

    const fuse::world_partition::GridCoord far_cell{5, 0};
    residency.add(far_cell, 900.f);
    const fuse::world_partition::GridCoord picked =
        fuse::world_partition::pick_budget_eviction_candidate_from_set(
            residency,
            [&](fuse::world_partition::GridCoord coord) {
                return residency.focus_distance_for(coord);
            },
            950.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus, score);
    expectTrue(picked == far_cell, "guarded set pick returns eligible candidate");
    expectNear(score, 900.f, 1e-4f, "guarded set pick records candidate score");
}

void testHasBudgetEvictionCandidate() {
    fuse::world_partition::ResidencySet residency;
    expectTrue(!fuse::world_partition::has_budget_eviction_candidate(
                   residency,
                   [&](fuse::world_partition::GridCoord) { return 0.f; }, 100.f,
                   fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               "empty residency has no budget eviction candidate");

    const fuse::world_partition::GridCoord near_cell{1, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};
    residency.add(near_cell, 100.f);
    residency.add(far_cell, 900.f);

    expectTrue(!fuse::world_partition::has_budget_eviction_candidate(
                   residency,
                   [&](fuse::world_partition::GridCoord coord) {
                       return residency.focus_distance_for(coord);
                   },
                   50.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               "weak incoming yields no eligible budget candidate");
    expectTrue(fuse::world_partition::has_budget_eviction_candidate(
                   residency,
                   [&](fuse::world_partition::GridCoord coord) {
                       return residency.focus_distance_for(coord);
                   },
                   950.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               "strong incoming finds eligible budget candidate");
}

void testBudgetEvictionScoreStub() {
    expectNear(fuse::world_partition::budget_eviction_score(900.f, 0.f, 0u, 10u,
                                                            fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               900.f, 1e-4f, "budget score prefers focus distance");
    expectNear(fuse::world_partition::budget_eviction_score(-1.f, 50.f, 0u, 10u,
                                                            fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               50.f, 1e-4f, "budget score falls back to unload priority");
    expectNear(fuse::world_partition::budget_eviction_score(100.f, 0.f, 2u, 10u,
                                                            fuse::world_partition::EvictionPolicy::Lru),
               8.f, 1e-4f, "budget score uses LRU age");
    expectNear(fuse::world_partition::budget_eviction_score_stub(900.f, 0.f, 0u, 10u,
                                                                 fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               fuse::world_partition::budget_eviction_score(900.f, 0.f, 0u, 10u,
                                                            fuse::world_partition::EvictionPolicy::DistanceFromFocus),
               1e-4f, "budget score stub alias matches budget_eviction_score");
}

void testCollectEvictionCandidatesOrdering() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord near_cell{0, 0};
    const fuse::world_partition::GridCoord mid_cell{2, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};

    residency.add(near_cell, 100.f);
    residency.add(mid_cell, 500.f);
    residency.add(far_cell, 900.f);

    const auto all = residency.collect_eviction_candidates();
    expectEq(static_cast<fuse::u32>(all.size()), 3u, "collect all candidates");
    expectTrue(all[0] == far_cell && all[1] == mid_cell && all[2] == near_cell,
               "candidates sorted farthest-first");

    const auto top_two = residency.collect_eviction_candidates(2u);
    expectEq(static_cast<fuse::u32>(top_two.size()), 2u, "collect limits candidate count");
    expectTrue(top_two[0] == far_cell && top_two[1] == mid_cell, "limited list keeps eviction order");

    const fuse::world_partition::GridCoord tie_a{3, 0};
    const fuse::world_partition::GridCoord tie_b{1, 0};
    fuse::world_partition::ResidencySet tied;
    tied.add(tie_a, 500.f);
    tied.add(tie_b, 500.f);
    const auto tied_order = tied.collect_eviction_candidates();
    expectTrue(tied_order[0] == tie_a && tied_order[1] == tie_b,
               "equal focus distance tie-break prefers larger grid key");

    residency.clear();
    expectTrue(residency.collect_eviction_candidates().empty(), "empty residency has no candidates");
    expectTrue(!residency.has_eviction_candidate(), "empty residency reports no candidate");
}

void testBudgetEvictionCounters() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.max_loaded_cells = 2;
    desc.default_cell_bytes = 1024u;
    desc.stream_in_distance = 1.f;
    desc.stream_out_distance = 10000.f;
    desc.eviction_policy = fuse::world_partition::EvictionPolicy::Lru;
    partition.init(desc);

    const fuse::world_partition::GridCoord old_cell{0, 0};
    const fuse::world_partition::GridCoord recent_cell{1, 0};
    const fuse::world_partition::GridCoord incoming{2, 0};

    partition.force_load(old_cell);
    partition.update({0.f, 0.f, 0.f, 0.f});
    partition.force_load(recent_cell);
    expectEq(partition.budget_counters().budget_evictions, 0u, "no evictions before cap pressure");

    partition.force_load(incoming);
    expectEq(partition.budget_counters().budget_evictions, 1u, "budget eviction increments on cap pressure");
    expectEq(partition.budget_counters().bytes_evicted, 1024u, "bytes evicted tracks resident footprint");
    expectEq(partition.budget_counters().rejected_loads, 0u, "successful eviction avoids rejection");
    expectEq(partition.loaded_cell_count(), 2u, "resident count stays at cap after eviction load");

    partition.destroy();
}

void testDistancePolicyEvictionBlocked() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.cell_size = 256.f;
    desc.stream_in_distance = 500.f;
    desc.stream_out_distance = 700.f;
    desc.max_loaded_cells = 2;
    desc.eviction_policy = fuse::world_partition::EvictionPolicy::DistanceFromFocus;
    partition.init(desc);

    const fuse::world_partition::GridCoord anchor{10, 0};
    const fuse::world_partition::GridCoord neighbor{11, 0};
    partition.force_load(anchor);
    partition.force_load(neighbor);
    expectEq(partition.loaded_cell_count(), 2u, "two cells resident at cap");

    const fuse::ecs::vec3 camera = fuse::world_partition::grid_to_world_center(anchor, desc.cell_size);
    const fuse::u32 skipped_before = partition.budget_counters().eviction_skipped;
    const fuse::u32 rejected_before = partition.budget_counters().rejected_loads;
    partition.update(camera);

    const fuse::world_partition::GridCoord incoming{9, 0};
    expectTrue(partition.budget_counters().eviction_skipped > skipped_before,
               "weak incoming blocked from evicting nearer residents");
    expectTrue(partition.budget_counters().rejected_loads > rejected_before,
               "blocked eviction rejects incoming load");
    expectTrue(!partition.cell_loaded(incoming), "incoming cell stays unloaded when eviction blocked");
    expectTrue(partition.cell_loaded(anchor) && partition.cell_loaded(neighbor),
               "existing residents remain when eviction blocked");

    partition.destroy();
}

void testEmptyResidencyBudgetReject() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.max_loaded_cells = 0;
    desc.default_cell_bytes = 1024u;
    partition.init(desc);

    expectTrue(partition.residency_set().empty(), "partition starts with empty residency set");
    expectTrue(partition.residency_set().collect_eviction_candidates().empty(),
               "empty residency yields no eviction candidates");

    const fuse::world_partition::GridCoord origin{0, 0};
    partition.force_load(origin);
    expectEq(partition.budget_counters().rejected_loads, 1u, "zero cell cap rejects without eviction candidates");
    expectEq(partition.budget_counters().budget_evictions, 0u, "no eviction attempted on empty residency");
    expectEq(partition.budget_counters().eviction_skipped, 1u, "empty residency increments eviction_skipped");
    expectTrue(partition.cell_residency(origin) == fuse::world_partition::CellResidencyState::Unloaded,
               "rejected load stays unloaded");

    partition.destroy();
}

void testResidencySetFocusDistance() {
    fuse::world_partition::ResidencySet residency;
    expectTrue(residency.empty(), "new residency set is empty");

    const fuse::world_partition::GridCoord near_cell{0, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};
    expectTrue(residency.add(near_cell, 100.f), "add near cell");
    expectTrue(residency.add(far_cell, 900.f), "add far cell");
    expectEq(residency.size(), 2u, "two resident cells tracked");
    expectTrue(residency.contains(near_cell) && residency.contains(far_cell), "contains resident coords");

    expectTrue(residency.pick_eviction_candidate() == far_cell, "farthest focus distance evicts first");
    expectNear(residency.focus_distance_for(far_cell), 900.f, 1e-4f, "focus distance stored for far cell");

    expectTrue(residency.update_focus_distance(near_cell, 50.f), "update near focus distance");
    expectTrue(residency.pick_eviction_candidate() == far_cell, "far cell still evicts first after update");

    expectTrue(residency.remove(near_cell), "remove near cell");
    expectEq(residency.size(), 1u, "size drops after remove");
    expectTrue(!residency.contains(near_cell), "removed cell no longer resident");
    expectTrue(residency.pick_eviction_candidate() == far_cell, "remaining cell is eviction candidate");

    residency.clear();
    expectTrue(residency.empty(), "clear empties residency set");
    expectTrue(residency.pick_eviction_candidate() == fuse::world_partition::kInvalidGridCoord,
               "empty set has no candidate");
}

void testWorldPartitionResidencySetTracking() {
    fuse::world_partition::WorldPartition partition;
    fuse::world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    desc.max_loaded_cells = 4;
    partition.init(desc);

    const fuse::world_partition::GridCoord near_cell{1, 0};
    const fuse::world_partition::GridCoord far_cell{5, 0};
    partition.force_load(near_cell);
    partition.force_load(far_cell);
    expectEq(partition.residency_set().size(), 2u, "residency set tracks loaded cells");
    expectTrue(partition.residency_set().pick_eviction_candidate() == far_cell,
               "partition residency set prefers farther cell for eviction");

    partition.force_unload(far_cell);
    expectEq(partition.residency_set().size(), 1u, "unload removes cell from residency set");
    expectTrue(partition.residency_set().contains(near_cell), "near cell remains in residency set");
    expectTrue(!partition.residency_set().contains(far_cell), "unloaded cell no longer tracked");

    partition.destroy();
}

void testStreamingRequestQueueEnqueueFlushOrdering() {
    withScheduler(2, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        std::atomic<fuse::u32> worker_count{0};

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::world_partition::StreamingRequest request{};
            request.coord = {static_cast<fuse::s32>(i), 0};
            request.kind = fuse::world_partition::StreamingRequestKind::Load;
            request.priority = static_cast<fuse::f32>(i);
            expectTrue(queue.enqueue(request), "enqueue pending request");
        }
        expectEq(queue.pending_enqueue_count(), 3u, "three requests pending before flush");

        const fuse::u32 flushed = queue.flush(3u, [&](fuse::world_partition::GridCoord,
                                                      fuse::world_partition::StreamingRequestKind) {
            worker_count.fetch_add(1u);
            return true;
        });
        expectEq(flushed, 3u, "flush submits all pending requests");
        expectEq(queue.pending_enqueue_count(), 0u, "pending queue empty after flush");

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 3u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 3u, "flush produces ordered completions");
        expectTrue(completed[0].coord.x == 2 && completed[1].coord.x == 1 && completed[2].coord.x == 0,
                   "flush drain orders highest priority first");
        expectEq(worker_count.load(), 3u, "all flushed workers executed");
    });
}

void testStreamingRequestQueueDequeueOrdering() {
    fuse::world_partition::StreamingRequestQueue queue;

    fuse::world_partition::StreamingRequest low{};
    low.coord = {0, 0};
    low.kind = fuse::world_partition::StreamingRequestKind::Load;
    low.priority = 1.f;

    fuse::world_partition::StreamingRequest high_unload{};
    high_unload.coord = {1, 0};
    high_unload.kind = fuse::world_partition::StreamingRequestKind::Unload;
    high_unload.priority = 5.f;

    fuse::world_partition::StreamingRequest mid{};
    mid.coord = {2, 0};
    mid.kind = fuse::world_partition::StreamingRequestKind::Load;
    mid.priority = 3.f;

    expectTrue(queue.enqueue(low), "enqueue low-priority load");
    expectTrue(queue.enqueue(high_unload), "enqueue equal-priority unload");
    expectTrue(queue.enqueue(mid), "enqueue mid-priority load");
    expectEq(queue.pending_enqueue_count(), 3u, "three requests pending before dequeue");

    fuse::world_partition::StreamingRequest first{};
    expectTrue(queue.dequeue(first), "dequeue highest-priority pending request");
    expectTrue(first.kind == fuse::world_partition::StreamingRequestKind::Unload && first.coord.x == 1,
               "dequeue prefers highest-priority unload");

    fuse::world_partition::StreamingRequest second{};
    expectTrue(queue.dequeue(second), "dequeue second pending request");
    expectTrue(second.coord.x == 2 && second.priority == 3.f, "dequeue follows priority order");

    fuse::world_partition::StreamingRequest third{};
    expectTrue(queue.dequeue(third), "dequeue final pending request");
    expectTrue(third.coord.x == 0 && third.priority == 1.f, "dequeue drains lowest-priority last");
    expectEq(queue.pending_enqueue_count(), 0u, "pending queue empty after dequeue drain");
}

void testStreamingRequestQueueDequeueEmptyGuard() {
    fuse::world_partition::StreamingRequestQueue queue;
    expectTrue(queue.empty(), "fresh queue is empty");

    fuse::world_partition::StreamingRequest out{};
    expectTrue(!queue.dequeue(out), "dequeue on empty queue returns false");
    expectEq(queue.pending_enqueue_count(), 0u, "empty dequeue leaves pending count at zero");

    std::vector<fuse::world_partition::StreamingRequest> ordered;
    expectEq(queue.order_by_priority(ordered), 0u, "order_by_priority on empty queue returns zero");
    expectTrue(ordered.empty(), "order_by_priority leaves output empty");
}

void testStreamingRequestQueueOrderByPriority() {
    fuse::world_partition::StreamingRequestQueue queue;

    fuse::world_partition::StreamingRequest first{};
    first.coord = {0, 0};
    first.kind = fuse::world_partition::StreamingRequestKind::Load;
    first.priority = 2.f;

    fuse::world_partition::StreamingRequest second{};
    second.coord = {1, 0};
    second.kind = fuse::world_partition::StreamingRequestKind::Unload;
    second.priority = 2.f;

    fuse::world_partition::StreamingRequest third{};
    third.coord = {2, 0};
    third.kind = fuse::world_partition::StreamingRequestKind::Load;
    third.priority = 9.f;

    expectTrue(queue.enqueue(first), "enqueue first pending request");
    expectTrue(queue.enqueue(second), "enqueue equal-priority unload");
    expectTrue(queue.enqueue(third), "enqueue highest-priority load");

    std::vector<fuse::world_partition::StreamingRequest> ordered;
    expectEq(queue.order_by_priority(ordered), 3u, "order_by_priority returns pending count");
    expectEq(queue.pending_enqueue_count(), 3u, "order_by_priority does not remove pending requests");
    expectTrue(ordered[0].coord.x == 2, "order_by_priority lists highest priority first");
    expectTrue(ordered[1].kind == fuse::world_partition::StreamingRequestKind::Unload,
               "order_by_priority prefers unload before load at equal priority");
    expectTrue(ordered[2].coord.x == 0, "order_by_priority preserves FIFO for lowest-priority load");

    std::vector<fuse::world_partition::StreamingRequest> helper_input;
    std::vector<fuse::u64> sequences;
    helper_input.push_back(first);
    sequences.push_back(1u);
    helper_input.push_back(second);
    sequences.push_back(2u);
    helper_input.push_back(third);
    sequences.push_back(3u);

    std::vector<fuse::world_partition::StreamingRequest> helper_output;
    expectEq(fuse::world_partition::order_by_priority(helper_output, helper_input, sequences), 3u,
             "free helper orders requests with enqueue sequence tie-break");
    expectTrue(helper_output[0].coord.x == 2, "free helper lists highest priority first");
    expectTrue(helper_output[1].kind == fuse::world_partition::StreamingRequestKind::Unload,
               "free helper prefers unload before load at equal priority");
    expectTrue(helper_output[2].coord.x == 0, "free helper preserves FIFO for equal-priority loads");

    fuse::world_partition::StreamingRequest dequeued{};
    expectTrue(queue.dequeue(dequeued), "dequeue after order_by_priority still succeeds");
    expectTrue(dequeued.coord.x == 2, "dequeue still removes highest-priority pending request");
}

void testResidencySetContainsClear() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord a{1, 2};
    const fuse::world_partition::GridCoord b{3, 4};

    expectTrue(!residency.contains(a), "contains returns false before add");
    expectTrue(residency.empty(), "fresh residency set is empty");

    expectTrue(residency.add(a, 100.f), "add first resident cell");
    expectTrue(residency.add(b, 500.f), "add second resident cell");
    expectTrue(residency.contains(a) && residency.contains(b), "contains tracks both residents");
    expectEq(residency.size(), 2u, "size reflects both residents");

    residency.clear();
    expectTrue(residency.empty(), "clear empties residency set");
    expectTrue(!residency.contains(a) && !residency.contains(b), "contains false after clear");
    expectEq(residency.size(), 0u, "size zero after clear");
    expectTrue(residency.collect_eviction_candidates().empty(), "no eviction candidates after clear");
}

void testResidencyGuardedRemoveAndPresence() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord resident{2, 2};
    const fuse::world_partition::GridCoord invalid = fuse::world_partition::kInvalidGridCoord;

    expectTrue(!fuse::world_partition::has_residency_guarded(residency),
               "has_residency_guarded false on empty set");
    expectTrue(!fuse::world_partition::remove_resident_guarded(residency, resident),
               "remove guard false when coord absent");
    expectTrue(!fuse::world_partition::remove_resident_guarded(residency, invalid),
               "remove guard rejects invalid coord");

    expectTrue(residency.add(resident, 100.f), "add resident for guarded remove");
    expectTrue(fuse::world_partition::has_residency_guarded(residency),
               "has_residency_guarded true when set non-empty");
    expectTrue(fuse::world_partition::remove_resident_guarded(residency, resident),
               "remove guard evicts resident coord");
    expectTrue(!fuse::world_partition::has_residency_guarded(residency),
               "has_residency_guarded false after guarded remove");
    expectTrue(!fuse::world_partition::contains_resident_guarded(residency, resident),
               "contains guard false after guarded remove");
}

void testResidencyContainsClearGuards() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord resident{2, 2};
    const fuse::world_partition::GridCoord missing{9, 9};
    const fuse::world_partition::GridCoord invalid = fuse::world_partition::kInvalidGridCoord;

    expectTrue(!fuse::world_partition::contains_resident_guarded(residency, resident),
               "contains guard false on empty set");
    expectTrue(!fuse::world_partition::contains_resident_guarded(residency, invalid),
               "contains guard rejects invalid coord");
    expectTrue(!fuse::world_partition::clear_residency_guarded(residency),
               "clear guard returns false when already empty");

    expectTrue(residency.add(resident, 100.f), "add resident for guard tests");
    expectTrue(fuse::world_partition::contains_resident_guarded(residency, resident),
               "contains guard true for resident coord");
    expectTrue(!fuse::world_partition::contains_resident_guarded(residency, missing),
               "contains guard false for absent coord");
    expectTrue(!fuse::world_partition::contains_resident_guarded(residency, invalid),
               "contains guard false for invalid coord even when set non-empty");

    expectTrue(fuse::world_partition::clear_residency_guarded(residency),
               "clear guard succeeds when set non-empty");
    expectTrue(residency.empty(), "clear guard empties residency set");
    expectTrue(!fuse::world_partition::clear_residency_guarded(residency),
               "clear guard returns false on second clear");
}

void testStreamingRequestQueuePendingEnqueueGuards() {
    fuse::world_partition::StreamingRequestQueue queue;
    expectTrue(!fuse::world_partition::has_pending_enqueue(queue),
               "has_pending_enqueue false on empty queue");
    expectTrue(!queue.has_pending_enqueue(), "member has_pending_enqueue false on empty queue");
    expectNear(fuse::world_partition::peek_highest_pending_priority(queue), -1.f, 1e-4f,
               "peek priority returns -1 on empty queue");

    fuse::world_partition::StreamingRequest request{};
    request.coord = {4, 4};
    request.kind = fuse::world_partition::StreamingRequestKind::Load;
    request.priority = 6.f;
    expectTrue(queue.enqueue(request), "enqueue for pending guard tests");
    expectTrue(fuse::world_partition::has_pending_enqueue(queue),
               "has_pending_enqueue true when pending non-empty");
    expectNear(fuse::world_partition::peek_highest_pending_priority(queue), 6.f, 1e-4f,
               "peek priority returns highest pending priority");

    fuse::world_partition::StreamingRequest peeked{};
    expectTrue(queue.peek_pending(peeked), "peek_pending succeeds when pending non-empty");
    expectTrue(peeked.coord.x == 4 && peeked.priority == 6.f, "peek_pending copies highest-priority request");
    expectEq(queue.pending_enqueue_count(), 1u, "peek_pending leaves pending queue unchanged");
}

void testStreamingRequestQueueDequeueIfGuard() {
    fuse::world_partition::StreamingRequestQueue queue;

    fuse::world_partition::StreamingRequest low{};
    low.coord = {0, 0};
    low.kind = fuse::world_partition::StreamingRequestKind::Load;
    low.priority = 2.f;

    fuse::world_partition::StreamingRequest high{};
    high.coord = {1, 0};
    high.kind = fuse::world_partition::StreamingRequestKind::Unload;
    high.priority = 7.f;

    expectTrue(queue.enqueue(low), "enqueue low-priority load");
    expectTrue(queue.enqueue(high), "enqueue high-priority unload");

    fuse::world_partition::StreamingRequest out{};
    expectTrue(!fuse::world_partition::try_dequeue_pending_if(queue, 10.f, out),
               "dequeue_if false when highest priority below threshold");
    expectEq(queue.pending_enqueue_count(), 2u, "failed dequeue_if leaves pending queue unchanged");

    expectTrue(fuse::world_partition::try_dequeue_pending_if(queue, 7.f, out),
               "dequeue_if succeeds at exact priority threshold");
    expectTrue(out.coord.x == 1 && out.kind == fuse::world_partition::StreamingRequestKind::Unload,
               "dequeue_if removes highest-priority unload");
    expectEq(queue.pending_enqueue_count(), 1u, "dequeue_if shrinks pending queue");

    expectTrue(fuse::world_partition::try_dequeue_pending_if(queue, 1.f, out),
               "dequeue_if drains remaining pending request");
    expectEq(queue.pending_enqueue_count(), 0u, "dequeue_if drains pending queue");
    expectTrue(!fuse::world_partition::has_pending_enqueue(queue),
               "has_pending_enqueue false after drain");
}

void testStreamingRequestQueueDequeueHelpers() {
    fuse::world_partition::StreamingRequestQueue queue;

    fuse::world_partition::StreamingRequest out{};
    expectTrue(!fuse::world_partition::try_dequeue_pending(queue, out),
               "try_dequeue_pending false on empty queue");
    expectTrue(!fuse::world_partition::peek_highest_pending(queue, out),
               "peek_highest_pending false on empty queue");

    fuse::world_partition::StreamingRequest low{};
    low.coord = {0, 0};
    low.kind = fuse::world_partition::StreamingRequestKind::Load;
    low.priority = 1.f;

    fuse::world_partition::StreamingRequest high{};
    high.coord = {1, 0};
    high.kind = fuse::world_partition::StreamingRequestKind::Unload;
    high.priority = 8.f;

    expectTrue(queue.enqueue(low), "enqueue low-priority load");
    expectTrue(queue.enqueue(high), "enqueue high-priority unload");
    expectEq(queue.pending_enqueue_count(), 2u, "two requests pending for helper tests");

    fuse::world_partition::StreamingRequest peeked{};
    expectTrue(fuse::world_partition::peek_highest_pending(queue, peeked),
               "peek_highest_pending succeeds when pending non-empty");
    expectTrue(peeked.kind == fuse::world_partition::StreamingRequestKind::Unload && peeked.coord.x == 1,
               "peek returns highest-priority unload without removing");
    expectEq(queue.pending_enqueue_count(), 2u, "peek leaves pending queue unchanged");

    fuse::world_partition::StreamingRequest dequeued{};
    expectTrue(fuse::world_partition::try_dequeue_pending(queue, dequeued),
               "try_dequeue_pending removes highest-priority request");
    expectTrue(dequeued.coord.x == 1 && dequeued.kind == fuse::world_partition::StreamingRequestKind::Unload,
               "dequeue helper removes highest-priority unload");
    expectEq(queue.pending_enqueue_count(), 1u, "dequeue helper shrinks pending queue");

    expectTrue(fuse::world_partition::peek_highest_pending(queue, peeked), "peek remaining pending request");
    expectTrue(peeked.coord.x == 0 && peeked.priority == 1.f, "peek sees surviving low-priority load");

    expectTrue(fuse::world_partition::try_dequeue_pending(queue, dequeued),
               "try_dequeue_pending drains final pending request");
    expectEq(queue.pending_enqueue_count(), 0u, "dequeue helpers drain pending queue");
    expectTrue(!fuse::world_partition::peek_highest_pending(queue, peeked),
               "peek false after pending queue drained");
}

void testStreamingRequestQueueEnqueuePromoteDemote() {
    fuse::world_partition::StreamingRequestQueue queue;

    fuse::world_partition::StreamingRequest first{};
    first.coord = {4, 4};
    first.kind = fuse::world_partition::StreamingRequestKind::Load;
    first.priority = 2.f;
    expectTrue(queue.enqueue(first), "initial enqueue");

    fuse::world_partition::StreamingRequest promote{};
    promote.coord = {4, 4};
    promote.kind = fuse::world_partition::StreamingRequestKind::Load;
    promote.priority = 9.f;
    expectTrue(queue.enqueue(promote), "duplicate enqueue promotes priority");
    expectEq(queue.pending_enqueue_count(), 1u, "dedupe keeps single pending entry");

    expectTrue(queue.demote({4, 4}, fuse::world_partition::StreamingRequestKind::Load, 0.5f),
               "demote lowers pending priority");
    expectTrue(!queue.demote({9, 9}, fuse::world_partition::StreamingRequestKind::Unload, 0.5f),
               "demote returns false when pending entry missing");

    expectNear(fuse::world_partition::promote_streaming_priority(2.f, 9.f), 9.f, 1e-4f, "promote helper");
    expectNear(fuse::world_partition::demote_streaming_priority(8.f, 0.5f), 4.f, 1e-4f, "demote helper");
}

void testStreamingRequestQueueFlushBudget() {
    withScheduler(1, [] {
        fuse::world_partition::StreamingRequestQueue queue;
        queue.set_max_pending_submits(2);

        for (fuse::u32 i = 0; i < 3u; ++i) {
            fuse::world_partition::StreamingRequest request{};
            request.coord = {static_cast<fuse::s32>(i), 0};
            request.kind = fuse::world_partition::StreamingRequestKind::Load;
            request.priority = static_cast<fuse::f32>(i);
            expectTrue(queue.enqueue(request), "enqueue pending request");
        }

        expectEq(queue.flush(2, [](fuse::world_partition::GridCoord,
                                   fuse::world_partition::StreamingRequestKind) { return true; }),
                 2u, "flush respects budget cap");
        expectEq(queue.pending_enqueue_count(), 1u, "flush leaves lower-priority pending request");
        expectNear(queue.pending_priority_for({0, 0}, fuse::world_partition::StreamingRequestKind::Load), 0.f,
                   1e-4f, "pending lookup returns surviving priority");

        for (int attempt = 0; attempt < 100 && queue.completed_count() < 2u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        queue.drain_completed(completed);
        expectEq(completed.size(), 2u, "flush submits complete");
        expectTrue(completed[0].coord.x == 2, "flush submits highest priority first");
    });
}

void testStreamingRequestQueueEqualPriorityKindOrdering() {
    withScheduler(2, [] {
        fuse::world_partition::StreamingRequestQueue queue;

        fuse::world_partition::StreamingRequest load{};
        load.coord = {0, 0};
        load.kind = fuse::world_partition::StreamingRequestKind::Load;
        load.priority = 5.f;

        fuse::world_partition::StreamingRequest unload{};
        unload.coord = {1, 0};
        unload.kind = fuse::world_partition::StreamingRequestKind::Unload;
        unload.priority = 5.f;

        expectTrue(queue.submit(load, [](fuse::world_partition::GridCoord,
                                         fuse::world_partition::StreamingRequestKind) { return true; }),
                   "submit equal-priority load");
        expectTrue(queue.submit(unload, [](fuse::world_partition::GridCoord,
                                           fuse::world_partition::StreamingRequestKind) { return true; }),
                   "submit equal-priority unload");

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 2u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 2u, "equal-priority drain returns both completions");
        expectTrue(completed[0].kind == fuse::world_partition::StreamingRequestKind::Unload,
                   "equal-priority drain prefers unload before load");
        expectTrue(completed[1].kind == fuse::world_partition::StreamingRequestKind::Load,
                   "equal-priority load follows unload");
    });
}

void testApplyResidencyCompletionStubs() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord coord{2, 3};

    expectTrue(!fuse::world_partition::apply_residency_on_load_complete(residency, coord, 100.f, false),
               "failed load completion does not add residency");
    expectTrue(residency.empty(), "failed load leaves residency empty");

    expectTrue(fuse::world_partition::apply_residency_on_load_complete(residency, coord, 100.f, true),
               "successful load completion adds residency");
    expectTrue(residency.contains(coord), "load stub marks coord resident");

    expectTrue(!fuse::world_partition::apply_residency_on_unload_complete(residency, coord, false),
               "failed unload completion keeps residency");
    expectTrue(residency.contains(coord), "failed unload leaves coord resident");

    expectTrue(fuse::world_partition::apply_residency_on_unload_complete(residency, coord, true),
               "successful unload completion removes residency");
    expectTrue(residency.empty(), "unload stub clears residency set");
}

void testStreamingRequestQueueMixedCompletionOrdering() {
    withScheduler(2, [] {
        fuse::world_partition::StreamingRequestQueue queue;

        fuse::world_partition::StreamingRequest low_load{};
        low_load.coord = {0, 0};
        low_load.kind = fuse::world_partition::StreamingRequestKind::Load;
        low_load.priority = 1.f;

        fuse::world_partition::StreamingRequest high_unload{};
        high_unload.coord = {1, 0};
        high_unload.kind = fuse::world_partition::StreamingRequestKind::Unload;
        high_unload.priority = 10.f;

        fuse::world_partition::StreamingRequest mid_load{};
        mid_load.coord = {2, 0};
        mid_load.kind = fuse::world_partition::StreamingRequestKind::Load;
        mid_load.priority = 5.f;

        expectTrue(queue.submit(low_load, [](fuse::world_partition::GridCoord,
                                             fuse::world_partition::StreamingRequestKind) { return true; }),
                   "submit low-priority load");
        expectTrue(queue.submit(high_unload, [](fuse::world_partition::GridCoord,
                                                fuse::world_partition::StreamingRequestKind) { return true; }),
                   "submit high-priority unload");
        expectTrue(queue.submit(mid_load, [](fuse::world_partition::GridCoord,
                                             fuse::world_partition::StreamingRequestKind) { return true; }),
                   "submit mid-priority load");

        for (int attempt = 0; attempt < 200 && queue.completed_count() < 3u; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::vector<fuse::world_partition::CompletedStreamingRequest> completed;
        expectEq(queue.drain_completed(completed), 3u, "mixed-kind drain returns all completions");
        expectTrue(completed[0].kind == fuse::world_partition::StreamingRequestKind::Unload &&
                       completed[0].priority == 10.f,
                   "highest-priority unload completes first");
        expectTrue(completed[1].coord.x == 2 && completed[2].coord.x == 0,
                   "remaining completions follow priority order");
    });
}

void testResidencySetEmptyStubOperations() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord missing{7, 7};
    const fuse::world_partition::GridCoord invalid = fuse::world_partition::kInvalidGridCoord;

    expectTrue(residency.empty(), "fresh residency set is empty");
    expectTrue(!residency.contains(missing), "missing coord not resident");
    expectTrue(residency.pick_eviction_candidate() == fuse::world_partition::kInvalidGridCoord,
               "empty residency has no eviction candidate");
    expectTrue(residency.collect_eviction_candidates().empty(), "empty residency yields no candidates");
    expectTrue(!fuse::world_partition::try_remove_resident(residency, missing),
               "remove stub fails on empty residency");
    expectTrue(!residency.remove(missing), "remove returns false when coord absent");
    expectTrue(!residency.add(missing, -1.f), "add rejects negative focus distance");
    expectTrue(!fuse::world_partition::try_add_resident(residency, missing, -1.f),
               "add stub rejects negative focus distance");
    expectEq(residency.size(), 0u, "empty residency unchanged after failed ops");

    expectTrue(fuse::world_partition::try_add_resident(residency, missing, 100.f), "add stub succeeds");
    expectTrue(fuse::world_partition::try_remove_resident(residency, missing), "remove stub succeeds");
    expectTrue(residency.empty(), "residency empty after stub remove");
}

void testInvalidCoordResidencyEarlyOuts() {
    fuse::world_partition::ResidencySet residency;
    const fuse::world_partition::GridCoord invalid = fuse::world_partition::kInvalidGridCoord;
    const fuse::world_partition::GridCoord negative{-1, 0};

    expectTrue(!residency.add(invalid, 100.f), "add rejects invalid coord");
    expectTrue(!residency.add(negative, 100.f), "add rejects negative coord");
    expectTrue(!residency.remove(invalid), "remove rejects invalid coord");
    expectTrue(!residency.update_focus_distance(invalid, 50.f), "update rejects invalid coord");
    expectTrue(!fuse::world_partition::try_add_resident(residency, invalid, 100.f),
               "try_add_resident rejects invalid coord");
    expectTrue(!fuse::world_partition::try_remove_resident(residency, invalid),
               "try_remove_resident rejects invalid coord");
    expectTrue(!fuse::world_partition::apply_residency_on_load_complete(residency, invalid, 100.f, true),
               "load completion stub rejects invalid coord");
    expectTrue(!fuse::world_partition::apply_residency_on_unload_complete(residency, invalid, true),
               "unload completion stub rejects invalid coord");
    expectTrue(residency.empty(), "invalid coord ops leave residency empty");

    const auto candidates_with_invalid = std::vector<fuse::world_partition::GridCoord>{
        invalid, {3, 0}, {1, 0}};
    const auto eligible = fuse::world_partition::collect_budget_eviction_candidates(
        candidates_with_invalid,
        [](fuse::world_partition::GridCoord coord) {
            return coord.x == 3 ? 500.f : 100.f;
        },
        450.f, fuse::world_partition::EvictionPolicy::DistanceFromFocus);
    expectEq(static_cast<fuse::u32>(eligible.size()), 1u, "invalid coord skipped in budget candidate collect");
    expectTrue(eligible[0].x == 1, "eligible list ignores invalid coord entries");
}

void testWorldPartitionAsyncEnqueueFlushCarryover() {
    withScheduler(1, [] {
        fuse::world_partition::WorldPartition partition;
        fuse::world_partition::WorldPartitionDesc desc{};
        desc.async_loading = true;
        desc.budget.max_async_in_flight = 1;
        desc.budget.max_loads_per_tick = 2;
        desc.max_loaded_cells = 4;
        partition.init(desc);

        const fuse::world_partition::GridCoord low{0, 0};
        const fuse::world_partition::GridCoord high{1, 0};
        partition.force_load(low);
        partition.force_load(high);

        expectTrue(partition.cell_residency(low) == fuse::world_partition::CellResidencyState::Loading ||
                       partition.cell_residency(high) == fuse::world_partition::CellResidencyState::Loading,
                   "async force_load enqueues work");

        for (int frame = 0; frame < 64 && partition.loaded_cell_count() < 2u; ++frame) {
            partition.drain_completed_requests();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        expectTrue(partition.cell_loaded(low) && partition.cell_loaded(high),
                   "enqueue/flush carryover completes both loads");
        expectEq(partition.in_flight_request_count(), 0u, "no in-flight work after both loads complete");

        partition.destroy();
    });
}

void testWorldPartitionAsyncResidency() {
    withScheduler(1, [] {
        fuse::world_partition::WorldPartition partition;
        fuse::world_partition::WorldPartitionDesc desc{};
        desc.async_loading = true;
        desc.budget.max_async_in_flight = 2;
        desc.max_loaded_cells = 4;
        partition.init(desc);

        fuse::world_partition::CellLoadCallbacks callbacks{};
        callbacks.on_load = on_test_load;
        callbacks.on_unload = on_test_unload;
        partition.set_callbacks(callbacks);

        const fuse::world_partition::GridCoord origin{0, 0};
        partition.force_load(origin);

        expectTrue(partition.cell_residency(origin) == fuse::world_partition::CellResidencyState::Loading ||
                       partition.cell_residency(origin) == fuse::world_partition::CellResidencyState::Resident,
                   "force_load submits async work");

        for (int frame = 0; frame < 64 && !partition.cell_loaded(origin); ++frame) {
            partition.drain_completed_requests();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        expectTrue(partition.cell_loaded(origin), "async load completes after drain");
        expectEq(partition.in_flight_request_count(), 0u, "no in-flight requests after completion");

        partition.force_unload(origin);
        for (int frame = 0; frame < 64 &&
                            partition.cell_residency(origin) !=
                                fuse::world_partition::CellResidencyState::Unloaded;
             ++frame) {
            partition.drain_completed_requests();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        expectTrue(partition.cell_residency(origin) == fuse::world_partition::CellResidencyState::Unloaded,
                   "async unload returns to Unloaded");
        partition.destroy();
    });
}

} // namespace

int main() {
    fuse::core::initialize();
    testGridCoordHelpers();
    testResidencyStateHelpers();
    testStreamingVolumeHysteresis();
    testWorldPartitionLoadUnloadStubs();
    testWorldPartitionStreamingUpdate();
    testStreamingBudgetHelperFunctions();
    testEffectiveUnloadPriority();
    testRankUnloadPriorityStub();
    testRankUnloadPriority();
    testRankBudgetUnloadPriority();
    testCanAttemptBudgetEviction();
    testCanAttemptBudgetEvictionFromSet();
    testEmptyResidencyEvictionSkipGuard();
    testResidencyEvictionCandidateGuards();
    testIsBudgetEvictionScoreEligible();
    testIsValidUnloadRank();
    testEvictionUnloadPriorityGuarded();
    testInvalidGridCoordSentinel();
    testPickEvictionCandidateGuarded();
    testCollectBudgetEvictionCandidates();
    testResidentCapIncomingGuards();
    testIncomingOutranksEviction();
    testPickBudgetEvictionCandidate();
    testPickBudgetEvictionCandidateFromSet();
    testHasBudgetEvictionCandidate();
    testBudgetEvictionScoreStub();
    testCollectEvictionCandidatesOrdering();
    testBudgetEvictionCounters();
    testDistancePolicyEvictionBlocked();
    testEmptyResidencyBudgetReject();
    testStreamingBudgetCaps();
    testResidentCellBudgetReject();
    testByteBudgetClamp();
    testUnloadPriorityOrdering();
    testLruEvictionOrdering();
    testStreamingRequestQueueStub();
    testStreamingRequestQueueMultipleSubmits();
    testStreamingRequestQueueInFlightTracking();
    testStreamingRequestQueueDrainOrdering();
    testStreamingRequestQueuePendingReject();
    testStreamingRequestQueueEmptyDrain();
    testStreamingRequestQueueFifoOrdering();
    testStreamingRequestQueueEnqueueFlushOrdering();
    testStreamingRequestQueueDequeueOrdering();
    testStreamingRequestQueueDequeueEmptyGuard();
    testStreamingRequestQueueOrderByPriority();
    testStreamingRequestQueuePendingEnqueueGuards();
    testStreamingRequestQueueDequeueIfGuard();
    testStreamingRequestQueueDequeueHelpers();
    testResidencySetContainsClear();
    testResidencyGuardedRemoveAndPresence();
    testResidencyContainsClearGuards();
    testStreamingRequestQueueEnqueuePromoteDemote();
    testStreamingRequestQueueFlushBudget();
    testStreamingRequestQueueEqualPriorityKindOrdering();
    testApplyResidencyCompletionStubs();
    testStreamingRequestQueueMixedCompletionOrdering();
    testResidencySetEmptyStubOperations();
    testInvalidCoordResidencyEarlyOuts();
    testResidencySetFocusDistance();
    testWorldPartitionResidencySetTracking();
    testWorldPartitionAsyncEnqueueFlushCarryover();
    testWorldPartitionAsyncResidency();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world_partition_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world_partition_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
