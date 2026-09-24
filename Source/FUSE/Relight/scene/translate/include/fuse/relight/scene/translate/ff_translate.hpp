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
// Ported from dxvk-remix src/d3d9/d3d9_rtx_utils.cpp@0867d3c (setLegacyMaterialState,
// setTextureStageState, setFogState, convertTextureOp / convertColorSource / convertTextureArg),
// src/d3d9/d3d9_rtx.cpp@0867d3c (processRenderState transforms and clip plane, the texture-factor
// blending part of processTextures<FixedFunction>, the terrain-as-decal modulate fix-up),
// src/dxvk/rtx_render/rtx_materials.h@0867d3c (LegacyMaterialData fields, defaults and hash) and
// src/dxvk/rtx_render/rtx_types.h@0867d3c (DrawCallTransforms, FogState). The D3D9 -> Vulkan enum
// decoding (DecodeCompareOp / DecodeBlendFactor / DecodeBlendOp / FixupBlendState / DecodeD3DCOLOR)
// follows DXVK's d3d9_util (zlib) as the Remix fork uses it.
//
// Fixed-function state translation (plan §1.5 "Fixed-function -> material"): the per-draw material,
// texture-stage, transform and fog state Remix derives from the D3D9 state. Pure functions of a
// D3DStateModel (RL-1.2) plus the fixed-function inputs the model does not carry (FixedFunctionState).
// Vulkan enum values are plain numbers (VkCompareOp, VkBlendFactor, VkBlendOp), so no Vulkan header.
#pragma once

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/scene/classify/d3d_state_model.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <array>
#include <cstdint>

namespace fuse::relight::scene {

using Mat4f = std::array<float, 16>;

// ---- D3D9 / Vulkan values ------------------------------------------------------------------------------
namespace d3dff {
inline constexpr std::uint32_t RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_SRCBLEND = 19, RS_DESTBLEND = 20,
                               RS_FOGENABLE = 28, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36,
                               RS_FOGEND = 37, RS_FOGDENSITY = 38, RS_TEXTUREFACTOR = 60, RS_LIGHTING = 137,
                               RS_FOGVERTEXMODE = 140, RS_COLORVERTEX = 141, RS_DIFFUSEMATERIALSOURCE = 145,
                               RS_SPECULARMATERIALSOURCE = 146, RS_CLIPPLANEENABLE = 152, RS_BLENDOP = 171,
                               RS_SEPARATEALPHABLENDENABLE = 206, RS_SRCBLENDALPHA = 207, RS_DESTBLENDALPHA = 208,
                               RS_BLENDOPALPHA = 209;
inline constexpr std::uint32_t MCS_MATERIAL = 0, MCS_COLOR1 = 1, MCS_COLOR2 = 2;
inline constexpr std::uint32_t TA_SPECULAR = 4;
inline constexpr std::uint32_t TOP_MODULATE2X = 5, TOP_MODULATE4X = 6, TOP_ADD = 7;
inline constexpr std::uint32_t TSS_TEXTURETRANSFORMFLAGS = 24;
inline constexpr std::uint32_t TTFF_DISABLE = 0, TTFF_PROJECTED = 256;
inline constexpr std::uint32_t TCI_PASSTHRU = 0, TCI_CAMERASPACENORMAL = 0x10000, TCI_CAMERASPACEPOSITION = 0x20000,
                               TCI_CAMERASPACEREFLECTIONVECTOR = 0x30000, TCI_SPHEREMAP = 0x40000;
inline constexpr std::uint32_t FOG_NONE = 0, FOG_EXP = 1, FOG_EXP2 = 2, FOG_LINEAR = 3;
inline constexpr std::uint32_t BLEND_ZERO = 1, BLEND_ONE = 2, BLEND_SRCCOLOR = 3, BLEND_INVSRCCOLOR = 4,
                               BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, BLEND_DESTALPHA = 7, BLEND_INVDESTALPHA = 8,
                               BLEND_DESTCOLOR = 9, BLEND_INVDESTCOLOR = 10, BLEND_SRCALPHASAT = 11,
                               BLEND_BOTHSRCALPHA = 12, BLEND_BOTHINVSRCALPHA = 13, BLEND_BLENDFACTOR = 14,
                               BLEND_INVBLENDFACTOR = 15, BLEND_SRCCOLOR2 = 16, BLEND_INVSRCCOLOR2 = 17;
inline constexpr std::uint32_t BLENDOP_ADD = 1, BLENDOP_SUBTRACT = 2, BLENDOP_REVSUBTRACT = 3, BLENDOP_MIN = 4,
                               BLENDOP_MAX = 5;
inline constexpr std::uint32_t CMP_NEVER = 1, CMP_LESS = 2, CMP_EQUAL = 3, CMP_LESSEQUAL = 4, CMP_GREATER = 5,
                               CMP_NOTEQUAL = 6, CMP_GREATEREQUAL = 7, CMP_ALWAYS = 8;
inline constexpr std::uint32_t DECLUSAGE_COLOR = 10;
inline constexpr std::uint32_t kMaxClipPlanes = 6;
} // namespace d3dff

namespace vk {
inline constexpr std::uint32_t COMPARE_OP_NEVER = 0, COMPARE_OP_LESS = 1, COMPARE_OP_EQUAL = 2,
                               COMPARE_OP_LESS_OR_EQUAL = 3, COMPARE_OP_GREATER = 4, COMPARE_OP_NOT_EQUAL = 5,
                               COMPARE_OP_GREATER_OR_EQUAL = 6, COMPARE_OP_ALWAYS = 7;
inline constexpr std::uint32_t BLEND_FACTOR_ZERO = 0, BLEND_FACTOR_ONE = 1, BLEND_FACTOR_SRC_COLOR = 2,
                               BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 3, BLEND_FACTOR_DST_COLOR = 4,
                               BLEND_FACTOR_ONE_MINUS_DST_COLOR = 5, BLEND_FACTOR_SRC_ALPHA = 6,
                               BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7, BLEND_FACTOR_DST_ALPHA = 8,
                               BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 9, BLEND_FACTOR_CONSTANT_COLOR = 10,
                               BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR = 11, BLEND_FACTOR_CONSTANT_ALPHA = 12,
                               BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA = 13, BLEND_FACTOR_SRC_ALPHA_SATURATE = 14,
                               BLEND_FACTOR_SRC1_COLOR = 15, BLEND_FACTOR_ONE_MINUS_SRC1_COLOR = 16;
inline constexpr std::uint32_t BLEND_OP_ADD = 0, BLEND_OP_SUBTRACT = 1, BLEND_OP_REVERSE_SUBTRACT = 2,
                               BLEND_OP_MIN = 3, BLEND_OP_MAX = 4;
} // namespace vk

/// DXVK DecodeCompareOp (D3DCMPFUNC -> VkCompareOp; unknown values -> NEVER).
std::uint32_t decodeCompareOp(std::uint32_t d3dCmpFunc);
/// DXVK DecodeBlendFactor (D3DBLEND -> VkBlendFactor; unknown values -> ZERO).
std::uint32_t decodeBlendFactor(std::uint32_t d3dBlend, bool isAlpha);
/// DXVK DecodeBlendOp (D3DBLENDOP -> VkBlendOp; unknown values -> ADD).
std::uint32_t decodeBlendOp(std::uint32_t d3dBlendOp);
/// DXVK DecodeD3DCOLOR: ARGB -> {r, g, b, a} in [0, 1].
std::array<float, 4> decodeD3DColor(std::uint32_t argb);
/// DXVK 3.1 render-target alpha swizzle (m_alphaSwizzleRTs): formats whose image view maps alpha to ONE
/// (X8R8G8B8, X1R5G5B5, X8B8G8R8, G16R16, L8, V8U8, X8L8V8U8, V16U16, W11V11U10, L16, R16F,
/// G16R16F, R32F, G32R32F, ATI1, ATI2, DF24, DF16).
bool renderTargetHasAlphaSwizzle(std::uint32_t d3dFormat);

// ---- Remix material enums (shaders/rtx/concept/surface/surface_shared.h) -------------------------------
enum class TextureArgSource : std::uint8_t { None = 0, Texture, VertexColor0, TFactor };
enum class TextureOperation : std::uint8_t {
    Disable = 0,
    SelectArg1,
    SelectArg2,
    Modulate,
    Modulate2x,
    Modulate4x,
    Add,
    Force_Modulate2x,
};
enum class TexGenMode : std::uint8_t { None = 0, ViewPositions, CascadedViewPositions, ViewNormals };

const char* textureArgSourceName(TextureArgSource s);
const char* textureOperationName(TextureOperation o);
const char* texGenModeName(TexGenMode m);

// ---- inputs ----------------------------------------------------------------------------------------------

/// The fixed-function inputs a D3DStateModel does not carry.
struct FixedFunctionState {
    tap::Material material;            ///< Direct3DState9::material (D3DMATERIAL9)
    bool hasColor0 = false;            ///< D3D9VertexDeclFlag::HasColor0 (COLOR usage index 0)
    bool hasColor1 = false;            ///< D3D9VertexDeclFlag::HasColor1 (COLOR usage index 1)
    std::array<Mat4f, 8> textureTransforms{}; ///< D3DTS_TEXTURE0..7
    std::array<std::array<float, 4>, d3dff::kMaxClipPlanes> clipPlanes{};
};
/// From the tap's DrawState (identity texture transforms when it has no transforms).
FixedFunctionState buildFixedFunctionState(const tap::DrawState& state);

// ---- material ---------------------------------------------------------------------------------------------

/// DxvkBlendMode (Vulkan enum values).
struct BlendMode {
    bool enableBlending = false;
    std::uint32_t colorSrcFactor = vk::BLEND_FACTOR_ONE;
    std::uint32_t colorDstFactor = vk::BLEND_FACTOR_ZERO;
    std::uint32_t colorBlendOp = vk::BLEND_OP_ADD;
    std::uint32_t alphaSrcFactor = vk::BLEND_FACTOR_ONE;
    std::uint32_t alphaDstFactor = vk::BLEND_FACTOR_ZERO;
    std::uint32_t alphaBlendOp = vk::BLEND_OP_ADD;
    std::uint32_t writeMask = 0xf; ///< VkColorComponentFlags (D3DRS_COLORWRITEENABLE)
};

/// Remix LegacyMaterialData without the GPU objects: the colour textures are the classifier's choice
/// (tap sampler slots and their Remix image hashes).
struct LegacyMaterialRecord {
    bool alphaTestEnabled = false;
    std::uint8_t alphaTestReferenceValue = 0;
    std::uint32_t alphaTestCompareOp = vk::COMPARE_OP_ALWAYS;
    BlendMode blendMode;
    TextureArgSource diffuseColorSource = TextureArgSource::None;
    TextureArgSource specularColorSource = TextureArgSource::None;
    TextureArgSource textureColorArg1Source = TextureArgSource::Texture;
    TextureArgSource textureColorArg2Source = TextureArgSource::None;
    TextureOperation textureColorOperation = TextureOperation::Modulate;
    TextureArgSource textureAlphaArg1Source = TextureArgSource::Texture;
    TextureArgSource textureAlphaArg2Source = TextureArgSource::None;
    TextureOperation textureAlphaOperation = TextureOperation::SelectArg1;
    std::uint32_t tFactor = 0xffffffffu; ///< D3DRS_TEXTUREFACTOR (default opaque white)
    tap::Material d3dMaterial;
    bool isTextureFactorBlend = false;
    bool isVertexColorBakedLighting = true;
    std::array<std::int32_t, kMaxSupportedTextures> colorTextureSlots{-1, -1};
    std::array<hash::Hash64, kMaxSupportedTextures> colorTextureHashes{hash::kEmptyHash, hash::kEmptyHash};

    /// LegacyMaterialData::getHash (updateCachedHash): the colour texture 0 hash (plan §4.1.4).
    hash::Hash64 hash() const { return colorTextureHashes[0]; }
    /// LegacyMaterialData::usesTexture.
    bool usesTexture() const {
        return colorTextureHashes[0] != hash::kEmptyHash || colorTextureHashes[1] != hash::kEmptyHash;
    }
};

/// setLegacyMaterialState: colour sources, alpha test, texture factor, blend state (normalized for an
/// alpha-swizzled render target), D3DMATERIAL9 and rtx.vertexColorIsBakedLighting.
LegacyMaterialRecord setLegacyMaterialState(const D3DStateModel& state, const FixedFunctionState& ff, bool alphaSwizzle);

/// The texture-factor blending flags processTextures<FixedFunction> computes before setTextureStageState.
struct TextureFactorBlending {
    bool useStageTextureFactorBlending = true;
    bool useMultipleStageTextureFactorBlending = false;
};
/// The texture-factor part of processTextures (fixed function: stage scan with
/// rtx.enableMultiStageTextureFactorBlending and rtx.ignoreBakedLightingTextures; programmable pixel
/// shader: the defaults).
TextureFactorBlending textureFactorBlending(const D3DStateModel& state);

/// DrawCallTransforms (row-major D3DMATRIX layout; objectToView = objectToWorld x worldToView).
struct DrawTransforms {
    Mat4f objectToWorld{};
    Mat4f objectToView{};
    Mat4f worldToView{};
    Mat4f viewToProjection{};
    Mat4f textureTransform{};
    bool enableClipPlane = false;
    std::array<float, 4> clipPlane{0.f, 0.f, 0.f, 0.f};
    TexGenMode texgenMode = TexGenMode::None;
};

/// setTextureStageState: stage `stageIdx`'s colour / alpha operations and arguments into `material`
/// (TFACTOR arguments dropped unless useStageTextureFactorBlending), its texture transform and texgen
/// mode into `transforms`.
void setTextureStageState(const D3DStateModel& state, const FixedFunctionState& ff, std::uint32_t stageIdx,
                          bool useStageTextureFactorBlending, bool useMultipleStageTextureFactorBlending,
                          LegacyMaterialRecord& material, DrawTransforms& transforms);

/// processTextures' terrain-as-decal fix-up: with rtx.terrainAsDecalsEnabledIfNoBaker, no terrain baker and
/// rtx.terrain.terrainAsDecalsAllowOverModulate, a terrain texture's Modulate2x / Modulate4x colour
/// operation becomes Force_Modulate2x. `colorTextureHash` is the draw's colour texture 0 hash.
void applyTerrainAsDecalModulate(hash::Hash64 colorTextureHash, LegacyMaterialRecord& material);

/// processRenderState's transforms: objectToWorld (identity for a programmable VS unless vertex capture
/// and rtx.useWorldMatricesForShaders), VIEW, PROJECTION, objectToView, sanitize() and the single
/// supported user clip plane (the first enabled, non-degenerate one).
DrawTransforms processTransforms(const D3DStateModel& state, const FixedFunctionState& ff, bool useVertexCapture);

/// Row-vector matrix product a x b (D3DMATRIX layout).
Mat4f multiply(const Mat4f& a, const Mat4f& b);

// ---- fog -------------------------------------------------------------------------------------------------

/// Remix FogState.
struct FogRecord {
    std::uint32_t mode = d3dff::FOG_NONE; ///< D3DFOGMODE: table mode when set, else vertex mode
    std::array<float, 3> color{0.f, 0.f, 0.f};
    float scale = 0.f; ///< 1 / (FOGEND - FOGSTART)
    float end = 0.f;
    float density = 0.f;

    /// FogState::getHash: XXH3-64 over the 28-byte struct (mode, color, scale, end, density).
    hash::Hash64 hash() const;
};

/// setFogState (FOGENABLE off: mode NONE, the other fields untouched = zero).
FogRecord setFogState(const D3DStateModel& state);

} // namespace fuse::relight::scene
