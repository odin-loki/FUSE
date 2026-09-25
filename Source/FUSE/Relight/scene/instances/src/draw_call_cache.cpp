/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_draw_call_cache.cpp@0867d3c and
// src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c (processGeometryInfo, onSceneObjectAdded / Updated,
// garbageCollection). See draw_call_cache.hpp.
#include <fuse/relight/scene/instances/draw_call_cache.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>

#include <algorithm>
#include <limits>

namespace fuse::relight::scene::instances {

namespace {

using hash::HashComponent;

bool exactMatch(const SceneDrawInput& drawCall, const BlasEntry& blas) {
    const auto isSky = [](CameraType t) { return t == CameraType::Sky; };
    if (isSky(drawCall.cameraType) != isSky(blas.input.cameraType)) {
        return false;
    }
    return drawCall.materialHash == blas.input.materialHash &&
           drawCall.geometry.hashForRule(hash::rules::kFullGeometry) ==
               blas.input.geometry.hashForRule(hash::rules::kFullGeometry) &&
           drawCall.boneHash == blas.input.boneHash;
}

} // namespace

const char* objectCacheStateName(ObjectCacheState s) {
    switch (s) {
    case ObjectCacheState::BuildBVH: return "build";
    case ObjectCacheState::UpdateBVH: return "refit";
    case ObjectCacheState::UpdateInstance: return "instance";
    case ObjectCacheState::Invalid: return "invalid";
    }
    return "?";
}

void BlasEntry::linkInstance(RtInstance* instance) {
    linkedInstances.push_back(instance);
}

void BlasEntry::unlinkInstance(RtInstance* instance) {
    const auto it = std::find(linkedInstances.begin(), linkedInstances.end(), instance);
    if (it != linkedInstances.end()) {
        *it = linkedInstances.back();
        linkedInstances.pop_back();
    }
}

DrawCallCache::CacheState DrawCallCache::get(const SceneDrawInput& drawCall, std::uint32_t currentFrame, BlasEntry** out) {
    *out = nullptr;
    // First, find the right bucket:
    const Hash64 hash = drawCall.geometry.hashForRule(hash::rules::kTopological);
    const auto range = m_entries.equal_range(hash);
    if (range.first == range.second) {
        // New bucket
        *out = allocateEntry(hash, drawCall, currentFrame);
        return CacheState::New;
    }
    // Handle buckets with 1 entry:
    auto iter = range.first;
    ++iter;
    if (iter == range.second) {
        BlasEntry& entry = range.first->second;
        const bool updatedThisFrame = entry.frameLastTouched == currentFrame;
        const bool vertexDataMatches = entry.input.geometry.hashForRule(hash::rules::kVertexData) ==
                                       drawCall.geometry.hashForRule(hash::rules::kVertexData);
        const bool boneHashesMatch = entry.input.boneHash == drawCall.boneHash;
        const bool materialHashesMatch = entry.input.materialHash == drawCall.materialHash;

        if (exactMatch(drawCall, entry) || (!updatedThisFrame && ((vertexDataMatches && boneHashesMatch) || materialHashesMatch))) {
            // Exact vertex match that is reusable for the current draw call, or something that hasn't been
            // updated this frame and is similar enough (the multi-element loop below applies the same logic).
            *out = &entry;
            return CacheState::Existed;
        }
        // First frame of having two mismatching instances, and the first instance has already been paired
        // with the existing BlasEntry.
        *out = allocateEntry(hash, drawCall, currentFrame);
        return CacheState::New;
    }

    // Bucket has multiple BlasEntries.
    // Upstream starts from std::numeric_limits<float>::min() (the smallest positive float), so an entry only
    // wins with a positive score; kept as is.
    float bestScore = std::numeric_limits<float>::min();
    const Vec3 newWorldPosition = drawCall.boundingBox.getTransformedCentroid(drawCall.objectToWorld);
    for (auto it = range.first; it != range.second; ++it) {
        BlasEntry& blas = it->second;
        if (exactMatch(drawCall, blas)) {
            *out = &blas;
            return CacheState::Existed;
        }
        if (blas.frameLastTouched == currentFrame) {
            continue;
        }
        // TODO (upstream) these heuristics could use more refinement.
        float score = 0;
        if (blas.modifiedHashes[HashComponent::Positions] == drawCall.geometry[HashComponent::Positions] &&
            blas.input.boneHash == drawCall.boneHash) {
            score += 1000.f;
        }
        if (blas.modifiedHashes[HashComponent::Texcoords] == drawCall.geometry[HashComponent::Texcoords]) {
            score += 1000.f;
        }
        if (blas.input.materialHash == drawCall.materialHash) {
            score += 1000.f;
        }
        // Upstream note: this only checks the distance to the first instance that created the BlasEntry, and
        // has no ray portal logic.
        const Vec3 worldPosition = blas.input.boundingBox.getTransformedCentroid(blas.input.objectToWorld);
        score -= lengthSqr(newWorldPosition - worldPosition);
        if (score > bestScore) {
            bestScore = score;
            *out = &blas;
        }
    }
    if (*out == nullptr) {
        // Failed to find similar blas, so allocate a new one
        *out = allocateEntry(hash, drawCall, currentFrame);
        return CacheState::New;
    }
    return CacheState::Existed;
}

ObjectCacheState DrawCallCache::process(BlasEntry& blas, CacheState state, const SceneDrawInput& draw, std::uint32_t currentFrame) {
    ObjectCacheState result = ObjectCacheState::BuildBVH;
    if (state == CacheState::Existed) {
        // onSceneObjectUpdated
        if (blas.frameLastTouched == currentFrame) {
            // Another draw already refreshed this entry this frame (cacheMaterial): instance update only.
            return ObjectCacheState::UpdateInstance;
        }
        // processGeometryInfo<false>
        if (draw.geometry[HashComponent::Indices] == blas.modifiedHashes[HashComponent::Indices]) {
            if (draw.geometry[HashComponent::Positions] == blas.modifiedHashes[HashComponent::Positions] &&
                draw.geometry[HashComponent::VertexShader] == blas.modifiedHashes[HashComponent::VertexShader] &&
                draw.boneHash == blas.lastBoneHash) {
                result = ObjectCacheState::UpdateInstance;
            } else {
                result = ObjectCacheState::UpdateBVH;
            }
        }
    }
    blas.lastBoneHash = draw.boneHash;
    blas.modifiedHashes = draw.geometry;
    // "Assume we won't need this, and update the value if required": only a refit keeps previous positions.
    blas.previousPositionsDefined = result == ObjectCacheState::UpdateBVH;
    if (result == ObjectCacheState::BuildBVH || result == ObjectCacheState::UpdateBVH) {
        blas.frameLastUpdated = currentFrame;
    }
    blas.input = draw; // cache the draw state for the next time
    // processDrawCallState: "Update the input state, so we always have a reference to the original draw call state".
    blas.frameLastTouched = currentFrame;
    return result;
}

void DrawCallCache::garbageCollection(std::uint32_t currentFrame) {
    // Only GC entries with no linked instances: instances still reference the BlasEntry for the TLAS build.
    const std::uint32_t keep = InstanceOptions::numFramesToKeepGeometryData();
    if (currentFrame <= keep) {
        return;
    }
    const std::uint32_t oldestFrame = currentFrame - keep;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.frameLastTouched < oldestFrame && it->second.linkedInstances.empty()) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

BlasEntry* DrawCallCache::allocateEntry(Hash64 hash, const SceneDrawInput& draw, std::uint32_t currentFrame) {
    auto it = m_entries.emplace(hash, BlasEntry{});
    BlasEntry* result = &it->second;
    result->id = m_nextId++;
    result->input = draw;
    result->frameCreated = currentFrame;
    return result;
}

} // namespace fuse::relight::scene::instances
