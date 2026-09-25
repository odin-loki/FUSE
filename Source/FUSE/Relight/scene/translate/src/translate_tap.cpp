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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (internalPrepareDraw) and
// src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c (submitDrawState fog discovery). See translate_tap.hpp.
#include <fuse/relight/scene/translate/translate_options.hpp>
#include <fuse/relight/scene/translate/translate_tap.hpp>

#include <fuse/relight/scene/classify/classify_options.hpp>

#include <algorithm>
#include <utility>

namespace fuse::relight::scene {

bool reachesMaterialTranslation(const DrawClassification& c) {
    switch (c.reason) {
    case ClassifyReason::RayTraced:
    case ClassifyReason::ColorTextureWithoutHash:
    case ClassifyReason::IgnoreTexture:
    case ClassifyReason::SkyInRaytracedTarget:
        return true;
    default:
        return false;
    }
}

void FogTracker::processDraw(const FogRecord& fog) {
    if (fog.mode == d3dff::FOG_NONE) {
        return;
    }
    const hash::Hash64 h = fog.hash();
    for (const FogRecord& seen : m_states) {
        if (seen.hash() == h) {
            return; // only act on fog states not seen this frame
        }
    }
    m_states.push_back(fog);
    // Render the first unreplaced fog.
    if (m_fog.mode == d3dff::FOG_NONE) {
        m_fog = fog;
    }
}

void FogTracker::endFrame() {
    // Reset the fog state to get it re-discovered on the next frame.
    m_fog = FogRecord();
    m_states.clear();
}

TranslateTap::TranslateTap(tap::IRelightTap* forward, DrawSink drawSink, FrameSink frameSink, bool applyDecisions)
    : m_drawSink(std::move(drawSink)),
      m_frameSink(std::move(frameSink)),
      m_classify(forward, [this](const ClassifiedDraw& d) { m_lastClassified = d; }, applyDecisions) {}

tap::DrawDecision TranslateTap::onDraw(const tap::DrawCall& call, const tap::DrawState& state) {
    const tap::DrawDecision decision = m_classify.onDraw(call, state);

    TranslatedDraw d;
    d.frame = m_lastClassified.frame;
    d.indexInFrame = m_lastClassified.indexInFrame;
    d.classification = m_lastClassified.result;
    const DrawClassification& c = d.classification;

    const D3DStateModel model = m_classify.tracker().buildModel(call, state);
    const FixedFunctionState ff = buildFixedFunctionState(state);
    // m_alphaSwizzleRTs & (1 << kRenderTargetIndex): DXVK's own mask when the producer has it.
    d.alphaSwizzle = state.hasAlphaSwizzleMask
                         ? (state.alphaSwizzleRenderTargets & 1u) != 0
                         : model.renderTarget0.valid() && renderTargetHasAlphaSwizzle(model.renderTarget0.format);

    if (reachesMaterialTranslation(c)) {
        d.translated = true;
        // Fetch all the legacy state (colour modes, alpha test, etc.) and the fog state.
        d.material = setLegacyMaterialState(model, ff, d.alphaSwizzle);
        d.fog = setFogState(model);
        // processRenderState: transforms, clip plane, lights, then textures.
        d.transforms = processTransforms(model, ff, ClassifyOptions::useVertexCapture());
        if (state.clipPlanesVersion != 0) {
            // if (DirtyClipPlanes) { clear; find one truly enabled clip plane } - else keep the last one.
            if (state.clipPlanesVersion != m_clipPlanesVersion) {
                m_clipPlanesVersion = state.clipPlanesVersion;
                m_enableClipPlane = d.transforms.enableClipPlane;
                m_clipPlane = d.transforms.clipPlane;
            }
            d.transforms.enableClipPlane = m_enableClipPlane;
            d.transforms.clipPlane = m_clipPlane;
        }
        if (state.lightsVersion != 0) {
            // if (DirtyLights) { clear; addLights(enabled lights) }
            const bool dirty = state.lightsVersion != m_lightsVersion;
            m_lightsVersion = state.lightsVersion;
            d.addedLights = m_lights.processDraw(state.lights, state.lightCount, dirty);
        } else {
            d.addedLights = m_lights.processDraw(state.lights, state.lightCount);
        }
        d.stencilEnabled = model.rs(d3d::RS_STENCILENABLE) != 0;
        if (c.reason != ClassifyReason::ColorTextureWithoutHash) {
            const ColorTextureSelection sel = selectColorTextures(model);
            for (std::size_t i = 0; i < kMaxSupportedTextures; ++i) {
                d.material.colorTextureSlots[i] = sel.slots[i];
                d.material.colorTextureHashes[i] =
                    sel.slots[i] >= 0 ? model.textures[static_cast<std::size_t>(sel.slots[i])].imageHash : hash::kEmptyHash;
            }
            const TextureFactorBlending tf = textureFactorBlending(model);
            setTextureStageState(model, ff, sel.firstStage, tf.useStageTextureFactorBlending,
                                 tf.useMultipleStageTextureFactorBlending, d.material, d.transforms);
            d.textureStageApplied = true;
            applyTerrainAsDecalModulate(sel.texture0Hash, d.material);
        }
        d.minZ = std::clamp(model.viewport.minZ, 0.0f, 1.0f);
        d.maxZ = std::clamp(model.viewport.maxZ, 0.0f, 1.0f);
        d.zWriteEnable = model.rs(d3d::RS_ZWRITEENABLE) != 0;
        d.zEnable = model.rs(d3d::RS_ZENABLE) == d3d::ZB_TRUE;
        if (TranslateOptions::fogIgnoreSky() && c.categories.test(InstanceCategories::Sky)) {
            d.fog.mode = d3dff::FOG_NONE;
        }
    } else if (c.reason == ClassifyReason::PositionT) {
        // FUSE: the raster remaster's translation of a pre-transformed draw Remix leaves to DXVK (see rasterOnly).
        d.rasterOnly = true;
        d.material = setLegacyMaterialState(model, ff, d.alphaSwizzle);
        d.fog = setFogState(model);
        d.stencilEnabled = model.rs(d3d::RS_STENCILENABLE) != 0;
        const ColorTextureSelection sel = selectColorTextures(model);
        for (std::size_t i = 0; i < kMaxSupportedTextures; ++i) {
            d.material.colorTextureSlots[i] = sel.slots[i];
            d.material.colorTextureHashes[i] =
                sel.slots[i] >= 0 ? model.textures[static_cast<std::size_t>(sel.slots[i])].imageHash : hash::kEmptyHash;
        }
        const TextureFactorBlending tf = textureFactorBlending(model);
        DrawTransforms unused;
        setTextureStageState(model, ff, sel.firstStage, tf.useStageTextureFactorBlending,
                             tf.useMultipleStageTextureFactorBlending, d.material, unused);
        d.transforms.textureTransform = unused.textureTransform;
        d.transforms.texgenMode = unused.texgenMode;
        d.textureStageApplied = true;
        d.minZ = std::clamp(model.viewport.minZ, 0.0f, 1.0f);
        d.maxZ = std::clamp(model.viewport.maxZ, 0.0f, 1.0f);
        d.zWriteEnable = model.rs(d3d::RS_ZWRITEENABLE) != 0;
        d.zEnable = model.rs(d3d::RS_ZENABLE) == d3d::ZB_TRUE;
    }
    if (d.translated || d.rasterOnly) {
        d.viewportX = model.viewport.x;
        d.viewportY = model.viewport.y;
        d.viewportWidth = model.viewport.width;
        d.viewportHeight = model.viewport.height;
    }

    if (c.committed()) {
        CameraDrawInput in;
        in.worldToView = d.transforms.worldToView;
        in.viewToProjection = d.transforms.viewToProjection;
        in.objectToWorld = d.transforms.objectToWorld;
        in.objectToView = d.transforms.objectToView;
        in.isSky = c.categories.test(InstanceCategories::Sky);
        in.isDrawingToRaytracedRenderTarget = c.isDrawingToRaytracedRenderTarget;
        in.maxZ = d.maxZ;
        in.viewportWidth = model.viewport.width;
        in.viewportHeight = model.viewport.height;
        d.cameraType = m_cameras.processCameraData(in, static_cast<std::uint32_t>(m_frame));
        m_fog.processDraw(d.fog);
    }

    if (m_drawSink) {
        m_drawSink(d);
    }
    return decision;
}

void TranslateTap::onPresent(const tap::FrameEvent& f) {
    if (m_frameSink) {
        TranslatedFrame frame;
        frame.frame = m_frame;
        frame.lights = m_lights.frameLights();
        frame.rejectedLights = m_lights.frameRejected();
        frame.fog = m_fog.frameFog();
        frame.fogStates = m_fog.frameStates();
        const std::uint32_t frameId = static_cast<std::uint32_t>(m_frame);
        for (std::uint32_t t = 0; t < static_cast<std::uint32_t>(CameraType::Unknown); ++t) {
            const CameraType type = static_cast<CameraType>(t);
            if (m_cameras.isCameraValid(type, frameId)) {
                frame.cameras.push_back(m_cameras.getCamera(type));
            }
        }
        frame.cameraCut = m_cameras.isCameraCutThisFrame(frameId);
        m_frameSink(frame);
    }
    m_classify.onPresent(f);
    m_lights.endFrame();
    m_cameras.onFrameEnd();
    m_fog.endFrame();
    ++m_frame;
}

} // namespace fuse::relight::scene
