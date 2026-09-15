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

- `Source/FUSE/Core/include/fuse/io/vfs.hpp` — mount registry, resolve, `submitLoadAsync`, `drainCompletedLoads`
- `Source/FUSE/Core/include/fuse/handle_table.hpp` — `enqueuePublish` / `commit`
- Tests: `Source/FUSE/Core/tests/test_io_handle.cpp`, existing `test_services.cpp` VFS resolve checks

## Next slices

1. Frame-allocator scratch for decode/cook staging on workers  
2. TSan nightly on I/O handoff path  
3. Cancellation when `fuse::platform::getPowerState() == Background`
