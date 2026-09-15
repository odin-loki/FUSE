# FUSE U0 — Subsystem Matrix

**Phase:** U0 Inventory & collision map  
**Date:** 2026-09-15  
**Purpose:** Per-subsystem comparison of T3D vs T2D implementations with a recommended unification strategy for FUSE.

**Stakeholder direction (2026-09-15):** Selective **2D→3D extension merge** for scene/object identity — see [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md). Sim/objects recommendation below updated accordingly.

**Legend — Recommendation column:**

| Value | Meaning |
|-------|---------|
| **Replace with FUSE** | New `fuse::` implementation; legacy quarantined until strangler completes |
| **Merge** | Single shared implementation extracted from one or both trees |
| **Keep dual** | Both legacy implementations coexist behind facades (short–medium term) |
| **⚠ STAKEHOLDER** | Needs explicit sign-off before implementation |

---

## Matrix

| Subsystem | T3D implementation | T2D implementation | Recommendation | Rationale |
|-----------|-------------------|-------------------|----------------|------------|
| **Memory** | `Engine/source/util/` (`DataChunker`, allocators in `core/`), chunk allocators tied to `SimObject` / console | `engine/source/memory/` (dedicated tree), `string/` buffers | **Replace with FUSE** | Track A P1: C++23 allocators, handles, ASan-first. Neither legacy allocator is memory-safe. ~111 util class collisions if merged raw. |
| **Strings** | `Engine/source/core/strings/`, `stringTable`, `stringUnit`, `StringTable` singleton | `engine/source/string/` (`stringTable`, `stringBuffer`, `stringStack`) | **Replace with FUSE** ⚠ STAKEHOLDER | Dual `StringTable` singletons will explode at link. FUSE core string table + interned handles. **Sign-off:** migration timeline for existing `.mis`/module string refs. |
| **Console / script** | `Engine/source/console/` (116 files), `ts/` + `cinterface/` (81 files), TorqueScript compiler under `torquescript/` | `engine/source/console/` (50 files), script in same tree | **Merge** → **Replace with FUSE** | Extract shared `ConsoleObject` / `Object` DNA into `fuse_core` (U3); dual VMs quarantined U0–U2 (`compat/ts_t3d`, `compat/ts_t2d`). U3+: single FUSE script host. **⚠ STAKEHOLDER:** Lua vs single TorqueScript dialect (prestarter §18 gate 1). 33 `Con::` collisions — single `fuse::` reflection path per [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md). |
| **Sim / objects** | `Engine/source/console/simObject.*`, `sim/`, `scene/`, `T3D/` (389 files) — 3D `SceneObject`, zones, `ShapeBase` | `engine/source/sim/`, `2d/`, `game/`, `component/` (197 files) — `Scene2D`, `SceneObject2D`, components | **Merge / Replace with FUSE** | **Stakeholder:** greenfield `fuse::Object` → `SceneObject2D` → `SceneObject3D` (3D extends 2D). Legacy `SimObject` in prefixed libs only; adapters U2–U4. `World2D` / `World3D` as `IDimension` (U4). Do **not** link dual legacy `SimObject` definitions. |
| **Input** | `Engine/source/platform/input/` (keyboard, mouse, gamepad via platform layers) | `engine/source/input/` + platform input | **Merge** via FUSE facade | U3: `fuse::input` feeding both dimensions. Editor path via Qt (U6). Low unique symbol overlap; behavioural API similar. |
| **Gfx** | `Engine/source/gfx/` + `renderInstance/`, `shaderGen/`, `postFx/`, `lighting/`, `materials/`, `windowManager/` (417 files) — D3D11/GL/null backends | `engine/source/graphics/` (35 files) — GL ortho 2D pipeline | **Keep dual** (composition) | `SceneObject2D/3D` hold render **component refs** — do not inherit T2D GL under T3D gfx. U4 hybrid compositor composites separate targets. **⚠ STAKEHOLDER:** 2D renderer path until Track B Vulkan (prestarter §18 gate 3). |
| **Audio** | `Engine/source/sfx/` (65 files) — OpenAL/FMOD/XAudio/etc. | `engine/source/audio/` (19 files) | **Merge** via FUSE mixer API | U3: single mixer facade; backends may stay dual short-term. Only 1 major class name collision (`AudioDescription`). |
| **Net** | `Engine/source/T3D/gameBase/gameConnection.h`, ghosting, server/client model, `BitStream` in `core/stream/` | `engine/source/network/` — `NetConnection`, `ConnectionProtocol`, `gameConnection.h`, `BitStream` in `io/` | **Keep dual** ⚠ STAKEHOLDER | Both define `GameConnection`, `BitStream`, `NetConnection` — link collision. Game networking models differ (T3D FPS/ghost vs T2D simpler model). Shared only at FUSE session/save layer until unified net design. |
| **Files / I/O** | `Engine/source/core/stream/`, `platform/` file dialogs, zip streams, `persistence/` | `engine/source/io/`, `persistence/`, `platform/` streams | **Replace with FUSE** | U3: FUSE VFS mounts (`/t3d/`, `/t2d/`, `/game/`). 25+ stream basename collisions (`fileStream.h`, `bitStream.h`, …). |
| **Gui** | `Engine/source/gui/` (247 files) — in-engine editor + HUD | `engine/source/gui/` (124 files) + separate `editor/` tree | **Replace with FUSE** (Qt 6) | 45+ class collisions. Product UI must be `fuse_editor` (U6). Legacy Gui* retained only for parity testing. **No merge.** |
| **Physics** | `Engine/source/collision/` (27 files) — T3D internal + optional Bullet in `Engine/lib/bullet` | `engine/source/Box2D/` (113 files) — in-tree Box2D | **Keep dual** (composition) ⚠ STAKEHOLDER | `World2D` / `World3D` **compose** backends — **no** `World3D : Box2DWorld` or physics inheritance ([merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md)). **Sign-off:** Box2D forever for 2D vs Track B unified physics (prestarter §18 gate 2). |
| **Assets** | `Engine/source/assets/`, `persistence/taml/`, `module/`, AssetPtr system | `engine/source/assets/`, `persistence/`, `bitmapFont/`, module/asset tags | **Merge** via FUSE asset DB | U3/U7: `fuse::AssetId`, dimension tags (`dim:2d`, `dim:3d`). 33 Taml/Asset class collisions — extract shared serialisation concepts, not raw headers. |

---

## Subsystem file counts (evidence)

| Subsystem | T3D dirs (under `Engine/source`) | T3D files | T2D dirs (under `engine/source`) | T2D files |
|-----------|----------------------------------|-----------|----------------------------------|-----------|
| console | `console` | 116 | `console` | 50 |
| platform | `platform`, `platformWin32`, `platformPOSIX`, `platformMac`, `platformSDL`, `platformX11`, `platformX86UNIX` | 193 | `platform`, `platformWin32`, `platformX86UNIX`, `platformAndroid`, `platformEmscripten`, `platformiOS`, `platformOSX` | 287 |
| math | `math` | 97 | `math` | 49 |
| util | `util`, `core` | 211 | `string`, `memory`, `algorithm`, `delegates`, `debug`, `collection`, `messaging` | 85 |
| gui | `gui` | 247 | `gui` | 124 |
| sim | `sim`, `scene`, `T3D` | 389 | `sim`, `2d`, `game`, `component` | 197 |
| gfx | `gfx`, `renderInstance`, `shaderGen`, `postFx`, `lighting`, `materials`, `windowManager` | 417 | `graphics` | 35 |
| audio | `sfx` | 65 | `audio` | 19 |
| physics | `collision` | 27 | `Box2D` | 113 |
| assets | `assets`, `persistence` | 95 | `assets`, `persistence`, `bitmapFont` | 81 |
| script | `ts`, `cinterface` | 81 | (in `console`) | — |

---

## Stakeholder sign-off summary

Items marked **⚠ STAKEHOLDER** require explicit decisions before U3+ implementation:

| # | Decision | Options | Prestarter reference |
|---|----------|---------|---------------------|
| 1 | Script host end-state | Lua / single TorqueScript / other | §18 gate 1 |
| 2 | Physics end-state | Box2D for 2D forever / Track B unified physics | §18 gate 2 |
| 3 | 2D renderer path | T2D GL until Vulkan 2D / early common RHI | §18 gate 3 |
| 4 | Networking unification | Dual net stacks indefinitely / new FUSE net layer | §8.3, this matrix |
| 5 | String table migration | Big-bang FUSE strings / gradual compat aliases | Track A P1 |

---

## Track A alignment notes

| U phase | Subsystems touched | Master plan parallel |
|---------|-------------------|---------------------|
| U1–U2 | Build graph only | Track A P0 skeleton |
| U3 | memory, strings, log, I/O, input, audio, assets IDs | Track A P1–P3 |
| U4 | gfx compose, `SceneObject2D/3D`, World2D/3D | Track A P4 handles |
| U6 | gui → Qt | Track A P5 / B6 |
| U7 | assets import, persistence | Track A P6 compat |

---

## Gate U0 checklist item

- [x] Subsystem matrix published under `docs/unification/`
- [x] Stakeholder direction on sim/objects merge (2D→3D extension) — [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md)
- [ ] Remaining sign-off on script host, physics end-state, 2D renderer (pending)
