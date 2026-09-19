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
- Built-in type ids: `bb.sequence`, `bb.selector`, `bb.parallel`, `bb.inverter`, `bb.loop`, `bb.succeed_always`, `bb.root`, `bb.condition.distance_less`, `bb.condition.distance_greater`, `bb.condition.blackboard_get`, `bb.condition.allies_in_radius`, `bb.action.set_flag`, `bb.action.blackboard_set`, `bb.action.wait`, `bb.action.distance`, `bb.action.nearest_ally`, `gb.action.move_toward`
- Flat behavior tree evaluator with decorator support (Inverter, Loop, SucceedAlways, Root)
- `BehaviorRuntime`: `buildSnapshots()` → `evaluate()` (`JobScheduler::parallel_for`) → `commit()`
- Demo tree `makePatrolWhenNearTarget()` + registry/text load equivalents
- UAISK script-only template hooks (`uaisk_template_hooks.hpp`, `Samples/Modules/ai/uaisk-templates/`)
- Blackboard spatial query stubs: `spatial_query.hpp` — ally radius count/filter, nearest-ally lookup (max-radius, tie-break); `teamId` on `AgentSnapshot` / `AgentBinding`; optional ally-index scalar write on `bb.action.nearest_ally`
- `BehaviorRuntime::commit()` applies `gb.action.move_toward` position deltas via `AgentBinding::moveSpeed`
- `BehaviorTree::makeMoveTowardDemoTree()` — single-node hybrid 3D agent chase tree
- `BehaviorTree::makePatrolWithAllySupportDemoTree()` — selector(squad `allies_in_radius` → flag 1, patrol `distance_less` → flag 0)
- UAISK template pack: `Samples/Modules/ai/uaisk-templates/patrol_squad.bt` (text loader import)
- `BehaviorRuntime::setBindingPosition()` — sync squad-mate positions for multi-runtime ally spatial
- Unit tests: `fuse_ai_behavior_tree` — registry parity, composite child-status aggregation (sequence/selector/parallel with success/fail thresholds + abort-on-fail + parallel/spatial), empty blackboard/ally context, blackboard try-get/set + typed scalars, leaf nodes (wait, blackboard set/get, distance, allies_in_radius, nearest_ally), spatial query radius filter, text loader, UAISK hooks + patrol_squad template, multi-agent parallel runtime, move-toward runtime commit, move-toward demo factory, patrol-with-ally demo factory
- Hybrid demo: `demo_hybrid_hud` drives `SceneObject3D` agent via `makeMoveTowardDemoTree()` + ally patrol runtime with spatial squad flag (`hybrid_module_gates.cpp`)

### Ore extraction (BadBehaviour / GuideBot / UAISK)

| Status | Source path | FUSE destination |
|--------|-------------|------------------|
| ✅ P0 | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/` | `node_registry.hpp`, flat `BehaviorNode` |
| ✅ P1 | `.../BadBehavior/composite/` | `bb.sequence`, `bb.selector`, `bb.parallel` |
| ✅ P1 | `.../BadBehavior/decorator/` | `bb.inverter`, `bb.loop`, `bb.succeed_always`, `bb.root` |
| ✅ P1 | `.../BadBehavior/leaf/` | Scripted/compiled leaves — `bb.action.set_flag`, `bb.action.wait`, `bb.action.blackboard_set`, `bb.condition.blackboard_get`, `bb.action.distance` |
| ✅ P1 | `.../BadBehavior/decorator/Monitor.h` | `bb.monitor` decorator (guarded + observer children) |
| ✅ P2 | `third_party/addons/GuideBot/.../guideBot/actionMove.h` | `gb.action.move_toward` — Running/Success arrival, direction scalar, runtime position commit (custom license) |
| ✅ P3 | `third_party/addons/UAISK/.../UAISK/*.cs` | Template hooks + `Samples/Modules/ai/uaisk-templates/` |

**Do not compile** from `third_party/addons/*/Engine/` — extract kernels into `Source/FUSE/Modules/ai/`. See [Modules/ai/README.md](../../Source/FUSE/Modules/ai/README.md).

---

## 4. Scaffold modules (compile-ready TODOs)

### `fuse_cinematics`

- `Timeline`, `Playhead`, `Track`, `TimelineEvent`, `TrackGroup` — Verve `VController`/`VTrack`/`VEvent`/`VGroup` kernel (see [U5-MODULES-cinematics.md](./U5-MODULES-cinematics.md))
- Typed tracks: `CameraTrack` (FOV/roll keyframes + `LookAtResolver` stub), `SpriteTrack`, `PropertyTrack`, `AudioTrack`, `EventTrack`
- `Playhead::scrub_to`, `Timeline::scrub_to` — editor seek without playback; forward scrub enqueues cues
- `CueQueue` — pending cue buffer with `drain()` for game-thread commit
- Tests: `fuse_cinematics_tests` (30s advance, track span, interpolation, scrub, cue queue)
- **MotionTrack** / **MotionPath** — `VMotionTrack` + `VPath` linear waypoint sampling (no Torque `PathObject` bridge)
- **ActorTrack** — VActor mount/unmount event lane (`mount_point_at` sampling)
- **HybridTimelineSample** / `sample_hybrid_timeline_drive()` — camera clear tint + sprite position sampling for hybrid frame
- Tests: `fuse_cinematics_tests` includes motion path length + midpoint sampling, actor mount/unmount, hybrid timeline drive
- **VActorBridge** / `drain_actor_cues()` — headless mount/unmount bridge (VActor event signal without ShapeBase)
- **Outpost intro stub** — `make_outpost_intro_30s_stub()` (30s sprite/camera/actor/motion content)
- Hybrid demo: `demo_hybrid_hud` timeline advances sprite track + camera clear tint + VActor mount cue each frame
- TODO: full Torque `ShapeBase` attach; load 30s sequence from `Samples/Modules/cinematics/` asset file

### `fuse_fx`

- **Implemented (vertical slice)**
  - `EffectDescriptor` / `SpellDescriptor` registry with `makeSparkBurst`, `makeMuzzleFlash`, `makeFireball`
  - `EffectTimeline` — `afxPhrase`/`afxEffectron` duration + loop tick on socket attach
  - `FxSocket` attach validates registered effects and starts timeline playback
  - `CastPipeline` — spell phase state machine with `CastPhaseEnterHook`
  - Impact phase auto-enqueues `ResidualEffectQueue` entries (fireball scorch path)
  - `EffectGraph` — parent/child group tick stub (`afxEffectGroup` ore)
  - `bind::ParameterBinder` — runtime `caster` / `target` cast slot resolution
  - `FxComposer::tick()` advances effect timeline, effect graph, casts, and residuals
  - `MissileDescriptor` / `MissilePipeline` — `afxMagicMissileData` velocity/ballistic/lifetime stub wired into `FxComposer::tick`
  - `FxComposer::registerDemoVerticalSlice()` — spark/muzzle/fireball bundle for demos
  - `registerAfxTemplateSamplePack()` + `Samples/Modules/fx/afx_minimal_pack.txt` — AFX-Template sample effect ids
  - `FxComposer::particlePool()` — CPU pool tick hooked into composer (spawn on active effect timeline)
  - Tests: `fuse_fx_runtime` — registry, socket reject, timeline completion, graph tick, parameter bind, phase progression, cast→residual, missile pipeline, demo vertical slice registration, AFX template pack, composer particle pool
  - Demo: `demo_fx` registers descriptors, attaches 2D/3D sockets, begins fireball cast
  - Hybrid demo: `demo_hybrid_hud` attaches spark (2D) + muzzle (3D) sockets, AFX template pack, and ticks `FxComposer` + particle pool each frame
- TODO: refactor remaining `Engine/source/afx/` (218 files) into module; GPU particle pool backend

#### Ore extraction (AFX)

| Status | Source path | FUSE destination |
|--------|-------------|------------------|
| ✅ P0 | `Engine/source/afx/afxEffectron.h` | `EffectDescriptor`, `EffectTimeline` |
| ✅ P0 | `Engine/source/afx/afxMagicSpell.h` | `SpellDescriptor`, `CastPipeline`, `SpellPhase` |
| ✅ P1 | `Engine/source/afx/afxEffectWrapper.h` | `EffectEntry`, `EffectTiming` |
| ✅ P1 | `Engine/source/afx/afxResidueMgr.h` | `ResidualEffectQueue` |
| ✅ P1 | `Engine/source/afx/afxChoreographer.h` | `FxComposer` orchestration |
| ✅ P2 | `Engine/source/afx/afxEffectGroup.h` | `EffectGraph` group tick stub |
| ✅ P2 | `Engine/source/afx/afxConstraint.h` | `SocketConstraintManager`, `parse_constraint_spec`, socket remap |
| ✅ P2 | `Engine/source/afx/util/afxParticlePool.h` | `ParticlePool` CPU stub (spawn/tick/cull) |
| ✅ P2 | `Engine/source/afx/afxMagicMissile.h` | `MissileDescriptor`, `MissilePipeline` projectile sim stub |
| ✅ P3 | `third_party/addons/AFX-Template/game/` | `registerAfxTemplateSamplePack()` + `Samples/Modules/fx/afx_minimal_pack.txt` |

**Do not compile** from `third_party/addons/*/Engine/` — extract kernels into `Source/FUSE/Modules/fx/`. See [Modules/fx/README.md](../../Source/FUSE/Modules/fx/README.md).

### `fuse_mechanics`

- **Component kernel** (`Component`, `ComponentInterface`, `ComponentInterfaceCache`) — GMK `SimComponent` pattern without SimObject/Con::
- **Interactable surface** (`IInteractable`, `InteractableComponent`, `InteractableInterface`) with verb/item `InteractionContext`
- **Action stubs** (`InteractActionKind`, `InteractAction`, `UseActionStub`, `PickupActionStub`, `ExamineActionStub`) — verb dispatch without adventure deps
- **Inventory provider** (`IInventoryProvider`, `InventoryProviderInterface`) — string-keyed instigator bag for adventure bridge
- **Registry** (`MechanicsRegistry`) — explicit registration + component-tree `resolveInteractable`
- **HealthComponent** (`IHealthProvider`, `HealthProviderInterface`) — GMK damageable `SimpleComponent` leaf
- **TriggerZoneComponent** — T3D `Trigger` enter/leave/tickPeriodMS AABB volume stub
- **ToggleComponent** — GMK toggle/switch leaf (`toggle()`, `setState()`)
- **ConsoleMethodComponent** — GMK `DynamicConsoleMethodComponent` distilled (`registerMethod`, `invoke`)
- **PolyhedronTriggerZone** / `ConvexPolyhedron` — T3D `Trigger` polyhedron half-space containment (no physics bridge)
- Tests: `fuse_mechanics_tests`, `fuse_mechanics_inventory_provider`, `fuse_mechanics_action_stubs`, `fuse_mechanics_health`, `fuse_mechanics_trigger_zone`, `fuse_mechanics_toggle`, `fuse_mechanics_console_method`, `fuse_mechanics_polyhedron_trigger`
- Hybrid demo: `demo_hybrid_hud` registers 3D lever `InteractableComponent`; polyhedron trigger + console method toggles lever when AI agent enters volume
- TODO: extract remaining GMK `SimComponent` leaves from `third_party/addons/GMK/Engine/source/component/`; physics trigger polyhedron bridge

### `fuse_adventure`

- **Inventory** (`Inventory`, `InventoryComponent`) — 3DAAK `ShapeBase::incInventory` / `decInventory` / `hasInventory` parity
- **Interaction** (`InteractionSystem`, `IInteractable`, `PickupInteractable`, `PuzzleGate`)
- **Interaction queue** (`InteractionQueue`, `QueuedInteraction`) — game-thread enqueue/drain via `MechanicsBridge` (mirrors `fuse::editor::CommandQueue`)
- **Mechanics bridge** (`AdventureInteractableComponent`, `MechanicsBridge`) — dispatches mechanics `InteractionContext` through adventure use/pickup
- Links `fuse_mechanics`; vertical slice: pickup key → use on puzzle gate (Outpost flow)
- **ExamineInteractable** — 3DAAK examine / lore interaction (`InteractionSystem::examine`, `InteractResult::Examined`)
- **DoorInteractable** — locked door opened by key consumption (Outpost door pattern)
- **HudPromptInteractable** — 2D HUD prompt string (`InteractionSystem::promptFor`, `showHudPrompt`)
- **WeaponPickupInteractable** — weapon + ammo grant on pickup (`weapon.cs` ore)
- **ConversationInteractable** — multi-line NPC dialogue (`InteractionSystem::converse`)
- **Content stub** — `Samples/Modules/adventure/outpost_stub.json` (door, weapon, conversation ids)
- Tests: `fuse_adventure_inventory`, `fuse_adventure_interact`, `fuse_adventure_vertical_slice`, `fuse_adventure_interaction_queue`, `fuse_adventure_examine`, `fuse_adventure_door_hud`, `fuse_adventure_weapon_conversation`
- Demo: `demo_adventure_stub` exercises inventory + door unlock
- Hybrid demo: `demo_hybrid_hud` drives 2D HUD prompt text via `showHudPrompt` on trigger enter
- TODO: JSON content loader; remaining 3DAAK interaction scripts

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
ctest --test-dir build-fuse -R "fuse_ai_behavior_tree|fuse_cinematics_tests|fuse_fx_runtime|fuse_mechanics_toggle|fuse_mechanics_console_method|fuse_mechanics_polyhedron_trigger|fuse_adventure_door_hud|fuse_adventure_weapon_conversation|fuse_hybrid_module_gates" --output-on-failure
./build-fuse/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

---

## 7. U5 gates (prestarter §10) — status

| Module | Gate | Status |
|--------|------|--------|
| `fuse_ai` | BT drives 2D + 3D agents in hybrid demo | ✅ `makeMoveTowardDemoTree` + `SceneObject3D` commit in `demo_hybrid_hud` |
| `fuse_cinematics` | 30s timeline moves camera + sprite | ✅ `sample_hybrid_timeline_drive` drives sprite + clear tint (1s demo loop; 30s content pack TODO) |
| `fuse_fx` | AFX on 3D model + 2D sprite | ✅ spark + muzzle sockets ticked in hybrid demo |
| `fuse_mechanics` | One 3D interactable | ✅ lever `InteractableComponent` + trigger zone in hybrid demo |
| `fuse_adventure` | Pick-up / use in 3D + 2D interface | ✅ `HudPromptInteractable` + `showHudPrompt` on trigger enter |

Headless proof: `fuse_hybrid_module_gates_tests` (shared `hybrid_module_gates.cpp` with demo).

### Remaining U5 backlog (post-gate)

| Module | Next ore / work |
|--------|-----------------|
| `fuse_ai` | Per-agent tree selection; full UAISK `.cs` script-host import |
| `fuse_cinematics` | Torque `ShapeBase` VActor attach; load 30s sequence from asset file |
| `fuse_fx` | GPU particle pool backend (`afxParticlePool`); full AFX-Template mission scripts |
| `fuse_mechanics` | Physics trigger polyhedron bridge; remaining GMK `SimComponent` leaves |
| `fuse_adventure` | JSON content loader for `outpost_stub.json`; NPC conversation in hybrid demo |

---

## 8. Related docs

- [work-plan.md](./work-plan.md) — WP-07 / WP-08 status
- [unified-layout.md](./unified-layout.md) — L3 directory map
- [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) — U8 demo mapping
