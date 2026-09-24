#pragma once

// WP-5.3 cluster streamer (CPU side, backend-agnostic): the core_logic page residency model
// (core_logic/residency, ClusterPageResidency) driven by the cut's feedback, plus pool-slot bookkeeping
// and the resident-page bitmask the cut kernel reads (stream_cut_kernel.hpp).
//
// Per frame:
//   begin_frame(frame, completedFrame)   releases pool slots evicted by frames the GPU has finished
//   apply_feedback(words) / request(..)  the feedback of an earlier frame's "stream.cut" (or any
//                                        caller-chosen requests): priorities per page + request list
//   update()                             residency update: evictions (bit cleared, slot pending), loads
//                                        (slot assigned) -> load_page(i) / load_slot(i)
//   ... IO: copy file.page_payload(page) into the pool slot ...
//   complete_load(page) / fail_load(page)
// Coarse pages (the terminal groups) are loaded before anything else (core_logic coarse barrier) and never
// evicted; coarse_resident() says when the first frame may render (the GPU wrapper waits for it at init).
//
// Budget: `budget_pages` pool slots of page_bytes each (resident + loading pages), never exceeded.
// Pool slots = budget_pages + reserve_slots: a slot freed by an eviction at frame f is reused only after
// frame f - 1 completed (frames in flight may still read it); a load that finds no free slot fails
// (counted, retried by a later frame).
// Heap: init() allocates everything (the residency model, ~2 MiB, once); the per-frame calls allocate
// nothing.

#include <fuse/core_logic/cluster_page_residency.hpp>
#include <fuse/renderer/geometry_streaming/cluster_page_file.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer::geometry_streaming {

struct StreamerDesc {
    u32 budget_pages = 256;        ///< resident + loading pages (>= coarse pages)
    u32 reserve_slots = 16;        ///< extra pool slots for evictions still in flight
    u32 max_loads_per_frame = 16;
};

struct StreamerStats {
    u32 frame = 0;
    u32 feedback_pages = 0;  ///< pages requested by the last apply_feedback
    u32 candidates = 0;
    u32 loads = 0;
    u32 evictions = 0;
    u32 deferred = 0;
    u32 slot_failures = 0;   ///< loads failed for lack of a free pool slot
    u32 resident_pages = 0;
    u32 loading_pages = 0;
};

class ClusterStreamer {
public:
    typedef core_logic::ClusterPageResidencyDefault Residency;
    static constexpr u32 kMaxPages = 16384u;

    ClusterStreamer() = default;
    ClusterStreamer(const ClusterStreamer&) = delete;
    ClusterStreamer& operator=(const ClusterStreamer&) = delete;

    /// Registers every page of `file` (which must outlive the streamer). Fails when the file has more
    /// than kMaxPages pages / dependency edges or the coarse pages do not fit in budget_pages.
    bool init(const ClusterPageFile& file, const StreamerDesc& desc, std::string* error = nullptr);
    bool valid() const { return m_file != nullptr; }

    bool begin_frame(u32 frame, u32 completedFrame);
    /// Feedback words (stream_kernel::FeedbackLayout for page_count() pages): every listed page is
    /// requested with its priority, in ascending page order (the GPU list order is scheduling-dependent;
    /// sorting keeps the residency deterministic). Returns the number of pages requested.
    u32 apply_feedback(const u32* words, u32 wordCount);
    bool request(u32 page, u32 priority);
    const StreamerStats& update();
    /// Changes the budget (evicting LRU pages when it shrinks; eviction list = this call's).
    bool set_budget_pages(u32 pages);

    u32 load_count() const { return m_loadCount; }
    u32 load_page(u32 i) const { return m_loads[i]; }
    u32 load_slot(u32 i) const { return m_pageSlot[m_loads[i]]; }
    u32 evict_count() const { return m_evictCount; }
    u32 evict_page(u32 i) const { return m_evicts[i]; }

    bool complete_load(u32 page);
    bool fail_load(u32 page);

    u32 page_count() const { return m_pageCount; }
    u32 slot_count() const { return m_slotCount; }
    u32 budget_pages() const { return m_budgetPages; }
    u32 page_bytes() const { return m_pageBytes; }
    bool page_resident(u32 page) const { return page < m_pageCount && ((m_bits[page >> 5u] >> (page & 31u)) & 1u) != 0u; }
    /// Pool slot assigned to a loading or resident page (kPageNone otherwise).
    u32 assigned_slot(u32 page) const { return page < m_pageCount ? m_pageSlot[page] : kPageNone; }
    /// Pool slot of a resident page (kPageNone otherwise).
    u32 resident_slot(u32 page) const { return page < m_pageCount ? m_slotTable[page] : kPageNone; }
    const u32* resident_bits() const { return m_bits.data(); }
    u32 resident_words() const { return static_cast<u32>(m_bits.size()); }
    /// page -> slot of resident pages, kPageNone otherwise (what a page-aware draw path indexes).
    const u32* slot_table() const { return m_slotTable.data(); }
    bool coarse_resident() const { return m_residency->all_coarse_resident(); }
    u32 resident_page_count() const { return m_residentPages; }
    u32 loading_page_count() const { return m_loadingPages; }
    const Residency& residency() const { return *m_residency; }
    const StreamerStats& stats() const { return m_stats; }
    /// Incremented whenever the bitmask / slot table change (GPU copies re-upload on change).
    u64 state_version() const { return m_stateVersion; }

private:
    void setResident(u32 page, bool resident);
    void handleEvictions();

    const ClusterPageFile* m_file = nullptr;
    std::unique_ptr<Residency> m_residency;
    StreamerDesc m_desc{};
    u32 m_pageCount = 0;
    u32 m_pageBytes = 0;
    u32 m_budgetPages = 0;
    u32 m_slotCount = 0;
    u32 m_frame = 0;
    std::vector<u32> m_bits;
    std::vector<u32> m_slotTable; ///< resident pages only
    std::vector<u32> m_pageSlot;  ///< assigned at load issue
    std::vector<u32> m_freeSlots;
    u32 m_freeCount = 0;
    std::vector<u32> m_pendingSlot;  ///< ring: slots evicted, reusable after m_pendingFrame + completion
    std::vector<u32> m_pendingFrame;
    u32 m_pendingHead = 0;
    u32 m_pendingCount = 0;
    std::vector<u32> m_loads;
    std::vector<u32> m_sortScratch; ///< feedback list, sorted (deterministic request order)
    u32 m_loadCount = 0;
    std::vector<u32> m_evicts;
    u32 m_evictCount = 0;
    u32 m_residentPages = 0;
    u32 m_loadingPages = 0;
    u64 m_stateVersion = 0;
    StreamerStats m_stats{};
};

} // namespace fuse::renderer::geometry_streaming
