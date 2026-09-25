#pragma once

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/math/vec.hpp>

namespace fuse::spatial {

using Frustum = fuse::ecs::Camera::Frustum;

Frustum extract_frustum(const fuse::ecs::mat4& view_projection);
bool test_aabb_frustum(const Frustum& frustum, const fuse::ecs::vec3& aabb_min,
                       const fuse::ecs::vec3& aabb_max);
bool test_sphere_frustum(const Frustum& frustum, const fuse::ecs::vec3& center, fuse::f32 radius);

} // namespace fuse::spatial
