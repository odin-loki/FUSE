# FUSE Pre-Starter Plan — Unified Tree, Unified Features, One Program

**Document type:** Pre-starter (before Track A P0 implementation)  
**Engine:** FUSE — Fast Unified Simulation Engine  
**Repo:** https://github.com/odin-loki/FUSE (`main`)  
**Date:** 2026-09-14  
**Status:** Planning — comprehensive  

---

## 0. What you mean (and what this plan delivers)

**Today:** One *git* monorepo. Three *product* realities:

| Piece | Where it lives | What it actually is |
|-------|----------------|---------------------|
| Torque3D (community) | Repo root (`Engine/`, CMake, Templates…) | Full 3D engine + tools |
| Torque2D (community) | `third_party/Torque2D/` (submodule) | Separate 2D engine (own CMake, `engine/source/2d`, Box2D, own editor) |
| Addons | `third_party/addons/*` (submodules) | T3D-era kits: GMK, Verve, BadBehaviour, GuideBot, UAISK, AFX, 3DAAK — mostly whole trees or project packs, not clean plugins |

**Goal:** **One program.** One process. One editor. One player/runtime. One feature surface where:

- 2D and 3D are **modes / layers / worlds** of the same engine, not two EXEs  
- Addon capabilities (AI, FX, cutscenes, adventure scaffolding, mechanics) are **first-class FUSE features**, not optional zip merges into a Torque project  
- Build, package, and ship **one** product named FUSE  

This pre-starter sits **in front of** (and feeds) the full [FUSE_MASTER_PLAN.md](./FUSE_MASTER_PLAN.md) (C++23, memory-safe, Qt 6, Track A→B). Unification without the port = forever dual CMake and dual Gui. Port without unification = modernise two engines and seven kits separately. **Do both, sequenced.**

---

## 1. North-star product definition

### 1.1 Single binary family

| Target | Role |
|--------|------|
| `fuse_editor` | Qt 6 shell: project hub, 2D/3D/hybrid viewports, inspectors, addon feature panels |
| `fuse_runtime` / game player | Headless of editor chrome; loads FUSE projects (2D, 3D, or hybrid) |
| `fuse_tools` | Cookers, asset converters, DTS/DIFF/T2D asset pipelines (CLI; optional Qt) |

Shipping configs: `debug`, `release`, `profile`, `shipping` (from master plan). No separate “Torque2D.exe” / “Torque3D.exe” for end users.

### 1.2 Unified feature surface (what “all the features” means)

Expose as **FUSE capabilities** (toggleable modules), not as “run this other engine”:

**From T3D lineage**

- 3D scene graph / zones / interiors / terrain / forests  
- Legacy + community rendering path → migrate toward Vulkan (Track B)  
- Networking, TorqueScript (compat), Gui* (runtime HUD only until replaced)  
- Mission/world workflow, datablocks, shapebase, vehicles, weapons patterns  

**From T2D lineage**

- Scene2D, SceneObject2D, sprites, tilemaps, particle 2D, Box2D (or successor) physics  
- 2D camera, layers, sorting, orthographic pipeline  
- T2D asset/module story (`module`, `assets`, bitmap fonts)  
- 2D editor concepts (merged into Qt, not T2D’s separate editor forever)  

**From addons (capability map)**

| Addon | Becomes FUSE feature module(s) |
|-------|--------------------------------|
| **GMK** | Mechanics graph, rigid/ragdoll helpers, cutscene hooks overlapping Verve |
| **Verve** | Timeline / cinematic director, tracks, triggers |
| **BadBehaviour** | Behavior-tree AI runtime + editor graph |
| **GuideBot** | Higher-level action AI / navigation helpers |
| **UAISK** | Starter AI behaviours, perception stubs, kit scripts → FUSE AI templates |
| **AFX** | Spell/FX composer, residual effects, casting pipeline |
| **3DAAK** | Adventure scaffolding (inventory, interactions, puzzles) as optional game template + systems |

**Hybrid features (only possible once unified)**

- 2D UI / HUD over 3D world (and reverse: 3D diorama in 2D shell)  
- Shared input, audio, networking, save/load, scripting API  
- One asset database with dimension tags (`dim:2d`, `dim:3d`, `dim:any`)  
- Behavior trees driving both Sprite and ShapeBase agents  
- AFX / Verve timelines affecting 2D and 3D scenes  
- Single project format that can contain 2D levels + 3D zones  

### 1.3 Non-goals for pre-starter (explicit)

- Not “keep three CMake roots and a launcher that picks an EXE”  
- Not “full FUSE master plan Track B Vulkan/DDGI before unification spine exists”  
- Not promising bit-identical replay of every legacy T2D/T3D demo on day one  
- Not merging Git histories of all submodules into one blob without an extraction plan  

---

## 2. Current-state reality check (why this is hard)

### 2.1 Two engines, shared DNA, divergent flesh

| Concern | T3D (root) | T2D (`third_party/Torque2D`) |
|---------|------------|------------------------------|
| CMake | App-name driven, `Tools/CMake`, C++17 today | Modern target-based CMake 3.21+, C++ (own source lists) |
| Source layout | `Engine/source/...` | `engine/source/{2d,Box2D,console,graphics,...}` |
| Scene model | 3D SimObject / SceneObject / zones | Scene2D + component-ish 2D objects |
| Physics | T3D collision / community options | Box2D in-tree |
| Editor | In-engine Gui* + tools | Separate `editor/` tree |
| Platforms | Desktop-focused community builds | Win/mac/Linux/iOS/Android/Emscripten noted in CMake |

They both speak “Torque” (console, TorqueScript flavour, company history) but **are not linkable as-is into one process** without symbol collisions, duplicate `Platform::`, duplicate console, duplicate math, duplicate Gui.

### 2.2 Addons are not plugins

| Addon | Shape | Implication |
|-------|-------|-------------|
| GMK | Full Engine + My Projects + old Qt4 DLLs | Extract systems; discard bundled Qt4 |
| Verve | Engine + Templates + Tools | Extract cinematic core |
| BadBehaviour | Full T3D-shaped tree | Extract BT runtime + scripts |
| GuideBot | `guideBotT3D` + docs/PDF | Port guidebot module + assets |
| UAISK | Kit folder under `The_Universal_AI_Starter_Kit` | Harvest scripts/datablocks as templates |
| AFX-Template | Project/template style | Harvest AFX engine hooks + art |
| 3DAAK | Full T3D-shaped tree | Harvest adventure systems + content |

Treat addons as **ore**, not as drop-in `.so` modules.

### 2.3 Monorepo ≠ unified program

Submodules solve **acquisition**. This plan solves **architecture and product**.

---

## 3. Target architecture — one program

```
┌──────────────────────────────────────────────────────────────┐
│                     fuse_editor (Qt 6)                         │
│  Project · Hierarchy · Inspectors · Timeline · AI graphs · FX │
└───────────────────────────┬──────────────────────────────────┘
                            │ FUSE Editor API (no Qt in core)
┌───────────────────────────▼──────────────────────────────────┐
│                     fuse_runtime                               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐   │
│  │ World 3D    │  │ World 2D    │  │ Hybrid compositor   │   │
│  │ (from T3D)  │  │ (from T2D)  │  │ (shared frame)      │   │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘   │
│         └────────────┬───┴────────────────────┘               │
│              Shared FUSE Core                                  │
│   types · handles · allocators · jobs · math · log · I/O       │
│   assets · audio · input · net · script host · reflection      │
│   feature modules: AI · FX · Cinematics · Adventure · Mechanics│
└──────────────────────────────────────────────────────────────┘
```

### 3.1 Layer cake

1. **L0 — FUSE Core** (new; Track A foundation): C++23, handles, allocators, jobs, math/GRIA, logging, platform  
2. **L1 — Shared services:** assets, audio, input, networking, serialisation, reflection, scripting host  
3. **L2a — Dimension:3D:** scene, rendering backend, terrain, etc. (strangle T3D)  
4. **L2b — Dimension:2D:** scene2d, sprite/tile, Box2D-or-successor (strangle T2D)  
5. **L3 — Feature modules:** `fuse_ai`, `fuse_fx`, `fuse_cinematics`, `fuse_mechanics`, `fuse_adventure` (ore from addons)  
6. **L4 — Tools:** Qt editor + cookers  

### 3.2 Process and module loading

- **Static modules** for core + dimensions in v1 (simpler, one EXE)  
- Optional later: dynamic feature packs for adventure/AI kits  
- Feature flags: `FUSE_WITH_2D`, `FUSE_WITH_3D`, `FUSE_WITH_AI`, … all **ON** for the unified product build; slim runtimes may strip  

### 3.3 Symbol / namespace law

- Everything new: `fuse::`  
- Legacy T3D during strangler: quarantine under `fuse::legacy::t3d` or compile as `fuse_t3d_legacy` static lib with symbol renaming where required  
- Legacy T2D: `fuse::legacy::t2d` / `fuse_t2d_legacy`  
- **Ban** linking both raw upstreams without renaming — duplicate `Con::`, `Platform::`, `StringTable` will explode  

### 3.4 Scripting unification

| Phase | Approach |
|-------|----------|
| U0–U2 | Dual TorqueScript VMs quarantined (`compat/ts_t3d`, `compat/ts_t2d`) only for loading old content |
| U3+ | One **FUSE script host** (prefer Lua or a single TS dialect — decision gate) calling `fuse::` APIs |
| End state | New content never needs two script dialects; old content runs in compat until cooked forward |

### 3.5 Asset & project unification

**FUSE project** (`*.fuseproj` / directory format):

```
MyGame/
  project.json          # dimensions enabled, modules enabled
  assets/               # unified database (or virtual mounts)
  worlds/
    hub.3d.fuselevel
    side.2d.fuselevel
  scripts/              # FUSE scripts
  compat/               # optional mounts of raw .mis / T2D modules
```

Converters:

- T3D mission/project → FUSE world3d  
- T2D module/project → FUSE world2d  
- Datablocks / AFX / BT assets → FUSE feature assets  

### 3.6 Editor unification (Qt)

Single `fuse_editor`:

- **Viewport modes:** 3D, 2D, Hybrid (2D overlay / dual)  
- **Shared:** hierarchy, inspector (reflection), content browser, console, profiler  
- **Feature panes:** Behavior Tree (BadBehaviour ore), Timeline (Verve), FX Composer (AFX), Mechanics (GMK), Adventure (3DAAK)  
- **No** shipping Torque Gui editor or T2D stock editor as the product UI (legacy may remain for parity testing only)  

---

## 4. Unification strategy — strangler fig

Do **not** big-bang rewrite both engines in one quarter. Extract shared core, wrap dimensions, mine addons.

```
Phase U0  Inventory & collision map          ──► gate
Phase U1  Umbrella build (still multi-lib)   ──► gate
Phase U2  Shared core + dual runtimes linked ──► gate  ("one process" smoke)
Phase U3  Shared services (I/O, assets…)     ──► gate
Phase U4  Dimension APIs + hybrid frame      ──► gate
Phase U5  Feature modules from addon ore     ──► gate
Phase U6  Qt unified editor vertical slice   ──► gate
Phase U7  Project format + converters        ──► gate
Phase U8  Parity demos (2D, 3D, hybrid, AI/FX) ──► UNIFICATION EXIT
         └── hands off to Track A remaining / Track B features
```

Track A (C++23, memory safety) **starts inside U1–U2** for *new* core code; legacy dims stay dirty until stranglers finish. Do not wait for full Track A P7 before U0–U2.

---

## 5. Phase U0 — Inventory & collision map (comprehensive)

**Duration:** ~1–2 weeks AI-assisted  

### 5.1 Deliverables

1. **Symbol collision report** — duplicate globals/classes across T3D `Engine/source` vs T2D `engine/source` (console, platform, math, util, gui, sim)  
2. **Subsystem matrix** — for each of: memory, strings, console/script, sim/objects, input, gfx, audio, net, files, gui, physics, assets  

   | Subsystem | T3D impl | T2D impl | Merge / Keep dual / Replace with FUSE |
   |-----------|----------|----------|----------------------------------------|

3. **Addon ore catalog** — per addon: engine patches vs scripts vs art vs tools; license file noted; estimate “extractable kernel” vs “content pack”  
4. **Demo corpus** — list golden 3D missions, T2D scenes, and one demo per addon feature to become parity targets  
5. **Risk register** — Box2D vs T3D physics; Gui duplication; network model differences; mobile/web from T2D vs desktop T3D  

### 5.2 Gate U0

- [ ] Collision report published under `docs/unification/`  
- [ ] Subsystem matrix reviewed (stakeholder sign-off on Merge/Dual/Replace column)  
- [ ] Addon ore catalog lists kernel entry points for all seven addons  
- [ ] Parity demo list frozen (can grow later, not shrink below minimum)  

---

## 6. Phase U1 — Umbrella build (one repo build graph)

**Duration:** ~1–2 weeks  

### 6.1 Work

- Root `CMakeLists.txt` becomes **FUSE umbrella**:  
  - `add_subdirectory` / imported targets for `fuse_core` (stub), `fuse_t3d_legacy` (existing T3D), `fuse_t2d_legacy` (T2D), tools  
- Stop requiring humans to open two totally separate generate scripts for daily work  
- CMake options: `FUSE_BUILD_T3D`, `FUSE_BUILD_T2D`, `FUSE_BUILD_EDITOR`, `FUSE_BUILD_ADDON_CONTENT`  
- CI job: configure + build umbrella on Linux (and Win when available)  
- Document: `docs/unification/BUILD.md`  

### 6.2 Gate U1

- [ ] One CMake configure builds T3D app target *and* T2D engine target from the FUSE root (even if still two binaries)  
- [ ] `vcpkg` / deps story documented (T3D vcpkg.json vs T2D needs)  
- [ ] Zero “secret” generate steps only known to one tree  

---

## 7. Phase U2 — One process smoke (“the wedding”)

**Duration:** ~2–4 weeks  

### 7.1 Goal

A single executable `fuse_runtime_smoke` that:

1. Initialises **FUSE core** (types, log, allocators stubs OK)  
2. Brings up **legacy T3D** *or* initialises enough of it to create a 3D world worldless clear-colour / empty mission  
3. Brings up **legacy T2D** Scene2D in isolation *or* via renamed symbols  
4. Proves **both can live in one process** without crashing at static init  

Practical tactics (pick based on U0 collision report):

- **Preferred:** compile each legacy engine as a static library with **namespace wrappers / symbol prefix scripts** (`fuse_t3d_`, `fuse_t2d_`) for conflicting C symbols  
- **Fallback:** multiprocess compositor (editor hosts two sandboxed runtimes) — **allowed only as temporary scaffold**, must be scheduled for deletion; does *not* count as unification exit  

### 7.2 Gate U2

- [ ] One process initialises core + both legacy dims (or prefixed libs) under ASan  
- [ ] No duplicate `main` / static init order deadlocks  
- [ ] Documented list of remaining symbol conflicts (trend must be downward)  

**This is the first moment “one program” is real.**

---

## 8. Phase U3 — Shared services extraction

**Duration:** ~3–5 weeks (overlaps Track A P1–P3)  

### 8.1 Extract or replace (priority order)

1. Logging → FUSE logger  
2. File I/O / paths → FUSE VFS mounts (`/t3d/...`, `/t2d/...`, `/game/...`)  
3. Input → FUSE input (game path); editor via Qt  
4. Time / timers / job submission → FUSE jobs  
5. Strings / tables → plan migration off dual StringTables  
6. Audio → single mixer API (backends may remain dual short-term)  
7. Asset identifiers → `fuse::AssetId` handles  

### 8.2 Rule

New code **must not** call raw T3D/T2D platform APIs; only through FUSE facades. Legacy internals may still call their own until stranglers finish.

### 8.3 Gate U3

- [ ] Both dims log through FUSE logger  
- [ ] Both dims read at least one asset via FUSE VFS  
- [ ] Input path documented for editor vs game  

---

## 9. Phase U4 — Dimension APIs & hybrid frame

**Duration:** ~4–6 weeks  

### 9.1 Stable APIs

```cpp
namespace fuse {
  struct FrameCtx; // dt, time, handles

  class IDimension {
  public:
    virtual ~IDimension() = default;
    virtual void tick(FrameCtx&) = 0;
    virtual void render(FrameCtx&) = 0;
    virtual void load_world(Handle<World>) = 0;
  };

  class World3D : public IDimension { /* wraps strangling T3D */ };
  class World2D : public IDimension { /* wraps strangling T2D */ };

  class HybridComposer {
    // order: 3D opaque → 3D transparent → 2D scene → UI
  };
}
```

### 9.2 Hybrid compositor (v1)

- Single swapchain / window  
- 3D renders to colour+depth target  
- 2D renders to colour target (ortho)  
- Compose in deterministic order; shared HDR/tonemap later (Track B)  

### 9.3 Gate U4

- [ ] API headers stable enough for feature modules  
- [ ] Demo: empty 3D clear + spinning 2D sprite in one window  
- [ ] Dimension enable/disable from project flags  

---

## 10. Phase U5 — Feature modules from addon ore (comprehensive)

**Duration:** ~6–10 weeks (parallelisable per module after U4 APIs exist)  

Mine each addon into a **FUSE module** with:

- Runtime library (`fuse_ai`, etc.)  
- Optional editor panel hooks  
- Sample content under `Samples/`  
- Compat notes (what scripts still need TS)  

### 10.1 Module: `fuse_ai` (BadBehaviour + GuideBot + UAISK)

| Source | Extract |
|--------|---------|
| BadBehaviour | BT nodes, blackboard, tick scheduler, editor graph schema |
| GuideBot | Navigation/action helpers, character AI utilities |
| UAISK | Starter trees/behaviours as **templates**, not engine forks |

**Unified feature:** Any agent (2D sprite or 3D shapebase) can run a FUSE BT.  

**Gate:** BT drives a 3D bot *and* a 2D bot in the hybrid smoke demo.

### 10.2 Module: `fuse_cinematics` (Verve + GMK cutscene overlap)

| Source | Extract |
|--------|---------|
| Verve | Timeline, tracks, keys, triggers, director |
| GMK | Overlapping cutscene/mechanics editor ideas — merge, don’t dual-ship |

**Unified feature:** One timeline can animate 2D transforms, 3D transforms, cameras, audio, FX events.  

**Gate:** 30s timeline moves a 3D camera and a 2D sprite, fires an FX event.

### 10.3 Module: `fuse_fx` (AFX)

| Source | Extract |
|--------|---------|
| AFX-Template | Effectron/spell residual patterns, casting pipeline, art hooks |

**Unified feature:** FX system attachable to 2D and 3D sockets; script/API parity.  

**Gate:** One AFX-style effect on a 3D model and a 2D sprite.

### 10.4 Module: `fuse_mechanics` (GMK)

| Source | Extract |
|--------|---------|
| GMK | Mechanics/graph gameplay scaffolding, physics helpers (rebind to FUSE physics later) |

**Discard:** Bundled Qt4 DLLs, Project Manager.exe, duplicate Engine trees once extracted.  

**Gate:** One mechanics-driven interactable in 3D sample; stub binding point for 2D.

### 10.5 Module: `fuse_adventure` (3DAAK)

| Source | Extract |
|--------|---------|
| 3DAAK | Inventory, interaction, adventure flow — as template + systems |

**Unified feature:** Adventure template works in 3D; 2D adventure template shares inventory/interaction interfaces.  

**Gate:** Pick-up / use interaction works in 3D sample; interface tested with a 2D object stub.

### 10.6 Cross-module rules

- Modules depend on **FUSE APIs only**, not on `third_party/addons/...` at compile time once extraction lands  
- After extraction, submodules become **read-only ore** (or archived under `third_party/addons` for reference)  
- Licenses: preserve MIT/third-party notices in `THIRD_PARTY.md`  

---

## 11. Phase U6 — Qt unified editor vertical slice

**Duration:** ~4–6 weeks (aligns with Track A P5 / master plan B6 Qt rewrite)  

### 11.1 Must-have vertical slice

1. Open FUSE project  
2. Create/open World3D and World2D  
3. Hierarchy + inspector for objects in both dims  
4. Play-in-editor (PIE) using `fuse_runtime` logic  
5. One feature pane end-to-end (recommend **Timeline** or **BT** first)  

### 11.2 Gate U6

- [ ] Designer can create hybrid project without CLI dark arts  
- [ ] PIE runs one process with both dims  
- [ ] No dependency on stock T3D Gui editor or stock T2D editor for the slice  

---

## 12. Phase U7 — Project format, converters, content pipeline

**Duration:** ~3–5 weeks  

### 12.1 Work

- Spec `project.json` + world formats (versioned)  
- Importers: T3D mission, T2D project/module  
- Cookers: forward assets into FUSE asset DB  
- Migration guide for community content  

### 12.2 Gate U7

- [ ] Import one T3D sample mission → playable World3D  
- [ ] Import one T2D sample → playable World2D  
- [ ] Round-trip save/load FUSE hybrid project  

---

## 13. Phase U8 — Parity demos & unification exit criteria

**Duration:** ~3–4 weeks  

### 13.1 Required demos (minimum set)

| Demo | Proves |
|------|--------|
| `demo_3d_empty` | 3D dimension path |
| `demo_2d_sprites` | 2D dimension path |
| `demo_hybrid_hud` | Shared frame / compositor |
| `demo_ai_bt` | fuse_ai on both dims |
| `demo_timeline` | fuse_cinematics |
| `demo_fx` | fuse_fx |
| `demo_adventure_stub` | fuse_adventure interactions |

### 13.2 Unification EXIT gate (all must pass)

- [ ] **One** shipped program family (`fuse_editor` + `fuse_runtime`) builds from FUSE root on `main`  
- [ ] Users enable 2D/3D/modules via project flags, not by cloning other repos  
- [ ] Addon capabilities reachable as FUSE features without merging Engine trees by hand  
- [ ] Submodules are reference ore only (or removed after extraction)  
- [ ] ASan clean smoke of hybrid PIE  
- [ ] Docs: player guide + programmer guide for unified API  
- [ ] Stakeholder agrees: “this is one program”  

After EXIT → continue FUSE_MASTER_PLAN Track A remainder / Track B (Vulkan, ECS, physics replacement, etc.) **on the unified spine**, not on raw T3D/T2D forever.

---

## 14. Mapping to FUSE_MASTER_PLAN (Track A / B)

| Unification | Master plan |
|-------------|-------------|
| U0 | Feeds baseline assumptions / risks |
| U1–U2 | Parallel to Track A **P0** (skeleton) — umbrella + core stubs |
| U3 | Track A **P1–P3** (memory, math, jobs, log) |
| U4 | Uses handles/resources (**P4**); renderer still legacy until Track B |
| U5 | Feature work; later rebind to Track B ECS/physics/renderer |
| U6 | Track A **P5** + B6 Qt editor |
| U7–U8 | Compat (**P6**) + unlock Track B properly (**P7**) |

**Hard rule remains:** memory-safe C++23 FUSE core for all *new* code; legacy dims quarantined until replaced.

---

## 15. Sequencing diagram (comprehensive)

```
U0 Inventory
   │
U1 Umbrella CMake ─────────────── Track A P0 skeleton
   │
U2 One-process smoke
   │
U3 Shared services ────────────── Track A P1–P3
   │
U4 Dimension APIs + hybrid frame ─ Track A P4 (handles)
   │
   ├─► U5a fuse_ai
   ├─► U5b fuse_cinematics
   ├─► U5c fuse_fx
   ├─► U5d fuse_mechanics
   └─► U5e fuse_adventure
   │
U6 Qt editor slice ────────────── Track A P5 / B6
   │
U7 Project + converters ───────── Track A P6 compat
   │
U8 Parity demos ───────────────── Track A P7 unlock
   │
   ▼
Track B: Vulkan, ECS, physics, advanced lighting, production…
   (implemented once against fuse:: APIs, both dimensions)
```

---

## 16. Org / repo hygiene during unification

| Item | Policy |
|------|--------|
| `third_party/Torque2D` | Submodule until L2b strangler replaces it; then drop or archive |
| `third_party/addons/*` | Ore; after U5 extraction, pin tags and stop “merging Engine into Engine” |
| Branch | All work on **`main`** (user policy) |
| Upstream sync | `upstream` TorqueGameEngines/Torque3D — merge selectively; avoid clobbering FUSE umbrella |
| Licensing | MIT notices + addon licenses → `THIRD_PARTY.md` |

---

## 17. Risks & mitigations (comprehensive)

| Risk | Impact | Mitigation |
|------|--------|------------|
| Symbol collisions | Can’t link one process | U0 report; prefix/rename; static lib quarantine |
| Infinite dual-maintenance | Never finish | Exit criteria; kill multiprocess fallback on schedule |
| Addon ore too entangled | U5 slips | Kernel-first extraction; content as Samples |
| Qt scope explosion | Editor never ships | U6 vertical slice only; one feature pane |
| Physics mismatch (Box2D vs T3D) | Hybrid gameplay weird | Separate worlds; shared only at interface; Track B may unify later |
| Script dual-VM forever | Complexity tax | Hard deadline for FUSE script host decision in U3 |
| Team tries Track B renderer before U2 | Wasted work on non-unified glue | This pre-starter gates “one process” first |
| Expecting bit-perfect old demos | Morale / schedule | Parity tolerances; “FUSE demos” as primary |

---

## 18. Decision gates (need stakeholder answers during U0–U3)

1. **Script host end-state:** Lua vs single TorqueScript vs other?  
2. **Physics end-state:** keep Box2D for 2D forever vs unify under Track B physics?  
3. **2D renderer:** keep T2D GL path until Vulkan 2D, or early common RHI?  
4. **Multiprocess fallback:** forbidden after U2, or allowed until U6? (Recommendation: **forbidden after U4**)  
5. **Product skew:** ship “FUSE 3D-only” SKU or always unified binary with flags?  

---

## 19. Immediate next actions (start now)

1. Create `docs/unification/` and begin **U0** collision + subsystem matrix  
2. Sketch umbrella `CMakeLists.txt` branch work on `main` (**U1**) without breaking current T3D generate  
3. Pick parity demos and add `Samples/unification/README.md` placeholders  
4. Do **not** start random addon merges into `Engine/`  
5. Keep master plan Track A principles for all new FUSE core files  

---

## 20. Success one-liner

> **FUSE is one program:** one editor, one runtime, worlds that can be 2D, 3D, or both, with AI, FX, cinematics, mechanics, and adventure as modules — Torque3D, Torque2D, and the community kits are ancestry and ore, not separate products.

---

## Appendix A — Capability checklist (unified product)

### Dimensions
- [ ] World3D load/save/play  
- [ ] World2D load/save/play  
- [ ] Hybrid compose in one window  
- [ ] Shared input/audio/net  

### Features
- [ ] Behavior trees (AI)  
- [ ] Guide/action AI helpers  
- [ ] AI starter templates  
- [ ] Cinematic timeline  
- [ ] Mechanics graph/interactables  
- [ ] AFX-style FX  
- [ ] Adventure inventory/interact  

### Tools
- [ ] Qt project hub  
- [ ] Dual-dimension editors  
- [ ] Feature panes (BT, timeline, FX)  
- [ ] Importers (T3D, T2D)  
- [ ] Cookers  

### Engineering
- [ ] C++23 FUSE core  
- [ ] Memory-safe handles/allocators for new code  
- [ ] ASan CI smoke  
- [ ] One CMake umbrella  
- [ ] THIRD_PARTY / licenses  

## Appendix B — Related docs

- [FUSE_MASTER_PLAN.md](./FUSE_MASTER_PLAN.md) — full port + feature fidelity  
- [FUSE_MASTER_PLAN_TOC.md](./FUSE_MASTER_PLAN_TOC.md) — Track B subsection index  
- `third_party/addons/README.md` — addon ore map  
- Upstream: TorqueGameEngines/Torque3D, TorqueGameEngines/Torque2D  

## Appendix C — Document control

| Version | Date | Notes |
|---------|------|-------|
| 0.1 | 2026-09-14 | Comprehensive pre-starter: unified tree → unified features → one program |
