// CBMC harness (WP-5.3): cluster-page residency (cluster_page_residency.hpp) over a nondeterministic
// 4-page DAG (each page depends on any subset of the earlier pages; a nondeterministic closed coarse
// set; sizes 1..8), budget and BMC_STEPS nondeterministic operations (begin_frame, request with a
// nondeterministic priority, update with 0..4 loads, complete / fail a load, set_budget).
// Properties after every step: check_invariants() (byte accounting, dependency closure of Loading /
// Resident pages, dependent counts, LRU == resident set, coarse closure); the budget is never
// exceeded; coarse pages are never evicted; evictions only remove resident pages without an active
// dependent; requested pages are only preempted by a more urgent (or a coarse) candidate; loads are
// only issued for pages whose dependencies are all resident; a request marks the whole dependency
// closure with at least its priority.
//   cbmc --cpp11 -I../include -I../../include -I../../bmc bmc_page_residency.cpp --unwind 9
//        --unwinding-assertions --bounds-check --pointer-check
#include "bmc_shim.hpp"
#include "fuse/core_logic/cluster_page_residency.hpp"

using namespace fuse::core_logic;

#ifndef BMC_STEPS
#define BMC_STEPS 4
#endif

typedef ClusterPageResidency<4u, 6u, 4u> Res;
static Res g_res;

static int harness() {
    const uint64_t budget = 1u + nondet_u32() % 16u;
    g_res.reset(budget);
    uint8_t coarse[4];
    uint32_t depMask[4];
    for (uint32_t i = 0u; i < 4u; ++i) {
        uint32_t deps[3];
        uint32_t n = 0u;
        const uint32_t mask = nondet_u32() & ((1u << i) - 1u);
        const bool c = (nondet_u32() & 1u) != 0u;
        for (uint32_t d = 0u; d < i; ++d) {
            if ((mask & (1u << d)) != 0u) {
                deps[n++] = d;
            }
        }
        const ClStatus s = g_res.register_page(i, 1u + nondet_u32() % 8u, deps, n, c);
        BMC_ASSUME(s == ClStatus::Ok);
        coarse[i] = c ? 1u : 0u;
        depMask[i] = mask;
    }
    uint32_t frame = 0u;
    for (uint32_t step = 0u; step < BMC_STEPS; ++step) {
        const uint32_t op = nondet_u32() % 6u;
        const uint32_t id = nondet_u32() % 4u;
        PageState before[4];
        for (uint32_t i = 0u; i < 4u; ++i) {
            before[i] = g_res.state(i);
        }
        if (op == 0u) {
            frame += 1u;
            BMC_ASSERT(g_res.begin_frame(frame) == ClStatus::Ok, "begin_frame");
        } else if (op == 1u) {
            const uint32_t prio = nondet_u32() % 4u;
            BMC_ASSERT(g_res.request(id, prio) == ClStatus::Ok, "request fits (4 slots, 4 pages)");
            for (uint32_t d = 0u; d < 4u; ++d) {
                if ((depMask[id] & (1u << d)) != 0u) {
                    BMC_ASSERT(g_res.requested(d) && g_res.priority(d) >= prio, "request marks the dependency closure");
                }
            }
        } else if (op == 2u) {
            uint32_t maxCandPrio = 0u;
            bool coarseCand = false;
            for (uint32_t i = 0u; i < 4u; ++i) {
                if (before[i] == PageState::NotResident && g_res.requested(i) && g_res.priority(i) > maxCandPrio) {
                    maxCandPrio = g_res.priority(i);
                }
                coarseCand = coarseCand || (coarse[i] != 0u && before[i] == PageState::NotResident);
            }
            uint32_t prioBefore[4];
            bool reqBefore[4];
            for (uint32_t i = 0u; i < 4u; ++i) {
                prioBefore[i] = g_res.priority(i);
                reqBefore[i] = g_res.requested(i);
            }
            g_res.update(nondet_u32() % 5u);
            for (uint32_t i = 0u; i < g_res.evict_count(); ++i) {
                const uint32_t e = g_res.evict_id(i);
                BMC_ASSERT(e < 4u && (coarseCand || !reqBefore[e] || prioBefore[e] < maxCandPrio),
                           "requested pages are only preempted by a more urgent (or coarse) candidate");
            }
            for (uint32_t i = 0u; i < g_res.load_count(); ++i) {
                const uint32_t l = g_res.load_id(i);
                BMC_ASSERT(l < 4u && before[l] == PageState::NotResident, "only non-resident pages are loaded");
                for (uint32_t d = 0u; d < 4u; ++d) {
                    if ((depMask[l] & (1u << d)) != 0u) {
                        BMC_ASSERT(g_res.state(d) == PageState::Resident, "loads only on a resident closure");
                    }
                }
            }
        } else if (op == 3u) {
            const ClStatus s = g_res.complete_load(id);
            BMC_ASSERT((s == ClStatus::Ok) == (before[id] == PageState::Loading), "complete_load");
        } else if (op == 4u) {
            const ClStatus s = g_res.fail_load(id);
            BMC_ASSERT((s == ClStatus::Ok) == (before[id] == PageState::Loading), "fail_load");
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
                BMC_ASSERT(before[e] == PageState::Resident, "only resident pages are evicted");
                BMC_ASSERT(g_res.dependents(e) == 0u, "no page with an active dependent is evicted");
            }
        }
        BMC_ASSERT(g_res.check_invariants(), "page residency invariants");
        BMC_ASSERT(g_res.resident_bytes() + g_res.loading_bytes() <= g_res.budget(), "budget never exceeded");
    }
    return 0;
}

BMC_MAIN(harness)
