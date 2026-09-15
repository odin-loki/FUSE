#include <fuse/scene/camera.hpp>

#include <cmath>

namespace fuse {

namespace {

constexpr float kPi = 3.14159265358979323846f;

void identity4(float m[16]) {
    for (u32 i = 0; i < 16; ++i) {
        m[i] = 0.f;
    }
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

void multiply4(const float a[16], const float b[16], float out[16]) {
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            float sum = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

void perspectiveReversedZ(float fovYRad, float aspect, float nearZ, float farZ, float m[16]) {
    const float f = 1.f / std::tan(fovYRad * 0.5f);
    identity4(m);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = 0.f;
    m[11] = -1.f;
    m[14] = nearZ;
}

void lookAt(float eyeX, float eyeY, float eyeZ, float targetX, float targetY, float targetZ,
            float upX, float upY, float upZ, float m[16]) {
    float fx = targetX - eyeX;
    float fy = targetY - eyeY;
    float fz = targetZ - eyeZ;
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen > 0.f) {
        fx /= flen;
        fy /= flen;
        fz /= flen;
    }

    float sx = fy * upZ - fz * upY;
    float sy = fz * upX - fx * upZ;
    float sz = fx * upY - fy * upX;
    const float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (slen > 0.f) {
        sx /= slen;
        sy /= slen;
        sz /= slen;
    }

    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;

    identity4(m);
    m[0] = sx;
    m[1] = ux;
    m[2] = -fx;
    m[4] = sy;
    m[5] = uy;
    m[6] = -fy;
    m[8] = sz;
    m[9] = uz;
    m[10] = -fz;
    m[12] = -(sx * eyeX + sy * eyeY + sz * eyeZ);
    m[13] = -(ux * eyeX + uy * eyeY + uz * eyeZ);
    m[14] = fx * eyeX + fy * eyeY + fz * eyeZ;
}

void extractFrustum(const float vp[16], Camera::Frustum& frustum) {
    const float rows[4][4] = {
        {vp[0], vp[4], vp[8], vp[12]},
        {vp[1], vp[5], vp[9], vp[13]},
        {vp[2], vp[6], vp[10], vp[14]},
        {vp[3], vp[7], vp[11], vp[15]},
    };

    for (u32 i = 0; i < 4; ++i) {
        frustum.planes[0][i] = rows[3][i] + rows[0][i];
        frustum.planes[1][i] = rows[3][i] - rows[0][i];
        frustum.planes[2][i] = rows[3][i] + rows[1][i];
        frustum.planes[3][i] = rows[3][i] - rows[1][i];
        frustum.planes[4][i] = rows[3][i] + rows[2][i];
        frustum.planes[5][i] = rows[3][i] - rows[2][i];
    }

    for (u32 plane = 0; plane < 6; ++plane) {
        const float a = frustum.planes[plane][0];
        const float b = frustum.planes[plane][1];
        const float c = frustum.planes[plane][2];
        const float len = std::sqrt(a * a + b * b + c * c);
        if (len > 0.f) {
            frustum.planes[plane][0] /= len;
            frustum.planes[plane][1] /= len;
            frustum.planes[plane][2] /= len;
            frustum.planes[plane][3] /= len;
        }
    }
}

} // namespace

void Camera::setPosition(float x, float y, float z) {
    positionX = x;
    positionY = y;
    positionZ = z;
    m_matricesValid = false;
}

void Camera::setOrientation(float yawDegrees, float pitchDegrees) {
    yawDeg = yawDegrees;
    pitchDeg = pitchDegrees;
    m_matricesValid = false;
}

void Camera::update() {
    const float yawRad = yawDeg * (kPi / 180.f);
    const float pitchRad = pitchDeg * (kPi / 180.f);

    const float cosPitch = std::cos(pitchRad);
    const float forwardX = std::sin(yawRad) * cosPitch;
    const float forwardY = std::sin(pitchRad);
    const float forwardZ = std::cos(yawRad) * cosPitch;

    const float targetX = positionX + forwardX;
    const float targetY = positionY + forwardY;
    const float targetZ = positionZ + forwardZ;

    lookAt(positionX, positionY, positionZ, targetX, targetY, targetZ, 0.f, 1.f, 0.f, m_view);

    const float fovRad = fovDeg * (kPi / 180.f);
    perspectiveReversedZ(fovRad, aspectRatio, nearPlane, farPlane, m_projection);
    multiply4(m_projection, m_view, m_viewProjection);
    extractFrustum(m_viewProjection, m_frustum);
    m_matricesValid = true;
}

float Camera::viewMatrixRow(u32 row, u32 col) const {
    return m_view[col * 4 + row];
}

float Camera::projectionMatrixRow(u32 row, u32 col) const {
    return m_projection[col * 4 + row];
}

} // namespace fuse
