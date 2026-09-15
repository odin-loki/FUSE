#include <fuse/ecs/math/mat.hpp>

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::ecs {

mat4 from_trs(const vec3& position, const quat& rotation, const vec3& scale) {
    const f32 x = rotation.x;
    const f32 y = rotation.y;
    const f32 z = rotation.z;
    const f32 w = rotation.w;

    const f32 xx = x * x;
    const f32 yy = y * y;
    const f32 zz = z * z;
    const f32 xy = x * y;
    const f32 xz = x * z;
    const f32 yz = y * z;
    const f32 wx = w * x;
    const f32 wy = w * y;
    const f32 wz = w * z;

    mat4 result{};
    result.data[0] = (1.f - 2.f * (yy + zz)) * scale.x;
    result.data[1] = (2.f * (xy + wz)) * scale.x;
    result.data[2] = (2.f * (xz - wy)) * scale.x;

    result.data[4] = (2.f * (xy - wz)) * scale.y;
    result.data[5] = (1.f - 2.f * (xx + zz)) * scale.y;
    result.data[6] = (2.f * (yz + wx)) * scale.y;

    result.data[8] = (2.f * (xz + wy)) * scale.z;
    result.data[9] = (2.f * (yz - wx)) * scale.z;
    result.data[10] = (1.f - 2.f * (xx + yy)) * scale.z;

    result.data[12] = position.x;
    result.data[13] = position.y;
    result.data[14] = position.z;
    result.data[15] = 1.f;
    return result;
}

mat4 multiply(const mat4& a, const mat4& b) {
    mat4 result{};
    for (u32 row = 0; row < 4; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a.data[row + k * 4] * b.data[k + col * 4];
            }
            result.data[row + col * 4] = sum;
        }
    }
    return result;
}

mat4 inverse_affine(const mat4& matrix) {
    const f32 r00 = matrix.data[0];
    const f32 r01 = matrix.data[4];
    const f32 r02 = matrix.data[8];
    const f32 tx = matrix.data[12];
    const f32 r10 = matrix.data[1];
    const f32 r11 = matrix.data[5];
    const f32 r12 = matrix.data[9];
    const f32 ty = matrix.data[13];
    const f32 r20 = matrix.data[2];
    const f32 r21 = matrix.data[6];
    const f32 r22 = matrix.data[10];
    const f32 tz = matrix.data[14];

    mat4 result{};
    result.data[0] = r00;
    result.data[1] = r10;
    result.data[2] = r20;

    result.data[4] = r01;
    result.data[5] = r11;
    result.data[6] = r21;

    result.data[8] = r02;
    result.data[9] = r12;
    result.data[10] = r22;

    result.data[12] = -(r00 * tx + r01 * ty + r02 * tz);
    result.data[13] = -(r10 * tx + r11 * ty + r12 * tz);
    result.data[14] = -(r20 * tx + r21 * ty + r22 * tz);
    result.data[15] = 1.f;
    return result;
}

mat4 perspective(f32 fov_deg, f32 aspect, f32 near_plane, f32 far_plane) {
    const f32 f = 1.f / std::tan(fov_deg * 0.5f * 3.14159265f / 180.f);
    mat4 result{};
    result.data[0] = f / aspect;
    result.data[5] = f;
    result.data[10] = far_plane / (near_plane - far_plane);
    result.data[11] = -1.f;
    result.data[14] = (near_plane * far_plane) / (near_plane - far_plane);
    return result;
}

mat4 look_at(const vec3& eye, const vec3& target, const vec3& up) {
    vec3 forward{target.x - eye.x, target.y - eye.y, target.z - eye.z, 0.f};
    const f32 flen = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
    if (flen > 0.f) {
        forward.x /= flen;
        forward.y /= flen;
        forward.z /= flen;
    }

    vec3 side{
        forward.y * up.z - forward.z * up.y,
        forward.z * up.x - forward.x * up.z,
        forward.x * up.y - forward.y * up.x,
        0.f,
    };
    const f32 slen = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
    if (slen > 0.f) {
        side.x /= slen;
        side.y /= slen;
        side.z /= slen;
    }

    const vec3 cam_up{
        side.y * forward.z - side.z * forward.y,
        side.z * forward.x - side.x * forward.z,
        side.x * forward.y - side.y * forward.x,
        0.f,
    };

    mat4 result = mat4::identity();
    result.data[0] = side.x;
    result.data[1] = cam_up.x;
    result.data[2] = -forward.x;

    result.data[4] = side.y;
    result.data[5] = cam_up.y;
    result.data[6] = -forward.y;

    result.data[8] = side.z;
    result.data[9] = cam_up.z;
    result.data[10] = -forward.z;

    result.data[12] = -(side.x * eye.x + side.y * eye.y + side.z * eye.z);
    result.data[13] = -(cam_up.x * eye.x + cam_up.y * eye.y + cam_up.z * eye.z);
    result.data[14] = forward.x * eye.x + forward.y * eye.y + forward.z * eye.z;
    return result;
}

vec3 transform_point(const mat4& matrix, const vec3& point) {
    return {
        matrix.data[0] * point.x + matrix.data[4] * point.y + matrix.data[8] * point.z + matrix.data[12],
        matrix.data[1] * point.x + matrix.data[5] * point.y + matrix.data[9] * point.z + matrix.data[13],
        matrix.data[2] * point.x + matrix.data[6] * point.y + matrix.data[10] * point.z + matrix.data[14],
        1.f,
    };
}

} // namespace fuse::ecs
