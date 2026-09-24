/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_camera_manager.cpp@0867d3c and
// src/dxvk/rtx_render/rtx_camera.cpp@0867d3c. See camera_manager.hpp.
#include <fuse/relight/scene/camera/camera_manager.hpp>
#include <fuse/relight/scene/camera/camera_options.hpp>

#include <fuse/relight/options/option_manager.hpp>

#include <cmath>
#include <variant>

namespace fuse::relight::scene {

namespace {

constexpr float kFovToleranceRadians = 0.001f;

bool isFovValid(float fov) { return fov >= kFovToleranceRadians; }
bool areFovsClose(float fov, const CameraState& camera) { return std::abs(fov - camera.fov) < kFovToleranceRadians; }

bool equalExact(const Mat4& a, const Mat4& b) {
    for (std::size_t i = 0; i < 16; ++i) {
        if (!(a[i] == b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* cameraTypeName(CameraType type) {
    switch (type) {
    case CameraType::Main: return "Main";
    case CameraType::ViewModel: return "ViewModel";
    case CameraType::Portal0: return "Portal0";
    case CameraType::Portal1: return "Portal1";
    case CameraType::Sky: return "Sky";
    case CameraType::RenderToTexture: return "RenderToTexture";
    case CameraType::Unknown: return "Unknown";
    case CameraType::Count: break;
    }
    return "?";
}

float CameraOptions::uniqueObjectDistance() {
    if (const options::OptionBase* o = options::OptionManager::findOption("rtx.uniqueObjectDistance")) {
        const options::OptionValue v = o->getResolvedValue();
        if (const float* f = std::get_if<float>(&v)) {
            return *f;
        }
    }
    return 300.f; // RtxOptions::uniqueObjectDistance default
}

bool isIdentityExact(const Mat4& m) {
    static const Mat4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return equalExact(m, kIdentity);
}

std::array<double, 16> inverseMatrix(const Mat4& mf) {
    double m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = mf[static_cast<std::size_t>(i)];
    }
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    std::array<double, 16> out{};
    if (det == 0.0) {
        return out;
    }
    const double invDet = 1.0 / det;
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = inv[i] * invDet;
    }
    return out;
}

std::array<float, 3> CameraState::position() const {
    return {static_cast<float>(viewToWorld[12]), static_cast<float>(viewToWorld[13]), static_cast<float>(viewToWorld[14])};
}

std::array<float, 3> CameraState::direction() const {
    const float s = isLHS ? 1.0f : -1.0f;
    return {s * static_cast<float>(viewToWorld[8]), s * static_cast<float>(viewToWorld[9]),
            s * static_cast<float>(viewToWorld[10])};
}

bool CameraState::isCameraCut() const {
    double lengthSqr = 0.0;
    for (std::size_t i = 12; i < 16; ++i) {
        const double d = viewToWorld[i] - previousViewToWorld[i];
        lengthSqr += d * d;
    }
    const float distance = CameraOptions::uniqueObjectDistance();
    return lengthSqr > distance * distance; // RtxOptions::getUniqueObjectDistanceSqr()
}

CameraManager::CameraManager() {
    for (std::size_t i = 0; i < m_cameras.size(); ++i) {
        m_cameras[i].type = static_cast<CameraType>(i);
        m_firstUpdate[i] = true;
    }
}

CameraState& CameraManager::camera(CameraType type) {
    // Unknown aliases the Main camera object (reads only; Unknown is never updated).
    if (type == CameraType::Unknown || type == CameraType::Count) {
        return m_cameras[static_cast<std::size_t>(CameraType::Main)];
    }
    return m_cameras[static_cast<std::size_t>(type)];
}

const CameraState& CameraManager::getCamera(CameraType type) const {
    if (type == CameraType::Unknown || type == CameraType::Count) {
        return m_cameras[static_cast<std::size_t>(CameraType::Main)];
    }
    return m_cameras[static_cast<std::size_t>(type)];
}

void CameraManager::onFrameEnd() { m_lastSetCameraType = CameraType::Unknown; }

bool CameraManager::updateCamera(CameraState& c, std::uint32_t frameId, const CameraDrawInput& input,
                                 const DecomposeProjectionParams& params) {
    if (c.frameLastTouched == frameId) {
        return false;
    }
    c.worldToView = input.worldToView;
    c.viewToProjection = input.viewToProjection;
    c.fov = params.fov;
    c.aspectRatio = params.aspectRatio;
    c.nearPlane = params.nearPlane;
    c.farPlane = params.farPlane;
    c.isLHS = params.isLHS;
    c.isReverseZ = params.isReverseZ;
    c.shearX = params.shearX;
    c.shearY = params.shearY;

    // World / view matrix data (rtx.camera.freeCameraViewRelative default: the game's view).
    c.previousViewToWorld = c.viewToWorld;
    c.viewToWorld = inverseMatrix(input.worldToView);

    // FUSE: game TAA jitter.
    const ProjectionJitter previousJitter = c.jitter;
    const bool hadUpdate = c.frameLastTouched != kInvalidFrameIndex;
    c.jitter = projectionJitter(input.viewToProjection, input.viewportWidth, input.viewportHeight);
    c.jitterDetected = c.jitter.subPixel && hadUpdate &&
                       (c.jitter.ndcX != previousJitter.ndcX || c.jitter.ndcY != previousJitter.ndcY);

    c.frameLastTouched = frameId;

    // For our first update, initialize both previous and current to the same value.
    const std::size_t index = static_cast<std::size_t>(c.type);
    if (m_firstUpdate[index]) {
        c.previousViewToWorld = c.viewToWorld;
        m_firstUpdate[index] = false;
    }
    return c.isCameraCut();
}

CameraType CameraManager::processCameraData(const CameraDrawInput& input, std::uint32_t frameId) {
    const CameraType noCamera = input.isSky ? CameraType::Sky : CameraType::Unknown;

    // If there's no real camera data here - bail.
    if (isIdentityExact(input.viewToProjection)) {
        return noCamera;
    }

    switch (CameraOptions::fusedWorldViewMode()) {
    case FusedWorldViewMode::None:
        if (equalExact(input.objectToView, input.objectToWorld) && !isIdentityExact(input.objectToView)) {
            return noCamera;
        }
        break;
    case FusedWorldViewMode::View:  // Remix warns when World is not identity
    case FusedWorldViewMode::World: // Remix warns when View is not identity
        break;
    }

    const DecomposeProjectionParams params = decomposeProjection(input.viewToProjection);

    // Filter invalid cameras, extreme shearing.
    if (std::abs(params.shearX) > 0.01f || !isFovValid(params.fov)) {
        ++m_rejectedCameras;
        return noCamera;
    }

    auto isViewModel = [this, frameId](float fov, float maxZ) {
        if (CameraOptions::enable()) {
            // Note: max Z check is the top-priority.
            if (maxZ <= CameraOptions::maxZThreshold()) {
                return true;
            }
            // FOV different from the Main camera => assume that it's a view model one.
            const CameraState& main = getCamera(CameraType::Main);
            if (main.isValid(frameId) && !areFovsClose(fov, main)) {
                return true;
            }
        }
        return false;
    };

    CameraType cameraType = CameraType::Main;
    if (input.isDrawingToRaytracedRenderTarget) {
        cameraType = CameraType::RenderToTexture;
    } else if (input.isSky) {
        cameraType = CameraType::Sky;
    } else if (isViewModel(params.fov, input.maxZ)) {
        cameraType = CameraType::ViewModel;
    }

    // Check FOV consistency across frames.
    if (frameId > 0) {
        const CameraState& c = getCamera(cameraType);
        if (c.isValid(frameId - 1) && !areFovsClose(params.fov, c)) {
            ++m_fovChanges;
        }
    }

    const bool isCameraCut = updateCamera(camera(cameraType), frameId, input, params);

    // Register camera cut when there are significant interruptions to the view (level change, menu).
    if (isCameraCut && cameraType == CameraType::Main) {
        m_lastCameraCutFrameId = frameId;
    }
    m_lastSetCameraType = cameraType;
    return cameraType;
}

} // namespace fuse::relight::scene
