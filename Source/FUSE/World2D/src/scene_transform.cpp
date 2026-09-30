#include <fuse/world2d/scene_transform.hpp>

#include <fuse/object.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

#include <algorithm>
#include <cmath>

namespace fuse {

const SceneObject2D* asSceneObject2D(const Object* obj) {
    if (obj == nullptr || obj->sceneNodeKind() == Object::SceneNodeKind::None) {
        return nullptr;
    }
    return static_cast<const SceneObject2D*>(obj);
}

SceneObject2D* asSceneObject2D(Object* obj) {
    return const_cast<SceneObject2D*>(asSceneObject2D(static_cast<const Object*>(obj)));
}

namespace scene_math {

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;

math::Quat axisQuat(float ax, float ay, float az, float radians) {
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return {ax * s, ay * s, az * s, std::cos(half)};
}

} // namespace

math::Quat quatFromEulerDeg(float yawDeg, float pitchDeg, float rollDeg) {
    const math::Quat yaw = axisQuat(0.f, 0.f, 1.f, yawDeg * kDegToRad);
    const math::Quat pitch = axisQuat(1.f, 0.f, 0.f, pitchDeg * kDegToRad);
    const math::Quat roll = axisQuat(0.f, 1.f, 0.f, rollDeg * kDegToRad);
    return (yaw * pitch * roll).normalized();
}

void eulerDegFromQuat(const math::Quat& qIn, float& yawDeg, float& pitchDeg, float& rollDeg) {
    const math::Quat q = qIn.normalized();
    // R = Rz(yaw) Rx(pitch) Ry(roll): R[2][1] = sin(pitch); R[0][1] = -sin(yaw) cos(pitch);
    // R[1][1] = cos(yaw) cos(pitch); R[2][0] = -cos(pitch) sin(roll); R[2][2] = cos(pitch) cos(roll).
    const float r21 = 2.f * (q.y * q.z + q.w * q.x);
    const float r01 = 2.f * (q.x * q.y - q.w * q.z);
    const float r11 = 1.f - 2.f * (q.x * q.x + q.z * q.z);
    const float r20 = 2.f * (q.x * q.z - q.w * q.y);
    const float r22 = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float sinPitch = std::clamp(r21, -1.f, 1.f);
    pitchDeg = std::asin(sinPitch) * kRadToDeg;
    if (std::fabs(sinPitch) < 0.99999f) {
        yawDeg = std::atan2(-r01, r11) * kRadToDeg;
        rollDeg = std::atan2(-r20, r22) * kRadToDeg;
    } else {
        // Gimbal lock: yaw and roll share an axis; attribute the twist to yaw.
        const float r10 = 2.f * (q.x * q.y + q.w * q.z);
        const float r00 = 1.f - 2.f * (q.y * q.y + q.z * q.z);
        yawDeg = std::atan2(r10, r00) * kRadToDeg;
        rollDeg = 0.f;
    }
}

math::Mat4 composeTRS(const math::Vec3& t, const math::Quat& r, const math::Vec3& s) {
    return math::fromTRS(t, r, s);
}

math::Mat4 inverseAffineGeneral(const math::Mat4& m) {
    // 3x3 block a(row, col) = m.at(row, col).
    const float a00 = m.at(0, 0), a01 = m.at(0, 1), a02 = m.at(0, 2);
    const float a10 = m.at(1, 0), a11 = m.at(1, 1), a12 = m.at(1, 2);
    const float a20 = m.at(2, 0), a21 = m.at(2, 1), a22 = m.at(2, 2);
    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = a12 * a20 - a10 * a22;
    const float c02 = a10 * a21 - a11 * a20;
    const float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) < 1e-20f) {
        return math::Mat4::identity();
    }
    const float inv = 1.f / det;
    math::Mat4 out = math::Mat4::identity();
    out.at(0, 0) = c00 * inv;
    out.at(0, 1) = (a02 * a21 - a01 * a22) * inv;
    out.at(0, 2) = (a01 * a12 - a02 * a11) * inv;
    out.at(1, 0) = c01 * inv;
    out.at(1, 1) = (a00 * a22 - a02 * a20) * inv;
    out.at(1, 2) = (a02 * a10 - a00 * a12) * inv;
    out.at(2, 0) = c02 * inv;
    out.at(2, 1) = (a01 * a20 - a00 * a21) * inv;
    out.at(2, 2) = (a00 * a11 - a01 * a10) * inv;
    const float tx = m.at(0, 3), ty = m.at(1, 3), tz = m.at(2, 3);
    for (u32 row = 0; row < 3u; ++row) {
        out.at(row, 3) = -(out.at(row, 0) * tx + out.at(row, 1) * ty + out.at(row, 2) * tz);
    }
    return out;
}

math::Quat quatFromBasis(const math::Vec3& x, const math::Vec3& y, const math::Vec3& z) {
    // Shepperd's method on R = [x y z] (columns).
    const float m00 = x.x, m10 = x.y, m20 = x.z;
    const float m01 = y.x, m11 = y.y, m21 = y.z;
    const float m02 = z.x, m12 = z.y, m22 = z.z;
    const float trace = m00 + m11 + m22;
    math::Quat q;
    if (trace > 0.f) {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    q = q.normalized();
    if (q.w < 0.f) {
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    return q;
}

void decomposeTRS(const math::Mat4& m, math::Vec3& t, math::Quat& r, math::Vec3& s) {
    t = {m.data[12], m.data[13], m.data[14]};
    math::Vec3 cx{m.data[0], m.data[1], m.data[2]};
    math::Vec3 cy{m.data[4], m.data[5], m.data[6]};
    math::Vec3 cz{m.data[8], m.data[9], m.data[10]};
    float sx = cx.length();
    const float sy = cy.length();
    const float sz = cz.length();
    // A mirrored block (negative determinant) is represented with a negative x scale.
    if (math::cross(cx, cy).dot(cz) < 0.f) {
        sx = -sx;
    }
    s = {sx, sy, sz};
    if (std::fabs(sx) < 1e-20f || sy < 1e-20f || sz < 1e-20f) {
        r = math::Quat::identity();
        return;
    }
    cx = cx * (1.f / sx);
    cy = cy * (1.f / sy);
    cz = cz * (1.f / sz);
    r = quatFromBasis(cx, cy, cz);
}

float maxAbsDiff(const math::Mat4& a, const math::Mat4& b) {
    float worst = 0.f;
    for (usize i = 0; i < 16u; ++i) {
        worst = std::max(worst, std::fabs(a.data[i] - b.data[i]));
    }
    return worst;
}

} // namespace scene_math

} // namespace fuse
