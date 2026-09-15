# Track B — Core B1.3 Memory & Allocator System (deepen)

**Status:** B1.3 frame allocator deepen, pool/stack stubs, and stats hooks on `fuse_core`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.3  
**Related:** WP-04 frame scratch; B2 per-frame GPU staging allocators (follow-up)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `allocator.hpp` | `Source/FUSE/Core/include/fuse/alloc/` | `AllocInfo`, `IAllocator` surface |
| `alloc_stats.hpp` | `Source/FUSE/Core/include/fuse/alloc/` | `AllocStats`, global stats hook |
| `frame_allocator.hpp` | `Source/FUSE/Core/include/fuse/alloc/` | Ping-pong bump buffers, peak + failure counters |
| `pool_allocator.hpp` | `Source/FUSE/Core/include/fuse/alloc/` | Fixed-size freelist stub |
| `stack_allocator.hpp` | `Source/FUSE/Core/include/fuse/alloc/` | LIFO mark/rollback stub |
| `test_allocator.cpp` | `Source/FUSE/Core/tests/` | Frame, pool, stack, stats hook acceptance |

**Not in scope (follow-up PRs):** TLSF / ring / GPU allocators, memory budgets, ASan intercept of `new`/`malloc`, Torque `Memory::` routing.

---

## Build

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --target fuse_core_allocator_tests
ctest --test-dir build -R fuse_core_allocator
```

---

## Frame allocator (ping-pong)

```cpp
#include <fuse/alloc/frame_allocator.hpp>

fuse::alloc::FrameAllocator scratch(64 * 1024);
auto* jobData = scratch.allocate<MyJobData>();
scratch.advanceFrame(); // previous frame bytes readable via previousFrameData()
scratch.reset();        // manual bump reset on active buffer
```

- Dual buffers retain one frame of scratch for debug inspection.
- `stats()` exposes `usedBytes`, `peakUsedBytes`, `allocCount`, `failedAllocs`.
- Implements `IAllocator` for future domain wiring.

---

## Pool / stack stubs

```cpp
fuse::alloc::PoolAllocator components(sizeof(Component), 1024, "ecs_components");
Component* c = components.allocate<Component>();

fuse::alloc::StackAllocator temp(32 * 1024);
const auto mark = temp.pushMark();
// ...
temp.popToMark(mark);
```

Pool: O(1) fixed-block freelist. Stack: bump + mark rollback and strict LIFO `free`.

---

## Stats hooks

```cpp
fuse::alloc::setGlobalStatsHook(
  [](const char* name, const fuse::alloc::AllocStats& stats, void*) {
    // debug overlay / budget enforcement stub
  });
```

Allocators call `notifyStats` after successful/failed operations and on `reset` / `advanceFrame`.

---

## Tests

| Test binary | CTest name | Coverage |
|-------------|------------|----------|
| `fuse_core_allocator_tests` | `fuse_core_allocator` | Bump reset, alignment, ping-pong, OOM stats, pool freelist, stack rollback, global hook |
| `fuse_core_services_tests` | `fuse_core_services` | Legacy frame allocator smoke (unchanged) |

---

## Gates (B1.3 deepen)

- [x] Expanded `FrameAllocator` with ping-pong + stats on FUSE APIs
- [x] `PoolAllocator` / `StackAllocator` stubs + `IAllocator` surface
- [x] Global stats hook + unit tests + this doc
- [ ] TLSF / ring / GPU allocator implementations
- [ ] Memory budget enforcement
- [ ] 1M alloc/free stress + ASan intercept milestone
