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

} // namespace fuse::cinematics
