#pragma once

// WP-6.5 radiance cascades (research track, renderer plan 6d; Sannikov, "Radiance Cascades: A Novel Approach
// to Calculating Global Illumination", 2023). Flatland (2D) prototype: the CPU reference of the Vulkan
// kernels (rc_cascade_record / rc_gather_pixel are the line-for-line twins of shaders/rc_cascade.* and
// shaders/rc_gather.*), a brute-force per-pixel ray tracer (the ground truth of the metrics) and the metric
// helpers the gates and report.md use. Vulkan-free: builds and runs in the stub backend.
//
// Cascade layout (RcLayout::compute), cascade i = 0 .. n-1:
//   probe spacing   s_i = s_0 * 2^i                 (probe (x, y) at ((x + 0.5) s_i, (y + 0.5) s_i))
//   directions      N_i = N_0 * b^i                 (angle 2 pi (k + 0.5) / N_i; children of k: bk .. bk + b-1)
//   ray interval    [t_i, t_{i+1}),  t_i = r_0 (b^i - 1) / (b - 1)   (length r_0 b^i)
//   n               the smallest count with t_n >= the scene diagonal (the top cascade merges with the sky)
// with the branching factor b (RcSettings::branch):
//   b = 4  (default; the common 2D choice) every cascade stores ceil(W / s_i) ceil(H / s_i) N_i ~ W H N_0 / s_0^2
//          records (constant memory per cascade); ray length grows 4x per cascade, so the march cost per
//          cascade grows 4x and the top cascades dominate (cost ~ pixels x extent)
//   b = 2  Sannikov's penumbra condition taken literally (angular resolution and interval length both scale
//          with the spacing): records halve per cascade while ray length doubles, so every cascade costs the
//          same and the total is ~ pixels x log(extent); half the angular resolution of b = 4 per cascade
//
// Merge (coarse to fine, cascade n-1 first): record (probe p, direction k) of cascade i
//   vanilla        L = trace(p + d t_i -> p + d t_{i+1}) ; out = L.rgb + L.T * sum_j w_j avg(upper[j], bk..bk+b-1)
//   bilinear fix   out = sum_j w_j (L_j.rgb + L_j.T * avg(upper[j], ...)),  L_j = trace(p + d t_i -> q_j + d t_{i+1})
// with q_j / w_j the 4 bilinear neighbours / weights of p on cascade i + 1's probe grid (indices clamped),
// avg the pre-averaged b child directions, L.T in {0, 1} the ray's transmittance (opaque flatland texels).
// Gather: per pixel, bilinear over cascade 0's probes of the mean over its N_0 directions (the mean radiance,
// = fluence / 2 pi).
//
// Ray march (rc_trace): fixed steps of `step` texels at t = k * step, nearest texel; the first opaque texel
// ends the ray with its emission; outside the grid the ray continues (empty) unless it moves away from it.

#include <fuse/renderer/research/rc/rc_types.hpp>

#include <vector>

namespace fuse::renderer::research::rc {

/// Flatland scene: f32x4 per texel (rgb emission, opacity).
struct RcScene {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> texels; ///< 4 per texel, row-major

    void resize(u32 w, u32 h);
    void clear();
    void set(u32 x, u32 y, f32 r, f32 g, f32 b, bool opaque);
    /// Fills [x0, x1) x [y0, y1) (clipped).
    void fillRect(i32 x0, i32 y0, i32 x1, i32 y1, f32 r, f32 g, f32 b, bool opaque);
    /// Fills the texels whose centre lies within `radius` of (cx, cy).
    void fillDisk(f32 cx, f32 cy, f32 radius, f32 r, f32 g, f32 b, bool opaque);
    bool opaque(u32 x, u32 y) const { return texels[(static_cast<usize>(y) * width + x) * 4u + 3u] > 0.5f; }
};

struct RcSettings {
    u32 probeSpacing0 = 1u;  ///< s_0 in texels (power of two)
    u32 dirs0 = 4u;          ///< N_0
    f32 interval0 = 1.0f;    ///< r_0 in texels (cascade 0 covers [0, r_0))
    f32 step = 0.5f;         ///< march step in texels
    bool bilinearFix = true; ///< false: vanilla (one ray per record, bilinear merge of the far field)
    u32 branch = 4u;         ///< b: 4 (default) or 2 (see the header comment)
    f32 sky[3] = {0.0f, 0.0f, 0.0f};
    u32 cascadeCount = 0u; ///< 0: automatic (reach the scene diagonal); else clamped to [1, kRcMaxCascades]
};

struct RcLayout {
    u32 cascades = 0;
    u32 probesX[kRcMaxCascades] = {};
    u32 probesY[kRcMaxCascades] = {};
    u32 dirs[kRcMaxCascades] = {};
    u32 dirOffset[kRcMaxCascades] = {}; ///< first direction of the cascade in the table
    f32 spacing[kRcMaxCascades] = {};
    f32 tStart[kRcMaxCascades] = {};
    f32 tEnd[kRcMaxCascades] = {};
    u64 records[kRcMaxCascades] = {};
    u32 dirTotal = 0;
    u64 maxRecords = 0;
    u64 totalRecords = 0;

    /// False for an empty extent, a spacing that is not a power of two, zero directions or a non-positive
    /// interval / step.
    static bool compute(const RcSettings& settings, u32 width, u32 height, RcLayout& out);
};

/// Direction table: 4 f32 per direction (cos, sin, 0, 0), cascade after cascade (RcLayout::dirOffset).
void buildDirectionTable(const RcLayout& layout, std::vector<f32>& out);

/// Push constants of cascade `i` (addresses left 0) / of the gather.
RcPush makeCascadePush(const RcLayout& layout, const RcSettings& settings, u32 cascade, u32 width, u32 height);
RcPush makeGatherPush(const RcLayout& layout, const RcSettings& settings, u32 width, u32 height);

struct RcHit {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 t = 1.0f; ///< transmittance (0: hit an opaque texel)
};

struct RcStats {
    u64 rays = 0;
    u64 samples = 0;
    u64 raysPerCascade[kRcMaxCascades] = {};
    u64 samplesPerCascade[kRcMaxCascades] = {};
    void reset() { *this = RcStats{}; }
};

/// Marches from (ax, ay) along the unit direction (dx, dy) over [0, len) (see the header comment).
/// `samples` (optional) counts the texel lookups.
RcHit rc_trace(const f32* scene, u32 width, u32 height, f32 ax, f32 ay, f32 dx, f32 dy, f32 len, f32 step,
               f32 invStep, u64* samples);

/// rc.cascade for record `index` (< push.count): writes 4 f32 to dst + index * 4.
void rc_cascade_record(const RcPush& push, const f32* scene, const f32* dirs, const f32* upper, f32* dst, u32 index,
                       u64* rays, u64* samples);
/// rc.gather for pixel `index` (< push.count): writes 4 f32 to out + index * 4.
void rc_gather_pixel(const RcPush& push, const f32* cascade0, f32* out, u32 index);

/// CPU radiance-cascades solver (reuses its buffers across solves).
class RcCpuSolver {
public:
    /// out: 4 f32 per pixel (mean radiance rgb, 1). False when the layout is invalid.
    bool solve(const RcScene& scene, const RcSettings& settings, std::vector<f32>& out, RcStats* stats = nullptr);
    const RcLayout& layout() const { return m_layout; }

private:
    RcLayout m_layout{};
    std::vector<f32> m_dirs;
    std::vector<f32> m_cascade[2];
};

/// Ground truth: per pixel, `rays` equally spaced directions (angle 2 pi (k + 0.5) / rays) from the pixel
/// centre, marched to the grid exit (then the sky); out: 4 f32 per pixel (mean radiance, 1).
void bruteForce(const RcScene& scene, u32 rays, f32 step, const f32 (&sky)[3], std::vector<f32>& out,
                RcStats* stats = nullptr);

struct RcErrorMetrics {
    f64 relL1 = 0.0;   ///< sum |test - ref| / sum |ref| over masked pixels and rgb
    f64 relRmse = 0.0; ///< sqrt(mean (test - ref)^2) / mean |ref|
    f64 maxAbs = 0.0;
    f64 meanRef = 0.0; ///< mean over masked pixels of (r + g + b) / 3
    f64 meanTest = 0.0;
    u32 pixels = 0;
};

/// Pixels whose `mask` entry is non-zero (mask may be empty: every pixel); test / ref: 4 f32 per pixel.
RcErrorMetrics compareImages(const std::vector<f32>& test, const std::vector<f32>& ref, const std::vector<u8>& mask);

/// Mask of the non-opaque texels (1) of a scene.
std::vector<u8> freeSpaceMask(const RcScene& scene);

/// Mean of (r + g + b) / 3 over the texels whose mask entry is non-zero.
f64 maskedMean(const std::vector<f32>& image, const std::vector<u8>& mask);

/// Angular non-uniformity around (cx, cy): for each integer radius in [r0, r1], the coefficient of variation
/// (stddev / mean of (r + g + b) / 3) over the free pixels whose centre lies within 0.5 of the circle; returns
/// the mean over radii (the ray-shaped "ringing" artefact of undersampled directions shows up here).
f64 ringNonUniformity(const std::vector<f32>& image, const RcScene& scene, f32 cx, f32 cy, u32 r0, u32 r1);

// --- test scenes ------------------------------------------------------------------------------------------
/// Empty scene (sky only).
void sceneEmpty(RcScene& scene, u32 w, u32 h);
/// A disk emitter of radius `radius` at the centre, nothing else.
void sceneDisk(RcScene& scene, u32 w, u32 h, f32 radius);
/// Rooms: 2-texel walls with doorways, a warm area light, a cool small light, blockers (variant 0 .. 3 moves
/// the lights).
void sceneRooms(RcScene& scene, u32 w, u32 h, u32 variant);
/// Leak test: a sealed box (walls `wall` texels thick) with a bright emitter strip hugging its outside;
/// `inside` receives the interior mask (ground truth: exactly 0 there).
void sceneLeakBox(RcScene& scene, u32 w, u32 h, u32 wall, std::vector<u8>& inside);

} // namespace fuse::renderer::research::rc
