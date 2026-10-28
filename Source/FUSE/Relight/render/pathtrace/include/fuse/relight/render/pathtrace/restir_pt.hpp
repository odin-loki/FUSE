// FUSE Relight: bounded CPU ReSTIR PT initial reservoir (Lin et al. 2022 is the hybrid-shift "Ultra" preset;
// that shift is not implemented here, and nothing in the frame renderer calls this).
//
// One reservoir per pixel for the second vertex of a diffuse path:
//   primary ray (the path tracer's camera) -> diffuse hit x1 -> one cosine-hemisphere direction -> ptTraceScene
//   (the same CPU scene ray the ReSTIR GI core uses; kernels/pt_reference_core.h).
// Target: p_hat = throughput * Le at the second hit, and 0 when that ray misses.
// throughput is the Lambertian weight of the cosine sample at x1: f = albedo/pi, pdf = cos(theta)/pi, so
// f * cos / pdf = albedo. Le is the hit's emission (unlit surfaces emit their base colour, as the path tracer does).
//
// The reservoir stores that direction, the hit distance, p_hat and M (the sample count). A miss stores M = 0.
//
// temporal_reuse(previous, current, pixel) merges the previous pixel's reservoir into the current one with
// balance-heuristic MIS when the previous second-vertex hit is still valid, and caps M at `mCap`:
//   * If both reservoirs carry a primary hit id — the (instance slot, primitive) pair ptTraceScene reports,
//     which is the id the compiled scene provides — the previous sample is eligible only when the ids match.
//   * Otherwise (no id on at least one reservoir) it is eligible only when the camera origin and the primary
//     hit position each moved by strictly less than kRestirPtMotionEpsilon (1e-3 world units, Euclidean).
// There is no reprojection and no reconnection shift: the selected sample keeps the direction it was traced with.
// A current miss stays M = 0 (the previous reservoir is not copied onto a pixel that missed).
#pragma once

#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <vector>

namespace fuse::relight::render::pathtrace {

/// Euclidean motion test for temporal_reuse when the scene did not provide a primary hit id.
/// Strictly less than this (world units) on both the camera origin and the primary hit position.
inline constexpr float kRestirPtMotionEpsilon = 1e-3f;

/// Initial second-vertex sample, plus the primary-hit fields temporal_reuse uses to accept history.
struct RestirPtReservoir {
    float direction[3] = {0.f, 0.f, 0.f}; ///< world direction of the diffuse continuation
    float hitDistance = 0.f;              ///< distance x1 -> x2 (0 on a miss)
    float pHat[3] = {0.f, 0.f, 0.f};      ///< throughput * Le (0 on a miss)
    float M = 0.f;                        ///< sample count (0 on a miss)

    float pdf = 0.f; ///< cosine-hemisphere density of `direction` (cos(theta) / pi)
    float camera[3] = {0.f, 0.f, 0.f};
    float primaryPos[3] = {0.f, 0.f, 0.f};
    u32 primaryInstance = 0xFFFFFFFFu;
    u32 primaryPrimitive = 0xFFFFFFFFu;
    bool hasPrimaryId = false; ///< ptTraceScene reported an (instance, primitive) id for x1
    bool secondHit = false;    ///< the continuation hit a surface
};

/// CPU initial reservoirs for one frame. Not used by the frame renderer.
class RestirPtCpu {
public:
    /// One cosine-hemisphere continuation per pixel. False when `scene` is invalid or the image is empty.
    /// A size change replaces the buffer. `settings.flags` honours kPtFlagJitter; the continuation is still
    /// one cosine sample (not a full BSDF sample).
    bool initial(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
                 u32 sampleBase = 0);

    const std::vector<RestirPtReservoir>& reservoirs() const { return m_reservoirs; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

private:
    std::vector<RestirPtReservoir> m_reservoirs;
    u32 m_width = 0;
    u32 m_height = 0;
};

/// MIS-merge `previous` into `current` for `pixel` (the pixel index seeds the draw). See the file comment.
/// `mCap` is the maximum merged M (the count is clamped, the stored p_hat stays the selected sample's target).
RestirPtReservoir temporal_reuse(const RestirPtReservoir& previous, const RestirPtReservoir& current, u32 pixel,
                                 float mCap = 20.f);

} // namespace fuse::relight::render::pathtrace
