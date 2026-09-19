#pragma once

#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// Stub HRTF impulse-response descriptor — empty IR falls back to ILD/ITD pan.
struct HrtfIrStub {
    const float* samples = nullptr;
    u32 length = 0;
};

/// Listener-local distance below which a source is treated as co-located.
float hrtf_co_located_epsilon();

/// Canonical empty IR stub for guard fallbacks.
HrtfIrStub make_empty_hrtf_ir();

/// Validated IR stub factory — returns empty when samples are null or length is zero.
HrtfIrStub make_hrtf_ir_stub(const float* samples, u32 length);

/// True when an HRTF IR stub has non-null, non-empty sample data.
bool has_hrtf_ir(const HrtfIrStub& ir);

/// Readable alias — true when IR samples are null or zero-length.
bool is_empty_hrtf_ir(const HrtfIrStub& ir);

/// Alias for `has_hrtf_ir` — convolution path is available when true.
bool should_use_hrtf_ir(const HrtfIrStub& ir);

/// True when IR convolution should not run (empty/null/zero-length stub).
bool should_skip_hrtf_convolution(const HrtfIrStub& ir);

/// True when IR samples are non-null but length is zero (malformed stub).
bool is_nonnull_zero_length_hrtf_ir(const HrtfIrStub& ir);

/// Why HRTF IR convolution would early-out (B7.2 deepen follow-up pass).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    NullSamples,
    ZeroLength,
    MalformedIr,
};

/// Human-readable label for empty-IR reject reasons (logging / tests).
const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason);

/// Diagnose why IR convolution would skip; vacuously succeeds on valid IR stubs.
HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir);

/// Returns true when \c hrtf_ir_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected);

/// Empty-IR preflight diagnostics — read-only guard bundle (B7.2 deepen).
struct HrtfIrPreflight {
    HrtfIrRejectReason reason = HrtfIrRejectReason::None;
    bool emptyIr = false;
    bool nullSamples = false;
    bool zeroLength = false;
    bool malformedIr = false;

    bool can_convolve() const { return !emptyIr && !malformedIr; }

    /// True when convolution dispatch should be skipped (empty or malformed IR).
    bool should_skip_convolution() const { return !can_convolve(); }
};

/// Preflight an HRTF IR stub before convolution dispatch.
HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// Non-mutating convolution predicate — mirrors \c HrtfIrPreflight::can_convolve.
bool can_convolve_hrtf_ir(const HrtfIrPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfIrPreflight::should_skip_convolution.
bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight);

/// HRTF pan routing — empty IR uses ILD/ITD stub; convolution deferred until IR wired.
enum class HrtfPanPath {
    Bypass,
    IldItdStub,
    Convolution,
};

/// Select pan path from HRTF enable flag, IR stub, and listener-local offset.
HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);

/// Select pan path when no IR is wired (ILD/ITD stub or bypass).
HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Why HRTF pan routing would bypass spatial pan (B7.2 deepen follow-up pass).
enum class HrtfPanPathRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
};

/// Human-readable label for pan-path reject reasons (logging / tests).
const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason);

/// Diagnose why pan routing would bypass; vacuously succeeds on spatial paths.
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when \c hrtf_pan_path_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected);

/// Pan-path preflight diagnostics — read-only guard bundle (B7.2 deepen).
struct HrtfPanPathPreflight {
    HrtfPanPathRejectReason reason = HrtfPanPathRejectReason::None;
    HrtfPanPath path = HrtfPanPath::Bypass;
    bool hrtfDisabled = false;
    bool coLocated = false;
    bool emptyIr = false;
    bool skipped = false;

    bool can_spatial_pan() const { return !skipped && path != HrtfPanPath::Bypass; }
    bool can_convolve() const { return can_spatial_pan() && path == HrtfPanPath::Convolution; }

    /// True when pan routing resolves to centre bypass (disabled or co-located).
    bool should_skip() const { return skipped; }

    /// True when the resolved path selects ILD/ITD stub (empty IR fallback).
    bool uses_ild_itd_stub() const { return path == HrtfPanPath::IldItdStub; }
};

/// Preflight HRTF pan routing from enable flag, IR stub, and listener-local offset.
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener);

/// Preflight pan routing when no IR is wired (ILD/ITD stub or bypass).
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Non-mutating spatial-pan predicate — mirrors \c HrtfPanPathPreflight::can_spatial_pan.
bool can_apply_spatial_hrtf_pan(const HrtfPanPathPreflight& preflight);

/// Non-mutating convolution predicate — mirrors \c HrtfPanPathPreflight::can_convolve.
bool can_convolve_hrtf_pan_path(const HrtfPanPathPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfPanPathPreflight::should_skip.
bool should_skip_hrtf_pan_path_preflight(const HrtfPanPathPreflight& preflight);

/// True when pan-path preflight selects ILD/ITD stub (empty IR fallback).
bool uses_ild_itd_stub_hrtf_pan_path(const HrtfPanPathPreflight& preflight);

/// True when a resolved pan path bypasses HRTF (disabled or co-located).
bool is_hrtf_pan_bypassed(HrtfPanPath path);

/// Alias for \c is_hrtf_pan_bypassed — skip pan/coupling when true.
bool should_skip_hrtf_pan_path(HrtfPanPath path);

/// True when the resolved path produces a lateral spatial image (not centre bypass).
bool is_spatial_hrtf_pan_path(HrtfPanPath path);

/// True when the pan path selects convolution (non-empty IR stub).
bool hrtf_pan_path_uses_convolution(HrtfPanPath path);

/// True when the pan path selects ILD/ITD stub (empty IR fallback).
bool hrtf_pan_path_uses_ild_itd_stub(HrtfPanPath path);

/// True when the pan path is centre bypass (disabled or co-located).
bool is_hrtf_pan_path_bypass(HrtfPanPath path);

/// Readable alias for \c is_hrtf_pan_path_bypass.
bool is_bypass_hrtf_pan_path(HrtfPanPath path);

/// Early-out: true when spatial panning should be skipped (bypass path).
bool should_skip_hrtf_spatial_pan(HrtfPanPath path);

/// True when listener and source share the same listener-local position.
bool is_co_located_hrtf_source(const Vec3& rel_listener);

/// Early-out inverse of \c should_apply_hrtf_pan.
bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);

/// True when HRTF pan should run (enabled and source is not co-located).
bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);

/// Inverse of `should_apply_hrtf_pan` — disabled or co-located sources.
bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);

/// Stereo pan law used for ILD stub gains.
enum class PanLaw {
    EqualPower,
    Linear,
};

/// Per-channel gains from a pan position in [-1, 1] (left .. right).
struct PanLawGains {
    float left = 0.5f;
    float right = 0.5f;
};

/// Binaural pan stub parameters — ILD pan law + Woodworth ITD placeholder.
struct BinauralPanParams {
    PanLaw pan_law = PanLaw::EqualPower;
    float max_itd_seconds = 0.0007f;
    float max_ild_pan = 1.f;
    float elevation_rolloff = 0.15f;
    float min_spatial_blend = 0.25f;
};

/// Azimuth/elevation in listener-local space (radians).
struct BinauralPanAngles {
    float azimuth = 0.f;
    float elevation = 0.f;
};

/// Per-ear gain stubs plus ITD placeholder for future delay-line wiring.
struct BinauralPanGains {
    float left = 1.f;
    float right = 1.f;
    float itd_seconds = 0.f;
};

/// Azimuth/elevation from a listener-local offset (+Z forward, +X right).
BinauralPanAngles compute_binaural_angles(const Vec3& rel_listener);

/// Transform a world-space offset through `basis`, then compute angles.
BinauralPanAngles compute_binaural_angles(const Vec3& world_relative, const ListenerBasis& basis);

/// Clamp pan position to [-1, 1].
float clamp_pan_position(float pan);

/// Map azimuth (radians) to a pan position in [-1, 1] for ILD stub sampling.
float compute_pan_position_from_azimuth(float azimuth, float max_ild_pan = 1.f);

/// Woodworth ITD stub from listener-local azimuth (radians).
float compute_itd_from_azimuth(float azimuth, const BinauralPanParams& params = {});

/// Symmetric elevation rolloff factor applied to both ears.
float compute_elevation_factor(float elevation, const BinauralPanParams& params = {});

/// Sample L/R gains for a pan law at the endpoints and interior.
PanLawGains sample_pan_law(float pan, PanLaw law);

/// ILD stub via selected pan law; ITD stub scales with sin(azimuth).
BinauralPanGains compute_binaural_pan_gains(const BinauralPanAngles& angles,
                                             const BinauralPanParams& params = {});

/// Convenience — angles and gains from listener-local offset.
BinauralPanGains compute_binaural_pan_gains(const Vec3& rel_listener,
                                             const BinauralPanParams& params = {});

/// Convenience — world-space offset through `basis`, then compute gains.
BinauralPanGains compute_binaural_pan_gains(const Vec3& world_relative, const ListenerBasis& basis,
                                             const BinauralPanParams& params = {});

/// Build a safe listener basis from an audio listener component.
ListenerBasis compute_listener_basis(const AudioListener& listener);

/// Azimuth/elevation from listener and source world positions.
BinauralPanAngles compute_binaural_angles(const AudioListener& listener, const Vec3& source_position);

/// Binaural gains from listener and source world positions.
BinauralPanGains compute_binaural_pan_gains(const AudioListener& listener, const Vec3& source_position,
                                             const BinauralPanParams& params = {});

/// Mono centre fallback when HRTF pan is bypassed.
BinauralPanGains make_centre_binaural_pan_gains();

/// L/R pan spread in [0, 1] — 0 when centre-panned.
float compute_pan_spread(const BinauralPanGains& gains);

/// True when left and right gains are within \p epsilon of centre pan.
bool is_centre_panned(const BinauralPanGains& gains, float epsilon = 1e-3f);

/// Equal-power energy metric for L/R gains (left² + right²).
float compute_binaural_pan_energy(const BinauralPanGains& gains);

/// True when ITD stub is non-zero within \p epsilon.
bool has_nonzero_itd(const BinauralPanGains& gains, float epsilon = 1e-6f);

/// Scale per-ear gains, then clamp to [0, 1].
void scale_binaural_pan_gains(BinauralPanGains& gains, float scale);

/// Apply mono sample through binaural L/R gains and attenuation.
void apply_binaural_pan_to_sample(float mono, const BinauralPanGains& pan, float attenuation,
                                  float& left, float& right);

/// Centre mono fallback — equal L/R contribution with attenuation.
void apply_centre_binaural_pan_to_sample(float mono, float attenuation, float& left, float& right);

/// One-shot binaural gains with empty-HRTF guards (disabled / co-located → centre).
BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const Vec3& rel_listener,
                                                    const BinauralPanParams& params = {});

/// IR-aware guarded pan — empty IR keeps ILD/ITD stub; bypass only when disabled/co-located.
BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    const BinauralPanParams& params = {});

/// Gains for a resolved HRTF pan path (bypass → centre mono).
BinauralPanGains compute_binaural_pan_gains_for_path(HrtfPanPath path, const Vec3& rel_listener,
                                                     const BinauralPanParams& params = {});

/// Linear interpolation between two binaural pan gain states.
BinauralPanGains lerp_binaural_pan_gains(const BinauralPanGains& from, const BinauralPanGains& to,
                                         float t);

/// Clamp per-ear gains to [0, 1].
void clamp_binaural_pan_gains(BinauralPanGains& gains);

/// Narrow or widen L/R spread — blend 1 preserves image, 0 collapses to mono centre.
void apply_spatial_blend(BinauralPanGains& gains, float blend);

/// Apply spatial blend only when blend is below unity (no-op at full separation).
void apply_hrtf_spatial_blend_guarded(BinauralPanGains& gains, float blend);

/// Attenuation coupling — blends distance and occlusion into spatial image narrowing.
struct HrtfAttenuationCoupling {
    float occlusion_weight = 0.5f;
};

/// Clamp distance or occlusion attenuation scalars into [0, 1].
float clamp_hrtf_attenuation(float attenuation);

/// Clamp occlusion blend weight into [0, 1].
float clamp_hrtf_attenuation_coupling_weight(float weight);

/// True when distance and occlusion are both fully audible (no narrowing).
bool is_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain);

/// True when distance/occlusion coupling should narrow the binaural image.
bool should_apply_hrtf_attenuation_coupling(HrtfPanPath path);

/// Early-out inverse of \c should_apply_hrtf_attenuation_coupling.
bool should_skip_hrtf_attenuation_coupling(HrtfPanPath path);

/// Combined guard — spatial path and non-unity attenuation warrant narrowing.
bool should_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain);

/// Why distance/occlusion coupling would skip spatial narrowing (B7.2 deepen follow-up pass).
enum class HrtfAttenuationCouplingRejectReason : u8 {
    None = 0,
    BypassPath,
    UnityAttenuation,
};

/// Human-readable label for attenuation-coupling reject reasons (logging / tests).
const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason);

/// Diagnose why attenuation coupling would skip; vacuously succeeds when narrowing applies.
HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain);

/// Returns true when \c hrtf_attenuation_coupling_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected);

/// Attenuation-coupling preflight diagnostics — read-only guard bundle (B7.2 deepen).
struct HrtfAttenuationCouplingPreflight {
    HrtfAttenuationCouplingRejectReason reason = HrtfAttenuationCouplingRejectReason::None;
    bool bypassPath = false;
    bool unityAttenuation = false;
    bool skipped = false;
    float spatialBlend = 1.f;

    bool can_narrow() const { return !skipped; }

    /// True when distance/occlusion coupling should be skipped (bypass or unity attenuation).
    bool should_skip() const { return skipped; }
};

/// Preflight distance + occlusion coupling before spatial image narrowing.
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

/// Non-mutating narrowing predicate — mirrors \c HrtfAttenuationCouplingPreflight::can_narrow.
bool can_narrow_hrtf_spatial_image(const HrtfAttenuationCouplingPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfAttenuationCouplingPreflight::should_skip.
bool should_skip_hrtf_attenuation_coupling_preflight(const HrtfAttenuationCouplingPreflight& preflight);

/// True when a spatial blend preserves full L/R separation.
bool is_unity_hrtf_spatial_blend(float blend, float epsilon = 1e-5f);

/// True when distance/occlusion scalars produce unity spatial blend — skip narrowing.
bool should_skip_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                    const HrtfAttenuationCoupling& coupling = {},
                                    const BinauralPanParams& params = {});

/// Combined spatial blend from distance attenuation and occlusion LF gain.
float compute_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Apply distance + occlusion coupling to narrow the binaural image toward mono centre.
void apply_hrtf_attenuation_coupling(BinauralPanGains& gains, float distance_attenuation,
                                     float occlusion_gain,
                                     const HrtfAttenuationCoupling& coupling = {},
                                     const BinauralPanParams& params = {});

/// Apply coupling only when the pan path is spatial (skips bypass centre mono).
void apply_hrtf_attenuation_coupling_for_path(BinauralPanGains& gains, HrtfPanPath path,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Narrow binaural image at distance — full separation when attenuation is unity.
float compute_hrtf_distance_factor(float distance_attenuation,
                                   const BinauralPanParams& params = {});

/// Apply distance-based spatial narrowing to L/R gains (mono centre preserved).
void apply_hrtf_distance_factor(BinauralPanGains& gains, float distance_attenuation,
                                const BinauralPanParams& params = {});

/// One-shot guarded pan with distance + occlusion attenuation coupling.
BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling = {},
                                                    const BinauralPanParams& params = {});

/// IR-aware one-shot guarded pan with attenuation coupling.
BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling = {},
                                                    const BinauralPanParams& params = {});

/// One-shot spatial pan with path-aware attenuation coupling.
BinauralPanGains compute_binaural_pan_gains_coupled_for_path(HrtfPanPath path,
                                                              const Vec3& rel_listener,
                                                              float distance_attenuation,
                                                              float occlusion_gain,
                                                              const HrtfAttenuationCoupling& coupling = {},
                                                              const BinauralPanParams& params = {});

/// Composite binaural/HRTF reject reasons — unified labels for bundled guards (B7.2 deepen follow-up pass).
enum class HrtfBinauralRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
    NullSamples,
    ZeroLength,
    MalformedIr,
    BypassPath,
    UnityAttenuation,
};

/// Human-readable label for composite binaural reject reasons (logging / tests).
const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason);

/// Composite binaural/HRTF preflight — bundles empty-IR, pan-path, and attenuation-coupling guards (B7.2 deepen).
struct HrtfBinauralPreflight {
    HrtfIrPreflight ir{};
    HrtfPanPathPreflight panPath{};
    HrtfAttenuationCouplingPreflight attenuationCoupling{};
    HrtfBinauralRejectReason reason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason narrowingReason = HrtfBinauralRejectReason::None;

    HrtfPanPath path() const { return panPath.path; }

    bool can_spatial_pan() const { return panPath.can_spatial_pan(); }
    bool can_convolve() const { return panPath.can_convolve(); }
    bool can_narrow_spatial_image() const { return attenuationCoupling.can_narrow(); }
    bool is_bypass() const { return panPath.skipped; }

    /// True when composite preflight selects centre bypass (disabled or co-located).
    bool should_skip() const { return is_bypass(); }

    /// True when the resolved path selects ILD/ITD stub (empty IR fallback).
    bool uses_ild_itd_stub() const { return panPath.uses_ild_itd_stub(); }

    /// True when IR convolution should be skipped (empty or malformed IR, or bypass).
    bool should_skip_convolution() const { return !can_convolve(); }
};

/// Primary pan bypass reject reason from a composite preflight bundle.
HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight);

/// Convolution reject reason from a composite preflight bundle.
HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(const HrtfBinauralPreflight& preflight);

/// Spatial-narrowing reject reason from a composite preflight bundle.
HrtfBinauralRejectReason hrtf_binaural_narrowing_reject_reason(const HrtfBinauralPreflight& preflight);

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected);

/// Returns true when \c hrtf_binaural_convolution_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_binaural_rejects_for_convolution_reason(const HrtfBinauralPreflight& preflight,
                                                  HrtfBinauralRejectReason expected);

/// Returns true when \c hrtf_binaural_narrowing_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_binaural_rejects_for_narrowing_reason(const HrtfBinauralPreflight& preflight,
                                                HrtfBinauralRejectReason expected);

/// Preflight all binaural/HRTF guards for one source (IR-aware).
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Preflight all binaural/HRTF guards when no IR is wired.
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Preflight all binaural/HRTF guards from listener and source world positions (IR-aware).
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, const HrtfIrStub& ir,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Preflight all binaural/HRTF guards from listener and source world positions (no IR wired).
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Non-mutating spatial-pan predicate — mirrors \c HrtfBinauralPreflight::can_spatial_pan.
bool can_apply_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight);

/// Non-mutating convolution predicate — mirrors \c HrtfBinauralPreflight::can_convolve.
bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight);

/// Non-mutating narrowing predicate — mirrors \c HrtfBinauralPreflight::can_narrow_spatial_image.
bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight);

/// True when the composite preflight selects centre bypass (disabled or co-located).
bool should_skip_hrtf_binaural(const HrtfBinauralPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfBinauralPreflight::should_skip_convolution.
bool should_skip_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight);

/// True when composite preflight selects ILD/ITD stub (empty IR fallback).
bool uses_ild_itd_stub_hrtf_binaural(const HrtfBinauralPreflight& preflight);

/// Apply pan + coupling using a preflight bundle (read-only guards; valid paths unchanged).
BinauralPanGains compute_binaural_pan_gains_from_preflight(const HrtfBinauralPreflight& preflight,
                                                           const Vec3& rel_listener,
                                                           const HrtfAttenuationCoupling& coupling = {},
                                                           const BinauralPanParams& params = {});

/// Apply one mono sample through a preflight bundle (bypass → centre pan; valid paths unchanged).
void apply_binaural_pan_to_sample_from_preflight(float mono, const HrtfBinauralPreflight& preflight,
                                                 const Vec3& rel_listener, float attenuation,
                                                 float& left, float& right,
                                                 const HrtfAttenuationCoupling& coupling = {},
                                                 const BinauralPanParams& params = {});

/// Why HRTF IR preflight rejected convolution (B7.2 deepen).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    EmptyIr,
    NullSamples,
    ZeroLength,
    MalformedIr,
};

/// Why pan-path preflight rejected spatial pan or convolution (B7.2 deepen).
enum class HrtfPanPathRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
    EmptyIr,
};

/// Why attenuation-coupling preflight rejected spatial narrowing (B7.2 deepen).
enum class HrtfAttenuationCouplingRejectReason : u8 {
    None = 0,
    BypassPath,
    UnityAttenuation,
};

/// Why composite binaural/HRTF preflight rejected the requested path (B7.2 deepen).
enum class HrtfBinauralRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
    EmptyIr,
    MalformedIr,
    BypassPath,
    UnityAttenuation,
};

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason);

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);
HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(const HrtfPanPathPreflight& preflight);
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight);
HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight);
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight);
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(const HrtfBinauralPreflight& preflight);

/// IR preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir);

/// Pan-path preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason = nullptr);
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason = nullptr);
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason);
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason);
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener);
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener);

/// Pan-path convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_convolution_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener,
                                           HrtfPanPathRejectReason* reason = nullptr);
bool try_preflight_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfPanPathRejectReason& reason);
bool should_skip_hrtf_convolution_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                 const Vec3& rel_listener);

/// Attenuation-coupling preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               float occlusion_gain,
                                               const HrtfAttenuationCoupling& coupling = {},
                                               const BinauralPanParams& params = {},
                                               HrtfAttenuationCouplingRejectReason* reason = nullptr);
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling = {},
                                             const BinauralPanParams& params = {});
bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
                                                     float occlusion_gain,
                                                     const HrtfAttenuationCoupling& coupling = {},
                                                     const BinauralPanParams& params = {});

/// Composite binaural/HRTF preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling = {},
                                   const BinauralPanParams& params = {},
                                   HrtfBinauralRejectReason* reason = nullptr);
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling = {},
                                   const BinauralPanParams& params = {},
                                   HrtfBinauralRejectReason* reason = nullptr);
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling = {},
                                         const BinauralPanParams& params = {});
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling = {},
                                         const BinauralPanParams& params = {});

/// Composite convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain,
                                               const HrtfAttenuationCoupling& coupling = {},
                                               const BinauralPanParams& params = {},
                                               HrtfBinauralRejectReason* reason = nullptr);
bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain, HrtfBinauralRejectReason& reason,
                                            const HrtfAttenuationCoupling& coupling = {},
                                            const BinauralPanParams& params = {});

/// Composite spatial-narrowing preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling = {},
                                             const BinauralPanParams& params = {},
                                             HrtfBinauralRejectReason* reason = nullptr);
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling = {},
                                           const BinauralPanParams& params = {});

} // namespace fuse::audio

// --- deepen additive from deepen-b72-hrtf-pan-guards-e91a ---
bool should_skip_hrtf_attenuation_coupling(float distance_attenuation, float occlusion_gain,

// --- deepen additive from deepen-hrtf-guards-71c7 ---
enum class HrtfPanRejectReason : u8 {
const char* hrtf_pan_reject_reason_label(HrtfPanRejectReason reason);
struct HrtfPanPreflight {
    HrtfPanRejectReason reject_reason = HrtfPanRejectReason::None;
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir);
bool try_resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                HrtfPanPath& out_path, HrtfPanRejectReason& out_reason);
HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);
bool should_skip_hrtf_attenuation_coupling_mapping(float distance_attenuation,
HrtfAttenuationCouplingPreflight preflight_hrtf_coupled_pan(bool hrtf_enabled,

// --- deepen additive from hrtf-preflight-guards-9323 ---
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);
[[nodiscard]] HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
struct HrtfSpatialPanPreflight {
    HrtfPanPathPreflight pan{};
    HrtfAttenuationCouplingPreflight coupling{};
[[nodiscard]] HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(

// --- deepen additive from hrtf-preflight-guards-4c1e ---
    [[nodiscard]] bool should_skip_spatial_pan() const { return bypass; }
    bool would_narrow = false;
    [[nodiscard]] bool should_skip() const { return skipped; }

// --- deepen additive from hrtf-preflight-guards-1a3b ---
    [[nodiscard]] bool should_skip() const { return empty_ir; }
    [[nodiscard]] bool should_skip() const { return bypass; }

// --- deepen additive from hrtf-preflight-guards-69c4 ---
    [[nodiscard]] bool should_skip() const { return !will_narrow; }

// --- deepen additive from hrtf-preflight-guards-3b8d ---
    [[nodiscard]] bool should_skip_coupling() const { return bypass_path || unity_attenuation; }
[[nodiscard]] HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(bool hrtf_enabled,

// --- deepen additive from hrtf-preflight-guards-3b23 ---
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const float* samples, u32 length);
    HrtfIrPreflight ir_preflight{};

// --- deepen additive from deepen-hrtf-preflights-44d8 ---
bool should_skip_hrtf_attenuation_coupling_for_inputs(HrtfPanPath path, float distance_attenuation,

// --- deepen additive from deepen-hrtf-preflight-guards-1cf6 ---
    bool should_skip_pan() const;
    bool should_skip_coupling() const;

// --- deepen additive from deepen-hrtf-preflights-ccde ---
enum class HrtfPanSkipReason : u8 {
    bool should_skip_pan() const { return path == HrtfPanPath::Bypass; }
    bool should_skip_spatial_pan() const { return should_skip_pan(); }
    bool should_skip_attenuation_coupling() const { return should_skip_pan(); }
    bool can_apply_spatial_pan() const { return !should_skip_spatial_pan(); }
    bool should_skip_coupling() const { return bypass_pan_path || unity_spatial_blend; }
    bool should_narrow_spatial_image() const { return !should_skip_coupling(); }
    bool can_couple() const { return !should_skip_coupling(); }

// --- deepen additive from deepen-b72-hrtf-preflights-815c ---
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& outReason);
bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);
bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPath& outPath, HrtfPanPathRejectReason& outReason);
                                HrtfPanPathRejectReason* reason = nullptr);
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             HrtfAttenuationCouplingRejectReason& outReason);
                                         HrtfAttenuationCouplingRejectReason* reason = nullptr);

// --- deepen additive from deepen-b72-hrtf-preflights-eec6 ---
enum class HrtfIrPreflightRejectReason : u8 {
enum class HrtfPanPreflightRejectReason : u8 {
enum class HrtfAttenuationCouplingPreflightRejectReason : u8 {
                                       HrtfIrPreflightRejectReason* reason = nullptr);
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPreflightRejectReason* reason = nullptr);
                                             HrtfAttenuationCouplingPreflightRejectReason* reason =

// --- deepen additive from deepen-b72-hrtf-pan-empty-ir-guards-e7fc ---
enum class HrtfIrRejectReason {
enum class HrtfPanPathSkipReason {
enum class HrtfAttenuationCouplingSkipReason {
bool should_skip_hrtf_attenuation_coupling_apply(HrtfPanPath path, float distance_attenuation,

// --- deepen additive from deepen-hrtf-preflights-981f ---
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir);
bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener);
    HrtfPanPathRejectReason reject_reason = HrtfPanPathRejectReason::None;
                              HrtfPanPathRejectReason* reason);
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfAttenuationCouplingRejectReason reject_reason = HrtfAttenuationCouplingRejectReason::None;
                                        HrtfAttenuationCouplingRejectReason* reason,

// --- deepen additive from deepen-hrtf-b7-2-guards-9316 ---
enum class HrtfIrPreflightReject : u8 {
    HrtfIrPreflightReject reject = HrtfIrPreflightReject::None;
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflightReject* reject = nullptr);
enum class HrtfPanPathPreflightReject : u8 {
    HrtfPanPathPreflightReject reject = HrtfPanPathPreflightReject::None;
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathPreflightReject* reject = nullptr);
enum class HrtfAttenuationCouplingPreflightReject : u8 {
    HrtfAttenuationCouplingPreflightReject reject = HrtfAttenuationCouplingPreflightReject::None;
                                             HrtfAttenuationCouplingPreflightReject* reject = nullptr,

// --- deepen additive from deepen-b72-hrtf-preflights-bc2e ---
enum class HrtfIrSkipReason : u8 {
HrtfIrPreflight preflight_hrtf_ir_stub(const HrtfIrStub& ir);
enum class HrtfPanPathSkipReason : u8 {
HrtfPanPathPreflight preflight_hrtf_pan_path_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
enum class HrtfAttenuationCouplingSkipReason : u8 {
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(

// --- deepen additive from deepen-b72-hrtf-preflight-guards-df8b ---
    HrtfIrRejectReason rejectReason = HrtfIrRejectReason::Empty;
    bool can_use_convolution() const { return rejectReason == HrtfIrRejectReason::None; }
    HrtfPanPathRejectReason rejectReason = HrtfPanPathRejectReason::Disabled;
    HrtfAttenuationCouplingRejectReason rejectReason = HrtfAttenuationCouplingRejectReason::BypassPath;
                                         HrtfAttenuationCouplingRejectReason* reason);
struct HrtfBinauralPanPreflight {
HrtfBinauralPanPreflight preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,

// --- deepen additive from deepen-hrtf-preflights-bb6d ---
    bool can_convolve() const { return reason == HrtfIrRejectReason::None; }

// --- deepen additive from b72-hrtf-preflight-guards-2202 ---
    [[nodiscard]] bool should_skip_convolution() const { return !can_convolve(); }
    [[nodiscard]] bool should_skip_spatial_pan() const { return !can_apply_spatial_pan(); }
    [[nodiscard]] bool should_skip_coupling() const { return !can_apply_coupling(); }

// --- deepen additive from deepen-hrtf-preflights-0f68 ---
struct EmptyHrtfIrPreflight {
EmptyHrtfIrPreflight preflightEmptyHrtfIr(const HrtfIrStub& ir);
HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener);
HrtfAttenuationCouplingPreflight preflightHrtfAttenuationCoupling(

// --- deepen additive from deepen-b72-hrtf-preflight-guards-f1c9 ---
    [[nodiscard]] bool can_apply_spatial_pan() const { return !should_skip(); }
    [[nodiscard]] bool should_skip() const { return is_hrtf_pan_path_bypass(path); }
struct HrtfGuardedPanPreflight {
    HrtfPanPathPreflight pan_path{};
    [[nodiscard]] bool should_skip_coupling() const { return coupling.should_skip(); }
HrtfGuardedPanPreflight preflight_hrtf_guarded_pan(bool hrtf_enabled, const HrtfIrStub& ir,

// --- deepen additive from deepen-b7-2-hrtf-composite-preflights-84ae ---
    HrtfAttenuationCouplingPreflight attenuation{};
HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, const HrtfIrStub& ir,
HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, float distance_attenuation,
bool can_apply_binaural_hrtf_pan(const HrtfBinauralPreflight& preflight);
bool can_convolve_binaural_hrtf(const HrtfBinauralPreflight& preflight);
bool can_narrow_binaural_hrtf_spatial_image(const HrtfBinauralPreflight& preflight);

// --- deepen additive from deepen-b72-hrtf-preflights-9877 ---
    const HrtfPanPathPreflight& preflight, const Vec3& rel_listener,
    bool should_skip_convolution() const { return ir.should_skip_convolution(); }
bool is_consistent_hrtf_binaural_preflight(const HrtfBinauralPreflight& preflight);
bool has_empty_hrtf_ir(const HrtfBinauralPreflight& preflight);
bool should_apply_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight);
bool should_skip_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight);

// --- deepen additive from deepen-fuse-b7-2-hrtf-guards-708b ---
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight);
bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir);
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener);
    HrtfAttenuationCouplingPreflight& preflight,
bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason);
HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight);
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener);

// --- deepen additive from deepen-b72-hrtf-preflight-reasons-53ba ---
    bool ok() const { return reason == HrtfIrRejectReason::None; }
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
    bool ok() const { return reason == HrtfPanPathRejectReason::None; }
                                  const Vec3& rel_listener, HrtfPanPathRejectReason& reason);
                                  HrtfPanPathRejectReason& reason);
    bool ok() const { return reason == HrtfAttenuationCouplingRejectReason::None; }
                                              HrtfAttenuationCouplingRejectReason& reason,
HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    bool ok() const { return reason == HrtfBinauralRejectReason::None; }
                              HrtfBinauralRejectReason* reason,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                                  HrtfBinauralRejectReason& reason,
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from b7-2-hrtf-reject-reasons-1115 ---
    bool can_spatial_pan() const { return reason == HrtfPanPathRejectReason::None; }
    bool can_narrow() const { return reason == HrtfAttenuationCouplingRejectReason::None; }
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);
    bool is_bypass() const { return reason != HrtfBinauralRejectReason::None; }

// --- deepen additive from b72-hrtf-reject-reason-preflights-62b4 ---
HrtfBinauralRejectReason hrtf_binaural_reject_reason(HrtfPanPathRejectReason pan_reason);
    HrtfBinauralRejectReason rejectReason() const {
    bool is_bypass() const { return panPath.reason != HrtfPanPathRejectReason::None; }

// --- deepen additive from deepen-hrtf-reject-reason-preflights-83b7 ---
    HrtfIrRejectReason rejectReason = HrtfIrRejectReason::None;
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight,
    HrtfPanPathRejectReason rejectReason = HrtfPanPathRejectReason::None;
    HrtfIrRejectReason convolveRejectReason = HrtfIrRejectReason::None;
                                 HrtfPanPathPreflight& preflight, HrtfPanPathRejectReason& reason);
    HrtfAttenuationCouplingRejectReason rejectReason = HrtfAttenuationCouplingRejectReason::None;
    HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason& reason,
                                                  HrtfAttenuationCouplingRejectReason expected,
HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const Vec3& rel_listener);
    HrtfBinauralRejectReason rejectReason = HrtfBinauralRejectReason::None;
                                 HrtfBinauralPreflight& preflight, HrtfBinauralRejectReason& reason,

// --- deepen additive from deepen-hrtf-reject-reason-preflights-cb75 ---
const char* hrtfIrRejectReasonName(HrtfIrRejectReason reason);
HrtfIrRejectReason hrtfIrRejectReason(const HrtfIrStub& ir);
bool hrtfIrRejectsForReason(const HrtfIrStub& ir, HrtfIrRejectReason expected);
const char* hrtfPanPathRejectReasonName(HrtfPanPathRejectReason reason);
HrtfPanPathRejectReason hrtfPanPathRejectReason(bool hrtf_enabled, const Vec3& rel_listener);
const char* hrtfAttenuationCouplingRejectReasonName(HrtfAttenuationCouplingRejectReason reason);
HrtfAttenuationCouplingRejectReason hrtfAttenuationCouplingRejectReason(
struct HrtfBinauralPreflight;
const char* hrtfBinauralRejectReasonName(HrtfBinauralRejectReason reason);
HrtfBinauralRejectReason hrtfBinauralRejectReason(const HrtfBinauralPreflight& preflight);
HrtfIrRejectReason hrtfBinauralConvolutionRejectReason(const HrtfBinauralPreflight& preflight);
HrtfAttenuationCouplingRejectReason hrtfBinauralNarrowingRejectReason(
bool hrtfBinauralRejectsForReason(const HrtfBinauralPreflight& preflight,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-0339 ---
    bool is_bypass() const { return rejectReason != HrtfBinauralRejectReason::None; }

// --- deepen additive from deepen-b72-hrtf-reject-reasons-54bb ---
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      float occlusion_gain, HrtfBinauralRejectReason expected,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-db94 ---
                                      float occlusion_gain, HrtfBinauralRejectReason expected);

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e7b1 ---
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan(HrtfPanPathRejectReason reason);
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_ir(HrtfIrRejectReason reason);
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_coupling(

// --- deepen additive from deepen-b72-hrtf-reject-reasons-a636 ---
bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason);
bool hrtf_pan_path_reject_reason_is_blocking_spatial(HrtfPanPathRejectReason reason);
bool hrtf_pan_path_reject_reason_is_blocking_convolution(HrtfPanPathRejectReason reason);
HrtfPanPathRejectReason classify_hrtf_pan_path_spatial_reject(bool hrtf_enabled,
HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(bool hrtf_enabled,
    HrtfPanPathRejectReason spatialRejectReason = HrtfPanPathRejectReason::None;
    HrtfPanPathRejectReason convolutionRejectReason = HrtfPanPathRejectReason::None;
bool hrtf_attenuation_coupling_reject_reason_is_blocking(HrtfAttenuationCouplingRejectReason reason);
bool hrtf_binaural_reject_reason_is_blocking_spatial(HrtfBinauralRejectReason reason);
bool hrtf_binaural_reject_reason_is_blocking_convolution(HrtfBinauralRejectReason reason);
bool hrtf_binaural_reject_reason_is_blocking_coupling(HrtfBinauralRejectReason reason);
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(bool hrtf_enabled,
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(bool hrtf_enabled,
HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(HrtfPanPath path,
                                     HrtfBinauralRejectReason* reason = nullptr);
    HrtfBinauralRejectReason spatialRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason couplingRejectReason = HrtfBinauralRejectReason::None;

// --- deepen additive from b72-hrtf-reject-reason-preflights-6c04 ---
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& out, HrtfIrRejectReason& reason);
    bool should_skip() const { return reason != HrtfPanPathRejectReason::None; }
                                 HrtfPanPathPreflight& out, HrtfPanPathRejectReason& reason);
    bool should_skip() const { return reason != HrtfAttenuationCouplingRejectReason::None; }
    HrtfAttenuationCouplingPreflight& out, HrtfAttenuationCouplingRejectReason& reason,
                                      const Vec3& rel_listener, HrtfBinauralRejectReason expected);
bool hrtf_binaural_reject_reason_is_bypass(HrtfBinauralRejectReason reason);
bool hrtf_binaural_reject_reason_blocks_convolution(HrtfBinauralRejectReason reason);
                                 HrtfBinauralPreflight& out, HrtfBinauralRejectReason& reason,

// --- deepen additive from b72-hrtf-reject-reason-preflights-d934 ---
const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason);
    bool rejected() const { return reason != HrtfIrRejectReason::None; }
const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, HrtfPanPathRejectReason expected);
const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason);
const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason);
    bool rejected() const { return reason != HrtfBinauralRejectReason::None; }

// --- deepen additive from deepen-b72-hrtf-reject-reasons-4c1f ---
bool is_blocking_hrtf_binaural_reject_reason(HrtfBinauralRejectReason reason);

// --- deepen additive from deepen-b7-2-hrtf-reject-preflights-f8d6 ---
bool hrtf_ir_preflight_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected);
HrtfPanPathRejectReason classify_hrtf_pan_path_skip_reject(bool hrtf_enabled, const Vec3& rel_listener);
    HrtfPanPathRejectReason skipReason = HrtfPanPathRejectReason::None;
bool hrtf_pan_path_preflight_skips_for_reason(const HrtfPanPathPreflight& preflight,
bool hrtf_pan_path_preflight_convolution_rejects_for_reason(const HrtfPanPathPreflight& preflight,
    bool should_skip() const { return !can_narrow(); }
                                             float occlusion_gain, HrtfAttenuationCouplingPreflight& preflight,
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason expected);
HrtfBinauralRejectReason classify_hrtf_binaural_skip_reject(bool hrtf_enabled, const Vec3& rel_listener);
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(HrtfPanPath path,
    HrtfBinauralRejectReason skipReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason narrowingRejectReason = HrtfBinauralRejectReason::None;
bool hrtf_binaural_preflight_skips_for_reason(const HrtfBinauralPreflight& preflight,
bool hrtf_binaural_preflight_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
bool hrtf_binaural_preflight_narrowing_rejects_for_reason(const HrtfBinauralPreflight& preflight,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-2fef ---
bool is_blocking_hrtf_ir_reject_reason(HrtfIrRejectReason reason);
HrtfIrPreflight try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
bool is_blocking_hrtf_pan_path_reject_reason(HrtfPanPathRejectReason reason);
HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
bool is_blocking_hrtf_attenuation_coupling_reject_reason(HrtfAttenuationCouplingRejectReason reason);
HrtfAttenuationCouplingPreflight try_preflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling = {},
bool is_blocking_hrtf_binaural_convolution_reject_reason(HrtfBinauralRejectReason reason);
HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  float occlusion_gain, HrtfBinauralRejectReason& reason,
HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-aa20 ---
bool hrtf_pan_path_reject_reason_is_blocking(HrtfPanPathRejectReason reason);
                                             HrtfAttenuationCouplingRejectReason& reason);
bool hrtf_binaural_reject_reason_is_blocking(HrtfBinauralRejectReason reason);
    bool can_spatial_pan() const { return reason == HrtfBinauralRejectReason::None; }
                              const BinauralPanParams& params, HrtfBinauralRejectReason* reason);
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason);

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e7e2 ---
    HrtfAttenuationCouplingPreflight& preflight, const HrtfAttenuationCoupling& coupling = {},
HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(bool hrtf_enabled, const Vec3& rel_listener);
HrtfBinauralRejectReason classify_hrtf_binaural_attenuation_reject(
bool hrtf_binaural_reject_reason_is_pan_blocking(HrtfBinauralRejectReason reason);
bool hrtf_binaural_reject_reason_is_convolution_blocking(HrtfBinauralRejectReason reason);
    HrtfBinauralRejectReason panRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason attenuationRejectReason = HrtfBinauralRejectReason::None;

// --- deepen additive from deepen-b72-hrtf-reject-preflights-e16c ---
    HrtfBinauralRejectReason spatialPanReject = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionReject = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason narrowingReject = HrtfBinauralRejectReason::None;
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-494b ---
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);
bool hrtf_ir_reject_reason_blocks_convolution(HrtfIrRejectReason reason);
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);
bool hrtf_pan_path_reject_reason_blocks_spatial_pan(HrtfPanPathRejectReason reason);
enum class HrtfBinauralConvolutionRejectReason : u8 {
enum class HrtfBinauralNarrowingRejectReason : u8 {
const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason);
const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason);
HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
bool hrtf_binaural_reject_reason_blocks_spatial_pan(HrtfBinauralRejectReason reason);

// --- deepen additive from deepen-hrtf-b7-2-guards-64dd ---
    HrtfBinauralRejectReason spatialPanRejectReason = HrtfBinauralRejectReason::None;

// --- deepen additive from deepen-b7.2-hrtf-reject-reasons-a8a9 ---
bool hrtfIrRejectReasonIsBlocking(HrtfIrRejectReason reason);
HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrStub& ir);
bool tryPreflightHrtfIr(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
bool hrtfPanPathRejectReasonIsBlocking(HrtfPanPathRejectReason reason);
HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const Vec3& rel_listener);
bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
bool hrtfAttenuationCouplingRejectReasonIsBlocking(HrtfAttenuationCouplingRejectReason reason);
HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(
bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
bool hrtfBinauralRejectReasonIsBypass(HrtfBinauralRejectReason reason);
bool hrtfBinauralRejectReasonBlocksConvolution(HrtfBinauralRejectReason reason);
HrtfBinauralRejectReason classifyHrtfBinauralReject(
bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from hrtf-b7-2-deepen-guards-653a ---
HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const HrtfIrStub& ir,
bool hrtfBinauralRejectReasonIsBlocking(HrtfBinauralRejectReason reason);
HrtfBinauralRejectReason classifyHrtfBinauralReject(const HrtfBinauralPreflight& preflight);

// --- deepen additive from deepen-b72-hrtf-reject-reasons-a304 ---
    HrtfAttenuationCouplingRejectReason* reason = nullptr,
HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight);
HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(
                                   float occlusion_gain, HrtfBinauralRejectReason* reason = nullptr,
                                   HrtfBinauralRejectReason* reason = nullptr,

// --- deepen additive from b72-hrtf-reject-reason-guards-3aaf ---
bool preflight_hrtf_ir_convolution_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool preflight_hrtf_ir_convolution_ready(const HrtfIrPreflight& preflight,
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
bool try_preflight_hrtf_ir_convolution(const HrtfIrPreflight& preflight, HrtfIrRejectReason& reason);
                                      const Vec3& rel_listener, HrtfPanPathRejectReason* reason = nullptr);
bool preflight_hrtf_pan_spatial_ready(const HrtfPanPathPreflight& preflight,
bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_pan_spatial(const HrtfPanPathPreflight& preflight,
bool should_skip_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener);
bool try_preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
bool try_preflight_hrtf_attenuation_narrowing(
bool should_skip_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight);
bool preflight_hrtf_binaural_spatial_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason);
bool try_preflight_hrtf_binaural_spatial(const HrtfBinauralPreflight& preflight,
bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,
bool preflight_hrtf_binaural_narrowing_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_binaural_narrowing(const HrtfBinauralPreflight& preflight,

// --- deepen additive from hrtf-reject-reason-enums-2d4f ---
enum class HrtfConvolutionRejectReason : u8 {
const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason);
HrtfConvolutionRejectReason hrtf_convolution_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                         HrtfConvolutionRejectReason expected);
    HrtfConvolutionRejectReason convolutionReason = HrtfConvolutionRejectReason::None;
HrtfConvolutionRejectReason hrtf_binaural_convolution_reject_reason(
bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-0e3e ---
bool tryPreflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
bool tryPreflight_hrtf_attenuation_coupling(
bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-cf7e ---
bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected);
bool hrtf_pan_path_rejects_for_reason(const HrtfPanPathPreflight& preflight,
HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfBinauralPreflight& preflight);
bool hrtf_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-69f7 ---
    HrtfIrRejectReason reject = HrtfIrRejectReason::None;
    HrtfPanPathRejectReason reject = HrtfPanPathRejectReason::None;
    HrtfAttenuationCouplingRejectReason reject = HrtfAttenuationCouplingRejectReason::None;
enum class HrtfBinauralConvolveRejectReason : u8 {
    HrtfBinauralRejectReason reject = HrtfBinauralRejectReason::None;
    HrtfBinauralConvolveRejectReason convolveReject = HrtfBinauralConvolveRejectReason::None;
const char* hrtf_binaural_convolve_reject_reason_label(HrtfBinauralConvolveRejectReason reason);
HrtfBinauralConvolveRejectReason classify_hrtf_binaural_convolve_reject(

// --- deepen additive from deepen-b72-hrtf-reject-reasons-d5f8 ---
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_pan_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_pan_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e6c1 ---
bool hrtf_pan_path_matches_preflight(HrtfPanPath path, const HrtfPanPathPreflight& preflight);
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan_path(HrtfPanPathRejectReason reason);
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_attenuation(
HrtfBinauralRejectReason hrtf_binaural_skip_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);
HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(const HrtfIrStub& ir);
HrtfBinauralRejectReason hrtf_binaural_attenuation_reject_reason(
    HrtfBinauralRejectReason attenuationReason = HrtfBinauralRejectReason::None;

// --- deepen additive from b72-hrtf-reject-reasons-9d6b ---
enum class HrtfPanPathConvolutionRejectReason : u8 {
const char* hrtf_pan_path_convolution_reject_reason_label(HrtfPanPathConvolutionRejectReason reason);
    HrtfPanPathConvolutionRejectReason convolutionReason = HrtfPanPathConvolutionRejectReason::None;
HrtfPanPathConvolutionRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir_preflight);
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir_preflight,
    HrtfPanPathConvolutionRejectReason expected);
bool hrtf_attenuation_coupling_rejects_for_reason(const HrtfAttenuationCouplingPreflight& preflight,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-1b78 ---
enum class HrtfPanConvolutionRejectReason : u8 {
const char* hrtf_pan_convolution_reject_reason_label(HrtfPanConvolutionRejectReason reason);
HrtfPanConvolutionRejectReason classify_hrtf_pan_convolution_reject(
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir);
                                          HrtfPanConvolutionRejectReason* reason = nullptr);
                                        HrtfPanConvolutionRejectReason& reason);

// --- deepen additive from deepen-b72-hrtf-reject-enums-0847 ---
bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool try_preflight_hrtf_pan_path_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, HrtfIrRejectReason& reason);
HrtfPanPathRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight);

// --- deepen additive from deepen-b72-hrtf-reject-reasons-d95a ---
bool should_skip_hrtf_ir_ready(const HrtfIrStub& ir);
bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener);
bool should_skip_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfConvolutionRejectReason* reason = nullptr);
bool try_preflight_hrtf_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                    const Vec3& rel_listener, HrtfConvolutionRejectReason& reason);
bool should_skip_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
