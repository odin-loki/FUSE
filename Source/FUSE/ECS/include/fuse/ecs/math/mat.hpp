#pragma once

#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

mat4 from_trs(const vec3& position, const quat& rotation, const vec3& scale);
mat4 multiply(const mat4& a, const mat4& b);
mat4 inverse_affine(const mat4& matrix);
mat4 perspective(f32 fov_deg, f32 aspect, f32 near_plane, f32 far_plane);
mat4 look_at(const vec3& eye, const vec3& target, const vec3& up);
vec3 transform_point(const mat4& matrix, const vec3& point);

inline mat4 operator*(const mat4& a, const mat4& b) { return multiply(a, b); }

} // namespace fuse::ecs
