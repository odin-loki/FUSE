// FUSE Relight RL-5.6: opacity micromaps - index mapping, conservative build, traversal emulation (see omm.hpp).
#include <fuse/relight/render/pathtrace/omm/omm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace fuse::relight::render::pathtrace::omm {

namespace {

using i64 = std::int64_t;

struct P2 {
    i64 x = 0;
    i64 y = 0;
};

P2 mid(const P2& a, const P2& b) { return P2{(a.x + b.x) / 2, (a.y + b.y) / 2}; }

i64 cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

/// p strictly inside (or on the boundary of) triangle (a, b, c) of either orientation.
bool inside(const P2& p, const P2& a, const P2& b, const P2& c) {
    const i64 d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
    const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

/// The four children of (a, b, c) in curve order: nearest vertex 0, the middle one (flipped), nearest vertex 1,
/// nearest vertex 2 (flipped). Vertex orders follow the curve's orientation.
void children(const P2& a, const P2& b, const P2& c, P2 out[4][3]) {
    const P2 ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
    out[0][0] = a;
    out[0][1] = ab;
    out[0][2] = ca;
    out[1][0] = ca;
    out[1][1] = bc;
    out[1][2] = ab;
    out[2][0] = ab;
    out[2][1] = b;
    out[2][2] = bc;
    out[3][0] = bc;
    out[3][1] = ca;
    out[3][2] = c;
}

/// Index of the micro-triangle containing lattice point p (units: 1 / (3N) of the barycentric domain).
u32 descend(const P2& p, u32 level) {
    const i64 n3 = i64(3) << level;
    P2 a{0, 0}, b{n3, 0}, c{0, n3};
    u32 index = 0;
    for (u32 l = 0; l < level; ++l) {
        P2 ch[4][3];
        children(a, b, c, ch);
        u32 k = 0;
        while (k < 3u && !inside(p, ch[k][0], ch[k][1], ch[k][2])) {
            ++k;
        }
        index = index * 4u + k;
        a = ch[k][0];
        b = ch[k][1];
        c = ch[k][2];
    }
    return index;
}

void microVertices(u32 index, u32 level, P2 out[3]) {
    const i64 n = i64(1) << level;
    P2 a{0, 0}, b{n, 0}, c{0, n};
    for (u32 l = 0; l < level; ++l) {
        const u32 k = (index >> (2u * (level - 1u - l))) & 3u;
        P2 ch[4][3];
        children(a, b, c, ch);
        a = ch[k][0];
        b = ch[k][1];
        c = ch[k][2];
    }
    out[0] = a;
    out[1] = b;
    out[2] = c;
}

u32 wrapIndex(i64 i, u32 n) { return static_cast<u32>(((i % i64(n)) + i64(n)) % i64(n)); }

/// [lo, hi] of the texture alpha anywhere in the UV box (bilinear footprint: every texel a sample in the box can
/// weight). False when the footprint is too large to scan (caller: unknown).
bool footprintRange(const OmmAlphaTexture& tex, float u0, float v0, float u1, float v1, float& lo, float& hi) {
    constexpr float kMargin = 0.01f; // texel units: float rounding of the hit's interpolated UV
    const double sx0 = double(u0) * tex.width - 0.5 - kMargin, sx1 = double(u1) * tex.width - 0.5 + kMargin;
    const double sy0 = double(v0) * tex.height - 0.5 - kMargin, sy1 = double(v1) * tex.height - 0.5 + kMargin;
    const i64 x0 = i64(std::floor(sx0)), x1 = i64(std::floor(sx1)) + 1;
    const i64 y0 = i64(std::floor(sy0)), y1 = i64(std::floor(sy1)) + 1;
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > 65536) {
        return false;
    }
    lo = 1e30f;
    hi = -1e30f;
    for (i64 y = y0; y <= y1; ++y) {
        const u32 ty = wrapIndex(y, tex.height);
        for (i64 x = x0; x <= x1; ++x) {
            const float a = tex.alpha[std::size_t(ty) * tex.width + wrapIndex(x, tex.width)];
            lo = std::min(lo, a);
            hi = std::max(hi, a);
        }
    }
    return true;
}

/// Every alpha in [lo, hi] passes / fails the test (with a margin for the float evaluation of the any-hit path).
void classifyRange(u32 op, float ref, float lo, float hi, bool& allPass, bool& allFail) {
    constexpr float kEps = 1e-5f;
    const float h = 0.5f / 255.0f;
    const float l = lo - kEps, u = hi + kEps;
    allPass = false;
    allFail = false;
    switch (op) {
    case 0u: allFail = true; break;
    case 1u: allPass = u < ref; allFail = l >= ref; break;
    case 2u: allPass = l >= ref - h && u <= ref + h; allFail = u < ref - h || l > ref + h; break;
    case 3u: allPass = u <= ref + h; allFail = l > ref + h; break;
    case 4u: allPass = l > ref + h; allFail = u <= ref + h; break;
    case 5u: allPass = u < ref - h || l > ref + h; allFail = l >= ref - h && u <= ref + h; break;
    case 6u: allPass = l >= ref - h; allFail = u < ref - h; break;
    default: allPass = true; break;
    }
}

float lerpUv(const OmmTriangle& t, int k, float u, float v) {
    return t.uv[0][k] * (1.f - u - v) + t.uv[1][k] * u + t.uv[2][k] * v;
}

} // namespace

u32 ommIndexFromBarycentrics(float u, float v, u32 level) {
    level = std::min(level, kOmmMaxLevel);
    if (level == 0u) {
        return 0u;
    }
    u = std::min(std::max(u, 0.f), 1.f);
    v = std::min(std::max(v, 0.f), 1.f);
    const i64 n = i64(1) << level;
    const float fu = u * float(n), fv = v * float(n);
    const float flu = std::floor(fu), flv = std::floor(fv);
    const float ru = fu - flu, rv = fv - flv;
    i64 iu = std::min(i64(flu), n - 1);
    i64 iv = std::min(i64(flv), n - 1);
    const i64 sum = iu + iv;
    bool upper = false;
    if (sum >= n) {
        iu = n - 1 - iv; // clamp floor(u) so that the cell lies in the triangle
    } else {
        upper = ru + rv >= 1.f && sum < n - 1;
    }
    const P2 p = upper ? P2{3 * iu + 2, 3 * iv + 2} : P2{3 * iu + 1, 3 * iv + 1};
    return descend(p, level);
}

void ommCellOfIndex(u32 index, u32 level, u32& iu, u32& iv, bool& upper) {
    P2 v[3];
    microVertices(index, level, v);
    const i64 sx = v[0].x + v[1].x + v[2].x, sy = v[0].y + v[1].y + v[2].y; // 3 x centroid
    iu = static_cast<u32>(sx / 3);
    iv = static_cast<u32>(sy / 3);
    upper = sx % 3 == 2;
}

void ommMicroTriangle(u32 index, u32 level, float uv[3][2]) {
    P2 v[3];
    microVertices(index, level, v);
    const float inv = 1.f / float(i64(1) << level);
    for (int k = 0; k < 3; ++k) {
        uv[k][0] = float(v[k].x) * inv;
        uv[k][1] = float(v[k].y) * inv;
    }
}

float ommSampleAlpha(const OmmAlphaTexture& tex, float u, float v) {
    if (tex.alpha == nullptr || tex.width == 0u || tex.height == 0u) {
        return 1.f;
    }
    const float x = u * float(tex.width) - 0.5f;
    const float y = v * float(tex.height) - 0.5f;
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float ax = x - fx;
    const float ay = y - fy;
    const u32 x0 = wrapIndex(i64(fx), tex.width), x1 = wrapIndex(i64(fx) + 1, tex.width);
    const u32 y0 = wrapIndex(i64(fy), tex.height), y1 = wrapIndex(i64(fy) + 1, tex.height);
    const float a = tex.alpha[std::size_t(y0) * tex.width + x0];
    const float b = tex.alpha[std::size_t(y0) * tex.width + x1];
    const float c = tex.alpha[std::size_t(y1) * tex.width + x0];
    const float d = tex.alpha[std::size_t(y1) * tex.width + x1];
    const float top = a + (b - a) * ax;
    const float bottom = c + (d - c) * ax;
    return top + (bottom - top) * ay;
}

bool ommAlphaPasses(u32 op, float a, float ref) {
    // VkCompareOp with the RL-5.1 path tracer's half-step tolerance (ptCompare).
    const float h = 0.5f / 255.0f;
    switch (op) {
    case 0u: return false;
    case 1u: return a < ref;
    case 2u: return std::fabs(a - ref) <= h;
    case 3u: return a <= ref + h;
    case 4u: return a > ref + h;
    case 5u: return std::fabs(a - ref) > h;
    case 6u: return a >= ref - h;
    default: return true;
    }
}

bool ommAlphaTestHit(const OmmTriangle& t, const OmmAlphaTexture& tex, const OmmAlphaTest& test, float u, float v) {
    const float tu = lerpUv(t, 0, u, v), tv = lerpUv(t, 1, u, v);
    const float va = t.vertexAlpha[0] * (1.f - u - v) + t.vertexAlpha[1] * u + t.vertexAlpha[2] * v;
    const float alpha = ommSampleAlpha(tex, tu, tv) * va * test.baseAlpha;
    return ommAlphaPasses(test.compare, alpha, test.reference);
}

bool ommBuild(std::span<const OmmTriangle> triangles, const OmmBuildDesc& desc, OmmBuildResult& out) {
    out = OmmBuildResult{};
    const u32 level = std::min(desc.level, kOmmMaxLevel);
    const bool four = desc.format == OmmFormat::FourState;
    const u32 micro = ommMicroTriangleCount(level);
    const u32 bits = four ? 2u : 1u;
    const u32 blockBytes = std::max(1u, (micro * bits + 7u) / 8u);
    OmmAlphaTexture none{};
    const OmmAlphaTexture& tex = desc.texture != nullptr ? *desc.texture : none;
    const bool hasTex = tex.alpha != nullptr && tex.width > 0u && tex.height > 0u;
    std::vector<u8> states(micro);
    std::vector<u8> block(blockBytes);
    std::unordered_map<std::string, u32> shared;
    u32 usageCount = 0;
    out.indices.reserve(triangles.size());
    for (const OmmTriangle& t : triangles) {
        for (u32 m = 0; m < micro; ++m) {
            float b[3][2];
            ommMicroTriangle(m, level, b);
            float uMin = 1e30f, uMax = -1e30f, vMin = 1e30f, vMax = -1e30f, aMin = 1e30f, aMax = -1e30f;
            for (int k = 0; k < 3; ++k) {
                const float tu = lerpUv(t, 0, b[k][0], b[k][1]), tv = lerpUv(t, 1, b[k][0], b[k][1]);
                uMin = std::min(uMin, tu);
                uMax = std::max(uMax, tu);
                vMin = std::min(vMin, tv);
                vMax = std::max(vMax, tv);
                const float va = t.vertexAlpha[0] * (1.f - b[k][0] - b[k][1]) + t.vertexAlpha[1] * b[k][0] +
                                 t.vertexAlpha[2] * b[k][1];
                aMin = std::min(aMin, va);
                aMax = std::max(aMax, va);
            }
            float tLo = 1.f, tHi = 1.f;
            bool known = true;
            if (hasTex) {
                known = footprintRange(tex, uMin, vMin, uMax, vMax, tLo, tHi);
            }
            known = known && tLo >= 0.f && aMin >= 0.f && desc.test.baseAlpha >= 0.f;
            bool allPass = false, allFail = false;
            if (known) {
                const float lo = tLo * aMin * desc.test.baseAlpha;
                const float hi = tHi * aMax * desc.test.baseAlpha;
                classifyRange(desc.test.compare, desc.test.reference, lo, hi, allPass, allFail);
            }
            // Centroid decision (2-state value, unknown-opaque / unknown-transparent).
            const float cu = (b[0][0] + b[1][0] + b[2][0]) * (1.f / 3.f);
            const float cv = (b[0][1] + b[1][1] + b[2][1]) * (1.f / 3.f);
            const bool centroid = ommAlphaTestHit(t, tex, desc.test, cu, cv);
            u8 s;
            if (allPass) {
                s = kOmmOpaque;
                ++out.stats.microOpaque;
            } else if (allFail) {
                s = kOmmTransparent;
                ++out.stats.microTransparent;
            } else {
                ++out.stats.microUnknown;
                s = four ? (centroid ? kOmmUnknownOpaque : kOmmUnknownTransparent) : (centroid ? kOmmOpaque : kOmmTransparent);
            }
            states[m] = s;
        }
        bool uniform = true;
        for (u32 m = 1; m < micro && uniform; ++m) {
            uniform = states[m] == states[0];
        }
        if (desc.useSpecialIndices && uniform) {
            out.indices.push_back(-i32(states[0]) - 1);
            ++out.stats.specialTriangles;
            continue;
        }
        std::fill(block.begin(), block.end(), u8{0});
        for (u32 m = 0; m < micro; ++m) {
            const u32 bit = m * bits;
            block[bit / 8u] = static_cast<u8>(block[bit / 8u] | (states[m] << (bit % 8u)));
        }
        if (desc.deduplicate) {
            const std::string key(reinterpret_cast<const char*>(block.data()), block.size());
            const auto it = shared.find(key);
            if (it != shared.end()) {
                out.indices.push_back(i32(it->second));
                ++out.stats.sharedBlocks;
                continue;
            }
            shared.emplace(key, static_cast<u32>(out.records.size()));
        }
        OmmTriangleRecord r;
        r.dataOffset = static_cast<u32>(out.data.size());
        r.subdivisionLevel = static_cast<u16>(level);
        r.format = static_cast<u16>(desc.format);
        out.data.insert(out.data.end(), block.begin(), block.end());
        out.indices.push_back(i32(out.records.size()));
        out.records.push_back(r);
        ++usageCount;
    }
    out.stats.uniqueBlocks = usageCount;
    if (usageCount > 0u) {
        out.usage.push_back(OmmUsage{usageCount, level, static_cast<u32>(desc.format)});
    }
    return true;
}

OmmHit ommLookup(const OmmBuildResult& omm, u32 triangle, float u, float v, bool force2State) {
    if (triangle >= omm.indices.size()) {
        return OmmHit::NonOpaque;
    }
    const i32 index = omm.indices[triangle];
    u32 state = 0;
    bool four = true;
    if (index < 0) {
        state = static_cast<u32>(-index - 1);
    } else {
        const OmmTriangleRecord& r = omm.records[static_cast<u32>(index)];
        four = r.format == static_cast<u16>(OmmFormat::FourState);
        const u32 bits = four ? 2u : 1u;
        const u32 m = ommIndexFromBarycentrics(u, v, r.subdivisionLevel);
        const u32 bit = m * bits;
        state = (omm.data[r.dataOffset + bit / 8u] >> (bit % 8u)) & (four ? 3u : 1u);
    }
    if (state == kOmmTransparent) {
        return OmmHit::Ignored;
    }
    if (state == kOmmOpaque) {
        return OmmHit::Opaque;
    }
    if (force2State) {
        return state == kOmmUnknownOpaque ? OmmHit::Opaque : OmmHit::Ignored;
    }
    return OmmHit::NonOpaque;
}

bool ommResolveHit(const OmmBuildResult& omm, u32 triangle, float u, float v, const OmmTriangle& t,
                   const OmmAlphaTexture* tex, const OmmAlphaTest& test, u32* anyHit) {
    const OmmHit h = ommLookup(omm, triangle, u, v, false);
    if (h != OmmHit::NonOpaque) {
        return h == OmmHit::Opaque;
    }
    if (anyHit != nullptr) {
        ++*anyHit;
    }
    OmmAlphaTexture none{};
    return ommAlphaTestHit(t, tex != nullptr ? *tex : none, test, u, v);
}

OmmPath ommSelectPath(const OmmDeviceCaps& caps, const OmmBuildDesc& desc) {
    const u32 maxLevel = desc.format == OmmFormat::FourState ? caps.max4StateLevel : caps.max2StateLevel;
    return caps.extension && caps.micromap && desc.level <= maxLevel ? OmmPath::Hardware : OmmPath::AnyHitFallback;
}

} // namespace fuse::relight::render::pathtrace::omm
