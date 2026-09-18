#pragma once

#include <fuse/math/quat.hpp>
#include <fuse/math/vec.hpp>

#include <array>
#include <cmath>

namespace fuse::math {

/// Column-major 3x3 matrix (OpenGL / Vulkan convention).
struct Mat3 {
    std::array<f32, 9> data{};

    static Mat3 identity() {
        Mat3 m{};
        m.data[0] = 1.f;
        m.data[4] = 1.f;
        m.data[8] = 1.f;
        return m;
    }

    f32& at(u32 row, u32 col) { return data[row + col * 3]; }
    f32 at(u32 row, u32 col) const { return data[row + col * 3]; }
};

/// Column-major 4x4 matrix (OpenGL / Vulkan convention).
struct Mat4 {
    std::array<f32, 16> data{};

    static Mat4 identity() {
        Mat4 m{};
        m.data[0] = 1.f;
        m.data[5] = 1.f;
        m.data[10] = 1.f;
        m.data[15] = 1.f;
        return m;
    }

    f32& at(u32 row, u32 col) { return data[row + col * 4]; }
    f32 at(u32 row, u32 col) const { return data[row + col * 4]; }

    Mat3 upper3x3() const {
        Mat3 m{};
        m.data[0] = data[0];
        m.data[1] = data[1];
        m.data[2] = data[2];
        m.data[3] = data[4];
        m.data[4] = data[5];
        m.data[5] = data[6];
        m.data[6] = data[8];
        m.data[7] = data[9];
        m.data[8] = data[10];
        return m;
    }
};

inline Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 result{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 3; ++col) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 3; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            result.at(row, col) = sum;
        }
    }
    return result;
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (u32 row = 0; row < 4; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            result.at(row, col) = sum;
        }
    }
    return result;
}

inline Mat4 fromTRS(const Vec3& position, const Quat& rotation, const Vec3& scale) {
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

    Mat4 result{};
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

/// Rigid transform (rotation + translation, unit scale) without building a full TRS matrix.
inline Mat4 fromRotationTranslation(const Quat& rotation, const Vec3& translation) {
    return fromTRS(translation, rotation, {1.f, 1.f, 1.f});
}

/// True when the upper 3×3 block columns are mutually orthogonal with unit length (pure rotation).
inline bool isOrthogonalUpper3x3(const Mat4& matrix, f32 epsilon = 1e-4f) {
    const Vec3 x{matrix.data[0], matrix.data[1], matrix.data[2]};
    const Vec3 y{matrix.data[4], matrix.data[5], matrix.data[6]};
    const Vec3 z{matrix.data[8], matrix.data[9], matrix.data[10]};

    const f32 xy = std::fabs(x.dot(y));
    const f32 xz = std::fabs(x.dot(z));
    const f32 yz = std::fabs(y.dot(z));
    const f32 xLen = x.length();
    const f32 yLen = y.length();
    const f32 zLen = z.length();

    if (xLen < epsilon || yLen < epsilon || zLen < epsilon) {
        return false;
    }

    const f32 unitTolerance = epsilon * std::max(1.f, std::max(xLen, std::max(yLen, zLen)));
    if (std::fabs(xLen - 1.f) > unitTolerance || std::fabs(yLen - 1.f) > unitTolerance ||
        std::fabs(zLen - 1.f) > unitTolerance) {
        return false;
    }

    return xy <= unitTolerance && xz <= unitTolerance && yz <= unitTolerance;
}

/// True when the upper 3×3 block is orthogonal with uniform column length (rotation ± uniform scale).
inline bool isRigidUpper3x3(const Mat4& matrix, f32 epsilon = 1e-4f) {
    const Vec3 x{matrix.data[0], matrix.data[1], matrix.data[2]};
    const Vec3 y{matrix.data[4], matrix.data[5], matrix.data[6]};
    const Vec3 z{matrix.data[8], matrix.data[9], matrix.data[10]};

    const f32 xLen = x.length();
    const f32 yLen = y.length();
    const f32 zLen = z.length();

    if (xLen < epsilon || yLen < epsilon || zLen < epsilon) {
        return false;
    }

    const f32 scaleTolerance = epsilon * std::max(1.f, std::max(xLen, std::max(yLen, zLen)));
    if (std::fabs(xLen - yLen) > scaleTolerance || std::fabs(yLen - zLen) > scaleTolerance ||
        std::fabs(xLen - zLen) > scaleTolerance) {
        return false;
    }

    const f32 orthoTolerance = scaleTolerance;
    return std::fabs(x.dot(y)) <= orthoTolerance && std::fabs(x.dot(z)) <= orthoTolerance &&
           std::fabs(y.dot(z)) <= orthoTolerance;
}

inline Mat4 inverseAffine(const Mat4& matrix) {
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

    Mat4 result{};
    result.data[0] = r00;
    result.data[1] = r01;
    result.data[2] = r02;

    result.data[4] = r10;
    result.data[5] = r11;
    result.data[6] = r12;

    result.data[8] = r20;
    result.data[9] = r21;
    result.data[10] = r22;

    result.data[12] = -(r00 * tx + r10 * ty + r20 * tz);
    result.data[13] = -(r01 * tx + r11 * ty + r21 * tz);
    result.data[14] = -(r02 * tx + r12 * ty + r22 * tz);
    result.data[15] = 1.f;
    return result;
}

/// True when the bottom row is `[0, 0, 0, 1]` (affine transform, not perspective).
inline bool isAffine(const Mat4& matrix, f32 epsilon = 1e-5f) {
    return std::fabs(matrix.data[3]) <= epsilon && std::fabs(matrix.data[7]) <= epsilon &&
           std::fabs(matrix.data[11]) <= epsilon && std::fabs(matrix.data[15] - 1.f) <= epsilon;
}

/// True when the matrix is an affine rigid transform (orthogonal upper 3×3 with uniform column length).
inline bool isRigid(const Mat4& matrix, f32 epsilon = 1e-4f) {
    return isAffine(matrix, epsilon) && isRigidUpper3x3(matrix, epsilon);
}

/// Translation column of an affine matrix; returns zero when the matrix is not affine.
inline Vec3 extractTranslation(const Mat4& matrix, f32 epsilon = 1e-5f) {
    if (!isAffine(matrix, epsilon)) {
        return {};
    }
    return {matrix.data[12], matrix.data[13], matrix.data[14]};
}

/// Uniform column length of a rigid upper 3×3 block; returns `0` when the block is not rigid.
inline f32 uniformScaleUpper3x3(const Mat4& matrix, f32 epsilon = 1e-4f) {
    if (!isRigidUpper3x3(matrix, epsilon)) {
        return 0.f;
    }
    const Vec3 x{matrix.data[0], matrix.data[1], matrix.data[2]};
    return x.length();
}

/// Writes `inverseAffine(matrix)` to `out` when the upper 3×3 block is a pure rotation; returns false for any scale.
inline bool tryInverseAffine(const Mat4& matrix, Mat4& out) {
    if (!isOrthogonalUpper3x3(matrix)) {
        return false;
    }
    out = inverseAffine(matrix);
    return true;
}

/// Writes the inverse of a rigid affine matrix (rotation ± uniform scale + translation) to `out`.
inline bool tryInverseRigid(const Mat4& matrix, Mat4& out, f32 epsilon = 1e-4f) {
    if (!isAffine(matrix, epsilon) || !isRigidUpper3x3(matrix, epsilon)) {
        return false;
    }

    const f32 scale = uniformScaleUpper3x3(matrix, epsilon);
    if (scale < epsilon) {
        return false;
    }

    const f32 invScaleSq = 1.f / (scale * scale);
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

    Mat4 result{};
    result.data[0] = r00 * invScaleSq;
    result.data[1] = r01 * invScaleSq;
    result.data[2] = r02 * invScaleSq;

    result.data[4] = r10 * invScaleSq;
    result.data[5] = r11 * invScaleSq;
    result.data[6] = r12 * invScaleSq;

    result.data[8] = r20 * invScaleSq;
    result.data[9] = r21 * invScaleSq;
    result.data[10] = r22 * invScaleSq;

    result.data[12] = -(r00 * tx + r10 * ty + r20 * tz) * invScaleSq;
    result.data[13] = -(r01 * tx + r11 * ty + r21 * tz) * invScaleSq;
    result.data[14] = -(r02 * tx + r12 * ty + r22 * tz) * invScaleSq;
    result.data[15] = 1.f;
    out = result;
    return true;
}

inline Mat4 perspective(f32 fov_deg, f32 aspect, f32 near_plane, f32 far_plane) {
    const f32 f = 1.f / std::tan(fov_deg * 0.5f * 3.14159265f / 180.f);
    Mat4 result{};
    result.data[0] = f / aspect;
    result.data[5] = f;
    result.data[10] = far_plane / (near_plane - far_plane);
    result.data[11] = -1.f;
    result.data[14] = (near_plane * far_plane) / (near_plane - far_plane);
    return result;
}

inline Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 forward = (target - eye).normalized();
    const Vec3 side = cross(forward, up).normalized();
    const Vec3 cam_up = cross(side, forward);

    Mat4 result = Mat4::identity();
    result.data[0] = side.x;
    result.data[1] = cam_up.x;
    result.data[2] = -forward.x;

    result.data[4] = side.y;
    result.data[5] = cam_up.y;
    result.data[6] = -forward.y;

    result.data[8] = side.z;
    result.data[9] = cam_up.z;
    result.data[10] = -forward.z;

    result.data[12] = -side.dot(eye);
    result.data[13] = -cam_up.dot(eye);
    result.data[14] = forward.dot(eye);
    return result;
}

inline Vec3 transformPoint(const Mat4& matrix, const Vec3& point) {
    return {
        matrix.data[0] * point.x + matrix.data[4] * point.y + matrix.data[8] * point.z + matrix.data[12],
        matrix.data[1] * point.x + matrix.data[5] * point.y + matrix.data[9] * point.z + matrix.data[13],
        matrix.data[2] * point.x + matrix.data[6] * point.y + matrix.data[10] * point.z + matrix.data[14],
    };
}

inline Vec3 transformDirection(const Mat3& matrix, const Vec3& direction) {
    return {
        matrix.data[0] * direction.x + matrix.data[3] * direction.y + matrix.data[6] * direction.z,
        matrix.data[1] * direction.x + matrix.data[4] * direction.y + matrix.data[7] * direction.z,
        matrix.data[2] * direction.x + matrix.data[5] * direction.y + matrix.data[8] * direction.z,
    };
}

inline Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) {
    return transformDirection(matrix.upper3x3(), direction);
}

/// Transposed upper 3×3 block (inverse rotation when the block is orthogonal).
inline Mat3 transposeUpper3x3(const Mat4& matrix) {
    Mat3 result{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 3; ++col) {
            result.at(row, col) = matrix.at(col, row);
        }
    }
    return result;
}

/// Writes `a * b` to `out` when both operands are rigid affine transforms.
inline bool tryMultiplyRigid(const Mat4& a, const Mat4& b, Mat4& out, f32 epsilon = 1e-4f) {
    if (!isRigid(a, epsilon) || !isRigid(b, epsilon)) {
        return false;
    }
    out = multiply(a, b);
    return true;
}

/// Extracts a unit quaternion from a pure-rotation upper 3×3 block.
inline bool tryToRotationQuat(const Mat4& matrix, Quat& out, f32 epsilon = 1e-4f) {
    if (!isOrthogonalUpper3x3(matrix, epsilon)) {
        return false;
    }

    const Mat3 rot = matrix.upper3x3();
    const f32 trace = rot.at(0, 0) + rot.at(1, 1) + rot.at(2, 2);

    if (trace > 0.f) {
        const f32 s = std::sqrt(trace + 1.f) * 2.f;
        out = Quat{
            (rot.at(2, 1) - rot.at(1, 2)) / s,
            (rot.at(0, 2) - rot.at(2, 0)) / s,
            (rot.at(1, 0) - rot.at(0, 1)) / s,
            0.25f * s,
        };
    } else if (rot.at(0, 0) > rot.at(1, 1) && rot.at(0, 0) > rot.at(2, 2)) {
        const f32 s = std::sqrt(1.f + rot.at(0, 0) - rot.at(1, 1) - rot.at(2, 2)) * 2.f;
        out = Quat{
            0.25f * s,
            (rot.at(0, 1) + rot.at(1, 0)) / s,
            (rot.at(0, 2) + rot.at(2, 0)) / s,
            (rot.at(2, 1) - rot.at(1, 2)) / s,
        };
    } else if (rot.at(1, 1) > rot.at(2, 2)) {
        const f32 s = std::sqrt(1.f + rot.at(1, 1) - rot.at(0, 0) - rot.at(2, 2)) * 2.f;
        out = Quat{
            (rot.at(0, 1) + rot.at(1, 0)) / s,
            0.25f * s,
            (rot.at(1, 2) + rot.at(2, 1)) / s,
            (rot.at(0, 2) - rot.at(2, 0)) / s,
        };
    } else {
        const f32 s = std::sqrt(1.f + rot.at(2, 2) - rot.at(0, 0) - rot.at(1, 1)) * 2.f;
        out = Quat{
            (rot.at(0, 2) + rot.at(2, 0)) / s,
            (rot.at(1, 2) + rot.at(2, 1)) / s,
            0.25f * s,
            (rot.at(1, 0) - rot.at(0, 1)) / s,
        };
    }

    out = out.normalized();
    return true;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b) { return multiply(a, b); }
inline Mat3 operator*(const Mat3& a, const Mat3& b) { return multiply(a, b); }

} // namespace fuse::math
