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
// FUSE Relight RL-1.3: fixed-function vertex blending (skinning) data.
//
// Ported from dxvk-remix @0867d3c (MIT):
//   src/d3d9/d3d9_rtx.cpp        D3D9Rtx::processSkinning
//   src/d3d9/d3d9_rtx_utils.cpp  getMinMaxBoneIndices
//   src/dxvk/rtx_render/rtx_types.h  SkinningData::computeHash
// Modifications Copyright (c) 2026 FUSE contributors (MIT).
//
// Rules kept:
//   * programmable vertex shaders never produce skinning data;
//   * D3DRS_VERTEXBLEND = DISABLE -> none; 1..3WEIGHTS need a BLENDWEIGHT[0] stream; 0WEIGHTS
//     needs indexed blending (a BLENDINDICES element in the declaration and
//     D3DRS_INDEXEDVERTEXBLENDENABLE);
//   * numBonesPerVertex = weights + 1 (0WEIGHTS: 1); with indexed blending and a BLENDINDICES[0]
//     stream, the first numBonesPerVertex bytes of each vertex give the min / max bone index and
//     numBones = max + 1, otherwise numBones = numBonesPerVertex and minBoneIndex = 0;
//   * boneMatrices = WORLDMATRIX(0 .. numBones - 1), as D3D9 stores them (row-major D3DMATRIX);
//     boneHash = XXH3_64bits over matrices [minBoneIndex, numBones).
// Upstream stages only WORLDMATRIX(0 .. m_maxBone) (the highest world matrix the game set, 255
// when none) and indexes past that for larger bone indices; FUSE stages all 256 for indexed
// blending, so a bone index above m_maxBone reads the device's (identity) matrix, not stale memory.
#pragma once

#include <fuse/relight/capture/geometry/vertex_streams.hpp>
#include <fuse/relight/hash/xxh.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fuse::relight::capture::geometry {

using Matrix4 = std::array<float, 16>; ///< row-major D3DMATRIX

/// D3DRS_* and D3DVBF_* values.
inline constexpr std::uint32_t kD3DRSVertexBlend = 151;
inline constexpr std::uint32_t kD3DRSIndexedVertexBlendEnable = 167;
inline constexpr std::uint32_t kD3DVbfDisable = 0, kD3DVbf1Weights = 1, kD3DVbf2Weights = 2, kD3DVbf3Weights = 3,
                               kD3DVbfTweening = 255, kD3DVbf0Weights = 256;
inline constexpr std::uint32_t kMaxBones = 256;

/// SkinningData.
struct SkinningData {
    std::vector<Matrix4> boneMatrices; ///< WORLDMATRIX(0 .. numBones - 1)
    std::uint32_t numBones = 0;
    std::uint32_t numBonesPerVertex = 0;
    std::uint32_t minBoneIndex = 0;
    hash::Hash64 boneHash = 0;
    /// The blend attributes of the draw (for the skinning pass of the scene backend).
    VertexAttribute blendWeights;
    VertexAttribute blendIndices;
};

struct SkinningInput {
    bool programmableVs = false;
    std::uint32_t vertexBlend = kD3DVbfDisable; ///< D3DRS_VERTEXBLEND
    bool indexedVertexBlendEnable = false;      ///< D3DRS_INDEXEDVERTEXBLENDENABLE
    const SlicedVertices* vertices = nullptr;
    std::uint32_t vertexCount = 0;
    /// DrawState::transforms (kTransformCount entries): WORLDMATRIX(n) at tap::kTransformWorld0 + n.
    const float (*transforms)[16] = nullptr;
};

/// The synchronous half of processSkinning: what the job needs, with the bone matrices staged.
struct SkinningJob {
    std::uint32_t numBonesPerVertex = 0;
    std::uint32_t vertexCount = 0;
    bool indexed = false; ///< scan BLENDINDICES[0] for the bone range
    VertexAttribute blendWeights;
    VertexAttribute blendIndices;
    std::shared_ptr<const std::vector<Matrix4>> stagedBones; ///< WORLDMATRIX(0 ..)
};

/// nullopt where upstream returns the empty future (no skinning).
[[nodiscard]] std::optional<SkinningJob> prepareSkinning(const SkinningInput& input);

/// The job body.
[[nodiscard]] SkinningData runSkinningJob(const SkinningJob& job);

/// getMinMaxBoneIndices: false for vertexCount 0 (min 256 / max -1 otherwise when nothing is read).
bool minMaxBoneIndices(const std::uint8_t* boneIndices, std::uint32_t stride, std::uint32_t vertexCount,
                       std::uint32_t numBonesPerVertex, int& minBoneIndex, int& maxBoneIndex) noexcept;

} // namespace fuse::relight::capture::geometry
