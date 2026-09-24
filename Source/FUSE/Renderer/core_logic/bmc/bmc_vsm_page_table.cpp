// CBMC harness (WP-0.8): VSM page table + physical pool under a nondeterministic sequence
// of BMC_STEPS operations with nondeterministic (also out-of-range) arguments.
// Properties: check_invariants() (free-list conservation, no double map, LRU consistency)
// after every step, plus: requested mapped pages never move during update(), and no
// physical page backs two virtual pages (checked independently of check_invariants()).
//   cbmc --cpp11 -I../include bmc_vsm_page_table.cpp --unwind 10 --unwinding-assertions
//        --bounds-check --pointer-check --signed-overflow-check
#include "bmc_shim.hpp"
#include "fuse/core_logic/vsm_page_table.hpp"

using namespace fuse::core_logic;

#ifndef BMC_STEPS
#define BMC_STEPS 4
#endif

// (CBMC resolves neither Vsm:: scopes through the typedef nor member constants here, so the
// sizes are repeated as plain constants.)
typedef VsmPageTable<2u, 2u, 3u> Vsm; // 8 virtual pages, 3 physical pages
static const uint32_t kV = 8u;
static Vsm g_table;

static int harness() {
    g_table.reset();
    uint32_t frame = 0u;
    for (uint32_t step = 0u; step < BMC_STEPS; ++step) {
        const uint32_t op = nondet_u32() % 6u;
        const uint32_t a = nondet_u32() % 3u; // level (2 == invalid)
        const uint32_t x = nondet_u32() % 3u; // 2 == invalid
        const uint32_t y = nondet_u32() % 3u;
        if (op == 0u) {
            frame += 1u;
            BMC_ASSERT(g_table.begin_frame(frame) == ClStatus::Ok, "begin_frame accepts increasing frames");
        } else if (op == 1u) {
            const ClStatus s = g_table.mark(a, x, y);
            BMC_ASSERT((s == ClStatus::Ok) == (a < 2u && x < 2u && y < 2u), "mark validates");
        } else if (op == 2u) {
            uint32_t before[kV];
            for (uint32_t v = 0u; v < kV; ++v) {
                before[v] = g_table.lookup(v / 4u, v % 2u, (v / 2u) % 2u);
            }
            const VsmUpdateStats st = g_table.update();
            BMC_ASSERT(st.allocated <= 3u && st.evicted <= st.allocated, "update counts bounded");
            for (uint32_t v = 0u; v < kV; ++v) {
                const uint32_t now = g_table.lookup(v / 4u, v % 2u, (v / 2u) % 2u);
                BMC_ASSERT(before[v] == kClInvalid || now == kClInvalid || now == before[v],
                           "a mapped page keeps its physical page");
            }
        } else if (op == 3u) {
            const ClStatus s = g_table.unmap(a, x, y);
            BMC_ASSERT(s != ClStatus::Ok || g_table.lookup(a, x, y) == kClInvalid, "unmap unmaps");
        } else if (op == 4u) {
            g_table.evict_older_than(nondet_u32() % 3u);
        } else {
            g_table.invalidate_rect(a, x, y, x + nondet_u32() % 2u, y + nondet_u32() % 2u);
        }
        BMC_ASSERT(g_table.check_invariants(), "page-table invariants");
        BMC_ASSERT(g_table.free_count() + g_table.mapped_count() == 3u, "free-list conservation");
        for (uint32_t v1 = 0u; v1 < kV; ++v1) {
            const uint32_t p1 = g_table.lookup(v1 / 4u, v1 % 2u, (v1 / 2u) % 2u);
            for (uint32_t v2 = v1 + 1u; v2 < kV; ++v2) {
                const uint32_t p2 = g_table.lookup(v2 / 4u, v2 % 2u, (v2 / 2u) % 2u);
                BMC_ASSERT(p1 == kClInvalid || p1 != p2, "no physical page mapped twice");
            }
        }
    }
    return 0;
}

BMC_MAIN(harness)
