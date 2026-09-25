#pragma once

// RL-5.5 radiance denoiser: an analytic test scene with a converged reference for the fuse_rp_rdn_* gates (host only,
// Vulkan-free). It stands in for the path tracer where the gates need exact truth, per-frame camera motion and
// controlled lighting changes; the Relight gates (rl_denoise_*) run the real path tracer.
//
// World (y up, metres): a mirror-like floor (y = 0, roughness floorRoughness) reflecting a striped diffuse wall
// (z = -10) and a diffuse occluder quad (z = 1, x in [ox, ox + 1.2], y in [0.3, 1.5]) moving along +x by
// occluderSpeed per frame (its floor shadow moves with it); the sky beyond is depth 0. The camera at
// (camSpeed t, 1.2, 6) looks along (0, -0.15, -1) with tan(fov / 2) = 0.6 (x aspect horizontally): a moving-camera
// mirror scene - the reflection of the wall moves with the virtual image's parallax, not with the floor.
// Channels (demodulated, i.e. illumination; rgb + hit distance of the continuation):
//   diffuse   smooth irradiance per surface x the occluder's soft shadow on the floor
//   specular  floor: the radiance of the reflected ray (wall stripes / occluder / sky), hit distance = its length;
//             wall / occluder: a constant glossy term (hit distance 5)
// Noise per pixel, frame and seed: diffuse x Exp(1) (heavy tail), specular x U[0.5, 1.5] on the mirror, x 2U
// elsewhere; motion = previous - current UV of the surface point (moving with the occluder); instance ids 1 wall,
// 2 floor, 3 occluder. From lightStepFrame on every light is scaled by lightStepScale.
// A-SVGF gradient samples (the Relight producer's semantics): per 3 x 3 stratum one pixel q (hash of seed, frame,
// stratum); the previous camera's ray through q shaded with q's previous-frame random number in the previous and in
// the current scene state (dCur, dPrev, sCur, sPrev: luminances of the demodulated channels; dPrev = -1 on frame 0).

#include <fuse/renderer/denoise/radiance_denoise.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::denoise {

struct RdnSyntheticDesc {
    u32 width = 96;
    u32 height = 64;
    f32 camSpeed = 0.05f; ///< camera x per frame
    f32 occluderSpeed = 0.06f; ///< occluder x per frame
    f32 occluderStart = -2.f;
    bool occluder = true;
    f32 floorRoughness = 0.f;
    u32 lightStepFrame = ~0u;
    f32 lightStepScale = 0.25f;
    u32 firefliesPerMille = 0; ///< per mille of pixels with a x fireflyScale diffuse outlier (firefly gate)
    f32 fireflyScale = 200.f;
};

enum RdnSyntheticSurface : u8 { kRdnSky = 0, kRdnWall = 1, kRdnFloor = 2, kRdnOccluder = 3 };

struct RdnSyntheticFrame {
    u32 width = 0;
    u32 height = 0;
    std::vector<rdnk::float4> diffuse; ///< noisy (rgb, hit distance)
    std::vector<rdnk::float4> specular;
    std::vector<rdnk::float4> truthD; ///< converged (rgb, 0)
    std::vector<rdnk::float4> truthS;
    std::vector<rdnk::float4> normal; ///< (n, roughness)
    std::vector<f32> depth;
    std::vector<rdnk::float2> motion; ///< previous - current UV (RdnSettings::motionScale = +1)
    std::vector<u32> instance;
    std::vector<u8> surface; ///< RdnSyntheticSurface
    std::vector<rdnk::float4> gradient; ///< per stratum
    RdnCamera camera{};
    RdnCamera prevCamera{};

    RdnReferenceInputs inputs(bool gradients, bool instances) const;
};

/// Frame `t` of the sequence (noise from `seed`). Allocates on the first call / an extent change only.
void rdn_synthetic_frame(const RdnSyntheticDesc& desc, u32 t, u32 seed, RdnSyntheticFrame& out);

} // namespace fuse::renderer::denoise
