#pragma once

// Merges per-feature contacts against a concave shape (triangles of a mesh, voxels, SDF samples) into at
// most kMaxManifoldsPerPair manifolds per body pair: candidates are grouped by normal (within ~18 deg of
// a cluster's deepest normal), each cluster gets the depth-weighted mean normal and its points reduced
// to four (deepest first, then largest area). Coplanar triangles / voxel faces of a floor therefore give
// one four-point manifold, and a box in a V-groove keeps one manifold per wall.

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

constexpr u32 kMaxManifoldsPerPair = 4u;

struct CandidateContact {
    vec3 point{};
    vec3 normal{}; ///< unit, from the concave shape (B) towards the convex shape (A)
    f32 depth = 0.f;
};

class ContactClusterer {
public:
    void reset() { m_candidates.clear(); }
    void add(vec3 point, vec3 normal, f32 depth) { m_candidates.push_back({point, normal, depth}); }
    [[nodiscard]] u32 candidateCount() const { return static_cast<u32>(m_candidates.size()); }
    [[nodiscard]] const std::vector<CandidateContact>& candidates() const { return m_candidates; }

    /// Writes up to `maxOut` manifolds (deepest cluster first) with bodies (idxA, idxB); returns the
    /// count. Candidates stay (call reset for the next pair).
    u32 build(u32 idxA, u32 idxB, ContactManifold* out, u32 maxOut);

private:
    std::vector<CandidateContact> m_candidates;
    std::vector<u32> m_order;
    std::vector<u32> m_cluster;
    std::vector<vec3> m_clusterNormal;
    std::vector<vec3> m_clusterSum;
    std::vector<f32> m_clusterDepth;
};

} // namespace fuse::physics::narrowphase
