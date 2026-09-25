#pragma once

// WP-7.1 light tree: the single-source sampler. The CPU sampler (LightTreeSampler, light_tree.hpp) calls
// these functions; shaders/light_tree/lt_common.{glsl,slang} are their line-for-line twins, so a GPU sample
// equals the CPU sample bit for bit.
//
// Bit-exactness rules (checked by fuse_rp_light_tree_vk_sample on Lavapipe):
//   * only IEEE-exact operations: + - * /, sqrt, abs, comparisons, float <-> u32 conversions; min / max are
//     written as comparisons (lt_min / lt_max) so NaN semantics never matter; no transcendental functions
//     (the disk mapping uses a fixed polynomial for sin / cos on [-pi/4, pi/4]);
//   * every expression evaluated in the same order in all three sources (dot products left to right);
//   * no multiply-add contraction: the C++ build uses -ffp-contract=off (cmake/rp_wp71.cmake), GLSL keeps
//     every arithmetic result in a `precise` variable, Slang is compiled with -fp-mode precise.
//
// Importance of a node at shading point p with normal n (PBRT-v4 LightBounds::Importance, Conty Estevez and
// Kulla 2018): phi x cos(theta') / d^2 x cos(theta'_i), where theta' = max(0, theta_w - theta_o - theta_b) is
// the smallest angle between the node's orientation cone and the direction to p (theta_b bounds the node as
// seen from p), zero outside theta_o + theta_e, and theta'_i the same bound for the receiver's normal. d^2 is
// clamped to the squared half diagonal of the box (inside the box the distance means little).
//
// Traversal: at an interior node the two children's importances i0, i1 give p0 = i0 / (i0 + i1); the random
// number is rescaled into the chosen branch. If both are zero (possible below a node with non-zero importance:
// the children's bounds are tighter) the branch probabilities fall back to 1/2, so the selection pmf is a
// distribution over every light at every shading point (it sums to 1); lights under a zero-importance node
// have no contribution at p (the bounds are conservative), so this costs nothing in bias.
//
// Device-safe (only <fuse/types.hpp>, <cmath> for sqrt / fabs).

#include <fuse/renderer/light_tree/light_tree_types.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::light_tree {

/// Non-owning view of one tree (the CPU tables of LightTree, or a read-back GPU copy).
struct LightTreeView {
    const LightTreeNode* nodes = nullptr;
    const LightTreeEmitter* emitters = nullptr;
    const u32* directional = nullptr;
    u32 nodeCount = 0;
    u32 emitterCount = 0;
    u32 directionalCount = 0;
};

FUSE_HOST_DEVICE inline f32 lt_min(f32 a, f32 b) { return b < a ? b : a; }
FUSE_HOST_DEVICE inline f32 lt_max(f32 a, f32 b) { return a < b ? b : a; }
FUSE_HOST_DEVICE inline f32 lt_safe_sqrt(f32 x) { return std::sqrt(lt_max(x, 0.f)); }

/// cos(max(0, a - b)) from the sines and cosines of a and b (a, b in [0, pi]).
FUSE_HOST_DEVICE inline f32 lt_cos_sub_clamped(f32 sinA, f32 cosA, f32 sinB, f32 cosB) {
    if (cosA > cosB) {
        return 1.f;
    }
    const f32 x = cosA * cosB;
    const f32 y = sinA * sinB;
    return x + y;
}

/// sin(max(0, a - b)).
FUSE_HOST_DEVICE inline f32 lt_sin_sub_clamped(f32 sinA, f32 cosA, f32 sinB, f32 cosB) {
    if (cosA > cosB) {
        return 0.f;
    }
    const f32 x = sinA * cosB;
    const f32 y = cosA * sinB;
    return x - y;
}

/// Node importance at p (normal n used when hasNormal).
FUSE_HOST_DEVICE inline f32 lt_importance(const LightTreeNode& node, const f32 p[3], const f32 n[3], bool hasNormal) {
    const f32 cx = (node.boundsMin[0] + node.boundsMax[0]) * 0.5f;
    const f32 cy = (node.boundsMin[1] + node.boundsMax[1]) * 0.5f;
    const f32 cz = (node.boundsMin[2] + node.boundsMax[2]) * 0.5f;
    const f32 dx = p[0] - cx;
    const f32 dy = p[1] - cy;
    const f32 dz = p[2] - cz;
    const f32 dist2 = dx * dx + dy * dy + dz * dz;
    const f32 gx = node.boundsMax[0] - node.boundsMin[0];
    const f32 gy = node.boundsMax[1] - node.boundsMin[1];
    const f32 gz = node.boundsMax[2] - node.boundsMin[2];
    const f32 radius2 = (gx * gx + gy * gy + gz * gz) * 0.25f;
    const f32 d2 = lt_max(lt_max(dist2, radius2), kLtMinDistance2);
    f32 wx = 0.f;
    f32 wy = 0.f;
    f32 wz = 0.f;
    if (dist2 > 0.f) {
        const f32 len = std::sqrt(dist2);
        wx = dx / len;
        wy = dy / len;
        wz = dz / len;
    }
    f32 cosW = node.axis[0] * wx + node.axis[1] * wy + node.axis[2] * wz;
    if ((node.flags & kLtFlagTwoSided) != 0u) {
        cosW = std::fabs(cosW);
    }
    const f32 sinW = lt_safe_sqrt(1.f - cosW * cosW);
    f32 cosB = -1.f;
    f32 sinB = 0.f;
    if (dist2 > radius2) {
        const f32 sin2 = radius2 / dist2;
        cosB = lt_safe_sqrt(1.f - sin2);
        sinB = std::sqrt(sin2);
    }
    const f32 cosX = lt_cos_sub_clamped(sinW, cosW, node.sinThetaO, node.cosThetaO);
    const f32 sinX = lt_sin_sub_clamped(sinW, cosW, node.sinThetaO, node.cosThetaO);
    const f32 cosP = lt_cos_sub_clamped(sinX, cosX, sinB, cosB);
    if (cosP <= node.cosThetaE) {
        return 0.f;
    }
    f32 importance = node.phi * cosP / d2;
    if (hasNormal) {
        const f32 cosI = std::fabs(wx * n[0] + wy * n[1] + wz * n[2]);
        const f32 sinI = lt_safe_sqrt(1.f - cosI * cosI);
        const f32 cosPI = lt_cos_sub_clamped(sinI, cosI, sinB, cosB);
        importance = importance * cosPI;
    }
    return lt_max(importance, 0.f);
}

/// Branch probabilities from the children's importances (1/2 each when both are zero).
FUSE_HOST_DEVICE inline void lt_branch(f32 i0, f32 i1, f32& p0, f32& p1) {
    const f32 sum = i0 + i1;
    if (sum > 0.f) {
        p0 = i0 / sum;
        p1 = i1 / sum;
    } else {
        p0 = 0.5f;
        p1 = 0.5f;
    }
}

FUSE_HOST_DEVICE inline bool lt_has_normal(const f32 n[3]) { return n[0] != 0.f || n[1] != 0.f || n[2] != 0.f; }

/// Probability of choosing the directional part (dirCount / (dirCount + 1), 1 without a tree).
FUSE_HOST_DEVICE inline f32 lt_directional_probability(const LightTreeView& t) {
    const u32 total = t.directionalCount + (t.nodeCount > 0u ? 1u : 0u);
    if (total == 0u) {
        return 0.f;
    }
    return static_cast<f32>(t.directionalCount) / static_cast<f32>(total);
}

/// sin / cos on [-pi/4, pi/4]: fixed polynomials (Taylor to x^9 / x^8, |error| < 4e-7), identical on the GPU.
FUSE_HOST_DEVICE inline f32 lt_sin_poly(f32 t) {
    const f32 t2 = t * t;
    const f32 a = t2 * 2.7557319e-06f;
    const f32 b = t2 * (-0.00019841270f + a);
    const f32 c = t2 * (0.0083333333f + b);
    const f32 d = t2 * (-0.16666667f + c);
    const f32 e = t * d;
    return t + e;
}
FUSE_HOST_DEVICE inline f32 lt_cos_poly(f32 t) {
    const f32 t2 = t * t;
    const f32 a = t2 * 2.4801587e-05f;
    const f32 b = t2 * (-0.0013888889f + a);
    const f32 c = t2 * (0.041666668f + b);
    const f32 d = t2 * (-0.5f + c);
    return 1.f + d;
}

/// Shirley-Chiu concentric square -> unit disk mapping.
FUSE_HOST_DEVICE inline void lt_concentric(f32 u1, f32 u2, f32& x, f32& y) {
    const f32 a = u1 * 2.f - 1.f;
    const f32 b = u2 * 2.f - 1.f;
    x = 0.f;
    y = 0.f;
    if (a == 0.f && b == 0.f) {
        return;
    }
    if (std::fabs(a) > std::fabs(b)) {
        const f32 t = (b / a) * 0.78539816f;
        x = a * lt_cos_poly(t);
        y = a * lt_sin_poly(t);
    } else {
        const f32 t = (a / b) * 0.78539816f;
        x = b * lt_sin_poly(t);
        y = b * lt_cos_poly(t);
    }
}

/// Point on the light for (u1, u2) and its area density (0 for delta lights).
FUSE_HOST_DEVICE inline void lt_sample_point(const LightTreeEmitter& e, f32 u1, f32 u2, f32 out[3], f32& pdfArea) {
    f32 a = 0.f;
    f32 b = 0.f;
    bool surface = true;
    if (e.kind == kLtKindTriangle) {
        const f32 su = std::sqrt(u1);
        a = su * (1.f - u2);
        b = su * u2;
    } else if (e.kind == kLtKindRect) {
        a = u1 * 2.f - 1.f;
        b = u2 * 2.f - 1.f;
    } else if (e.kind == kLtKindDisk) {
        lt_concentric(u1, u2, a, b);
    } else {
        surface = false;
    }
    if (!surface) {
        const bool directional = e.kind == kLtKindDirectional;
        out[0] = directional ? e.normal[0] : e.p0[0];
        out[1] = directional ? e.normal[1] : e.p0[1];
        out[2] = directional ? e.normal[2] : e.p0[2];
        pdfArea = 0.f;
        return;
    }
    out[0] = e.p0[0] + e.e1[0] * a + e.e2[0] * b;
    out[1] = e.p0[1] + e.e1[1] * a + e.e2[1] * b;
    out[2] = e.p0[2] + e.e1[2] * a + e.e2[2] * b;
    pdfArea = e.area > 0.f ? 1.f / e.area : 0.f;
}

/// Chooses a light for (p, n) with u0 and a point on it with (u1, u2).
FUSE_HOST_DEVICE inline LightTreeSample lt_sample(const LightTreeView& t, const f32 p[3], const f32 n[3], f32 u0, f32 u1,
                                                  f32 u2) {
    LightTreeSample s{};
    const f32 pDir = lt_directional_probability(t);
    if (t.directionalCount == 0u && t.nodeCount == 0u) {
        return s;
    }
    const bool hasNormal = lt_has_normal(n);
    f32 u = u0;
    u32 light = kLtInvalid;
    f32 pmf = 0.f;
    if (u < pDir) {
        u = lt_min(u / pDir, kLtOneMinusEpsilon);
        u32 k = static_cast<u32>(u * static_cast<f32>(t.directionalCount));
        if (k >= t.directionalCount) {
            k = t.directionalCount - 1u;
        }
        light = t.directional[k];
        pmf = pDir / static_cast<f32>(t.directionalCount);
    } else {
        const f32 pTree = 1.f - pDir;
        u = lt_min((u - pDir) / pTree, kLtOneMinusEpsilon);
        pmf = pTree;
        u32 node = 0u;
        for (u32 level = 0u; level <= kLtMaxDepth; ++level) {
            const LightTreeNode& current = t.nodes[node];
            if ((current.flags & kLtFlagLeaf) != 0u) {
                light = current.childOrEmitter;
                break;
            }
            const u32 c0 = node + 1u;
            const u32 c1 = current.childOrEmitter;
            const f32 i0 = lt_importance(t.nodes[c0], p, n, hasNormal);
            const f32 i1 = lt_importance(t.nodes[c1], p, n, hasNormal);
            f32 p0 = 0.f;
            f32 p1 = 0.f;
            lt_branch(i0, i1, p0, p1);
            if (u < p0) {
                u = lt_min(u / p0, kLtOneMinusEpsilon);
                pmf = pmf * p0;
                node = c0;
            } else {
                u = lt_min((u - p0) / p1, kLtOneMinusEpsilon);
                pmf = pmf * p1;
                node = c1;
            }
        }
    }
    if (light >= t.emitterCount) {
        return s;
    }
    const LightTreeEmitter& e = t.emitters[light];
    s.light = light;
    s.pmf = pmf;
    s.kind = e.kind;
    lt_sample_point(e, u1, u2, s.position, s.pdfArea);
    return s;
}

/// Probability that lt_sample chooses `light` at (p, n): the same operations as the sampling walk along the
/// light's bit trail, so pmf(sample.light) == sample.pmf bit for bit.
FUSE_HOST_DEVICE inline f32 lt_pmf(const LightTreeView& t, const f32 p[3], const f32 n[3], u32 light) {
    if (light >= t.emitterCount) {
        return 0.f;
    }
    const LightTreeEmitter& e = t.emitters[light];
    const f32 pDir = lt_directional_probability(t);
    if (e.kind == kLtKindDirectional) {
        return t.directionalCount > 0u ? pDir / static_cast<f32>(t.directionalCount) : 0.f;
    }
    if (e.leafNode == kLtInvalid || t.nodeCount == 0u) {
        return 0.f;
    }
    const bool hasNormal = lt_has_normal(n);
    f32 pmf = 1.f - pDir;
    u32 node = 0u;
    for (u32 level = 0u; level < e.depth; ++level) {
        const LightTreeNode& current = t.nodes[node];
        const u32 c0 = node + 1u;
        const u32 c1 = current.childOrEmitter;
        const f32 i0 = lt_importance(t.nodes[c0], p, n, hasNormal);
        const f32 i1 = lt_importance(t.nodes[c1], p, n, hasNormal);
        f32 p0 = 0.f;
        f32 p1 = 0.f;
        lt_branch(i0, i1, p0, p1);
        if (((e.bitTrail >> level) & 1u) == 0u) {
            pmf = pmf * p0;
            node = c0;
        } else {
            pmf = pmf * p1;
            node = c1;
        }
    }
    return pmf;
}

} // namespace fuse::renderer::light_tree
