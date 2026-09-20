#include <fuse/cinematics/mount_orientation.hpp>

#include <cmath>

namespace fuse::cinematics {

namespace {
constexpr f32 kPi = 3.14159265358979323846f;

MountQuaternion multiply_quaternions(const MountQuaternion& lhs, const MountQuaternion& rhs) {
    MountQuaternion out{};
    out.x = lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y;
    out.y = lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x;
    out.z = lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w;
    out.w = lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z;
    return out;
}

MountQuaternion axis_angle_quaternion(f32 axisX, f32 axisY, f32 axisZ, f32 angleDeg) {
    const f32 half = angleDeg * (kPi / 180.f) * 0.5f;
    MountQuaternion quat{};
    const f32 s = std::sin(half);
    quat.x = axisX * s;
    quat.y = axisY * s;
    quat.z = axisZ * s;
    quat.w = std::cos(half);
    return quat;
}

} // namespace

MountQuaternion yaw_deg_to_quaternion(f32 yaw_deg) {
    return axis_angle_quaternion(0.f, 0.f, 1.f, yaw_deg);
}

MountQuaternion euler_deg_to_quaternion(const MountEulerDeg& euler) {
    const MountQuaternion yawQuat = axis_angle_quaternion(0.f, 0.f, 1.f, euler.yaw_deg);
    const MountQuaternion pitchQuat = axis_angle_quaternion(0.f, 1.f, 0.f, euler.pitch_deg);
    const MountQuaternion rollQuat = axis_angle_quaternion(1.f, 0.f, 0.f, euler.roll_deg);
    return multiply_quaternions(multiply_quaternions(yawQuat, pitchQuat), rollQuat);
}

f32 quaternion_to_yaw_deg(const MountQuaternion& quat) {
    const f32 siny_cosp = 2.f * (quat.w * quat.z);
    const f32 cosy_cosp = 1.f - 2.f * (quat.z * quat.z);
    return std::atan2(siny_cosp, cosy_cosp) * (180.f / kPi);
}

MountEulerDeg quaternion_to_euler_deg(const MountQuaternion& quat) {
    MountEulerDeg euler{};

    const f32 sinp = 2.f * (quat.w * quat.y - quat.z * quat.x);
    if (std::abs(sinp) >= 1.f) {
        euler.pitch_deg = std::copysign(90.f, sinp);
    } else {
        euler.pitch_deg = std::asin(sinp) * (180.f / kPi);
    }

    const f32 siny_cosp = 2.f * (quat.w * quat.z + quat.x * quat.y);
    const f32 cosy_cosp = 1.f - 2.f * (quat.y * quat.y + quat.z * quat.z);
    euler.yaw_deg = std::atan2(siny_cosp, cosy_cosp) * (180.f / kPi);

    const f32 sinr_cosp = 2.f * (quat.w * quat.x + quat.y * quat.z);
    const f32 cosr_cosp = 1.f - 2.f * (quat.x * quat.x + quat.y * quat.y);
    euler.roll_deg = std::atan2(sinr_cosp, cosr_cosp) * (180.f / kPi);
    return euler;
}

f32 combine_mount_yaw_deg(f32 mount_offset_yaw_deg, f32 event_yaw_deg) {
    return mount_offset_yaw_deg + event_yaw_deg;
}

MountEulerDeg combine_mount_euler_deg(const MountEulerDeg& mount_offset, const MountEulerDeg& event) {
    MountEulerDeg combined{};
    combined.yaw_deg = mount_offset.yaw_deg + event.yaw_deg;
    combined.pitch_deg = mount_offset.pitch_deg + event.pitch_deg;
    combined.roll_deg = mount_offset.roll_deg + event.roll_deg;
    return combined;
}

MountQuaternion multiply_mount_quaternions(const MountQuaternion& lhs, const MountQuaternion& rhs) {
    return multiply_quaternions(lhs, rhs);
}

MountQuaternion combine_mount_orientation(const MountQuaternion& mount_offset,
                                          const MountQuaternion& event_orientation) {
    return multiply_mount_quaternions(mount_offset, event_orientation);
}

MountQuaternion combine_mount_chain(const std::vector<MountQuaternion>& chain) {
    MountQuaternion combined{};
    combined.w = 1.f;
    for (const MountQuaternion& link : chain) {
        combined = multiply_mount_quaternions(combined, link);
    }
    return combined;
}

MountEulerDeg combine_mount_chain_euler(const std::vector<MountEulerDeg>& chain) {
    MountEulerDeg combined{};
    for (const MountEulerDeg& link : chain) {
        combined.yaw_deg += link.yaw_deg;
        combined.pitch_deg += link.pitch_deg;
        combined.roll_deg += link.roll_deg;
    }
    return combined;
}

} // namespace fuse::cinematics
