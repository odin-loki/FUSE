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

/// True when listener and source share the same listener-local position.
bool is_co_located_hrtf_source(const Vec3& rel_listener);

/// Canonical empty IR stub for guard fallbacks.
HrtfIrStub make_empty_hrtf_ir();

/// Validated IR stub factory — returns empty when samples are null or length is zero.
/// Build an IR stub from external sample data (may still be empty).
HrtfIrStub make_hrtf_ir_stub(const float* samples, u32 length);

/// Read-only empty-IR guard diagnostics (B7.2 deepen follow-up).
struct HrtfIrPreflight {
    bool empty_ir = true;
    bool null_samples = true;
    bool zero_length = true;
    u32 length = 0;

    [[nodiscard]] bool can_use_convolution() const { return !empty_ir; }
    [[nodiscard]] bool should_fallback_to_stub() const { return empty_ir; }
};

/// Preflight an HRTF IR stub without mutating state.
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// Convenience guard — `preflight_hrtf_ir(ir).can_use_convolution()`.
[[nodiscard]] bool can_use_hrtf_convolution(const HrtfIrStub& ir);

/// True when an HRTF IR stub has non-null, non-empty sample data.
bool has_hrtf_ir(const HrtfIrStub& ir);

/// Readable alias — true when IR samples are null or zero-length.
/// True when an HRTF IR stub is null or zero-length — inverse of `has_hrtf_ir`.
bool is_empty_hrtf_ir(const HrtfIrStub& ir);

/// Read-only empty-IR diagnostics — no mutation (B7.2 deepen follow-up).
struct HrtfIrPreflight {
    bool null_samples = true;
    bool zero_length = true;
    bool empty_ir = true;

    [[nodiscard]] bool can_use_convolution() const { return !empty_ir; }
};

/// Preflight an HRTF IR stub before selecting the convolution path.
HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

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

/// Preflight an HRTF IR stub before convolution dispatch.
HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// Non-mutating convolution predicate — mirrors \c HrtfIrPreflight::can_convolve.
bool can_convolve_hrtf_ir(const HrtfIrPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfIrPreflight::should_skip_convolution.
bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight);
/// True when IR stub cannot select convolution (null or zero-length samples).
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir);

/// Readable alias — empty IR falls back to ILD/ITD stub path.
bool should_fallback_hrtf_to_ild_itd_stub(const HrtfIrStub& ir);

/// True when enabled, non-co-located source with empty IR selects ILD/ITD stub.
bool should_fallback_hrtf_to_ild_itd_stub(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener);

/// True when enabled, non-co-located source with valid IR selects convolution.
bool should_use_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir,
/// True when convolution is unavailable and ILD/ITD stub pan should be used.
bool should_fallback_to_ild_itd_stub(const HrtfIrStub& ir);

/// Return a validated IR stub — empty when samples are null or length is zero.
HrtfIrStub normalize_hrtf_ir_stub(const HrtfIrStub& ir);

/// Safe sample count — zero when the IR stub is empty.
u32 hrtf_ir_stub_sample_count(const HrtfIrStub& ir);
/// Empty-IR preflight for stub convolution wiring (B7.2 HRTF deepen follow-up).
    bool has_samples = false;
    bool length_ok = false;
    bool is_empty = true;
    u32 ir_length = 0;

    [[nodiscard]] bool can_use_convolution() const;
    [[nodiscard]] bool should_fallback_to_ild_itd() const;
    [[nodiscard]] bool ready_for_stub() const;

/// Build empty-IR preflight from an HRTF IR stub.
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// HRTF pan routing — empty IR uses ILD/ITD stub; convolution deferred until IR wired.
enum class HrtfPanPath {
    Bypass,
    IldItdStub,
    Convolution,
};

/// Why pan-path resolution selected bypass (B7.2 HRTF deepen preflight).
enum class HrtfPanRejectReason : u8 {
    None = 0,
    Disabled,
    CoLocated,
};

/// Human-readable label for pan-path reject reasons (logging / tests).
const char* hrtf_pan_reject_reason_label(HrtfPanRejectReason reason);

/// Pan-path preflight for stub mix wiring (B7.2 HRTF deepen).
struct HrtfPanPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfPanRejectReason reject_reason = HrtfPanRejectReason::None;
    bool hrtf_enabled = false;
    bool co_located = false;
    bool has_valid_ir = false;

    [[nodiscard]] bool can_apply_spatial_pan() const;
    [[nodiscard]] bool skip_convolution() const;
    [[nodiscard]] bool ready_for_stub_mix() const;

/// Attenuation-coupling preflight for path-aware narrowing (B7.2 HRTF deepen).
struct HrtfAttenuationCouplingPreflight {
    float distance_attenuation = 1.f;
    float occlusion_gain = 1.f;
    bool unity_attenuation = true;
    bool skip_coupling = true;

    [[nodiscard]] bool can_narrow_image() const;
    [[nodiscard]] bool ready_for_coupling() const;

/// True when IR convolution can be skipped (null or zero-length samples).
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir);

/// True when IR convolution may run (non-empty IR stub).
bool should_apply_hrtf_ir_convolution(const HrtfIrStub& ir);
/// Read-only HRTF pan-path guard diagnostics (B7.2 deepen follow-up).
struct HrtfPanPathPreflight {
    bool hrtf_disabled = false;
    bool empty_ir = true;
    bool bypass = true;
    bool spatial = false;
    bool uses_convolution = false;
    bool uses_ild_itd_stub = false;

    [[nodiscard]] bool can_apply_spatial_pan() const { return spatial; }
    [[nodiscard]] bool should_skip_spatial_pan() const { return bypass; }

/// Preflight HRTF pan routing without computing binaural gains.
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                                           const Vec3& rel_listener);

/// Preflight HRTF pan routing when no IR is wired (ILD/ITD stub or bypass).
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Convenience guard — `preflight_hrtf_pan_path(...).can_apply_spatial_pan()`.
[[nodiscard]] bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,

/// Select pan path from HRTF enable flag, IR stub, and listener-local offset.
HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);

/// Diagnose pan-path resolution; returns false when bypassed (disabled or co-located).
bool try_resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                HrtfPanPath& out_path, HrtfPanRejectReason& out_reason);

/// One-shot pan-path preflight from enable flag, IR stub, and listener-local offset.
HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                    const Vec3& rel_listener);

/// One-shot pan-path preflight when no IR is wired.
HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);

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

/// Read-only pan-path diagnostics — no mutation (B7.2 deepen follow-up).
    bool hrtf_disabled = false;
    bool co_located = false;
    bool empty_ir = true;

    [[nodiscard]] bool can_apply_spatial_pan() const { return path != HrtfPanPath::Bypass; }
    [[nodiscard]] bool uses_convolution() const { return path == HrtfPanPath::Convolution; }
    [[nodiscard]] bool uses_ild_itd_stub() const { return path == HrtfPanPath::IldItdStub; }


/// Preflight HRTF pan routing when no IR is wired (ILD/ITD stub or bypass).

/// True when the resolved path produces a lateral spatial image (not centre bypass).
bool is_spatial_hrtf_pan_path(HrtfPanPath path);

/// True when the pan path selects convolution (non-empty IR stub).
bool hrtf_pan_path_uses_convolution(HrtfPanPath path);

/// Readable alias for \c hrtf_pan_path_uses_convolution.
bool is_convolution_hrtf_pan_path(HrtfPanPath path);

/// True when the pan path selects ILD/ITD stub (empty IR fallback).
bool hrtf_pan_path_uses_ild_itd_stub(HrtfPanPath path);

/// True when the pan path is centre bypass (disabled or co-located).
bool is_hrtf_pan_path_bypass(HrtfPanPath path);

/// Readable alias for \c is_hrtf_pan_path_bypass.
bool is_bypass_hrtf_pan_path(HrtfPanPath path);

/// Early-out: true when spatial panning should be skipped (bypass path).
bool should_skip_hrtf_spatial_pan(HrtfPanPath path);

/// True when the pan path is centre-mono bypass.
/// Inverse of \c should_skip_hrtf_spatial_pan — spatial path produces lateral image.
bool should_apply_hrtf_spatial_pan(HrtfPanPath path);
/// Pan-path preflight for stub HRTF routing (B7.2 HRTF deepen follow-up).
struct HrtfPanPathPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfIrPreflight ir{};
    bool hrtf_enabled = false;
    bool co_located = false;
    bool skip_spatial_pan = true;
    bool uses_convolution = false;
    bool uses_ild_itd_stub = false;

    [[nodiscard]] bool can_apply_spatial_pan() const;
    [[nodiscard]] bool should_bypass() const;
    [[nodiscard]] bool ready_for_stub() const;
};

/// Build pan-path preflight from HRTF enable flag, IR stub, and listener-local offset.
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                                            const Vec3& rel_listener);

/// Build pan-path preflight when no IR is wired (ILD/ITD stub or bypass).
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// True when listener and source share the same listener-local position.
bool is_co_located_hrtf_source(const Vec3& rel_listener);

/// Early-out inverse of \c should_apply_hrtf_pan.
bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);


/// Alias for `is_hrtf_pan_bypassed` — skip pan/coupling when true.

/// True when HRTF pan should run (enabled and source is not co-located).
bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);

/// Inverse of `should_apply_hrtf_pan` — disabled or co-located sources.
bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);
/// True when the pan path uses the ILD/ITD stub (empty or unwired IR).
bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path);

/// True when the pan path is centre bypass (no lateral image).


/// True when HRTF pan should not run (disabled or co-located).

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

/// Stereo sample pair produced by applying binaural pan gains to a mono input.
struct BinauralStereoSample {
    float left = 0.f;
    float right = 0.f;
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

/// Path-aware sample stub — bypass uses centre mono; spatial paths use per-ear gains.
void apply_binaural_pan_to_sample_for_path(HrtfPanPath path, float mono,
                                           const BinauralPanGains& pan, float attenuation,
                                           float& left, float& right);

/// Centre mono fallback — equal L/R contribution with attenuation.
void apply_centre_binaural_pan_to_sample(float mono, float attenuation, float& left, float& right);

/// One-shot sample apply with HRTF bypass guards (disabled / co-located → centre mono).
void apply_guarded_binaural_pan_to_sample(bool hrtf_enabled, const Vec3& rel_listener, float mono,
                                          const BinauralPanGains& pan, float attenuation,
                                          float& left, float& right);

/// IR-aware guarded sample apply — empty IR keeps spatial stub; bypass only when disabled/co-located.
void apply_guarded_binaural_pan_to_sample(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float mono,
/// Path-aware sample dispatch — bypass uses centre mono, spatial paths use pan gains.
void apply_binaural_pan_for_path_to_sample(HrtfPanPath path, float mono, const BinauralPanGains& pan,
                                           float attenuation, float& left, float& right);

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

/// Scale per-ear gains in place; ITD is preserved.
void scale_binaural_pan_gains(BinauralPanGains& gains, float scale);

/// Scale per-ear gains and return a clamped copy; ITD is preserved.
BinauralPanGains scale_binaural_pan_gains_copy(const BinauralPanGains& gains, float scale);

/// Apply mono sample through pan gains and output attenuation.
BinauralStereoSample apply_binaural_pan_gains(float mono, const BinauralPanGains& pan,
                                              float output_attenuation = 1.f);

/// Accumulate a mono sample into stereo bus channels via pan gains and attenuation.
void accumulate_binaural_pan(float mono, const BinauralPanGains& pan, float output_attenuation,
                             float& left, float& right);

/// Narrow or widen L/R spread — blend 1 preserves image, 0 collapses to mono centre.
void apply_spatial_blend(BinauralPanGains& gains, float blend);

/// Apply spatial blend only when blend is below unity (no-op at full separation).
/// Apply spatial blend only when blend is below unity.
void apply_hrtf_spatial_blend_guarded(BinauralPanGains& gains, float blend);

/// Attenuation coupling — blends distance and occlusion into spatial image narrowing.
struct HrtfAttenuationCoupling {
    float occlusion_weight = 0.5f;
};

/// Read-only attenuation-coupling guard diagnostics (B7.2 deepen follow-up).
struct HrtfAttenuationCouplingPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    bool bypass_path = true;
    bool unity_attenuation = true;
    bool skipped = true;
    bool would_narrow = false;
    float distance_attenuation = 1.f;
    float occlusion_gain = 1.f;

    [[nodiscard]] bool can_apply() const { return !skipped && would_narrow; }
    [[nodiscard]] bool should_skip() const { return skipped; }
};

/// Preflight distance/occlusion spatial narrowing without mutating binaural gains.
[[nodiscard]] HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain);

/// Convenience guard — `preflight_hrtf_attenuation_coupling(...).can_apply()`.
[[nodiscard]] bool can_apply_hrtf_attenuation_coupling_narrowing(HrtfPanPath path,
                                                                  float distance_attenuation,
                                                                  float occlusion_gain);

/// Clamp distance or occlusion attenuation scalars into [0, 1].
float clamp_hrtf_attenuation(float attenuation);

/// Clamp occlusion blend weight into [0, 1].
float clamp_hrtf_attenuation_coupling_weight(float weight);

/// True when distance and occlusion are both fully audible (no narrowing).
bool is_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain);
float clamp_hrtf_occlusion_coupling_weight(float weight);

/// True when distance attenuation is fully audible (no distance narrowing).
bool is_unity_hrtf_distance_attenuation(float distance_attenuation);

/// True when occlusion LF gain is fully audible.
bool is_unity_hrtf_occlusion_gain(float occlusion_gain);

/// Early-out when both distance and occlusion are unity — coupling is a no-op.
bool should_skip_hrtf_attenuation_coupling(float distance_attenuation, float occlusion_gain);

/// True when distance attenuation warrants spatial narrowing.
bool should_apply_hrtf_distance_coupling(float distance_attenuation);

/// True when occlusion gain warrants spatial narrowing.
bool should_apply_hrtf_occlusion_coupling(float occlusion_gain);

/// True when distance or occlusion attenuation is below full audibility.
bool is_non_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain);

/// True when distance/occlusion coupling should narrow the binaural image.
bool should_apply_hrtf_attenuation_coupling(HrtfPanPath path);

/// Combined guard — spatial path and non-unity attenuation warrant coupling.
bool should_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                            float occlusion_gain);

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

/// Preflight distance + occlusion coupling before spatial image narrowing.
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

/// Non-mutating narrowing predicate — mirrors \c HrtfAttenuationCouplingPreflight::can_narrow.
bool can_narrow_hrtf_spatial_image(const HrtfAttenuationCouplingPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfAttenuationCouplingPreflight::should_skip.
bool should_skip_hrtf_attenuation_coupling_preflight(const HrtfAttenuationCouplingPreflight& preflight);

/// True when attenuation coupling should not narrow the binaural image.
/// Early-out inverse of `should_apply_hrtf_attenuation_coupling`.

/// Clamp occlusion blend weight into [0, 1].
float clamp_hrtf_occlusion_coupling_weight(float weight);

/// True when distance and occlusion are both fully audible (no narrowing).
bool is_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain);

/// True when a spatial blend preserves full L/R separation.
bool is_unity_hrtf_spatial_blend(float blend, float epsilon = 1e-5f);

/// True when distance/occlusion scalars produce unity spatial blend — skip narrowing.
bool should_skip_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,

/// Combined guard — spatial path and non-unity attenuation warrant narrowing.
bool should_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain);
/// Early-out inverse of \c should_narrow_hrtf_spatial_image.
bool should_preserve_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
/// True when attenuation coupling mapping can be bypassed (unity distance and occlusion).
bool should_skip_hrtf_attenuation_coupling_mapping(float distance_attenuation,

/// One-shot attenuation-coupling preflight for a resolved pan path.

/// Combined pan + attenuation preflight for coupled one-shot helpers.
HrtfAttenuationCouplingPreflight preflight_hrtf_coupled_pan(bool hrtf_enabled,
                                                            const HrtfIrStub& ir,
                                                            const Vec3& rel_listener,
                                                            float distance_attenuation,

/// Combined pan + attenuation preflight when no IR is wired.

/// Attenuation-coupling preflight for stub spatial-image narrowing (B7.2 HRTF deepen follow-up).
    HrtfPanPath path = HrtfPanPath::Bypass;
    float distance_attenuation = 1.f;
    float occlusion_gain = 1.f;
    float coupling_weight = 0.5f;
    bool unity_attenuation = true;
    bool skip_coupling = true;
    bool should_narrow = false;
    float spatial_blend = 1.f;

    [[nodiscard]] bool can_apply_coupling() const;
    [[nodiscard]] bool ready_for_stub() const;

/// Build attenuation-coupling preflight for a resolved pan path.
[[nodiscard]] HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {});

/// Combined spatial pan preflight bundling IR, pan path, and attenuation coupling (B7.2 HRTF deepen).
struct HrtfSpatialPanPreflight {
    HrtfPanPathPreflight pan{};
    HrtfAttenuationCouplingPreflight coupling{};

    [[nodiscard]] bool can_apply_spatial_pan() const;
    [[nodiscard]] bool can_apply_attenuation_coupling() const;

/// Build combined spatial pan preflight for one-shot guarded pan wiring.
[[nodiscard]] HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(
    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    float occlusion_gain, const HrtfAttenuationCoupling& coupling = {},

/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen follow-up).
    bool bypass_path = true;
    bool spatial_path = false;

    [[nodiscard]] bool can_apply_coupling() const { return spatial_path && !unity_attenuation; }
    [[nodiscard]] bool should_narrow() const { return can_apply_coupling(); }

/// Preflight distance/occlusion coupling before narrowing the binaural image.

/// Combined spatial blend from distance attenuation and occlusion LF gain.
float compute_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// True when spatial blend is at or above unity — coupling is a no-op.
bool is_fully_spatial_hrtf_blend(float spatial_blend, float epsilon = 1e-5f);

/// True when distance and occlusion leave the binaural image fully separated.
bool should_skip_hrtf_attenuation_coupling(float distance_attenuation, float occlusion_gain,
                                           const HrtfAttenuationCoupling& coupling = {},
                                           const BinauralPanParams& params = {});
/// Path-aware spatial blend — bypass returns unity (no narrowing).
float compute_hrtf_spatial_blend_for_path(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
/// Path-aware spatial blend — bypass and unity attenuation preserve full separation.
float compute_hrtf_spatial_blend_guarded(HrtfPanPath path, float distance_attenuation,

/// Apply distance + occlusion coupling to narrow the binaural image toward mono centre.
void apply_hrtf_attenuation_coupling(BinauralPanGains& gains, float distance_attenuation,
                                     float occlusion_gain,
                                     const HrtfAttenuationCoupling& coupling = {},
                                     const BinauralPanParams& params = {});

/// Resolve pan path, then apply coupling only when spatial and attenuation is non-unity.
void apply_hrtf_attenuation_coupling_guarded(BinauralPanGains& gains, bool hrtf_enabled,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling = {},
                                             const BinauralPanParams& params = {});

/// IR-aware guarded coupling — empty IR keeps ILD/ITD stub path with coupling guards.
void apply_hrtf_attenuation_coupling_guarded(BinauralPanGains& gains, bool hrtf_enabled,
                                             const HrtfIrStub& ir, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
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

} // namespace fuse::audio
