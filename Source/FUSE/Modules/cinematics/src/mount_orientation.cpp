#include <fuse/cinematics/mount_orientation.hpp>

#include <cmath>

namespace fuse::cinematics {

namespace {
constexpr f32 kPi = 3.14159265358979323846f;
}

MountQuaternion yaw_deg_to_quaternion(f32 yaw_deg) {
    const f32 half = yaw_deg * (kPi / 180.f) * 0.5f;
    MountQuaternion quat{};
    quat.z = std::sin(half);
    quat.w = std::cos(half);
    return quat;
}

f32 quaternion_to_yaw_deg(const MountQuaternion& quat) {
    const f32 siny_cosp = 2.f * (quat.w * quat.z);
    const f32 cosy_cosp = 1.f - 2.f * (quat.z * quat.z);
    return std::atan2(siny_cosp, cosy_cosp) * (180.f / kPi);
}

f32 combine_mount_yaw_deg(f32 mount_offset_yaw_deg, f32 event_yaw_deg) {
    return mount_offset_yaw_deg + event_yaw_deg;
}

MountQuaternion multiply_mount_quaternions(const MountQuaternion& lhs, const MountQuaternion& rhs) {
    MountQuaternion out{};
    out.x = lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y;
    out.y = lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x;
    out.z = lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w;
    out.w = lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z;
    return out;
}

MountQuaternion combine_mount_orientation(const MountQuaternion& mount_offset,
                                          const MountQuaternion& event_orientation) {
    return multiply_mount_quaternions(mount_offset, event_orientation);
}

} // namespace fuse::cinematics
