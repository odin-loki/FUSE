# fuse_ai — behavior trees and agent runtime

FUSE AI module extracted from addon **ore** (read-only submodules). No addon `Engine/` trees are compiled or merged into FUSE `Engine/`.

## Ore sources

| FUSE API | Ore path | License |
|----------|----------|---------|
| `NodeRegistry` / `bb.*` type ids | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/` | MIT (Guy Allard) |
| `bb.sequence`, `bb.selector`, `bb.parallel` | `.../BadBehavior/composite/Sequence.h`, `Selector.h` (parallel: FUSE aggregate) | MIT |
| `bb.inverter`, `bb.loop`, `bb.succeed_always`, `bb.root` | `.../BadBehavior/decorator/` | MIT |
| `bb.action.set_flag`, `bb.action.wait`, `bb.action.blackboard_set`, `bb.condition.blackboard_get`, `bb.action.distance` | `.../BadBehavior/leaf/ScriptedBehavior.h` (pattern) | MIT |
| `bb.condition.allies_in_radius`, `bb.action.nearest_ally` | FUSE `spatial_query.hpp` (blackboard ally lookup stubs) | — |
| `gb.action.move_toward` | `third_party/addons/GuideBot/guideBotT3D/engine/lib/guideBot/include/guideBot/actionMove.h` | Custom — see `guidebot_license_agreement.txt` |
| UAISK template hooks (scripts only) | `third_party/addons/UAISK/.../UAISK/*.cs` | MIT |

## Load paths

1. **`NodeRegistry`** — register factories by string type id (BadBehaviour `DECLARE_CONOBJECT` analogue).
2. **`loadTreeFromSpecs`** — build flat trees from `NodeLoadSpec` vectors (editor/asset pipeline).
3. **`loadTreeFromText`** — compact text format for tests and future `.bt` assets.

UAISK script modules are mapped in `fuse/ai/uaisk_template_hooks.hpp` and documented under `Samples/Modules/ai/uaisk-templates/` — **no C++ compile** from UAISK.

## Threading

`BehaviorRuntime` follows [architecture-parallel.md](../../../docs/unification/architecture-parallel.md): snapshots + job eval + game-thread blackboard commit.

## Spatial query (blackboard)

`fuse/ai/spatial_query.hpp` provides CPU stubs for same-team ally radius filter and nearest-ally lookup. `BehaviorRuntime` publishes `AllyCandidate` snapshots each eval; BT leaves `bb.condition.allies_in_radius` and `bb.action.nearest_ally` read them through `BehaviorEvalContext::allies`. See [TRACK-B-AI.md](../../../docs/unification/TRACK-B-AI.md) § Blackboard spatial query.
