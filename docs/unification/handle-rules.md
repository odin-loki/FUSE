# FUSE Handle Rules (WP-04 / U3)

**Phase:** U3 shared services  
**Status:** Locked for product code  
**Related:** [architecture-parallel.md](./architecture-parallel.md) §3.1, [vfs-mount-plan.md](./vfs-mount-plan.md)

---

## 1. `fuse::Handle<T>`

- Opaque `(index, generation)` pair — never pass raw `T*` across job boundaries.
- `isValid()` is false for default-constructed handles (`index == UINT32_MAX`).
- Stale handles fail `HandleMap::valid()` / `HandleTable::valid()` when generation mismatches after slot reuse.

## 2. `HandleTable<T>` — publish vs commit

| Operation | Thread | Visibility |
|-----------|--------|------------|
| `insert()` / `remove()` / `get()` | **Game thread only** | Live slots |
| `enqueuePublish(T&&)` | **Workers** (I/O lane, cook jobs) | Pending queue — invisible to `get()` |
| `commit()` | **Game thread only** | Moves pending → live; returns new `Handle<T>`s |

**Rule:** workers never call `insert`, `remove`, `get`, or `commit`. Pending publishes are not readable until the game thread commits.

## 3. I/O lane handoff (VFS)

```
Game thread                          I/O worker (JobScheduler)
    │                                      │
    ├─ submitLoadAsync("/game/foo.bin") ──►│ resolve + read bytes
    │                                      ├─ pushCompleted(Asset)
    ├─ drainCompletedLoads(table) ◄────────┤  → table.enqueuePublish()
    ├─ table.commit() → Handle<Asset>      │
    └─ table.get(handle)                     │
```

- Failed resolve/read completes with `success=false`; no publish is enqueued.
- Completed loads drain in FIFO completion order (`peekCompletedLoadOrder()` for observation).

## 4. Forbidden patterns

- Worker writes into live `HandleMap` slots.
- Game thread blocks on I/O mutex in the hot tick path.
- Storing `fuse::Object*` or legacy `SceneObject*` in job payloads — publish `Handle<Object>` after commit instead.

## 5. Tests

- `Source/FUSE/Core/tests/test_io_handle.cpp` — direct insert, worker publish, concurrent races, async VFS round-trip.
- `Source/FUSE/Core/tests/test_u3_gate.cpp` — U3 exit: both dims log via FUSE logger + one VFS job load commits a handle.
