# FUSE VFS Mount Plan (WP-04)

**Phase:** U3 / WP-04  
**Status:** Dedicated I/O lane (UNI-VFS-1: I/O threads, priorities, cancellation, power suspend, frame budget, decode scratch) + HandleTable commit path landed in `fuse::io::VirtualFileSystem`

---

## Goal

One logical namespace for hybrid 2D/3D content without merging legacy trees:

| Virtual prefix | MountKind | Physical source (future) |
|----------------|-----------|---------------------------|
| `/game/` | `Game` | Project `Assets/` cooked output |
| `/t3d/` | `T3DLegacy` | Quarantined T3D asset roots |
| `/t2d/` | `T2DLegacy` | Quarantined T2D asset roots |

## Threading

- Reads run on **1-2 dedicated I/O threads owned by the VFS** (`IoLaneConfig::ioThreadCount`), never on JobScheduler compute workers. Requests sit in a four-class priority queue (`IoPriority::Critical/High/Normal/Background`, FIFO inside a class). A read hands its bytes to a JobScheduler **decode job** (`LoadRequest::decode`, per-worker `DecodeScratch` linear arena reset per job), to a caller callback (`LoadRequest::onComplete`), or to the game-thread drain queue (`submitLoadAsync`).
- `cancel(LoadId)` drops a queued read, discards an in-flight read/decode result, or removes an undrained completion.
- `suspend()` / `resume()`, and `fuse::platform` `PowerState::Background` (also reached through `notifyAppVisibility(Background)`): in-flight reads finish, queued reads stay queued until Foreground.
- Per-frame read budget (`setFrameReadBudget`, `advanceFrame`): the lane stalls when the next read would exceed the frame's bytes; an oversized read is issued as the first read of a frame so it cannot starve.
- Mount table guarded by a `shared_mutex` (`mount` / `unmount` exclusive, `resolve` shared) so worker-side resolve is safe while mounts change.
- `readFileSync` is implemented on the lane (Critical, bypasses suspend and budget; inline when called from a lane callback).
- Workers enqueue `fuse::io::Asset` payloads with `HandleTable::enqueuePublish`.
- **Game-thread commit rule (locked):** only the game thread may call `HandleTable::commit()` and `HandleTable::insert()` / `remove()` / `get()`. Pending publishes are invisible to `get()` until `commit()` runs — workers must never install live handles directly (see [handle-rules.md](./handle-rules.md), [architecture-parallel.md](./architecture-parallel.md) §3.1).
- Game thread calls `VirtualFileSystem::drainCompletedLoads()` then `HandleTable::commit()` to install live handles.

```
Game thread                          I/O thread (VFS-owned lane)
    │                                      │
    ├─ submitLoadAsync("/game/foo.bin") ──►│ priority queue → budget → raw read
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
- Tests: `Source/FUSE/Core/tests/test_io_lane.cpp` (`fuse_core_io_lane`, label `gate`, picked up by the `^fuse_core_` TSan nightly) — priority ordering, cancel before read / during read / during decode, suspend+resume on a synthetic `PowerState::Background` event and app visibility, frame budget respected (no starvation for oversized reads), concurrent mount/unmount/resolve stress, decode on workers with per-job scratch reset, **zero heap allocations per steady-state callback/decode read** once the lane and scratch are warm (global operator-new counter), sync reads, two I/O threads. Drain-queue loads (`submitLoadAsync`) still move bytes into the queue and so allocate per read by design.

### Async load completion ordering

Completed loads queue in FIFO **completion** order (`pushCompleted` → `drainCompletedLoads` swap). `peekCompletedLoadOrder()` exposes pending `LoadId`s without draining so the game thread can observe completion ordering before commit. Load ids are monotonic per submit call; with one I/O thread and one priority class completion order equals submission order, with two I/O threads loads may complete out of submission order.

## Next slices

1. [x] Linear scratch for decode staging on workers — UNI-VFS-1: `DecodeScratch` (per-thread `fuse::alloc::StackAllocator`, reset per decode job); cook staging still to adopt it  
2. TSan nightly on I/O handoff path (`fuse_core_io_handle` and `fuse_core_io_lane` match `^fuse_core_` in [fuse-tsan-nightly.yml](../../.github/workflows/fuse-tsan-nightly.yml)); a full `-DFUSE_CORE_ENABLE_TSAN=ON` tree was not run in the E01 session (a hand-built TSan binary of the lane + jobs + platform sources passed 5/5 runs clean)  
3. [x] Power-state handling — UNI-VFS-1: the queue suspends (in-flight reads finish, queued reads stay) on `PowerState::Background` and resumes on Foreground; `cancel(LoadId)` is the explicit cancellation API  
4. Callers still to move onto priorities / budget: E04 asset loading and streaming should call `advanceFrame()` once per frame and pick `IoPriority` per request

## Wave 10 (U7 project wiring)

`fuse::project::mountProjectAssetRoots` mounts project `Assets/` + `data/` (+ project root for `/t2d/`) into the process VFS. `materialAssetToVirtualPath` maps legacy `MaterialAsset` refs to `/t3d/materials/...` for resolve checks; runtime embed loads call this on world load (`RuntimeEmbedSession::projectVfsMounts`). Wave 12: `submitT3DMaterialLoadsAsync` + `drainT3DMaterialLoads` optionally integrate `CookCache` (hits skip I/O; drains store `cooked/materials/.../*.fusetex` entries). Wave 15: `remapLegacyAssetPath`, `shaderAssetToVirtualPath`, and `shaderVirtualPathToCookOutput` extend path remapping for shader cooks.
