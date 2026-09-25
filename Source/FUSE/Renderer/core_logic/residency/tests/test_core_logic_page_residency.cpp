// WP-5.3 core_logic cluster-page residency (ClusterPageResidency): property tests
// (`fuse_core_logic_page_residency_tests [model|props|live|big|errors|all]`).
//
//   model   random page DAGs (1..4 dependencies on earlier pages, closed coarse set, random sizes) and
//           random operation sequences (frames, prioritised requests, updates with random load caps,
//           completed / failed loads, budget changes) against an independent STL oracle (std::sort +
//           std::list LRU): page states, load and eviction lists (order included), byte counts, LRU
//           order and priorities equal after every operation
//   props   properties after every operation: check_invariants() (byte accounting, dependency closure
//           of Loading / Resident pages, dependent counts, LRU == resident set, coarse closure), budget
//           never exceeded, coarse pages never evicted, evictions only of resident pages with no active
//           dependent and — unless a coarse page was a candidate — either not requested this frame or
//           requested with a lower priority than a candidate (priority preemption), loads only
//           of pages whose dependencies are all resident, loads issued in (priority desc, depth asc)
//           order among the candidates that were ready
//   live    liveness: with a budget that holds the coarse pages plus the closure of a requested set,
//           after noise frames that fill the budget with other pages, requesting the set every frame
//           and completing every load makes the whole closure resident within depth + 8 frames and
//           then issues nothing; the coarse LOD is resident after the first update and stays resident
//   big     the default 16384-page configuration (heap-allocated once): 12k-page 8-layer DAG, 200 frames
//   errors  argument validation and every check_invariants() negative path (ClTestAccess)
#include "fuse/core_logic/cluster_page_residency.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <vector>

namespace fuse {
namespace core_logic {
// Test-only: corrupts residency state to exercise the negative paths of check_invariants().
struct ClTestAccess {
    template <typename R>
    static PageState& state(R& r, uint32_t i) {
        return r.state_[i];
    }
    template <typename R>
    static uint32_t& dependents(R& r, uint32_t i) {
        return r.dependents_[i];
    }
    template <typename R>
    static uint64_t& resident_bytes(R& r) {
        return r.residentBytes_;
    }
    template <typename R>
    static uint64_t& budget(R& r) {
        return r.budget_;
    }
    template <typename R>
    static uint8_t& coarse(R& r, uint32_t i) {
        return r.coarse_[i];
    }
    template <typename R>
    static uint32_t& lru_count(R& r) {
        return r.lruCount_;
    }
    template <typename R>
    static uint32_t& dep(R& r, uint32_t edge) {
        return r.deps_[edge];
    }
    template <typename R>
    static uint32_t& request(R& r, uint32_t i) {
        return r.requests_[i];
    }
    template <typename R>
    static uint32_t& page_count(R& r) {
        return r.pageCount_;
    }
    template <typename R>
    static uint32_t& dep_offset(R& r, uint32_t i) {
        return r.depOffset_[i];
    }
    template <typename R>
    static uint32_t& prev(R& r, uint32_t i) {
        return r.prev_[i];
    }
    template <typename R>
    static uint32_t& requested_frame(R& r, uint32_t i) {
        return r.requestedFrame_[i];
    }
    template <typename R>
    static uint64_t& coarse_bytes(R& r) {
        return r.coarseBytes_;
    }
};
} // namespace core_logic
} // namespace fuse

namespace {

using namespace fuse::core_logic;

int g_failures = 0;

#define PR_CHECK(cond)                                                                        \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            ++g_failures;                                                                     \
            if (g_failures < 50) {                                                            \
                std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            }                                                                                 \
        }                                                                                     \
    } while (0)

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return n == 0u ? 0u : static_cast<uint32_t>(next() % n); }
    bool chance(uint32_t percent) { return below(100u) < percent; }
};

uint32_t iterScale() {
    const char* e = std::getenv("FUSE_CORE_LOGIC_ITERS");
    const long v = e != nullptr ? std::strtol(e, nullptr, 10) : 1;
    return v > 0 ? static_cast<uint32_t>(v) : 1u;
}

constexpr uint32_t kPages = 48u;
constexpr uint32_t kDeps = 256u;
constexpr uint32_t kRequests = 48u;
typedef ClusterPageResidency<kPages, kDeps, kRequests> Res;

struct PageSpec {
    uint64_t size = 1;
    std::vector<uint32_t> deps;
    bool coarse = false;
};

// Random page DAG: coarse pages first (dependencies among coarse pages only), then the rest.
std::vector<PageSpec> randomDag(Rng& rng, uint32_t pages, uint32_t coarsePages, uint32_t maxSize) {
    std::vector<PageSpec> out(pages);
    for (uint32_t i = 0; i < pages; ++i) {
        PageSpec& p = out[i];
        p.size = 1u + rng.below(maxSize);
        p.coarse = i < coarsePages;
        // Coarse pages depend on earlier (coarse) pages only; the others mostly on non-coarse ones.
        const uint32_t want = i == 0u ? 0u : (i < coarsePages ? rng.below(2u) : 1u + rng.below(4u));
        for (uint32_t k = 0; k < want; ++k) {
            const uint32_t lo = (!p.coarse && coarsePages < i && rng.chance(70)) ? coarsePages : 0u;
            const uint32_t d = lo + rng.below(i - lo);
            if (std::find(p.deps.begin(), p.deps.end(), d) == p.deps.end()) {
                p.deps.push_back(d);
            }
        }
    }
    return out;
}

// --- oracle ------------------------------------------------------------------------------------------
struct Oracle {
    std::vector<PageSpec> pages;
    std::vector<PageState> state;
    std::vector<uint32_t> depth, dependents, priority, reqFrame;
    std::list<uint32_t> lru; // front = most recently used
    std::vector<uint32_t> requests, loads, evicts;
    uint64_t budget = 0, resident = 0, loading = 0;
    uint32_t frame = 0;

    void init(const std::vector<PageSpec>& p, uint64_t b) {
        pages = p;
        const size_t n = p.size();
        state.assign(n, PageState::NotResident);
        depth.assign(n, 0u);
        dependents.assign(n, 0u);
        priority.assign(n, 0u);
        reqFrame.assign(n, 0xFFFFFFFFu);
        for (size_t i = 0; i < n; ++i) {
            for (uint32_t d : p[i].deps) {
                depth[i] = std::max(depth[i], depth[d] + 1u);
            }
        }
        lru.clear();
        requests.clear();
        loads.clear();
        evicts.clear();
        budget = b;
        resident = loading = 0;
        frame = 0;
    }
    void beginFrame(uint32_t f) {
        frame = f;
        requests.clear();
    }
    void touch(uint32_t id) {
        if (state[id] == PageState::Resident) {
            lru.remove(id);
            lru.push_front(id);
        }
    }
    bool request(uint32_t id, uint32_t prio) {
        // Mirrors the implementation's visit order (DFS with an explicit stack, deps in order).
        auto mark = [&](uint32_t p) -> int {
            bool push = false;
            if (reqFrame[p] != frame) {
                if (requests.size() >= kRequests) {
                    return -1;
                }
                reqFrame[p] = frame;
                priority[p] = 0;
                requests.push_back(p);
                touch(p);
                push = true;
            }
            if (priority[p] < prio) {
                priority[p] = prio;
                push = true;
            }
            return push ? 1 : 0;
        };
        std::vector<uint32_t> st;
        const int m = mark(id);
        if (m < 0) {
            return false;
        }
        if (m > 0) {
            st.push_back(id);
        }
        while (!st.empty()) {
            const uint32_t cur = st.back();
            st.pop_back();
            for (uint32_t d : pages[cur].deps) {
                const int r = mark(d);
                if (r < 0) {
                    return false;
                }
                if (r > 0) {
                    st.push_back(d);
                }
            }
        }
        return true;
    }
    bool depsResident(uint32_t id) const {
        for (uint32_t d : pages[id].deps) {
            if (state[d] != PageState::Resident) {
                return false;
            }
        }
        return true;
    }
    void evict(uint32_t p) {
        lru.remove(p);
        state[p] = PageState::NotResident;
        resident -= pages[p].size;
        for (uint32_t d : pages[p].deps) {
            --dependents[d];
        }
        evicts.push_back(p);
    }
    void update(uint32_t maxLoads) {
        loads.clear();
        evicts.clear();
        std::vector<uint32_t> cand;
        for (uint32_t i = 0; i < pages.size(); ++i) {
            if (pages[i].coarse && state[i] == PageState::NotResident) {
                cand.push_back(i);
            }
        }
        for (uint32_t id : requests) {
            if (!pages[id].coarse && state[id] == PageState::NotResident) {
                cand.push_back(id);
            }
        }
        std::sort(cand.begin(), cand.end(), [&](uint32_t a, uint32_t b) {
            if (pages[a].coarse != pages[b].coarse) {
                return pages[a].coarse;
            }
            const uint32_t pa = pages[a].coarse ? 0xFFFFFFFFu : priority[a];
            const uint32_t pb = pages[b].coarse ? 0xFFFFFFFFu : priority[b];
            if (pa != pb) {
                return pa > pb;
            }
            if (depth[a] != depth[b]) {
                return depth[a] < depth[b];
            }
            return a < b;
        });
        bool coarseDeferred = false;
        for (uint32_t id : cand) {
            if (loads.size() >= maxLoads || !depsResident(id) || (coarseDeferred && !pages[id].coarse)) {
                coarseDeferred = coarseDeferred || pages[id].coarse;
                continue;
            }
            const uint64_t sz = pages[id].size;
            const bool isCoarse = pages[id].coarse;
            const uint32_t prio = priority[id];
            auto victimOk = [&](uint32_t v, uint32_t p) {
                return !pages[v].coarse && dependents[v] == 0u && (isCoarse || reqFrame[v] != frame || priority[v] < p);
            };
            std::vector<uint32_t> tailToHead(lru.rbegin(), lru.rend());
            uint64_t reclaim = 0;
            for (uint32_t v : tailToHead) {
                if (resident + loading + sz <= budget + reclaim) {
                    break;
                }
                reclaim += victimOk(v, prio) ? pages[v].size : 0u;
            }
            const uint32_t preempt = resident + loading + sz <= budget + reclaim ? prio : 0u;
            for (uint32_t v : tailToHead) {
                if (resident + loading + sz <= budget) {
                    break;
                }
                if (victimOk(v, preempt)) {
                    evict(v);
                }
            }
            if (resident + loading + sz > budget) {
                coarseDeferred = coarseDeferred || pages[id].coarse;
                continue;
            }
            state[id] = PageState::Loading;
            loading += sz;
            for (uint32_t d : pages[id].deps) {
                ++dependents[d];
            }
            loads.push_back(id);
        }
    }
    bool complete(uint32_t id) {
        if (state[id] != PageState::Loading) {
            return false;
        }
        state[id] = PageState::Resident;
        loading -= pages[id].size;
        resident += pages[id].size;
        lru.push_front(id);
        return true;
    }
    bool fail(uint32_t id) {
        if (state[id] != PageState::Loading) {
            return false;
        }
        state[id] = PageState::NotResident;
        loading -= pages[id].size;
        for (uint32_t d : pages[id].deps) {
            --dependents[d];
        }
        return true;
    }
    bool setBudget(uint64_t b) {
        evicts.clear();
        uint64_t coarse = 0;
        for (const PageSpec& p : pages) {
            coarse += p.coarse ? p.size : 0u;
        }
        if (b < coarse) {
            return false;
        }
        while (resident + loading > b) {
            uint32_t victim = 0xFFFFFFFFu;
            for (auto it = lru.rbegin(); it != lru.rend(); ++it) {
                if (!pages[*it].coarse && dependents[*it] == 0u) {
                    victim = *it;
                    break;
                }
            }
            if (victim == 0xFFFFFFFFu) {
                break;
            }
            evict(victim);
        }
        if (resident + loading > b) {
            return false;
        }
        budget = b;
        return true;
    }
};

bool registerAll(Res& r, const std::vector<PageSpec>& pages) {
    for (uint32_t i = 0; i < pages.size(); ++i) {
        const PageSpec& p = pages[i];
        if (r.register_page(i, p.size, p.deps.data(), static_cast<uint32_t>(p.deps.size()), p.coarse) != ClStatus::Ok) {
            return false;
        }
    }
    return true;
}

bool sameAsOracle(const Res& r, const Oracle& o) {
    bool ok = r.resident_bytes() == o.resident && r.loading_bytes() == o.loading && r.budget() == o.budget;
    for (uint32_t i = 0; i < o.pages.size(); ++i) {
        ok = ok && r.state(i) == o.state[i] && r.dependents(i) == o.dependents[i] && r.depth(i) == o.depth[i];
        if (o.reqFrame[i] == o.frame) {
            ok = ok && r.requested(i) && r.priority(i) == o.priority[i];
        }
    }
    uint32_t cur = r.lru_head();
    for (uint32_t id : o.lru) {
        ok = ok && cur == id;
        cur = r.lru_next(cur);
    }
    return ok && cur == Res::kNone;
}

struct Stepper {
    Res& r;
    Oracle& o;
    Rng& rng;
    std::vector<PageSpec> pages;
    uint32_t frame = 0;
    uint32_t ops = 0;
    uint32_t loads = 0, evictions = 0, deferred = 0, preempted = 0;

    // One random operation; checks the implementation against the oracle and the properties.
    void step() {
        ++ops;
        std::vector<PageState> before(pages.size());
        for (uint32_t i = 0; i < pages.size(); ++i) {
            before[i] = r.state(i);
        }
        const uint32_t op = rng.below(100u);
        if (op < 12u) {
            frame += 1u + rng.below(2u);
            PR_CHECK(r.begin_frame(frame) == ClStatus::Ok);
            o.beginFrame(frame);
        } else if (op < 55u) {
            const uint32_t id = rng.below(static_cast<uint32_t>(pages.size()));
            const uint32_t prio = rng.below(40u);
            const bool a = r.request(id, prio) == ClStatus::Ok;
            const bool b = o.request(id, prio);
            PR_CHECK(a == b);
        } else if (op < 75u) {
            const uint32_t maxLoads = rng.below(6u);
            bool coarseCandidate = false;
            for (uint32_t i = 0; i < pages.size(); ++i) {
                coarseCandidate = coarseCandidate || (pages[i].coarse && before[i] == PageState::NotResident);
            }
            uint32_t maxCandPrio = 0;
            for (uint32_t i = 0; i < pages.size(); ++i) {
                if (before[i] == PageState::NotResident && r.requested(i)) {
                    maxCandPrio = std::max(maxCandPrio, r.priority(i));
                }
            }
            std::vector<uint32_t> prioBefore(pages.size());
            for (uint32_t i = 0; i < pages.size(); ++i) {
                prioBefore[i] = r.priority(i);
            }
            const PageUpdateStats st = r.update(maxLoads);
            o.update(maxLoads);
            loads += st.loads;
            evictions += st.evictions;
            deferred += st.deferred;
            PR_CHECK(st.loads == r.load_count() && st.evictions == r.evict_count());
            PR_CHECK(st.loads <= maxLoads && st.loads + st.deferred == st.candidates);
            PR_CHECK(r.load_count() == o.loads.size() && r.evict_count() == o.evicts.size());
            for (uint32_t i = 0; i < r.load_count() && i < o.loads.size(); ++i) {
                PR_CHECK(r.load_id(i) == o.loads[i]);
                const uint32_t id = r.load_id(i);
                PR_CHECK(before[id] == PageState::NotResident);
                for (uint32_t d : pages[id].deps) {
                    PR_CHECK(r.state(d) == PageState::Resident); // loads only on a resident closure
                }
            }
            // Loads in candidate order (ready candidates: priority desc, depth asc).
            for (uint32_t i = 1; i < r.load_count(); ++i) {
                const uint32_t a = r.load_id(i - 1u), b = r.load_id(i);
                if (!pages[a].coarse && !pages[b].coarse) {
                    PR_CHECK(r.priority(a) > r.priority(b) || (r.priority(a) == r.priority(b) && r.depth(a) <= r.depth(b)));
                }
                PR_CHECK(pages[a].coarse || !pages[b].coarse);
            }
            for (uint32_t i = 0; i < r.evict_count() && i < o.evicts.size(); ++i) {
                const uint32_t e = r.evict_id(i);
                PR_CHECK(e == o.evicts[i]);
                PR_CHECK(!pages[e].coarse);
                PR_CHECK(before[e] == PageState::Resident);
                // Requested pages are only preempted by a more urgent candidate (or a coarse one).
                PR_CHECK(coarseCandidate || !r.requested(e) || prioBefore[e] < maxCandPrio);
                preempted += (!coarseCandidate && r.requested(e)) ? 1u : 0u;
            }
        } else if (op < 90u) {
            // Complete (mostly) or fail a random loading page, if any.
            std::vector<uint32_t> loadingPages;
            for (uint32_t i = 0; i < pages.size(); ++i) {
                if (r.state(i) == PageState::Loading) {
                    loadingPages.push_back(i);
                }
            }
            const uint32_t id = loadingPages.empty() ? rng.below(static_cast<uint32_t>(pages.size()))
                                                     : loadingPages[rng.below(static_cast<uint32_t>(loadingPages.size()))];
            if (rng.chance(85)) {
                const ClStatus s = r.complete_load(id);
                PR_CHECK((s == ClStatus::Ok) == o.complete(id));
                PR_CHECK((s == ClStatus::Ok) == (before[id] == PageState::Loading));
            } else {
                const ClStatus s = r.fail_load(id);
                PR_CHECK((s == ClStatus::Ok) == o.fail(id));
                PR_CHECK((s == ClStatus::Ok) == (before[id] == PageState::Loading));
            }
        } else {
            const uint64_t nb = rng.below(static_cast<uint32_t>(o.budget + 20u));
            const ClStatus s = r.set_budget(nb);
            const bool ob = o.setBudget(nb);
            PR_CHECK((s == ClStatus::Ok) == ob);
            PR_CHECK(nb >= r.coarse_bytes() || s == ClStatus::OutOfBudget);
            PR_CHECK(r.evict_count() == o.evicts.size());
            for (uint32_t i = 0; i < r.evict_count(); ++i) {
                PR_CHECK(!pages[r.evict_id(i)].coarse && before[r.evict_id(i)] == PageState::Resident);
            }
        }
        PR_CHECK(r.check_invariants());
        PR_CHECK(r.resident_bytes() + r.loading_bytes() <= r.budget());
        PR_CHECK(sameAsOracle(r, o));
    }
};

void testModel() {
    const uint32_t runs = 300u * iterScale();
    auto res = std::make_unique<Res>();
    Oracle oracle;
    uint32_t totalOps = 0, loads = 0, evictions = 0, deferred = 0, preempted = 0;
    for (uint32_t run = 0; run < runs; ++run) {
        Rng rng(0xC0FFEEull + run * 7919ull);
        const uint32_t n = 2u + rng.below(kPages - 1u);
        const uint32_t coarse = 1u + rng.below(std::min(n, 4u));
        const std::vector<PageSpec> pages = randomDag(rng, n, coarse, 8u);
        uint64_t coarseBytes = 0;
        for (const PageSpec& p : pages) {
            coarseBytes += p.coarse ? p.size : 0u;
        }
        const uint64_t budget = coarseBytes + rng.below(60u);
        res->reset(budget);
        PR_CHECK(registerAll(*res, pages));
        oracle.init(pages, budget);
        Stepper s{*res, oracle, rng, pages};
        for (uint32_t k = 0; k < 400u; ++k) {
            s.step();
        }
        totalOps += s.ops;
        loads += s.loads;
        evictions += s.evictions;
        deferred += s.deferred;
        preempted += s.preempted;
    }
    std::printf("model: %u runs, %u operations == oracle (%u loads, %u evictions of which %u preempted requested pages, %u "
                "deferred)\n",
                runs, totalOps, loads, evictions, preempted, deferred);
    PR_CHECK(loads > 1000u && evictions > 100u && deferred > 100u && preempted > 10u);
}

void testProps() {
    // Structured scenario: every frame requests the pages of a moving window, completes a random
    // subset of the in-flight loads; properties are checked by Stepper-like assertions.
    const uint32_t runs = 200u * iterScale();
    auto res = std::make_unique<Res>();
    uint32_t frames = 0, coarseEvictAttempts = 0;
    for (uint32_t run = 0; run < runs; ++run) {
        Rng rng(0xBADC0DEull + run * 104729ull);
        const uint32_t n = 8u + rng.below(kPages - 7u);
        const uint32_t coarse = 1u + rng.below(3u);
        const std::vector<PageSpec> pages = randomDag(rng, n, coarse, 6u);
        uint64_t coarseBytes = 0;
        for (const PageSpec& p : pages) {
            coarseBytes += p.coarse ? p.size : 0u;
        }
        res->reset(coarseBytes + 4u + rng.below(40u));
        PR_CHECK(registerAll(*res, pages));
        std::vector<uint32_t> inflight;
        for (uint32_t f = 1; f <= 60u; ++f, ++frames) {
            PR_CHECK(res->begin_frame(f) == ClStatus::Ok);
            const uint32_t base = (f * 3u) % n;
            for (uint32_t k = 0; k < 6u; ++k) {
                const uint32_t id = (base + k * 5u) % n;
                PR_CHECK(res->request(id, 1u + rng.below(30u)) == ClStatus::Ok);
                // Priorities propagate: every dependency has at least the child's priority.
                for (uint32_t d : pages[id].deps) {
                    PR_CHECK(res->requested(d) && res->priority(d) >= res->priority(id));
                }
            }
            const PageUpdateStats st = res->update(1u + rng.below(4u));
            (void)st;
            for (uint32_t i = 0; i < res->evict_count(); ++i) {
                const uint32_t e = res->evict_id(i);
                coarseEvictAttempts += pages[e].coarse ? 1u : 0u;
                PR_CHECK(res->dependents(e) == 0u);
            }
            for (uint32_t i = 0; i < res->load_count(); ++i) {
                inflight.push_back(res->load_id(i));
            }
            // Complete a random subset (IO latency), fail a few.
            std::vector<uint32_t> keep;
            for (uint32_t id : inflight) {
                const uint32_t roll = rng.below(100u);
                if (roll < 60u) {
                    PR_CHECK(res->complete_load(id) == ClStatus::Ok);
                } else if (roll < 65u) {
                    PR_CHECK(res->fail_load(id) == ClStatus::Ok);
                } else {
                    keep.push_back(id);
                }
            }
            inflight.swap(keep);
            PR_CHECK(res->check_invariants());
            PR_CHECK(res->resident_bytes() + res->loading_bytes() <= res->budget());
            // Once resident, the coarse LOD stays resident.
            if (f > 20u) {
                PR_CHECK(res->all_coarse_resident());
            }
        }
    }
    std::printf("props: %u runs, %u frames, coarse evictions %u\n", runs, frames, coarseEvictAttempts);
    PR_CHECK(coarseEvictAttempts == 0u);
}

void testLive() {
    const uint32_t runs = 300u * iterScale();
    auto res = std::make_unique<Res>();
    uint32_t maxFrames = 0;
    for (uint32_t run = 0; run < runs; ++run) {
        Rng rng(0x11FEull + run * 31337ull);
        const uint32_t n = 4u + rng.below(kPages - 3u);
        const uint32_t coarse = 1u + rng.below(3u);
        const std::vector<PageSpec> pages = randomDag(rng, n, coarse, 5u);
        // Requested set + its dependency closure.
        std::vector<uint32_t> want;
        const uint32_t wantCount = 1u + rng.below(5u);
        for (uint32_t k = 0; k < wantCount; ++k) {
            want.push_back(rng.below(n));
        }
        std::vector<char> inClosure(n, 0);
        std::vector<uint32_t> st(want.begin(), want.end());
        while (!st.empty()) {
            const uint32_t p = st.back();
            st.pop_back();
            if (inClosure[p] != 0) {
                continue;
            }
            inClosure[p] = 1;
            for (uint32_t d : pages[p].deps) {
                st.push_back(d);
            }
        }
        uint64_t need = 0;
        uint32_t maxDepth = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (inClosure[i] != 0 || pages[i].coarse) {
                need += pages[i].size;
            }
        }
        res->reset(need + rng.below(10u));
        PR_CHECK(registerAll(*res, pages));
        for (uint32_t i = 0; i < n; ++i) {
            maxDepth = std::max(maxDepth, res->depth(i));
        }
        // Noise first: other pages get loaded and must make room later.
        uint32_t f = 1;
        for (; f <= 5u; ++f) {
            PR_CHECK(res->begin_frame(f) == ClStatus::Ok);
            for (uint32_t k = 0; k < 4u; ++k) {
                (void)res->request(rng.below(n), 50u);
            }
            res->update(n);
            for (uint32_t i = 0; i < res->load_count(); ++i) {
                PR_CHECK(res->complete_load(res->load_id(i)) == ClStatus::Ok);
            }
            // Coarse first, always fits: a chain of <= 3 coarse pages is resident after 3 frames.
            PR_CHECK(f < 3u || res->all_coarse_resident());
        }
        uint32_t settled = 0;
        for (uint32_t g = 0; g < maxDepth + 12u; ++g, ++f) {
            PR_CHECK(res->begin_frame(f) == ClStatus::Ok);
            for (uint32_t id : want) {
                PR_CHECK(res->request(id, 1u) == ClStatus::Ok);
            }
            res->update(n);
            for (uint32_t i = 0; i < res->load_count(); ++i) {
                PR_CHECK(res->complete_load(res->load_id(i)) == ClStatus::Ok);
            }
            bool all = true;
            for (uint32_t i = 0; i < n; ++i) {
                all = all && (inClosure[i] == 0 || res->state(i) == PageState::Resident);
            }
            if (all && settled == 0u) {
                settled = g + 1u;
            }
        }
        // depth + 1 frames to load the closure level by level, plus at most one extra frame per stale
        // dependency chain of noise pages that must be evicted leaf first (one LRU walk per candidate).
        PR_CHECK(settled != 0u && settled <= maxDepth + 8u);
        maxFrames = std::max(maxFrames, settled);
        // Steady: nothing more to load.
        PR_CHECK(res->begin_frame(f) == ClStatus::Ok);
        for (uint32_t id : want) {
            PR_CHECK(res->request(id, 1u) == ClStatus::Ok);
        }
        const PageUpdateStats s = res->update(n);
        PR_CHECK(s.loads == 0u && s.evictions == 0u);
        PR_CHECK(res->all_coarse_resident() && res->check_invariants());
    }
    std::printf("live: %u runs, requested closure resident within depth + 8 frames (max %u frames)\n", runs, maxFrames);
}

void testBig() {
    typedef ClusterPageResidencyDefault Big;
    std::unique_ptr<Big> big(new Big);
    Rng rng(42);
    // Layered like a cluster DAG's pages: layer 0 = 64 coarse pages, each finer page depends on 1-4
    // pages of the previous (coarser) layer near its own position.
    const uint32_t layers[8] = {64u, 128u, 256u, 512u, 1024u, 2048u, 4000u, 3968u};
    uint32_t n = 0;
    big->reset(1500u * 65536ull);
    uint32_t edges = 0;
    uint32_t prevStart = 0, prevSize = 0;
    for (uint32_t L = 0; L < 8u; ++L) {
        const uint32_t start = n;
        for (uint32_t j = 0; j < layers[L]; ++j) {
            uint32_t deps[4];
            uint32_t k = 0;
            if (L > 0u) {
                const uint32_t want = 1u + rng.below(4u);
                const uint32_t pos = static_cast<uint32_t>((static_cast<uint64_t>(j) * prevSize) / layers[L]);
                for (uint32_t w = 0; w < want; ++w) {
                    const uint32_t off = std::min(prevSize - 1u, pos + rng.below(3u));
                    const uint32_t d = prevStart + off;
                    if (std::find(deps, deps + k, d) == deps + k) {
                        deps[k++] = d;
                    }
                }
            }
            edges += k;
            PR_CHECK(big->register_page(n, 65536u, deps, k, L == 0u) == ClStatus::Ok);
            ++n;
        }
        prevStart = start;
        prevSize = layers[L];
    }
    uint32_t loads = 0, evictions = 0;
    std::vector<uint32_t> inflight;
    for (uint32_t f = 1; f <= 200u; ++f) {
        PR_CHECK(big->begin_frame(f) == ClStatus::Ok);
        // A moving window over the two finest layers.
        const uint32_t fine = n - 3968u - 4000u;
        const uint32_t centre = (f * 61u) % 7968u;
        for (uint32_t k = 0; k < 300u; ++k) {
            (void)big->request(fine + (centre + k) % 7968u, 1u + rng.below(1000u));
        }
        const PageUpdateStats st = big->update(64u);
        loads += st.loads;
        evictions += st.evictions;
        for (uint32_t id : inflight) {
            PR_CHECK(big->complete_load(id) == ClStatus::Ok);
        }
        inflight.assign(big->load_count(), 0u);
        for (uint32_t i = 0; i < big->load_count(); ++i) {
            inflight[i] = big->load_id(i);
        }
        if (f % 50u == 0u) {
            PR_CHECK(big->check_invariants());
        }
        PR_CHECK(big->resident_bytes() + big->loading_bytes() <= big->budget());
        PR_CHECK(f < 3u || big->all_coarse_resident());
    }
    std::printf("big: %u pages, %u edges, 200 frames: %u loads, %u evictions, sizeof = %.2f MiB\n", n, edges, loads, evictions,
                static_cast<double>(sizeof(Big)) / (1024.0 * 1024.0));
    PR_CHECK(loads > 1000u && evictions > 100u);
}

void testErrors() {
    auto r = std::make_unique<Res>();
    r->reset(10u);
    const uint32_t d0[1] = {0u};
    const uint32_t dDup[2] = {0u, 0u};
    const uint32_t dLater[1] = {5u};
    PR_CHECK(r->register_page(1u, 1u, nullptr, 0u, false) == ClStatus::InvalidArgument); // not the next id
    PR_CHECK(r->register_page(0u, 0u, nullptr, 0u, false) == ClStatus::InvalidArgument); // zero size
    PR_CHECK(r->register_page(0u, 1u, nullptr, 1u, false) == ClStatus::InvalidArgument); // null deps
    PR_CHECK(r->register_page(0u, 11u, nullptr, 0u, true) == ClStatus::OutOfBudget);     // coarse over budget
    PR_CHECK(r->register_page(0u, 3u, nullptr, 0u, true) == ClStatus::Ok);
    PR_CHECK(r->register_page(1u, 2u, d0, 1u, false) == ClStatus::Ok);
    PR_CHECK(r->register_page(2u, 1u, dLater, 1u, false) == ClStatus::InvalidArgument); // forward edge
    PR_CHECK(r->register_page(2u, 1u, dDup, 2u, false) == ClStatus::InvalidArgument);   // duplicate edge
    const uint32_t d1[1] = {1u};
    PR_CHECK(r->register_page(2u, 1u, d1, 1u, true) == ClStatus::InvalidArgument); // coarse on a non-coarse page
    PR_CHECK(r->register_page(2u, 2u, d1, 1u, false) == ClStatus::Ok);
    PR_CHECK(r->page_count() == 3u && r->dep_count(2u) == 1u && r->dep(2u, 0u) == 1u && r->dep(2u, 1u) == Res::kNone);
    PR_CHECK(r->depth(2u) == 2u && r->is_coarse(0u) && !r->is_coarse(1u));
    PR_CHECK(r->request(7u, 1u) == ClStatus::InvalidArgument);
    PR_CHECK(r->begin_frame(0u) == ClStatus::InvalidArgument);
    PR_CHECK(r->begin_frame(Res::kNone) == ClStatus::InvalidArgument);
    PR_CHECK(r->begin_frame(3u) == ClStatus::Ok);
    PR_CHECK(r->begin_frame(3u) == ClStatus::InvalidArgument);
    PR_CHECK(r->complete_load(0u) == ClStatus::NotFound && r->fail_load(0u) == ClStatus::NotFound);
    PR_CHECK(r->complete_load(999u) == ClStatus::NotFound && r->fail_load(999u) == ClStatus::NotFound);
    PR_CHECK(r->state(999u) == PageState::Unregistered && r->dependents(999u) == 0u && r->depth(999u) == 0u);
    PR_CHECK(r->lru_next(999u) == Res::kNone && r->load_id(5u) == Res::kNone && r->evict_id(5u) == Res::kNone);
    PR_CHECK(r->request(2u, 9u) == ClStatus::Ok);
    PR_CHECK(r->priority(0u) == 9u && r->priority(1u) == 9u && r->request_count() == 3u);
    PageUpdateStats st = r->update(8u); // coarse 0 loads; 1, 2 wait for their dependencies
    PR_CHECK(st.loads == 1u && r->load_id(0u) == 0u && st.deferred == 2u);
    PR_CHECK(r->complete_load(0u) == ClStatus::Ok);
    st = r->update(8u);
    PR_CHECK(st.loads == 1u && r->load_id(0u) == 1u);
    PR_CHECK(r->set_budget(2u) == ClStatus::OutOfBudget); // below coarse bytes
    PR_CHECK(r->set_budget(4u) == ClStatus::OutOfBudget); // page 1 in flight
    PR_CHECK(r->budget() == 10u);
    PR_CHECK(r->complete_load(1u) == ClStatus::Ok);
    st = r->update(8u);
    PR_CHECK(st.loads == 1u && r->load_id(0u) == 2u);
    PR_CHECK(r->fail_load(2u) == ClStatus::Ok && r->dependents(1u) == 0u);
    PR_CHECK(r->set_budget(3u) == ClStatus::Ok && r->evict_count() == 1u && r->evict_id(0u) == 1u);
    PR_CHECK(r->check_invariants());

    // Request capacity.
    {
        typedef ClusterPageResidency<4u, 8u, 2u> Tiny;
        Tiny t;
        t.reset(100u);
        const uint32_t a[1] = {0u};
        const uint32_t b[1] = {1u};
        PR_CHECK(t.register_page(0u, 1u, nullptr, 0u, false) == ClStatus::Ok);
        PR_CHECK(t.register_page(1u, 1u, a, 1u, false) == ClStatus::Ok);
        PR_CHECK(t.register_page(2u, 1u, b, 1u, false) == ClStatus::Ok);
        PR_CHECK(t.begin_frame(1u) == ClStatus::Ok);
        PR_CHECK(t.request(2u, 1u) == ClStatus::CapacityExceeded);
        PR_CHECK(t.check_invariants());
        // Dependency-edge capacity.
        typedef ClusterPageResidency<4u, 1u, 4u> Edges;
        Edges e;
        e.reset(100u);
        const uint32_t two[2] = {0u, 1u};
        PR_CHECK(e.register_page(0u, 1u, nullptr, 0u, false) == ClStatus::Ok);
        PR_CHECK(e.register_page(1u, 1u, nullptr, 0u, false) == ClStatus::Ok);
        PR_CHECK(e.register_page(2u, 1u, two, 2u, false) == ClStatus::CapacityExceeded);
        PR_CHECK(e.register_page(2u, 1u, two, 1u, false) == ClStatus::Ok);
        PR_CHECK(e.register_page(3u, 1u, two + 1, 1u, false) == ClStatus::CapacityExceeded);
        typedef ClusterPageResidency<1u, 1u, 1u> One;
        One o;
        o.reset(10u);
        PR_CHECK(o.register_page(0u, 1u, nullptr, 0u, false) == ClStatus::Ok);
        PR_CHECK(o.register_page(1u, 1u, nullptr, 0u, false) == ClStatus::InvalidArgument);
    }

    // check_invariants() negative paths.
    auto corrupt = [](int which) {
        auto c = std::make_unique<Res>();
        c->reset(20u);
        const uint32_t z[1] = {0u};
        const uint32_t y[1] = {1u};
        c->register_page(0u, 2u, nullptr, 0u, true);
        c->register_page(1u, 2u, z, 1u, false);
        c->register_page(2u, 2u, y, 1u, false);
        c->begin_frame(1u);
        c->request(2u, 1u);
        for (int k = 0; k < 3; ++k) {
            c->update(4u);
            for (uint32_t i = 0; i < c->load_count(); ++i) {
                c->complete_load(c->load_id(i));
            }
        }
        switch (which) {
        case 0: break;                                                                      // valid
        case 1: ClTestAccess::state(*c, 1u) = PageState::NotResident; break;                // orphan / bytes
        case 2: ClTestAccess::dependents(*c, 0u) = 5u; break;                               // dependent count
        case 3: ClTestAccess::resident_bytes(*c) += 1u; break;                              // accounting
        case 4: ClTestAccess::budget(*c) = 3u; break;                                       // over budget
        case 5: ClTestAccess::coarse(*c, 1u) = 1u; ClTestAccess::coarse(*c, 0u) = 0u; break; // coarse closure
        case 6: ClTestAccess::lru_count(*c) = 7u; break;                                    // LRU count
        case 7: ClTestAccess::dep(*c, 1u) = 2u; break;                                      // forward edge
        case 8: ClTestAccess::request(*c, 0u) = 40u; break;                                 // bad request entry
        case 9: ClTestAccess::page_count(*c) = 2u; break;                                   // registered past count
        case 10: ClTestAccess::state(*c, 2u) = PageState::Loading; break;                   // loading not counted
        case 11: // orphan with consistent counts: page 1 gone, page 2 still resident on it
            ClTestAccess::state(*c, 1u) = PageState::NotResident;
            ClTestAccess::dependents(*c, 0u) = 0u;
            ClTestAccess::resident_bytes(*c) -= 2u;
            break;
        case 12: ClTestAccess::dep_offset(*c, 1u) = 1u; break;                              // edge ranges
        case 13: ClTestAccess::prev(*c, 1u) = 0u; break;                                    // LRU links
        case 14: ClTestAccess::requested_frame(*c, 2u) = 0u; break;                         // stale request entry
        case 15: ClTestAccess::coarse_bytes(*c) = 50u; break;                               // coarse accounting
        default: ClTestAccess::page_count(*c) = 100u; break;                                // count over capacity
        }
        return c->check_invariants();
    };
    for (int k = 0; k < 17; ++k) {
        PR_CHECK(corrupt(k) == (k == 0));
    }
    // Query paths out of range / before loading.
    {
        auto q = std::make_unique<Res>();
        q->reset(10u);
        PR_CHECK(q->register_page(0u, 2u, nullptr, 0u, true) == ClStatus::Ok);
        PR_CHECK(!q->all_coarse_resident() && q->coarse_count() == 1u && q->coarse_bytes() == 2u);
        PR_CHECK(!q->is_coarse(999u) && !q->requested(999u) && q->priority(999u) == 0u);
        PR_CHECK(q->dep_count(5u) == 0u && q->dep(5u, 0u) == Res::kNone && q->frame() == 0u);
        PR_CHECK(q->set_budget(40u) == ClStatus::Ok && q->budget() == 40u && q->evict_count() == 0u);
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "all";
    const bool all = std::strcmp(which, "all") == 0;
    bool ran = false;
    struct Suite {
        const char* name;
        void (*fn)();
    } suites[] = {{"model", testModel}, {"props", testProps}, {"live", testLive}, {"big", testBig}, {"errors", testErrors}};
    for (const Suite& s : suites) {
        if (all || std::strcmp(which, s.name) == 0) {
            ran = true;
            s.fn();
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite '%s' (model|props|live|big|errors|all)\n", which);
        return 2;
    }
    std::printf("core_logic page_residency[%s]: %s (%d failed checks)\n", which, g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
