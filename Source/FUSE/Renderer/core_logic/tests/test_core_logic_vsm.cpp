// WP-0.8: VSM page table + physical page pool — unit tests, a shadow-model property test
// (random operation sequences) and invariant-checker negative tests.
#include "cl_test_access.hpp"
#include "cl_test_util.hpp"

#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace fuse::core_logic;

namespace {

// 5x5 pages x 3 levels = 75 virtual pages (2 request words, the second one partial).
typedef VsmPageTable<5u, 3u, 12u> SmallVsm;
typedef ClTestAccess TA;

template <class T> std::unique_ptr<T> make_table() {
    std::unique_ptr<T> t(new T());
    t->reset();
    return t;
}

void decode(uint32_t v, uint32_t P, uint32_t& level, uint32_t& x, uint32_t& y) {
    level = v / (P * P);
    const uint32_t r = v % (P * P);
    y = r / P;
    x = r % P;
}

void test_basic() {
    std::unique_ptr<SmallVsm> t = make_table<SmallVsm>();
    CL_CHECK(t->check_invariants() && t->free_count() == 12u && t->mapped_count() == 0u);
    CL_CHECK(t->begin_frame(0u) == ClStatus::InvalidArgument);
    CL_CHECK(t->begin_frame(1u) == ClStatus::Ok && t->frame() == 1u);
    CL_CHECK(t->begin_frame(1u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark(3u, 0u, 0u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark(0u, 5u, 0u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark(0u, 0u, 5u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark_rect(3u, 0u, 0u, 1u, 1u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark_rect(0u, 2u, 0u, 1u, 1u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark_rect(0u, 0u, 2u, 1u, 1u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark_rect(0u, 0u, 0u, 5u, 1u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark_rect(0u, 0u, 0u, 1u, 5u) == ClStatus::InvalidArgument);
    CL_CHECK(t->mark_rect(0u, 0u, 0u, 1u, 1u) == ClStatus::Ok);
    CL_CHECK(t->mark(2u, 4u, 4u) == ClStatus::Ok);
    VsmUpdateStats st = t->update();
    CL_CHECK(st.requested == 5u && st.allocated == 5u && st.toRender == 5u && st.failed == 0u);
    CL_CHECK(t->lookup(0u, 0u, 0u) == 0u && t->lookup(2u, 4u, 4u) == 4u);
    CL_CHECK(t->lookup(3u, 0u, 0u) == SmallVsm::kNone && t->lookup(0u, 5u, 0u) == SmallVsm::kNone &&
             t->lookup(0u, 0u, 5u) == SmallVsm::kNone && t->lookup(0u, 2u, 2u) == SmallVsm::kNone);
    CL_CHECK(t->is_cached(0u, 0u, 0u) && !t->is_cached(1u, 0u, 0u));
    CL_CHECK(!t->is_cached(3u, 0u, 0u) && !t->is_cached(0u, 5u, 0u) && !t->is_cached(0u, 0u, 5u));
    CL_CHECK(t->render_count() == 5u && t->render_page(0u) == 0u && t->render_page(5u) == SmallVsm::kNone);
    CL_CHECK(t->owner_of(0u) == 0u && t->owner_of(99u) == SmallVsm::kNone);
    CL_CHECK(t->last_used(0u) == 1u && t->last_used(99u) == 0u && t->lru_tail() != SmallVsm::kNone);
    // Same pages next frame: cached, nothing to render.
    CL_CHECK(t->begin_frame(2u) == ClStatus::Ok);
    t->mark_rect(0u, 0u, 0u, 1u, 1u);
    st = t->update();
    CL_CHECK(st.alreadyMapped == 4u && st.allocated == 0u && st.toRender == 0u);
    // Invalidate one page -> re-render when requested.
    CL_CHECK(t->invalidate_rect(0u, 1u, 1u, 1u, 1u) == 1u);
    CL_CHECK(t->invalidate_rect(0u, 1u, 1u, 1u, 1u) == 0u); // already clear
    CL_CHECK(t->invalidate_rect(3u, 0u, 0u, 1u, 1u) == 0u && t->invalidate_rect(0u, 2u, 0u, 1u, 1u) == 0u &&
             t->invalidate_rect(0u, 0u, 2u, 1u, 1u) == 0u && t->invalidate_rect(0u, 5u, 0u, 6u, 1u) == 0u &&
             t->invalidate_rect(0u, 0u, 5u, 1u, 6u) == 0u);
    CL_CHECK(t->begin_frame(3u) == ClStatus::Ok);
    t->mark(0u, 1u, 1u);
    st = t->update();
    CL_CHECK(st.toRender == 1u && t->render_page(0u) == t->index(0u, 1u, 1u));
    // Unmap / NotFound / invalid.
    CL_CHECK(t->unmap(0u, 1u, 1u) == ClStatus::Ok && t->unmap(0u, 1u, 1u) == ClStatus::NotFound);
    CL_CHECK(t->unmap(3u, 0u, 0u) == ClStatus::InvalidArgument && t->unmap(0u, 5u, 0u) == ClStatus::InvalidArgument &&
             t->unmap(0u, 0u, 5u) == ClStatus::InvalidArgument);
    CL_CHECK(t->check_invariants() && t->free_count() + t->mapped_count() == 12u);
    // Age-based eviction: frame 3 minus last use (1..3).
    CL_CHECK(t->evict_older_than(5u) == 0u);
    CL_CHECK(t->evict_older_than(1u) == 1u); // (2,4,4) last used at frame 1
    CL_CHECK(t->evict_older_than(0u) == 3u);
    CL_CHECK(t->mapped_count() == 0u && t->evict_older_than(0u) == 0u && t->check_invariants());
    // Bounds: an inverted box is ignored; a box fully outside the clipmap is skipped per level.
    VsmBounds inv = {1, 0, 0, 0};
    CL_CHECK(t->invalidate_bounds(inv) == 0u);
    VsmBounds invy = {0, 1, 0, 0};
    CL_CHECK(t->invalidate_bounds(invy) == 0u);
    CL_CHECK(t->invalidate_level(0u) == 0u);
}

void test_pool_exhaustion() {
    std::unique_ptr<SmallVsm> t = make_table<SmallVsm>();
    t->begin_frame(1u);
    // 25 pages requested, 12 physical: 13 fail, no eviction of pages requested this frame.
    t->mark_rect(0u, 0u, 0u, 4u, 4u);
    VsmUpdateStats st = t->update();
    CL_CHECK(st.allocated == 12u && st.failed == 13u && st.evicted == 0u && t->free_count() == 0u);
    CL_CHECK(t->check_invariants());
    // Next frame a different level: the 12 LRU pages are evicted.
    t->begin_frame(2u);
    t->mark_rect(1u, 0u, 0u, 4u, 4u);
    st = t->update();
    CL_CHECK(st.evicted == 12u && st.allocated == 12u && st.failed == 13u);
    CL_CHECK(t->check_invariants());
    // Full pool, all pages used this frame -> an LRU walk of kPhysPages entries.
    std::unique_ptr<SmallVsm> u = make_table<SmallVsm>();
    u->begin_frame(1u);
    u->mark_rect(0u, 0u, 0u, 4u, 4u);
    u->update();
    CL_CHECK(u->check_invariants());
}

void test_default_config_smoke() {
    std::unique_ptr<VsmPageTable16k> t = make_table<VsmPageTable16k>();
    CL_CHECK(VsmPageTable16k::kVirtualPages == 128u * 128u * 16u);
    t->begin_frame(1u);
    for (uint32_t l = 0u; l < 16u; ++l) {
        t->mark_rect(l, 60u, 60u, 67u, 67u); // 64 pages per level around the centre
    }
    VsmUpdateStats st = t->update();
    CL_CHECK(st.allocated == 1024u && st.failed == 0u && t->check_invariants());
    VsmBounds b = {-8, -8, 7, 7};
    CL_CHECK(t->invalidate_bounds(b) > 0u && t->check_invariants());
}

// Brute force: does level-L page (x, y) intersect level-0 box b (centre-relative)?
bool page_hits_bounds(uint32_t P, uint32_t L, uint32_t x, uint32_t y, const VsmBounds& b) {
    const int64_t s = static_cast<int64_t>(1) << L;
    const int64_t half = static_cast<int64_t>(P / 2u);
    const int64_t X0 = (static_cast<int64_t>(x) - half) * s;
    const int64_t Y0 = (static_cast<int64_t>(y) - half) * s;
    return X0 <= b.x1 && b.x0 <= X0 + s - 1 && Y0 <= b.y1 && b.y0 <= Y0 + s - 1;
}

template <class Vsm, uint32_t P, uint32_t L, uint32_t PH> void property_run(uint64_t seed, uint32_t steps) {
    std::unique_ptr<Vsm> t = make_table<Vsm>();
    cltest::Rng rng(seed);
    const int base = cltest::failures();
    const uint32_t V = P * P * L;
    std::map<uint32_t, uint32_t> lastUse; // shadow: virtual page -> frame of last use (mapped pages)
    std::set<uint32_t> cached;            // shadow cached flags
    std::set<uint32_t> requested;
    uint32_t frame = 0u;
    for (uint32_t step = 0u; step < steps; ++step) {
        const uint32_t op = rng.below(100u);
        if (op < 12u) {
            frame += 1u + rng.below(3u);
            CL_CHECK(t->begin_frame(frame) == ClStatus::Ok);
            requested.clear();
        } else if (op < 40u) {
            const uint32_t lvl = rng.below(L + 1u); // sometimes invalid
            const uint32_t x0 = rng.below(P + 1u), y0 = rng.below(P + 1u);
            if (rng.chance(50u)) {
                const ClStatus s = t->mark(lvl, x0, y0);
                const bool ok = lvl < L && x0 < P && y0 < P;
                CL_CHECK((s == ClStatus::Ok) == ok);
                if (ok) {
                    requested.insert(t->index(lvl, x0, y0));
                }
            } else {
                const uint32_t x1 = x0 + rng.below(3u), y1 = y0 + rng.below(3u);
                const ClStatus s = t->mark_rect(lvl, x0, y0, x1, y1);
                const bool ok = lvl < L && x1 < P && y1 < P;
                CL_CHECK((s == ClStatus::Ok) == ok);
                if (ok) {
                    for (uint32_t y = y0; y <= y1; ++y) {
                        for (uint32_t x = x0; x <= x1; ++x) {
                            requested.insert(t->index(lvl, x, y));
                        }
                    }
                }
            }
        } else if (op < 60u) {
            std::vector<uint32_t> before(V);
            for (uint32_t v = 0u; v < V; ++v) {
                uint32_t l, x, y;
                decode(v, P, l, x, y);
                before[v] = t->lookup(l, x, y);
            }
            const std::map<uint32_t, uint32_t> lastBefore = lastUse;
            const std::set<uint32_t> cachedBefore = cached;
            const uint32_t freeBefore = t->free_count();
            const VsmUpdateStats st = t->update();
            uint32_t alreadyMapped = 0u, newly = 0u, failed = 0u, evicted = 0u;
            std::set<uint32_t> expectRender;
            for (uint32_t v = 0u; v < V; ++v) {
                uint32_t l, x, y;
                decode(v, P, l, x, y);
                const uint32_t now = t->lookup(l, x, y);
                const bool req = requested.count(v) != 0u;
                if (req) {
                    if (before[v] != Vsm::kNone) {
                        ++alreadyMapped;
                        CL_CHECK(now == before[v]); // requested mapped pages never move
                        if (cachedBefore.count(v) == 0u) {
                            expectRender.insert(v);
                        }
                    } else if (now != Vsm::kNone) {
                        ++newly;
                        expectRender.insert(v);
                    } else {
                        ++failed;
                    }
                    if (now != Vsm::kNone) {
                        lastUse[v] = frame;
                        cached.insert(v);
                        CL_CHECK(t->is_cached(l, x, y));
                    }
                } else if (before[v] != Vsm::kNone && now == Vsm::kNone) {
                    ++evicted;
                    lastUse.erase(v);
                    cached.erase(v);
                } else {
                    CL_CHECK(now == before[v]);
                }
            }
            CL_CHECK(st.requested == requested.size());
            CL_CHECK(st.alreadyMapped == alreadyMapped && st.allocated == newly && st.failed == failed &&
                     st.evicted == evicted);
            CL_CHECK(newly <= freeBefore + evicted);
            if (failed > 0u) {
                // No free page and nothing evictable: every mapped page was used this frame.
                CL_CHECK(t->free_count() == 0u);
                for (std::map<uint32_t, uint32_t>::const_iterator it = lastUse.begin(); it != lastUse.end(); ++it) {
                    CL_CHECK(it->second == frame);
                }
            }
            // LRU: every evicted page was at least as old as every surviving unrequested page.
            if (evicted > 0u) {
                uint32_t newestEvicted = 0u;
                for (std::map<uint32_t, uint32_t>::const_iterator it = lastBefore.begin(); it != lastBefore.end(); ++it) {
                    if (lastUse.count(it->first) == 0u && it->second > newestEvicted) {
                        newestEvicted = it->second;
                    }
                }
                for (std::map<uint32_t, uint32_t>::const_iterator it = lastUse.begin(); it != lastUse.end(); ++it) {
                    if (requested.count(it->first) == 0u) {
                        CL_CHECK(it->second >= newestEvicted);
                    }
                }
            }
            std::set<uint32_t> rendered;
            for (uint32_t i = 0u; i < t->render_count(); ++i) {
                CL_CHECK(rendered.insert(t->render_page(i)).second);
            }
            CL_CHECK(rendered == expectRender && st.toRender == rendered.size());
        } else if (op < 68u) {
            const uint32_t v = rng.below(V);
            uint32_t l, x, y;
            decode(v, P, l, x, y);
            const bool mapped = lastUse.count(v) != 0u;
            CL_CHECK(t->unmap(l, x, y) == (mapped ? ClStatus::Ok : ClStatus::NotFound));
            lastUse.erase(v);
            cached.erase(v);
        } else if (op < 74u) {
            const uint32_t age = rng.below(4u);
            uint32_t expect = 0u;
            for (std::map<uint32_t, uint32_t>::iterator it = lastUse.begin(); it != lastUse.end();) {
                if (frame - it->second > age) {
                    ++expect;
                    cached.erase(it->first);
                    lastUse.erase(it++);
                } else {
                    ++it;
                }
            }
            CL_CHECK(t->evict_older_than(age) == expect);
        } else if (op < 84u) {
            const uint32_t lvl = rng.below(L);
            const uint32_t x0 = rng.below(P + 1u), y0 = rng.below(P + 1u);
            const uint32_t x1 = x0 + rng.below(P), y1 = y0 + rng.below(P);
            uint32_t expect = 0u;
            for (uint32_t y = y0; y <= y1 && y < P; ++y) {
                for (uint32_t x = x0; x <= x1 && x < P; ++x) {
                    expect += static_cast<uint32_t>(cached.erase(t->index(lvl, x, y)));
                }
            }
            CL_CHECK(t->invalidate_rect(lvl, x0, y0, x1, y1) == expect);
        } else if (op < 96u) {
            const int32_t span = static_cast<int32_t>(P) << L;
            VsmBounds b;
            b.x0 = static_cast<int32_t>(rng.below(static_cast<uint32_t>(2 * span))) - span;
            b.y0 = static_cast<int32_t>(rng.below(static_cast<uint32_t>(2 * span))) - span;
            b.x1 = b.x0 + static_cast<int32_t>(rng.below(static_cast<uint32_t>(span / 2)));
            b.y1 = b.y0 + static_cast<int32_t>(rng.below(static_cast<uint32_t>(span / 2)));
            uint32_t expect = 0u;
            for (uint32_t v = 0u; v < V; ++v) {
                uint32_t l, x, y;
                decode(v, P, l, x, y);
                if (page_hits_bounds(P, l, x, y, b)) {
                    expect += static_cast<uint32_t>(cached.erase(v));
                }
            }
            CL_CHECK(t->invalidate_bounds(b) == expect);
        } else {
            const uint32_t lvl = rng.below(L);
            uint32_t expect = 0u;
            for (uint32_t v = lvl * P * P; v < (lvl + 1u) * P * P; ++v) {
                expect += static_cast<uint32_t>(cached.erase(v));
            }
            CL_CHECK(t->invalidate_level(lvl) == expect);
        }
        // Global invariants after every operation.
        CL_CHECK(t->check_invariants());
        CL_CHECK(t->free_count() + t->mapped_count() == PH);
        CL_CHECK(t->mapped_count() == lastUse.size());
        std::set<uint32_t> phys;
        for (uint32_t v = 0u; v < V; ++v) {
            uint32_t l, x, y;
            decode(v, P, l, x, y);
            const uint32_t p = t->lookup(l, x, y);
            CL_CHECK((p != Vsm::kNone) == (lastUse.count(v) != 0u));
            CL_CHECK(t->is_cached(l, x, y) == (cached.count(v) != 0u));
            if (p != Vsm::kNone) {
                CL_CHECK(p < PH && phys.insert(p).second); // no physical page mapped twice
                CL_CHECK(t->owner_of(p) == v);
            }
        }
        if (cltest::failures() != base) {
            std::fprintf(stderr, "vsm property: seed %llu failed at step %u (op %u)\n",
                         static_cast<unsigned long long>(seed), step, op);
            return;
        }
    }
}

void test_invariant_checker() {
    // Each corruption must be detected (exercises every negative path of check_invariants()).
    typedef VsmPageTable<2u, 1u, 3u> Tiny; // 4 virtual pages, 3 physical
    struct Setup {
        static std::unique_ptr<Tiny> mapped_two() {
            std::unique_ptr<Tiny> t = make_table<Tiny>();
            t->begin_frame(1u);
            t->mark(0u, 0u, 0u);
            t->update();
            t->begin_frame(2u);
            t->mark(0u, 1u, 0u);
            t->update(); // head = phys 1 (frame 2), tail = phys 0 (frame 1); free = {2}
            return t;
        }
        static std::unique_ptr<Tiny> full() {
            std::unique_ptr<Tiny> t = make_table<Tiny>();
            t->begin_frame(1u);
            t->mark_rect(0u, 0u, 0u, 1u, 1u);
            t->update(); // 3 mapped, 1 failed
            return t;
        }
    };
    int n = 0;
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); CL_CHECK(t->check_invariants()); ++n; }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::freeTop(*t) = 4u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::freeStack(*t)[0] = 7u; CL_CHECK(!t->check_invariants()); }
    {
        std::unique_ptr<Tiny> t = Setup::mapped_two();
        TA::freeTop(*t) = 2u;
        TA::freeStack(*t)[1] = TA::freeStack(*t)[0];
        CL_CHECK(!t->check_invariants()); // duplicate on the free stack
    }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::owner(*t)[2] = 3u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::next(*t)[1] = 9u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::next(*t)[1] = 2u; CL_CHECK(!t->check_invariants()); } // free page on LRU
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::prev(*t)[0] = 2u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::owner(*t)[1] = 9u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::pte(*t)[1] = 0u; CL_CHECK(!t->check_invariants()); }
    {
        std::unique_ptr<Tiny> t = Setup::mapped_two();
        TA::pte(*t)[1] = Tiny::kPteMapped | 0u;
        CL_CHECK(!t->check_invariants()); // entry points at another physical page
    }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::lastUsed(*t)[0] = 5u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::tail(*t) = 1u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::lruCount(*t) = 1u; CL_CHECK(!t->check_invariants()); }
    {
        std::unique_ptr<Tiny> t = Setup::mapped_two();
        TA::freeTop(*t) = 0u; // page 2 neither free nor mapped (leak)
        TA::lruCount(*t) = 2u;
        CL_CHECK(!t->check_invariants());
    }
    {
        std::unique_ptr<Tiny> t = Setup::full();
        CL_CHECK(t->check_invariants());
        const uint32_t tail = TA::tail(*t);
        TA::next(*t)[tail] = TA::head(*t); // cycle through a full LRU
        CL_CHECK(!t->check_invariants());
    }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::pte(*t)[3] = Tiny::kPteMapped | 9u; CL_CHECK(!t->check_invariants()); }
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::pte(*t)[3] = Tiny::kPteMapped | 1u; CL_CHECK(!t->check_invariants()); } // 2 virtual -> 1 physical
    { std::unique_ptr<Tiny> t = Setup::mapped_two(); TA::pte(*t)[3] = Tiny::kPteCached; CL_CHECK(!t->check_invariants()); }
    CL_CHECK(n == 1);
}

} // namespace

int run_vsm_tests() {
    test_basic();
    test_pool_exhaustion();
    test_default_config_smoke();
    test_invariant_checker();
    const uint32_t runs = 60u * cltest::iter_scale();
    for (uint32_t i = 0u; i < runs; ++i) {
        property_run<SmallVsm, 5u, 3u, 12u>(1000u + i, 400u);
        property_run<VsmPageTable<8u, 2u, 40u>, 8u, 2u, 40u>(5000u + i, 200u);
    }
    std::printf("vsm property: %u runs x (400 + 200) random operations\n", runs);
    return cltest::failures();
}
