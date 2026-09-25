#include <fuse/renderer/gpu_scene/scene_table.hpp>

#include <algorithm>

namespace fuse::renderer::gpu_scene {

// --- DirtySet ------------------------------------------------------------------------------------

void DirtySet::resize(u32 capacity) {
    if (capacity <= m_flags.size()) {
        return;
    }
    m_flags.resize(capacity, 0u);
    m_list.resize(capacity, 0u);
}

bool DirtySet::mark(u32 row) {
    if (row >= m_flags.size() || m_flags[row] != 0u) {
        return false;
    }
    m_flags[row] = 1u;
    m_list[m_count++] = row;
    return true;
}

void DirtySet::clear() {
    for (u32 i = 0; i < m_count; ++i) {
        m_flags[m_list[i]] = 0u;
    }
    m_count = 0;
}

DirtyView DirtySet::view() {
    DirtyView v{};
    v.flags = kernel::Span<u8>{m_flags.data(), static_cast<u32>(m_flags.size())};
    v.list = kernel::Span<u32>{m_list.data(), static_cast<u32>(m_list.size())};
    v.count = &m_count;
    return v;
}

namespace {

/// Appends `row` to the coalesced range list.
inline void appendRow(std::vector<RowRange>& out, u32 row, u32 mergeGapRows) {
    if (!out.empty()) {
        RowRange& last = out.back();
        const u32 end = last.first + last.count; // one past the last row of the range
        if (row >= end && row - end <= mergeGapRows) {
            last.count = row + 1u - last.first;
            return;
        }
    }
    out.push_back(RowRange{row, 1u});
}

} // namespace

void DirtySet::buildRangesSorted(std::vector<RowRange>& out, u32 mergeGapRows) {
    out.clear();
    std::sort(m_list.begin(), m_list.begin() + m_count);
    for (u32 i = 0; i < m_count; ++i) {
        appendRow(out, m_list[i], mergeGapRows);
    }
}

void DirtySet::buildRanges(std::vector<RowRange>& out, u32 mergeGapRows, u32 highWater) {
    const u32 limit = std::min<u32>(highWater, static_cast<u32>(m_flags.size()));
    // Dense: a linear flag scan (sequential, branch-predictable) beats sorting the list.
    if (m_count > 0u && static_cast<u64>(m_count) * 16u > limit) {
        out.clear();
        u32 seen = 0;
        for (u32 row = 0; row < m_flags.size() && seen < m_count; ++row) {
            if (m_flags[row] != 0u) {
                appendRow(out, row, mergeGapRows);
                ++seen;
            }
        }
        return;
    }
    buildRangesSorted(out, mergeGapRows);
}

// --- SlotAllocator -------------------------------------------------------------------------------

void SlotAllocator::reserve(u32 capacity) {
    m_generations.reserve(capacity);
    m_free.reserve(capacity);
    m_parked.reserve(capacity);
}

SlotAllocator::Handle SlotAllocator::allocate() {
    u32 slot = 0;
    if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
    } else {
        if (m_highWater == 0xFFFFFFFEu) {
            return Handle{};
        }
        slot = m_highWater++;
        m_generations.push_back(0u);
    }
    u32& generation = m_generations[slot];
    generation += 1u; // even (free) -> odd (live)
    ++m_live;
    return Handle{slot, generation};
}

bool SlotAllocator::alive(Handle handle) const {
    return handle.slot < m_generations.size() && m_generations[handle.slot] == handle.generation &&
           (handle.generation & 1u) != 0u;
}

bool SlotAllocator::release(Handle handle) {
    if (!alive(handle)) {
        return false;
    }
    m_generations[handle.slot] += 1u; // odd -> even: every outstanding handle is stale now
    m_parked.push_back(handle.slot);
    --m_live;
    return true;
}

void SlotAllocator::beginFrame() {
    for (u32 slot : m_parked) {
        m_free.push_back(slot);
    }
    m_parked.clear();
}

} // namespace fuse::renderer::gpu_scene
