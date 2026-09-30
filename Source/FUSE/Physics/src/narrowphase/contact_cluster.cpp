#include <fuse/physics/narrowphase/contact_cluster.hpp>

#include <fuse/physics/narrowphase/primitive_contacts.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::narrowphase {

namespace {

constexpr f32 kClusterCos = 0.95f;
constexpr u32 kMaxClusterPoints = 64u;

vec3 perpendicular(vec3 n) {
    const vec3 hint = std::fabs(n.x) < 0.9f ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f};
    return n.cross(hint).normalized();
}

} // namespace

u32 ContactClusterer::build(u32 idxA, u32 idxB, ContactManifold* out, u32 maxOut) {
    const u32 n = static_cast<u32>(m_candidates.size());
    if (n == 0u || out == nullptr || maxOut == 0u) {
        return 0u;
    }
    // Deepest first; ties keep insertion order (deterministic).
    m_order.resize(n);
    for (u32 i = 0; i < n; ++i) {
        m_order[i] = i;
    }
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&](u32 a, u32 b) { return m_candidates[a].depth > m_candidates[b].depth; });

    m_cluster.assign(n, 0u);
    m_clusterNormal.clear();
    m_clusterSum.clear();
    m_clusterDepth.clear();
    for (const u32 i : m_order) {
        const CandidateContact& c = m_candidates[i];
        u32 best = static_cast<u32>(m_clusterNormal.size());
        f32 bestAlign = kClusterCos;
        for (u32 k = 0; k < m_clusterNormal.size(); ++k) {
            const f32 align = m_clusterNormal[k].dot(c.normal);
            if (align > bestAlign) {
                bestAlign = align;
                best = k;
            }
        }
        if (best == m_clusterNormal.size()) {
            if (m_clusterNormal.size() < maxOut) {
                m_clusterNormal.push_back(c.normal);
                m_clusterSum.push_back({});
                m_clusterDepth.push_back(c.depth);
            } else {
                // Out of manifolds: join the most aligned cluster.
                f32 most = -2.f;
                for (u32 k = 0; k < m_clusterNormal.size(); ++k) {
                    const f32 align = m_clusterNormal[k].dot(c.normal);
                    if (align > most) {
                        most = align;
                        best = k;
                    }
                }
            }
        }
        m_cluster[i] = best;
        const f32 weight = std::max(c.depth, 0.f) + 1e-3f;
        m_clusterSum[best] += c.normal * weight;
    }

    u32 written = 0;
    for (u32 k = 0; k < m_clusterNormal.size() && written < maxOut; ++k) {
        vec3 normal = m_clusterSum[k];
        const f32 len = normal.length();
        normal = len > 1e-9f ? normal * (1.f / len) : m_clusterNormal[k];
        hd::DepthPoint points[kMaxClusterPoints];
        u32 count = 0;
        for (const u32 i : m_order) {
            if (m_cluster[i] != k || count >= kMaxClusterPoints) {
                continue;
            }
            const CandidateContact& c = m_candidates[i];
            const f32 depth = c.depth * std::max(c.normal.dot(normal), 0.f);
            bool duplicate = false;
            for (u32 j = 0; j < count && !duplicate; ++j) {
                const vec3 d = points[j].point - c.point;
                if (d.dot(d) < 1e-8f) {
                    duplicate = true;
                    points[j].depth = std::max(points[j].depth, depth);
                }
            }
            if (!duplicate) {
                points[count++] = {c.point, depth};
            }
        }
        if (count == 0u) {
            continue;
        }
        const vec3 s1 = perpendicular(normal);
        const vec3 s2 = normal.cross(s1);
        count = hd::reduceToFour(points, count, normal, s1 * 0.8f + s2 * 0.6f, 1e-3f);
        ContactManifold& m = out[written++];
        m = ContactManifold{};
        m.contactNormal = normal;
        m.bodyA = idxA;
        m.bodyB = idxB;
        m.valid = true;
        for (u32 i = 0; i < count; ++i) {
            m.addPoint(points[i].point, points[i].depth);
        }
    }
    return written;
}

} // namespace fuse::physics::narrowphase
