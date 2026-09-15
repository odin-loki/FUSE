# fuse_mechanics — GMK component and interactable kernel

FUSE mechanics module extracted from addon **ore** (read-only submodules). No addon `Engine/` trees are compiled or merged into FUSE `Engine/`.

## Ore sources

| FUSE API | Ore path | License |
|----------|----------|---------|
| `Component` / `ComponentInterface` | `third_party/addons/GMK/Engine/source/component/simComponent.h`, `componentInterface.h` | MIT |
| `InteractableComponent` | `third_party/addons/GMK/Engine/source/component/simpleComponent.h` | MIT |
| `MechanicsRegistry` | GMK interaction dispatch pattern | MIT |

## Threading

Per [architecture-parallel.md](../../../docs/unification/architecture-parallel.md): scene/component mutation is game-thread only. `InteractionContext::instigatorRoot` is a game-thread pointer — do not publish across job boundaries.

## Adventure bridge

`fuse_adventure` links `fuse_mechanics` and maps `InteractionContext` verbs (`use`, `pickup`) + item ids into `InteractionSystem` dispatch. See `fuse/adventure/mechanics_bridge.hpp`.
