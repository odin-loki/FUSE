#pragma once

// Screen-space radiance cascades (SSRC; the WP-6.5 recommendation: a 2.5D radiance-cascades variant over the depth
// buffer for near-field diffuse GI + AO on T0, beside DDGI which feeds the rays that leave the screen). CPU
// reference of the Vulkan kernels (ssrc_cascade_record / ssrc_gather_pixel are the line-for-line twins of
// shaders/ssrc/src_cascade.* / src_gather.*: the same f32 operations in the same order, IEEE-exact operations
// only, so CPU and GPU agree bit for bit), a brute-force screen-space ray marcher on the same depth-buffer model
// (the ground truth of the cascades), and the metrics. Vulkan-free: builds and runs in the stub backend.
//
// Cascades (the flatland layout of research/radiance_cascades with 2D screen-space probes and 3D directions):
//   cascade i = 0 .. n-1
//   probes       spacing s_i = s_0 2^i pixels; probe (x, y) sits on pixel floor((x + 0.5) s_i, (y + 0.5) s_i)
//                (clamped to the image), at that pixel's view position, pushed originBias x depth along the pixel's
//                camera-facing normal; a probe on a sky pixel is invalid (records = far field; zero merge weight)
//   directions   the (R_0 2^i)^2 texel centres of an octahedral map of the whole sphere (view space), R_0 = 4 by
//                default (N_0 = 16); the children of texel (u, v) are the 2 x 2 texels (2u.., 2v..) of cascade i+1,
//                so the branching factor is b = 4 and each cascade stores ~ W H N_0 / s_0^2 records (constant)
//   intervals    screen-space distance along the projected ray: [t_i, t_{i+1}), t_i = r_0 (4^i - 1) / 3 pixels,
//                r_0 = 2 s_0 by default; n = the smallest count with t_n >= the screen diagonal (<= 8)
// A ray of direction d from a probe origin O runs to E = O + d L (L = maxDistance, clipped 1% in front of the
// near plane); its projection is a straight screen segment of Euclidean length len, and the point at screen
// distance t is the perspective-correct point at fraction t / len (1/z and p/z are linear in screen space).
//
// Depth-buffer thickness model (the "2.5D" part): every depth pixel h with linear depth z_h > 0 is a surfel slab:
// the tangent plane through its centre point (depth z_h, G-buffer normal n_h) extruded thickness + thicknessSlope z_h
// away from the camera along the view rays; sky pixels are empty. The surfel depth s_h(x, y) at a screen point is
// the plane's depth on that view ray (its ratio to z_h clamped to [0.5, 2]), so a plane is represented exactly and
// a ray leaving a plane never re-hits it (no self-occlusion from comparing depths at different screen positions).
// A segment A -> B is marched in screen space with steps of `stride` pixels along its major axis (DDA-like,
// nearest pixel, perspective-correct ray depth); at the step landing on pixel h (the probe's own pixel is skipped),
// with d0 / d1 the ray depth minus s_h at the previous / current screen point, it hits when
//   0 <= d1 <= th (inside the slab), or d0 < 0 && d1 > th (stepped over it away from the camera), or
//   d0 > th && d1 < 0 (stepped over it towards the camera).
// A hit returns the pixel's lit radiance when the pixel's normal faces the ray (0 for a back face: it still
// occludes). A step that leaves the screen ends the ray ("escaped"). Consequences (bounds measured by the gates):
//   - geometry hidden behind the first depth layer occludes only within the slab: light passing behind a wall whose
//     extent along the view ray exceeds the slab (without crossing its visible surface) leaks; inherent to screen
//     space, the brute force leaks identically (fuse_rp_ssrc_leak: slab 0.1 vs 0.5 m walls: brute force 3.2%,
//     SSRC 4.9% of the outside light). A slab thicker than the real object adds false occlusion behind it.
//   - with stride <= 1 pixel every column (row) of the segment's major axis is visited, and the crossing clauses
//     catch rays that pass a slab between two steps.
// Record (cascade i, probe p, direction k): trace the interval; hit -> (hit radiance, v = 0); escaped, or the ray
// ends (its projection shorter than t_{i+1}) -> (far field(d), v = 1); else merge with cascade i + 1:
//   bilinear fix (default)  out = sum_j w_j value_j / sum_j w_j over the valid bilinear parents q_j (sky parents
//                           get weight 0): value_j = trace(A -> B_j) where A = the child's point at t_i and B_j the
//                           parent's point at t_{i+1} along the same d (the child's own end point when the parent's
//                           ray ends before it); continuing -> the parent's child-average in direction k
//   vanilla                 one segment A -> B (the child's own interval), continuing -> sum_j w_j avg_j / sum_j w_j
//   top cascade             continuing -> far field
// Horizon awareness (probes sit on surfaces; without it a surface lights itself through the bins straddling its
// horizon, +22% bias in the room): the child average runs over the children in the receiving probe's hemisphere
// (solid-angle weights; on cascade 0, whose records only the gather reads, solid angle x cosine, so a bin's gathered
// contribution is the cosine quadrature over its children); a cascade-0 bin centred below the probe's horizon is not
// traced (merge only). Optional plane test (planeTolerance): parents off the child's tangent plane are dropped.
// v (the AO channel) is forced to 1 for cascades >= aoCascades, so AO = the cosine-weighted fraction of directions
// that hit within the screen distance tEnd[aoCascades - 1]. Far field = sky + ddgiScale x E_ddgi(probe position,
// ray direction) (the DDGI hook; world space through viewToWorld / cameraWorld).
// Gather (per pixel): bilinear over cascade 0's valid probes (s_0 = 1: the pixel's own probe); bin weight c_k = the
// cosine-weighted solid angle of bin k summed over its 4 cascade-1 children; E / pi = sum_k c_k L_k / sum_k c_k (a
// white furnace is exact), AO = sum_k c_k v_k / sum_k c_k; indirect = intensity x diffuse albedo x E / pi (the SSGI
// convention: outgoing indirect radiance).
// Compose (optional): lit + indirect + ambient x albedo x material AO x (AO - 1) (the WP-6.3 AO rule), >= 0.
// Error vs the brute force on the same model (fuse_rp_ssrc_error, SSFX room 128 x 96, 576 rays/px reference):
// relative L1 <= 0.10 (measured 0.068-0.069, mean bias +2%), mean |AO error| <= 0.03 (0.021); with a sky far field
// relative L1 <= 0.03 (0.012).
// Records are f16 (software round-to-nearest-even encode / exact decode on both sides; values clamped to 65504).

#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/renderer/ssrc/ssrc_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::ddgi_kernel {
struct VolumeView;
} // namespace fuse::renderer::ddgi_kernel

namespace fuse::renderer::ssrc {

struct SsrcSettings {
    u32 probeSpacing0 = 1u; ///< s_0 in pixels (power of two, <= 64)
    u32 dirRes0 = 4u;       ///< R_0: cascade 0 has R_0^2 octahedral directions (N_0 = 16)
    f32 interval0 = 0.f;    ///< r_0 in pixels; 0 = 2 s_0
    bool bilinearFix = true;
    u32 cascadeCount = 0u; ///< 0: automatic (reach the screen diagonal); else clamped to [1, kSsrcMaxCascades]
    u32 aoCascades = 2u;   ///< 0: every cascade; else clamped to [1, cascades]
    f32 stride = 1.f;      ///< >= 0.25
    f32 thickness = 0.5f;
    f32 thicknessSlope = 0.f;
    f32 maxDistance = 10.f;
    f32 originBias = 0.f;
    /// Plane-aware merge (optional): a bilinear parent whose tangent plane is farther than planeTolerance x depth from
    /// the child probe is dropped (all valid parents are used when none passes); 0 = plain bilinear weights over the
    /// valid parents (default: the plane test extrapolates from the remaining parents and over-brightens receivers
    /// next to a bright surface, +21% vs +3.5% in fuse_rp_ssrc_leak, for ~3% lower error in the room).
    f32 planeTolerance = 0.f;
    f32 intensity = 1.f;
    f32 sky[3] = {0.f, 0.f, 0.f};
    /// DDGI far field (needs a volume: SsrcView::ddgi on the CPU, SsrcFrameImages::ddgiVolume on the GPU).
    bool ddgi = false;
    f32 ddgiScale = 0.31830988618f; ///< 1 / pi: the volume's E / pi as the far-field radiance
    bool compose = false;
};

/// Per-cascade record counts of a resolved frame.
struct SsrcLayoutInfo {
    u64 records[kSsrcMaxCascades] = {};
    u64 maxRecords = 0;
    u64 totalRecords = 0;
    u32 dirTotal = 0;
};

/// Fills the extent, flags, march / far-field parameters and the cascade layout of `out` (camera, rotations and
/// addresses untouched). False for an empty extent or invalid settings.
bool resolve_layout(const SsrcSettings& settings, u32 width, u32 height, SsrcFrameConstants& out,
                    SsrcLayoutInfo* info = nullptr);

/// resolve_layout + camera intrinsics (ssfx_gpu::camera_from_projection), world <-> view rotations, camera position
/// and ambient. False on an invalid camera or layout.
bool resolve_constants(const SsrcSettings& settings, const ssfx_gpu::SsfxCameraDesc& camera, const f32 (&ambient)[3],
                       u32 width, u32 height, SsrcFrameConstants& out, SsrcLayoutInfo* info = nullptr);

/// Direction table of a resolved layout: 4 f32 per direction (dx, dy, dz, solid angle), cascade after cascade.
void buildDirectionTable(const SsrcFrameConstants& c, std::vector<f32>& out);

// --- f16 records ---------------------------------------------------------------------------------------------
/// f32 -> f16 bits, round to nearest even (the kernels' software conversion; magnitudes clamped to 65504 first).
u32 f32ToF16(f32 v);
/// f16 bits -> f32 (exact).
f32 f16ToF32(u32 h);

// --- kernels ----------------------------------------------------------------------------------------------------
/// The frame's inputs as the kernels read them (4 f32 per pixel each; the work-buffer sections).
struct SsrcView {
    const f32* geo = nullptr;
    const f32* lit = nullptr;
    const f32* diffuse = nullptr;
    const f32* albedo = nullptr;
    const f32* dirs = nullptr; ///< buildDirectionTable
    const ddgi_kernel::VolumeView* ddgi = nullptr;
};

/// SsrcSegment::state
enum SsrcSegmentState : u32 { kSsrcHit = 0u, kSsrcContinue = 1u, kSsrcEscaped = 2u };

struct SsrcSegment {
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    u32 state = kSsrcContinue;
    f32 f = 1.f; ///< fraction of A -> B at the hit step
};

struct SsrcCounters {
    u64 samples = 0;  ///< depth-buffer lookups (march steps)
    u64 segments = 0; ///< traced segments
};

/// Marches the view-space segment a -> b (see the header comment). `startPixel` (y * width + x) is skipped.
SsrcSegment trace_segment(const SsrcFrameConstants& c, const SsrcView& v, const f32 (&a)[3], const f32 (&b)[3],
                          u32 startPixel, SsrcCounters* counters);

/// ssrc.cascade for record `index` of cascade `cascade`: writes 2 u32 to dst + index * 2. `upper` = cascade + 1's
/// records (ignored on the top cascade).
void ssrc_cascade_record(const SsrcFrameConstants& c, const SsrcView& v, u32 cascade, const u32* upper, u32* dst,
                         u32 index, SsrcCounters* counters);
/// ssrc.gather for pixel `index`: writes 4 f32 to indirect + index * 4 (rgb, AO) and 4 f32 to image + index * 4
/// (the output image's value: the composed lit image or indirect + AO).
void ssrc_gather_pixel(const SsrcFrameConstants& c, const SsrcView& v, const u32* records0, f32* indirect, f32* image,
                       u32 index);

// --- frame-level CPU solver ---------------------------------------------------------------------------------
/// The prepared inputs of a frame (4 f32 per pixel each), e.g. read back from the GPU or from inputsFromPrepared.
struct SsrcInputs {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> geo;
    std::vector<f32> lit;
    std::vector<f32> diffuse;
    std::vector<f32> albedo;
};

/// SsrcInputs from WP-6.3's prepared frame (ssfx_gpu::prepare_pixel over a G-buffer: the ssrc.prepare oracle).
void inputsFromPrepared(const ssfx_gpu::SsfxPreparedFrame& in, SsrcInputs& out);

struct SsrcStats {
    u64 samples = 0;
    u64 segments = 0;
    u64 samplesPerCascade[kSsrcMaxCascades] = {};
    u64 records = 0;
};

class SsrcCpuSolver {
public:
    /// Runs every cascade (top first) and the gather. indirect / image: 4 f32 per pixel. False on bad input.
    bool solve(const SsrcFrameConstants& c, const SsrcInputs& in, const ddgi_kernel::VolumeView* ddgi,
               std::vector<f32>& indirect, std::vector<f32>& image, SsrcStats* stats = nullptr);
    const std::vector<f32>& dirs() const { return m_dirs; }
    /// The ping-pong record sections after solve (cascade i in [i & 1]).
    const std::vector<u32>& records(u32 section) const { return m_records[section & 1u]; }

private:
    std::vector<f32> m_dirs;
    std::vector<u32> m_records[2];
};

/// Ground truth on the same depth-buffer model: per geometry pixel, raysSqrt^2 stratified cosine-weighted rays
/// (ssfx::ssgi_kernel::sample_direction around the facing normal) from the same origin, each marched over its
/// whole length with trace_segment; hit -> hit radiance, escaped / ended -> far field. indirect: 4 f32 per pixel
/// (intensity x diffuse x E / pi, AO = fraction of rays hitting within tEnd[aoCascades - 1] screen pixels).
void bruteForce(const SsrcFrameConstants& c, const SsrcInputs& in, const ddgi_kernel::VolumeView* ddgi, u32 raysSqrt,
                std::vector<f32>& indirect, SsrcStats* stats = nullptr);

struct SsrcErrorMetrics {
    f64 relL1 = 0.0;   ///< sum |test - ref| / sum |ref| over masked pixels and rgb
    f64 relRmse = 0.0; ///< sqrt(mean (test - ref)^2) / mean |ref|
    f64 maxAbs = 0.0;
    f64 meanRef = 0.0; ///< mean (r + g + b) / 3
    f64 meanTest = 0.0;
    f64 aoMeanAbs = 0.0; ///< mean |ao_test - ao_ref| (channel 3)
    u32 pixels = 0;
};

/// test / ref: 4 f32 per pixel; pixels with mask == 0 are skipped (empty mask: every pixel).
SsrcErrorMetrics compareIndirect(const std::vector<f32>& test, const std::vector<f32>& ref, const std::vector<u8>& mask);
/// 1 where the geo depth is > 0.
std::vector<u8> geometryMask(const SsrcInputs& in);

} // namespace fuse::renderer::ssrc
