// Cluster-page residency manager (WP-0.8): budgeted LRU streaming with priorities and a
// coarse-LOD guarantee. Pure logic, no Vulkan types, no heap, bounded loops.
//
// Pages form a forest (virtual-geometry cluster hierarchy): a page may only be Loading or
// Resident while its parent is Resident, and a page with Loading/Resident children cannot be
// evicted. Pages registered as `coarse` (the always-available coarse LOD; their parent must be
// coarse too) are requested implicitly every update with top priority and are never evicted.
// register_page() refuses a coarse page that would push the coarse total over the budget,
// and set_budget() refuses a budget below it, so the coarse LOD always fits.
//
// Per frame: begin_frame(frame) -> request(id, priority)* -> update(maxLoads) -> the caller
// issues IO for load_id(0..load_count()-1), frees GPU memory for evict_id(...), and later
// reports complete_load(id) / fail_load(id).
// request() raises the priority of all ancestors to at least the child's and keeps Resident
// pages hot in the LRU. update() sorts non-resident candidates (coarse first, then priority
// desc, depth asc, id asc), and for each one whose parent is Resident, evicts LRU pages not
// requested this frame (coarse candidates may also evict requested non-coarse pages) until
// it fits in the budget. The budget covers Resident + Loading bytes and is never exceeded.
#pragma once

#include "fuse/core_logic/cl_common.hpp"

namespace fuse {
namespace core_logic {

enum class ResidencyState : uint8_t { Unregistered = 0, NotResident, Loading, Resident };

struct ResidencyUpdateStats {
    uint32_t candidates;   // non-resident pages considered
    uint32_t loads;        // loads issued (Loading)
    uint32_t evictions;
    uint32_t deferred;     // candidates that did not fit / parent not resident / over maxLoads
};

template <uint32_t kMaxPages, uint32_t kMaxRequests>
class ClusterResidency {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint32_t kCoarsePriority = 0xFFFFFFFFu;

    void reset(uint64_t budgetBytes) {
        for (uint32_t i = 0u; i < kMaxPages; ++i) {
            state_[i] = ResidencyState::Unregistered;
            coarse_[i] = 0u;
            size_[i] = 0u;
            parent_[i] = kNone;
            depth_[i] = 0u;
            activeChildren_[i] = 0u;
            priority_[i] = 0u;
            requestedFrame_[i] = kNone;
            prev_[i] = kNone;
            next_[i] = kNone;
        }
        budget_ = budgetBytes;
        residentBytes_ = 0u;
        loadingBytes_ = 0u;
        coarseBytes_ = 0u;
        head_ = kNone;
        tail_ = kNone;
        lruCount_ = 0u;
        frame_ = 0u;
        requestCount_ = 0u;
        loadCount_ = 0u;
        evictCount_ = 0u;
        coarseCount_ = 0u;
    }

    ClStatus register_page(uint32_t id, uint64_t sizeBytes, uint32_t parent, bool coarse) {
        if (id >= kMaxPages || state_[id] != ResidencyState::Unregistered || sizeBytes == 0u) {
            return ClStatus::InvalidArgument;
        }
        if (parent != kNone) {
            if (parent >= kMaxPages || state_[parent] == ResidencyState::Unregistered) {
                return ClStatus::InvalidArgument;
            }
            if (coarse && coarse_[parent] == 0u) {
                return ClStatus::InvalidArgument; // coarse LOD must be closed under parents
            }
        }
        if (coarse && coarseBytes_ + sizeBytes > budget_) {
            return ClStatus::OutOfBudget;
        }
        state_[id] = ResidencyState::NotResident;
        size_[id] = sizeBytes;
        parent_[id] = parent;
        depth_[id] = parent == kNone ? 0u : depth_[parent] + 1u;
        if (coarse) {
            coarse_[id] = 1u;
            coarseBytes_ += sizeBytes;
            coarseList_[coarseCount_++] = id;
        }
        return ClStatus::Ok;
    }

    ClStatus begin_frame(uint32_t frame) {
        if (frame <= frame_ || frame == kNone) {
            return ClStatus::InvalidArgument;
        }
        frame_ = frame;
        requestCount_ = 0u;
        return ClStatus::Ok;
    }

    ClStatus request(uint32_t id, uint32_t priority) {
        if (id >= kMaxPages || state_[id] == ResidencyState::Unregistered) {
            return ClStatus::InvalidArgument;
        }
        // Parents are registered before children, so the ancestor chain is acyclic and at
        // most kMaxPages long.
        uint32_t cur = id;
        while (cur != kNone) {
            if (requestedFrame_[cur] != frame_) {
                if (requestCount_ >= kMaxRequests) {
                    return ClStatus::CapacityExceeded;
                }
                requestedFrame_[cur] = frame_;
                priority_[cur] = 0u;
                requests_[requestCount_++] = cur;
                if (state_[cur] == ResidencyState::Resident) {
                    lru_unlink(cur);
                    lru_push_front(cur);
                }
            }
            if (priority_[cur] < priority) {
                priority_[cur] = priority;
            }
            cur = parent_[cur];
        }
        return ClStatus::Ok;
    }

    ResidencyUpdateStats update(uint32_t maxLoads) {
        ResidencyUpdateStats st;
        st.candidates = 0u;
        st.loads = 0u;
        st.evictions = 0u;
        st.deferred = 0u;
        loadCount_ = 0u;
        evictCount_ = 0u;
        uint32_t n = 0u;
        for (uint32_t i = 0u; i < coarseCount_; ++i) {
            const uint32_t id = coarseList_[i];
            if (state_[id] == ResidencyState::NotResident) {
                cand_[n++] = id;
            }
        }
        for (uint32_t i = 0u; i < requestCount_; ++i) {
            const uint32_t id = requests_[i];
            if (state_[id] == ResidencyState::NotResident && coarse_[id] == 0u) {
                cand_[n++] = id;
            }
        }
        st.candidates = n;
        // Insertion sort (bounded by n <= kMaxPages).
        for (uint32_t i = 1u; i < n; ++i) {
            const uint32_t key = cand_[i];
            uint32_t j = i;
            while (j > 0u && before(key, cand_[j - 1u])) {
                cand_[j] = cand_[j - 1u];
                --j;
            }
            cand_[j] = key;
        }
        for (uint32_t i = 0u; i < n; ++i) {
            const uint32_t id = cand_[i];
            const uint32_t par = parent_[id];
            if (loadCount_ >= maxLoads || (par != kNone && state_[par] != ResidencyState::Resident)) {
                ++st.deferred;
                continue;
            }
            const bool allowRequested = coarse_[id] != 0u;
            // Walks the LRU once from the tail (bounded by its length <= kMaxPages).
            uint32_t cursor = tail_;
            while (cursor != kNone && !fits(size_[id])) {
                const uint32_t victim = cursor;
                cursor = prev_[cursor];
                if (evictable(victim, allowRequested)) {
                    evict(victim);
                    ++st.evictions;
                }
            }
            if (!fits(size_[id])) {
                ++st.deferred;
                continue;
            }
            state_[id] = ResidencyState::Loading;
            loadingBytes_ += size_[id];
            if (par != kNone) {
                ++activeChildren_[par];
            }
            loads_[loadCount_++] = id;
            ++st.loads;
        }
        return st;
    }

    ClStatus complete_load(uint32_t id) {
        if (id >= kMaxPages || state_[id] != ResidencyState::Loading) {
            return ClStatus::NotFound;
        }
        state_[id] = ResidencyState::Resident;
        loadingBytes_ -= size_[id];
        residentBytes_ += size_[id];
        lru_push_front(id);
        return ClStatus::Ok;
    }

    ClStatus fail_load(uint32_t id) {
        if (id >= kMaxPages || state_[id] != ResidencyState::Loading) {
            return ClStatus::NotFound;
        }
        state_[id] = ResidencyState::NotResident;
        loadingBytes_ -= size_[id];
        if (parent_[id] != kNone) {
            --activeChildren_[parent_[id]];
        }
        return ClStatus::Ok;
    }

    // Lowers or raises the budget, evicting non-coarse leaves (LRU first, requested or not).
    // Fails with OutOfBudget (budget unchanged) when coarse + in-flight bytes cannot fit.
    // The eviction list then describes this call.
    ClStatus set_budget(uint64_t budgetBytes) {
        evictCount_ = 0u;
        if (budgetBytes < coarseBytes_) {
            return ClStatus::OutOfBudget;
        }
        // Each iteration evicts one resident page, so at most kMaxPages iterations.
        while (residentBytes_ + loadingBytes_ > budgetBytes) {
            uint32_t victim = kNone;
            uint32_t cursor = tail_;
            while (cursor != kNone) {
                if (coarse_[cursor] == 0u && activeChildren_[cursor] == 0u) {
                    victim = cursor;
                    break;
                }
                cursor = prev_[cursor];
            }
            if (victim == kNone) {
                break;
            }
            evict(victim);
        }
        if (residentBytes_ + loadingBytes_ > budgetBytes) {
            return ClStatus::OutOfBudget;
        }
        budget_ = budgetBytes;
        return ClStatus::Ok;
    }

    ResidencyState state(uint32_t id) const { return id < kMaxPages ? state_[id] : ResidencyState::Unregistered; }
    uint64_t budget() const { return budget_; }
    uint64_t resident_bytes() const { return residentBytes_; }
    uint64_t loading_bytes() const { return loadingBytes_; }
    uint64_t coarse_bytes() const { return coarseBytes_; }
    uint32_t load_count() const { return loadCount_; }
    uint32_t load_id(uint32_t i) const { return i < loadCount_ ? loads_[i] : kNone; }
    uint32_t evict_count() const { return evictCount_; }
    uint32_t evict_id(uint32_t i) const { return i < evictCount_ ? evicts_[i] : kNone; }
    bool all_coarse_resident() const {
        for (uint32_t i = 0u; i < coarseCount_; ++i) {
            if (state_[coarseList_[i]] != ResidencyState::Resident) {
                return false;
            }
        }
        return true;
    }

    bool check_invariants() const {
        uint64_t res = 0u;
        uint64_t loading = 0u;
        uint64_t coarse = 0u;
        uint32_t residentCount = 0u;
        uint32_t kids[kMaxPages];
        for (uint32_t i = 0u; i < kMaxPages; ++i) {
            kids[i] = 0u;
        }
        for (uint32_t c = 0u; c < kMaxPages; ++c) {
            const ResidencyState s = state_[c];
            if (parent_[c] != kNone && (s == ResidencyState::Loading || s == ResidencyState::Resident)) {
                ++kids[parent_[c]];
            }
        }
        for (uint32_t i = 0u; i < kMaxPages; ++i) {
            const ResidencyState s = state_[i];
            if (s == ResidencyState::Unregistered) {
                continue;
            }
            if (coarse_[i] != 0u) {
                coarse += size_[i];
            }
            if (kids[i] != activeChildren_[i]) {
                return false;
            }
            if (s == ResidencyState::NotResident) {
                continue;
            }
            if (parent_[i] != kNone && state_[parent_[i]] != ResidencyState::Resident) {
                return false; // orphan: loading/resident page without a resident parent
            }
            if (s == ResidencyState::Loading) {
                loading += size_[i];
            } else {
                res += size_[i];
                ++residentCount;
            }
        }
        if (res != residentBytes_ || loading != loadingBytes_ || coarse != coarseBytes_ ||
            res + loading > budget_ || coarse > budget_) {
            return false;
        }
        uint32_t count = 0u;
        uint32_t prev = kNone;
        uint32_t cur = head_;
        for (uint32_t i = 0u; i < kMaxPages && cur != kNone; ++i) {
            if (state_[cur] != ResidencyState::Resident || prev_[cur] != prev) {
                return false;
            }
            ++count;
            prev = cur;
            cur = next_[cur];
        }
        return cur == kNone && tail_ == prev && count == residentCount && count == lruCount_;
    }

private:
    friend struct ClTestAccess;

    bool fits(uint64_t bytes) const { return residentBytes_ + loadingBytes_ + bytes <= budget_; }

    bool before(uint32_t a, uint32_t b) const {
        if (coarse_[a] != coarse_[b]) {
            return coarse_[a] != 0u;
        }
        const uint32_t pa = coarse_[a] != 0u ? kCoarsePriority : priority_[a];
        const uint32_t pb = coarse_[b] != 0u ? kCoarsePriority : priority_[b];
        if (pa != pb) {
            return pa > pb;
        }
        if (depth_[a] != depth_[b]) {
            return depth_[a] < depth_[b];
        }
        return a < b;
    }

    bool evictable(uint32_t p, bool allowRequested) const {
        return coarse_[p] == 0u && activeChildren_[p] == 0u && (allowRequested || requestedFrame_[p] != frame_);
    }

    void evict(uint32_t p) {
        lru_unlink(p);
        state_[p] = ResidencyState::NotResident;
        residentBytes_ -= size_[p];
        if (parent_[p] != kNone) {
            --activeChildren_[parent_[p]];
        }
        // Evictions are listed since the last update()/set_budget(); each one removes a
        // distinct resident page, so evictCount_ <= kMaxPages.
        evicts_[evictCount_++] = p;
    }

    void lru_unlink(uint32_t p) {
        const uint32_t pr = prev_[p];
        const uint32_t nx = next_[p];
        if (pr != kNone) {
            next_[pr] = nx;
        } else {
            head_ = nx;
        }
        if (nx != kNone) {
            prev_[nx] = pr;
        } else {
            tail_ = pr;
        }
        prev_[p] = kNone;
        next_[p] = kNone;
        --lruCount_;
    }

    void lru_push_front(uint32_t p) {
        prev_[p] = kNone;
        next_[p] = head_;
        if (head_ != kNone) {
            prev_[head_] = p;
        } else {
            tail_ = p;
        }
        head_ = p;
        ++lruCount_;
    }

    ResidencyState state_[kMaxPages];
    uint8_t coarse_[kMaxPages];
    uint64_t size_[kMaxPages];
    uint32_t parent_[kMaxPages];
    uint32_t depth_[kMaxPages];
    uint32_t activeChildren_[kMaxPages];
    uint32_t priority_[kMaxPages];
    uint32_t requestedFrame_[kMaxPages];
    uint32_t prev_[kMaxPages];
    uint32_t next_[kMaxPages];
    uint32_t coarseList_[kMaxPages];
    uint32_t requests_[kMaxRequests];
    uint32_t cand_[kMaxPages];
    uint32_t loads_[kMaxPages];
    uint32_t evicts_[kMaxPages];
    uint64_t budget_;
    uint64_t residentBytes_;
    uint64_t loadingBytes_;
    uint64_t coarseBytes_;
    uint32_t head_;
    uint32_t tail_;
    uint32_t lruCount_;
    uint32_t frame_;
    uint32_t requestCount_;
    uint32_t loadCount_;
    uint32_t evictCount_;
    uint32_t coarseCount_;
};

// 16384 cluster pages (e.g. 128 KiB each = 2 GiB addressable), 4096 requests per frame.
typedef ClusterResidency<16384u, 4096u> ClusterResidencyDefault;

} // namespace core_logic
} // namespace fuse
