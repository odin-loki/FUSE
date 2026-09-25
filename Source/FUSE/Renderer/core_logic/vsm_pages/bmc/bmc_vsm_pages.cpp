// CBMC harness (WP-3.1): VSM page pool (vsm_pages.hpp) under BMC_STEPS nondeterministic frames:
// nondeterministic request and invalidation masks, window moves of -1..1 page per axis, depth-key
// changes, invalidate-all and frame gaps.
// Properties after every frame: check_invariants() (no double map, conservation, no cached bit
// without a mapping, lastUsed <= frame); requested pages that were mapped keep their physical page;
// every requested page is mapped unless the frame failed needs, and a failed need implies every
// physical page was used this frame; free pages are taken before any eviction; the render list holds
// only requested, mapped, now-cached pages; statistics add up.
//   cbmc --cpp11 -I../../include -I. -I../../bmc bmc_vsm_pages.cpp --unwind 20 --unwinding-assertions
//        --bounds-check --pointer-check --signed-overflow-check
#include "bmc_shim.hpp"
#include "fuse/core_logic/vsm_pages.hpp"

using namespace fuse::core_logic;

#ifndef BMC_STEPS
#define BMC_STEPS 3
#endif

// 2 x 2 pages per level, 2 levels (8 virtual pages, one request word), 3 physical pages.
typedef VsmPagePool<2u, 2u, 3u> Pool;
static const uint32_t kV = 8u;
static const uint32_t kP = 3u;
static Pool g_pool;

static int harness() {
    BMC_ASSERT(g_pool.reset(2u, kP) == ClStatus::Ok, "reset");
    VsmFrameInput in;
    in.levels = 2u;
    in.invalidateAll = 0u;
    in.pad = 0u;
    for (uint32_t l = 0u; l < kVsmMaxLevels; ++l) {
        in.level[l].originX = 0;
        in.level[l].originY = 0;
        in.level[l].depthKey = 0;
        in.level[l].pad = 0;
    }
    uint32_t frame = 0u;
    for (uint32_t step = 0u; step < BMC_STEPS; ++step) {
        frame += 1u + nondet_u32() % 3u;
        in.frame = frame;
        in.invalidateAll = nondet_u32() % 4u == 0u ? 1u : 0u;
        for (uint32_t l = 0u; l < 2u; ++l) {
            in.level[l].originX += static_cast<int32_t>(nondet_u32() % 3u) - 1;
            in.level[l].originY += static_cast<int32_t>(nondet_u32() % 3u) - 1;
            in.level[l].depthKey += nondet_u32() % 4u == 0u ? 1 : 0;
        }
        uint32_t req[1];
        uint32_t inv[1];
        req[0] = nondet_u32() & 0xFFu;
        inv[0] = nondet_u32() & 0xFFu;
        const bool useInv = nondet_u32() % 2u == 0u;
        uint32_t before[kV];
        for (uint32_t v = 0u; v < kV; ++v) {
            before[v] = g_pool.pte(v);
        }
        VsmPageStats st;
        const uint32_t* invPtr = 0;
        if (useInv) {
            invPtr = inv;
        }
        BMC_ASSERT(g_pool.update(in, req, invPtr, st) == ClStatus::Ok, "update accepts a valid frame");
        BMC_ASSERT(g_pool.check_invariants(), "page-pool invariants");
        BMC_ASSERT(g_pool.mapped_count() + g_pool.free_count() == kP, "conservation");
        BMC_ASSERT(st.allocated + st.failed == st.needed && st.needed + st.alreadyMapped == st.requested,
                   "statistics add up");
        BMC_ASSERT(st.evicted <= st.allocated && st.allocated <= st.candidates && st.toRender <= kP, "counts bounded");
        BMC_ASSERT(st.evicted == 0u || g_pool.free_count() == 0u, "free pages before evictions");
        for (uint32_t v = 0u; v < kV; ++v) {
            const bool requested = ((req[0] >> v) & 1u) != 0u;
            const uint32_t now = g_pool.pte(v);
            if (requested && (before[v] & kVsmPteMapped) != 0u) {
                BMC_ASSERT((now & kVsmPteMapped) != 0u && (now & kVsmPtePhysMask) == (before[v] & kVsmPtePhysMask),
                           "a requested mapped page keeps its physical page");
            }
            if (requested) {
                BMC_ASSERT((now & kVsmPteMapped) != 0u || st.failed > 0u, "requested pages are mapped unless the pool ran out");
                BMC_ASSERT((now & kVsmPteMapped) == 0u || (now & kVsmPteCached) != 0u, "requested mapped pages end cached");
            }
            for (uint32_t w = v + 1u; w < kV; ++w) {
                const uint32_t other = g_pool.pte(w);
                BMC_ASSERT((now & kVsmPteMapped) == 0u || (other & kVsmPteMapped) == 0u ||
                               (now & kVsmPtePhysMask) != (other & kVsmPtePhysMask),
                           "no physical page mapped twice");
            }
        }
        if (st.failed > 0u) {
            for (uint32_t p = 0u; p < kP; ++p) {
                BMC_ASSERT(g_pool.owner(p) != kClInvalid && g_pool.last_used(p) == frame, "failure only when the pool is exhausted");
            }
        }
        for (uint32_t i = 0u; i < g_pool.render_count(); ++i) {
            const uint32_t v = g_pool.render_page(i);
            BMC_ASSERT(v < kV && ((req[0] >> v) & 1u) != 0u && (g_pool.pte(v) & kVsmPteCached) != 0u &&
                           (g_pool.pte(v) & kVsmPtePhysMask) == g_pool.render_phys(i),
                       "render list holds requested, mapped pages");
        }
    }
    return 0;
}

BMC_MAIN(harness)
