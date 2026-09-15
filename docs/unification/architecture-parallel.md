# FUSE — Parallel Architecture (Multi-Threading First-Class)

**Phase:** U0 architecture (stakeholder direction)  
**Date:** 2026-09-15  
**Status:** Target design — documentation only  
**Evidence base:** [concurrency-inventory.md](./concurrency-inventory.md)  
**Aligns with:** [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md), [unified-layout.md](./unified-layout.md), [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) B1.5 / P3

> **Tagline:** Exact where it matters. Parallel everywhere else.

---

## 1. Design principles

1. **Jobs are the spine** — rendering prep, physics islands, asset decode, AI/FX/cinematics work submit to one scheduler; no ad-hoc `std::thread` in product code.
2. **Main thread owns mutation** — `fuse::Object` graph changes, script callbacks that touch scene state, and GPU **submit** (v1) run on the **game thread**.
3. **Handles, not pointers, across threads** — `fuse::Handle<T>` + epoch/generation; workers produce **command buffers** / immutable snapshots consumed on game thread.
4. **Composition over false inheritance for MT** — physics, gfx devices, and net stacks are **composed** behind facades ([merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md)); never inherit Box2D or GL contexts up the scene tree.
5. **Deterministic fallback** — single-threaded mode (`FUSE_JOBS_SINGLE_THREAD=1`) for replay, tests, and bisect; behaviour must match modulo timing.
6. **Sanitizers as gates** — ASan on all CI smoke; TSan job on parallel paths from U3 onward.

---

## 2. Process model

### 2.1 Binaries

| Binary | Process | Threads |
|--------|---------|---------|
| `fuse_runtime` | One OS process per game/player | Game + job workers + I/O + audio (see §3) |
| `fuse_editor` | **Same process** as embedded runtime for PIE (recommended default) | Qt UI thread + game thread + workers |
| `fuse_tools` | CLI; may use workers only | Optional job pool; no GPU |

**One program** (prestarter): hybrid 2D/3D in one `fuse_runtime` — not separate Torque EXEs.

### 2.2 In-process layout (runtime)

```
┌─────────────────────────────────────────────────────────────┐
│  fuse_runtime (single process)                               │
│  ┌─────────────┐  ┌──────────────────────────────────────┐  │
│  │ Qt (editor │  │ Game thread (main)                    │  │
│  │  only)     │  │  input → tick → render record → present│  │
│  └──────┬──────┘  └───────────────┬──────────────────────┘  │
│         │ post commands            │ submit jobs              │
│         ▼                          ▼                          │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │ JobScheduler (fiber work-stealing, N workers)            │ │
│  └──────────────────────────────────────────────────────────┘ │
│  ┌────────────┐ ┌────────────┐ ┌─────────────────────────┐ │
│  │ I/O lane   │ │ Audio lane │ │ Legacy adapters (quarantine)│ │
│  │ (optional) │ │ (1 thread) │ │ fuse_t3d_legacy / t2d    │ │
│  └────────────┘ └────────────┘ └─────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

**Recommended default:** Editor **in-process** (not out-of-process) — matches prestarter one-program goal; Qt on UI thread, engine on game thread ([stakeholder defaults §12](#12-stakeholder-questions--recommended-defaults)).

---

## 3. Threading model

### 3.1 Thread roles

| Role | Count | Responsibilities | Must not |
|------|-------|------------------|----------|
| **Game thread** | 1 | Input sampling, `Sim`/`fuse::Object` mutation, script host callbacks, render **recording** + present (v1), job sync points | Block on mutex in hot path; call legacy `Con::execute` from workers |
| **Job workers** | `max(1, physical_cores - 2)` default | Fiber scheduling, `parallel_for`, asset decode, mesh cook, BT eval (read-only phase), FX sim buffers | Touch `fuse::Object` without command buffer; Open GL/D3D on v1 |
| **I/O thread(s)** | 0–2 (default 1) | Blocking read/decompress; hands buffers to job queue | Publish handles without game-thread commit |
| **Audio thread** | 1 | Device feed (OpenAL/FMOD pattern from T3D `SFXUpdateThread`) | Scene graph access |
| **Net poll** | Game thread (v1) | Packet read/write integrated in tick | — |
| **Qt UI thread** | 1 (editor only) | Widgets, inspectors, timelines | Direct `fuse::Object*` — use Editor API + queued commands |

**Worker count policy (recommended default):** `physical_cores - 2`, minimum 1, maximum 16; reserve one core for game thread and one for OS/audio. Override via `project.json` → `jobs.worker_count` and env `FUSE_JOB_WORKERS`.

### 3.2 Job system — recommendation

| Option | Verdict |
|--------|---------|
| **Fiber work-stealing scheduler** | **Recommended** — master plan B1.5; `JobCounter::wait()` yields without blocking OS threads; supports dependency chains for render prep and asset staging |
| Plain thread pool (T3D-style) | **Reject as spine** — T3D `ThreadPool` forbids global submit off main thread (`threadPool.h` L51–55); no fiber wait; encourages blocking `waitForAllItems()` on game thread |
| `std::async` / per-task threads | **Forbidden** in product code |

**API home:** `Source/FUSE/Core/include/fuse/jobs/` — `JobScheduler`, `JobCounter`, `parallel_for`, `JobPriority`.

**Decision gate (non-blocking):** CUDA job lane timing (Track B) — architecture reserves `submit_cuda()` hook; implementation can trail P3.

### 3.3 Fibers vs OS threads (rationale)

- **Fibers** cheaply express fork-join (`parallel_for`), staggered asset dependencies, and "wait for N jobs" without parking worker threads — matches master plan hot-path rule (no `std::condition_variable` on gameplay path).
- **OS threads** remain the pool backing fibers; I/O and audio stay real threads due to blocking APIs.

---

## 4. Frame pipeline

### 4.1 High-level order (one frame)

```
1. Poll OS input / window events          [game]
2. Net receive + ghost prep (if any)    [game]
3. Job sync: consume completed I/O        [game applies handle publishes]
4. Parallel dimension tick (§4.2)         [game + jobs]
5. Feature modules (AI, FX, cinematics)  [jobs → game merge]
6. Render record 3D                       [game]
7. Render record 2D                       [game]
8. Hybrid compose + present               [game]
9. Audio mix commit                       [audio thread ← commands from game]
10. processMainThread legacy queue (compat) [game — shrink over time]
```

### 4.2 Parallel dimension ticks

| Phase | World2D | World3D | Parallelism |
|-------|---------|---------|-------------|
| **Read-only cull / gather** | Sprite bounds, layer lists | Zone/frustum cull | `parallel_for` over chunks |
| **Physics step** | Box2D world | T3D collision world | **Sequential per world** on game thread (v1); optional island parallel later |
| **Scene graph mutation** | Add/remove/move `SceneObject2D` | Same for `SceneObject3D` | **Game thread only** |
| **Script tick** | Compat VM | Compat VM | Game thread; jobify only compiled bytecode regions (late) |

**Sync point:** `FrameBarrier` at end of tick — all jobs for frame `N` complete before render record starts.

**Hybrid ordering (U4):** 3D opaque → 3D transparent → 2D scene → UI overlay (prestarter §9.2). Compositor runs on game thread; each dimension writes to separate GPU targets then blits/composes.

### 4.3 Render strategy (v1 → v2)

| Version | Model | Rationale |
|---------|-------|-----------|
| **v1 (U4–U6)** | Record + execute on **game thread** | T3D/T2D both assume main-thread GFX; Theora explicitly cannot upload on workers (`theoraTexture.h` L151–152) |
| **v2 (Track B)** | Render graph record on game thread, execute on **async compute/graphics queue** where RHI allows | Requires FUSE RHI abstraction; dual GL contexts not attempted |

---

## 5. Scene merge strategy × multi-threading

From [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md):

### 5.1 Type hierarchy (unchanged by MT)

```cpp
fuse::Object
  └── fuse::SceneObject2D    // xy, layer, 2D bounds
        └── fuse::SceneObject3D  // + z, quat, 3D AABB, render/collision handles
```

### 5.2 Ownership rules

| Rule | Detail |
|------|--------|
| **Identity** | `fuse::Handle<ObjectId>` stable across frames; generation bumped on delete |
| **Mutation** | Only game thread calls `setTransform`, parent, destroy |
| **Worker read** | `ObjectSnapshot` / SOA arrays built on game thread, then `parallel_for` read |
| **Cross-dimension** | 2D HUD over 3D: separate `World2D` overlay graph; no pointer from 3D `ShapeBase` to 2D sprite |
| **Legacy adapter** | `fuse::legacy::t3d::SceneObject*` never escapes to workers; adapter copies data into snapshots |

### 5.3 Command buffers & double buffering

- **Render:** per-frame `RenderCommandList` (draws, constants) filled by jobs, merged on game thread.
- **Transform:** double-buffered `TransformSoA` — workers read buffer `N`, game writes `N+1` during tick.
- **Publish pattern:** worker completes → `JobCounter` → game thread `apply()` publishes handles to live graph.

---

## 6. Memory & allocators

| Allocator | Thread | Use |
|-----------|--------|-----|
| **Global** `fuse::Heap` | any (atomic buckets) | long-lived assets |
| **Per-frame linear** | game thread write; workers read prior frame | temp arrays, cull lists |
| **Per-job scratch** | worker-local bump | decode, cook, BT eval |
| **Legacy** | quarantined in `fuse_t3d_legacy` | no new allocations from workers into legacy heaps |

**Forbidden:**
- Raw `SimObject*` / `SceneObject*` on worker threads
- `StringTable` intern from workers (game thread marshalling only)
- `Con::execute` / `Con::printf` from workers (use `fuse::log` thread-safe queue)

---

## 7. Subsystem mapping (parallel behaviour)

| Subsystem | Game thread | Jobs | Notes |
|-----------|-------------|------|-------|
| **Core / math** | tick setup | `parallel_for` bulk math | Immutable after publish |
| **Assets** | handle publish | disk read, decode, cook | T3D `fileStream` + ThreadPool pattern → FUSE I/O lane |
| **World2D** | graph mutation | cull, batch build | Box2D step on game thread (v1) |
| **World3D** | graph mutation | cull, LOD gather | T3D collision on game thread (v1) |
| **Physics** | step + sync | — (v1) | Composition boundary |
| **Gfx** | record + present | mesh/skin prep, texture upload **staging** | GPU submit game thread v1 |
| **Audio** | command write | decode stream | Dedicated audio thread |
| **Net** | poll + apply | DNS only (like `NetAsync`) | No parallel ghost mutation |
| **Script** | all mutation | — | Single host (U3+) |
| **AI (`fuse_ai`)** | blackboard commit | BT eval on snapshots | BadBehaviour ore → jobified trees |
| **FX (`fuse_fx`)** | spawn/despawn | particle sim SOA | AFX-style effectron buffers |
| **Cinematics** | director tick | key eval | Verve tracks → sampled curves in parallel |

---

## 8. Editor (Qt) threading

```
UI thread (Qt)                    Game thread
     │                                  │
     │  Inspector edits property        │
     ├──── CommandQueue::post ─────────►│ apply to fuse::Object
     │                                  │
     │◄─── StateSnapshot (handles) ─────┤ periodic pull for UI
     │                                  │
     │  Play-in-editor                  │
     ├──── start fuse_runtime embed ───►│ same process, shared JobScheduler
```

- **No Qt in `fuse_core`** — Editor API only ([unified-layout.md](./unified-layout.md)).
- **PIE:** game thread runs full loop; UI throttled to ~30 Hz snapshots for hierarchy.
- **Viewport:** Qt widget owns window; game thread renders into embedded surface (platform-specific interop).

---

## 9. Legacy quarantine (U2+)

| Legacy | MT rule |
|--------|---------|
| `fuse_t3d_legacy` | Expose **adapter API** callable only from game thread; internal `ThreadPool::GLOBAL()` stays inside lib — **do not** submit work that callbacks into FUSE graph without marshalling |
| `fuse_t2d_legacy` | Same; T2D has no pool — even stricter main-thread assumption |
| Dual `Con::` | Workers **never** call; compat script only on game thread |
| `_forceAllMainThread` | Map to `FUSE_JOBS_SINGLE_THREAD` for parity debugging |

---

## 10. Safety & determinism

| Mode | Config | Use |
|------|--------|-----|
| **Production** | workers = auto | Ship default |
| **Deterministic** | `FUSE_JOBS_SINGLE_THREAD=1`, fixed seed | Golden tests, repro |
| **ASan** | CI mandatory | All merges to `main` |
| **TSan** | CI nightly from U3 | Job + snapshot paths |
| **Replay** | record job DAG + inputs | Optional U8+ |

---

## 11. Anti-patterns (explicit non-goals)

| Anti-pattern | Why |
|--------------|-----|
| Global `Con::` from workers | Legacy main-thread contract (`codeBlock.cpp` asserts) |
| `waitForAllItems()` every frame on hot path | Stalls game thread — use `JobCounter` + fiber yield inside workers |
| Sharing `Mutex` on `SimObject` across engines | Dual legacy + race |
| `SceneObject3D : b2World` or gfx device inheritance | Violates composition boundary (R16) |
| Parallel script VM without proof | U3+ gate |
| Separate render thread + dual GL (v1) | Context hell on Win/Linux/macOS |
| Merging addon Engine for "more threads" | Forbidden — ore only |

---

## 12. Stakeholder questions & recommended defaults

Coordinator may confirm; **work proceeds with defaults** unless overridden.

| # | Question | Options | **Recommended default** |
|---|----------|---------|-------------------------|
| Q1 | Target platforms (MT policy) | Desktop only / +mobile / +web | **Desktop first** (Win/Linux/macOS); mobile/web keep T2D single-thread compat path until RHI rewrite |
| Q2 | Job model | Fiber work-stealing vs thread pool | **Fiber work-stealing** (master plan B1.5) |
| Q3 | Max worker policy | Fixed cap vs auto | **Auto:** `cores - 2`, cap 16, min 1 |
| Q4 | Editor process model | In-process PIE vs out-of-process | **In-process** (one program) |
| Q5 | Hard realtime | Fixed tick vs best-effort | **Best-effort** variable dt; optional fixed sim step for networking only |
| Q6 | Render thread (v1) | Same thread vs async | **Same thread** record+present until Track B RHI |
| Q7 | TSan in CI | Nightly vs per-PR | **Nightly** from U3; ASan per-PR |
| Q8 | Determinism for MP | Lockstep vs async | **Defer** — single-player determinism mode first |

See also [work-plan.md](./work-plan.md) for scheduling.

---

## 13. Related documents

| Doc | Link |
|-----|------|
| Evidence inventory | [concurrency-inventory.md](./concurrency-inventory.md) |
| 2D→3D merge | [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) |
| Layout | [unified-layout.md](./unified-layout.md) |
| Risks | [risk-register.md](./risk-register.md) (R14, R16, R17) |
| Work packages | [work-plan.md](./work-plan.md) |
