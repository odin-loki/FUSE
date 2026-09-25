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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c (submitDrawState's tracking and
// preserve / dynamic paths without replacements, processDrawCallState, garbageCollection, the
// uniqueObjectDistance spatial-map rebuild of onFrameEnd, the anti-culling support check, the thin-opaque /
// SSS retain-release counts) and src/dxvk/rtx_render/rtx_accel_manager.cpp@0867d3c (addPointInstancerBlas:
// the per-frame point instancer batches, N reserved surface slots each, plus an SSS batch for subsurface
// instancers). FUSE assigns surface slots in instance-table order over the instances drawn this frame (one
// slot each, N for an instancer); the TLAS types and the 21-bit surface index clamp belong to RL-3.x.
//
// SceneModel is the CPU scene of Relight (plan §1.5): feed it every committed draw of a frame
// (submitDraw), then endFrame at Present. Per draw it answers which instance the draw is (a stable id that
// survives motion within rtx.uniqueObjectDistance), its BLAS, and the instance's current and previous
// objectToWorld (motion vectors); per frame it garbage-collects (anti-culling aware), keeps the foliage
// (thin-opaque) and SSS instance counts, and expands point instancers.
//
// Frame numbering is the model's own (0 for the first frame, +1 per endFrame), as Remix's device frame id.
#pragma once

#include <fuse/relight/scene/camera/camera_manager.hpp>
#include <fuse/relight/scene/instances/anti_culling.hpp>
#include <fuse/relight/scene/instances/draw_call_cache.hpp>
#include <fuse/relight/scene/instances/draw_call_tracker.hpp>
#include <fuse/relight/scene/instances/instance_manager.hpp>
#include <fuse/relight/scene/instances/point_instancer.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fuse::relight::scene::instances {

struct SceneDrawResult {
    std::uint64_t instanceId = 0;          ///< 0: no instance (ignored material)
    std::uint32_t replacementInstanceId = 0;
    std::uint64_t blasId = 0;
    TrackerMatch match = TrackerMatch::New;
    ObjectCacheState cacheState = ObjectCacheState::Invalid; ///< Invalid on the preserve path
    bool preserved = false;       ///< the preserve path ran (instance state reused)
    bool created = false;         ///< a new RtInstance
    bool hasTransformChanged = false;
    bool isStatic = false;
    Mat4f objectToWorld = identityMatrix();
    Mat4f prevObjectToWorld = identityMatrix();
};

struct SceneFrameStats {
    std::uint32_t frame = 0;
    std::uint32_t draws = 0;
    std::uint32_t preserved = 0;
    std::uint32_t createdInstances = 0;
    std::uint32_t activeInstances = 0;        ///< after garbage collection
    std::uint32_t replacementInstances = 0;   ///< after garbage collection
    std::uint32_t blasEntries = 0;            ///< after garbage collection
    TrackerGcStats gc;
    bool antiCullingSupported = false;
    std::uint32_t thinOpaqueInstances = 0;    ///< foliage (thin opaque) instances alive
    std::uint32_t sssInstances = 0;           ///< diffusion-profile SSS instances alive
    std::uint32_t pointInstances = 0;         ///< point instancer entries this frame
    std::uint32_t visiblePointInstances = 0;
};

class SceneModel {
public:
    SceneModel();
    ~SceneModel();
    SceneModel(const SceneModel&) = delete;
    SceneModel& operator=(const SceneModel&) = delete;

    /// SceneManager::submitDrawState for a draw without replacements.
    SceneDrawResult submitDraw(const SceneDrawInput& draw);

    /// End of frame (prepareSceneData + onFrameEnd): point instancer batches of the instances drawn this frame,
    /// garbage collection (BLAS cache, tracker with anti-culling, instances), the anti-culling support check
    /// (`mainCamera`: the main camera, null when there is none) and the uniqueObjectDistance rebuild.
    SceneFrameStats endFrame(const CameraState* mainCamera);

    void clear();

    std::uint32_t currentFrame() const { return m_frame; }
    const InstanceManager& instances() const { return m_instances; }
    const DrawCallTracker& tracker() const { return m_tracker; }
    const DrawCallCache& blasCache() const { return m_cache; }
    /// The point instancer batches of the last endFrame, and their expansion.
    const std::vector<PointInstancerBatch>& pointInstancerBatches() const { return m_pointBatches; }
    const std::vector<PointInstance>& pointInstances() const { return m_pointInstances; }

    /// SceneManager::isThinOpaqueMaterialExist (foliage) and the SSS count.
    bool isThinOpaqueMaterialExist() const { return m_thinOpaqueCount > 0; }
    std::uint32_t thinOpaqueCount() const { return m_thinOpaqueCount; }
    std::uint32_t sssCount() const { return m_sssCount; }
    /// Retain / release mismatches caught (upstream logs and asserts; FUSE counts them).
    std::uint32_t subsurfaceUnderflows() const { return m_subsurfaceUnderflows; }

private:
    void retainSubsurface(SubsurfaceKind kind);
    void releaseSubsurface(SubsurfaceKind kind);

    DrawCallCache m_cache;
    DrawCallTracker m_tracker;
    InstanceManager m_instances;
    std::uint32_t m_frame = 0;
    float m_uniqueObjectSearchDistance = 1.f;
    bool m_isAntiCullingSupported = false;
    SceneFrameStats m_stats;
    std::vector<PointInstancerBatch> m_pointBatches;
    std::vector<PointInstance> m_pointInstances;
    std::unordered_map<std::uint64_t, SubsurfaceKind> m_retainedSubsurface;
    std::uint32_t m_thinOpaqueCount = 0;
    std::uint32_t m_sssCount = 0;
    std::uint32_t m_subsurfaceUnderflows = 0;
};

} // namespace fuse::relight::scene::instances
