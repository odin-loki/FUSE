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

- [x] Handle publish through `HandleTable` on scene insert/destroy (UNI-WP05-1): `SceneHandleTable` (`World2D/include/fuse/world2d/scene_handle_table.hpp`, over Core `HandleTable`) publishes `Handle<Object>` on `World2D::addSprite` / `World3D::addObject`; `removeSprite` / `destroySprite` / `removeObject` / `clearDynamicObjects` and `~SceneObject2D` (node destroyed while published) remove the slot, so stale handles are rejected (generation bump, slot reuse does not revive them); the worlds keep handles, not raw pointers, and prune destroyed nodes on tick — `fuse_scene_transform_gates`
- [x] Quat + full TRS world matrices (replace additive xy/z stub) (UNI-WP05-1): `SceneObject2D` (base of `SceneObject3D`) stores translation + unit quaternion + per-axis scale (yaw/pitch/roll setters convert, R = Rz(yaw) Rx(pitch) Ry(roll)); world = parentWorld * T * R * S along scene ancestors, cached per node with a version stamp (a change, a parent change or a plain `Object::reparent` invalidates the subtree lazily); `reparentKeepWorld`, `setWorldPose`; `transform_stubs.hpp` replaced by `scene_transform.hpp`. Gate `fuse_scene_transform_gates`: rotated + non-uniformly scaled 3-level chain vs a double-precision reference product (max error ~1e-6), cache invalidation, reparent keeps world pose, handle staleness, snapshot matrices bitwise equal to the cached ones
- [x] Wire `World2D/3D::buildSnapshot` to `fillSnapshotSoA` when handle table lands: the snapshot SoA now carries `worldMatrix` (full world matrix) per row next to the handle, filled top-down in one pass from the caches
