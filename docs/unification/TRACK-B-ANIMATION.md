# Track B — Animation System (B7.1 deepen)

**Status:** B7.1 deepen — `PoseSoA` pose buffers, hierarchy-aware clip evaluate, layered blend weights, closed-form two-bone IK, retarget map stubs  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.1  
**Source narrative:** [P7.md](../sources/P7.md) §7.1

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `Skeleton` / `Bone` | `Animation/include/fuse/animation/skeleton.hpp` | Hierarchy lookup, bind-pose world matrices |
| `Pose` / `PoseSoA` | `Animation/include/fuse/animation/skeleton.hpp` | AoS world matrices; SoA local TRS columns + `allocate` / `resize` / `clear` |
| `blend_pose_soa` | `Animation/src/skeleton.cpp` | Per-bone local TRS lerp into a reusable output buffer |
| `AnimationClip` | `Animation/include/fuse/animation/clip.hpp` | `evaluate` (SoA) composes channel TRS onto bind; `sample` (AoS) wrapper |
| `BlendNode` graph | `Animation/include/fuse/animation/blend_tree.hpp` | `ClipNode`, `BlendNode2`, `BlendSpace1D/2D`, `LayeredBlendNode`, state machine |
| `FABRIKChain` / `TwoBoneIK` | `Animation/include/fuse/animation/ik_solver.hpp` | FABRIK stub; closed-form two-bone IK (AoS + SoA) |
| `RetargetMap` | `Animation/include/fuse/animation/retarget.hpp` | Name-driven bone map + local TRS copy stub |
| `skin_vertices` | `Animation/include/fuse/animation/skinning.hpp` | CPU linear blend skinning (`Cuda` when `FUSE_HAS_CUDA`) |
| `Animator` | `Animation/include/fuse/animation/animator.hpp` | Game-thread facade over state machine + pose cache |

**Not in scope (deferred):** CUDA skinning device path, job-system parallel clip eval, additive blend layers, animation asset import, GPU pose upload.

---

## Design

### PoseSoA buffer lifecycle

`PoseSoA` stores animation-friendly structure-of-arrays columns:

| Column | Role |
|--------|------|
| `local_positions` | Per-bone local translation |
| `local_rotations` | Per-bone local rotation (quaternion) |
| `local_scales` | Per-bone local scale |
| `bone_world_transforms` | Hierarchy-walk output matrices |

Buffer helpers:

- `allocate(capacity)` — reserve column capacity without setting `bone_count`.
- `resize(count)` — grow/shrink active bones; fills identity rotation and unit scale defaults.
- `clear()` — drop all columns and reset `bone_count` to zero for reuse on the next frame.
- `from_bind_pose(skel)` — seed local TRS from skeleton bind matrices, then compute world transforms.
- `compute_world_transforms(skel)` — parent-chain multiply from local TRS columns.

`blend_pose_soa(a, b, weight, out)` resizes `out` to cover both inputs and lerps local TRS per bone. Callers (e.g. `BlendSpace2D`) may reuse the same `out` buffer across accumulation steps.

### Clip evaluation

`AnimationClip::evaluate` starts from bind pose, samples sparse per-bone position/rotation/scale channels, composes each channel transform onto the bone's bind-local matrix, decomposes back into SoA columns, then walks the hierarchy.

### Layered blend weights

`LayeredBlendNode` evaluates base and layer subgraphs, converts both poses to SoA, then for each index in `masked_bones` lerps local TRS by `layer_weight` (clamped to `[0, 1]`). Bones **not** listed in `masked_bones` keep the base pose unchanged. World matrices are recomputed after masking.

### Two-bone IK (closed form)

`TwoBoneIK::solve` uses law-of-cosines shoulder angle plus a pole-vector bend plane to place the mid joint in O(1). Targets beyond `upper + lower` bone length are clamped along the root→target ray. Both `Pose` (world translation writeback) and `PoseSoA` (local position + hierarchy recompute) entry points are provided.

### Retarget map (stub)

`RetargetMap::build_by_name` pairs bones with matching `Bone::name` strings. `apply_pose_soa` seeds the target skeleton bind pose, copies mapped local TRS columns from the source pose, and recomputes world transforms. Unmapped target bones remain at bind pose. Rotation/scale offsets and animation-space retargeting are deferred.

---

## Build

`fuse_animation` builds when `FUSE_BUILD_CORE=ON` and `FUSE_BUILD_CORE_TESTS=ON` (default under the FUSE umbrella).

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build --target fuse_animation_tests
ctest --test-dir build --output-on-failure -R fuse_animation_runtime
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_CORE_TESTS=OFF` | `fuse_animation_tests` omitted |
| `FUSE_HAS_CUDA` defined | `skinning_backend()` reports `Cuda`; CPU path still tested |

---

## Tests

`Source/FUSE/Animation/tests/test_animation.cpp` (`fuse_animation_runtime`):

| Test | Gate |
|------|------|
| `testSkeletonHierarchy` | Bone lookup and bind-pose propagation |
| `testPoseSoAHierarchy` | SoA local edit + hierarchy recompute |
| `testPoseBufferClearReuse` | `clear` / `resize` / re-populate cycle |
| `testBlendPoseSoAReuse` | `blend_pose_soa` output buffer reuse at multiple weights |
| `testPoseSoAToPoseRoundtrip` | `to_pose` preserves world transforms |
| `testPoseSoAResizeDefaults` | `resize` grow/shrink and default TRS seeding |
| `testBlendPoseSoARotationScale` | `blend_pose_soa` rotation and scale lerp |
| `testTwoBoneIKReachable` | Closed-form IK reaches target with preserved bone lengths |
| `testTwoBoneIKUnreachableClamps` | Out-of-reach target clamps to max limb extension |
| `testTwoBoneIKSoA` | SoA IK path writes local positions and recomputes hierarchy |
| `testRetargetMapBuildByName` | Name pairing and validity |
| `testRetargetApplyPoseSoA` | Mapped local TRS copy; unmapped bones stay at bind |
| `testClipSampling` | AoS `sample` position channel |
| `testClipEvaluateLocalChannels` | SoA `evaluate` composes position + rotation |
| `testBlendNodeInterpolation` | `BlendNode2` world translation lerp |
| `testBlendSpace1D` | Speed-axis bracketing blend |
| `testLayeredBlendMask` | Masked bone at 0.5 layer weight |
| `testLayeredBlendWeights` | Layer weight 0 / 1 / fractional / clamped |
| `testLayeredBlendUnmaskedBone` | Parent stays base when only child is masked |
| `testStateMachineTransition` | Condition-driven state change |
| `testFabrikConverges` | FABRIK end-effector error bound |
| `testSkinningCpuPath` | CPU skinning applies bone transform |
| `testAnimatorTick` | `Animator::tick` advances playback state |

---

## Gates (B7.1 deepen)

- [x] `PoseSoA` local TRS columns with `allocate` / `resize` / `clear`
- [x] `blend_pose_soa` helper for SoA accumulation
- [x] `AnimationClip::evaluate` hierarchy-aware SoA path
- [x] `BlendSpace1D`, `BlendSpace2D`, `LayeredBlendNode` stubs
- [x] Layered blend weight sweep and unmasked-bone isolation tests
- [x] Pose buffer clear/reuse tests
- [x] Closed-form `TwoBoneIK` (AoS + SoA) with reach/clamp tests
- [x] `RetargetMap` name pairing + `apply_pose_soa` stub
- [x] Expanded `PoseSoA` roundtrip, resize, and rotation/scale blend tests
- [x] `fuse_animation_runtime` CTest target green
- [ ] CUDA skinning device kernel — deferred
- [ ] Job-system parallel evaluate — deferred

---

## References

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.1
- [P7.md](../sources/P7.md) §7.1
- [Source/FUSE/Animation/README.md](../../Source/FUSE/Animation/README.md)
- [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md) — B7.10 integration checklist
