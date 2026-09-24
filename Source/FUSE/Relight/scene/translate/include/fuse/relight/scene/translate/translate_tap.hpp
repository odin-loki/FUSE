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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (the translation steps of internalPrepareDraw:
// setLegacyMaterialState, setFogState, processRenderState, the colour-texture material binding,
// minZ / maxZ / z / stencil state, fogIgnoreSky) and src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c
// (SceneManager::submitDrawState's fog discovery, SceneManager::addLight, processCameraData per
// committed draw).
//
// TranslateTap: an IRelightTap that runs RL-1.2's classifier (a ClassifyTap) and then, for every draw,
// the fixed-function translation of this package (material, texture stage, transforms, fog), the game
// lights (scene/lights) and camera classification (scene/camera), in Remix's order:
//   - draws that reach setLegacyMaterialState (classified RayTraced, or rejected only later in
//     processTextures / by the sky-in-raytraced-render-target rule) get material, fog, transforms and
//     the light step; texture-stage state follows the colour texture choice;
//   - committed draws (CommitToRayTracing) get a camera type and feed the frame's fog discovery.
// Frames end at Present: the sink receives a TranslatedFrame (lights, fog, cameras) first.
//
// Lights and the clip plane follow processRenderState's DirtyLights / DirtyClipPlanes flags when the tap
// producer tracks them (tap::DrawState::lightsVersion / clipPlanesVersion non-zero): a translated draw
// whose counter differs from the one last acted on re-sends the lights / recomputes the clip plane, and
// the clip plane of the other translated draws is the last one computed (Remix keeps it in the active draw
// call state). Producers that do not track them (counter 0) fall back to LightTranslator's state
// comparison and a clip plane computed per draw.
//
// FUSE additions (RL-4.2 raster remaster): every translated draw carries its viewport rectangle; pre-transformed draws
// Remix rasterizes (reason PositionT) get a raster-only translation (TranslatedDraw::rasterOnly); the legacy material
// carries the emissive colour source (LegacyMaterialRecord::emissiveSource). Nothing Remix computes changes. The render-target alpha swizzle is DXVK's mask when the
// producer reports it (hasAlphaSwizzleMask), else derived from render target 0's format.
#pragma once

#include <fuse/relight/scene/camera/camera_manager.hpp>
#include <fuse/relight/scene/classify/classify_tap.hpp>
#include <fuse/relight/scene/lights/light_translator.hpp>
#include <fuse/relight/scene/translate/ff_translate.hpp>

#include <array>
#include <functional>
#include <vector>

namespace fuse::relight::scene {

/// SceneManager's fog discovery: the distinct fog states of a frame (by FogState hash) and the frame
/// fog (the first one seen; fog replacement materials are the replacement package's).
class FogTracker {
public:
    void processDraw(const FogRecord& fog);
    void endFrame();
    const FogRecord& frameFog() const { return m_fog; }
    const std::vector<FogRecord>& frameStates() const { return m_states; }

private:
    FogRecord m_fog;
    std::vector<FogRecord> m_states;
};

/// One draw after classification and translation.
struct TranslatedDraw {
    std::uint64_t frame = 0;
    std::uint32_t indexInFrame = 0;
    DrawClassification classification;
    /// Reached setLegacyMaterialState / setFogState / processRenderState (material, fog, transforms and
    /// the light step are valid).
    bool translated = false;
    /// processTextures reached setTextureStageState (texture-stage fields of the material are valid).
    bool textureStageApplied = false;
    LegacyMaterialRecord material;
    FogRecord fog;
    DrawTransforms transforms;
    /// Lights this draw added to the frame (processRenderState's light step).
    std::vector<LightRecord> addedLights;
    /// processCameraData result (Unknown for draws that are not committed).
    CameraType cameraType = CameraType::Unknown;
    float minZ = 0.f, maxZ = 1.f; ///< viewport, clamped to [0, 1]
    /// FUSE: the D3D viewport rectangle (D3DVIEWPORT9 X, Y, Width, Height) of translated and raster-only draws
    /// (Remix keeps only MinZ / MaxZ; the raster remaster draws into this rectangle).
    std::uint32_t viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
    /// FUSE: a pre-transformed (POSITIONT) draw Remix rasterizes (ClassifyReason::PositionT: not UI, not committed)
    /// that the raster remaster renders: material, texture stage, fog, depth state and viewport are translated as
    /// for a translated draw; of the transforms only the texture transform / texgen mode, no light step, no camera,
    /// no fog discovery. `translated` stays false.
    bool rasterOnly = false;
    bool zWriteEnable = false, zEnable = false, stencilEnabled = false;
    bool alphaSwizzle = false; ///< render target 0 reads alpha as one
};

/// The frame summary delivered at Present.
struct TranslatedFrame {
    std::uint64_t frame = 0;
    std::vector<LightRecord> lights;
    std::uint32_t rejectedLights = 0;
    FogRecord fog;
    std::vector<FogRecord> fogStates;
    /// Cameras updated this frame (CameraManager::isCameraValid), in CameraType order.
    std::vector<CameraState> cameras;
    bool cameraCut = false;
};

class TranslateTap final : public tap::IRelightTap {
public:
    using DrawSink = std::function<void(const TranslatedDraw&)>;
    using FrameSink = std::function<void(const TranslatedFrame&)>;

    /// `forward`: optional tap receiving every event first (not owned). `applyDecisions`: onDraw returns the
    /// classifier's decision (see ClassifyTap).
    TranslateTap(tap::IRelightTap* forward, DrawSink drawSink, FrameSink frameSink, bool applyDecisions = false);

    ClassifyTap& classifyTap() { return m_classify; }
    D3DStateTracker& tracker() { return m_classify.tracker(); }
    CameraManager& cameras() { return m_cameras; }
    LightTranslator& lights() { return m_lights; }

    void onDeviceCreate(const tap::DeviceEvent& e) override { m_classify.onDeviceCreate(e); }
    void onDeviceReset(const tap::DeviceEvent& e) override { m_classify.onDeviceReset(e); }
    void onDeviceDestroy() override { m_classify.onDeviceDestroy(); }
    void onTextureCreate(const tap::TextureDesc& d) override { m_classify.onTextureCreate(d); }
    void onTextureUpload(const tap::TextureUpload& u) override { m_classify.onTextureUpload(u); }
    void onTextureCopy(const tap::TextureCopy& c) override { m_classify.onTextureCopy(c); }
    void onTextureWriteLock(const tap::TextureWriteLock& l) override { m_classify.onTextureWriteLock(l); }
    void onImageDestroy(const tap::ImageDestroy& d) override { m_classify.onImageDestroy(d); }
    void onBufferCreate(const tap::BufferDesc& d) override { m_classify.onBufferCreate(d); }
    void onBufferWrite(const tap::BufferWrite& w) override { m_classify.onBufferWrite(w); }
    void onBufferDestroy(tap::ResourceId id) override { m_classify.onBufferDestroy(id); }
    tap::DrawDecision onDraw(const tap::DrawCall& call, const tap::DrawState& state) override;
    bool substituteVertexShader(const tap::ShaderModule& m, std::vector<std::uint32_t>& replacement) override {
        return m_classify.substituteVertexShader(m, replacement);
    }
    void onQueryBegin(const tap::QueryEvent& q) override { m_classify.onQueryBegin(q); }
    void onQueryEnd(const tap::QueryEvent& q) override { m_classify.onQueryEnd(q); }
    void onClear(const tap::ClearEvent& c) override { m_classify.onClear(c); }
    void onSetRenderTarget(const tap::SetRenderTargetEvent& e) override { m_classify.onSetRenderTarget(e); }
    void onInjectPoint(const tap::FrameEvent& f) override { m_classify.onInjectPoint(f); }
    void onPresent(const tap::FrameEvent& f) override;

private:
    DrawSink m_drawSink;
    FrameSink m_frameSink;
    ClassifiedDraw m_lastClassified;
    ClassifyTap m_classify;
    LightTranslator m_lights;
    CameraManager m_cameras;
    FogTracker m_fog;
    std::uint64_t m_frame = 0;
    // The DirtyLights / DirtyClipPlanes emulation: the counter values last acted on (0: none yet).
    std::uint32_t m_lightsVersion = 0;
    std::uint32_t m_clipPlanesVersion = 0;
    bool m_enableClipPlane = false;
    std::array<float, 4> m_clipPlane{0.f, 0.f, 0.f, 0.f};
};

/// The classify reasons after which Remix has already run setLegacyMaterialState / processRenderState.
bool reachesMaterialTranslation(const DrawClassification& c);

} // namespace fuse::relight::scene
