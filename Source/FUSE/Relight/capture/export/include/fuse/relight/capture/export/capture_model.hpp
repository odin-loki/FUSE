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
// Ported from dxvk-remix src/lssusd/game_exporter_types.h@0867d3c (lss::Export, Mesh, Instance, Material,
// SphereLight, DistantLight, Camera, Skeleton, RenderingMetaData) and src/lssusd/remix_category_names.h@0867d3c
// (the remix_category:* attribute names).
//
// FUSE Relight RL-1.8: the capture, as GameCapturer (capture_builder.hpp) assembles it and the writers
// (usda_writer.hpp, poco_store.hpp, capture_writer.hpp) serialise it. Plain values, no USD types: matrices
// are row-major doubles in D3D / USD row-vector convention (translation in m[12..14]), which is the layout
// of both a D3DMATRIX and a GfMatrix4d, so game transforms go through unchanged.
//
// FUSE changes: one buffer set per mesh (no per-frame mesh time samples, see capture_builder.hpp); the
// captured albedo texture is part of the model (CaptureTexture: canonical mip-0 bytes, written as DDS) and
// the key set (captureKeys) is derived from it for the replacement DB (plan §4.1.5).
#pragma once

#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fuse::relight::capture::exporter {

using hash::Hash64;
using Mat4d = std::array<double, 16>;
using Vec3f = std::array<float, 3>;
using Vec2f = std::array<float, 2>;
using Vec4f = std::array<float, 4>;

Mat4d identity4d();

/// lss::SampledXform.
struct SampledXform {
    double time = 0.0;
    Mat4d xform = identity4d();
};

/// The captured albedo texture of a material (upstream: textures/<mat hash>.dds from the GPU image).
/// The texture is keyed even when its bytes are not available (a render target has no uploaded content;
/// upstream would read the GPU image back): the material hash is still a Remix key.
struct CaptureTexture {
    Hash64 hash = 0;              ///< Remix texture hash (XXH3, or XXH64 when obsoleteHash)
    bool obsoleteHash = false;    ///< rtx.useObsoleteHashOnTextureUpload ("remix.tex.obsolete")
    Hash64 descriptorHash = 0;    ///< render targets: D3D9_COMMON_TEXTURE_DESC hash ("remix.rtdesc"), else 0
    std::uint32_t d3dFormat = 0;  ///< D3DFORMAT (0 when unknown)
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> mip0; ///< canonical packed layout (plan §4.1.3), empty when not available
};

/// Sampler state in MDL terms (lss::Mdl::Filter / WrapMode values).
namespace mdl {
inline constexpr std::uint32_t kFilterNearest = 0, kFilterLinear = 1;
inline constexpr std::uint32_t kWrapClamp = 0, kWrapRepeat = 1, kWrapMirroredRepeat = 2, kWrapClip = 3;
} // namespace mdl

/// lss::Material (+ the legacy state the FUSE store keeps in its material_ext record).
struct CaptureMaterial {
    Hash64 hash = 0;          ///< LegacyMaterialData::getHash: the colour texture 0 hash
    Hash64 albedoTexture = 0; ///< texture key of textures/<hash>.dds (== hash)
    bool enableOpacity = false;
    std::uint32_t filter = mdl::kFilterLinear;
    std::uint32_t wrapU = mdl::kWrapRepeat, wrapV = mdl::kWrapRepeat;
    // Legacy fixed-function facts (FUSE store only).
    bool alphaTestEnabled = false;
    std::uint32_t alphaTestCompareOp = 7; ///< VkCompareOp (ALWAYS)
    std::uint32_t alphaTestReferenceValue = 0;
    bool blendEnabled = false;
    std::uint32_t tFactor = 0xffffffffu;
};

/// lss::RenderingMetaData (the _remix_metadata:* primvars of a captured instance).
struct RenderingMetaData {
    bool alphaTestEnabled = false;
    std::uint32_t alphaTestReferenceValue = 0;
    std::uint32_t alphaTestCompareOp = 7;
    bool alphaBlendEnabled = false;
    std::uint32_t srcColorBlendFactor = 1, dstColorBlendFactor = 0, colorBlendOp = 0;
    std::uint32_t srcAlphaBlendFactor = 1, dstAlphaBlendFactor = 0, alphaBlendOp = 0;
    std::uint32_t writeMask = 0xf;
    std::uint32_t textureColorArg1Source = 1, textureColorArg2Source = 0, textureColorOperation = 3;
    std::uint32_t textureAlphaArg1Source = 1, textureAlphaArg2Source = 0, textureAlphaOperation = 1;
    std::uint32_t tFactor = 0xffffffffu;
    bool isTextureFactorBlend = false;
    bool isVertexColorBakedLighting = true;
};

/// lss::Mesh with lss::MeshBuffers (one sample; indices form a triangle list).
struct CaptureMesh {
    Hash64 hash = 0;                   ///< DrawCallState::getHash(rtx.geometryAssetHashRule)
    hash::GeometryHashes components;   ///< customLayerData of the mesh layer
    Hash64 legacy0 = 0, legacy1 = 0;   ///< meshReplacementHashLegacy keys when the generation rule has them
    scene::CategoryFlags categories;
    bool isDoubleSided = false;
    bool isLhs = false;                ///< RtInstance::isFrontFaceFlipped (FUSE: always false, not tracked yet)
    Hash64 materialHash = 0;           ///< material of the first instance (upstream Mesh::matHash)
    std::vector<Vec3f> points;
    std::vector<Vec3f> normals;        ///< empty or one per point
    std::vector<Vec2f> texcoords;      ///< empty or one per point; (u, 1 - v) as upstream writes them
    std::vector<Vec4f> colors;         ///< empty or one per point (RGBA 0..1)
    std::vector<std::int32_t> indices; ///< triangle list
    std::uint32_t numBones = 0;
    std::uint32_t bonesPerVertex = 0;
    std::vector<float> jointWeights;          ///< bonesPerVertex per point
    std::vector<std::int32_t> jointIndices;   ///< bonesPerVertex per point
    std::array<float, 6> bounds{};            ///< min xyz, max xyz of the points
    /// Dynamic geometry (evalNewBufferAndCache): later samples of the points / normals that differ from the
    /// previous sample by more than rtx.captureMesh{Position,Normal}Delta, reduced like `points`. The first
    /// sample is `points` / `normals` at `firstTime`.
    double firstTime = 0.0;
    std::map<double, std::vector<Vec3f>> pointSamples;
    std::map<double, std::vector<Vec3f>> normalSamples;
    /// The reduction: the source vertex of each point, and the source vertex count.
    std::vector<std::int32_t> reducedFrom;
    std::uint32_t sourceVertexCount = 0;
};

/// lss::Skeleton (generated at export from the skinning data, upstream generateSkeleton).
struct CaptureSkeleton {
    std::vector<std::string> jointNames;
    std::vector<Mat4d> bindPose; ///< world (global) transforms
    std::vector<Mat4d> restPose; ///< local transforms
};

/// lss::SampledBoneXform.
struct SampledBoneXforms {
    double time = 0.0;
    std::vector<Mat4d> xforms;
};

/// lss::Instance.
struct CaptureInstance {
    std::uint64_t id = 0;     ///< RtInstance id (stable while the instance lives)
    Hash64 mesh = 0;
    Hash64 material = 0;      ///< 0: no material
    std::uint32_t meshInstNum = 0;
    bool isSky = false;
    double firstTime = 0.0, finalTime = 0.0;
    std::vector<SampledXform> xforms;
    std::vector<SampledBoneXforms> boneXforms;
    RenderingMetaData metadata;

    /// "inst_<MESH>_<n>" / "sky_<MESH>_<n>" (upstream: meshName + "_" + meshInstNum).
    std::string primName() const;
};

/// lss::SphereLight.
struct CaptureSphereLight {
    Hash64 hash = 0;
    Vec3f color{0.f, 0.f, 0.f};
    float radius = 0.f;
    float intensity = 0.f;
    bool shapingEnabled = false;
    float coneAngleDegrees = 180.f;
    float coneSoftness = 0.f;
    float focusExponent = 0.f;
    double firstTime = 0.0, finalTime = 0.0;
    std::vector<SampledXform> xforms;
};

/// lss::DistantLight.
struct CaptureDistantLight {
    Hash64 hash = 0;
    Vec3f color{0.f, 0.f, 0.f};
    float intensity = 0.f;
    float angleDegrees = 0.f;
    Vec3f direction{0.f, 0.f, 1.f};
    double firstTime = 0.0, finalTime = 0.0;
};

/// lss::Camera.
struct CaptureCamera {
    bool valid = false;
    float fov = NAN; ///< vertical, radians
    float aspectRatio = NAN;
    float nearPlane = NAN;
    float farPlane = NAN;
    bool isReverseZ = false;
    double firstTime = NAN, finalTime = NAN;
    std::vector<SampledXform> xforms; ///< camera to world (USD camera: looks down -Z)
    bool viewInv = false, projInv = false;
    bool viewLhs = false, projLhs = false;
    bool isLHS() const { return viewLhs != projLhs; }
};

/// lss::Export::Meta.
struct CaptureMeta {
    std::string gameId = "game";  ///< Remaster game id (store namespace, oaid seed)
    std::string windowTitle;      ///< lightspeed_game_name
    std::string exeName;          ///< lightspeed_exe_name
    std::string iconPath;         ///< lightspeed_game_icon (relative; empty: none)
    std::string geometryHashRule; ///< rtx.geometryAssetHashRuleString
    std::string stageName = "capture";
    double metersPerUnit = 1.0;   ///< rtx.sceneScale
    double timeCodesPerSecond = 24.0;
    double startTimeCode = 0.0;
    double endTimeCode = 0.0;
    std::size_t numFramesCaptured = 0;
    bool isZUp = false;
};

/// lss::Export.
struct CaptureData {
    CaptureMeta meta;
    std::map<Hash64, CaptureTexture> textures;
    std::map<Hash64, CaptureMaterial> materials;
    std::map<Hash64, CaptureMesh> meshes;
    std::map<Hash64, CaptureSkeleton> skeletons; ///< by mesh hash, skinned meshes only
    std::map<std::uint64_t, CaptureInstance> instances;
    std::map<Hash64, CaptureSphereLight> sphereLights;
    std::map<Hash64, CaptureDistantLight> distantLights;
    CaptureCamera camera;
    Mat4d globalXform = identity4d(); ///< the lights root correction (upstream Export::globalXform)
};

// ---- keys (plan §4.1.5) -----------------------------------------------------------------------------------

namespace key_algo {
inline constexpr const char* kGeomAsset = "remix.geom.asset";
inline constexpr const char* kGeomLegacy0 = "remix.geom.legacy0";
inline constexpr const char* kGeomLegacy1 = "remix.geom.legacy1";
inline constexpr const char* kTexture = "remix.tex";
inline constexpr const char* kTextureObsolete = "remix.tex.obsolete";
inline constexpr const char* kRtDescriptor = "remix.rtdesc";
inline constexpr const char* kLight = "remix.light";
inline constexpr const char* kCaptureSha256 = "fuse.capture.sha256";
} // namespace key_algo

/// One hash_key row: (algo, value) -> the original asset. Remix values are hashToString (16 upper-case hex
/// digits), sha256 values 64 lower-case hex digits. `ruleId` qualifies remix.geom.* keys (hashToString of
/// hash::hashRuleId(rule)).
struct CaptureKey {
    std::string algo;
    std::string value;
    std::string ruleId;
    std::string kind; ///< original_asset kind: texture, mesh, light

    friend bool operator==(const CaptureKey&, const CaptureKey&) = default;
    friend auto operator<=>(const CaptureKey&, const CaptureKey&) = default;
};

/// The canonical mesh stream the "fuse.capture.sha256" mesh key hashes (Remaster W0.2 is not in the tree
/// yet; this is the shim's definition): u32 point count, u32 index count, points (3 x f32 each), indices
/// (u32 each), all little-endian.
std::vector<std::uint8_t> canonicalMeshStream(const CaptureMesh& mesh);
/// sha256 of canonicalMeshStream.
std::string canonicalMeshSha256(const CaptureMesh& mesh);
/// sha256 of decodeRgba8(mip 0); empty when the format has no RGBA8 decode or no bytes were captured.
std::string canonicalTextureSha256(const CaptureTexture& texture);

/// Every hash_key row the capture yields, sorted and unique: remix.geom.asset (+ legacy keys) and
/// fuse.capture.sha256 per mesh; remix.tex (or remix.tex.obsolete) per material texture, plus remix.rtdesc for
/// a render target and fuse.capture.sha256 when its bytes were captured and decode; remix.light per light.
/// `assetRule` is the rule the mesh keys were computed with.
std::vector<CaptureKey> captureKeys(const CaptureData& capture, hash::HashRule assetRule);

// ---- row-vector 4x4 helpers (USD / D3D convention; defined in capture_builder.cpp) -------------------------

Mat4d multiply(const Mat4d& a, const Mat4d& b);
/// General inverse (identity for a singular matrix).
Mat4d inverse(const Mat4d& m);
/// GfRotation(rotateFrom, rotateTo) as a row-vector matrix with translation `t` (GfMatrix4d(rotation, t)).
Mat4d rotationBetween(const Vec3f& from, const Vec3f& to, const Vec3f& t);
/// sanitizeBoneXforms: bone transforms relative to the root as UsdSkel expects them.
std::vector<Mat4d> sanitizeBoneXforms(const std::vector<Mat4d>& xforms, const std::vector<Mat4d>& bindPose);

/// The remix_category:* attribute name of a category (remix_category_names.h).
const char* remixCategoryAttribute(scene::InstanceCategories category);

} // namespace fuse::relight::capture::exporter
