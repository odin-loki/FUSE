# FUSE Unification — Phase U0 Deliverables

**Engine:** FUSE — Fast Unified Simulation Engine  
**Phase:** U0 — Inventory & collision map  
**Date:** 2026-09-15  
**Plan:** [FUSE_UNIFIED_PRESTARTER.md](../plans/FUSE_UNIFIED_PRESTARTER.md) §5

This directory contains evidence-based inventory and collision analysis for merging Torque3D (repo root), Torque2D (`third_party/Torque2D`), and seven community addons into **one program**.

**Constraint:** Investigation and documentation only — no addon `Engine/` merges into FUSE `Engine/` in this phase.

**Stakeholder direction (2026-09-15):**
- **2D→3D extension merge** for scene/object identity — [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md)
- **Multi-threading first-class** — fiber job spine, game-thread mutation, handle-based cross-thread rules — [architecture-parallel.md](./architecture-parallel.md)
- **Desktop + mobile from start** (locked) — adaptive worker count, background pool reduction, portable GLES/Metal/Vulkan threading rules; Emscripten deferred

Physics, gfx, and net remain composition/dual-backend. No physical `Engine/` + T2D source marriage.

---

## Documents

| Document | Description |
|----------|-------------|
| [symbol-collision-report.md](./symbol-collision-report.md) | Duplicate globals/classes across T3D vs T2D — console, platform, math, util, gui, sim, and more |
| [subsystem-matrix.md](./subsystem-matrix.md) | Per-subsystem T3D vs T2D comparison with Merge / Keep dual / Replace with FUSE recommendations |
| [addon-ore-catalog.md](./addon-ore-catalog.md) | All seven addons: patches vs scripts vs art; licenses; kernel entry points |
| [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) | Frozen minimum U8 demo set mapped to legacy missions and T2D toybox |
| [risk-register.md](./risk-register.md) | Box2D vs T3D physics, Gui, net, symbols, dual VMs, mobile/web, addon ore, … |
| [unified-layout.md](./unified-layout.md) | Target monorepo tree, migration map (today→target), U0–U2 stay-put policy, naming conventions |
| [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) | **Stakeholder:** selective 2D→3D inheritance merge; composition boundaries; U2–U4 sequencing |
| [concurrency-inventory.md](./concurrency-inventory.md) | Datamine: T3D ThreadPool, T2D single-thread loop, frame assumptions (paths, counts) |
| [architecture-parallel.md](./architecture-parallel.md) | **MT spine:** process model, job system, frame pipeline, editor threading, safety |
| [work-plan.md](./work-plan.md) | Ordered WPs U1–U8 + parallel workstreams; immediate next 5 actions |
| [vfs-mount-plan.md](./vfs-mount-plan.md) | WP-04 VFS mount prefixes (`/game`, `/t3d`, `/t2d`) |
| [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md) | U4 hybrid composer, worlds, software demo |
| [U5-MODULES.md](./U5-MODULES.md) | U5 feature module scaffolds, `fuse_ai` vertical slice, ore backlog |
| [TRACK-B-AI.md](./TRACK-B-AI.md) | Track B / U5 `fuse_ai` BT registry, parallel composite, runtime threading |
| [TRACK-B-FX.md](./TRACK-B-FX.md) | Track B / U5 `fuse_fx` effect graph tick, parameter bind, AFX vertical slice |
| [U6-EDITOR.md](./U6-EDITOR.md) | U6 Qt 6 editor shell, command queue, desktop-only policy |
| [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) | U7 `project.json` schema, importer stubs, `fuse_import` CLI |

**Related (not U0):**
- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) — Track A/B port (consult for alignment only)
- [third_party/addons/README.md](../../third_party/addons/README.md) — submodule acquisition map

**U1 deliverable:** [BUILD.md](./BUILD.md) (umbrella CMake + `fuse_core` stub) — ✅ landed in U1 PR.  
**U2 deliverable:** [U2-SMOKE.md](./U2-SMOKE.md) (quarantine libs + `fuse_runtime_smoke`) — ✅ U2 PR.

**WP-03–05 (in progress):** Cooperative POSIX fiber wait, shared services stubs (logger/handle/allocator/VFS), greenfield scene hierarchy — see [wp03-fiber-remaining.md](./wp03-fiber-remaining.md). U2 full Engine init blockers unchanged ([U2-SMOKE.md §3](./U2-SMOKE.md#3-honest-blockers--full-dual-legacy-engine-init)).

**WP-06 / U4 (scaffolding):** Dimension APIs + hybrid frame — [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md). `demo_hybrid_hud` runs 3D clear + spinning 2D sprite via software placeholder renderer (no real GL yet).

**WP-09 / U7 (minimal):** `fuse_project` loads versioned `project.json`; T3D/T2D importer stubs + `fuse_import` dry-run CLI — [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md).

**WP-08 / U6 (minimal):** Qt 6 `fuse_editor` desktop shell (conditional on Qt6) + Qt-free `fuse_editor_api` / `EditorHost` — [U6-EDITOR.md](./U6-EDITOR.md).

**WP-10 / U8 (minimum set):** Seven parity demos under `Samples/unification/` with headless binaries (`demo_3d_empty`, `demo_2d_sprites`, …) — see [Samples/unification/README.md](../../Samples/unification/README.md).

---

## Gate U0 checklist

| Item | Status |
|------|--------|
| Collision report published under `docs/unification/` | ✅ [symbol-collision-report.md](./symbol-collision-report.md) |
| Subsystem matrix published | ✅ [subsystem-matrix.md](./subsystem-matrix.md) |
| Stakeholder direction: 2D→3D extension merge (sim/objects) | ✅ [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) |
| Remaining sign-off: script host, physics end-state, 2D renderer | ⏳ **Pending review** |
| Addon ore catalog lists kernel entry points for all seven addons | ✅ [addon-ore-catalog.md](./addon-ore-catalog.md) |
| Parity demo list frozen (minimum set) | ✅ [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) |
| Risk register published | ✅ [risk-register.md](./risk-register.md) |
| Unified file layout proposed | ✅ [unified-layout.md](./unified-layout.md) |
| Concurrency inventory (evidence) | ✅ [concurrency-inventory.md](./concurrency-inventory.md) |
| Parallel architecture proposed | ✅ [architecture-parallel.md](./architecture-parallel.md) |
| Work plan / backlog | ✅ [work-plan.md](./work-plan.md) |
| Umbrella build guide (U1) | ✅ [BUILD.md](./BUILD.md) |

**U0 exit:** Documentation complete; stakeholder review of matrix recommendations and decision gates (script host, physics, 2D renderer, multiprocess policy) before U1 umbrella CMake.

---

## Key findings (executive)

1. **Cannot link raw T3D + T2D** — 237 filename collisions, 311+ class collisions, 33 `Con::` API overlaps. U2 requires prefixed static libraries.
2. **Merge strategy:** lift shared DNA into `fuse::Object` → `SceneObject2D` → `SceneObject3D`; physics/gfx/net stay composition-only.
3. **Gui and console are the hottest collision domains** — 45+ Gui* classes; identical `SimObject` / `ConsoleObject` hierarchies (addressed by greenfield hierarchy, not dual link).
4. **Physics and gfx stay dual (composed)** — Box2D behind `World2D`; T3D collision behind `World3D`; render refs on scene objects — no inheritance across backends.
5. **Addons are mostly content** — kernels are small (BadBehaviour 74 files, Verve 163, GMK component 26); AFX/Verve already in FUSE root `Engine/source/`.
6. **UAISK is scripts-only** — template pack for `fuse_ai`, no C++ ore.
7. **Legacy is main-thread-first** — T3D `ThreadPool` (132 refs) with main-only global submit; T2D has no pool; FUSE needs fiber job spine (master plan B1.5).

---

## Stakeholder decisions (locked)

Platform scope, adaptive workers, mobile GFX threading, background lifecycle — [architecture-parallel.md §12](./architecture-parallel.md#12-stakeholder-decisions-locked--remaining-questions). Remaining optional: editor in-process, MP determinism.

---

## Submodule SHAs (inventory evidence)

| Submodule | Path | Commit (at inventory) |
|-----------|------|------------------------|
| Torque2D | `third_party/Torque2D` | `e7b0011a913793fabb5e564549b3d3ddb3913fe9` |
| GMK | `third_party/addons/GMK` | `e322f148ee2e5fe15cf572472b3644325b4f9e44` |
| Verve | `third_party/addons/Verve` | `0ea77b28767b665b40ddee0909a74f6bc06cebd3` |
| BadBehaviour | `third_party/addons/BadBehaviour` | `9fd487314d4636f7e91834acb2994aa56a63f8a7` |
| GuideBot | `third_party/addons/GuideBot` | `0e1e4230f5aa3f32690476d94f1cdfd3b1bf012e` |
| UAISK | `third_party/addons/UAISK` | `c8224f7684de68aaa143b2d4252dba3c5b983861` |
| AFX-Template | `third_party/addons/AFX-Template` | `1907e55b7aac8e0e239f820ec14ad4367ca7430b` |
| 3DAAK | `third_party/addons/3DAAK` | `8684cd1a65084e52f76131d74520c2fa9e1904a1` |

---

## Next phase

**U1 — Umbrella build:** Root `CMakeLists.txt` as FUSE umbrella; `docs/unification/BUILD.md`; CI configure both legacy targets from one graph. See prestarter §6.
