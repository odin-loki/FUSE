#pragma once

#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/types.hpp>

namespace fuse::ssfx {

/// Horizon-based ambient occlusion parameters (B5.7 — mirrors `HBAOParams` in the plan).
struct HbaoParams {
    /// World-space sample radius; occluders further than this from the shaded point are ignored.
    f32 radius = 1.f;
    /// Samples within asin(bias) of the tangent plane are ignored — suppresses self-occlusion (0 = none).
    f32 bias = 0.05f;
    /// Screen-space horizon searches per pixel (two per slice).
    u32 directions = 8;
    u32 steps_per_dir = 4;
    /// Visibility exponent (1 = physically based cosine-weighted AO).
    f32 strength = 1.f;
    /// Screen-space clamp on the projected radius.
    f32 max_radius_px = 64.f;
    /// Weight each slice by the angle around the view vector it covers (off-centre pixels).
    bool azimuth_weighting = true;
};

/// Clamp HBAO knobs to safe ranges.
HbaoParams clampHbaoParams(const HbaoParams& raw);

/// CPU reference of the `hbao_kernel` for one pixel — returns cosine-weighted visibility in [0, 1]
/// (1 = unoccluded). For `directions / 2` view-aligned slices the horizon is searched in screen space on both
/// sides (in-radius samples only); each slice's visibility is the exact cosine-weighted integral between its
/// two horizons using the normal projected into the slice (GTAO form). Exact for the depth-buffer height-field
/// that screen-space AO assumes, so it converges to the brute-force hemisphere reference below.
f32 hbaoPixelVisibility(const SsfxGBufferView& view, const HbaoParams& params, u32 x, u32 y);

/// Full-frame CPU HBAO; writes `width * height` visibility values (1 for sky pixels). False on bad input.
bool computeHbaoCpu(const SsfxGBufferView& view, const HbaoParams& params, f32* visibilityOut);

/// Brute-force SSAO reference: `sampleSqrt^2` stratified cosine-weighted hemisphere rays per pixel, each
/// marched `marchSteps` times up to `radius` and tested against the same depth buffer (infinite-thickness
/// height-field, identical to HBAO's assumption). Returns visibility in [0, 1].
f32 ssaoHemisphereReferenceVisibility(const SsfxGBufferView& view, u32 x, u32 y, f32 radius, u32 sampleSqrt = 16u,
                                      u32 marchSteps = 48u);

} // namespace fuse::ssfx
