# FUSE VFS Mount Plan (WP-04)

**Phase:** U3 / WP-04  
**Status:** Async I/O lane + HandleTable commit path landed in `fuse::io::VirtualFileSystem`

---

## Goal

One logical namespace for hybrid 2D/3D content without merging legacy trees:

| Virtual prefix | MountKind | Physical source (future) |
|----------------|-----------|---------------------------|
| `/game/` | `Game` | Project `Assets/` cooked output |
| `/t3d/` | `T3DLegacy` | Quarantined T3D asset roots |
| `/t2d/` | `T2DLegacy` | Quarantined T2D asset roots |

## Threading

- Blocking read on the **I/O lane** via `JobScheduler::submit()` (`VirtualFileSystem::submitLoadAsync`).
- Workers enqueue `fuse::io::Asset` payloads with `HandleTable::enqueuePublish`.
- **Game-thread commit rule (locked):** only the game thread may call `HandleTable::commit()` and `HandleTable::insert()` / `remove()` / `get()`. Pending publishes are invisible to `get()` until `commit()` runs — workers must never install live handles directly (see [architecture-parallel.md](./architecture-parallel.md) §3.1).
- Game thread calls `VirtualFileSystem::drainCompletedLoads()` then `HandleTable::commit()` to install live handles.

```
Game thread                          I/O worker (job lane)
    │                                      │
    ├─ submitLoadAsync("/game/foo.bin") ──►│ resolve + readFileBytes
    │                                      ├─ pushCompleted(Asset)
    ├─ drainCompletedLoads(table) ◄────────┤
    ├─ table.commit() → Handle<Asset>      │
    └─ table.get(handle)                     │
```

## API (Core)

| Header | Role |
|--------|------|
| `fuse/io/vfs.hpp` | Mount registry, path resolve, async load submission, completed-load drain |
| `fuse/io/asset.hpp` | Raw byte payload + virtual path |
| `fuse/handle_table.hpp` | Worker publish queue + game-thread commit into `HandleMap` |

## Current implementation

- `Source/FUSE/Core/include/fuse/io/vfs.hpp` — mount registry, resolve, `submitLoadAsync`, `drainCompletedLoads`, `peekCompletedLoadOrder`
- `Source/FUSE/Core/include/fuse/handle_table.hpp` — `enqueuePublish` / `commit`
- Tests: `Source/FUSE/Core/tests/test_io_handle.cpp` — direct insert, worker publish + commit, concurrent publish/commit races, async load ordering, existing `test_services.cpp` VFS resolve checks

### Async load completion ordering (stub)

Completed loads queue in FIFO **completion** order (`pushCompleted` → `drainCompletedLoads` swap). `peekCompletedLoadOrder()` exposes pending `LoadId`s without draining so the game thread can observe completion ordering before commit. Load ids are monotonic per submit call; parallel I/O workers may complete out of submission order.

## Next slices

1. Frame-allocator scratch for decode/cook staging on workers  
2. TSan nightly on I/O handoff path (`fuse_core_io_handle` in [fuse-tsan-nightly.yml](../../.github/workflows/fuse-tsan-nightly.yml))  
3. Cancellation when `fuse::platform::getPowerState() == Background`
