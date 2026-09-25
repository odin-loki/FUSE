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
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (copyIndices) and
// src/util/util_memoization.h@0867d3c (MemoryRegionMemoizer::invalidate).
#include <fuse/relight/capture/geometry/index_rebase.hpp>

#include <fuse/relight/hash/geometry_hash.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace fuse::relight::capture::geometry {

namespace {

template <typename T>
RebasedIndicesPtr rebaseTyped(const std::uint8_t* src, std::uint32_t indexCount) {
    // fast::findMinMax
    T minValue = std::numeric_limits<T>::max();
    T maxValue = 0;
    for (std::uint32_t i = 0; i < indexCount; ++i) {
        T v;
        std::memcpy(&v, src + std::size_t(i) * sizeof(T), sizeof(T));
        minValue = std::min(minValue, v);
        maxValue = std::max(maxValue, v);
    }
    // fast::copySubtract (or memcpy when the minimum is 0)
    std::vector<std::uint8_t> bytes(std::size_t(indexCount) * sizeof(T));
    if (minValue == 0) {
        std::memcpy(bytes.data(), src, bytes.size());
    } else {
        for (std::uint32_t i = 0; i < indexCount; ++i) {
            T v;
            std::memcpy(&v, src + std::size_t(i) * sizeof(T), sizeof(T));
            v = T(v - minValue);
            std::memcpy(bytes.data() + std::size_t(i) * sizeof(T), &v, sizeof(T));
        }
    }
    return std::make_shared<RebasedIndices>(std::uint32_t(sizeof(T)), indexCount, std::uint32_t(minValue),
                                            std::uint32_t(maxValue), std::move(bytes));
}

} // namespace

std::uint32_t RebasedIndices::at(std::uint32_t i) const noexcept {
    if (m_indexSize == 2) {
        std::uint16_t v;
        std::memcpy(&v, m_bytes.data() + std::size_t(i) * 2, 2);
        return v;
    }
    std::uint32_t v;
    std::memcpy(&v, m_bytes.data() + std::size_t(i) * 4, 4);
    return v;
}

const std::vector<std::uint32_t>& RebasedIndices::uniqueIndices() const {
    std::call_once(m_uniqueOnce, [this] {
        m_unique = hash::sortedUniqueIndices(m_bytes.data(), m_indexCount, m_indexSize, m_maxIndex - m_minIndex);
    });
    return m_unique;
}

hash::Hash64 RebasedIndices::indicesHash() const {
    std::call_once(m_indicesOnce, [this] { m_indicesHash = hash::hashContiguousMemory(m_bytes.data(), m_bytes.size()); });
    return m_indicesHash;
}

hash::Hash64 RebasedIndices::legacyIndicesHash() const {
    std::call_once(m_legacyOnce,
                   [this] { m_legacyHash = hash::hashIndicesLegacy(m_bytes.data(), m_indexCount, m_indexSize); });
    return m_legacyHash;
}

RebasedIndicesPtr rebaseIndices(const void* indices, std::uint32_t indexCount, std::uint32_t indexSize) {
    const auto* src = static_cast<const std::uint8_t*>(indices);
    return indexSize == 2 ? rebaseTyped<std::uint16_t>(src, indexCount) : rebaseTyped<std::uint32_t>(src, indexCount);
}

void IndexRangeMemoizer::invalidate(std::size_t start, std::size_t size) {
    const Range invalid{start, start + size};
    auto it = m_cache.lower_bound(start);
    if (it != m_cache.begin()) {
        --it;
    }
    while (it != m_cache.end() && it->second.range.start < invalid.end) {
        if (it->second.range.overlaps(invalid)) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace fuse::relight::capture::geometry
