#pragma once

#include <fuse/types.hpp>

namespace fuse {

/// Perspective camera parameters (B3.8 stub — matrices derived on update).
class Camera {
public:
    static constexpr const char* typeName = "Camera";

    struct Frustum {
        float planes[6][4] = {}; // left, right, top, bottom, near, far — Ax+By+Cz+D=0
    };

    float fovDeg = 75.f;
    float nearPlane = 0.1f;
    float farPlane = 10000.f;
    float aspectRatio = 16.f / 9.f;
    bool isActive = true;

    float positionX = 0.f;
    float positionY = 0.f;
    float positionZ = 10.f;
    float yawDeg = 0.f;
    float pitchDeg = 0.f;

    void setPosition(float x, float y, float z);
    void setOrientation(float yawDegrees, float pitchDegrees);

    /// Recomputes view/projection/frustum caches (reversed-Z projection stub).
    void update();

    bool matricesValid() const { return m_matricesValid; }
    const Frustum& frustum() const { return m_frustum; }

    float viewMatrixRow(u32 row, u32 col) const;
    float projectionMatrixRow(u32 row, u32 col) const;
    float viewProjectionMatrixRow(u32 row, u32 col) const;

    struct ProjectedPoint {
        float clipX = 0.f;
        float clipY = 0.f;
        float clipZ = 0.f;
        float clipW = 0.f;
        bool behindCamera = true;
    };

    /// Homogeneous clip-space projection. Requires `update()` first.
    ProjectedPoint projectWorldPoint(float worldX, float worldY, float worldZ) const;

    /// Normalised device coordinates in [-1, 1]. Returns false when behind the camera.
    bool worldToNdc(float worldX, float worldY, float worldZ, float& ndcX, float& ndcY,
                    float& ndcZ) const;

private:
    bool m_matricesValid = false;
    Frustum m_frustum{};
    float m_view[16] = {};
    float m_projection[16] = {};
    float m_viewProjection[16] = {};
};

} // namespace fuse
