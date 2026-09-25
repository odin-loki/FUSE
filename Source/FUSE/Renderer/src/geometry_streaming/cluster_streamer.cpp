// WP-5.3 cluster streamer: see include/fuse/renderer/geometry_streaming/cluster_streamer.hpp.
#include <fuse/renderer/geometry_streaming/cluster_streamer.hpp>

#include <fuse/renderer/geometry_streaming/stream_cut_kernel.hpp>

#include <algorithm>

namespace fuse::renderer::geometry_streaming {

using core_logic::ClStatus;
using core_logic::PageState;

namespace {
bool fail(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}
} // namespace

bool ClusterStreamer::init(const ClusterPageFile& file, const StreamerDesc& desc, std::string* error) {
    m_file = nullptr;
    const u32 pages = file.page_count();
    if (pages == 0u || pages > kMaxPages || file.page_deps.size() > 262144u) {
        return fail(error, "page file empty or over the streamer capacity (16384 pages, 262144 dependencies)");
    }
    if (desc.budget_pages == 0u || desc.max_loads_per_frame == 0u) {
        return fail(error, "budget_pages and max_loads_per_frame must be > 0");
    }
    if (m_residency == nullptr) {
        m_residency = std::make_unique<Residency>();
    }
    m_desc = desc;
    m_pageCount = pages;
    m_pageBytes = file.page_bytes;
    m_budgetPages = desc.budget_pages;
    m_residency->reset(static_cast<u64>(desc.budget_pages) * file.page_bytes);
    for (u32 p = 0; p < pages; ++p) {
        const ClusterPageEntry& e = file.pages[p];
        const ClStatus s = m_residency->register_page(p, file.page_bytes, file.page_deps.data() + e.dep_offset, e.dep_count,
                                                      (e.flags & kPageFlagCoarse) != 0u);
        if (s == ClStatus::OutOfBudget) {
            return fail(error, "the coarse pages do not fit in budget_pages");
        }
        if (s != ClStatus::Ok) {
            return fail(error, "page file rejected by the residency model (dependencies not topological?)");
        }
    }
    m_slotCount = desc.budget_pages + desc.reserve_slots;
    m_bits.assign((pages + 31u) / 32u, 0u);
    m_slotTable.assign(pages, kPageNone);
    m_pageSlot.assign(pages, kPageNone);
    m_freeSlots.resize(m_slotCount);
    for (u32 s = 0; s < m_slotCount; ++s) {
        m_freeSlots[s] = m_slotCount - 1u - s; // pop order 0, 1, 2, ...
    }
    m_freeCount = m_slotCount;
    m_pendingSlot.assign(m_slotCount, 0u);
    m_pendingFrame.assign(m_slotCount, 0u);
    m_pendingHead = 0;
    m_pendingCount = 0;
    m_loads.assign(pages, 0u);
    m_sortScratch.assign(pages, 0u);
    m_evicts.assign(pages, 0u);
    m_loadCount = 0;
    m_evictCount = 0;
    m_residentPages = 0;
    m_loadingPages = 0;
    m_frame = 0;
    m_stats = StreamerStats{};
    ++m_stateVersion;
    m_file = &file;
    return true;
}

bool ClusterStreamer::begin_frame(u32 frame, u32 completedFrame) {
    if (m_file == nullptr || m_residency->begin_frame(frame) != ClStatus::Ok) {
        return false;
    }
    m_frame = frame;
    m_stats.frame = frame;
    m_stats.feedback_pages = 0;
    // Slots evicted at frame f were last readable by frame f - 1.
    while (m_pendingCount > 0u && m_pendingFrame[m_pendingHead] <= completedFrame + 1u) {
        m_freeSlots[m_freeCount++] = m_pendingSlot[m_pendingHead];
        m_pendingHead = (m_pendingHead + 1u) % m_slotCount;
        --m_pendingCount;
    }
    return true;
}

bool ClusterStreamer::request(u32 page, u32 priority) {
    return m_file != nullptr && m_residency->request(page, priority) == ClStatus::Ok;
}

u32 ClusterStreamer::apply_feedback(const u32* words, u32 wordCount) {
    const stream_kernel::FeedbackLayout l = stream_kernel::feedback_layout(m_pageCount);
    if (m_file == nullptr || words == nullptr || wordCount < l.words) {
        return 0u;
    }
    const u32 count = words[stream_kernel::kFeedbackRequestCount] < m_pageCount ? words[stream_kernel::kFeedbackRequestCount]
                                                                                 : m_pageCount;
    // The GPU appends the list with atomics (scheduling order); requests touch the LRU in call order, so
    // apply them in ascending page order: the residency then evolves deterministically.
    std::copy(words + l.list, words + l.list + count, m_sortScratch.begin());
    std::sort(m_sortScratch.begin(), m_sortScratch.begin() + count);
    u32 applied = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 page = m_sortScratch[i];
        if (page < m_pageCount && (i == 0u || page != m_sortScratch[i - 1u]) && words[l.priorities + page] != 0u &&
            request(page, words[l.priorities + page])) {
            ++applied;
        }
    }
    m_stats.feedback_pages = applied;
    return applied;
}

void ClusterStreamer::setResident(u32 page, bool resident) {
    if (resident) {
        m_bits[page >> 5u] |= 1u << (page & 31u);
        m_slotTable[page] = m_pageSlot[page];
    } else {
        m_bits[page >> 5u] &= ~(1u << (page & 31u));
        m_slotTable[page] = kPageNone;
    }
    ++m_stateVersion;
}

void ClusterStreamer::handleEvictions() {
    m_evictCount = m_residency->evict_count();
    for (u32 i = 0; i < m_evictCount; ++i) {
        const u32 page = m_residency->evict_id(i);
        m_evicts[i] = page;
        setResident(page, false);
        // Every slot is either free, pending or assigned, so the ring never overflows.
        const u32 at = (m_pendingHead + m_pendingCount) % m_slotCount;
        m_pendingSlot[at] = m_pageSlot[page];
        m_pendingFrame[at] = m_frame;
        ++m_pendingCount;
        m_pageSlot[page] = kPageNone;
        --m_residentPages;
    }
}

const StreamerStats& ClusterStreamer::update() {
    m_loadCount = 0;
    if (m_file == nullptr) {
        return m_stats;
    }
    const core_logic::PageUpdateStats st = m_residency->update(m_desc.max_loads_per_frame);
    handleEvictions();
    m_stats.candidates = st.candidates;
    m_stats.evictions = st.evictions;
    m_stats.deferred = st.deferred;
    m_stats.slot_failures = 0;
    for (u32 i = 0; i < m_residency->load_count(); ++i) {
        const u32 page = m_residency->load_id(i);
        if (m_freeCount == 0u) {
            m_residency->fail_load(page);
            ++m_stats.slot_failures;
            continue;
        }
        m_pageSlot[page] = m_freeSlots[--m_freeCount];
        m_loads[m_loadCount++] = page;
        ++m_loadingPages;
    }
    m_stats.loads = m_loadCount;
    m_stats.resident_pages = m_residentPages;
    m_stats.loading_pages = m_loadingPages;
    return m_stats;
}

bool ClusterStreamer::set_budget_pages(u32 pages) {
    if (m_file == nullptr || pages > m_slotCount) {
        return false;
    }
    const bool ok = m_residency->set_budget(static_cast<u64>(pages) * m_pageBytes) == ClStatus::Ok;
    handleEvictions();
    if (ok) {
        m_budgetPages = pages;
    }
    return ok;
}

bool ClusterStreamer::complete_load(u32 page) {
    if (m_file == nullptr || m_residency->complete_load(page) != ClStatus::Ok) {
        return false;
    }
    setResident(page, true);
    --m_loadingPages;
    ++m_residentPages;
    return true;
}

bool ClusterStreamer::fail_load(u32 page) {
    if (m_file == nullptr || m_residency->fail_load(page) != ClStatus::Ok) {
        return false;
    }
    m_freeSlots[m_freeCount++] = m_pageSlot[page]; // never visible to the GPU: reusable at once
    m_pageSlot[page] = kPageNone;
    --m_loadingPages;
    return true;
}

} // namespace fuse::renderer::geometry_streaming
