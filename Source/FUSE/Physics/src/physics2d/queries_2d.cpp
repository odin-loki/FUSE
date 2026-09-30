// Ray casts and AABB / point queries over a BVH of the shape AABBs. The tree is rebuilt lazily (median
// split on the longest axis) the first time a query runs after a step or an edit.

#include "rigid_world_2d.hpp"

#include <algorithm>
#include <cfloat>

namespace fuse::physics::p2d {

namespace {

constexpr u32 kLeafSize = 4u;

bool aabbOverlap(const Aabb2D& a, const Aabb2D& b) {
    return a.lower.x <= b.upper.x && b.lower.x <= a.upper.x && a.lower.y <= b.upper.y && b.lower.y <= a.upper.y;
}

Aabb2D merge(const Aabb2D& a, const Aabb2D& b) { return {vmin(a.lower, b.lower), vmax(a.upper, b.upper)}; }

/// Slab test of the segment p1 + t * d, t in [0, maxT].
bool segmentHitsBox(vec2 p1, vec2 d, f32 maxT, const Aabb2D& box) {
    f32 tmin = 0.f;
    f32 tmax = maxT;
    const f32 p[2] = {p1.x, p1.y};
    const f32 dir[2] = {d.x, d.y};
    const f32 lo[2] = {box.lower.x, box.lower.y};
    const f32 hi[2] = {box.upper.x, box.upper.y};
    for (u32 i = 0; i < 2u; ++i) {
        if (std::fabs(dir[i]) < kEpsilon) {
            if (p[i] < lo[i] || p[i] > hi[i]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.f / dir[i];
        f32 t1 = (lo[i] - p[i]) * inv;
        f32 t2 = (hi[i] - p[i]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) {
            return false;
        }
    }
    return true;
}

bool passes(const Shape2D& s, u32 maskBits, bool includeSensors) {
    return s.alive && (s.filter.categoryBits & maskBits) != 0u && (includeSensors || !s.isSensor);
}

} // namespace

u32 RigidWorld2D::buildBvhNode(u32 first, u32 count, u32 depth) const {
    const u32 index = static_cast<u32>(m_bvhNodes.size());
    m_bvhNodes.emplace_back();
    Aabb2D box = m_queryAabbs[m_bvhItems[first]];
    for (u32 i = 1; i < count; ++i) {
        box = merge(box, m_queryAabbs[m_bvhItems[first + i]]);
    }
    m_bvhNodes[index].box = box;
    if (count <= kLeafSize || depth >= 48u) {
        m_bvhNodes[index].first = first;
        m_bvhNodes[index].count = count;
        return index;
    }
    const bool splitX = (box.upper.x - box.lower.x) >= (box.upper.y - box.lower.y);
    const u32 half = count / 2u;
    auto begin = m_bvhItems.begin() + static_cast<std::ptrdiff_t>(first);
    auto key = [&](u32 s) {
        const Aabb2D& b = m_queryAabbs[s];
        return splitX ? b.lower.x + b.upper.x : b.lower.y + b.upper.y;
    };
    std::nth_element(begin, begin + static_cast<std::ptrdiff_t>(half), begin + static_cast<std::ptrdiff_t>(count),
                     [&](u32 l, u32 r) {
                         const f32 kl = key(l);
                         const f32 kr = key(r);
                         return kl < kr || (kl == kr && l < r);
                     });
    const u32 left = buildBvhNode(first, half, depth + 1u);
    const u32 right = buildBvhNode(first + half, count - half, depth + 1u);
    m_bvhNodes[index].left = left;
    m_bvhNodes[index].right = right;
    return index;
}

void RigidWorld2D::rebuildBvh() const {
    // Query AABBs are exact for the current poses (setTransform may have moved bodies since the step).
    m_bvhItems.clear();
    m_queryAabbs.resize(m_shapes.size());
    for (u32 i = 0; i < m_shapes.size(); ++i) {
        const Shape2D& s = m_shapes[i];
        if (!s.alive) {
            continue;
        }
        m_queryAabbs[i] = computeAabb(s, m_bodies[s.body].xf);
        m_bvhItems.push_back(i);
    }
    m_bvhNodes.clear();
    m_bvhRoot = m_bvhItems.empty() ? kInvalidId2D : buildBvhNode(0u, static_cast<u32>(m_bvhItems.size()), 0u);
    m_bvhDirty = false;
}

bool RigidWorld2D::rayCastShape(ShapeId2D id, vec2 p1, vec2 p2, f32 maxFraction, RayHit2D& out) const {
    if (!shapeValid(id)) {
        return false;
    }
    const Shape2D& s = m_shapes[id];
    f32 fraction = 0.f;
    vec2 normal{0.f, 0.f};
    if (!rayCast(s, m_bodies[s.body].xf, p1, p2, maxFraction, fraction, normal)) {
        return false;
    }
    out.hit = true;
    out.shape = id;
    out.body = s.body;
    out.fraction = fraction;
    out.normal = normal;
    out.point = p1 + fraction * (p2 - p1);
    return true;
}

RayHit2D RigidWorld2D::rayCastClosest(vec2 p1, vec2 p2, u32 maskBits, bool includeSensors) const {
    RayHit2D best;
    if (m_bvhDirty) {
        rebuildBvh();
    }
    if (m_bvhRoot == kInvalidId2D) {
        return best;
    }
    const vec2 d = p2 - p1;
    f32 maxFraction = 1.f;
    m_bvhStack.clear();
    m_bvhStack.push_back(m_bvhRoot);
    while (!m_bvhStack.empty()) {
        const BvhNode2D& node = m_bvhNodes[m_bvhStack.back()];
        m_bvhStack.pop_back();
        if (!segmentHitsBox(p1, d, maxFraction, node.box)) {
            continue;
        }
        if (node.left == kInvalidId2D) {
            for (u32 i = 0; i < node.count; ++i) {
                const u32 id = m_bvhItems[node.first + i];
                if (!passes(m_shapes[id], maskBits, includeSensors)) {
                    continue;
                }
                RayHit2D hit;
                if (rayCastShape(id, p1, p2, maxFraction, hit)) {
                    // Ties go to the lower shape id so the answer does not depend on tree layout.
                    if (!best.hit || hit.fraction < best.fraction || (hit.fraction == best.fraction && id < best.shape)) {
                        best = hit;
                        maxFraction = hit.fraction;
                    }
                }
            }
        } else {
            m_bvhStack.push_back(node.right);
            m_bvhStack.push_back(node.left);
        }
    }
    return best;
}

u32 RigidWorld2D::rayCastAll(vec2 p1, vec2 p2, std::vector<RayHit2D>& out, u32 maskBits, bool includeSensors) const {
    out.clear();
    if (m_bvhDirty) {
        rebuildBvh();
    }
    if (m_bvhRoot == kInvalidId2D) {
        return 0u;
    }
    const vec2 d = p2 - p1;
    m_bvhStack.clear();
    m_bvhStack.push_back(m_bvhRoot);
    while (!m_bvhStack.empty()) {
        const u32 nodeIndex = m_bvhStack.back();
        m_bvhStack.pop_back();
        const BvhNode2D& node = m_bvhNodes[nodeIndex];
        if (!segmentHitsBox(p1, d, 1.f, node.box)) {
            continue;
        }
        if (node.left == kInvalidId2D) {
            for (u32 i = 0; i < node.count; ++i) {
                const u32 id = m_bvhItems[node.first + i];
                if (!passes(m_shapes[id], maskBits, includeSensors)) {
                    continue;
                }
                RayHit2D hit;
                if (rayCastShape(id, p1, p2, 1.f, hit)) {
                    out.push_back(hit);
                }
            }
        } else {
            m_bvhStack.push_back(node.right);
            m_bvhStack.push_back(node.left);
        }
    }
    std::sort(out.begin(), out.end(), [](const RayHit2D& l, const RayHit2D& r) {
        return l.fraction < r.fraction || (l.fraction == r.fraction && l.shape < r.shape);
    });
    return static_cast<u32>(out.size());
}

u32 RigidWorld2D::queryAabb(const Aabb2D& box, std::vector<ShapeId2D>& out, u32 maskBits) const {
    out.clear();
    if (m_bvhDirty) {
        rebuildBvh();
    }
    if (m_bvhRoot == kInvalidId2D) {
        return 0u;
    }
    m_bvhStack.clear();
    m_bvhStack.push_back(m_bvhRoot);
    while (!m_bvhStack.empty()) {
        const u32 nodeIndex = m_bvhStack.back();
        m_bvhStack.pop_back();
        const BvhNode2D& node = m_bvhNodes[nodeIndex];
        if (!aabbOverlap(node.box, box)) {
            continue;
        }
        if (node.left == kInvalidId2D) {
            for (u32 i = 0; i < node.count; ++i) {
                const u32 id = m_bvhItems[node.first + i];
                const Shape2D& s = m_shapes[id];
                if (s.alive && (s.filter.categoryBits & maskBits) != 0u && aabbOverlap(m_queryAabbs[id], box)) {
                    out.push_back(id);
                }
            }
        } else {
            m_bvhStack.push_back(node.right);
            m_bvhStack.push_back(node.left);
        }
    }
    std::sort(out.begin(), out.end());
    return static_cast<u32>(out.size());
}

u32 RigidWorld2D::queryPoint(vec2 p, std::vector<ShapeId2D>& out, u32 maskBits) const {
    const Aabb2D box{p, p};
    queryAabb(box, out, maskBits);
    usize write = 0u;
    for (usize i = 0; i < out.size(); ++i) {
        const Shape2D& s = m_shapes[out[i]];
        if (testPoint(s, m_bodies[s.body].xf, p)) {
            out[write++] = out[i];
        }
    }
    out.resize(write);
    return static_cast<u32>(out.size());
}

} // namespace fuse::physics::p2d
