# fuse_world_partition — B7.6 World Partition & Streaming (stub)

Large-world streaming scaffolding for Track B7.6. Cells are keyed on an XZ grid; a camera-centered `StreamingVolume` queues load/unload with hysteresis. Load and unload paths are stubbed — production wiring will attach scene instantiation and asset I/O.

## Layout

| Header | Role |
|--------|------|
| `grid_cell.hpp` | `GridCoord`, `WorldCell`, `CellResidencyState`, grid ↔ world helpers |
| `streaming_volume.hpp` | Camera-centered stream-in / stream-out radii |
| `world_partition.hpp` | `WorldPartitionDesc`, `WorldPartition` update + force load/unload |

## Residency states

`Unloaded` → `QueuedLoad` → `Loading` → `Resident` → `QueuedUnload` → `Unloading` → `Unloaded`

Async mode processes one load and one unload per `update` tick; synchronous mode drains queues immediately (`async_loading = false`).

## Tests

`fuse_world_partition_tests` (`ctest` name `fuse_world_partition_b76`) covers grid math, streaming hysteresis, callback stubs, and camera-driven residency.

## Dependencies

`fuse_core`, `fuse_ecs` (ECS `EntityID`, `vec3`, spatial `AABB` via `fuse_ecs`).
