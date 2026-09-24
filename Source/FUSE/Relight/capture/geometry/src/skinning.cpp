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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (D3D9Rtx::processSkinning),
// src/d3d9/d3d9_rtx_utils.cpp@0867d3c (getMinMaxBoneIndices) and
// src/dxvk/rtx_render/rtx_types.h@0867d3c (SkinningData::computeHash).
// FUSE changes: the bone matrices are staged from the tap's DrawState transforms (all 256 world
// matrices for indexed blending, numBonesPerVertex otherwise) instead of SkinningMatrixPool.
#include <fuse/relight/capture/geometry/skinning.hpp>

#include <fuse/relight/tap/relight_tap.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::geometry {

bool minMaxBoneIndices(const std::uint8_t* boneIndices, std::uint32_t stride, std::uint32_t vertexCount,
                       std::uint32_t numBonesPerVertex, int& minBoneIndex, int& maxBoneIndex) noexcept {
    if (vertexCount == 0) {
        return false;
    }
    minBoneIndex = 256;
    maxBoneIndex = -1;
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        for (std::uint32_t j = 0; j < numBonesPerVertex; ++j) {
            minBoneIndex = std::min(minBoneIndex, int(boneIndices[j]));
            maxBoneIndex = std::max(maxBoneIndex, int(boneIndices[j]));
        }
        boneIndices += stride;
    }
    return true;
}

std::optional<SkinningJob> prepareSkinning(const SkinningInput& in) {
    if (in.programmableVs || in.vertices == nullptr) {
        return std::nullopt;
    }
    // Some games set vertex blend without enough data to actually do the blending.
    const bool hasBlendWeight = in.vertices->blendWeight.defined();
    const bool hasBlendIndices = in.vertices->hasBlendIndicesElement;
    const bool indexedVertexBlend = hasBlendIndices && in.indexedVertexBlendEnable;

    if (in.vertexBlend == kD3DVbfDisable) {
        return std::nullopt;
    }
    if (in.vertexBlend != kD3DVbf0Weights) {
        if (!hasBlendWeight) {
            return std::nullopt;
        }
    } else if (!indexedVertexBlend) {
        return std::nullopt;
    }

    SkinningJob job;
    switch (in.vertexBlend) {
    case kD3DVbf0Weights:
        job.numBonesPerVertex = 1;
        break;
    case kD3DVbf1Weights:
        job.numBonesPerVertex = 2;
        break;
    case kD3DVbf2Weights:
        job.numBonesPerVertex = 3;
        break;
    case kD3DVbf3Weights:
        job.numBonesPerVertex = 4;
        break;
    default:
        job.numBonesPerVertex = 0; // D3DVBF_TWEENING and unknown values
        break;
    }
    job.vertexCount = in.vertexCount;
    job.indexed = indexedVertexBlend && in.vertices->blendIndices.defined();
    job.blendWeights = in.vertices->blendWeight;
    job.blendIndices = in.vertices->blendIndices;

    // Stage the bone matrices the job may read: all of them when the bone range comes from the
    // vertex data, numBonesPerVertex otherwise.
    const std::uint32_t staged = job.indexed ? kMaxBones : std::min(job.numBonesPerVertex, kMaxBones);
    auto bones = std::make_shared<std::vector<Matrix4>>(staged);
    for (std::uint32_t n = 0; n < staged; ++n) {
        if (in.transforms != nullptr) {
            std::memcpy((*bones)[n].data(), in.transforms[tap::kTransformWorld0 + n], sizeof(Matrix4));
        } else {
            (*bones)[n] = Matrix4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        }
    }
    job.stagedBones = std::move(bones);
    return job;
}

SkinningData runSkinningJob(const SkinningJob& job) {
    std::uint32_t numBones = job.numBonesPerVertex;
    int minBoneIndex = 0;
    if (job.indexed) {
        // Find out how many bone indices are specified for each vertex; needed to find the min
        // bone index and ignore the padding zeroes.
        int maxBoneIndex = -1;
        if (!minMaxBoneIndices(job.blendIndices.base(), job.blendIndices.stride, job.vertexCount, job.numBonesPerVertex,
                               minBoneIndex, maxBoneIndex)) {
            minBoneIndex = 0;
            maxBoneIndex = 0;
        }
        numBones = std::uint32_t(maxBoneIndex + 1);
    }

    SkinningData data;
    data.boneMatrices.reserve(numBones);
    for (std::uint32_t n = 0; n < numBones && n < job.stagedBones->size(); ++n) {
        data.boneMatrices.push_back((*job.stagedBones)[n]);
    }
    data.numBones = numBones;
    data.numBonesPerVertex = job.numBonesPerVertex;
    data.minBoneIndex = std::uint32_t(minBoneIndex);
    // computeHash()
    if (numBones > 0 && std::uint32_t(minBoneIndex) < numBones) {
        data.boneHash = hash::xxh3_64(data.boneMatrices.data() + minBoneIndex,
                                      (numBones - std::uint32_t(minBoneIndex)) * sizeof(Matrix4));
    } else {
        data.boneHash = 0;
    }
    data.blendWeights = job.blendWeights;
    data.blendIndices = job.blendIndices;
    return data;
}

} // namespace fuse::relight::capture::geometry
