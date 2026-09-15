# Track B — Animation System (B7.1 deepen)

**Status:** B7.1 deepen — `PoseSoA` pose buffers + `accumulate_weighted_pose_soa` / `copy_pose_soa_local` / `finalize_weighted_pose_soa` / `pose_soa_matches_bind` helpers, hierarchy-aware clip evaluate, blend-tree `evaluate_soa`, 1D/2D parameter sampling (empty-tree bind-pose fallback), additive/layered empty-node bind fallback, state enter/exit + crossfade (initial `on_enter`, zero-duration snap, self-transition guard, bidirectional transition edge queries + blend-duration lookup + active/pending state names), closed-form two-bone IK (in-place solve + `has_valid_chain` parent-chain validation + `max_reach`), retarget map stubs (`build_identity`, `find_source_bone` / `find_target_bone`, `translation_scale`, empty-skeleton guards)  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.1  
**Source narrative:** [P7.md](../sources/P7.md) §7.1

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `Skeleton` / `Bone` | `Animation/include/fuse/animation/skeleton.hpp` | Hierarchy lookup, bind-pose world matrices |
| `Pose` / `PoseSoA` | `Animation/include/fuse/animation/skeleton.hpp` | AoS world matrices; SoA local TRS columns + `allocate` / `resize` / `clear` |
| `blend_pose_soa` | `Animation/src/skeleton.cpp` | Per-bone local TRS lerp into a reusable output buffer |
| `accumulate_weighted_pose_soa` | `Animation/src/skeleton.cpp` | Incremental weighted pose accumulation for multi-entry blend spaces |
| `finalize_weighted_pose_soa` | `Animation/src/skeleton.cpp` | Bind-pose fallback or hierarchy recompute after weighted accumulation |
| `copy_pose_soa_local` | `Animation/src/skeleton.cpp` | Copy local TRS columns between `PoseSoA` buffers |
| `pose_soa_matches_bind` | `Animation/src/skeleton.cpp` | Compare local TRS columns against skeleton bind pose |
| `AnimationClip` | `Animation/include/fuse/animation/clip.hpp` | `evaluate` (SoA) composes channel TRS onto bind; `sample` (AoS) wrapper |
| `BlendNode` graph | `Animation/include/fuse/animation/blend_tree.hpp` | `ClipNode`, `BlendNode2`, `BlendSpace1D/2D`, `LayeredBlendNode`, `AdditiveBlendNode`, state machine |
| `sample_blend_space_1d/2d` | `Animation/src/blend_tree.cpp` | Parameter-space weight sampling for 1D bracketing and 2D inverse-distance weights |
| `add_pose_soa` | `Animation/src/skeleton.cpp` | Masked additive local TRS delta relative to bind pose |
| `FABRIKChain` / `TwoBoneIK` | `Animation/include/fuse/animation/ik_solver.hpp` | FABRIK stub; closed-form two-bone IK (in-place AoS + SoA, parent-chain `has_valid_chain`, `max_reach`) |
| `RetargetMap` | `Animation/include/fuse/animation/retarget.hpp` | Name-driven bone map, `build_identity`, `find_source_bone` / `find_target_bone`, `translation_scale`, local TRS copy stub |
| `skin_vertices` | `Animation/include/fuse/animation/skinning.hpp` | CPU linear blend skinning (`Cuda` when `FUSE_HAS_CUDA`) |
| `Animator` | `Animation/include/fuse/animation/animator.hpp` | Game-thread facade over state machine + pose cache |

**Not in scope (deferred):** CUDA skinning device path, job-system parallel clip eval, full quaternion additive rotation, animation asset import, GPU pose upload.

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

### Blend tree PoseSoA evaluation

All `BlendNode` types expose `evaluate_soa(dt, skel, out)` alongside the legacy `evaluate` AoS path. `ClipNode` calls `AnimationClip::evaluate` directly; blend/layer/state nodes accumulate in SoA space via `blend_pose_soa` / `add_pose_soa`, then call `compute_world_transforms`.

`sample_blend_space_1d` returns bracket indices and interpolation alpha for a runtime parameter. `sample_blend_space_2d` returns normalized inverse-distance weights per entry.

### Layered and additive blend weights

`LayeredBlendNode` evaluates base and layer subgraphs in SoA, then for each index in `masked_bones` lerps local TRS by `layer_weight` (clamped to `[0, 1]`). Bones **not** listed in `masked_bones` keep the base pose unchanged.

`AdditiveBlendNode` adds a masked local TRS delta relative to bind pose: `out = base + weight * (layer - bind)` on position/scale; rotation uses a bind-relative lerp stub. `add_pose_soa` implements the masked accumulation helper.

### State machine crossfade

`AnimStateMachine` supports per-state `on_enter` / `on_exit` callbacks. The initial state's `on_enter` fires on the first `evaluate` call. Transitions capture the outgoing pose, crossfade toward the target state over `blend_duration`, and expose `crossfade_alpha()` in `[0, 1]` (0 when idle). A zero `blend_duration` snaps instantly to the target pose. Self-transitions are rejected at registration; transitions are not re-evaluated mid-crossfade. Weight is clamped; `active_state` advances when alpha reaches 1. `reset()` returns to the first state without callbacks. `find_state_index`, `outgoing_transition_count`, `incoming_transition_count`, `has_transition`, and `transition_blend_duration` expose registered state/edge queries for tests and tooling. `state_name`, `active_state_name`, and `pending_state_name` report human-readable state labels during evaluation.

### Two-bone IK (closed form)

`TwoBoneIK::solve` uses law-of-cosines shoulder angle plus a pole-vector bend plane to place the mid joint in O(1). Targets beyond `upper + lower` bone length are clamped along the root→target ray. `max_reach` reports the same clamp limit from the current pose segment lengths minus `reach_epsilon`. When the pole vector is parallel to the root→target axis, a secondary fallback axis is chosen so the bend plane remains stable. `has_valid_chain` rejects empty skeletons, out-of-range or duplicate bone indices, and non root→mid→end parent chains. Solvers operate in-place on the current pose (only seeding bind pose when the output buffer is empty or mismatched). Both `Pose` (world translation writeback) and `PoseSoA` (local position writes + hierarchy recompute) entry points are provided.

### Retarget map (stub)

`RetargetMap::build_by_name` pairs bones with matching `Bone::name` strings. `build_identity` maps each bone index to itself for same-skeleton reuse. `find_source_bone` / `find_target_bone` and `mapped_bone_count` support bidirectional lookup without scanning `bone_map`. `apply_pose_soa` seeds the target skeleton bind pose, copies mapped local TRS columns from the source pose (scaled by per-entry `translation_scale`), and recomputes world transforms. `apply_pose` (AoS stub) copies mapped world transforms directly; unmapped target bones remain at bind pose. Empty skeletons or empty maps clear the output pose and return early. Rotation/scale offsets and animation-space retargeting are deferred.

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
| `testTwoBoneIKPoleBend` | Opposite pole vectors flip mid-joint bend side |
| `testTwoBoneIKUnreachableClamps` | Out-of-reach target clamps to max limb extension |
| `testTwoBoneIKSoA` | SoA IK path writes local positions and recomputes hierarchy |
| `testTwoBoneIKEmptySkeleton` | Empty skeleton rejected by IK; FABRIK no-op |
| `testTwoBoneIKInPlace` | In-place IK preserves root offset while reaching target |
| `testTwoBoneIKChainOrder` | Duplicate, out-of-range, and non-chain bone indices rejected |
| `testTwoBoneIKMaxReach` | `max_reach` matches unreachable clamp distance |
| `testRetargetMapBuildByName` | Name pairing and validity |
| `testRetargetApplyPoseSoA` | Mapped local TRS copy; unmapped bones stay at bind |
| `testRetargetApplyPose` | AoS world-transform copy stub for mapped bones |
| `testRetargetEmptySkeleton` | Empty skeleton maps invalid; apply clears output |
| `testRetargetIdentity` | `build_identity` round-trips local/world TRS unchanged |
| `testRetargetFindTargetBone` | `find_target_bone` reverse lookup |
| `testRetargetTranslationScale` | Per-entry `translation_scale` applied in `apply_pose_soa` |
| `testClipSampling` | AoS `sample` position channel |
| `testClipEvaluateLocalChannels` | SoA `evaluate` composes position + rotation |
| `testBlendNodeInterpolation` | `BlendNode2` world translation lerp |
| `testBlendSpace1D` | Speed-axis bracketing blend |
| `testLayeredBlendMask` | Masked bone at 0.5 layer weight |
| `testLayeredBlendWeights` | Layer weight 0 / 1 / fractional / clamped |
| `testLayeredBlendUnmaskedBone` | Parent stays base when only child is masked |
| `testBlendSpace1DParameterSample` | 1D parameter bracket/alpha sampling |
| `testBlendSpace2D` | 2D blend-space `evaluate_soa` |
| `testBlendSpace2DParameterSample` | 2D normalized weight sampling |
| `testAdditiveBlendLayer` | Masked additive local TRS delta |
| `testBlendTreeEvaluateSoA` | `BlendNode2::evaluate_soa` local lerp |
| `testStateMachineEnterExit` | `on_enter` / `on_exit` callbacks |
| `testStateMachineCrossfadeClamp` | Crossfade alpha mid-transition and reset |
| `testStateMachineTransition` | Condition-driven state change |
| `testEmptyBlendSpace1D` / `testEmptyBlendSpace2D` | Empty blend spaces return bind pose |
| `testEmptyStateMachine` | Empty state machine returns bind pose |
| `testBlendNode2WeightClamp` | `BlendNode2` clamps blend param to `[0, 1]` |
| `testBlendNode2NullChildren` | Null child nodes evaluate to bind pose |
| `testAccumulateWeightedPoseSoA` | Weighted pose accumulation helper |
| `testAccumulateWeightedPoseSoASkipsZeroWeight` | Zero-weight accumulation is a no-op |
| `testFinalizeWeightedPoseSoAEmpty` / `testFinalizeWeightedPoseSoARecompute` | Weighted finalize bind fallback and hierarchy recompute |
| `testCopyPoseSoALocal` | Local TRS column copy helper |
| `testEmptyClipNode` / `testEmptyBlendSpace1DSoA` | Null clip and empty 1D SoA bind-pose fallback |
| `testAdditiveBlendWeightClamp` | Additive layer weight clamp to `[0, 1]` |
| `testStateMachineFindStateAndEdges` | State lookup, outgoing/incoming edge counts, blend-duration lookup |
| `testStateMachineEvaluateSoACrossfade` | SoA crossfade path, pending/active state names |
| `testEmptyLayeredBlendNode` / `testEmptyAdditiveBlendNode` | Null-child layered/additive nodes return bind pose |
| `testPoseSoAMatchesBind` | `pose_soa_matches_bind` helper |
| `testStateMachineConditionFalse` | False condition leaves active state unchanged |
| `testStateMachineFirstTransitionWins` | First matching transition wins when multiple are true |
| `testStateMachineReset` | `reset()` clears crossfade without extra callbacks |
| `testStateMachineInitialOnEnter` | Initial state `on_enter` on first tick |
| `testStateMachineZeroBlendDuration` | Instant transition when duration is zero |
| `testStateMachineIgnoresSelfTransition` | Self-transition rejected |
| `testStateMachineInvalidTransitionIgnored` | Invalid state names skipped |
| `testStateMachineNoInterruptDuringCrossfade` | No retarget mid-crossfade |
| `testFabrikConverges` | FABRIK end-effector error bound |
| `testSkinningCpuPath` | CPU skinning applies bone transform |
| `testAnimatorTick` | `Animator::tick` advances playback state |

---

## Gates (B7.1 deepen)

- [x] `PoseSoA` local TRS columns with `allocate` / `resize` / `clear`
- [x] `blend_pose_soa` helper for SoA accumulation
- [x] `AnimationClip::evaluate` hierarchy-aware SoA path
- [x] `BlendSpace1D`, `BlendSpace2D`, `LayeredBlendNode`, `AdditiveBlendNode` stubs
- [x] `sample_blend_space_1d/2d` parameter sampling helpers
- [x] `BlendNode::evaluate_soa` PoseSoA integration across blend graph
- [x] `AnimStateMachine` enter/exit callbacks and `crossfade_alpha`
- [x] Layered blend weight sweep and unmasked-bone isolation tests
- [x] Pose buffer clear/reuse tests
- [x] Closed-form `TwoBoneIK` (in-place AoS + SoA) with reach/clamp, pole-bend, parent-chain validation, `max_reach`, and empty-skeleton tests
- [x] `RetargetMap` name pairing, `build_identity`, bidirectional lookup helpers, `translation_scale`, and `apply_pose_soa` / `apply_pose` stubs
- [x] Expanded `PoseSoA` roundtrip, resize, and rotation/scale blend tests
- [x] `fuse_animation_runtime` CTest target green
- [x] Empty blend-tree / state-machine bind-pose fallback tests
- [x] Blend weight clamp and state transition edge-case tests
- [x] `accumulate_weighted_pose_soa` / `copy_pose_soa_local` / `finalize_weighted_pose_soa` PoseSoA helpers
- [x] State machine transition edge queries (`find_state_index`, `outgoing_transition_count`, `incoming_transition_count`, `has_transition`, `transition_blend_duration`, state name helpers)
- [x] `pose_soa_matches_bind` helper and empty layered/additive blend-node fallback tests
- [x] Additive blend weight clamp, empty clip/1D SoA, and state reset/edge-case tests
- [ ] CUDA skinning device kernel — deferred
- [ ] Job-system parallel evaluate — deferred

---

## References

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.1
- [P7.md](../sources/P7.md) §7.1
- [Source/FUSE/Animation/README.md](../../Source/FUSE/Animation/README.md)
- [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md) — B7.10 integration checklist
