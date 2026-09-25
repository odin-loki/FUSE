// Brute-force oracle for rg_compile() (WP-0.8). Shared by the property test and the CBMC
// harness, so it follows the core_logic dialect (no STL, no heap, bounded loops).
//
// It re-derives, independently of the compiler's incremental tracking:
//   * every hazard pair (i < j accessing r; a write on either side, a state change, or i being
//     a layout transition and j on another queue) and
//     checks that the barrier list orders it (a chain of barrier edges on r, see rg_model.hpp);
//   * that every cross-queue barrier edge is backed by a timeline wait;
//   * happens-before from same-queue order + sync points, and that aliased transients whose
//     byte ranges overlap are totally ordered by it;
//   * structural properties of barriers / sync points / plans.
#pragma once

#include "fuse/core_logic/rg_model.hpp"

namespace fuse {
namespace core_logic {

struct RgOracleReport {
    uint32_t hazards;
    uint32_t uncovered;          // hazard pairs with no barrier chain
    uint32_t unbackedCrossQueue; // cross-queue barrier edges without a sync point
    uint32_t badBarrier;         // structural barrier errors
    uint32_t badSync;            // structural sync-point errors
    uint32_t hbMismatch;         // compiler happensBefore != oracle
    uint32_t aliasViolations;    // overlapping bytes between unordered transients
    uint32_t badPlan;            // lifetime / offset / alignment errors
};

inline uint64_t rg_oracle_bit(uint32_t i) { return 1ull << i; }

// State of resource r in pass p, or Undefined when p does not access r.
inline RgState rg_oracle_access(const RgGraphDesc& g, uint32_t p, uint32_t r) {
    for (uint32_t a = 0u; a < g.passes[p].accessCount; ++a) {
        if (g.passes[p].accesses[a].resource == r) {
            return g.passes[p].accesses[a].state;
        }
    }
    return RgState::Undefined;
}

inline uint32_t rg_oracle_total(const RgOracleReport& r) {
    return r.uncovered + r.unbackedCrossQueue + r.badBarrier + r.badSync + r.hbMismatch + r.aliasViolations +
           r.badPlan;
}

inline void rg_oracle_check(const RgGraphDesc& g, const RgCompileResult& out, RgOracleReport& rep) {
    rep.hazards = 0u;
    rep.uncovered = 0u;
    rep.unbackedCrossQueue = 0u;
    rep.badBarrier = 0u;
    rep.badSync = 0u;
    rep.hbMismatch = 0u;
    rep.aliasViolations = 0u;
    rep.badPlan = 0u;
    const uint32_t P = g.passCount;

    // ---- sync points ------------------------------------------------------------------------
    uint32_t signalPass[kRgMaxSyncPoints];
    for (uint32_t s = 0u; s < out.syncPointCount; ++s) {
        const RgSyncPoint& sp = out.syncPoints[s];
        signalPass[s] = kClInvalid;
        if (sp.waitBeforePass >= P || sp.srcQueue == sp.dstQueue || out.passQueue[sp.waitBeforePass] != sp.dstQueue ||
            (s > 0u && out.syncPoints[s - 1u].waitBeforePass > sp.waitBeforePass)) {
            ++rep.badSync;
            continue;
        }
        for (uint32_t p = 0u; p < sp.waitBeforePass; ++p) {
            if (out.passQueue[p] == sp.srcQueue && out.passTimelineValue[p] == sp.waitValue) {
                signalPass[s] = p;
            }
        }
        if (signalPass[s] == kClInvalid || out.passSignals[signalPass[s]] == 0u) {
            ++rep.badSync;
        }
    }

    // ---- happens-before: next pass on the same queue + signal -> wait --------------------
    uint64_t hb[kRgMaxPasses];
    for (uint32_t i = P; i > 0u; --i) {
        const uint32_t p = i - 1u;
        uint64_t m = 0u;
        uint32_t nextSame = kClInvalid;
        for (uint32_t d = P; d > p + 1u; --d) {
            if (out.passQueue[d - 1u] == out.passQueue[p]) {
                nextSame = d - 1u;
            }
        }
        if (nextSame != kClInvalid) {
            m |= rg_oracle_bit(nextSame) | hb[nextSame];
        }
        // Waits on any value >= our timeline value order us too (the queue signals in order).
        for (uint32_t s = 0u; s < out.syncPointCount; ++s) {
            const uint32_t sig = signalPass[s];
            if (sig != kClInvalid && out.passQueue[sig] == out.passQueue[p] && sig >= p) {
                const uint32_t d = out.syncPoints[s].waitBeforePass;
                m |= rg_oracle_bit(d) | hb[d];
            }
        }
        hb[p] = m;
        if (m != out.happensBefore[p]) {
            ++rep.hbMismatch;
        }
    }

    // ---- barrier structure ------------------------------------------------------------------
    uint32_t cursor = 0u;
    for (uint32_t p = 0u; p < P; ++p) {
        if (out.passBarrierBegin[p] != cursor) {
            ++rep.badBarrier;
        }
        cursor += out.passBarrierCount[p];
    }
    if (cursor != out.barrierCount) {
        ++rep.badBarrier;
    }
    for (uint32_t bi = 0u; bi < out.barrierCount; ++bi) {
        const RgBarrier& b = out.barriers[bi];
        if (b.beforePass >= P || b.resource >= g.resourceCount ||
            (bi > 0u && out.barriers[bi - 1u].beforePass > b.beforePass)) {
            ++rep.badBarrier;
            continue;
        }
        if ((b.srcPassMask >> b.beforePass) != 0u || rg_oracle_access(g, b.beforePass, b.resource) != b.dstState) {
            ++rep.badBarrier;
        }
        uint8_t cross = 0u;
        for (uint32_t s = 0u; s < b.beforePass; ++s) {
            if ((b.srcPassMask & rg_oracle_bit(s)) != 0u && out.passQueue[s] != out.passQueue[b.beforePass]) {
                cross = 1u;
            }
        }
        if (cross != b.crossQueue) {
            ++rep.badBarrier;
        }
        if (b.kind == RgBarrierKind::Initial && b.srcPassMask != 0u) {
            ++rep.badBarrier;
        }
        if (b.kind == RgBarrierKind::Alias && cross != 0u) {
            ++rep.badBarrier;
        }
        if (b.kind != RgBarrierKind::Initial && b.kind != RgBarrierKind::Alias &&
            (b.srcPassMask == 0u || (b.srcPassMask & ~out.resources[b.resource].usersMask) != 0u)) {
            ++rep.badBarrier;
        }
    }

    // ---- per-resource hazards vs. barrier edges ---------------------------------------------
    for (uint32_t r = 0u; r < g.resourceCount; ++r) {
        uint64_t edge[kRgMaxPasses];
        for (uint32_t p = 0u; p < P; ++p) {
            edge[p] = 0u;
        }
        for (uint32_t bi = 0u; bi < out.barrierCount; ++bi) {
            const RgBarrier& b = out.barriers[bi];
            if (b.resource != r || b.kind == RgBarrierKind::Initial || b.kind == RgBarrierKind::Alias ||
                b.beforePass >= P) {
                continue;
            }
            const uint32_t d = b.beforePass;
            const RgQueue qd = out.passQueue[d];
            // The barrier's scope ends at the next barrier on r recorded on the same queue.
            uint32_t limit = P;
            for (uint32_t bj = 0u; bj < out.barrierCount; ++bj) {
                const RgBarrier& o = out.barriers[bj];
                if (o.resource == r && o.beforePass > d && o.beforePass < limit && out.passQueue[o.beforePass] == qd) {
                    limit = o.beforePass;
                }
            }
            for (uint32_t e = d; e < limit; ++e) {
                if (out.passQueue[e] != qd) {
                    continue;
                }
                if (rg_oracle_access(g, e, r) != b.dstState) {
                    continue;
                }
                for (uint32_t s = 0u; s < d; ++s) {
                    if ((b.srcPassMask & rg_oracle_bit(s)) == 0u) {
                        continue;
                    }
                    edge[s] |= rg_oracle_bit(e);
                    if (out.passQueue[s] != qd) {
                        bool backed = false;
                        for (uint32_t k = 0u; k < out.syncPointCount; ++k) {
                            const RgSyncPoint& sp = out.syncPoints[k];
                            if (sp.srcQueue == out.passQueue[s] && sp.dstQueue == qd &&
                                sp.waitValue >= out.passTimelineValue[s] && sp.waitBeforePass <= e) {
                                backed = true;
                            }
                        }
                        if (!backed) {
                            ++rep.unbackedCrossQueue;
                        }
                    }
                }
            }
        }
        // Transitive closure (edges point forward).
        uint64_t reach[kRgMaxPasses];
        for (uint32_t i = P; i > 0u; --i) {
            const uint32_t p = i - 1u;
            uint64_t m = edge[p];
            for (uint32_t e = p + 1u; e < P; ++e) {
                if ((edge[p] & rg_oracle_bit(e)) != 0u) {
                    m |= reach[e];
                }
            }
            reach[p] = m;
        }
        // A read whose state differs from the previous one is a layout transition, i.e. a
        // write performed by the barrier before it: later same-state readers on OTHER queues
        // must be ordered after it too (same-queue readers are covered by that barrier).
        RgState prevState = g.resources[r].imported != 0u ? g.resources[r].initialState : RgState::Undefined;
        uint64_t transition = 0u;
        for (uint32_t p = 0u; p < P; ++p) {
            const RgState sp = rg_oracle_access(g, p, r);
            if (sp == RgState::Undefined) {
                continue;
            }
            if (sp != prevState) { // a transient's first use always is (it starts Undefined)
                transition |= rg_oracle_bit(p);
            }
            prevState = sp;
        }
        for (uint32_t i = 0u; i < P; ++i) {
            const RgState si = rg_oracle_access(g, i, r);
            if (si == RgState::Undefined) {
                continue;
            }
            for (uint32_t j = i + 1u; j < P; ++j) {
                const RgState sj = rg_oracle_access(g, j, r);
                if (sj == RgState::Undefined) {
                    continue;
                }
                const bool transitionHazard =
                    (transition & rg_oracle_bit(i)) != 0u && out.passQueue[i] != out.passQueue[j];
                if (rg_state_is_write(si) || rg_state_is_write(sj) || si != sj || transitionHazard) {
                    ++rep.hazards;
                    if ((reach[i] & rg_oracle_bit(j)) == 0u) {
                        ++rep.uncovered;
                    }
                }
            }
        }
    }

    // ---- lifetimes / aliasing ---------------------------------------------------------------
    for (uint32_t r = 0u; r < g.resourceCount; ++r) {
        const RgResourcePlan& a = out.resources[r];
        uint64_t users = 0u;
        for (uint32_t p = 0u; p < P; ++p) {
            if (rg_oracle_access(g, p, r) != RgState::Undefined) {
                users |= rg_oracle_bit(p);
            }
        }
        if (users != a.usersMask) {
            ++rep.badPlan;
        }
        const bool placedExpected = g.resources[r].imported == 0u && users != 0u;
        if (placedExpected != (a.heapOffset != kRgNoOffset)) {
            ++rep.badPlan;
            continue;
        }
        if (!placedExpected) {
            continue;
        }
        if ((a.heapOffset & (g.resources[r].alignment - 1u)) != 0u ||
            a.heapOffset + g.resources[r].sizeBytes > out.transientHeapBytes) {
            ++rep.badPlan;
        }
        for (uint32_t o = 0u; o < g.resourceCount; ++o) {
            const RgResourcePlan& b = out.resources[o];
            if (o == r || b.heapOffset == kRgNoOffset || g.resources[o].imported != 0u) {
                continue;
            }
            const bool overlap = a.heapOffset < b.heapOffset + g.resources[o].sizeBytes &&
                                 b.heapOffset < a.heapOffset + g.resources[r].sizeBytes;
            if (!overlap) {
                continue;
            }
            bool aBeforeB = true;
            for (uint32_t x = 0u; x < P; ++x) {
                if ((a.usersMask & rg_oracle_bit(x)) != 0u && (hb[x] & b.usersMask) != b.usersMask) {
                    aBeforeB = false;
                }
            }
            bool bBeforeA = true;
            for (uint32_t x = 0u; x < P; ++x) {
                if ((b.usersMask & rg_oracle_bit(x)) != 0u && (hb[x] & a.usersMask) != a.usersMask) {
                    bBeforeA = false;
                }
            }
            if (!aBeforeB && !bBeforeA) {
                ++rep.aliasViolations;
            }
        }
    }
}

} // namespace core_logic
} // namespace fuse
