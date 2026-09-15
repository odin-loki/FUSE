# Track B — B7.6 World Partition & Streaming (deepen)

**Status:** Expanded load/unload residency, unload priority, per-tick budget stubs, and JobScheduler async request queue on `fuse_world_partition`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.6  
**Depends on:** B1.3 JobScheduler (`fuse_core`), B7.6 initial stubs

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| Residency helpers | `grid_cell.hpp` | `is_unloading_state`, `is_transitional_state`, `is_queued_state`, `unload_priority` |
| `StreamingBudget` | `streaming_budget.hpp` | Per-tick `max_loads_per_tick`, `max_unloads_per_tick`, `max_async_in_flight` caps |
| `StreamingVolume` | `streaming_volume.hpp` | `load_priority_for` (closer first), `unload_priority_for` (farther first) |
| `StreamingRequestQueue` | `streaming_request_queue.hpp/.cpp` | Mutex-backed completion buffer; worker I/O stub via `JobScheduler::submit` |
| `WorldPartition` deepen | `world_partition.hpp/.cpp` | Priority-sorted unload queue, budget-aware `process_queues_`, `drain_completed_requests()` |
| Tests | `tests/test_world_partition.cpp` | Budget caps, unload priority ordering, queue batch/in-flight tracking, async residency |

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
partition.init(desc);

partition.update(camera_pos);              // drains completions, queues new work
partition.drain_completed_requests();      // explicit drain (also called from update)
const fuse::u32 inFlight = partition.in_flight_request_count();
```

`StreamingRequestQueue` can also be exercised directly in tests or tooling:

```cpp
fuse::world_partition::StreamingRequestQueue queue;
queue.submit({coord, fuse::world_partition::StreamingRequestKind::Load, priority},
             [](fuse::world_partition::GridCoord, fuse::world_partition::StreamingRequestKind) {
                 return true; // worker I/O stub
             });
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
| Single-threaded scheduler | Falls back to synchronous execute path |

---

## Tests

| Check | Validates |
|-------|-----------|
| Residency helpers | `is_*_state` predicates |
| Unload priority | `unload_priority_for` ordering; farther cells evict first |
| Streaming budget | `max_loads_per_tick` / `max_unloads_per_tick` cap queue drain |
| `StreamingRequestQueue` | Submit, batch drain, in-flight tracking |
| Async residency | `Loading` → `Resident` → `Unloading` → `Unloaded` with 1 worker |
| Streaming update | Camera-driven load/unload with async cap |

---

## Gates (B7.6 deepen)

- [x] Expanded residency state helpers
- [x] Unload priority + per-tick streaming budget stubs
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
