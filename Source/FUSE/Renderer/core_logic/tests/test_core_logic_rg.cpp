// WP-0.8: render-graph compiler model — unit, scenario and property tests against the
// brute-force oracle (rg_oracle.hpp).
#include "cl_test_util.hpp"
#include "rg_oracle.hpp"

#include <cstring>
#include <memory>

using namespace fuse::core_logic;

namespace {

struct Fixture {
    std::unique_ptr<RgGraphDesc> g{new RgGraphDesc()};
    std::unique_ptr<RgCompileResult> out{new RgCompileResult()};
    Fixture() { std::memset(g.get(), 0, sizeof(RgGraphDesc)); }

    uint32_t transient(uint64_t size, uint64_t align = 256u) {
        const uint32_t r = g->resourceCount++;
        g->resources[r].sizeBytes = size;
        g->resources[r].alignment = align;
        g->resources[r].initialState = RgState::Undefined;
        g->resources[r].imported = 0u;
        return r;
    }
    uint32_t imported(RgState initial) {
        const uint32_t r = g->resourceCount++;
        g->resources[r].sizeBytes = 0u;
        g->resources[r].alignment = 0u;
        g->resources[r].initialState = initial;
        g->resources[r].imported = 1u;
        return r;
    }
    uint32_t pass(RgQueue q) {
        const uint32_t p = g->passCount++;
        g->passes[p].queue = q;
        g->passes[p].accessCount = 0u;
        return p;
    }
    void access(uint32_t p, uint32_t r, RgState s) {
        RgPassDesc& pd = g->passes[p];
        pd.accesses[pd.accessCount].resource = r;
        pd.accesses[pd.accessCount].state = s;
        ++pd.accessCount;
    }
    ClStatus compile() { return rg_compile(*g, *out); }
    void check_oracle() {
        RgOracleReport rep;
        rg_oracle_check(*g, *out, rep);
        CL_CHECK(rg_oracle_total(rep) == 0u);
        if (rg_oracle_total(rep) != 0u) {
            std::fprintf(stderr,
                         "oracle: hazards=%u uncovered=%u unbacked=%u badBarrier=%u badSync=%u hb=%u alias=%u plan=%u\n",
                         rep.hazards, rep.uncovered, rep.unbackedCrossQueue, rep.badBarrier, rep.badSync, rep.hbMismatch,
                         rep.aliasViolations, rep.badPlan);
        }
    }
};

void test_validation() {
    {
        Fixture f;
        f.g->passCount = kRgMaxPasses + 1u;
        CL_CHECK(f.compile() == ClStatus::CapacityExceeded);
    }
    {
        Fixture f;
        f.g->resourceCount = kRgMaxResources + 1u;
        CL_CHECK(f.compile() == ClStatus::CapacityExceeded);
    }
    {
        Fixture f;
        const uint32_t r = f.imported(RgState::Count);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument && f.out->errorResource == r);
    }
    {
        Fixture f;
        f.transient(0u);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument);
    }
    {
        Fixture f;
        f.transient(64u, 3u);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument);
    }
    {
        Fixture f;
        f.transient(64u, 0u);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument);
    }
    {
        Fixture f;
        f.pass(static_cast<RgQueue>(3));
        CL_CHECK(f.compile() == ClStatus::InvalidArgument && f.out->errorPass == 0u);
    }
    {
        Fixture f;
        const uint32_t p = f.pass(RgQueue::Graphics);
        f.g->passes[p].accessCount = kRgMaxAccessesPerPass + 1u;
        CL_CHECK(f.compile() == ClStatus::CapacityExceeded);
    }
    {
        Fixture f;
        f.transient(64u);
        const uint32_t p = f.pass(RgQueue::Graphics);
        f.access(p, 5u, RgState::ShaderRead);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument && f.out->errorResource == 5u);
    }
    {
        Fixture f;
        const uint32_t r = f.transient(64u);
        const uint32_t p = f.pass(RgQueue::Graphics);
        f.access(p, r, RgState::Undefined);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument);
    }
    {
        Fixture f;
        const uint32_t r = f.transient(64u);
        const uint32_t p = f.pass(RgQueue::Graphics);
        f.access(p, r, RgState::Count);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument);
    }
    {
        Fixture f;
        const uint32_t r = f.transient(64u);
        const uint32_t r2 = f.transient(64u);
        const uint32_t p = f.pass(RgQueue::Graphics);
        f.access(p, r2, RgState::ShaderRead);
        f.access(p, r, RgState::ShaderRead);
        f.access(p, r, RgState::ShaderWrite);
        CL_CHECK(f.compile() == ClStatus::InvalidArgument && f.out->errorResource == r);
    }
    {
        // Empty graph compiles.
        Fixture f;
        CL_CHECK(f.compile() == ClStatus::Ok && f.out->barrierCount == 0u && f.out->transientHeapBytes == 0u);
    }
}

void test_queue_fallback() {
    const RgQueue req[3] = {RgQueue::Graphics, RgQueue::AsyncCompute, RgQueue::Transfer};
    for (uint32_t cfg = 0u; cfg < 4u; ++cfg) {
        Fixture f;
        f.g->queues.hasAsyncCompute = static_cast<uint8_t>(cfg & 1u);
        f.g->queues.hasTransfer = static_cast<uint8_t>((cfg >> 1) & 1u);
        for (uint32_t i = 0u; i < 3u; ++i) {
            f.pass(req[i]);
        }
        CL_CHECK(f.compile() == ClStatus::Ok);
        CL_CHECK(f.out->passQueue[0] == RgQueue::Graphics);
        CL_CHECK(f.out->passQueue[1] == ((cfg & 1u) ? RgQueue::AsyncCompute : RgQueue::Graphics));
        const RgQueue t = (cfg & 2u) ? RgQueue::Transfer : ((cfg & 1u) ? RgQueue::AsyncCompute : RgQueue::Graphics);
        CL_CHECK(f.out->passQueue[2] == t);
    }
}

void test_scenarios() {
    // Single queue: write -> read -> read(same) -> read(other state) -> write -> write.
    {
        Fixture f;
        const uint32_t r = f.transient(1024u);
        uint32_t p[6];
        for (uint32_t i = 0u; i < 6u; ++i) {
            p[i] = f.pass(RgQueue::Graphics);
        }
        f.access(p[0], r, RgState::ColorAttachment);
        f.access(p[1], r, RgState::ShaderRead);
        f.access(p[2], r, RgState::ShaderRead);
        f.access(p[3], r, RgState::TransferSrc);
        f.access(p[4], r, RgState::ShaderWrite);
        f.access(p[5], r, RgState::ShaderWrite);
        CL_CHECK(f.compile() == ClStatus::Ok);
        CL_CHECK(f.out->barrierCount == 5u);
        CL_CHECK(f.out->barriers[0].kind == RgBarrierKind::Initial && f.out->barriers[0].beforePass == 0u);
        CL_CHECK(f.out->barriers[1].kind == RgBarrierKind::ReadAfterWrite && f.out->barriers[1].srcPassMask == 1u);
        CL_CHECK(f.out->barriers[2].kind == RgBarrierKind::LayoutTransition && f.out->barriers[2].srcPassMask == 0x6u);
        CL_CHECK(f.out->barriers[3].kind == RgBarrierKind::WriteAfterRead && f.out->barriers[3].srcPassMask == 0x8u);
        CL_CHECK(f.out->barriers[4].kind == RgBarrierKind::WriteAfterWrite && f.out->barriers[4].srcPassMask == 0x10u);
        CL_CHECK(f.out->passBarrierCount[2] == 0u && f.out->syncPointCount == 0u);
        f.check_oracle();
    }
    // Imported: first read in the initial state needs nothing; first read elsewhere transitions;
    // first write in the initial state needs nothing; read-after-transition in another state.
    {
        Fixture f;
        const uint32_t a = f.imported(RgState::ShaderRead);
        const uint32_t b = f.imported(RgState::Present);
        const uint32_t c = f.imported(RgState::ShaderWrite);
        const uint32_t p0 = f.pass(RgQueue::Graphics);
        const uint32_t p1 = f.pass(RgQueue::Graphics);
        f.access(p0, a, RgState::ShaderRead);
        f.access(p0, b, RgState::ColorAttachment);
        f.access(p0, c, RgState::ShaderWrite);
        f.access(p1, a, RgState::ShaderRead);
        f.access(p1, b, RgState::Present);
        CL_CHECK(f.compile() == ClStatus::Ok);
        CL_CHECK(f.out->passBarrierCount[0] == 1u && f.out->barriers[0].resource == b);
        CL_CHECK(f.out->passBarrierCount[1] == 1u && f.out->barriers[1].kind == RgBarrierKind::ReadAfterWrite);
        CL_CHECK(f.out->resources[a].heapOffset == kRgNoOffset && f.out->transientHeapBytes == 0u);
        f.check_oracle();
    }
    // Cross-queue: graphics writes, async compute reads twice (one sync), then graphics reads,
    // then graphics writes after an async read (WAR across queues).
    {
        Fixture f;
        f.g->queues.hasAsyncCompute = 1u;
        const uint32_t r = f.transient(4096u);
        const uint32_t s = f.transient(4096u);
        const uint32_t p0 = f.pass(RgQueue::Graphics);
        const uint32_t p1 = f.pass(RgQueue::AsyncCompute);
        const uint32_t p2 = f.pass(RgQueue::AsyncCompute);
        const uint32_t p3 = f.pass(RgQueue::Graphics);
        const uint32_t p4 = f.pass(RgQueue::Graphics);
        f.access(p0, r, RgState::ShaderWrite);
        f.access(p0, s, RgState::ShaderWrite);
        f.access(p1, r, RgState::ShaderRead);
        f.access(p2, s, RgState::ShaderRead);
        f.access(p3, r, RgState::ShaderRead);
        f.access(p4, r, RgState::ShaderWrite);
        CL_CHECK(f.compile() == ClStatus::Ok);
        CL_CHECK(f.out->syncPointCount == 2u);
        CL_CHECK(f.out->syncPoints[0].srcQueue == RgQueue::Graphics && f.out->syncPoints[0].waitBeforePass == p1);
        // p1's read transitioned r on the compute queue, so graphics p3 waits for p1 (the
        // transition), and p4's WAR against p1 is already covered by that wait.
        CL_CHECK(f.out->syncPoints[1].srcQueue == RgQueue::AsyncCompute && f.out->syncPoints[1].waitBeforePass == p3);
        CL_CHECK(f.out->barriers[f.out->passBarrierBegin[p4]].kind == RgBarrierKind::WriteAfterRead &&
                 f.out->barriers[f.out->passBarrierBegin[p4]].crossQueue == 1u);
        CL_CHECK(f.out->passSignals[p0] == 1u && f.out->passSignals[p1] == 1u && f.out->passSignals[p2] == 0u);
        CL_CHECK(f.out->resources[r].heapOffset != f.out->resources[s].heapOffset);
        f.check_oracle();
    }
    // Aliasing on one queue: A [0,1], B [2,3] share offset 0; C [1,2] overlaps both lifetimes.
    // D arrives after A..C and overlaps two previous tenants (predecessor = latest last use).
    {
        Fixture f;
        const uint32_t A = f.transient(1000u, 256u);
        const uint32_t B = f.transient(1000u, 256u);
        const uint32_t C = f.transient(500u, 256u);
        const uint32_t D = f.transient(3000u, 1024u);
        uint32_t p[6];
        for (uint32_t i = 0u; i < 6u; ++i) {
            p[i] = f.pass(RgQueue::Graphics);
        }
        f.access(p[0], A, RgState::ColorAttachment);
        f.access(p[1], A, RgState::ShaderRead);
        f.access(p[1], C, RgState::ShaderWrite);
        f.access(p[2], B, RgState::ColorAttachment);
        f.access(p[2], C, RgState::ShaderRead);
        f.access(p[3], B, RgState::ShaderRead);
        f.access(p[4], D, RgState::ShaderWrite);
        f.access(p[5], D, RgState::ShaderRead);
        CL_CHECK(f.compile() == ClStatus::Ok);
        CL_CHECK(f.out->resources[A].heapOffset == 0u);
        CL_CHECK(f.out->resources[C].heapOffset == 1024u);
        CL_CHECK(f.out->resources[B].heapOffset == 0u);
        CL_CHECK(f.out->resources[B].aliasPredecessor == A);
        CL_CHECK(f.out->resources[D].heapOffset == 0u);
        CL_CHECK(f.out->resources[D].aliasPredecessor == B);
        const RgBarrier& bd = f.out->barriers[f.out->resources[D].initialBarrier];
        CL_CHECK(bd.kind == RgBarrierKind::Alias && bd.srcPassMask == 0xFu);
        CL_CHECK(f.out->transientHeapBytes == 3000u);
        f.check_oracle();
    }
    // Aliasing across queues: only allowed when the sync points order the users.
    {
        Fixture f;
        f.g->queues.hasAsyncCompute = 1u;
        const uint32_t A = f.transient(256u);
        const uint32_t B = f.transient(256u);
        const uint32_t C = f.transient(256u);
        const uint32_t p0 = f.pass(RgQueue::AsyncCompute);
        const uint32_t p1 = f.pass(RgQueue::Graphics);
        const uint32_t p2 = f.pass(RgQueue::Graphics);
        f.access(p0, A, RgState::ShaderWrite);
        f.access(p1, A, RgState::ShaderRead); // sync compute -> graphics
        f.access(p2, B, RgState::ShaderWrite); // after p1 on graphics: B may alias A
        const uint32_t p3 = f.pass(RgQueue::AsyncCompute);
        f.access(p3, C, RgState::ShaderWrite); // compute p3 is unordered w.r.t. graphics p2: no alias with B
        CL_CHECK(f.compile() == ClStatus::Ok);
        CL_CHECK(f.out->resources[B].heapOffset == f.out->resources[A].heapOffset);
        CL_CHECK(f.out->resources[C].heapOffset != f.out->resources[B].heapOffset);
        const RgBarrier& bb = f.out->barriers[f.out->resources[B].initialBarrier];
        CL_CHECK(bb.kind == RgBarrierKind::Alias && bb.srcPassMask == (1u << p1));
        f.check_oracle();
    }
    // Unused transient gets no memory.
    {
        Fixture f;
        const uint32_t u = f.transient(64u);
        f.pass(RgQueue::Graphics);
        CL_CHECK(f.compile() == ClStatus::Ok && f.out->resources[u].heapOffset == kRgNoOffset &&
                 f.out->resources[u].firstUse == kClInvalid);
        f.check_oracle();
    }
}

// windowed = pipeline-like graphs: pass i touches resources near i * R / P, so transients are
// short-lived and aliasing is exercised heavily.
void random_graph(cltest::Rng& rng, Fixture& f, uint32_t maxPasses, uint32_t maxRes, bool windowed = false) {
    f.g->queues.hasAsyncCompute = static_cast<uint8_t>(rng.below(2u));
    f.g->queues.hasTransfer = static_cast<uint8_t>(rng.below(2u));
    const uint32_t R = 1u + rng.below(maxRes);
    const RgState states[9] = {RgState::ColorAttachment, RgState::DepthStencilWrite, RgState::DepthStencilRead,
                               RgState::ShaderRead,      RgState::ShaderWrite,       RgState::TransferSrc,
                               RgState::TransferDst,     RgState::IndirectArgs,      RgState::Present};
    for (uint32_t r = 0u; r < R; ++r) {
        if (rng.chance(25u)) {
            f.imported(states[rng.below(9u)]);
        } else {
            f.transient(1u + rng.below(1u << (4u + rng.below(12u))), 1ull << rng.below(12u));
        }
    }
    const uint32_t P = 1u + rng.below(maxPasses);
    // Few states per resource makes same-state read chains (and their skips) likely.
    const uint32_t stateSpan = 2u + rng.below(8u);
    for (uint32_t i = 0u; i < P; ++i) {
        const uint32_t p = f.pass(static_cast<RgQueue>(rng.below(3u)));
        const uint32_t n = rng.below(kRgMaxAccessesPerPass + 1u);
        for (uint32_t a = 0u; a < n; ++a) {
            uint32_t r = rng.below(R);
            if (windowed) {
                r = (i * R / P + rng.below(3u)) % R;
            }
            bool dup = false;
            for (uint32_t b = 0u; b < f.g->passes[p].accessCount; ++b) {
                dup = dup || f.g->passes[p].accesses[b].resource == r;
            }
            if (!dup) {
                f.access(p, r, states[(r * 3u + rng.below(stateSpan)) % 9u]);
            }
        }
    }
}

void test_property(uint32_t iterations) {
    cltest::Rng rng(0xC0FFEEull);
    const int base = cltest::failures();
    uint64_t hazards = 0u;
    uint64_t barriers = 0u;
    uint64_t syncs = 0u;
    uint64_t aliased = 0u;
    for (uint32_t it = 0u; it < iterations; ++it) {
        Fixture f;
        const bool big = (it % 8u) == 0u;
        random_graph(rng, f, big ? kRgMaxPasses : 12u, big ? 24u : 6u, (it % 2u) == 1u);
        CL_CHECK(f.compile() == ClStatus::Ok);
        RgOracleReport rep;
        rg_oracle_check(*f.g, *f.out, rep);
        CL_CHECK(rg_oracle_total(rep) == 0u);
        if (rg_oracle_total(rep) != 0u || cltest::failures() != base) {
            std::fprintf(stderr, "iteration %u: uncovered=%u unbacked=%u badBarrier=%u badSync=%u hb=%u alias=%u plan=%u\n",
                         it, rep.uncovered, rep.unbackedCrossQueue, rep.badBarrier, rep.badSync, rep.hbMismatch,
                         rep.aliasViolations, rep.badPlan);
            break;
        }
        hazards += rep.hazards;
        barriers += f.out->barrierCount;
        syncs += f.out->syncPointCount;
        uint64_t sum = 0u;
        for (uint32_t r = 0u; r < f.g->resourceCount; ++r) {
            if (f.out->resources[r].heapOffset != kRgNoOffset) {
                sum += f.g->resources[r].sizeBytes;
            }
            if (f.out->resources[r].aliasPredecessor != kClInvalid) {
                ++aliased;
            }
        }
        // Single-queue graphs: aliased transients have disjoint [firstUse, lastUse] intervals.
        if (f.g->queues.hasAsyncCompute == 0u && f.g->queues.hasTransfer == 0u) {
            for (uint32_t a = 0u; a < f.g->resourceCount; ++a) {
                for (uint32_t b = a + 1u; b < f.g->resourceCount; ++b) {
                    const RgResourcePlan& A = f.out->resources[a];
                    const RgResourcePlan& B = f.out->resources[b];
                    if (A.heapOffset == kRgNoOffset || B.heapOffset == kRgNoOffset) {
                        continue;
                    }
                    const bool mem = A.heapOffset < B.heapOffset + f.g->resources[b].sizeBytes &&
                                     B.heapOffset < A.heapOffset + f.g->resources[a].sizeBytes;
                    const bool live = A.firstUse <= B.lastUse && B.firstUse <= A.lastUse;
                    CL_CHECK(!(mem && live));
                }
            }
        }
        (void)sum;
    }
    std::printf("rg property: %u graphs, %llu hazards checked, %llu barriers, %llu sync points, %llu aliased transients\n",
                iterations, static_cast<unsigned long long>(hazards), static_cast<unsigned long long>(barriers),
                static_cast<unsigned long long>(syncs), static_cast<unsigned long long>(aliased));
}

// The oracle must reject corrupted plans: drop a hazard barrier's sources, drop a sync
// point, or move an aliased transient onto an unordered neighbour.
void test_oracle_mutations(uint32_t iterations) {
    cltest::Rng rng(0xBADC0DEull);
    uint32_t barrierMut = 0u, barrierCaught = 0u, syncMut = 0u, syncCaught = 0u, aliasMut = 0u, aliasCaught = 0u;
    for (uint32_t it = 0u; it < iterations; ++it) {
        Fixture f;
        random_graph(rng, f, 16u, 6u, (it % 2u) == 1u);
        CL_CHECK(f.compile() == ClStatus::Ok);
        RgOracleReport rep;
        // (1) a hazard barrier loses its sources (becomes a plain initial transition).
        uint32_t candidates[kRgMaxBarriers];
        uint32_t nc = 0u;
        for (uint32_t b = 0u; b < f.out->barrierCount; ++b) {
            const RgBarrierKind k = f.out->barriers[b].kind;
            if (k != RgBarrierKind::Initial && k != RgBarrierKind::Alias) {
                candidates[nc++] = b;
            }
        }
        if (nc > 0u) {
            RgBarrier& b = f.out->barriers[candidates[rng.below(nc)]];
            const RgBarrier saved = b;
            b.kind = RgBarrierKind::Initial;
            b.srcPassMask = 0u;
            b.crossQueue = 0u;
            rg_oracle_check(*f.g, *f.out, rep);
            ++barrierMut;
            barrierCaught += rep.uncovered > 0u ? 1u : 0u;
            b = saved;
        }
        // (2) a sync point disappears.
        if (f.out->syncPointCount > 0u) {
            const uint32_t k = rng.below(f.out->syncPointCount);
            const RgSyncPoint saved = f.out->syncPoints[k];
            f.out->syncPoints[k] = f.out->syncPoints[f.out->syncPointCount - 1u];
            --f.out->syncPointCount;
            rg_oracle_check(*f.g, *f.out, rep);
            ++syncMut;
            syncCaught += rg_oracle_total(rep) > 0u ? 1u : 0u;
            f.out->syncPoints[f.out->syncPointCount++] = f.out->syncPoints[k];
            f.out->syncPoints[k] = saved;
        }
        // (3) a transient is moved onto the bytes of a transient it is not ordered with.
        for (uint32_t a = 0u; a < f.g->resourceCount; ++a) {
            for (uint32_t b = 0u; b < f.g->resourceCount; ++b) {
                RgResourcePlan& A = f.out->resources[a];
                const RgResourcePlan& B = f.out->resources[b];
                if (a == b || A.heapOffset == kRgNoOffset || B.heapOffset == kRgNoOffset ||
                    (A.usersMask & B.usersMask) == 0u) {
                    continue;
                }
                const uint64_t moved = B.heapOffset & ~(f.g->resources[a].alignment - 1u);
                if (moved + f.g->resources[a].sizeBytes <= B.heapOffset) {
                    continue; // aligned down below B without touching it: not a conflict
                }
                const uint64_t saved = A.heapOffset;
                A.heapOffset = moved;
                rg_oracle_check(*f.g, *f.out, rep);
                ++aliasMut;
                aliasCaught += rep.aliasViolations > 0u ? 1u : 0u;
                A.heapOffset = saved;
                a = f.g->resourceCount; // one per graph
                break;
            }
        }
        rg_oracle_check(*f.g, *f.out, rep);
        CL_CHECK(rg_oracle_total(rep) == 0u); // restored plan is clean again
    }
    std::printf("rg oracle mutations: barrier %u/%u caught, sync %u/%u caught, alias %u/%u caught\n", barrierCaught,
                barrierMut, syncCaught, syncMut, aliasCaught, aliasMut);
    CL_CHECK(aliasCaught == aliasMut && syncCaught == syncMut && barrierCaught * 10u >= barrierMut * 9u);
}

} // namespace

int run_rg_tests() {
    test_validation();
    test_queue_fallback();
    test_scenarios();
    test_property(3000u * cltest::iter_scale());
    test_oracle_mutations(1000u);
    return cltest::failures();
}
