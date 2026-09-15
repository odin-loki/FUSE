#pragma once

#include <fuse/animation/math.hpp>
#include <fuse/types.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace fuse::animation {

struct Bone {
    char name[64] = {};
    s32 parent_index = -1;
    mat4 inverse_bind = mat4::identity();
    mat4 local_transform = mat4::identity();
};

struct Skeleton {
    std::vector<Bone> bones;
    u32 bone_count = 0;

    s32 find_bone(const char* name) const;
    mat4 compute_world_transform(u32 bone_idx) const;
};

struct Pose {
    std::vector<mat4> bone_world_transforms;
    u32 bone_count = 0;

    static Pose make_bind_pose(const Skeleton& skel);
};

/// Structure-of-arrays bone pose — local TRS columns plus computed world matrices.
struct PoseSoA {
    std::vector<vec3> local_positions;
    std::vector<quat> local_rotations;
    std::vector<vec3> local_scales;
    std::vector<mat4> bone_world_transforms;
    u32 bone_count = 0;

    static PoseSoA allocate(u32 bone_capacity);
    void resize(u32 bone_count);
    void clear();

    static PoseSoA from_bind_pose(const Skeleton& skel);
    Pose to_pose() const;
    void compute_world_transforms(const Skeleton& skel);
};

void blend_pose_soa(const PoseSoA& a, const PoseSoA& b, f32 weight, PoseSoA& out);

/// Copy local TRS columns and bone count from src into dst (world transforms are not copied).
void copy_pose_soa_local(const PoseSoA& src, PoseSoA& dst);

/// Incrementally accumulate a weighted pose into result for multi-entry blend spaces.
/// On first contribution (accumulated_weight == 0), result is seeded from entry.
/// Subsequent calls blend entry in proportionally to its weight relative to the running total.
void accumulate_weighted_pose_soa(PoseSoA& result,
                                  f32& accumulated_weight,
                                  const PoseSoA& entry,
                                  f32 weight);

/// Finalize a weighted accumulation: bind pose when no weight was contributed, otherwise
/// recompute world transforms from the accumulated local TRS columns.
void finalize_weighted_pose_soa(PoseSoA& pose,
                                f32 accumulated_weight,
                                const Skeleton& skel,
                                PoseSoA& out);

/// Additive local TRS delta from bind pose: out = base + weight * (delta - bind) on masked bones.
void add_pose_soa(const PoseSoA& base,
                  const PoseSoA& delta,
                  const PoseSoA& bind,
                  f32 weight,
                  const std::vector<u32>& masked_bones,
                  PoseSoA& out);

/// True when every local TRS column matches the skeleton bind pose within epsilon.
bool pose_soa_matches_bind(const PoseSoA& pose, const Skeleton& skel, f32 epsilon = 1e-4f);

/// True when `pose` has no bones or its bone count does not match the skeleton.
[[nodiscard]] bool needs_pose_soa_bind_fallback(const PoseSoA& pose, const Skeleton& skel);

/// Seed `pose` from skeleton bind pose when it is empty or mismatched.
void ensure_pose_soa_bind_fallback(PoseSoA& pose, const Skeleton& skel);

} // namespace fuse::animation
