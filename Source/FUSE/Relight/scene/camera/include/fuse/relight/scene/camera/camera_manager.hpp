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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_camera_manager.{h,cpp}@0867d3c (CameraManager::
// processCameraData, isCameraCutThisFrame, onFrameEnd) and src/dxvk/rtx_render/rtx_camera.{h,cpp}@0867d3c
// (CameraType, RtCamera::update's per-frame bookkeeping, isValid, isCameraCut, getPosition /
// getDirection), with the options of src/dxvk/rtx_render/rtx_options.h@0867d3c they read.
//
// Camera classification (plan §1.5 "Camera"): each committed draw is assigned a camera (Main, Sky,
// ViewModel, RenderToTexture, or Unknown when the draw carries no usable camera) and the first draw of
// each camera type in a frame updates that camera from the draw's VIEW / PROJECTION. Not ported: the
// free camera, camera shake, the camera sequence recorder, anti-culling frusta, near-plane override and
// Remix's own jitter (render-side, RL-4/5). Added (FUSE): game TAA jitter detection per camera.
#pragma once

#include <fuse/relight/scene/camera/projection.hpp>

#include <array>
#include <cstdint>

namespace fuse::relight::scene {

/// rtx_camera.h CameraType::Enum.
enum class CameraType : std::uint32_t {
    Main = 0,        ///< main camera
    ViewModel,       ///< view-model (first-person weapon) camera
    Portal0,         ///< camera of ray portal 0
    Portal1,         ///< camera of ray portal 1
    Sky,             ///< separate world / sky camera
    RenderToTexture, ///< camera of a raytraced render target
    Unknown,         ///< no camera (aliases Main for reads, never updated)
    Count,
};
const char* cameraTypeName(CameraType type);

/// rtx_options.h FusedWorldViewMode.
enum class FusedWorldViewMode : std::int32_t { None = 0, View, World };

inline constexpr std::uint32_t kInvalidFrameIndex = 0xffffffffu;

/// What processCameraData reads from the DrawCallState.
struct CameraDrawInput {
    Mat4 worldToView{};      ///< D3DTS_VIEW (row-major)
    Mat4 viewToProjection{}; ///< D3DTS_PROJECTION
    Mat4 objectToWorld{};    ///< the draw's world transform (identity for untrusted shader draws)
    Mat4 objectToView{};     ///< objectToWorld x worldToView (row-vector order)
    bool isSky = false;      ///< InstanceCategories::Sky
    bool isDrawingToRaytracedRenderTarget = false;
    float maxZ = 1.0f; ///< viewport MaxZ clamped to [0, 1]
    /// Viewport size, for the jitter in pixels (FUSE).
    std::uint32_t viewportWidth = 0, viewportHeight = 0;
};

/// One camera's state (the RtCamera fields camera classification maintains).
struct CameraState {
    CameraType type = CameraType::Main;
    std::uint32_t frameLastTouched = kInvalidFrameIndex;
    Mat4 worldToView{};
    Mat4 viewToProjection{};
    float fov = 0.f, aspectRatio = 0.f, nearPlane = 0.f, farPlane = 0.f;
    bool isLHS = false;
    bool isReverseZ = false;
    float shearX = 0.f, shearY = 0.f;
    /// inverse(worldToView) this and last update (double precision, as RtCamera's matrix cache).
    std::array<double, 16> viewToWorld{};
    std::array<double, 16> previousViewToWorld{};
    /// Game TAA jitter of this update (FUSE): the projection's sub-pixel offset and whether it looks like
    /// jitter (sub-pixel, and different from the previous update's offset).
    ProjectionJitter jitter;
    bool jitterDetected = false;

    /// RtCamera::isValid: updated in `frameId`.
    bool isValid(std::uint32_t frameId) const { return frameLastTouched == frameId; }
    /// RtCamera::getPosition (no free camera): viewToWorld translation.
    std::array<float, 3> position() const;
    /// RtCamera::getDirection: +Z (LHS) or -Z (RHS) of viewToWorld.
    std::array<float, 3> direction() const;
    /// RtCamera::isCameraCut: the camera moved more than rtx.uniqueObjectDistance since the last update.
    bool isCameraCut() const;
};

class CameraManager {
public:
    CameraManager();

    /// CameraManager::processCameraData: the camera type of a committed draw; updates that camera if it
    /// was not updated in `frameId` yet.
    CameraType processCameraData(const CameraDrawInput& input, std::uint32_t frameId);

    /// CameraManager::onFrameEnd.
    void onFrameEnd();

    /// accessCamera: Unknown reads the Main camera.
    const CameraState& getCamera(CameraType type) const;
    const CameraState& getMainCamera() const { return getCamera(CameraType::Main); }
    bool isCameraValid(CameraType type, std::uint32_t frameId) const { return getCamera(type).isValid(frameId); }
    CameraType getLastSetCameraType() const { return m_lastSetCameraType; }
    std::uint32_t getLastCameraCutFrameId() const { return m_lastCameraCutFrameId; }
    bool isCameraCutThisFrame(std::uint32_t frameId) const { return m_lastCameraCutFrameId == frameId; }

    /// Diagnostics (Remix logs these once): cameras rejected as invalid, FOV changes between frames.
    std::uint32_t rejectedCameras() const { return m_rejectedCameras; }
    std::uint32_t fovChanges() const { return m_fovChanges; }

private:
    CameraState& camera(CameraType type);
    /// RtCamera::update: false when already updated this frame, else whether it is a camera cut.
    bool updateCamera(CameraState& camera, std::uint32_t frameId, const CameraDrawInput& input,
                      const DecomposeProjectionParams& params);

    std::array<CameraState, static_cast<std::size_t>(CameraType::Count)> m_cameras;
    std::array<bool, static_cast<std::size_t>(CameraType::Count)> m_firstUpdate;
    CameraType m_lastSetCameraType = CameraType::Unknown;
    std::uint32_t m_lastCameraCutFrameId = kInvalidFrameIndex;
    std::uint32_t m_rejectedCameras = 0;
    std::uint32_t m_fovChanges = 0;
};

/// util_matrix.h isIdentityExact.
bool isIdentityExact(const Mat4& m);

/// Row-major 4x4 inverse in double precision (util_matrix inverse); the zero matrix when singular.
std::array<double, 16> inverseMatrix(const Mat4& m);

} // namespace fuse::relight::scene
