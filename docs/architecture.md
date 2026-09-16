# Architecture

FUSE is one process, one scene identity, and one job spine. 2D and 3D are dimensions of the same engine, not two programs.

Tagline: *Exact where it matters. Parallel everywhere else.*

## Binaries

| Binary | Role |
|--------|------|
| `fuse_runtime` | Game / player — one OS process |
| `fuse_editor` | Qt 6 chrome in-process with the runtime (desktop) |
| `fuse_tools` | CLI cookers and importers; optional workers, no GPU required |

Mobile ships **runtime only**. The editor is a desktop product.

## Layers

```
L4  Editor / tools     fuse_editor, fuse_editor_api, Tools/FUSE
L3  Feature modules    fuse_ai, fuse_fx, fuse_cinematics, fuse_mechanics, fuse_adventure
L2  Dimensions         World2D, World3D, Hybrid composer
L1  Shared services    assets, audio, script, net, VFS, project
L0  Core               types, handles, allocators, math, jobs, platform, log
```

New product code lives under `Source/FUSE/` in namespace `fuse`.

## Object identity

`fuse::Object` is the shared root. 3D scene nodes extend 2D scene nodes:

```
fuse::Object
 └── SceneObject2D
      └── SceneObject3D
```

Physics, graphics, and networking are **composed** behind world facades (`World2D`, `World3D`). They are not inherited up the scene tree.

A hybrid frame can run a 3D world and a 2D HUD (or the reverse) through the compositor in `Source/FUSE/Hybrid`.

## Threading

| Role | Count | Does | Must not |
|------|-------|------|----------|
| Game thread | 1 | Input, `fuse::Object` mutation, script callbacks, render record + present (v1) | Block on mutex in the hot path |
| Job workers | Adaptive `N` | Fiber work-stealing, `parallel_for`, decode, AI/FX read phases | Hold raw `Object*` across jobs |
| I/O | 0–2 | Blocking read / decompress, budgeted per frame | Publish handles without a game-thread commit |
| Audio | 1 | Device feed | Touch the scene graph |
| Qt UI | 1 (editor) | Widgets and inspectors | Dereference scene pointers — post `EditorCommand`s |

Cross-thread traffic uses `fuse::Handle<T>` (generation-checked) and immutable snapshots. Workers produce command buffers; the game thread commits them.

### Adaptive workers

`N = clamp(usable_cores - reserve, min, max)` with desktop and mobile profiles. See `fuse/jobs/worker_count.hpp`. `FUSE_JOBS_SINGLE_THREAD=ON` forces `N = 0` so every job runs on the caller — required for replay and golden tests.

## Memory

Public APIs do not own with raw pointers. Long-lived objects use `Handle` / `HandleMap`. Domains allocate through the hierarchy in `fuse/alloc/` (frame, pool, stack, stats). Debug intercepts are intended to ban `new` / `delete` / `malloc` in engine libraries once the allocator gate is green.

Errors are `std::expected` / status codes. Exceptions do not cross engine ↔ script ↔ Qt. Qt may throw only at the UI edge.

## Frame

A typical runtime tick:

1. Pump input (game thread)
2. Tick worlds and modules; submit jobs
3. Wait on job counters at defined barriers (`fuse/frame/`)
4. Record rendering; present
5. Drain editor commands if the Qt shell is attached

`demo_hybrid_hud` proves the compositor with a software placeholder renderer. Real GL/Vulkan present is Track B.

## Platform

`fuse::platform` owns window, threads, fibers, power, lifecycle, and crash reporting. Gameplay code stays platform-neutral. Desktop + mobile are in scope from day one; web is deferred.

More detail: [`unification/architecture-parallel.md`](unification/architecture-parallel.md) (engineering note).
