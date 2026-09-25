#pragma once

// RL-5.5 radiance denoiser (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.5; extends WP-6.4 additively): the host-side
// vocabulary shared by the GPU denoiser (radiance_denoiser.hpp) and the CPU reference (RdnReference below) - settings,
// the constant resolution, the buffer layout (one state / work / output arena, identical on both sides so the parity
// gates compare sections byte-range by byte-range) and the pass list. Vulkan-free: builds in the stub backend.
//
// The algorithm and the RdnFrame record live in the single-source core shaders/denoise/rdn_core.h (C++ dialect:
// rdn_kernel.hpp). In short: separate demodulated diffuse / specular channels; firefly clamp and hit-distance pre-blur;
// A-SVGF temporal gradients from the producer's re-shaded samples; reprojection with plane / normal / instance
// disocclusion tests (3 x 3 fallback at jittered edges), specular virtual (hit-distance) motion, optional history
// clipping and anti-firefly; history fix of short histories from a 3-level pyramid; SVGF variance and variance-guided
// a-trous (5 levels) with the specular lobe footprint. The shadow (scalar visibility) path stays WP-6.4's
// SvgfDenoiser (DenoiseSignal::Shadow).
//
//   RdnReference ref;                       // CPU (CpuReference / CpuParallel: same bits)
//   ref.init(w, h, rdn_preset());
//   ref.runFrame(inputs, camera, prevCamera);
//   ref.outputD()[i] / outputS()[i]         // (rgb, variance), demodulated: the caller remodulates by the albedos

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/denoise/rdn_kernel.hpp>
#include <fuse/types.hpp>

#include <cstddef>
#include <vector>

namespace fuse::renderer::denoise {

inline constexpr u32 kRdnTile = 8u;
inline constexpr u32 kRdnStratum = FUSE_RDN_STRATUM;
inline constexpr u32 kRdnMaxAtrous = 5u;
inline constexpr u32 kRdnMaxGradientIterations = 5u;
inline constexpr u32 kRdnMipLevels = FUSE_RDN_MIP_LEVELS;
/// Upper bound of the passes of one frame (prepare, preblur, gradients, temporal, mips, fix, variance, a-trous).
inline constexpr u32 kRdnMaxPasses = 2u + 1u + kRdnMaxGradientIterations + 1u + kRdnMipLevels + 1u + 1u + kRdnMaxAtrous;

using rdnk::RdnFrame;
static_assert(sizeof(RdnFrame) % 8u == 0u, "RdnFrame: 8-byte multiple (std430 struct with 64-bit members)");
static_assert(offsetof(RdnFrame, camOx) == 32u * 8u, "RdnFrame: 32 addresses, then the scalars");
static_assert(offsetof(RdnFrame, width) == 32u * 8u + 24u * 4u, "RdnFrame::width");
static_assert(sizeof(RdnFrame) == 32u * 8u + 24u * 4u + 12u * 4u + 30u * 4u, "RdnFrame size (rdn_core.h)");

/// Push constants of the GPU kernel (64 bytes): the frame record's address, six pass addresses, pass, step.
struct RdnPush {
    u64 frame = 0;
    u64 a[6] = {0, 0, 0, 0, 0, 0};
    u32 pass = 0;
    u32 step = 1;
};
static_assert(sizeof(RdnPush) == 64u, "RdnPush (rdn.comp / rdn.slang)");

/// A pinhole camera in the Relight path tracer's model: dir = normalize(forward
/// + right x + up y) with x = 2u - 1, y = 1 - 2v; right / up are scaled by
/// tan(fov / 2) (x aspect for right); forward is unit.
struct RdnCamera {
    f32 origin[3] = {0.f, 0.f, 0.f};
    f32 right[3] = {1.f, 0.f, 0.f};
    f32 up[3] = {0.f, 1.f, 0.f};
    f32 forward[3] = {0.f, 0.f, -1.f};
};

struct RdnSettings {
    f32 motionScale = -1.f; ///< prevUV = uv + motionScale x motion (-1: WP-4.1 current - previous)
    f32 alphaD = 0.05f;
    f32 alphaS = 0.1f;
    f32 maxHistD = 32.f;
    f32 maxHistS = 16.f;
    f32 reprojNormal = 0.9f;
    f32 reprojDepth = 0.02f;
    f32 minReprojWeight = 1e-3f;
    bool virtualMotion = true; ///< specular hit-distance (virtual image) reprojection
    f32 virtRough0 = 0.15f;
    f32 virtRough1 = 0.5f;
    f32 clampSigmaD = 0.f; ///< history clipping k (0 = off)
    f32 clampSigmaS = 0.f; ///< (off: biased on heavy-tailed path-traced specular; the virtual motion handles ghosting)
    f32 fireflyRatio = 0.f; ///< spatial firefly bound (0 = off)
    f32 fireflySigma = 0.f; ///< temporal anti-firefly (0 = off; biased on heavy-tailed path-traced input)
    bool preblur = true;
    f32 preblurRadiusD = 3.f;
    f32 preblurRadiusS = 3.f;
    f32 hitDistScale = 1.f;
    bool historyFix = true;
    f32 fixFrames = 4.f;
    f32 fixMaxWeight = 8.f;
    bool spatialVariance = true;
    f32 varianceHistory = 4.f;
    f32 varianceBoost = 4.f;
    f32 sigmaPlane = 0.02f;
    f32 sigmaLumD = 4.f;
    f32 sigmaLumS = 4.f;
    u32 sigmaNormalD = 128u;
    u32 sigmaNormalS = 128u;
    f32 specRadiusMin = 0.5f;
    f32 specRadiusMax = 32.f;
    u32 atrousIterations = 5u;
    u32 historyTap = 0u;
    bool gradients = false; ///< A-SVGF (needs the producer's gradient samples every frame)
    u32 gradientIterations = 3u;
    f32 gradientScale = 2.f;
    f32 gradientEpsilon = 1e-4f;
    f32 depthEpsilon = 1e-3f;
    f32 sigmaRoughness = 10.f; ///< specular taps: exp(-sigmaRoughness |r_p - r_q|)
    bool instanceTest = false; ///< instance ids take part in the disocclusion test (needs the instance input)
};

/// The defaults above (the thresholds of the rl_denoise_* / fuse_rp_rdn_* gates were measured with them).
RdnSettings rdn_preset();

/// Settings -> constants (addresses 0, cameras from the arguments). Clamps the iteration counts.
RdnFrame rdn_resolve(const RdnSettings& settings, u32 width, u32 height, bool history, const RdnCamera& camera,
                     const RdnCamera& prevCamera);

/// Byte offsets of the state / work / output arenas (sections 256-aligned).
struct RdnBufferLayout {
    // state (persistent, [parity])
    u64 guide[2] = {0, 0};
    u64 aux[2] = {0, 0};
    u64 histD[2] = {0, 0};
    u64 histS[2] = {0, 0};
    u64 mom[2] = {0, 0};
    u64 stateBytes = 0;
    // work (per frame)
    u64 preD = 0, preS = 0, blurD = 0, blurS = 0, accD = 0, accS = 0;
    u64 mip[kRdnMipLevels] = {0, 0, 0};
    u64 fixD = 0, fixS = 0, varD = 0, varS = 0;
    u64 atrousD[kRdnMaxAtrous] = {}; ///< iteration k's result (the last iteration writes the output arena)
    u64 atrousS[kRdnMaxAtrous] = {};
    u64 gradient[kRdnMaxGradientIterations + 1u] = {}; ///< 0 = prepare, k = after iteration k
    u64 workBytes = 0;
    // output
    u64 outD = 0, outS = 0;
    u64 outputBytes = 0;
    u32 width = 0, height = 0, strataW = 0, strataH = 0;
    u32 mipW[kRdnMipLevels] = {0, 0, 0};
    u32 mipH[kRdnMipLevels] = {0, 0, 0};
    bool keepIntermediates = false;

    /// keepIntermediates: every a-trous / gradient iteration gets its own section
    /// (parity gates); otherwise they ping-pong through two.
    static RdnBufferLayout compute(u32 width, u32 height, bool keepIntermediates);
};

/// Fills the address fields of `c` from the arena bases (host pointers or
/// device addresses) and the parity this frame writes. Inputs are left alone.
void rdn_bind_arenas(RdnFrame& c, const RdnBufferLayout& l, u64 state, u64 work, u64 output, u32 parity);

/// One pass of the frame.
struct RdnPassDesc {
    const char* name = "";
    rdnk::RdnPassArgs args{};
    u32 gridW = 0; ///< items (pixels, strata or pyramid texels)
    u32 gridH = 0;
    bool readsInputs = false; ///< prepare / gradient prepare / temporal read the caller's input buffers
    bool writesOutput = false; ///< the last a-trous iteration writes the output arena
};

/// The passes of one frame in execution order (both sides iterate the same list). Returns the count (<= max).
u32 rdn_build_passes(const RdnFrame& c, const RdnBufferLayout& l, u64 work, u64 output, RdnPassDesc* out, u32 max);

/// One frame's inputs on the host (the GPU buffers' layouts).
struct RdnReferenceInputs {
    const rdnk::float4* diffuse = nullptr; ///< (rgb, hit distance) per pixel
    const rdnk::float4* specular = nullptr; ///< (rgb, hit distance)
    const rdnk::float4* normal = nullptr; ///< (unit world normal, roughness)
    const f32* depth = nullptr; ///< linear view depth (0 = sky)
    const rdnk::float2* motion = nullptr; ///< UV motion (convention: RdnSettings::motionScale)
    const u32* instance = nullptr; ///< optional (RdnSettings::instanceTest)
    const rdnk::float4* gradient = nullptr; ///< per stratum (dCur, dPrev, sCur, sPrev); required when gradients
    bool reset = false; ///< camera cut: start every pixel over
};

class RdnReference {
public:
    void init(u32 width, u32 height, const RdnSettings& settings, bool keepIntermediates = false);
    void setSettings(const RdnSettings& settings) { m_settings = settings; }
    const RdnSettings& settings() const { return m_settings; }
    void reset() { m_history = false; }
    /// Every pass of one frame; no heap allocation after init().
    bool runFrame(const RdnReferenceInputs& in, const RdnCamera& camera, const RdnCamera& prevCamera,
                  kernel::Backend backend = kernel::Backend::CpuReference);
    /// Replays one pass with the frame record `c` (addresses: host pointers) - the GPU parity gates.
    static void runPass(const RdnFrame& c, const RdnPassDesc& pass,
                        kernel::Backend backend = kernel::Backend::CpuReference);

    u32 width() const { return m_layout.width; }
    u32 height() const { return m_layout.height; }
    const RdnBufferLayout& layout() const { return m_layout; }
    const RdnFrame& constants() const { return m_constants; }
    u32 parity() const { return m_parity; }
    const u8* stateArena() const { return reinterpret_cast<const u8*>(m_state.data()); }
    const u8* workArena() const { return reinterpret_cast<const u8*>(m_work.data()); }
    const rdnk::float4* outputD() const { return m_output.data() + m_layout.outD / 16u; }
    const rdnk::float4* outputS() const { return m_output.data() + m_layout.outS / 16u; }
    /// A work / state section (byte offset) as f32x4 records.
    const rdnk::float4* work(u64 offset) const { return m_work.data() + offset / 16u; }
    const rdnk::float4* state(u64 offset) const { return m_state.data() + offset / 16u; }

private:
    RdnSettings m_settings{};
    RdnBufferLayout m_layout{};
    RdnFrame m_constants{};
    bool m_history = false;
    u32 m_parity = 1u;
    std::vector<rdnk::float4> m_state;
    std::vector<rdnk::float4> m_work;
    std::vector<rdnk::float4> m_output;
    RdnPassDesc m_passes[kRdnMaxPasses]{};
};

} // namespace fuse::renderer::denoise
