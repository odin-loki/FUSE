# FUSE U0 — Unified File Layout (Target Monorepo)

**Phase:** U0 Inventory & collision map  
**Date:** 2026-09-15  
**Status:** Proposed target — **documentation only** in U0; no mass directory moves in this PR  
**Aligns with:** [FUSE_UNIFIED_PRESTARTER.md](../plans/FUSE_UNIFIED_PRESTARTER.md) §3 (layer cake L0–L4), §3.3 (symbol law), §16 (repo hygiene)

---

## 1. Purpose

This document proposes the **end-state monorepo layout** for the one-program FUSE product: one editor, one runtime, 2D/3D/hybrid worlds, and feature modules mined from addon ore.

**Hard rules (carry forward):**

- **Do not merge** addon `Engine/` trees into FUSE `Engine/` ([addon-ore-catalog.md](./addon-ore-catalog.md), prestarter §2.2).
- **Do not** big-bang move `Engine/` before U1 umbrella CMake exists — current T3D `generateProjects` / root `CMakeLists.txt` must keep working.
- **Do not** physically marry `Engine/source` and `third_party/Torque2D/engine/source` — strangler only ([merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md)).
- New product code lives under `fuse::` namespaces and `Source/FUSE/` CMake targets; legacy stays quarantined until stranglers finish.
- **Inheritance direction (stakeholder):** `fuse::SceneObject3D : SceneObject2D` — 3D extends 2D for scene identity; physics/gfx/net use **composition**, not inheritance.

---

## 2. Layer cake → directory mapping

| Layer | Prestaster name | Target directory | CMake target (typical) |
|-------|-----------------|------------------|------------------------|
| **L0** | FUSE Core | `Source/FUSE/Core/` | `fuse_core` |
| **L1** | Shared services | `Source/FUSE/Services/` | `fuse_services` (or split: `fuse_assets`, `fuse_audio`, …) |
| **L2a** | Dimension: 3D | `Source/FUSE/Legacy/T3D/` → later `Source/FUSE/World3D/` | `fuse_t3d_legacy` → `fuse_world3d` |
| **L2b** | Dimension: 2D | `Source/FUSE/Legacy/T2D/` → later `Source/FUSE/World2D/` | `fuse_t2d_legacy` → `fuse_world2d` |
| **L3** | Feature modules | `Source/FUSE/Modules/{ai,fx,cinematics,mechanics,adventure}/` | `fuse_ai`, `fuse_fx`, … |
| **L4** | Tools | `Source/FUSE/Editor/`, `Tools/FUSE/` | `fuse_editor`, `fuse_tools` |
| **Apps** | Shipped binaries | `Source/FUSE/Apps/` | `fuse_runtime`, `fuse_runtime_smoke` |
| **Compat** | Old content VMs | `Source/FUSE/Compat/` | `fuse_compat_ts_t3d`, `fuse_compat_ts_t2d` |

---

## 3. Target tree (end-state)

Legend: `(new)` = greenfield FUSE code; `(legacy)` = quarantined strangler; `(ore)` = read-only submodule reference; `(today)` = exists now, migrates later.

```
FUSE/                                    # repo root (today)
├── CMakeLists.txt                       # (today) → U1: FUSE umbrella entry
├── vcpkg.json                           # (today) → U1: merged manifest + T2D deps doc
├── LICENSE.md
├── README.md
├── THIRD_PARTY.md                       # (new, U5+) consolidated notices
│
├── Source/                              # (new, U1+) all compileable product source
│   └── FUSE/
│       ├── CMakeLists.txt               # umbrella: options FUSE_BUILD_*
│       │
│       ├── Core/                        # L0 — fuse_core
│       │   ├── include/fuse/           # public headers: types, handles, allocators, math, log
│       │   │   ├── object.h            # fuse::Object — shared root (replaces dual SimObject over time)
│       │   │   └── jobs/               # JobScheduler, JobCounter, parallel_for — MT spine
│       │   └── src/
│       │
│       ├── Services/                    # L1 — shared services (strangle from U3)
│       │   ├── Assets/
│       │   ├── Audio/
│       │   ├── Input/
│       │   ├── Net/                     # session facade; legacy stacks underneath
│       │   ├── Script/                  # FUSE script host (U3+)
│       │   ├── IO/                      # VFS mounts: /t3d/, /t2d/, /game/
│       │   └── Reflection/
│       │
│       ├── Legacy/                      # U2 quarantine — prefixed static libs
│       │   ├── T3D/                     # fuse_t3d_legacy — NOT a copy of all Engine/
│       │   │   ├── CMakeLists.txt       # wraps Engine/source via add_subdirectory or OBJECT lib
│       │   │   └── README.md            # prefix map, symbol rename policy
│       │   └── T2D/                     # fuse_t2d_legacy — wraps third_party/Torque2D/engine
│       │       ├── CMakeLists.txt
│       │       └── README.md
│       │
│       ├── World2D/                     # L2b strangler home (U3–U4) — 2D base hierarchy
│       │   ├── include/fuse/world2d/
│       │   │   ├── scene_object_2d.h   # fuse::SceneObject2D : Object
│       │   │   ├── world_2d.h          # fuse::World2D (composes Box2D — no inheritance)
│       │   │   └── camera_2d.h
│       │   └── src/
│       │
│       ├── World3D/                     # L2a strangler home (U4+) — 3D extends 2D
│       │   ├── include/fuse/world3d/
│       │   │   ├── scene_object_3d.h   # fuse::SceneObject3D : SceneObject2D
│       │   │   ├── world_3d.h          # fuse::World3D (composes T3D collision — no inheritance)
│       │   │   └── camera_3d.h         # fuse::Camera3D : SceneObject3D (or Camera2D chain)
│       │   └── src/
│       │
│       ├── Hybrid/                      # U4 compositor
│       │   └── Composer/
│       │
│       ├── Modules/                     # L3 — feature modules (U5)
│       │   ├── ai/                      # fuse_ai — BadBehaviour, GuideBot, UAISK ore
│       │   ├── fx/                      # fuse_fx — AFX ore
│       │   ├── cinematics/              # fuse_cinematics — Verve, GMK cutscene overlap
│       │   ├── mechanics/               # fuse_mechanics — GMK component ore
│       │   └── adventure/               # fuse_adventure — 3DAAK scripts/systems
│       │
│       ├── Compat/                      # U0–U2 dual script VMs
│       │   ├── ts_t3d/
│       │   └── ts_t2d/
│       │
│       ├── Apps/
│       │   ├── Runtime/                 # fuse_runtime main
│       │   └── RuntimeSmoke/            # fuse_runtime_smoke (U2 gate)
│       │
│       └── Editor/                      # L4 — Qt 6 (U6)
│           ├── include/fuse/editor/     # Editor API — no Qt in Core
│           └── src/
│
├── Engine/                              # (today) T3D engine — STAYS until L2a strangler complete
│   ├── source/                          # 2,513 files — afx/, Verve/ already here
│   ├── lib/                             # vendored deps (bullet, assimp, …)
│   ├── modules/
│   └── bin/
│
├── Tools/
│   ├── CMake/                           # (today) T3D torque_macros — U1: called from umbrella
│   ├── FUSE/                            # (new, U1+) cookers, converters, CLI
│   │   ├── Cook/
│   │   ├── Convert/                     # U7: .mis → .fuselevel, T2D module → world2d
│   │   └── CMakeLists.txt
│   ├── dae2dts/                         # (today) legacy asset tools — keep path or alias
│   ├── map2dif/
│   └── Vagrant/
│
├── Templates/                           # (today) T3D project templates — U7: FUSE project templates
│   ├── BaseGame/                        # → reference for importers
│   └── FUSE/                            # (new, U7) *.fuseproj templates
│
├── My Projects/                         # (today) per-developer T3D apps — unchanged short-term
│   └── <AppName>/                       # TORQUE_APP_NAME CMake still valid U0–U2
│
├── Samples/
│   ├── unification/                     # (today, U0) U8 parity demo placeholders
│   │   ├── README.md
│   │   ├── demo_3d_empty/               # (new, U8) stub per demo
│   │   ├── demo_2d_sprites/
│   │   └── …
│   ├── T3D/                             # (new, optional) golden missions symlink/copy
│   ├── T2D/                             # (new, optional) toybox references
│   └── Modules/                         # extracted addon sample content (U5+)
│       ├── ai/
│       ├── fx/
│       └── …
│
├── docs/
│   ├── plans/                           # (today) FUSE_MASTER_PLAN, prestarter
│   └── unification/                     # (today) U0 deliverables
│
└── third_party/
    ├── Torque2D/                        # (ore) submodule — engine/source strangulated, not copied into Engine/
    │   ├── engine/
    │   ├── editor/                      # legacy T2D editor — parity only, not product UI
    │   ├── toybox/
    │   └── tutorials/
    ├── addons/                          # (ore) read-only after U5 extraction
    │   ├── GMK/
    │   ├── Verve/
    │   ├── BadBehaviour/
    │   ├── GuideBot/
    │   ├── UAISK/
    │   ├── AFX-Template/
    │   ├── 3DAAK/
    │   └── README.md
    └── vendored/                        # (new, optional U3+) non-submodule deps if split from Engine/lib
```

---

## 4. Migration map (today → target)

### 4.1 Root & build

| Today | Target | When | Notes |
|-------|--------|------|-------|
| `CMakeLists.txt` (T3D app-name driven) | Same file becomes **umbrella** + defers to `Source/FUSE/CMakeLists.txt` | **U1** | Keep `TORQUE_APP_NAME` path working until `fuse_runtime` ships |
| `vcpkg.json` | Merged / documented dual manifest | **U1** | T2D deps in `docs/unification/BUILD.md` |
| `Tools/CMake/` | `Tools/CMake/` (unchanged) + consumed by umbrella | **U1** | No move — include paths stay stable |
| `.gitmodules` | Unchanged; addons remain submodules | **U0–U8** | Pin tags post-U5; optional archive |

### 4.2 Engine & dimensions

| Today | Target | When | Notes |
|-------|--------|------|-------|
| `Engine/source/` (T3D) | **Stays in place**; wrapped by `Source/FUSE/Legacy/T3D/` | **U1–U2** wrap only | No file move — CMake `add_subdirectory` or OBJECT library |
| `Engine/source/afx/` | `Source/FUSE/Modules/fx/` (extract) | **U5** | Already in root Engine — refactor, not re-merge from addon |
| `Engine/source/Verve/` | `Source/FUSE/Modules/cinematics/` | **U5** | Same |
| `third_party/Torque2D/engine/source/` | Wrapped by `Source/FUSE/Legacy/T2D/` | **U1–U2** | Submodule stays; **never** copy into `Engine/` |
| `third_party/Torque2D/editor/` | Unchanged (ore); optional `Samples/T2D/editor-parity/` doc | **U6+** | Not product UI |
| `Engine/lib/` | `Engine/lib/` short-term; optional `third_party/vendored/` long-term | **U3+** | Large vendored trees — move only with care |

### 4.3 Tools, templates, projects

| Today | Target | When | Notes |
|-------|--------|------|-------|
| `Tools/dae2dts`, `map2dif` | `Tools/` or `Tools/FUSE/Legacy/` | **U4+** | Keep working paths in U1 |
| `Tools/FUSE/Cook/` | New cookers | **U7** | DTS/DIFF/T2D asset pipelines |
| `Tools/FUSE/Convert/` | Importers | **U7** | mission/module → FUSE world |
| `Templates/BaseGame/` | `Templates/BaseGame/` + `Templates/FUSE/` | **U7** | Old templates = import sources |
| `My Projects/<App>/` | `My Projects/` + future `Projects/` for `.fuseproj` | **U7–U8** | CMake `TORQUE_APP_NAME` until fuse project format lands |

### 4.4 Addons (ore — no Engine merge)

| Today | Target | When | Notes |
|-------|--------|------|-------|
| `third_party/addons/GMK/Engine/source/component/` | `Source/FUSE/Modules/mechanics/` | **U5** | Extract ~26 files; discard bundled Engine |
| `third_party/addons/Verve/Engine/source/Verve/` | `Source/FUSE/Modules/cinematics/` | **U5** | Root copy takes precedence today |
| `third_party/addons/BadBehaviour/.../BadBehavior/` | `Source/FUSE/Modules/ai/` | **U5** | ~74 C++ files |
| `third_party/addons/GuideBot/guideBotT3D/engine/` | `Source/FUSE/Modules/ai/guidebot/` | **U5** | Check custom license |
| `third_party/addons/UAISK/.../UAISK/*.cs` | `Samples/Modules/ai/uaisk-templates/` | **U5** | Scripts only |
| `third_party/addons/AFX-Template/game/` | `Samples/Modules/fx/` | **U5** | Engine already in `Engine/source/afx/` |
| `third_party/addons/3DAAK/Templates/.../scripts/` | `Source/FUSE/Modules/adventure/` + samples | **U5** | Script systems, not full Engine |

### 4.5 Docs & samples

| Today | Target | When | Notes |
|-------|--------|------|-------|
| `docs/plans/` | `docs/plans/` | **Now** | Unchanged |
| `docs/unification/` | `docs/unification/` | **U0** | This deliverable set |
| `Samples/unification/README.md` | Per-demo subdirs under `Samples/unification/` | **U8** | Placeholders only until converters exist |
| `third_party/Torque2D/toybox/` | Referenced by `Samples/T2D/` or docs | **U0–U8** | No move required — path-stable references |

---

## 5. Phasing: what stays put vs what moves

### 5.1 U0–U2 (current gate band) — **minimal physical moves**

| Path | Policy |
|------|--------|
| `Engine/` | **Stay** — T3D generate/build unchanged |
| `Tools/CMake/` | **Stay** |
| `Templates/`, `My Projects/` | **Stay** |
| `third_party/Torque2D/` | **Stay** (submodule) |
| `third_party/addons/*` | **Stay** (ore, read-only) |
| `docs/unification/` | **Add** documentation only |
| `Samples/unification/README.md` | **Add** placeholder (done) |
| `Source/FUSE/` | **Optional stub** — U1 may add `Source/FUSE/Core/` empty + CMake only; **not required in U0 PR** |

**U1 may add without breaking T3D:**

- `Source/FUSE/CMakeLists.txt` (options, no default target change)
- `Source/FUSE/Legacy/T2D/CMakeLists.txt` (imported target)
- `docs/unification/BUILD.md`

**U2 may add:**

- `Source/FUSE/Apps/RuntimeSmoke/`
- `Source/FUSE/Legacy/T3D/`, `Legacy/T2D/` wrapper CMake

### 5.2 After U2 gate — incremental strangler moves

| Gate | Allowed moves |
|------|----------------|
| **U3** | New code under `Source/FUSE/Core/`, `Services/`; facades call legacy |
| **U4** | `World3D/`, `World2D/`, `Hybrid/` headers; no bulk `Engine/source` relocation |
| **U5** | `Modules/*` populated from addon ore; submodules pinned read-only |
| **U6** | `Source/FUSE/Editor/` Qt tree |
| **U7** | `Templates/FUSE/`, `Tools/FUSE/Convert/`, `Projects/` format |
| **U8** | `Samples/unification/demo_*` full stubs |

### 5.3 End-state deprecation (post–U8 exit)

| Deprecated | Replaced by |
|------------|-------------|
| Separate T3D EXE workflow as *product* | `fuse_runtime` + `fuse_editor` |
| `third_party/addons/*/Engine/` as compile input | `Source/FUSE/Modules/*` |
| Dual CMake generate scripts (undocumented) | Single umbrella configure |
| T2D stock editor as product UI | Qt `fuse_editor` |

---

## 6. Naming conventions

### 6.1 C++ namespaces

| Namespace | Use |
|-----------|-----|
| `fuse::` | All new FUSE product code |
| `fuse::core::` | L0 types, allocators, handles, math, log |
| `fuse::services::` | L1 assets, audio, input, vfs, script host |
| `fuse::world2d::` | L2b public dimension API — `SceneObject2D`, `World2D`, `Camera2D` |
| `fuse::world3d::` | L2a public dimension API — `SceneObject3D : SceneObject2D`, `World3D`, `Camera3D` |
| `fuse::modules::ai::` etc. | L3 feature modules |
| `fuse::editor::` | L4 editor API (Qt-free headers) |
| `fuse::legacy::t3d::` | Thin adapters over quarantined T3D |
| `fuse::legacy::t2d::` | Thin adapters over quarantined T2D |

### 6.2 CMake targets & compile units

| Target | Contents |
|--------|----------|
| `fuse_core` | L0 static lib |
| `fuse_services` | L1 (may split later) |
| `fuse_t3d_legacy` | Wrapped `Engine/source` with symbol prefix |
| `fuse_t2d_legacy` | Wrapped `third_party/Torque2D/engine/source` with prefix |
| `fuse_world3d`, `fuse_world2d` | Strangler replacements (grow over U4+) |
| `fuse_ai`, `fuse_fx`, `fuse_cinematics`, `fuse_mechanics`, `fuse_adventure` | L3 modules |
| `fuse_runtime` | Ship player |
| `fuse_editor` | Qt 6 shell |
| `fuse_tools` | CLI cookers |
| `fuse_compat_ts_t3d`, `fuse_compat_ts_t2d` | Dual script VMs (U0–U2) |

### 6.3 Directories & samples

| Pattern | Meaning |
|---------|---------|
| `Source/FUSE/` | PascalCase `FUSE` segment matches CMake `fuse_*` targets |
| `Samples/unification/demo_<name>/` | U8 parity demo; see [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) |
| `*.fuseproj` / `project.json` | FUSE unified project (U7) |
| `worlds/*.3d.fuselevel`, `*.2d.fuselevel` | Converted worlds |
| `third_party/addons/` | **Ore** — reference only after U5 |
| `third_party/vendored/` | Explicit non-submodule third-party (optional) |
| `Engine/` | Legacy T3D tree — shrinking via strangler, not deleted abruptly |

### 6.4 Symbol prefix law (link time)

When `fuse_t3d_legacy` and `fuse_t2d_legacy` link together (U2+):

- C linkage / globals: `fuse_t3d_`, `fuse_t2d_` prefix scripts  
- Do **not** expose raw `Con::`, `Platform::`, or `StringTable` from both libs without rename ([symbol-collision-report.md](./symbol-collision-report.md))

---

## 7. `third_party`: ore vs vendored deps

| Location | Role | Compile? | Lifecycle |
|----------|------|----------|-----------|
| `third_party/Torque2D/` | Full T2D upstream (submodule) | Via `fuse_t2d_legacy` wrap only | Until L2b strangler done; then archive |
| `third_party/addons/*` | Community kits (submodule) | **No** after U5 | Read-only ore → extract to `Source/FUSE/Modules/` |
| `Engine/lib/` | T3D-bundled deps (bullet, assimp, …) | Yes (today) | Gradual move to `third_party/vendored/` if needed |
| `third_party/vendored/` (optional) | Shared vcpkg-adjacent pins | Yes | U3+ hygiene |

---

## 8. FUSE project layout (runtime content, U7)

Player-facing projects — distinct from repo layout:

```
MyGame/
  project.json              # dimensions, enabled modules
  assets/
  worlds/
    hub.3d.fuselevel
    side.2d.fuselevel
  scripts/
  compat/                   # optional raw .mis / T2D module mounts
```

Repo ships **templates** under `Templates/FUSE/` and **parity demos** under `Samples/unification/`.

---

## 9. Alignment checklist (prestarter)

| Prestaster rule | Layout enforcement |
|-----------------|-------------------|
| L0–L4 layer cake | `Source/FUSE/{Core,Services,World2D,World3D,Modules,Editor}` |
| 2D→3D extension merge | `World2D/Scene/` base; `World3D/Scene/` extends 2D — see [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) |
| No addon Engine → Engine merge | Addons stay in `third_party/addons/`; extract to `Modules/` |
| No physical Engine+T2D source marry | Legacy wrapped in `Legacy/` only |
| `fuse::` for new code | `include/fuse/` public headers |
| Legacy quarantine | `Legacy/T3D`, `Legacy/T2D` CMake wraps — no path merge |
| One program binaries | `Apps/Runtime`, `Editor/` |
| Submodule = acquisition | `third_party/`; unified program = `Source/FUSE/` |

---

## 10. Gate U0 checklist item

- [x] Unified layout proposed in `docs/unification/unified-layout.md`
- [x] Migration map from current paths documented
- [x] U0–U2 stay-put policy explicit
- [ ] Physical `Source/FUSE/` tree — **U1** (optional stubs only if umbrella needs them)

**Parallel architecture:** [architecture-parallel.md](./architecture-parallel.md) — game thread + fiber workers; `Core/jobs/` is first-class layout.

**This PR:** documentation only — no `Engine/` moves, no addon merges.
