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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_draw_call_tracker.{h,cpp}@0867d3c (DrawCallTracker) and
// src/dxvk/rtx_render/rtx_types.h@0867d3c (ReplacementInstance, its LookupKey and dirty flags).
//
// Frame-to-frame identity of draw calls (plan §1.5 "processSceneObject semantics"). Every committed draw maps
// to a ReplacementInstance, which owns the draw's RtInstance(s):
//   L1  the identity hash (full geometry hash, legacy material hash, bone hash, override material hash,
//       objectToWorld, texture transform, categories, camera type, texgen mode, raytraced-render-target use)
//       was seen before: same ReplacementInstance, even twice in one frame (two-pass rendering);
//   L2  within the spatial map of the draw's topological hash (the "same BLAS" bucket: indices + geometry
//       descriptor) and among candidates not yet seen this frame with the same legacy material hash: an entry
//       at exactly this transform with the same vertex positions hash, else the nearest one whose world
//       centroid (bounding-box centroid through objectToWorld) is within rtx.uniqueObjectDistance;
//   L3  otherwise a new ReplacementInstance (and so a new RtInstance id).
// Garbage collection drops ReplacementInstances not seen for rtx.numFramesToKeepInstances frames, unless
// object anti-culling keeps them: stable (seen after their creation frame), no camera cut, not skinned /
// player model / IgnoreAntiCulling / moving when last seen, and outside the anti-culling frustum.
//
// Not ported: ray-portal matching of view-model draws (tryPortalMatch; FUSE has no ray portals yet),
// replacement buckets (prims beyond the draw's own instance, light bounding boxes are carried but only
// replacement lights would fill them).
#pragma once

#include <fuse/relight/scene/instances/anti_culling.hpp>
#include <fuse/relight/scene/instances/scene_draw.hpp>
#include <fuse/relight/scene/instances/spatial_map.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fuse::relight::scene::instances {

class RtInstance;

struct ReplacementInstance {
    /// Bundled hash / position / transform parameters used to look up or create a ReplacementInstance.
    struct LookupKey {
        Hash64 identityHash = hash::kEmptyHash;
        /// The spatial map to search (the draw's topological hash).
        Hash64 spatialMapHash = hash::kEmptyHash;
        Hash64 materialHash = hash::kEmptyHash;
        Hash64 vertexPositionHash = hash::kEmptyHash;
        Vec3 worldPos;
        Mat4f transform = identityMatrix();
        Mat4f textureTransform = identityMatrix();
        TexGenMode texgenMode = TexGenMode::None;
    };

    /// Lookup-drift bits (Transform, VertexPosHash, MaterialHash, Other) reflect LookupKey changes and are
    /// cleared on the first exact-match lookup each frame; dynamic-feature bits (ParticleSystem) are set during
    /// processing and cleared when entering the dynamic path.
    enum DirtyFlag : std::uint32_t {
        Transform = 1u << 0,
        VertexPosHash = 1u << 1,
        MaterialHash = 1u << 2,
        Other = 1u << 3,
        ParticleSystem = 1u << 4,
    };
    static constexpr std::uint32_t kLookupDriftMask = Transform | VertexPosHash | MaterialHash | Other;
    static constexpr std::uint32_t kDynamicFeatureMask = ParticleSystem;
    static constexpr std::uint32_t kAllDirtyFlags = kLookupDriftMask | kDynamicFeatureMask;

    ReplacementInstance(const LookupKey& key, std::uint32_t newId, std::uint32_t frameId);

    /// Marks every prim instance for garbage collection and drops them (back to the just-constructed shape:
    /// no legacy material identity, invalid bounding boxes, every dirty bit set).
    void clear();

    std::vector<RtInstance*> prims;

    // Frame-to-frame tracking fields
    std::uint32_t id = 0;
    Hash64 identityHash = hash::kEmptyHash;
    Hash64 spatialMapHash = hash::kEmptyHash;
    Hash64 materialHash = hash::kEmptyHash;
    Hash64 legacyMaterialIdentityHash = hash::kEmptyHash;
    Hash64 vertexPositionHash = hash::kEmptyHash;
    Vec3 centroid;
    std::uint32_t frameCreated = 0;
    std::uint32_t frameLastSeen = 0;
    Hash64 spatialCacheTransformHash = hash::kEmptyHash;

    // Draw call properties that affect anti-culling GC decisions (set each time the RI is matched).
    CategoryFlags categoryFlags;
    bool isSkinned = false;

    /// Anti-culling bounding boxes in the space of objectToWorld (geometry: the draw's object space).
    AxisAlignedBoundingBox geometryBoundingBox;
    AxisAlignedBoundingBox lightBoundingBox;
    Mat4f objectToWorld = identityMatrix();

    Mat4f textureTransform = identityMatrix();
    TexGenMode texgenMode = TexGenMode::None;
    std::uint32_t dirtyFlags = 0;
};

/// Why the tracker kept or created an entry (FUSE, for tools and tests).
enum class TrackerMatch : std::uint8_t { Identity = 0, ExactTransform, Nearest, New };
const char* trackerMatchName(TrackerMatch m);

/// What the anti-culling GC pass did (FUSE, for tools and tests).
struct TrackerGcStats {
    std::uint32_t expired = 0;      ///< past rtx.numFramesToKeepInstances
    std::uint32_t antiCulled = 0;   ///< expired but kept alive by anti-culling
    std::uint32_t destroyed = 0;
};

class DrawCallTracker {
public:
    DrawCallTracker() = default;
    DrawCallTracker(const DrawCallTracker&) = delete;
    DrawCallTracker& operator=(const DrawCallTracker&) = delete;

    /// computeIdentityHash: XXH3 over the packed IdentityHashData (bit-exact with upstream's struct layout).
    static Hash64 computeIdentityHash(const SceneDrawInput& draw);
    /// The LookupKey of a draw (identity, topological hash, material, vertex positions, world centroid).
    static ReplacementInstance::LookupKey makeKey(const SceneDrawInput& draw);

    ReplacementInstance* findOrCreateReplacementInstance(const ReplacementInstance::LookupKey& key, std::uint32_t currentFrame,
                                                         TrackerMatch* how = nullptr);
    ReplacementInstance* findOrCreateReplacementInstance(const SceneDrawInput& draw, std::uint32_t currentFrame,
                                                         TrackerMatch* how = nullptr) {
        return findOrCreateReplacementInstance(makeKey(draw), currentFrame, how);
    }
    ReplacementInstance* findReplacementInstanceByIdentity(Hash64 identityHash);

    /// garbageCollectReplacementInstances. `frustum` null: anti-culling unsupported this frame.
    TrackerGcStats garbageCollectReplacementInstances(std::uint32_t currentFrame, const AntiCullingFrustum* frustum,
                                                      bool isCameraCut);

    void removeReplacementInstancesWithSpatialMapHash(Hash64 spatialMapHash);
    void clear();
    /// Rebuild all spatial maps with a new cell size (rtx.uniqueObjectDistance changed).
    void rebuildSpatialMaps(float cellSize);

    const std::vector<std::unique_ptr<ReplacementInstance>>& getReplacementInstances() const { return m_replacementInstances; }

private:
    using ReplacementSpatialMap = SpatialMap<ReplacementInstance>;

    void destroyReplacementInstance(ReplacementInstance* ri);
    void eraseFromSpatialMap(Hash64 bucketKey, Hash64 transformHash, const ReplacementInstance* data);
    ReplacementInstance* reassociateMatch(ReplacementInstance* match, const ReplacementInstance::LookupKey& key,
                                          ReplacementSpatialMap* moveInAssetMap);

    std::unordered_map<Hash64, ReplacementInstance*> m_identityHashMap;
    std::unordered_map<Hash64, ReplacementSpatialMap> m_assetSpatialMaps;
    std::vector<std::unique_ptr<ReplacementInstance>> m_replacementInstances;
    std::uint32_t m_nextReplacementInstanceId = 0;
};

} // namespace fuse::relight::scene::instances
