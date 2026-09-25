// FUSE Relight RL-5.4: the world-space hash-grid radiance cache (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.4 "Radiance
// cache (NRC replacement, default)", §5.8 row `radiance_cache_*`). FUSE's own code, written from the papers: spatial
// hashing of path vertices into a world-space grid after Binder, Fricke and Keller 2019, "Massively Parallel Path
// Space Filtering" (arXiv:1902.05942), with the per-distance level of detail of Gautron 2020, "Real-Time Ray-Traced
// Ambient Occlusion of Complex Scenes using Spatial Hashing" (SIGGRAPH Talks), and path termination by the path
// spread of Mueller, Rousselle, Novak and Keller 2021, "Real-time Neural Radiance Caching for Path Tracing" (ACM TOG
// 40(4)) (itself after Bekaert et al. 2003). No NVIDIA SDK source (NRC / SHaRC) was used.
//
// SINGLE SOURCE. Compiled three ways with the dialect macros set by the includer:
//   C++    kernels/radiance_cache_cpp.hpp        the CPU reference (RadianceCacheCpu; std::atomic_ref table access)
//   Slang  render/pathtrace/radiance_cache/shaders/radiance_cache.slang, kernels/radiance_cache_hook.slang
//   GLSL   render/pathtrace/radiance_cache/shaders/radiance_cache.comp,  kernels/radiance_cache_hook.glsl
// Common subset as pt_reference_core.h (HLSL type names, no const locals, f-suffixed literals).
// Required macros: RC_FN, RC_CONST, RC_OUT(T), RC_INOUT(T), RC_CTX_PARAM / RC_CTX_ARG (the table context),
// RC_ASUINT(f) / RC_ASFLOAT(u) (bit casts), RC_PRECISE (no contraction: GLSL / Slang `precise`), RC_PARAM_WORDS(name)
// (float4[kRcParamWords]).
// Required accessors (u32 word index into the table, header included):
//   uint rcTableLoad(RC_CTX_PARAM uint word);
//   void rcTableStore(RC_CTX_PARAM uint word, uint value);
//   uint rcTableCas(RC_CTX_PARAM uint word, uint compare, uint value);   returns the previous value
//   uint rcTableAdd(RC_CTX_PARAM uint word, uint value);                 returns the previous value
//
// CELLS. A path vertex (position p, normal n facing the incoming ray) falls into the cell
//   level L   the smallest L <= kRcMaxLevel with cellSize x 2^L >= |p - camera| x pixelAngle x cellPixels: cells cover
//             about cellPixels pixels on screen at their distance (adaptive cell size by distance; computed on
//             squared lengths by exact doubling, so every dialect picks the same level),
//   q         floor(p x invCellSize x 2^-L) per axis (exact power-of-two scaling),
//   bin       the dominant axis of n and its sign (6 bins: surfaces facing apart never share a cell),
// hashed by the PCG hash [Jarzynski and Olano 2020] into a probe start slot and, through a second avalanche
// (lowbias32), a 32-bit checksum. Open addressing: linear probing over maxProbe slots; a record whose probe
// sequence is full is dropped (counted).
//
// TABLE (u32 words; kRcHeaderWords of header, then capacity slots of kRcSlotWords):
//   header    words 0..19   RcParams (float4 words, the train stage copies them from the frame's ring block, so the
//                           trace pass reads the frame's parameters from the table alone), 32.. counters (kRcCounter*)
//   slot      0   checksum (0: empty; set by compare-and-swap on first insertion)
//             1-3 this frame's radiance sum (fixed point, x kRcFixedScale, round to nearest), 4 this frame's samples
//             5   age (frames since the last sample), 6 level | bin << 8 | 1 << 16 (diagnostics), 7 reserved
//             8-10 resolved radiance (f32 bits), 11 resolved sample count (f32 bits)
// Integer atomics make the update order-independent: the same records give the same per-cell sums on every backend
// (slot placement may differ when keys collide; the gates compare per key).
//
// FRAME. update (one record per item: insert, add), resolve (one slot per item: temporal blend of the frame's mean
// into the resolved radiance with the sample count capped at maxSamples - an exponential window -, clear the sums,
// age and evict cells untouched for maxAge frames), query (read the resolved radiance of the vertex's cell once it
// has minSamples samples).

// ---- hashing --------------------------------------------------------------------------------------------------

/// PCG hash [Jarzynski and Olano 2020, "Hash Functions for GPU Rendering", JCGT 9(3)].
RC_FN uint rcHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

/// lowbias32 avalanche (C. Wellons, "Prospecting for hash functions", 2018; public domain).
RC_FN uint rcMix(uint v) {
    uint x = v;
    x = x ^ (x >> 16u);
    x = x * 2146121005u;
    x = x ^ (x >> 15u);
    x = x * 2221713035u;
    x = x ^ (x >> 16u);
    return x;
}

RC_FN uint rcMinU(uint a, uint b) { return a < b ? a : b; }
RC_FN uint rcMaxU(uint a, uint b) { return a > b ? a : b; }

// ---- parameters -----------------------------------------------------------------------------------------------

RC_FN RcParams rcParamsUnpack(RC_PARAM_WORDS(w)) {
    RcParams R;
    R.camera = float3(w[0].x, w[0].y, w[0].z);
    R.pixelAngle = w[0].w;
    R.cellSize = w[1].x;
    R.invCellSize = w[1].y;
    R.cellPixels = w[1].z;
    R.capacity = uint(w[1].w);
    R.maxProbe = uint(w[2].x);
    R.trainStride = uint(w[2].y);
    R.frame = uint(w[2].z);
    R.flags = uint(w[2].w);
    R.minSamples = w[3].x;
    R.spreadThreshold = w[3].y;
    R.maxSamples = w[3].z;
    R.maxRadiance = w[3].w;
    R.maxAge = uint(w[4].x);
    R.minBounce = uint(w[4].y);
    R.minRoughness = w[4].z;
    R.maxVertices = uint(w[4].w);
    return R;
}

// ---- cells ----------------------------------------------------------------------------------------------------

/// The level of detail of point p (see CELLS).
RC_FN uint rcLevel(RcParams R, float3 p) {
    RC_PRECISE float3 v = p - R.camera;
    RC_PRECISE float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
    RC_PRECISE float k = R.pixelAngle * R.cellPixels;
    RC_PRECISE float f2 = d2 * (k * k);
    uint level = 0u;
    float s = R.cellSize;
    while (level < kRcMaxLevel && s * s < f2) {
        s = s * 2.0f;
        level = level + 1u;
    }
    return level;
}

/// Cell edge of a level (exact doubling).
RC_FN float rcLevelSize(RcParams R, uint level) {
    float s = R.cellSize;
    for (uint i = 0u; i < level; ++i) {
        s = s * 2.0f;
    }
    return s;
}

RC_FN uint rcNormalBin(float3 n) {
    float ax = abs(n.x);
    float ay = abs(n.y);
    float az = abs(n.z);
    if (ax >= ay && ax >= az) {
        return n.x < 0.0f ? 1u : 0u;
    }
    if (ay >= az) {
        return n.y < 0.0f ? 3u : 2u;
    }
    return n.z < 0.0f ? 5u : 4u;
}

RC_FN uint rcQuantize(float x) {
    float c = max(min(floor(x), 1073741824.0f), -1073741824.0f);
    return uint(int(c));
}

RC_FN RcKey rcCellKey(RcParams R, float3 p, float3 n) {
    RcKey k;
    k.level = rcLevel(R, p);
    k.bin = rcNormalBin(n);
    float inv = R.invCellSize;
    for (uint i = 0u; i < k.level; ++i) {
        inv = inv * 0.5f;
    }
    RC_PRECISE float fx = p.x * inv;
    RC_PRECISE float fy = p.y * inv;
    RC_PRECISE float fz = p.z * inv;
    uint qx = rcQuantize(fx);
    uint qy = rcQuantize(fy);
    uint qz = rcQuantize(fz);
    uint tag = k.level | (k.bin << 8u);
    uint h = rcHash(qx ^ rcHash(qy ^ rcHash(qz ^ rcHash(tag))));
    k.slot = h & (R.capacity - 1u);
    uint c = rcMix(h ^ rcMix(qz + 0x9E3779B9u * (tag + 1u)) ^ rcMix(qx * 0x85EBCA6Bu + qy));
    k.check = c == 0u ? 1u : c;
    return k;
}

RC_FN uint rcSlotBase(uint slot) { return kRcHeaderWords + slot * kRcSlotWords; }

// ---- table ----------------------------------------------------------------------------------------------------

/// The slot holding key k, or kRcInvalid.
RC_FN uint rcFind(RC_CTX_PARAM RcParams R, RcKey k) {
    for (uint i = 0u; i < R.maxProbe; ++i) {
        uint s = (k.slot + i) & (R.capacity - 1u);
        if (rcTableLoad(RC_CTX_ARG rcSlotBase(s)) == k.check) {
            return s;
        }
    }
    return kRcInvalid;
}

/// The slot holding key k, claiming the first empty slot of its probe sequence when absent (kRcInvalid: full).
RC_FN uint rcInsert(RC_CTX_PARAM RcParams R, RcKey k) {
    uint found = rcFind(RC_CTX_ARG R, k);
    if (found != kRcInvalid) {
        return found;
    }
    for (uint i = 0u; i < R.maxProbe; ++i) {
        uint s = (k.slot + i) & (R.capacity - 1u);
        uint prev = rcTableCas(RC_CTX_ARG rcSlotBase(s), 0u, k.check);
        if (prev == 0u) {
            rcTableStore(RC_CTX_ARG rcSlotBase(s) + 6u, k.level | (k.bin << 8u) | 65536u);
            return s;
        }
        if (prev == k.check) {
            return s;
        }
    }
    return kRcInvalid;
}

RC_FN uint rcFixed(float v, float maxRadiance) {
    float c = max(min(v, maxRadiance), 0.0f);
    if (!(c > 0.0f)) {
        return 0u; // also NaN
    }
    return uint(c * kRcFixedScale + 0.5f);
}

/// Accumulates one training record (w0 = position, valid flag; w1 = normal; w2 = radiance) into its cell. Returns 0
/// (invalid record), 1 (accumulated) or 2 (dropped: the probe sequence is full).
RC_FN uint rcUpdateRecord(RC_CTX_PARAM RcParams R, float4 w0, float4 w1, float4 w2) {
    if (!(w0.w > 0.5f)) {
        return 0u;
    }
    RcKey k = rcCellKey(R, float3(w0.x, w0.y, w0.z), float3(w1.x, w1.y, w1.z));
    uint s = rcInsert(RC_CTX_ARG R, k);
    if (s == kRcInvalid) {
        rcTableAdd(RC_CTX_ARG kRcCounterDropped, 1u);
        return 2u;
    }
    uint b = rcSlotBase(s);
    uint n = rcTableAdd(RC_CTX_ARG b + 4u, 1u);
    if (n < kRcMaxFrameSamples) {
        rcTableAdd(RC_CTX_ARG b + 1u, rcFixed(w2.x, R.maxRadiance));
        rcTableAdd(RC_CTX_ARG b + 2u, rcFixed(w2.y, R.maxRadiance));
        rcTableAdd(RC_CTX_ARG b + 3u, rcFixed(w2.z, R.maxRadiance));
    }
    rcTableAdd(RC_CTX_ARG kRcCounterInserts, 1u);
    return 1u;
}

/// Resolves slot s (see FRAME). Returns 0 (empty), 1 (live), 2 (evicted this frame).
RC_FN uint rcResolveSlot(RC_CTX_PARAM RcParams R, uint s) {
    uint b = rcSlotBase(s);
    if (rcTableLoad(RC_CTX_ARG b) == 0u) {
        return 0u;
    }
    uint n = rcTableLoad(RC_CTX_ARG b + 4u);
    if (n > 0u) {
        float m = float(rcMinU(n, kRcMaxFrameSamples));
        float scale = 1.0f / kRcFixedScale;
        float3 sum = float3(float(rcTableLoad(RC_CTX_ARG b + 1u)) * scale, float(rcTableLoad(RC_CTX_ARG b + 2u)) * scale,
                            float(rcTableLoad(RC_CTX_ARG b + 3u)) * scale);
        float oldN = RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 11u));
        float3 old = float3(RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 8u)), RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 9u)),
                            RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 10u)));
        float total = oldN + m;
        float inv = 1.0f / total;
        float3 L = (old * oldN + sum) * inv;
        rcTableStore(RC_CTX_ARG b + 8u, RC_ASUINT(L.x));
        rcTableStore(RC_CTX_ARG b + 9u, RC_ASUINT(L.y));
        rcTableStore(RC_CTX_ARG b + 10u, RC_ASUINT(L.z));
        rcTableStore(RC_CTX_ARG b + 11u, RC_ASUINT(min(total, R.maxSamples)));
        rcTableStore(RC_CTX_ARG b + 1u, 0u);
        rcTableStore(RC_CTX_ARG b + 2u, 0u);
        rcTableStore(RC_CTX_ARG b + 3u, 0u);
        rcTableStore(RC_CTX_ARG b + 4u, 0u);
        rcTableStore(RC_CTX_ARG b + 5u, 0u);
        rcTableAdd(RC_CTX_ARG kRcCounterFresh, 1u);
        rcTableAdd(RC_CTX_ARG kRcCounterLive, 1u);
        return 1u;
    }
    uint age = rcTableLoad(RC_CTX_ARG b + 5u) + 1u;
    if (age > R.maxAge) {
        for (uint i = 0u; i < kRcSlotWords; ++i) {
            rcTableStore(RC_CTX_ARG b + i, 0u);
        }
        rcTableAdd(RC_CTX_ARG kRcCounterEvicted, 1u);
        return 2u;
    }
    rcTableStore(RC_CTX_ARG b + 5u, age);
    rcTableAdd(RC_CTX_ARG kRcCounterLive, 1u);
    return 1u;
}

/// The resolved radiance of the cell of (p, n); false when the cell is absent or has fewer than minSamples samples.
RC_FN bool rcLookup(RC_CTX_PARAM RcParams R, float3 p, float3 n, RC_OUT(float3) L, RC_OUT(float) samples) {
    L = float3(0.0f, 0.0f, 0.0f);
    samples = 0.0f;
    RcKey k = rcCellKey(R, p, n);
    uint s = rcFind(RC_CTX_ARG R, k);
    if (s == kRcInvalid) {
        return false;
    }
    uint b = rcSlotBase(s);
    samples = RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 11u));
    if (!(samples > 0.0f) || samples < R.minSamples) {
        return false;
    }
    L = float3(RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 8u)), RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 9u)),
               RC_ASFLOAT(rcTableLoad(RC_CTX_ARG b + 10u)));
    return true;
}

/// Clears slot s (and, from item 0, the header counters).
RC_FN void rcClearSlot(RC_CTX_PARAM uint s) {
    uint b = rcSlotBase(s);
    for (uint i = 0u; i < kRcSlotWords; ++i) {
        rcTableStore(RC_CTX_ARG b + i, 0u);
    }
}

/// The frame's header (train stage, item 0): parameters (packed words) and zeroed counters.
RC_FN void rcWriteHeader(RC_CTX_PARAM RC_PARAM_WORDS(w)) {
    for (uint k = 0u; k < kRcParamWords; ++k) {
        rcTableStore(RC_CTX_ARG k * 4u, RC_ASUINT(w[k].x));
        rcTableStore(RC_CTX_ARG k * 4u + 1u, RC_ASUINT(w[k].y));
        rcTableStore(RC_CTX_ARG k * 4u + 2u, RC_ASUINT(w[k].z));
        rcTableStore(RC_CTX_ARG k * 4u + 3u, RC_ASUINT(w[k].w));
    }
    for (uint c = kRcCounterQueries; c < kRcHeaderWords; ++c) {
        rcTableStore(RC_CTX_ARG c, 0u);
    }
}

/// The training pixel of tile `tile` this frame (one per trainStride x trainStride tile, chosen by hash).
RC_FN uint rcTrainPixel(RcParams R, uint width, uint height, uint tile, RC_OUT(uint) px, RC_OUT(uint) py) {
    uint s = rcMaxU(R.trainStride, 1u);
    uint tilesX = (width + s - 1u) / s;
    uint tx = tile % tilesX;
    uint ty = tile / tilesX;
    uint o = rcHash(R.frame * 9781u + tile * 6271u + 17u) % (s * s);
    px = rcMinU(tx * s + o % s, width - 1u);
    py = rcMinU(ty * s + o / s, height - 1u);
    uint tilesY = (height + s - 1u) / s;
    return ty < tilesY ? 1u : 0u;
}

/// The tile of pixel (px, py) (training records base: tile x kRcMaxVertices).
RC_FN uint rcTrainTile(RcParams R, uint width, uint px, uint py) {
    uint s = rcMaxU(R.trainStride, 1u);
    uint tilesX = (width + s - 1u) / s;
    return (py / s) * tilesX + (px / s);
}
