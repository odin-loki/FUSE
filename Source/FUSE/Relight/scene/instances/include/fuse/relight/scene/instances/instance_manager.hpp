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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_instance_manager.{h,cpp}@0867d3c (RtInstance's identity,
// transform history and lifecycle; InstanceManager::processSceneObject, addInstance, updateInstance's
// per-frame bookkeeping and transform update, preserveInstance, garbageCollection,
// RtInstance::calculateAntiCullingHash) and src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c
// (SceneManager::preserveInstance's transform re-sync).
//
// An RtInstance is a draw's placement in the scene: a stable id (from 1, never reused), the linked BLAS,
// objectToWorld and the previous frame's objectToWorld (motion vectors), the frame it was created and last
// updated. Transform history follows Remix exactly:
//   * first update after creation     teleport: prev = current = objectToWorld (treated as still);
//   * first update in a later frame    move:     prev = last current, current = objectToWorld;
//   * further updates in the same frame moveAgain: current = objectToWorld, prev kept;
//   * preserve path (unchanged draw)   no update; the first preserve frame re-syncs prev = current.
// hasTransformChanged is a bytewise comparison of prev and current.
//
// Not ported (render-side): Vulkan instance masks / flags, material binding to GPU indices, billboards and
// beams, view-model / player-model / ray-portal virtual instances, OMM data, developer overrides.
#pragma once

#include <fuse/relight/scene/instances/draw_call_cache.hpp>
#include <fuse/relight/scene/instances/scene_draw.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fuse::relight::scene::instances {

struct ReplacementInstance;

class RtInstance {
public:
    static constexpr std::uint32_t kInvalidFrame = 0xffffffffu;

    RtInstance(std::uint64_t id, std::uint32_t vectorIdx) : m_id(id), m_vectorIdx(vectorIdx) {}

    std::uint64_t getId() const { return m_id; }
    std::uint32_t getVectorIdx() const { return m_vectorIdx; }
    BlasEntry* getBlas() const { return m_linkedBlas; }
    const Mat4f& getTransform() const { return m_objectToWorld; }
    const Mat4f& getPrevTransform() const { return m_prevObjectToWorld; }
    const std::array<float, 9>& getNormalObjectToWorld() const { return m_normalObjectToWorld; }
    Vec3 getWorldPosition() const { return translation(m_objectToWorld); }
    Vec3 getPrevWorldPosition() const { return translation(m_prevObjectToWorld); }

    bool isCreatedThisFrame(std::uint32_t frameIndex) const { return frameIndex == m_frameCreated; }
    std::uint32_t getFrameCreated() const { return m_frameCreated; }
    std::uint32_t getFrameLastUpdated() const { return m_frameLastUpdated; }
    std::uint32_t getFrameAge() const { return m_frameLastUpdated - m_frameCreated; }

    /// Sets current and previous transforms (a fresh instance: always still). Returns false.
    bool teleport(const Mat4f& objectToWorld);
    /// Sets current and previous transforms explicitly; returns prev != current.
    bool teleport(const Mat4f& objectToWorld, const Mat4f& prevObjectToWorld);
    /// Applies `oldToNew` to both current and previous transforms (portal crossings).
    void teleportWithHistory(const Mat4f& oldToNew);
    /// First transform change of a frame: prev = current, current = objectToWorld; returns prev != current.
    bool move(const Mat4f& objectToWorld);
    /// Further change in the same frame: history kept; returns prev != current.
    bool moveAgain(const Mat4f& objectToWorld);

    /// Returns true on the first call in `frameIndex` (resets the per-frame camera registrations).
    bool setFrameLastUpdated(std::uint32_t frameIndex);
    void markForGarbageCollection() const { m_isMarkedForGC = true; }
    bool isMarkedForGC() const { return m_isMarkedForGC; }
    /// Returns true when `cameraType` was not registered yet this frame.
    bool registerCamera(CameraType cameraType);
    bool isCameraRegistered(CameraType cameraType) const;

    CategoryFlags getCategoryFlags() const { return m_categoryFlags; }
    bool isHidden() const { return m_isHidden; }
    bool isStatic() const { return m_isStatic; }
    bool isPreservePath() const { return m_isPreservePath; }
    bool hasMaterialChanged() const { return m_hasMaterialChanged; }
    bool isBlasDirty() const { return m_blasDirty; }
    void clearBlasDirty() { m_blasDirty = false; }
    MaterialType getMaterialType() const { return m_materialType; }
    Hash64 getMaterialHash() const { return m_materialHash; }
    Hash64 getMaterialDataHash() const { return m_materialDataHash; }
    Hash64 getTexcoordHash() const { return m_texcoordHash; }
    Hash64 getIndexHash() const { return m_indexHash; }
    Hash64 getAssociatedGeometryHash() const { return m_associatedGeometryHash; }
    bool isSubsurface() const { return m_isSubsurface; }
    SubsurfaceKind getSubsurfaceKind() const { return m_subsurfaceKind; }
    const std::shared_ptr<const std::vector<Mat4f>>& getInstancesToObject() const { return m_instancesToObject; }
    ReplacementInstance* getReplacementInstance() const { return m_owner; }
    /// PrimInstanceOwner::setReplacementInstance (non-owning back-pointer; null detaches).
    void setReplacementInstance(ReplacementInstance* owner) { m_owner = owner; }

    /// RtInstance::calculateAntiCullingHash: XXH3 of the world position, seeded into the legacy material hash,
    /// then (rtx.antiCulling.object.hashInstanceWithBoundingBoxHash) the BLAS input's bounding box hash.
    /// 0 when object anti-culling is off.
    Hash64 calculateAntiCullingHash() const;

private:
    friend class InstanceManager;
    void onTransformChanged();

    const std::uint64_t m_id;
    mutable std::uint32_t m_vectorIdx;
    mutable std::uint32_t m_frameLastUpdated = kInvalidFrame;
    std::uint32_t m_frameCreated = kInvalidFrame;
    std::uint32_t m_seenCameraTypes = 0;
    Mat4f m_objectToWorld = identityMatrix();
    Mat4f m_prevObjectToWorld = identityMatrix();
    std::array<float, 9> m_normalObjectToWorld{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::shared_ptr<const std::vector<Mat4f>> m_instancesToObject;
    BlasEntry* m_linkedBlas = nullptr;
    ReplacementInstance* m_owner = nullptr;
    CategoryFlags m_categoryFlags;
    MaterialType m_materialType = MaterialType::Opaque;
    SubsurfaceKind m_subsurfaceKind = SubsurfaceKind::None;
    Hash64 m_materialHash = hash::kEmptyHash;
    Hash64 m_materialDataHash = hash::kEmptyHash;
    Hash64 m_texcoordHash = hash::kEmptyHash;
    Hash64 m_indexHash = hash::kEmptyHash;
    Hash64 m_associatedGeometryHash = hash::kEmptyHash;
    mutable bool m_isMarkedForGC = false;
    bool m_isHidden = false;
    bool m_isStatic = false;
    bool m_isPreservePath = false;
    bool m_hasMaterialChanged = false;
    bool m_isSubsurface = false;
    bool m_blasDirty = true;
};

/// What processSceneObject / preserveInstance did to an instance.
struct InstanceUpdate {
    bool created = false;             ///< addInstance ran
    bool firstUpdateThisFrame = false;
    bool hasTransformChanged = false;
    bool hasPreviousPositions = false; ///< the BLAS was rebuilt / refit this frame (vertex motion)
};

/// Optional notifications (Remix InstanceEventHandler).
struct InstanceEventHandler {
    void* owner = nullptr;
    std::function<void(RtInstance&)> onInstanceAdded;
    /// (instance, draw, material-bound: false on the preserve path, transform changed, previous positions)
    std::function<void(RtInstance&, const SceneDrawInput&, bool, bool, bool)> onInstanceUpdated;
    std::function<void(RtInstance&)> onInstanceDestroyed;
};

class InstanceManager {
public:
    InstanceManager() = default;
    ~InstanceManager();
    InstanceManager(const InstanceManager&) = delete;
    InstanceManager& operator=(const InstanceManager&) = delete;

    const std::vector<RtInstance*>& getInstanceTable() const { return m_instances; }
    std::uint32_t getActiveCount() const { return static_cast<std::uint32_t>(m_instances.size()); }
    /// Bumped on changes that affect acceleration-structure membership or build inputs.
    std::uint64_t getSceneGeneration() const { return m_sceneGeneration; }
    void notifySceneChanged() { ++m_sceneGeneration; }

    void addEventHandler(const InstanceEventHandler& handler) { m_eventHandlers.push_back(handler); }
    void removeEventHandler(void* owner);

    /// processSceneObject: `existingInstance` (the ReplacementInstance's prim) or a new instance, re-linked to
    /// `blas` when the BLAS changed, then updated from the draw.
    RtInstance* processSceneObject(BlasEntry& blas, const SceneDrawInput& drawCall, RtInstance* existingInstance,
                                   std::uint32_t currentFrame, InstanceUpdate* update = nullptr);

    /// The preserve path (SceneManager::preserveInstance + InstanceManager::preserveInstance): no transform
    /// update; the first preserve frame after a dynamic update re-syncs prev = current.
    void preserveInstance(RtInstance& instance, const SceneDrawInput& drawCall, std::uint32_t currentFrame);

    /// Removes the instances marked for garbage collection (swap-remove; vector indices follow).
    void garbageCollection();
    void clear();
    void onFrameEnd() {}

private:
    RtInstance* addInstance(BlasEntry& blas, std::uint32_t currentFrame);
    void updateInstance(RtInstance& instance, const BlasEntry& blas, const SceneDrawInput& drawCall,
                        std::uint32_t currentFrame, InstanceUpdate& update);
    void removeInstance(RtInstance* instance);

    // Start at 1 to avoid using 0 - makes it easier to detect a 0 initialized RtInstance (which is invalid)
    std::uint64_t m_nextInstanceId = 1;
    std::vector<RtInstance*> m_instances;
    std::uint64_t m_sceneGeneration = 0;
    std::vector<InstanceEventHandler> m_eventHandlers;
};

} // namespace fuse::relight::scene::instances
