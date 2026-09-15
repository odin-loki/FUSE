#pragma once

#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/math.hpp>
#include <fuse/types.hpp>

namespace fuse::terrain {

struct HeightfieldRayHit {
    bool hit = false;
    vec3 position{};
    vec3 normal{};
    f32 distance = 0.f;
};

/// Sample height at world XZ using the heightfield (alias for Heightfield::sample_height).
[[nodiscard]] f32 sample_height(const Heightfield& field, f32 world_x, f32 world_z);

/// March a ray against the heightfield surface (CPU stub, grid-stepping).
[[nodiscard]] HeightfieldRayHit raycast_heightfield(const Heightfield& field, vec3 origin, vec3 direction,
                                                    f32 max_distance);

} // namespace fuse::terrain
