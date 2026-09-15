# WP-05 — Scene hierarchy deepen

**Status:** 🚧 In progress (greenfield types + stubs)  
**Related:** [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md), [architecture-parallel.md](./architecture-parallel.md) §5

## Greenfield types

| Type | Location | Notes |
|------|----------|-------|
| `fuse::Object` | `Core/include/fuse/object.hpp` | Parent/child graph, `reparent`, handle slot |
| `fuse::SceneObject2D` | `World2D/…/scene_object_2d.hpp` | xy, layer, sort key; `localTransform` / `worldTransform` stubs |
| `fuse::SceneObject3D` | `World3D/…/scene_object_3d.hpp` | Extends 2D; `localTransform3D` / `worldTransform3D` stubs |

## Threading contract

- Scene graph mutation (`addChild`, `reparent`, `setPosition`) is **game-thread only**.
- Workers read **`SceneSnapshot2D/3D`** and **`SceneTransformSoA*`** built on the game thread via `fillSnapshotSoA` — no raw `Object*` on job threads.

## Legacy adapters (stub-only)

| Adapter | Header | Behaviour |
|---------|--------|-----------|
| T2D | `fuse/legacy/t2d/scene_adapter.hpp` | `LegacySceneObjectStub` ↔ `SceneObject2D` field copy; no Engine `SceneObject*` |
| T3D | `fuse/legacy/t3d/scene_adapter.hpp` | Same pattern for `SceneObject3D` |

Full legacy round-trip waits on U2 Engine init unblock; stubs prove the boundary shape.

## Tests

`fuse_scene_hierarchy` (`World3D/tests/test_scene_hierarchy.cpp`): inheritance, reparent, local/world transform stubs, snapshot + SoA fill, legacy adapter round-trip.

`fuse_core_services` covers base `Object::reparent`.

## Next

- Handle publish through `HandleTable` on scene insert/destroy
- Quat + full TRS world matrices (replace additive xy/z stub)
- Wire `World2D/3D::buildSnapshot` to `fillSnapshotSoA` when handle table lands
