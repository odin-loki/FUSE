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
// FUSE Relight RL-1.3: which TEXCOORD stream the texcoords hash and the geometry use.
//
// Ported from dxvk-remix @0867d3c (MIT), src/d3d9/d3d9_rtx.cpp D3D9Rtx::processTextures<>:
// Remix picks up to two colour textures (LegacyMaterialData::kMaxSupportedTextures). With a
// fixed-function pixel pipeline it bins the stages whose texture is actually read by their
// D3DTSS_TEXCOORDINDEX (two candidates per index, lowest index first) and the first texture found
// decides `firstStage`; with a pixel shader, firstStage is 0. Then
//   m_texcoordIndex = textureStages[firstStage][D3DTSS_TEXCOORDINDEX]
// (the whole DWORD: with D3DTSS_TCI_* generation flags it is > MAXD3DDECLUSAGEINDEX and selects no
// stream). The texture-factor bookkeeping of the same loop does not affect the choice and is not
// ported here (it belongs to the material translation, RL-1.5).
// Modifications Copyright (c) 2026 FUSE contributors (MIT).
#pragma once

#include <array>
#include <cstdint>

namespace fuse::relight::capture::geometry {

inline constexpr std::uint32_t kFixedFunctionStages = 8;   ///< caps::TextureStageCount
inline constexpr std::uint32_t kMaxSupportedTextures = 2;  ///< LegacyMaterialData::kMaxSupportedTextures
inline constexpr std::uint8_t kInvalidStage = 0xFF;

/// What processTextures needs to know about the texture bound to a stage.
struct StageTexture {
    bool bound = false;
    std::uint32_t resourceType = 0; ///< D3DRESOURCETYPE (3 texture, 4 volume, 5 cube)
    bool hashKnown = true;          ///< image hash != kEmptyHash (set on the first upload)
    bool lightmap = false;          ///< hash listed in rtx.lightmapTextures
};

struct TexcoordSelectInput {
    /// DrawState::textureStageStates: [stage][type - 1] = D3DTEXTURESTAGESTATETYPE `type`.
    const std::uint32_t (*textureStageStates)[32] = nullptr;
    std::array<StageTexture, kFixedFunctionStages> textures{};
    bool fixedFunctionPixel = true; ///< !UseProgrammablePS()
    bool allowCubemaps = false;     ///< rtx.allowCubemaps
};

struct TexcoordSelection {
    std::uint32_t firstStage = 0;
    std::uint32_t texcoordIndex = 0; ///< raw D3DTSS_TEXCOORDINDEX of firstStage
    /// Stages of colour textures 0 and 1 (kInvalidStage when none).
    std::array<std::uint8_t, kMaxSupportedTextures> colorTextureStages{kInvalidStage, kInvalidStage};
    /// Colour texture 0 has no image hash yet: upstream skips such a draw entirely
    /// ("Texture 0 without valid hash detected") and keeps the previous m_texcoordIndex.
    bool texture0HashMissing = false;

    /// The TEXCOORD usage index the draw reads, or -1 when texcoordIndex selects no stream.
    [[nodiscard]] std::int32_t usageIndex() const noexcept {
        return texcoordIndex <= 15 ? std::int32_t(texcoordIndex) : -1;
    }
};

/// D3DTEXTURESTAGESTATETYPE values used here.
namespace tss {
inline constexpr std::uint32_t kColorOp = 1, kColorArg1 = 2, kColorArg2 = 3, kAlphaOp = 4, kAlphaArg1 = 5,
                               kAlphaArg2 = 6, kTexCoordIndex = 11, kColorArg0 = 26, kAlphaArg0 = 27, kResultArg = 28;
} // namespace tss

[[nodiscard]] TexcoordSelection selectTexcoordIndex(const TexcoordSelectInput& input) noexcept;

} // namespace fuse::relight::capture::geometry
