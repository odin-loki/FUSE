/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_draw_call_tracker.cpp@0867d3c and
// src/dxvk/rtx_render/rtx_types.cpp@0867d3c (ReplacementInstance::clear). See draw_call_tracker.hpp.
#include <fuse/relight/scene/instances/draw_call_tracker.hpp>
#include <fuse/relight/scene/instances/instance_manager.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>

#include <fuse/relight/hash/xxh.hpp>

#include <cfloat>
#include <cstring>
#include <utility>

namespace fuse::relight::scene::instances {

namespace {

// Compute which lookup-key fields differ from the RI's cached values and write the lookup-drift bits (dynamic
// feature bits are preserved). Called BEFORE the RI's cached fields are overwritten.
void computeDirtyFlags(ReplacementInstance* ri, const ReplacementInstance::LookupKey& key) {
    ri->dirtyFlags &= ~ReplacementInstance::kLookupDriftMask;
    // Only called when the full identity hash doesn't match, so something must have changed.
    if (!bitwiseEqual(ri->objectToWorld, key.transform)) {
        ri->dirtyFlags |= ReplacementInstance::Transform;
    }
    if (ri->vertexPositionHash != key.vertexPositionHash) {
        ri->dirtyFlags |= ReplacementInstance::VertexPosHash;
    }
    if (ri->materialHash != key.materialHash) {
        ri->dirtyFlags |= ReplacementInstance::MaterialHash;
    }
    // textureTransform / texgenMode drift forces the dynamic path (catch-all Other bit).
    if (!bitwiseEqual(ri->textureTransform, key.textureTransform) || ri->texgenMode != key.texgenMode) {
        ri->dirtyFlags |= ReplacementInstance::Other;
    }
    if ((ri->dirtyFlags & ReplacementInstance::kLookupDriftMask) == 0) {
        ri->dirtyFlags |= ReplacementInstance::Other;
    }
}

} // namespace

const char* trackerMatchName(TrackerMatch m) {
    switch (m) {
    case TrackerMatch::Identity: return "identity";
    case TrackerMatch::ExactTransform: return "transform";
    case TrackerMatch::Nearest: return "nearest";
    case TrackerMatch::New: return "new";
    }
    return "?";
}

ReplacementInstance::ReplacementInstance(const LookupKey& key, std::uint32_t newId, std::uint32_t frameId)
    : id(newId),
      identityHash(key.identityHash),
      spatialMapHash(key.spatialMapHash),
      materialHash(key.materialHash),
      vertexPositionHash(key.vertexPositionHash),
      centroid(key.worldPos),
      frameCreated(frameId),
      textureTransform(key.textureTransform),
      texgenMode(key.texgenMode) {
    // No prior data to diff against; every field is effectively new: all dirty bits set so the first
    // submission runs the full update.
    dirtyFlags = kAllDirtyFlags;
}

void ReplacementInstance::clear() {
    for (RtInstance* instance : prims) {
        if (instance != nullptr) {
            instance->markForGarbageCollection();
            instance->setReplacementInstance(nullptr);
        }
    }
    prims.clear();
    legacyMaterialIdentityHash = hash::kEmptyHash;
    geometryBoundingBox = AxisAlignedBoundingBox();
    lightBoundingBox = AxisAlignedBoundingBox();
    dirtyFlags = kAllDirtyFlags;
}

Hash64 DrawCallTracker::computeIdentityHash(const SceneDrawInput& draw) {
    // struct IdentityHashData { geoHash, matHash, boneHash, overrideMaterialHash; Matrix4 xform, textureTransform;
    // uint32_t categories, miscFlags; } - 168 bytes, no padding, hashed with XXH3 (hashStructByMemory).
    struct IdentityHashData {
        Hash64 geoHash;
        Hash64 matHash;
        Hash64 boneHash;
        Hash64 overrideMaterialHash;
        float xform[16];
        float textureTransform[16];
        std::uint32_t categories;
        // Packed: cameraType (bits 0-7), texgenMode (bits 8-15), isUsingRaytracedRenderTarget (bit 16).
        std::uint32_t miscFlags;
    };
    static_assert(sizeof(IdentityHashData) == 4 * 8 + 2 * 64 + 2 * 4, "IdentityHashData must have no padding");
    IdentityHashData data{};
    data.geoHash = draw.geometry.hashForRule(hash::rules::kFullGeometry);
    data.matHash = draw.materialHash;
    data.overrideMaterialHash = draw.overrideMaterialHash;
    std::memcpy(data.xform, draw.objectToWorld.data(), sizeof data.xform);
    data.categories = draw.categories.raw();
    data.boneHash = draw.boneHash;
    std::memcpy(data.textureTransform, draw.textureTransform.data(), sizeof data.textureTransform);
    data.miscFlags = static_cast<std::uint32_t>(draw.cameraType) | (static_cast<std::uint32_t>(draw.texgenMode) << 8) |
                     (draw.isUsingRaytracedRenderTarget ? (1u << 16) : 0u);
    return hash::xxh3_64(&data, sizeof data);
}

ReplacementInstance::LookupKey DrawCallTracker::makeKey(const SceneDrawInput& draw) {
    // Tracking hash uses topology only (indices + geometry descriptor): the baseline grouped instances by
    // BlasEntry (topological hash). The material hash filters the spatial search instead of keying it, so a
    // draw still matches when its material changes between frames (LOD, texture animation).
    ReplacementInstance::LookupKey key;
    key.identityHash = computeIdentityHash(draw);
    key.spatialMapHash = draw.geometry.hashForRule(hash::rules::kTopological);
    key.materialHash = draw.materialHash;
    key.vertexPositionHash = draw.geometry[hash::HashComponent::Positions];
    key.worldPos = draw.boundingBox.getTransformedCentroid(draw.objectToWorld);
    key.transform = draw.objectToWorld;
    key.textureTransform = draw.textureTransform;
    key.texgenMode = draw.texgenMode;
    return key;
}

void DrawCallTracker::eraseFromSpatialMap(Hash64 bucketKey, Hash64 transformHash, const ReplacementInstance* data) {
    const auto it = m_assetSpatialMaps.find(bucketKey);
    if (it != m_assetSpatialMaps.end()) {
        it->second.erase(transformHash, data);
        if (it->second.size() == 0) {
            m_assetSpatialMaps.erase(it);
        }
    }
}

ReplacementInstance* DrawCallTracker::reassociateMatch(ReplacementInstance* match, const ReplacementInstance::LookupKey& key,
                                                       ReplacementSpatialMap* moveInAssetMap) {
    m_identityHashMap.erase(match->identityHash);
    match->identityHash = key.identityHash;
    match->vertexPositionHash = key.vertexPositionHash;
    match->materialHash = key.materialHash;
    match->centroid = key.worldPos;
    if (moveInAssetMap != nullptr) {
        match->spatialCacheTransformHash =
            moveInAssetMap->move(match->spatialCacheTransformHash, key.worldPos, key.transform, match);
    }
    m_identityHashMap[key.identityHash] = match;
    return match;
}

void DrawCallTracker::removeReplacementInstancesWithSpatialMapHash(Hash64 spatialMapKey) {
    for (std::size_t i = 0; i < m_replacementInstances.size();) {
        ReplacementInstance* ri = m_replacementInstances[i].get();
        if (ri->spatialMapHash == spatialMapKey) {
            destroyReplacementInstance(ri);
            std::swap(m_replacementInstances[i], m_replacementInstances.back());
            m_replacementInstances.pop_back();
            continue;
        }
        ++i;
    }
}

ReplacementInstance* DrawCallTracker::findOrCreateReplacementInstance(const ReplacementInstance::LookupKey& key,
                                                                      std::uint32_t currentFrame, TrackerMatch* how) {
    TrackerMatch local;
    TrackerMatch& match = how ? *how : local;

    // Level 1: exact identity match. Draw calls with the same identity hash return the same RI even if already
    // seen this frame (two-pass rendering: the second pass merges into the same instance).
    const auto exactMatchIter = m_identityHashMap.find(key.identityHash);
    if (exactMatchIter != m_identityHashMap.end()) {
        // Only clear lookup-drift bits on the first lookup of a new frame: a second lookup within the same frame
        // must not clobber flags the L2 path set when first matching this RI for the current frame.
        ReplacementInstance* ri = exactMatchIter->second;
        if (ri->frameLastSeen != currentFrame) {
            ri->dirtyFlags &= ~ReplacementInstance::kLookupDriftMask;
        }
        match = TrackerMatch::Identity;
        return ri;
    }

    // Level 2: tracking hash (topological, stable for animated geometry) + spatial proximity.
    const float uniqueObjectDistanceSqr = InstanceOptions::uniqueObjectDistanceSqr();
    const float spatialMapCellSize = InstanceOptions::uniqueObjectDistance() * 2.f;
    const auto l2Filter = [&](const ReplacementInstance* candidate) {
        return candidate->frameLastSeen != currentFrame && candidate->materialHash == key.materialHash;
    };

    const auto spatialMapIter = m_assetSpatialMaps.find(key.spatialMapHash);
    if (spatialMapIter != m_assetSpatialMaps.end()) {
        // Try exact transform + vertex position hash match first
        ReplacementInstance* exactTransformMatch = nullptr;
        spatialMapIter->second.forEachAtTransform(key.transform, [&](const ReplacementInstance* candidate) {
            if (candidate->vertexPositionHash == key.vertexPositionHash && l2Filter(candidate)) {
                exactTransformMatch = const_cast<ReplacementInstance*>(candidate);
                return true;
            }
            return false;
        });
        if (exactTransformMatch != nullptr) {
            // Transform / vertex / spatial map match by construction; only the material can diverge.
            computeDirtyFlags(exactTransformMatch, key);
            m_identityHashMap.erase(exactTransformMatch->identityHash);
            exactTransformMatch->identityHash = key.identityHash;
            m_identityHashMap[key.identityHash] = exactTransformMatch;
            match = TrackerMatch::ExactTransform;
            return exactTransformMatch;
        }

        // Spatial nearest-neighbor search
        float nearestDistSqr = FLT_MAX;
        const ReplacementInstance* nearestMatch =
            spatialMapIter->second.getNearestData(key.worldPos, uniqueObjectDistanceSqr, nearestDistSqr, l2Filter);
        if (nearestMatch != nullptr) {
            // The transform differs (else the exact-transform branch above hit); other fields may have changed.
            ReplacementInstance* ri = const_cast<ReplacementInstance*>(nearestMatch);
            computeDirtyFlags(ri, key);
            match = TrackerMatch::Nearest;
            return reassociateMatch(ri, key, &spatialMapIter->second);
        }
    }

    // Level 3: no match - create a new ReplacementInstance.
    auto created = std::make_unique<ReplacementInstance>(key, m_nextReplacementInstanceId++, currentFrame);
    ReplacementInstance* ri = created.get();
    m_identityHashMap[key.identityHash] = ri;
    auto [mapIter, inserted] = m_assetSpatialMaps.try_emplace(key.spatialMapHash, spatialMapCellSize);
    (void)inserted;
    ri->spatialCacheTransformHash = mapIter->second.insert(key.worldPos, key.transform, ri);
    m_replacementInstances.push_back(std::move(created));
    match = TrackerMatch::New;
    return ri;
}

ReplacementInstance* DrawCallTracker::findReplacementInstanceByIdentity(Hash64 identityHash) {
    const auto it = m_identityHashMap.find(identityHash);
    return it != m_identityHashMap.end() ? it->second : nullptr;
}

void DrawCallTracker::destroyReplacementInstance(ReplacementInstance* ri) {
    if (ri == nullptr) {
        return;
    }
    // The identity map may already point at another RI under this hash; only erase our own entry.
    const auto it = m_identityHashMap.find(ri->identityHash);
    if (it != m_identityHashMap.end() && it->second == ri) {
        m_identityHashMap.erase(it);
    }
    eraseFromSpatialMap(ri->spatialMapHash, ri->spatialCacheTransformHash, ri);
    ri->clear();
}

TrackerGcStats DrawCallTracker::garbageCollectReplacementInstances(std::uint32_t currentFrame, const AntiCullingFrustum* frustum,
                                                                   bool isCameraCut) {
    TrackerGcStats stats;
    const std::uint32_t numFramesToKeepObjects = InstanceOptions::numFramesToKeepInstances();
    const std::uint32_t numFramesToKeepLights = AntiCullingOptions::Light::numFramesToExtendLightLifetime();
    const bool objectAntiCullingEnabled = AntiCullingOptions::isObjectAntiCullingEnabled();
    const bool lightAntiCullingEnabled = AntiCullingOptions::isLightAntiCullingEnabled();
    const bool isAntiCullingSupported = frustum != nullptr;
    const bool highPrecision = AntiCullingOptions::Object::enableHighPrecisionAntiCulling();
    const bool forceGC = m_replacementInstances.size() >= AntiCullingOptions::Object::numObjectsToKeep();

    for (std::size_t i = 0; i < m_replacementInstances.size();) {
        ReplacementInstance* ri = m_replacementInstances[i].get();
        const bool hasLights = ri->lightBoundingBox.isValid();
        const bool hasMeshes = ri->geometryBoundingBox.isValid();

        if (ri->frameLastSeen + numFramesToKeepObjects <= currentFrame) {
            ++stats.expired;
            bool keepAlive = false;
            // Only anti-cull RIs that have been matched at least once after creation.
            const bool isStable = ri->frameLastSeen > ri->frameCreated;
            if (!isCameraCut && isStable) {
                // Skinned and player-model objects, IgnoreAntiCulling objects and objects that were moving when
                // last seen are exempt (anti-culling them freezes poses or motion).
                const bool wasMovingWhenLastSeen = (ri->dirtyFlags & ReplacementInstance::Transform) != 0;
                const bool exemptFromAntiCulling = ri->isSkinned ||
                                                   ri->categoryFlags.test(InstanceCategories::ThirdPersonPlayerModel) ||
                                                   ri->categoryFlags.test(InstanceCategories::IgnoreAntiCulling) ||
                                                   wasMovingWhenLastSeen;
                // Object anti-culling keeps objects OUTSIDE the camera frustum: the game may have culled them with
                // its own frustum. Visible objects the game stopped drawing had a reason (destruction, LOD).
                if (!keepAlive && objectAntiCullingEnabled && !forceGC && isAntiCullingSupported && hasMeshes &&
                    !exemptFromAntiCulling) {
                    keepAlive = !frustum->intersects(ri->geometryBoundingBox, ri->objectToWorld, highPrecision);
                }
                // Light anti-culling for mesh replacement lights, within the extended lifetime window.
                if (!keepAlive && lightAntiCullingEnabled && hasLights && isAntiCullingSupported && !exemptFromAntiCulling &&
                    !forceGC) {
                    const bool withinExtendedLifetime = ri->frameLastSeen + numFramesToKeepLights > currentFrame;
                    if (withinExtendedLifetime) {
                        keepAlive = !frustum->intersects(ri->lightBoundingBox, ri->objectToWorld, highPrecision);
                    }
                }
            }
            if (!keepAlive) {
                destroyReplacementInstance(ri);
                std::swap(m_replacementInstances[i], m_replacementInstances.back());
                m_replacementInstances.pop_back();
                ++stats.destroyed;
                continue;
            }
            ++stats.antiCulled;
        }
        ++i;
    }
    return stats;
}

void DrawCallTracker::clear() {
    for (auto& ri : m_replacementInstances) {
        ri->clear();
    }
    m_identityHashMap.clear();
    m_assetSpatialMaps.clear();
    m_replacementInstances.clear();
}

void DrawCallTracker::rebuildSpatialMaps(float cellSize) {
    for (auto& [hash, spatialMap] : m_assetSpatialMaps) {
        (void)hash;
        spatialMap.rebuild(cellSize);
    }
}

} // namespace fuse::relight::scene::instances
