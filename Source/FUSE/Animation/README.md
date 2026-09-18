# fuse_animation — B7.1 Animation System

CPU-first skeletal animation scaffolding for Track B7.1. Implements the P7 pipeline stages:

`sample → blend → solve IK → skin`

## Layout

| Header | Role |
|--------|------|
| `skeleton.hpp` | Bone hierarchy, bind pose, `Pose` (AoS), `PoseSoA` (local TRS columns) |
| `clip.hpp` | Keyframe channels, `evaluate` (SoA) and `sample` (AoS) |
| `blend_tree.hpp` | `ClipNode`, `BlendNode2`, `BlendSpace1D/2D`, `LayeredBlendNode`, `AdditiveBlendNode`, state machine |
| `ik_solver.hpp` | FABRIK stub; closed-form two-bone IK (AoS + SoA) with public solve helpers |
| `retarget.hpp` | Name-driven `RetargetMap` bone pairing, manual mapping, and pose copy stub |
| `skinning.hpp` | CPU linear blend skinning (`Cuda` when `FUSE_HAS_CUDA`) |
| `animator.hpp` | Game-thread animator component facade |

## Clip evaluation

`AnimationClip::evaluate` samples sparse per-bone position/rotation/scale channels into a `PoseSoA`, then walks the skeleton hierarchy to produce world-space bone matrices. `sample` wraps `evaluate` and returns the legacy `Pose` AoS layout for skinning callers.

## IK (stub)

- **FABRIKChain** — iterative reach toward a world-space target; `has_valid_chain` rejects empty/out-of-range index lists.
- **TwoBoneIK** — O(1) law-of-cosines limb solver with pole-vector bend plane; public `solve_two_bone_positions` / `clamp_two_bone_target` / `normalize_ik_pole_vector` helpers; zero/parallel pole fallback via `effective_pole_vector`; degenerate-segment guard; `has_valid_pose` bind-fallback validation; clamps unreachable targets via `max_reach`; parent-chain `has_valid_chain` validation; solves in-place.

## Retarget (stub)

`RetargetMap::build_by_name` pairs source/target bones by name; `build_identity` maps a skeleton onto itself. `add_bone_mapping` / `clear` / `is_source_mapped` / `is_target_mapped` support manual map editing. `find_source_bone` / `find_target_bone` / `mapped_bone_count` support lookup. `is_valid` rejects empty maps, out-of-range indices, and duplicate source/target pairings. `apply_pose_soa` copies mapped local TRS (with optional `translation_scale`) into a target skeleton bind pose and recomputes world transforms.

## Blend tree (stub)

- **BlendSpace1D / 2D** — parameter sampling via `sample_blend_space_1d/2d`, evaluated through `evaluate_soa`; empty entry lists fall back to bind pose via `finalize_weighted_pose_soa`.
- **LayeredBlendNode** — masked override blend over a base pose in SoA local TRS.
- **AdditiveBlendNode** — masked bind-relative local TRS delta via `add_pose_soa` (`layer_weight` clamped to `[0, 1]`).
- **AnimStateMachine** — `on_enter` / `on_exit` callbacks (initial state enters on first tick), crossfade weight via `crossfade_alpha()`, zero-duration instant snap, `reset()` for state rewind, bidirectional transition edge queries (`outgoing_transition_count`, `incoming_transition_count`, `transition_blend_duration`), and `active_state_name` / `pending_state_name` labels.

## Tests

`fuse_animation_tests` (`ctest` name `fuse_animation_runtime`) covers skeleton hierarchy, `PoseSoA` propagation, roundtrip, resize defaults, buffer clear/reuse, `blend_pose_soa` / `accumulate_weighted_pose_soa` / `finalize_weighted_pose_soa` / `copy_pose_soa_local` / `pose_soa_matches_bind`, clip evaluate/sample, blend interpolation and weight clamping, empty clip/1D/2D/layered/additive blend-tree fallbacks, 1D/2D blend spaces and parameter sampling, `evaluate_soa`, layered and additive mask weight sweeps, state enter/exit, SoA crossfade path, initial `on_enter`, zero-duration snap, self-transition rejection, bidirectional transition edge queries, false-condition guard, first-transition priority, `reset()`, crossfade non-interrupt, two-bone IK reach/clamp/in-place/empty-skeleton (AoS + SoA), retarget map pairing/identity/empty-skeleton/apply, FABRIK convergence, skinning, and animator ticks without GPU or editor dependencies.

Track B narrative: [docs/unification/TRACK-B-ANIMATION.md](../../../docs/unification/TRACK-B-ANIMATION.md).
