// CBMC harness (WP-0.8): render-graph compiler model on every graph with up to
// FUSE_CL_RG_MAX_PASSES passes, FUSE_CL_RG_MAX_RESOURCES resources and
// FUSE_CL_RG_MAX_ACCESSES_PER_PASS accesses per pass (nondeterministic queues, queue config,
// imported/transient resources, sizes, alignments and states). Property: the brute-force
// oracle finds no uncovered hazard, no unbacked cross-queue edge, no aliasing of unordered
// transients and no structural error.
// The capacities must be identical in every TU (the harness and src/rg_model.cpp), so they
// come from the command line:
//   cbmc --cpp11 -I../include -I../tests -DFUSE_CL_RG_MAX_PASSES=3u -DFUSE_CL_RG_MAX_RESOURCES=2u
//        -DFUSE_CL_RG_MAX_ACCESSES_PER_PASS=1u bmc_rg_compiler.cpp ../src/rg_model.cpp
//        --unwind 8 --unwinding-assertions --bounds-check --pointer-check
// (VERIFICATION SUCCESSFUL in ~7.5 min with CBMC 5.95.1; a 2-access shape did not finish in 50 min.)
#if !defined(FUSE_CL_RG_MAX_PASSES) || !defined(FUSE_CL_RG_MAX_RESOURCES) || !defined(FUSE_CL_RG_MAX_ACCESSES_PER_PASS)
#error "define the FUSE_CL_RG_MAX_* capacities for this harness and for rg_model.cpp"
#endif
#include "bmc_shim.hpp"
#include "rg_oracle.hpp"

using namespace fuse::core_logic;

#ifndef BMC_SHAPE_ACCESSES
#define BMC_SHAPE_ACCESSES FUSE_CL_RG_MAX_ACCESSES_PER_PASS
#endif

static RgGraphDesc g_graph;
static RgCompileResult g_out;

static int harness() {
    g_graph.queues.hasAsyncCompute = static_cast<uint8_t>(nondet_u32() & 1u);
    g_graph.queues.hasTransfer = static_cast<uint8_t>(nondet_u32() & 1u);
    // Loop bounds stay concrete (full capacity; BMC_SHAPE_ACCESSES accesses per pass): symbolic
    // counts make CBMC's symbolic execution explode. Contents (queues, queue config, imported,
    // sizes, alignments, initial states, which resource each access touches and in which
    // state) are nondeterministic, so every graph of that shape is covered.
    g_graph.resourceCount = kRgMaxResources;
    for (uint32_t r = 0u; r < g_graph.resourceCount; ++r) {
        RgResourceDesc& rd = g_graph.resources[r];
        rd.imported = static_cast<uint8_t>(nondet_u32() & 1u);
        rd.sizeBytes = 1u + nondet_u32() % 64u;
        rd.alignment = 1ull << (nondet_u32() % 4u);
        rd.initialState = static_cast<RgState>(nondet_u32() % static_cast<uint32_t>(RgState::Count));
    }
    g_graph.passCount = kRgMaxPasses;
    for (uint32_t p = 0u; p < g_graph.passCount; ++p) {
        RgPassDesc& pd = g_graph.passes[p];
        pd.queue = static_cast<RgQueue>(nondet_u32() % kRgQueueCount);
        pd.accessCount = BMC_SHAPE_ACCESSES;
        for (uint32_t a = 0u; a < pd.accessCount; ++a) {
            pd.accesses[a].resource = nondet_u32() % g_graph.resourceCount;
            pd.accesses[a].state = static_cast<RgState>(1u + nondet_u32() % (static_cast<uint32_t>(RgState::Count) - 1u));
            for (uint32_t b = 0u; b < a; ++b) {
                BMC_ASSUME(pd.accesses[b].resource != pd.accesses[a].resource);
            }
        }
    }
    const ClStatus s = rg_compile(g_graph, g_out);
    BMC_ASSERT(s == ClStatus::Ok, "valid graphs compile");
    RgOracleReport rep;
    rg_oracle_check(g_graph, g_out, rep);
    BMC_ASSERT(rep.uncovered == 0u, "every hazard is ordered by a barrier chain");
    BMC_ASSERT(rep.unbackedCrossQueue == 0u, "cross-queue barriers have a timeline wait");
    BMC_ASSERT(rep.aliasViolations == 0u, "aliased transients are totally ordered");
    BMC_ASSERT(rep.badBarrier == 0u && rep.badSync == 0u && rep.hbMismatch == 0u && rep.badPlan == 0u,
               "structural properties");
    return 0;
}

BMC_MAIN(harness)
