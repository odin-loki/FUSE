# FUSE U0 — Concurrency Inventory (Evidence)

**Phase:** U0 datamine  
**Date:** 2026-09-15  
**Scope:** Threading, async I/O, pools, and frame-loop assumptions in T3D, T2D, and FUSE plans.  
**Method:** Submodule trees scanned; pattern counts via repo-wide grep (see §7).

---

## 1. Executive summary

| Engine | Primary concurrency model | Job system | Main-thread centric? |
|--------|---------------------------|------------|----------------------|
| **T3D** (`Engine/source`) | `ThreadPool` global singleton + dedicated `Thread`/`NetAsync`/SFX async threads | Partial — work-stealing **not** present; priority queue pool | **Yes** — sim, console, GFX submit, most gameplay |
| **T2D** (`third_party/Torque2D/engine/source`) | `Mutex` + optional `NetAsync` **dedicated thread** | **No** `ThreadPool` in tree | **Yes** — single-threaded game loop |
| **FUSE (planned)** | Fiber work-stealing scheduler ([FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) B1.5, P3) | **Greenfield** — not stubbed in repo yet | Game thread owns mutation; workers for parallel read-mostly work |

**Key finding:** Legacy Torque is **main-thread-first**. T3D has real but **narrow** background parallelism (I/O, DNS, image compress, audio streaming). Neither engine is safe for arbitrary multi-threaded `SimObject` access. FUSE must treat MT as a **new spine**, not an emergent property of linking both engines.

---

## 2. T3D (`Engine/source`) — threading primitives

### 2.1 Core platform layer

| Component | Path | Role |
|-----------|------|------|
| `Thread` | `platform/threads/thread.h` | OS thread wrapper; `start()`, `join()`, `checkForStop()` |
| `ThreadManager` | `platform/threads/thread.h` | Tracks threads; `isMainThread()`, `getMainThreadId()` |
| `Mutex` / `Semaphore` | `platform/threads/mutex.h`, `semaphore.h` | Win32/SDL/POSIX implementations under `platformWin32/threads/`, `platformSDL/threads/` |
| `ThreadSafeRefCount` | `platform/threads/threadSafeRefCount.h` | Ref-counted work items, async packets |
| `ThreadSafeDeque` / `PriorityQueue` / `FreeList` | `platform/threads/threadSafe*.h` | Lock-protected containers |

**Counts (approx.):** `Mutex` 129 refs / 16 files; `Thread` 165 refs / 38 files; `ThreadSafe*` 264 refs / 48 files.

### 2.2 ThreadPool (primary async work manager)

| Item | Evidence |
|------|----------|
| Header | `platform/threads/threadPool.h` (422 lines) |
| Implementation | `platform/threads/threadPool.cpp` |
| Global singleton | `ThreadPool::GLOBAL()` via `GlobalThreadPool` |
| Default worker count | `numThreads == 0` → CPU core count (`threadPool.h` L297–298) |
| **Main-thread-only submit (global pool)** | Documented invariant: only main thread may submit to global pool; otherwise `flushWorkItems()` can deadlock (`threadPool.h` L51–55, L312–316) |
| Main-thread ping-back | `queueWorkItemOnMainThread()` + `processMainThreadWorkItems()` — called each frame from `app/mainLoop.cpp` L605 |
| Force single-thread debug | `smForceAllMainThread` / `_forceAllMainThread` console variable (`mainLoop.cpp` L269) |
| Tests | `testing/threadPoolTest.cpp`, `threadTest.cpp`, `mutexTest.cpp`, `semaphoreTest.cpp` |

**Call sites for `queueWorkItem` (8 files):**

| File | Use |
|------|-----|
| `gfx/bitmap/imageUtils.cpp` | Parallel image compress (`CompressJob` : `WorkItem`); `waitForAllItems()` on main |
| `core/stream/fileStream.cpp` | Async file read via thread pool |
| `platform/platformNetAsync.cpp` | DNS / `stringToAddress` lookup (`NameLookupWorkItem`) |
| `sfx/sfxInternal.h` / `sfxSystem.cpp` | `SFXThreadPool` singleton for streaming decode |
| `gfx/video/theoraTexture.cpp` | Video decode; `flushWorkItems()` before GPU upload |
| `platform/async/asyncBufferedStream.h` | Async packet streaming |

### 2.3 Async I/O subsystem

| Component | Path |
|-----------|------|
| `AsyncIOItem` | `platform/threads/threadPoolAsyncIO.h` — template work items for stream I/O |
| `AsyncPacketQueue` / `AsyncBufferedStream` | `platform/async/asyncPacketQueue.h`, `asyncBufferedStream.h` |
| `AsyncUpdateThread` | `platform/async/asyncUpdate.h` — background update loop for polled objects |
| Tests | `platform/async/test/asyncPacketQueueTest.cpp` |

**Warning in source:** `AsyncIOItem` notes chaos if multiple threads touch same stream without coordination (`threadPoolAsyncIO.h` L51–53).

### 2.4 Networking

| Component | Path | Thread model |
|-----------|------|--------------|
| `NetAsync` | `platform/platformNetAsync.h`, `.cpp` | T3D: **`ThreadPool::WorkItem`** for DNS (L68–136) |
| `platformNet.cpp` | `platform/platformNet.cpp` | Polls `gNetAsync.checkLookup()` during connect |
| Game net loop | `T3D/gameBase/gameConnection.h`, `processList` | Main-thread tick; packet handling on game thread |

T2D contrast: `third_party/Torque2D/engine/source/platform/platformNetAsync.cpp` uses a **dedicated `Thread`** (`StartThreadFunc`, L197–217), not `ThreadPool`.

### 2.5 Simulation / console (main-thread assumptions)

| Component | Path | Concurrency note |
|-----------|------|------------------|
| Event queue mutex | `console/simManager.cpp` L59–99 | `gEventQueueMutex` protects `Sim::postEvent` |
| `Sim::postEvent` | `simManager.cpp` | 45 call sites across 19 files — events queued under mutex |
| `Con::isMainThread()` | `console/console.cpp` L436–439 | Delegates to `ThreadManager::isMainThread()` |
| Script compile guard | `console/torquescript/codeBlock.cpp` L464, L572 | `AssertFatal(Con::isMainThread(), "Compiling code on a secondary thread")` |
| String stack guard | `console/stringStack.cpp` L92, L105 | Main-thread-only console buffers |
| `SimSet` mutex | `console/simSet.h` L253–260 | `Mutex::lockMutex(mMutex)` on iteration |
| Profiler | `platform/profiler.cpp` L254+ | Ignores samples off main thread |

**No fibers, no coroutines** in T3D tree (0 grep hits for `fiber` / `coroutine`).

### 2.6 Graphics

| Component | Path | Thread model |
|-----------|------|--------------|
| Main loop GFX | Driven by `Process::processEvents()` signal chain | **Main thread** presents and issues draw calls |
| `gFont` | `gfx/gFont.cpp` L214–294 | `mMutex` around platform font glyph cache |
| Image utils | `gfx/bitmap/imageUtils.cpp` | CPU compress on workers; GPU work on main after `waitForAllItems()` |
| Theora video | `gfx/video/theoraTexture.h` L76–77, L151–152 | Decode on workers; **cannot do GFX on worker threads** — explicit comment |
| Terrain | `terrain/*.cpp` | `PROFILE_SCOPE` only — **no ThreadPool** in terrain tree |
| Forest | `forest/` | No thread pool usage found |

**No dedicated render thread** in T3D source — rendering is main-thread coupled to window/GFX device.

### 2.7 Audio (SFX)

| Component | Path | Thread model |
|-----------|------|--------------|
| `SFXInternal::SFXThreadPool` | `sfx/sfxInternal.h`, `sfxSystem.cpp` L558 | Dedicated pool for stream decode |
| `SFXUpdateThread` | `sfxInternal.h` L66 | `AsyncUpdateThread` subclass |
| `sfxALVoice::mMutex` | `sfx/openal/sfxALVoice.h` L69 | Voice state shared between SFX thread and main |
| `sfxVoice.h` | L77–128 | Comments: methods called from **both** SFX update thread and main thread |

### 2.8 Third-party / tooling parallelism

| Component | Path | Notes |
|-----------|------|-------|
| VHACD | `ts/vhacd/VHACD.h` L5757+ | Internal `std::thread` pool (8 workers default L7208) — mesh cooking only |
| stb_image | `gfx/bitmap/loaders/stb/stb_image.h` | Optional parallel decode interface (comment L293) |

### 2.9 Frame loop (T3D)

```
StandardMainLoop::doMainLoop()          [app/mainLoop.cpp]
  → Process::processEvents()            [core/util/journal/process.cpp]
       → _signalProcess.trigger()       (modules register: input, sim tick, render, net, …)
  → ThreadPool::processMainThreadWorkItems()
  → ConsoleValue::resetConversionBuffer()
```

`Process::get()` explicitly **not thread-safe** (`process.cpp` L76–77).

---

## 3. T2D (`third_party/Torque2D/engine/source`)

### 3.1 Threading primitives

| Component | Path | Notes |
|-----------|------|-------|
| `Thread` / `ThreadManager` | `platform/threads/thread.h` | Same lineage as T3D; `threadPool` vector in manager (L104) — **not** T3D's `ThreadPool` class |
| `Mutex` | `platform/threads/mutex.h` + platform impls | **214 refs / 25 files** — used heavily for net, components |
| `ThreadSafe*` | 12 refs / 5 files | Far less than T3D |

**Counts:** `ThreadPool` **0**; `queueWorkItem` **0**; `processMainThreadWorkItems` **0**.

### 3.2 Networking async

| Component | Path | Model |
|-----------|------|-------|
| `NetAsync` | `platform/platformNetAsync.cpp` | Dedicated `Thread` with sleep-poll loop (L108–163) |
| Mutex | `gNetAsyncMutex` L43–63 | Protects lookup request vector |

### 3.3 Simulation / components

| Component | Path | Notes |
|-----------|------|-------|
| `SimComponent` | `component/simComponent.cpp` L35 | Per-component `mMutex`; `lockComponentList()` |
| `Sim::postEvent` | 20 refs / 10 files | Same event-queue pattern as T3D |
| `simSet.h` | Mutex on set iteration (like T3D) |

### 3.4 Graphics / audio

| Component | Notes |
|-----------|-------|
| `graphics/gFont.h` L111 | `void *mMutex` — glyph cache |
| `audio/` | No thread pool; platform audio on main/mobile callbacks |
| **No async texture pipeline** comparable to T3D Theora/thread pool |

### 3.5 Frame loop (T2D)

| Entry | Path |
|-------|------|
| `DefaultGame::mainLoop()` | `game/defaultGame.h` L44 |
| `GameInterface::processEvents()` | `game/gameInterface.h` L71 |
| `Tickable::advanceTime()` | `platform/Tickable.h` — frame-rate dependent, **not** fixed 32ms tick |
| Mobile | `platformAndroid/main.cpp` L62 `Game->mainLoop()`; iOS timer `mainLoopTimer` |

**Assumption:** single game thread; Box2D and Scene2D tick on main thread.

### 3.6 T2D config hint

`third_party/Torque2D/engine/source/torqueConfig.h` L87–90 — `TORQUE_MULTITHREAD` define: *"does not make the entire engine thread-safe nor is it a magic bullet"* for parallelism.

### 3.7 Mobile & web platform trees (FUSE must honour)

| Platform | Path | Thread / sync | GFX | Lifecycle |
|----------|------|---------------|-----|-----------|
| **iOS** | `platformiOS/` (41+ files) | `iOSThread.mm`, `iOSMutex.mm` | `iOSGL2ES.mm` — **OpenGL ES** | `backgrounded` in `platformiOS.h` L78; `iOSTime.mm` skips when backgrounded |
| **Android** | `platformAndroid/` (50+ files) | `AndroidThread.cpp`, `AndroidMutex.cpp` | `AndroidGL2ES.cpp` | `backgrounded` in `platformAndroid.h` L70 |
| **Emscripten** | `platformEmscripten/` (30+ files) | `EmscriptenThread.cpp` — limited | `EmscriptenGL2ES.cpp` | `backgrounded` in `EmscriptenWindow.cpp` |
| **macOS desktop** | `platformOSX/` | `osxTime.mm` checks `backgrounded` | Desktop GL | — |

**CMake evidence:** `third_party/Torque2D/CMakeLists.txt` L10–11 — targets Win, macOS, Linux, **iOS, Android, Emscripten**. iOS explicit `CMAKE_SYSTEM_NAME=iOS` (L108–110); GLES not desktop GL (L319).

**FUSE implication:** Job worker caps and background drain are **required** for parity with T2D mobile behaviour — not optional desktop stretch goals. See [architecture-parallel.md](./architecture-parallel.md) §3.1.1, §3.6.

---

## 4. FUSE plans & repo stubs

| Source | Concurrency content |
|--------|---------------------|
| [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §P3, B1.5 | Fiber work-stealing scheduler, `JobCounter`, `parallel_for`, CUDA jobs; no `std::mutex` on hot path |
| [FUSE_UNIFIED_PRESTARTER.md](../plans/FUSE_UNIFIED_PRESTARTER.md) §3.1 L0 | Core includes **jobs** |
| [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) | Handle-based cross-thread rules (to be implemented U3+) |
| `Source/FUSE/` | **Does not exist yet** — no job stubs in repo |
| Root `CMakeLists.txt` | C++17; no FUSE job target |

---

## 5. Collision / MT interaction (from U0 symbol report)

Linking both legacy engines increases **thread-safety risk** beyond symbol ODR:

| Shared concept | Risk if dual-linked |
|----------------|---------------------|
| `SimObject` / event queue | Two independent `gEventQueueMutex` domains if both sims active — adapter must not cross-post events |
| `StringTable` | Not thread-safe; dual singletons |
| `Con::` | Main-thread-only by design; **forbidden from worker fibers** without marshalling |
| `ThreadPool::GLOBAL()` | Two pools if both engines instantiate — must not share work items across legacy boundaries |

---

## 6. Gap analysis (legacy → FUSE target)

| Capability | T3D | T2D | FUSE target |
|------------|-----|-----|-------------|
| Work-stealing jobs | No | No | **Yes** (B1.5) |
| Dependency graphs / fiber wait | No | No | **Yes** |
| Parallel scene tick | No (main thread) | No | **Selective** (read-only passes) |
| Parallel physics | No | No (Box2D main) | Dual backends; **parallel islands** later |
| Async asset load | ThreadPool + AsyncIO | Sync-heavy | FUSE I/O jobs + handle publish |
| Render thread | No | No | Phase 2 optional; **main-thread record v1** |
| TSan-clean sim | No | No | **Gate** (U3+) |

---

## 7. Reproduction commands

```bash
# Pattern counts (used for §2–3 tables)
python3 -c "
import os,re
from collections import defaultdict
patterns = {'Mutex': r'\\bMutex\\b', 'ThreadPool': r'\\bThreadPool\\b',
            'Thread': r'\\bThread\\b', 'Sim::postEvent': r'Sim::postEvent'}
for label, root in [('T3D','Engine/source'),('T2D','third_party/Torque2D/engine/source')]:
    for name, pat in patterns.items():
        n = sum(len(re.findall(pat, open(os.path.join(dp,f),errors='ignore').read()))
                for dp,_,fs in os.walk(root) for f in fs if f.endswith(('.cpp','.h')))
        print(label, name, n)
"

# ThreadPool call sites
rg 'queueWorkItem|processMainThreadWorkItems' Engine/source --glob '*.cpp'

# Main-thread guards
rg 'isMainThread' Engine/source --glob '*.{cpp,h}'
```

---

## 8. Gate / consumers

- Feeds [architecture-parallel.md](./architecture-parallel.md) (target design)
- Feeds [work-plan.md](./work-plan.md) (WP scheduling)
- Risk items: R14, R16, R17 (new) in [risk-register.md](./risk-register.md)
