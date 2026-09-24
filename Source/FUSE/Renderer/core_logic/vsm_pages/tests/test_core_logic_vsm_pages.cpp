// WP-3.1 core_logic VSM page model: property tests (`fuse_core_logic_vsm_pages_tests [slots|model|props|big|errors|all]`).
//
//   slots   toroidal slot math: slot <-> absolute round trips, window membership, exhaustive over small windows
//   model   random frames (requests, window scrolls, depth-key changes, invalidate-all, invalidation masks,
//           frame gaps) against an independent brute-force oracle written with the STL (sorted candidate
//           list): page table, physical metadata, statistics and render list equal after every frame
//   props   properties: check_invariants(); requested pages that were mapped keep their physical page;
//           no page used this frame is evicted; a failed need implies no candidate left; the render list
//           is exactly the requested pages that are mapped and were not cached; a repeated static frame
//           renders nothing; LRU: every evicted page is at least as old (bucketed) as every candidate
//           left unused
//   big     the 16-level 128^2-page 16k configuration with 4096 physical pages (smoke + invariants)
//   errors  argument validation and every check_invariants() negative path (ClTestAccess)
#include "fuse/core_logic/vsm_pages.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace fuse {
namespace core_logic {
// Test-only: corrupts pool state to exercise the negative paths of check_invariants().
struct ClTestAccess {
    template <typename P>
    static uint32_t& pte(P& p, uint32_t v) {
        return p.pte_[v];
    }
    template <typename P>
    static uint32_t& owner(P& p, uint32_t i) {
        return p.owner_[i];
    }
    template <typename P>
    static uint32_t& last_used(P& p, uint32_t i) {
        return p.lastUsed_[i];
    }
    template <typename P>
    static uint32_t& phys(P& p) {
        return p.phys_;
    }
};
} // namespace core_logic
} // namespace fuse

namespace {

using namespace fuse::core_logic;

int g_failures = 0;

#define VP_CHECK(cond)                                                                        \
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

// --- slots -----------------------------------------------------------------------------------------
void testSlots() {
    for (uint32_t axis = 2u; axis <= 16u; axis *= 2u) {
        for (int32_t origin = -40; origin <= 40; ++origin) {
            uint32_t inWindow = 0u;
            for (int32_t a = origin - 3 * static_cast<int32_t>(axis); a <= origin + 3 * static_cast<int32_t>(axis); ++a) {
                const bool in = vsm_in_window(a, origin, axis);
                const bool expected = a >= origin - static_cast<int32_t>(axis / 2u) && a < origin + static_cast<int32_t>(axis / 2u);
                VP_CHECK(in == expected);
                const uint32_t s = vsm_abs_to_slot(a, axis);
                VP_CHECK(s < axis);
                VP_CHECK(((a - static_cast<int32_t>(s)) % static_cast<int32_t>(axis)) == 0);
                if (in) {
                    ++inWindow;
                    VP_CHECK(vsm_slot_to_abs(s, origin, axis) == a);
                }
            }
            VP_CHECK(inWindow == axis);
            for (uint32_t s = 0u; s < axis; ++s) {
                const int32_t a = vsm_slot_to_abs(s, origin, axis);
                VP_CHECK(vsm_in_window(a, origin, axis));
                VP_CHECK(vsm_abs_to_slot(a, axis) == s);
            }
        }
    }
    // Extremes of the allowed origin range stay exact.
    for (const int32_t origin : {-kVsmMaxOrigin, kVsmMaxOrigin, 0}) {
        for (uint32_t s = 0u; s < 128u; ++s) {
            const int32_t a = vsm_slot_to_abs(s, origin, 128u);
            VP_CHECK(vsm_in_window(a, origin, 128u) && vsm_abs_to_slot(a, 128u) == s);
        }
    }
    VP_CHECK(vsm_candidate_key(kClInvalid, 0u, 5u) == kVsmFreeKey);
    VP_CHECK(vsm_candidate_key(3u, 5u, 5u) == 0u);
    VP_CHECK(vsm_candidate_key(3u, 4u, 5u) == 1u);
    VP_CHECK(vsm_candidate_key(3u, 1u, 100u) == kVsmAgeBuckets - 1u);
}

// --- oracle ----------------------------------------------------------------------------------------
struct Oracle {
    uint32_t axis = 0, levels = 0, phys = 0;
    std::vector<uint32_t> pte;
    std::vector<uint32_t> owner, lastUsed;
    std::vector<VsmLevelPlacement> placement;
    bool placed = false;
    uint32_t frame = 0;
    std::vector<uint32_t> renderList; // ascending virtual page

    void reset(uint32_t a, uint32_t l, uint32_t p) {
        axis = a;
        levels = l;
        phys = p;
        pte.assign(static_cast<size_t>(a) * a * l, 0u);
        owner.assign(p, kClInvalid);
        lastUsed.assign(p, 0u);
        placement.assign(l, VsmLevelPlacement{0, 0, 0, 0});
        placed = false;
        frame = 0;
    }

    static int64_t absOf(uint32_t slot, int32_t origin, uint32_t axis) {
        // Brute force: scan the window.
        for (int64_t a = static_cast<int64_t>(origin) - axis / 2; a < static_cast<int64_t>(origin) + axis / 2; ++a) {
            if (((a % axis) + axis) % axis == slot) {
                return a;
            }
        }
        return INT64_MIN;
    }

    void update(const VsmFrameInput& in, const std::vector<uint32_t>& req, const std::vector<uint32_t>* inv, VsmPageStats& st) {
        vsm_clear_stats(st);
        frame = in.frame;
        const uint32_t per = axis * axis;
        auto isSet = [](const std::vector<uint32_t>& w, uint32_t v) { return ((w[v / 32] >> (v % 32)) & 1u) != 0u; };
        if (inv != nullptr) {
            for (uint32_t v = 0; v < per * levels; ++v) {
                if (isSet(*inv, v) && (pte[v] & kVsmPteCached)) {
                    pte[v] &= ~kVsmPteCached;
                    ++st.invalidated;
                }
            }
        }
        std::vector<uint32_t> needs;
        for (uint32_t v = 0; v < per * levels; ++v) {
            const uint32_t l = v / per;
            const uint32_t sx = (v % per) % axis;
            const uint32_t sy = (v % per) / axis;
            const VsmLevelPlacement& cur = in.level[l];
            const VsmLevelPlacement prev = placed ? placement[l] : cur;
            const bool changed = in.invalidateAll != 0 || prev.depthKey != cur.depthKey ||
                                 absOf(sx, prev.originX, axis) != absOf(sx, cur.originX, axis) ||
                                 absOf(sy, prev.originY, axis) != absOf(sy, cur.originY, axis);
            if (changed && (pte[v] & kVsmPteCached)) {
                pte[v] &= ~kVsmPteCached;
                ++st.scrolled;
            }
            if (!isSet(req, v)) {
                continue;
            }
            ++st.requested;
            if (pte[v] & kVsmPteMapped) {
                ++st.alreadyMapped;
                lastUsed[pte[v] & kVsmPtePhysMask] = frame;
            } else {
                needs.push_back(v);
            }
        }
        st.needed = static_cast<uint32_t>(needs.size());
        for (uint32_t l = 0; l < levels; ++l) {
            placement[l] = in.level[l];
        }
        placed = true;
        // Candidates: pages not used this frame; free first, then oldest (bucketed), then index.
        struct Cand {
            uint32_t key, p;
        };
        std::vector<Cand> cands;
        for (uint32_t p = 0; p < phys; ++p) {
            uint32_t key;
            if (owner[p] == kClInvalid) {
                key = 1000;
            } else if (lastUsed[p] == frame) {
                continue;
            } else {
                key = std::min(frame - lastUsed[p], 15u);
            }
            cands.push_back({key, p});
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.key > b.key; });
        st.candidates = static_cast<uint32_t>(cands.size());
        for (size_t i = 0; i < needs.size(); ++i) {
            if (i >= cands.size()) {
                ++st.failed;
                continue;
            }
            const uint32_t p = cands[i].p;
            if (owner[p] != kClInvalid) {
                pte[owner[p]] = 0;
                ++st.evicted;
            }
            owner[p] = needs[i];
            lastUsed[p] = frame;
            pte[needs[i]] = kVsmPteMapped | p;
            ++st.allocated;
        }
        renderList.clear();
        for (uint32_t v = 0; v < per * levels; ++v) {
            if (isSet(req, v) && (pte[v] & kVsmPteMapped) && !(pte[v] & kVsmPteCached)) {
                renderList.push_back(v);
                pte[v] |= kVsmPteCached;
            }
        }
        st.toRender = static_cast<uint32_t>(renderList.size());
    }
};

bool sameStats(const VsmPageStats& a, const VsmPageStats& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

template <typename Pool>
bool sameAsOracle(const Pool& pool, const Oracle& o) {
    for (uint32_t v = 0; v < Pool::kVirtualPages; ++v) {
        const uint32_t expected = v < o.pte.size() ? o.pte[v] : 0u;
        if (pool.pte(v) != expected) {
            return false;
        }
    }
    for (uint32_t p = 0; p < o.phys; ++p) {
        if (pool.owner(p) != o.owner[p]) {
            return false;
        }
        if (o.owner[p] != kClInvalid && pool.last_used(p) != o.lastUsed[p]) {
            return false;
        }
    }
    if (pool.render_count() != o.renderList.size()) {
        return false;
    }
    for (uint32_t i = 0; i < pool.render_count(); ++i) {
        if (pool.render_page(i) != o.renderList[i] || pool.render_phys(i) != (pool.pte(o.renderList[i]) & kVsmPtePhysMask)) {
            return false;
        }
    }
    return true;
}

/// Random request pattern: a few blobs of pages around each level's window centre (what a depth
/// buffer produces), plus scattered singles.
void randomRequests(Rng& rng, uint32_t axis, uint32_t levels, uint32_t density, std::vector<uint32_t>& words) {
    const uint32_t per = axis * axis;
    std::fill(words.begin(), words.end(), 0u);
    const uint32_t blobs = 1u + rng.below(density + 1u);
    for (uint32_t b = 0; b < blobs; ++b) {
        const uint32_t l = rng.below(levels);
        const uint32_t cx = rng.below(axis), cy = rng.below(axis);
        const uint32_t r = rng.below(3u);
        for (uint32_t dy = 0; dy <= 2 * r; ++dy) {
            for (uint32_t dx = 0; dx <= 2 * r; ++dx) {
                const uint32_t x = (cx + dx) % axis, y = (cy + dy) % axis;
                const uint32_t v = l * per + y * axis + x;
                words[v / 32] |= 1u << (v % 32);
            }
        }
    }
}

template <uint32_t A, uint32_t L, uint32_t P>
void runModel(uint64_t seed, uint32_t frames, uint32_t levels, uint32_t phys, uint32_t density, bool props) {
    using Pool = VsmPagePool<A, L, P>;
    std::unique_ptr<Pool> pool(new Pool());
    VP_CHECK(pool->reset(levels, phys) == ClStatus::Ok);
    Oracle o;
    o.reset(A, levels, phys);
    Rng rng(seed);
    VsmFrameInput in{};
    in.levels = levels;
    std::vector<uint32_t> req(Pool::kWords, 0u), inv(Pool::kWords, 0u), prevReq;
    uint32_t frame = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        frame += 1u + (rng.chance(10) ? rng.below(20u) : 0u);
        in.frame = frame;
        in.invalidateAll = rng.chance(3) ? 1u : 0u;
        for (uint32_t l = 0; l < levels; ++l) {
            if (rng.chance(15)) {
                in.level[l].originX += static_cast<int32_t>(rng.below(5u)) - 2;
            }
            if (rng.chance(15)) {
                in.level[l].originY += static_cast<int32_t>(rng.below(5u)) - 2;
            }
            if (rng.chance(3)) {
                in.level[l].originX += static_cast<int32_t>(rng.below(4u * A)) - static_cast<int32_t>(2u * A); // teleport
            }
            if (rng.chance(4)) {
                in.level[l].depthKey += 1;
            }
        }
        const bool repeat = props && f > 0 && rng.chance(20);
        if (repeat) {
            // A static frame: same requests, same placement, no invalidation.
            in.invalidateAll = 0u;
            for (uint32_t l = 0; l < levels; ++l) {
                in.level[l] = pool->placement(l);
            }
            req = prevReq;
        } else {
            randomRequests(rng, A, levels, density, req);
        }
        const bool useInv = !repeat && rng.chance(40);
        std::fill(inv.begin(), inv.end(), 0u);
        if (useInv) {
            for (uint32_t k = 0; k < 1u + rng.below(8u); ++k) {
                const uint32_t v = rng.below(A * A * levels);
                inv[v / 32] |= 1u << (v % 32);
            }
        }
        // Snapshot for the properties.
        std::vector<uint32_t> before(Pool::kVirtualPages);
        for (uint32_t v = 0; v < Pool::kVirtualPages; ++v) {
            before[v] = pool->pte(v);
        }
        std::vector<uint32_t> lastBefore(phys), ownerBefore(phys);
        for (uint32_t p = 0; p < phys; ++p) {
            lastBefore[p] = pool->last_used(p);
            ownerBefore[p] = pool->owner(p);
        }
        VsmPageStats st{}, ost{};
        VP_CHECK(pool->update(in, req.data(), useInv ? inv.data() : nullptr, st) == ClStatus::Ok);
        o.update(in, req, useInv ? &inv : nullptr, ost);
        VP_CHECK(sameStats(st, ost));
        VP_CHECK(sameAsOracle(*pool, o));
        VP_CHECK(pool->check_invariants());
        VP_CHECK(pool->mapped_count() + pool->free_count() == phys);
        VP_CHECK(st.allocated + st.failed == st.needed && st.needed + st.alreadyMapped == st.requested);
        VP_CHECK(st.evicted <= st.allocated && st.allocated <= st.candidates);
        if (props) {
            auto isReq = [&](uint32_t v) { return ((req[v / 32] >> (v % 32)) & 1u) != 0u; };
            uint32_t minEvictedKey = kVsmFreeKey + 1u;
            for (uint32_t v = 0; v < A * A * levels; ++v) {
                const uint32_t b = before[v], a = pool->pte(v);
                if (isReq(v) && (b & kVsmPteMapped)) {
                    VP_CHECK((a & kVsmPteMapped) && (a & kVsmPtePhysMask) == (b & kVsmPtePhysMask)); // kept its page
                }
                if (isReq(v)) {
                    VP_CHECK((a & kVsmPteMapped) != 0u || st.failed > 0u);
                }
                if ((b & kVsmPteMapped) && !(a & kVsmPteMapped)) {
                    // Evicted this frame: it was a candidate (not used this frame).
                    const uint32_t p = b & kVsmPtePhysMask;
                    VP_CHECK(!isReq(v));
                    minEvictedKey = std::min(minEvictedKey, vsm_candidate_key(ownerBefore[p], lastBefore[p], frame));
                }
            }
            if (st.evicted > 0u) {
                VP_CHECK(pool->free_count() == 0u); // free pages are used before any eviction
            }
            if (st.failed > 0u) {
                VP_CHECK(st.candidates == st.allocated);
                for (uint32_t p = 0; p < phys; ++p) {
                    VP_CHECK(pool->owner(p) != kClInvalid && pool->last_used(p) == frame); // pool exhausted by this frame
                }
            }
            // LRU (bucketed): candidates left unused are no older than any evicted page.
            for (uint32_t p = 0; p < phys; ++p) {
                if (pool->owner(p) == ownerBefore[p] && ownerBefore[p] != kClInvalid && pool->last_used(p) != frame) {
                    VP_CHECK(vsm_candidate_key(ownerBefore[p], lastBefore[p], frame) <= minEvictedKey || minEvictedKey > kVsmFreeKey);
                }
            }
            // Render list: requested, mapped, not cached before the render step.
            for (uint32_t i = 0; i < pool->render_count(); ++i) {
                const uint32_t v = pool->render_page(i);
                VP_CHECK(isReq(v) && (pool->pte(v) & kVsmPteCached));
            }
            if (repeat && prevReq == req) {
                VP_CHECK(st.toRender == 0u && st.allocated == 0u && st.evicted == 0u);
            }
        }
        prevReq = req;
    }
}

void testModel() {
    const uint32_t s = iterScale();
    // Pools that fit, that are tight, and that overflow every frame.
    runModel<8, 3, 64>(1, 400 * s, 3, 64, 3, false);
    runModel<8, 3, 64>(2, 400 * s, 3, 24, 4, false);
    runModel<8, 3, 64>(3, 400 * s, 2, 6, 6, false);
    runModel<4, 4, 16>(4, 600 * s, 4, 16, 2, false);
    runModel<16, 2, 128>(5, 200 * s, 2, 100, 5, false);
}

void testProps() {
    const uint32_t s = iterScale();
    runModel<8, 3, 64>(11, 500 * s, 3, 40, 3, true);
    runModel<8, 3, 64>(12, 500 * s, 3, 12, 5, true);
    runModel<4, 2, 8>(13, 800 * s, 2, 5, 2, true);
    // Directed: static camera, static scene -> the second frame renders nothing; scrolling by one
    // page re-renders exactly the new column; an invalidation bit re-renders exactly that page.
    using Pool = VsmPagePool<8, 1, 64>;
    std::unique_ptr<Pool> pool(new Pool());
    VP_CHECK(pool->reset(1, 64) == ClStatus::Ok);
    VsmFrameInput in{};
    in.levels = 1;
    std::vector<uint32_t> req(Pool::kWords, 0xFFFFFFFFu); // whole window requested (64 pages)
    VsmPageStats st{};
    in.frame = 1;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok && st.toRender == 64u && st.allocated == 64u);
    in.frame = 2;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok && st.toRender == 0u && st.alreadyMapped == 64u);
    in.frame = 3;
    in.level[0].originX = 1; // window moves right by one page: one column (8 pages) scrolls in
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok && st.toRender == 8u && st.scrolled == 8u);
    for (uint32_t i = 0; i < pool->render_count(); ++i) {
        VP_CHECK(pool->render_page(i) % 8u == vsm_abs_to_slot(1 + 3, 8u)); // absolute column origin + axis/2 - 1 = 4
    }
    std::vector<uint32_t> inv(Pool::kWords, 0u);
    inv[0] = 1u << 9; // one page
    in.frame = 4;
    VP_CHECK(pool->update(in, req.data(), inv.data(), st) == ClStatus::Ok && st.toRender == 1u && st.invalidated == 1u &&
             pool->render_page(0) == 9u);
    in.frame = 5;
    in.level[0].depthKey = 7; // depth range moved: the whole level re-renders
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok && st.toRender == 64u && st.scrolled == 64u);
    in.frame = 6;
    in.invalidateAll = 1u;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok && st.toRender == 64u);
    VP_CHECK(pool->lookup_abs(0, 1, 0) != kClInvalid && pool->lookup_abs(0, 100, 0) == kClInvalid &&
             pool->lookup_abs(3, 0, 0) == kClInvalid);
}

void testBig() {
    std::unique_ptr<VsmPagePool16k> pool(new VsmPagePool16k());
    VP_CHECK(pool->reset(16, 4096) == ClStatus::Ok);
    Rng rng(99);
    VsmFrameInput in{};
    in.levels = 16;
    std::vector<uint32_t> req(VsmPagePool16k::kWords, 0u);
    uint32_t renders = 0;
    for (uint32_t f = 1; f <= 24; ++f) {
        in.frame = f;
        for (uint32_t l = 0; l < 16; ++l) {
            in.level[l].originX = static_cast<int32_t>(f >> (l / 4u));
            in.level[l].originY = -static_cast<int32_t>(l);
        }
        randomRequests(rng, 128u, 16u, 40u, req);
        // ~1/4 of each of the finest levels requested (a camera footprint), up to overflowing the pool.
        for (uint32_t l = 0; l < 3; ++l) {
            for (uint32_t y = 32; y < 96; ++y) {
                for (uint32_t x = 40 + f; x < 72 + f; ++x) {
                    const uint32_t v = l * 16384u + y * 128u + x;
                    req[v / 32] |= 1u << (v % 32);
                }
            }
        }
        VsmPageStats st{};
        VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok);
        VP_CHECK(pool->check_invariants());
        VP_CHECK(st.allocated + st.failed == st.needed);
        renders += st.toRender;
    }
    VP_CHECK(renders > 4096u);
    VP_CHECK(pool->mapped_count() == 4096u);
    std::printf("  big: 16 levels x 128^2 pages, 4096 physical, 24 frames, %u page renders, sizeof %zu KiB\n", renders,
                sizeof(VsmPagePool16k) / 1024u);
}

void testErrors() {
    using Pool = VsmPagePool<4, 2, 8>;
    std::unique_ptr<Pool> pool(new Pool());
    VP_CHECK(pool->reset(0, 8) == ClStatus::InvalidArgument);
    VP_CHECK(pool->reset(3, 8) == ClStatus::InvalidArgument);
    VP_CHECK(pool->reset(2, 0) == ClStatus::InvalidArgument);
    VP_CHECK(pool->reset(2, 9) == ClStatus::InvalidArgument);
    std::vector<uint32_t> req(Pool::kWords, 0u);
    VsmFrameInput in{};
    in.levels = 2;
    in.frame = 1;
    VsmPageStats st{};
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::InvalidArgument); // reset failed
    VP_CHECK(pool->reset(2, 8) == ClStatus::Ok);
    VP_CHECK(pool->update(in, nullptr, nullptr, st) == ClStatus::InvalidArgument);
    in.levels = 1;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::InvalidArgument);
    in.levels = 2;
    in.level[1].originY = kVsmMaxOrigin + 1;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::InvalidArgument);
    in.level[1].originY = 0;
    in.level[0].originX = -kVsmMaxOrigin - 1;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::InvalidArgument);
    in.level[0].originX = 0;
    req[0] = 0xFFu;
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::Ok && st.allocated == 8u);
    VP_CHECK(pool->update(in, req.data(), nullptr, st) == ClStatus::InvalidArgument); // same frame
    VP_CHECK(pool->frame() == 1u && pool->levels() == 2u && pool->phys_pages() == 8u && pool->active_pages() == 32u);
    VP_CHECK(pool->pte(1000u) == 0u && pool->owner(100u) == kClInvalid && pool->last_used(100u) == 0u);
    VP_CHECK(pool->render_page(100u) == kClInvalid && pool->render_phys(100u) == kClInvalid);
    VP_CHECK(pool->candidate_count() == 8u && pool->candidate(0) == 0u && pool->candidate(100u) == kClInvalid);
    VP_CHECK(pool->placed() && pool->placement(99u).originX == 0);
    VP_CHECK(pool->check_invariants());
    // Negative paths of check_invariants(), each on a fresh copy.
    auto corrupt = [&](int which) {
        std::unique_ptr<Pool> c(new Pool(*pool));
        switch (which) {
        case 0: ClTestAccess::pte(*c, 3u) = kVsmPteCached; break;                 // cached without mapping
        case 1: ClTestAccess::pte(*c, 3u) |= 0x01000000u; break;                  // stray bits
        case 2: ClTestAccess::pte(*c, 3u) = kVsmPteMapped | 5u; break;             // two virtual pages on page 5
        case 3: ClTestAccess::owner(*c, 2u) = 20u; break;                           // owner / PTE mismatch
        case 4: ClTestAccess::owner(*c, 2u) = 1000u; break;                         // owner out of range
        case 5: ClTestAccess::last_used(*c, 2u) = 99u; break;                      // used in the future
        case 6: ClTestAccess::phys(*c) = 100u; break;                              // more pages than capacity
        case 7: ClTestAccess::pte(*c, 20u) = kVsmPteMapped | 7u; ClTestAccess::owner(*c, 7u) = 20u;
                ClTestAccess::pte(*c, 7u) = 0u; break;                              // consistent remap: still valid
        default: ClTestAccess::pte(*c, 1u) = kVsmPteMapped | 6u; break;             // entry points at another's page
        }
        return c->check_invariants();
    };
    for (int k = 0; k < 9; ++k) {
        VP_CHECK(corrupt(k) == (k == 7));
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
    } suites[] = {{"slots", testSlots}, {"model", testModel}, {"props", testProps}, {"big", testBig}, {"errors", testErrors}};
    for (const Suite& s : suites) {
        if (all || std::strcmp(which, s.name) == 0) {
            ran = true;
            s.fn();
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite '%s' (slots|model|props|big|errors|all)\n", which);
        return 2;
    }
    std::printf("core_logic vsm_pages[%s]: %s (%d failed checks)\n", which, g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
