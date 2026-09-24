// Render-graph compiler model (WP-0.8). See rg_model.hpp for the contract.
#include "fuse/core_logic/rg_model.hpp"

namespace fuse {
namespace core_logic {

bool rg_state_is_write(RgState s) {
    return s == RgState::ColorAttachment || s == RgState::DepthStencilWrite || s == RgState::ShaderWrite ||
           s == RgState::TransferDst;
}

// File-local helpers. (No anonymous namespace: CBMC 5.9x rejects it; `static` instead.)
struct RgTrack {
    RgState cur;
    uint32_t lastWriter;   // last pass that wrote or transitioned the resource (kClInvalid if none)
    uint64_t readers;      // readers since lastWriter
    uint8_t covered;       // queue bits on which reads in `cur` need no further barrier
    uint8_t touched;
};

static inline uint64_t bit64(uint32_t i) { return 1ull << i; }
static inline uint8_t qbit(RgQueue q) { return static_cast<uint8_t>(1u << static_cast<uint32_t>(q)); }
static inline uint32_t qidx(RgQueue q) { return static_cast<uint32_t>(q); }

static RgQueue assign_queue(RgQueue requested, const RgQueueConfig& qc) {
    if (requested == RgQueue::AsyncCompute) {
        return qc.hasAsyncCompute ? RgQueue::AsyncCompute : RgQueue::Graphics;
    }
    if (requested == RgQueue::Transfer) {
        if (qc.hasTransfer) {
            return RgQueue::Transfer;
        }
        return qc.hasAsyncCompute ? RgQueue::AsyncCompute : RgQueue::Graphics;
    }
    return RgQueue::Graphics;
}

static ClStatus fail(RgCompileResult& out, ClStatus st, uint32_t pass, uint32_t resource) {
    out.status = st;
    out.errorPass = pass;
    out.errorResource = resource;
    return st;
}

static ClStatus validate(const RgGraphDesc& g, RgCompileResult& out) {
    if (g.passCount > kRgMaxPasses || g.resourceCount > kRgMaxResources) {
        return fail(out, ClStatus::CapacityExceeded, kClInvalid, kClInvalid);
    }
    for (uint32_t r = 0u; r < g.resourceCount; ++r) {
        const RgResourceDesc& rd = g.resources[r];
        if (rd.imported != 0u) {
            if (static_cast<uint32_t>(rd.initialState) >= static_cast<uint32_t>(RgState::Count)) {
                return fail(out, ClStatus::InvalidArgument, kClInvalid, r);
            }
        } else if (rd.sizeBytes == 0u || !cl_is_pow2(rd.alignment)) {
            return fail(out, ClStatus::InvalidArgument, kClInvalid, r);
        }
    }
    for (uint32_t p = 0u; p < g.passCount; ++p) {
        const RgPassDesc& pd = g.passes[p];
        if (static_cast<uint32_t>(pd.queue) >= kRgQueueCount) {
            return fail(out, ClStatus::InvalidArgument, p, kClInvalid);
        }
        if (pd.accessCount > kRgMaxAccessesPerPass) {
            return fail(out, ClStatus::CapacityExceeded, p, kClInvalid);
        }
        for (uint32_t a = 0u; a < pd.accessCount; ++a) {
            const RgAccess& ac = pd.accesses[a];
            if (ac.resource >= g.resourceCount) {
                return fail(out, ClStatus::InvalidArgument, p, ac.resource);
            }
            const uint32_t st = static_cast<uint32_t>(ac.state);
            if (st == 0u || st >= static_cast<uint32_t>(RgState::Count)) {
                return fail(out, ClStatus::InvalidArgument, p, ac.resource);
            }
            for (uint32_t b = 0u; b < a; ++b) {
                if (pd.accesses[b].resource == ac.resource) {
                    return fail(out, ClStatus::InvalidArgument, p, ac.resource);
                }
            }
        }
    }
    return ClStatus::Ok;
}

static uint32_t emit_barrier(RgCompileResult& out, uint32_t resource, uint32_t pass, uint64_t srcMask, RgState srcState,
                      RgState dstState, RgBarrierKind kind) {
    // At most one barrier per access, so barrierCount <= passCount * kRgMaxAccessesPerPass
    // == kRgMaxBarriers: no overflow check is needed (and none would be reachable).
    const uint32_t idx = out.barrierCount++;
    RgBarrier& b = out.barriers[idx];
    b.resource = resource;
    b.beforePass = pass;
    b.srcPassMask = srcMask;
    b.srcState = srcState;
    b.dstState = dstState;
    b.kind = kind;
    b.crossQueue = 0u;
    for (uint32_t s = 0u; s < pass; ++s) {
        if ((srcMask & bit64(s)) != 0u && out.passQueue[s] != out.passQueue[pass]) {
            b.crossQueue = 1u;
        }
    }
    return idx;
}

// True when every user of `a` happens-before every user of `b`.
static bool all_before(const RgCompileResult& out, uint32_t passCount, uint32_t a, uint32_t b) {
    const uint64_t ub = out.resources[b].usersMask;
    for (uint32_t x = 0u; x < passCount; ++x) {
        if ((out.resources[a].usersMask & bit64(x)) != 0u && (out.happensBefore[x] & ub) != ub) {
            return false;
        }
    }
    return true;
}

static bool ranges_overlap(uint64_t a0, uint64_t as, uint64_t b0, uint64_t bs) { return a0 < b0 + bs && b0 < a0 + as; }

ClStatus rg_compile(const RgGraphDesc& g, RgCompileResult& out) {
    out.status = ClStatus::Ok;
    out.errorPass = kClInvalid;
    out.errorResource = kClInvalid;
    out.barrierCount = 0u;
    out.syncPointCount = 0u;
    out.transientHeapBytes = 0u;
    if (validate(g, out) != ClStatus::Ok) {
        return out.status;
    }

    // ---- queue assignment + per-queue timeline values ------------------------------------
    uint32_t queueCounter[kRgQueueCount] = {0u, 0u, 0u};
    uint64_t queuePasses[kRgQueueCount] = {0u, 0u, 0u};
    for (uint32_t p = 0u; p < g.passCount; ++p) {
        const RgQueue q = assign_queue(g.passes[p].queue, g.queues);
        out.passQueue[p] = q;
        out.passTimelineValue[p] = ++queueCounter[qidx(q)];
        out.passSignals[p] = 0u;
        out.passBarrierBegin[p] = 0u;
        out.passBarrierCount[p] = 0u;
        queuePasses[qidx(q)] |= bit64(p);
    }

    RgTrack track[kRgMaxResources];
    for (uint32_t r = 0u; r < g.resourceCount; ++r) {
        track[r].cur = g.resources[r].imported != 0u ? g.resources[r].initialState : RgState::Undefined;
        track[r].lastWriter = kClInvalid;
        track[r].readers = 0u;
        track[r].covered = 0u;
        track[r].touched = 0u;
        RgResourcePlan& rp = out.resources[r];
        rp.firstUse = kClInvalid;
        rp.lastUse = kClInvalid;
        rp.usersMask = 0u;
        rp.heapOffset = kRgNoOffset;
        rp.aliasPredecessor = kClInvalid;
        rp.initialBarrier = kClInvalid;
    }

    // waited[dst][src]: highest src timeline value the dst queue has already waited for.
    uint32_t waited[kRgQueueCount][kRgQueueCount] = {{0u, 0u, 0u}, {0u, 0u, 0u}, {0u, 0u, 0u}};

    // ---- hazard scan: barriers + sync points ---------------------------------------------
    for (uint32_t p = 0u; p < g.passCount; ++p) {
        const RgPassDesc& pd = g.passes[p];
        const RgQueue q = out.passQueue[p];
        out.passBarrierBegin[p] = out.barrierCount;
        uint32_t needValue[kRgQueueCount] = {0u, 0u, 0u};
        uint32_t needPass[kRgQueueCount] = {kClInvalid, kClInvalid, kClInvalid};

        for (uint32_t a = 0u; a < pd.accessCount; ++a) {
            const uint32_t r = pd.accesses[a].resource;
            const RgState st = pd.accesses[a].state;
            const bool write = rg_state_is_write(st);
            RgTrack& t = track[r];
            RgResourcePlan& rp = out.resources[r];
            if (rp.firstUse == kClInvalid) {
                rp.firstUse = p;
            }
            rp.lastUse = p;
            rp.usersMask |= bit64(p);

            uint32_t bidx = kClInvalid;
            if (t.touched == 0u) {
                t.touched = 1u;
                const bool imported = g.resources[r].imported != 0u;
                if (!imported || st != t.cur) {
                    bidx = emit_barrier(out, r, p, 0u, t.cur, st, RgBarrierKind::Initial);
                    rp.initialBarrier = bidx;
                }
                if (write) {
                    t.lastWriter = p;
                    t.readers = 0u;
                    t.covered = 0u;
                } else if (bidx != kClInvalid) {
                    t.lastWriter = p; // the transition acts as the write for later readers
                    t.readers = bit64(p);
                    t.covered = qbit(q);
                } else {
                    t.readers = bit64(p);
                    t.covered = 0x7u; // imported, read in its initial state: nothing to wait for
                }
                t.cur = st;
            } else if (write) {
                if (t.readers != 0u) {
                    bidx = emit_barrier(out, r, p, t.readers, t.cur, st, RgBarrierKind::WriteAfterRead);
                } else {
                    bidx = emit_barrier(out, r, p, bit64(t.lastWriter), t.cur, st, RgBarrierKind::WriteAfterWrite);
                }
                t.lastWriter = p;
                t.readers = 0u;
                t.covered = 0u;
                t.cur = st;
            } else if (st != t.cur) {
                if (t.readers != 0u) {
                    bidx = emit_barrier(out, r, p, t.readers, t.cur, st, RgBarrierKind::LayoutTransition);
                } else {
                    bidx = emit_barrier(out, r, p, bit64(t.lastWriter), t.cur, st, RgBarrierKind::ReadAfterWrite);
                }
                t.lastWriter = p;
                t.readers = bit64(p);
                t.covered = qbit(q);
                t.cur = st;
            } else {
                if ((t.covered & qbit(q)) == 0u) {
                    bidx = emit_barrier(out, r, p, bit64(t.lastWriter), t.cur, st, RgBarrierKind::ReadAfterWrite);
                    t.covered = static_cast<uint8_t>(t.covered | qbit(q));
                }
                t.readers |= bit64(p);
            }

            if (bidx != kClInvalid && out.barriers[bidx].crossQueue != 0u) {
                const uint64_t m = out.barriers[bidx].srcPassMask;
                for (uint32_t s = 0u; s < p; ++s) {
                    const uint32_t sq = qidx(out.passQueue[s]);
                    if ((m & bit64(s)) != 0u && sq != qidx(q) && out.passTimelineValue[s] > needValue[sq]) {
                        needValue[sq] = out.passTimelineValue[s];
                        needPass[sq] = s;
                    }
                }
            }
        }
        out.passBarrierCount[p] = out.barrierCount - out.passBarrierBegin[p];

        for (uint32_t sq = 0u; sq < kRgQueueCount; ++sq) {
            if (needValue[sq] > waited[qidx(q)][sq]) {
                // At most kRgQueueCount-1 sync points per pass (sq != q), so no overflow.
                RgSyncPoint& sp = out.syncPoints[out.syncPointCount++];
                sp.srcQueue = static_cast<RgQueue>(sq);
                sp.dstQueue = q;
                sp.waitValue = needValue[sq];
                sp.waitBeforePass = p;
                waited[qidx(q)][sq] = needValue[sq];
                out.passSignals[needPass[sq]] = 1u;
            }
        }
    }

    // ---- happens-before closure (same-queue order + sync points), reverse pass order -----
    for (uint32_t i = g.passCount; i > 0u; --i) {
        const uint32_t p = i - 1u;
        uint64_t m = 0u;
        for (uint32_t d = p + 1u; d < g.passCount; ++d) {
            if (out.passQueue[d] == out.passQueue[p]) {
                m |= bit64(d) | out.happensBefore[d];
                break;
            }
        }
        for (uint32_t s = 0u; s < out.syncPointCount; ++s) {
            const RgSyncPoint& sp = out.syncPoints[s];
            if (sp.srcQueue == out.passQueue[p] && sp.waitValue >= out.passTimelineValue[p]) {
                m |= bit64(sp.waitBeforePass) | out.happensBefore[sp.waitBeforePass];
            }
        }
        out.happensBefore[p] = m;
    }

    // ---- transient aliasing: first-fit placement in first-use order ----------------------
    uint32_t placed[kRgMaxResources];
    uint32_t placedCount = 0u;
    for (uint32_t p = 0u; p < g.passCount; ++p) {
        for (uint32_t r = 0u; r < g.resourceCount; ++r) {
            if (g.resources[r].imported != 0u || out.resources[r].firstUse != p) {
                continue;
            }
            const uint64_t size = g.resources[r].sizeBytes;
            const uint64_t align = g.resources[r].alignment;
            bool conflict[kRgMaxResources];
            for (uint32_t i = 0u; i < placedCount; ++i) {
                const uint32_t o = placed[i];
                // Placement is in first-use order and happens-before edges only point forward,
                // so "every use of r happens-before every use of o" can never hold here.
                conflict[i] = !all_before(out, g.passCount, o, r);
            }
            // Candidates: offset 0 and the aligned end of every conflicting placed resource.
            uint64_t best = kRgNoOffset;
            for (uint32_t c = 0u; c <= placedCount; ++c) {
                uint64_t cand = 0u;
                if (c > 0u) {
                    if (!conflict[c - 1u]) {
                        continue;
                    }
                    const uint32_t o = placed[c - 1u];
                    cand = cl_align_up(out.resources[o].heapOffset + g.resources[o].sizeBytes, align);
                }
                if (cand >= best) {
                    continue;
                }
                bool fits = true;
                for (uint32_t i = 0u; i < placedCount; ++i) {
                    const uint32_t o = placed[i];
                    if (conflict[i] &&
                        ranges_overlap(cand, size, out.resources[o].heapOffset, g.resources[o].sizeBytes)) {
                        fits = false;
                    }
                }
                if (fits) {
                    best = cand;
                }
            }
            // `best` is always found: the end of the highest conflicting range is free.
            RgResourcePlan& rp = out.resources[r];
            rp.heapOffset = best;
            if (best + size > out.transientHeapBytes) {
                out.transientHeapBytes = best + size;
            }
            // Previous tenants of any overlapping byte: all happen-before r (first-use order).
            uint64_t srcMask = 0u;
            uint32_t pred = kClInvalid;
            const uint64_t sameQueue = queuePasses[qidx(out.passQueue[p])];
            for (uint32_t i = 0u; i < placedCount; ++i) {
                const uint32_t o = placed[i];
                if (!conflict[i] && ranges_overlap(best, size, out.resources[o].heapOffset, g.resources[o].sizeBytes)) {
                    srcMask |= out.resources[o].usersMask & sameQueue;
                    if (pred == kClInvalid || out.resources[o].lastUse > out.resources[pred].lastUse) {
                        pred = o;
                    }
                }
            }
            if (pred != kClInvalid) {
                rp.aliasPredecessor = pred;
                RgBarrier& b = out.barriers[rp.initialBarrier];
                b.kind = RgBarrierKind::Alias;
                b.srcPassMask = srcMask;
            }
            placed[placedCount++] = r;
        }
    }
    return ClStatus::Ok;
}

} // namespace core_logic
} // namespace fuse
