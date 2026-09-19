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

/// Read-only empty-IR diagnostics — no mutation (B7.2 deepen).
struct HrtfIrPreflight {
    bool null_samples = false;
    bool zero_length = false;

    [[nodiscard]] bool is_empty() const { return null_samples || zero_length; }
    [[nodiscard]] bool has_valid_ir() const { return !is_empty(); }
};

/// Preflight empty-IR guard checks for an HRTF impulse-response stub.
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// Preflight empty-IR guard checks from raw sample pointer and length.
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const float* samples, u32 length);

/// Convenience guard — `preflight_hrtf_ir(ir).has_valid_ir()`.
[[nodiscard]] bool can_use_hrtf_ir_preflight(const HrtfIrStub& ir);
    bool empty_ir = true;

    bool can_use_convolution() const { return !empty_ir; }

/// Preflight empty-IR stub — surfaces null samples and zero-length guards.
HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);
/// Why HRTF IR convolution preflight rejected the stub (B7.2 deepen).
enum class HrtfIrPreflightRejectReason : u8 {
    None = 0,
    NullSamples = 1,
    ZeroLength = 2,

/// Why HRTF pan-path preflight would bypass spatial processing (B7.2 deepen).
enum class HrtfPanPreflightRejectReason : u8 {
    Disabled = 1,
    CoLocated = 2,

/// Why attenuation-coupling preflight would skip spatial narrowing (B7.2 deepen).
enum class HrtfAttenuationCouplingPreflightRejectReason : u8 {
    BypassPath = 1,
    UnityAttenuation = 2,
/// Empty-IR reject reason for convolution preflight (B7.2 deepen).
enum class HrtfIrRejectReason {
    None,
    NullSamples,
    ZeroLength,

/// Const preflight for HRTF IR convolution dispatch (B7.2 deepen).
    HrtfIrRejectReason reason = HrtfIrRejectReason::None;
    bool rejected = false;

    bool can_convolve() const { return !rejected; }

/// Populate IR preflight without running convolution (B7.2 deepen).

/// Reject reason for an IR stub — \c None when convolution may run.
HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir);

/// True when an IR stub is malformed (null samples with non-zero length, or non-null zero-length).
bool is_malformed_hrtf_ir(const HrtfIrStub& ir);

/// True when empty IR should fall back to ILD/ITD stub pan (alias of \c should_skip_hrtf_convolution).
bool should_fallback_to_ild_itd_stub(const HrtfIrStub& ir);
/// Why HRTF IR convolution preflight rejects the stub (B7.2 deepen).
enum class HrtfIrRejectReason : u8 {
    MalformedIr,

/// Human-readable label for empty-IR reject reasons (B7.2 deepen).
const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason);

/// Classify why an HRTF IR stub cannot convolve (B7.2 deepen).
/// Why HRTF IR convolution would be skipped (B7.2 deepen).
    EmptyIr,

/// Human-readable label for empty-IR reject reasons (logging / tests).
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);

/// Diagnose why IR convolution would skip; vacuously succeeds when convolution may proceed.
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir);

/// True when `classify_hrtf_ir_reject` matches `expected` (B7.2 deepen).
/// Why HRTF IR convolution is rejected (B7.2 deepen).

/// Human-readable label for HRTF IR reject reasons (B7.2 deepen).
const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason);

/// Diagnose why IR convolution is skipped; `None` when convolution may proceed.

/// Returns true when `hrtf_ir_reject_reason` matches `expected` (B7.2 deepen).
/// Why HRTF IR convolution is rejected (B7.2 deepen pass).

/// Human-readable label for HRTF IR reject reasons (logging / tests).

/// Returns the first IR reject reason, or `None` when convolution may proceed.

/// Returns true when `hrtf_ir_reject_reason` matches `expected` (B7.2 deepen pass).
bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected);

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
/// Read-only empty-IR diagnostics (B7.2 deepen follow-up).

    [[nodiscard]] bool can_use_convolution() const { return !empty_ir; }
    [[nodiscard]] bool should_fallback_to_stub() const { return empty_ir; }
};

/// Preflight an HRTF IR stub without mutating state.
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// Convenience guard — `preflight_hrtf_ir(ir).can_use_convolution()`.
[[nodiscard]] bool can_use_hrtf_convolution(const HrtfIrStub& ir);

/// Preflight empty-IR checks without mutating the stub (B7.2 deepen follow-up).
/// Normalize an IR stub — null/zero-length samples collapse to the canonical empty IR.
HrtfIrStub preflight_hrtf_ir(const HrtfIrStub& ir);

/// True when convolution should be skipped (empty or invalid IR stub).
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir);

/// True when pan routing should fall back to the ILD/ITD stub (empty IR).
bool should_fallback_to_ild_itd_stub(const HrtfIrStub& ir);

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
/// Why an HRTF IR stub cannot drive convolution (B7.2 deepen).
/// Why HRTF IR convolution preflight rejected the stub (B7.2 deepen follow-up).
enum class HrtfIrRejectReason : u8 {
    None = 0,
/// Why empty-IR preflight rejected convolution (B7.2 deepen).
    None,
/// Why HRTF IR convolution would early-out (B7.2 deepen follow-up).
    EmptyIr,
/// Why HRTF IR convolution is blocked (B7.2 deepen).
/// Why HRTF IR convolution would early-out (B7.2 deepen).
    NullSamples,
    ZeroLength,
/// Why IR convolution preflight rejected the request (B7.2 deepen follow-up).
/// Why HRTF IR preflight rejected convolution dispatch (B7.2 deepen).
/// Why HRTF IR convolution would be skipped (B7.2 deepen reject-reason pass).
/// Why IR convolution preflight rejected the stub (B7.2 deepen reject-reason pass).
/// Why HRTF IR convolution preflight rejected the request (B7.2 deepen).
/// Why empty-IR / convolution preflight rejected the request (B7.2 deepen).
/// Why an HRTF IR stub cannot enter convolution dispatch (B7.2 deepen reject-reason pass).
    MalformedIr,
};

/// Human-readable label for empty-IR reject reasons (logging / tests).
const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason);

/// Diagnose why IR convolution would skip; vacuously succeeds on valid IR stubs.
HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir);

/// Returns true when \c hrtf_ir_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected);
/// Human-readable label for empty-IR reject reasons (B7.2 deepen follow-up).
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);

/// Classify why an HRTF IR stub cannot convolve (B7.2 deepen follow-up).
/// Human-readable label for empty-IR reject reasons (B7.2 deepen).





/// Classify why an HRTF IR stub cannot be convolved (B7.2 deepen).
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir);

/// Diagnose why IR convolution would skip; vacuously succeeds on valid stubs.

/// Returns true when \c hrtf_ir_reject_reason matches \p expected (B7.2 deepen follow-up).
/// Diagnostic reason an HRTF IR stub cannot convolve (B7.2 deepen follow-up).
    MalformedStub,

/// Human-readable label for diagnostics and test assertions (B7.2 deepen follow-up).

/// Returns the first reject reason for an IR stub, or \c None when convolution may proceed.

/// Why HRTF IR convolution would early-out (B7.2 deepen reject-reason pass).

/// Human-readable label for IR reject reasons (logging / tests).


/// Returns true when \c hrtf_ir_reject_reason matches \p expected.


/// Classify why an HRTF IR stub cannot convolve (B7.2 deepen).
const char* hrtfIrRejectReasonName(HrtfIrRejectReason reason);

HrtfIrRejectReason hrtfIrRejectReason(const HrtfIrStub& ir);

/// Returns true when \c hrtfIrRejectReason matches \p expected (B7.2 deepen).
bool hrtfIrRejectsForReason(const HrtfIrStub& ir, HrtfIrRejectReason expected);

/// Returns true when `hrtf_ir_reject_reason` matches `expected` (B7.2 deepen follow-up).
/// Why HRTF IR convolution would early-out (B7.2 deepen pass).




/// Returns true when `hrtf_ir_reject_reason` matches `expected` (B7.2 deepen follow-up pass).

/// Returns the first IR reject reason, or \c None when convolution may proceed.

/// Diagnostic reason convolution is rejected for an HRTF IR stub (B7.2 deepen).



/// Returns true when \c hrtf_ir_reject_reason matches \p expected (B7.2 deepen).

/// Human-readable label for diagnostics and test assertions (B7.2 deepen).


/// Why HRTF IR convolution is rejected (B7.2 deepen).


/// First reject reason for an IR stub, or \c None when convolution may proceed.

/// Why HRTF IR convolution would be skipped (B7.2 deepen follow-up pass).

/// Human-readable label for empty-IR reject reasons (B7.2 deepen follow-up pass).

/// Diagnose why IR convolution would skip; vacuously succeeds when convolution may proceed.


/// True when an IR reject reason blocks convolution dispatch.
bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason);

/// Classify why IR convolution would be skipped — same ordering as \c preflight_hrtf_ir.

/// Non-mutating IR convolution preflight — returns true when convolution may proceed.
bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);

/// Why empty-IR preflight rejects convolution dispatch (B7.2 deepen).


/// Classify why IR convolution would be rejected (B7.2 deepen).

/// Returns the first IR reject reason, or `None` when convolution may proceed.

/// True when an IR reject reason would block convolution dispatch.
bool is_blocking_hrtf_ir_reject_reason(HrtfIrRejectReason reason);

/// Returns true when `hrtf_ir_reject_reason` matches `expected` (B7.2 deepen reject-reason pass).


/// Classify why HRTF IR convolution would be rejected — same ordering as \c preflight_hrtf_ir.
/// True when an empty-IR reject reason would block convolution dispatch (B7.2 deepen).

/// Classify why IR convolution would be rejected — same ordering as \c preflight_hrtf_ir.

/// Classify why an HRTF IR stub would skip convolution; \c None when convolution may proceed.

/// True when \c classify_hrtf_ir_reject matches \p expected.

/// Why empty-IR preflight rejected convolution dispatch (B7.2 deepen).



/// Classify why IR convolution would be rejected — mirrors \c HrtfIrPreflight ordering.
const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason);

/// True when an IR reject reason would block convolution dispatch (B7.2 deepen).
bool hrtfIrRejectReasonIsBlocking(HrtfIrRejectReason reason);

/// Classify why HRTF IR convolution would reject — same ordering as `tryPreflightHrtfIr`.
HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrStub& ir);

/// IR convolution preflight with mandatory reject-reason output (B7.2 deepen).
bool tryPreflightHrtfIr(const HrtfIrStub& ir, HrtfIrRejectReason& reason);


/// Human-readable label for HRTF IR reject reasons (logging / tests).

/// True when an IR reject reason blocks convolution dispatch (B7.2 deepen).

/// Classify why HRTF IR convolution would be rejected (B7.2 deepen).
/// Diagnostic reason HRTF IR convolution cannot proceed (B7.2 deepen).




/// Why IR preflight rejected convolution dispatch (B7.2 deepen).
/// Why HRTF IR preflight rejected convolution (B7.2 deepen pass).

/// Human-readable label for IR reject diagnostics (B7.2 deepen pass).

/// Diagnose why an IR stub cannot convolve; vacuously succeeds on valid IR.

/// True when \c hrtf_ir_reject_reason matches \p expected.


/// Human-readable label for empty-IR reject reasons (B7.2 deepen pass).


/// Returns true when `hrtf_ir_reject_reason` matches `expected` (B7.2 deepen pass).

/// Empty-IR preflight diagnostics — read-only guard bundle (B7.2 deepen).
struct HrtfIrPreflight {
    HrtfIrRejectReason reason = HrtfIrRejectReason::None;
    bool rejected = false;




/// Why HRTF IR convolution was rejected (B7.2 deepen pass).


    bool emptyIr = false;
    bool nullSamples = false;
    bool zeroLength = false;
    bool malformedIr = false;
    HrtfIrRejectReason reason = HrtfIrRejectReason::None;
    HrtfIrRejectReason rejectReason = HrtfIrRejectReason::None;
    HrtfIrRejectReason reject = HrtfIrRejectReason::None;

    bool can_convolve() const { return !emptyIr && !malformedIr; }
    bool ok() const { return reason == HrtfIrRejectReason::None; }
    bool can_convolve() const { return reason == HrtfIrRejectReason::None; }

    bool rejected() const { return reason != HrtfIrRejectReason::None; }




    /// True when convolution dispatch should be skipped (empty or malformed IR).
    bool should_skip_convolution() const { return !can_convolve(); }
    bool ok() const { return can_convolve(); }
    bool ok() const { return reason == HrtfIrRejectReason::None; }
    bool can_convolve() const { return reason == HrtfIrRejectReason::None; }

    bool can_convolve() const { return !rejected && !emptyIr && !malformedIr; }

    /// True when IR preflight reports no convolution reject reason.
};

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason);
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// Preflight an HRTF IR stub before convolution dispatch.
HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);

/// IR preflight with mandatory reject-reason output (B7.2 deepen follow-up).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight);
/// Preflight an HRTF IR stub with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason);

/// Empty-IR preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
/// IR preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight,
                           HrtfIrRejectReason& reason);
/// Returns true when IR preflight passes (\c reason is \c None); writes reject reason on failure.
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& out, HrtfIrRejectReason& reason);

/// True when IR convolution preflight rejects for `expected` (B7.2 deepen).
bool hrtf_ir_preflight_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected);
/// IR convolution preflight with optional reject-reason output.

/// IR convolution preflight with mandatory reject-reason output.

/// IR preflight with optional reject-reason output — valid paths unchanged.

/// IR preflight with mandatory reject-reason output.

/// Classify reject reason from an existing IR preflight bundle.
/// IR preflight with optional reject-reason output (B7.2 deepen).

/// IR convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool tryPreflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
/// Map IR preflight flags to a reject reason (B7.2 deepen pass).
/// Diagnose why IR convolution would be skipped.

/// Returns true when \c classify_hrtf_ir_reject matches \p expected.
bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected);


/// IR preflight with optional reject-reason output (B7.2 deepen pass).

/// IR preflight with mandatory reject-reason output — returns \c can_convolve().

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
/// Empty-IR preflight — reports why convolution is unavailable without mutating state.
    bool null_samples = true;
    bool zero_length = true;
    bool empty_ir = true;

    [[nodiscard]] bool can_use_convolution() const { return has_samples; }
    [[nodiscard]] bool should_use_ild_itd_stub() const { return empty_ir; }
    [[nodiscard]] bool should_skip() const { return empty_ir; }

/// Preflight an HRTF IR stub without mutating state.
/// Read-only empty-IR diagnostics — no mutation (B7.2 deepen follow-up).
    bool null_samples = false;
    bool zero_length = false;
    bool empty_ir = false;

    [[nodiscard]] bool can_use_convolution() const { return !empty_ir; }
    [[nodiscard]] bool should_fallback_to_ild_itd() const { return empty_ir; }

/// Preflight HRTF IR stub validity without mutating state.
[[nodiscard]] HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);
/// Read-only empty-IR diagnostics — no mutation (B7.2 deepen — empty-IR guard).

    bool isEmpty() const { return null_samples || zero_length; }
    bool canUseConvolution() const { return !isEmpty(); }


/// Non-mutating empty-IR predicate — same guards as \c preflight_hrtf_ir (B7.2 deepen).
bool can_use_hrtf_ir_for_convolution(const HrtfIrStub& ir);

/// Empty-IR guard preflight — diagnoses why convolution may be skipped.
    bool canConvolution = false;

    bool can_convolute() const { return canConvolution; }

/// Diagnose empty/malformed IR before convolution path selection.

/// Read-only empty-IR diagnostics (B7.2 deepen).
    bool hasValidIr = false;

    bool canUseConvolution() const { return hasValidIr; }
    bool shouldSkipConvolution() const { return !hasValidIr; }

/// Populate empty-IR preflight without mutating the stub.
    bool malformed_ir = false;

    bool empty_ir() const { return null_samples || zero_length; }
    bool can_convolve() const { return !empty_ir() && !malformed_ir; }

/// Preflight empty/null/zero-length IR without mutating the stub (B7.2 deepen follow-up).
/// Why HRTF IR convolution preflight rejected the request (B7.2 deepen).
    EmptyIr,

/// Human-readable label for IR reject reasons (logging / tests).
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);

/// Diagnose why IR convolution preflight would reject; vacuously succeeds on valid IR.
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& outReason);

/// Preflight guard before HRTF IR convolution; false on empty/null/zero-length stub.
bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
/// Preflight guard — true when IR stub is ready for convolution (non-empty samples).
bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir);

/// Diagnose why IR convolution preflight rejected; vacuously succeeds on valid IR.
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir,
                                       HrtfIrPreflightRejectReason* reason = nullptr);




/// Classify why an IR stub is empty or malformed (B7.2 deepen).
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir);

/// Read-only empty-IR diagnostics — no mutation (B7.2 deepen).
    bool malformed = false;

    bool can_convolve() const { return !empty_ir && !malformed; }

/// Populate empty-IR preflight without running convolution (B7.2 deepen).

/// True when IR stub passes convolution preflight (B7.2 deepen).
bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason);
/// Reject reason for empty-HRTF IR preflight.
enum class HrtfIrPreflightReject : u8 {

/// Empty-HRTF IR preflight — read-only guard diagnostics before convolution dispatch.

    bool ready = false;
    bool skipConvolution = true;
    HrtfIrPreflightReject reject = HrtfIrPreflightReject::None;

    bool can_convolve() const { return ready && !skipConvolution; }

/// Populate empty-HRTF IR preflight without mutating the stub.

/// Returns true when IR preflight reports a convolution-ready stub.
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflightReject* reject = nullptr);
/// Why HRTF IR convolution would be skipped (B7.2 deepen).
enum class HrtfIrSkipReason : u8 {
    NullSamples = 1,
    ZeroLength = 2,

/// Preflight diagnostics for HRTF IR convolution dispatch.


    bool skipped = false;
    HrtfIrSkipReason reason = HrtfIrSkipReason::None;

    bool can_convolve() const { return !skipped; }
};

/// Classify why an IR stub cannot run convolution.
HrtfIrSkipReason classify_hrtf_ir_skip(const HrtfIrStub& ir);

/// True when an IR skip reason blocks convolution.
bool hrtf_ir_skip_reason_is_blocking(HrtfIrSkipReason reason);

/// Preflight IR stub — returns false when convolution should be skipped.
bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrSkipReason* reason = nullptr);

/// One-shot IR preflight with skip diagnostics.
HrtfIrPreflight preflight_hrtf_ir_stub(const HrtfIrStub& ir);
/// Why HRTF IR convolution is blocked (B7.2 deepen).
    Empty,
    Malformed,

/// Human-readable label for HRTF IR reject reasons (B7.2 deepen).

/// Classify why an HRTF IR stub cannot drive convolution (B7.2 deepen).

/// Read-only IR convolution diagnostics — no mutation (B7.2 deepen).
    bool emptyIr = true;
    HrtfIrRejectReason rejectReason = HrtfIrRejectReason::Empty;

    bool can_use_convolution() const { return rejectReason == HrtfIrRejectReason::None; }

/// Non-mutating IR convolution preflight (B7.2 deepen).

/// True when IR stub can drive convolution; optional reject reason on failure (B7.2 deepen).
/// Why an HRTF IR stub cannot select the convolution path (B7.2 deepen).

    bool empty = false;

    bool can_convolve() const { return reason == HrtfIrRejectReason::None; }
    bool skips_convolution() const { return !can_convolve(); }

/// Preflight an HRTF IR stub without mutation.

/// True when \p ir fails preflight for \p expected.
    /// Non-null samples with zero length — malformed stub.
    bool malformed_stub = false;
    /// True when IR samples are non-null and length > 0.
    bool has_valid_ir = false;

    [[nodiscard]] bool can_convolve() const { return has_valid_ir; }
    [[nodiscard]] bool should_skip_convolution() const { return !can_convolve(); }

/// Preflight empty-IR guard without mutating the stub (B7.2 deepen).

/// Convenience guard — `preflight_hrtf_ir(ir).can_convolve()` (B7.2 deepen).
bool can_convolve_hrtf_ir(const HrtfIrStub& ir);
struct EmptyHrtfIrPreflight {
    bool nonnullZeroLength = false;

    bool hasIr() const { return !nullSamples && !zeroLength && !nonnullZeroLength; }
    bool shouldSkipConvolution() const { return !hasIr(); }

/// Non-mutating empty-IR preflight — same guards as `has_hrtf_ir` / `should_skip_hrtf_convolution`.
EmptyHrtfIrPreflight preflightEmptyHrtfIr(const HrtfIrStub& ir);

/// Non-mutating predicate — mirrors `preflightEmptyHrtfIr(...).hasIr()`.
bool canUseHrtfIr(const HrtfIrStub& ir);
/// Read-only empty-IR diagnostics — no mutation (B7.2 deepen — IR preflight).

    bool can_convolve() const { return !null_samples && !zero_length; }
    bool is_empty() const { return !can_convolve(); }


/// Non-mutating predicate — same guard as \c preflight_hrtf_ir().can_convolve().
    /// True when samples are non-null but length is zero.

    [[nodiscard]] bool can_convolve() const { return !empty_ir; }


/// Convenience guard — `preflight_hrtf_ir(ir).can_convolve()`.
struct HrtfIrPreflight {
    HrtfIrRejectReason reason = HrtfIrRejectReason::None;


HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir);



/// Early-out when IR convolution should be skipped (B7.2 deepen follow-up).
bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir);
/// True when \c classify_hrtf_ir_reject matches \p expected (B7.2 deepen).
bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected);

/// IR preflight with mandatory reject-reason output (B7.2 deepen reject-reason pass).
HrtfIrPreflight try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

/// Empty-IR preflight with mandatory reject-reason output (B7.2 deepen reject-reason pass).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);

/// Classify the dominant empty-IR reject reason from a preflight bundle.
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// True when an IR reject reason blocks convolution dispatch.
bool hrtf_ir_reject_reason_blocks_convolution(HrtfIrRejectReason reason);

/// IR convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);

/// IR convolution preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

/// Why IR convolution preflight rejected the stub (B7.2 deepen).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    MalformedIr,
    NullSamples,
    EmptyIr,
};

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// IR convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

/// Why empty-IR preflight rejected convolution (B7.2 deepen).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    NullSamples,
    ZeroLength,
    MalformedIr,
};

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);

/// Derive convolution reject reason from empty-IR preflight diagnostics.
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// IR convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_convolution_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool preflight_hrtf_ir_convolution_ready(const HrtfIrPreflight& preflight,
                                         HrtfIrRejectReason* reason = nullptr);
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
bool try_preflight_hrtf_ir_convolution(const HrtfIrPreflight& preflight, HrtfIrRejectReason& reason);
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir);

/// Why IR preflight rejected convolution dispatch (B7.2 deepen pass).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    NullSamples,
    ZeroLength,
    MalformedIr,
    EmptyIr,
};

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// IR preflight with optional reject-reason output (B7.2 deepen pass).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

/// Classify the first IR reject reason from a preflight bundle.
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// True when \c classify_hrtf_ir_reject matches \p expected.
bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected);

/// IR preflight with optional reject-reason output (B7.2 deepen pass).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);

/// IR preflight that always writes the reject reason (B7.2 deepen pass).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

/// Why empty-IR preflight rejected convolution dispatch (B7.2 deepen).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    NullSamples,
    ZeroLength,
    MalformedIr,
};

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// IR convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);
bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir);

/// Why HRTF pan routing is rejected to centre bypass (B7.2 deepen pass).
enum class HrtfPanPathRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
};

/// Human-readable label for pan-path reject reasons (logging / tests).
const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason);

/// Returns the pan-path reject reason, or `None` when spatial pan may proceed.
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when `hrtf_pan_path_reject_reason` matches `expected` (B7.2 deepen pass).
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected);

/// Why HRTF IR preflight rejects convolution (B7.2 deepen).
enum class HrtfIrRejectReason : u8 {
    None = 0,
    NullSamples,
    ZeroLength,
    MalformedIr,
};

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason);
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight);

/// IR preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason = nullptr);
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason);

/// HRTF pan routing — empty IR uses ILD/ITD stub; convolution deferred until IR wired.
enum class HrtfPanPath {
    Bypass,
    IldItdStub,
    Convolution,
};

/// Why pan-path resolution selected bypass (B7.2 HRTF deepen preflight).
enum class HrtfPanRejectReason : u8 {
    None = 0,
/// Pan-path skip reason for spatial preflight (B7.2 deepen).
enum class HrtfPanPathSkipReason {
    None,
    Disabled,
/// Why HRTF pan routing would bypass spatial pan (B7.2 deepen).
enum class HrtfPanPathRejectReason : u8 {
    HrtfDisabled,
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
/// Read-only pan-path diagnostics — no mutation (B7.2 deepen).
    bool empty_ir = false;
    HrtfIrPreflight ir_preflight{};

    [[nodiscard]] bool will_bypass() const { return path == HrtfPanPath::Bypass; }
    [[nodiscard]] bool will_use_ild_itd_stub() const { return path == HrtfPanPath::IldItdStub; }
    [[nodiscard]] bool will_use_convolution() const { return path == HrtfPanPath::Convolution; }
    [[nodiscard]] bool can_apply_spatial_pan() const { return !will_bypass(); }

/// Preflight pan-path guard checks from HRTF enable flag, IR stub, and listener-local offset.

/// Preflight pan-path guard checks when no IR is wired.
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled,

[[nodiscard]] bool can_apply_hrtf_spatial_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
/// Const preflight for HRTF pan routing (B7.2 deepen).
    HrtfPanPathSkipReason skip_reason = HrtfPanPathSkipReason::None;
    bool skipped = false;

    bool can_spatialize() const { return !skipped; }

/// Populate pan-path preflight without computing binaural gains (B7.2 deepen).
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,

/// Populate pan-path preflight when no IR is wired (B7.2 deepen).
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// True when a resolved pan path selects ILD/ITD stub fallback (empty IR).
bool should_fallback_to_ild_itd_pan_path(HrtfPanPath path);
/// Why HRTF pan-path preflight bypasses or falls back from convolution (B7.2 deepen).
    EmptyIr,

/// Human-readable label for pan-path reject reasons (B7.2 deepen).
const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason);

/// Classify why a resolved pan path bypasses or skips convolution (B7.2 deepen).
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const HrtfIrStub& ir,

/// Classify pan-path reject when no IR is wired (B7.2 deepen).
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener);

/// True when `classify_hrtf_pan_path_reject` matches `expected` (B7.2 deepen).
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, HrtfPanPathRejectReason expected);
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);

/// Diagnose why pan routing would bypass; vacuously succeeds when spatial pan may proceed.

/// Why HRTF pan routing bypasses or falls back (B7.2 deepen).
    CoLocatedSource,

/// Human-readable label for HRTF pan-path reject reasons (B7.2 deepen).
const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason);

/// Diagnose pan-path bypass or ILD/ITD fallback; `None` when convolution may proceed.
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,

/// Diagnose pan-path bypass or ILD/ITD fallback when no IR is wired.
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when `hrtf_pan_path_reject_reason` matches `expected` (B7.2 deepen).

/// Returns true when the no-IR overload `hrtf_pan_path_reject_reason` matches `expected`.
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected);

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
/// Why a resolved pan path bypasses spatial HRTF panning (B7.2 deepen follow-up).
enum class HrtfPanSkipReason : u8 {
/// Why HRTF pan-path preflight bypassed spatial pan (B7.2 deepen follow-up).
    None = 0,
/// Why pan-path preflight rejected spatial pan (B7.2 deepen).
    None,
/// Why HRTF pan routing would bypass spatial pan (B7.2 deepen follow-up).
/// Diagnostic reason HRTF pan routing bypasses spatial processing (B7.2 deepen follow-up).
/// Why HRTF pan routing would bypass spatial pan (B7.2 deepen reject-reason pass).
/// Why HRTF spatial pan is bypassed (B7.2 deepen).
/// Why HRTF pan routing would bypass spatial pan (B7.2 deepen).
/// Why HRTF pan routing would bypass spatial pan (B7.2 deepen pass).
/// Why pan-path preflight rejected spatial pan (B7.2 deepen follow-up).
/// Diagnostic reason spatial HRTF pan is bypassed (B7.2 deepen).
/// Diagnostic reason spatial pan is bypassed (B7.2 deepen).
/// Why HRTF pan routing is rejected (B7.2 deepen).
/// Why HRTF pan-path preflight rejected spatial pan (B7.2 deepen).
/// Why pan-path preflight rejected spatial pan routing (B7.2 deepen).
/// Why HRTF pan routing bypasses spatial pan (B7.2 deepen reject-reason pass).
/// Why HRTF pan-path resolution rejected spatial pan (B7.2 deepen).
/// Diagnostic reason HRTF pan routing bypasses spatial processing (B7.2 deepen).
/// Why HRTF pan-path preflight rejected spatial pan (B7.2 deepen pass).
/// Why spatial HRTF pan was rejected (B7.2 deepen pass).
    HrtfDisabled,
    CoLocated,
};

/// Human-readable label for pan-path reject reasons (logging / tests).
const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason);
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);

/// Diagnose why pan routing would bypass; vacuously succeeds on spatial paths.
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when \c hrtf_pan_path_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected);
/// Human-readable label for pan-path reject reasons (B7.2 deepen follow-up).

/// Classify why HRTF pan routing bypasses spatial pan (B7.2 deepen follow-up).
/// Human-readable label for pan-path reject reasons (B7.2 deepen).

/// Classify why HRTF pan routing would bypass spatial pan (B7.2 deepen).
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener);
/// Returns true when \c hrtf_pan_path_reject_reason matches \p expected (B7.2 deepen follow-up).

/// Human-readable label for diagnostics and test assertions (B7.2 deepen follow-up).

/// Returns the first reject reason for pan routing, or \c None when spatial pan may proceed.



/// Returns true when \c hrtf_pan_path_reject_reason matches \p expected.

/// Classify why HRTF pan routing bypasses spatial pan (B7.2 deepen).
const char* hrtfPanPathRejectReasonName(HrtfPanPathRejectReason reason);

HrtfPanPathRejectReason hrtfPanPathRejectReason(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when \c hrtfPanPathRejectReason matches \p expected (B7.2 deepen).
bool hrtfPanPathRejectsForReason(bool hrtf_enabled, const Vec3& rel_listener,

/// Diagnose why spatial pan would bypass; vacuously succeeds on routable sources.

/// Returns true when `hrtf_pan_path_reject_reason` matches `expected` (B7.2 deepen follow-up).

/// Diagnose why pan routing would bypass; empty IR still selects ILD/ITD stub.

/// Returns true when `hrtf_pan_path_reject_reason` matches `expected` (B7.2 deepen follow-up pass).

/// Returns the first pan-path reject reason, or \c None when spatial pan may proceed.



/// Returns true when \c hrtf_pan_path_reject_reason matches \p expected (B7.2 deepen).

/// Human-readable label for diagnostics and test assertions (B7.2 deepen).



/// First reject reason for pan routing, or \c None when spatial pan may proceed.

/// Human-readable label for pan-path reject reasons (B7.2 deepen follow-up pass).

/// Diagnose why pan routing would bypass; vacuously succeeds when spatial pan may proceed.

/// Why HRTF pan-path preflight rejected spatial pan or convolution (B7.2 deepen).
    EmptyIr,


/// True when a pan-path reject reason blocks spatial panning.
bool hrtf_pan_path_reject_reason_is_blocking_spatial(HrtfPanPathRejectReason reason);

/// True when a pan-path reject reason blocks IR convolution routing.
bool hrtf_pan_path_reject_reason_is_blocking_convolution(HrtfPanPathRejectReason reason);

/// Classify why pan routing would bypass spatial pan — same ordering as \c preflight_hrtf_pan_path.
HrtfPanPathRejectReason classify_hrtf_pan_path_spatial_reject(bool hrtf_enabled,
                                                              const Vec3& rel_listener);

/// Classify why pan routing would reject convolution — same ordering as \c preflight_hrtf_pan_path.
HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(bool hrtf_enabled,
                                                                  const HrtfIrStub& ir,

/// Non-mutating spatial-pan preflight — returns true when spatial pan may proceed.
bool preflight_hrtf_spatial_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfPanPathRejectReason* reason = nullptr);

/// Non-mutating convolution-routing preflight — returns true when convolution path may proceed.
bool preflight_hrtf_convolution_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener,

/// Why pan-path preflight rejects spatial pan or convolution (B7.2 deepen).
    MalformedIr,


/// Classify why spatial pan routing would be skipped (B7.2 deepen).
HrtfPanPathRejectReason classify_hrtf_pan_path_skip_reject(bool hrtf_enabled, const Vec3& rel_listener);

/// Classify why pan-path convolution would be rejected (B7.2 deepen).
/// Why pan-path preflight rejected spatial/convolution routing (B7.2 deepen reject-reason pass).


/// Returns the first pan-path reject reason, or `None` when spatial pan may proceed.
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,

/// True when a pan-path reject reason blocks spatial pan routing.
bool is_blocking_hrtf_pan_path_reject_reason(HrtfPanPathRejectReason reason);

/// Returns true when `hrtf_pan_path_reject_reason` matches `expected` (B7.2 deepen reject-reason pass).
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, HrtfPanPathRejectReason expected);

/// True when a pan-path reject reason blocks spatial pan dispatch.
bool hrtf_pan_path_reject_reason_is_blocking(HrtfPanPathRejectReason reason);

/// Classify why HRTF pan routing would bypass — same ordering as \c preflight_hrtf_pan_path.

/// True when a pan-path reject reason would block spatial pan (B7.2 deepen).

/// Classify why spatial pan would be rejected — same ordering as \c preflight_hrtf_pan_path.

/// Classify why pan routing would bypass; \c None when spatial pan may proceed.

/// True when \c classify_hrtf_pan_path_reject matches \p expected.

/// True when a pan-path reject reason blocks spatial pan.

/// Classify why spatial pan would be rejected.

const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason);

bool hrtfPanPathRejectReasonIsBlocking(HrtfPanPathRejectReason reason);

/// Classify why spatial pan would bypass — same ordering as `tryPreflightHrtfPanPath`.
HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const Vec3& rel_listener);

/// Pan-path preflight with mandatory reject-reason output (B7.2 deepen).
bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathRejectReason& reason);

/// Pan-path preflight with mandatory reject-reason output when no IR is wired (B7.2 deepen).
bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
/// Why HRTF pan-path routing preflight rejected or downgraded the request (B7.2 deepen).

/// Human-readable label for HRTF pan-path reject reasons (logging / tests).

/// True when a pan-path reject reason blocks spatial panning (B7.2 deepen).

/// Classify why HRTF pan routing would bypass or downgrade (B7.2 deepen).
HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const HrtfIrStub& ir,

/// Returns the first pan-path bypass reason, or \c None when spatial pan may proceed.


/// Diagnostic reason HRTF convolution cannot run for a resolved pan path (B7.2 deepen).
enum class HrtfConvolutionRejectReason : u8 {
    NullSamples,

const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason);

/// Returns the first convolution reject reason, or \c None when IR convolution may proceed.
HrtfConvolutionRejectReason hrtf_convolution_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,

/// Returns true when \c hrtf_convolution_reject_reason matches \p expected (B7.2 deepen).
bool hrtf_convolution_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                         HrtfConvolutionRejectReason expected);


/// Human-readable label for pan-path reject diagnostics (B7.2 deepen pass).

/// Diagnose why pan routing bypasses spatial pan; vacuously succeeds on spatial paths.

/// True when \c hrtf_pan_path_reject_reason matches \p expected.
/// Human-readable label for pan-path reject reasons (B7.2 deepen pass).

/// Diagnose why spatial pan would bypass; vacuously succeeds when pan may proceed.

/// IR-aware alias — pan-path reject does not depend on IR wiring.

/// Returns true when `hrtf_pan_path_reject_reason` matches `expected` (B7.2 deepen pass).
/// Why HRTF convolution on the resolved pan path was rejected (B7.2 deepen pass).
enum class HrtfPanPathConvolutionRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
};

/// Human-readable labels for pan-path reject reasons (logging / tests).
const char* hrtf_pan_path_convolution_reject_reason_label(HrtfPanPathConvolutionRejectReason reason);

/// Pan-path preflight diagnostics — read-only guard bundle (B7.2 deepen).
struct HrtfPanPathPreflight {
    HrtfPanPathRejectReason reason = HrtfPanPathRejectReason::None;
/// True when enabled, non-co-located source with empty IR selects ILD/ITD stub.
bool should_fallback_hrtf_to_ild_itd_stub(bool hrtf_enabled, const HrtfIrStub& ir,

/// True when enabled, non-co-located source with valid IR selects convolution.
bool should_use_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir,

/// Pan-path guard preflight — resolves path and records early-out reasons.
    bool rejected = false;

/// Classify why spatial pan routing would be rejected (B7.2 deepen).

/// True when a pan-path reject reason blocks spatial pan (B7.2 deepen).

    HrtfConvolutionRejectReason convolutionReason = HrtfConvolutionRejectReason::None;
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfPanPathRejectReason reason = HrtfPanPathRejectReason::None;
    HrtfPanPathConvolutionRejectReason convolutionReason = HrtfPanPathConvolutionRejectReason::None;
    bool hrtfDisabled = false;
    bool coLocated = false;
    bool emptyIr = false;
    bool skipped = false;
    HrtfPanPathRejectReason reason = HrtfPanPathRejectReason::None;
    HrtfPanPathRejectReason rejectReason = HrtfPanPathRejectReason::None;
    HrtfIrRejectReason convolveRejectReason = HrtfIrRejectReason::None;
    HrtfPanPathRejectReason spatialRejectReason = HrtfPanPathRejectReason::None;
    HrtfPanPathRejectReason skipReason = HrtfPanPathRejectReason::None;
    HrtfPanPathRejectReason convolutionRejectReason = HrtfPanPathRejectReason::None;
    HrtfPanPathRejectReason reject = HrtfPanPathRejectReason::None;

    bool can_spatial_pan() const { return reason == HrtfPanPathRejectReason::None; }
    bool can_spatial_pan() const { return !rejected && !skipped && path != HrtfPanPath::Bypass; }
    bool can_convolve() const { return can_spatial_pan() && path == HrtfPanPath::Convolution; }
    bool ok() const { return reason == HrtfPanPathRejectReason::None; }

    /// True when pan routing resolves to centre bypass (disabled or co-located).
    bool should_skip() const { return reason != HrtfPanPathRejectReason::None; }

    /// True when the resolved path selects ILD/ITD stub (empty IR fallback).
    bool uses_ild_itd_stub() const { return path == HrtfPanPath::IldItdStub; }
    bool ok() const { return can_spatial_pan(); }

    /// True when pan-path preflight reports no routing reject reason.
    bool ok() const { return reason == HrtfPanPathRejectReason::None; }
};

const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason);
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);

/// Diagnose why spatial pan would be skipped.

/// Diagnose why convolution on the resolved pan path would be skipped.
HrtfPanPathConvolutionRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir_preflight);

/// Returns true when \c classify_hrtf_pan_path_reject matches \p expected.
bool hrtf_pan_path_rejects_for_reason(const HrtfPanPathPreflight& preflight,
                                      HrtfPanPathRejectReason expected);

/// Returns true when \c classify_hrtf_pan_path_convolution_reject matches \p expected.
bool hrtf_pan_path_convolution_rejects_for_reason(
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir_preflight,
    HrtfPanPathConvolutionRejectReason expected);

/// Preflight HRTF pan routing from enable flag, IR stub, and listener-local offset.
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
/// Read-only pan-path diagnostics — no mutation (B7.2 deepen follow-up).
    bool hrtf_disabled = false;
    bool co_located = false;
    bool empty_ir = false;
    HrtfPanSkipReason skip_reason = HrtfPanSkipReason::None;

    bool should_skip_pan() const { return path == HrtfPanPath::Bypass; }
    bool should_skip_spatial_pan() const { return should_skip_pan(); }
    bool should_skip_attenuation_coupling() const { return should_skip_pan(); }
    bool can_apply_spatial_pan() const { return !should_skip_spatial_pan(); }
    bool uses_convolution() const { return path == HrtfPanPath::Convolution; }

/// Preflight pan routing from HRTF enable flag, IR stub, and listener-local offset.

/// Pan-path preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                            HrtfPanPathRejectReason* reason);

/// Pan-path preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason);

/// Map pan-path preflight flags to a reject reason (B7.2 deepen pass).
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);

/// Preflight pan routing when no IR is wired (ILD/ITD stub or bypass).
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Pan-path preflight with mandatory reject-reason output (B7.2 deepen follow-up).
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathPreflight& preflight);
/// Preflight pan routing with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfPanPathRejectReason* reason);

/// Preflight pan routing when no IR is wired with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,

/// Pan-path preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                  const Vec3& rel_listener, HrtfPanPathRejectReason& reason);

/// Pan-path preflight without IR with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                  HrtfPanPathRejectReason& reason);
                                 HrtfPanPathPreflight& preflight, HrtfPanPathRejectReason& reason);
/// Returns true when pan-path preflight passes (\c reason is \c None); writes reject reason on failure.
                                 HrtfPanPathPreflight& out, HrtfPanPathRejectReason& reason);

/// True when pan-path preflight skips for `expected` (B7.2 deepen).
bool hrtf_pan_path_preflight_skips_for_reason(const HrtfPanPathPreflight& preflight,
                                              HrtfPanPathRejectReason expected);

/// True when pan-path convolution preflight rejects for `expected` (B7.2 deepen).
bool hrtf_pan_path_preflight_convolution_rejects_for_reason(const HrtfPanPathPreflight& preflight,
/// Pan-path preflight with optional reject-reason output.

/// Pan-path preflight with mandatory reject-reason output.

/// Pan-path preflight with mandatory reject-reason output when no IR is wired (B7.2 deepen).

/// Pan-path preflight with optional reject-reason output — valid paths unchanged.


/// Classify reject reason from an existing pan-path preflight bundle.
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);
/// Pan-path preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason = nullptr);
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
/// Returns true when resolved pan path matches preflight path (B7.2 deepen pass).
bool hrtf_pan_path_matches_preflight(HrtfPanPath path, const HrtfPanPathPreflight& preflight);
/// Pan-path preflight with optional reject-reason output (B7.2 deepen pass).

/// Pan-path preflight with mandatory reject-reason output — returns \c can_spatial_pan().

/// Non-mutating spatial-pan predicate — mirrors \c HrtfPanPathPreflight::can_spatial_pan.
bool can_apply_spatial_hrtf_pan(const HrtfPanPathPreflight& preflight);

/// Non-mutating convolution predicate — mirrors \c HrtfPanPathPreflight::can_convolve.
bool can_convolve_hrtf_pan_path(const HrtfPanPathPreflight& preflight);

/// Non-mutating skip predicate — mirrors \c HrtfPanPathPreflight::should_skip.
bool should_skip_hrtf_pan_path_preflight(const HrtfPanPathPreflight& preflight);

/// True when pan-path preflight selects ILD/ITD stub (empty IR fallback).
bool uses_ild_itd_stub_hrtf_pan_path(const HrtfPanPathPreflight& preflight);


    bool can_spatial_pan() const;
    bool should_skip_pan() const;

/// Diagnose pan-path routing before spatial pan or coupling.

/// Diagnose pan-path routing when no IR is wired.
/// Reject reason for HRTF pan-path preflight.
enum class HrtfPanPathPreflightReject : u8 {
    Disabled,

/// HRTF pan-path preflight — read-only routing diagnostics before spatial pan dispatch.

    bool skipSpatialPan = true;
    bool skipConvolution = true;
    HrtfPanPathPreflightReject reject = HrtfPanPathPreflightReject::None;

    bool is_spatial() const { return !skipSpatialPan; }

/// Populate pan-path preflight from HRTF enable flag, IR stub, and listener-local offset.

/// Populate pan-path preflight when no IR is wired.
};

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener);

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when pan-path preflight selects a spatial path (ILD/ITD or convolution).
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathPreflightReject* reject = nullptr);
/// Why HRTF pan resolves to centre bypass (B7.2 deepen).
    Disabled = 1,
    CoLocated = 2,

/// Read-only pan-path diagnostics — no mutation (B7.2 deepen).
    HrtfPanPathRejectReason reject_reason = HrtfPanPathRejectReason::None;

    bool bypassed() const { return path == HrtfPanPath::Bypass; }
    bool can_spatial_pan() const { return path != HrtfPanPath::Bypass; }

/// Preflight pan-path resolution without IR wiring.

/// IR-aware pan-path preflight.

/// True when pan-path preflight rejects for \p expected.

    [[nodiscard]] bool can_apply_spatial_pan() const { return path != HrtfPanPath::Bypass; }
    [[nodiscard]] bool should_skip_spatial_pan() const { return !can_apply_spatial_pan(); }
    [[nodiscard]] bool uses_convolution() const { return path == HrtfPanPath::Convolution; }
    [[nodiscard]] bool uses_ild_itd_stub() const { return path == HrtfPanPath::IldItdStub; }

/// Preflight pan-path resolution without computing gains (B7.2 deepen).

/// Convenience guard — `preflight_hrtf_pan_path(...).can_apply_spatial_pan()` (B7.2 deepen).
bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);
bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener);
/// Read-only pan-path diagnostics — no mutation (B7.2 deepen — pan-path guard).

    bool canApplySpatialPan() const { return path != HrtfPanPath::Bypass; }
    bool shouldSkipSpatialPan() const { return path == HrtfPanPath::Bypass; }
    bool canUseConvolution() const { return path == HrtfPanPath::Convolution; }

/// Non-mutating pan-path preflight — same routing as `resolve_hrtf_pan_path`.
HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener);

/// Non-mutating predicate — mirrors `preflightHrtfPanPath(...).canApplySpatialPan()`.
bool canApplyHrtfPanPath(HrtfPanPath path);
/// Read-only pan-path diagnostics — no mutation (B7.2 deepen — pan-path preflight).
    bool has_valid_ir = false;

    bool can_spatialize() const { return path != HrtfPanPath::Bypass; }
    bool should_convolve() const { return path == HrtfPanPath::Convolution; }
    bool should_stub() const { return path == HrtfPanPath::IldItdStub; }
    bool is_bypassed() const { return path == HrtfPanPath::Bypass; }

/// Preflight pan-path resolution without computing gains.

/// Non-mutating predicate — same guard as \c preflight_hrtf_pan_path().can_spatialize().
bool can_spatialize_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);
/// Early-out when pan-path preflight would bypass spatial pan (B7.2 deepen follow-up).
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener);
/// True when \c classify_hrtf_pan_path_reject matches \p expected (B7.2 deepen).
bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected);
/// Pan-path preflight with mandatory reject-reason output (B7.2 deepen reject-reason pass).
HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                                 const Vec3& rel_listener,
                                                 HrtfPanPathRejectReason& reason);

/// Pan-path preflight with mandatory reject-reason output when no IR is wired.
HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,

/// Pan-path preflight when no IR is wired, with mandatory reject-reason output.
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);

/// Classify the dominant pan-path reject reason from a preflight bundle.
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);

/// True when a pan-path reject reason blocks spatial pan.
bool hrtf_pan_path_reject_reason_blocks_spatial_pan(HrtfPanPathRejectReason reason);

/// Pan-path spatial-pan preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason = nullptr);

/// Pan-path spatial-pan preflight when no IR is wired (B7.2 deepen).
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,

/// Pan-path spatial-pan preflight with mandatory reject-reason output (B7.2 deepen).

/// Why pan-path preflight rejected spatial pan (B7.2 deepen).
/// Why pan-path preflight rejected spatial pan (B7.2 deepen pass).
/// Why pan-path preflight rejected spatial pan or convolution (B7.2 deepen).
/// Why HRTF pan-path preflight rejects spatial pan (B7.2 deepen).
enum class HrtfPanPathRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,


/// Pan-path preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason);


/// Derive spatial-pan reject reason from pan-path preflight diagnostics.

/// Pan-path spatial preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, HrtfPanPathRejectReason* reason = nullptr);
bool preflight_hrtf_pan_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
bool preflight_hrtf_pan_spatial_ready(const HrtfPanPathPreflight& preflight,
bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_pan_spatial(const HrtfPanPathPreflight& preflight,
bool should_skip_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener);


/// Pan-path preflight with optional reject-reason output (B7.2 deepen pass).
/// Classify the first pan-path reject reason from a preflight bundle.

/// True when \c classify_hrtf_pan_path_reject matches \p expected.
bool hrtf_pan_path_rejects_for_reason(const HrtfPanPathPreflight& preflight,


/// Pan-path preflight when no IR is wired, with optional reject-reason output (B7.2 deepen pass).

/// Pan-path preflight that always writes the reject reason (B7.2 deepen pass).

/// Pan-path preflight when no IR is wired that always writes the reject reason (B7.2 deepen pass).
    EmptyIr,
};

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight);
HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& preflight);

/// Spatial pan preflight with optional reject-reason output (B7.2 deepen).
                                   const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason = nullptr);
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason);
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener);
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener);

/// Convolution pan-path preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_pan_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_pan_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,

/// Pan-path preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,

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
/// Pan-path preflight — resolved routing without computing binaural gains.
    bool empty_ir = false;
    bool bypass = true;
    bool uses_convolution = false;
    bool uses_ild_itd_stub = false;

    [[nodiscard]] bool can_apply_spatial_pan() const { return !bypass; }
    [[nodiscard]] bool should_skip() const { return bypass; }

/// Preflight HRTF pan-path resolution without computing binaural gains.
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,

/// Preflight pan-path resolution when no IR is wired.
[[nodiscard]] HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled,
    bool bypass = false;
    bool ild_itd_stub = false;
    bool convolution = false;

    [[nodiscard]] bool will_use_convolution() const { return convolution; }

/// Preflight HRTF pan routing without mutating state.

/// Preflight HRTF pan routing when no IR is wired.
/// Read-only pan-path resolution diagnostics (B7.2 deepen follow-up).
    bool has_valid_ir = false;

    [[nodiscard]] bool should_bypass() const { return path == HrtfPanPath::Bypass; }
    [[nodiscard]] bool should_use_ild_itd_stub() const { return path == HrtfPanPath::IldItdStub; }
    [[nodiscard]] bool should_use_convolution() const { return path == HrtfPanPath::Convolution; }

/// Preflight pan-path resolution without computing gains (B7.2 deepen follow-up).

/// Preflight pan-path resolution when no IR is wired (B7.2 deepen follow-up).
/// Preflight pan-path resolution with empty-IR and co-located guards applied.
HrtfPanPath preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);

HrtfPanPath preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// True when the resolved path selects convolution (enabled, non-empty IR, not co-located).
bool should_use_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);

/// Inverse of \c should_skip_hrtf_spatial_pan — path produces a lateral spatial image.
bool should_apply_hrtf_spatial_pan(HrtfPanPath path);
/// Read-only pan-path diagnostics — no mutation (B7.2 deepen).

    bool can_apply_spatial_pan() const { return path != HrtfPanPath::Bypass; }
    bool uses_convolution() const { return path == HrtfPanPath::Convolution; }
    bool is_bypass() const { return path == HrtfPanPath::Bypass; }

/// Preflight pan-path routing — empty IR, co-located, and disabled guards.

/// Preflight pan-path routing when no IR is wired.

/// True when the resolved path produces a lateral spatial image (not centre bypass).
bool is_spatial_hrtf_pan_path(HrtfPanPath path);

/// True when the pan path selects convolution (non-empty IR stub).
bool hrtf_pan_path_uses_convolution(HrtfPanPath path);

/// Readable alias for \c hrtf_pan_path_uses_convolution.
bool is_convolution_hrtf_pan_path(HrtfPanPath path);

/// True when the pan path selects ILD/ITD stub (empty IR fallback).
bool hrtf_pan_path_uses_ild_itd_stub(HrtfPanPath path);

/// Readable alias for \c hrtf_pan_path_uses_ild_itd_stub.
bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path);

/// Why HRTF pan routing bypasses spatial image production.
enum class HrtfPanPathSkipReason : u8 {
    None = 0,
    Disabled = 1,
    CoLocated = 2,
};

/// Preflight diagnostics for HRTF pan-path resolution.
struct HrtfPanPathPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfPanPathSkipReason skip_reason = HrtfPanPathSkipReason::None;
    bool skipped = false;

    bool can_spatial_pan() const { return !skipped && path != HrtfPanPath::Bypass; }
};

/// Classify why HRTF pan would bypass (disabled or co-located).
HrtfPanPathSkipReason classify_hrtf_pan_path_skip(bool hrtf_enabled, const Vec3& rel_listener);

/// True when a pan-path skip reason blocks spatial panning.
bool hrtf_pan_path_skip_reason_is_blocking(HrtfPanPathSkipReason reason);

/// Preflight pan-path resolution — returns false when bypassed.
bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathSkipReason* reason = nullptr);

/// One-shot pan-path preflight with resolved path and skip diagnostics.
HrtfPanPathPreflight preflight_hrtf_pan_path_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener);

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
/// Read-only pan-path diagnostics (B7.2 deepen).
    bool hrtfDisabled = false;
    bool coLocated = false;
    bool bypassed = false;
    bool spatial = false;
    bool usesConvolution = false;
    bool usesIldItdStub = false;

    bool canApplySpatialPan() const { return spatial; }
    bool shouldSkipSpatialPan() const { return bypassed; }

/// Populate pan-path preflight without computing binaural gains.
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,

/// Populate pan-path preflight when no IR is wired (ILD/ITD stub or bypass).
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);
/// Why HRTF spatial pan preflight rejected the request (B7.2 deepen).
/// Why HRTF pan path is bypassed (B7.2 deepen).
enum class HrtfPanPathRejectReason : u8 {
    None = 0,
    Disabled,
    CoLocated,
/// Why HRTF pan routing bypasses spatial processing (B7.2 deepen).
    HrtfDisabled,

/// Human-readable label for pan-path reject reasons (logging / tests).
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason);

/// Diagnose why spatial pan preflight would reject; resolves \p outPath when provided.
bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPath& outPath, HrtfPanPathRejectReason& outReason);

/// Preflight guard before spatial pan; false when disabled or co-located (bypass path).
bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                HrtfPanPath* out_path = nullptr,
                                HrtfPanPathRejectReason* reason = nullptr);

/// IR-aware spatial pan preflight; false when bypass, otherwise resolves pan path.
bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                const Vec3& rel_listener, HrtfPanPath* out_path = nullptr,
/// Classify why pan routing selects bypass (B7.2 deepen).
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener);

/// Read-only pan-path diagnostics — no mutation (B7.2 deepen).
    HrtfPanPathRejectReason reject_reason = HrtfPanPathRejectReason::None;

    bool can_spatial_pan() const { return !bypassed; }
    bool can_convolve() const { return uses_convolution; }

/// Populate pan-path preflight without computing gains (B7.2 deepen).

/// Populate pan-path preflight when no IR is wired (B7.2 deepen).
struct HrtfPanPathPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    bool bypassed = false;
    bool uses_convolution = false;
    bool uses_ild_itd_stub = false;

};

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener);

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// True when pan path passes spatial-pan preflight (B7.2 deepen).
bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfPanPathRejectReason* reason);

/// Human-readable label for HRTF pan-path reject reasons (B7.2 deepen).

/// Classify why HRTF pan is bypassed for a listener-local offset (B7.2 deepen).

/// Read-only pan-path routing diagnostics — no mutation (B7.2 deepen).
    HrtfPanPathRejectReason rejectReason = HrtfPanPathRejectReason::Disabled;
    bool bypassed = true;

    bool can_apply_spatial_pan() const { return spatial; }
    bool can_apply_attenuation_coupling() const { return spatial; }

/// Non-mutating pan-path preflight with IR stub (B7.2 deepen).

/// Non-mutating pan-path preflight when no IR is wired (B7.2 deepen).

/// True when pan path is spatial; optional reject reason on bypass (B7.2 deepen).
/// Read-only pan-path diagnostics — no mutation (B7.2 deepen follow-up).
    bool hrtf_disabled = false;
    bool empty_ir = true;

    [[nodiscard]] bool can_apply_spatial_pan() const { return !should_skip(); }
    [[nodiscard]] bool should_skip() const { return is_hrtf_pan_path_bypass(path); }
    [[nodiscard]] bool uses_convolution() const { return hrtf_pan_path_uses_convolution(path); }
    [[nodiscard]] bool uses_ild_itd_stub() const { return hrtf_pan_path_uses_ild_itd_stub(path); }

/// Preflight HRTF pan routing from enable flag, IR stub, and listener-local offset.

/// Preflight HRTF pan routing when no IR is wired.

/// Convenience guard — `preflight_hrtf_pan_path(...).can_apply_spatial_pan()`.
bool can_apply_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);

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

/// Read-only pan-path diagnostics — no mutation (B7.2 deepen — pan-path guard).
struct HrtfPanPathPreflight {
    bool hrtf_disabled = false;
    bool co_located = false;
    bool empty_ir = false;
    HrtfPanPath path = HrtfPanPath::Bypass;

    bool canApplyPan() const { return path != HrtfPanPath::Bypass; }
};

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener);
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Non-mutating pan-path predicate — same guards as \c preflight_hrtf_pan_path (B7.2 deepen).
bool can_apply_hrtf_spatial_pan(HrtfPanPath path);

/// Preflight guard — true when spatial HRTF pan may run (enabled, not co-located).
bool preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// Preflight guard on a resolved pan path — true when path is spatial (not bypass).
bool preflight_hrtf_pan_path(HrtfPanPath path);

/// Diagnose why pan-path preflight would bypass spatial processing.
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPreflightRejectReason* reason = nullptr);

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

/// Apply pan gains from a pan-path preflight bundle (read-only guards; valid paths unchanged).
BinauralPanGains compute_binaural_pan_gains_from_pan_path_preflight(
    const HrtfPanPathPreflight& preflight, const Vec3& rel_listener,
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
/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen).
    bool bypass_path = false;
    bool unity_attenuation = false;
    bool will_narrow = false;
    float clamped_distance_attenuation = 1.f;
    float clamped_occlusion_gain = 1.f;

    [[nodiscard]] bool should_apply_coupling() const { return !bypass_path; }
    [[nodiscard]] bool should_skip_coupling() const { return bypass_path || unity_attenuation; }
    [[nodiscard]] bool should_narrow_spatial_image() const { return will_narrow; }

/// Preflight attenuation-coupling guard checks for a resolved pan path.

/// Convenience guard — `preflight_hrtf_attenuation_coupling(...).should_narrow_spatial_image()`.
[[nodiscard]] bool should_narrow_hrtf_spatial_image_preflight(HrtfPanPath path,
/// Attenuation-coupling skip reason for spatial narrowing preflight (B7.2 deepen).
enum class HrtfAttenuationCouplingSkipReason {
    None,
    BypassPath,
    UnityAttenuation,

/// Const preflight for distance/occlusion spatial narrowing (B7.2 deepen).
    HrtfAttenuationCouplingSkipReason reason = HrtfAttenuationCouplingSkipReason::None;
    bool skipped = false;
    float spatial_blend = 1.f;

    bool can_apply() const { return !skipped; }

/// Populate attenuation-coupling preflight without mutating pan gains (B7.2 deepen).

    [[nodiscard]] bool can_apply_coupling() const { return !bypass_path && !unity_attenuation; }
    [[nodiscard]] bool should_skip_coupling() const { return !can_apply_coupling(); }

/// Preflight attenuation coupling without mutating pan gains (B7.2 deepen).
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {});

/// True when path-aware attenuation coupling can be skipped (B7.2 deepen).
bool should_skip_hrtf_attenuation_coupling_apply(HrtfPanPath path, float distance_attenuation,
                                                 float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling = {},
                                                 const BinauralPanParams& params = {});
/// Convenience guard — `preflight_hrtf_attenuation_coupling(...).can_apply_coupling()` (B7.2 deepen).
bool can_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
/// Why distance/occlusion coupling preflight skips spatial narrowing (B7.2 deepen).
enum class HrtfAttenuationCouplingRejectReason : u8 {
    None = 0,

/// Human-readable label for attenuation-coupling reject reasons (B7.2 deepen).
const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason);

/// Classify why attenuation coupling would be skipped (B7.2 deepen).
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(

/// True when `classify_hrtf_attenuation_coupling_reject` matches `expected` (B7.2 deepen).
bool hrtf_attenuation_coupling_rejects_for_reason(
    HrtfAttenuationCouplingRejectReason expected,
/// Why distance/occlusion coupling would skip spatial image narrowing (B7.2 deepen).

/// Human-readable label for attenuation-coupling reject reasons (logging / tests).
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);

/// Diagnose why attenuation coupling would skip; vacuously succeeds when narrowing may proceed.

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
/// Why distance/occlusion coupling skips spatial image narrowing (B7.2 deepen).

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason);

/// Diagnose why attenuation coupling is skipped; `None` when narrowing may proceed.
HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(

/// Returns true when `hrtf_attenuation_coupling_reject_reason` matches `expected` (B7.2 deepen).
                                                  HrtfAttenuationCouplingRejectReason expected);

/// Clamp distance or occlusion attenuation scalars into [0, 1].
float clamp_hrtf_attenuation(float attenuation);

/// Epsilon below which distance/occlusion attenuation is treated as fully audible.
float hrtf_unity_attenuation_epsilon();

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

/// Preflight attenuation coupling — true when spatial narrowing should run.
bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain);

/// Combined early-out — bypass path or unity attenuation skips coupling.
bool should_skip_hrtf_attenuation_coupling_for_inputs(HrtfPanPath path, float distance_attenuation,
                                                     float occlusion_gain);

/// True when distance attenuation is fully audible (no distance narrowing).
bool is_unity_hrtf_distance_attenuation(float distance_attenuation);

/// True when occlusion LF gain is fully audible.
bool is_unity_hrtf_occlusion_gain(float occlusion_gain);

/// Attenuation-coupling preflight — diagnoses no-op coupling cases.
struct HrtfAttenuationCouplingPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    bool bypassPath = false;
    bool unityAttenuation = false;
    bool unitySpatialBlend = false;
    float spatialBlend = 1.f;

    bool can_couple() const;
    bool should_skip_coupling() const;
};

/// Diagnose attenuation coupling before spatial image narrowing.
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {});

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
/// Why attenuation-coupling preflight rejected the request (B7.2 deepen).
enum class HrtfAttenuationCouplingRejectReason : u8 {
    None = 0,
    BypassPath,
/// Why attenuation coupling should not narrow the binaural image (B7.2 deepen).
    PanBypassed,
/// Why attenuation coupling would skip spatial image narrowing (B7.2 deepen follow-up).
/// Why attenuation coupling would skip spatial image narrowing (B7.2 deepen reject-reason pass).
    UnityAttenuation,
/// Why attenuation coupling would skip spatial image narrowing (B7.2 deepen).
/// Diagnostic reason attenuation coupling is skipped (B7.2 deepen).
/// Diagnostic reason distance/occlusion coupling skips spatial narrowing (B7.2 deepen).
};

/// Human-readable label for diagnostics and test assertions (B7.2 deepen).
const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason);

/// Returns the first coupling reject reason, or \c None when narrowing may proceed.
/// Why attenuation coupling narrows the spatial image (B7.2 deepen).

/// Human-readable label for attenuation-coupling reject reasons (logging / tests).

/// First reject reason for attenuation coupling, or \c None when narrowing may proceed.
/// Returns the first attenuation-coupling skip reason, or \c None when narrowing may proceed.
HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain);

/// Returns true when \c hrtf_attenuation_coupling_reject_reason matches \p expected (B7.2 deepen).
/// Why attenuation coupling would skip spatial image narrowing (B7.2 deepen follow-up pass).

/// Human-readable label for attenuation-coupling reject reasons (B7.2 deepen follow-up pass).

/// Diagnose why attenuation coupling would skip narrowing; vacuously succeeds when narrowing may proceed.

/// Returns true when `hrtf_attenuation_coupling_reject_reason` matches `expected` (B7.2 deepen follow-up pass).
bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected);
/// Why attenuation-coupling preflight rejected spatial image narrowing (B7.2 deepen).



const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);

/// True when an attenuation-coupling reject reason blocks spatial image narrowing.
bool hrtf_attenuation_coupling_reject_reason_is_blocking(HrtfAttenuationCouplingRejectReason reason);

/// Classify why attenuation coupling would be skipped — same ordering as \c preflight_hrtf_attenuation_coupling.
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(

/// Non-mutating attenuation-coupling preflight — returns true when narrowing may proceed.
bool preflight_hrtf_attenuation_coupling_narrowing(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason = nullptr);
/// Why attenuation-coupling preflight rejected spatial narrowing (B7.2 deepen).
    None,

/// Human-readable label for attenuation-coupling reject reasons (B7.2 deepen).

/// Classify why distance/occlusion coupling would skip narrowing (B7.2 deepen).


/// Diagnose why attenuation coupling would skip narrowing; vacuously succeeds when narrowing applies.

/// Returns true when \c hrtf_attenuation_coupling_reject_reason matches \p expected.
/// Why attenuation-coupling preflight rejects spatial image narrowing (B7.2 deepen).


/// Classify why attenuation coupling would be skipped (B7.2 deepen).
/// Why attenuation-coupling preflight skipped spatial image narrowing (B7.2 deepen reject-reason pass).


/// Returns the first attenuation-coupling reject reason, or `None` when narrowing may proceed.

/// True when an attenuation-coupling reject reason would block spatial image narrowing.
bool is_blocking_hrtf_attenuation_coupling_reject_reason(HrtfAttenuationCouplingRejectReason reason);

/// Returns true when `hrtf_attenuation_coupling_reject_reason` matches `expected`.
/// Classify why attenuation coupling would skip — same ordering as
/// \c preflight_hrtf_attenuation_coupling.


/// True when an attenuation-coupling reject reason would block narrowing (B7.2 deepen).

/// Classify why spatial narrowing would be rejected — same ordering as
/// Why distance/occlusion coupling would skip spatial image narrowing (B7.2 deepen reject-reason pass).


/// Classify why attenuation coupling would skip narrowing; \c None when narrowing may proceed.

/// True when \c classify_hrtf_attenuation_coupling_reject matches \p expected.

/// Classify why distance/occlusion coupling would be skipped.
/// Why attenuation-coupling preflight skipped narrowing (B7.2 deepen).


/// Classify why distance/occlusion coupling would be skipped (B7.2 deepen).

const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason);

bool hrtfAttenuationCouplingRejectReasonIsBlocking(HrtfAttenuationCouplingRejectReason reason);

/// Classify why distance/occlusion coupling would skip — same ordering as
/// `tryPreflightHrtfAttenuationCoupling`.
HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

/// Attenuation-coupling preflight with mandatory reject-reason output (B7.2 deepen).
bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                         HrtfAttenuationCouplingRejectReason& reason,
/// Why HRTF attenuation-coupling preflight skipped narrowing (B7.2 deepen).


/// True when an attenuation-coupling reject reason blocks spatial narrowing (B7.2 deepen).

/// Classify why attenuation coupling would skip spatial narrowing (B7.2 deepen).



/// Why attenuation-coupling preflight rejected spatial image narrowing (B7.2 deepen pass).

/// Human-readable label for attenuation-coupling reject diagnostics (B7.2 deepen pass).

/// Diagnose why distance/occlusion coupling would skip narrowing.

/// True when \c hrtf_attenuation_coupling_reject_reason matches \p expected.

/// Why attenuation coupling is rejected (B7.2 deepen pass).


/// Returns the attenuation-coupling reject reason, or `None` when narrowing may proceed.

/// Returns true when `hrtf_attenuation_coupling_reject_reason` matches `expected` (B7.2 deepen pass).
/// Why distance/occlusion coupling would skip spatial image narrowing (B7.2 deepen pass).

/// Human-readable label for attenuation-coupling reject reasons (B7.2 deepen pass).

/// Diagnose why attenuation coupling would skip; vacuously succeeds when narrowing may proceed.


/// Attenuation-coupling preflight diagnostics — read-only guard bundle (B7.2 deepen).
struct HrtfAttenuationCouplingPreflight {
    HrtfAttenuationCouplingRejectReason reason = HrtfAttenuationCouplingRejectReason::None;
    bool rejected = false;
/// Why attenuation coupling was rejected (B7.2 deepen pass).


    bool bypassPath = false;
    bool unityAttenuation = false;
    bool skipped = false;
    float spatialBlend = 1.f;
    HrtfAttenuationCouplingRejectReason rejectReason = HrtfAttenuationCouplingRejectReason::None;

    bool can_narrow() const { return reason == HrtfAttenuationCouplingRejectReason::None; }
    bool can_narrow() const { return !rejected && !skipped; }

    /// True when distance/occlusion coupling should be skipped (bypass or unity attenuation).
    bool should_skip() const { return reason != HrtfAttenuationCouplingRejectReason::None; }


    bool should_skip() const { return !can_narrow(); }


const char* hrtfAttenuationCouplingRejectReasonName(HrtfAttenuationCouplingRejectReason reason);

/// Diagnose why attenuation coupling would skip; vacuously succeeds when narrowing applies.
HrtfAttenuationCouplingRejectReason hrtfAttenuationCouplingRejectReason(

/// Returns true when \c hrtfAttenuationCouplingRejectReason matches \p expected (B7.2 deepen).
bool hrtfAttenuationCouplingRejectsForReason(HrtfPanPath path, float distance_attenuation,

/// Why distance/occlusion coupling would skip spatial image narrowing (B7.2 deepen follow-up pass).


/// Diagnose why attenuation coupling would skip narrowing.

/// Returns true when `hrtf_attenuation_coupling_reject_reason` matches `expected` (B7.2 deepen follow-up).
/// Why attenuation coupling would skip spatial image narrowing (B7.2 deepen pass).




/// Why attenuation-coupling preflight rejected spatial narrowing (B7.2 deepen follow-up).


/// Returns the first attenuation-coupling reject reason, or \c None when narrowing may proceed.
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

bool hrtf_attenuation_coupling_rejects_for_reason(
    HrtfAttenuationCouplingRejectReason expected,
/// Diagnostic reason attenuation coupling skips spatial image narrowing (B7.2 deepen).








/// Classify why distance/occlusion coupling would be skipped (B7.2 deepen).

/// True when an attenuation-coupling reject reason blocks narrowing (B7.2 deepen).

    bool bypassPath = false;
    bool unityAttenuation = false;
    bool skipped = false;
    float spatialBlend = 1.f;
    HrtfAttenuationCouplingRejectReason reason = HrtfAttenuationCouplingRejectReason::None;
    HrtfAttenuationCouplingRejectReason reject = HrtfAttenuationCouplingRejectReason::None;

    bool can_narrow() const { return !skipped; }
    bool ok() const { return reason == HrtfAttenuationCouplingRejectReason::None; }



/// Returns true when \c hrtf_attenuation_coupling_reject_reason matches \p expected (B7.2 deepen follow-up pass).

/// Diagnose why attenuation coupling would skip; vacuously succeeds on narrowable paths.

/// Diagnostic reason distance/occlusion coupling skips spatial narrowing (B7.2 deepen follow-up).

/// Human-readable label for diagnostics and test assertions (B7.2 deepen follow-up).

/// Returns the first reject reason for attenuation coupling, or \c None when narrowing may proceed.

/// Returns true when \c hrtf_attenuation_coupling_reject_reason matches \p expected (B7.2 deepen follow-up).


/// Why attenuation-coupling preflight skipped spatial narrowing (B7.2 deepen follow-up).

/// Human-readable label for attenuation-coupling reject reasons (B7.2 deepen follow-up).

/// Classify why attenuation coupling would skip spatial narrowing (B7.2 deepen follow-up).










/// Why attenuation coupling skips spatial image narrowing (B7.2 deepen).


/// Classify why attenuation coupling would skip narrowing (B7.2 deepen).


    bool can_narrow() const { return reason == HrtfAttenuationCouplingRejectReason::None; }

    /// True when distance/occlusion coupling should be skipped (bypass or unity attenuation).
    bool should_skip() const { return skipped; }
    bool ok() const { return can_narrow(); }

    bool should_skip() const { return reason != HrtfAttenuationCouplingRejectReason::None; }

    /// True when attenuation-coupling preflight reports no skip reason.

};

const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason);
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight);

/// Diagnose why distance/occlusion coupling would be skipped.

/// Returns true when \c classify_hrtf_attenuation_coupling_reject matches \p expected.
bool hrtf_attenuation_coupling_rejects_for_reason(const HrtfAttenuationCouplingPreflight& preflight,
                                                  HrtfAttenuationCouplingRejectReason expected);

/// Preflight distance + occlusion coupling before spatial image narrowing.
/// Read-only attenuation-coupling diagnostics (B7.2 deepen).
    HrtfPanPath path = HrtfPanPath::Bypass;
    float distanceAttenuation = 1.f;
    float occlusionGain = 1.f;

    bool canApplyCoupling() const { return !skipped; }
    bool shouldSkipCoupling() const { return skipped; }

/// Populate attenuation-coupling preflight without mutating pan gains.
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);

/// Classify why attenuation coupling is skipped (B7.2 deepen).
/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen).

    bool canApplyCoupling() const { return !bypassPath && !unityAttenuation; }
    bool shouldNarrowSpatialImage() const { return canApplyCoupling(); }

/// Non-mutating attenuation-coupling preflight — same guards as path-aware coupling.
HrtfAttenuationCouplingPreflight preflightHrtfAttenuationCoupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

    HrtfAttenuationCouplingRejectReason reject_reason = HrtfAttenuationCouplingRejectReason::None;
    bool bypass_path = false;
    bool unity_attenuation = false;
    float spatial_blend = 1.f;

    bool can_apply_coupling() const { return !skipped; }

/// Populate attenuation-coupling preflight without mutating gains (B7.2 deepen).
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
/// Attenuation-coupling preflight with mandatory reject-reason output (B7.2 deepen follow-up).
bool try_preflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingPreflight& preflight,
/// Preflight attenuation coupling with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,

/// Attenuation-coupling preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              HrtfAttenuationCouplingRejectReason& reason,




    HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason& reason,


/// Returns true when attenuation-coupling preflight passes (\c reason is \c None).
    HrtfAttenuationCouplingPreflight& out, HrtfAttenuationCouplingRejectReason& reason,
    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {});
                                             float occlusion_gain, HrtfAttenuationCouplingPreflight& preflight,

/// True when attenuation-coupling preflight rejects for `expected` (B7.2 deepen).
bool hrtf_attenuation_coupling_preflight_rejects_for_reason(
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason expected);
/// Attenuation-coupling preflight with optional reject-reason output.
/// Attenuation-coupling preflight with optional reject-reason output — valid paths unchanged.
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params,
                                         HrtfAttenuationCouplingRejectReason* reason);

/// Attenuation-coupling preflight with mandatory reject-reason output.
                                             HrtfAttenuationCouplingRejectReason& reason);
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingPreflight& preflight, const HrtfAttenuationCoupling& coupling = {},
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

/// Classify reject reason from an existing attenuation-coupling preflight bundle.
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight);
/// Attenuation-coupling preflight with optional reject-reason output (B7.2 deepen).



bool preflight_hrtf_attenuation_coupling_ready(
    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {},
    HrtfAttenuationCouplingRejectReason* reason = nullptr);
bool tryPreflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling = {},


bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               HrtfAttenuationCouplingRejectReason* reason = nullptr,
/// Map attenuation-coupling preflight flags to a reject reason (B7.2 deepen pass).

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

/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen follow-up).
    bool bypass_pan_path = false;
    bool unity_spatial_blend = false;

    bool should_skip_coupling() const { return bypass_pan_path || unity_spatial_blend; }
    bool should_narrow_spatial_image() const { return !should_skip_coupling(); }
    bool can_couple() const { return !should_skip_coupling(); }

/// Preflight distance/occlusion coupling for a resolved pan path.
    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {});

/// Preflight pan path plus attenuation coupling in one read-only bundle.
    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    float occlusion_gain, const HrtfAttenuationCoupling& coupling = {},

/// Diagnose why attenuation-coupling preflight would reject.
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             HrtfAttenuationCouplingRejectReason& outReason);

/// Preflight guard before attenuation coupling; false on bypass path or unity attenuation.
bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         HrtfAttenuationCouplingRejectReason* reason = nullptr);
/// Preflight guard — true when attenuation coupling should narrow the spatial image.
                                         float occlusion_gain);

/// Diagnose why attenuation-coupling preflight would skip spatial narrowing.
                                             HrtfAttenuationCouplingPreflightRejectReason* reason =
                                                 nullptr);
/// True when attenuation coupling may narrow the spatial image (B7.2 deepen).
                                        HrtfAttenuationCouplingRejectReason* reason,
/// Reject reason for attenuation-coupling preflight.
enum class HrtfAttenuationCouplingPreflightReject : u8 {

/// Attenuation-coupling preflight — read-only diagnostics before spatial blend narrowing.
    bool applyCoupling = false;
    bool narrowImage = false;
    HrtfAttenuationCouplingPreflightReject reject = HrtfAttenuationCouplingPreflightReject::None;

    bool can_skip() const { return !narrowImage; }

/// Populate attenuation-coupling preflight for a resolved pan path.

/// Returns true when attenuation coupling should narrow the binaural image.
                                             HrtfAttenuationCouplingPreflightReject* reject = nullptr,
/// Why HRTF attenuation coupling would not narrow the spatial image.
enum class HrtfAttenuationCouplingSkipReason : u8 {
    BypassPath = 1,
    UnityAttenuation = 2,
    UnitySpatialBlend = 3,

/// Preflight diagnostics for HRTF attenuation coupling.
    HrtfAttenuationCouplingSkipReason reason = HrtfAttenuationCouplingSkipReason::None;


/// Classify why attenuation coupling would not narrow the binaural image.
HrtfAttenuationCouplingSkipReason classify_hrtf_attenuation_coupling_skip(







/// True when an attenuation-coupling skip reason blocks spatial narrowing.
bool hrtf_attenuation_coupling_skip_reason_is_blocking(HrtfAttenuationCouplingSkipReason reason);

/// Preflight attenuation coupling — returns false when narrowing should be skipped.
                                       HrtfAttenuationCouplingSkipReason* reason = nullptr,

/// One-shot attenuation-coupling preflight with skip diagnostics.
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(
/// Why attenuation coupling would skip spatial narrowing (B7.2 deepen).

    HrtfAttenuationCouplingSkipReason skip_reason = HrtfAttenuationCouplingSkipReason::None;
    float distance_attenuation = 1.f;
    float occlusion_gain = 1.f;

    bool should_narrow() const { return skip_reason == HrtfAttenuationCouplingSkipReason::None; }
    bool can_apply_coupling() const { return should_narrow(); }
    bool skips_coupling() const { return !can_apply_coupling(); }

/// Preflight distance/occlusion coupling without mutating pan gains.

/// True when attenuation-coupling preflight skips for \p expected.
bool hrtf_attenuation_coupling_skips_for_reason(HrtfPanPath path, float distance_attenuation,
                                                HrtfAttenuationCouplingSkipReason expected,
/// Non-mutating predicate — mirrors `preflightHrtfAttenuationCoupling(...).canApplyCoupling()`.
bool canApplyHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen — coupling preflight).
    bool bypass_pan = false;

    bool should_narrow() const { return !bypass_pan && !unity_spatial_blend; }

/// Preflight attenuation coupling without mutating pan gains.

/// Non-mutating predicate — same guard as \c preflight_hrtf_attenuation_coupling().can_apply_coupling().
bool can_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
    bool bypass_path = true;
    bool unity_attenuation = true;
    bool skipped = true;

    [[nodiscard]] bool can_narrow() const { return !skipped; }
    [[nodiscard]] bool should_skip() const { return skipped; }


/// Convenience guard — `preflight_hrtf_attenuation_coupling(...).can_narrow()`.
bool can_narrow_hrtf_spatial_image_preflight(HrtfPanPath path, float distance_attenuation,

/// Bundled empty-IR, pan-path, and attenuation-coupling preflights (B7.2 deepen follow-up).
struct HrtfGuardedPanPreflight {
    HrtfIrPreflight ir{};
    HrtfPanPathPreflight pan_path{};
    HrtfAttenuationCouplingPreflight coupling{};

    [[nodiscard]] bool can_compute_spatial_pan() const { return pan_path.can_apply_spatial_pan(); }
    [[nodiscard]] bool should_skip_coupling() const { return coupling.should_skip(); }

/// One-shot preflight for guarded pan + attenuation coupling.
HrtfGuardedPanPreflight preflight_hrtf_guarded_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                                   const Vec3& rel_listener,
                                                   float distance_attenuation, float occlusion_gain,








/// Composite binaural preflight — aggregates IR, pan-path, and attenuation-coupling guard bundles (B7.2 deepen).
struct HrtfBinauralPreflight {
    HrtfPanPathPreflight panPath{};
    HrtfAttenuationCouplingPreflight attenuation{};
    bool skipped = false;

    bool can_spatial_pan() const { return !skipped && panPath.can_spatial_pan(); }
    bool can_convolve() const { return !skipped && ir.can_convolve() && panPath.can_convolve(); }
    bool can_narrow() const { return !skipped && panPath.can_spatial_pan() && attenuation.can_narrow(); }
};

/// Preflight full binaural HRTF dispatch from enable flag, IR stub, offset, and attenuation scalars.
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Preflight binaural HRTF when no IR is wired (ILD/ITD stub or bypass).
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,

/// Preflight binaural HRTF from a resolved pan path and IR stub.
HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, const HrtfIrStub& ir,

/// Preflight binaural HRTF from a resolved pan path when no IR is wired.
HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, float distance_attenuation,

/// Non-mutating spatial-pan predicate — mirrors \c HrtfBinauralPreflight::can_spatial_pan.
bool can_apply_binaural_hrtf_pan(const HrtfBinauralPreflight& preflight);

/// Non-mutating convolution predicate — mirrors \c HrtfBinauralPreflight::can_convolve.
bool can_convolve_binaural_hrtf(const HrtfBinauralPreflight& preflight);

/// Non-mutating narrowing predicate — mirrors \c HrtfBinauralPreflight::can_narrow.
bool can_narrow_binaural_hrtf_spatial_image(const HrtfBinauralPreflight& preflight);
/// Early-out when attenuation-coupling preflight would skip narrowing (B7.2 deepen follow-up).
bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
/// True when \c classify_hrtf_attenuation_coupling_reject matches \p expected (B7.2 deepen).
bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  HrtfAttenuationCouplingRejectReason expected,
/// Attenuation-coupling preflight with mandatory reject-reason output (B7.2 deepen reject-reason pass).
HrtfAttenuationCouplingPreflight try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling = {},
                                             HrtfAttenuationCouplingRejectReason& reason,
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);

/// Classify the dominant attenuation-coupling reject reason from a preflight bundle.
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight);

/// True when an attenuation-coupling reject reason blocks spatial image narrowing.
bool hrtf_attenuation_coupling_reject_reason_blocks_narrowing(
    HrtfAttenuationCouplingRejectReason reason);

/// Attenuation-coupling preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               const BinauralPanParams& params = {},

/// Attenuation-coupling preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_attenuation_coupling(
/// Why attenuation-coupling preflight rejected spatial image narrowing (B7.2 deepen).
/// Why attenuation-coupling preflight rejected spatial image narrowing (B7.2 deepen pass).
/// Why attenuation-coupling preflight rejects spatial narrowing (B7.2 deepen).
enum class HrtfAttenuationCouplingRejectReason : u8 {
    None = 0,
    BypassPath,
    UnityAttenuation,


bool preflight_hrtf_attenuation_coupling_ready(
    HrtfAttenuationCouplingRejectReason* reason = nullptr,


/// Derive narrowing reject reason from attenuation-coupling preflight diagnostics.

/// Attenuation-coupling narrowing preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_attenuation_narrowing_ready(HrtfPanPath path, float distance_attenuation,
bool preflight_hrtf_attenuation_narrowing_ready(
    const HrtfAttenuationCouplingPreflight& preflight,
bool try_preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
bool try_preflight_hrtf_attenuation_narrowing(
    HrtfAttenuationCouplingRejectReason& reason);
bool should_skip_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,


/// Attenuation-coupling preflight with optional reject-reason output (B7.2 deepen pass).
/// Classify the first attenuation-coupling reject reason from a preflight bundle.

/// True when \c classify_hrtf_attenuation_coupling_reject matches \p expected.
bool hrtf_attenuation_coupling_rejects_for_reason(
    HrtfAttenuationCouplingRejectReason expected);

    const HrtfAttenuationCoupling& coupling = {}, const BinauralPanParams& params = {},

/// Attenuation-coupling preflight that always writes the reject reason (B7.2 deepen pass).



/// Attenuation-coupling preflight with mandatory reject-reason output — returns \c can_narrow().
};

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight);

/// Attenuation-coupling preflight with optional reject-reason output (B7.2 deepen).
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {},
    HrtfAttenuationCouplingRejectReason* reason = nullptr);
bool try_preflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingRejectReason& reason,
    const BinauralPanParams& params = {});

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
/// Attenuation-coupling preflight — reports whether spatial narrowing would run.
    bool skipped = true;

    [[nodiscard]] bool can_narrow() const { return !skipped; }
    [[nodiscard]] bool can_apply_coupling() const { return !skipped; }
    [[nodiscard]] bool should_skip() const { return skipped; }

/// Preflight attenuation coupling for a resolved pan path.

/// Preflight attenuation coupling from HRTF enable flag, IR stub, and listener offset.

    bool bypass_path = false;
    bool unity_attenuation = false;
    bool will_narrow = false;

    [[nodiscard]] bool can_apply_coupling() const { return will_narrow; }
    [[nodiscard]] bool should_skip() const { return !will_narrow; }

/// Preflight attenuation coupling without mutating binaural gains.
/// Read-only attenuation-coupling diagnostics (B7.2 deepen follow-up).

    [[nodiscard]] bool should_skip_coupling() const { return bypass_path || unity_attenuation; }
    [[nodiscard]] bool should_narrow() const { return spatial_path && !unity_attenuation; }
    [[nodiscard]] bool can_apply_coupling() const { return should_narrow(); }

/// Preflight attenuation coupling without mutating pan gains (B7.2 deepen follow-up).

/// Combined spatial pan preflight — IR, pan path, and attenuation coupling (B7.2 deepen follow-up).
    HrtfIrPreflight ir{};

    [[nodiscard]] bool can_apply_spatial_pan() const { return pan.can_apply_spatial_pan(); }
    [[nodiscard]] bool should_narrow_spatial_image() const { return coupling.should_narrow(); }

/// Preflight spatial pan pipeline without computing gains (B7.2 deepen follow-up).
[[nodiscard]] HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(bool hrtf_enabled,
/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen).

    bool canApplyCoupling() const { return !bypass_path && !unity_attenuation; }


/// Non-mutating coupling predicate — same guards as \c preflight_hrtf_attenuation_coupling.
bool can_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,

    bool can_narrow_spatial_image() const { return !skipped; }
    bool can_apply_coupling() const { return !skipped; }

/// Preflight attenuation coupling — bypass path and unity-attenuation early-outs.

    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    float distance_attenuation, float occlusion_gain);
                                    const HrtfAttenuationCoupling& coupling = {},
                                    const BinauralPanParams& params = {});

/// Why HRTF attenuation coupling is skipped (B7.2 deepen).
enum class HrtfAttenuationCouplingRejectReason : u8 {
    None = 0,
    BypassPath,
    UnityAttenuation,
};

/// Human-readable label for attenuation-coupling reject reasons (B7.2 deepen).
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason);

/// Classify why attenuation coupling would be skipped for a resolved pan path (B7.2 deepen).
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain);

/// Read-only attenuation-coupling diagnostics — no mutation (B7.2 deepen).
struct HrtfAttenuationCouplingPreflight {
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfAttenuationCouplingRejectReason rejectReason = HrtfAttenuationCouplingRejectReason::BypassPath;
    bool bypassed = true;
    bool unityAttenuation = false;
    bool skipped = true;
    float spatialBlend = 1.f;

    bool can_narrow() const { return !skipped; }
};

/// Non-mutating attenuation-coupling preflight for a resolved pan path (B7.2 deepen).
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

/// Non-mutating attenuation-coupling preflight with pan-path resolution (B7.2 deepen).
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    float occlusion_gain, const HrtfAttenuationCoupling& coupling = {},
    const BinauralPanParams& params = {});

/// True when attenuation coupling should narrow the spatial image (B7.2 deepen).
bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         HrtfAttenuationCouplingRejectReason* reason);

/// Combined HRTF pan + coupling preflight for one-shot guarded pan (B7.2 deepen).
struct HrtfBinauralPanPreflight {
    HrtfIrPreflight ir{};
    HrtfPanPathPreflight panPath{};
    HrtfAttenuationCouplingPreflight attenuationCoupling{};

    bool can_apply_spatial_pan() const { return panPath.can_apply_spatial_pan(); }
    bool can_apply_attenuation_coupling() const { return attenuationCoupling.can_narrow(); }
};

/// Non-mutating combined IR/pan-path/attenuation preflight (B7.2 deepen).
HrtfBinauralPanPreflight preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener,
                                                     float distance_attenuation, float occlusion_gain,
                                                     const HrtfAttenuationCoupling& coupling = {},
                                                     const BinauralPanParams& params = {});

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
/// Why composite binaural/HRTF would bypass spatial pan (B7.2 deepen follow-up).
/// Primary composite reject reason — mirrors pan-path bypass (B7.2 deepen follow-up).
/// Why composite binaural/HRTF processing would bypass (B7.2 deepen reject-reason pass).
/// Primary composite binaural bypass reason (B7.2 deepen).
/// Why composite binaural/HRTF processing would bypass spatial pan (B7.2 deepen).
/// Why composite binaural/HRTF processing would centre-bypass (B7.2 deepen follow-up).
/// Composite binaural bypass / early-out reason (B7.2 deepen pass).
/// Primary composite binaural reject reason — pan bypass wins over convolution skips (B7.2 deepen follow-up pass).
/// Primary composite binaural reject reason — spatial bypass or sub-guard skip (B7.2 deepen follow-up).
/// Composite binaural reject reason — first pipeline-stage reject in evaluation order (B7.2 deepen).
/// Diagnostic reason the composite binaural path is bypassed or degraded (B7.2 deepen).
/// Why composite binaural/HRTF processing is rejected or degraded (B7.2 deepen).
/// Why composite binaural/HRTF would bypass spatial pan (B7.2 deepen follow-up pass).
/// Why composite binaural/HRTF preflight rejected a pipeline stage (B7.2 deepen).
/// Primary composite binaural reject reason — pan bypass beats IR fallback (B7.2 deepen reject-reason pass).
/// Composite binaural/HRTF reject reason — primary skip/fallback diagnostic (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejects a stage (B7.2 deepen).
/// Primary composite binaural/HRTF reject reason — first guard that blocks the requested stage (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejected the primary spatial path (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejected a stage (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejected the request (B7.2 deepen).
/// Composite binaural reject reasons — bundles empty-IR, pan-path, and coupling skips (B7.2 deepen).
/// Why composite binaural preflight rejected a processing stage (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejected a guard stage (B7.2 deepen).
/// Primary reject reason for composite binaural/HRTF preflight (B7.2 deepen).
/// Primary diagnostic reason composite binaural/HRTF bypasses centre mono (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejected spatial processing (B7.2 deepen).
/// Why HRTF convolution is unavailable from a composite preflight (B7.2 deepen pass).
enum class HrtfConvolutionRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
    EmptyIr,
    MalformedIr,
};

/// Why composite binaural/HRTF preflight rejected spatial pan (B7.2 deepen pass).
/// Why composite binaural preflight rejected centre bypass (B7.2 deepen).
/// Why composite binaural/HRTF processing is rejected (B7.2 deepen pass).
/// Unified reject reason across binaural/HRTF guard domains (B7.2 deepen pass).
/// Primary composite binaural/HRTF reject reason (B7.2 deepen pass).
enum class HrtfBinauralRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
    NullSamples,
    ZeroLength,
/// Why composite binaural preflight rejected the request (B7.2 deepen).
    None,
    EmptyIr,
    MalformedIr,
    UnityAttenuation,
};

/// Why composite binaural/HRTF preflight rejected convolution dispatch (B7.2 deepen).
enum class HrtfBinauralConvolutionRejectReason : u8 {

/// Why composite binaural/HRTF preflight rejected spatial image narrowing (B7.2 deepen).
enum class HrtfBinauralNarrowingRejectReason : u8 {
    BypassPath,

/// Human-readable label for composite binaural reject reasons (logging / tests).
const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason);
/// Why composite binaural/HRTF preflight degraded the pipeline (B7.2 deepen follow-up).
    PanBypass,
    IrFallback,
    AttenuationSkipped,

/// Human-readable label for composite binaural reject reasons (B7.2 deepen follow-up).
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason);

/// Human-readable label for composite binaural reject reasons (B7.2 deepen).


/// Classify primary composite binaural skip reason from pan-path and IR guards (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener);

/// Classify composite attenuation-coupling reject reason (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain);


/// Diagnose why composite binaural would bypass; vacuously succeeds on spatial paths.

/// Human-readable label for diagnostics and test assertions (B7.2 deepen follow-up).

/// Map pan-path reject reason to composite binaural reject reason (B7.2 deepen follow-up).
HrtfBinauralRejectReason hrtf_binaural_reject_reason(HrtfPanPathRejectReason pan_reason);

/// Returns the composite binaural reject reason for one source (IR-aware).
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected (B7.2 deepen follow-up).

/// Returns the first reject reason for composite binaural dispatch, or \c None when pan may proceed.

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected.
bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected);


/// Classify why composite binaural pan is bypassed (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const Vec3& rel_listener);

struct HrtfBinauralPreflight;

const char* hrtfBinauralRejectReasonName(HrtfBinauralRejectReason reason);

/// Primary spatial-bypass reject from a composite preflight bundle.
HrtfBinauralRejectReason hrtfBinauralRejectReason(const HrtfBinauralPreflight& preflight);

/// Convolution reject carried by the composite IR preflight sub-bundle.
HrtfIrRejectReason hrtfBinauralConvolutionRejectReason(const HrtfBinauralPreflight& preflight);

/// Narrowing reject carried by the composite attenuation-coupling sub-bundle.
HrtfAttenuationCouplingRejectReason hrtfBinauralNarrowingRejectReason(
    const HrtfBinauralPreflight& preflight);

/// Returns true when \c hrtfBinauralRejectReason matches \p expected (B7.2 deepen).
bool hrtfBinauralRejectsForReason(const HrtfBinauralPreflight& preflight,


/// Diagnose why composite binaural processing would bypass spatial pan.

/// Returns true when `hrtf_binaural_reject_reason` matches `expected` (B7.2 deepen follow-up).



/// Diagnose the primary composite reject reason for one source (IR-aware).
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling = {},
                                                    const BinauralPanParams& params = {});

/// Diagnose the primary composite reject reason when no IR is wired.
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener,

/// Returns true when `hrtf_binaural_reject_reason` matches `expected` (B7.2 deepen follow-up pass).
bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, float distance_attenuation,
                                      float occlusion_gain, HrtfBinauralRejectReason expected,

/// Returns the primary composite reject reason for one source (IR-aware).

/// Returns the primary composite reject reason when no IR is wired.


/// Returns the first composite reject reason for one source (pan-path, then IR, then coupling).

/// First composite reject reason for one source, or \c None when all guards pass.
                                                     float distance_attenuation,
                                                     float occlusion_gain);

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected (B7.2 deepen).
                                      float occlusion_gain, HrtfBinauralRejectReason expected);
/// Human-readable label for diagnostics and test assertions (B7.2 deepen).

/// Map pan-path reject reason into the composite namespace (B7.2 deepen).
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan(HrtfPanPathRejectReason reason);

/// Map IR reject reason into the composite namespace (B7.2 deepen).
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_ir(HrtfIrRejectReason reason);

/// Map attenuation-coupling reject reason into the composite namespace (B7.2 deepen).
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_coupling(
    HrtfAttenuationCouplingRejectReason reason);


/// Human-readable label for composite binaural reject reasons (B7.2 deepen follow-up pass).

/// Diagnose why composite binaural would bypass; vacuously succeeds when spatial pan may proceed.


/// True when a composite reject reason blocks spatial panning.
bool hrtf_binaural_reject_reason_is_blocking_spatial(HrtfBinauralRejectReason reason);

/// True when a composite reject reason blocks IR convolution.
bool hrtf_binaural_reject_reason_is_blocking_convolution(HrtfBinauralRejectReason reason);

/// True when a composite reject reason blocks attenuation-coupling narrowing.
bool hrtf_binaural_reject_reason_is_blocking_coupling(HrtfBinauralRejectReason reason);

/// Classify composite spatial-pan reject — same ordering as \c preflight_hrtf_binaural.
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(bool hrtf_enabled,

/// Classify composite convolution reject — same ordering as \c preflight_hrtf_binaural.
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(bool hrtf_enabled,
                                                                   const HrtfIrStub& ir,

/// Classify composite attenuation-coupling reject — same ordering as \c preflight_hrtf_binaural.
HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(HrtfPanPath path,

/// Non-mutating composite spatial-pan preflight — returns true when spatial pan may proceed.
bool preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfBinauralRejectReason* reason = nullptr);

/// Non-mutating composite convolution preflight — returns true when convolution may proceed.
bool preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,

/// Non-mutating composite attenuation-coupling preflight — returns true when narrowing may proceed.
bool preflight_hrtf_binaural_coupling(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain,



/// Returns true when \c hrtf_binaural_reject_reason matches \p expected (IR-aware).
                                      const Vec3& rel_listener, HrtfBinauralRejectReason expected);

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected (no IR wired).

/// True when a composite reject reason blocks centre bypass (disabled or co-located).
bool hrtf_binaural_reject_reason_is_bypass(HrtfBinauralRejectReason reason);

/// True when a composite reject reason blocks IR convolution (empty or malformed IR).
bool hrtf_binaural_reject_reason_blocks_convolution(HrtfBinauralRejectReason reason);

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason);
/// Why composite binaural/HRTF preflight would reject a sub-operation (B7.2 deepen).
    PanBypassDisabled,
    PanBypassCoLocated,
    ConvolutionEmptyIr,
    ConvolutionMalformedIr,
    NarrowingBypassPath,
    NarrowingUnityAttenuation,


/// True when a composite reject reason blocks spatial pan entirely.
bool is_blocking_hrtf_binaural_reject_reason(HrtfBinauralRejectReason reason);

/// Diagnose the primary composite reject reason for diagnostics (B7.2 deepen).

/// True when `classify_hrtf_binaural_reject` matches `expected` (B7.2 deepen).

/// Classify why composite binaural spatial pan would be skipped (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_skip_reject(bool hrtf_enabled, const Vec3& rel_listener);

/// Classify why composite binaural convolution would be rejected (B7.2 deepen).

/// Classify why composite binaural attenuation coupling would be skipped (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(HrtfPanPath path,

/// Classify the primary composite reject reason for one source (IR-aware).
                                                     float distance_attenuation, float occlusion_gain);

/// True when a composite reject reason would block binaural spatial pan.

/// True when a composite reject reason would block IR convolution dispatch.
bool is_blocking_hrtf_binaural_convolution_reject_reason(HrtfBinauralRejectReason reason);

/// Returns true when `hrtf_binaural_reject_reason` matches `expected`.


/// Classify the primary pan-path reject reason for composite preflight (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(bool hrtf_enabled, const Vec3& rel_listener);

/// Classify why composite preflight would skip IR convolution (B7.2 deepen).

/// Classify why composite preflight would skip attenuation coupling (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_attenuation_reject(

/// True when a composite reject reason blocks spatial pan (B7.2 deepen).
bool hrtf_binaural_reject_reason_is_pan_blocking(HrtfBinauralRejectReason reason);

/// True when a composite reject reason blocks IR convolution (B7.2 deepen).
bool hrtf_binaural_reject_reason_is_convolution_blocking(HrtfBinauralRejectReason reason);

/// True when a composite binaural reject reason would block the requested path (B7.2 deepen).
bool hrtf_binaural_reject_reason_is_blocking(HrtfBinauralRejectReason reason);

/// Classify primary composite reject reason for spatial pan (B7.2 deepen).

/// Classify primary composite reject reason for IR convolution (B7.2 deepen).

/// Classify primary composite reject reason for attenuation coupling (B7.2 deepen).
    bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation, float occlusion_gain);



/// True when a composite reject reason blocks the requested binaural stage.

/// True when a composite reject reason would block centre bypass (B7.2 deepen).
bool hrtfBinauralRejectReasonIsBypass(HrtfBinauralRejectReason reason);

/// True when a composite reject reason would block IR convolution (B7.2 deepen).
bool hrtfBinauralRejectReasonBlocksConvolution(HrtfBinauralRejectReason reason);

/// Classify the primary composite reject reason — same ordering as `tryPreflightHrtfBinaural`.
HrtfBinauralRejectReason classifyHrtfBinauralReject(
    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    float occlusion_gain, const HrtfAttenuationCoupling& coupling = {},

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfBinauralRejectReason& reason,

/// Composite binaural preflight with mandatory reject-reason output when no IR is wired (B7.2 deepen).
bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener,

/// True when a composite reject reason blocks spatial panning (B7.2 deepen).
bool hrtfBinauralRejectReasonIsBlocking(HrtfBinauralRejectReason reason);

/// Primary composite reject reason for binaural/HRTF dispatch (B7.2 deepen).
    CoLocatedSource,

/// Human-readable label for composite binaural/HRTF reject reasons (B7.2 deepen).

/// Human-readable labels for composite reject diagnostics (B7.2 deepen pass).
const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason);

/// Why composite binaural preflight rejected IR convolution (B7.2 deepen).
enum class HrtfBinauralConvolveRejectReason : u8 {
    None = 0,
    Bypass,


/// Composite binaural/HRTF preflight — bundles empty-IR, pan-path, and attenuation-coupling guards (B7.2 deepen).
struct HrtfBinauralPreflight {
    HrtfBinauralRejectReason reason = HrtfBinauralRejectReason::None;
    bool rejected = false;


/// True when a composite binaural reject reason blocks spatial pan dispatch.

/// Classify why composite binaural preflight would bypass — mirrors pan-path reject ordering.



/// Returns the first composite bypass reason, or \c None when spatial pan may proceed.


    HrtfConvolutionRejectReason convolutionReason = HrtfConvolutionRejectReason::None;

/// Human-readable label for composite binaural/HRTF reject reasons (B7.2 deepen pass).

/// Map an empty-IR reject reason into the composite enum (B7.2 deepen pass).

/// Map a pan-path reject reason into the composite enum (B7.2 deepen pass).
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan_path(HrtfPanPathRejectReason reason);

/// Map an attenuation-coupling reject reason into the composite enum (B7.2 deepen pass).
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_attenuation(

/// Primary bypass reject for spatial pan (disabled or co-located source).
HrtfBinauralRejectReason hrtf_binaural_skip_reject_reason(bool hrtf_enabled, const Vec3& rel_listener);

/// IR convolution reject mapped into the composite enum.
HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(const HrtfIrStub& ir);

/// Attenuation-coupling reject mapped into the composite enum.
HrtfBinauralRejectReason hrtf_binaural_attenuation_reject_reason(

/// Returns true when composite skip reject matches `expected` (B7.2 deepen pass).
bool hrtf_binaural_skip_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,

/// Returns true when composite convolution reject matches `expected` (B7.2 deepen pass).
bool hrtf_binaural_convolution_rejects_for_reason(const HrtfIrStub& ir,

    HrtfBinauralRejectReason skipReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason attenuationReason = HrtfBinauralRejectReason::None;

    HrtfIrPreflight ir{};
    HrtfPanPathPreflight panPath{};
    HrtfAttenuationCouplingPreflight attenuationCoupling{};
    HrtfBinauralRejectReason reason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason narrowingReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason rejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason spatialRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason couplingRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason skipReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason narrowingRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason panRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason attenuationRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason spatialPanReject = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason convolutionReject = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason narrowingReject = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason spatialPanRejectReason = HrtfBinauralRejectReason::None;
    HrtfBinauralRejectReason reject = HrtfBinauralRejectReason::None;
    HrtfBinauralConvolveRejectReason convolveReject = HrtfBinauralConvolveRejectReason::None;

    HrtfPanPath path() const { return panPath.path; }
    HrtfBinauralRejectReason rejectReason() const {
        return hrtf_binaural_reject_reason(panPath.reason);
    }

    bool rejected() const { return reason != HrtfBinauralRejectReason::None; }

    bool can_spatial_pan() const { return !rejected && panPath.can_spatial_pan(); }
    bool can_convolve() const { return can_spatial_pan() && panPath.can_convolve(); }
    bool can_narrow_spatial_image() const { return attenuationCoupling.can_narrow(); }
    bool is_bypass() const { return panPath.skipped; }
    bool ok() const { return reason == HrtfBinauralRejectReason::None; }
    bool is_bypass() const { return hrtf_binaural_reject_reason_is_bypass(reason); }
    bool can_spatial_pan() const { return reason == HrtfBinauralRejectReason::None; }
    bool can_convolve() const { return can_spatial_pan() && ir.can_convolve(); }
    bool is_bypass() const { return reason != HrtfBinauralRejectReason::None; }

    /// True when composite preflight selects centre bypass (disabled or co-located).
    bool should_skip() const { return is_bypass(); }

    /// True when the resolved path selects ILD/ITD stub (empty IR fallback).
    bool uses_ild_itd_stub() const { return panPath.uses_ild_itd_stub(); }

    /// True when the composite preflight carries an empty or malformed IR stub.
    bool has_empty_ir() const { return ir.emptyIr; }

    /// True when IR convolution should be skipped (empty or malformed IR, or bypass).
    bool should_skip_convolution() const { return !can_convolve(); }
    bool ok() const { return reason == HrtfBinauralRejectReason::None; }
    bool is_bypass() const { return reason != HrtfBinauralRejectReason::None; }
    bool is_bypass() const { return panPath.reason != HrtfPanPathRejectReason::None; }
    bool is_bypass() const { return rejected || panPath.skipped; }
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

/// Returns true when \c hrtf_binaural_narrowing_reject_reason matches \p expected (B7.2 deepen follow-up pass).
bool hrtf_binaural_rejects_for_narrowing_reason(const HrtfBinauralPreflight& preflight,

    bool should_skip_convolution() const { return ir.should_skip_convolution(); }

/// True when composite IR and pan-path preflights agree on convolution eligibility.
bool is_consistent_hrtf_binaural_preflight(const HrtfBinauralPreflight& preflight);
    bool ok() const { return can_spatial_pan(); }

/// Classify the primary composite binaural reject reason (B7.2 deepen follow-up).
HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight);


    bool is_bypass() const { return rejectReason != HrtfBinauralRejectReason::None; }

/// Primary bypass reason from a composite preflight bundle.

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected.

/// Returns true when a populated composite preflight matches `expected`.

/// Primary composite reject reason — pan bypass takes precedence over IR/coupling (B7.2 deepen).

/// Returns true when \c hrtf_binaural_reject_reason matches \p expected (B7.2 deepen).
/// Classify why spatial pan would be rejected from a composite preflight bundle.
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
    const HrtfBinauralPreflight& preflight);

/// Classify why IR convolution would be rejected from a composite preflight bundle.
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(

/// Classify why spatial image narrowing would be rejected from a composite preflight bundle.
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(

    /// True when composite preflight reports no primary reject reason.

/// Classify the primary composite reject reason from bundled preflights (B7.2 deepen).
HrtfBinauralRejectReason classifyHrtfBinauralReject(const HrtfBinauralPreflight& preflight);
/// Returns the composite convolution reject reason from a preflight bundle (B7.2 deepen).
HrtfConvolutionRejectReason hrtf_binaural_convolution_reject_reason(

/// Returns true when composite convolution reject matches \p expected (B7.2 deepen).
bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                   HrtfConvolutionRejectReason expected);

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason);
/// Why composite binaural preflight rejected the request (B7.2 deepen pass).
enum class HrtfBinauralRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
};

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason);
HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight);
HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(

/// Composite binaural preflight with optional reject-reason output (B7.2 deepen pass).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener, float distance_attenuation,
                                     float occlusion_gain, HrtfBinauralRejectReason* reason = nullptr,
                                     const HrtfAttenuationCoupling& coupling = {},
                                     const BinauralPanParams& params = {});
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                     float distance_attenuation, float occlusion_gain,
                                     HrtfBinauralRejectReason* reason = nullptr,
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                  float occlusion_gain, HrtfBinauralRejectReason& reason,
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                  HrtfBinauralRejectReason& reason,

/// Classify why convolution cannot run from a composite preflight bundle.
HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfBinauralPreflight& preflight);

/// Classify why spatial pan is bypassed from a composite preflight bundle.

/// True when \c classify_hrtf_convolution_reject matches \p expected.
bool hrtf_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,

/// True when \c classify_hrtf_binaural_reject matches \p expected.

const char* hrtf_binaural_convolve_reject_reason_label(HrtfBinauralConvolveRejectReason reason);
HrtfBinauralConvolveRejectReason classify_hrtf_binaural_convolve_reject(

/// Map composite binaural preflight flags to a reject reason (B7.2 deepen pass).

/// Diagnose the primary composite reject reason from bundled preflights.

/// Returns true when \c classify_hrtf_binaural_reject matches \p expected.

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

/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                            float distance_attenuation, float occlusion_gain,
                            HrtfBinauralRejectReason* reason,
                            const HrtfAttenuationCoupling& coupling = {},
                            const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Preflight all binaural/HRTF guards from listener and source world positions (IR-aware).
HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, const HrtfIrStub& ir,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling = {},
                                              const BinauralPanParams& params = {});

/// Preflight all binaural/HRTF guards from listener and source world positions (no IR wired).
                                              const Vec3& source_position, float distance_attenuation,
                                              float occlusion_gain,
/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen follow-up).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfBinauralPreflight& preflight,
/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfBinauralRejectReason* reason,

/// Composite binaural preflight without IR with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
                                  HrtfBinauralRejectReason& reason,

/// Composite binaural preflight without IR with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,








                                 HrtfBinauralPreflight& preflight, HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,


/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

/// Composite binaural preflight without IR with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling = {},
                                  const BinauralPanParams& params = {});

/// Composite binaural preflight without IR with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling = {},
                                  const BinauralPanParams& params = {});

/// Returns true when composite preflight has no bypass reject (\c reason is \c None or IR fallback only).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& out, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// True when composite binaural preflight skips spatial pan for `expected` (B7.2 deepen).
bool hrtf_binaural_preflight_skips_for_reason(const HrtfBinauralPreflight& preflight,
                                              HrtfBinauralRejectReason expected);

/// True when composite binaural preflight rejects convolution for `expected` (B7.2 deepen).
bool hrtf_binaural_preflight_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                            HrtfBinauralRejectReason expected);

/// True when composite binaural preflight rejects narrowing for `expected` (B7.2 deepen).
bool hrtf_binaural_preflight_narrowing_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                          HrtfBinauralRejectReason expected);

/// Composite binaural preflight with optional reject-reason output.
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params, HrtfBinauralRejectReason* reason);

/// Composite binaural preflight with mandatory reject-reason output.
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason);

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output when no IR is wired (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& out, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Composite binaural preflight with optional reject-reason output — valid paths unchanged.
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params, HrtfBinauralRejectReason* reason);

/// Composite binaural preflight with mandatory reject-reason output.
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason);

/// Classify why IR convolution would be rejected within a composite preflight.
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight);

/// Classify why spatial image narrowing would be rejected within a composite preflight.
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight);

/// Classify the primary spatial-pan reject reason within a composite preflight.
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
    const HrtfBinauralPreflight& preflight);

/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

/// Composite binaural preflight without IR with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling = {},
                                  const BinauralPanParams& params = {});

/// Composite binaural preflight without IR with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling = {},
                                  const BinauralPanParams& params = {});

/// Composite binaural/HRTF preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling = {},
                                   const BinauralPanParams& params = {},
                                   HrtfBinauralRejectReason* reason = nullptr);
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling = {},
                                   const BinauralPanParams& params = {},
                                   HrtfBinauralRejectReason* reason = nullptr);
bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                float distance_attenuation, float occlusion_gain,
                                HrtfBinauralRejectReason& reason,
                                const HrtfAttenuationCoupling& coupling = {},
                                const BinauralPanParams& params = {});
bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                float distance_attenuation, float occlusion_gain,
                                HrtfBinauralRejectReason& reason,
                                const HrtfAttenuationCoupling& coupling = {},
                                const BinauralPanParams& params = {});

/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

/// Composite binaural preflight without IR with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling = {},
                              const BinauralPanParams& params = {});

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling = {},
                                  const BinauralPanParams& params = {});

/// Composite binaural preflight without IR with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling = {},
                                  const BinauralPanParams& params = {});

/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason = nullptr,
                                   const HrtfAttenuationCoupling& coupling = {},
                                   const BinauralPanParams& params = {});
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason = nullptr,
                                   const HrtfAttenuationCoupling& coupling = {},
                                   const BinauralPanParams& params = {});
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
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


/// Non-mutating convolution predicate — mirrors \c HrtfBinauralPreflight::can_convolve.
bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight);


/// True when the composite preflight carries an empty or malformed IR stub.
bool has_empty_hrtf_ir(const HrtfBinauralPreflight& preflight);

/// True when distance/occlusion coupling should narrow the binaural image.
bool should_apply_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight);

/// Early-out inverse of \c should_apply_hrtf_attenuation_coupling.
bool should_skip_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight);

/// Non-mutating narrowing predicate — mirrors \c HrtfBinauralPreflight::can_narrow_spatial_image.
bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight);

/// True when composite preflight carries an empty or malformed IR stub.



/// Early-out inverse of composite \c should_apply_hrtf_attenuation_coupling.
/// Early-out when composite binaural preflight would bypass spatial pan (B7.2 deepen follow-up).
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener);




/// True when \c classify_hrtf_binaural_reject matches \p expected (B7.2 deepen).
bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected);

/// Classify the primary composite binaural reject reason from a preflight bundle (B7.2 deepen).
HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight);

/// True when `classify_hrtf_binaural_reject` matches `expected` (B7.2 deepen).
bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,

/// IR preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight);

/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen).
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});
/// Composite binaural preflight with mandatory reject-reason output (B7.2 deepen reject-reason pass).
HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  const Vec3& rel_listener, float distance_attenuation,
                                                  float occlusion_gain, HrtfBinauralRejectReason& reason,

/// Composite binaural preflight with mandatory reject-reason output when no IR is wired.
HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                                  HrtfBinauralRejectReason& reason,
/// Composite binaural preflight with mandatory spatial-pan reject-reason output.
bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,

/// Composite binaural preflight with mandatory convolution reject-reason output.
bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,

/// Composite binaural preflight with mandatory narrowing reject-reason output.
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason);
const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason);
const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason);

/// Classify the dominant composite binaural reject reason from a preflight bundle.

/// Classify convolution reject reason from a composite binaural preflight bundle.
HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight);

/// Classify narrowing reject reason from a composite binaural preflight bundle.
HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(

/// True when a composite reject reason blocks spatial pan.
bool hrtf_binaural_reject_reason_blocks_spatial_pan(HrtfBinauralRejectReason reason);

/// True when a composite reject reason blocks convolution dispatch.
bool hrtf_binaural_reject_reason_blocks_convolution(HrtfBinauralRejectReason reason);

/// Composite binaural spatial-pan preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   const BinauralPanParams& params = {},
                                   HrtfBinauralRejectReason* reason = nullptr);

/// Composite binaural spatial-pan preflight when no IR is wired (B7.2 deepen).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,

/// Composite binaural spatial-pan preflight with mandatory reject-reason output (B7.2 deepen).

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
/// Why composite binaural preflight rejected spatial pan (B7.2 deepen).
/// Why composite binaural preflight rejected a guarded operation (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejected a path (B7.2 deepen).
/// Why composite binaural/HRTF preflight rejects an operation (B7.2 deepen).
enum class HrtfBinauralRejectReason : u8 {
    None = 0,
    HrtfDisabled,
    CoLocated,
};


/// Composite convolution reject — delegates to IR preflight classification.
HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight);

/// Composite narrowing reject — delegates to attenuation-coupling classification.
HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(

/// Composite binaural preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason = nullptr,
                                   HrtfBinauralRejectReason* reason = nullptr,
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
    EmptyIr,
    MalformedIr,
    BypassPath,
    UnityAttenuation,


/// Derive spatial-pan reject reason from composite binaural preflight.
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight);

/// Derive convolution reject reason from composite binaural preflight.
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(

/// Derive narrowing reject reason from composite binaural preflight.
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(

/// Composite binaural spatial preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
bool preflight_hrtf_binaural_spatial_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfBinauralRejectReason& reason);
bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason);
bool try_preflight_hrtf_binaural_spatial(const HrtfBinauralPreflight& preflight,

/// Composite binaural convolution preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,

/// Composite binaural narrowing preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             float occlusion_gain,
bool preflight_hrtf_binaural_narrowing_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_binaural_narrowing(const HrtfBinauralPreflight& preflight,

/// Diagnose the primary composite reject reason from a preflight bundle.
HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight);

/// Returns true when `hrtf_binaural_reject_reason` matches `expected` (B7.2 deepen).
/// Composite binaural preflight with optional reject-reason output (B7.2 deepen pass).
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling = {},

/// Composite binaural preflight when no IR is wired, with optional reject-reason output (B7.2 deepen pass).

/// Composite binaural preflight that always writes the reject reason (B7.2 deepen pass).

/// Composite binaural preflight when no IR is wired that always writes the reject reason (B7.2 deepen pass).


bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,

/// Composite convolution preflight with optional reject-reason output (B7.2 deepen).

/// Composite narrowing preflight with optional reject-reason output (B7.2 deepen).

/// Composite binaural preflight with mandatory reject-reason output — returns \c can_spatial_pan().
};

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason);
HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight);
    const HrtfBinauralPreflight& preflight);

/// Composite pan preflight with optional reject-reason output (B7.2 deepen).
bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                       HrtfBinauralRejectReason* reason = nullptr);
bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const Vec3& rel_listener,

bool preflight_hrtf_binaural_convolution_ready(const HrtfIrStub& ir,
bool try_preflight_hrtf_binaural_convolution(const HrtfIrStub& ir,

bool preflight_hrtf_binaural_narrowing_ready(HrtfPanPath path, float distance_attenuation,
                                             const HrtfAttenuationCoupling& coupling = {},
                                             const BinauralPanParams& params = {},
bool try_preflight_hrtf_binaural_narrowing(HrtfPanPath path, float distance_attenuation,
                                           HrtfBinauralRejectReason& reason,
                                           const BinauralPanParams& params = {});

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
