#pragma once

// Upscaler abstraction: ITemporalUpscaler / ISpatialUpscaler, a backend registry with capability queries,
// quality modes, the jitter provider contract and history invalidation
// (docs/upscalers.md; design: docs/research/upscaling-framegen-and-post-injectors.md §4.1).
//
// Built-in backends (registered by UpscalerRegistry::instance()):
//   "native_taau"  temporal   FUSE native TAAU: adapter over renderer::TaauUpscaler (taa/taau.hpp, the
//                             single-source "taau" kernel), all quality modes.
//   "fsr1"         spatial    AMD FidelityFX FSR 1 (EASU + RCAS), single-source CPU kernels (fsr1_kernel.hpp).
//   "nis"          spatial    NVIDIA Image Scaling NVScaler (nis_kernel.hpp), up to 2x.
//   "cas"          sharpen    AMD FidelityFX CAS (cas_kernel.hpp), 1x only; also a post-process pass via
//                             upscale_passes.hpp run_cas().
// Optional, compiled only with FUSE_UPSCALER_FSR3 (off by default; not vendored yet — docs/upscalers.md):
//   "fsr3"         temporal   FidelityFX SDK v1.1.4 FSR 3.1 upscaler (Vulkan).
//
// Temporal backends take the renderer's canonical `UpscaleInputs` (upscale/upscale_inputs.hpp: render/display
// resolution, jittered linear colour, depth, UV motion, exposure, reactive / transparency masks, cameras,
// reset_history) — a superset of what DLSS / FSR 2-3 / XeSS / DirectSR consume. Spatial backends take one
// tone-mapped RGBA frame (SpatialUpscaleInputs). Images are CPU views today; GPU backends take the same
// contracts once the render graph hands out texture handles.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fuse::renderer::upscale {

// ---- Quality modes ------------------------------------------------------------------------------------

/// The renderer's quality modes (upscale_inputs.hpp): NativeAA 1.0x, UltraQuality 1.3x, Quality 1.5x,
/// Balanced 1.7x, Performance 2.0x, UltraPerformance 3.0x (per axis, display / render).
using QualityMode = fuse::renderer::UpscaleQualityMode;
/// The canonical per-frame temporal input contract (upscale_inputs.hpp, owned by the TAAU work stream).
using UpscaleInputs = fuse::renderer::UpscaleInputs;

inline constexpr u32 kQualityModeCount = 6;

/// Bit for `mode` in UpscalerCaps::quality_modes.
constexpr u8 quality_mode_bit(QualityMode mode) { return static_cast<u8>(1u << static_cast<u32>(mode)); }
inline constexpr u8 kAllQualityModes = 0x3Fu;

/// Per-axis upscale ratio (display / render) of `mode` (upscaleRatioForMode).
f32 quality_mode_ratio(QualityMode mode);
const char* quality_mode_name(QualityMode mode);

struct Extent2D {
    u32 width = 0;
    u32 height = 0;

    constexpr bool valid() const { return width > 0u && height > 0u; }
    constexpr bool operator==(const Extent2D&) const = default;
};

/// Render resolution for `display` at `mode` (makeUpscaleResolution: round(display / ratio), at least 1).
Extent2D render_extent(Extent2D display, QualityMode mode);
/// Per-axis ratio display / render (x = width ratio).
math::Vec2 upscale_ratio(Extent2D render, Extent2D display);
/// Texture mip bias for materials rendered at `render` and displayed at `display`
/// (upscaleTextureMipBias): log2(render.width / display.width) - 1 for temporal upscalers (FSR / DLSS
/// guidance), log2(render.width / display.width) for spatial ones (no temporal super-sampling to recover
/// the extra detail).
f32 mip_lod_bias(Extent2D render, Extent2D display, bool temporal);
fuse::renderer::UpscaleResolution make_resolution(Extent2D render, Extent2D display);

// ---- Capabilities -------------------------------------------------------------------------------------

enum class UpscalerKind : u8 {
    Temporal = 0, ///< Accumulates history (jitter + motion vectors); ITemporalUpscaler.
    Spatial = 1,  ///< Single frame in, single frame out at display resolution; ISpatialUpscaler.
    Sharpen = 2,  ///< Same-resolution sharpener (ratio 1 only); ISpatialUpscaler.
};

/// Execution APIs, as bits in UpscalerCaps::apis.
enum class UpscalerApi : u8 { Cpu = 1u << 0, Vulkan = 1u << 1, Cuda = 1u << 2 };

constexpr u8 api_bit(UpscalerApi api) { return static_cast<u8>(api); }

/// Queried from the registry before creating a backend (CI only ever sees the CPU backends).
struct UpscalerCaps {
    const char* name = "";         ///< Registry id ("fsr1").
    const char* display_name = ""; ///< UI label.
    const char* license = "";      ///< SPDX id of the vendored code the backend uses.
    UpscalerKind kind = UpscalerKind::Spatial;
    bool temporal = false;
    bool needs_depth = false;
    bool needs_motion_vectors = false;
    bool needs_jitter = false;            ///< Projection must be jittered by the backend's jitter provider.
    bool needs_reactive_mask = false;     ///< Required reactive mask (optional ones: accepts_reactive_mask).
    bool accepts_reactive_mask = false;
    bool accepts_transparency_mask = false;
    bool needs_exposure = false;          ///< Required exposure value (optional: accepts_exposure).
    bool accepts_exposure = false;
    bool hdr_input = false;               ///< Wants pre-tonemap linear HDR (false: tone-mapped perceptual [0,1]).
    bool built_in_sharpening = false;     ///< Honours SpatialUpscaleInputs::sharpness.
    bool supports_dynamic_resolution = false;
    f32 min_ratio = 1.f;                  ///< Per-axis display/render ratio range.
    f32 max_ratio = 1.f;
    u8 quality_modes = 0;                 ///< quality_mode_bit() set.
    u8 apis = 0;                          ///< UpscalerApi bits compiled into this build.
    bool stub = false;                    ///< Adapter present, algorithm pending (excluded by select() unless allowed).

    bool supports_mode(QualityMode mode) const { return (quality_modes & quality_mode_bit(mode)) != 0u; }
    bool supports_api(UpscalerApi api) const { return (apis & api_bit(api)) != 0u; }
    /// True when a per-axis ratio lies in [min_ratio, max_ratio]. Integer render extents make the realised ratio
    /// drift from the preset (256 / round(256 / 3) = 3.012), so the range is widened by 2 %.
    bool supports_ratio(f32 ratio) const {
        return ratio >= min_ratio * (1.f - kRatioSlack) && ratio <= max_ratio * (1.f + kRatioSlack);
    }
    static constexpr f32 kRatioSlack = 0.02f;
};

// ---- Images and the spatial / output contracts -----------------------------------------------------------

/// Non-owning row-major 2D image view (`width * height` elements, no padding).
template <typename T>
struct Image2D {
    T* data = nullptr;
    u32 width = 0;
    u32 height = 0;

    bool valid() const { return data != nullptr && width > 0u && height > 0u; }
    Extent2D extent() const { return {width, height}; }
};

using ConstRgbaImage = Image2D<const math::Vec4>;
using RgbaImage = Image2D<math::Vec4>;
using RgbImage = Image2D<math::Vec3>;

/// Spatial upscaler / sharpener input: one tone-mapped (display-referred, [0, 1]) RGBA frame. FSR1, NIS and
/// CAS run after tonemapping and before grain / UI (research doc §3.1, §4.1 item 6).
struct SpatialUpscaleInputs {
    ConstRgbaImage color{};
    /// Backend sharpening in [0, 1] (FSR1 RCAS, NIS, CAS); 1 = sharpest.
    f32 sharpness = 0.2f;
};

/// Spatial output: display-resolution RGBA.
struct UpscaleOutputs {
    RgbaImage color{};
};

/// Temporal output: display-resolution linear RGB (pre-tonemap, like the TAAU / FSR 2-3 / DLSS output).
struct TemporalUpscaleOutputs {
    RgbImage color{};
};

enum class UpscaleStatus : u8 {
    Ok = 0,
    InvalidInputs,       ///< Missing / mis-sized surfaces for what the backend declares it needs.
    UnsupportedRatio,    ///< Ratio outside [min_ratio, max_ratio].
    BackendUnavailable,  ///< Backend present but cannot run this request (stub, no device).
    LaunchFailed,
};

const char* upscale_status_name(UpscaleStatus status);

/// Execution options for one evaluate().
struct UpscaleDispatch {
    kernel::Backend backend = kernel::Backend::CpuParallel;
};

// ---- History ------------------------------------------------------------------------------------------

enum class HistoryResetReason : u8 {
    None = 0,
    CameraCut,
    Teleport,
    ResolutionChange,
    QualityModeChange,
    Explicit,
};

const char* history_reset_reason_name(HistoryResetReason reason);

// ---- Jitter -------------------------------------------------------------------------------------------

/// Sub-pixel projection jitter source. The renderer asks the active temporal upscaler's provider each frame
/// and passes the result both to the projection (offset_ndc) and to UpscaleInputs::jitter_px.
class IJitterProvider {
public:
    virtual ~IJitterProvider() = default;
    /// Number of distinct jitter phases before the sequence repeats for this render/display pair.
    virtual u32 phase_count(Extent2D render, Extent2D display) const = 0;
    /// Jitter in render pixels, in [-0.5, 0.5) per axis (+x right, +y down), for monotonic `frame_index`.
    virtual math::Vec2 offset_px(u32 frame_index, Extent2D render, Extent2D display) const = 0;
    /// Same offset in NDC for a projection matrix: (2 * x / render.width, -2 * y / render.height).
    math::Vec2 offset_ndc(u32 frame_index, Extent2D render, Extent2D display) const;
};

/// Halton(2, 3) jitter with the FSR 2/3 phase count ceil(8 * (display.width / render.width)^2) (so every
/// display pixel sees ~8 samples per cycle): ffxFsr3GetJitterPhaseCount / ffxFsr3GetJitterOffset, via the
/// renderer's upscaleJitterPhaseCount / upscaleJitterOffset.
class HaltonJitterProvider final : public IJitterProvider {
public:
    static constexpr u32 kBasePhaseCount = 8;
    u32 phase_count(Extent2D render, Extent2D display) const override;
    math::Vec2 offset_px(u32 frame_index, Extent2D render, Extent2D display) const override;
};

/// No jitter (spatial backends).
class ZeroJitterProvider final : public IJitterProvider {
public:
    u32 phase_count(Extent2D, Extent2D) const override { return 1u; }
    math::Vec2 offset_px(u32, Extent2D, Extent2D) const override { return math::Vec2(0.f, 0.f); }
};

// ---- Interfaces ---------------------------------------------------------------------------------------

/// Common base: every backend reports caps and its preferred render size.
class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    virtual const UpscalerCaps& caps() const = 0;
    /// Render extent this backend wants for `display` at `mode` (default: render_extent()).
    virtual Extent2D render_size(Extent2D display, QualityMode mode) const { return render_extent(display, mode); }
    /// True when the per-axis render -> display ratio lies in the caps' range.
    bool supports(Extent2D render, Extent2D display) const;
};

/// Spatial upscalers and sharpeners: one tone-mapped frame in, display-resolution RGBA out; no jitter, no
/// history. Kind Spatial or Sharpen.
class ISpatialUpscaler : public IUpscaler {
public:
    virtual UpscaleStatus evaluate(const UpscaleDispatch& dispatch, const SpatialUpscaleInputs& inputs,
                                   const UpscaleOutputs& outputs) = 0;
    /// Colour / output present and the ratio in range.
    UpscaleStatus validate(const SpatialUpscaleInputs& inputs, const UpscaleOutputs& outputs) const;
};

/// Temporal upscalers: consume the renderer's UpscaleInputs, own display-resolution history and a jitter
/// provider, and output linear display-resolution RGB.
class ITemporalUpscaler : public IUpscaler {
public:
    virtual UpscaleStatus evaluate(const UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                   const TemporalUpscaleOutputs& outputs) = 0;
    virtual const IJitterProvider& jitter_provider() const = 0;
    /// Drop history (next evaluate() starts a new accumulation). Bumps history_generation().
    virtual void invalidate_history(HistoryResetReason reason) = 0;
    /// Incremented on every invalidation (explicit, reset_history, resolution change).
    virtual u32 history_generation() const = 0;
    virtual HistoryResetReason last_reset_reason() const = 0;
    /// Frames accumulated since the last invalidation.
    virtual u32 accumulated_frames() const = 0;
    /// validateUpscaleInputs + caps (required masks, ratio range) + output sized to the display resolution.
    UpscaleStatus validate(const UpscaleInputs& inputs, const TemporalUpscaleOutputs& outputs) const;
};

// ---- Registry -----------------------------------------------------------------------------------------

using UpscalerFactory = std::unique_ptr<IUpscaler> (*)();

struct UpscalerBackendEntry {
    UpscalerCaps caps{};
    UpscalerFactory factory = nullptr;
};

/// What a caller needs from a backend (for UpscalerRegistry::select).
struct UpscalerRequirements {
    QualityMode mode = QualityMode::Quality;
    UpscalerApi api = UpscalerApi::Cpu;
    bool allow_temporal = true;
    bool allow_spatial = true;
    bool allow_stub = false;
    bool have_depth = true;
    bool have_motion_vectors = true;
    bool have_reactive_mask = false;
    bool have_exposure = true;
};

/// True when `caps` can serve `req`.
bool caps_satisfy(const UpscalerCaps& caps, const UpscalerRequirements& req);

class UpscalerRegistry {
public:
    /// Process-wide registry with the built-in backends registered.
    static UpscalerRegistry& instance();

    /// Empty registry (tests); call register_builtin_backends() to populate.
    UpscalerRegistry() = default;
    void register_builtin_backends();

    /// False when `caps.name` is empty, already registered, or `factory` is null.
    bool register_backend(const UpscalerCaps& caps, UpscalerFactory factory);
    bool unregister_backend(std::string_view name);

    const UpscalerCaps* find(std::string_view name) const;
    std::span<const UpscalerBackendEntry> backends() const { return m_entries; }
    std::unique_ptr<IUpscaler> create(std::string_view name) const;
    /// First registered backend (registration order = preference order) satisfying `req`; null when none.
    const UpscalerCaps* select(const UpscalerRequirements& req) const;

private:
    std::vector<UpscalerBackendEntry> m_entries;
};

/// Built-in backend ids.
inline constexpr const char* kNativeTaauName = "native_taau";
inline constexpr const char* kFsr1Name = "fsr1";
inline constexpr const char* kNisName = "nis";
inline constexpr const char* kCasName = "cas";
inline constexpr const char* kFsr3Name = "fsr3";

} // namespace fuse::renderer::upscale
