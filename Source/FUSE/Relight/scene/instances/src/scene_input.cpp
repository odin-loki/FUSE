/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_materials.cpp@0867d3c (LegacyMaterialData::computeIdentityHash).
// FUSE Relight RL-1.7: SceneDrawInput builders (see scene_input.hpp), subsurface classification.
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/hash/xxh.hpp>

#include <cstring>

namespace fuse::relight::scene::instances {

SubsurfaceKind classifySubsurface(const SubsurfaceInput& in) {
    // createSurfaceMaterial: thin opaque when the (scaled) measurement distance is positive, diffusion
    // profile when flagged; each only when its rtx.subsurface switch is on. Diffusion profile wins.
    const float measurementDistance = in.measurementDistance * SubsurfaceOptions::surfaceThicknessScale();
    const bool thinOpaque = SubsurfaceOptions::enableThinOpaque() && measurementDistance > 0.0f;
    const bool diffusion = SubsurfaceOptions::enableDiffusionProfile() && in.diffusionProfile;
    if (!thinOpaque && !diffusion) {
        return SubsurfaceKind::None;
    }
    return in.diffusionProfile ? SubsurfaceKind::DiffusionProfile : SubsurfaceKind::ThinOpaque;
}

Hash64 legacyMaterialIdentityHash(const LegacyMaterialRecord& m) {
    // Only the fields consumed by determineMaterialData(), mergeLegacyMaterial() and the instance surface
    // alpha / blend setup; D3DMATERIAL9 constants, colour-source intermediates, texture slots and
    // alphaTestEnabled are excluded. 88 bytes, no implicit padding, XXH3 (hashStructByMemory).
    struct LegacyMaterialIdentityHashData {
        std::uint64_t colorTextureHash0;
        std::uint64_t colorTextureHash1;
        std::uint64_t samplerHash0;
        std::uint64_t samplerHash1;
        std::uint32_t alphaTestCompareOp;
        std::uint32_t tFactor;
        std::uint32_t blendEnableBlending;
        std::uint32_t blendColorSrcFactor;
        std::uint32_t blendColorDstFactor;
        std::uint32_t blendColorBlendOp;
        std::uint32_t blendAlphaSrcFactor;
        std::uint32_t blendAlphaDstFactor;
        std::uint32_t blendAlphaBlendOp;
        std::uint32_t blendWriteMask;
        std::uint8_t alphaTestReferenceValue;
        std::uint8_t textureColorArg1Source;
        std::uint8_t textureColorArg2Source;
        std::uint8_t textureColorOperation;
        std::uint8_t textureAlphaArg1Source;
        std::uint8_t textureAlphaArg2Source;
        std::uint8_t textureAlphaOperation;
        std::uint8_t isTextureFactorBlend;
        std::uint8_t isVertexColorBakedLighting;
        std::uint8_t padding[7];
    };
    static_assert(sizeof(LegacyMaterialIdentityHashData) == 4 * 8 + 10 * 4 + 9 + 7, "no implicit padding");
    LegacyMaterialIdentityHashData data{};
    data.colorTextureHash0 = m.colorTextureHashes[0];
    data.colorTextureHash1 = m.colorTextureHashes[1];
    data.samplerHash0 = hash::kEmptyHash; // FUSE: no DxvkSampler yet
    data.samplerHash1 = hash::kEmptyHash;
    data.alphaTestCompareOp = m.alphaTestCompareOp;
    data.tFactor = m.tFactor;
    data.blendEnableBlending = m.blendMode.enableBlending ? 1u : 0u;
    data.blendColorSrcFactor = m.blendMode.colorSrcFactor;
    data.blendColorDstFactor = m.blendMode.colorDstFactor;
    data.blendColorBlendOp = m.blendMode.colorBlendOp;
    data.blendAlphaSrcFactor = m.blendMode.alphaSrcFactor;
    data.blendAlphaDstFactor = m.blendMode.alphaDstFactor;
    data.blendAlphaBlendOp = m.blendMode.alphaBlendOp;
    data.blendWriteMask = m.blendMode.writeMask;
    data.alphaTestReferenceValue = m.alphaTestReferenceValue;
    data.textureColorArg1Source = static_cast<std::uint8_t>(m.textureColorArg1Source);
    data.textureColorArg2Source = static_cast<std::uint8_t>(m.textureColorArg2Source);
    data.textureColorOperation = static_cast<std::uint8_t>(m.textureColorOperation);
    data.textureAlphaArg1Source = static_cast<std::uint8_t>(m.textureAlphaArg1Source);
    data.textureAlphaArg2Source = static_cast<std::uint8_t>(m.textureAlphaArg2Source);
    data.textureAlphaOperation = static_cast<std::uint8_t>(m.textureAlphaOperation);
    data.isTextureFactorBlend = m.isTextureFactorBlend ? 1u : 0u;
    data.isVertexColorBakedLighting = m.isVertexColorBakedLighting ? 1u : 0u;
    return hash::xxh3_64(&data, sizeof data);
}

SceneDrawInput sceneDrawInput(const TranslatedDraw& draw) {
    SceneDrawInput in;
    in.drawCallId = draw.classification.drawCallId;
    in.materialHash = draw.material.hash();
    in.materialIdentityHash = legacyMaterialIdentityHash(draw.material);
    in.objectToWorld = draw.transforms.objectToWorld;
    in.textureTransform = draw.transforms.textureTransform;
    in.texgenMode = draw.transforms.texgenMode;
    in.categories = draw.classification.categories;
    in.cameraType = draw.cameraType;
    in.isUsingRaytracedRenderTarget = draw.classification.isUsingRaytracedRenderTarget;
    return in;
}

AxisAlignedBoundingBox toBoundingBox(const capture::geometry::BoundingBox& box) {
    AxisAlignedBoundingBox out;
    out.minPos = {box.minPos[0], box.minPos[1], box.minPos[2]};
    out.maxPos = {box.maxPos[0], box.maxPos[1], box.maxPos[2]};
    return out;
}

SceneDrawInput sceneDrawInput(const TranslatedDraw& draw, const capture::geometry::CapturedDraw& geometry,
                              hash::HashRule assetRule) {
    SceneDrawInput in = sceneDrawInput(draw);
    if (geometry.hashes.valid()) {
        in.geometry = geometry.hashes.get();
        in.assetHash = geometry.assetHash(assetRule);
    }
    if (geometry.boundingBox.valid()) {
        in.boundingBox = toBoundingBox(geometry.boundingBox.get());
    }
    if (geometry.skinning.valid()) {
        const capture::geometry::SkinningData& skin = geometry.skinning.get();
        in.boneHash = skin.boneHash;
        in.numBones = skin.numBones;
    }
    return in;
}

} // namespace fuse::relight::scene::instances
