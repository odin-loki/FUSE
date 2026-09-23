#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Bloom pass parameters (B5.10 — P5 §5.10).
struct BloomParams {
    f32 threshold = 1.f;
    f32 knee = 0.5f;
    f32 intensity = 0.05f;
    u32 mip_levels = 7;
    f32 scatter = 0.7f;
};

/// Bloom threshold + dual-filter pyramid. The per-pixel helpers and the image pass are the CPU
/// reference for the GPU bloom kernels (same weights, same level combination).
///
/// Threshold: soft knee on Rec.709 luminance. Pixels with luminance <= threshold - knee contribute
/// exactly zero; the contribution ramps quadratically over [threshold - knee, threshold + knee] and
/// is `lum - threshold` (scaled back to RGB) above it. The ramp is C1-continuous at both ends.
///
/// Pyramid: level 0 is the prefiltered full-resolution image; each downsample is a separable
/// [1 3 3 1] / 8 tent over a 4x4 footprint (2x decimation), each upsample is the matching separable
/// bilinear [1/4 3/4] expansion, and levels combine as up[k] = lerp(down[k], upsample(up[k+1]),
/// scatter). All weights are non-negative and normalised, so a frame with no pixel above the knee
/// yields exactly zero bloom and interior energy is conserved.
class Bloom {
public:
    void setParams(const BloomParams& params) { m_params = params; }
    const BloomParams& params() const { return m_params; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    static f32 luminance(const fuse::math::Vec3& rgb);
    /// Soft-knee luminance response in luminance units (0 at/below threshold - knee).
    static f32 thresholdResponse(f32 luminance, const BloomParams& params);
    static fuse::math::Vec3 extractBright(const fuse::math::Vec3& hdr, const BloomParams& params);
    /// Single-pixel composite (no spatial spread) — kept for per-pixel stack evaluation.
    static fuse::math::Vec3 apply(const fuse::math::Vec3& hdr, const BloomParams& params);

private:
    BloomParams m_params{};
    bool m_ready = false;
};

/// Number of pyramid levels actually used for a `width` x `height` image (>= 1).
u32 bloom_level_count(u32 width, u32 height, const BloomParams& params);

/// Prefilter (soft-knee threshold) a full-resolution HDR image.
void bloom_prefilter(const fuse::math::Vec3* hdr, u32 width, u32 height, const BloomParams& params,
                     std::vector<fuse::math::Vec3>& out);

/// 2x tent downsample with clamp-to-edge; output is ceil(width/2) x ceil(height/2).
void bloom_downsample(const std::vector<fuse::math::Vec3>& src, u32 width, u32 height,
                      std::vector<fuse::math::Vec3>& out, u32& out_width, u32& out_height);

/// 2x bilinear upsample from `src` (src_width x src_height) to `dst_width` x `dst_height`.
void bloom_upsample(const std::vector<fuse::math::Vec3>& src, u32 src_width, u32 src_height,
                    u32 dst_width, u32 dst_height, std::vector<fuse::math::Vec3>& out);

/// Full bloom pass: returns the (un-scaled) bloom buffer at full resolution.
void bloom_image(const fuse::math::Vec3* hdr, u32 width, u32 height, const BloomParams& params,
                 std::vector<fuse::math::Vec3>& out_bloom);

/// Bloom composite: out = hdr + bloom * intensity (in place allowed when `out` aliases nothing).
void bloom_composite(const fuse::math::Vec3* hdr, u32 width, u32 height, const BloomParams& params,
                     std::vector<fuse::math::Vec3>& out);

} // namespace fuse::renderer
