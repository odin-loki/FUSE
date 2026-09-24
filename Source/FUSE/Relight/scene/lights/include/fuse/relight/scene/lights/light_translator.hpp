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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (D3D9Rtx::processRenderState, DirtyLights
// branch), src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c (SceneManager::addLight, game-light
// path) and src/dxvk/rtx_render/rtx_light_manager.cpp@0867d3c (LightManager::addGameLight and the
// "off" and same-frame rules of addLight).
//
// The game lights of a frame. Remix re-sends the enabled D3D9 lights to the light manager whenever
// SetLight / LightEnable dirtied them, at the next draw that reaches processRenderState; the light
// manager drops "off" lights, filters by type (rtx.ignoreGame*Lights) and ignores a second add of the
// same light hash within a frame. Light replacement (mods) and the cross-frame light lifecycle
// (similarity matching, sleeping, garbage collection) belong to the replacement and render packages.
//
// Dirty: when the tap producer tracks light changes (tap::DrawState::lightsVersion, the DXVK hooks
// RL-1.1-24/25), the caller passes Remix's DirtyLights flag itself (processDraw with `dirty`). FUSE
// fallback for producers that report state only: "dirty" is a change of the enabled lights' D3D9 data
// since the last processed draw, plus the first processed draw of each frame.
#pragma once

#include <fuse/relight/scene/lights/legacy_light.hpp>

#include <cstdint>
#include <vector>

namespace fuse::relight::scene {

class LightTranslator {
public:
    /// processRenderState's light step for one draw: `lights` is the tap's light table (every slot ever
    /// set, with its enable bit). Returns the lights this draw added to the frame (empty when the lights
    /// were not dirty or every enabled light was already added this frame).
    std::vector<LightRecord> processDraw(const tap::Light* lights, std::uint32_t count);
    /// As above with Remix's DirtyLights flag given by the caller: re-sends the enabled lights when `dirty`,
    /// adds nothing otherwise (the content-comparison fallback is not used).
    std::vector<LightRecord> processDraw(const tap::Light* lights, std::uint32_t count, bool dirty);

    /// End of frame (Present): the next processed draw re-sends the lights.
    void endFrame();

    /// Lights added this frame, in add order (unique by hash).
    const std::vector<LightRecord>& frameLights() const { return m_frameLights; }
    /// Enabled D3D9 lights skipped this frame by rtx.ignoreGame*Lights, invalid type or "off" radiance.
    std::uint32_t frameRejected() const { return m_frameRejected; }

private:
    /// SceneManager::addLight for each enabled light: conversion, type filter, "off" and same-frame rules.
    std::vector<LightRecord> addLights(const std::vector<tap::Light>& enabled);

    std::vector<tap::Light> m_lastEnabled;
    bool m_haveLastEnabled = false;
    std::vector<LightRecord> m_frameLights;
    std::uint32_t m_frameRejected = 0;
};

/// LightManager::addGameLight's type filter: false when rtx.ignoreGame{Directional,Point,Spot}Lights
/// drops the light type.
bool acceptsGameLightType(std::uint32_t d3dLightType);

} // namespace fuse::relight::scene
