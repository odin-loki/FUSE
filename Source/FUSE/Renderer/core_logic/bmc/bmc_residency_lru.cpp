// CBMC harness (WP-0.8): cluster residency with a nondeterministic 4-page forest, budget and
// operation sequence. Properties after every step: check_invariants() (byte accounting,
// no orphans, LRU == resident set), budget never exceeded, coarse pages never evicted, and
// evictions only ever remove resident pages.
//   cbmc --cpp11 -I../include bmc_residency_lru.cpp --unwind 7 --unwinding-assertions
//        --bounds-check --pointer-check
#include "bmc_shim.hpp"
#include "fuse/core_logic/residency_lru.hpp"

using namespace fuse::core_logic;

#ifndef BMC_STEPS
#define BMC_STEPS 4
#endif

typedef ClusterResidency<4u, 4u> Res;
static Res g_res;

static int harness() {
    const uint64_t budget = 1u + nondet_u32() % 16u;
    g_res.reset(budget);
    uint8_t coarse[4];
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t pr = nondet_u32() % (i + 1u); // == i means "no parent"
        const uint32_t parent = pr == i ? kClInvalid : pr;
        const bool c = (nondet_u32() & 1u) != 0u;
        const ClStatus s = g_res.register_page(i, 1u + nondet_u32() % 8u, parent, c);
        BMC_ASSUME(s == ClStatus::Ok);
        coarse[i] = c ? 1u : 0u;
    }
    uint32_t frame = 0u;
    for (uint32_t step = 0u; step < BMC_STEPS; ++step) {
        const uint32_t op = nondet_u32() % 6u;
        const uint32_t id = nondet_u32() % 4u;
        ResidencyState before[4];
        for (uint32_t i = 0u; i < 4u; ++i) {
            before[i] = g_res.state(i);
        }
        if (op == 0u) {
            frame += 1u;
            BMC_ASSERT(g_res.begin_frame(frame) == ClStatus::Ok, "begin_frame");
        } else if (op == 1u) {
            BMC_ASSERT(g_res.request(id, nondet_u32() % 4u) == ClStatus::Ok, "request fits (4 slots, 4 pages)");
        } else if (op == 2u) {
            g_res.update(nondet_u32() % 5u);
        } else if (op == 3u) {
            const ClStatus s = g_res.complete_load(id);
            BMC_ASSERT((s == ClStatus::Ok) == (before[id] == ResidencyState::Loading), "complete_load");
        } else if (op == 4u) {
            const ClStatus s = g_res.fail_load(id);
            BMC_ASSERT((s == ClStatus::Ok) == (before[id] == ResidencyState::Loading), "fail_load");
        } else {
            const uint64_t nb = nondet_u32() % 20u;
            const ClStatus s = g_res.set_budget(nb);
            BMC_ASSERT(s != ClStatus::Ok || g_res.budget() == nb, "set_budget applies");
            BMC_ASSERT(nb >= g_res.coarse_bytes() || s == ClStatus::OutOfBudget, "coarse LOD always fits");
        }
        if (op == 2u || op == 5u) {
            for (uint32_t i = 0u; i < g_res.evict_count(); ++i) {
                const uint32_t e = g_res.evict_id(i);
                BMC_ASSERT(e < 4u && coarse[e] == 0u, "coarse pages are never evicted");
                BMC_ASSERT(before[e] == ResidencyState::Resident, "only resident pages are evicted");
            }
        }
        BMC_ASSERT(g_res.check_invariants(), "residency invariants");
        BMC_ASSERT(g_res.resident_bytes() + g_res.loading_bytes() <= g_res.budget(), "budget never exceeded");
    }
    return 0;
}

BMC_MAIN(harness)
