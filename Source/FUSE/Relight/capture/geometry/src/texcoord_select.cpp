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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (D3D9Rtx::processTextures<>: the stage
// binning, the choice of the colour textures and m_texcoordIndex).
// FUSE changes: only the texture choice is ported (texture factor blending and sampler setup
// belong to the material translation); texture facts come from the caller.
#include <fuse/relight/capture/geometry/texcoord_select.hpp>

namespace fuse::relight::capture::geometry {

namespace {

// D3DTEXTUREOP / D3DTA values.
constexpr std::uint32_t kTopDisable = 1, kTopSelectArg1 = 2, kTopSelectArg2 = 3, kTopPremodulate = 17,
                        kTopMultiplyAdd = 25, kTopLerp = 26;
constexpr std::uint32_t kTaSelectMask = 0x0000000f, kTaTexture = 0x00000002;
constexpr std::uint32_t kResourceTexture = 3, kResourceCubeTexture = 5;
constexpr std::uint32_t kMaxTexcoord = 8; // D3DDP_MAXTEXCOORD

/// Used args for a given operation (bit 0: arg0, bit 1: arg1, bit 2: arg2).
std::uint32_t argsMask(std::uint32_t op) noexcept {
    switch (op) {
    case kTopDisable:
        return 0b000u;
    case kTopSelectArg1:
    case kTopPremodulate:
        return 0b010u;
    case kTopSelectArg2:
        return 0b100u;
    case kTopMultiplyAdd:
    case kTopLerp:
        return 0b111u;
    default:
        return 0b110u;
    }
}

} // namespace

TexcoordSelection selectTexcoordIndex(const TexcoordSelectInput& in) noexcept {
    TexcoordSelection out;
    const auto state = [&](std::uint32_t stage, std::uint32_t type) -> std::uint32_t {
        return in.textureStageStates ? in.textureStageStates[stage][type - 1] : 0u;
    };

    constexpr std::uint32_t kBinsFixedFunction = kMaxTexcoord * kMaxSupportedTextures;
    const std::uint32_t numBins = in.fixedFunctionPixel ? kBinsFixedFunction : kMaxSupportedTextures;

    // Build a mapping of texcoord indices to stage.
    std::uint8_t texcoordIndexToStage[kBinsFixedFunction];
    for (std::uint8_t& s : texcoordIndexToStage) {
        s = kInvalidStage;
    }
    if (in.fixedFunctionPixel) {
        for (std::uint32_t stage = 0; stage < kFixedFunctionStages; ++stage) {
            const StageTexture& tex = in.textures[stage];
            if (!tex.bound) {
                continue;
            }
            // Subsequent stages do not occur if this is true.
            if (state(stage, tss::kColorOp) == kTopDisable) {
                break;
            }
            const std::uint32_t used = argsMask(state(stage, tss::kColorOp)) | argsMask(state(stage, tss::kAlphaOp));
            const auto isTexture = [&](std::uint32_t colorArg, std::uint32_t alphaArg) {
                return (state(stage, colorArg) & kTaSelectMask) == kTaTexture ||
                       (state(stage, alphaArg) & kTaSelectMask) == kTaTexture;
            };
            const std::uint32_t texMask = (isTexture(tss::kColorArg0, tss::kAlphaArg0) ? 0b001u : 0u) |
                                          (isTexture(tss::kColorArg1, tss::kAlphaArg1) ? 0b010u : 0u) |
                                          (isTexture(tss::kColorArg2, tss::kAlphaArg2) ? 0b100u : 0u);
            // Is texture used?
            if ((used & texMask) == 0) {
                continue;
            }
            // Remix can only handle 2D textures - no volumes.
            if (tex.resourceType != kResourceTexture && (!in.allowCubemaps || tex.resourceType != kResourceCubeTexture)) {
                continue;
            }
            // Currently only regular textures are supported, skip lightmaps.
            if (tex.lightmap) {
                continue;
            }
            // Allow for two stage candidates per texcoord index.
            const std::uint32_t texcoordIndex = state(stage, tss::kTexCoordIndex) & 0b111u;
            const std::uint32_t candidate = texcoordIndex * kMaxSupportedTextures;
            const std::uint32_t sub = texcoordIndexToStage[candidate] == kInvalidStage ? 0u : 1u;
            // Don't override if candidate exists.
            if (texcoordIndexToStage[candidate + sub] == kInvalidStage) {
                texcoordIndexToStage[candidate + sub] = std::uint8_t(stage);
            }
        }
    }

    // Find the ideal textures for raytracing.
    std::uint32_t firstStage = 0;
    for (std::uint32_t idx = 0, textureId = 0; idx < numBins && textureId < kMaxSupportedTextures; ++idx) {
        const std::uint8_t stage = in.fixedFunctionPixel ? texcoordIndexToStage[idx] : std::uint8_t(textureId);
        if (stage == kInvalidStage || stage >= kFixedFunctionStages || !in.textures[stage].bound) {
            continue;
        }
        if (textureId == 0) {
            if (!in.textures[stage].hashKnown) {
                // Upstream: "Texture 0 without valid hash detected, skipping drawcall."
                out.texture0HashMissing = true;
            }
            if (in.fixedFunctionPixel) {
                firstStage = stage;
            }
        }
        out.colorTextureStages[textureId] = stage;
        ++textureId;
    }

    out.firstStage = firstStage;
    out.texcoordIndex = state(firstStage, tss::kTexCoordIndex);
    return out;
}

} // namespace fuse::relight::capture::geometry
