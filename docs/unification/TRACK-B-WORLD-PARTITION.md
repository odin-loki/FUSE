# Track B — B7.6 World Partition & Streaming (deepen)

**Status:** Expanded load/unload residency, unload priority, per-tick budget stubs, and JobScheduler async request queue on `fuse_world_partition`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.6  
**Depends on:** B1.3 JobScheduler (`fuse_core`), B7.6 initial stubs

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| Residency helpers | `grid_cell.hpp` | `is_unloading_state`, `is_transitional_state`, `is_queued_state`, `effective_unload_priority`, `rank_unload_priority_stub` |
| `ResidencySet` | `residency_set.hpp` | Focus-distance resident set: `add`/`remove`, `try_add_resident`/`try_remove_resident` stubs, `has_eviction_candidate`, `pick_eviction_candidate`, `collect_eviction_candidates` (farthest-first with grid-key tie-break) |
| `StreamingBudget` | `streaming_budget.hpp` | Per-tick caps, `max_resident_bytes`, `EvictionPolicy` (distance / LRU), `StreamingBudgetCounters` (`rejected_loads`, `budget_evictions`, `eviction_skipped`, `bytes_evicted`), headroom/clamp helpers, `budget_eviction_score`, `needs_budget_eviction` |
| `StreamingVolume` | `streaming_volume.hpp` | `load_priority_for` (closer first), `unload_priority_for` (farther first) |
| `StreamingRequestQueue` | `streaming_request_queue.hpp/.cpp` | Pending `enqueue`/`flush` with coord+kind dedupe; priority-first drain with FIFO tie-break; worker I/O stub via `JobScheduler::submit` |
| `WorldPartition` deepen | `world_partition.hpp/.cpp` | `ResidencySet` tracking, `StreamingBudgetCounters`, resident-cell + byte budget rejection, priority-aware eviction, budget-aware `process_queues_`, `drain_completed_requests()` |
| Tests | `tests/test_world_partition.cpp` | Budget helper/clamp/counters, `budget_eviction_score` + `rank_unload_priority_stub`, eviction-candidate ordering/tie-break, empty-residency reject/stub ops, distance + LRU eviction, byte/cell cap eviction swap, queue enqueue/flush/FIFO/empty/mixed-kind completion ordering, residency set, batch/in-flight tracking, async residency |

**Not in scope (follow-up PRs):** binary cell asset I/O, scene spawn on load, dirty-cell save, GPU residency.

---

## Residency lifecycle

```
Unloaded ──queue──▶ QueuedLoad ──submit──▶ Loading ──drain──▶ Resident
   ▲                                              │
   │                                              │
   └── drain ◀── Unloading ◀── submit ◀── QueuedUnload
```

Game thread owns `WorldCell` state and callbacks. Worker threads only run the `StreamingWorkFn` stub (disk read simulation in production).

---

## Async request queue

Mirrors `fuse::io::VirtualFileSystem::submitLoadAsync` / `drainCompletedLoads`:

```cpp
#include <fuse/world_partition/world_partition.hpp>

fuse::world_partition::WorldPartition partition;
fuse::world_partition::WorldPartitionDesc desc{};
desc.async_loading = true;
desc.budget.max_loads_per_tick = 2;
desc.budget.max_unloads_per_tick = 1;
desc.budget.max_async_in_flight = 4;
desc.budget.max_resident_bytes = 64u * 1024u * 1024u;
desc.eviction_policy = fuse::world_partition::EvictionPolicy::Lru;
partition.init(desc);

partition.update(camera_pos);              // drains completions, queues new work
partition.drain_completed_requests();      // explicit drain (also called from update)
const fuse::u32 inFlight = partition.in_flight_request_count();
```

`StreamingRequestQueue` can also be exercised directly in tests or tooling:

```cpp
fuse::world_partition::StreamingRequestQueue queue;
queue.enqueue({coord, fuse::world_partition::StreamingRequestKind::Load, priority});
queue.flush(budget, [](fuse::world_partition::GridCoord, fuse::world_partition::StreamingRequestKind) {
    return true; // worker I/O stub
});
queue.submit({coord, fuse::world_partition::StreamingRequestKind::Unload, unload_priority},
             [](fuse::world_partition::GridCoord, fuse::world_partition::StreamingRequestKind) {
                 return true;
             });
```

`enqueue` dedupes by coord+kind and promotes priority via `promote_streaming_priority`. `flush` submits the highest-priority pending batch with FIFO tie-break on equal priority. `drain_completed` returns completions highest-priority-first; equal priorities preserve FIFO submit order via `submit_sequence`. `empty()` is true when no work is pending, in-flight, or buffered for drain. `WorldPartition` propagates unload priority into async submissions so mixed load/unload completions sort correctly.

`ResidencySet` tracks loaded cells by planar focus distance — `WorldPartition` refreshes distances each `update` and uses `pick_eviction_candidate()` under `EvictionPolicy::DistanceFromFocus`:

```cpp
fuse::world_partition::ResidencySet residency;
try_add_resident(residency, coord, focus_distance);
const auto evict = residency.pick_eviction_candidate(); // farthest cell
try_remove_resident(residency, coord);
```

---

## Build

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build --target fuse_world_partition_tests
ctest --test-dir build --output-on-failure -R fuse_world_partition_b76
```

| Option | Effect |
|--------|--------|
| `async_loading = false` | Synchronous queue drain on force_load / update |
| `budget.max_loads_per_tick` | Max load submissions per `update` tick |
| `budget.max_unloads_per_tick` | Max unload submissions per `update` tick |
| `budget.max_async_in_flight` | Cap concurrent JobScheduler submissions per partition |
| `budget.max_resident_bytes` | Reject load enqueue when resident bytes would exceed cap (0 = unlimited) |
| `eviction_policy` | `DistanceFromFocus` (default) or `Lru` (`last_touch_tick`) |
| `rejected_load_count()` / `budget_counters()` | Loads rejected after eviction could not free budget; `budget_evictions`, `eviction_skipped`, and `bytes_evicted` track eviction pressure |
| Single-threaded scheduler | Falls back to synchronous execute path |

---

## Tests

| Check | Validates |
|-------|-----------|
| Residency helpers | `is_*_state` predicates |
| `ResidencySet` | Add/remove, `collect_eviction_candidates` ordering, focus-distance eviction candidate, clear/empty |
| Budget helpers | `resident_cell_headroom`, `clamp_incoming_bytes`, `clamp_pending_submits`, `needs_budget_eviction`, cap predicates |
| Budget counters | Eviction vs rejection vs `eviction_skipped` accounting on cell/byte cap pressure |
| Unload priority | `unload_priority_for`, `rank_unload_priority_stub`, `budget_eviction_score` focus-distance eviction |
| Streaming budget | Per-tick caps; resident-cell + byte budget eviction swap |
| Eviction | Distance-from-focus (`ResidencySet`) and LRU ordering |
| `StreamingRequestQueue` | Enqueue/flush ordering, promote/demote, priority-ordered drain, mixed-kind completion order, FIFO tie-break, empty drain, pending-cap reject, in-flight tracking |
| Empty residency stubs | `try_add_resident`/`try_remove_resident` reject invalid ops; empty set has no eviction candidates |
| Async residency | `Loading` → `Resident` → `Unloading` → `Unloaded` with 1 worker |
| Streaming update | Camera-driven load/unload with async cap |

---

## Gates (B7.6 deepen)

- [x] Expanded residency state helpers
- [x] Unload priority + per-tick / byte / resident-cell streaming budget stubs
- [x] LRU + distance eviction policy stubs
- [x] Load enqueue rejection + priority-aware eviction
- [x] Queue enqueue/flush ordering + drain priority ordering + FIFO tie-break + empty drain
- [x] Mixed load/unload completion ordering (unload priority propagated to async queue)
- [x] Residency add/remove stubs (`try_add_resident` / `try_remove_resident`)
- [x] `ResidencySet` focus-distance add/remove + eviction candidate list
- [x] `StreamingBudgetCounters` + headroom/clamp/`needs_budget_eviction` helpers + `eviction_skipped`
- [x] `budget_eviction_score` focus-distance eviction + `rank_unload_priority_stub`
- [x] Empty-residency budget rejection + `eviction_skipped` tests
- [x] JobScheduler async request queue stub
- [x] Game-thread drain applies callbacks
- [x] Queue batch / in-flight acceptance tests
- [x] CTest target green in Linux umbrella CI
- [ ] Production cell asset I/O + scene wiring — deferred

---

## Related docs

- [Source/FUSE/WorldPartition/README.md](../../Source/FUSE/WorldPartition/README.md)
- [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md) — B7.6 module status
- [TRACK-B-ASSETS.md](./TRACK-B-ASSETS.md) — future cell binary cook path
