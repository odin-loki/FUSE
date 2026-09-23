// B7.6 World Partition & Streaming gate tests (docs/plans/FUSE_MASTER_PLAN.md, B7.6 + B7.10
// "World Streaming" rows). Cells are real binary files on disk; entities land in a real ECS
// registry; async loads run on JobScheduler workers and apply on the game thread.

#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/world_partition/cell_file.hpp>
#include <fuse/world_partition/world_partition.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::s64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
namespace wp = fuse::world_partition;
namespace ecs = fuse::ecs;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

#ifdef NDEBUG // timing gates only run in optimised builds
void expectLe(f64 actual, f64 limit, const char* message) {
    if (!(actual <= limit)) {
        std::fprintf(stderr, "FAIL: %s (%.6g > limit %.6g)\n", message, actual, limit);
        ++g_failures;
    }
}
#endif

template <typename Body>
void withScheduler(u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

f64 nowMs() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<f64, std::milli>(clock::now().time_since_epoch()).count();
}

// --- callback probes (CellLoadCallbacks are plain function pointers) -------------------------
std::thread::id g_mainThread;
u32 g_loadCalls = 0;
u32 g_unloadCalls = 0;
u32 g_offThreadCalls = 0;
std::vector<std::tuple<u32, s32, s32, bool>>* g_events = nullptr; // (tick, x, y, load?)
u32 g_tick = 0;

void onLoadProbe(wp::WorldCell& cell) {
    ++g_loadCalls;
    if (std::this_thread::get_id() != g_mainThread) {
        ++g_offThreadCalls;
    }
    if (g_events != nullptr) {
        g_events->emplace_back(g_tick, cell.coord.x, cell.coord.y, true);
    }
}

void onUnloadProbe(wp::WorldCell& cell) {
    ++g_unloadCalls;
    if (std::this_thread::get_id() != g_mainThread) {
        ++g_offThreadCalls;
    }
    if (g_events != nullptr) {
        g_events->emplace_back(g_tick, cell.coord.x, cell.coord.y, false);
    }
}

void resetProbes() {
    g_loadCalls = 0;
    g_unloadCalls = 0;
    g_offThreadCalls = 0;
    g_events = nullptr;
    g_tick = 0;
}

// --- cell fixtures ----------------------------------------------------------------------------
struct CellFixture {
    std::string root;
    f32 cellSize = 128.f;
    std::map<std::pair<s32, s32>, wp::CellFileData> saved;
};

wp::CellFileData makeCell(wp::GridCoord coord, u32 count, f32 cellSize, std::mt19937& rng) {
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    wp::CellFileData data{};
    data.coord = coord;
    for (u32 i = 0; i < count; ++i) {
        wp::CellEntityRecord r{};
        r.stable_id = (static_cast<u64>(static_cast<u32>(coord.x)) << 40) ^
                      (static_cast<u64>(static_cast<u32>(coord.y)) << 20) ^ i;
        r.position[0] = (static_cast<f32>(coord.x) + unit(rng)) * cellSize;
        r.position[1] = unit(rng) * 50.f;
        r.position[2] = (static_cast<f32>(coord.y) + unit(rng)) * cellSize;
        const f32 a = unit(rng) * 3.14159f;
        r.rotation[0] = 0.f;
        r.rotation[1] = std::sin(a);
        r.rotation[2] = 0.f;
        r.rotation[3] = std::cos(a);
        r.scale[0] = r.scale[1] = r.scale[2] = 0.5f + unit(rng);
        data.entities.push_back(r);
    }
    return data;
}

CellFixture writeFixture(const char* name, s32 minX, s32 maxX, s32 minY, s32 maxY, u32 perCell, f32 cellSize) {
    CellFixture fixture{};
    fixture.cellSize = cellSize;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("fuse_b7_streaming_" + std::string(name) + "_" +
                                        std::to_string(static_cast<unsigned long long>(
                                            std::chrono::steady_clock::now().time_since_epoch().count())));
    fixture.root = root.string();
    std::mt19937 rng(static_cast<u32>(minX * 131 + maxX * 17 + perCell));
    for (s32 y = minY; y <= maxY; ++y) {
        for (s32 x = minX; x <= maxX; ++x) {
            wp::CellFileData data = makeCell({x, y}, perCell + static_cast<u32>((x * 7 + y * 3) & 7), cellSize, rng);
            const bool ok =
                wp::write_cell_file(fixture.root + "/" + wp::cell_asset_relative_path({x, y}), data);
            expectTrue(ok, "fixture cell file written");
            fixture.saved[{x, y}] = std::move(data);
        }
    }
    return fixture;
}

void removeFixture(const CellFixture& fixture) {
    std::error_code ec;
    std::filesystem::remove_all(fixture.root, ec);
}

/// Compares a resident cell's live entities with the saved file; returns mismatches.
u32 verifyCellEntities(const wp::WorldPartition& partition, const ecs::Registry& registry, const CellFixture& fixture,
                       wp::GridCoord coord) {
    const wp::WorldCell* cell = partition.find_cell(coord);
    const auto it = fixture.saved.find({coord.x, coord.y});
    const size_t expected = it == fixture.saved.end() ? 0u : it->second.entities.size();
    if (cell == nullptr || cell->entities.size() != expected) {
        return 1u;
    }
    u32 bad = 0;
    for (size_t i = 0; i < expected; ++i) {
        const ecs::EntityID id = cell->entities[i];
        const ecs::Transform* t = registry.get<ecs::Transform>(id);
        const wp::CellEntityRecord& r = it->second.entities[i];
        if (!registry.alive(id) || t == nullptr || t->position.x != r.position[0] || t->position.y != r.position[1] ||
            t->position.z != r.position[2] || t->rotation.y != r.rotation[1] || t->rotation.w != r.rotation[3] ||
            t->scale.x != r.scale[0]) {
            ++bad;
        }
    }
    return bad;
}

u64 residentEntityTotal(const wp::WorldPartition& partition, s32 minX, s32 maxX, s32 minY, s32 maxY) {
    u64 total = 0;
    for (s32 y = minY; y <= maxY; ++y) {
        for (s32 x = minX; x <= maxX; ++x) {
            const wp::WorldCell* cell = partition.find_cell({x, y});
            if (cell != nullptr) {
                total += cell->entities.size();
            }
        }
    }
    return total;
}

template <typename Pred>
void pumpUntil(wp::WorldPartition& partition, fuse::ecs::vec3 camera, Pred done, int maxIters = 5000) {
    for (int i = 0; i < maxIters && !done(); ++i) {
        partition.update(camera);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

bool partitionIdle(const wp::WorldPartition& partition) { return partition.streaming_idle(); }

// ---------------------------------------------------------------------------------------------
// Cell file format: round trip, corruption detection.
// ---------------------------------------------------------------------------------------------
void testCellFileFormat() {
    std::mt19937 rng(1);
    const wp::CellFileData data = makeCell({-3, 7}, 50, 128.f, rng);
    std::vector<u8> bytes;
    wp::encode_cell_file(data, bytes);
    wp::CellFileData decoded{};
    expectTrue(wp::decode_cell_file(bytes.data(), bytes.size(), decoded) == wp::CellFileStatus::Ok,
               "cell file decodes");
    bool identical = decoded.coord == data.coord && decoded.entities.size() == data.entities.size();
    for (size_t i = 0; identical && i < data.entities.size(); ++i) {
        identical = std::memcmp(&decoded.entities[i], &data.entities[i], sizeof(wp::CellEntityRecord)) == 0;
    }
    expectTrue(identical, "cell file round trip is bit-exact (coord, ids, transforms)");

    std::vector<u8> flipped = bytes;
    flipped[40] ^= 0x01u;
    expectTrue(wp::decode_cell_file(flipped.data(), flipped.size(), decoded) == wp::CellFileStatus::Corrupt,
               "single bit flip is rejected by checksum");
    expectTrue(wp::decode_cell_file(bytes.data(), bytes.size() - 1, decoded) == wp::CellFileStatus::Corrupt,
               "truncated file is rejected");
    std::vector<u8> again;
    wp::encode_cell_file(data, again);
    expectTrue(again == bytes, "encoding is deterministic");
}

// ---------------------------------------------------------------------------------------------
// Gate: "Cell loads correctly from disk — entity count and positions match saved state"
// (sync path, async worker path and force_load path) + corrupt cell handling.
// ---------------------------------------------------------------------------------------------
void testCellLoadsFromDisk() {
    const CellFixture fixture = writeFixture("load", 0, 5, 0, 5, 24, 128.f);
    for (int mode = 0; mode < 2; ++mode) {
        const bool async = mode == 1;
        auto body = [&] {
            ecs::Registry registry;
            registry.init(1u << 16);
            wp::WorldPartition partition;
            wp::WorldPartitionDesc desc{};
            desc.cell_size = fixture.cellSize;
            desc.stream_in_distance = 300.f;
            desc.stream_out_distance = 400.f;
            desc.max_loaded_cells = 256;
            desc.budget.max_loads_per_tick = 4;
            desc.budget.max_unloads_per_tick = 4;
            desc.async_loading = async;
            desc.cell_root = fixture.root;
            partition.init(desc);
            partition.set_registry(&registry);

            const fuse::ecs::vec3 camera{384.f, 0.f, 384.f, 0.f};
            partition.update(camera);
            pumpUntil(partition, camera, [&] { return partitionIdle(partition); });

            u32 resident = 0;
            u32 bad = 0;
            for (const auto& entry : fixture.saved) {
                const wp::GridCoord coord{entry.first.first, entry.first.second};
                if (partition.cell_loaded(coord)) {
                    ++resident;
                    bad += verifyCellEntities(partition, registry, fixture, coord);
                }
            }
            expectTrue(resident >= 5u, "cells within stream-in radius loaded from disk");
            expectTrue(bad == 0u, async ? "async: entity count/transforms match saved cell files"
                                        : "sync: entity count/transforms match saved cell files");
            expectTrue(registry.count() == residentEntityTotal(partition, 0, 5, 0, 5),
                       "registry holds exactly the resident cells' entities");
            partition.destroy();
            expectTrue(registry.count() == 0u, "partition destroy releases every streamed entity");
        };
        if (async) {
            withScheduler(4, body);
        } else {
            body();
        }
    }

    // Corrupt cell: load fails, nothing leaks, neighbours unaffected.
    {
        const std::string path = fixture.root + "/" + wp::cell_asset_relative_path({2, 2});
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        if (f != nullptr) {
            std::fseek(f, 30, SEEK_SET);
            std::fputc(0x5A, f);
            std::fclose(f);
        }
        withScheduler(2, [&] {
            ecs::Registry registry;
            registry.init(1u << 16);
            wp::WorldPartition partition;
            wp::WorldPartitionDesc desc{};
            desc.cell_size = fixture.cellSize;
            desc.stream_in_distance = 200.f;
            desc.stream_out_distance = 300.f;
            desc.budget.max_loads_per_tick = 8;
            desc.cell_root = fixture.root;
            partition.init(desc);
            partition.set_registry(&registry);
            const fuse::ecs::vec3 camera{320.f, 0.f, 320.f, 0.f};
            pumpUntil(partition, camera, [&] { return partition.failed_load_count() > 0u && partitionIdle(partition); },
                      400);
            expectTrue(partition.failed_load_count() > 0u, "corrupt cell file reported as failed load");
            const wp::WorldCell* broken = partition.find_cell({2, 2});
            expectTrue(broken == nullptr || broken->entities.empty(), "corrupt cell spawns no entities");
            expectTrue(verifyCellEntities(partition, registry, fixture, {3, 2}) == 0u,
                       "neighbour of corrupt cell still loads correctly");
            partition.destroy();
            expectTrue(registry.count() == 0u, "no leaked entities after corrupt-cell scenario");
        });
    }
    removeFixture(fixture);
}

// ---------------------------------------------------------------------------------------------
// Gate: "force_load completes synchronously and correctly — used for teleport test"
// ---------------------------------------------------------------------------------------------
void testForceLoadTeleport() {
    const CellFixture fixture = writeFixture("teleport", 0, 3, 0, 3, 16, 128.f);
    const CellFixture far = writeFixture("teleport_far", 78, 80, 78, 80, 16, 128.f);
    withScheduler(4, [&] {
        ecs::Registry registry;
        registry.init(1u << 16);
        wp::WorldPartition partition;
        wp::WorldPartitionDesc desc{};
        desc.cell_size = 128.f;
        desc.stream_in_distance = 200.f;
        desc.stream_out_distance = 300.f;
        desc.budget.max_loads_per_tick = 2;
        desc.cell_root = fixture.root;
        partition.init(desc);
        partition.set_registry(&registry);

        // Start async streaming around the origin, then force a cell whose load is still in the pipeline.
        const fuse::ecs::vec3 home{256.f, 0.f, 256.f, 0.f};
        partition.update(home);
        const wp::GridCoord racing{1, 1};
        const bool loaded = partition.force_load(racing);
        expectTrue(loaded && partition.cell_loaded(racing), "force_load returns with the cell Resident (no update)");
        expectTrue(verifyCellEntities(partition, registry, fixture, racing) == 0u,
                   "force_load spawned the saved entities synchronously");
        const size_t countAfterForce = partition.find_cell(racing)->entities.size();
        pumpUntil(partition, home, [&] { return partitionIdle(partition); });
        expectTrue(partition.find_cell(racing)->entities.size() == countAfterForce &&
                       verifyCellEntities(partition, registry, fixture, racing) == 0u,
                   "superseded async completion does not duplicate force-loaded entities");

        // Teleport ~10 km: the destination is available the same frame via force_load.
        partition.destroy();
        wp::WorldPartitionDesc farDesc = desc;
        farDesc.cell_root = far.root;
        partition.init(farDesc);
        partition.set_registry(&registry);
        const wp::GridCoord dest{79, 79};
        const f64 t0 = nowMs();
        const bool ok = partition.force_load(dest);
        const f64 forceMs = nowMs() - t0;
        expectTrue(ok && partition.cell_loaded(dest) && partition.in_flight_request_count() == 0u,
                   "teleport destination resident synchronously with no pending work for it");
        expectTrue(verifyCellEntities(partition, registry, far, dest) == 0u,
                   "teleport destination entities match saved state");
        pumpUntil(partition, {79.5f * 128.f, 0.f, 79.5f * 128.f, 0.f}, [&] { return partitionIdle(partition); });
        expectTrue(partition.cell_loaded(dest), "teleport cell stays resident as streaming resumes there");
        std::printf("  force_load teleport: %.3f ms\n", forceMs);
        partition.destroy();
        expectTrue(registry.count() == 0u, "no entities leaked across teleport");
    });
    removeFixture(fixture);
    removeFixture(far);
}

// ---------------------------------------------------------------------------------------------
// Streaming radius + hysteresis: a cell loads once inside stream-in, survives oscillation inside
// the hysteresis band, and unloads once beyond stream-out.
// ---------------------------------------------------------------------------------------------
void testHysteresis() {
    resetProbes();
    g_mainThread = std::this_thread::get_id();
    wp::WorldPartition partition;
    wp::WorldPartitionDesc desc{};
    desc.cell_size = 128.f;
    desc.stream_in_distance = 512.f;
    desc.stream_out_distance = 600.f;
    desc.max_loaded_cells = 512;
    desc.budget.max_loads_per_tick = 64;
    desc.budget.max_unloads_per_tick = 64;
    desc.async_loading = false;
    partition.init(desc);
    wp::CellLoadCallbacks callbacks{};
    callbacks.on_load = &onLoadProbe;
    callbacks.on_unload = &onUnloadProbe;
    partition.set_callbacks(callbacks);

    const wp::GridCoord probe{10, 0};
    const f32 cx = 10.5f * 128.f;
    const f32 cz = 64.f;
    auto cameraAt = [&](f32 distance) { return fuse::ecs::vec3{cx - distance, 0.f, cz, 0.f}; };

    partition.update(cameraAt(700.f));
    expectTrue(!partition.cell_loaded(probe), "cell beyond stream-in not loaded");
    partition.update(cameraAt(510.f));
    expectTrue(partition.cell_loaded(probe), "cell loads once centre is within stream-in distance");

    u32 unloadsOfProbe = 0;
    for (int i = 0; i < 200; ++i) {
        const f32 d = 520.f + 75.f * (0.5f + 0.5f * std::sin(static_cast<f32>(i) * 0.37f)); // 520..595
        partition.update(cameraAt(d));
        if (!partition.cell_loaded(probe)) {
            ++unloadsOfProbe;
        }
    }
    expectTrue(unloadsOfProbe == 0u, "oscillating inside the hysteresis band never unloads the cell");
    partition.update(cameraAt(610.f));
    expectTrue(!partition.cell_loaded(probe), "cell unloads once beyond stream-out distance");

    // Residency invariant: every resident cell centre <= stream-out, every cell within stream-in resident.
    partition.update(cameraAt(300.f));
    const fuse::ecs::vec3 cam = cameraAt(300.f);
    u32 violations = 0;
    for (s32 y = -8; y <= 8; ++y) {
        for (s32 x = 0; x <= 20; ++x) {
            const fuse::ecs::vec3 c = wp::grid_to_world_center({x, y}, 128.f);
            const f32 d = std::sqrt((c.x - cam.x) * (c.x - cam.x) + (c.z - cam.z) * (c.z - cam.z));
            const bool loaded = partition.cell_loaded({x, y});
            if ((d <= desc.stream_in_distance && !loaded) || (d > desc.stream_out_distance && loaded)) {
                ++violations;
            }
        }
    }
    expectTrue(violations == 0u, "resident set matches stream-in/stream-out radii");
    partition.destroy();
    resetProbes();
}

// ---------------------------------------------------------------------------------------------
// Gates: "Stream-in fires before camera enters cell bounds — no pop-in at 60fps with 512m draw
// distance", "Stream-out correctly destroys all entities in unloaded cell — no leaked entities",
// "World partition streams 16 cells seamlessly as camera traverses 2km at 30m/s", memory budget,
// async loads applied on the game thread, frame budget.
// ---------------------------------------------------------------------------------------------
void testTraverse2kmAt30mps() {
    const f32 cellSize = 128.f;
    const f32 drawDistance = 512.f;
    const s32 minX = -8;
    const s32 maxX = 24;
    const s32 minY = -7;
    const s32 maxY = 7;
    const CellFixture fixture = writeFixture("traverse", minX, maxX, minY, maxY, 32, cellSize);
    resetProbes();
    g_mainThread = std::this_thread::get_id();

    withScheduler(4, [&] {
        ecs::Registry registry;
        registry.init(1u << 18);
        wp::WorldPartition partition;
        wp::WorldPartitionDesc desc{};
        desc.cell_size = cellSize;
        // Load cells before any part of them is inside the draw distance: centre-distance trigger =
        // draw distance + half cell diagonal + lead margin (64 m = ~2 s of travel at 30 m/s).
        desc.stream_in_distance = drawDistance + cellSize * 0.7072f + 64.f;
        desc.stream_out_distance = desc.stream_in_distance + cellSize;
        desc.max_loaded_cells = 160;
        desc.default_cell_bytes = 1u << 20;
        desc.budget.max_resident_bytes = 160ull << 20;
        desc.budget.max_loads_per_tick = 4;
        desc.budget.max_unloads_per_tick = 4;
        desc.budget.max_async_in_flight = 8;
        desc.cell_root = fixture.root;
        partition.init(desc);
        partition.set_registry(&registry);
        wp::CellLoadCallbacks callbacks{};
        callbacks.on_load = &onLoadProbe;
        callbacks.on_unload = &onUnloadProbe;
        partition.set_callbacks(callbacks);

        const f32 z = 64.f; // travel along the centre of row 0
        fuse::ecs::vec3 camera{0.f, 0.f, z, 0.f};
        // Level load: spawn area streamed in before the first frame (as a loading screen would).
        pumpUntil(partition, camera, [&] { return partitionIdle(partition) && partition.loaded_cell_count() > 0u; });
        std::map<std::pair<s32, s32>, s64> firstResident;
        for (s32 y = minY; y <= maxY; ++y) {
            for (s32 x = minX; x <= maxX; ++x) {
                if (partition.cell_loaded({x, y})) {
                    firstResident[{x, y}] = -1; // resident before the first traversal frame
                }
            }
        }

        const f32 dt = 1.f / 60.f;
        const f32 speed = 30.f;
        const u32 frames = static_cast<u32>(std::ceil(2048.f / (speed * dt)));
        u32 popIn = 0;
        u32 overBudget = 0;
        u32 maxResident = 0;
        std::vector<f64> frameMs;
        frameMs.reserve(frames);
        std::map<std::pair<s32, s32>, s64> firstNeeded;
        std::vector<ecs::EntityID> unloadedIds;
        std::map<std::pair<s32, s32>, std::vector<ecs::EntityID>> liveIds;

        for (u32 frame = 0; frame < frames; ++frame) {
            camera.x = speed * dt * static_cast<f32>(frame + 1);
            g_tick = frame;
            const f64 t0 = nowMs();
            partition.update(camera);
            frameMs.push_back(nowMs() - t0);

            maxResident = std::max(maxResident, partition.resident_cell_count());
            if (partition.resident_cell_count() > desc.max_loaded_cells ||
                partition.resident_byte_count() > desc.budget.max_resident_bytes) {
                ++overBudget;
            }

            const wp::GridCoord camCell = wp::world_to_grid(camera, cellSize);
            const s32 ring = static_cast<s32>(std::ceil(drawDistance / cellSize)) + 1;
            for (s32 dy = -ring; dy <= ring; ++dy) {
                for (s32 dx = -ring; dx <= ring; ++dx) {
                    const wp::GridCoord c{camCell.x + dx, camCell.y + dy};
                    // Nearest point of the cell's bounds to the camera.
                    const f32 nx = std::clamp(camera.x, c.x * cellSize, (c.x + 1) * cellSize);
                    const f32 nz = std::clamp(camera.z, c.y * cellSize, (c.y + 1) * cellSize);
                    const f32 d = std::sqrt((nx - camera.x) * (nx - camera.x) + (nz - camera.z) * (nz - camera.z));
                    if (d > drawDistance) {
                        continue;
                    }
                    firstNeeded.emplace(std::make_pair(c.x, c.y), static_cast<s64>(frame));
                    if (!partition.cell_loaded(c)) {
                        ++popIn;
                    }
                }
            }

            // Track entity ids of cells as they come and go.
            for (s32 y = minY; y <= maxY; ++y) {
                for (s32 x = minX; x <= maxX; ++x) {
                    const auto key = std::make_pair(x, y);
                    const bool loaded = partition.cell_loaded({x, y});
                    auto live = liveIds.find(key);
                    if (loaded && live == liveIds.end()) {
                        firstResident.emplace(key, static_cast<s64>(frame));
                        liveIds[key] = partition.find_cell({x, y})->entities;
                    } else if (!loaded && live != liveIds.end() &&
                               partition.cell_residency({x, y}) == wp::CellResidencyState::Unloaded) {
                        unloadedIds.insert(unloadedIds.end(), live->second.begin(), live->second.end());
                        liveIds.erase(live);
                    }
                }
            }

            // Pace at ~8x real time so async jobs see realistic wall-clock latency per frame.
            std::this_thread::sleep_for(std::chrono::microseconds(2000));
        }

        // Lead: how many frames before a cell was first needed was it already resident?
        s64 minLead = std::numeric_limits<s64>::max();
        u32 pathCellsStreamed = 0;
        for (const auto& needed : firstNeeded) {
            const auto resident = firstResident.find(needed.first);
            if (resident != firstResident.end() && resident->second >= 0) { // streamed during traversal
                minLead = std::min<s64>(minLead, needed.second - resident->second);
            }
        }
        for (s32 x = 0; x < 16; ++x) {
            pathCellsStreamed += firstResident.count({x, 0}) != 0u ? 1u : 0u;
        }

        u32 leaked = 0;
        for (const ecs::EntityID id : unloadedIds) {
            leaked += registry.alive(id) ? 1u : 0u;
        }
        u32 wrongEntities = 0;
        for (const auto& live : liveIds) {
            if (partition.cell_loaded({live.first.first, live.first.second})) {
                wrongEntities += verifyCellEntities(partition, registry, fixture, {live.first.first, live.first.second});
            }
        }
        const u64 expectedAlive = residentEntityTotal(partition, minX, maxX, minY, maxY);

        std::sort(frameMs.begin(), frameMs.end());
        const f64 p99 = frameMs[frameMs.size() * 99 / 100];
        const f64 p999 = frameMs[frameMs.size() * 999 / 1000];
        const f64 worst = frameMs.back();

        expectTrue(popIn == 0u, "no cell inside the 512 m draw distance is missing on any frame (no pop-in)");
        expectTrue(pathCellsStreamed == 16u, "all 16 cells along the 2 km path streamed in");
        expectTrue(minLead > 0, "every needed cell was resident before the camera's draw distance reached it");
        expectTrue(g_unloadCalls > 0u && !unloadedIds.empty(), "cells behind the camera streamed out");
        expectTrue(leaked == 0u, "every entity of every unloaded cell was destroyed (no leaks)");
        expectTrue(registry.count() == expectedAlive, "registry population equals resident cells' entities");
        expectTrue(wrongEntities == 0u, "resident cells' entities match their saved files after streaming");
        expectTrue(overBudget == 0u, "resident cells and bytes never exceeded the memory budget");
        expectTrue(g_offThreadCalls == 0u, "load/unload callbacks ran only on the game thread");
        std::printf("  traverse 2 km @ 30 m/s: %u frames, path cells %u, loads %u, unloads %u, min lead %lld frames, "
                    "max resident %u/%u, update p99 %.3f ms, p99.9 %.3f ms, worst %.3f ms\n",
                    frames, pathCellsStreamed, g_loadCalls, g_unloadCalls, static_cast<long long>(minLead),
                    maxResident, desc.max_loaded_cells, p99, p999, worst);
#ifdef NDEBUG
        // Worst single frame is dominated by OS preemption on shared CI hosts, so the hitch gate is the
        // 99.9th percentile against a quarter frame, plus no update ever costing a whole 60 fps frame.
        expectLe(p99, 1.0, "streaming update p99 under 1 ms (NDEBUG)");
        expectLe(p999, 4.0, "streaming update p99.9 under 4 ms: no hitch (NDEBUG)");
        expectLe(worst, 1000.0 / 60.0, "no streaming update costs a whole 60 fps frame (NDEBUG)");
#endif
        partition.destroy();
        expectTrue(registry.count() == 0u, "partition destroy leaves no streamed entities");
    });
    removeFixture(fixture);
    resetProbes();
}

// ---------------------------------------------------------------------------------------------
// Memory budget under async streaming: in-flight loads count against the cap, so completions can
// never push residency past it; the cap produces rejections instead.
// ---------------------------------------------------------------------------------------------
void testAsyncBudgetRespected() {
    withScheduler(4, [] {
        for (int variant = 0; variant < 2; ++variant) {
            wp::WorldPartition partition;
            wp::WorldPartitionDesc desc{};
            desc.cell_size = 64.f;
            desc.stream_in_distance = 400.f; // ~120 cells wanted
            desc.stream_out_distance = 480.f;
            desc.max_loaded_cells = variant == 0 ? 24u : 512u;
            desc.default_cell_bytes = 1000u;
            desc.budget.max_resident_bytes = variant == 0 ? 0u : 30000u;
            desc.budget.max_loads_per_tick = 16;
            desc.budget.max_unloads_per_tick = 16;
            desc.budget.max_async_in_flight = 16;
            partition.init(desc);
            const u32 cellCap = variant == 0 ? 24u : 30u;
            u32 over = 0;
            u32 maxCommitted = 0;
            for (int frame = 0; frame < 400; ++frame) {
                const fuse::ecs::vec3 camera{1000.f + frame * 4.f, 0.f, 1000.f, 0.f};
                partition.update(camera);
                maxCommitted = std::max(maxCommitted, partition.committed_cell_count());
                if (partition.resident_cell_count() > cellCap || partition.committed_cell_count() > cellCap) {
                    ++over;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            std::printf("  async budget variant %d: max committed %u (cap %u), over %u, rejected %u, evictions %u\n",
                        variant, maxCommitted, cellCap, over, partition.rejected_load_count(),
                        partition.budget_counters().budget_evictions);
            expectTrue(over == 0u, variant == 0 ? "async: resident cell cap never exceeded"
                                                : "async: resident byte budget never exceeded");
            expectTrue(partition.rejected_load_count() > 0u || partition.budget_counters().budget_evictions > 0u,
                       "budget pressure was actually exercised");
            expectTrue(maxCommitted == cellCap, "streaming fills the budget up to the cap");
            // Evictions only swap a far resident for a strictly nearer cell, so the set tracks the camera
            // without ping-ponging (at most about one swap per frame at this speed).
            expectTrue(partition.budget_counters().budget_evictions < 400u, "budget eviction does not thrash");
            partition.destroy();
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Determinism: identical camera path -> identical load/unload event sequence (sync mode).
// ---------------------------------------------------------------------------------------------
void testDeterministicStreaming() {
    auto run = [](std::vector<std::tuple<u32, s32, s32, bool>>& events) {
        resetProbes();
        g_mainThread = std::this_thread::get_id();
        g_events = &events;
        wp::WorldPartition partition;
        wp::WorldPartitionDesc desc{};
        desc.cell_size = 128.f;
        desc.stream_in_distance = 400.f;
        desc.stream_out_distance = 500.f;
        desc.max_loaded_cells = 40;
        desc.budget.max_loads_per_tick = 2;
        desc.budget.max_unloads_per_tick = 2;
        desc.async_loading = false;
        partition.init(desc);
        wp::CellLoadCallbacks callbacks{};
        callbacks.on_load = &onLoadProbe;
        callbacks.on_unload = &onUnloadProbe;
        partition.set_callbacks(callbacks);
        for (u32 frame = 0; frame < 900; ++frame) {
            g_tick = frame;
            const f32 t = static_cast<f32>(frame) * 0.01f;
            partition.update({t * 300.f, 0.f, 200.f * std::sin(t), 0.f});
        }
        partition.destroy();
        resetProbes();
    };
    std::vector<std::tuple<u32, s32, s32, bool>> a;
    std::vector<std::tuple<u32, s32, s32, bool>> b;
    run(a);
    run(b);
    expectTrue(!a.empty() && a == b, "streaming event sequence is deterministic for identical inputs");
}

} // namespace

int main() {
    fuse::core::initialize();
    std::printf("fuse_b7_streaming_gates\n");
    testCellFileFormat();
    testCellLoadsFromDisk();
    testForceLoadTeleport();
    testHysteresis();
    testTraverse2kmAt30mps();
    testAsyncBudgetRespected();
    testDeterministicStreaming();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b7_streaming_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b7_streaming_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
