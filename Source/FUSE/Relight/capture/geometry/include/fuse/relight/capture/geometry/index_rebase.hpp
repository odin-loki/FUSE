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
// FUSE Relight RL-1.3: index rebasing with per-index-buffer memoization.
//
// Ported from dxvk-remix @0867d3c (MIT):
//   src/d3d9/d3d9_rtx.cpp        D3D9Rtx::copyIndices / processIndexBuffer (min/max, idx - min)
//   src/util/util_memoization.h  MemoryRegionMemoizer (keyed by the byte range of the index buffer)
//   src/d3d9/d3d9_device.cpp     LockBuffer: D3DLOCK_DISCARD invalidates everything, any other
//                                non-read-only lock invalidates the locked range
// Modifications Copyright (c) 2026 FUSE contributors (MIT).
//
// A memoized result is shared (shared_ptr) between every draw that hits it, so the derived data
// the hash jobs need (sorted unique indices, the indices / legacyindices component hashes) is also
// computed once per memoized range (lazily, thread-safe) instead of once per draw.
#pragma once

#include <fuse/relight/hash/xxh.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace fuse::relight::capture::geometry {

/// upstream RemixIndexBufferMemoizationData: the rebased copy plus the original min / max index.
class RebasedIndices {
public:
    RebasedIndices(std::uint32_t indexSize, std::uint32_t indexCount, std::uint32_t minIndex, std::uint32_t maxIndex,
                   std::vector<std::uint8_t> bytes)
        : m_indexSize(indexSize), m_indexCount(indexCount), m_minIndex(minIndex), m_maxIndex(maxIndex),
          m_bytes(std::move(bytes)) {}

    [[nodiscard]] std::uint32_t indexSize() const noexcept { return m_indexSize; }   ///< 2 or 4
    [[nodiscard]] std::uint32_t indexCount() const noexcept { return m_indexCount; }
    [[nodiscard]] std::uint32_t minIndex() const noexcept { return m_minIndex; }
    [[nodiscard]] std::uint32_t maxIndex() const noexcept { return m_maxIndex; }
    /// idx - minIndex for every index, original width (little endian), indexCount * indexSize bytes.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return m_bytes; }
    /// Rebased index i.
    [[nodiscard]] std::uint32_t at(std::uint32_t i) const noexcept;

    /// deduplicateSortIndices over the rebased indices (maxIndexValue = max - min). Computed once.
    [[nodiscard]] const std::vector<std::uint32_t>& uniqueIndices() const;
    /// XXH3_64bits(rebased bytes): the `indices` component. Computed once.
    [[nodiscard]] hash::Hash64 indicesHash() const;
    /// hashIndicesLegacy over the rebased bytes: the `legacyindices` component. Computed once.
    [[nodiscard]] hash::Hash64 legacyIndicesHash() const;

private:
    std::uint32_t m_indexSize;
    std::uint32_t m_indexCount;
    std::uint32_t m_minIndex;
    std::uint32_t m_maxIndex;
    std::vector<std::uint8_t> m_bytes;

    mutable std::once_flag m_uniqueOnce, m_indicesOnce, m_legacyOnce;
    mutable std::vector<std::uint32_t> m_unique;
    mutable hash::Hash64 m_indicesHash = 0;
    mutable hash::Hash64 m_legacyHash = 0;
};

using RebasedIndicesPtr = std::shared_ptr<const RebasedIndices>;

/// copyIndices<T>: min / max over `indexCount` indices of `indexSize` bytes (2 or 4) and the copy
/// with the minimum subtracted. `indexCount` must be > 0.
[[nodiscard]] RebasedIndicesPtr rebaseIndices(const void* indices, std::uint32_t indexCount, std::uint32_t indexSize);

/// MemoryRegionMemoizer<RemixIndexBufferMemoizationData> for one index buffer. Semantics kept:
/// a lookup hits only on an exact (start, size) match; a miss first invalidates the cached ranges
/// that overlap [start, start + end) (upstream passes the range end as the size, which widens the
/// invalidation; it only affects the hit rate, never a result) and then caches the new result.
class IndexRangeMemoizer {
public:
    template <typename Compute>
    RebasedIndicesPtr memoize(std::size_t start, std::size_t size, Compute&& compute) {
        const Range current{start, start + size};
        if (auto it = m_cache.find(start);
            it != m_cache.end() && it->second.range.start == current.start && it->second.range.end == current.end) {
            ++m_hits;
            return it->second.result;
        }
        ++m_misses;
        invalidate(current.start, current.end);
        RebasedIndicesPtr result = compute(start, size);
        m_cache[start] = Entry{current, result};
        return result;
    }

    /// Drops every cached range overlapping [start, start + size).
    void invalidate(std::size_t start, std::size_t size);
    void invalidateAll() { m_cache.clear(); }

    [[nodiscard]] std::size_t entryCount() const noexcept { return m_cache.size(); }
    [[nodiscard]] std::uint64_t hits() const noexcept { return m_hits; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return m_misses; }

private:
    struct Range {
        std::size_t start = 0;
        std::size_t end = 0;
        [[nodiscard]] bool overlaps(const Range& other) const noexcept {
            return (start <= other.start && other.start < end) || (start < other.end && other.end <= end) ||
                   (other.start <= start && end <= other.end);
        }
    };
    struct Entry {
        Range range;
        RebasedIndicesPtr result;
    };
    std::map<std::size_t, Entry> m_cache;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
};

/// D3DLOCK_* flags that matter for invalidation.
inline constexpr std::uint32_t kD3DLockReadOnly = 0x00000010u;
inline constexpr std::uint32_t kD3DLockDiscard = 0x00002000u;

} // namespace fuse::relight::capture::geometry
