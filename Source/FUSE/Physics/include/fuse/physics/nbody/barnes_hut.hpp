#pragma once

#include <fuse/physics/math.hpp>

#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

struct BHNode {
    vec3 center_of_mass{};
    f32 total_mass = 0.f;
    vec3 min_bounds{};
    vec3 max_bounds{};
    u32 first_child = 0;
    u32 body_count = 0;
};

struct BHParams {
    f32 theta = 0.5f;
    f32 gravitational_G = 6.674e-11f;
    f32 softening = 0.1f;
    u32 max_depth = 20;
};

/// B4.5 — Barnes-Hut n-body force field (CPU reference; GPU tree build is a B4.1 follow-on).
class BarnesHut {
public:
    void computeForces(const std::vector<vec3>& positions,
                       const std::vector<f32>& masses,
                       std::vector<vec3>& out_accelerations,
                       const BHParams& params);

    u32 nodeCount() const { return static_cast<u32>(nodes_.size()); }

private:
    std::vector<BHNode> nodes_;
};

/// Reference O(n^2) force accumulation for validation tests.
void compute_nbody_forces_naive(const std::vector<vec3>& positions,
                                const std::vector<f32>& masses,
                                std::vector<vec3>& out_accelerations,
                                const BHParams& params);

} // namespace fuse::physics
