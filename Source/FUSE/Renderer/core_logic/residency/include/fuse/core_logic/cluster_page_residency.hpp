// WP-5.3 cluster-page residency over a page DAG: budgeted LRU streaming with cut-driven
// priorities, dependency closure and a coarse-LOD guarantee. Pure logic, no Vulkan types, no heap,
// bounded loops; same CBMC-parsable dialect as the WP-0.8 cores (cl_common.hpp).
//
// Generalises the WP-0.8 ClusterResidency (residency_lru.hpp: one parent per page) to the page
// graph of a cluster DAG (geometry_streaming/cluster_page_file.hpp): a page may depend on several
// earlier pages. For DAG group pages, page P depends on page Q when a group stored in P produced
// clusters that are members of a group stored in Q (Q holds the coarser version of P's surface):
// P's clusters can only replace Q's in a cut when Q is resident too.
//
// Rules:
//   * pages are registered in topological order: ids 0, 1, 2, ... and every dependency of page i
//     is an already registered page with a smaller id (no duplicates), so the graph is acyclic;
//   * Loading / Resident pages have every dependency Resident (closure: "no orphans"), so a
//     resident page's whole coarser neighbourhood is resident and a cut that treats a missing
//     page as "not refinable" stays watertight (stream_cut_kernel.hpp);
//   * a page with a Loading / Resident dependent is never evicted;
//   * coarse pages (the root / terminal LOD; their dependencies must be coarse too) are requested
//     implicitly every update with top priority and never evicted; register_page() refuses a
//     coarse page that would push the coarse total over the budget and set_budget() refuses a
//     budget below it, so the coarse LOD always fits;
//   * the budget covers Resident + Loading bytes and is never exceeded.
//
// Per frame: begin_frame(frame) -> request(id, priority)* -> update(maxLoads) -> the caller issues
// IO for load_id(0..load_count()-1), frees memory for evict_id(...), and later reports
// complete_load(id) / fail_load(id).
// request() marks the page and all its (transitive) dependencies requested this frame, raises
// their priority to at least `priority` and moves resident ones to the LRU front.
// update() orders the non-resident candidates (coarse first, then priority desc, depth asc, id
// asc; heap sort, the order is total), and for each one whose dependencies are all Resident and
// while fewer than maxLoads loads were issued, evicts LRU pages that are evictable (not coarse, no
// active dependent, and either not requested this frame or requested with a LOWER priority than the
// candidate — a coarse candidate may evict any requested page) walking from the LRU tail until it
// fits; requested pages are preempted only when a dry run shows the walk makes it fit (otherwise only
// unrequested pages go); then issues the load or defers the candidate. Priority preemption keeps a full budget converging on the pages the view values most:
// a cut-driven priority is the same log2 error ratio for pages in use (their group's error) and pages
// wanted (stream_cut_kernel.hpp), and a page's dependencies carry at least its priority, so a
// candidate never evicts its own dependencies.
// Coarse barrier: once a coarse candidate is deferred, every non-coarse candidate of that update is
// deferred too, so in-flight / resident non-coarse bytes drain until the coarse LOD fits (liveness
// of the coarse guarantee under any request pattern, budget change or load failure).
//
// Storage: fixed-size members (~70 bytes per page + 4 per dependency edge + 4 per request); the
// default 16384-page configuration is ~1.8 MiB, so allocate it once (static or at init). No heap.
#pragma once

#include "fuse/core_logic/cl_common.hpp"

namespace fuse {
namespace core_logic {

enum class PageState : uint8_t { Unregistered = 0, NotResident, Loading, Resident };

struct PageUpdateStats {
    uint32_t candidates; // non-resident pages considered (coarse + requested)
    uint32_t loads;      // loads issued (Loading)
    uint32_t evictions;
    uint32_t deferred;   // candidates that did not fit / had a dependency not resident / over maxLoads
};

template <uint32_t kMaxPages, uint32_t kMaxDeps, uint32_t kMaxRequests>
class ClusterPageResidency {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint32_t kCoarsePriority = 0xFFFFFFFFu;

    void reset(uint64_t budgetBytes) {
        for (uint32_t i = 0u; i < kMaxPages; ++i) {
            state_[i] = PageState::Unregistered;
            coarse_[i] = 0u;
            size_[i] = 0u;
            depOffset_[i] = 0u;
            depCount_[i] = 0u;
            depth_[i] = 0u;
            dependents_[i] = 0u;
            priority_[i] = 0u;
            requestedFrame_[i] = kNone;
            prev_[i] = kNone;
            next_[i] = kNone;
        }
        budget_ = budgetBytes;
        residentBytes_ = 0u;
        loadingBytes_ = 0u;
        coarseBytes_ = 0u;
        pageCount_ = 0u;
        depTotal_ = 0u;
        head_ = kNone;
        tail_ = kNone;
        lruCount_ = 0u;
        frame_ = 0u;
        requestCount_ = 0u;
        loadCount_ = 0u;
        evictCount_ = 0u;
        coarseCount_ = 0u;
    }

    // `id` must be the next id (page_count()); deps: distinct registered ids (all < id).
    ClStatus register_page(uint32_t id, uint64_t sizeBytes, const uint32_t* deps, uint32_t depCount, bool coarse) {
        if (id != pageCount_ || id >= kMaxPages || sizeBytes == 0u || (depCount > 0u && deps == 0)) {
            return ClStatus::InvalidArgument;
        }
        if (depCount > kMaxDeps - depTotal_) {
            return ClStatus::CapacityExceeded;
        }
        uint32_t depth = 0u;
        for (uint32_t i = 0u; i < depCount; ++i) {
            const uint32_t d = deps[i];
            if (d >= id) {
                return ClStatus::InvalidArgument; // unregistered, self or later page: not topological
            }
            if (coarse && coarse_[d] == 0u) {
                return ClStatus::InvalidArgument; // the coarse LOD must be closed under dependencies
            }
            for (uint32_t j = 0u; j < i; ++j) {
                if (deps[j] == d) {
                    return ClStatus::InvalidArgument; // duplicate edge
                }
            }
            if (depth_[d] + 1u > depth) {
                depth = depth_[d] + 1u;
            }
        }
        if (coarse && coarseBytes_ + sizeBytes > budget_) {
            return ClStatus::OutOfBudget;
        }
        for (uint32_t i = 0u; i < depCount && depTotal_ + i < kMaxDeps; ++i) {
            deps_[depTotal_ + i] = deps[i];
        }
        state_[id] = PageState::NotResident;
        size_[id] = sizeBytes;
        depOffset_[id] = depTotal_;
        depCount_[id] = depCount;
        depth_[id] = depth;
        depTotal_ += depCount;
        ++pageCount_;
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

    // Marks `id` and its transitive dependencies requested this frame with priority >= `priority`.
    // CapacityExceeded when more than kMaxRequests distinct pages would be requested this frame
    // (the pages marked so far stay marked; state stays consistent).
    ClStatus request(uint32_t id, uint32_t priority) {
        if (id >= pageCount_) {
            return ClStatus::InvalidArgument;
        }
        uint32_t sp = 0u;
        ClStatus st = mark(id, priority, sp);
        // Each page is pushed at most once per call (a push strictly raises its (requested,
        // priority) state to (true, priority)), so the loop runs at most kMaxPages times.
        while (sp > 0u && st == ClStatus::Ok) {
            const uint32_t cur = stack_[--sp];
            const uint32_t off = depOffset_[cur];
            const uint32_t n = depCount_[cur];
            for (uint32_t i = 0u; i < n && st == ClStatus::Ok; ++i) {
                st = mark(deps_[off + i], priority, sp);
            }
        }
        return st;
    }

    PageUpdateStats update(uint32_t maxLoads) {
        PageUpdateStats st;
        st.candidates = 0u;
        st.loads = 0u;
        st.evictions = 0u;
        st.deferred = 0u;
        loadCount_ = 0u;
        evictCount_ = 0u;
        uint32_t n = 0u;
        for (uint32_t i = 0u; i < coarseCount_; ++i) {
            const uint32_t id = coarseList_[i];
            if (state_[id] == PageState::NotResident) {
                cand_[n++] = id;
            }
        }
        for (uint32_t i = 0u; i < requestCount_; ++i) {
            const uint32_t id = requests_[i];
            if (state_[id] == PageState::NotResident && coarse_[id] == 0u) {
                cand_[n++] = id;
            }
        }
        st.candidates = n;
        heap_sort(n);
        bool coarseDeferred = false;
        for (uint32_t i = 0u; i < n; ++i) {
            const uint32_t id = cand_[i];
            // Coarse barrier: while a coarse page could not be issued, no other page is loaded
            // (they would take the bytes the coarse page is waiting for: starvation).
            if (loadCount_ >= maxLoads || !deps_resident(id) || (coarseDeferred && coarse_[id] == 0u)) {
                coarseDeferred = coarseDeferred || coarse_[id] != 0u;
                ++st.deferred;
                continue;
            }
            const bool isCoarse = coarse_[id] != 0u;
            const uint32_t prio = priority_[id];
            // Pass 1 (dry run): would evicting the evictable LRU pages from the tail make it fit? Only
            // then may pass 2 preempt requested (in-use) pages; otherwise it evicts unrequested pages
            // only (freeing them is harmless and lets their dependencies become evictable next time).
            uint64_t reclaim = 0u;
            uint32_t cursor = tail_;
            while (cursor != kNone && residentBytes_ + loadingBytes_ + size_[id] > budget_ + reclaim) {
                if (evictable(cursor, isCoarse, prio)) {
                    reclaim += size_[cursor];
                }
                cursor = prev_[cursor];
            }
            const uint32_t preempt = residentBytes_ + loadingBytes_ + size_[id] <= budget_ + reclaim ? prio : 0u;
            // Pass 2: evict from the tail until it fits. Both passes walk the LRU once (<= kMaxPages).
            cursor = tail_;
            while (cursor != kNone && !fits(size_[id])) {
                const uint32_t victim = cursor;
                cursor = prev_[cursor];
                if (evictable(victim, isCoarse, preempt)) {
                    evict(victim);
                    ++st.evictions;
                }
            }
            if (!fits(size_[id])) {
                coarseDeferred = coarseDeferred || coarse_[id] != 0u;
                ++st.deferred;
                continue;
            }
            state_[id] = PageState::Loading;
            loadingBytes_ += size_[id];
            add_dependents(id, true);
            loads_[loadCount_++] = id;
            ++st.loads;
        }
        return st;
    }

    ClStatus complete_load(uint32_t id) {
        if (id >= kMaxPages || state_[id] != PageState::Loading) {
            return ClStatus::NotFound;
        }
        state_[id] = PageState::Resident;
        loadingBytes_ -= size_[id];
        residentBytes_ += size_[id];
        lru_push_front(id);
        return ClStatus::Ok;
    }

    ClStatus fail_load(uint32_t id) {
        if (id >= kMaxPages || state_[id] != PageState::Loading) {
            return ClStatus::NotFound;
        }
        state_[id] = PageState::NotResident;
        loadingBytes_ -= size_[id];
        add_dependents(id, false);
        return ClStatus::Ok;
    }

    // Lowers or raises the budget, evicting non-coarse pages without active dependents (LRU
    // first, requested or not). Fails with OutOfBudget (budget unchanged) when the coarse pages
    // do not fit or the pages that must stay (coarse, in flight, their dependencies) exceed it.
    // The eviction list then describes this call.
    ClStatus set_budget(uint64_t budgetBytes) {
        evictCount_ = 0u;
        if (budgetBytes < coarseBytes_) {
            return ClStatus::OutOfBudget;
        }
        // Each iteration evicts one resident page, so at most kMaxPages iterations.
        for (uint32_t guard = 0u; guard < kMaxPages && residentBytes_ + loadingBytes_ > budgetBytes; ++guard) {
            uint32_t victim = kNone;
            uint32_t cursor = tail_;
            while (cursor != kNone) {
                if (coarse_[cursor] == 0u && dependents_[cursor] == 0u) {
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

    PageState state(uint32_t id) const { return id < kMaxPages ? state_[id] : PageState::Unregistered; }
    bool is_coarse(uint32_t id) const { return id < kMaxPages && coarse_[id] != 0u; }
    bool requested(uint32_t id) const { return id < kMaxPages && requestedFrame_[id] == frame_; }
    uint32_t priority(uint32_t id) const { return requested(id) ? priority_[id] : 0u; }
    uint32_t depth(uint32_t id) const { return id < kMaxPages ? depth_[id] : 0u; }
    uint32_t dep_count(uint32_t id) const { return id < pageCount_ ? depCount_[id] : 0u; }
    uint32_t dep(uint32_t id, uint32_t i) const { return id < pageCount_ && i < depCount_[id] ? deps_[depOffset_[id] + i] : kNone; }
    uint32_t dependents(uint32_t id) const { return id < kMaxPages ? dependents_[id] : 0u; }
    uint32_t page_count() const { return pageCount_; }
    uint32_t frame() const { return frame_; }
    uint32_t request_count() const { return requestCount_; }
    uint64_t budget() const { return budget_; }
    uint64_t resident_bytes() const { return residentBytes_; }
    uint64_t loading_bytes() const { return loadingBytes_; }
    uint64_t coarse_bytes() const { return coarseBytes_; }
    uint32_t coarse_count() const { return coarseCount_; }
    uint32_t load_count() const { return loadCount_; }
    uint32_t load_id(uint32_t i) const { return i < loadCount_ ? loads_[i] : kNone; }
    uint32_t evict_count() const { return evictCount_; }
    uint32_t evict_id(uint32_t i) const { return i < evictCount_ ? evicts_[i] : kNone; }
    uint32_t lru_head() const { return head_; }
    uint32_t lru_next(uint32_t id) const { return id < kMaxPages ? next_[id] : kNone; }
    bool all_coarse_resident() const {
        for (uint32_t i = 0u; i < coarseCount_; ++i) {
            if (state_[coarseList_[i]] != PageState::Resident) {
                return false;
            }
        }
        return true;
    }
    bool deps_resident(uint32_t id) const {
        const uint32_t off = depOffset_[id];
        const uint32_t n = depCount_[id];
        for (uint32_t i = 0u; i < n; ++i) {
            if (state_[deps_[off + i]] != PageState::Resident) {
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
        uint32_t coarseCount = 0u;
        uint32_t edges = 0u;
        uint32_t kids[kMaxPages];
        for (uint32_t i = 0u; i < kMaxPages; ++i) {
            kids[i] = 0u;
        }
        if (pageCount_ > kMaxPages || depTotal_ > kMaxDeps || coarseCount_ > pageCount_ || requestCount_ > kMaxRequests) {
            return false;
        }
        for (uint32_t c = 0u; c < pageCount_; ++c) {
            const PageState s = state_[c];
            if (depOffset_[c] != edges || depCount_[c] > depTotal_ - edges) {
                return false;
            }
            edges += depCount_[c];
            if (s == PageState::Loading || s == PageState::Resident) {
                for (uint32_t i = 0u; i < depCount_[c]; ++i) {
                    const uint32_t d = deps_[depOffset_[c] + i];
                    if (d >= c) {
                        return false;
                    }
                    ++kids[d];
                }
            }
        }
        if (edges != depTotal_) {
            return false;
        }
        for (uint32_t i = 0u; i < kMaxPages; ++i) {
            const PageState s = state_[i];
            if ((i < pageCount_) != (s != PageState::Unregistered)) {
                return false;
            }
            if (s == PageState::Unregistered) {
                continue;
            }
            if (coarse_[i] != 0u) {
                coarse += size_[i];
                ++coarseCount;
                for (uint32_t k = 0u; k < depCount_[i]; ++k) {
                    if (coarse_[deps_[depOffset_[i] + k]] == 0u) {
                        return false; // coarse LOD not closed under dependencies
                    }
                }
            }
            if (kids[i] != dependents_[i]) {
                return false;
            }
            if (s == PageState::NotResident) {
                continue;
            }
            if (!deps_resident(i)) {
                return false; // orphan: loading/resident page with a dependency not resident
            }
            if (s == PageState::Loading) {
                loading += size_[i];
            } else {
                res += size_[i];
                ++residentCount;
            }
        }
        if (res != residentBytes_ || loading != loadingBytes_ || coarse != coarseBytes_ || coarseCount != coarseCount_ ||
            res + loading > budget_ || coarse > budget_) {
            return false;
        }
        for (uint32_t i = 0u; i < requestCount_; ++i) {
            if (requests_[i] >= pageCount_ || requestedFrame_[requests_[i]] != frame_) {
                return false;
            }
        }
        uint32_t count = 0u;
        uint32_t prev = kNone;
        uint32_t cur = head_;
        for (uint32_t i = 0u; i < kMaxPages && cur != kNone; ++i) {
            if (cur >= kMaxPages || state_[cur] != PageState::Resident || prev_[cur] != prev) {
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

    ClStatus mark(uint32_t id, uint32_t priority, uint32_t& sp) {
        bool push = false;
        if (requestedFrame_[id] != frame_) {
            if (requestCount_ >= kMaxRequests) {
                return ClStatus::CapacityExceeded;
            }
            requestedFrame_[id] = frame_;
            priority_[id] = 0u;
            requests_[requestCount_++] = id;
            if (state_[id] == PageState::Resident) {
                lru_unlink(id);
                lru_push_front(id);
            }
            push = true;
        }
        if (priority_[id] < priority) {
            priority_[id] = priority;
            push = true;
        }
        if (push && sp < kMaxPages) {
            stack_[sp++] = id;
        }
        return ClStatus::Ok;
    }

    void add_dependents(uint32_t id, bool increment) {
        const uint32_t off = depOffset_[id];
        const uint32_t n = depCount_[id];
        for (uint32_t i = 0u; i < n; ++i) {
            if (increment) {
                ++dependents_[deps_[off + i]];
            } else {
                --dependents_[deps_[off + i]];
            }
        }
    }

    bool fits(uint64_t bytes) const { return residentBytes_ + loadingBytes_ + bytes <= budget_; }

    // Total order: coarse first, then priority desc, depth asc, id asc.
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

    void sift_down(uint32_t root, uint32_t n) {
        // The index at least doubles every iteration, so at most log2(n) < kMaxPages iterations.
        for (uint32_t guard = 0u; guard < kMaxPages; ++guard) {
            const uint32_t left = 2u * root + 1u;
            if (left >= n || left >= kMaxPages) { // n <= kMaxPages; the second test states it for the compiler
                return;
            }
            uint32_t largest = root;
            // "largest" under before(): the element that sorts LAST ends up at the root.
            if (before(cand_[largest], cand_[left])) {
                largest = left;
            }
            const uint32_t right = left + 1u;
            if (right < n && right < kMaxPages && before(cand_[largest], cand_[right])) {
                largest = right;
            }
            if (largest == root) {
                return;
            }
            const uint32_t t = cand_[root];
            cand_[root] = cand_[largest];
            cand_[largest] = t;
            root = largest;
        }
    }

    void heap_sort(uint32_t n) {
        if (n < 2u) {
            return;
        }
        for (uint32_t i = n / 2u; i > 0u; --i) {
            sift_down(i - 1u, n);
        }
        for (uint32_t end = n - 1u; end > 0u; --end) {
            const uint32_t t = cand_[0];
            cand_[0] = cand_[end];
            cand_[end] = t;
            sift_down(0u, end);
        }
    }

    // Victims for a candidate of priority `prio`: never coarse pages or pages with a Loading /
    // Resident dependent; pages requested this frame only when their priority is lower than the
    // candidate's (a coarse candidate may take any of them).
    bool evictable(uint32_t p, bool coarseCandidate, uint32_t prio) const {
        return coarse_[p] == 0u && dependents_[p] == 0u &&
               (coarseCandidate || requestedFrame_[p] != frame_ || priority_[p] < prio);
    }

    void evict(uint32_t p) {
        lru_unlink(p);
        state_[p] = PageState::NotResident;
        residentBytes_ -= size_[p];
        add_dependents(p, false);
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

    PageState state_[kMaxPages];
    uint8_t coarse_[kMaxPages];
    uint64_t size_[kMaxPages];
    uint32_t depOffset_[kMaxPages];
    uint32_t depCount_[kMaxPages];
    uint32_t depth_[kMaxPages];
    uint32_t dependents_[kMaxPages];
    uint32_t priority_[kMaxPages];
    uint32_t requestedFrame_[kMaxPages];
    uint32_t prev_[kMaxPages];
    uint32_t next_[kMaxPages];
    uint32_t coarseList_[kMaxPages];
    uint32_t cand_[kMaxPages];
    uint32_t loads_[kMaxPages];
    uint32_t evicts_[kMaxPages];
    uint32_t stack_[kMaxPages];
    uint32_t deps_[kMaxDeps];
    uint32_t requests_[kMaxRequests];
    uint64_t budget_;
    uint64_t residentBytes_;
    uint64_t loadingBytes_;
    uint64_t coarseBytes_;
    uint32_t pageCount_;
    uint32_t depTotal_;
    uint32_t head_;
    uint32_t tail_;
    uint32_t lruCount_;
    uint32_t frame_;
    uint32_t requestCount_;
    uint32_t loadCount_;
    uint32_t evictCount_;
    uint32_t coarseCount_;
};

// 16384 pages (64 KiB each = 1 GiB addressable), 262144 dependency edges, 16384 requests per frame.
typedef ClusterPageResidency<16384u, 262144u, 16384u> ClusterPageResidencyDefault;

} // namespace core_logic
} // namespace fuse
