// WP-3.1 "vsm.alloc". Slang twin of vsm_alloc.comp (see there).
#define FUSE_VSM_COHERENT_WORDS 1
#include "vsm_includes.slang"

groupshared uint s_scan[256];

uint scan(uint t, uint value, out uint total) {
    s_scan[t] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < 256u; off <<= 1u) {
        uint v = s_scan[t];
        if (t >= off) {
            v += s_scan[t - off];
        }
        GroupMemoryBarrierWithGroupSync();
        s_scan[t] = v;
        GroupMemoryBarrierWithGroupSync();
    }
    total = s_scan[255];
    const uint inclusive = s_scan[t];
    GroupMemoryBarrierWithGroupSync();
    return inclusive - value;
}

[numthreads(256, 1, 1)]
void main(uint3 lid: SV_GroupThreadID) {
    VsmConstants* C = fuseVsmConstants();
    const uint t = lid.x;
    const uint work = fuseVsmHandleIndex(C.workHandle);
    const uint meta = fuseVsmHandleIndex(C.physMetaHandle);
    const uint table = fuseVsmHandleIndex(C.pageTableHandle);
    const uint frame = C.frame;
    const uint offNeed = C.offNeed;
    const uint offCandidates = C.offCandidates;
    // A: needs.
    const uint words = C.requestWords;
    const uint wpt = (words + 255u) / 256u;
    const uint w0 = min(t * wpt, words);
    const uint w1 = min(w0 + wpt, words);
    uint count = 0u;
    for (uint w = w0; w < w1; ++w) {
        count += countbits(fuse_vsm_words[work][offNeed + w]);
    }
    uint needs = 0u;
    const uint needBase = scan(t, count, needs);
    // B: candidates.
    const uint phys = C.physPages;
    const uint ppt = (phys + 255u) / 256u;
    const uint p0 = min(t * ppt, phys);
    const uint p1 = min(p0 + ppt, phys);
    uint candidates = 0u;
    for (uint key = kFuseVsmFreeKey; key >= 1u; --key) {
        uint k = 0u;
        for (uint p = p0; p < p1; ++p) {
            if (fuseVsmCandidateKey(fuse_vsm_words[meta][p * 2u], fuse_vsm_words[meta][p * 2u + 1u], frame) == key) {
                ++k;
            }
        }
        uint total = 0u;
        uint pos = candidates + scan(t, k, total);
        if (k != 0u) {
            for (uint p = p0; p < p1; ++p) {
                if (fuseVsmCandidateKey(fuse_vsm_words[meta][p * 2u], fuse_vsm_words[meta][p * 2u + 1u], frame) == key) {
                    fuse_vsm_words[work][offCandidates + pos] = p;
                    ++pos;
                }
            }
        }
        candidates += total;
    }
    AllMemoryBarrierWithGroupSync();
    // C: assignment.
    const uint grant = min(needs, candidates);
    uint rank = needBase;
    for (uint w = w0; w < w1 && rank < grant; ++w) {
        uint bits = fuse_vsm_words[work][offNeed + w];
        while (bits != 0u && rank < grant) {
            const uint b = firstbitlow(bits);
            bits &= bits - 1u;
            const uint v = w * 32u + b;
            const uint p = fuse_vsm_words[work][offCandidates + rank];
            const uint old = fuse_vsm_words[meta][p * 2u];
            if (old != kFuseVsmPageNone) {
                fuse_vsm_words[table][old] = 0u;
                InterlockedAdd(fuse_vsm_words[work][kFuseVsmCounterEvicted], 1u);
            }
            fuse_vsm_words[meta][p * 2u] = v;
            fuse_vsm_words[meta][p * 2u + 1u] = frame;
            fuse_vsm_words[table][v] = kFuseVsmPteMapped | p;
            ++rank;
        }
    }
    if (t == 0u) {
        fuse_vsm_words[work][kFuseVsmCounterCandidates] = candidates;
        fuse_vsm_words[work][kFuseVsmCounterAllocated] = grant;
        fuse_vsm_words[work][kFuseVsmCounterFailed] = needs - grant;
    }
}
