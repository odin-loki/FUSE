# fuse_world_partition — B7.6 World Partition & Streaming

Large-world streaming scaffolding for Track B7.6. Cells are keyed on an XZ grid; a camera-centered `StreamingVolume` queues load/unload with hysteresis. Load and unload paths are stubbed — production wiring will attach scene instantiation and asset I/O.

## Layout

| Header | Role |
|--------|------|
| `grid_cell.hpp` | `GridCoord`, `WorldCell`, `CellResidencyState`, grid ↔ world helpers |
| `streaming_volume.hpp` | Camera-centered stream-in / stream-out radii |
| `streaming_request_queue.hpp` | JobScheduler-backed async request queue stub |
| `world_partition.hpp` | `WorldPartitionDesc`, `WorldPartition` update + force load/unload |

## Residency states

`Unloaded` → `QueuedLoad` → `Loading` → `Resident` → `QueuedUnload` → `Unloading` → `Unloaded`

Helpers in `grid_cell.hpp`: `is_resident_state`, `is_loading_state`, `is_unloading_state`, `is_transitional_state`, `is_queued_state`.

## Async streaming (JobScheduler)

When `async_loading = true` and `JobScheduler` has worker threads:

1. `update()` drains completed requests on the game thread.
2. `process_queues_()` submits up to `max_async_in_flight` load/unload jobs to `StreamingRequestQueue`.
3. Worker threads run the I/O stub (`StreamingWorkFn`); callbacks and entity wiring run on drain via `execute_load_` / `execute_unload_`.

Synchronous mode (`async_loading = false` or single-threaded scheduler) drains queues immediately on the calling thread.

## Tests

`fuse_world_partition_tests` (`ctest` name `fuse_world_partition_b76`) covers grid math, residency helpers, streaming hysteresis, callback stubs, camera-driven residency, `StreamingRequestQueue`, and async load/unload completion.

## Dependencies

`fuse_core` (JobScheduler), `fuse_ecs` (ECS `EntityID`, `vec3`, spatial `AABB` via `fuse_ecs`).

## Related docs

- [TRACK-B-WORLD-PARTITION.md](../../../docs/unification/TRACK-B-WORLD-PARTITION.md)
