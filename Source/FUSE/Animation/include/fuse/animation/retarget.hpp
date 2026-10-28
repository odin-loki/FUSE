#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

/// One source→target bone correspondence, with a translation scale and a parent-local rotation offset.
///
/// `translation_scale` scales the copied local translation. `rotation_offset` is composed after the
/// source local rotation is copied:
/// `local_rotation = quat_multiply(normalize(rotation_offset), source_local_rotation)`.
/// `quat_multiply(a, b)` applies `b` first, then `a`. That is the parent-local order used by world
/// transforms, `world = parent * local` (`mat4_multiply(parent, local)`): the source rotation takes
/// bone axes into the parent frame, and the offset is a further rotation in that parent frame.
/// Identity `(0, 0, 0, 1)` leaves the copied source rotation unchanged. Non-finite offsets are
/// rejected. Finite offsets are normalized when a pose is applied; a zero-length offset normalizes
/// to identity.
struct RetargetBoneEntry {
    u32 source_bone = 0;
    u32 target_bone = 0;
    f32 translation_scale = 1.f;
    quat rotation_offset = {0.f, 0.f, 0.f, 1.f};
};

/// Name-driven skeleton retarget map.
struct RetargetMap {
    std::vector<RetargetBoneEntry> bone_map;
    u32 source_bone_count = 0;
    u32 target_bone_count = 0;

    /// True when bone counts are non-zero, every index is in range, and every rotation offset is finite.
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

    /// Append a mapping when indices are in range, neither bone is already mapped, and
    /// `rotation_offset` is finite. `translation_scale` defaults to 1. `rotation_offset` defaults
    /// to identity `(0, 0, 0, 1)`.
    [[nodiscard]] bool add_bone_mapping(u32 source_bone,
                                        u32 target_bone,
                                        f32 translation_scale = 1.f,
                                        quat rotation_offset = {0.f, 0.f, 0.f, 1.f});

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

    /// Pair bones that share the same name in source and target skeletons. Offsets are identity.
    static RetargetMap build_by_name(const Skeleton& source, const Skeleton& target);

    /// One-to-one identity map for a single skeleton (bone i → bone i). Offsets are identity.
    static RetargetMap build_identity(const Skeleton& skel);

    /// Copy mapped local TRS from a source pose into a target PoseSoA (unmapped bones keep bind pose).
    /// Local rotation is `normalize(rotation_offset) * source_local_rotation` (see `RetargetBoneEntry`).
    /// Local translation is multiplied by `translation_scale`; local scale is copied. World transforms
    /// are recomputed from the target hierarchy. No-ops and clears `out_pose` when the map is invalid,
    /// the source pose is empty or incompatible, or the target skeleton is empty.
    void apply_pose_soa(const PoseSoA& source_pose, const Skeleton& target_skel, PoseSoA& out_pose) const;

    /// Copy mapped world transforms from a source pose into a target Pose (unmapped bones keep bind pose).
    /// The copied matrix keeps its translation and column scale. Its rotation is composed with the same
    /// parent-local product as `apply_pose_soa`: `normalize(rotation_offset) * source_rotation`.
    /// An identity offset leaves the copied matrix unchanged. No-ops and clears `out_pose` when the map
    /// is invalid, the source pose is empty or incompatible, or the target skeleton is empty.
    void apply_pose(const Pose& source_pose, const Skeleton& target_skel, Pose& out_pose) const;
};

} // namespace fuse::animation
