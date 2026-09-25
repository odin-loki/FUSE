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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c, src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c
// and src/dxvk/rtx_render/rtx_light_manager.cpp@0867d3c. See light_translator.hpp.
#include <fuse/relight/scene/lights/light_options.hpp>
#include <fuse/relight/scene/lights/light_translator.hpp>

namespace fuse::relight::scene {

namespace {

bool sameLight(const tap::Light& a, const tap::Light& b) {
    // Field-wise (not memcmp: the struct has padding after the enable flag).
    auto c4 = [](const tap::Color4& x, const tap::Color4& y) {
        return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
    };
    auto v3 = [](const tap::Vec3& x, const tap::Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    return a.index == b.index && a.enabled == b.enabled && a.type == b.type && c4(a.diffuse, b.diffuse) &&
           c4(a.specular, b.specular) && c4(a.ambient, b.ambient) && v3(a.position, b.position) &&
           v3(a.direction, b.direction) && a.range == b.range && a.falloff == b.falloff &&
           a.attenuation0 == b.attenuation0 && a.attenuation1 == b.attenuation1 &&
           a.attenuation2 == b.attenuation2 && a.theta == b.theta && a.phi == b.phi;
}

} // namespace

bool acceptsGameLightType(std::uint32_t type) {
    switch (type) {
    case d3dlight::DIRECTIONAL: return !LightOptions::ignoreGameDirectionalLights();
    case d3dlight::POINT: return !LightOptions::ignoreGamePointLights();
    case d3dlight::SPOT: return !LightOptions::ignoreGameSpotLights();
    default: return true; // upstream asserts; tryCreate has already rejected invalid types
    }
}

std::vector<LightRecord> LightTranslator::processDraw(const tap::Light* lights, std::uint32_t count) {
    std::vector<tap::Light> enabled;
    for (std::uint32_t i = 0; lights && i < count; ++i) {
        if (lights[i].enabled) {
            enabled.push_back(lights[i]);
        }
    }
    bool dirty = !m_haveLastEnabled || enabled.size() != m_lastEnabled.size();
    for (std::size_t i = 0; !dirty && i < enabled.size(); ++i) {
        dirty = !sameLight(enabled[i], m_lastEnabled[i]);
    }
    if (!dirty) {
        return {};
    }
    m_lastEnabled = enabled;
    m_haveLastEnabled = true;
    return addLights(enabled);
}

std::vector<LightRecord> LightTranslator::processDraw(const tap::Light* lights, std::uint32_t count, bool dirty) {
    if (!dirty) {
        return {};
    }
    std::vector<tap::Light> enabled;
    for (std::uint32_t i = 0; lights && i < count; ++i) {
        if (lights[i].enabled) {
            enabled.push_back(lights[i]);
        }
    }
    // Keep the fallback's snapshot current, in case a producer mixes tracked and untracked draws.
    m_lastEnabled = enabled;
    m_haveLastEnabled = true;
    return addLights(enabled);
}

std::vector<LightRecord> LightTranslator::addLights(const std::vector<tap::Light>& enabled) {
    std::vector<LightRecord> added;
    for (const tap::Light& light : enabled) {
        // SceneManager::addLight: LightData::tryCreate (skip malformed lights) -> toRtLight.
        std::optional<LightRecord> record = convertLegacyLight(light);
        if (!record) {
            ++m_frameRejected;
            continue;
        }
        // LightManager::addGameLight: type filters.
        if (!acceptsGameLightType(light.type)) {
            ++m_frameRejected;
            continue;
        }
        // LightManager::addLight: "off" lights (including subtractive ones) are not added.
        if (record->isOff()) {
            ++m_frameRejected;
            continue;
        }
        // LightManager::addLight: an exact hash match touched this frame ignores the change.
        bool seen = false;
        for (const LightRecord& l : m_frameLights) {
            seen = seen || l.hash == record->hash;
        }
        if (seen) {
            continue;
        }
        m_frameLights.push_back(*record);
        added.push_back(*record);
    }
    return added;
}

void LightTranslator::endFrame() {
    m_haveLastEnabled = false;
    m_lastEnabled.clear();
    m_frameLights.clear();
    m_frameRejected = 0;
}

} // namespace fuse::relight::scene
