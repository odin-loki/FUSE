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

    /// True when `source_pose` has at least `source_bone_count` bones (empty or truncated poses are rejected).
    [[nodiscard]] bool is_source_pose_compatible(const PoseSoA& source_pose) const;

    /// AoS variant of `is_source_pose_compatible`.
    [[nodiscard]] bool is_source_pose_compatible(const Pose& source_pose) const;

    /// True when `target_pose` has at least `target_bone_count` bones (empty or truncated poses are rejected).
    [[nodiscard]] bool is_target_pose_compatible(const PoseSoA& target_pose) const;

    /// AoS variant of `is_target_pose_compatible`.
    [[nodiscard]] bool is_target_pose_compatible(const Pose& target_pose) const;

    /// True when `source_skel.bone_count` matches `source_bone_count` and the map is valid.
    [[nodiscard]] bool is_source_skeleton_compatible(const Skeleton& source_skel) const;

    /// True when `target_skel.bone_count` matches `target_bone_count` and the map is valid.
    [[nodiscard]] bool is_target_skeleton_compatible(const Skeleton& target_skel) const;

    /// True when both skeletons match the map's recorded bone counts.
    [[nodiscard]] bool are_skeletons_compatible(const Skeleton& source_skel, const Skeleton& target_skel) const;

    [[nodiscard]] u32 mapped_bone_count() const { return static_cast<u32>(bone_map.size()); }

    /// Drop all mappings and reset bone counts to zero.
    void clear();

    /// Append a mapping when indices are in range and neither bone is already mapped.
    [[nodiscard]] bool add_bone_mapping(u32 source_bone, u32 target_bone, f32 translation_scale = 1.f);

    [[nodiscard]] bool is_source_mapped(u32 source_bone) const;
    [[nodiscard]] bool is_target_mapped(u32 target_bone) const;

    /// True when the map is valid, the target skeleton is non-empty, and the source pose has bones.
    [[nodiscard]] bool can_apply_pose_soa(const PoseSoA& source_pose, const Skeleton& target_skel) const;

    /// True when the map is valid, the target skeleton is non-empty, and the source pose has bones.
    [[nodiscard]] bool can_apply_pose(const Pose& source_pose, const Skeleton& target_skel) const;

    /// Source bone index for a target bone, or -1 when unmapped.
    [[nodiscard]] s32 find_source_bone(u32 target_bone) const;

    /// Target bone index for a source bone, or -1 when unmapped.
    [[nodiscard]] s32 find_target_bone(u32 source_bone) const;

    /// Pair bones that share the same name in source and target skeletons.
    static RetargetMap build_by_name(const Skeleton& source, const Skeleton& target);

    /// One-to-one identity map for a single skeleton (bone i → bone i).
    static RetargetMap build_identity(const Skeleton& skel);

    /// Copy mapped local TRS from a source pose into a target PoseSoA (unmapped bones keep bind pose).
    /// No-ops and clears `out_pose` when the map is invalid, the source pose is empty or incompatible,
    /// or the target skeleton is empty.
    void apply_pose_soa(const PoseSoA& source_pose, const Skeleton& target_skel, PoseSoA& out_pose) const;

    /// Copy mapped world transforms from a source pose into a target Pose (AoS stub).
    /// No-ops and clears `out_pose` when the map is invalid, the source pose is empty or incompatible,
    /// or the target skeleton is empty.
    void apply_pose(const Pose& source_pose, const Skeleton& target_skel, Pose& out_pose) const;
};

} // namespace fuse::animation
