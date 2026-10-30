# FUSE Engine — Master Development Plan

**FUSE** = Fast Unified Simulation Engine  
*(Meridian dropped — name already in use.)*

**Name:** FUSE Engine  
**Source lineage:** Torque3D (MIT) → full modernisation + feature tracks  
**Document status:** Unified master plan (port first, features second)  
**Language:** ISO C++23 host; CUDA device dialect may remain C++20  
**Editor GUI:** Qt 6 (not ImGui — stakeholder requirement)  
**Date:** 2026-09-14

---

## 0. Naming

**FUSE** = **F**ast **U**nified **S**imulation **E**ngine (locked).

Prior working name Meridian was dropped (already in use). Keep Torque3D only in legal notices, migration docs, `compat/`, and git history.

Optional tagline: *Exact where it matters. Parallel everywhere else.*

Namespaces / macros: `fuse::`, `FUSE_*` (e.g. `FUSE_ASSERT`, `FUSE_HOST_DEVICE`). Legacy `TORQUE_*` / TorqueScript only inside `compat/`.

---

## 1. Executive summary

This plan has **two sequential tracks**. Do not invert them.

1. **Track A — Port & harden Torque3D** into FUSE: ISO C++23, memory-safe ownership (handles + allocators), Qt 6 replaces the legacy editor GUI, CMake-first build, ASan/UBSan/CI gates, behavioural parity on golden scenes.
2. **Track B — Feature tracks** adapted from the seven phase plans: Foundation → Vulkan/CUDA → ECS/Spatial → Physics → Advanced Rendering → Qt Editor → Production. Each feature is a **modification of the ported engine**, not a greenfield Torque patch and not a greenfield-only rewrite that ignores Torque content pipelines.

**Hard rule:** no major feature work on a pre-port Torque subsystem until that subsystem’s Track A gate is green. Feature sections below state Torque3D deltas (remove / replace / keep / compat loaders) and which P-milestones they depend on.

**Port gate before feature work per subsystem** — e.g. no Vulkan feature merge until resource handles (P4) are green; no editor feature polish until Qt vertical slice (P5) is green; no production networking/audio until P7 unlock (or subsystem-specific earlier gates where noted).

---

## 2. Torque3D baseline assumptions

Confirm against the actual tree; typical T3D MIT realities the port must face:

- Mixed C++98/03/11 idioms, raw `new`/`delete`, custom containers, console/`TorqueScript` as first-class glue.
- Editor/tools GUI historically platform-native or in-engine `Gui*` (not Qt); tooling entangled with engine types.
- OpenGL-era rendering with fork-dependent D3D/GL options; resource lifetimes often raw-pointer-based.
- Soft module boundaries: platform, gfx, terrain, collada/dts, gui, console, `SimObject` share headers liberally.
- Build systems historically project-file / older CMake hybrids — not C++23-ready.
- Content: `.mis` missions, DTS/DIFF shapes, terrains, TorqueScript behaviours — treat as **compat assets**, not sacred source layout.

FUSE treats Torque3D as a **behavioural and asset-compat reference**, not as sacred architecture.

---

# TRACK A — Port plan (C++23, memory safety, Qt)

## A0. Goals (definition of done)

| Goal | Concrete bar |
|------|----------------|
| **ISO C++23** | `CMAKE_CXX_STANDARD 23` on host libs; CI with `-std=c++23` / `/std:c++23`. CUDA targets may use C++20 device dialect if the toolkit requires it. |
| **Memory safety** | No owning raw pointers in FUSE public APIs; `Handle`/`HandleMap` + allocator hierarchy; ASan/UBSan(/LSan) clean on smoke + unit suites; debug intercept bans `new`/`delete`/`malloc` in engine libs after allocator milestone. |
| **Qt GUI** | All *editor/tooling* chrome is Qt 6 Widgets (docking via Qt Advanced Docking or `QDockWidget`); Torque editor `Gui*` deleted or shim-only. Viewport embeds Vulkan via native `QWindow`/`QWidget` container. **No `#include <Q*>` in engine core.** |
| **Behavioural parity** | Golden Torque scenes/demos load and match reference captures within agreed tolerances before Track B features land on that subsystem. |

## A1. Principles

1. **Strangler fig, not big-bang rewrite.** Wrap Torque subsystems behind FUSE interfaces; replace guts per milestone; keep a compat binary that runs old missions until parity.
2. **Handles over pointers.** Adopt `Handle<T>` / `HandleMap` for long-lived objects (SimObject successors, resources, GPU objects).
3. **Allocators over `new`.** Linear / Pool / FreeList–TLSF / Ring / GPUAllocator + per-domain budgets; route Torque `FrameAllocator` / `Memory::` call sites through FUSE allocators during port.
4. **Qt owns chrome; engine owns viewport.** Qt main window + docks; Vulkan surface embedded; editor input Qt→FUSE; game path uses raw platform input.
5. **Scripts last.** TorqueScript quarantined under `compat/`; new gameplay prefers C++23 (+ Lua in Track B7).
6. **Vendored deps, pinned.** Catch2, VMA, etc. under `third_party/`; Qt via SDK is the documented exception (pin version in `THIRD_PARTY.md`).

## A2. Language subset & banned patterns

### Allowed (host C++23)

- Concepts, `requires`, `std::expected`, `std::optional`, `std::span`, `std::string_view`, `std::mdspan` where grids/voxels appear
- `std::format` / `std::print` for tooling (engine logger remains custom for shipping strip)
- Ranges on non-hot paths; coroutines only inside job/fiber layer
- `[[nodiscard]]`, aggressive `constexpr`/`consteval` on math & handles
- Modules optional late; headers first for CUDA interop sanity

### Banned / gated

| Pattern | Rule |
|---------|------|
| Owning raw `T*` in public APIs | Ban → `Handle`, allocator + size, or tooling-only `unique_ptr` |
| `new` / `delete` / `malloc` in engine libs | Ban after P1; debug overload asserts |
| Exceptions across engine↔script↔Qt | No; `std::expected` / error codes. Qt may throw at UI edge only |
| Unbounded C arrays of objects | Prefer `span` + sized buffers |
| Silent narrowing | `-Wconversion` in CI for FUSE-owned code |
| `#include <Q*>` in `engine_*` libs | Hard ban |

### Memory-safety toolkit

- Debug: ASan + UBSan (+ LSan where supported); generation-checked handles; domain memory budgets
- CI: sanitizer job mandatory for merge to `main`
- Later: clang-tidy (`modernize-*`, `cppcoreguidelines-owning-memory`)

## A3. Qt replaces GUI

### In scope

- World editor shell, inspector, hierarchy, asset browser, material editor, terrain/SDF tools chrome
- Preferences, project launcher, cook dialogs
- Docking, menus, shortcuts; undo via Qt Undo Framework wrapping FUSE `Command` objects

### Out of scope (first cut)

- In-game HUD / diegetic UI (legacy GuiControl runtime) — migrate later or keep engine retained-mode UI
- Full QML game UI — not required for editor port

### Architecture

```
┌─────────────────────────────────────────────┐
│  Qt 6 Application (editor process)          │
│  ├── QMainWindow / QDockWidget / dialogs    │
│  ├── FUSEEditorApi (C++23; no Qt types  │
│  │     leaking into engine core)            │
│  └── ViewportWidget → native Vulkan surface │
└─────────────────┬───────────────────────────┘
                  │ opaque handles / commands
┌─────────────────▼───────────────────────────┐
│  FUSE runtime (shared or sibling lib)   │
│  core / renderer / sim / assets / …         │
└─────────────────────────────────────────────┘
```

**Conflict resolved:** Phase 6 source planned ImGui dockspace + ImGui Vulkan backend + ImDrawList gizmos/profiler. Stakeholder requires **Qt 6**. All editor panels, flame graphs, console, and docking are Qt widgets/views; gizmos may use Qt overlay or engine debug draw into the viewport texture — not Dear ImGui.

### GUI migration steps

1. Inventory Torque `Gui*` editor entry points and tools.
2. Stand up Qt skeleton + empty viewport clearing colour via existing gfx → then Vulkan.
3. Reimplement inspector against reflection (SimObject properties → FUSE reflection) one panel at a time.
4. Delete Torque editor GUI resources once each panel has parity.
5. Wire undo/redo and selection sync.

## A4. Repo layout

```
fuse/
├── core/           # allocators, types, math, handles, GRIA α, log, assert, profile
├── platform/       # OS, window (game), raw input
├── renderer/       # gfx → Vulkan-first
├── compute/        # CUDA + Vulkan interop
├── physics/
├── ecs/            # evolve from SimObject carefully
├── assets/
├── audio/
├── editor/         # Qt 6 only
├── runtime_ui/     # optional in-game UI (post-editor)
├── game/           # sample / template game
├── compat/         # TorqueScript, legacy loaders, DTS/DIFF bridges
├── tools/          # offline cooks (Qt or CLI)
├── tests/
└── third_party/
```

CMake: C++23 host; `find_package(Qt6 COMPONENTS Widgets Gui Concurrent REQUIRED)` for `fuse_editor` only. Presets: `debug`, `release`, `profile`, `shipping`, `editor-debug`.

## A5. Milestones P0–P7

### P0 — Skeleton (1–2 weeks AI-assisted)

- Repo bootstrap, CMake presets, Catch2, sanitizer flags
- `core/types.hpp`, `Handle`, empty `Logger`, platform window smoke (game path)
- Qt editor process that links core and shows a window
- **Gate:** debug + release build; ASan clean empty app

### P1 — Memory & core types (Foundation §§1.2–1.3)

- Allocators: Linear, Pool, FreeList/TLSF, Ring, budgets; ban `new` in `engine_core`
- Port Torque frame/string allocator call sites
- **Gate:** memory stress tests green under ASan/UBSan

### P2 — Math & logging (Foundation §§1.4, 1.6)

- vec/mat/quat + SDF/noise; GRIA `Alpha`; Logger/assert/profiler
- Replace ad-hoc Torque logging gradually
- **Gate:** math + log unit tests; chrome://tracing JSON from profiler

### P3 — Jobs (Foundation §1.5)

- Fiber/work-stealing scheduler + `parallel_for` + CUDA job hooks
- Route safe Torque update loops onto jobs
- **Gate:** job system tests; single-thread fallback unchanged behaviour

### P4 — Sim & resources behind handles

- Inventory `SimObject`, `NetObject`, resource managers
- `HandleMap` facades; dual-run pointer + handle lookup during migration
- **Gate:** golden `.mis` loads with zero UAF under ASan

### P5 — Qt editor vertical slice

- Hierarchy + inspector + viewport for selected objects; save/load mission via FUSE APIs
- **Gate:** designer can open, select, tweak a property, save, reload

### P6 — Compat quarantine

- TorqueScript VM under `compat/`; legacy editor GUI out of default build
- **Gate:** `shipping` game runtime builds without Qt; editor is separate target

### P7 — Port complete / Track B unlock

- Parity suite (scenes, input, networking smoke if present); clang-tidy on FUSE-owned code
- **Gate:** Track B features merge only behind feature flags on FUSE APIs (subsystem gates still apply)

## A6. Port risk register

| Risk | Mitigation |
|------|------------|
| TorqueScript deeply coupled to editor | Keep VM in `compat/`; rewrite editor against C++ reflection |
| Hidden UB in old C++ | ASan every milestone; do not defer |
| Qt vs game input fighting | Explicit focus modes; raw input only when viewport captured |
| CUDA + C++23 + Qt toolchains | CUDA in separate lib; C++20 device dialect if needed; host C++23 |
| Scope explosion into full rewrite | Parity gates; features wait for subsystem green |
| ImGui assumptions in phase docs | **Superseded by Qt 6** — see A3 |

---

# TRACK B — Features (FUSE adaptations of Phases 1–7)

*Every section: duration from source; condensed FUSE design; Torque3D delta; Track A deps; Qt notes; condensed gates.*

---

## B1 — Foundation & Core Systems

**Duration:** 3–4 weeks (AI-assisted)  
**Depends on:** P0–P3 (implements alongside / immediately after those milestones)

### FUSE design summary

Build the permanent core everything else sits on:

1. **CMake / presets** — debug, release, profile, shipping; unity builds; PCH; ASan/UBSan in debug; vendored Catch2 + VMA.
2. **Types** — `u8`…`f64`, `Handle<T>`, `FUSE_HOST_DEVICE` macros; concepts `EngineValue`, `Component`, `GPUResident`, `Tickable`.
3. **GRIA Alpha** — first-class `Alpha` ∈ [0,1] (exact ↔ irreversible); drives LOD, SDF blend, physics precision later.
4. **Allocators** — Linear (frame ping-pong), Pool, FreeList/TLSF, Ring, GPUAllocator (Device/Pinned/Managed/Mapped); domain budgets (Renderer, Physics, ECS, Assets, Audio, Compute, Editor, Scratch).
5. **Math** — SIMD `vec4`/`mat4`/`quat` on host, scalar on device; analytic SDF library; Noise (Perlin/Simplex/Voronoi/FBM + Izaac-seeded).
6. **Jobs** — fiber work-stealing, `JobCounter`, `parallel_for`, CUDA jobs via events (no hot-path `cudaDeviceSynchronize`).
7. **Log / assert / profile** — lock-free ring logger; `FUSE_ASSERT` / `FUSE_VERIFY`; RDTSC scopes + GPU/CUDA markers.
8. **Platform** — own window/input (no GLFW required); Vulkan surface creation; game path separate from Qt editor.

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ad-hoc `Memory::`, frame allocators → FUSE allocator hierarchy |
| **Replace** | Console log → `fuse::Logger` channels |
| **Replace** | Older thread helpers → job scheduler where safe |
| **Keep** | Platform behaviours (window, file IO patterns) behind `platform/` façade |
| **Compat** | Map Torque `Platform::` call sites gradually |

### Gates (condensed)

- All four presets build zero-warnings; CUDA compiles against shared headers
- Allocator stress 1M cycles; zero `new`/`malloc` in engine libs; budgets assert in debug
- Job system 1M jobs + dependency chains; math SIMD ≡ scalar; ASan/UBSan + leak-free tests

---

## B2 — Vulkan Renderer & CUDA Interop

**Duration:** 4–5 weeks  
**Depends on:** P1 (memory), P4 (resource handles); unlocks fully after P7 for production merges — vertical “triangle” may start post-P4

### FUSE design summary

- **Vulkan bootstrap** — instance, physical device scoring, graphics/compute/transfer queues, VMA, sync2, dynamic rendering, descriptor indexing, buffer device address, external memory/semaphore for CUDA.
- **Swapchain & frames** — triple-buffer; per-frame cmd pools, timeline semaphores, LinearAllocator scratch.
- **Bindless resources** — global descriptor table; `TextureHandle`/`BufferHandle` via `HandleMap`; staging ring; async uploads.
- **Shaders / pipelines** — offline SPIR-V; hot-reload in editor/debug; content-hashed pipeline cache; push constants.
- **Render graph** — declares reads/writes; auto barriers; CUDA passes as first-class nodes (`CUDAWrite`/`CUDARead`).
- **CUDA–Vulkan interop** — external memory import; shared timeline semaphores; named streams (Render, Physics, AI, Particles, Upload).
- **Hybrid frame** — Vulkan G-buffer/depth → CUDA SDF ray march (GRIA α step quality) → Vulkan composite.
- **Week gates from source:** triangle week 1; hybrid SDF+raster week 3; full interop scene week 5 @ 60 fps 1080p.

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Remove / retire** | Legacy fixed-function / primary GL path as default |
| **Replace** | GFX resource lifetimes with handle + VMA-backed FUSE resources |
| **Keep** | Mesh/material *content* via importers feeding FUSE GPU resources |
| **Compat** | Optional GL backend only if stakeholder chooses one-release bridge (open decision) |

### Qt note

Viewport presents FUSE swapchain/colour target into Qt native container; no ImGui Vulkan backend.

### Gates (condensed)

- Zero validation errors on startup; resize-safe swapchain; bindless register/unregister clean
- External memory + timeline sync: 10k frames race-free; `compute-sanitizer` clean
- Triangle → hybrid SDF scene; < 8 ms GPU for 10-object SDF @ 1080p on RTX-class target; zero per-frame heap allocs

---

## B3 — ECS, Spatial Structures & Scene

**Duration:** 4–5 weeks  
**Depends on:** P4 (Sim→handles); B1 jobs/allocators; B2 renderer consuming `SceneData`

### FUSE design summary

- **Archetype ECS** — `EntityID` handles (index+generation); packed columns; `each` / `each_parallel`; CUDA-accessible managed/pinned columns where needed.
- **Components** — Transform (hierarchy, dirty), Mesh, SDFObject (+ GRIA blend α), RigidBody stub, Camera (reversed-Z frustum), lights, tags.
- **Systems** — TransformSystem, CameraSystem, CullingSystem (BVH), SceneBuildSystem → `DrawList` + SDF object buffer for CUDA.
- **BVH** — SAH build, refit, ray/AABB/sphere/frustum queries; GPU upload of nodes/leaves.
- **SVO** — sparse voxels, carve, dual-contour extract, SDF query at leaves; BVH leaves can be Mesh/SDF/VoxelChunk/Light.
- **Scene** — owns Registry + BVH + SVO + active camera; binary serialisation (magic/versioned); async load.

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | `SimObject` graph as primary world model → ECS (dual-run during P4) |
| **Replace** | Informal scene lists → `Scene` + binary format (new) with `.mis` importer |
| **Keep** | Mission/entity *semantics* via `compat/` loaders mapping into entities/components |
| **Remove** | Inheritance/`GameObject`-style god objects for new content |

### Gates (condensed)

- 1M entity create/destroy; archetype migration correct; parallel ≡ serial
- BVH vs brute-force ray/frustum; SVO round-trip + carve; scene save/load byte-identical components
- 500+ SDF objects @ 1080p > 60 fps from SceneData

---

## B4 — Physics Engine

**Duration:** 5–6 weeks  
**Depends on:** B3 (ECS + BVH + SVO); P1 GPU allocators; CUDA stream manager from B2

### FUSE design summary

CUDA-first physics (CPU submits, GPU owns SoA state):

- **SoA RigidBody / CollisionShape** — managed memory; flags STATIC/SLEEPING/TRIGGER/KINEMATIC/CCD.
- **Broad phase** — spatial hash + GPU radix sort → candidate pairs.
- **Narrow phase** — analytic sphere/plane/capsule/box; SDF gradient contacts; GJK/EPA for convex hulls.
- **Solver** — XPBD/PBD contacts + distance constraints; predict → iterate → velocity update → sleep.
- **Extras** — Barnes-Hut n-body (opt-in); CCD swept sphere TOI; SVO voxel destruction → dual-contour debris rigid bodies; cloth/soft-body particle XPBD.
- **PhysicsManager** — ECS sync ↔ SoA; collision Enter/Stay/Exit/Trigger events.

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary collision/dynamics path with FUSE CUDA PBD stack (or run dual until parity) |
| **Keep** | High-level gameplay hooks (triggers, impulses) via FUSE APIs |
| **Compat** | Map legacy collision types into `CollisionShapeSoA` where feasible |
| **Remove** | CPU-only bottleneck assumptions for sim hot path |

### Gates (condensed)

- Broad/narrow vs references; stable 10-sphere stack; restitution/friction analytic checks
- CCD prevents tunnelling; destruction carve + debris; cloth 32×32 stable
- < ~4–8 ms for 1k active bodies on RTX-class; ECS transforms match GPU state

---

## B5 — Advanced Rendering & Lighting

**Duration:** 6–7 weeks  
**Depends on:** B2 hybrid frame stable; B3 scene; B4 optional for dynamic lighting stress

### FUSE design summary

Production deferred + CUDA lighting stack:

- **Frame** — depth prepass → G-buffer (oct normals, albedo, rough/metal, velocity, depth, emissive) → CSM → CUDA (SDF march, DDGI, clustered lights, deferred PBR, SDF soft shadows, HBAO) → transparent / atmosphere → TAA → post (bloom, DoF, motion blur, ACES grade, lens flare, volumetrics).
- **PBR** — GGX/Smith/Schlick; material SSBO bindless; shading models Opaque/Translucent/Emissive/SSS/ClearCoat/Cloth; procedural analytic materials (wood/metal/concrete).
- **Clustered deferred** — 3D cluster grid; O(visible lights) per pixel.
- **Shadows** — 4-cascade CSM + analytic SDF soft penumbra.
- **DDGI** — probe grid, partial updates, hysteresis, SH/oct irradiance.
- **Sky** — Rayleigh/Mie analytic atmosphere (no cubemap required).

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Forward/legacy material models with FUSE PBR + deferred |
| **Keep** | Texture/mesh assets via cook pipeline → bindless textures |
| **Compat** | Torque material names/slots mapped into `MaterialSystem` where possible |
| **Remove** | Per-draw legacy fixed pipeline state as default |

### Qt note

Material editor / exposure / GRIA α debug knobs live in Qt panels (B6); renderer remains Qt-free.

### Gates (condensed)

- G-buffer encode round-trip; clustered 1k lights correct; CSM stable; SDF shadows vs path-trace tolerance
- DDGI updates; TAA no ghosting; HBAO/SSR sanity; ACES 0.18 grey calibration
- Full frame < 16 ms @ 1080p on RTX-class with listed pass budgets

---

## B6 — Editor (Qt 6 — not ImGui)

**Duration:** 5–6 weeks  
**Depends on:** P5 Qt vertical slice; B3 scene serialisation; B5 for full visual fidelity (can start earlier with simpler viewport)

### FUSE design summary (rewritten for Qt)

Same *capabilities* as Phase 6, different *presentation layer*:

| Concern | FUSE / Qt approach |
|---------|-------------------------|
| App shell | `QApplication` + `QMainWindow`; dockable panels (`QDockWidget` or ADS) |
| Decoupling | Editor submits `Command`s via `FUSEEditorApi`; engine runs headless without editor linked |
| Undo/redo | FUSE `Command` / `CommandStack` + `QUndoStack` adapter; merge consecutive transforms |
| Viewport | `QWidget`/`QWindow` hosting Vulkan; free-cam; picking; context menus as `QMenu` |
| Gizmos | Engine debug-draw or Qt overlay on viewport; analytic hit tests retained |
| Hierarchy | `QTreeView` + model over ECS parent/child |
| Inspector | Property widgets generated from reflection; GRIA α slider first-class |
| Materials | Qt material editor + live preview render target |
| SDF sculpt | Brush panel (add/subtract/smooth/roughen/paint); α blend; symmetry; convert to voxels / extract mesh |
| Assets | `QFileSystemModel`-style browser + thumbnails; drag-drop into viewport |
| Profiler | **Qt widgets**: scrolling GPU/CPU plots, pass breakdown table, **flame graph** view over FUSE profile events (not ImGui PlotLines) |
| Console | `QPlainTextEdit` + filters + command line hooked to logger / debug exec |
| Play mode | Snapshot scene → simulate → restore on stop |

Hot-reload: shaders/assets/scripts visible and triggerable from Qt without restarting.

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Remove** | Torque editor `Gui*` from default editor build (after parity) |
| **Replace** | World Editor chrome with Qt FUSE editor |
| **Keep** | Mission save/load semantics via FUSE scene + compat `.mis` bridge |
| **Compat** | Document unsupported Torque editor plugins |

### Qt note (critical)

**Phase 6 source = ImGui. This plan = Qt 6 only.** Do not vendor Dear ImGui for editor chrome. Profiler flame graph, docking, and themes are Qt. Optional immediate-mode debug overlay *inside* the game viewport is a separate runtime concern — not a substitute for the editor.

### Gates (condensed)

- Editor start < ~1 s; default layout restores; DPI-aware fonts
- Command undo/redo LIFO; delete restore; transform merge
- Viewport camera/pick/resize clean; gizmo translate/rotate/scale + snap
- Hierarchy reparent + search; inspector live SDF/α; sculpt add/subtract
- Profiler timings within 0.5 ms of CUDA events; play mode snapshot restore exact
- Editor UI budget < ~2 ms/frame; no Qt types in engine core includes

---

## B7 — Production Systems

**Duration:** 8–10 weeks  
**Depends on:** P6–P7 port complete preferred; B3–B6 for integration; subsystem gates may allow early starts (e.g. asset tools after P1)

### FUSE design summary

1. **Animation** — skeleton/clips (Hermite), blend trees / 1D–2D spaces / layered / state machine; FABRIK + two-bone IK; CUDA LBS skinning; `Animator` component.
2. **Audio** — custom spatial engine (no FMOD/Wwise); OpenAL output; HRTF; CUDA FFT convolution reverb; `AudioSource` / `AudioListener` components.
3. **Scripting** — **Lua** public API (Entity/Physics/Input/Audio/Scene/SDF lifecycle); hot-reload. TorqueScript remains in `compat/` for legacy content — not the primary new API.
4. **Networking** — transport abstraction (ENet / optional Steam); rollback (GGPO-style) + authoritative sync + client interpolation; compact binary serialiser.
5. **Terrain** — heightfield + LOD chunks + underground SVO caves; analytic noise generate; deform/paint; slope-based procedural layers.
6. **World partition** — cell stream in/out async by camera distance; force load for teleports.
7. **VFX** — GPU particles SoA; emit/simulate/collide with SDF; billboard render; no CPU readback.
8. **Platform hardening** — Win minidump / Linux signals; shipping strip (`FUSE_SHIPPING`); leak detector debug; DPI awareness.
9. **Offline asset pipeline** — mesh (FBX/GLTF/OBJ → binary + LODs), texture (BC7 etc.), audio cook; dependency graph incremental reimport. Bridge DTS/DIFF via `compat/` importers where needed.

### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary gameplay scripting story → Lua (new) while retaining TorqueScript loader |
| **Replace / extend** | Audio, animation, net with FUSE systems; map Torque net object ideas carefully |
| **Keep** | Large-world / mission workflows via partition + scene format + `.mis` import |
| **Compat** | DTS/DIFF/terrain legacy → cooks into FUSE binaries |
| **Remove** | Requirement that editor plugins stay Torque-GUI-based |

### Qt note

Animation state-machine editor, audio debug meters, network session dialogs, terrain brushes, and cook UI are Qt tools/panels — same ABI rule (no Qt in core).

### Gates (condensed)

- Animation sample/blend/IK/skinning; audio spatial + reverb accuracy; Lua API + hot-reload
- Rollback resim + desync checksum; terrain LOD + caves; partition no pop-in; 4k particles
- Shipping binary stripped; crash dump valid; importers deterministic; full boot+editor+scene < ~3 s on reference workstation

---

## Combined roadmap timeline

```
Track A:  P0 → P1 → P2 → P3 → P4 → P5 → P6 → P7
           │     │     │           │
Track B:   B1────┴─────┘           │
                 B2 (post-P4) ─────┤
                 B3 (post-P4) ─────┤
                 B4 (post-B3)      │
                 B5 (post-B2/B3)   │
                 B6 (post-P5; full after B5)
                 B7 (prefer post-P7; tools earlier)
```

| Track | AI-assisted duration (from sources) |
|-------|-------------------------------------|
| A P0–P7 | Re-estimate after repo inventory; expect multiple Foundation-scale slices |
| B1 | 3–4 weeks |
| B2 | 4–5 weeks |
| B3 | 4–5 weeks |
| B4 | 5–6 weeks |
| B5 | 6–7 weeks |
| B6 | 5–6 weeks |
| B7 | 8–10 weeks |

Parallelism: B1 coincides with P1–P3; later B tracks overlap only where Track A subsystem gates are green.

---

## Risk & open decisions

### Cross-cutting risks

| Risk | Mitigation |
|------|------------|
| Feature work before port gates | Enforce per-subsystem green gates in CI/review |
| SimObject ↔ ECS dual-run complexity | Time-box dual-run; golden mission ASan gate |
| CUDA/Qt/MSVC toolchain friction | Isolate `engine_compute`; pin toolkit versions |
| Scope vs parity | Ship compat loaders; defer non-critical B7 items behind flags |
| GRIA α misunderstood as “just a float” | Document as cross-cutting policy; expose in Qt inspector |

### Open decisions (stakeholder)

1. Confirm **FUSE** vs Helix / Lodestar / Stratum for public branding.
2. Editor-only Qt vs also a Qt launcher/installer shell.
3. Vulkan-only vs one-release GL compat backend.
4. TorqueScript: maintain indefinitely vs freeze after content migration.
5. Networking priority: rollback vs authoritative-first for first product.
6. Qt licence path: LGPL vs commercial for shipping editor.

---

## Appendix A — Rename checklist

- [ ] CMake `project(FUSE …)`
- [ ] Namespace `fuse::`
- [ ] Macros `FUSE_*` / `FUSE_ASSERT` / `FUSE_HOST_DEVICE`
- [ ] Default window title “FUSE”
- [ ] Icons, installer, docs, CI badge names
- [ ] `TORQUE_*` only inside `compat/`
- [ ] Log channels / memory domains renamed
- [ ] Scene file magic updated (versioned; old magic in compat loader)

## Appendix B — Legal notes

- Preserve MIT notices from Torque3D and all `third_party/` components.
- Qt: LGPL or commercial per distribution plan — document in `THIRD_PARTY.md`.
- Confirm “FUSE” trademark clearance before public release.
- CUDA / proprietary SDK redistributables: follow NVIDIA (and any audio codec) EULAs.
- Do not strip copyright headers when moving files into `compat/` or FUSE trees.

## Appendix C — Phase 1 checklist carry-forward

Retain Foundation deliverable gates under FUSE naming, plus:

- [ ] `CMAKE_CXX_STANDARD 23` on all non-CUDA host targets
- [ ] `fuse_editor` on Qt 6 with embedded Vulkan viewport
- [ ] No `#include <Q*>` from `engine_core` / `engine_renderer` / `engine_physics` / `engine_ecs` / `engine_compute`
- [ ] Golden Torque mission loads under ASan after P4
- [ ] Profiler events consumable by Qt flame-graph panel (B6)

## Appendix D — Document control

| Version | Date | Notes |
|---------|------|-------|
| 1.0 | 2026-09-14 | Full unified plan: Track A P0–P7 + Track B1–B7 from all seven phase files; ImGui→Qt rewrite; C++23 host |

**Inputs read:** Phase 1 Foundation & Core Systems; Phase 2 Vulkan & CUDA; Phase 3 ECS/Spatial/Scene; Phase 4 Physics; Phase 5 Advanced Rendering; Phase 6 Editor (ImGui → rewritten as Qt); Phase 7 Production Systems; prior draft Track A structure retained and extended.