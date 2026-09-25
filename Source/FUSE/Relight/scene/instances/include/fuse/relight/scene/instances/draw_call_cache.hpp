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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_draw_call_cache.{h,cpp}@0867d3c (DrawCallCache::get) and
// src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c (processGeometryInfo's build / refit / instance-only
// decision, onSceneObjectAdded / onSceneObjectUpdated, the BlasEntry garbage collection).
//
// The BLAS cache: BlasEntries bucketed by the draw's topological hash (indices + geometry descriptor), with
// stable addresses until garbage collection. A draw reuses the entry of an exact match (sky-ness, legacy
// material hash, full geometry hash, bone hash) or, if the entry was not touched this frame, a similar one
// (same vertex data + bones, or same material); buckets with several entries pick the best score
// (positions + bones, texcoords, material: 1000 each, minus the squared distance to the entry's first
// instance), else a new entry is allocated.
//
// FUSE: an entry carries the draw it was last built from (SceneDrawInput) and the geometry hashes of its
// "modified geometry" in place of GPU buffers; the ids are FUSE's (for tools and tests).
#pragma once

#include <fuse/relight/scene/instances/scene_draw.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fuse::relight::scene::instances {

class RtInstance;

/// SceneManager::ObjectCacheState.
enum class ObjectCacheState : std::uint8_t {
    BuildBVH = 0,    ///< new geometry
    UpdateBVH,       ///< same topology, vertex data changed (refit)
    UpdateInstance,  ///< geometry unchanged: only the instance
    Invalid,
};
const char* objectCacheStateName(ObjectCacheState s);

struct BlasEntry {
    std::uint64_t id = 0;           ///< FUSE: creation order, from 1
    SceneDrawInput input;           ///< the draw last processed into this entry
    hash::GeometryHashes modifiedHashes; ///< RaytraceGeometry::hashes
    Hash64 lastBoneHash = hash::kEmptyHash;
    std::uint32_t frameCreated = 0;
    std::uint32_t frameLastTouched = kInvalidFrame;
    std::uint32_t frameLastUpdated = kInvalidFrame; ///< last BuildBVH / UpdateBVH
    /// RaytraceGeometry::previousPositionBuffer.defined(): the last processing was a refit (UpdateBVH), so
    /// last frame's vertex positions exist for vertex motion.
    bool previousPositionsDefined = false;
    std::vector<RtInstance*> linkedInstances;

    static constexpr std::uint32_t kInvalidFrame = 0xffffffffu;

    void linkInstance(RtInstance* instance);
    void unlinkInstance(RtInstance* instance);
};

class DrawCallCache {
public:
    enum class CacheState : std::uint8_t { New = 0, Existed = 1 };

    /// DrawCallCache::get: the entry for `draw` (allocated when none fits).
    CacheState get(const SceneDrawInput& draw, std::uint32_t currentFrame, BlasEntry** out);

    /// processGeometryInfo<isNew> + onSceneObjectAdded / onSceneObjectUpdated: records the draw into the entry
    /// and returns what the BVH needs. Sets frameLastUpdated on BuildBVH / UpdateBVH and frameLastTouched.
    ObjectCacheState process(BlasEntry& blas, CacheState state, const SceneDrawInput& draw, std::uint32_t currentFrame);

    /// SceneManager::garbageCollection's BlasEntry pass: entries untouched for rtx.numFramesToKeepBLAS frames
    /// and with no linked instance are erased.
    void garbageCollection(std::uint32_t currentFrame);

    void clear() { m_entries.clear(); }
    std::size_t size() const { return m_entries.size(); }

    using MultimapType = std::unordered_multimap<Hash64, BlasEntry>;
    const MultimapType& getEntries() const { return m_entries; }

private:
    BlasEntry* allocateEntry(Hash64 hash, const SceneDrawInput& draw, std::uint32_t currentFrame);

    MultimapType m_entries;
    std::uint64_t m_nextId = 1;
};

} // namespace fuse::relight::scene::instances
