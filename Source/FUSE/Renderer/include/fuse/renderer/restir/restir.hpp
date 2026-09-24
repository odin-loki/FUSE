#pragma once

// WP-7.2 ReSTIR DI and GI (renderer plan Phase 7; execution doc WP-7.2): settings, the frame constants both the
// GPU passes (RestirGpu, restir_gpu.hpp) and the CPU runner (RestirCpu, restir_reference.hpp) consume, and the
// per-emitter RGB table (RestirLight) that goes with a WP-7.1 LightTree. Vulkan-free; builds in the stub backend.
//
//   std::vector<LightTreeLight> lights = ...;          // WP-7.1 input list (selection importance)
//   std::vector<RestirLight> table;                    // RGB emission, same order / index as the tree emitters
//   for (...) table.push_back(makeRestirLight(lights[i], rgb[i]));
//   RestirSettings settings;                           // restirSanitize() clamps every field
//   settings.unbiased = true;                          // Talbot MIS with visibility-tested target pdfs
//
// Algorithm and conventions: restir_kernel.hpp.

#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/restir/restir_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer::restir {

struct RestirSettings {
    bool di = true;                 ///< ReSTIR DI chain
    bool gi = true;                 ///< ReSTIR GI chain
    bool unbiased = false;          ///< Talbot / generalized-balance MIS with visibility-tested targets
    bool visibilityReuse = true;    ///< DI initial: zero W of an occluded candidate
    bool diTemporal = true;
    bool giTemporal = true;
    u32 diCandidates = 8;           ///< 1..kRestirMaxCandidates
    u32 diSpatialIterations = 1;    ///< 0..kRestirMaxSpatial
    u32 diNeighbors = 3;            ///< 1..kRestirMaxNeighbors
    f32 diRadius = 16.f;            ///< pixels
    f32 diMCap = 20.f;              ///< history M <= cap x canonical M
    u32 giSpatialIterations = 1;
    u32 giNeighbors = 3;
    f32 giRadius = 16.f;
    f32 giMCap = 20.f;
    f32 giJacobianClamp = 10.f;     ///< biased GI only (0: no clamp)
    bool giShadeVisibility = true;  ///< GI shade traces the reconnection (always visible in the unbiased mode)
    f32 normalThreshold = 0.9f;
    f32 depthThreshold = 0.1f;
    f32 normalBias = 1.0e-3f;
    f32 viewBias = 1.0e-4f;
    f32 farDistance = 1.0e4f;
    f32 giRayTMin = 1.0e-3f;
    u32 cullMask = 0x3u;            ///< rt::kRtMaskVisible | rt::kRtMaskShadow (ANDed with rt::kRtMaskAll)
};

/// Every field clamped to its documented range.
RestirSettings restirSanitize(const RestirSettings& s);

/// Camera of the frame (the G-buffer's: Vulkan clip, forward z/w).
struct RestirCamera {
    f32 viewProj[16] = {};          ///< column-major
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 forward[3] = {0.f, 0.f, -1.f}; ///< view axis (normalised on use)
};

/// The per-frame scalars of RestirFrameConstants (addresses and bindless handles left 0).
struct RestirFrameParams {
    u32 width = 0;
    u32 height = 0;
    u32 frameIndex = 0;
    u32 seed = 0x5EEDu;
    u32 lightCount = 0;
    bool history = false; ///< the previous frame's surfaces / reservoirs are valid
    bool motion = false;  ///< a motion buffer is supplied
};

/// Fills `out` (addresses / handles 0). False for a zero extent or a singular view-projection.
bool buildRestirFrameConstants(const RestirSettings& settings, const RestirCamera& camera, const RestirFrameParams& params,
                               RestirFrameConstants& out);

/// RGB table row of one light-tree input light: `rgb` = emitted radiance (area kinds), radiant intensity
/// (point / spot) or irradiance (directional); the spot cone from the light.
RestirLight makeRestirLight(const light_tree::LightTreeLight& light, const f32 (&rgb)[3]);

/// Column-major 4x4 inverse in double (false when singular).
bool restirInvert(const f32 in[16], f64 out[16]);

} // namespace fuse::renderer::restir
