# fuse_world_partition — B7.6 World Partition & Streaming

Large-world streaming scaffolding for Track B7.6. Cells are keyed on an XZ grid; a camera-centered `StreamingVolume` queues load/unload with hysteresis. Load and unload paths are stubbed — production wiring will attach scene instantiation and asset I/O.

## Layout

| Header | Role |
|--------|------|
| `grid_cell.hpp` | `GridCoord`, `WorldCell`, `CellResidencyState`, grid ↔ world helpers |
| `residency_set.hpp` | Focus-distance resident set (`add`/`remove`, `try_add_resident`/`try_remove_resident`, `pick_eviction_candidate`, `collect_eviction_candidates`) |
| `streaming_volume.hpp` | Camera-centered stream-in / stream-out radii + load/unload priority |
| `streaming_budget.hpp` | Per-tick caps, byte/resident budgets, `StreamingBudgetCounters`, `EvictionPolicy` (distance / LRU) |
| `streaming_request_queue.hpp` | JobScheduler-backed async request queue (`enqueue`/`flush`, priority drain) |
| `world_partition.hpp` | `WorldPartitionDesc`, `WorldPartition` update + force load/unload |

## Residency states

`Unloaded` → `QueuedLoad` → `Loading` → `Resident` → `QueuedUnload` → `Unloading` → `Unloaded`

Helpers in `grid_cell.hpp`: `is_resident_state`, `is_loading_state`, `is_unloading_state`, `is_transitional_state`, `is_queued_state`.

## Async streaming (JobScheduler)

When `async_loading = true` and `JobScheduler` has worker threads:

1. `update()` drains completed requests on the game thread.
2. `process_queues_()` submits up to `budget.max_loads_per_tick` / `budget.max_unloads_per_tick` jobs per tick, capped by `budget.max_async_in_flight` concurrent in-flight work on `StreamingRequestQueue`.
3. Worker threads run the I/O stub (`StreamingWorkFn`); callbacks and entity wiring run on drain via `execute_load_` / `execute_unload_`.

Load requests sort by `StreamingVolume::load_priority_for` (closer cells first). Unload requests sort by `unload_priority_for` (farther cells evict first) and combine with per-cell priority via `rank_unload_priority` (`rank_unload_priority_stub` alias). `ResidencySet` mirrors resident cells with planar focus distance; `pick_budget_eviction_candidate` walks `collect_eviction_candidates` farthest-first and skips residents protected by `can_evict_for_incoming` / `incoming_outranks_eviction`. When `needs_budget_eviction` reports cell or byte cap pressure, `evict_for_budget_` queues unloads using `budget_eviction_score` (`EvictionPolicy::DistanceFromFocus` or `EvictionPolicy::Lru` via `last_touch_tick`); successful evictions increment `budget_evictions` and `bytes_evicted`, while `eviction_skipped` tracks cap pressure with no evictable resident. Loads that still cannot fit increment `rejected_loads`. `flush_async_queue_` clamps submits via `clamp_pending_submits` and `init` seeds the async queue pending cap from `budget.max_async_in_flight`. `StreamingRequestQueue::drain_completed` returns completions highest-priority-first with FIFO tie-break on equal priority; `empty()` reports no in-flight or buffered completions. Optional `set_max_pending_submits` rejects enqueue when the in-flight + completed buffer is full. Synchronous mode (`async_loading = false` or single-threaded scheduler) drains queues immediately on the calling thread.

## Tests

`fuse_world_partition_tests` (`ctest` name `fuse_world_partition_b76`) covers grid math, residency helpers, budget headroom/clamp/counters, `rank_unload_priority`/`budget_eviction_score`/`pick_budget_eviction_candidate`/`can_evict_for_incoming`, `ResidencySet` eviction-candidate ordering/tie-break, empty-residency stub ops and rejection, streaming hysteresis, unload priority ordering, resident-cell and byte budget eviction swap, LRU eviction, per-tick budget caps, callback stubs, camera-driven residency, `StreamingRequestQueue` enqueue/flush/batch/in-flight/FIFO/empty/mixed-kind completion/priority-drain/pending-cap tracking, and async load/unload completion.

## Dependencies

`fuse_core` (JobScheduler), `fuse_ecs` (ECS `EntityID`, `vec3`, spatial `AABB` via `fuse_ecs`).

## Related docs

- [TRACK-B-WORLD-PARTITION.md](../../../docs/unification/TRACK-B-WORLD-PARTITION.md)
