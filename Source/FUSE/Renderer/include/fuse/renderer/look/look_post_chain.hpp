#pragma once

// LookPostChain — executes a validated LookEffectGraph over a frame with the single-source look
// kernels (look_kernels.hpp). It runs inside the renderer's PostProcessStack pass, behind the existing
// B5 post stack: DoF and motion blur reuse the existing CPU reference passes (dof_pass /
// motion_blur_pass), auto exposure reuses renderer::AutoExposure, the tone-map curves and mid-grey
// calibration match renderer::ToneMap, and the bloom kernels reproduce renderer::bloom_image bit for bit.
// `apply_look_to_post_stack` feeds the same blended look into a plain PostStack for callers that still
// use PostStack::processFrame.
//
// All buffers are allocated by init(); process() makes no heap allocation of its own (the reused
// dof_pass / motion_blur_pass allocate internally when those nodes actually run).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/postprocess/auto_exposure.hpp>
#include <fuse/types.hpp>

#include <array>
#include <vector>

namespace fuse::renderer {
class PostStack;
}

namespace fuse::renderer::look {

enum class LookOutputEncoding : u8 { Srgb = 0, Hdr10Pq = 1, ScRgb = 2 };
const char* look_output_encoding_name(LookOutputEncoding encoding);

/// Pyramid depth of the look bloom (matches BloomParams::mip_levels' default).
inline constexpr u32 kLookBloomLevels = 7u;

struct LookChainConfig {
    u32 width = 1;
    u32 height = 1;
    LookOutputEncoding encoding = LookOutputEncoding::Srgb;
    /// Display capability; 0 = trust the look's outputTransform.peakNits. The effective peak is the min.
    f32 display_peak_nits = 0.f;
    kernel::Backend backend = kernel::Backend::CpuParallel;
};

struct LookChainInput {
    const math::Vec3* hdr = nullptr;           ///< width * height scene-linear HDR (display resolution)
    const f32* linear_depth_m = nullptr;       ///< optional (enables depthOfField)
    const math::Vec2* velocity_px = nullptr;   ///< optional (enables motionBlur)
    u64 frame_seed = 0;                        ///< film grain seed; 0 disables grain
    f32 delta_seconds = 1.f / 60.f;            ///< auto-exposure adaptation step
};

struct LookChainStats {
    std::array<LookEffect, kLookEffectCount> executed{};
    u32 executed_count = 0;
    f32 exposure_ev = 0.f;    ///< total EV applied by the exposure node
    f32 auto_exposure_ev = 0.f;
    f32 hdr_lmax = 0.f;       ///< HDR shoulder asymptote (peak / paper white), 0 for SDR
    u32 bloom_levels = 0;
    bool sharpen_hook_used = false;
    bool executed_effect(LookEffect e) const {
        for (u32 i = 0; i < executed_count; ++i) {
            if (executed[i] == e) {
                return true;
            }
        }
        return false;
    }
};

/// Optional sharpening provider (e.g. the upscaler module's AMD CAS port). Return false to fall back
/// to the built-in look_sharpen kernel.
using LookSharpenHook = bool (*)(const math::Vec3* src, math::Vec3* dst, u32 width, u32 height, f32 sharpness,
                                 void* user);

/// Deterministic procedural lens-dirt mask in [0, 1] (soft smudges + faint streaks) for looks that do
/// not supply a texture.
void generate_procedural_lens_dirt(u64 seed, u32 width, u32 height, std::vector<f32>& out);

class LookPostChain {
public:
    bool init(const LookChainConfig& config);
    void destroy();
    bool ready() const { return m_ready; }
    const LookChainConfig& config() const { return m_config; }
    void setBackend(kernel::Backend backend) { m_config.backend = backend; }
    void setEncoding(LookOutputEncoding encoding, f32 display_peak_nits = 0.f) {
        m_config.encoding = encoding;
        m_config.display_peak_nits = display_peak_nits;
    }

    /// Copies a dirt mask (any resolution, row-major, [0, 1]).
    bool setLensDirtTexture(const f32* texels, u32 width, u32 height);
    void setSharpenHook(LookSharpenHook hook, void* user) {
        m_sharpenHook = hook;
        m_sharpenUser = user;
    }

    AutoExposure& autoExposure() { return m_autoExposure; }

    /// Runs `graph` (must validate) over `input` with the blended `look` and baked `grade_lut`, writing
    /// width * height output-encoded pixels to `out`.
    bool process(const LookChainInput& input, const LookEffectGraph& graph, const LookResolved& look,
                 const Lut3D& grade_lut, math::Vec3* out);

    const LookChainStats& lastStats() const { return m_stats; }
    /// Full-resolution bloom of the last frame (valid when the bloom node ran).
    const std::vector<math::Vec3>& bloomBuffer() const { return m_bloom; }
    const std::vector<math::Vec3>& flareBuffer() const { return m_flare; }
    u32 flareWidth() const { return m_levels > 1u ? m_levelW[1] : 0u; }
    u32 flareHeight() const { return m_levels > 1u ? m_levelH[1] : 0u; }

private:
    template <typename Kernel, typename Params>
    void run(const Params& params, kernel::Dim3 grid);
    void runBloom(const LookResolved& look);

    LookChainConfig m_config{};
    bool m_ready = false;
    std::vector<math::Vec3> m_a, m_b;         ///< ping-pong colour
    std::vector<math::Vec3> m_tmp;            ///< separable-pass intermediate
    std::vector<math::Vec3> m_upA, m_upB;     ///< upsample chain
    std::vector<math::Vec3> m_bloom;          ///< full-res bloom result
    std::vector<math::Vec3> m_down[kLookBloomLevels];
    u32 m_levelW[kLookBloomLevels] = {};
    u32 m_levelH[kLookBloomLevels] = {};
    u32 m_levels = 0;
    std::vector<math::Vec3> m_flare;
    std::vector<f32> m_dirt;
    u32 m_dirtW = 0, m_dirtH = 0;
    std::vector<math::Vec3> m_spatialScratch; ///< dof_pass / motion_blur_pass output
    AutoExposure m_autoExposure{};
    LookSharpenHook m_sharpenHook = nullptr;
    void* m_sharpenUser = nullptr;
    LookChainStats m_stats{};
    bool m_bloomValid = false;
};

/// Feeds a blended look into the existing post stack (bloom, DoF, motion blur, auto exposure, tone
/// mapper, lift/gamma/gain/saturation/contrast, vignette, grain).
void apply_look_to_post_stack(const LookResolved& look, PostStack& stack);

} // namespace fuse::renderer::look
