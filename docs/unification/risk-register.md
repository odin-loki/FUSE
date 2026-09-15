# FUSE U0 — Risk Register

**Phase:** U0 Inventory & collision map  
**Date:** 2026-09-15  
**Scope:** Unification risks with evidence, impact, mitigation, and owner phase.

Severity: **Critical** / **High** / **Medium** / **Low**  
Likelihood: **High** / **Medium** / **Low**

---

## Risk summary matrix

| ID | Risk | Severity | Likelihood | Phase | Status |
|----|------|----------|------------|-------|--------|
| R01 | Symbol / ODR collisions block one-process link | Critical | High | U0–U2 | **Open** — mitigations defined |
| R02 | Dual TorqueScript VMs forever | High | Medium | U0–U3 | **Open** — decision gate |
| R03 | Box2D vs T3D physics semantic mismatch | High | High | U4–U8 | **Open** |
| R04 | Gui duplication / editor scope explosion | High | High | U6 | **Open** |
| R05 | Network model incompatibility | High | Medium | U3+ | **Open** |
| R06 | T2D mobile/web vs T3D desktop platform gap | Medium | High | U1+ | **Open** |
| R07 | Addon ore entanglement (full Engine trees) | High | High | U5 | **Open** |
| R08 | Infinite dual-maintenance of two CMake roots | High | Medium | U1 | **Open** |
| R09 | Pre-merged AFX/Verve in root Engine confuses extraction | Medium | Medium | U5 | **Open** |
| R10 | GuideBot custom license | Medium | Low | U5 | **Open** |
| R11 | Multiprocess fallback becomes permanent | Medium | Medium | U2–U4 | **Open** |
| R12 | Bit-perfect legacy demo expectation | Medium | Medium | U8 | **Open** |
| R13 | Track B renderer before U2 one-process | High | Medium | U0–U2 | **Open** |
| R14 | StringTable / singleton init order | Critical | High | U2 | **Open** |
| R15 | Qt scope / U6 vertical slice slips | High | Medium | U6 | **Open** |
| R16 | Inheritance misuse across physics/gfx/net | High | Medium | U4 | **Open** — mitigated by policy |
| R17 | Data races on scene graph / SimObject | Critical | High | U3–U6 | **Open** — handles + snapshots |
| R18 | GL/GFX context affinity on worker threads | High | Medium | U4 | **Open** — game-thread record v1 |
| R19 | Job scheduler scope creep / blocking waits | Medium | Medium | U3 | **Open** — fiber yield policy |
| R20 | Mobile thermal throttling / battery drain | High | High | U3–U6 | **Open** — adaptive N, background drain |
| R21 | GLES/Metal context on wrong thread | Critical | Medium | U4 | **Open** — renderThread() invariant |

---

## Detailed risks

### R01 — Symbol / ODR collisions

**Evidence:** 237 basename collisions; 311 filtered class collisions; 33 `Con::` function overlaps; identical `SimObject`, `GuiCanvas`, `BitStream` definitions in both trees ([symbol-collision-report.md](./symbol-collision-report.md)).

**Impact:** Cannot link `fuse_runtime` with both raw engines — build failure or undefined behaviour.

**Mitigation:**
- U0 report (this deliverable)  
- U2: static libs `fuse_t3d_legacy`, `fuse_t2d_legacy` with symbol prefix scripts  
- Ban raw dual link (prestarter §3.3)

**Residual:** Prefix maintenance cost until stranglers complete.

---

### R02 — Dual script VMs

**Evidence:** T3D `ts/` + `torquescript/` (81 files); T2D script in `console/` (50 files). Separate parsers, `CodeBlock`, AST headers.

**Impact:** Complexity tax, duplicate tooling, incompatible dialect drift.

**Mitigation:**
- U0–U2: quarantine `compat/ts_t3d`, `compat/ts_t2d`  
- U3: **decision gate** — Lua vs single TorqueScript vs other (prestarter §18.1)  
- Hard deadline for FUSE script host

**⚠ STAKEHOLDER:** Script end-state decision required.

---

### R03 — Box2D vs T3D physics

**Evidence:** T3D `collision/` (27 files) + optional Bullet; T2D in-tree `Box2D/` (113 files). No shared API.

**Impact:** Hybrid gameplay (2D character in 3D world) needs careful world boundaries; forces may not match.

**Mitigation:**
- Separate physics worlds per dimension (prestarter §17)  
- Shared interface at FUSE gameplay layer only  
- Track B may unify later

**⚠ STAKEHOLDER:** Box2D forever vs unified physics (prestarter §18.2).

---

### R04 — Gui duplication

**Evidence:** 45+ `Gui*` class collisions; T3D 247 gui files; T2D 124 + separate `editor/` tree.

**Impact:** Editor never ships if scope includes both Gui stacks; link collisions.

**Mitigation:**
- Product UI = Qt 6 `fuse_editor` only (U6)  
- Legacy Gui for parity testing only  
- One Gui stack max per runtime link unit

---

### R05 — Network model differences

**Evidence:**
- T3D: `GameConnection` extends `NetConnection`, ghosting, move lists (`T3D/gameBase/`)  
- T2D: `NetConnection` extends `ConnectionProtocol` + `SimGroup` (`network/netConnection.h`)  
- Both define `BitStream`, `ConnectionProtocol`, `GameConnection` — link collision

**Impact:** Shared multiplayer across 2D/3D dimensions not trivial; dual stacks if kept.

**Mitigation:**
- Keep dual net stacks behind FUSE session API short-term  
- Unified net is post-U8 / Track B consideration  
- Do not attempt merge without new protocol design

**⚠ STAKEHOLDER:** Sign-off on dual-net vs greenfield FUSE networking.

---

### R06 — Mobile / web (T2D) vs desktop (T3D)

**Evidence:** T2D CMake lists `platformAndroid`, `platformiOS`, `platformEmscripten`; T3D community builds desktop-focused (`platformSDL`, Win/Linux/macOS).

**Impact:** Single FUSE binary SKU may not cover all T2D deployment targets without platform matrix expansion.

**Mitigation:**
- Document platform matrix in U1 `BUILD.md`  
- `FUSE_WITH_*` flags per platform  
- Emscripten/Android as stretch goals post-U8

---

### R07 — Addon ore entanglement

**Evidence:**
- GMK, BadBehaviour, 3DAAK: full `Engine/` trees (~1,900–2,000 C++ files each)  
- Only small kernel dirs are unique (GMK `component/` 26 files; BadBehaviour `BadBehavior/` 74 files)

**Impact:** U5 slips if teams merge whole addon engines into FUSE `Engine/`.

**Mitigation:**
- Kernel-first extraction ([addon-ore-catalog.md](./addon-ore-catalog.md))  
- Submodules read-only after extract  
- **Explicit ban** on addon Engine merges (user constraint)

---

### R08 — Infinite dual CMake maintenance

**Evidence:** T3D root `CMakeLists.txt` + `Tools/CMake`; T2D `third_party/Torque2D/CMakeLists.txt` (CMake 3.21+, separate scripts).

**Impact:** Daily dev friction; CI duplication; drift.

**Mitigation:** U1 umbrella build — one configure graph, `FUSE_BUILD_T3D` / `FUSE_BUILD_T2D` options.

---

### R09 — Pre-merged AFX / Verve in FUSE root

**Evidence:** `Engine/source/afx/` (218 files), `Engine/source/Verve/` (163 files) already in FUSE root; submodules duplicate reference copies.

**Impact:** Double-merge attempts; unclear module boundaries for U5.

**Mitigation:**
- Treat root copies as **staging** for `fuse_fx` / `fuse_cinematics`  
- Submodule ore is reference only  
- U5 moves to proper `fuse_*` module targets

---

### R10 — GuideBot license

**Evidence:** `third_party/addons/GuideBot/guidebot_license_agreement.txt` — custom terms, not MIT.

**Impact:** Redistribution / commercial use constraints.

**Mitigation:** Legal review before shipping GuideBot-derived code; extract algorithms vs ship PDF/docs.

---

### R11 — Multiprocess fallback permanence

**Evidence:** Prestaster allows multiprocess compositor only as **temporary scaffold** (§7.1); does not count as unification exit.

**Impact:** "Two EXEs in a launcher" product failure mode.

**Mitigation:**
- Forbidden after U4 (prestarter recommendation §18.4)  
- Schedule explicit deletion milestone in U2 PR

**⚠ STAKEHOLDER:** Confirm multiprocess forbidden after U4.

---

### R12 — Bit-perfect legacy demo expectation

**Evidence:** Large addon art/script corpus; conversion pipeline not built until U7.

**Impact:** Morale / schedule slip if stakeholders expect pixel-identical Torque demos.

**Mitigation:** Document parity tolerance ([demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md)); FUSE demos as primary acceptance.

---

### R13 — Track B renderer before one-process spine

**Evidence:** Master plan Track B (Vulkan, DDGI) depends on unified `fuse::` APIs (prestarter §14).

**Impact:** Wasted glue on non-unified renderer.

**Mitigation:** Gate Track B on U2 one-process smoke + U4 dimension APIs.

---

### R14 — Singleton / static init order

**Evidence:** Dual `StringTable`, `Sim` dictionary, `Con::init`, platform globals — all historically static-init fragile.

**Impact:** U2 smoke crashes at startup even with renamed symbols if init order wrong.

**Mitigation:**
- U2 under ASan  
- Explicit init phases in `fuse_core` before legacy dim boot  
- No duplicate singletons in one link unit

---

### R15 — Qt editor scope (U6)

**Evidence:** Seven addon editor paradigms (BT graph, timeline, FX composer, mechanics, adventure, …).

**Impact:** Editor never ships if all panes required day one.

**Mitigation:** U6 vertical slice — **one** feature pane first (Timeline or BT recommended); project hub + dual viewports + PIE.

---

### R16 — Inheritance misuse (physics / gfx / net)

**Evidence:** Stakeholder 2D→3D extension strategy ([merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md)) applies inheritance only to scene/object identity. Temptation to “unify” by subclassing `Box2DWorld`, T3D `SceneObject`, or `GameConnection` for convenience.

**Impact:** Fragile hybrids — wrong lifetime coupling, impossible hybrid physics, renderer lock-in, link collisions resurface.

**Mitigation:**
- **Composition boundary:** `World2D` / `World3D` compose physics backends; `SceneObject2D/3D` hold render component refs — no backend inheritance.
- Code review gate at U4: reject PRs that inherit across physics/gfx/net layers.
- Document do/don't table in merge-strategy doc.

**Related risks:** R03 (physics), R04 (gfx), R05 (net).

---

### R17 — Data races on scene graph / SimObject

**Evidence:** T3D/T2D assume main-thread sim mutation; `SimSet` mutex protects iteration only (`console/simSet.h`); `Con::isMainThread()` guards script (`codeBlock.cpp`); 19 `Sim*` class collisions if dual-linked ([symbol-collision-report.md](./symbol-collision-report.md)). [concurrency-inventory.md](./concurrency-inventory.md) §2.5, §5.

**Impact:** Silent corruption, TSan explosions, non-reproducible crashes when jobs touch `fuse::Object` or legacy adapters unsafely.

**Mitigation:**
- [architecture-parallel.md](./architecture-parallel.md) §5–6: handles, snapshots, game-thread mutation only
- `SceneObject2D` → `SceneObject3D` merge uses SOA + command buffers ([merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md))
- TSan nightly from U3; forbid raw `SimObject*` in worker code review
- WP-05 / WP-06 in [work-plan.md](./work-plan.md)

**Residual:** Legacy compat scripts may force main-thread tick until U7 converters.

---

### R18 — GL / GFX context affinity

**Evidence:** T3D Theora explicitly cannot upload textures on worker threads (`gfx/video/theoraTexture.h` L151–152); no render thread in either engine ([concurrency-inventory.md](./concurrency-inventory.md) §2.6).

**Impact:** GPU crashes or black screens if render record moves to workers prematurely.

**Mitigation:** v1 record+present on game thread ([architecture-parallel.md](./architecture-parallel.md) §4.3); workers only stage CPU buffers.

---

### R19 — Job scheduler misuse (blocking game thread)

**Evidence:** T3D pattern `waitForAllItems()` on main thread (`imageUtils.cpp`); global `ThreadPool` deadlock if non-main submits (`threadPool.h` L51–55).

**Impact:** Frame hitches, deadlocks when mixing legacy pool with FUSE jobs.

**Mitigation:** Fiber `JobCounter::wait()` yields workers not game thread; legacy pool quarantined inside prefixed libs; WP-03 in [work-plan.md](./work-plan.md).

---

### R20 — Mobile thermal / battery

**Evidence:** Phones have 2–8 cores with thermal limits; T2D tracks `backgrounded` on iOS/Android (`platformiOS.h`, `platformAndroid.h`); desktop `cores-2` formula overheats or wastes battery on mobile.

**Impact:** Frame drops, OS kills app, poor store ratings.

**Mitigation:** Adaptive `N = clamp(cores - reserve, min, max)` with **mobile max 4**; background `N ≤ 1`; I/O frame budgets ([architecture-parallel.md](./architecture-parallel.md) §3.1.1, §3.6); WP-03 + Agent G in [work-plan.md](./work-plan.md).

---

### R21 — GPU context affinity (mobile GLES/Metal)

**Evidence:** T2D GLES on main thread (`iOSGL2ES.mm`, `AndroidGL2ES.cpp`); T3D forbids worker texture upload (`theoraTexture.h`).

**Impact:** Black screen, GL errors, crashes on device if desktop-only render-thread assumptions leak.

**Mitigation:** `fuse::platform::renderThread()` invariant; workers produce staging buffers only (§4.4); Track B RHI must preserve rule on Metal/Vulkan mobile.

---

## Decision gates (from prestarter §18)

| Gate | Risk IDs | Recommendation |
|------|----------|----------------|
| Script host end-state | R02 | Decide by end of U3 |
| Physics end-state | R03, R16 | Box2D for 2D short-term; composition only — no World3D : Box2D |
| 2D renderer | R04, R06, R16 | Keep T2D GL until Vulkan 2D; render via composition on SceneObject* |
| 2D→3D scene merge | R16, R17 | Inheritance for SceneObject2D/3D only; MT via handles/snapshots |
| Job model | R19 | Fiber work-stealing default — [architecture-parallel.md](./architecture-parallel.md) §12 |
| Editor in-process | R17 | Qt UI thread vs game thread queue |
| Mobile platform scope | R20, R21 | Desktop + mobile locked — adaptive jobs + portable GFX |
| Multiprocess fallback | R11 | **Forbidden after U4** |
| Product SKU | R06 | Unified binary with `FUSE_WITH_2D` / `FUSE_WITH_3D` flags |

---

## Gate U0 checklist item

- [x] Risk register published under `docs/unification/`
- [ ] Risks reviewed in stakeholder session
- [ ] Decision gates scheduled
