#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

/// One source→target bone correspondence with optional translation scale.
struct RetargetBoneEntry {
    u32 source_bone = 0;
    u32 target_bone = 0;
    f32 translation_scale = 1.f;
};

/// Name-driven skeleton retarget map (CPU stub — rotation offsets deferred).
struct RetargetMap {
    std::vector<RetargetBoneEntry> bone_map;
    u32 source_bone_count = 0;
    u32 target_bone_count = 0;

    [[nodiscard]] bool is_valid() const;

    /// Pair bones that share the same name in source and target skeletons.
    static RetargetMap build_by_name(const Skeleton& source, const Skeleton& target);

    /// Copy mapped local TRS from a source pose into a target PoseSoA (unmapped bones keep bind pose).
    void apply_pose_soa(const PoseSoA& source_pose, const Skeleton& target_skel, PoseSoA& out_pose) const;

    /// Copy mapped world transforms from a source pose into a target Pose (AoS stub).
    void apply_pose(const Pose& source_pose, const Skeleton& target_skel, Pose& out_pose) const;
};

} // namespace fuse::animation
