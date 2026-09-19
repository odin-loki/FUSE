# WP-05 — Scene hierarchy deepen

**Status:** ✅ Done — greenfield hierarchy, snapshot SoA, legacy adapter round-trip, handle-only worker reads  
**Related:** [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md), [architecture-parallel.md](./architecture-parallel.md) §5

## Greenfield types

| Type | Location | Notes |
|------|----------|-------|
| `fuse::Object` | `Core/include/fuse/object.hpp` | Parent/child graph, `reparent`, handle slot, `legacyId` for adapter round-trip |
| `fuse::SceneObject2D` | `World2D/…/scene_object_2d.hpp` | xy, layer, sort key; `localTransform` / `worldTransform` stubs |
| `fuse::SceneObject3D` | `World3D/…/scene_object_3d.hpp` | Extends 2D; `localTransform3D` / `worldTransform3D` stubs |

## Threading contract

- Scene graph mutation (`addChild`, `reparent`, `setPosition`, legacy import/export) is **game-thread only** (`platform::isMainThread()`).
- Workers read **`SceneSnapshot2D/3D`** and **`SceneTransformSoA*`** built on the game thread via `fillSnapshotSoA` — no raw `Object*` on job threads.

## Legacy adapters

| Adapter | Header | Behaviour |
|---------|--------|-----------|
| T2D | `fuse/legacy/t2d/scene_adapter.hpp` | `LegacySceneObjectStub` ↔ `SceneObject2D` field copy (id, name, xy, layer); game-thread guard; no Engine `SceneObject*` |
| T3D | `fuse/legacy/t3d/scene_adapter.hpp` | Same pattern for `SceneObject3D` (adds z) |

Full Engine `SimObject`/`SceneObject` round-trip waits on U2 Engine init unblock; stub records prove the boundary shape and preserve legacy ids for strangler mapping.

## Tests

`fuse_scene_hierarchy` (`World3D/tests/test_scene_hierarchy.cpp`): inheritance, reparent, local/world transform stubs, snapshot + SoA fill, handle-only worker read, legacy adapter round-trip (T2D + T3D), off-main-thread adapter rejection.

`fuse_core_services` covers base `Object::reparent`.

## Next (post WP-05)

- Handle publish through `HandleTable` on scene insert/destroy
- Quat + full TRS world matrices (replace additive xy/z stub)
- Wire `World2D/3D::buildSnapshot` to `fillSnapshotSoA` when handle table lands
