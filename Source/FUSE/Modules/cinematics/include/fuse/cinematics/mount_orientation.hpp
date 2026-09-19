#pragma once

// Ore: Verve ShapeBase mount — yaw vs quaternion rotation stub (no Torque math)

#include <fuse/types.hpp>

namespace fuse::cinematics {

struct MountQuaternion {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 1.f;
};

/// Build a yaw-only quaternion from degrees (Z-up stub).
[[nodiscard]] MountQuaternion yaw_deg_to_quaternion(f32 yaw_deg);

/// Extract yaw degrees from a yaw-only quaternion.
[[nodiscard]] f32 quaternion_to_yaw_deg(const MountQuaternion& quat);

/// Combine mount-point offset yaw with event yaw (degrees).
[[nodiscard]] f32 combine_mount_yaw_deg(f32 mount_offset_yaw_deg, f32 event_yaw_deg);

/// Multiply two yaw-only mount quaternions (ShapeBase mount chain ore).
[[nodiscard]] MountQuaternion multiply_mount_quaternions(const MountQuaternion& lhs, const MountQuaternion& rhs);

/// Apply mount-point offset quaternion to event quaternion.
[[nodiscard]] MountQuaternion combine_mount_orientation(const MountQuaternion& mount_offset,
                                                          const MountQuaternion& event_orientation);

} // namespace fuse::cinematics
