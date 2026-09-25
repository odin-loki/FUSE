/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/util/util_spatial_map.h@0867d3c (SpatialMap<T>).
//
// A structure to allow for quickly returning data close to a specific position. Entries live in cubic
// cells of `cellSize` (the tracker uses 2 x rtx.uniqueObjectDistance); a nearest query looks at the 2x2x2
// cells around the query point, so every entry within cellSize / 2 is found. Multiple entries may share
// the same transform hash (distinct draw calls at the same position): the transform cache is a multimap
// and callers iterate the colliding entries through a visitor.
//
// Differences from upstream: std::unordered_(multi)map instead of fast_spatial_cache /
// fast_unordered_multimap, and the transform hash takes the D3DMATRIX float[16] (the same 64 bytes as
// Remix's Matrix4). The "invalid cell size" / "missing entry" diagnostics are dropped (upstream logs them
// once and carries on the same way).
#pragma once

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/scene/instances/instance_math.hpp>

#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fuse::relight::scene::instances {

template <class T>
class SpatialMap {
public:
    using Hash64 = hash::Hash64;

    explicit SpatialMap(float cellSize) : m_cellSize(cellSize) {
        if (!(m_cellSize > 0)) {
            // "Invalid cell size in SpatialMap. cellSize must be greater than 0."
            m_cellSize = 1.f;
        }
    }

    static Hash64 transformHash(const Mat4f& transform) { return hash::xxh64(transform.data(), sizeof(Mat4f), 0); }

    /// Iterates all entries at the given transform and calls visitor(data) for each; a visitor returning true
    /// stops the iteration.
    void forEachAtTransform(const Mat4f& transform, const std::function<bool(const T*)>& visitor) const {
        const auto range = m_cache.equal_range(transformHash(transform));
        for (auto it = range.first; it != range.second; ++it) {
            if (visitor(it->second.data)) {
                return;
            }
        }
    }

    /// The entry closest to `centroid` that passes `filter` and is at most sqrt(maxDistSqr) away.
    const T* getNearestData(const Vec3& centroid, float maxDistSqr, float& nearestDistSqr,
                            const std::function<bool(const T*)>& filter) const {
        static constexpr std::array<std::array<int, 3>, 8> kOffsets{{
            {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {1, 0, 1}, {1, 1, 0}, {1, 1, 1},
        }};
        const Vec3 cellPosition = centroid / m_cellSize - Vec3{0.5f, 0.5f, 0.5f};
        const Cell floorPos{int(std::floor(cellPosition.x)), int(std::floor(cellPosition.y)), int(std::floor(cellPosition.z))};

        const T* nearestData = nullptr;
        nearestDistSqr = FLT_MAX;
        for (const auto& offset : kOffsets) {
            const auto cell = m_cells.find(Cell{floorPos.x + offset[0], floorPos.y + offset[1], floorPos.z + offset[2]});
            if (cell == m_cells.end()) {
                continue;
            }
            for (const Entry& entry : cell->second) {
                if (!filter(entry.data)) {
                    continue;
                }
                const float distSqr = lengthSqr(entry.centroid - centroid);
                if (distSqr <= maxDistSqr && distSqr < nearestDistSqr) {
                    nearestDistSqr = distSqr;
                    if (nearestDistSqr == 0.0f) {
                        return entry.data;
                    }
                    nearestData = entry.data;
                }
            }
        }
        return nearestData;
    }

    Hash64 insert(const Vec3& centroid, const Mat4f& transform, const T* data) {
        const Hash64 hash = transformHash(transform);
        m_cache.emplace(hash, Entry{data, centroid, hash});
        m_cells[getCellPos(centroid)].push_back(Entry{data, centroid, hash});
        return hash;
    }

    void erase(Hash64 transformHash, const T* data) {
        const auto range = m_cache.equal_range(transformHash);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.data == data) {
                eraseFromCell(it->second.centroid, transformHash, data);
                m_cache.erase(it);
                return;
            }
        }
        // "Specified entry was missing in SpatialMap::erase()."
    }

    /// Re-files `data` under `newTransform` (and `centroid`) when the transform hash changed; returns the hash.
    Hash64 move(Hash64 oldTransformHash, const Vec3& centroid, const Mat4f& newTransform, const T* data) {
        Hash64 hash = transformHash(newTransform);
        if (oldTransformHash != hash) {
            erase(oldTransformHash, data);
            hash = insert(centroid, newTransform, data);
        }
        return hash;
    }

    void rebuild(float cellSize) {
        m_cellSize = cellSize;
        m_cells.clear();
        for (const auto& pair : m_cache) {
            m_cells[getCellPos(pair.second.centroid)].push_back(pair.second);
        }
    }

    std::size_t size() const { return m_cache.size(); }
    float cellSize() const { return m_cellSize; }

private:
    struct Entry {
        const T* data = nullptr;
        Vec3 centroid;
        Hash64 transformHash = 0;
    };
    struct Cell {
        int x = 0, y = 0, z = 0;
        friend bool operator==(const Cell& a, const Cell& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
    };
    struct CellHash {
        std::size_t operator()(const Cell& c) const {
            const std::int32_t v[3] = {c.x, c.y, c.z};
            return static_cast<std::size_t>(hash::xxh3_64(v, sizeof v));
        }
    };
    struct PassthroughHash {
        std::size_t operator()(Hash64 h) const { return static_cast<std::size_t>(h); }
    };

    Cell getCellPos(const Vec3& position) const {
        const Vec3 scaled = position / m_cellSize;
        return {int(std::floor(scaled.x)), int(std::floor(scaled.y)), int(std::floor(scaled.z))};
    }

    void eraseFromCell(const Vec3& pos, Hash64 hash, const T* data) {
        const auto cellIt = m_cells.find(getCellPos(pos));
        if (cellIt == m_cells.end()) {
            return; // "Specified cell was already empty in SpatialMap::erase()."
        }
        std::vector<Entry>& cell = cellIt->second;
        for (auto it = cell.begin(); it != cell.end(); ++it) {
            if (it->transformHash == hash && it->data == data) {
                if (cell.size() > 1) {
                    std::swap(*it, cell.back());
                    cell.pop_back();
                } else {
                    m_cells.erase(cellIt);
                }
                return;
            }
        }
        // "Couldn't find matching data in SpatialMap::erase()."
    }

    float m_cellSize;
    std::unordered_map<Cell, std::vector<Entry>, CellHash> m_cells;
    std::unordered_multimap<Hash64, Entry, PassthroughHash> m_cache;
};

} // namespace fuse::relight::scene::instances
