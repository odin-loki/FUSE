# Track B — ECS Core (B3.1–B3.2)

**Status:** B3.1 archetype registry sketch + B3.2 core component types landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B3.1–B3.2  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4 (systems deferred to B3.3)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `EntityID` | `Source/FUSE/ECS/include/fuse/ecs/entity.hpp` | Index + generation handle; stale detection O(1) |
| `Registry` | `Source/FUSE/ECS/include/fuse/ecs/registry.hpp` | Create/destroy, add/remove/get/has, `each` bulk iteration |
| `Archetype` / `ComponentColumn` | `Source/FUSE/ECS/include/fuse/ecs/archetype.hpp` | SoA columns keyed by `std::type_index`; archetype migration on add/remove |
| `IsComponentV` trait | `Source/FUSE/ECS/include/fuse/ecs/component.hpp` | Plain data + `component_name` string (C++17) |
| Math types (`vec3`, `quat`, `mat4`) | `Source/FUSE/ECS/include/fuse/ecs/math/vec.hpp` | Minimal POD until shared `fuse/math` lands in Core |
| Core components | `Source/FUSE/ECS/include/fuse/ecs/components/` | Transform, Mesh, SDFObject, RigidBody, Camera, lights, tags |

**Not in scope:** TransformSystem / physics sync (B3.3+), CUDA-managed columns, `each_parallel`, Renderer/Physics/Modules integration, BVH/SVO (B3.4–B3.5).

---

## Design (B3.1 sketch)

### Handle-based identity

```cpp
struct EntityID {
  u32 index;
  u32 generation;
  bool valid() const { return generation != 0; }
};
```

`Registry::alive()` compares generation against the slot record. Destroyed indices are recycled; generation increments on reuse.

### Archetype storage

Entities with the **same component signature** share one `Archetype`:

- Each component type is a `ComponentColumn` (byte vector + `element_size`).
- Adding/removing a component **migrates** the entity to a new archetype (swap-remove in source).
- `Registry::each<Transform, RigidBody>(fn)` scans archetypes whose signature is a superset of the requested types.

Empty archetype (index 0, hash 0) holds newly created entities before their first component is added.

### CUDA / parallel iteration

Managed-memory columns and `each_parallel` are **deferred** to B3.3 systems work. Current columns use `std::vector<std::byte>` on the CPU.

---

## B3.2 — Core component types

All components are plain data (`IsComponentV` + trivially destructible). No virtual methods.

| Component | Header | Notes |
|-----------|--------|-------|
| `Transform` | `components/transform.hpp` | Position/rotation/scale/parent + derived matrices (filled by B3.3 TransformSystem) |
| `Mesh` | `components/mesh.hpp` | ECS-local `fuse::Handle` aliases — no `fuse_rhi` dependency |
| `SDFObject` | `components/sdf_object.hpp` | Primitive enum + params; GRIA α placeholder constant |
| `RigidBody` | `components/rigidbody.hpp` | SoA-friendly dynamics fields (solver in B4) |
| `Camera` | `components/camera.hpp` | Projection params + derived matrices/frustum |
| `DirectionalLight` / `PointLight` / `SpotLight` | `components/light.hpp` | Light payloads for future culling |
| `TagStatic` / `TagPlayer` / `TagDestroy` | `components/tags.hpp` | Zero-size marker components |

---

## Build

`fuse_ecs` builds with the umbrella by default (no extra CMake flag).

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_VULKAN=OFF

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_ecs
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_VULKAN=OFF` | ECS unaffected — no renderer dependency |
| `FUSE_BUILD_CORE_TESTS=ON` | `fuse_ecs_registry` + `fuse_ecs_components` CTest targets |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_ecs_registry` | Create/destroy, stale handles, add/get/remove, archetype migration, `each` |
| `fuse_ecs_components` | Component names, defaults, registry storage for lights/tags |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_ecs
```

---

## Gates (B3.1–B3.2)

- [x] **B3.1** `EntityID`, `Registry`, archetype SoA columns on FUSE APIs
- [x] **B3.2** Core component POD types (Transform, Mesh, SDF, RigidBody, Camera, lights, tags)
- [x] CTest targets green with `FUSE_BUILD_VULKAN=OFF`
- [ ] ASan/UBSan smoke on ECS tests (umbrella ASan job covers runtime smoke; ECS-specific ASan optional follow-up)
- [x] No owning raw pointers in public ECS APIs

---

## Next

- [ ] B3.3 — `TransformSystem`, `each_parallel` via job scheduler
- [ ] Bridge `Mesh` handles → `renderer::BufferHandle` when scene submit lands
- [ ] Managed/pinned column allocators for CUDA physics path (B4)
- [ ] SimObject ↔ `EntityID` compat shim (dual-run during P4)

---

## Related docs

- [work-plan.md](./work-plan.md) — Track B ECS entry (future WP)
- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B3
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — parallel Track B renderer lane
