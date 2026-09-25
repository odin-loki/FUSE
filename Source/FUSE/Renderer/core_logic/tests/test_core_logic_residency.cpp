// WP-0.8: cluster-page residency LRU — unit tests, random-operation property test against an
// independent shadow model, coarse-LOD liveness, and invariant-checker negative tests.
#include "cl_test_access.hpp"
#include "cl_test_util.hpp"

#include <memory>
#include <vector>

using namespace fuse::core_logic;

namespace {

typedef ClusterResidency<16u, 16u> Small;
typedef ClTestAccess TA;
const uint32_t kNone = 0xFFFFFFFFu;

template <class T> std::unique_ptr<T> make(uint64_t budget) {
    std::unique_ptr<T> t(new T());
    t->reset(budget);
    return t;
}

void complete_all(Small& t) {
    for (uint32_t i = 0u; i < t.load_count(); ++i) {
        CL_CHECK(t.complete_load(t.load_id(i)) == ClStatus::Ok);
    }
}

void test_basic() {
    std::unique_ptr<Small> t = make<Small>(1000u);
    CL_CHECK(t->check_invariants());
    // Registration errors.
    CL_CHECK(t->register_page(16u, 10u, kNone, false) == ClStatus::InvalidArgument);
    CL_CHECK(t->register_page(0u, 0u, kNone, false) == ClStatus::InvalidArgument);
    CL_CHECK(t->register_page(0u, 10u, 16u, false) == ClStatus::InvalidArgument);
    CL_CHECK(t->register_page(0u, 10u, 5u, false) == ClStatus::InvalidArgument); // parent unregistered
    CL_CHECK(t->register_page(0u, 1001u, kNone, true) == ClStatus::OutOfBudget);
    CL_CHECK(t->register_page(0u, 100u, kNone, true) == ClStatus::Ok);  // coarse root
    CL_CHECK(t->register_page(0u, 100u, kNone, true) == ClStatus::InvalidArgument); // twice
    CL_CHECK(t->register_page(1u, 100u, 0u, true) == ClStatus::Ok);     // coarse child of coarse
    CL_CHECK(t->register_page(2u, 300u, 1u, false) == ClStatus::Ok);    // fine
    CL_CHECK(t->register_page(3u, 300u, 2u, false) == ClStatus::Ok);    // finer
    CL_CHECK(t->register_page(4u, 50u, 2u, true) == ClStatus::InvalidArgument); // coarse under fine
    CL_CHECK(t->register_page(4u, 400u, kNone, false) == ClStatus::Ok);
    CL_CHECK(t->coarse_bytes() == 200u && t->state(0u) == ResidencyState::NotResident &&
             t->state(99u) == ResidencyState::Unregistered);
    // Frames.
    CL_CHECK(t->begin_frame(0u) == ClStatus::InvalidArgument);
    CL_CHECK(t->begin_frame(kNone) == ClStatus::InvalidArgument);
    CL_CHECK(t->begin_frame(1u) == ClStatus::Ok && t->begin_frame(1u) == ClStatus::InvalidArgument);
    // Requests.
    CL_CHECK(t->request(16u, 1u) == ClStatus::InvalidArgument && t->request(9u, 1u) == ClStatus::InvalidArgument);
    CL_CHECK(t->request(3u, 7u) == ClStatus::Ok); // pulls 2, 1, 0 in with priority >= 7
    CL_CHECK(!t->all_coarse_resident());
    // Update: coarse 0 loads first; 1 waits for 0; 2 and 3 wait for their parents.
    ResidencyUpdateStats st = t->update(8u);
    CL_CHECK(st.loads == 1u && t->load_id(0u) == 0u && t->load_id(1u) == kNone && st.candidates == 4u);
    CL_CHECK(t->check_invariants());
    CL_CHECK(t->complete_load(0u) == ClStatus::Ok && t->complete_load(0u) == ClStatus::NotFound);
    CL_CHECK(t->complete_load(99u) == ClStatus::NotFound && t->fail_load(99u) == ClStatus::NotFound &&
             t->fail_load(0u) == ClStatus::NotFound);
    t->begin_frame(2u);
    t->request(3u, 7u);
    st = t->update(8u);
    CL_CHECK(st.loads == 1u && t->load_id(0u) == 1u);
    complete_all(*t);
    t->begin_frame(3u);
    t->request(3u, 7u);
    st = t->update(8u);
    CL_CHECK(st.loads == 1u && t->load_id(0u) == 2u);
    // IO failure returns the bytes; the next frame retries.
    CL_CHECK(t->fail_load(2u) == ClStatus::Ok && t->loading_bytes() == 0u);
    t->begin_frame(4u);
    t->request(3u, 7u);
    t->update(8u);
    complete_all(*t);
    t->begin_frame(5u);
    t->request(3u, 7u);
    t->update(8u);
    complete_all(*t);
    CL_CHECK(t->state(3u) == ResidencyState::Resident && t->resident_bytes() == 800u);
    CL_CHECK(t->all_coarse_resident() && t->check_invariants());
    // Page 4 (400 B) needs room: 3 is requested only in this frame... not requested -> evictable.
    t->begin_frame(6u);
    t->request(4u, 1u);
    st = t->update(8u);
    CL_CHECK(st.evictions == 1u && t->evict_id(0u) == 3u && t->evict_id(1u) == kNone && st.loads == 1u);
    CL_CHECK(t->state(2u) == ResidencyState::Resident); // 2 had to wait: 3 was its child
    complete_all(*t);
    CL_CHECK(t->check_invariants() && t->resident_bytes() + t->loading_bytes() <= t->budget());
    // maxLoads = 0 defers everything.
    t->begin_frame(7u);
    t->request(3u, 1u);
    st = t->update(0u);
    CL_CHECK(st.loads == 0u && st.deferred == 1u);
    // Budget changes.
    CL_CHECK(t->set_budget(199u) == ClStatus::OutOfBudget);
    CL_CHECK(t->set_budget(200u) == ClStatus::Ok && t->resident_bytes() == 200u && t->evict_count() == 2u);
    CL_CHECK(t->set_budget(5000u) == ClStatus::Ok && t->budget() == 5000u);
    CL_CHECK(t->check_invariants());
}

void test_budget_blocked_by_loading() {
    std::unique_ptr<Small> t = make<Small>(1000u);
    t->register_page(0u, 100u, kNone, true);
    t->register_page(1u, 500u, kNone, false);
    t->begin_frame(1u);
    t->request(1u, 1u);
    t->update(8u); // both loading
    CL_CHECK(t->loading_bytes() == 600u);
    CL_CHECK(t->set_budget(300u) == ClStatus::OutOfBudget && t->budget() == 1000u);
    CL_CHECK(t->check_invariants());
}

void test_request_capacity_and_priority_order() {
    typedef ClusterResidency<8u, 3u> T;
    std::unique_ptr<T> t = make<T>(1000u);
    for (uint32_t i = 0u; i < 8u; ++i) {
        t->register_page(i, 10u, kNone, false);
    }
    t->begin_frame(1u);
    CL_CHECK(t->request(5u, 1u) == ClStatus::Ok && t->request(6u, 9u) == ClStatus::Ok);
    CL_CHECK(t->request(5u, 4u) == ClStatus::Ok); // same frame: priority raised, no new slot
    CL_CHECK(t->request(7u, 4u) == ClStatus::Ok);
    CL_CHECK(t->request(2u, 1u) == ClStatus::CapacityExceeded);
    t->update(8u);
    // 6 (9) first, then 5 and 7 tie at 4 -> id order.
    CL_CHECK(t->load_count() == 3u && t->load_id(0u) == 6u && t->load_id(1u) == 5u && t->load_id(2u) == 7u);
    // Depth tie-break: parent before child at equal priority.
    typedef ClusterResidency<4u, 4u> U;
    std::unique_ptr<U> u = make<U>(1000u);
    u->register_page(0u, 10u, kNone, false);
    u->register_page(1u, 10u, kNone, false);
    u->register_page(2u, 10u, 1u, false);
    u->begin_frame(1u);
    u->request(2u, 5u);
    u->request(0u, 5u);
    u->update(8u);
    CL_CHECK(u->load_count() == 2u && u->load_id(0u) == 0u && u->load_id(1u) == 1u);
}

void test_coarse_evicts_requested() {
    // Coarse loads may evict requested fine pages; fine loads never evict requested pages.
    std::unique_ptr<Small> t = make<Small>(300u);
    t->register_page(0u, 100u, kNone, false);
    t->register_page(1u, 200u, kNone, false);
    t->begin_frame(1u);
    t->request(0u, 1u);
    t->request(1u, 1u);
    t->update(8u);
    complete_all(*t);
    t->register_page(2u, 150u, kNone, true); // coarse registered late (fits the budget)
    t->begin_frame(2u);
    t->request(0u, 1u);
    t->request(1u, 1u);
    t->register_page(3u, 200u, kNone, false);
    t->request(3u, 99u);
    ResidencyUpdateStats st = t->update(8u);
    CL_CHECK(t->state(2u) == ResidencyState::Loading && st.evictions >= 1u);
    CL_CHECK(t->state(3u) == ResidencyState::NotResident && st.deferred == 1u); // cannot evict requested
    CL_CHECK(t->check_invariants());
}

// Independent shadow model for the property test.
struct Shadow {
    std::vector<int> parent;
    std::vector<uint64_t> size;
    std::vector<bool> coarse;
    std::vector<ResidencyState> st;
};

void property_run(uint64_t seed, uint32_t steps) {
    cltest::Rng rng(seed);
    const int base = cltest::failures();
    const uint64_t budget = 200u + rng.below(800u);
    std::unique_ptr<Small> t = make<Small>(budget);
    Shadow sh;
    uint64_t coarseBytes = 0u;
    const uint32_t N = 4u + rng.below(13u);
    for (uint32_t i = 0u; i < N; ++i) {
        const int par = (i == 0u || rng.chance(30u)) ? -1 : static_cast<int>(rng.below(i));
        const uint64_t sz = 1u + rng.below(120u);
        bool coarse = rng.chance(25u) && (par < 0 || sh.coarse[static_cast<size_t>(par)]);
        const ClStatus s = t->register_page(i, sz, par < 0 ? kNone : static_cast<uint32_t>(par), coarse);
        if (coarse && coarseBytes + sz > budget) {
            CL_CHECK(s == ClStatus::OutOfBudget);
            coarse = false;
            CL_CHECK(t->register_page(i, sz, par < 0 ? kNone : static_cast<uint32_t>(par), false) == ClStatus::Ok);
        } else {
            CL_CHECK(s == ClStatus::Ok);
        }
        if (coarse) {
            coarseBytes += sz;
        }
        sh.parent.push_back(par);
        sh.size.push_back(sz);
        sh.coarse.push_back(coarse);
        sh.st.push_back(ResidencyState::NotResident);
    }
    uint32_t frame = 0u;
    std::vector<uint32_t> requestedFrame(N, kNone);
    for (uint32_t step = 0u; step < steps; ++step) {
        const uint32_t op = rng.below(100u);
        if (op < 15u) {
            ++frame;
            CL_CHECK(t->begin_frame(frame) == ClStatus::Ok);
        } else if (op < 45u) {
            const uint32_t id = rng.below(N);
            const ClStatus s = t->request(id, rng.below(50u));
            if (s == ClStatus::Ok) {
                for (int c = static_cast<int>(id); c >= 0; c = sh.parent[static_cast<size_t>(c)]) {
                    requestedFrame[static_cast<size_t>(c)] = frame;
                }
            }
        } else if (op < 70u) {
            const std::vector<ResidencyState> before = sh.st;
            const ResidencyUpdateStats us = t->update(rng.below(6u));
            bool coarseLoaded = false;
            for (uint32_t i = 0u; i < t->load_count(); ++i) {
                const uint32_t id = t->load_id(i);
                CL_CHECK(before[id] == ResidencyState::NotResident);
                sh.st[id] = ResidencyState::Loading;
                coarseLoaded = coarseLoaded || sh.coarse[id];
                // Parent resident when the load is issued.
                CL_CHECK(sh.parent[id] < 0 || sh.st[static_cast<size_t>(sh.parent[id])] == ResidencyState::Resident);
                // Load order: coarse first, then non-increasing priority.
                if (i > 0u) {
                    CL_CHECK(!(sh.coarse[id] && !sh.coarse[t->load_id(i - 1u)]));
                }
            }
            CL_CHECK(us.loads == t->load_count() && us.evictions == t->evict_count());
            for (uint32_t i = 0u; i < t->evict_count(); ++i) {
                const uint32_t id = t->evict_id(i);
                CL_CHECK(before[id] == ResidencyState::Resident && !sh.coarse[id]); // coarse never evicted
                CL_CHECK(coarseLoaded || requestedFrame[id] != frame); // fine loads keep requested pages
                sh.st[id] = ResidencyState::NotResident;
            }
        } else if (op < 85u) {
            // Complete or fail a random loading page.
            for (uint32_t k = 0u; k < N; ++k) {
                const uint32_t id = (k + rng.below(N)) % N;
                if (sh.st[id] == ResidencyState::Loading) {
                    if (rng.chance(85u)) {
                        CL_CHECK(t->complete_load(id) == ClStatus::Ok);
                        sh.st[id] = ResidencyState::Resident;
                    } else {
                        CL_CHECK(t->fail_load(id) == ClStatus::Ok);
                        sh.st[id] = ResidencyState::NotResident;
                    }
                    break;
                }
            }
        } else if (op < 92u) {
            const uint64_t nb = rng.below(static_cast<uint32_t>(budget * 2u));
            uint64_t inflight = 0u;
            for (uint32_t i = 0u; i < N; ++i) {
                inflight += sh.st[i] == ResidencyState::Loading ? sh.size[i] : 0u;
            }
            const ClStatus s = t->set_budget(nb);
            if (nb < coarseBytes) {
                CL_CHECK(s == ClStatus::OutOfBudget);
            }
            if (s == ClStatus::Ok) {
                CL_CHECK(t->budget() == nb);
            }
            for (uint32_t i = 0u; i < t->evict_count(); ++i) {
                const uint32_t id = t->evict_id(i);
                CL_CHECK(sh.st[id] == ResidencyState::Resident && !sh.coarse[id]);
                sh.st[id] = ResidencyState::NotResident;
            }
            (void)inflight;
        } else {
            // Coarse-LOD guarantee (liveness): with an unconstrained IO queue, the coarse LOD is
            // fully resident after at most 2N+2 update/complete rounds (fine chains are evicted
            // leaf-first, coarse chains load one level per round).
            for (uint32_t round = 0u; round < 2u * N + 2u; ++round) {
                ++frame;
                t->begin_frame(frame);
                t->update(N);
                for (uint32_t i = 0u; i < t->evict_count(); ++i) {
                    sh.st[t->evict_id(i)] = ResidencyState::NotResident;
                }
                for (uint32_t i = 0u; i < N; ++i) {
                    if (t->state(i) == ResidencyState::Loading) {
                        t->complete_load(i);
                    }
                }
                for (uint32_t i = 0u; i < N; ++i) {
                    sh.st[i] = t->state(i);
                }
            }
            CL_CHECK(t->all_coarse_resident());
        }
        // Invariants after every operation, recomputed from the shadow model.
        CL_CHECK(t->check_invariants());
        uint64_t res = 0u, loading = 0u;
        for (uint32_t i = 0u; i < N; ++i) {
            CL_CHECK(t->state(i) == sh.st[i]);
            if (sh.st[i] == ResidencyState::Resident) {
                res += sh.size[i];
            } else if (sh.st[i] == ResidencyState::Loading) {
                loading += sh.size[i];
            }
            if (sh.st[i] != ResidencyState::NotResident && sh.parent[i] >= 0) {
                CL_CHECK(sh.st[static_cast<size_t>(sh.parent[i])] == ResidencyState::Resident); // no orphans
            }
        }
        CL_CHECK(res == t->resident_bytes() && loading == t->loading_bytes());
        CL_CHECK(res + loading <= t->budget()); // budget never exceeded
        if (cltest::failures() != base) {
            std::fprintf(stderr, "residency property: seed %llu failed at step %u (op %u)\n",
                         static_cast<unsigned long long>(seed), step, op);
            return;
        }
    }
}

void test_invariant_checker() {
    struct Setup {
        static std::unique_ptr<Small> two() {
            std::unique_ptr<Small> t = make<Small>(1000u);
            t->register_page(0u, 100u, kNone, true);
            t->register_page(1u, 100u, 0u, false);
            t->begin_frame(1u);
            t->request(1u, 1u);
            t->update(8u);
            complete_all(*t);
            t->begin_frame(2u);
            t->request(1u, 1u);
            t->update(8u);
            complete_all(*t); // 0 and 1 resident
            return t;
        }
    };
    { std::unique_ptr<Small> t = Setup::two(); CL_CHECK(t->check_invariants()); }
    { std::unique_ptr<Small> t = Setup::two(); TA::activeChildren(*t)[0] = 0u; CL_CHECK(!t->check_invariants()); }
    {
        std::unique_ptr<Small> t = Setup::two();
        TA::state(*t)[0] = ResidencyState::Loading; // child resident under a loading parent
        CL_CHECK(!t->check_invariants());
    }
    { std::unique_ptr<Small> t = Setup::two(); TA::residentBytes(*t) = 1u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Small> t = Setup::two(); TA::loadingBytes(*t) = 1u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Small> t = Setup::two(); TA::coarseBytes(*t) = 1u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Small> t = Setup::two(); TA::budget(*t) = 150u; CL_CHECK(!t->check_invariants()); }
    {
        std::unique_ptr<Small> t = make<Small>(1000u);
        t->register_page(0u, 100u, kNone, true);
        TA::budget(*t) = 50u; // coarse total over budget, nothing resident
        CL_CHECK(!t->check_invariants());
    }
    {
        std::unique_ptr<Small> t = Setup::two();
        TA::state(*t)[1] = ResidencyState::Loading; // on the LRU but not resident
        TA::residentBytes(*t) = 100u;
        TA::loadingBytes(*t) = 100u;
        CL_CHECK(!t->check_invariants());
    }
    { std::unique_ptr<Small> t = Setup::two(); TA::prev(*t)[TA::head(*t)] = 5u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Small> t = Setup::two(); TA::tail(*t) = TA::head(*t); CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Small> t = Setup::two(); TA::lruCount(*t) = 1u; CL_CHECK(!t->check_invariants()); }
    {
        std::unique_ptr<Small> t = Setup::two();
        const uint32_t tail = TA::tail(*t);
        TA::next(*t)[tail] = kNone;
        TA::head(*t) = tail; // LRU shows 1 page, 2 are resident
        TA::prev(*t)[tail] = kNone;
        CL_CHECK(!t->check_invariants());
    }
    {
        // Full LRU (all kMaxPages resident) with a cycle: walk hits the bound.
        typedef ClusterResidency<3u, 3u> T3;
        std::unique_ptr<T3> t = make<T3>(1000u);
        for (uint32_t i = 0u; i < 3u; ++i) {
            t->register_page(i, 10u, kNone, false);
        }
        t->begin_frame(1u);
        t->request(0u, 1u);
        t->request(1u, 1u);
        t->request(2u, 1u);
        t->update(8u);
        for (uint32_t i = 0u; i < 3u; ++i) {
            t->complete_load(i);
        }
        CL_CHECK(t->check_invariants());
        TA::next(*t)[TA::tail(*t)] = TA::head(*t);
        CL_CHECK(!t->check_invariants());
    }
}

} // namespace

int run_residency_tests() {
    test_basic();
    test_budget_blocked_by_loading();
    test_request_capacity_and_priority_order();
    test_coarse_evicts_requested();
    test_invariant_checker();
    const uint32_t runs = 400u * cltest::iter_scale();
    for (uint32_t i = 0u; i < runs; ++i) {
        property_run(7000u + i, 300u);
    }
    std::printf("residency property: %u runs x 300 random operations\n", runs);
    // Default configuration smoke (16384 pages).
    std::unique_ptr<ClusterResidencyDefault> d(new ClusterResidencyDefault());
    d->reset(64ull << 20);
    for (uint32_t i = 0u; i < 1024u; ++i) {
        d->register_page(i, 128u << 10, i < 8u ? kNone : i / 8u - 1u, i < 8u);
    }
    d->begin_frame(1u);
    for (uint32_t i = 0u; i < 1024u; ++i) {
        d->request(i, i);
    }
    d->update(64u);
    CL_CHECK(d->load_count() == 8u && d->check_invariants()); // only the coarse roots are loadable
    return cltest::failures();
}
