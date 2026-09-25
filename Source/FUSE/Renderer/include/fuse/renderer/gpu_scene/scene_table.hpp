#pragma once

// WP-1.1 GPU scene: CPU mirror tables with dirty-row tracking (Vulkan-free; builds and unit-tests in
// the stub backend).
//
// A table is the CPU mirror of one persistent GPU buffer: `capacity` rows of a POD record, of which
// [0, count) are meaningful. Writes go through write(), which compares the new record with the
// mirror byte for byte and marks the row dirty only when it changed. commit (GpuScene) turns the
// dirty rows into sorted, coalesced ranges and uploads just those bytes, so upload volume scales
// with the rows that changed, not with the table.
//
// Allocation rules (renderer B2.11 zero-alloc frame): every per-row structure (mirror rows, dirty
// flags, dirty list, range list) is sized to `capacity` when the table grows; mark / write / clear /
// buildRanges never touch the heap. Growth (reserve / ensureCount beyond capacity) allocates and is
// expected only while a scene is being populated.
//
// Kernels: DirtyView exposes the flag / list / counter storage so a single-source kernel
// (gpu_scene_extract_kernel.hpp) can mark rows from worker threads: a row is claimed by exactly one
// work item, the list slot comes from kernel::global_atomic_add, and buildRanges sorts the list, so
// the result is independent of the backend and of thread timing.

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <cstring>
#include <type_traits>
#include <vector>

namespace fuse::renderer::gpu_scene {

/// Rows [first, first + count) of one table.
struct RowRange {
    u32 first = 0;
    u32 count = 0;
};

/// Raw dirty-set storage for kernels (see file comment).
struct DirtyView {
    kernel::Span<u8> flags; ///< 1 = row already listed
    kernel::Span<u32> list; ///< listed rows (unsorted), capacity == flags.size
    u32* count = nullptr;   ///< rows listed

    /// Marks `row` (claimed by the calling work item only). Returns true when newly listed.
    FUSE_HOST_DEVICE bool mark(u32 row) const {
        if (flags[row] != 0u) {
            return false;
        }
        flags[row] = 1u;
        const u32 slot = kernel::global_atomic_add(count, 1u);
        list[slot] = row;
        return true;
    }
};

/// Set of dirty rows: O(1) mark, O(dirty) clear, deterministic sorted ranges.
class DirtySet {
public:
    /// Grows the set to `capacity` rows (keeps marks). Allocates only when capacity grows.
    void resize(u32 capacity);
    u32 capacity() const { return static_cast<u32>(m_flags.size()); }

    bool mark(u32 row);
    bool isMarked(u32 row) const { return row < m_flags.size() && m_flags[row] != 0u; }
    u32 count() const { return m_count; }
    const u32* rows() const { return m_list.data(); }
    void clear();

    DirtyView view();

    /// Sorted, coalesced ranges of the dirty rows. Two runs separated by at most `mergeGapRows`
    /// clean rows are merged (the clean rows in between are re-uploaded unchanged: one copy region
    /// instead of two). `highWater` bounds a dense scan: when more than 1/16 of [0, highWater) is
    /// dirty the flags are scanned linearly instead of sorting the list; both give the same ranges.
    /// `out` is cleared first; it never grows beyond capacity() (reserved by resize()).
    void buildRanges(std::vector<RowRange>& out, u32 mergeGapRows, u32 highWater);
    /// Same result through the sort path only (test oracle for the dense scan).
    void buildRangesSorted(std::vector<RowRange>& out, u32 mergeGapRows);

private:
    std::vector<u8> m_flags;
    std::vector<u32> m_list;
    u32 m_count = 0;
};

/// Type-erased view of a mirror table for the uploader.
struct TableBytes {
    const u8* data = nullptr;
    u32 stride = 0;
    u32 count = 0;
    u32 capacity = 0;
};

/// CPU mirror of one GPU table of `T` rows.
template <typename T>
class MirrorTable {
    static_assert(std::is_trivially_copyable_v<T>, "GPU scene rows must be trivially copyable");

public:
    /// Grows capacity (never shrinks). New rows hold T{} on the CPU but are unknown on the GPU:
    /// they are marked dirty when ensureCount() brings them into [0, count).
    void reserve(u32 capacity) {
        if (capacity <= m_rows.size()) {
            return;
        }
        m_rows.resize(capacity);
        m_dirty.resize(capacity);
        m_ranges.reserve(capacity);
    }

    /// Raises count to at least `count` (growing capacity to the next power of two when needed).
    /// Rows entering [0, count) are marked dirty: the GPU has never seen them. Returns true when the
    /// capacity changed (the GPU buffer must be reallocated and fully re-uploaded).
    bool ensureCount(u32 count) {
        bool grew = false;
        if (count > m_rows.size()) {
            u32 cap = m_rows.empty() ? 64u : static_cast<u32>(m_rows.size());
            while (cap < count) {
                cap *= 2u;
            }
            reserve(cap);
            grew = true;
        }
        for (u32 row = m_count; row < count; ++row) {
            m_dirty.mark(row);
        }
        if (count > m_count) {
            m_count = count;
        }
        return grew;
    }

    /// Writes `value` to `row` (< count). Marks the row dirty only when its bytes changed.
    bool write(u32 row, const T& value) {
        T& dst = m_rows[row];
        if (std::memcmp(&dst, &value, sizeof(T)) == 0) {
            return false;
        }
        dst = value;
        m_dirty.mark(row);
        return true;
    }

    const T& operator[](u32 row) const { return m_rows[row]; }
    u32 count() const { return m_count; }
    u32 capacity() const { return static_cast<u32>(m_rows.size()); }
    DirtySet& dirty() { return m_dirty; }
    const DirtySet& dirty() const { return m_dirty; }
    std::vector<RowRange>& ranges() { return m_ranges; }

    /// Kernel access: rows [0, capacity) and the dirty storage.
    kernel::Span<T> span() { return kernel::Span<T>{m_rows.data(), static_cast<u32>(m_rows.size())}; }
    TableBytes bytes() const {
        return TableBytes{reinterpret_cast<const u8*>(m_rows.data()), static_cast<u32>(sizeof(T)), m_count,
                          static_cast<u32>(m_rows.size())};
    }

private:
    std::vector<T> m_rows;
    DirtySet m_dirty;
    std::vector<RowRange> m_ranges;
    u32 m_count = 0;
};

/// Stable slot allocator with generations (instances, lights). A released slot is parked until
/// the next beginFrame() so it is never reused within the frame that freed it; the GPU therefore
/// always sees at least one frame of the "free" record before a new occupant.
class SlotAllocator {
public:
    struct Handle {
        u32 slot = 0xFFFFFFFFu;
        u32 generation = 0;
        bool valid() const { return slot != 0xFFFFFFFFu; }
        bool operator==(const Handle& o) const { return slot == o.slot && generation == o.generation; }
        bool operator!=(const Handle& o) const { return !(*this == o); }
    };

    /// Pre-sizes the bookkeeping for `capacity` slots (the only call that allocates besides growth).
    void reserve(u32 capacity);
    /// Lowest free slot is not guaranteed: LIFO reuse of slots freed before the current frame, else
    /// the high-water mark. Returns an invalid handle only on u32 exhaustion.
    Handle allocate();
    bool release(Handle handle);
    bool alive(Handle handle) const;
    /// Moves slots released during the previous frame onto the free list.
    void beginFrame();

    u32 highWater() const { return m_highWater; }
    u32 live() const { return m_live; }
    u32 generation(u32 slot) const { return slot < m_generations.size() ? m_generations[slot] : 0u; }

private:
    std::vector<u32> m_generations; ///< per slot; odd = live, even = free
    std::vector<u32> m_free;
    std::vector<u32> m_parked;
    u32 m_highWater = 0;
    u32 m_live = 0;
};

} // namespace fuse::renderer::gpu_scene
