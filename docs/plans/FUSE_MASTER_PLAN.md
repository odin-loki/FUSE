# FUSE Engine — Master Development Plan (Full Fidelity)

**FUSE** = Fast Unified Simulation Engine  
*(Meridian dropped — name already in use.)*

**Name:** FUSE Engine  
**Source lineage:** Torque3D (MIT) → full modernisation + feature tracks  
**Document status:** Full-fidelity unified master plan (port first, features second)
**Fidelity:** Track B mirrors every source `## N.M` as `### Bn.M`
**Document control:** v0.3 full fidelity  
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

# TRACK A — Port (full existing + polish)

*Track A retained and polished. Do not delete. Track B features depend on these milestones.*

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

- Allocators: Linear, Pool, FreeList/TLSF, Ring, budgets; ban `new` in `fuse_core`
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

# TRACK B — Features

*Every `### Bn.M` mirrors source `## N.M`. Each includes **FUSE design**, **Torque3D delta**, **Depends on**, and **Gates**. Engine=FUSE; editor=Qt 6; host=C++23. **Port-first applies to every subsection.**

---

## B1 — Foundation & Core Systems

**Duration:** 3–4 weeks (AI-assisted)  
**Depends on (phase-level):** P0–P3 (implements alongside / immediately after those milestones)

*Full fidelity: every source `## 1.*` → `### B1.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B1.1 — Project Structure & Build System

#### FUSE design

The build system is the skeleton everything else attaches to. Get this wrong and you'll be fighting CMake instead of writing engine code for the entire project. Set it up once, set it up right.

#### Directory Layout

```
fuse/
├── core/           # allocators, types, math, handles, RTTI
├── platform/       # OS abstraction, window, raw input
├── renderer/       # Vulkan layer, pipelines, shader system
├── compute/        # CUDA kernels, CUDA-Vulkan interop
├── physics/        # broadphase, narrowphase, dynamics, solver
├── ecs/            # entity, component, archetype storage
├── assets/         # pipeline, binary formats, streaming
├── audio/          # spatial audio, convolution reverb
├── editor/         # Qt 6 only — docking, gizmos chrome, scene tools
├── game/           # game layer, scripting, prefabs
├── tools/          # offline asset pipeline tools
├── tests/          # unit + integration + fuzz tests
└── third_party/    # vendored only — no package manager surprises
```

#### CMake Configuration

```cmake
cmake_minimum_required(VERSION 3.28)
project(FUSE LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_ARCHITECTURES 86)  # RTX 3090 = sm_86

find_package(Vulkan REQUIRED)
find_package(CUDAToolkit REQUIRED)

# Separate lib targets per subsystem
add_library(fuse_core     STATIC)
add_library(fuse_renderer STATIC)
add_library(fuse_compute  STATIC)
add_library(fuse_physics  STATIC)
add_library(fuse_ecs      STATIC)

    // … truncated in master plan (full sketch in phase source)
```

#### Compiler Flags

| Config | Flags |
|--------|-------|
| C++23 host | `-std=c++23 -Wall -Wextra -Werror` (CI only) |
| CUDA | `--extended-lambda --expt-relaxed-constexpr -lineinfo` |
| Release | `-O3 -march=native -ffast-math` (non-physics code only) |
| Debug | `-O0 -g3 -DFUSE_DEBUG` + sanitizers |
| Profile | `-O2 -g -DFUSE_PROFILE` — no sanitizers |

#### Build Configurations

Three CMake presets: `debug`, `release`, `profile`. A fourth `shipping` preset strips all logging, asserts, and profiler hooks via preprocessor — the binary that would ship to a client or be deployed in a defence system. All four must build cleanly with zero warnings before any phase is considered complete.

#### Third-Party Policy

No package manager (vcpkg, Conan). All dependencies vendored under `third_party/` as git submodules with pinned commit hashes. This gives you reproducible builds in air-gapped environments. Permitted third-party libs at this phase:

- **Catch2** — unit testing only
- **VMA (Vulkan Memory Allocator)** — GPU heap management
- **Tracy** (optional) — external profiler backend

Everything else is written from scratch.

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque project-file / older CMake hybrids with FUSE CMake presets (`debug`/`release`/`profile`/`shipping`/`editor-debug`) |
| **Replace** | Ad-hoc directory sprawl with `fuse/` layout (`core/`, `platform/`, `renderer/`, `compute/`, `physics/`, `ecs/`, `assets/`, `audio/`, `editor/` Qt-only, `compat/`, `tools/`, `tests/`, `third_party/`) |
| **Keep** | Ability to build a game runtime without editor (shipping preset) |
| **Compat** | Legacy build scripts quarantined; not default |

#### Depends on

- Track A: P0 Skeleton

#### Gates

- [ ] **B1.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.2 — Type System & Fundamental Types

#### FUSE design

Define FUSE's type vocabulary before writing a single system. Every subsystem will use these. They must be correct, fast, and CUDA-compatible where needed.

#### Primitive Typedefs

```cpp
// core/types.hpp
#pragma once
#include <cstdint>
#include <cstddef>

// Sized integers — no ambiguity ever
using u8    = uint8_t;
using u16   = uint16_t;
using u32   = uint32_t;
using u64   = uint64_t;
using i8    = int8_t;
using i16   = int16_t;
using i32   = int32_t;
using i64   = int64_t;
using f32   = float;
using f64   = double;
using usize = size_t;
using isize = ptrdiff_t;
    // … truncated in master plan (full sketch in phase source)
```

#### GRIA Alpha — First-Class Engine Primitive

Alpha is not a float. It is a typed concept that encodes the reversibility/irreversibility of any operation in FUSE — from rendering LOD to physics precision to material evaluation. It lives in `core/gria.hpp` and is passed by value everywhere.

```cpp
// core/gria.hpp
#pragma once
#include "types.hpp"

// α ∈ [0,1]: 0 = fully reversible/exact, 1 = fully irreversible/approximate
struct Alpha {
  f32 value;

  FUSE_HOST_DEVICE constexpr Alpha(f32 v) : value(v) {}
  FUSE_HOST_DEVICE constexpr f32 get() const { return value; }

  FUSE_HOST_DEVICE bool is_exact()         const { return value < 0.01f; }
  FUSE_HOST_DEVICE bool is_approximate()   const { return value > 0.99f; }
  FUSE_HOST_DEVICE bool at_edge_of_chaos() const { return value > 0.49f && value < 0.51f; }

  // Grand Unified Law: α = 1 − H(f(X))/H(X)
  static Alpha from_entropy_ratio(f32 h_output, f32 h_input) {
    return Alpha(1.0f - h_output / h_input);
    // … truncated in master plan (full sketch in phase source)
```

#### Handle System

Raw pointers are banned in all engine interfaces. Everything long-lived is accessed via a `Handle<T>` — a 64-bit value (32-bit index + 32-bit generation counter) that detects stale references automatically. Handle validation is O(1) — compare generation against the slot's current generation in the backing store. This eliminates an entire class of use-after-free bugs without overhead.

```cpp
// core/handle_map.hpp
template<typename T>
class HandleMap {
public:
  Handle<T>  insert(T&& value);
  void       remove(Handle<T> handle);
  T*         get(Handle<T> handle);       // returns nullptr if stale
  const T*   get(Handle<T> handle) const;
  bool       valid(Handle<T> handle) const;

private:
  struct Slot { T value; u32 generation; bool occupied; };
  std::vector<Slot> slots_;
  std::vector<u32>  free_list_;
};
```

#### Type Traits & Concepts

C++23 concepts replace SFINAE throughout the codebase. Define engine-specific concepts in `core/concepts.hpp`:

```cpp
// core/concepts.hpp
#pragma once
#include <concepts>
#include "types.hpp"

// Trivially copyable and movable — safe to memcpy, safe in CUDA managed memory
template<typename T>
concept FuseValue = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// A component type — must be an FuseValue with a static constexpr name
template<typename T>
concept Component = FuseValue<T> && requires {
  { T::component_name } -> std::convertible_to<const char*>;
};

// A GPU-resident type — must be trivially copyable and aligned to at least 4 bytes
template<typename T>
concept GPUResident = FuseValue<T> && (alignof(T) >= 4);
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ambiguous Torque typedefs / platform int types with FUSE `u8`…`f64`, `Handle<T>`, concepts |
| **Replace** | Raw pointer long-lived object IDs with generation-checked `Handle` / `HandleMap` |
| **Keep** | Semantic meaning of SimObject IDs during dual-run (map into handles) |
| **Compat** | `TORQUE_*` macros only inside `compat/` |

#### Depends on

- Track A: P0–P1

#### Gates

- [ ] **B1.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.3 — Memory & Allocator System

#### FUSE design

This is the most important system in Phase 1. Every allocation in FUSE goes through here. No `new`, no `delete`, no `malloc` anywhere in engine code after this is built. The allocator system must support both CPU and GPU memory from a unified interface.

#### Allocator Interface

```cpp
// core/memory/allocator.hpp
#pragma once
#include "../types.hpp"

struct AllocInfo {
  usize       size;
  usize       alignment;
  const char* tag;      // debug label — stripped in shipping builds
};

struct Allocator {
  virtual void*  alloc(AllocInfo info)                           = 0;
  virtual void   free(void* ptr, usize size)                     = 0;
  virtual void*  realloc(void* ptr, usize old_size, usize new_size) = 0;
  virtual usize  used()  const                                   = 0;
  virtual usize  total() const                                   = 0;
  virtual void   reset()                                         = 0;
  virtual ~Allocator() = default;
    // … truncated in master plan (full sketch in phase source)
```

#### Allocator Implementations

**LinearAllocator** — bump pointer. O(1) alloc, mass free only. Used for per-frame scratch, temp strings, job scratch space. Two buffers ping-pong per frame so last frame's data survives one frame for debugging.

```cpp
class LinearAllocator : public Allocator {
  byte*  base_;
  usize  offset_;
  usize  capacity_;
public:
  LinearAllocator(usize capacity);
  void* alloc(AllocInfo info) override;
  void  free(void*, usize) override {}     // no-op — reset() frees all
  void  reset() override { offset_ = 0; }
  usize used()  const override { return offset_; }
  usize total() const override { return capacity_; }
};
```

**PoolAllocator\<T\>** — fixed-size blocks from a freelist. O(1) alloc and free. Zero fragmentation. Cache-friendly — all T objects are contiguous. Used for components, handles, particles, small uniform-size objects.

**FreeListAllocator** — variable size blocks, O(1) amortised with TLSF. Used for assets, long-lived heterogeneous allocations, strings.

**RingAllocator** — circular buffer with a read and write cursor. O(1). Used for GPU upload staging buffers, streaming data, network packets.

**TLSFAllocator** — Two-Level Segregated Fit. True O(1) alloc and free with low fragmentation. Used as the general-purpose heap for anything that doesn't fit the above.

**GPUAllocator** — wraps VMA for Vulkan allocations and direct CUDA calls for compute memory. Unified interface.

#### GPU Memory Manager

```cpp
// core/memory/gpu_allocator.hpp
#pragma once
#include "../types.hpp"
#include <cuda_runtime.h>

enum class GPUMemoryType {
  Device,       // cudaMalloc           — fast GPU-only, no CPU access
  Pinned,       // cudaMallocHost       — CPU pinned, DMA-able, fast H2D transfer
  Managed,      // cudaMallocManaged    — unified, auto-migrate on access
  DeviceMapped, // cudaHostAlloc MAPPED — zero-copy, CPU and GPU share physical pages
};

struct GPUAllocation {
  void*         device_ptr;
  void*         host_ptr;    // null if Device-only
  usize         size;
  GPUMemoryType type;
};
    // … truncated in master plan (full sketch in phase source)
```

#### Memory Budget System

Every engine subsystem declares a memory budget at startup. The allocator system enforces it. If a subsystem overruns its budget in debug builds, it asserts. In release builds it logs and falls back to the general heap. This prevents any one system from silently consuming all available memory.

```cpp
// core/memory/budget.hpp
enum class MemoryDomain : u8 {
  Renderer, Physics, ECS, Assets, Audio, Compute, Editor, Scratch
};

struct MemoryBudget {
  MemoryDomain domain;
  usize        cpu_bytes;
  usize        gpu_bytes;
};

// Declared at engine init, before any subsystem allocates
void declare_memory_budgets(std::span<MemoryBudget> budgets);
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque `Memory::` / `FrameAllocator` / ad-hoc `new` with FUSE allocator hierarchy + domain budgets |
| **Remove** | Owning raw `new`/`delete`/`malloc` in engine libs after this milestone |
| **Keep** | Frame-scratch *behaviour* (short-lived per-frame buffers) via LinearAllocator ping-pong |
| **Compat** | Route legacy call sites through FUSE allocators during port |

#### Depends on

- Track A: P1 Memory & core types

#### Gates

- [ ] **B1.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.4 — Math Library

#### FUSE design

SIMD-accelerated on CPU, scalar on CUDA device, exact where geometry demands it.

#### Vector Types

```cpp
// core/math/vec.hpp
#pragma once
#include "../types.hpp"
#include <immintrin.h>

// Primary 3D vector — vec3 is an alias, w=0 for directions, w=1 for positions
struct alignas(16) vec4 {
  union {
    struct { f32 x, y, z, w; };
    __m128 simd;
    f32    data[4];
  };

  FUSE_HOST_DEVICE constexpr vec4() : x(0), y(0), z(0), w(0) {}
  FUSE_HOST_DEVICE constexpr vec4(f32 x, f32 y, f32 z, f32 w = 0.f)
    : x(x), y(y), z(z), w(w) {}

  // SIMD on host, scalar on CUDA device
    // … truncated in master plan (full sketch in phase source)
```

#### Matrix & Quaternion

`mat4` — column-major, SIMD-accelerated multiply. Stores four `vec4` columns. SSE `_mm_dp_ps` for dot products in multiply.

`quat` — `{x, y, z, w}` convention. All rotation operations go through quaternions — no Euler angles in engine internals. Euler angles exist only at the editor input layer and are immediately converted.

`mat3` — used for normal transforms. Derived from `mat4` via upper-left 3×3 extraction.

#### SDF Primitive Library

```cpp
// core/math/sdf.hpp — all functions FUSE_HOST_DEVICE
#pragma once
#include "vec.hpp"

namespace SDF {

  // Exact sphere — not a polygon approximation
  FUSE_HOST_DEVICE inline f32
  sphere(vec3 p, f32 r) { return p.length() - r; }

  // Exact axis-aligned box
  FUSE_HOST_DEVICE inline f32
  box(vec3 p, vec3 b) {
    vec3 q = { fabsf(p.x)-b.x, fabsf(p.y)-b.y, fabsf(p.z)-b.z };
    vec3 c = { fmaxf(q.x,0.f), fmaxf(q.y,0.f), fmaxf(q.z,0.f) };
    return c.length() + fminf(fmaxf(q.x, fmaxf(q.y, q.z)), 0.f);
  }

    // … truncated in master plan (full sketch in phase source)
```

#### Noise as a First-Class Primitive

Procedural noise is not a texture lookup — it is an analytic function evaluated at infinite resolution.

```cpp
// core/math/noise.hpp
namespace Noise {
  FUSE_HOST_DEVICE f32 perlin(vec3 p);
  FUSE_HOST_DEVICE f32 simplex(vec3 p);
  FUSE_HOST_DEVICE f32 voronoi(vec3 p, vec3* closest_point = nullptr);
  FUSE_HOST_DEVICE f32 fbm(vec3 p, u32 octaves, f32 lacunarity = 2.f, f32 gain = 0.5f);
  FUSE_HOST_DEVICE f32 domain_warp(vec3 p, u32 octaves);

  // Izaac-seeded noise — cryptographic quality, deterministic from VRF seed
  FUSE_HOST_DEVICE f32 izaac_noise(vec3 p, u64 vrf_seed);
}
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Fragmented Torque math helpers with SIMD `vec4`/`mat4`/`quat` + analytic SDF/Noise |
| **Keep** | Gameplay-facing vector/matrix semantics; Euler only at editor input edge |
| **Compat** | Conversion helpers from Torque `Point3F`/`MatrixF` in `compat/` |

#### Depends on

- Track A: P2 Math & logging

#### Gates

- [ ] **B1.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.5 — Job System & Fiber Scheduler

#### FUSE design

The job system is FUSE's spine. Everything — rendering, physics, asset streaming, simulation — submits work here. A fiber-based work-stealing scheduler gives you dependency graphs, coroutine-style waiting, and full utilisation of all cores with zero thread blocking.

#### Architecture

- N worker threads (one per physical core minus one for the OS)
- Each worker owns a local deque of ready fibers
- Work stealing: idle workers steal from the tail of busy workers' deques
- Fibers: 64KB stacks, switched via `setjmp`/`longjmp` or platform inline asm
- Jobs: plain C++23 lambdas or coroutines submitted to the global queue
- `JobCounter`: atomic u32 tracking group completion — fibers yield-wait on counter reaching zero
- No `std::mutex`, no `std::condition_variable`, no OS blocking anywhere in the hot path

#### Scheduler API

```cpp
// core/jobs/scheduler.hpp
#pragma once
#include "../types.hpp"
#include <functional>
#include <atomic>
#include <coroutine>
#include <span>

struct JobCounter {
  std::atomic<u32> count{0};
  void wait();   // yields current fiber — never blocks the thread
  void add(u32 n = 1) { count.fetch_add(n, std::memory_order_relaxed); }
  void done()    { count.fetch_sub(1, std::memory_order_release); }
};

enum class JobPriority : u8 { Low = 0, Normal = 1, High = 2, Critical = 3 };

struct JobDesc {
    // … truncated in master plan (full sketch in phase source)
```

#### Parallel-For Primitive

```cpp
// core/jobs/parallel_for.hpp
template<typename Fn>
void parallel_for(u32 count, u32 batch_size, Fn&& fn, JobPriority priority = JobPriority::Normal) {
  if (count == 0) return;
  u32 batch_count = (count + batch_size - 1) / batch_size;

  JobCounter counter;
  counter.add(batch_count);

  for (u32 b = 0; b < batch_count; b++) {
    u32 begin = b * batch_size;
    u32 end   = std::min(begin + batch_size, count);
    JobScheduler::submit({
      .fn       = [fn, begin, end]{ for (u32 i = begin; i < end; i++) fn(i); },
      .counter  = &counter,
      .priority = priority
    });
  }
    // … truncated in master plan (full sketch in phase source)
```

#### CUDA-Job Integration

CUDA kernels are dispatched as jobs. The job wraps the kernel launch and records a CUDA event for synchronisation. A waiting fiber checks the event rather than calling `cudaDeviceSynchronize`, keeping the thread free.

```cpp
struct CUDAJobDesc {
  std::function<void(cudaStream_t)> kernel_launcher;
  JobCounter* counter = nullptr;
  const char* tag     = nullptr;
};

void submit_cuda(CUDAJobDesc desc);  // launches kernel on a managed stream, signals counter on completion
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Older Torque thread helpers / blocking worker patterns with fiber work-stealing scheduler where safe |
| **Keep** | Single-thread fallback behaviour for determinism tests |
| **Compat** | Route safe Torque update loops onto jobs gradually |

#### Depends on

- Track A: P3 Jobs

#### Gates

- [ ] **B1.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.6 — Logging, Assert & Profiler

#### FUSE design

#### Logging System

Lock-free ring buffer. Async flush to sinks on a dedicated IO thread. Zero allocation in the hot path — format strings are written into the ring buffer in-place. Zero overhead in shipping builds.

```cpp
// core/log.hpp
#pragma once
#include "types.hpp"
#include <format>

enum class LogLevel   : u8  { Trace, Debug, Info, Warn, Error, Fatal };
enum class LogChannel : u32 {
  Core     = 1 << 0,
  Renderer = 1 << 1,
  Physics  = 1 << 2,
  ECS      = 1 << 3,
  Assets   = 1 << 4,
  Compute  = 1 << 5,
  Audio    = 1 << 6,
  Editor   = 1 << 7,
  All      = ~0u
};

    // … truncated in master plan (full sketch in phase source)
```

#### Assert System

```cpp
// core/assert.hpp
#pragma once

// Debug assert — stripped in release
#ifdef FUSE_DEBUG
  #define FUSE_ASSERT(cond, msg)                                                     \
    do { if (!(cond)) {                                                                 \
      LOG_ERROR(LogChannel::Core, "ASSERT FAILED: {} at {}:{}", msg, __FILE__, __LINE__); \
      __debugbreak();                                                                    \
    }} while(0)
#else
  #define FUSE_ASSERT(cond, msg) ((void)(cond))
#endif

// Release assert — always on, for invariants that must hold in shipping
#define FUSE_VERIFY(cond, msg)                                                       \
  do { if (!(cond)) {                                                                   \
    Logger::log(LogLevel::Fatal, LogChannel::Core, __FILE__, __LINE__,                 \
    // … truncated in master plan (full sketch in phase source)
```

#### Profiler

```cpp
// core/profile.hpp
#pragma once
#include "types.hpp"

// CPU scope timer — RDTSC based, < 5ns overhead
struct ProfileScope {
  const char* name;
  u64         start;
  ProfileScope(const char* name);
  ~ProfileScope();
};

// GPU markers — Vulkan timestamp queries correlated with CPU timeline
void profile_gpu_begin(const char* name, void* cmd_buffer);
void profile_gpu_end(void* cmd_buffer);

// CUDA event markers
void profile_cuda_begin(const char* name, cudaStream_t stream);
    // … truncated in master plan (full sketch in phase source)
```

Output formats: chrome://tracing JSON (offline analysis), or live Qt flame-graph panel in the editor via a circular event buffer (B6; Dear Qt forbidden for editor chrome).

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Console / ad-hoc printf logging with `fuse::Logger` channels + lock-free ring |
| **Replace** | Ad-hoc asserts with `FUSE_ASSERT` / `FUSE_VERIFY` |
| **Keep** | Ability to sink logs to console/file; chrome://tracing JSON for offline |
| **Compat** | Map Torque `Con::printf` style calls during dual-run |

#### Depends on

- Track A: P2 Math & logging

#### Gates

- [ ] **B1.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.7 — Platform Abstraction & Window

#### FUSE design

Thin OS layer. Windows first. Linux second. No GLFW — own your window and input from day one. GLFW is a single header swap if you ever want it, but you won't.

#### Platform Interface

```cpp
// platform/platform.hpp
#pragma once
#include "../core/types.hpp"

struct WindowDesc {
  const char* title      = "FUSE";
  u32         width      = 1920;
  u32         height     = 1080;
  bool        fullscreen = false;
  bool        borderless = false;
  bool        vsync      = true;
};

struct WindowHandle { void* native; };  // HWND on Windows, Window on X11/Wayland

class Platform {
public:
  static bool         init();
    // … truncated in master plan (full sketch in phase source)
```

#### Raw Input System

```cpp
// platform/input.hpp
#pragma once
#include "../core/types.hpp"

enum class Key : u16 {
  A=0, B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  Space, Enter, Escape, Tab, Backspace, Delete, Insert,
  Left, Right, Up, Down, PageUp, PageDown, Home, End,
  Ctrl, Shift, Alt, Super, CapsLock,
  COUNT
};

enum class MouseButton : u8 { Left, Right, Middle, X1, X2, COUNT };

struct InputState {
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Entangled platform/window ownership for *game* path with thin `fuse::Platform` |
| **Keep** | Window/file IO behavioural patterns behind façade |
| **Compat** | Map Torque `Platform::` call sites gradually |
| **Note** | Editor windowing is **Qt 6** (Track A / B6) — game path stays Qt-free |

#### Depends on

- Track A: P0 Platform window smoke; editor uses Qt separately (P5)

#### Gates

- [ ] **B1.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B1.8 — Phase 1 Deliverables & Test Suite

#### FUSE design

Phase 1 is complete when every item in this checklist passes. Nothing moves to Phase 2 until the foundation is solid.

#### Build & Compilation

#### Memory

#### Job System

#### Math

#### Logging & Profiler

#### Platform

#### Final Gate

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | All Foundation checklist items under FUSE naming + C++23 host + sanitizer gates |
| **Add** | Track A gates: no `#include <Q*>` in engine libs; Qt editor separate target |

#### Depends on

- Track A: P0–P3 green

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] CMake builds cleanly in Debug, Release, Profile, Shipping — zero warnings with `-Wall -Wextra`
- [ ] CUDA compiles against C++23 FUSE host headers (CUDA device dialect may remain C++23) — no separate CUDA type system or duplicate definitions
- [ ] All four build configs produce correct binaries on Windows; Linux build compiles without error
- [ ] Third-party dependencies (Catch2, VMA) build from vendored source with pinned commits
- [ ] Unity builds reduce full rebuild time below 60 seconds on ThinkStation P920
- [ ] All allocators pass 1M alloc/free stress cycles — zero leaks, alignment always correct
- [ ] LinearAllocator correctly ping-pongs between frames — no use-after-reset in debug
- [ ] PoolAllocator correctly detects double-free via generation counter — asserts in debug
- [ ] GPUAllocator allocates and frees Device, Pinned, Managed memory — verified with `cuda-memcheck`
- [ ] Budget system correctly asserts when a domain exceeds declared limits in debug builds
- [ ] Zero heap allocations (`new`/`malloc`) anywhere in engine code — verified via overloaded operators
- [ ] Scheduler initialises N worker threads — verified via thread count query
- [ ] 1M independent jobs complete with correct results across all workers
- [ ] Dependency chain test: Job C depends on B depends on A — always executes in order
- [ ] Fiber yield/resume: a fiber waiting on a counter correctly resumes after counter reaches zero
- [ ] No deadlock under any ordering of job submission — verified with 10k random submission tests
- [ ] `parallel_for` of 1M items produces identical result to serial loop
- [ ] `vec4` SIMD operations produce bit-identical results to scalar reference implementation
- [ ] All SDF primitives match reference ray marcher within 0.0001f tolerance
- [ ] SDF normals via gradient match finite-difference normals within 0.001f
- [ ] GRIA `Alpha` evaluates correctly on both host and CUDA device kernel
- [ ] `mat4` multiply matches reference scalar implementation exactly
- [ ] Quaternion slerp produces unit quaternion at all interpolation points
- [ ] Logger ring buffer survives concurrent writes from all worker threads — no corruption
- [ ] Log entries appear with correct timestamps, file, and line numbers
- [ ] `FUSE_ASSERT` fires and breaks in debug, is a no-op in release — verified by disassembly
- [ ] Profiler scope overhead < 10ns per scope on ThinkStation hardware
- [ ] Profiler output writes valid chrome://tracing JSON
- [ ] Platform opens a window, displays title, receives and dispatches events cleanly
- [ ] Input system correctly reports `key_pressed` for exactly one frame on a keydown event
- [ ] Raw mouse delta is unaffected by OS cursor acceleration settings
- [ ] `get_vulkan_surface` returns a valid `VkSurfaceKHR` — verified by Vulkan validation layers in Phase 2
- [ ] All unit tests pass under AddressSanitizer + UndefinedBehaviorSanitizer
- [ ] `valgrind --leak-check=full` reports zero leaks on the test suite binary
- [ ] A benchmark of the hot path (job submit → execute → complete → alloc from pool → free) shows zero heap allocations per iteration

---

#### What Phase 2 / B2 Builds On This

With Phase 1 complete, FUSE has a memory-safe, fully parallel, mathematically exact and typed foundation. Phase 2 sits the Vulkan renderer on top — a triangle on screen by week 5, the hybrid SDF raster/ray march pipeline by week 8, and CUDA-Vulkan interop producing a real rendered frame by the end of the phase. Every rendering abstraction will submit work through the job system, allocate through the allocator hierarchy, and operate on handle-based resources. Nothing in Phase 2 bypasses what was built here.

*Bridge:* After **B1** gates are green (and required Track A milestones), begin **B2** on FUSE APIs only.

## B2 — Vulkan Renderer & CUDA Interop

**Duration:** 4–5 weeks  
**Depends on (phase-level):** P1 (memory), P4 (resource handles); vertical triangle may start post-P4; production merges after P7

*Full fidelity: every source `## 2.*` → `### B2.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B2.1 — Vulkan Bootstrap

#### FUSE design

Vulkan has a famously verbose initialisation path. Do it right once, wrap it thinly, and never touch it again.

#### Instance & Physical Device

```cpp
// renderer/vk/instance.hpp
#pragma once
#include "../../core/types.hpp"
#include <vulkan/vulkan.h>
#include <span>

struct VulkanInstanceDesc {
  const char* app_name       = "Engine";
  u32         app_version    = VK_MAKE_VERSION(1, 0, 0);
  bool        enable_validation = true;   // forced off in Shipping build
  std::span<const char*> extra_extensions = {};
};

struct VulkanInstance {
  VkInstance               handle       = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

  static VulkanInstance create(const VulkanInstanceDesc& desc);
    // … truncated in master plan (full sketch in phase source)
```

#### Logical Device & Queues

```cpp
// renderer/vk/device.hpp
#pragma once
#include "instance.hpp"

struct VulkanQueues {
  VkQueue graphics;     // primary render + present
  VkQueue compute;      // async compute
  VkQueue transfer;     // dedicated DMA — no stall on graphics pipeline
  u32     graphics_family;
  u32     compute_family;
  u32     transfer_family;
};

struct VulkanDevice {
  VkDevice       handle  = VK_NULL_HANDLE;
  VulkanQueues   queues;
  VmaAllocator   vma    = VK_NULL_HANDLE;   // Vulkan Memory Allocator

    // … truncated in master plan (full sketch in phase source)
```

#### Required Device Extensions

```cpp
static constexpr const char* REQUIRED_EXTENSIONS[] = {
  VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,        // timeline semaphores
  VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,         // no render pass objects
  VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,       // bindless resources
  VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,     // GPU pointers
  VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           // CUDA interop
  VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,        // CUDA-Vulkan sync
  VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,     // Windows HANDLE sharing
  VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
};

static constexpr const char* OPTIONAL_EXTENSIONS[] = {
  VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,     // RT pipeline — optional
  VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
  VK_NV_MESH_SHADER_EXTENSION_NAME,
};
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Remove / retire** | Legacy fixed-function / primary GL path as default device bootstrap |
| **Replace** | GFX device init with Vulkan instance/device/queues/VMA |
| **Compat** | Optional GL backend only if stakeholder chooses one-release bridge (open decision) |

#### Depends on

- Track A: P1 allocators; P4 resource handles for production merge

#### Gates

- [ ] **B2.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.2 — Swapchain & Frame Management

#### FUSE design

#### Swapchain

```cpp
// renderer/vk/swapchain.hpp
#pragma once
#include "device.hpp"

struct SwapchainDesc {
  VkSurfaceKHR surface;
  u32          width, height;
  bool         vsync      = true;
  u32          image_count = 3;         // triple buffering
  VkFormat     preferred_format = VK_FORMAT_B8G8R8A8_UNORM;
};

struct SwapchainImage {
  VkImage     image;
  VkImageView view;
  u32         index;
};

    // … truncated in master plan (full sketch in phase source)
```

#### Frame-in-Flight Management

Triple-buffered. Frame N+1 never waits for frame N-1 to finish presenting. Each frame has its own command pool, descriptor pool, and per-frame scratch allocator.

```cpp
// renderer/vk/frame.hpp
#pragma once
#include "device.hpp"
#include "../../core/memory/allocator.hpp"

static constexpr u32 FRAMES_IN_FLIGHT = 3;

struct FrameData {
  VkCommandPool    cmd_pool;
  VkCommandBuffer  cmd;              // primary command buffer
  VkCommandBuffer  transfer_cmd;     // async transfer commands
  VkSemaphore      image_available;  // swapchain acquire signal
  VkSemaphore      render_finished;  // present wait
  VkSemaphore      timeline;         // timeline semaphore for CUDA sync
  u64              timeline_value;
  VkFence          in_flight_fence;
  LinearAllocator  scratch;          // 8MB per-frame CPU scratch — reset each frame
  VkDescriptorPool descriptor_pool;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Legacy swap/present paths with FUSE triple-buffered swapchain + per-frame resources |
| **Keep** | Resize / vsync user-facing behaviour |
| **Note** | Editor presents into Qt native Vulkan container, not a standalone game window only |

#### Depends on

- Track A: P4; P5 for Qt viewport embedding

#### Gates

- [ ] **B2.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.3 — Resource System & Bindless Architecture

#### FUSE design

Bindless from day one. No per-draw descriptor set updates. All textures, buffers, and samplers live in a global descriptor table indexed by u32. This is the architecture modern engines (Unreal 5, id Tech 7) moved to and it maps perfectly to CUDA's flat memory model.

#### Resource Types

```cpp
// renderer/resources.hpp
#pragma once
#include "../core/types.hpp"
#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

// All GPU resources identified by typed handles
using TextureHandle = Handle<struct Texture_>;
using BufferHandle  = Handle<struct Buffer_>;
using SamplerHandle = Handle<struct Sampler_>;
using ShaderHandle  = Handle<struct Shader_>;
using PipelineHandle= Handle<struct Pipeline_>;

struct TextureDesc {
  u32         width, height, depth = 1;
  u32         mip_levels   = 1;
  u32         array_layers = 1;
  VkFormat    format;
    // … truncated in master plan (full sketch in phase source)
```

#### Bindless Descriptor System

```cpp
// renderer/vk/bindless.hpp
#pragma once
#include "../resources.hpp"

// Global descriptor set layout — one set for entire engine
// Binding 0: storage images   (readwrite textures)
// Binding 1: sampled images   (readonly textures)
// Binding 2: samplers
// Binding 3: storage buffers  (SSBOs)
// Binding 4: uniform buffers

static constexpr u32 MAX_TEXTURES  = 65536;
static constexpr u32 MAX_BUFFERS   = 65536;
static constexpr u32 MAX_SAMPLERS  = 1024;

class BindlessDescriptors {
public:
  void init(const VulkanDevice& device);
    // … truncated in master plan (full sketch in phase source)
```

#### Resource Manager

```cpp
// renderer/resource_manager.hpp
#pragma once
#include "resources.hpp"
#include "vk/bindless.hpp"

class ResourceManager {
public:
  void init(const VulkanDevice& device, BindlessDescriptors& bindless);
  void destroy();

  // Synchronous creation (blocks until GPU-side is ready)
  TextureHandle  create_texture(const TextureDesc& desc, const void* initial_data = nullptr);
  BufferHandle   create_buffer(const BufferDesc& desc, const void* initial_data = nullptr);
  SamplerHandle  create_sampler(VkFilter min, VkFilter mag, VkSamplerAddressMode wrap);

  // Async upload — returns immediately, signals fence on completion
  void           upload_texture_async(TextureHandle h, const void* data, usize size, VkFence fence);
  void           upload_buffer_async(BufferHandle h, const void* data, usize size, VkFence fence);
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Raw-pointer GFX resource lifetimes with bindless `TextureHandle`/`BufferHandle` + VMA |
| **Keep** | Mesh/texture *content* via importers feeding FUSE GPU resources |
| **Compat** | Legacy resource managers dual-run behind HandleMap façades |

#### Depends on

- Track A: P4 Sim & resources behind handles

#### Gates

- [ ] **B2.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.4 — Shader System & Pipeline Compiler

#### FUSE design

#### Shader Compilation

All shaders compiled offline to SPIR-V as part of the asset pipeline. Hot-reload in debug/editor builds via `inotify`/`ReadDirectoryChangesW` file watchers.

```cpp
// renderer/shaders/compiler.hpp
#pragma once
#include "../../core/types.hpp"
#include <span>
#include <vector>

enum class ShaderStage : u8 {
  Vertex, Fragment, Compute, Mesh, Task,
  RayGen, RayMiss, RayClosestHit, RayAnyHit
};

struct ShaderDesc {
  const char* source_path;     // GLSL or HLSL source
  const char* entry_point = "main";
  ShaderStage stage;
  std::span<const char*> defines = {};
};

    // … truncated in master plan (full sketch in phase source)
```

#### Graphics Pipeline Builder

```cpp
// renderer/vk/pipeline_builder.hpp
#pragma once
#include "device.hpp"
#include "../resources.hpp"

struct GraphicsPipelineDesc {
  // Shader stages
  ShaderHandle vert_shader;
  ShaderHandle frag_shader;
  ShaderHandle mesh_shader   = {};   // optional — replaces vert
  ShaderHandle task_shader   = {};

  // Rasterisation
  VkPolygonMode   polygon_mode = VK_POLYGON_MODE_FILL;
  VkCullModeFlags cull_mode    = VK_CULL_MODE_BACK_BIT;
  bool            depth_test   = true;
  bool            depth_write  = true;
  VkCompareOp     depth_op     = VK_COMPARE_OP_LESS;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ad-hoc shader compile/load with offline SPIR-V + content-hashed pipeline cache |
| **Keep** | Hot-reload in editor/debug builds |
| **Compat** | Map legacy material shader names where feasible |

#### Depends on

- Track A: P4; B2.1 device

#### Gates

- [ ] **B2.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.5 — Command Buffer & Render Graph

#### FUSE design

A lightweight render graph drives the frame. Nodes declare their resource reads and writes. The graph resolves image layout transitions, pipeline barriers, and pass ordering automatically. No manual `vkCmdPipelineBarrier` calls outside the graph.

#### Render Graph

```cpp
// renderer/render_graph.hpp
#pragma once
#include "resources.hpp"
#include "../core/types.hpp"
#include <functional>
#include <span>

enum class RGResourceAccess : u32 {
  ColorAttachmentWrite,
  DepthAttachmentWrite,
  ShaderRead,
  ShaderWrite,      // storage image / SSBO
  TransferSrc,
  TransferDst,
  Present,
  CUDAWrite,        // resource will be written by a CUDA kernel this frame
  CUDARead,
};
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Manual barrier spaghetti / fixed forward passes with render graph (CUDA nodes first-class) |
| **Remove** | Per-frame heap allocs in command recording hot path |

#### Depends on

- Track A: P1 LinearAllocator; B2.1–B2.4

#### Gates

- [ ] **B2.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.6 — CUDA-Vulkan Interop

#### FUSE design

This is the most technically demanding section of Phase 2. CUDA and Vulkan share the same physical GPU but have separate memory and execution models. Interop is achieved via external memory handles and timeline semaphores.

#### Shared Memory

```cpp
// compute/interop.hpp
#pragma once
#include "../core/types.hpp"
#include <vulkan/vulkan.h>
#include <cuda_runtime.h>

// Import a Vulkan buffer into CUDA — returns a CUDA device pointer
// The Vulkan buffer must have been created with VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
void* import_vulkan_buffer_to_cuda(VkDevice vk_device, VkDeviceMemory memory,
                                   usize offset, usize size);

// Import a Vulkan image into CUDA as a surface object (read/write)
cudaSurfaceObject_t import_vulkan_image_to_cuda(VkDevice vk_device, VkDeviceMemory memory,
                                                u32 width, u32 height, VkFormat format);

// Free imported CUDA handles — must be called before destroying Vulkan resource
void free_cuda_import(void* cuda_ptr);
void free_cuda_surface(cudaSurfaceObject_t surf);
```

#### Timeline Semaphore Sync

```cpp
// compute/cuda_vk_sync.hpp
#pragma once
#include "../core/types.hpp"
#include <vulkan/vulkan.h>
#include <cuda_runtime.h>

// Wrapper around a shared timeline semaphore
// Vulkan signals it after render pass, CUDA waits before compute
// CUDA signals it after compute, Vulkan waits before next pass
struct SharedTimeline {
  VkSemaphore           vk_semaphore;
  cudaExternalSemaphore_t cuda_semaphore;
  u64                   value = 0;

  static SharedTimeline create(VkDevice vk_device);
  void                  destroy(VkDevice vk_device);

  // CPU-side signal/wait for synchronisation point setup
    // … truncated in master plan (full sketch in phase source)
```

#### Interop Pattern — Frame Flow

```
Frame N:

[Vulkan] Rasterise coarse geometry → depth buffer, G-buffer
         signal(vk_to_cuda, frame_N)
         ↓
[CUDA]   wait(vk_to_cuda, frame_N)
         Ray march SDF scene from depth buffer hits
         Evaluate exact lighting, shadows, reflections
         Write to output texture (shared Vulkan/CUDA)
         signal(cuda_to_vk, frame_N)
         ↓
[Vulkan] wait(cuda_to_vk, frame_N)
         Composite CUDA output with Vulkan raster output
         Post-process, UI
         Present
```

#### CUDA Stream Manager

```cpp
// compute/stream_manager.hpp
#pragma once
#include "../core/types.hpp"
#include <cuda_runtime.h>

// Named streams for workload separation
enum class CUDAStream : u8 {
  Render,      // SDF ray marching — tied to Vulkan interop sync
  Physics,     // Rigid body, cloth, fluid sim
  AI,          // Agent updates, pathfinding
  Particles,   // Particle simulation
  Upload,      // H2D data transfers
  COUNT
};

class StreamManager {
public:
  void        init();
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | CUDA–Vulkan external memory + timeline semaphores (greenfield on ported core) |
| **Keep** | Named streams (Render, Physics, AI, Particles, Upload) |
| **Compat** | N/A for classic Torque — new capability |

#### Depends on

- Track A: P1 GPUAllocator; B2.1 extensions

#### Gates

- [ ] **B2.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.7 — SDF Ray Marcher (CUDA)

#### FUSE design

The first CUDA render pass. Takes the depth buffer from Vulkan rasterisation and ray marches the SDF scene from each pixel's world-space position.

#### Ray Marcher Kernel

```cpp
// compute/kernels/ray_march.cuh
#pragma once
#include "../../core/math/sdf.hpp"
#include "../../core/math/vec.hpp"
#include "../../core/types.hpp"
#include <cuda_runtime.h>
#include <surface_functions.h>

struct RayMarchParams {
  cudaSurfaceObject_t output;       // RGBA16F output — shared with Vulkan
  cudaSurfaceObject_t depth_in;     // Vulkan depth buffer read-back
  u32                 width, height;

  // Camera
  vec3 cam_pos;
  vec3 cam_forward, cam_right, cam_up;
  f32  fov_rad;

    // … truncated in master plan (full sketch in phase source)
```

#### SDF Scene Evaluation

```cpp
__device__ f32 scene_sdf(const RayMarchParams& p, vec3 pos) {
  f32 d = p.max_dist;

  for (u32 i = 0; i < p.object_count; i++) {
    const auto& obj = p.objects[i];
    vec3 local = {pos.x - obj.position.x,
                  pos.y - obj.position.y,
                  pos.z - obj.position.z};
    f32 obj_d;
    switch (obj.type) {
      case 0: obj_d = SDF::sphere(local, obj.params.x); break;
      case 1: obj_d = SDF::box(local, obj.params);      break;
      case 2: obj_d = SDF::capsule(local, {0,0,0}, {0, obj.params.y, 0}, obj.params.x); break;
      case 3: obj_d = SDF::torus(local, obj.params.x, obj.params.y); break;
      default: obj_d = p.max_dist; break;
    }
    d = SDF::op_smooth_union(d, obj_d, obj.alpha);
  }
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | CUDA SDF ray marcher with GRIA α step quality (new) |
| **Keep** | Integrates with scene SDF objects from B3 |

#### Depends on

- Track A: B1 math/SDF; B2.6 interop

#### Gates

- [ ] **B2.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.8 — Rasterisation Pipeline

#### FUSE design

The Vulkan raster side — renders proxy geometry for primary visibility, populates the depth buffer and G-buffer for CUDA to consume.

#### G-Buffer Layout

```
Attachment 0: RGBA16F — world-space normal (xyz) + metallic (w)
Attachment 1: RGBA8   — base color (rgb) + roughness (a)
Attachment 2: R32F    — depth (for CUDA ray march seed)
Attachment 3: RG16F   — velocity (for TAA in Phase 5)
```

#### Geometry Pass Shader (GLSL)

```glsl
// shaders/geometry_pass.frag.glsl
#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in  vec3 frag_normal;
layout(location = 1) in  vec2 frag_uv;
layout(location = 2) in  vec3 frag_world_pos;

layout(location = 0) out vec4 g_normal_metallic;
layout(location = 1) out vec4 g_albedo_roughness;

// Bindless material data
layout(push_constant) uniform PC {
  uint material_id;
  mat4 model;
  mat4 mvp;
} pc;

    // … truncated in master plan (full sketch in phase source)
```

#### Draw Call Submission

```cpp
// renderer/draw_list.hpp
#pragma once
#include "resources.hpp"
#include "../ecs/components.hpp"

struct DrawCall {
  BufferHandle  vertex_buffer;
  BufferHandle  index_buffer;
  u32           index_count;
  u32           material_id;
  // Per-instance data passed via push constants
  struct PushData {
    mat4 model;
    mat4 mvp;
    u32  material_id;
    u32  _pad[3];
  } push;
};
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary mesh draw path with Vulkan G-buffer / depth prepass raster |
| **Keep** | Mesh asset pipe via cooks → GPU meshes |

#### Depends on

- Track A: B2.3–B2.5

#### Gates

- [ ] **B2.8** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.9 — Composite Pass

#### FUSE design

Merges the CUDA ray-marched output with the Vulkan raster output into the final frame.

```glsl
// shaders/composite.frag.glsl
#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in  vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
  uint raster_texture;    // bindless index — Vulkan G-buffer composite
  uint cuda_texture;      // bindless index — CUDA ray-marched output
  uint depth_texture;
  float blend;            // GRIA alpha — 0=all CUDA, 1=all raster
} pc;

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 2) uniform sampler   samplers[];

void main() {
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | Hybrid composite of raster + SDF CUDA results |
| **Replace** | Single forward blit as sole present path |

#### Depends on

- Track A: B2.7 + B2.8

#### Gates

- [ ] **B2.9** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.10 — Renderer Initialisation & Main Loop

#### FUSE design

```cpp
// renderer/renderer.hpp
#pragma once
#include "vk/instance.hpp"
#include "vk/device.hpp"
#include "vk/swapchain.hpp"
#include "vk/frame.hpp"
#include "vk/bindless.hpp"
#include "vk/pipeline_builder.hpp"
#include "resource_manager.hpp"
#include "render_graph.hpp"
#include "../compute/stream_manager.hpp"
#include "../compute/cuda_vk_sync.hpp"

struct RendererDesc {
  void*  window_handle;    // platform WindowHandle
  u32    width, height;
  bool   validation    = true;
  Alpha  default_alpha = ALPHA_CHAOS_EDGE;  // GRIA α default
    // … truncated in master plan (full sketch in phase source)
```

#### Main Loop Integration

```cpp
// main.cpp — wiring it all together
int main() {
  Platform::init();
  auto wnd = Platform::create_window({.title      = "FUSE", .width=1920, .height=1080});

  JobScheduler::init();

  Renderer renderer;
  renderer.init({.window_handle = wnd.native, .width=1920, .height=1080});

  f64 last_time = Platform::get_time_seconds();

  while (Platform::poll_events()) {
    f64 now = Platform::get_time_seconds();
    f32 dt  = (f32)(now - last_time);
    last_time = now;

    Input::update();
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque main-loop gfx tick with FUSE renderer init + frame graph execute |
| **Keep** | Game loop ownership separation from editor loop |

#### Depends on

- Track A: B2.1–B2.9; P7 for unlocked production default

#### Gates

- [ ] **B2.10** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B2.11 — Phase 2 Deliverables & Test Suite

#### FUSE design

#### Vulkan Infrastructure

#### Resource System

#### Shader & Pipeline System

#### CUDA-Vulkan Interop

#### Rendering Correctness

#### Performance Baselines (RTX 3090)

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Full Phase 2 deliverable/test checklist under FUSE naming |
| **Add** | Qt viewport present smoke (no ImGui Vulkan backend) |

#### Depends on

- Track A: P4 + B2 vertical slice gates

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] Instance creates cleanly with validation layers enabled — zero validation errors on startup
- [ ] Physical device selection picks the RTX 3090 correctly over any integrated GPU
- [ ] Logical device created with graphics, compute, and transfer queues on separate families where available
- [ ] Swapchain creates at 1920×1080, triple-buffered — resize correctly rebuilds without crash
- [ ] Frame-in-flight management holds three independent frame data sets — verified by timeline semaphore values
- [ ] All Vulkan objects named via `vkSetDebugUtilsObjectNameEXT` — visible in RenderDoc
- [ ] Texture and buffer creation with all VMA memory types — verified with `vkconfig` overlay
- [ ] Bindless descriptor table registers and unregisters textures — no descriptor heap corruption
- [ ] Staging ring buffer correctly wraps — upload of 256MB in 1MB chunks with no corruption
- [ ] Async upload completes and signals fence correctly — verified with fence wait timeout test
- [ ] External memory textures allocate with correct Win32 handle — `cudaImportExternalMemory` succeeds
- [ ] Shader compiler produces valid SPIR-V for all test shaders — verified with `spirv-val`
- [ ] Pipeline cache serialises to disk and restores on next run — first frame pipeline stalls eliminated
- [ ] Hot-reload triggers pipeline rebuild in < 200ms — verified by timing shader file write to first redrawn frame
- [ ] Push constants correctly pass per-draw data to shaders — verified with RenderDoc capture
- [ ] `SharedTimeline` semaphore correctly serialises Vulkan and CUDA execution — no race conditions under 10k frames
- [ ] Vulkan-allocated external memory buffer reads back identical data when accessed via CUDA pointer
- [ ] CUDA surface write to shared texture appears correctly in Vulkan composite pass
- [ ] `cuda-memcheck` and `compute-sanitizer` report zero errors across full frame loop
- [ ] Triangle on screen — white triangle, black background, correct winding, no validation errors: **Week 1 gate**
- [ ] G-buffer pass populates normal, albedo, depth attachments correctly — verified with RenderDoc
- [ ] CUDA ray marcher produces correct sphere SDF at all angles — verified against reference renderer
- [ ] SDF normals are smooth at surface — no faceting visible at any zoom level
- [ ] Composite pass correctly blends CUDA and raster output at all GRIA α values
- [ ] Final frame presents to screen at stable 60fps at 1920×1080 with a 10-object SDF scene: **Week 5 gate**
- [ ] GPU frame time < 8ms for a 10-object SDF scene at 1080p (target 120fps headroom)
- [ ] CUDA ray march kernel achieves > 60% occupancy — verified with Nsight Compute
- [ ] Zero per-frame heap allocations — all frame memory from per-frame LinearAllocator
- [ ] Render graph compiles in < 1ms CPU time per frame

---

#### What Phase 3 / B3 Builds On This

Phase 2 delivers the renderer's permanent foundation. Phase 3 builds the ECS, spatial structures (Sparse Voxel Octree + hybrid BVH), and the scene representation that feeds the renderer. For the first time FUSE will have actual objects — entities with transform, mesh, and SDF components — living in a scene graph, culled by the BVH, and submitted to the draw list and ray marcher automatically. By the end of Phase 3 FUSE renders a real scene, not just hand-authored test geometry.

*Bridge:* After **B2** gates are green (and required Track A milestones), begin **B3** on FUSE APIs only.

## B3 — ECS, Spatial Structures & Scene

**Duration:** 4–5 weeks  
**Depends on (phase-level):** P4 (Sim→handles); B1 jobs/allocators; B2 renderer consuming SceneData

*Full fidelity: every source `## 3.*` → `### B3.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B3.1 — Entity-Component-System Architecture

#### FUSE design

The ECS is FUSE's data model. Everything that exists in the game world is an entity. Everything an entity *is* or *does* is described by components. Systems process components in bulk. No inheritance, no virtual dispatch, no `GameObject` god objects.

#### Design Principles

- **Archetype storage** — entities with the same component set share a contiguous memory block (archetype). Iteration over a component type is a linear scan through packed arrays. Cache miss rate approaches zero for bulk processing.
- **Handle-based identity** — entities are `EntityID` handles (index + generation). No raw pointers. Stale handles detected O(1).
- **CUDA-accessible component arrays** — archetypes backed by managed or pinned memory where physics and simulation components need GPU access.
- **No per-entity virtual calls** — systems are free functions or lambdas that receive typed component spans.
- **C++23 concepts** constrain all component and system types at compile time.

#### Entity ID & Registry

```cpp
// ecs/entity.hpp
#pragma once
#include "../core/types.hpp"

struct EntityID {
  u32 index;
  u32 generation;
  bool operator==(const EntityID&) const = default;
  [[nodiscard]] bool valid() const { return generation != 0; }
  static EntityID null() { return {0, 0}; }
};

static constexpr u32 MAX_ENTITIES = 1 << 20;  // 1M entities

class Registry {
public:
  void      init(usize max_entities = MAX_ENTITIES);
  void      destroy();
    // … truncated in master plan (full sketch in phase source)
```

#### Archetype Storage

```cpp
// ecs/archetype.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/memory/allocator.hpp"
#include <typeindex>
#include <unordered_map>

// Identifies a unique set of component types
// Stored as a sorted vector of type_index — same set = same ArchetypeID
struct ArchetypeID {
  u64 hash;
  bool operator==(const ArchetypeID&) const = default;
};

// One column per component type — all columns same length (entity count)
struct ComponentColumn {
  void*  data;        // raw pointer into allocator-owned memory
  usize  element_size;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | `SimObject` graph as primary world model → archetype ECS (dual-run during P4) |
| **Remove** | Inheritance/`GameBase`-style god objects for *new* content |
| **Compat** | Map SimObject IDs ↔ `EntityID` during migration |

#### Depends on

- Track A: P4 Sim & resources behind handles

#### Gates

- [ ] **B3.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.2 — Core Component Types

#### FUSE design

All components are plain data — `FuseValue` concept enforced. No methods, no constructors with side effects.

#### Transform Component

```cpp
// ecs/components/transform.hpp
#pragma once
#include "../../core/math/vec.hpp"
#include "../../core/types.hpp"

struct Transform {
  static constexpr const char* component_name = "Transform";

  vec3  position   = {0, 0, 0, 1};  // w=1 (point)
  quat  rotation   = {0, 0, 0, 1};  // identity quaternion
  vec3  scale      = {1, 1, 1, 0};  // w=0 (direction/scale)
  EntityID parent  = EntityID::null();

  // Derived — recomputed by TransformSystem each frame
  mat4  local_to_world;
  mat4  world_to_local;
  bool  dirty = true;

    // … truncated in master plan (full sketch in phase source)
```

#### Mesh Component

```cpp
// ecs/components/mesh.hpp
#pragma once
#include "../../renderer/resources.hpp"

struct Mesh {
  static constexpr const char* component_name = "Mesh";

  BufferHandle  vertex_buffer;
  BufferHandle  index_buffer;
  u32           index_count;
  u32           material_id;

  // AABB in local space — updated when mesh changes
  vec3 aabb_min;
  vec3 aabb_max;

  bool cast_shadow   = true;
  bool receive_shadow= true;
    // … truncated in master plan (full sketch in phase source)
```

#### SDF Component

```cpp
// ecs/components/sdf_object.hpp
#pragma once
#include "../../core/math/sdf.hpp"
#include "../../core/gria.hpp"

enum class SDFPrimitive : u8 {
  Sphere, Box, Capsule, Torus, Cylinder,
  Custom   // references a GPU-resident function pointer (advanced)
};

struct SDFObject {
  static constexpr const char* component_name = "SDFObject";

  SDFPrimitive type     = SDFPrimitive::Sphere;
  vec3         params   = {1, 0, 0, 0};  // r for sphere, half_extents for box, etc.
  u32          material_id;
  Alpha        blend_alpha = ALPHA_CHAOS_EDGE;  // GRIA α for smooth union blending
  bool         casts_shadow = true;
    // … truncated in master plan (full sketch in phase source)
```

#### Physics Component

```cpp
// ecs/components/rigidbody.hpp
#pragma once
#include "../../core/math/vec.hpp"

struct RigidBody {
  static constexpr const char* component_name = "RigidBody";

  vec3  velocity          = {0, 0, 0};
  vec3  angular_velocity  = {0, 0, 0};
  vec3  force_accumulator = {0, 0, 0};
  vec3  torque_accumulator= {0, 0, 0};
  f32   mass              = 1.f;
  f32   inv_mass          = 1.f;      // 0 = infinite mass (static)
  f32   restitution       = 0.4f;
  f32   linear_damping    = 0.99f;
  f32   angular_damping   = 0.98f;
  bool  is_static         = false;
  bool  is_sleeping       = false;
    // … truncated in master plan (full sketch in phase source)
```

#### Camera Component

```cpp
// ecs/components/camera.hpp
#pragma once
#include "../../core/math/vec.hpp"

struct Camera {
  static constexpr const char* component_name = "Camera";

  f32  fov_deg      = 75.f;
  f32  near_plane   = 0.1f;
  f32  far_plane    = 10000.f;
  f32  aspect_ratio = 16.f / 9.f;
  bool is_active    = false;      // only one active camera per scene

  // Derived — computed from Transform + these params
  mat4 view;
  mat4 projection;
  mat4 view_projection;

    // … truncated in master plan (full sketch in phase source)
```

#### Additional Components

```cpp
// ecs/components/light.hpp
struct DirectionalLight {
  static constexpr const char* component_name = "DirectionalLight";
  vec3  color     = {1, 1, 1};
  f32   intensity = 1.f;
};

struct PointLight {
  static constexpr const char* component_name = "PointLight";
  vec3  color     = {1, 1, 1};
  f32   intensity = 1.f;
  f32   radius    = 10.f;         // influence radius — used for BVH light culling
};

struct SpotLight {
  static constexpr const char* component_name = "SpotLight";
  vec3  color     = {1, 1, 1};
  f32   intensity = 1.f;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Informal Torque field bags with typed FUSE components (Transform, Mesh, SDFObject, lights, Camera, tags) |
| **Keep** | Mission/entity *semantics* via compat loaders |
| **Compat** | `.mis` field names → component writers |

#### Depends on

- Track A: P4; B3.1

#### Gates

- [ ] **B3.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.3 — Core Systems

#### FUSE design

Systems are free functions that operate on component arrays via `Registry::each`. They run on the job system. Dependencies between systems are declared explicitly so the scheduler can parallelise where safe.

#### Transform System

```cpp
// ecs/systems/transform_system.hpp
#pragma once
#include "../entity.hpp"

class TransformSystem {
public:
  // Recomputes local_to_world for all dirty transforms
  // Parent transforms are resolved first — hierarchy traversal via topological sort
  static void update(Registry& reg);

private:
  static void update_recursive(Registry& reg, EntityID id, const mat4& parent_matrix);
};

// Implementation sketch
void TransformSystem::update(Registry& reg) {
  // First pass: collect all root transforms (parent == null)
  // Submit as jobs per-archetype — O(n) parallel
    // … truncated in master plan (full sketch in phase source)
```

#### Visibility & Frustum Culling System

```cpp
// ecs/systems/culling_system.hpp
#pragma once
#include "../entity.hpp"
#include "../../renderer/draw_list.hpp"

struct CullResult {
  std::vector<EntityID> visible_meshes;
  std::vector<EntityID> visible_sdf_objects;
  std::vector<EntityID> visible_lights;
  u32                   culled_count;
};

class CullingSystem {
public:
  // Frustum-cull all Mesh and SDFObject entities against active camera
  // Uses BVH for O(log n + k) rather than O(n) per-entity test
  static CullResult cull(Registry& reg, const Camera& camera, const struct BVH& bvh);

    // … truncated in master plan (full sketch in phase source)
```

#### Scene Build System

Converts ECS query results into renderer-consumable data structures each frame.

```cpp
// ecs/systems/scene_build_system.hpp
#pragma once
#include "../entity.hpp"
#include "../../renderer/draw_list.hpp"
#include "../../compute/kernels/ray_march.cuh"

struct SceneData {
  DrawList             draw_list;           // raster draw calls
  std::vector<RayMarchParams::SDFObject> sdf_objects;  // CUDA ray march scene
  std::vector<PointLight>                point_lights;
  DirectionalLight                       sun;
  bool                                   has_sun = false;
  // GPU-resident copies — uploaded each frame to shared buffers
  BufferHandle         sdf_objects_buf;
  BufferHandle         point_lights_buf;
};

class SceneBuildSystem {
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Monolithic processAfter/processTick tangles with Transform/Camera/Culling/SceneBuild systems |
| **Keep** | Ordered tick phases; job-parallel where safe |

#### Depends on

- Track A: P3 Jobs; B3.1–B3.2

#### Gates

- [ ] **B3.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.4 — Bounding Volume Hierarchy (BVH)

#### FUSE design

The BVH is the primary spatial acceleration structure for rendering culling, ray queries, and physics broad-phase overlap tests. Leaf nodes can be triangles, SDF primitives, or voxel chunks — the traversal algorithm is agnostic.

#### BVH Node Layout

```cpp
// spatial/bvh.hpp
#pragma once
#include "../core/math/vec.hpp"
#include "../core/types.hpp"

// AABB — axis-aligned bounding box
struct AABB {
  vec3 min, max;

  FUSE_HOST_DEVICE vec3  center()   const { return (min + max) * 0.5f; }
  FUSE_HOST_DEVICE vec3  extents()  const { return (max - min) * 0.5f; }
  FUSE_HOST_DEVICE f32   surface_area() const;
  FUSE_HOST_DEVICE bool  contains(vec3 p) const;
  FUSE_HOST_DEVICE bool  overlaps(const AABB& b) const;
  FUSE_HOST_DEVICE AABB  merge(const AABB& b) const;
  FUSE_HOST_DEVICE f32   ray_intersect(vec3 ro, vec3 inv_rd) const;  // returns t or -1
};

    // … truncated in master plan (full sketch in phase source)
```

#### BVH Builder

```cpp
// spatial/bvh_builder.hpp
#pragma once
#include "bvh.hpp"
#include <span>

struct BVHBuildDesc {
  u32  max_leaf_primitives = 4;
  bool use_sah             = true;    // Surface Area Heuristic — O(n log n) build
  bool parallel            = true;    // parallel build via job system
};

class BVH {
public:
  void build(std::span<BVHLeaf> leaves, const BVHBuildDesc& desc = {});
  void refit();   // O(n) refit for small deformations — no full rebuild

  // O(log n + k) queries
  bool         ray_cast(vec3 ro, vec3 rd, f32 max_t, BVHLeaf& hit, f32& t) const;
    // … truncated in master plan (full sketch in phase source)
```

#### SAH Split Algorithm

```cpp
u32 BVH::build_recursive(std::span<BVHLeaf> leaves, u32 depth) {
  u32 node_idx = (u32)nodes_.size();
  nodes_.push_back({});
  BVHNode& node = nodes_[node_idx];

  // Compute enclosing AABB
  node.aabb = leaves[0].aabb;
  for (auto& l : leaves.subspan(1)) node.aabb = node.aabb.merge(l.aabb);

  // Leaf if small enough or max depth
  if (leaves.size() <= desc_.max_leaf_primitives || depth > 32) {
    node.leaf_count = (u16)leaves.size();
    // store leaves inline
    return node_idx;
  }

  // SAH: try all 3 axes, pick lowest cost split
  f32 best_cost  = std::numeric_limits<f32>::max();
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Informal spatial queries with SAH BVH (CPU build/refit + GPU upload) |
| **Keep** | Ray/AABB/sphere/frustum query *capabilities* for gameplay |

#### Depends on

- Track A: B1 math; B3.1

#### Gates

- [ ] **B3.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.5 — Sparse Voxel Octree (SVO)

#### FUSE design

The SVO is the secondary spatial structure — used for voxel-based objects, destruction, and global illumination probe placement. Coexists with the BVH. The BVH contains SVO chunks as leaf nodes.

#### SVO Node

```cpp
// spatial/svo.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"

// Each node covers a cubic region of space
// Internal nodes have 8 children (octants)
// Leaf nodes store voxel data
struct SVONode {
  u32  children[8];    // indices into node pool, 0 = empty
  u32  parent;
  u8   depth;
  bool is_leaf;
  // Leaf data — 8 voxels (2×2×2 sub-grid at leaf level)
  u32  voxel_data;     // packed RGBA, or index into voxel material table
  // SDF value stored at this node — enables SDF-voxel hybrid
  f32  sdf_value;
};
    // … truncated in master plan (full sketch in phase source)
```

#### SVO-SDF Hybrid

The SVO stores a distance field value at each leaf node. This gives you the best of both worlds: discrete voxel representation for destruction and collision, smooth SDF surface for rendering and exact contact normals for physics.

```cpp
// SDF query via SVO — trilinear interpolation of stored leaf SDF values
f32 SVO::sdf_query(vec3 world_pos) const {
  // Walk tree to find leaf at world_pos
  // Trilinearly interpolate SDF values from 8 surrounding leaf nodes
  // Falls back to analytic sphere for empty regions
}

// Carve a sphere from the SVO — updates SDF values at affected leaves
void SVO::carve(vec3 center, f32 radius) {
  // Find all leaves within (center, radius + leaf_size)
  // For each: new_sdf = op_smooth_union(leaf.sdf, SDF::sphere(leaf_pos - center, radius))
  // If new_sdf > 0 and was <= 0: leaf transitions surface→empty, remove child
  // If new_sdf <= 0 and was > 0: leaf transitions empty→surface, add child
}
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | Sparse Voxel Octree (carve, dual-contour, SDF query) — new vs classic Torque |
| **Compat** | Optional bridge from Torque voxel/terrain chunks later |

#### Depends on

- Track A: B1 allocators; B3.4 for leaf typing

#### Gates

- [ ] **B3.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.6 — Scene Manager

#### FUSE design

The Scene is the top-level container. It owns the Registry, BVH, SVO, and the active camera. It provides the interface the renderer and physics system query each frame.

#### Scene

```cpp
// scene/scene.hpp
#pragma once
#include "../ecs/entity.hpp"
#include "../spatial/bvh.hpp"
#include "../spatial/svo.hpp"
#include "../renderer/draw_list.hpp"
#include "../core/types.hpp"

struct SceneDesc {
  const char* name       = "Untitled";
  f32         world_size = 4096.f;    // total world bounds side length
  u32         svo_depth  = 10;        // 1024³ voxel resolution at leaf level
  bool        has_voxels = true;
};

class Scene {
public:
  void init(const SceneDesc& desc, Allocator& alloc);
    // … truncated in master plan (full sketch in phase source)
```

#### Scene Update Order

```cpp
void Scene::update(f32 dt) {
  // Systems run in dependency order via job system
  // Dependencies declared explicitly — parallelise where safe

  JobCounter transforms_done;
  JobScheduler::submit({
    .fn = [&]{ TransformSystem::update(registry_); },
    .counter = &transforms_done,
    .tag = "TransformSystem"
  });
  transforms_done.wait();

  // Camera update depends on transforms
  JobCounter cameras_done;
  JobScheduler::submit({
    .fn = [&]{ CameraSystem::update(registry_); },
    .counter = &cameras_done,
    .tag = "CameraSystem"
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Informal scene lists / mission group roots with `fuse::Scene` owning Registry+BVH+SVO+camera |
| **Keep** | Active camera / load level workflows |

#### Depends on

- Track A: B3.1–B3.5; P4

#### Gates

- [ ] **B3.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.7 — Scene Serialisation

#### FUSE design

Custom binary format — no JSON, no XML at runtime. Fast load, deterministic, version-tagged.

#### Binary Scene Format

```
SCENE FILE FORMAT v1
─────────────────────────────────────────────────────
Header (64 bytes)
  magic:          u32    = 0x454E4743  ('ENGC')
  version:        u32    = 1
  entity_count:   u32
  archetype_count:u32
  asset_count:    u32
  svo_present:    u32    = 0 or 1
  reserved:       u8[40]

Asset Table
  [asset_count × AssetRef]
  path_hash:  u64
  type:       u32   (mesh=0, texture=1, material=2)
  flags:      u32

Archetype Blocks
    // … truncated in master plan (full sketch in phase source)
```

```cpp
// scene/serialiser.hpp
#pragma once
#include "scene.hpp"

class SceneSerialiser {
public:
  static bool save(const Scene& scene, const char* path);
  static bool load(const char* path, Scene& scene, ResourceManager& resources);

  // Async load — returns immediately, calls on_complete when done
  static void load_async(const char* path, Scene& scene, ResourceManager& resources,
                         std::function<void(bool success)> on_complete);

private:
  static constexpr u32 MAGIC   = 0x454E4743;
  static constexpr u32 VERSION = 1;
};
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary save format with versioned binary scene (magic/version) |
| **Compat** | `.mis` importer → entities/components; old magic in compat loader |
| **Keep** | Async load behaviour |

#### Depends on

- Track A: B3.6; P4 golden .mis gate

#### Gates

- [ ] **B3.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.8 — Camera System

#### FUSE design

```cpp
// ecs/systems/camera_system.hpp
#pragma once
#include "../entity.hpp"

class CameraSystem {
public:
  // Recomputes view, projection, view_projection, frustum planes for active camera
  static void update(Registry& reg);

  // Free-camera controller — used in editor and debug builds
  static void update_free_camera(Registry& reg, const InputState& input, f32 dt,
                                 f32 move_speed = 10.f, f32 look_sensitivity = 0.2f);

private:
  static Camera::Frustum compute_frustum(const mat4& vp);
};

void CameraSystem::update(Registry& reg) {
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Camera control internals with FUSE Camera component (reversed-Z frustum) |
| **Keep** | Free-cam / game-cam behavioural expectations |

#### Depends on

- Track A: B3.2; B1 math

#### Gates

- [ ] **B3.8** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B3.9 — Phase 3 Deliverables & Test Suite

#### FUSE design

#### ECS

#### Spatial Structures

#### Scene

#### Integration

#### Performance Baselines

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Full Phase 3 checklist under FUSE naming + ASan golden mission still green |

#### Depends on

- Track A: P4 + B3 complete

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] Registry creates and destroys 1M entities — no leaks, generation counter correctly invalidates stale handles
- [ ] Archetype storage correctly groups entities by component set — verified by checking column layout after add/remove
- [ ] `each<T>` iterates exactly the correct entities — no missed entities, no spurious iterations
- [ ] `each_parallel<T>` produces identical results to `each<T>` across 100 randomised test cases
- [ ] Component add/remove triggers archetype migration correctly — entity moves to new archetype, data preserved
- [ ] 100k entities with Transform + Mesh + RigidBody iterated at > 500M components/sec on a single thread
- [ ] CUDA kernel reads Transform positions from managed-memory ECS column — verified with device-side assert
- [ ] BVH SAH build on 100k random AABBs completes in < 500ms
- [ ] BVH ray cast returns correct closest hit for 100k random rays against 10k objects — verified against brute-force
- [ ] BVH frustum query returns identical results to O(n) brute-force frustum test on 10k objects
- [ ] BVH refit after 1k transform updates is correct — no stale bounds
- [ ] SVO insert/get round-trips correctly for 1M voxels at depth 10
- [ ] SVO ray cast matches brute-force voxel traversal for 10k random rays
- [ ] SVO carve produces correct surface voxel transitions — verified by re-querying carved region
- [ ] SDF query on SVO returns smooth values at leaf boundaries — no discontinuities
- [ ] Scene creates 1000 mesh + SDF entities and builds render data in < 1ms
- [ ] Active camera frustum correctly culls out-of-view entities — verified by checking draw list count vs total entity count
- [ ] Scene save/load round-trips 10k entities with zero data loss — byte-identical component arrays
- [ ] Async scene load completes and calls callback on the main thread
- [ ] SceneData SDF object buffer uploads to GPU and ray marcher renders correct scene
- [ ] Renderer receives SceneData from scene and renders 500 SDF objects at 1080p > 60fps
- [ ] Adding and removing entities mid-frame does not corrupt the BVH or draw list
- [ ] Free camera moves through the scene with correct frustum culling visible in draw call count
- [ ] RenderDoc capture shows correct draw list ordering — sorted by material, no redundant state changes
- [ ] 10k entity transform update < 1ms on all cores via `each_parallel`
- [ ] BVH frustum cull of 10k objects < 0.1ms
- [ ] Full scene build (cull → draw list → SDF object buffer) < 2ms for 1k entities
- [ ] SVO ray cast for 1M rays < 10ms on CUDA (RTX 3090) — verified with CUDA event timing

---

#### What Phase 4 / B4 Builds On This

Phase 3 delivers the data model: entities, components, spatial structures, and the scene representation that feeds the renderer. Phase 4 builds the physics engine on top of this foundation — rigid bodies, collision detection using the BVH and analytic SDF shapes, a GPU-parallel constraint solver, and voxel destruction via SVO carving. For the first time the world will move, collide, and break.

*Bridge:* After **B3** gates are green (and required Track A milestones), begin **B4** on FUSE APIs only.

## B4 — Physics Engine

**Duration:** 5–6 weeks  
**Depends on (phase-level):** B3 (ECS + BVH + SVO); P1 GPU allocators; CUDA stream manager from B2

*Full fidelity: every source `## 4.*` → `### B4.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B4.1 — Physics Architecture

#### FUSE design

#### Design Philosophy

Traditional physics engines (Bullet, PhysX, Havok) are CPU-centric with GPU acceleration bolted on. This engine inverts that model. The simulation is a CUDA-first pipeline. The CPU submits work, the GPU owns the data and runs the solver. Results are written back to ECS component arrays (managed memory) and consumed by the renderer without an explicit readback.

#### Pipeline Per Frame

```
CPU Frame N:
├── Collect RigidBody + Transform components → GPU integration buffers
├── Submit BroadPhase kernel (spatial hash)
├── Submit NarrowPhase kernel (analytic + GJK)
├── Submit ConstraintSolver kernels (PBD iterations)
├── Submit IntegrateState kernel
├── Signal physics_done semaphore
└── Continue with render submission

GPU (CUDA Physics Stream):
├── BroadPhaseKernel   — O(n) spatial hash, produces candidate pairs
├── NarrowPhaseKernel  — O(k) exact collision, produces contact manifolds
├── PBDSolverKernel    — N iterations of position-based constraint resolution
├── IntegrateKernel    — Euler integration, damping, sleep detection
└── WriteBackKernel    — update managed-memory Transform + RigidBody arrays
```

#### Data Layout

All physics data is stored in Structure-of-Arrays (SoA) layout for coalesced GPU memory access. Never Array-of-Structures in hot paths.

```cpp
// physics/physics_data.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"

// SoA rigid body state — GPU resident, managed memory
struct RigidBodySoA {
  // Position and orientation
  vec3*  positions;           // world-space centre of mass
  quat*  orientations;
  vec3*  linear_velocities;
  vec3*  angular_velocities;

  // Mass properties
  f32*   inv_masses;          // 0 = static
  mat3*  inv_inertia_world;   // inertia tensor in world space

  // Forces accumulated this frame
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary collision/dynamics path with FUSE CUDA-first SoA physics (or dual until parity) |
| **Remove** | CPU-only bottleneck assumptions for sim hot path |
| **Keep** | High-level gameplay hooks (triggers, impulses) via FUSE APIs |

#### Depends on

- Track A: B3 ECS; B2 CUDA streams; P1 GPU allocators

#### Gates

- [ ] **B4.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.2 — Broad Phase: Spatial Hash

#### FUSE design

The broad phase finds candidate collision pairs in O(n). No BVH traversal here — the spatial hash is faster for large uniform distributions of dynamic bodies and maps perfectly to CUDA's parallel model.

#### Spatial Hash Algorithm

```cpp
// physics/broadphase/spatial_hash.cuh
#pragma once
#include "../../core/types.hpp"
#include "../../core/math/vec.hpp"
#include "../physics_data.hpp"

struct SpatialHashParams {
  f32  cell_size;          // should be ~ 2× average body radius
  u32  table_size;         // prime number, >= 2 × body count
  u32  body_count;
};

struct CandidatePair {
  u32 body_a;
  u32 body_b;
};

// Phase 1: hash each body's AABB into overlapping cells
    // … truncated in master plan (full sketch in phase source)
```

#### Hash Function

```cpp
// Spatial hash — maps 3D cell coords to table index
FUSE_HOST_DEVICE inline u32
spatial_hash(i32 cx, i32 cy, i32 cz, u32 table_size) {
  // Large primes for good distribution
  constexpr u32 p1 = 73856093u;
  constexpr u32 p2 = 19349663u;
  constexpr u32 p3 = 83492791u;
  return ((u32)(cx * p1) ^ (u32)(cy * p2) ^ (u32)(cz * p3)) % table_size;
}

FUSE_HOST_DEVICE inline ivec3
world_to_cell(vec3 pos, f32 cell_size) {
  return { (i32)floorf(pos.x / cell_size),
           (i32)floorf(pos.y / cell_size),
           (i32)floorf(pos.z / cell_size) };
}
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Legacy broadphase structures with GPU spatial hash + radix sort candidates |
| **Compat** | Map legacy collision world queries to FUSE broadphase results |

#### Depends on

- Track A: B4.1

#### Gates

- [ ] **B4.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.3 — Narrow Phase: Exact Collision Detection

#### FUSE design

For each candidate pair from the broad phase, compute the exact contact manifold. Use closed-form analytic solutions where available — only fall back to iterative algorithms (GJK/EPA) for convex hulls.

#### Analytic Collision Dispatch

```cpp
// physics/narrowphase/collision_dispatch.cuh
#pragma once
#include "../../core/types.hpp"
#include "../../core/math/vec.hpp"
#include "../physics_data.hpp"

// Contact manifold — result of narrow phase for one pair
struct ContactManifold {
  vec3  contact_point;      // world-space point on body A surface
  vec3  contact_normal;     // from body B toward body A
  f32   penetration_depth;  // positive = overlapping
  u32   body_a, body_b;
  bool  valid;
};

// Closed-form sphere-sphere — exact, no iteration
FUSE_HOST_DEVICE inline ContactManifold
collide_sphere_sphere(vec3 pos_a, f32 r_a, vec3 pos_b, f32 r_b,
    // … truncated in master plan (full sketch in phase source)
```

#### GJK Implementation

```cpp
// physics/narrowphase/gjk.cuh

// Support function — furthest point in direction d
FUSE_HOST_DEVICE inline vec3
support(const vec3* verts, u32 count, vec3 d) {
  f32  max_dot = -1e30f;
  vec3 best    = verts[0];
  for (u32 i = 0; i < count; i++) {
    f32 dot = verts[i].dot(d);
    if (dot > max_dot) { max_dot = dot; best = verts[i]; }
  }
  return best;
}

// Minkowski difference support
FUSE_HOST_DEVICE inline vec3
minkowski_support(const vec3* a, u32 na, const mat4& ta,
                  const vec3* b, u32 nb, const mat4& tb, vec3 d) {
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Approximate mesh soup defaults with analytic sphere/plane/capsule/box + SDF gradient + GJK/EPA |
| **Compat** | Legacy collision types → `CollisionShapeSoA` where feasible |

#### Depends on

- Track A: B4.2; B1 SDF math

#### Gates

- [ ] **B4.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.4 — Constraint Solver: Position Based Dynamics

#### FUSE design

PBD (Position Based Dynamics) is chosen over traditional impulse-based solvers because it is unconditionally stable, parallelises naturally on CUDA, and produces visually plausible results with a low iteration count. Each constraint directly corrects positions rather than accumulating impulses — no velocity-level instability.

#### PBD Overview

```
Each frame:
1. Apply external forces → predict new positions
2. For N iterations:
   a. For each contact constraint: project positions apart (parallel)
   b. For each joint constraint: project joint limits (parallel)
   c. For each friction constraint: apply tangential correction
3. Update velocities from position deltas
4. Apply damping and sleep detection
```

#### PBD Kernels

```cpp
// physics/solver/pbd_solver.cuh
#pragma once
#include "../physics_data.hpp"
#include "../narrowphase/collision_dispatch.cuh"

// Step 1: Semi-implicit Euler predict
__global__ void pbd_predict_kernel(
  RigidBodySoA  bodies,
  f32           dt,
  vec3          gravity
);

// Step 2a: Resolve contact constraints — parallel over all contacts
// Each thread handles one contact manifold
__global__ void pbd_resolve_contacts_kernel(
  RigidBodySoA           bodies,
  const ContactManifold* manifolds,
  u32                    manifold_count,
    // … truncated in master plan (full sketch in phase source)
```

#### XPBD Contact Resolution

Extended PBD (XPBD) adds a compliance parameter that gives physically correct stiffness scaling independent of timestep and iteration count.

```cpp
__device__ void resolve_contact_xpbd(
  RigidBodySoA& bodies,
  const ContactManifold& c,
  f32 dt,
  f32 compliance
) {
  if (!c.valid) return;

  u32 a = c.body_a, b = c.body_b;
  f32 inv_mass_a = bodies.inv_masses[a];
  f32 inv_mass_b = bodies.inv_masses[b];
  f32 w_sum      = inv_mass_a + inv_mass_b;
  if (w_sum < 1e-10f) return;

  // Current separation along normal
  vec3 pa = bodies.predicted_positions[a];
  vec3 pb = bodies.predicted_positions[b];
  vec3 diff = {pa.x-pb.x, pa.y-pb.y, pa.z-pb.z};
    // … truncated in master plan (full sketch in phase source)
```

#### Solver Host Interface

```cpp
// physics/solver/pbd_solver.hpp
#pragma once
#include "../physics_data.hpp"

struct SolverParams {
  u32  iterations       = 10;      // PBD substep iterations
  u32  substeps         = 4;       // substeps per frame (increases stability)
  vec3 gravity          = {0, -9.81f, 0};
  f32  linear_damping   = 0.98f;
  f32  angular_damping  = 0.95f;
  f32  contact_compliance = 0.f;   // 0 = fully rigid
  f32  sleep_linear_threshold  = 0.01f;
  f32  sleep_angular_threshold = 0.01f;
  f32  sleep_time_required     = 0.5f;
};

class PBDSolver {
public:
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary constraint solver with XPBD/PBD contacts + distance constraints |
| **Keep** | Sleeping / kinematic / static flags semantics |

#### Depends on

- Track A: B4.3

#### Gates

- [ ] **B4.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.5 — Barnes-Hut N-Body (Gravity & Force Fields)

#### FUSE design

For scenes requiring n-body gravitational or force-field simulation (space, large-scale fluid, particle systems with mutual attraction) the O(n²) naive approach is replaced with Barnes-Hut at O(n log n).

#### Barnes-Hut Tree (CUDA)

```cpp
// physics/nbody/barnes_hut.cuh
#pragma once
#include "../../core/types.hpp"
#include "../../core/math/vec.hpp"

// Linearised octree node for GPU traversal (no pointer chasing)
struct BHNode {
  vec3  center_of_mass;
  f32   total_mass;
  vec3  min_bounds, max_bounds;
  u32   first_child;    // index of first child, or body index if leaf
  u32   body_count;     // 0 = internal, 1 = leaf body, >1 = internal with mass
};

struct BHParams {
  f32  theta           = 0.5f;   // Barnes-Hut opening angle — lower = more accurate
  f32  gravitational_G = 6.674e-11f;
  f32  softening       = 0.1f;   // softening length to avoid singularity
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | Barnes-Hut n-body (opt-in) — new capability |
| **Compat** | N/A |

#### Depends on

- Track A: B4.1 CUDA jobs

#### Gates

- [ ] **B4.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.6 — Continuous Collision Detection

#### FUSE design

For fast-moving objects, discrete collision detection misses contacts between frames (tunnelling). CCD sweeps the shape's trajectory and finds the earliest time-of-impact.

```cpp
// physics/ccd/ccd.cuh
#pragma once
#include "../physics_data.hpp"

struct TOIResult {
  f32  toi;           // time of impact in [0,1] — fraction of frame timestep
  vec3 contact_point;
  vec3 contact_normal;
  u32  body_a, body_b;
  bool valid;
};

// Sphere swept against sphere — exact closed-form TOI
FUSE_HOST_DEVICE inline TOIResult
swept_sphere_sphere(
  vec3 pos_a0, vec3 vel_a, f32 ra,
  vec3 pos_b0, vec3 vel_b, f32 rb
) {
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** / **Add** | Swept CCD TOI to prevent tunnelling on fast bodies |
| **Keep** | Gameplay expectation that bullets/fast movers hit |

#### Depends on

- Track A: B4.3–B4.4

#### Gates

- [ ] **B4.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.7 — Voxel Destruction

#### FUSE design

Destruction is the killer feature of the SVO. When a rigid body collides with a voxel entity above a destruction threshold, a sphere-carve is issued to the SVO, surface voxels are extracted as a new mesh, and the debris becomes new rigid bodies.

#### Destruction Pipeline

```cpp
// physics/destruction/voxel_destruction.hpp
#pragma once
#include "../../spatial/svo.hpp"
#include "../../ecs/entity.hpp"

struct DestructionEvent {
  EntityID    target;          // entity with SVO component
  vec3        impact_point;    // world-space impact centre
  vec3        impact_normal;
  f32         impulse;         // magnitude — determines carve radius
  f32         carve_radius;    // derived from impulse + material hardness
};

struct VoxelMaterial {
  f32  hardness;           // impulse threshold to destroy
  f32  density;            // affects debris mass
  u32  debris_material_id; // renderer material for debris chunks
  bool breakable;
    // … truncated in master plan (full sketch in phase source)
```

#### Debris Spawning

```cpp
void DestructionSystem::apply_destruction(
  const DestructionEvent& e,
  SVO& svo,
  Registry& reg,
  ResourceManager& resources
) {
  // 1. Carve sphere from SVO
  svo.carve(e.impact_point, e.carve_radius);

  // 2. Extract changed surface region as mesh (dual contouring)
  AABB affected_region = {
    {e.impact_point.x - e.carve_radius * 2,
     e.impact_point.y - e.carve_radius * 2,
     e.impact_point.z - e.carve_radius * 2},
    {e.impact_point.x + e.carve_radius * 2,
     e.impact_point.y + e.carve_radius * 2,
     e.impact_point.z + e.carve_radius * 2}
  };
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | SVO voxel destruction → dual-contour debris rigid bodies |
| **Depends** | B3.5 SVO |

#### Depends on

- Track A: B3.5; B4.4

#### Gates

- [ ] **B4.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.8 — Soft Body & Cloth

#### FUSE design

PBD handles soft bodies naturally — just add distance constraints between adjacent particles with non-zero compliance.

#### Particle System

```cpp
// physics/softbody/particle_system.hpp
#pragma once
#include "../../core/types.hpp"
#include "../../core/math/vec.hpp"

// GPU-resident particle arrays (SoA)
struct ParticleSoA {
  vec3* positions;
  vec3* prev_positions;
  vec3* velocities;
  f32*  inv_masses;
  u32   count;

  static ParticleSoA allocate(u32 capacity);
  static void        free(ParticleSoA& p);
};

// Distance constraint between two particles
    // … truncated in master plan (full sketch in phase source)
```

#### Cloth Solver Kernel

```cpp
// physics/softbody/cloth_kernels.cuh

__global__ void cloth_predict_kernel(ParticleSoA p, f32 dt, vec3 gravity) {
  u32 i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.count) return;
  if (p.inv_masses[i] == 0.f) return;  // pinned particle

  p.velocities[i].x += gravity.x * dt;
  p.velocities[i].y += gravity.y * dt;
  p.velocities[i].z += gravity.z * dt;
  p.prev_positions[i] = p.positions[i];
  p.positions[i].x   += p.velocities[i].x * dt;
  p.positions[i].y   += p.velocities[i].y * dt;
  p.positions[i].z   += p.velocities[i].z * dt;
}

__global__ void cloth_solve_constraints_kernel(
  ParticleSoA p,
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | Cloth/soft-body particle XPBD |
| **Compat** | Optional later mapping of any Torque cloth prototypes |

#### Depends on

- Track A: B4.4

#### Gates

- [ ] **B4.8** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.9 — Physics Manager & ECS Integration

#### FUSE design

The PhysicsManager bridges ECS components and the low-level GPU solvers. Each frame it syncs ECS data into the SoA buffers, runs the solver, and writes results back.

```cpp
// physics/physics_manager.hpp
#pragma once
#include "physics_data.hpp"
#include "solver/pbd_solver.hpp"
#include "broadphase/spatial_hash.cuh"
#include "../ecs/entity.hpp"
#include "../compute/stream_manager.hpp"

struct PhysicsManagerDesc {
  u32  max_bodies      = 65536;
  u32  max_contacts    = 262144;
  u32  max_constraints = 131072;
  SolverParams solver;
  BHParams nbody;
  bool enable_ccd      = true;
  bool enable_nbody    = false;  // opt-in — expensive
  bool enable_destruction = true;
};
    // … truncated in master plan (full sketch in phase source)
```

#### Frame Step Implementation

```cpp
void PhysicsManager::step(Registry& registry, f32 dt, StreamManager& streams) {
  PROFILE_SCOPE("PhysicsManager::step");
  cudaStream_t s = streams.get(CUDAStream::Physics);

  // 1. Sync ECS → SoA (managed memory — no explicit copy needed if ECS is managed)
  sync_ecs_to_soa_(registry);

  // 2. Run full solver on physics stream
  solver_.step(soa_, shapes_, desc_.solver, dt, s);

  // 3. Optional n-body forces
  if (desc_.enable_nbody) {
    compute_nbody_forces(soa_.positions, soa_.inv_masses,  // 1/mass → compute mass
                         soa_.count, soa_.forces,
                         desc_.nbody, s);
  }

  // 4. Write results back to ECS (async — stream ordering guarantees correctness)
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque physics tick integration with `PhysicsManager` ECS ↔ SoA sync |
| **Keep** | Component-facing velocity/impulse APIs |

#### Depends on

- Track A: B3 systems; B4.1–B4.8

#### Gates

- [ ] **B4.9** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.10 — Collision Callbacks & Event System

#### FUSE design

```cpp
// physics/events/collision_events.hpp
#pragma once
#include "../../ecs/entity.hpp"

enum class CollisionEventType : u8 {
  Enter,    // first frame of contact
  Stay,     // persisting contact
  Exit,     // contact ended
  Trigger,  // trigger volume overlap (no response)
};

struct CollisionEvent {
  CollisionEventType type;
  EntityID           entity_a;
  EntityID           entity_b;
  vec3               contact_point;
  vec3               contact_normal;
  f32                impulse;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Legacy collision callbacks with Enter/Stay/Exit/Trigger event bus |
| **Keep** | Trigger volume gameplay patterns |

#### Depends on

- Track A: B4.9

#### Gates

- [ ] **B4.10** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B4.11 — Phase 4 Deliverables & Test Suite

#### FUSE design

#### Broad Phase

#### Narrow Phase

#### Solver

#### CCD

#### Destruction

#### Soft Body

#### Integration

#### Performance Baselines (RTX 3090)

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Full Phase 4 deliverable/test checklist under FUSE naming |

#### Depends on

- Track A: B3 + B4 complete

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] Spatial hash correctly identifies all overlapping pairs for 10k random spheres — verified against brute force O(n²)
- [ ] No missed pairs for bodies straddling multiple cells — edge case tested with grid-aligned bodies
- [ ] GPU radix sort produces correctly sorted (key, value) pairs — verified with reference CPU sort
- [ ] Broad phase runs in < 2ms for 10k bodies on RTX 3090 — measured with CUDA events
- [ ] Sphere-sphere analytic result matches Bullet reference to within 0.001f
- [ ] Sphere-plane produces correct normal and penetration depth at all angles
- [ ] Capsule-capsule handles parallel capsules and endpoint degeneracies correctly
- [ ] GJK returns correct intersection result for 10k random convex hull pairs — verified against SAT reference
- [ ] EPA returns penetration depth within 0.01f of reference for all test cases
- [ ] SDF collision produces smooth contact normals — no discontinuity at surface transitions
- [ ] Narrow phase runs in < 3ms for 1k contact pairs on RTX 3090
- [ ] Single sphere under gravity hits ground plane at correct time (analytical reference: t = √(2h/g))
- [ ] Stack of 10 spheres remains stable at rest after 5 seconds of simulation — no drift or explosion
- [ ] Restitution correctly produces elastic bounce (coefficient 1.0 → equal rebound height)
- [ ] Friction correctly stops a sliding box at expected distance — matches analytical result
- [ ] Distance constraint holds two bodies at rest_length ± 0.01f under external force
- [ ] Constraint solver runs 10 iterations over 10k contacts in < 5ms on RTX 3090
- [ ] Sleep detection correctly deactivates resting bodies — confirmed by zero velocity reads
- [ ] High-velocity sphere (100 m/s) does not tunnel through a 0.1m wall — discrete misses, CCD catches
- [ ] TOI binary search converges in < 8 iterations for all test cases
- [ ] CCD introduces < 1ms overhead per frame for 100 fast-moving bodies
- [ ] Sphere carve correctly removes voxels within radius — verified by querying carved region
- [ ] Dual contouring extracts watertight mesh from carved SVO surface
- [ ] Debris entities spawn with correct mass proportional to voxel count
- [ ] Debris rigid bodies collide correctly with scene after spawning
- [ ] 10 simultaneous impacts each spawning 5 debris pieces — no frame spike > 10ms
- [ ] Cloth 32×32 grid simulates under gravity without instability at dt=1/60
- [ ] Pinned corners hold position exactly
- [ ] Wind force deflects cloth in correct direction
- [ ] Cloth-sphere collision resolves without interpenetration
- [ ] PhysicsManager::step completes in < 8ms for 1000 active rigid bodies at 60fps
- [ ] ECS Transform components correctly reflect physics positions every frame
- [ ] `apply_impulse` produces physically plausible velocity change — verified with known mass and impulse
- [ ] CollisionEventSystem dispatches Enter/Exit events correctly — no missed or spurious callbacks
- [ ] Kinematic body moves along programmed path, correctly pushes dynamic bodies
- [ ] 1000 dynamic rigid bodies, full pipeline (broad + narrow + 10 PBD iters + integrate): < 4ms
- [ ] 10k sleeping bodies: < 0.5ms (sleep check only)
- [ ] 64×64 cloth simulation: < 1ms
- [ ] 5 simultaneous destruction events with 10 debris each: < 16ms total

---

#### What Phase 5 / B5 Builds On This

Phase 4 delivers a physics world that moves, collides, and breaks. Phase 5 is the full rendering upgrade — the advanced lighting model, PBR materials, global illumination, shadow systems, post-processing, and the complete visual pipeline that turns FUSE from technically functional into visually competitive with Unreal and CryEngine. Every physics state from Phase 4 will feed into the renderer — contact sparks, destruction debris, cloth deformation, all rendered with exact analytic lighting and SDF-accurate shadows.

*Bridge:* After **B4** gates are green (and required Track A milestones), begin **B5** on FUSE APIs only.

## B5 — Advanced Rendering & Lighting

**Duration:** 6–7 weeks  
**Depends on (phase-level):** B2 hybrid frame stable; B3 scene; B4 optional for dynamic lighting stress

*Full fidelity: every source `## 5.*` → `### B5.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B5.1 — Rendering Architecture Upgrade

#### FUSE design

Phase 2 built a thin hybrid rasteriser. Phase 5 replaces the placeholder shading with a production-grade deferred pipeline and expands the CUDA ray marcher into a full lighting and GI system.

#### Full Frame Pipeline

```
Frame N:

[Vulkan — Graphics Queue]
├── PASS 1: Depth Prepass
│   └── Render all opaque geometry to depth buffer only (no shading)
│       Early-Z rejection eliminates overdraw in G-buffer pass
│
├── PASS 2: G-Buffer Pass
│   └── Render opaque geometry → normal, albedo, roughness, metallic, velocity, depth
│
├── PASS 3: Shadow Maps (cascaded, for triangle geometry)
│   └── 4 cascades rendered for directional light
│
└── Signal(vk_to_cuda, frame_N)

[CUDA — Compute Stream: Render]
├── SDF Ray March    — exact surface detail, fills gaps in raster
├── DDGI Probe Update — irradiance probes via ray casting
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Forward/legacy frame graph with production deferred + CUDA lighting stack |
| **Remove** | Per-draw legacy fixed pipeline state as default |

#### Depends on

- Track A: B2 hybrid frame stable

#### Gates

- [ ] **B5.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.2 — G-Buffer Layout (Revised)

#### FUSE design

Expanded from Phase 2 to carry all data needed for deferred PBR shading.

```
RT0: RGBA16F   — World-space normal (xyz, oct-encoded) | AO (w)
RT1: RGBA8     — Albedo/base colour (rgb) | Alpha (a)
RT2: RGBA8     — Roughness (r) | Metallic (g) | Emissive mask (b) | Shading model ID (a)
RT3: RG16F     — Screen-space velocity (motion vectors for TAA)
RT4: R32F      — Depth (reversed-Z, 32-bit float)
RT5: RGBA16F   — Emissive colour (rgb) | unused (w)  — only written by emissive surfaces
```

#### G-Buffer Encoding Helpers

```glsl
// shaders/common/gbuffer.glsl

// Octahedral normal encoding — 2 floats instead of 3, fully reversible
vec2 encode_normal(vec3 n) {
  n /= (abs(n.x) + abs(n.y) + abs(n.z));
  vec2 o = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
  return o * 0.5 + 0.5;
}

vec3 decode_normal(vec2 enc) {
  enc = enc * 2.0 - 1.0;
  vec3 n = vec3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
  if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
  return normalize(n);
}

// Pack G-buffer from fragment shader outputs
void write_gbuffer(
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ad-hoc MRT layouts with revised G-buffer (oct normals, albedo, rough/metal, velocity, depth, emissive) |
| **Keep** | Debug visualization hooks for buffer channels |

#### Depends on

- Track A: B2.8 raster

#### Gates

- [ ] **B5.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.3 — PBR Material System

#### FUSE design

#### BRDF — GGX Microfacet Model

FUSE uses the same BRDF model as Unreal Engine 4 — GGX specular with Smith masking, Lambert diffuse — but implemented analytically with no texture-baked approximations.

```glsl
// shaders/common/brdf.glsl

const float PI = 3.14159265358979;

// GGX normal distribution function — D term
float D_GGX(float NoH, float roughness) {
  float a  = roughness * roughness;
  float a2 = a * a;
  float d  = (NoH * a2 - NoH) * NoH + 1.0;
  return a2 / (PI * d * d);
}

// Smith masking-shadowing — G term
float G_SmithGGX(float NoV, float NoL, float roughness) {
  float a  = roughness * roughness;
  float gv = NoL * sqrt(NoV * NoV * (1.0 - a) + a);
  float gl = NoV * sqrt(NoL * NoL * (1.0 - a) + a);
  return 0.5 / max(gv + gl, 1e-5);
    // … truncated in master plan (full sketch in phase source)
```

#### Material Table

```cpp
// renderer/material_system.hpp
#pragma once
#include "../core/types.hpp"
#include "../renderer/resources.hpp"

enum class ShadingModel : u8 {
  Opaque         = 0,
  Translucent    = 1,
  Emissive       = 2,
  SubsurfaceSSS  = 3,   // skin, wax, jade
  ClearCoat      = 4,   // car paint
  Cloth          = 5,   // velvet, fabric
};

struct Material {
  // Base colour — either flat value or texture index
  vec3          base_color     = {1, 1, 1};
  TextureHandle base_color_tex = {};       // invalid = use flat value
    // … truncated in master plan (full sketch in phase source)
```

#### Procedural Materials

```cpp
// renderer/procedural_materials.cuh
// Analytic material functions — evaluated per-point, infinite resolution

struct MaterialSample {
  vec3  albedo;
  f32   roughness;
  f32   metallic;
  vec3  emissive;
  vec3  normal_offset;  // tangent-space normal perturbation
};

// Wood grain — analytic, no texture
__device__ MaterialSample mat_wood(vec3 world_pos, u32 seed) {
  f32 ring = sinf(Noise::perlin(world_pos * 0.3f) * 6.28f * 4.f) * 0.5f + 0.5f;
  f32 grain = Noise::fbm(world_pos * 10.f, 4) * 0.1f;
  vec3 base = {0.6f + ring*0.3f, 0.35f + ring*0.15f, 0.1f};
  return { .albedo=base, .roughness=0.6f + grain, .metallic=0.f };
}
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque material models with FUSE PBR (GGX/Smith/Schlick) + bindless material SSBO |
| **Compat** | Torque material names/slots mapped into `MaterialSystem` where possible |
| **Keep** | Texture assets via cook → bindless textures |

#### Depends on

- Track A: B2.3 bindless; B5.2

#### Gates

- [ ] **B5.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.4 — Clustered Deferred Shading

#### FUSE design

O(n) lighting complexity regardless of light count. Screen space divided into a 3D cluster grid. Lights are assigned to clusters. Shading reads only the lights touching the cluster of each pixel.

#### Cluster Grid

```cpp
// renderer/lighting/clustered.hpp
#pragma once
#include "../resources.hpp"

struct ClusterDesc {
  u32  tiles_x      = 16;   // screen subdivisions
  u32  tiles_y      = 9;
  u32  slices_z     = 24;   // depth slices (exponential distribution)
  u32  max_lights_per_cluster = 256;
};

struct ClusterAABB {
  vec3 min_p, max_p;
};

// GPU buffers
struct ClusterBuffers {
  BufferHandle cluster_aabbs;     // ClusterDesc.tiles_x × tiles_y × slices_z AABBs
    // … truncated in master plan (full sketch in phase source)
```

#### Cluster Build Kernel

```cpp
// renderer/lighting/cluster_kernels.cuh

// Build cluster AABBs from camera frustum (once per camera change)
__global__ void build_cluster_aabbs_kernel(
  ClusterAABB* aabbs,
  u32 tiles_x, u32 tiles_y, u32 slices_z,
  mat4 inv_proj,
  f32 near_plane, f32 far_plane,
  u32 screen_width, u32 screen_height
);

// Cull lights against cluster AABBs — one thread per (light × cluster) pair
__global__ void cull_lights_kernel(
  const ClusterAABB*  aabbs,
  u32                 cluster_count,
  const GPUPointLight* lights,
  u32                 light_count,
  u32*                light_list,        // output: flat list
    // … truncated in master plan (full sketch in phase source)
```

#### Deferred Shade Kernel Body

```cpp
__global__ void deferred_shade_kernel(/* params */) {
  u32 px = blockIdx.x * blockDim.x + threadIdx.x;
  u32 py = blockIdx.y * blockDim.y + threadIdx.y;
  if (px >= width || py >= height) return;

  // Reconstruct world position from depth
  float depth;
  surf2Dread(&depth, gbuf_depth, px * sizeof(float), py);
  if (depth <= 0.f) { surf2Dwrite(make_float4(0,0,0,1), output, px*16, py); return; }

  vec2 ndc = {2.f*px/width - 1.f, 1.f - 2.f*py/height};
  vec4 clip = {ndc.x, ndc.y, depth, 1.f};
  vec4 world = inv_view_proj * clip;
  vec3 world_pos = {world.x/world.w, world.y/world.w, world.z/world.w};
  vec3 V = (cam_pos - world_pos).normalized();

  // Read G-buffer
  float4 n_ao  = surf2Dread<float4>(gbuf_normal_ao,          px*16, py);
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Naïve light loops with clustered deferred 3D grid (O(visible lights) per pixel) |
| **Keep** | Many dynamic lights gameplay goal |

#### Depends on

- Track A: B5.2–B5.3; B3 lights

#### Gates

- [ ] **B5.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.5 — Shadow System

#### FUSE design

#### Cascaded Shadow Maps (Directional Light)

```cpp
// renderer/shadows/csm.hpp
#pragma once
#include "../resources.hpp"

static constexpr u32 CSM_CASCADE_COUNT = 4;

struct CSMDesc {
  u32   resolution          = 2048;
  f32   cascade_splits[CSM_CASCADE_COUNT] = {0.05f, 0.15f, 0.4f, 1.0f};  // fraction of far plane
  f32   depth_bias          = 0.005f;
  f32   normal_offset_bias  = 0.01f;
  bool  stabilise           = true;   // snap to texel grid — eliminates shimmering
};

struct CSMData {
  mat4          light_view_proj[CSM_CASCADE_COUNT];
  f32           cascade_far_z[CSM_CASCADE_COUNT];
  TextureHandle shadow_maps[CSM_CASCADE_COUNT];  // R32F depth maps
    // … truncated in master plan (full sketch in phase source)
```

#### SDF Soft Shadows (CUDA)

Exact soft shadows for all SDF geometry — analytically correct penumbra, no filtering hacks.

```cpp
// compute/kernels/sdf_shadows.cuh

// Soft shadow via penumbra ray marching — analytically correct
// Returns shadow factor in [0,1], 1 = fully lit, 0 = fully shadowed
__device__ f32 sdf_soft_shadow(
  auto        scene_sdf,   // callable — same SDF as ray marcher
  vec3        ray_origin,
  vec3        ray_dir,
  f32         t_min,
  f32         t_max,
  f32         penumbra_k   // higher k = sharper shadow
) {
  f32 res  = 1.f;
  f32 t    = t_min;
  f32 prev = 1e30f;

  for (u32 i = 0; i < 64 && t < t_max; i++) {
    vec3 p = {ray_origin.x + ray_dir.x*t,
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Legacy shadow approaches with 4-cascade CSM + analytic SDF soft penumbra |
| **Keep** | Directional sun shadow expectations |

#### Depends on

- Track A: B5.1; B2.7 SDF

#### Gates

- [ ] **B5.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.6 — Global Illumination: DDGI

#### FUSE design

Dynamic Diffuse Global Illumination via irradiance probes. Probes are placed on a 3D grid. Each frame a subset of probes is updated by casting rays from the probe position and accumulating radiance. Probes store spherical harmonic-encoded irradiance — correct low-frequency diffuse GI with zero flickering.

#### Probe Grid

```cpp
// renderer/gi/ddgi.hpp
#pragma once
#include "../resources.hpp"

struct DDGIDesc {
  vec3  grid_origin;
  vec3  probe_spacing     = {2.f, 2.f, 2.f};   // metres between probes
  uvec3 grid_dims         = {16, 8, 16};         // 16×8×16 = 2048 probes
  u32   rays_per_probe    = 256;                 // rays cast per probe per update
  u32   probes_per_frame  = 64;                  // subset updated per frame
  u32   irradiance_res    = 8;                   // texels per probe face (octahedral)
  u32   depth_res         = 16;
  f32   hysteresis        = 0.97f;               // temporal blend — stability vs latency
  f32   max_ray_distance  = 20.f;
};

struct ProbeData {
  TextureHandle irradiance_atlas;   // all probe irradiance (octahedral packed)
    // … truncated in master plan (full sketch in phase source)
```

#### Probe Update Kernel

```cpp
// renderer/gi/ddgi_kernels.cuh

// For each probe being updated this frame:
//   Cast rays_per_probe rays in random hemisphere directions
//   For each ray: trace against SDF scene + fetch radiance from previous frame's probes
//   Accumulate irradiance into spherical harmonic coefficients
//   Blend with previous irradiance via hysteresis

__global__ void probe_trace_kernel(
  u32*            probe_indices_to_update,
  u32             probe_update_count,
  const vec3*     probe_world_positions,
  const RayMarchParams::SDFObject* objects,
  u32             object_count,
  cudaSurfaceObject_t prev_irradiance,    // previous frame's probes — for multi-bounce
  cudaSurfaceObject_t out_radiance,       // per-probe per-ray radiance accumulator
  u32             rays_per_probe,
  u64             frame_seed              // Izaac VRF seed for ray direction sampling
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | DDGI probe grid GI (partial updates, hysteresis, SH/oct irradiance) |
| **Compat** | N/A — new |

#### Depends on

- Track A: B5.4; B2 CUDA

#### Gates

- [ ] **B5.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.7 — Screen-Space Effects (CUDA)

#### FUSE design

#### Horizon-Based Ambient Occlusion (HBAO)

```cpp
// compute/kernels/hbao.cuh

struct HBAOParams {
  f32  radius          = 1.f;      // world-space sample radius
  f32  bias            = 0.1f;     // angle bias to avoid self-occlusion
  u32  directions      = 8;        // angular samples
  u32  steps_per_dir   = 4;        // steps per direction
  f32  strength        = 1.5f;
  f32  max_radius_px   = 64.f;
};

__global__ void hbao_kernel(
  cudaSurfaceObject_t depth_in,
  cudaSurfaceObject_t normal_in,
  cudaSurfaceObject_t ao_out,
  mat4                proj,
  mat4                inv_proj,
  HBAOParams          params,
    // … truncated in master plan (full sketch in phase source)
```

#### Screen-Space Reflections

```cpp
// compute/kernels/ssr.cuh

struct SSRParams {
  u32  max_steps         = 64;
  f32  ray_step_size     = 0.2f;
  f32  thickness         = 0.5f;    // depth test tolerance
  f32  max_distance      = 20.f;
  f32  fade_screen_edge  = 0.1f;    // fade out near screen border
  bool use_hiz           = true;    // hierarchical Z for large steps
};

__global__ void ssr_kernel(
  cudaSurfaceObject_t gbuf_depth,
  cudaSurfaceObject_t gbuf_normal,
  cudaSurfaceObject_t gbuf_roughness,
  cudaSurfaceObject_t scene_color,    // previous frame's resolved colour
  cudaSurfaceObject_t ssr_out,
  mat4                view_proj,
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | CUDA HBAO / SSR (and related screen-space) passes |
| **Keep** | Toggleable quality settings |

#### Depends on

- Track A: B5.2; B2.6

#### Gates

- [ ] **B5.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.8 — Atmosphere & Sky

#### FUSE design

Physically-based sky using Rayleigh and Mie scattering — no cubemap, no texture. Evaluated analytically per pixel. True colour shift at sunrise/sunset from first principles.

```cpp
// renderer/atmosphere/sky.cuh

struct AtmosphereParams {
  f32  earth_radius       = 6371000.f;   // metres
  f32  atmo_radius        = 6471000.f;
  vec3 rayleigh_coeff     = {5.8e-6f, 13.5e-6f, 33.1e-6f};  // RGB
  f32  mie_coeff          = 21e-6f;
  f32  rayleigh_scale_h   = 8000.f;
  f32  mie_scale_h        = 1200.f;
  f32  mie_scatter_dir    = 0.758f;      // Henyey-Greenstein g parameter
  u32  view_samples       = 16;
  u32  light_samples      = 8;
};

// Rayleigh phase function — exact
__device__ f32 rayleigh_phase(f32 cos_theta) {
  return (3.f / (16.f * 3.14159f)) * (1.f + cos_theta * cos_theta);
}
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Cubemap-only sky defaults with analytic Rayleigh/Mie atmosphere |
| **Keep** | Time-of-day / sun direction controls |

#### Depends on

- Track A: B5.1

#### Gates

- [ ] **B5.8** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.9 — Temporal Anti-Aliasing

#### FUSE design

TAA accumulates multiple sub-pixel samples over time, producing noise-free high-quality output with history rejection for fast-moving objects.

```cpp
// renderer/taa/taa.hpp
#pragma once
#include "../resources.hpp"

struct TAAParams {
  f32  blend_factor     = 0.1f;    // current frame weight — lower = more stable
  f32  velocity_rejection= 0.9f;   // velocity-weighted history rejection
  f32  depth_rejection  = 0.1f;    // depth-based disocclusion rejection
  f32  clamp_gamma      = 1.25f;   // variance clamp extent
  bool use_catmull_rom  = true;    // higher quality history sampling
};

class TAA {
public:
  void init(u32 width, u32 height, ResourceManager& resources);
  void destroy();
  void resolve(
    cudaSurfaceObject_t current_frame,
    // … truncated in master plan (full sketch in phase source)
```

#### TAA Resolve Kernel

```cpp
// renderer/taa/taa_kernels.cuh

__global__ void taa_resolve_kernel(
  cudaSurfaceObject_t current,
  cudaSurfaceObject_t history,
  cudaSurfaceObject_t velocity,
  cudaSurfaceObject_t depth,
  cudaSurfaceObject_t output,
  TAAParams           params,
  u32                 width, u32 height
) {
  u32 px = blockIdx.x * blockDim.x + threadIdx.x;
  u32 py = blockIdx.y * blockDim.y + threadIdx.y;
  if (px >= width || py >= height) return;

  float4 cur  = surf2Dread<float4>(current, px*16, py);
  float2 vel  = surf2Dread<float2>(velocity, px*8,  py);

    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | TAA with velocity from G-buffer; anti-ghosting controls |
| **Keep** | Optional FXAA/off modes for debugging |

#### Depends on

- Track A: B5.2 velocity

#### Gates

- [ ] **B5.9** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.10 — Post-Processing Stack

#### FUSE design

All post-processing runs as CUDA compute passes after TAA resolve.

#### Bloom

```cpp
// renderer/postprocess/bloom.cuh

struct BloomParams {
  f32  threshold     = 1.f;      // HDR luminance threshold
  f32  knee          = 0.5f;     // soft knee width
  f32  intensity     = 0.05f;
  u32  mip_levels    = 7;
  f32  scatter       = 0.7f;     // bloom spread
};

// Threshold + dual kawase blur pyramid
void bloom_pass(
  cudaSurfaceObject_t hdr_input,
  cudaSurfaceObject_t output,
  TextureHandle       bloom_mips,   // pre-allocated mip chain
  const BloomParams&  params,
  u32 width, u32 height,
  cudaStream_t stream
    // … truncated in master plan (full sketch in phase source)
```

#### Depth of Field

```cpp
// renderer/postprocess/dof.cuh

struct DOFParams {
  f32  focal_distance = 10.f;    // metres from camera
  f32  focal_length   = 50.f;    // mm
  f32  f_stop         = 2.8f;
  f32  sensor_width   = 36.f;    // mm (full frame)
  u32  bokeh_blades   = 6;       // aperture blade count — affects bokeh shape
  bool near_blur      = true;
};

void dof_pass(
  cudaSurfaceObject_t color_in,
  cudaSurfaceObject_t depth_in,
  cudaSurfaceObject_t output,
  const DOFParams& params,
  f32 near_plane, f32 far_plane,
  u32 width, u32 height,
    // … truncated in master plan (full sketch in phase source)
```

#### Motion Blur

```cpp
// renderer/postprocess/motion_blur.cuh

struct MotionBlurParams {
  u32  max_samples    = 16;
  f32  shutter_angle  = 180.f;  // degrees — 180 = standard cinematic
  f32  max_blur_px    = 32.f;
};

__global__ void motion_blur_kernel(
  cudaSurfaceObject_t color_in,
  cudaSurfaceObject_t velocity_in,
  cudaSurfaceObject_t depth_in,
  cudaSurfaceObject_t output,
  MotionBlurParams    params,
  u32 width, u32 height
);
```

#### Tone Mapping & Colour Grade

```cpp
// renderer/postprocess/tonemapping.cuh

enum class ToneMapper : u8 {
  ACES,       // Academy Color Encoding System — industry standard
  Filmic,     // Hejl-Burgess-Dawson
  Reinhard,   // classic, less contrast
  Neutral,    // no tonemap — for calibration
};

struct ColorGradeParams {
  ToneMapper tone_mapper   = ToneMapper::ACES;
  f32        exposure      = 0.f;       // EV stops
  f32        contrast      = 1.f;
  f32        saturation    = 1.f;
  vec3       lift          = {0,0,0};   // shadow colour offset
  vec3       gamma         = {1,1,1};   // midtone gamma
  vec3       gain          = {1,1,1};   // highlight multiplier
  f32        vignette      = 0.3f;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ad-hoc post chain with bloom, DoF, motion blur, ACES grade stack |
| **Keep** | Exposure / grading knobs (surfaced in Qt later) |

#### Depends on

- Track A: B5.1–B5.9

#### Gates

- [ ] **B5.10** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.11 — Lens Flare & Volumetric Lighting

#### FUSE design

#### Analytic Lens Flare

```cpp
// renderer/postprocess/lens_flare.cuh
// Procedural lens flare — no texture, pure math

struct LensFlareSample {
  vec2  position;    // screen-space position
  f32   size;
  vec3  color;
  f32   intensity;
};

// Generate flare elements from sun screen position
// Each element: ghost, starburst, halo — analytic shapes
void generate_lens_flare(
  vec2  sun_screen_pos,    // NDC
  f32   sun_intensity,
  vec3  sun_color,
  f32   occlusion,         // 0 = occluded, 1 = fully visible — from depth test
  std::vector<LensFlareSample>& out_elements
    // … truncated in master plan (full sketch in phase source)
```

#### Volumetric Fog

```cpp
// renderer/volumetric/volumetric_fog.cuh

struct VolumetricFogParams {
  f32  density              = 0.02f;
  f32  anisotropy           = 0.3f;   // scattering direction bias
  vec3 fog_color            = {0.8f, 0.85f, 0.9f};
  f32  height_falloff       = 0.2f;
  f32  base_height          = 0.f;
  u32  march_steps          = 32;
  bool receive_shadows      = true;
};

// Volumetric ray march along each pixel ray — accumulate in-scattering
__global__ void volumetric_fog_kernel(
  cudaSurfaceObject_t depth_in,
  cudaSurfaceObject_t output,   // additive fog layer — composited after shading
  mat4                inv_view_proj,
  vec3                cam_pos,
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | Lens flare + volumetric lighting passes |
| **Keep** | Artistic toggles |

#### Depends on

- Track A: B5.10; B5.5

#### Gates

- [ ] **B5.11** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B5.12 — Phase 5 Deliverables & Test Suite

#### FUSE design

#### G-Buffer & Materials

#### Lighting

#### Global Illumination

#### Temporal & Screen-Space

#### Atmosphere & Sky

#### Post-Processing

#### Full Frame Performance (RTX 3090, 1920×1080)

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Full Phase 5 checklist; material/exposure debug via Qt (B6), renderer Qt-free |

#### Depends on

- Track A: B2 + B3 + B5

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] G-buffer octahedral normal encoding round-trips with < 0.001 angular error
- [ ] PBR material renders smooth transition from dielectric (metallic=0) to metallic (metallic=1) — no colour discontinuity
- [ ] Procedural wood, metal, and concrete materials evaluate correctly on SDF surfaces — no seams or tiling artifacts
- [ ] Material SSBO bindless lookup correct for 1000 different materials in one frame — verified with RenderDoc
- [ ] Emissive surfaces contribute correct radiance to GI probes
- [ ] Clustered light culler assigns zero lights to clusters with no light overlap — verified by reading light_grid buffer
- [ ] 1000 point lights in scene — deferred shading correct, no light leaking through walls — verified visually
- [ ] Clustered cull + deferred shade runs in < 3ms for 1000 lights at 1080p — measured with CUDA events
- [ ] CSM renders correct shadow for directional light across all 4 cascades — no cascade seam visible
- [ ] CSM stabilisation eliminates shadow shimmer on a static scene — confirmed by frame diff
- [ ] SDF soft shadows produce correct penumbra width proportional to distance to occluder
- [ ] SDF shadow matches reference path tracer within 5% luminance error per pixel
- [ ] DDGI probes initialise and first update completes without CUDA error
- [ ] 2048 probes update 64 per frame at < 2ms per update cycle
- [ ] Irradiance correctly responds to dynamic light changes within 64 frames (hysteresis)
- [ ] Moving the sun rotates GI colour cast correctly — verified by recording 10-second timelapse
- [ ] GI contribution on a white Lambertian surface matches reference Monte Carlo within 10%
- [ ] TAA eliminates aliasing on geometry edges — subpixel detail visible without shimmering
- [ ] TAA history rejection fires on fast-moving objects — no ghost trails at > 10m/s
- [ ] HBAO produces correct occlusion in concave corners — verified against SSAO reference
- [ ] SSR reflects correct colour from reflective floor — matches ray-marched ground truth within 15%
- [ ] Sky renders correct Rayleigh scattering — blue midday, orange/red at low sun angles
- [ ] No banding artifacts in sky gradient — verified at 10-bit display output
- [ ] Sun disk correct angular size (0.5° apparent diameter)
- [ ] Volumetric fog density falloff matches analytic exponential reference
- [ ] Bloom only affects pixels above threshold — black frame with no bright pixels produces zero bloom
- [ ] DOF circle of confusion radius matches thin lens formula for test focal distances
- [ ] Motion blur samples correctly trail in direction of velocity vector
- [ ] ACES tonemap maps 0.18 grey to 0.18 sRGB — reference calibration check
- [ ] Film grain is temporally decorrelated — no fixed pattern visible on static frame
- [ ] Depth prepass: < 0.5ms
- [ ] G-buffer pass (1000 objects): < 2ms
- [ ] SDF ray march (100 objects, 128 steps): < 3ms
- [ ] DDGI probe update (64 probes, 256 rays): < 2ms
- [ ] Deferred shading + clustered lights (1000 lights): < 3ms
- [ ] SDF shadows: < 2ms
- [ ] HBAO: < 1ms
- [ ] SSR: < 1.5ms
- [ ] TAA: < 0.5ms
- [ ] Bloom + DoF + Motion blur + Tonemap: < 1ms
- [ ] **Total GPU frame time: < 16ms (60fps headroom)**

---

#### What Phase 6 / B6 Builds On This

Phase 5 delivers a visually complete engine. Every pixel is shaded correctly, lit physically, anti-aliased temporally, and post-processed with cinematographic precision. Phase 6 builds the editor — the full Qt-based application layer that lets a human actually use everything built so far. Scene hierarchy, property inspector, material editor, SDF primitive sculpting tools, profiler overlay, asset browser, and in-viewport gizmos. FUSE stops being a tech demo and becomes a tool.

*Bridge:* After **B5** gates are green (and required Track A milestones), begin **B6** on FUSE APIs only.

## B6 — Editor (Qt 6 — not ImGui)

**Duration:** 5–6 weeks  
**Depends on (phase-level):** P5 Qt vertical slice; B3 scene serialisation; B5 for full visual fidelity (can start earlier with simpler viewport)

*Full fidelity: every source `## 6.*` → `### B6.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B6.1 — Editor Architecture

#### FUSE design

#### Design Principles

- **Non-destructive** — every editor action goes through a command object. Full undo/redo stack. Nothing mutates engine state directly.
- **Dockable layout** — `QMainWindow` + `QDockWidget` (or Qt Advanced Docking). Panels are dockable, floatable, and closeable. Layout saved and restored per project.
- **Decoupled from engine** — the editor is a layer that sits above FUSE. It reads engine state and submits commands. It does not call engine internals directly. This means the same engine can run headless without the editor layer compiled in.
- **Hot-reload aware** — shader hot-reload, script hot-reload, asset reimport all visible and triggerable from the editor without restarting FUSE.
- **GRIA-aware UI** — alpha values, reversibility parameters, and exactness controls are first-class editor concepts, not hidden internals.

#### Application Structure

```cpp
// editor/editor_app.hpp
#pragma once
#include "../core/types.hpp"
#include "../renderer/renderer.hpp"
#include "../scene/scene.hpp"

// Forward declarations for all panels
class SceneHierarchyPanel;
class InspectorPanel;
class ViewportPanel;
class AssetBrowserPanel;
class MaterialEditorPanel;
class SDFSculptPanel;
class ProfilerPanel;
class ConsolePanel;
class SettingsPanel;

struct EditorState {
    // … truncated in master plan (full sketch in phase source)
```

#### Qt Editor Initialisation

```cpp
// editor/qt_editor_shell.hpp
#pragma once
// Qt 6 — no Qt 6
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QUndoStack>
#include <QTreeView>
#include <QPlainTextEdit>
// Vulkan via QWindow/QWidget container — not qt_editor_impl_vulkan
#include "../platform/platform.hpp"
#include "../renderer/vk/device.hpp"

class QtEditorShell {
public:
  void init(const VulkanDevice& device, VkRenderPass render_pass,
            WindowHandle window, u32 image_count);
  void destroy();
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Remove** | Torque editor `Gui*` from default editor build (after parity) |
| **Replace** | World Editor chrome with Qt 6 FUSE editor (`QMainWindow` + docks) |
| **Forbid** | Dear ImGui for editor chrome (Phase 6 source superseded) |
| **Keep** | Engine runnable headless without editor linked |

#### Depends on

- Track A: P5 Qt vertical slice

#### Gates

- [ ] **B6.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.2 — Command System & Undo/Redo

#### FUSE design

Every mutation to the scene goes through a `Command` object. No exceptions. This enables unlimited undo/redo, macro recording, and eventually remote collaboration.

#### Command Interface

```cpp
// editor/commands/command.hpp
#pragma once
#include "../../core/types.hpp"
#include <string>

struct Command {
  virtual ~Command() = default;
  virtual void execute() = 0;
  virtual void undo()    = 0;
  virtual std::string description() const = 0;  // shown in undo history panel
  virtual bool merge(Command* other) { return false; }  // merge consecutive drag operations
};

class CommandStack {
public:
  void execute(std::unique_ptr<Command> cmd);
  void undo();
  void redo();
    // … truncated in master plan (full sketch in phase source)
```

#### Core Commands

```cpp
// editor/commands/transform_command.hpp
struct TransformCommand : Command {
  EntityID id;
  Transform before, after;

  TransformCommand(EntityID id, Transform before, Transform after)
    : id(id), before(before), after(after) {}

  void execute() override { registry->get<Transform>(id) = after; }
  void undo()    override { registry->get<Transform>(id) = before; }
  std::string description() const override { return "Move Entity"; }

  bool merge(Command* other) override {
    if (auto* o = dynamic_cast<TransformCommand*>(other)) {
      if (o->id == id) { after = o->after; return true; }  // merge consecutive drags
    }
    return false;
  }
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ad-hoc editor mutations with FUSE `Command` / `CommandStack` + `QUndoStack` adapter |
| **Keep** | Undo/redo LIFO + merge consecutive transforms |

#### Depends on

- Track A: P5; B6.1

#### Gates

- [ ] **B6.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.3 — Viewport Panel

#### FUSE design

The viewport is the 3D view into the scene. It renders into an Qt image backed by FUSE's output texture. It handles camera navigation, gizmo interaction, object picking, and drag-and-drop from the asset browser.

#### Viewport Panel

```cpp
// editor/panels/viewport_panel.hpp
#pragma once
#include "../editor_app.hpp"

class ViewportPanel {
public:
  void render(EditorState& state, Scene& scene, Renderer& renderer,
              CommandStack& cmds, f32 dt);

private:
  // Free camera state
  struct FreeCamera {
    vec3  position      = {0, 2, -5};
    f32   yaw           = 0.f;
    f32   pitch         = 0.f;
    f32   move_speed    = 10.f;
    f32   look_sensitivity = 0.2f;
    bool  is_flying     = false;
    // … truncated in master plan (full sketch in phase source)
```

#### Free Camera Controller

```cpp
void ViewportPanel::handle_camera_input_(EditorState& state, f32 dt) {
  const InputState& input = Input::state();

  // Right-click held → enable fly mode
  if (input.mouse_held(MouseButton::Right)) {
    if (!free_cam_.is_flying) {
      Input::capture_mouse(true);
      free_cam_.is_flying = true;
    }

    // Mouse look
    free_cam_.yaw   += input.mouse_dx * free_cam_.look_sensitivity;
    free_cam_.pitch -= input.mouse_dy * free_cam_.look_sensitivity;
    free_cam_.pitch  = std::clamp(free_cam_.pitch, -89.f, 89.f);

    // WASD + QE movement
    vec3 forward = spherical_to_cartesian(free_cam_.yaw, free_cam_.pitch);
    vec3 right   = forward.cross({0,1,0}).normalized();
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque viewport Gui with `QWidget`/`QWindow` hosting Vulkan |
| **Remove** | Immediate-mode image viewport path (Phase 6 ImGui) — use Qt Vulkan container |
| **Keep** | Free-cam, picking, context menus (`QMenu`) |

#### Depends on

- Track A: P5; B2 presentable colour target

#### Gates

- [ ] **B6.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.4 — In-Viewport Gizmos

#### FUSE design

Transform gizmos drawn directly into the viewport using Qt's DrawList. Translation arrows, rotation rings, scale handles. All exact — no approximation in hit testing.

```cpp
// editor/gizmos/transform_gizmo.hpp
#pragma once
#include "../../core/math/vec.hpp"
#include "../../ecs/entity.hpp"

struct GizmoResult {
  bool     active;         // gizmo is being dragged
  bool     changed;        // transform was modified this frame
  Transform new_transform;
};

class TransformGizmo {
public:
  GizmoResult render(
    EntityID           entity,
    const Transform&   transform,
    const Camera&      camera,
    GizmoMode          mode,
    // … truncated in master plan (full sketch in phase source)
```

#### Gizmo Colours & Scale

```cpp
// Gizmo constants
static constexpr ImU32 GIZMO_X      = IM_COL32(220,  50,  50, 255);
static constexpr ImU32 GIZMO_Y      = IM_COL32( 50, 200,  50, 255);
static constexpr ImU32 GIZMO_Z      = IM_COL32( 50, 100, 220, 255);
static constexpr ImU32 GIZMO_HOVER  = IM_COL32(255, 220,   0, 255);
static constexpr ImU32 GIZMO_ACTIVE = IM_COL32(255, 255, 255, 255);
static constexpr f32   GIZMO_SCREEN_SIZE = 120.f;  // pixels — scale invariant
static constexpr f32   GIZMO_LINE_WIDTH  = 2.5f;
static constexpr f32   GIZMO_ARROW_SIZE  = 0.2f;   // fraction of total length
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | ImDrawList gizmos with engine debug-draw and/or Qt overlay on viewport |
| **Keep** | Analytic hit tests for translate/rotate/scale + snap |

#### Depends on

- Track A: B6.3; B1 math

#### Gates

- [ ] **B6.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.5 — Scene Hierarchy Panel

#### FUSE design

```cpp
// editor/panels/hierarchy_panel.hpp
#pragma once
#include "../editor_app.hpp"

class SceneHierarchyPanel {
public:
  void render(EditorState& state, Scene& scene, CommandStack& cmds);

private:
  char  search_buf_[128] = {};
  bool  rename_active_   = false;
  char  rename_buf_[128] = {};

  void render_entity_node_(EntityID id, EditorState& state,
                            Scene& scene, CommandStack& cmds);
  void render_context_menu_(EntityID id, EditorState& state,
                             Scene& scene, CommandStack& cmds);
  bool passes_filter_(EntityID id, Scene& scene) const;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque hierarchy tree Gui with `QTreeView` model over ECS parent/child |
| **Keep** | Reparent, search, multi-select behaviours |

#### Depends on

- Track A: B3 scene; P5

#### Gates

- [ ] **B6.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.6 — Property Inspector Panel

#### FUSE design

The inspector shows all components on the selected entity and allows live editing.

```cpp
// editor/panels/inspector_panel.hpp
#pragma once
#include "../editor_app.hpp"

class InspectorPanel {
public:
  void render(EditorState& state, Scene& scene, CommandStack& cmds,
              MaterialSystem& materials);

private:
  void render_transform_  (EntityID id, Scene& scene, CommandStack& cmds);
  void render_mesh_       (EntityID id, Scene& scene, CommandStack& cmds);
  void render_sdf_object_ (EntityID id, Scene& scene, CommandStack& cmds);
  void render_rigidbody_  (EntityID id, Scene& scene, CommandStack& cmds);
  void render_camera_     (EntityID id, Scene& scene, CommandStack& cmds);
  void render_point_light_(EntityID id, Scene& scene, CommandStack& cmds);
  void render_add_component_menu_(EntityID id, Scene& scene, CommandStack& cmds);

    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque inspector with reflection-driven Qt property widgets; GRIA α slider first-class |
| **Keep** | Live edit of selected entity fields |

#### Depends on

- Track A: P5 reflection bridge; B3 components

#### Gates

- [ ] **B6.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.7 — Material Editor Panel

#### FUSE design

```cpp
// editor/panels/material_editor_panel.hpp
#pragma once
#include "../editor_app.hpp"
#include "../../renderer/material_system.hpp"

class MaterialEditorPanel {
public:
  void render(EditorState& state, MaterialSystem& materials,
              ResourceManager& resources, CommandStack& cmds);

private:
  u32   selected_mat_id_  = INVALID_INDEX;
  bool  preview_dirty_    = true;

  void render_material_list_(MaterialSystem& materials);
  void render_material_properties_(Material& mat, MaterialSystem& materials,
                                    ResourceManager& resources, CommandStack& cmds);
  void render_material_preview_();
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Legacy material tool chrome with Qt material editor + live preview RT |
| **Keep** | PBR parameter editing against B5 materials |

#### Depends on

- Track A: B5.3; B6.1

#### Gates

- [ ] **B6.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.8 — SDF Sculpt Panel

#### FUSE design

Live SDF sculpting — add, subtract, blend primitives in real time. GRIA alpha controls blend mode. The most distinctive editor feature no other engine has.

```cpp
// editor/panels/sdf_sculpt_panel.hpp
#pragma once
#include "../editor_app.hpp"

class SDFSculptPanel {
public:
  void render(EditorState& state, Scene& scene, CommandStack& cmds);
  // Called from viewport — handles brush strokes on mouse drag
  void handle_brush_stroke(vec3 hit_point, vec3 hit_normal,
                            Scene& scene, CommandStack& cmds);

private:
  enum class BrushOp { Add, Subtract, Smooth, Roughen, Paint };

  struct BrushState {
    SDFPrimitive shape        = SDFPrimitive::Sphere;
    BrushOp      op           = BrushOp::Add;
    f32          radius       = 1.f;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | SDF sculpt brush panel (add/subtract/smooth/roughen/paint, symmetry, voxel convert) |
| **Keep** | α blend control |

#### Depends on

- Track A: B3 SDF components; B1 SDF math

#### Gates

- [ ] **B6.8** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.9 — Asset Browser Panel

#### FUSE design

```cpp
// editor/panels/asset_browser_panel.hpp
#pragma once
#include "../editor_app.hpp"

class AssetBrowserPanel {
public:
  void init(const char* project_root);
  void render(EditorState& state, ResourceManager& resources, CommandStack& cmds);

private:
  struct AssetEntry {
    std::string  path;
    std::string  name;
    enum class Type { Folder, Mesh, Texture, Material, Scene, Audio, Script, Unknown } type;
    TextureHandle thumbnail;
    u64          last_modified;
  };

    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Torque asset Gui with Qt asset browser (`QFileSystemModel`-style + thumbnails, drag-drop) |
| **Compat** | Show cooked + legacy assets via importers |

#### Depends on

- Track A: P5; assets façade

#### Gates

- [ ] **B6.9** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.10 — Profiler Panel

#### FUSE design

```cpp
// editor/panels/profiler_panel.hpp
#pragma once
#include "../editor_app.hpp"

class ProfilerPanel {
public:
  void render(bool& open);
  void push_frame_data(const FrameProfileData& data);  // called end of each frame

private:
  static constexpr u32 HISTORY_FRAMES = 256;

  struct FrameProfileData {
    f32   cpu_ms;
    f32   gpu_ms;
    f32   depth_prepass_ms;
    f32   gbuffer_ms;
    f32   sdf_march_ms;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Phase-6 ImGui plots/flame graph with **Qt** scrolling plots, pass table, flame-graph view over FUSE profile events |
| **Forbid** | Dear ImGui profiler chrome |

#### Depends on

- Track A: B1 profiler event buffer; B6.1

#### Gates

- [ ] **B6.10** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.11 — Console Panel

#### FUSE design

```cpp
// editor/panels/console_panel.hpp
#pragma once
#include "../editor_app.hpp"

class ConsolePanel {
public:
  void render(bool& open);
  void add_log(LogLevel level, const char* message);

private:
  struct LogLine {
    LogLevel    level;
    std::string text;
    u64         timestamp;
    u32         repeat_count = 1;
  };

  std::vector<LogLine> lines_;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Phase-6 ImGui console with `QPlainTextEdit` + filters + command line hooked to logger / debug exec |
| **Compat** | Optional bridge to TorqueScript console in `compat/` mode |

#### Depends on

- Track A: B1 Logger; B6.1

#### Gates

- [ ] **B6.11** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.12 — Play Mode & Scene Simulation

#### FUSE design

```cpp
// editor/play_mode.hpp
#pragma once
#include "../scene/scene.hpp"

// Play mode snapshots the scene state, runs the simulation,
// and restores the snapshot on stop
class PlayMode {
public:
  void enter_play(Scene& scene, PhysicsManager& physics);
  void pause(Scene& scene, PhysicsManager& physics);
  void resume(Scene& scene, PhysicsManager& physics);
  void stop(Scene& scene, PhysicsManager& physics);

  bool is_playing() const { return state_ == State::Playing; }
  bool is_paused()  const { return state_ == State::Paused; }

private:
  enum class State { Stopped, Playing, Paused } state_ = State::Stopped;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Play mode snapshot → simulate → restore on stop |
| **Replace** | Editor play transport chrome with Qt actions/toolbars |

#### Depends on

- Track A: B3 serialisation; B6.2 commands

#### Gates

- [ ] **B6.12** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs
- [ ] Qt 6 only for editor chrome — **no Dear ImGui**

---

### B6.13 — Phase 6 Deliverables & Test Suite

#### FUSE design

#### Editor Startup

#### Command System

#### Viewport

#### Gizmos

#### Scene Hierarchy

#### Inspector

#### Material Editor

#### SDF Sculpt

#### Profiler

#### Play Mode

#### Performance

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Full Phase 6 capability checklist rewritten for Qt gates |
| **Add** | No Qt types in engine core includes; no Dear ImGui editor dependency |

#### Depends on

- Track A: P5 + B6 panels

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] Editor initialises all panels and dockspace in < 1 second
- [ ] Default layout loads correctly — viewport, hierarchy, inspector, console visible
- [ ] Qt dark theme applied correctly — all colours match specification
- [ ] Font loaded and rendering correctly at all DPI scales
- [ ] TransformCommand undo/redo correctly restores exact before/after state
- [ ] 100 commands execute and fully undo in correct LIFO order — entity state matches pre-execution
- [ ] Consecutive transform drags merge into a single undo step — verified by undo count
- [ ] DeleteEntity undo correctly restores all components exactly
- [ ] Command stack respects MAX_HISTORY — oldest commands dropped correctly
- [ ] Free camera WASD movement smooth at 60fps — no input lag or jitter
- [ ] Mouse look angular rate matches sensitivity setting exactly
- [ ] Right-click context menu appears at correct screen position
- [ ] Entity picking correctly identifies the front-most entity under cursor — verified with overlapping objects
- [ ] Viewport resizes cleanly — renderer framebuffer rebuilt, no validation errors
- [ ] Translation gizmo moves entity in correct world/local axis — verified numerically
- [ ] Rotation gizmo produces correct quaternion from drag — no gimbal lock in world space
- [ ] Scale gizmo scales uniformly on XYZ handle, per-axis on individual handles
- [ ] Gizmo axis highlight fires on hover, deactivates on mouse release
- [ ] Snap-to-grid correctly quantises position to snap_translate increment
- [ ] Gizmo remains correct screen size at all camera distances
- [ ] All entities displayed correctly — tree structure matches parent/child relationships
- [ ] Drag-and-drop reparent emits ReparentCommand and updates hierarchy immediately
- [ ] Search filter correctly shows only matching entities — case-insensitive
- [ ] Right-click context menu creates/deletes entities correctly
- [ ] All component types render in inspector without crash
- [ ] Transform DragFloat values update renderer immediately — live preview
- [ ] SDFObject type change updates rendered shape within one frame
- [ ] GRIA alpha slider updates SDF smooth blending in real time — no frame delay
- [ ] Material ID change on mesh correctly updates rendered material
- [ ] Roughness and metallic sliders produce correct visual changes in viewport
- [ ] Colour picker correctly updates base colour — no colour space error
- [ ] Procedural material switch correctly replaces texture-based shading
- [ ] Material changes persist after save/load cycle
- [ ] Add brush creates SDF sphere at correct world position
- [ ] Subtract brush correctly removes volume — verified by ray marching through carved region
- [ ] GRIA alpha blend produces smooth/hard transitions as expected
- [ ] X symmetry correctly mirrors stroke across X=0 plane
- [ ] Stroke spacing prevents redundant kernel dispatches at slow cursor speeds
- [ ] GPU pass breakdown times match CUDA event measurements within 0.5ms
- [ ] Frame history scrolls correctly — no off-by-one in ring buffer
- [ ] Pause correctly freezes history display without stopping engine
- [ ] Enter play: snapshot taken, physics initialised correctly
- [ ] Stop play: scene state restored exactly — entity positions, velocities reset
- [ ] Pause/resume: simulation correctly halts and continues without state corruption
- [ ] Editor UI render time < 2ms per frame (Qt draw call submission)
- [ ] No frame spikes from editor on non-interactive frames — verified over 10,000 frames
- [ ] Memory overhead of editor layer < 256MB

---

#### What Phase 7 / B7 Builds On This

Phase 6 delivers a complete, usable tool. A developer can now open the editor, build a scene using SDF primitives and meshes, tune materials, run physics, and save a project. Phase 7 is the final phase — it fills in the remaining production features that separate a working engine from a shipping one: animation, audio, scripting, networking, terrain, world streaming, and platform targets. Phase 7 is where FUSE becomes a product.

*Bridge:* After **B6** gates are green (and required Track A milestones), begin **B7** on FUSE APIs only.

## B7 — Production Systems

**Duration:** 8–10 weeks  
**Depends on (phase-level):** P6–P7 port complete preferred; B3–B6 for integration; subsystem gates may allow early starts (e.g. asset tools after P1)

*Full fidelity: every source `## 7.*` → `### B7.*`, adapted for FUSE (C++23 host, `fuse::`/`FUSE_*`, Qt 6 editor — not ImGui). Port-first hard rule remains.*

---

### B7.1 — Animation System

#### FUSE design

#### Design

Skeletal animation runs on the CPU via the job system for small character counts, promoted to CUDA for large crowds. The animation pipeline is: sample → blend → solve IK → skin. Skinning always runs on CUDA — it is embarrassingly parallel and maps perfectly to warp execution.

#### Skeleton & Bone Hierarchy

```cpp
// animation/skeleton.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"

struct Bone {
  char    name[64];
  i32     parent_index;    // -1 = root
  mat4    inverse_bind;    // bind-pose inverse — transforms from mesh to bone local space
  mat4    local_transform; // current local transform (from animation)
};

struct Skeleton {
  std::vector<Bone> bones;
  u32               bone_count;

  i32  find_bone(const char* name) const;
  mat4 compute_world_transform(u32 bone_idx) const;  // traverses hierarchy
    // … truncated in master plan (full sketch in phase source)
```

#### Animation Clip

```cpp
// animation/clip.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"

// Keyframe channels — one per bone per property
struct KeyframeChannel {
  std::vector<f32>   times;
  std::vector<vec3>  values_vec3;   // position or scale
  std::vector<quat>  values_quat;   // rotation

  // Hermite spline interpolation between keyframes
  vec3 sample_vec3(f32 time) const;
  quat sample_quat(f32 time) const;
};

struct AnimationClip {
  char     name[128];
    // … truncated in master plan (full sketch in phase source)
```

#### Blend Tree

A node-based blend graph evaluated at runtime. Supports 1D/2D blend spaces, additive layers, and masked blending.

```cpp
// animation/blend_tree.hpp
#pragma once
#include "clip.hpp"

// Base blend node — all nodes derive from this
struct BlendNode {
  virtual void evaluate(f32 dt, const Skeleton& skel, Pose& out) = 0;
  virtual ~BlendNode() = default;
};

// Leaf: plays a single clip
struct ClipNode : BlendNode {
  const AnimationClip* clip;
  f32  time       = 0.f;
  f32  play_rate  = 1.f;
  bool looping    = true;

  void evaluate(f32 dt, const Skeleton& skel, Pose& out) override {
    // … truncated in master plan (full sketch in phase source)
```

#### Procedural IK

```cpp
// animation/ik/fabrik.hpp
#pragma once
#include "../skeleton.hpp"

// FABRIK — Forward and Backward Reaching Inverse Kinematics
// Iterative, works on arbitrary chain lengths, stable and fast
struct FABRIKChain {
  std::vector<u32> bone_indices;   // chain from root to end effector
  vec3             target;         // world-space target position
  u32              max_iterations  = 10;
  f32              tolerance       = 0.001f;
  // Constraints
  f32              min_angle_deg   = 0.f;
  f32              max_angle_deg   = 160.f;

  void solve(Pose& pose, const Skeleton& skel);
};

    // … truncated in master plan (full sketch in phase source)
```

#### GPU Skinning (CUDA)

```cpp
// animation/skinning.cuh
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"

// Linear blend skinning — runs one thread per vertex
// Weights and indices in vertex buffer, bone transforms in SSBO
__global__ void skin_vertices_kernel(
  const vec3*  rest_positions,    // bind-pose positions
  const vec3*  rest_normals,
  const u32*   bone_indices,      // 4 bones per vertex
  const f32*   bone_weights,      // 4 weights per vertex (sum = 1)
  const mat4*  bone_transforms,   // final bone matrices (world × inv_bind)
  vec3*        out_positions,
  vec3*        out_normals,
  u32          vertex_count
);

    // … truncated in master plan (full sketch in phase source)
```

#### Animation Component

```cpp
// ecs/components/animator.hpp
struct Animator {
  static constexpr const char* component_name = "Animator";

  Handle<Skeleton>       skeleton;
  Handle<AnimStateMachine> state_machine;
  Pose                   current_pose;

  // Runtime parameters read by blend tree nodes
  f32    speed         = 0.f;
  f32    direction     = 0.f;
  bool   is_grounded   = true;
  bool   is_attacking  = false;
  // Add more as needed — accessed by condition lambdas in transitions

  f32    playback_rate = 1.f;
  bool   paused        = false;
};
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** / **Extend** | Animation with skeleton/clips, blend trees, IK, CUDA LBS skinning |
| **Compat** | Map DTS sequences via `compat/` importers where needed |

#### Depends on

- Track A: B3 ECS; prefer P7 unlock

#### Gates

- [ ] **B7.1** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.2 — Spatial Audio Engine

#### FUSE design

#### Design

No middleware (FMOD, Wwise). Custom spatial audio engine using CUDA for convolution reverb and a CPU pipeline for source mixing and HRTF spatialisation. OpenAL as the output backend — thin, cross-platform, well-understood.

```cpp
// audio/audio_engine.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"
#include "../ecs/entity.hpp"

struct AudioDesc {
  u32  sample_rate     = 48000;
  u32  frames_per_buf  = 512;
  u32  max_sources     = 256;
  u32  max_reverb_zones= 32;
  bool hrtf_enabled    = true;   // head-related transfer function spatialisation
  bool cuda_reverb     = true;   // GPU convolution reverb
};

struct AudioClip {
  std::vector<f32> samples;    // interleaved stereo f32 PCM
  u32              sample_rate;
    // … truncated in master plan (full sketch in phase source)
```

#### Convolution Reverb (CUDA FFT)

```cpp
// audio/reverb_cuda.cuh
#pragma once
#include "../core/types.hpp"
#include <cufft.h>

struct ConvolutionReverb {
  cufftHandle  fft_plan;
  cufftHandle  ifft_plan;
  f32*         d_ir_spectrum;   // pre-FFT'd impulse response
  u32          fft_size;        // next power of 2 >= (block_size + ir_length - 1)
  u32          ir_length;

  void init(const f32* ir_samples, u32 ir_len, u32 block_size);
  void destroy();

  // Apply convolution to one block — runs on audio stream
  void process(f32* d_input, f32* d_output, u32 frames, cudaStream_t stream);
};
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary audio path with custom spatial engine (OpenAL out, HRTF, CUDA FFT reverb) |
| **Keep** | AudioSource / AudioListener component model |

#### Depends on

- Track A: B3 components; P1

#### Gates

- [ ] **B7.2** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.3 — Scripting Layer

#### FUSE design

Lua scripting for game logic. FUSE exposes a clean, minimal API surface. Scripts cannot touch engine internals — only the public API. Hot-reload: scripts can be modified and reloaded without restarting.

#### Lua Binding Architecture

```cpp
// scripting/lua_state.hpp
#pragma once
#include "../core/types.hpp"
#include "../ecs/entity.hpp"
#include <lua.hpp>

class LuaState {
public:
  void init(Registry* registry, Scene* scene, AudioEngine* audio);
  void destroy();

  bool  load_script(const char* path);
  bool  reload_script(const char* path);
  void  call_on_start(EntityID id);
  void  call_on_update(EntityID id, f32 dt);
  void  call_on_collision(EntityID id, EntityID other, vec3 contact_point);
  void  call_on_trigger_enter(EntityID id, EntityID other);
  void  call_on_destroy(EntityID id);
    // … truncated in master plan (full sketch in phase source)
```

#### Script Component

```cpp
// ecs/components/script.hpp
struct Script {
  static constexpr const char* component_name = "Script";

  char   script_path[256];
  bool   started      = false;    // on_start called
  bool   enabled      = true;
  // Per-entity Lua table reference — stores instance data
  i32    lua_ref      = LUA_NOREF;
};
```

#### Engine API Surface (Lua)

```lua
-- Full API exposed to scripts

-- Entity
local id = Entity.create("MyObject")
local pos = Entity.get_position(id)           -- returns {x,y,z}
Entity.set_position(id, {x=1, y=2, z=3})
Entity.set_rotation_euler(id, {x=0, y=90, z=0})
Entity.destroy(id)
local alive = Entity.alive(id)

-- Physics
Physics.apply_impulse(id, {x=0, y=10, z=0})
Physics.set_velocity(id, {x=5, y=0, z=0})
local vel = Physics.get_velocity(id)
local hit = Physics.ray_cast({x=0,y=10,z=0}, {x=0,y=-1,z=0}, 100)
-- hit = {entity=id, point={x,y,z}, normal={x,y,z}, distance=5.0} or nil

-- Input
    // … truncated in master plan (full sketch in phase source)
```

#### Hot Reload

```cpp
// scripting/hot_reload.hpp
class ScriptHotReload {
public:
  void watch(const char* script_dir);
  void poll(LuaState& lua, Registry& registry);  // call each frame

private:
  struct WatchedScript {
    std::string path;
    u64         last_modified;
  };
  std::vector<WatchedScript> watched_;
};
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Primary *new* gameplay scripting story → **Lua** |
| **Compat** | TorqueScript VM remains under `compat/` for legacy content — not primary new API |
| **Keep** | Hot-reload |

#### Depends on

- Track A: P6 Compat quarantine; P7

#### Gates

- [ ] **B7.3** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.4 — Networking

#### FUSE design

Deterministic lockstep for authoritative multiplayer (strategy, fighting games). Rollback networking (GGPO-style) for real-time action. The implementation is modular — swap transport layer without touching game logic.

#### Transport Abstraction

```cpp
// network/transport.hpp
#pragma once
#include "../core/types.hpp"
#include <functional>
#include <span>

enum class PacketChannel : u8 {
  Reliable,       // TCP-like — guaranteed, ordered
  Unreliable,     // UDP — fire and forget
  UnreliableSeq,  // UDP with sequencing — newest packet wins
};

struct Packet {
  std::vector<byte> data;
  u32               peer_id;
  PacketChannel     channel;
  u64               timestamp_us;
};
    // … truncated in master plan (full sketch in phase source)
```

#### Rollback Networking

```cpp
// network/rollback.hpp
#pragma once
#include "../core/types.hpp"
#include "../scene/scene.hpp"

// Snapshot of all deterministic simulation state at a given frame
struct GameSnapshot {
  u32              frame;
  std::vector<byte> physics_state;   // serialised RigidBodySoA
  std::vector<byte> ecs_state;       // serialised Transform + game components
  u64              checksum;         // for desync detection
};

struct PlayerInput {
  u32  frame;
  u32  player_id;
  u32  buttons;      // bitfield
  i16  axis_lx, lx;  // left stick
    // … truncated in master plan (full sketch in phase source)
```

#### Network State Synchronisation

```cpp
// network/state_sync.hpp
// For non-rollback scenarios: authoritative server, client interpolation

struct EntityNetState {
  EntityID entity;
  vec3     position;
  quat     rotation;
  vec3     linear_velocity;
  u64      timestamp;
  u32      sequence;
};

// Client-side: interpolate between received states
class ClientInterpolator {
public:
  void receive_state(const EntityNetState& state);
  void update(Registry& registry, u64 current_time_us);

    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** / **Extend** | Net stack with transport abstraction (ENet / optional Steam), rollback + authoritative + interpolation |
| **Keep** | Map Torque net object ideas carefully during migration |
| **Compat** | Ghosting concepts documented against FUSE net entities |

#### Depends on

- Track A: P7; B3

#### Gates

- [ ] **B7.4** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.5 — Terrain System

#### FUSE design

Heightfield terrain with SDF cave/overhang integration. The surface is a heightfield (fast hardware rasterisation), the subsurface is voxel SVO (full 3D destruction, caves, tunnels).

#### Heightfield

```cpp
// terrain/heightfield.hpp
#pragma once
#include "../core/types.hpp"
#include "../renderer/resources.hpp"
#include "../spatial/svo.hpp"

struct TerrainDesc {
  u32   resolution     = 4096;    // heightmap texels
  f32   world_size     = 4096.f;  // metres
  f32   max_height     = 512.f;   // metres
  u32   lod_levels     = 8;
  bool  has_svo_caves  = true;    // SVO layer for underground content
  u32   svo_depth      = 8;       // resolution of underground SVO
};

struct TerrainChunk {
  u32          lod;
  ivec2        chunk_coord;    // grid position
    // … truncated in master plan (full sketch in phase source)
```

#### Terrain Rendering

Terrain rendered via vertex shader heightmap displacement — no CPU-side mesh generation per frame.

```glsl
// shaders/terrain.vert.glsl
#version 460

layout(location = 0) in  vec2 in_uv;        // chunk UV [0,1]
layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec3 out_world_pos;

layout(push_constant) uniform PC {
  mat4  view_proj;
  vec4  chunk_offset_scale;   // (world_x, world_z, chunk_size, max_height)
  uint  heightmap_tex;
  uint  normal_map_tex;
} pc;

layout(set=0, binding=1) uniform texture2D textures[];
layout(set=0, binding=2) uniform sampler   samplers[];

    // … truncated in master plan (full sketch in phase source)
```

#### Terrain Layers & Procedural Texturing

```glsl
// shaders/terrain.frag.glsl
// Slope-based layer blending — no manual painting required as default
// Painter override composited on top

void main() {
  float slope     = 1.0 - abs(dot(normalize(frag_normal), vec3(0,1,0)));
  float height_n  = frag_world_pos.y / max_height;

  // Layer 0: grass (flat, low altitude)
  // Layer 1: rock  (steep slope)
  // Layer 2: snow  (high altitude)
  // Layer 3: dirt  (painted by editor)
  float w_grass = clamp(1.0 - slope * 3.0, 0.0, 1.0) * (1.0 - height_n * 1.5);
  float w_rock  = clamp(slope * 2.5, 0.0, 1.0);
  float w_snow  = clamp((height_n - 0.7) * 5.0, 0.0, 1.0);

  // Normalise
  float total = w_grass + w_rock + w_snow + 0.001;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** / **Extend** | Terrain with heightfield LOD chunks + underground SVO caves + analytic noise |
| **Compat** | Legacy Torque terrain → cook into FUSE heightfield/SVO |

#### Depends on

- Track A: B3.5 SVO; B1 noise

#### Gates

- [ ] **B7.5** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.6 — World Partition & Streaming

#### FUSE design

For large open worlds. Divide the world into cells. Cells stream in/out asynchronously based on camera proximity. The active simulation always works on a small, loaded subset of the world.

```cpp
// streaming/world_partition.hpp
#pragma once
#include "../core/types.hpp"
#include "../scene/scene.hpp"

struct WorldCell {
  ivec2        coord;          // grid coordinate
  AABB         world_bounds;
  std::string  asset_path;     // binary cell file on disk
  bool         loaded     = false;
  bool         visible    = false;
  f32          load_priority = 0.f;
  // Entities in this cell — managed by partition system
  std::vector<EntityID> entities;
};

struct WorldPartitionDesc {
  f32   cell_size          = 256.f;    // metres per cell side
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | World partition cell stream in/out by camera distance |
| **Keep** | Large-world / mission workflows |

#### Depends on

- Track A: B3.6–B3.7; B7.5

#### Gates

- [ ] **B7.6** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.7 — VFX System

#### FUSE design

GPU particle simulation in CUDA. Emitters define particles via C++ descriptors. Particles simulate on the physics stream, render on the render stream. No CPU readback.

```cpp
// vfx/particle_system.hpp
#pragma once
#include "../core/types.hpp"
#include "../core/math/vec.hpp"

struct ParticleEmitterDesc {
  // Emission
  u32   max_particles        = 4096;
  f32   emit_rate            = 100.f;   // particles per second
  f32   lifetime_min         = 1.f;
  f32   lifetime_max         = 3.f;

  // Initial state distribution
  vec3  velocity_min         = {-1,-1,-1};
  vec3  velocity_max         = {1, 5, 1};
  vec3  position_spread      = {0.5f, 0.f, 0.5f};
  f32   size_start           = 0.1f;
  f32   size_end             = 0.f;
    // … truncated in master plan (full sketch in phase source)
```

#### Particle Simulation Kernels

```cpp
// vfx/particle_kernels.cuh

__global__ void particle_simulate_kernel(
  ParticleSoAGPU  p,
  vec3            gravity,
  f32             drag,
  f32             dt,
  const RayMarchParams::SDFObject* sdf_objects,
  u32             sdf_count,
  bool            collide
) {
  u32 i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.count || !p.alive_flags[i]) return;

  // Age particle
  p.ages[i] += dt / p.lifetimes[i];
  if (p.ages[i] >= 1.f) { p.alive_flags[i] = 0; return; }

    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | GPU particle VFX SoA (emit/simulate/collide with SDF; billboard; no CPU readback) |
| **Compat** | Legacy particle datablocks → FUSE emitters where feasible |

#### Depends on

- Track A: B2 CUDA; B4 optional collide; B3

#### Gates

- [ ] **B7.7** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.8 — Platform Hardening

#### FUSE design

#### Windows Production Build

```cpp
// platform/win32/win32_platform.cpp
// Full Win32 implementation — no CRT dependency in shipping builds

// Crash handler — structured exception handler captures minidump
LONG WINAPI engine_crash_handler(EXCEPTION_POINTERS* ep) {
  // Write minidump with full stack, registers, heap
  HANDLE file = CreateFileA("engine_crash.dmp", GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  MINIDUMP_EXCEPTION_INFORMATION mei = {
    GetCurrentThreadId(), ep, FALSE
  };
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    MiniDumpWithFullMemory, &mei, nullptr, nullptr);
  CloseHandle(file);
  return EXCEPTION_EXECUTE_HANDLER;
}

// DPI awareness — correct on all monitor configurations
    // … truncated in master plan (full sketch in phase source)
```

#### Linux Production Build

```cpp
// platform/linux/linux_platform.cpp
// Signal handler for crash dumps
void linux_crash_handler(int sig, siginfo_t* info, void* ctx) {
  // Write stack trace via libunwind
  // Write core dump annotation
  // Log to file
}

void install_crash_handlers() {
  struct sigaction sa;
  sa.sa_sigaction = linux_crash_handler;
  sa.sa_flags     = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGFPE,  &sa, nullptr);
  sigaction(SIGBUS,  &sa, nullptr);
}
```

#### Build Configuration

```cmake
# CMakeLists.txt — Shipping target
add_executable(engine_shipping main.cpp)
target_compile_definitions(engine_shipping PRIVATE
  FUSE_SHIPPING=1
  FUSE_NO_LOGGING=1
  FUSE_NO_PROFILER=1
  FUSE_NO_EDITOR=1
  FUSE_NO_ASSERT=1
)
target_compile_options(engine_shipping PRIVATE
  -O3 -march=native -ffast-math
  -fno-rtti -fno-exceptions    # stripped in shipping
  -fvisibility=hidden
  -flto                        # link-time optimisation
)
# Strip debug symbols to separate .pdb / .sym file
# Package: engine_shipping.exe + engine_shipping.pdb (kept for crash reports)
```

#### Memory Leak Detection

```cpp
// core/memory/leak_detector.hpp
// Debug builds only — tracks all allocations

#ifdef FUSE_DEBUG
class LeakDetector {
public:
  static void record_alloc(void* ptr, usize size, const char* tag,
                            const char* file, u32 line);
  static void record_free(void* ptr);
  static void report_leaks();  // called at shutdown — prints all live allocations

private:
  struct AllocRecord {
    void*       ptr;
    usize       size;
    const char* tag;
    const char* file;
    u32         line;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Add** | Win minidump / Linux signals; `FUSE_SHIPPING` strip; leak detector; DPI awareness |
| **Keep** | Shipping game runtime builds without Qt |

#### Depends on

- Track A: P6–P7

#### Gates

- [ ] **B7.8** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.9 — Asset Pipeline (Offline Tools)

#### FUSE design

#### Mesh Importer

```cpp
// tools/mesh_importer.cpp
// Converts FBX / GLTF / OBJ → engine binary format
// Runs as offline tool — not linked into runtime

struct MeshImportDesc {
  const char* input_path;
  const char* output_path;
  bool        generate_tangents  = true;
  bool        generate_normals   = true;
  bool        optimise_vertex_cache = true;   // Forsyth post-transform cache
  bool        generate_lods      = true;
  u32         lod_count          = 4;
  f32         lod_error_target   = 0.01f;     // Meshoptimizer error threshold
  bool        compress            = true;      // meshoptimizer vertex compression
};

bool import_mesh(const MeshImportDesc& desc);
```

#### Texture Importer

```cpp
// tools/texture_importer.cpp

struct TextureImportDesc {
  const char* input_path;
  const char* output_path;
  enum class ColorSpace { Linear, sRGB } color_space = ColorSpace::sRGB;
  bool        generate_mipmaps = true;
  enum class Compression { None, BC1, BC3, BC4, BC5, BC7 } compression = Compression::BC7;
  bool        is_normal_map    = false;   // selects BC5 automatically
  bool        is_hdr           = false;   // f16 output
};

bool import_texture(const TextureImportDesc& desc);
```

#### Audio Importer

```cpp
// tools/audio_importer.cpp

struct AudioImportDesc {
  const char* input_path;       // WAV, OGG, FLAC, MP3
  const char* output_path;
  u32         target_sample_rate = 48000;
  bool        normalise           = true;
  bool        trim_silence        = true;
  enum class Format { PCM_F32, OGG_VORBIS } format = Format::OGG_VORBIS;
  f32         ogg_quality         = 0.6f;   // 0=lowest, 1=highest
};

bool import_audio(const AudioImportDesc& desc);
```

#### Asset Dependency Graph

```cpp
// tools/asset_graph.hpp
// Tracks which assets depend on which source files
// Reimport only assets whose dependencies have changed

class AssetGraph {
public:
  void  add_asset(const char* output_path, const char* source_path);
  void  add_dependency(const char* output_path, const char* dep_path);
  void  scan_for_changes();       // checks file modification times
  const std::vector<std::string>& dirty_assets() const;
  void  reimport_dirty(const char* project_dir);
  bool  save(const char* path) const;
  bool  load(const char* path);

private:
  struct AssetRecord {
    std::string              output_path;
    std::string              source_path;
    // … truncated in master plan (full sketch in phase source)
```

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Replace** | Ad-hoc imports with offline cooks (mesh/texture/audio) + dependency graph |
| **Compat** | DTS/DIFF/terrain legacy → FUSE binaries via `compat/` importers |
| **Note** | Cook UI may be Qt tools; CLI required for CI |

#### Depends on

- Track A: P1; tools earlier than full P7 allowed

#### Gates

- [ ] **B7.9** implemented on FUSE APIs (not ungated Torque guts)
- [ ] Source narrative tests/acceptance for this topic green
- [ ] ASan/UBSan clean on subsystem smoke
- [ ] No owning raw pointers in public FUSE APIs

---

### B7.10 — Phase 7 Deliverables & Test Suite

#### FUSE design

#### Animation

#### Audio

#### Scripting

#### Networking

#### Terrain

#### World Streaming

#### VFX

#### Platform

#### Full Engine Integration

---

#### Torque3D delta

| Action | Detail |
|--------|--------|
| **Keep** | Full Phase 7 deliverable/test checklist under FUSE naming + port-first rule still holds |

#### Depends on

- Track A: P7 + B7 systems

#### Gates

*Carry-forward deliverable/test checklist (FUSE-adapted):*

- [ ] Skeleton loads from binary format with correct hierarchy — parent/child chain verified
- [ ] ClipNode samples position and rotation channels within 0.001f of reference at all keyframes
- [ ] BlendNode2 interpolates pose correctly at blend values 0.0, 0.5, and 1.0
- [ ] AnimStateMachine transitions between two states — blend completes in specified duration
- [ ] FABRIK IK converges within tolerance in < 10 iterations for 95% of random target positions
- [ ] TwoBoneIK produces analytically correct limb pose — verified against reference solver
- [ ] GPU skinning transforms 10k vertices in < 0.5ms — verified with CUDA events
- [ ] Animator component updates pose and uploads bone buffer every frame without memory leak
- [ ] Audio engine initialises OpenAL device and context without error
- [ ] Mono and stereo WAV files load and play correctly
- [ ] Spatial attenuation: source at max_distance has near-zero volume — verified with gain readback
- [ ] play_at correctly positions source at world position — panning matches camera orientation
- [ ] Convolution reverb CUDA FFT produces output within -60dB noise floor of reference CPU FFT
- [ ] 32 simultaneous spatial sources mix without crackling at 48kHz
- [ ] Lua state initialises and executes a hello-world script without error
- [ ] Entity.get_position / set_position round-trip correctly through Lua
- [ ] Physics.ray_cast returns correct hit from Lua — verified against C++ ray_cast result
- [ ] on_update called every frame with correct dt — verified by accumulating dt over 60 frames
- [ ] on_collision fires on first frame of contact — verified with controlled rigid body test
- [ ] Hot-reload replaces script function mid-run — new behaviour active within 1 frame
- [ ] ENet transport sends and receives reliable packet between two local processes — no data corruption
- [ ] Rollback snapshot correctly captures and restores rigid body positions — byte-identical round-trip
- [ ] Rollback resimulates 4 frames correctly after remote input arrives — final state matches non-rollback reference
- [ ] Desync detection correctly flags checksum mismatch when physics state diverges
- [ ] ClientInterpolator smoothly interpolates entity position over 3 received states — no discontinuity
- [ ] Heightfield generates without artifacts at 4096×4096 resolution
- [ ] get_height returns correct value — matches heightmap texel read within 0.01f
- [ ] LOD system loads and unloads chunks correctly as camera moves — no missing geometry
- [ ] Terrain-SVO cave correctly renders below terrain surface — SVO ray march transitions from heightfield
- [ ] Terrain deform correctly updates affected chunk mesh within 1 frame
- [ ] Cell loads correctly from disk — entity count and positions match saved state
- [ ] Stream-in fires before camera enters cell bounds — no pop-in visible at 60fps with 512m draw distance
- [ ] Stream-out correctly destroys all entities in unloaded cell — no leaked entities
- [ ] force_load completes synchronously and correctly — used for teleport test
- [ ] 4096 particles simulate under gravity and collide with SDF sphere — verified by visual inspection
- [ ] emit_rate correctly emits expected particle count per second — tested over 5 seconds
- [ ] Particle lifetime correctly ages and kills particles — alive count converges to rate × lifetime
- [ ] CUDA particle kernel achieves > 70% occupancy — verified with Nsight Compute
- [ ] Shipping build compiles with zero warnings, zero debug code included — verified by binary inspection
- [ ] Crash handler writes valid minidump on intentional null dereference — dmp opens in WinDbg
- [ ] Leak detector correctly reports zero leaks after clean shutdown in debug build
- [ ] Mesh importer produces byte-identical output from same source on two machines — deterministic
- [ ] Texture BC7 compression PSNR > 40dB vs original — verified with image comparison tool
- [ ] Engine boots, editor opens, scene loads, physics runs, audio plays in < 3 seconds on ThinkStation P920
- [ ] 1000 animated skinned characters in scene with physics and audio — frame time < 16ms
- [ ] Save, close, reload cycle — scene state bit-identical after round-trip
- [ ] Script-controlled entity destroys itself on collision — no dangling entity handles
- [ ] World partition streams 16 cells seamlessly as camera traverses 2km at 30m/s

---

#### Final bridge / engine summary

At the completion of all seven phases FUSE delivers:

| System | Capability |
|--------|-----------|
| Renderer | Hybrid raster + CUDA SDF ray march, PBR, DDGI GI, clustered deferred, TAA |
| Geometry | Exact analytic SDF, SVO voxels, triangle meshes — all coexist in one BVH |
| Physics  | Full GPU PBD solver, Barnes-Hut O(n log n), CCD, voxel destruction |
| Animation| Skeletal, blend trees, state machines, FABRIK IK, GPU skinning |
| Audio    | Spatial, HRTF, CUDA convolution reverb |
| Scripting| Lua, hot-reload, full engine API |
| Networking| Rollback + authoritative server modes |
| Terrain  | Heightfield + SVO caves, procedural generation, runtime deformation |
| Streaming| World partition, async cell load/unload |
| VFX      | GPU particle simulation, SDF collision |
| Editor   | Full dockable editor, gizmos, command stack, SDF sculpt, profiler |
| Math     | GRIA α, Izaac noise, analytic SDF library — first-class engine primitives |

*[Bridge note truncated for master size — full text in phase source.]*

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
                 B6 (post-P5; full after B5)  [Qt 6 — not ImGui]
                 B7 (prefer post-P7; tools earlier)
```

| Track | AI-assisted duration (from sources) |
|-------|-------------------------------------|
| A P0–P7 | Re-estimate after repo inventory |
| B1 | 3–4 weeks |
| B2 | 4–5 weeks |
| B3 | 4–5 weeks |
| B4 | 5–6 weeks |
| B5 | 6–7 weeks |
| B6 | 5–6 weeks |
| B7 | 8–10 weeks |

Parallelism: B1 coincides with P1–P3; later B tracks overlap only where Track A subsystem gates are green.

**Port-first hard rule:** no major feature work on a pre-port Torque subsystem until that subsystem’s Track A gate is green.

---

## Risk & open decisions

### Cross-cutting risks

| Risk | Mitigation |
|------|------------|
| Feature work before port gates | Per-subsystem green gates in CI/review |
| SimObject ↔ ECS dual-run | Time-box; golden mission ASan gate |
| CUDA/Qt/MSVC friction | Isolate `fuse_compute`; pin toolkits |
| Scope vs parity | Compat loaders; defer B7 behind flags |
| GRIA α as “just a float” | Cross-cutting policy; Qt inspector |
| ImGui in phase docs | **Superseded by Qt 6** — Dear ImGui forbidden for editor chrome |

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
- [ ] No Meridian; no ImGui editor dependency in tree

## Appendix B — Legal notes

- Preserve MIT notices from Torque3D and all `third_party/` components.
- Qt: LGPL or commercial per distribution plan — document in `THIRD_PARTY.md`.
- Confirm “FUSE” trademark clearance before public release.
- CUDA / proprietary SDK redistributables: follow NVIDIA (and any audio codec) EULAs.
- Do not strip copyright headers when moving files into `compat/` or FUSE trees.
- Dear ImGui is **not** a FUSE editor dependency — do not vendor it for chrome.

## Appendix C — Phase 1 checklist carry-forward

Retain Foundation deliverable gates under FUSE naming, plus:

- [ ] `CMAKE_CXX_STANDARD 23` on all non-CUDA host targets
- [ ] `fuse_editor` on Qt 6 with embedded Vulkan viewport
- [ ] No `#include <Q*>` from `fuse_core` / `fuse_renderer` / `fuse_physics` / `fuse_ecs` / `fuse_compute`
- [ ] Golden Torque mission loads under ASan after P4
- [ ] Profiler events consumable by Qt flame-graph panel (B6)
- [ ] Every `### B*.*` subsection has a matching source `## N.M`

## Appendix D — Document control

| Version | Date | Notes |
|---------|------|-------|
| 0.2 | 2026-09-14 | Condensed unified plan (~30 KB) |
| 0.3 | 2026-09-14 | **Full fidelity:** Track A retained; every Phase 1–7 `##` → `### Bn.M`; ImGui→Qt; C++23 host; TOC companion |

**Inputs read in full:** Phases 1–7 source plans; condensed Track A retained and polished.

**Companion:** `FUSE_MASTER_PLAN_TOC.md` — every `###` heading for verification.
