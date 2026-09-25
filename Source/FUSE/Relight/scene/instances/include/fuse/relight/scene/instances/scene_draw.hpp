/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_types.h@0867d3c (the DrawCallState fields the scene
// manager's instance tracking reads) and src/dxvk/rtx_render/rtx_materials.cpp@0867d3c
// (LegacyMaterialData::computeIdentityHash).
//
// SceneDrawInput: one committed draw as SceneManager::submitDrawState sees it, reduced to what instance
// tracking needs: the geometry hash components (RL-1.3), the object-space bounding box, the legacy
// material hash and identity hash (RL-1.5), skinning, the transforms, categories and camera type
// (RL-1.2 / RL-1.5), plus the render-material facts replacements would supply (material type, subsurface)
// and a point instancer's per-instance transforms. Builders from TranslatedDraw + CapturedDraw live in
// scene_input.hpp.
#pragma once

#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/scene/camera/camera_manager.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>
#include <fuse/relight/scene/instances/instance_math.hpp>
#include <fuse/relight/scene/translate/ff_translate.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fuse::relight::scene::instances {

using hash::Hash64;

/// MaterialDataType, as far as instance tracking distinguishes it.
enum class MaterialType : std::uint8_t { Opaque = 0, Translucent, RayPortal };

/// The subsurface facts of the render material (OpaqueMaterialData, replacement-driven; zero for legacy
/// materials).
struct SubsurfaceInput {
    float measurementDistance = 0.f;  ///< getSubsurfaceMeasurementDistance (> 0: thin opaque / foliage)
    bool diffusionProfile = false;    ///< getSubsurfaceDiffusionProfile (SSS)
};

/// createSurfaceMaterial's subsurface classification: RtSubsurfaceMaterial radiusScale < 0 (thin opaque,
/// the foliage single-scattering model) or > 0 (diffusion profile), or no subsurface extension.
enum class SubsurfaceKind : std::uint8_t { None = 0, ThinOpaque, DiffusionProfile };

/// Classifies `in` with rtx.subsurface.enableThinOpaque / enableDiffusionProfile / surfaceThicknessScale.
SubsurfaceKind classifySubsurface(const SubsurfaceInput& in);

struct SceneDrawInput {
    std::uint32_t drawCallId = 0;
    /// RasterGeometry::hashes (the 9 components; 0 = not computed).
    hash::GeometryHashes geometry;
    /// RasterGeometry::boundingBox (object space; invalid when not computed).
    AxisAlignedBoundingBox boundingBox;
    /// DrawCallState::getHash(rtx.geometryAssetHashRule): the mesh replacement key (surface bookkeeping).
    Hash64 assetHash = hash::kEmptyHash;
    /// LegacyMaterialData::getHash (colour texture 0) and computeIdentityHash.
    Hash64 materialHash = hash::kEmptyHash;
    Hash64 materialIdentityHash = hash::kEmptyHash;
    /// overrideMaterialData->getHash() (terrain baker, particles), 0 when none.
    Hash64 overrideMaterialHash = hash::kEmptyHash;
    /// SkinningData::boneHash / numBones.
    Hash64 boneHash = hash::kEmptyHash;
    std::uint32_t numBones = 0;
    /// DrawCallTransforms.
    Mat4f objectToWorld = identityMatrix();
    Mat4f textureTransform = identityMatrix();
    TexGenMode texgenMode = TexGenMode::None;
    CategoryFlags categories;
    CameraType cameraType = CameraType::Main;
    bool isUsingRaytracedRenderTarget = false;
    /// The render material (MaterialData) type and subsurface facts.
    MaterialType materialType = MaterialType::Opaque;
    SubsurfaceInput subsurface;
    /// DrawCallTransforms::instancesToObject: a point instancer's per-instance transforms (null otherwise).
    std::shared_ptr<const std::vector<Mat4f>> instancesToObject;
};

/// LegacyMaterialData::computeIdentityHash over a translated legacy material. FUSE has no DxvkSampler yet:
/// the two sampler hashes are 0.
Hash64 legacyMaterialIdentityHash(const LegacyMaterialRecord& material);

} // namespace fuse::relight::scene::instances
