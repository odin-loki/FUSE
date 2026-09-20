#pragma once

// Ore: Verve ShapeBase mount — yaw/pitch/roll vs quaternion rotation stub (no Torque math)

#include <fuse/types.hpp>

#include <vector>

namespace fuse::cinematics {

struct MountQuaternion {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 1.f;
};

struct MountEulerDeg {
    f32 yaw_deg = 0.f;
    f32 pitch_deg = 0.f;
    f32 roll_deg = 0.f;
};

/// Build a yaw-only quaternion from degrees (Z-up stub).
[[nodiscard]] MountQuaternion yaw_deg_to_quaternion(f32 yaw_deg);

/// Build a yaw/pitch/roll quaternion (ShapeBase mount rotation deepen).
[[nodiscard]] MountQuaternion euler_deg_to_quaternion(const MountEulerDeg& euler);

/// Extract yaw degrees from a yaw-only quaternion.
[[nodiscard]] f32 quaternion_to_yaw_deg(const MountQuaternion& quat);

/// Extract yaw/pitch/roll degrees from a quaternion (ShapeBase mount rotation deepen).
[[nodiscard]] MountEulerDeg quaternion_to_euler_deg(const MountQuaternion& quat);

/// Combine mount-point offset yaw with event yaw (degrees).
[[nodiscard]] f32 combine_mount_yaw_deg(f32 mount_offset_yaw_deg, f32 event_yaw_deg);

/// Combine mount-point offset euler with event euler (degrees).
[[nodiscard]] MountEulerDeg combine_mount_euler_deg(const MountEulerDeg& mount_offset, const MountEulerDeg& event);

/// Multiply two yaw-only mount quaternions (ShapeBase mount chain ore).
[[nodiscard]] MountQuaternion multiply_mount_quaternions(const MountQuaternion& lhs, const MountQuaternion& rhs);

/// Apply mount-point offset quaternion to event quaternion.
[[nodiscard]] MountQuaternion combine_mount_orientation(const MountQuaternion& mount_offset,
                                                          const MountQuaternion& event_orientation);

/// Combine a ShapeBase mount chain (parent ⊗ child ⊗ … ⊗ event).
[[nodiscard]] MountQuaternion combine_mount_chain(const std::vector<MountQuaternion>& chain);

/// Combine mount-chain euler offsets (additive degrees stub).
[[nodiscard]] MountEulerDeg combine_mount_chain_euler(const std::vector<MountEulerDeg>& chain);

} // namespace fuse::cinematics
