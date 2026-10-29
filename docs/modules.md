# Modules

Feature modules sit on layer L3. Enable them with `FUSE_BUILD_MODULES=ON` (default) and toggle per project in `project.json` — see [projects.md](projects.md).

All of these are first-class FUSE APIs. They do not compile third-party engine trees into the product.

## `fuse_ai`

Behavior trees and an agent runtime.

- `NodeRegistry` — factories keyed by type id (`bb.sequence`, `bb.selector`, `bb.parallel`, decorators, leaf actions)
- `loadTreeFromSpecs` / `loadTreeFromText` — editor and test loaders
- `BehaviorRuntime` — snapshot eval on jobs, blackboard commit on the game thread
- Spatial helpers: `bb.condition.allies_in_radius`, `bb.action.nearest_ally`
- Faction leaves: `bb.condition.hostiles_in_radius`, `bb.action.nearest_hostile` (3D, masks from `AgentBinding::friendlyMask` / `hostileMask` against `factionMask`)
- Navmesh reachability (`fuse/ai/nav_query.hpp`): `find_nearest_reachable_ally` / `count_reachable_allies` rank same-team allies by walking distance on a `fuse_navigation` grid (one flood fill per query), skipping allies behind walls or off the grid

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
| `fuse_asset` | Runtime asset core: cooked readers (FMSH, `.fusetex`, `.fusemat`), `AssetRegistry` (AssetId ↔ VFS path ↔ handle, refcount, async load/unload), job decode, render-upload queue. Links `fuse_core` only; `Tools/FUSE/Cook` links it for reading |
| `fuse_navigation` | Uniform 4-connected grid navmesh with deterministic A* — see below |
| `fuse_compute` | Single-source compute kernels: CPU backends everywhere, CUDA trampolines when the toolkit is found — [compute-kernels.md](compute-kernels.md) |
| `fuse_ssfx` | Screen-space effect references (HBAO, SSR, SSGI) as single-source kernels, shared by `fuse_rhi` and `fuse_compute`; depends on `fuse_core` only |
| `fuse_compat_ts*`, `fuse_compat_sim_object` | Quarantined dialect script VMs and the SimObject id ↔ `EntityID` table. Never linked into `fuse_core`, `fuse_runtime_smoke`, `fuse_editor_api`, or Qt targets; `FUSE_BUILD_COMPAT_TS` (forced OFF in shipping) |

Engine-level VFX is distinct from `fuse_fx`: particles vs. authored spell/effect graphs.

## `fuse_navigation`

`fuse::navigation::GridNavmesh` (`Source/FUSE/Navigation/include/fuse/navigation/grid_navmesh.hpp`) is a uniform 4-connected grid.

- `build(width, height, cell_size, is_blocked)` takes a predicate `(x, y) -> bool`; an overload takes a list of blocked `Cell`s (out-of-grid entries ignored). Zero dimensions, a non-finite or non-positive `cell_size`, or a cell count that does not fit a 32-bit index fail and clear the mesh.
- `find_path(start, goal)` is A* with unit step cost and a Manhattan heuristic. Ties break toward smaller x, then smaller y, and a parent is replaced only on a strictly smaller g, so the waypoint list is deterministic.
- Waypoints are cell centres in world xy, start through goal. Out-of-grid endpoints, a blocked goal, a blocked start (unless it equals the goal), or no route return an empty path.
- `step_field(start, out)` floods breadth-first from `start` and writes the step count to every cell (`kUnreachable` for blocked or walled-off cells); `steps_to(field, cell)` reads it. Steps equal the `find_path` waypoint count minus one, so walking distance is `steps * cell_size()`. A blocked or off-grid start fails and clears `out`.
- `cell_at(x, y, out)` maps a world point to its cell (false when off-grid, non-finite, or unbuilt); `built()`, `width()`, `height()`, `cell_size()` expose the grid.
- `build` allocates the grid and search scratch; a query allocates only its result (plus the transient open set; `step_field` reuses the search scratch as its queue). Queries share scratch, so do not call `find_path` or `step_field` concurrently on one mesh.

Helpers: `path_length` (polyline length, non-finite segments ignored), `path_cost_ok` (finite, `[0, 10000]`), `nav_cell_ok` (cell size in metres, finite, `[0.1, 4]`). Tests: `fuse_navigation_tests`.
