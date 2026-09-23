#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

using vec3 = fuse::ecs::vec3;
using quat = fuse::ecs::quat;
using mat4 = fuse::ecs::mat4;

struct vec2 {
    f32 x = 0.f;
    f32 y = 0.f;
};

inline vec3 lerp(const vec3& a, const vec3& b, f32 t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        0.f,
    };
}

inline quat lerp(const quat& a, const quat& b, f32 t) {
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    quat rhs = b;
    if (dot < 0.f) {
        rhs = {-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }

    if (dot > 0.9995f) {
        // Nearly parallel: normalized lerp avoids the sin(theta) -> 0 division.
        quat out = {
            a.x + t * (rhs.x - a.x),
            a.y + t * (rhs.y - a.y),
            a.z + t * (rhs.z - a.z),
            a.w + t * (rhs.w - a.w),
        };
        const f32 len = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
        if (len > 1e-8f) {
            out = {out.x / len, out.y / len, out.z / len, out.w / len};
        }
        return out;
    }

    const f32 theta = std::acos(std::clamp(dot, -1.f, 1.f));
    const f32 sinTheta = std::sin(theta);
    const f32 w0 = std::sin((1.f - t) * theta) / sinTheta;
    const f32 w1 = std::sin(t * theta) / sinTheta;
    return {
        w0 * a.x + w1 * rhs.x,
        w0 * a.y + w1 * rhs.y,
        w0 * a.z + w1 * rhs.z,
        w0 * a.w + w1 * rhs.w,
    };
}

inline vec3 mat4_translation(const mat4& m) {
    return {m.data[12], m.data[13], m.data[14], 1.f};
}

inline mat4 mat4_multiply(const mat4& a, const mat4& b) {
    mat4 out{};
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a.data[k * 4 + row] * b.data[col * 4 + k];
            }
            out.data[col * 4 + row] = sum;
        }
    }
    return out;
}

inline mat4 mat4_from_trs(const vec3& translation, const quat& rotation, const vec3& scale) {
    const f32 xx = rotation.x * rotation.x;
    const f32 yy = rotation.y * rotation.y;
    const f32 zz = rotation.z * rotation.z;
    const f32 xy = rotation.x * rotation.y;
    const f32 xz = rotation.x * rotation.z;
    const f32 yz = rotation.y * rotation.z;
    const f32 wx = rotation.w * rotation.x;
    const f32 wy = rotation.w * rotation.y;
    const f32 wz = rotation.w * rotation.z;

    mat4 out{};
    out.data[0] = scale.x * (1.f - 2.f * (yy + zz));
    out.data[1] = scale.x * (2.f * (xy + wz));
    out.data[2] = scale.x * (2.f * (xz - wy));
    out.data[4] = scale.y * (2.f * (xy - wz));
    out.data[5] = scale.y * (1.f - 2.f * (xx + zz));
    out.data[6] = scale.y * (2.f * (yz + wx));
    out.data[8] = scale.z * (2.f * (xz + wy));
    out.data[9] = scale.z * (2.f * (yz - wx));
    out.data[10] = scale.z * (1.f - 2.f * (xx + yy));
    out.data[12] = translation.x;
    out.data[13] = translation.y;
    out.data[14] = translation.z;
    out.data[15] = 1.f;
    return out;
}

inline quat quat_normalize(const quat& q) {
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-8f) {
        return {0.f, 0.f, 0.f, 1.f};
    }
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

inline quat quat_conjugate(const quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

/// Hamilton product: rotating by `b` first, then by `a`.
inline quat quat_multiply(const quat& a, const quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline vec3 quat_rotate(const quat& q, const vec3& v) {
    // v' = v + 2w (u x v) + 2 u x (u x v), u = q.xyz
    const f32 cx = q.y * v.z - q.z * v.y;
    const f32 cy = q.z * v.x - q.x * v.z;
    const f32 cz = q.x * v.y - q.y * v.x;
    const f32 ccx = q.y * cz - q.z * cy;
    const f32 ccy = q.z * cx - q.x * cz;
    const f32 ccz = q.x * cy - q.y * cx;
    return {
        v.x + 2.f * (q.w * cx + ccx),
        v.y + 2.f * (q.w * cy + ccy),
        v.z + 2.f * (q.w * cz + ccz),
        0.f,
    };
}

/// Shortest-arc rotation taking direction `from` onto direction `to` (inputs need not be unit length).
inline quat quat_from_to(const vec3& from, const vec3& to) {
    const f32 lf = std::sqrt(from.x * from.x + from.y * from.y + from.z * from.z);
    const f32 lt = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    if (lf < 1e-8f || lt < 1e-8f) {
        return {0.f, 0.f, 0.f, 1.f};
    }
    const vec3 a = {from.x / lf, from.y / lf, from.z / lf, 0.f};
    const vec3 b = {to.x / lt, to.y / lt, to.z / lt, 0.f};
    const f32 d = a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < -0.999999f) {
        // Opposite directions: rotate 180 degrees about any axis perpendicular to `a`.
        vec3 axis = {0.f, -a.z, a.y, 0.f};
        if (axis.y * axis.y + axis.z * axis.z < 1e-6f) {
            axis = {a.z, 0.f, -a.x, 0.f};
        }
        const f32 la = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        return {axis.x / la, axis.y / la, axis.z / la, 0.f};
    }
    return quat_normalize({
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
        1.f + d,
    });
}

/// Rotation quaternion from an orthonormal 3x3 basis stored in column-major `m` (columns at 0, 4, 8).
inline quat quat_from_rotation_columns(const f32* c0, const f32* c1, const f32* c2) {
    // Shepperd's method: pick the largest diagonal term for numerical stability.
    const f32 m00 = c0[0], m10 = c0[1], m20 = c0[2];
    const f32 m01 = c1[0], m11 = c1[1], m21 = c1[2];
    const f32 m02 = c2[0], m12 = c2[1], m22 = c2[2];
    const f32 trace = m00 + m11 + m22;
    quat q{};
    if (trace > 0.f) {
        const f32 s = std::sqrt(trace + 1.f) * 2.f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const f32 s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    q = quat_normalize(q);
    if (q.w < 0.f) {
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    return q;
}

/// Decompose an affine TRS matrix (no shear). Scale is the per-axis column length; a negative
/// determinant is folded into scale.x so the extracted rotation stays proper.
inline void decompose_trs(const mat4& matrix, vec3& out_translation, quat& out_rotation, vec3& out_scale) {
    out_translation = mat4_translation(matrix);
    out_translation.w = 0.f;

    const f32* d = matrix.data.data();
    f32 sx = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const f32 sy = std::sqrt(d[4] * d[4] + d[5] * d[5] + d[6] * d[6]);
    const f32 sz = std::sqrt(d[8] * d[8] + d[9] * d[9] + d[10] * d[10]);
    const f32 det = d[0] * (d[5] * d[10] - d[9] * d[6]) - d[4] * (d[1] * d[10] - d[9] * d[2]) +
                    d[8] * (d[1] * d[6] - d[5] * d[2]);
    if (det < 0.f) {
        sx = -sx;
    }
    out_scale = {sx, sy, sz, 0.f};

    if (std::fabs(sx) < 1e-8f || sy < 1e-8f || sz < 1e-8f) {
        out_rotation = {0.f, 0.f, 0.f, 1.f};
        return;
    }

    const f32 c0[3] = {d[0] / sx, d[1] / sx, d[2] / sx};
    const f32 c1[3] = {d[4] / sy, d[5] / sy, d[6] / sy};
    const f32 c2[3] = {d[8] / sz, d[9] / sz, d[10] / sz};
    out_rotation = quat_from_rotation_columns(c0, c1, c2);
}

/// Inverse of an affine matrix (general 3x3 linear part + translation). Returns identity when singular.
inline mat4 mat4_inverse_affine(const mat4& m) {
    const f32* d = m.data.data();
    const f32 a00 = d[0], a10 = d[1], a20 = d[2];
    const f32 a01 = d[4], a11 = d[5], a21 = d[6];
    const f32 a02 = d[8], a12 = d[9], a22 = d[10];
    const f32 c00 = a11 * a22 - a12 * a21;
    const f32 c01 = a12 * a20 - a10 * a22;
    const f32 c02 = a10 * a21 - a11 * a20;
    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) < 1e-12f) {
        return mat4::identity();
    }
    const f32 inv = 1.f / det;
    // Inverse = adjugate / det (adjugate = transpose of the cofactor matrix).
    const f32 i00 = c00 * inv;
    const f32 i01 = (a02 * a21 - a01 * a22) * inv;
    const f32 i02 = (a01 * a12 - a02 * a11) * inv;
    const f32 i10 = c01 * inv;
    const f32 i11 = (a00 * a22 - a02 * a20) * inv;
    const f32 i12 = (a02 * a10 - a00 * a12) * inv;
    const f32 i20 = c02 * inv;
    const f32 i21 = (a01 * a20 - a00 * a21) * inv;
    const f32 i22 = (a00 * a11 - a01 * a10) * inv;

    mat4 out{};
    out.data[0] = i00;
    out.data[1] = i10;
    out.data[2] = i20;
    out.data[4] = i01;
    out.data[5] = i11;
    out.data[6] = i21;
    out.data[8] = i02;
    out.data[9] = i12;
    out.data[10] = i22;
    const f32 tx = d[12], ty = d[13], tz = d[14];
    out.data[12] = -(i00 * tx + i01 * ty + i02 * tz);
    out.data[13] = -(i10 * tx + i11 * ty + i12 * tz);
    out.data[14] = -(i20 * tx + i21 * ty + i22 * tz);
    out.data[15] = 1.f;
    return out;
}

/// Transform a point by an affine column-major matrix.
inline vec3 mat4_transform_point(const mat4& m, const vec3& p) {
    return {
        m.data[0] * p.x + m.data[4] * p.y + m.data[8] * p.z + m.data[12],
        m.data[1] * p.x + m.data[5] * p.y + m.data[9] * p.z + m.data[13],
        m.data[2] * p.x + m.data[6] * p.y + m.data[10] * p.z + m.data[14],
        0.f,
    };
}

} // namespace fuse::animation
