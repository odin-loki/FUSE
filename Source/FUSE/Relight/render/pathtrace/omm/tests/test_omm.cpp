// FUSE Relight RL-5.6 opacity micromap gates (omm.hpp). Suites (argv[1]; "all" runs every one):
//
//   index        the VK_EXT_opacity_micromap micro-triangle ordering: for levels 0..8 every micro-triangle centroid maps
//                to a distinct index in [0, 4^level) and ommCellOfIndex inverts it; the level-3 order equals the golden
//                table (tabulated from the specification's ordering); consecutive micro-triangles share a vertex (a
//                continuous space-filling curve); level 1 visits the corner of vertex 0, the middle, vertex 1, vertex 2; the index
//                is hierarchical (level L >> 2 == level L - 1); edge / corner / out-of-range barycentrics stay in range.
//   build        special indices for uniform triangles, 1- / 2-bit data blocks rounded up to bytes, usage counts, shared
//                blocks, the conservative classification (no opaque / transparent micro-triangle contains a point
//                that the alpha test decides the other way, on a dense sample).
//   equivalence  4-state micromap + any-hit fallback == the plain alpha test on every sampled hit (bit for bit), for
//                levels 0..6 and every VkCompareOp; the any-hit calls saved; the 2-state (lossy) mismatch reported.
//   traversal    closest hit through stacked alpha-tested quads (brute-force traversal emulation): the micromap path
//                returns the same triangle and t as the alpha-test path for every ray.
//   zero_alloc   lookups and resolves make no heap allocation.
#include <fuse/relight/render/pathtrace/omm/omm.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace {
std::atomic<bool> g_count{false};
std::atomic<unsigned long long> g_allocations{0};
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (g_count.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

namespace om = fuse::relight::render::pathtrace::omm;
using fuse::i32;
using fuse::u32;
using fuse::u64;
using fuse::u8;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

struct Rng {
    u32 s = 1u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) * (1.f / 16777216.f);
    }
};

// ---- index ----------------------------------------------------------------------------------------------------------

/// Level-3 order: (iu, iv, upper) of micro-triangle 0..63, tabulated from the specification's micromap ordering.
constexpr u8 kGolden3[64][3] = {
    {0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 1, 1}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}, {2, 0, 0}, {2, 0, 1},
    {3, 0, 0}, {2, 1, 0}, {1, 2, 0}, {0, 2, 1}, {0, 2, 0}, {0, 3, 0}, {0, 3, 1}, {1, 3, 0}, {1, 3, 1}, {1, 2, 1},
    {2, 2, 0}, {2, 2, 1}, {3, 2, 0}, {2, 3, 0}, {2, 3, 1}, {3, 3, 0}, {3, 3, 1}, {3, 2, 1}, {3, 1, 1}, {3, 1, 0},
    {2, 1, 1}, {3, 0, 1}, {4, 0, 0}, {4, 0, 1}, {5, 0, 0}, {4, 1, 0}, {4, 1, 1}, {5, 1, 0}, {5, 1, 1}, {5, 0, 1},
    {6, 0, 0}, {6, 0, 1}, {7, 0, 0}, {6, 1, 0}, {5, 2, 0}, {4, 2, 1}, {4, 2, 0}, {4, 3, 0}, {3, 4, 0}, {2, 4, 1},
    {2, 4, 0}, {2, 5, 0}, {1, 5, 1}, {1, 5, 0}, {0, 5, 1}, {1, 4, 1}, {1, 4, 0}, {0, 4, 1}, {0, 4, 0}, {0, 5, 0},
    {0, 6, 0}, {0, 6, 1}, {1, 6, 0}, {0, 7, 0}};

void suiteIndex() {
    bool bijective = true, inverse = true, continuous = true, hierarchical = true;
    for (u32 level = 0; level <= 8u; ++level) {
        const u32 n = 1u << level;
        const u32 count = om::ommMicroTriangleCount(level);
        std::vector<u8> seen(count, 0u);
        for (u32 iv = 0; iv < n; ++iv) {
            for (u32 iu = 0; iu + iv < n; ++iu) {
                for (u32 up = 0; up < (iu + iv + 1u < n ? 2u : 1u); ++up) {
                    const float o = up != 0u ? 2.f / 3.f : 1.f / 3.f;
                    const float u = (float(iu) + o) / float(n), v = (float(iv) + o) / float(n);
                    const u32 idx = om::ommIndexFromBarycentrics(u, v, level);
                    if (idx >= count || seen[idx] != 0u) {
                        bijective = false;
                        continue;
                    }
                    seen[idx] = 1u;
                    u32 cu = 0, cv = 0;
                    bool cup = false;
                    om::ommCellOfIndex(idx, level, cu, cv, cup);
                    inverse = inverse && cu == iu && cv == iv && cup == (up != 0u);
                    if (level > 0u) {
                        hierarchical = hierarchical && (idx >> 2u) == om::ommIndexFromBarycentrics(u, v, level - 1u);
                    }
                }
            }
        }
        for (u32 i = 0; i + 1u < count; ++i) {
            float a[3][2], b[3][2];
            om::ommMicroTriangle(i, level, a);
            om::ommMicroTriangle(i + 1u, level, b);
            u32 shared = 0;
            for (const auto& p : a) {
                for (const auto& q : b) {
                    shared += p[0] == q[0] && p[1] == q[1] ? 1u : 0u;
                }
            }
            continuous = continuous && shared >= 1u;
        }
    }
    bool golden = true;
    for (u32 i = 0; i < 64u; ++i) {
        u32 iu = 0, iv = 0;
        bool up = false;
        om::ommCellOfIndex(i, 3u, iu, iv, up);
        golden = golden && iu == kGolden3[i][0] && iv == kGolden3[i][1] && up == (kGolden3[i][2] != 0u);
        const float o = up ? 2.f / 3.f : 1.f / 3.f;
        golden = golden && om::ommIndexFromBarycentrics((iu + o) / 8.f, (iv + o) / 8.f, 3u) == i;
    }
    // Level 1 in the prose order: near vertex 0, middle, near vertex 1 (u = 1), near vertex 2 (v = 1).
    const bool prose = om::ommIndexFromBarycentrics(0.1f, 0.1f, 1u) == 0u &&
                       om::ommIndexFromBarycentrics(0.3f, 0.3f, 1u) == 1u &&
                       om::ommIndexFromBarycentrics(0.8f, 0.1f, 1u) == 2u &&
                       om::ommIndexFromBarycentrics(0.1f, 0.8f, 1u) == 3u;
    bool edges = true;
    const float probes[][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}, {0.5f, 0.5f}, {1.f, 1.f}, {-0.5f, 2.f},
                               {0.25f, 0.75f}, {0.999999f, 0.f}, {0.f, 0.999999f}, {0.5f, 0.5000001f}};
    for (u32 level = 0; level <= om::kOmmMaxLevel; ++level) {
        for (const auto& p : probes) {
            edges = edges && om::ommIndexFromBarycentrics(p[0], p[1], level) < om::ommMicroTriangleCount(level);
        }
    }
    std::printf("index: levels 0..8 bijective %d, inverse %d, curve continuous (shared vertex) %d, hierarchical %d; level-3 golden %d; "
                "level-1 prose order %d; edge probes in range %d\n",
                bijective, inverse, continuous, hierarchical, golden, prose, edges);
    check(bijective && inverse, "index: centroid -> index is a bijection onto [0, 4^level), inverted by ommCellOfIndex");
    check(continuous, "index: consecutive micro-triangles share a vertex (continuous curve)");
    check(hierarchical, "index: level L >> 2 == level L - 1");
    check(golden, "index: level-3 order == the specification's ordering");
    check(prose, "index: level 1 = vertex 0, middle, vertex 1, vertex 2");
    check(edges, "index: edge / corner / out-of-range barycentrics stay in range");
}

// ---- scenes ---------------------------------------------------------------------------------------------------------

/// A foliage-like alpha texture: soft-edged blobs on transparent ground, a hard-edged stripe and a gradient band.
std::vector<float> makeTexture(u32 w, u32 h) {
    std::vector<float> a(std::size_t(w) * h, 0.f);
    const float blobs[6][3] = {{0.2f, 0.3f, 0.15f}, {0.7f, 0.25f, 0.2f}, {0.45f, 0.7f, 0.18f},
                               {0.85f, 0.8f, 0.1f}, {0.1f, 0.85f, 0.08f}, {0.55f, 0.45f, 0.06f}};
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float al = 0.f;
            for (const auto& b : blobs) {
                const float d = std::sqrt((u - b[0]) * (u - b[0]) + (v - b[1]) * (v - b[1]));
                al = std::max(al, std::min(1.f, std::max(0.f, (b[2] - d) / (0.3f * b[2]))));
            }
            if (x % 16u == 3u) {
                al = 1.f;
            }
            if (y >= h - 6u) {
                al = float(x) / float(w - 1u);
            }
            a[std::size_t(y) * w + x] = al;
        }
    }
    return a;
}

std::vector<om::OmmTriangle> makeTriangles(u32 count, Rng& r) {
    std::vector<om::OmmTriangle> t(count);
    for (u32 i = 0; i < count; ++i) {
        const float ou = r.next() * 2.f - 0.5f, ov = r.next() * 2.f - 0.5f, s = 0.05f + r.next() * 1.2f;
        for (int k = 0; k < 3; ++k) {
            t[i].uv[k][0] = ou + s * (r.next() - 0.5f) * 2.f;
            t[i].uv[k][1] = ov + s * (r.next() - 0.5f) * 2.f;
            t[i].vertexAlpha[k] = i % 3u == 0u ? 0.5f + 0.5f * r.next() : 1.f;
        }
    }
    // Uniform triangles: inside a fully opaque blob core and in the transparent ground.
    om::OmmTriangle opaque{}; // the core of the first blob (alpha 1 over the whole bilinear footprint)
    for (int k = 0; k < 3; ++k) {
        opaque.uv[k][0] = 0.2f + 0.005f * k;
        opaque.uv[k][1] = 0.3f + 0.005f * (2 - k);
    }
    t.push_back(opaque);
    om::OmmTriangle clear{};
    for (int k = 0; k < 3; ++k) {
        clear.uv[k][0] = 0.97f + 0.002f * k;
        clear.uv[k][1] = 0.55f + 0.002f * k;
    }
    t.push_back(clear);
    return t;
}

// ---- build ----------------------------------------------------------------------------------------------------------

void suiteBuild() {
    const u32 tw = 64, th = 64;
    const std::vector<float> alpha = makeTexture(tw, th);
    const om::OmmAlphaTexture tex{tw, th, alpha.data()};
    Rng r{7u};
    const std::vector<om::OmmTriangle> tris = makeTriangles(200, r);
    bool layout = true, conservative = true;
    u32 specials = 0, shared = 0;
    for (u32 fmt = 1; fmt <= 2u; ++fmt) {
        for (u32 level : {0u, 1u, 3u, 5u}) {
            om::OmmBuildDesc d;
            d.level = level;
            d.format = static_cast<om::OmmFormat>(fmt);
            d.texture = &tex;
            om::OmmBuildResult res;
            check(om::ommBuild(tris, d, res), "build: ommBuild");
            const u32 bits = fmt == 2u ? 2u : 1u;
            const u32 bytes = std::max(1u, (om::ommMicroTriangleCount(level) * bits + 7u) / 8u);
            layout = layout && res.indices.size() == tris.size() && res.data.size() == res.records.size() * bytes;
            u32 used = 0;
            for (const om::OmmUsage& u : res.usage) {
                used += u.count;
                layout = layout && u.subdivisionLevel == level && u.format == fmt;
            }
            layout = layout && used == res.records.size();
            for (std::size_t i = 0; i < res.records.size(); ++i) {
                layout = layout && res.records[i].dataOffset == i * bytes && res.records[i].subdivisionLevel == level &&
                         res.records[i].format == fmt;
            }
            specials += res.stats.specialTriangles;
            shared += res.stats.sharedBlocks;
            // The two uniform triangles at the end use the special indices.
            layout = layout && res.indices[tris.size() - 2u] == om::kOmmSpecialFullyOpaque &&
                     res.indices[tris.size() - 1u] == om::kOmmSpecialFullyTransparent;
            if (fmt == 2u) {
                // Conservative: dense samples inside definite micro-triangles agree with the alpha test.
                for (u32 t = 0; t < tris.size(); ++t) {
                    for (u32 s = 0; s < 64u; ++s) {
                        float u = r.next(), v = r.next();
                        if (u + v > 1.f) {
                            u = 1.f - u;
                            v = 1.f - v;
                        }
                        const om::OmmHit h = om::ommLookup(res, t, u, v);
                        if (h == om::OmmHit::NonOpaque) {
                            continue;
                        }
                        const bool test = om::ommAlphaTestHit(tris[t], tex, d.test, u, v);
                        conservative = conservative && (h == om::OmmHit::Opaque) == test;
                    }
                }
            }
        }
    }
    std::printf("build: layout %d (VkMicromapTriangleEXT records, 1- / 2-bit blocks, usage counts, per-triangle indices), "
                "special-index triangles %u, shared blocks %u, conservative %d\n",
                layout, specials, shared, conservative);
    check(layout, "build: data layout / usage / special indices");
    check(shared > 0u, "build: identical blocks are shared");
    check(conservative, "build: definite micro-triangles never contradict the alpha test");
}

// ---- equivalence ----------------------------------------------------------------------------------------------------

void suiteEquivalence() {
    const u32 tw = 64, th = 64;
    const std::vector<float> alpha = makeTexture(tw, th);
    const om::OmmAlphaTexture tex{tw, th, alpha.data()};
    Rng r{99u};
    const std::vector<om::OmmTriangle> tris = makeTriangles(120, r);
    u64 samples = 0, mismatches = 0, anyHit = 0, lossy = 0, lossySamples = 0;
    for (u32 op = 0; op < 8u; ++op) {
        for (u32 level = 0; level <= 6u; ++level) {
            om::OmmBuildDesc d;
            d.level = level;
            d.test.compare = op;
            d.test.reference = 0.3f + 0.1f * float(op % 4u);
            d.texture = &tex;
            om::OmmBuildResult four;
            om::ommBuild(tris, d, four);
            d.format = om::OmmFormat::TwoState;
            om::OmmBuildResult two;
            om::ommBuild(tris, d, two);
            for (u32 t = 0; t < tris.size(); ++t) {
                for (u32 s = 0; s < 48u; ++s) {
                    float u = r.next(), v = r.next();
                    if (u + v > 1.f) {
                        u = 1.f - u;
                        v = 1.f - v;
                    }
                    if (s < 4u) { // vertices and edges
                        u = s == 1u ? 1.f : (s == 3u ? 0.5f : 0.f);
                        v = s == 2u ? 1.f : (s == 3u ? 0.5f : 0.f);
                    }
                    const bool ref = om::ommAlphaTestHit(tris[t], tex, d.test, u, v);
                    u32 calls = 0;
                    const bool got = om::ommResolveHit(four, t, u, v, tris[t], &tex, d.test, &calls);
                    mismatches += got != ref ? 1u : 0u;
                    anyHit += calls;
                    ++samples;
                    const om::OmmHit h2 = om::ommLookup(two, t, u, v);
                    lossy += (h2 == om::OmmHit::Opaque) != ref ? 1u : 0u;
                    ++lossySamples;
                }
            }
        }
    }
    std::printf("equivalence: %llu hits over 8 compare ops x levels 0..6: 4-state + any-hit mismatches %llu, any-hit "
                "calls %.1f%% of candidates; 2-state (no any-hit) disagrees on %.2f%%\n",
                (unsigned long long)samples, (unsigned long long)mismatches, 100.0 * double(anyHit) / double(samples),
                100.0 * double(lossy) / double(lossySamples));
    check(mismatches == 0u, "equivalence: 4-state micromap + any-hit == alpha test, bit for bit");
    check(anyHit * 2u < samples, "equivalence: the micromap resolves most candidates without the any-hit test");
}

// ---- traversal ------------------------------------------------------------------------------------------------------

struct Tri3 {
    float p[3][3];
};

/// Moller-Trumbore (float): the candidate intersection a traversal reports (t, barycentrics u, v).
bool intersect(const Tri3& t, const float o[3], const float d[3], float& tt, float& u, float& v) {
    const float e1[3] = {t.p[1][0] - t.p[0][0], t.p[1][1] - t.p[0][1], t.p[1][2] - t.p[0][2]};
    const float e2[3] = {t.p[2][0] - t.p[0][0], t.p[2][1] - t.p[0][1], t.p[2][2] - t.p[0][2]};
    const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const float inv = 1.f / det;
    const float tv[3] = {o[0] - t.p[0][0], o[1] - t.p[0][1], o[2] - t.p[0][2]};
    u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (u < 0.f || u > 1.f) {
        return false;
    }
    const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
    v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
    if (v < 0.f || u + v > 1.f) {
        return false;
    }
    tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    return tt > 0.f;
}

void suiteTraversal() {
    const u32 tw = 64, th = 64;
    const std::vector<float> alpha = makeTexture(tw, th);
    const om::OmmAlphaTexture tex{tw, th, alpha.data()};
    // 12 stacked quads (24 triangles), each with its own UV transform (the repeat wraps).
    std::vector<Tri3> geo;
    std::vector<om::OmmTriangle> tris;
    Rng r{3u};
    for (u32 q = 0; q < 12u; ++q) {
        const float z = 0.2f * float(q);
        const float ou = r.next(), ov = r.next(), s = 0.6f + r.next() * 1.5f;
        const float c[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
        const u32 idx[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& tri : idx) {
            Tri3 g{};
            om::OmmTriangle t{};
            for (int k = 0; k < 3; ++k) {
                g.p[k][0] = c[tri[k]][0];
                g.p[k][1] = c[tri[k]][1];
                g.p[k][2] = z;
                t.uv[k][0] = ou + s * (c[tri[k]][0] * 0.5f + 0.5f);
                t.uv[k][1] = ov + s * (0.5f - c[tri[k]][1] * 0.5f);
            }
            geo.push_back(g);
            tris.push_back(t);
        }
    }
    om::OmmBuildDesc d;
    d.level = 7;
    d.texture = &tex;
    om::OmmBuildResult res;
    om::ommBuild(tris, d, res);
    u32 rays = 0, same = 0, hitsRef = 0;
    u64 anyHitRef = 0, anyHitOmm = 0;
    for (u32 i = 0; i < 20000u; ++i) {
        const float o[3] = {(r.next() - 0.5f) * 1.8f, (r.next() - 0.5f) * 1.8f, -1.f};
        float dvec[3] = {(r.next() - 0.5f) * 0.4f, (r.next() - 0.5f) * 0.4f, 1.f};
        u32 bestRef = ~0u, bestOmm = ~0u;
        float tRef = 1e30f, tOmm = 1e30f;
        for (u32 t = 0; t < geo.size(); ++t) {
            float tt = 0.f, u = 0.f, v = 0.f;
            if (!intersect(geo[t], o, dvec, tt, u, v)) {
                continue;
            }
            if (tt < tRef) {
                ++anyHitRef;
                if (om::ommAlphaTestHit(tris[t], tex, d.test, u, v)) {
                    tRef = tt;
                    bestRef = t;
                }
            }
            if (tt < tOmm) {
                u32 calls = 0;
                if (om::ommResolveHit(res, t, u, v, tris[t], &tex, d.test, &calls)) {
                    tOmm = tt;
                    bestOmm = t;
                }
                anyHitOmm += calls;
            }
        }
        ++rays;
        hitsRef += bestRef != ~0u ? 1u : 0u;
        same += bestRef == bestOmm && std::memcmp(&tRef, &tOmm, sizeof(float)) == 0 ? 1u : 0u;
    }
    std::printf("traversal: %u rays through 24 alpha-tested triangles (%u hit): identical closest hit %u/%u; any-hit "
                "calls %llu (alpha-test path) -> %llu (micromap path)\n",
                rays, hitsRef, same, rays, (unsigned long long)anyHitRef, (unsigned long long)anyHitOmm);
    check(same == rays, "traversal: micromap path == alpha-test path (triangle and t, bit for bit)");
    check(hitsRef > rays / 4u && hitsRef < rays, "traversal: the scene has both hits and misses");
    check(anyHitOmm * 2u < anyHitRef, "traversal: the micromap saves most any-hit calls");
}

// ---- zero_alloc -----------------------------------------------------------------------------------------------------

void suiteZeroAlloc() {
    const u32 tw = 32, th = 32;
    const std::vector<float> alpha = makeTexture(tw, th);
    const om::OmmAlphaTexture tex{tw, th, alpha.data()};
    Rng r{5u};
    const std::vector<om::OmmTriangle> tris = makeTriangles(32, r);
    om::OmmBuildDesc d;
    d.texture = &tex;
    om::OmmBuildResult res;
    om::ommBuild(tris, d, res);
    u32 accepted = 0;
    g_allocations = 0;
    g_count = true;
    for (u32 i = 0; i < 100000u; ++i) {
        const u32 t = i % static_cast<u32>(tris.size());
        float u = r.next(), v = r.next();
        if (u + v > 1.f) {
            u = 1.f - u;
            v = 1.f - v;
        }
        accepted += om::ommResolveHit(res, t, u, v, tris[t], &tex, d.test) ? 1u : 0u;
    }
    g_count = false;
    std::printf("zero_alloc: 100000 lookups (%u accepted): %llu operator-new calls\n", accepted,
                (unsigned long long)g_allocations.load());
    check(g_allocations.load() == 0u, "zero_alloc: lookups make no heap allocation");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"index", suiteIndex},         {"build", suiteBuild},         {"equivalence", suiteEquivalence},
                            {"traversal", suiteTraversal}, {"zero_alloc", suiteZeroAlloc}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            s.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_omm %s\n", suite.c_str());
    return 0;
}
