# FUSE U5 — Feature Modules (WP-07)

**Phase:** U5 scaffolding + one vertical slice  
**Date:** 2026-09-15  
**Depends on:** [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md) (`World2D`/`World3D`, `HybridComposer`, job spine)  
**Ore policy:** [addon-ore-catalog.md](./addon-ore-catalog.md) — **no** addon `Engine/` merges into FUSE `Engine/`

---

## 1. CMake targets

| Target | Path | Links | Ore hint |
|--------|------|-------|----------|
| `fuse_ai` | `Source/FUSE/Modules/ai/` | `fuse_core`, `fuse_world2d` | BadBehaviour, GuideBot, UAISK |
| `fuse_cinematics` | `Source/FUSE/Modules/cinematics/` | `fuse_core`, `fuse_world2d`, `fuse_world3d` | Verve |
| `fuse_fx` | `Source/FUSE/Modules/fx/` | `fuse_core`, `fuse_world2d`, `fuse_world3d` | AFX |
| `fuse_mechanics` | `Source/FUSE/Modules/mechanics/` | `fuse_core`, `fuse_world3d` | GMK |
| `fuse_adventure` | `Source/FUSE/Modules/adventure/` | `fuse_core`, `fuse_mechanics` | 3DAAK |

Umbrella options (root `CMakeLists.txt`):

- `FUSE_BUILD_MODULES=ON` (default) — builds all five module libraries
- `FUSE_BUILD_EDITOR_API=ON` (default) — builds `fuse_editor_api` command-queue stub (WP-08 light touch)

---

## 2. Threading model (all modules)

Per [architecture-parallel.md](./architecture-parallel.md) §7:

| Phase | Thread | Rule |
|-------|--------|------|
| Scene / blackboard commit | Game | Only thread that mutates `fuse::Object` graph |
| Eval / simulate | Job workers | Read-only snapshots; write per-agent scratch buffers |
| Apply results | Game | Commit blackboard, spawn FX, inventory changes |

Modules **must not** hold raw scene pointers across worker jobs. Use handles + immutable snapshots (same pattern as `SceneSnapshot2D`).

---

## 3. Vertical slice: `fuse_ai`

### Implemented

- **Node registry** (`NodeRegistry`, `loadTreeFromSpecs`, `loadTreeFromText`) — BadBehaviour `DECLARE_CONOBJECT` pattern without SimObject/Con::
- Built-in type ids: `bb.sequence`, `bb.selector`, `bb.inverter`, `bb.loop`, `bb.succeed_always`, `bb.root`, `bb.condition.distance_less`, `bb.condition.distance_greater`, `bb.condition.blackboard_get`, `bb.action.set_flag`, `bb.action.blackboard_set`, `bb.action.wait`, `bb.action.distance`, `gb.action.move_toward`
- Flat behavior tree evaluator with decorator support (Inverter, Loop, SucceedAlways, Root)
- `BehaviorRuntime`: `buildSnapshots()` → `evaluate()` (`JobScheduler::parallel_for`) → `commit()`
- Demo tree `makePatrolWhenNearTarget()` + registry/text load equivalents
- UAISK script-only template hooks (`uaisk_template_hooks.hpp`, `Samples/Modules/ai/uaisk-templates/`)
- Unit tests: `fuse_ai_tests` — registry parity, decorators, leaf nodes (wait, blackboard set/get, distance), text loader, UAISK hooks, parallel runtime
- Hybrid demo: `demo_hybrid_hud` ticks `BehaviorRuntime` each frame after `HybridComposer::tick`

### Ore extraction (BadBehaviour / GuideBot / UAISK)

| Status | Source path | FUSE destination |
|--------|-------------|------------------|
| ✅ P0 | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/` | `node_registry.hpp`, flat `BehaviorNode` |
| ✅ P1 | `.../BadBehavior/composite/` | `bb.sequence`, `bb.selector` |
| ✅ P1 | `.../BadBehavior/decorator/` | `bb.inverter`, `bb.loop`, `bb.succeed_always`, `bb.root` |
| 🚧 P1 | `.../BadBehavior/leaf/` | Scripted/compiled leaves — `bb.action.set_flag`, `bb.action.wait`, `bb.action.blackboard_set`, `bb.condition.blackboard_get`, `bb.action.distance` |
| 🚧 P2 | `third_party/addons/GuideBot/.../guideBot/actionMove.h` | `gb.action.move_toward` stub (custom license) |
| ✅ P3 | `third_party/addons/UAISK/.../UAISK/*.cs` | Template hooks + `Samples/Modules/ai/uaisk-templates/` |

**Do not compile** from `third_party/addons/*/Engine/` — extract kernels into `Source/FUSE/Modules/ai/`. See [Modules/ai/README.md](../../Source/FUSE/Modules/ai/README.md).

---

## 4. Scaffold modules (compile-ready TODOs)

### `fuse_cinematics`

- `Timeline`, `Playhead`, `Track`, `TimelineEvent`, `TrackGroup` — Verve `VController`/`VTrack`/`VEvent`/`VGroup` kernel (see [U5-MODULES-cinematics.md](./U5-MODULES-cinematics.md))
- Tests: `fuse_cinematics_tests` (30s advance, track span, interpolation)
- TODO: Torque bridge tracks (`VMotionTrack`, `VPath`, …) and hybrid demo camera/sprite drive

### `fuse_fx`

- **Implemented (vertical slice)**
  - `EffectDescriptor` / `SpellDescriptor` registry with `makeSparkBurst`, `makeMuzzleFlash`, `makeFireball`
  - `EffectTimeline` — `afxPhrase`/`afxEffectron` duration + loop tick on socket attach
  - `FxSocket` attach validates registered effects and starts timeline playback
  - `CastPipeline` — spell phase state machine with `CastPhaseEnterHook`
  - Impact phase auto-enqueues `ResidualEffectQueue` entries (fireball scorch path)
  - `FxComposer::tick()` advances effect timeline, casts, and residuals
  - Tests: `fuse_fx_tests` — registry, socket reject, timeline completion, phase progression, cast→residual
  - Demo: `demo_fx` registers descriptors, attaches 2D/3D sockets, begins fireball cast
- TODO: refactor remaining `Engine/source/afx/` (218 files) into module; GPU particle pools; sample content from `third_party/addons/AFX-Template/game/`

#### Ore extraction (AFX)

| Status | Source path | FUSE destination |
|--------|-------------|------------------|
| ✅ P0 | `Engine/source/afx/afxEffectron.h` | `EffectDescriptor`, `EffectTimeline` |
| ✅ P0 | `Engine/source/afx/afxMagicSpell.h` | `SpellDescriptor`, `CastPipeline`, `SpellPhase` |
| ✅ P1 | `Engine/source/afx/afxEffectWrapper.h` | `EffectEntry`, `EffectTiming` |
| ✅ P1 | `Engine/source/afx/afxResidueMgr.h` | `ResidualEffectQueue` |
| ✅ P1 | `Engine/source/afx/afxChoreographer.h` | `FxComposer` orchestration |
| 🚧 P2 | `Engine/source/afx/afxConstraint.h` | Socket constraint remapping |
| 🚧 P2 | `Engine/source/afx/util/afxParticlePool.h` | Particle sim jobification |
| 🚧 P3 | `third_party/addons/AFX-Template/game/` | Sample spell/FX content pack |

**Do not compile** from `third_party/addons/*/Engine/` — extract kernels into `Source/FUSE/Modules/fx/`. See [Modules/fx/README.md](../../Source/FUSE/Modules/fx/README.md).

### `fuse_mechanics`

- **Component kernel** (`Component`, `ComponentInterface`, `ComponentInterfaceCache`) — GMK `SimComponent` pattern without SimObject/Con::
- **Interactable surface** (`IInteractable`, `InteractableComponent`, `InteractableInterface`) with verb/item `InteractionContext`
- **Action stubs** (`InteractActionKind`, `InteractAction`, `UseActionStub`, `PickupActionStub`, `ExamineActionStub`) — verb dispatch without adventure deps
- **Inventory provider** (`IInventoryProvider`, `InventoryProviderInterface`) — string-keyed instigator bag for adventure bridge
- **Registry** (`MechanicsRegistry`) — explicit registration + component-tree `resolveInteractable`
- Tests: `fuse_mechanics_tests`, `fuse_mechanics_inventory_provider`, `fuse_mechanics_action_stubs`
- TODO: extract remaining GMK `SimComponent` leaves from `third_party/addons/GMK/Engine/source/component/`

### `fuse_adventure`

- **Inventory** (`Inventory`, `InventoryComponent`) — 3DAAK `ShapeBase::incInventory` / `decInventory` / `hasInventory` parity
- **Interaction** (`InteractionSystem`, `IInteractable`, `PickupInteractable`, `PuzzleGate`)
- **Interaction queue** (`InteractionQueue`, `QueuedInteraction`) — game-thread enqueue/drain via `MechanicsBridge` (mirrors `fuse::editor::CommandQueue`)
- **Mechanics bridge** (`AdventureInteractableComponent`, `MechanicsBridge`) — dispatches mechanics `InteractionContext` through adventure use/pickup
- Links `fuse_mechanics`; vertical slice: pickup key → use on puzzle gate (Outpost flow)
- Tests: `fuse_adventure_inventory`, `fuse_adventure_interact`, `fuse_adventure_vertical_slice`, `fuse_adventure_interaction_queue`
- Demo: `demo_adventure_stub` exercises inventory + door unlock
- TODO: extract remaining 3DAAK interaction scripts as FUSE APIs + `Samples/Modules/adventure/`

---

## 5. WP-08 editor boundary (U6)

| Target | Header | Role |
|--------|--------|------|
| `fuse_editor_api` | `fuse/editor/command_queue.hpp`, `fuse/editor/editor_host.hpp` | UI `postFromUi()` → game `gameTick()` / `drain()` |
| `fuse_editor` | Qt 6 shell (`FUSE_BUILD_EDITOR`) | Project hub + viewport placeholder (desktop-only) |

- **No Qt** in `fuse_core` or module headers
- Full editor UX: [U6-EDITOR.md](./U6-EDITOR.md)
- Tests: `fuse_editor_command_queue`, `fuse_editor_host` (headless; no Qt required in CI)

---

## 6. Verification

```bash
cmake -B build-fuse -G Ninja \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_MODULES=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_LEGACY=ON \
  -DFUSE_BUILD_SMOKE=ON \
  -DFUSE_BUILD_HYBRID_DEMO=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-fuse
ctest --test-dir build-fuse --output-on-failure
./build-fuse/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

---

## 7. U5 gates (prestarter §10) — status

| Module | Gate | This PR |
|--------|------|---------|
| `fuse_ai` | BT drives 2D + 3D agents in hybrid demo | 🚧 registry + decorators landed; 3D agent stub next |
| `fuse_cinematics` | 30s timeline moves camera + sprite | 🚧 kernel + tests; hybrid drive next |
| `fuse_fx` | AFX on 3D model + 2D sprite | 🚧 socket attach + effect timeline + fireball cast path; hybrid drive next |
| `fuse_mechanics` | One 3D interactable | 🚧 component + action stubs + registry; hybrid demo next |
| `fuse_adventure` | Pick-up / use in 3D + 2D interface | 🚧 inventory + interaction queue + vertical slice + `demo_adventure_stub` |

---

## 8. Related docs

- [work-plan.md](./work-plan.md) — WP-07 / WP-08 status
- [unified-layout.md](./unified-layout.md) — L3 directory map
- [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) — U8 demo mapping
