# fuse_world_partition — B7.6 World Partition & Streaming

Large-world streaming scaffolding for Track B7.6. Cells are keyed on an XZ grid; a camera-centered `StreamingVolume` queues load/unload with hysteresis. Load and unload paths are stubbed — production wiring will attach scene instantiation and asset I/O.

## Layout

| Header | Role |
|--------|------|
| `grid_cell.hpp` | `GridCoord`, `WorldCell`, `CellResidencyState`, grid ↔ world helpers |
| `residency_set.hpp` | Focus-distance resident set (`add`/`remove`, `pick_eviction_candidate`) |
| `streaming_volume.hpp` | Camera-centered stream-in / stream-out radii + load/unload priority |
| `streaming_budget.hpp` | Per-tick caps, byte/resident budgets, `EvictionPolicy` (distance / LRU) |
| `streaming_request_queue.hpp` | JobScheduler-backed async request queue stub |
| `world_partition.hpp` | `WorldPartitionDesc`, `WorldPartition` update + force load/unload |

## Residency states

`Unloaded` → `QueuedLoad` → `Loading` → `Resident` → `QueuedUnload` → `Unloading` → `Unloaded`

Helpers in `grid_cell.hpp`: `is_resident_state`, `is_loading_state`, `is_unloading_state`, `is_transitional_state`, `is_queued_state`.

## Async streaming (JobScheduler)

When `async_loading = true` and `JobScheduler` has worker threads:

1. `update()` drains completed requests on the game thread.
2. `process_queues_()` submits up to `budget.max_loads_per_tick` / `budget.max_unloads_per_tick` jobs per tick, capped by `budget.max_async_in_flight` concurrent in-flight work on `StreamingRequestQueue`.
3. Worker threads run the I/O stub (`StreamingWorkFn`); callbacks and entity wiring run on drain via `execute_load_` / `execute_unload_`.

Load requests sort by `StreamingVolume::load_priority_for` (closer cells first). Unload requests sort by `unload_priority_for` (farther cells evict first). `ResidencySet` mirrors resident cells with planar focus distance; `pick_eviction_candidate` drives distance-policy eviction. When `max_loaded_cells` or `budget.max_resident_bytes` would be exceeded, `queue_load_` rejects the enqueue (tracked via `rejected_load_count()`) after attempting a priority-aware eviction pass (`EvictionPolicy::DistanceFromFocus` or `EvictionPolicy::Lru` via `last_touch_tick`). `StreamingRequestQueue::drain_completed` returns completions highest-priority-first with FIFO tie-break on equal priority; `empty()` reports no in-flight or buffered completions. Optional `set_max_pending_submits` rejects enqueue when the in-flight + completed buffer is full. Synchronous mode (`async_loading = false` or single-threaded scheduler) drains queues immediately on the calling thread.

## Tests

`fuse_world_partition_tests` (`ctest` name `fuse_world_partition_b76`) covers grid math, residency helpers, `ResidencySet` focus-distance tracking, streaming hysteresis, unload priority ordering, resident-cell and byte budget rejection, LRU eviction, per-tick budget caps, callback stubs, camera-driven residency, `StreamingRequestQueue` batch/in-flight/FIFO/empty/priority-drain/pending-cap tracking, and async load/unload completion.

## Dependencies

`fuse_core` (JobScheduler), `fuse_ecs` (ECS `EntityID`, `vec3`, spatial `AABB` via `fuse_ecs`).

## Related docs

- [TRACK-B-WORLD-PARTITION.md](../../../docs/unification/TRACK-B-WORLD-PARTITION.md)
