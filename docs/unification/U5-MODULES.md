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

- Flat behavior tree nodes (`Sequence`, `Selector`, `ConditionDistanceLess`, `ActionSetFlag`)
- `BehaviorRuntime`: `buildSnapshots()` → `evaluate()` (`JobScheduler::parallel_for`) → `commit()`
- Demo tree `makePatrolWhenNearTarget()` — patrol flag when agent within 5 units of target
- Unit tests: `fuse_ai_tests` (`ctest` name `fuse_ai_behavior_tree`)
- Hybrid demo: `demo_hybrid_hud` ticks `BehaviorRuntime` each frame after `HybridComposer::tick`

### Ore extraction backlog (BadBehaviour / GuideBot / UAISK)

| Priority | Source path | FUSE destination |
|----------|-------------|------------------|
| P0 | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/` | Replace flat nodes with ore node registry |
| P1 | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/decorator/` | Decorators (`Loop`, `Inverter`, …) |
| P1 | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/leaf/` | Scripted / compiled leaves |
| P2 | `third_party/addons/GuideBot/guideBotT3D/engine/.../guideBot/` | Navigation helpers (check custom license) |
| P3 | `third_party/addons/UAISK/.../UAISK/*.cs` | Templates under `Samples/Modules/ai/` only |

**Do not compile** from `third_party/addons/*/Engine/` — extract kernels into `Source/FUSE/Modules/ai/`.

---

## 4. Scaffold modules (compile-ready TODOs)

### `fuse_cinematics`

- `Timeline` + `Track` stubs with playhead advance on game thread
- TODO: extract `VController`, `VTrack`, `VEvent` from `Engine/source/Verve/` (already in FUSE root) into this target

### `fuse_fx`

- `FxComposer` + `FxSocket` attachment stub
- TODO: refactor `Engine/source/afx/` (218 files) into module; sample content from `third_party/addons/AFX-Template/game/`

### `fuse_mechanics`

- `Component` + `Interactable` stubs
- TODO: extract GMK `SimComponent` from `third_party/addons/GMK/Engine/source/component/`

### `fuse_adventure`

- `Inventory` + `InteractionSystem` stubs sharing mechanics handles
- TODO: extract 3DAAK interaction/inventory scripts as FUSE APIs + `Samples/Modules/adventure/`

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
| `fuse_ai` | BT drives 2D + 3D agents in hybrid demo | 🚧 2D HUD agent only; 3D agent stub next |
| `fuse_cinematics` | 30s timeline moves camera + sprite | ⬜ scaffold |
| `fuse_fx` | AFX on 3D model + 2D sprite | ⬜ scaffold |
| `fuse_mechanics` | One 3D interactable | ⬜ scaffold |
| `fuse_adventure` | Pick-up / use in 3D + 2D interface | ⬜ scaffold |

---

## 8. Related docs

- [work-plan.md](./work-plan.md) — WP-07 / WP-08 status
- [unified-layout.md](./unified-layout.md) — L3 directory map
- [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) — U8 demo mapping
