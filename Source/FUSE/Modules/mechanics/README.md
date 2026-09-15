# fuse_mechanics — GMK component and interactable kernel

FUSE mechanics module extracted from addon **ore** (read-only submodules). No addon `Engine/` trees are compiled or merged into FUSE `Engine/`.

## Ore sources

| FUSE API | Ore path | License |
|----------|----------|---------|
| `Component` / `ComponentInterface` | `third_party/addons/GMK/Engine/source/component/simComponent.h`, `componentInterface.h` | MIT |
| `InteractableComponent` | `third_party/addons/GMK/Engine/source/component/simpleComponent.h` | MIT |
| `InteractAction` / `UseActionStub` / `PickupActionStub` / `ExamineActionStub` | GMK leaf action components | MIT |
| `MechanicsRegistry` | GMK interaction dispatch pattern | MIT |

## Threading

Per [architecture-parallel.md](../../../docs/unification/architecture-parallel.md): scene/component mutation is game-thread only. `InteractionContext::instigatorRoot` is a game-thread pointer — do not publish across job boundaries.

## Action stubs

`InteractActionKind` maps built-in verbs (`use`, `pickup`, `examine`) to `InteractionContext`. Action stub components record executions for tests and future GMK leaf extraction — they live entirely in `fuse_mechanics` with no adventure dependency.

## Adventure bridge

`fuse_adventure` links `fuse_mechanics` and maps `InteractionContext` verbs + item ids into `InteractionSystem` dispatch via `MechanicsBridge`. `InteractionQueue` defers input/UI posts and drains on the game thread. See `fuse/adventure/mechanics_bridge.hpp` and `fuse/adventure/interaction_queue.hpp`.
