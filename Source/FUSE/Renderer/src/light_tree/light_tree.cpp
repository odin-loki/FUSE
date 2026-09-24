// WP-7.1 light tree: CPU build, refit and the GpuScene adapter. See include/fuse/renderer/light_tree/light_tree.hpp.
#include <fuse/renderer/light_tree/light_tree.hpp>

#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/geometry/meshlet_types.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#include <fuse/renderer/material/material.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace fuse::renderer::light_tree {

namespace {

constexpr f64 kPi = 3.14159265358979323846;
/// Angular padding added to every interior node's orientation cone (radians): the unions run in double but
/// the sampler reads float axes, so the stored cone must still contain every child's normals.
constexpr f64 kConePad = 1e-5;

std::atomic<u64> g_version{0};

f64 clamp1(f64 v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }
f64 dot3(const f64* a, const f64* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const f64* a, const f64* b, f64* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
f64 length3(const f64* a) { return std::sqrt(dot3(a, a)); }
bool normalize3(f64* a) {
    const f64 len = length3(a);
    if (!(len > 1e-30)) {
        return false;
    }
    a[0] /= len;
    a[1] /= len;
    a[2] /= len;
    return true;
}
void toDouble(const f32* in, f64* out) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
}

/// Largest float <= v / smallest float >= v.
f32 floatDown(f64 v) {
    f32 f = static_cast<f32>(v);
    if (static_cast<f64>(f) > v) {
        f = std::nextafter(f, -INFINITY);
    }
    return f;
}
f32 floatUp(f64 v) {
    f32 f = static_cast<f32>(v);
    if (static_cast<f64>(f) < v) {
        f = std::nextafter(f, INFINITY);
    }
    return f;
}

u32 ceilLog2(u32 n) {
    u32 r = 0;
    while ((1ull << r) < n) {
        ++r;
    }
    return r;
}

void setVec(f32* dst, f32 x, f32 y, f32 z) {
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
}

} // namespace

// --- light constructors -------------------------------------------------------------------------------

LightTreeLight makePointLight(const f32 (&position)[3], f32 intensity, u32 source) {
    LightTreeLight l{};
    l.kind = kLtKindPoint;
    std::memcpy(l.position, position, sizeof(l.position));
    l.intensity = intensity;
    l.source = source;
    return l;
}

LightTreeLight makeSpotLight(const f32 (&position)[3], const f32 (&axis)[3], f32 cosInner, f32 cosOuter, f32 intensity,
                             u32 source) {
    LightTreeLight l = makePointLight(position, intensity, source);
    l.kind = kLtKindSpot;
    std::memcpy(l.direction, axis, sizeof(l.direction));
    l.cosInner = cosInner;
    l.cosOuter = cosOuter;
    return l;
}

namespace {
LightTreeLight areaLight(u32 kind, const f32 (&p)[3], const f32 (&a)[3], const f32 (&b)[3], f32 radiance, bool twoSided,
                         u32 source) {
    LightTreeLight l{};
    l.kind = kind;
    std::memcpy(l.position, p, sizeof(l.position));
    std::memcpy(l.u, a, sizeof(l.u));
    std::memcpy(l.v, b, sizeof(l.v));
    l.intensity = radiance;
    l.twoSided = twoSided;
    l.source = source;
    return l;
}
} // namespace

LightTreeLight makeRectLight(const f32 (&center)[3], const f32 (&halfU)[3], const f32 (&halfV)[3], f32 radiance,
                             bool twoSided, u32 source) {
    return areaLight(kLtKindRect, center, halfU, halfV, radiance, twoSided, source);
}

LightTreeLight makeDiskLight(const f32 (&center)[3], const f32 (&radiusU)[3], const f32 (&radiusV)[3], f32 radiance,
                             bool twoSided, u32 source) {
    return areaLight(kLtKindDisk, center, radiusU, radiusV, radiance, twoSided, source);
}

LightTreeLight makeTriangleLight(const f32 (&v0)[3], const f32 (&v1)[3], const f32 (&v2)[3], f32 radiance, bool twoSided,
                                 u32 source) {
    return areaLight(kLtKindTriangle, v0, v1, v2, radiance, twoSided, source);
}

LightTreeLight makeDirectionalLight(const f32 (&direction)[3], f32 irradiance, u32 source) {
    LightTreeLight l{};
    l.kind = kLtKindDirectional;
    std::memcpy(l.direction, direction, sizeof(l.direction));
    l.intensity = irradiance;
    l.source = source;
    return l;
}

LightTreeEmitter makeEmitter(const LightTreeLight& light) {
    LightTreeEmitter e{};
    e.kind = light.kind;
    e.source = light.source;
    e.flags = light.twoSided ? static_cast<u32>(kLtFlagTwoSided) : 0u;
    const f32 intensity = light.intensity > 0.f ? light.intensity : 0.f;
    f64 n[3] = {0.0, 0.0, 1.0};
    switch (light.kind) {
    case kLtKindPoint:
        std::memcpy(e.p0, light.position, sizeof(e.p0));
        e.power = intensity;
        e.flags = 0u;
        break;
    case kLtKindSpot:
        std::memcpy(e.p0, light.position, sizeof(e.p0));
        toDouble(light.direction, n);
        if (!normalize3(n)) {
            n[0] = 0.0;
            n[1] = 0.0;
            n[2] = -1.0;
        }
        e.power = intensity;
        e.flags = 0u;
        break;
    case kLtKindRect:
    case kLtKindDisk: {
        std::memcpy(e.p0, light.position, sizeof(e.p0));
        std::memcpy(e.e1, light.u, sizeof(e.e1));
        std::memcpy(e.e2, light.v, sizeof(e.e2));
        f64 a[3], b[3];
        toDouble(light.u, a);
        toDouble(light.v, b);
        cross3(a, b, n);
        const f64 len = length3(n);
        e.area = static_cast<f32>(light.kind == kLtKindRect ? 4.0 * len : kPi * len);
        if (!normalize3(n)) {
            n[0] = 0.0;
            n[1] = 0.0;
            n[2] = 1.0;
        }
        e.power = intensity * e.area;
        break;
    }
    case kLtKindTriangle: {
        std::memcpy(e.p0, light.position, sizeof(e.p0));
        for (u32 i = 0; i < 3u; ++i) {
            e.e1[i] = light.u[i] - light.position[i];
            e.e2[i] = light.v[i] - light.position[i];
        }
        f64 a[3], b[3];
        toDouble(e.e1, a);
        toDouble(e.e2, b);
        cross3(a, b, n);
        e.area = static_cast<f32>(0.5 * length3(n));
        if (!normalize3(n)) {
            n[0] = 0.0;
            n[1] = 0.0;
            n[2] = 1.0;
        }
        e.power = intensity * e.area;
        break;
    }
    case kLtKindDirectional:
        toDouble(light.direction, n);
        if (!normalize3(n)) {
            n[0] = 0.0;
            n[1] = 0.0;
            n[2] = -1.0;
        }
        e.power = intensity;
        e.flags = 0u;
        break;
    default:
        e.kind = kLtKindNone;
        e.flags = 0u;
        break;
    }
    setVec(e.normal, static_cast<f32>(n[0]), static_cast<f32>(n[1]), static_cast<f32>(n[2]));
    return e;
}

// --- bounds ----------------------------------------------------------------------------------------------

LightTree::Bounds LightTree::leafBounds(const LightTreeLight& light, const LightTreeEmitter& e) {
    Bounds b{};
    b.empty = false;
    b.phi = e.power;
    toDouble(e.normal, b.axis);
    normalize3(b.axis);
    f64 p[3];
    toDouble(e.p0, p);
    for (u32 i = 0; i < 3u; ++i) {
        b.lo[i] = b.hi[i] = p[i];
    }
    switch (e.kind) {
    case kLtKindPoint:
        b.axis[0] = 0.0;
        b.axis[1] = 0.0;
        b.axis[2] = 1.0;
        b.cosO = -1.0;
        b.cosE = 0.0; // cos(pi / 2)
        break;
    case kLtKindSpot: {
        const f64 inner = std::acos(clamp1(light.cosInner));
        const f64 outer = std::max(inner, std::acos(clamp1(light.cosOuter)));
        b.cosO = std::cos(inner);
        b.cosE = std::cos(std::min(outer - inner, kPi));
        break;
    }
    case kLtKindRect:
    case kLtKindDisk:
    case kLtKindTriangle: {
        f64 a[3], c[3];
        toDouble(e.e1, a);
        toDouble(e.e2, c);
        for (u32 i = 0; i < 3u; ++i) {
            f64 lo = p[i];
            f64 hi = p[i];
            if (e.kind == kLtKindRect) {
                const f64 ext = std::fabs(a[i]) + std::fabs(c[i]);
                lo = p[i] - ext;
                hi = p[i] + ext;
            } else if (e.kind == kLtKindDisk) {
                const f64 ext = std::sqrt(a[i] * a[i] + c[i] * c[i]);
                lo = p[i] - ext;
                hi = p[i] + ext;
            } else {
                // The float vertices themselves (e1 / e2 were rounded; v0 + e1 need not be v1 exactly).
                const f64 v1 = static_cast<f64>(light.u[i]);
                const f64 v2 = static_cast<f64>(light.v[i]);
                const f64 w1 = p[i] + a[i];
                const f64 w2 = p[i] + c[i];
                lo = std::min({p[i], v1, v2, w1, w2});
                hi = std::max({p[i], v1, v2, w1, w2});
            }
            b.lo[i] = lo;
            b.hi[i] = hi;
        }
        b.cosO = 1.0;
        b.cosE = 0.0;
        b.twoSided = (e.flags & kLtFlagTwoSided) != 0u;
        break;
    }
    default:
        b.phi = 0.0;
        b.cosO = -1.0;
        b.cosE = 0.0;
        break;
    }
    return b;
}

void LightTree::unite(Bounds& a, const Bounds& b) {
    if (b.empty) {
        return;
    }
    if (a.empty) {
        a = b;
        return;
    }
    for (u32 i = 0; i < 3u; ++i) {
        a.lo[i] = std::min(a.lo[i], b.lo[i]);
        a.hi[i] = std::max(a.hi[i], b.hi[i]);
    }
    a.phi += b.phi;
    a.cosE = std::min(a.cosE, b.cosE);
    a.twoSided = a.twoSided || b.twoSided;
    // Direction cone union (PBRT-v4 DirectionCone Union).
    const f64 thetaA = std::acos(clamp1(a.cosO));
    const f64 thetaB = std::acos(clamp1(b.cosO));
    const f64 thetaD = std::acos(clamp1(dot3(a.axis, b.axis)));
    if (std::min(thetaD + thetaB, kPi) <= thetaA) {
        return;
    }
    if (std::min(thetaD + thetaA, kPi) <= thetaB) {
        std::memcpy(a.axis, b.axis, sizeof(a.axis));
        a.cosO = b.cosO;
        return;
    }
    const f64 thetaO = (thetaA + thetaD + thetaB) * 0.5;
    f64 wr[3];
    cross3(a.axis, b.axis, wr);
    if (thetaO >= kPi || !normalize3(wr)) {
        a.cosO = -1.0;
        return;
    }
    // Rotate a.axis about wr by thetaR (Rodrigues; wr is perpendicular to a.axis).
    const f64 thetaR = thetaO - thetaA;
    f64 k[3];
    cross3(wr, a.axis, k);
    const f64 c = std::cos(thetaR);
    const f64 s = std::sin(thetaR);
    f64 w[3] = {a.axis[0] * c + k[0] * s, a.axis[1] * c + k[1] * s, a.axis[2] * c + k[2] * s};
    normalize3(w);
    std::memcpy(a.axis, w, sizeof(a.axis));
    a.cosO = std::cos(thetaO);
}

/// PBRT-v4 BVHLightSampler::EvaluateCost: phi x M_omega x Kr x surface area (Kr from the parent's box: splits
/// across a thin parent axis cost more).
f64 LightTree::cost(const Bounds& b, const Bounds& parent, u32 dim) {
    const f64 thetaO = std::acos(clamp1(b.cosO));
    const f64 thetaE = std::acos(clamp1(b.cosE));
    const f64 thetaW = std::min(thetaO + thetaE, kPi);
    const f64 sinO = std::sqrt(std::max(0.0, 1.0 - b.cosO * b.cosO));
    const f64 mOmega = 2.0 * kPi * (1.0 - b.cosO) +
                       kPi / 2.0 * (2.0 * thetaW * sinO - std::cos(thetaO - 2.0 * thetaW) - 2.0 * thetaO * sinO + b.cosO);
    const f64 d[3] = {b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2]};
    const f64 pd[3] = {parent.hi[0] - parent.lo[0], parent.hi[1] - parent.lo[1], parent.hi[2] - parent.lo[2]};
    const f64 maxD = std::max({pd[0], pd[1], pd[2]});
    const f64 kr = pd[dim] > 0.0 ? maxD / pd[dim] : 1.0;
    const f64 area = 2.0 * (d[0] * d[1] + d[1] * d[2] + d[2] * d[0]);
    // A degenerate (flat / point) box still orders splits by power: floor the area term.
    return b.phi * mOmega * kr * std::max(area, 1e-12);
}

void LightTree::store(const Bounds& b, bool leaf, LightTreeNode& node) {
    for (u32 i = 0; i < 3u; ++i) {
        node.boundsMin[i] = floatDown(b.lo[i]);
        node.boundsMax[i] = floatUp(b.hi[i]);
        node.axis[i] = static_cast<f32>(b.axis[i]);
    }
    node.phi = static_cast<f32>(b.phi);
    f64 cosO = b.cosO;
    f64 cosE = b.cosE;
    if (!leaf && cosO > -1.0) {
        cosO = std::cos(std::min(std::acos(clamp1(cosO)) + kConePad, kPi));
    }
    if (cosE < 1.0) {
        cosE = std::cos(std::min(std::acos(clamp1(cosE)) + kConePad, kPi));
    }
    node.cosThetaO = floatDown(cosO);
    node.cosThetaE = floatDown(cosE);
    node.sinThetaO = lt_safe_sqrt(1.f - node.cosThetaO * node.cosThetaO);
    node.flags = (leaf ? static_cast<u32>(kLtFlagLeaf) : 0u) | (b.twoSided ? static_cast<u32>(kLtFlagTwoSided) : 0u);
}

// --- build -----------------------------------------------------------------------------------------------

void LightTree::reserve(u32 lights) {
    const usize n = lights;
    const usize nodes = n > 0u ? 2u * n - 1u : 0u;
    m_nodes.reserve(nodes);
    m_nodeBounds.reserve(nodes);
    m_emitters.reserve(n);
    m_directional.reserve(n);
    m_leaf.reserve(n);
    m_centroid.reserve(3u * n);
    m_order.reserve(n);
}

void LightTree::bump() { m_version = g_version.fetch_add(1u, std::memory_order_relaxed) + 1u; }

void LightTree::clear() {
    m_nodes.clear();
    m_nodeBounds.clear();
    m_emitters.clear();
    m_directional.clear();
    m_leaf.clear();
    m_centroid.clear();
    m_order.clear();
    m_nodeCount = 0;
    const u32 builds = m_stats.builds;
    const u32 refits = m_stats.refits;
    m_stats = LightTreeStats{};
    m_stats.builds = builds;
    m_stats.refits = refits;
    bump();
}

bool LightTree::build(const LightTreeLight* lights, u32 count, const LightTreeBuildOptions& options) {
    clear();
    if (count >= 0x80000000u || (count > 0u && lights == nullptr)) {
        return false;
    }
    m_buckets = std::clamp<u32>(options.buckets, 2u, kMaxBuckets);
    m_emitters.resize(count);
    m_leaf.resize(count);
    m_centroid.resize(3u * static_cast<usize>(count));
    for (u32 i = 0; i < count; ++i) {
        LightTreeEmitter e = makeEmitter(lights[i]);
        if (e.kind == kLtKindDirectional) {
            e.depth = static_cast<u32>(m_directional.size());
            m_directional.push_back(i);
        } else {
            m_order.push_back(i);
            m_leaf[i] = leafBounds(lights[i], e);
            for (u32 d = 0; d < 3u; ++d) {
                m_centroid[3u * i + d] = static_cast<f32>((m_leaf[i].lo[d] + m_leaf[i].hi[d]) * 0.5);
            }
        }
        m_emitters[i] = e;
    }
    const u32 treeLights = static_cast<u32>(m_order.size());
    const u32 nodeCount = treeLights > 0u ? 2u * treeLights - 1u : 0u;
    m_nodes.resize(nodeCount);
    m_nodeBounds.resize(nodeCount);
    m_nodeCount = 0;
    if (treeLights > 0u) {
        buildRange(0u, treeLights, 0u, 0u);
        fitInterior();
    }
    m_stats.nodes = nodeCount;
    m_stats.treeLights = treeLights;
    m_stats.directional = static_cast<u32>(m_directional.size());
    ++m_stats.builds;
    bump();
    return true;
}

u32 LightTree::chooseSplit(u32 begin, u32 end, u32 depth, const Bounds& centroids) {
    const u32 n = end - begin;
    const u32 mid = begin + n / 2u;
    // Depth cap: an SAH split here keeps every leaf within kLtMaxDepth only while depth + 1 + ceil(log2 n) fits.
    if (depth + 1u + ceilLog2(n) <= kLtMaxDepth) {
        f64 bestCost = INFINITY;
        u32 bestDim = 3u;
        u32 bestBucket = 0u;
        Bounds bucket[kMaxBuckets];
        Bounds below[kMaxBuckets];
        for (u32 dim = 0; dim < 3u; ++dim) {
            const f64 lo = centroids.lo[dim];
            const f64 hi = centroids.hi[dim];
            if (!(hi > lo)) {
                continue;
            }
            for (u32 b = 0; b < m_buckets; ++b) {
                bucket[b] = Bounds{};
            }
            const f64 scale = static_cast<f64>(m_buckets) / (hi - lo);
            for (u32 i = begin; i < end; ++i) {
                const u32 e = m_order[i];
                u32 b = static_cast<u32>((static_cast<f64>(m_centroid[3u * e + dim]) - lo) * scale);
                b = std::min(b, m_buckets - 1u);
                unite(bucket[b], m_leaf[e]);
            }
            Bounds parent{};
            for (u32 b = 0; b < m_buckets; ++b) {
                unite(parent, bucket[b]);
            }
            // Prefix unions, then sweep from the right.
            Bounds acc{};
            for (u32 b = 0; b + 1u < m_buckets; ++b) {
                unite(acc, bucket[b]);
                below[b] = acc;
            }
            Bounds above{};
            for (u32 b = m_buckets - 1u; b > 0u; --b) {
                unite(above, bucket[b]);
                if (below[b - 1u].empty || above.empty) {
                    continue;
                }
                const f64 c = cost(below[b - 1u], parent, dim) + cost(above, parent, dim);
                if (c < bestCost) {
                    bestCost = c;
                    bestDim = dim;
                    bestBucket = b - 1u;
                }
            }
        }
        if (bestDim < 3u) {
            const f64 lo = centroids.lo[bestDim];
            const f64 scale = static_cast<f64>(m_buckets) / (centroids.hi[bestDim] - lo);
            auto left = [&](u32 e) {
                u32 b = static_cast<u32>((static_cast<f64>(m_centroid[3u * e + bestDim]) - lo) * scale);
                return std::min(b, m_buckets - 1u) <= bestBucket;
            };
            // Deterministic in-place partition (order within each side kept stable enough to be reproducible:
            // it depends only on the input order, which the build fixes).
            u32 i = begin;
            u32 j = end;
            while (true) {
                while (i < j && left(m_order[i])) {
                    ++i;
                }
                while (i < j && !left(m_order[j - 1u])) {
                    --j;
                }
                if (i >= j) {
                    break;
                }
                std::swap(m_order[i], m_order[j - 1u]);
            }
            if (i > begin && i < end) {
                return i;
            }
        }
    }
    // Count median along the widest centroid axis (ties by light index: a unique order).
    u32 dim = 0u;
    f64 widest = -1.0;
    for (u32 d = 0; d < 3u; ++d) {
        const f64 w = centroids.hi[d] - centroids.lo[d];
        if (w > widest) {
            widest = w;
            dim = d;
        }
    }
    const f32* centroid = m_centroid.data();
    std::sort(m_order.begin() + begin, m_order.begin() + end, [centroid, dim](u32 a, u32 b) {
        const f32 ca = centroid[3u * a + dim];
        const f32 cb = centroid[3u * b + dim];
        return ca < cb || (ca == cb && a < b);
    });
    ++m_stats.medianSplits;
    return mid;
}

u32 LightTree::buildRange(u32 begin, u32 end, u32 depth, u32 trail) {
    const u32 index = m_nodeCount++;
    LightTreeNode& node = m_nodes[index];
    node = LightTreeNode{};
    node.depth = depth;
    m_stats.maxDepth = std::max(m_stats.maxDepth, depth);
    if (end - begin == 1u) {
        const u32 e = m_order[begin];
        node.childOrEmitter = e;
        node.flags = kLtFlagLeaf;
        m_nodeBounds[index] = m_leaf[e];
        LightTreeEmitter& emitter = m_emitters[e];
        emitter.leafNode = index;
        emitter.bitTrail = trail;
        emitter.depth = depth;
        return index;
    }
    Bounds centroids{};
    for (u32 i = begin; i < end; ++i) {
        const u32 e = m_order[i];
        Bounds c{};
        c.empty = false;
        for (u32 d = 0; d < 3u; ++d) {
            c.lo[d] = c.hi[d] = m_centroid[3u * e + d];
        }
        if (centroids.empty) {
            centroids = c;
        } else {
            for (u32 d = 0; d < 3u; ++d) {
                centroids.lo[d] = std::min(centroids.lo[d], c.lo[d]);
                centroids.hi[d] = std::max(centroids.hi[d], c.hi[d]);
            }
        }
    }
    const u32 mid = chooseSplit(begin, end, depth, centroids);
    buildRange(begin, mid, depth + 1u, trail);
    const u32 second = buildRange(mid, end, depth + 1u, trail | (1u << depth));
    m_nodes[index].childOrEmitter = second;
    return index;
}

/// Leaf nodes from m_nodeBounds (already set), interior nodes as the union of their children, bottom-up
/// (children always follow their parent in the depth-first order).
void LightTree::fitInterior() {
    for (u32 i = static_cast<u32>(m_nodes.size()); i-- > 0u;) {
        LightTreeNode& node = m_nodes[i];
        const bool leaf = (node.flags & kLtFlagLeaf) != 0u;
        if (!leaf) {
            Bounds b = m_nodeBounds[i + 1u];
            unite(b, m_nodeBounds[node.childOrEmitter]);
            m_nodeBounds[i] = b;
        }
        const u32 child = node.childOrEmitter;
        const u32 depth = node.depth;
        store(m_nodeBounds[i], leaf, node);
        node.childOrEmitter = child;
        node.depth = depth;
    }
}

bool LightTree::refit(const LightTreeLight* lights, u32 count) {
    if (count != m_emitters.size() || (count > 0u && lights == nullptr)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        const bool wasDirectional = m_emitters[i].kind == kLtKindDirectional;
        if ((lights[i].kind == kLtKindDirectional) != wasDirectional) {
            return false;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        LightTreeEmitter e = makeEmitter(lights[i]);
        const LightTreeEmitter& old = m_emitters[i];
        e.leafNode = old.leafNode;
        e.bitTrail = old.bitTrail;
        e.depth = old.depth;
        if (e.kind != kLtKindDirectional) {
            m_leaf[i] = leafBounds(lights[i], e);
            m_nodeBounds[e.leafNode] = m_leaf[i];
            for (u32 d = 0; d < 3u; ++d) {
                m_centroid[3u * i + d] = static_cast<f32>((m_leaf[i].lo[d] + m_leaf[i].hi[d]) * 0.5);
            }
        }
        m_emitters[i] = e;
    }
    fitInterior();
    ++m_stats.refits;
    bump();
    return true;
}

LightTreeView LightTree::view() const {
    LightTreeView v{};
    v.nodes = m_nodes.data();
    v.emitters = m_emitters.data();
    v.directional = m_directional.data();
    v.nodeCount = static_cast<u32>(m_nodes.size());
    v.emitterCount = static_cast<u32>(m_emitters.size());
    v.directionalCount = static_cast<u32>(m_directional.size());
    return v;
}

LightTreeSample LightTree::sample(const f32 (&p)[3], const f32 (&n)[3], f32 u0, f32 u1, f32 u2) const {
    return lt_sample(view(), p, n, u0, u1, u2);
}

f32 LightTree::pmf(const f32 (&p)[3], const f32 (&n)[3], u32 light) const { return lt_pmf(view(), p, n, light); }

f32 LightTree::importance(u32 node, const f32 (&p)[3], const f32 (&n)[3]) const {
    if (node >= m_nodes.size()) {
        return 0.f;
    }
    return lt_importance(m_nodes[node], p, n, lt_has_normal(n));
}

// --- GpuScene adapter ------------------------------------------------------------------------------------

bool lightFromGpuLight(const gpu_scene::GpuLight& light, u32 source, LightTreeLight& out) {
    const f32 colorMax = std::max({light.color[0], light.color[1], light.color[2], 0.f});
    const f32 scalar = colorMax * light.intensity;
    const f32 position[3] = {light.position[0], light.position[1], light.position[2]};
    const f32 direction[3] = {light.direction[0], light.direction[1], light.direction[2]};
    switch (light.type) {
    case static_cast<u32>(gpu_scene::GpuLightType::Directional):
        out = makeDirectionalLight(direction, scalar, source);
        return true;
    case static_cast<u32>(gpu_scene::GpuLightType::Point):
        out = makePointLight(position, scalar, source);
        return true;
    case static_cast<u32>(gpu_scene::GpuLightType::Spot):
        out = makeSpotLight(position, direction, light.cosInner, light.cosOuter, scalar, source);
        return true;
    case ltc::kLightRect:
    case ltc::kLightDisk: {
        const math::Vec3 normal = ltc::safe_unit(math::Vec3{direction[0], direction[1], direction[2]},
                                                 math::Vec3{0.f, 0.f, -1.f});
        const math::Vec3 tangent = ltc::decodeTangent(light.flags);
        math::Vec3 ex{};
        math::Vec3 ey{};
        ltc::area_axes(normal, tangent, light.cosInner, light.cosOuter, ex, ey);
        const f32 u[3] = {ex.x, ex.y, ex.z};
        const f32 v[3] = {ey.x, ey.y, ey.z};
        out = light.type == ltc::kLightRect ? makeRectLight(position, u, v, scalar, false, source)
                                            : makeDiskLight(position, u, v, scalar, false, source);
        return true;
    }
    default:
        return false;
    }
}

u32 appendSceneLights(const gpu_scene::GpuScene& scene, std::vector<LightTreeLight>& out) {
    u32 appended = 0;
    for (u32 slot = 0; slot < scene.lightHighWater(); ++slot) {
        LightTreeLight l{};
        if (lightFromGpuLight(scene.light(slot), slot, l)) {
            out.push_back(l);
            ++appended;
        }
    }
    return appended;
}

u32 appendEmissiveTriangles(const geometry::MeshletMesh& mesh, const gpu_scene::GpuTransform& transform, f32 radiance,
                            bool twoSided, std::vector<LightTreeLight>& out, std::vector<EmissiveTriangleRef>* refs,
                            u32 instance, u32 submesh) {
    u32 appended = 0;
    u32 triangle = 0;
    const geometry::QuantParams& q = mesh.quant;
    auto world = [&](u32 vertex, f32 (&w)[3]) {
        f32 o[3];
        for (u32 c = 0; c < 3u; ++c) {
            o[c] = q.offset[c] + static_cast<f32>(mesh.positions[4u * vertex + c]) * q.step[c];
        }
        for (u32 r = 0; r < 3u; ++r) {
            const f32* row = transform.rows[r];
            w[r] = row[0] * o[0] + row[1] * o[1] + row[2] * o[2] + row[3];
        }
    };
    for (const geometry::MeshletRecord& m : mesh.meshlets) {
        const bool wanted = submesh == kLtInvalid || m.submesh == submesh;
        for (u32 t = 0; t < m.triangle_count; ++t, ++triangle) {
            if (!wanted) {
                continue;
            }
            const u32 packed = mesh.meshlet_triangles[m.triangle_offset + t];
            f32 v[3][3];
            for (u32 corner = 0; corner < 3u; ++corner) {
                const u32 local = geometry::triangle_index(packed, corner);
                world(mesh.meshlet_vertices[m.vertex_offset + local], v[corner]);
            }
            u32 source = kLtInvalid;
            if (refs != nullptr) {
                source = static_cast<u32>(refs->size());
                refs->push_back(EmissiveTriangleRef{instance, triangle});
            }
            out.push_back(makeTriangleLight(v[0], v[1], v[2], radiance, twoSided, source));
            ++appended;
        }
    }
    return appended;
}

u32 appendSceneEmissiveTriangles(const gpu_scene::GpuScene& scene, const geometry::MeshletMesh* const* meshes,
                                 u32 meshCount, std::vector<LightTreeLight>& out, std::vector<EmissiveTriangleRef>* refs,
                                 bool twoSided) {
    const gpu_scene::TableBytes materials = scene.tableBytes(gpu_scene::GpuSceneTable::Materials);
    u32 appended = 0;
    for (u32 slot = 0; slot < scene.instanceHighWater(); ++slot) {
        const gpu_scene::GpuInstance& inst = scene.instance(slot);
        if ((inst.flags & gpu_scene::kInstanceValid) == 0u || (inst.flags & gpu_scene::kInstanceVisible) == 0u ||
            inst.mesh >= meshCount || meshes[inst.mesh] == nullptr || inst.material == gpu_scene::kInvalidIndex) {
            continue;
        }
        const geometry::MeshletMesh& mesh = *meshes[inst.mesh];
        const u32 submeshCount = static_cast<u32>(mesh.submeshes.size());
        for (u32 s = 0; s < std::max(submeshCount, 1u); ++s) {
            const u32 materialOffset = submeshCount > 0u ? mesh.submeshes[s].material_index : 0u;
            const u32 row = inst.material + materialOffset;
            Material::GPUMaterial gpu{};
            if (!MaterialLayout::fetchRow(materials.data, static_cast<usize>(materials.count) * materials.stride, row, gpu)) {
                continue;
            }
            const math::Vec3 l = MaterialEval::emissiveRadiance(gpu);
            const f32 radiance = std::max({l.x, l.y, l.z});
            if (!(radiance > 0.f)) {
                continue;
            }
            appended += appendEmissiveTriangles(mesh, scene.transform(slot), radiance, twoSided, out, refs, slot,
                                                submeshCount > 0u ? s : kLtInvalid);
            if (submeshCount == 0u) {
                break;
            }
        }
    }
    return appended;
}

} // namespace fuse::renderer::light_tree
