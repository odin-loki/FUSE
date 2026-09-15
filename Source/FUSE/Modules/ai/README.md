# fuse_ai — behavior trees and agent runtime

FUSE AI module extracted from addon **ore** (read-only submodules). No addon `Engine/` trees are compiled or merged into FUSE `Engine/`.

## Ore sources

| FUSE API | Ore path | License |
|----------|----------|---------|
| `NodeRegistry` / `bb.*` type ids | `third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/` | MIT (Guy Allard) |
| `bb.sequence`, `bb.selector` | `.../BadBehavior/composite/Sequence.h`, `Selector.h` | MIT |
| `bb.inverter`, `bb.loop`, `bb.succeed_always`, `bb.root` | `.../BadBehavior/decorator/` | MIT |
| `bb.action.set_flag` (demo leaf) | `.../BadBehavior/leaf/ScriptedBehavior.h` (pattern) | MIT |
| `gb.action.move_toward` | `third_party/addons/GuideBot/guideBotT3D/engine/lib/guideBot/include/guideBot/actionMove.h` | Custom — see `guidebot_license_agreement.txt` |
| UAISK template hooks (scripts only) | `third_party/addons/UAISK/.../UAISK/*.cs` | MIT |

## Load paths

1. **`NodeRegistry`** — register factories by string type id (BadBehaviour `DECLARE_CONOBJECT` analogue).
2. **`loadTreeFromSpecs`** — build flat trees from `NodeLoadSpec` vectors (editor/asset pipeline).
3. **`loadTreeFromText`** — compact text format for tests and future `.bt` assets.

UAISK script modules are mapped in `fuse/ai/uaisk_template_hooks.hpp` and documented under `Samples/Modules/ai/uaisk-templates/` — **no C++ compile** from UAISK.

## Threading

`BehaviorRuntime` follows [architecture-parallel.md](../../../docs/unification/architecture-parallel.md): snapshots + job eval + game-thread blackboard commit.
