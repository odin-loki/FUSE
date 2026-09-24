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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_options.cpp@0867d3c (the deprecated decal list
// migration) and src/dxvk/rtx_render/rtx_terrain_baker.cpp@0867d3c (needsTerrainBaking).
#include <fuse/relight/scene/classify/classify_options.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <fuse/relight/options/hash_set_layer.hpp>
#include <fuse/relight/options/option_layer.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cstdio>
#include <variant>

namespace fuse::relight::scene {

namespace {

/// Remix migrateHashSet: union-merge the source layer's hash set into the destination layer.
bool migrateHashSet(const options::OptionValue& src, options::OptionValue& dst, bool /*destHasExistingValue*/) {
    const auto* source = std::get_if<options::HashSetLayer>(&src);
    auto* dest = std::get_if<options::HashSetLayer>(&dst);
    if (!source || source->empty() || !dest) {
        return false;
    }
    dest->mergeFrom(*source);
    return true;
}

void migrateDecalList(options::Option<options::HashSet>& deprecated, const char* name) {
    if (deprecated.migrateValuesTo(&ClassifyOptions::decalTextures, migrateHashSet)) {
        deprecated.clearFromStrongerLayers(options::OptionLayer::getDefaultLayer());
        std::fprintf(stderr,
                     "fuse-relight: [Deprecated Config] %s has been deprecated, we have moved all your textures from this "
                     "list to rtx.decalTextures, no further action is required from you. Please re-save your rtx config to "
                     "get rid of this message.\n",
                     name);
    }
}

// Odr-use every option so each is emitted (and registered at start-up) by this translation unit,
// even the ones no classification rule reads yet: rtx.conf keys must resolve either way.
using O = ClassifyOptions;
[[maybe_unused]] const options::OptionBase* const kRegisteredOptions[] = {
    &O::orthographicIsUI, &O::preTransformedVerticesIsUI, &O::allowCubemaps, &O::useVertexCapture,
    &O::enableRaytracing, &O::skipDrawCallsPostRTXInjection, &O::enableAlphaBlend, &O::enableAlphaTest,
    &O::drawCallRange, &O::ignoreAllVertexColorBakedLighting, &O::skyDrawcallIdThreshold, &O::skyMinZThreshold,
    &O::skyAutoDetect, &O::skyAutoDetectUniqueCameraDistance, &O::terrainAsDecalsEnabledIfNoBaker,
    &O::raytracedRenderTargetTextures, &O::lightmapTextures, &O::skyBoxTextures, &O::skyBoxGeometries,
    &O::ignoreTextures, &O::ignoreLights, &O::uiTextures, &O::worldSpaceUiTextures,
    &O::worldSpaceUiBackgroundTextures, &O::hideInstanceTextures, &O::playerModelTextures,
    &O::playerModelBodyTextures, &O::particleTextures, &O::hairCardTextures, &O::beamTextures, &O::decalTextures,
    &O::dynamicDecalTextures, &O::singleOffsetDecalTextures, &O::nonOffsetDecalTextures, &O::terrainTextures,
    &O::opacityMicromapIgnoreTextures, &O::animatedWaterTextures, &O::ignoreBakedLightingTextures,
    &O::ignoreAlphaOnTextures, &O::antiCullingTextures, &O::motionBlurMaskOutTextures, &O::particleEmitterTextures,
    &O::smoothNormalsTextures, &O::RaytracedRenderTarget::enable};

} // namespace

void ClassifyOptions::onDynamicDecalTexturesChanged(void*) {
    migrateDecalList(dynamicDecalTextures, "rtx.dynamicDecalTextures");
}
void ClassifyOptions::onSingleOffsetDecalTexturesChanged(void*) {
    migrateDecalList(singleOffsetDecalTextures, "rtx.singleOffsetDecalTextures");
}
void ClassifyOptions::onNonOffsetDecalTexturesChanged(void*) {
    migrateDecalList(nonOffsetDecalTextures, "rtx.nonOffsetDecalTextures");
}

bool ClassifyOptions::terrainBakerEnableBaking() {
    if (const options::OptionBase* o = options::OptionManager::findOption("rtx.terrainBaker.enableBaking")) {
        const options::OptionValue v = o->getResolvedValue();
        if (const bool* b = std::get_if<bool>(&v)) {
            return *b;
        }
    }
    return true; // Remix TerrainBaker::enableBaking default
}

bool ClassifyOptions::needsTerrainBaking() {
    return terrainBakerEnableBaking() && !terrainTextures().empty();
}

// ---- instance_categories.hpp ---------------------------------------------------------------------------

const char* instanceCategoryName(InstanceCategories c) {
    static const char* const kNames[kInstanceCategoryCount] = {
        "WorldUI",           "WorldMatte",        "Sky",
        "Ignore",            "IgnoreLights",      "IgnoreAntiCulling",
        "IgnoreMotionBlur",  "IgnoreOpacityMicromap", "IgnoreAlphaChannel",
        "Hidden",            "Particle",          "Beam",
        "DecalStatic",       "DecalDynamic",      "DecalSingleOffset",
        "DecalNoOffset",     "AlphaBlendToCutout", "Terrain",
        "AnimatedWater",     "ThirdPersonPlayerModel", "ThirdPersonPlayerBody",
        "IgnoreBakedLighting", "ParticleEmitter", "SmoothNormals",
        "HairCards"};
    const auto i = static_cast<std::uint32_t>(c);
    return i < kInstanceCategoryCount ? kNames[i] : "?";
}

std::string CategoryFlags::toString() const {
    std::string s;
    for (std::uint32_t i = 0; i < kInstanceCategoryCount; ++i) {
        if (m_raw & (1u << i)) {
            if (!s.empty()) {
                s += '|';
            }
            s += instanceCategoryName(static_cast<InstanceCategories>(i));
        }
    }
    return s;
}

} // namespace fuse::relight::scene
