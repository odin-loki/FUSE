# Modules

Feature modules sit on layer L3. Enable them with `FUSE_BUILD_MODULES=ON` (default) and toggle per project in `project.json` — see [projects.md](projects.md).

All of these are first-class FUSE APIs. They do not compile third-party engine trees into the product.

## `fuse_ai`

Behavior trees and an agent runtime.

- `NodeRegistry` — factories keyed by type id (`bb.sequence`, `bb.selector`, `bb.parallel`, decorators, leaf actions)
- `loadTreeFromSpecs` / `loadTreeFromText` — editor and test loaders
- `BehaviorRuntime` — snapshot eval on jobs, blackboard commit on the game thread
- Spatial helpers: `bb.condition.allies_in_radius`, `bb.action.nearest_ally`

Headers: `Source/FUSE/Modules/ai/include/fuse/ai/`. Tests: `fuse_ai_*`. Sample: `Samples/unification/demo_ai_bt`.

## `fuse_fx`

Spell / effect composer: descriptors, timelines, graphs, casts, residuals.

- `EffectDescriptor`, `EffectTimeline`, `EffectGraph`
- `SpellDescriptor` + `CastPipeline` (casting → launch → delivery → impact → linger)
- `FxComposer::tick()` advances timelines, graphs, casts, and residual lifetimes
- `bind::ParameterBinder` resolves caster / target handles
- `FxSocket` attaches effects to hosts

Registration, cast begin, and socket attach are game-thread only. Sample: `Samples/unification/demo_fx`.

## `fuse_cinematics`

Timeline director: playhead, tracks, groups, cue queue.

```cpp
fuse::cinematics::Timeline timeline;
timeline.playhead().set_duration_ms(30'000);
auto& group = timeline.add_group("Director");
auto& camera = group.add_camera_track("MainCam");
camera.add_keyframe(/* position, look-at, FOV */);
timeline.play();
timeline.advance(100); // milliseconds
```

Track types include camera, sprite, property, audio, and event/cue lanes. `scrub_to` enqueues crossed cues for game-thread drain. Sample: `Samples/unification/demo_timeline`.

## `fuse_mechanics`

Components, interactables, and verb stubs (`use`, `pickup`, `examine`).

- `Component` / `ComponentInterface`
- `InteractableComponent` + `MechanicsRegistry`
- Action stubs record executions for tests and future leaf extraction

Scene and component mutation is game-thread only. `InteractionContext::instigatorRoot` must not cross job boundaries.

## `fuse_adventure`

Adventure scaffolding on top of mechanics: inventory, interactables, puzzle gates.

- `InteractionSystem` via `MechanicsBridge`
- `InteractionQueue` defers input/UI posts and drains on the game thread
- Item ids, pickup interactables, inventory components

Sample: `Samples/unification/demo_adventure_stub`.

## Related engine libraries (not L3 toggles)

These build with core rather than the module flags:

| Library | Role |
|---------|------|
| `fuse_vfx` | CPU particle SoA, emitters, effect instances (GPU kernels deferred) |
| `fuse_terrain` | Heightfield, LOD chunk grid, raycast stubs |
| `fuse_world_partition` | Streaming volumes, residency, budgets |
| `fuse_audio` | Spatial audio engine |
| `fuse_script` | Script host — [scripting.md](scripting.md) |
| `fuse_ecs` | Entity-component world |
| `fuse_physics` | Physics facade |
| `fuse_net` | Session / net stubs |
| `fuse_animation` | Animation system |

Engine-level VFX is distinct from `fuse_fx`: particles vs. authored spell/effect graphs.
