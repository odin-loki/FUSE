#pragma once

#include <fuse/compute/screen_space_effects.hpp>

namespace fuse::compute {

/// Modulate SSR roughness toward a floor as the ray hit approaches the surface (contact hardening).
f32 ssr_contact_harden_roughness(f32 ray_hit_distance, f32 material_roughness, const SSRParams& params);

/// Cross-bilateral weight for the HBAO blur pass (P5 §5.7 `hbao_blur_kernel` stub).
f32 ssao_blur_weight(f32 center_depth,
                     f32 neighbor_depth,
                     f32 center_normal_z,
                     f32 neighbor_normal_z,
                     const SSAOParams& params);

/// Contact-aware AO visibility multiplier for concave corners.
f32 ssao_contact_ao_weight(f32 depth_delta, f32 normal_similarity, const SSAOParams& params);

/// Screen-edge fade factor for SSR confidence (0 at border, 1 in interior).
f32 ssr_screen_edge_fade(f32 uv_x, f32 uv_y, const SSRParams& params);

/// Reject invalid launch parameters before host/CUDA dispatch.
bool validate_ssao_params(const SSAOParams& params);
bool validate_ssr_params(const SSRParams& params);

} // namespace fuse::compute
