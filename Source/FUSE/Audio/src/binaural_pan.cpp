#include <fuse/audio/binaural_pan.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

namespace {

constexpr float kHrtfCoLocatedEpsilon = 1e-5f;
constexpr float kHrtfUnityAttenuationEpsilon = 1e-5f;

} // namespace

float hrtf_co_located_epsilon() {
    return kHrtfCoLocatedEpsilon;
}

bool is_co_located_hrtf_source(const Vec3& rel_listener) {
    return rel_listener.length() < kHrtfCoLocatedEpsilon;
HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight result;
    result.null_samples = ir.samples == nullptr;
    result.zero_length = ir.length == 0;
    return result;
}

HrtfIrPreflight preflight_hrtf_ir(const float* samples, u32 length) {
    return preflight_hrtf_ir(HrtfIrStub{samples, length});

bool can_use_hrtf_ir_preflight(const HrtfIrStub& ir) {
    return preflight_hrtf_ir(ir).has_valid_ir();

    result.empty_ir = is_empty_hrtf_ir(ir);

HrtfIrStub make_empty_hrtf_ir() {
    return HrtfIrStub{};
}

HrtfIrStub make_hrtf_ir_stub(const float* samples, u32 length) {
    if (samples == nullptr || length == 0) {
        return make_empty_hrtf_ir();
    }
    return HrtfIrStub{samples, length};
}

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight result;
    result.null_samples = ir.samples == nullptr;
    result.zero_length = ir.length == 0;
    result.empty_ir = is_empty_hrtf_ir(ir);
    return result;
}

bool has_hrtf_ir(const HrtfIrStub& ir) {
    return ir.samples != nullptr && ir.length > 0;
}

bool is_empty_hrtf_ir(const HrtfIrStub& ir) {
    return !has_hrtf_ir(ir);
}

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight result;
    result.null_samples = ir.samples == nullptr;
    result.zero_length = ir.length == 0;
    result.empty_ir = is_empty_hrtf_ir(ir);
    return result;
}

bool should_use_hrtf_ir(const HrtfIrStub& ir) {
    return has_hrtf_ir(ir);
}

bool should_skip_hrtf_convolution(const HrtfIrStub& ir) {
    return !preflight_hrtf_ir_convolution(ir);
}

bool is_nonnull_zero_length_hrtf_ir(const HrtfIrStub& ir) {
    return ir.samples != nullptr && ir.length == 0;

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    return "Unknown";

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    if (ir.samples == nullptr && ir.length > 0) {
        return HrtfIrRejectReason::NullSamples;
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    if (ir.samples == nullptr) {

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::EmptyIr;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedStub:
        return "malformed_stub";
    }
    return "unknown";
}

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedStub;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::NullSamples;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    }
    return "unknown";
}

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight preflight{};
    preflight.reason = hrtf_ir_reject_reason(ir);
    preflight.nullSamples = ir.samples == nullptr;
    preflight.zeroLength = ir.length == 0;
    preflight.malformedIr = is_nonnull_zero_length_hrtf_ir(ir);
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    preflight.reason = classify_hrtf_ir_reject(ir);
    return preflight;

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight) {
    preflight = preflight_hrtf_ir(ir);
    return preflight.can_convolve();
}

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfIrRejectReason::None;
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir(ir, &reason);
}

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfIrRejectReason::None;
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir(ir, &reason);
}

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfIrRejectReason::None;
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir(ir, &reason);
}

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfIrRejectReason::None;
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir(ir, &reason);
}

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfIrRejectReason::None;
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir(ir, &reason);
}

bool can_convolve_hrtf_ir(const HrtfIrPreflight& preflight) {
    return preflight.can_convolve();

bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight) {
    return preflight.should_skip_convolution();

const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason) {
    case HrtfPanPathRejectReason::None:
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    return HrtfPanPathRejectReason::None;

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;

bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight) {
    return preflight.should_skip_convolution();
}

bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight) {
    return preflight.should_skip_convolution();
}

bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir) {
    return classify_hrtf_ir_reject(ir) != HrtfIrRejectReason::None;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight) {
    return preflight.should_skip_convolution();
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;
}

const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    preflight.hrtfDisabled = !hrtf_enabled;
    preflight.coLocated = is_co_located_hrtf_source(rel_listener);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.skipped = should_skip_hrtf_pan_path(preflight.path);

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool can_apply_spatial_hrtf_pan(const HrtfPanPathPreflight& preflight) {
    return preflight.can_spatial_pan();

bool can_convolve_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {

bool should_skip_hrtf_pan_path_preflight(const HrtfPanPathPreflight& preflight) {
    return preflight.should_skip();

bool uses_ild_itd_stub_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.uses_ild_itd_stub();
bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !hrtf_enabled || rel_listener.length() < 1e-5f;

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !should_skip_hrtf_pan(hrtf_enabled, rel_listener);
bool should_fallback_to_ild_itd_stub(const HrtfIrStub& ir) {

HrtfIrStub normalize_hrtf_ir_stub(const HrtfIrStub& ir) {
    if (!has_hrtf_ir(ir)) {
        return make_empty_hrtf_ir();
    return ir;

u32 hrtf_ir_stub_sample_count(const HrtfIrStub& ir) {
    return has_hrtf_ir(ir) ? ir.length : 0;
bool HrtfIrPreflight::can_use_convolution() const {
    return has_samples && length_ok && !is_empty;

bool HrtfIrPreflight::should_fallback_to_ild_itd() const {
    return is_empty;

bool HrtfIrPreflight::ready_for_stub() const {
    return can_use_convolution() || should_fallback_to_ild_itd();

    preflight.has_samples = ir.samples != nullptr;
    preflight.length_ok = ir.length > 0;
    preflight.is_empty = is_empty_hrtf_ir(ir);
    preflight.ir_length = ir.length;
    HrtfIrPreflight result;
    result.null_samples = ir.samples == nullptr;
    result.zero_length = ir.length == 0;
    result.length = ir.length;
    result.empty_ir = is_empty_hrtf_ir(ir);
    return result;

bool can_use_hrtf_convolution(const HrtfIrStub& ir) {
    return preflight_hrtf_ir(ir).can_use_convolution();

    HrtfIrPreflight preflight;
    preflight.null_samples = ir.samples == nullptr;
    preflight.zero_length = ir.length == 0;
    preflight.empty_ir = is_empty_hrtf_ir(ir);
    preflight.has_samples = has_hrtf_ir(ir);

bool can_use_hrtf_ir_for_convolution(const HrtfIrStub& ir) {
    return preflight_hrtf_ir(ir).canUseConvolution();

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    preflight.malformedIr = is_nonnull_zero_length_hrtf_ir(ir);
    preflight.hasValidIr = has_hrtf_ir(ir);
    return preflight;
}

    HrtfIrPreflight out;
    out.null_samples = ir.samples == nullptr;
    out.zero_length = ir.length == 0;
    out.malformed_ir = is_nonnull_zero_length_hrtf_ir(ir);
    return out;

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    default:
        return "Unknown";

bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& outReason) {
    if (has_hrtf_ir(ir)) {
        outReason = HrtfIrRejectReason::None;
        return true;

    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        outReason = HrtfIrRejectReason::ZeroLength;
        return false;
    if (ir.samples == nullptr && ir.length > 0) {
        outReason = HrtfIrRejectReason::NullSamples;

    outReason = HrtfIrRejectReason::EmptyIr;

bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    HrtfIrRejectReason localReason = HrtfIrRejectReason::None;
    const bool ok = try_preflight_hrtf_ir_convolution(ir, localReason);
    if (reason != nullptr) {
        *reason = localReason;
    return ok;

bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrPreflightRejectReason* reason) {
    if (ir.samples == nullptr) {
            *reason = HrtfIrPreflightRejectReason::NullSamples;
    if (ir.length == 0) {
            *reason = HrtfIrPreflightRejectReason::ZeroLength;
        *reason = HrtfIrPreflightRejectReason::None;

bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir) {
    return try_preflight_hrtf_ir_convolution(ir, nullptr);

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
        return HrtfIrRejectReason::None;
        return HrtfIrRejectReason::NullSamples;
        return HrtfIrRejectReason::ZeroLength;

    const HrtfIrRejectReason reason = hrtf_ir_reject_reason(ir);
    if (reason == HrtfIrRejectReason::None) {
    preflight.reason = reason;
    preflight.rejected = true;

bool is_malformed_hrtf_ir(const HrtfIrStub& ir) {
    return is_nonnull_zero_length_hrtf_ir(ir)
        || (ir.samples == nullptr && ir.length > 0);

    return should_skip_hrtf_convolution(ir);


HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {

namespace {

HrtfIrPreflight make_hrtf_ir_preflight(const HrtfIrStub& ir) {
    HrtfIrPreflight preflight{};

    preflight.empty_ir = true;
    preflight.reason = classify_hrtf_ir_reject(ir);
    preflight.malformed = is_nonnull_zero_length_hrtf_ir(ir);

} // namespace

    return make_hrtf_ir_preflight(ir);

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrPreflight preflight = make_hrtf_ir_preflight(ir);
        *reason = preflight.reason;
    return preflight.can_convolve();

        preflight.reject = HrtfIrPreflightReject::NullSamples;
        preflight.reject = HrtfIrPreflightReject::ZeroLength;
    preflight.ready = true;
    preflight.skipConvolution = false;
    preflight.reject = HrtfIrPreflightReject::None;

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflightReject* reject) {
    const HrtfIrPreflight preflight = preflight_hrtf_ir(ir);
    if (reject != nullptr) {
        *reject = preflight.reject;

HrtfIrSkipReason classify_hrtf_ir_skip(const HrtfIrStub& ir) {
        return HrtfIrSkipReason::NullSamples;
        return HrtfIrSkipReason::ZeroLength;
    return HrtfIrSkipReason::None;

bool hrtf_ir_skip_reason_is_blocking(HrtfIrSkipReason reason) {
    return reason != HrtfIrSkipReason::None;

bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrSkipReason* reason) {
    const HrtfIrSkipReason skip = classify_hrtf_ir_skip(ir);
        *reason = skip;
    return !hrtf_ir_skip_reason_is_blocking(skip);

HrtfIrPreflight preflight_hrtf_ir_stub(const HrtfIrStub& ir) {
    preflight.reason = classify_hrtf_ir_skip(ir);
    preflight.skipped = hrtf_ir_skip_reason_is_blocking(preflight.reason);

    case HrtfIrRejectReason::Empty:
        return "Empty";
    case HrtfIrRejectReason::Malformed:
        return "Malformed";

        return HrtfIrRejectReason::Malformed;
    if (is_empty_hrtf_ir(ir)) {
        return HrtfIrRejectReason::Empty;

    preflight.rejectReason = classify_hrtf_ir_reject(ir);

    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
        *reason = reject;
    return reject == HrtfIrRejectReason::None;

    preflight.empty = is_empty_hrtf_ir(ir);

    if (preflight.malformed || preflight.zero_length) {
        preflight.reason = HrtfIrRejectReason::ZeroLength;
    } else if (preflight.null_samples) {
        preflight.reason = HrtfIrRejectReason::NullSamples;

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return preflight_hrtf_ir(ir).reason == expected;

    preflight.malformed_stub = is_nonnull_zero_length_hrtf_ir(ir);
    preflight.has_valid_ir = has_hrtf_ir(ir);

bool can_convolve_hrtf_ir(const HrtfIrStub& ir) {
    return preflight_hrtf_ir(ir).can_convolve();

EmptyHrtfIrPreflight preflightEmptyHrtfIr(const HrtfIrStub& ir) {
    EmptyHrtfIrPreflight preflight;
    preflight.nullSamples = ir.samples == nullptr;
    preflight.zeroLength = ir.length == 0;
    preflight.nonnullZeroLength = is_nonnull_zero_length_hrtf_ir(ir);

bool canUseHrtfIr(const HrtfIrStub& ir) {
    return preflightEmptyHrtfIr(ir).hasIr();

    result.malformed = is_nonnull_zero_length_hrtf_ir(ir);






















HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    if (should_skip_hrtf_pan(hrtf_enabled, rel_listener)) {
    if (should_bypass_hrtf_pan(hrtf_enabled, rel_listener)) {
                                  const Vec3& rel_listener) {
        return HrtfPanPath::Bypass;
    if (!should_skip_hrtf_convolution(ir)) {
        return HrtfPanPath::Convolution;
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir) {

bool should_fallback_hrtf_to_ild_itd_stub(const HrtfIrStub& ir) {

bool should_fallback_hrtf_to_ild_itd_stub(bool hrtf_enabled, const HrtfIrStub& ir,
    return should_apply_hrtf_pan(hrtf_enabled, rel_listener)
        && should_fallback_hrtf_to_ild_itd_stub(ir);

bool should_use_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir,
        && !should_skip_hrtf_ir_convolution(ir);

    if (should_skip_hrtf_ir_convolution(ir)) {
        return HrtfPanPath::IldItdStub;

bool should_apply_hrtf_ir_convolution(const HrtfIrStub& ir) {
    return !should_skip_hrtf_ir_convolution(ir);

const char* hrtf_pan_reject_reason_label(HrtfPanRejectReason reason) {
    case HrtfPanRejectReason::None:
    case HrtfPanRejectReason::Disabled:
        return "Disabled";
    case HrtfPanRejectReason::CoLocated:

bool HrtfPanPreflight::can_apply_spatial_pan() const {
    return is_spatial_hrtf_pan_path(path);

bool HrtfPanPreflight::skip_convolution() const {
    return !has_valid_ir;

bool HrtfPanPreflight::ready_for_stub_mix() const {
    return reject_reason == HrtfPanRejectReason::None || path == HrtfPanPath::Bypass;

bool HrtfAttenuationCouplingPreflight::can_narrow_image() const {
    return !skip_coupling;

bool HrtfAttenuationCouplingPreflight::ready_for_coupling() const {
    return can_narrow_image();

bool try_resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                               HrtfPanPath& out_path, HrtfPanRejectReason& out_reason) {
    out_reason = HrtfPanRejectReason::None;
        out_path = HrtfPanPath::Bypass;
            out_reason = HrtfPanRejectReason::Disabled;
        } else if (is_co_located_hrtf_source(rel_listener)) {
            out_reason = HrtfPanRejectReason::CoLocated;
    if (should_apply_hrtf_ir_convolution(ir)) {
        out_path = HrtfPanPath::Convolution;
    } else {
        out_path = HrtfPanPath::IldItdStub;

HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfPanPreflight preflight;
    preflight.hrtf_enabled = hrtf_enabled;
    preflight.co_located = is_co_located_hrtf_source(rel_listener);
    preflight.has_valid_ir = should_apply_hrtf_ir_convolution(ir);
    try_resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener, preflight.path,
                              preflight.reject_reason);

HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfPanRejectReason reason = HrtfPanRejectReason::None;
    try_resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener, path, reason);
    return path;
HrtfIrStub preflight_hrtf_ir(const HrtfIrStub& ir) {
    return make_hrtf_ir_stub(ir.samples, ir.length);

    return !should_use_hrtf_ir(ir);

    return is_empty_hrtf_ir(ir);
    HrtfPanPathPreflight result;
    result.hrtf_disabled = !hrtf_enabled;
    result.co_located = is_co_located_hrtf_source(rel_listener);
    result.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfPanPathPreflight out;
    out.hrtf_disabled = !hrtf_enabled;
    out.co_located = is_co_located_hrtf_source(rel_listener);
    out.empty_ir = is_empty_hrtf_ir(ir);
    out.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (out.hrtf_disabled) {
        out.skip_reason = HrtfPanSkipReason::HrtfDisabled;
    } else if (out.co_located) {
        out.skip_reason = HrtfPanSkipReason::CoLocated;
        out.skip_reason = HrtfPanSkipReason::None;


    HrtfPanPathPreflight preflight;
    if (!hrtf_enabled) {
        preflight.path = HrtfPanPath::Bypass;
        preflight.reject = HrtfPanPathPreflightReject::Disabled;
    if (rel_listener.length() < kHrtfCoLocatedEpsilon) {
        preflight.reject = HrtfPanPathPreflightReject::CoLocated;

    preflight.skipSpatialPan = false;
    preflight.reject = HrtfPanPathPreflightReject::None;
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.skipConvolution = preflight.path != HrtfPanPath::Convolution;
    preflight.reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathPreflightReject* reject) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reject != nullptr) {
        *reject = preflight.reject;
    }
    return preflight.is_spatial();

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.hrtf_disabled = !hrtf_enabled;
    preflight.co_located = is_co_located_hrtf_source(rel_listener);
    preflight.empty_ir = is_empty_hrtf_ir(ir);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

    if (preflight.hrtf_disabled) {
        preflight.reject_reason = HrtfPanPathRejectReason::Disabled;
    } else if (preflight.co_located) {
        preflight.reject_reason = HrtfPanPathRejectReason::CoLocated;
    return preflight;
bool can_convolve_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.can_convolve();

bool should_skip_hrtf_pan_path_preflight(const HrtfPanPathPreflight& preflight) {
    return preflight.should_skip();

bool uses_ild_itd_stub_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.uses_ild_itd_stub();
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathPreflight& preflight) {
    preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfPanPathRejectReason* reason) {
    const HrtfPanPathRejectReason reject = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (reason != nullptr) {
        *reason = reject;
    return reject == HrtfPanPathRejectReason::None;

bool preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

                                  HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener, &reason);
    }

                              HrtfPanPathRejectReason* reason) {

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,











bool can_apply_spatial_hrtf_pan(const HrtfPanPathPreflight& preflight) {
    return preflight.can_spatial_pan();

bool can_apply_spatial_hrtf_pan(const HrtfPanPathPreflight& preflight) {

bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener) != HrtfPanPathRejectReason::None;

bool can_convolve_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.can_convolve();
}

bool should_skip_hrtf_pan_path_preflight(const HrtfPanPathPreflight& preflight) {
    return preflight.should_skip();
}

bool uses_ild_itd_stub_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.uses_ild_itd_stub();
}

HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    if (!should_apply_hrtf_pan(hrtf_enabled, rel_listener)) {
        return HrtfPanPath::Bypass;
    }
    if (can_convolve_hrtf_ir(preflight_hrtf_ir(ir))) {
        return HrtfPanPath::Convolution;
    }
    return HrtfPanPath::IldItdStub;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener).reject_reason == expected;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight;
    preflight.hrtf_disabled = !hrtf_enabled;
    preflight.co_located = is_co_located_hrtf_source(rel_listener);
    preflight.empty_ir = is_empty_hrtf_ir(ir);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener).can_apply_spatial_pan();
}

bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener).can_apply_spatial_pan();
}

HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight;
    preflight.hrtfDisabled = !hrtf_enabled;
    preflight.coLocated = is_co_located_hrtf_source(rel_listener);
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight;
}

HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflightHrtfPanPath(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool canApplyHrtfPanPath(HrtfPanPath path) {
    return is_spatial_hrtf_pan_path(path);
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight result;
    result.hrtf_disabled = !hrtf_enabled;
    result.co_located = is_co_located_hrtf_source(rel_listener);
    result.empty_ir = is_empty_hrtf_ir(ir);
    result.has_valid_ir = has_hrtf_ir(ir);
    result.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return result;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool can_spatialize_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener).can_spatialize();
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight;
    if (!hrtf_enabled) {
        preflight.path = HrtfPanPath::Bypass;
        preflight.reject = HrtfPanPathPreflightReject::Disabled;
        return preflight;
    }
    if (rel_listener.length() < kHrtfCoLocatedEpsilon) {
        preflight.path = HrtfPanPath::Bypass;
        preflight.reject = HrtfPanPathPreflightReject::CoLocated;
        return preflight;
    }

    preflight.skipSpatialPan = false;
    preflight.reject = HrtfPanPathPreflightReject::None;
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.skipConvolution = preflight.path != HrtfPanPath::Convolution;
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathPreflightReject* reject) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reject != nullptr) {
        *reject = preflight.reject;
    }
    return preflight.is_spatial();
}

bool is_hrtf_pan_bypassed(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);


HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight;
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.skipped = should_skip_hrtf_pan_path(preflight.path);
    if (!preflight.skipped) {
        return preflight;
    }
    if (!hrtf_enabled) {
        preflight.skip_reason = HrtfPanPathSkipReason::Disabled;
    } else if (is_co_located_hrtf_source(rel_listener)) {
        preflight.skip_reason = HrtfPanPathSkipReason::CoLocated;
    }
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool should_fallback_to_ild_itd_pan_path(HrtfPanPath path) {
    return hrtf_pan_path_uses_ild_itd_stub(path);
}

bool is_spatial_hrtf_pan_path(HrtfPanPath path) {
    return path != HrtfPanPath::Bypass;

bool hrtf_pan_path_uses_convolution(HrtfPanPath path) {
    return path == HrtfPanPath::Convolution;

bool hrtf_pan_path_uses_ild_itd_stub(HrtfPanPath path) {
    return path == HrtfPanPath::IldItdStub;

bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path) {
    return hrtf_pan_path_uses_ild_itd_stub(path);
}

HrtfPanPathSkipReason classify_hrtf_pan_path_skip(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathSkipReason::Disabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathSkipReason::CoLocated;
    }
    return HrtfPanPathSkipReason::None;
}

bool hrtf_pan_path_skip_reason_is_blocking(HrtfPanPathSkipReason reason) {
    return reason != HrtfPanPathSkipReason::None;
}

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathSkipReason* reason) {
    const HrtfPanPathSkipReason skip = classify_hrtf_pan_path_skip(hrtf_enabled, rel_listener);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !hrtf_pan_path_skip_reason_is_blocking(skip);
}

HrtfPanPathPreflight preflight_hrtf_pan_path_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.skip_reason = classify_hrtf_pan_path_skip(hrtf_enabled, rel_listener);
    preflight.skipped = hrtf_pan_path_skip_reason_is_blocking(preflight.skip_reason);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight;
}

bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path) {
    return hrtf_pan_path_uses_ild_itd_stub(path);
}

HrtfPanPathSkipReason classify_hrtf_pan_path_skip(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathSkipReason::Disabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathSkipReason::CoLocated;
    }
    return HrtfPanPathSkipReason::None;
}

bool hrtf_pan_path_skip_reason_is_blocking(HrtfPanPathSkipReason reason) {
    return reason != HrtfPanPathSkipReason::None;
}

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathSkipReason* reason) {
    const HrtfPanPathSkipReason skip = classify_hrtf_pan_path_skip(hrtf_enabled, rel_listener);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !hrtf_pan_path_skip_reason_is_blocking(skip);
}

HrtfPanPathPreflight preflight_hrtf_pan_path_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.skip_reason = classify_hrtf_pan_path_skip(hrtf_enabled, rel_listener);
    preflight.skipped = hrtf_pan_path_skip_reason_is_blocking(preflight.skip_reason);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight;
}

bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path) {
    return hrtf_pan_path_uses_ild_itd_stub(path);
}

HrtfPanPathSkipReason classify_hrtf_pan_path_skip(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathSkipReason::Disabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathSkipReason::CoLocated;
    }
    return HrtfPanPathSkipReason::None;
}

bool hrtf_pan_path_skip_reason_is_blocking(HrtfPanPathSkipReason reason) {
    return reason != HrtfPanPathSkipReason::None;
}

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathSkipReason* reason) {
    const HrtfPanPathSkipReason skip = classify_hrtf_pan_path_skip(hrtf_enabled, rel_listener);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !hrtf_pan_path_skip_reason_is_blocking(skip);
}

HrtfPanPathPreflight preflight_hrtf_pan_path_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.skip_reason = classify_hrtf_pan_path_skip(hrtf_enabled, rel_listener);
    preflight.skipped = hrtf_pan_path_skip_reason_is_blocking(preflight.skip_reason);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight;
}

bool is_hrtf_pan_path_bypass(HrtfPanPath path) {
    return path == HrtfPanPath::Bypass;

bool is_bypass_hrtf_pan_path(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);

bool should_skip_hrtf_spatial_pan(HrtfPanPath path) {

    return should_skip_hrtf_convolution(ir);

    preflight.canConvolution = !should_skip_hrtf_convolution(ir);
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight;
    preflight.hrtfDisabled = !hrtf_enabled;
    preflight.coLocated = is_co_located_hrtf_source(rel_listener);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.bypassed = should_skip_hrtf_pan_path(preflight.path);
    preflight.spatial = is_spatial_hrtf_pan_path(preflight.path);
    preflight.usesConvolution = hrtf_pan_path_uses_convolution(preflight.path);
    preflight.usesIldItdStub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "None";
    case HrtfPanPathRejectReason::Disabled:
        return "Disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";
    default:
        return "Unknown";
    }
}

bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPath& outPath, HrtfPanPathRejectReason& outReason) {
    outPath = resolve_hrtf_pan_path(hrtf_enabled, rel_listener);
    if (!hrtf_enabled) {
        outReason = HrtfPanPathRejectReason::Disabled;
        return false;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        outReason = HrtfPanPathRejectReason::CoLocated;
        return false;
    }

    outReason = HrtfPanPathRejectReason::None;
    return true;
}

bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                HrtfPanPath* out_path, HrtfPanPathRejectReason* reason) {
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfPanPathRejectReason localReason = HrtfPanPathRejectReason::None;
    const bool ok = try_preflight_hrtf_spatial_pan(hrtf_enabled, rel_listener, path, localReason);
    if (out_path != nullptr) {
        *out_path = path;
    }
    if (reason != nullptr) {
        *reason = localReason;
    }
    return ok;
}

bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                const Vec3& rel_listener, HrtfPanPath* out_path,
                                HrtfPanPathRejectReason* reason) {
    HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (out_path != nullptr) {
        *out_path = path;
    }

    if (!hrtf_enabled) {
        if (reason != nullptr) {
            *reason = HrtfPanPathRejectReason::Disabled;
        }
        return false;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        if (reason != nullptr) {
            *reason = HrtfPanPathRejectReason::CoLocated;
        }
        return false;
    }

    if (reason != nullptr) {
        *reason = HrtfPanPathRejectReason::None;
    }
    return true;
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "None";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";
    default:
        return "Unknown";
    }
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled,
                                                      const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

namespace {

HrtfPanPathPreflight make_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.bypassed = is_hrtf_pan_path_bypass(preflight.path);
    preflight.uses_convolution = hrtf_pan_path_uses_convolution(preflight.path);
    preflight.uses_ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    if (preflight.bypassed) {
        preflight.reject_reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    }
    return preflight;
}

} // namespace

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    return make_hrtf_pan_path_preflight(hrtf_enabled, ir, rel_listener);
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return make_hrtf_pan_path_preflight(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight =
        make_hrtf_pan_path_preflight(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = preflight.reject_reason;
    }
    return preflight.can_spatial_pan();
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "None";
    case HrtfPanPathRejectReason::Disabled:
        return "Disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";
    default:
        return "Unknown";
    }
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::Disabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.rejectReason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    preflight.bypassed = is_hrtf_pan_path_bypass(preflight.path);
    preflight.spatial = is_spatial_hrtf_pan_path(preflight.path);
    preflight.usesConvolution = hrtf_pan_path_uses_convolution(preflight.path);
    preflight.usesIldItdStub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathRejectReason* reason) {
    const HrtfPanPathRejectReason reject = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfPanPathRejectReason::None;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight;
    preflight.hrtf_disabled = !hrtf_enabled;
    preflight.co_located = is_co_located_hrtf_source(rel_listener);
    preflight.empty_ir = is_empty_hrtf_ir(ir);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool can_apply_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener).can_apply_spatial_pan();
}

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "None";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";
    default:
        return "Unknown";
    }
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled,
                                                      const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

namespace {

HrtfPanPathPreflight make_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.bypassed = is_hrtf_pan_path_bypass(preflight.path);
    preflight.uses_convolution = hrtf_pan_path_uses_convolution(preflight.path);
    preflight.uses_ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    if (preflight.bypassed) {
        preflight.reject_reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    }
    return preflight;
}

} // namespace

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    return make_hrtf_pan_path_preflight(hrtf_enabled, ir, rel_listener);
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return make_hrtf_pan_path_preflight(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight =
        make_hrtf_pan_path_preflight(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = preflight.reject_reason;
    }
    return preflight.can_spatial_pan();
}

bool is_co_located_hrtf_source(const Vec3& rel_listener) {
    return rel_listener.length() < kHrtfCoLocatedEpsilon;
}

bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !hrtf_enabled || is_co_located_hrtf_source(rel_listener);
}

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener);
}

bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan(hrtf_enabled, rel_listener);
}

bool is_hrtf_pan_bypassed(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
    return path == HrtfPanPath::Bypass;
bool is_hrtf_pan_path_bypass(HrtfPanPath path) {
}

bool is_bypass_hrtf_pan_path(HrtfPanPath path) {

bool should_skip_hrtf_spatial_pan(HrtfPanPath path) {

bool should_apply_hrtf_spatial_pan(HrtfPanPath path) {
    return !should_skip_hrtf_spatial_pan(path);


bool should_skip_hrtf_pan_path(HrtfPanPath path) {
    return is_hrtf_pan_bypassed(path);
HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight result;
    result.hrtf_disabled = !hrtf_enabled;
    result.co_located = is_co_located_hrtf_source(rel_listener);
    result.empty_ir = is_empty_hrtf_ir(ir);
    result.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return result;
    HrtfPanPathPreflight preflight;
    HrtfPanPathPreflight preflight{};
    preflight.hrtf_disabled = !hrtf_enabled;
    preflight.co_located = is_co_located_hrtf_source(rel_listener);
    preflight.empty_ir = is_empty_hrtf_ir(ir);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.bypass = is_hrtf_pan_path_bypass(preflight.path);
    preflight.uses_convolution = hrtf_pan_path_uses_convolution(preflight.path);
    preflight.uses_ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    return preflight;
    preflight.ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    preflight.convolution = hrtf_pan_path_uses_convolution(preflight.path);
    result.has_valid_ir = has_hrtf_ir(ir);

HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    if (should_skip_hrtf_pan(hrtf_enabled, rel_listener)) {
        return HrtfPanPath::Bypass;
    if (!should_skip_hrtf_convolution(ir)) {
        return HrtfPanPath::Convolution;
    return HrtfPanPath::IldItdStub;

HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return resolve_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool should_fallback_hrtf_to_ild_itd_stub(bool hrtf_enabled, const HrtfIrStub& ir,
    return resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener) == HrtfPanPath::IldItdStub;

bool should_use_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir,
    return resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener) == HrtfPanPath::Convolution;

bool HrtfPanPathPreflight::can_spatial_pan() const {
    return should_apply_hrtf_spatial_pan(path);

bool HrtfPanPathPreflight::should_skip_pan() const {
    return should_skip_hrtf_pan_path(path);

    preflight.hrtfDisabled = !hrtf_enabled;
    preflight.coLocated = is_co_located_hrtf_source(rel_listener);
    preflight.emptyIr = is_empty_hrtf_ir(ir);

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool is_spatial_hrtf_pan_path(HrtfPanPath path) {
    return path != HrtfPanPath::Bypass;

bool hrtf_pan_path_uses_convolution(HrtfPanPath path) {
    return path == HrtfPanPath::Convolution;

bool is_convolution_hrtf_pan_path(HrtfPanPath path) {
    return hrtf_pan_path_uses_convolution(path);

bool hrtf_pan_path_uses_ild_itd_stub(HrtfPanPath path) {
    return path == HrtfPanPath::IldItdStub;




bool is_co_located_hrtf_source(const Vec3& rel_listener) {
    return rel_listener.length() < kHrtfCoLocatedEpsilon;

bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !hrtf_enabled || is_co_located_hrtf_source(rel_listener);



bool HrtfPanPathPreflight::can_apply_spatial_pan() const {
    return !skip_spatial_pan;

bool HrtfPanPathPreflight::should_bypass() const {
    return skip_spatial_pan;

bool HrtfPanPathPreflight::ready_for_stub() const {
    return can_apply_spatial_pan() || should_bypass();

    preflight.ir = preflight_hrtf_ir(ir);
    preflight.hrtf_enabled = hrtf_enabled;
    preflight.skip_spatial_pan = should_skip_hrtf_spatial_pan(preflight.path);



HrtfPanPath preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    if (should_fallback_to_ild_itd_stub(preflight_hrtf_ir(ir))) {

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !should_skip_hrtf_pan(hrtf_enabled, rel_listener);
    return hrtf_enabled && !is_co_located_hrtf_source(rel_listener);

bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan(hrtf_enabled, rel_listener);
bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path) {


    result.bypass = is_hrtf_pan_path_bypass(result.path);
    result.spatial = is_spatial_hrtf_pan_path(result.path);
    result.uses_convolution = hrtf_pan_path_uses_convolution(result.path);
    result.uses_ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(result.path);


bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener).can_apply_spatial_pan();

    result.ir_preflight = preflight_hrtf_ir(ir);
    result.empty_ir = result.ir_preflight.is_empty();


bool can_apply_hrtf_spatial_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPath preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {

    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener);

    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener) == HrtfPanPath::Convolution;
bool can_apply_hrtf_spatial_pan(HrtfPanPath path) {
    return is_spatial_hrtf_pan_path(path);




bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPreflightRejectReason* reason) {
    if (!hrtf_enabled) {
        if (reason != nullptr) {
            *reason = HrtfPanPreflightRejectReason::Disabled;
        return false;
    if (is_co_located_hrtf_source(rel_listener)) {
            *reason = HrtfPanPreflightRejectReason::CoLocated;
        *reason = HrtfPanPreflightRejectReason::None;
    return true;

bool preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return try_preflight_hrtf_pan_path(hrtf_enabled, rel_listener, nullptr);

bool preflight_hrtf_pan_path(HrtfPanPath path) {
}

namespace {

float clamp_unit(float value) {
    return std::clamp(value, -1.f, 1.f);
}

PanLawGains sample_equal_power_pan(float pan) {
    PanLawGains gains;
    gains.left = std::sqrt(0.5f * (1.f - pan));
    gains.right = std::sqrt(0.5f * (1.f + pan));
    return gains;
}

PanLawGains sample_linear_pan(float pan) {
    PanLawGains gains;
    gains.left = 0.5f * (1.f - pan);
    gains.right = 0.5f * (1.f + pan);
    return gains;
}

} // namespace

float clamp_pan_position(float pan) {
    return std::clamp(pan, -1.f, 1.f);
}

float compute_pan_position_from_azimuth(float azimuth, float max_ild_pan) {
    return clamp_pan_position(std::sin(azimuth) * max_ild_pan);
}

float compute_itd_from_azimuth(float azimuth, const BinauralPanParams& params) {
    return params.max_itd_seconds * std::sin(azimuth);
}

float compute_elevation_factor(float elevation, const BinauralPanParams& params) {
    return 1.f - params.elevation_rolloff * std::fabs(std::sin(elevation));
}

PanLawGains sample_pan_law(float pan, PanLaw law) {
    const float clamped = clamp_pan_position(pan);
    switch (law) {
    case PanLaw::Linear:
        return sample_linear_pan(clamped);
    case PanLaw::EqualPower:
    default:
        return sample_equal_power_pan(clamped);
    }
}

BinauralPanAngles compute_binaural_angles(const Vec3& rel_listener) {
    BinauralPanAngles angles;
    const float distance = rel_listener.length();
    if (distance < kHrtfCoLocatedEpsilon) {
        return angles;
    }

    angles.azimuth = std::atan2(rel_listener.x, -rel_listener.z);
    angles.elevation = std::asin(clamp_unit(rel_listener.y / distance));
    return angles;
}

BinauralPanAngles compute_binaural_angles(const Vec3& world_relative, const ListenerBasis& basis) {
    return compute_binaural_angles(to_listener_space(world_relative, basis));
}

BinauralPanGains compute_binaural_pan_gains(const BinauralPanAngles& angles,
                                             const BinauralPanParams& params) {
    BinauralPanGains gains;

    const float pan = compute_pan_position_from_azimuth(angles.azimuth, params.max_ild_pan);
    const PanLawGains pan_gains = sample_pan_law(pan, params.pan_law);
    gains.left = pan_gains.left;
    gains.right = pan_gains.right;
    gains.itd_seconds = compute_itd_from_azimuth(angles.azimuth, params);

    const float elevation_factor = compute_elevation_factor(angles.elevation, params);
    gains.left *= elevation_factor;
    gains.right *= elevation_factor;

    clamp_binaural_pan_gains(gains);
    return gains;
}

BinauralPanGains compute_binaural_pan_gains(const Vec3& rel_listener,
                                             const BinauralPanParams& params) {
    return compute_binaural_pan_gains(compute_binaural_angles(rel_listener), params);
}

BinauralPanGains compute_binaural_pan_gains(const Vec3& world_relative, const ListenerBasis& basis,
                                             const BinauralPanParams& params) {
    return compute_binaural_pan_gains(compute_binaural_angles(world_relative, basis), params);
}

ListenerBasis compute_listener_basis(const AudioListener& listener) {
    return make_listener_basis_safe(listener.forward, listener.up);
}

BinauralPanAngles compute_binaural_angles(const AudioListener& listener, const Vec3& source_position) {
    const Vec3 world_relative = source_position - listener.position;
    return compute_binaural_angles(world_relative, compute_listener_basis(listener));
}

BinauralPanGains compute_binaural_pan_gains(const AudioListener& listener, const Vec3& source_position,
                                             const BinauralPanParams& params) {
    return compute_binaural_pan_gains(compute_binaural_angles(listener, source_position), params);
}

BinauralPanGains make_centre_binaural_pan_gains() {
    BinauralPanGains gains;
    gains.left = 0.5f;
    gains.right = 0.5f;
    gains.itd_seconds = 0.f;
    return gains;
}

float compute_pan_spread(const BinauralPanGains& gains) {
    return std::fabs(gains.left - gains.right);
}

bool is_centre_panned(const BinauralPanGains& gains, float epsilon) {
    return compute_pan_spread(gains) <= epsilon;
}

float compute_binaural_pan_energy(const BinauralPanGains& gains) {
    return gains.left * gains.left + gains.right * gains.right;
}

bool has_nonzero_itd(const BinauralPanGains& gains, float epsilon) {
    return std::fabs(gains.itd_seconds) > epsilon;
}

void scale_binaural_pan_gains(BinauralPanGains& gains, float scale) {
    gains.left *= scale;
    gains.right *= scale;
    clamp_binaural_pan_gains(gains);
}

void apply_binaural_pan_to_sample(float mono, const BinauralPanGains& pan, float attenuation,
                                  float& left, float& right) {
    const float scaled = mono * attenuation;
    left += scaled * pan.left;
    right += scaled * pan.right;
}

void apply_binaural_pan_to_sample_for_path(HrtfPanPath path, float mono,
                                           const BinauralPanGains& pan, float attenuation,
                                           float& left, float& right) {
    if (should_skip_hrtf_spatial_pan(path)) {
        apply_centre_binaural_pan_to_sample(mono, attenuation, left, right);
        return;
    }
    apply_binaural_pan_to_sample(mono, pan, attenuation, left, right);
}

void apply_centre_binaural_pan_to_sample(float mono, float attenuation, float& left, float& right) {
    const float scaled = mono * attenuation;
    left += scaled;
    right += scaled;
}

void apply_guarded_binaural_pan_to_sample(bool hrtf_enabled, const Vec3& rel_listener, float mono,
                                          const BinauralPanGains& pan, float attenuation,
                                          float& left, float& right) {
    apply_binaural_pan_to_sample_for_path(resolve_hrtf_pan_path(hrtf_enabled, rel_listener), mono,
                                          pan, attenuation, left, right);
}

void apply_guarded_binaural_pan_to_sample(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float mono,
    apply_binaural_pan_to_sample_for_path(resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener),
                                          mono, pan, attenuation, left, right);
void apply_binaural_pan_for_path_to_sample(HrtfPanPath path, float mono, const BinauralPanGains& pan,
                                           float attenuation, float& left, float& right) {
    if (should_skip_hrtf_spatial_pan(path)) {
        apply_centre_binaural_pan_to_sample(mono, attenuation, left, right);
        return;
    apply_binaural_pan_to_sample(mono, pan, attenuation, left, right);
}

BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, rel_listener, 1.f, 1.f, {}, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, {}, params);
    return compute_binaural_pan_gains_for_path(resolve_hrtf_pan_path(hrtf_enabled, rel_listener),
                                               rel_listener, params);
    return compute_binaural_pan_gains_for_path(
        resolve_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener), rel_listener,
        params);
    return compute_binaural_pan_gains_from_pan_path_preflight(
        preflight_hrtf_pan_path(hrtf_enabled, rel_listener), rel_listener, params);
}

BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f, {}, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, {}, params);
    return compute_binaural_pan_gains_for_path(
        preflight_hrtf_pan_path(hrtf_enabled, preflight_hrtf_ir(ir), rel_listener), rel_listener,
        params);
    return compute_binaural_pan_gains_from_pan_path_preflight(
        preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener), rel_listener, params);
}

BinauralPanGains compute_binaural_pan_gains_for_path(HrtfPanPath path, const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    if (is_bypass_hrtf_pan_path(path)) {
    if (is_hrtf_pan_path_bypass(path)) {
    if (is_hrtf_pan_bypassed(path)) {
        return make_centre_binaural_pan_gains();
    }
    // Convolution path deferred — ILD/ITD stub until delay-line / IR wiring lands.
    return compute_binaural_pan_gains(rel_listener, params);
}

BinauralPanGains compute_binaural_pan_gains_from_pan_path_preflight(
    const HrtfPanPathPreflight& preflight, const Vec3& rel_listener,
    const BinauralPanParams& params) {
    return compute_binaural_pan_gains_for_path(preflight.path, rel_listener, params);
}

BinauralPanGains lerp_binaural_pan_gains(const BinauralPanGains& from, const BinauralPanGains& to,
                                         float t) {
    const float blend = std::clamp(t, 0.f, 1.f);
    BinauralPanGains gains;
    gains.left = from.left + (to.left - from.left) * blend;
    gains.right = from.right + (to.right - from.right) * blend;
    gains.itd_seconds = from.itd_seconds + (to.itd_seconds - from.itd_seconds) * blend;
    clamp_binaural_pan_gains(gains);
    return gains;
}

void clamp_binaural_pan_gains(BinauralPanGains& gains) {
    gains.left = std::clamp(gains.left, 0.f, 1.f);
    gains.right = std::clamp(gains.right, 0.f, 1.f);
}

float clamp_hrtf_attenuation(float attenuation) {
    return std::clamp(attenuation, 0.f, 1.f);
}

float hrtf_unity_attenuation_epsilon() {
    return kHrtfUnityAttenuationEpsilon;
}

float clamp_hrtf_attenuation_coupling_weight(float weight) {
float clamp_hrtf_occlusion_coupling_weight(float weight) {
    return std::clamp(weight, 0.f, 1.f);
}

bool is_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain) {
    return clamp_hrtf_attenuation(distance_attenuation) >= 1.f - 1e-5f
        && clamp_hrtf_attenuation(occlusion_gain) >= 1.f - 1e-5f;
    return is_unity_hrtf_distance_attenuation(distance_attenuation)
        && is_unity_hrtf_occlusion_gain(occlusion_gain);
}

bool is_unity_hrtf_distance_attenuation(float distance_attenuation) {
    return clamp_hrtf_attenuation(distance_attenuation) >= 1.f - 1e-5f;

bool is_unity_hrtf_occlusion_gain(float occlusion_gain) {
    return clamp_hrtf_attenuation(occlusion_gain) >= 1.f - 1e-5f;

bool should_skip_hrtf_attenuation_coupling(float distance_attenuation, float occlusion_gain) {
    return is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);

bool should_apply_hrtf_distance_coupling(float distance_attenuation) {
    return !is_unity_hrtf_distance_attenuation(distance_attenuation);

bool should_apply_hrtf_occlusion_coupling(float occlusion_gain) {
    return !is_unity_hrtf_occlusion_gain(occlusion_gain);
bool is_non_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain) {
    return !is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    return clamp_hrtf_attenuation(distance_attenuation) >= 1.f - kHrtfUnityAttenuationEpsilon
        && clamp_hrtf_attenuation(occlusion_gain) >= 1.f - kHrtfUnityAttenuationEpsilon;

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain) {
    return should_apply_hrtf_spatial_pan(path)
        && !is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);

bool should_skip_hrtf_attenuation_coupling_for_inputs(HrtfPanPath path, float distance_attenuation,
    return !preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain);
}


bool HrtfAttenuationCouplingPreflight::can_couple() const {
    return !bypassPath && !unitySpatialBlend;

bool HrtfAttenuationCouplingPreflight::should_skip_coupling() const {
    return !can_couple();
}

bool should_apply_hrtf_attenuation_coupling(HrtfPanPath path) {
    return is_spatial_hrtf_pan_path(path);

bool should_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                            float occlusion_gain) {
    return should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);
}

bool should_skip_hrtf_attenuation_coupling(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
}


    return !should_skip_hrtf_attenuation_coupling(path);

bool is_unity_hrtf_spatial_blend(float blend, float epsilon) {
    return blend >= 1.f - epsilon;

bool should_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain) {
    return should_apply_hrtf_attenuation_coupling(path)
        && !is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    return "Unknown";

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    return HrtfAttenuationCouplingRejectReason::None;

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;
}

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.reason =
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
    preflight.bypassPath = is_hrtf_pan_path_bypass(path);
    preflight.unityAttenuation =
        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.bypassPath || preflight.unityAttenuation;
    preflight.reason = classify_hrtf_attenuation_coupling_reject(path, distance_attenuation,
                                                                 occlusion_gain);
    preflight.skipped = preflight.reason != HrtfAttenuationCouplingRejectReason::None;
    return preflight;

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingPreflight& preflight, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    preflight = preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                    coupling, params);
    return preflight.can_narrow();
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              float occlusion_gain,
                                              HrtfAttenuationCouplingRejectReason& reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, &reason,
                                               coupling, params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              float occlusion_gain,
                                              HrtfAttenuationCouplingRejectReason& reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, &reason,
                                               coupling, params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              float occlusion_gain,
                                              HrtfAttenuationCouplingRejectReason& reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, &reason,
                                               coupling, params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              float occlusion_gain,
                                              HrtfAttenuationCouplingRejectReason& reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, &reason,
                                               coupling, params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              float occlusion_gain,
                                              HrtfAttenuationCouplingRejectReason& reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, &reason,
                                               coupling, params);
}

bool can_narrow_hrtf_spatial_image(const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.can_narrow();

bool should_skip_hrtf_attenuation_coupling_preflight(const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.should_skip();
void scale_binaural_pan_gains(BinauralPanGains& gains, float scale) {
    gains.left *= scale;
    gains.right *= scale;
    clamp_binaural_pan_gains(gains);

BinauralPanGains scale_binaural_pan_gains_copy(const BinauralPanGains& gains, float scale) {
    BinauralPanGains scaled = gains;
    scale_binaural_pan_gains(scaled, scale);
    return scaled;

BinauralStereoSample apply_binaural_pan_gains(float mono, const BinauralPanGains& pan,
                                              float output_attenuation) {
    const float atten = std::clamp(output_attenuation, 0.f, 1.f);
    BinauralStereoSample out;
    out.left = mono * atten * pan.left;
    out.right = mono * atten * pan.right;
    return out;

void accumulate_binaural_pan(float mono, const BinauralPanGains& pan, float output_attenuation,
                             float& left, float& right) {
    const BinauralStereoSample out = apply_binaural_pan_gains(mono, pan, output_attenuation);
    left += out.left;
    right += out.right;

    return is_bypass_hrtf_pan_path(path);

float clamp_hrtf_occlusion_coupling_weight(float weight) {



        && !should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain);
        && is_non_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
}

bool should_preserve_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
    return !should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);

bool should_skip_hrtf_attenuation_coupling_mapping(float distance_attenuation,
    return is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);

    HrtfAttenuationCouplingPreflight preflight;
    preflight.path = path;
    preflight.distance_attenuation = clamp_hrtf_attenuation(distance_attenuation);
    preflight.occlusion_gain = clamp_hrtf_attenuation(occlusion_gain);
    preflight.unity_attenuation =
        should_skip_hrtf_attenuation_coupling_mapping(distance_attenuation, occlusion_gain);
    preflight.skip_coupling =
        !should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);

HrtfAttenuationCouplingPreflight preflight_hrtf_coupled_pan(bool hrtf_enabled,
                                                              const HrtfIrStub& ir,
                                                              const Vec3& rel_listener,
                                                              float distance_attenuation,
    const HrtfPanPreflight pan_preflight = preflight_hrtf_pan(hrtf_enabled, ir, rel_listener);
    return preflight_hrtf_attenuation_coupling(pan_preflight.path, distance_attenuation,
                                               occlusion_gain);

    return preflight_hrtf_coupled_pan(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                      distance_attenuation, occlusion_gain);

    HrtfAttenuationCouplingPreflight result;
    result.path = path;
    result.distance_attenuation = clamp_hrtf_attenuation(distance_attenuation);
    result.occlusion_gain = clamp_hrtf_attenuation(occlusion_gain);
    result.bypass_path = is_hrtf_pan_path_bypass(path);
    result.unity_attenuation = is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    result.would_narrow = should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);
    result.skipped = !result.would_narrow;
    return result;

bool can_apply_hrtf_attenuation_coupling_narrowing(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain)
        .can_apply();

    result.spatial_path = is_spatial_hrtf_pan_path(path);

    preflight.bypass_path = is_hrtf_pan_path_bypass(path);
    preflight.unity_attenuation = is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.skipped = !should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);

    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    const HrtfPanPathPreflight pan_preflight =
        preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(HrtfPanPath path,
    preflight.will_narrow =
        should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);


HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfSpatialPanPreflight result;
    result.ir = preflight_hrtf_ir(ir);
    result.pan = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    result.coupling =
        preflight_hrtf_attenuation_coupling(result.pan.path, distance_attenuation, occlusion_gain);

    result.clamped_distance_attenuation = clamp_hrtf_attenuation(distance_attenuation);
    result.clamped_occlusion_gain = clamp_hrtf_attenuation(occlusion_gain);
    result.unity_attenuation =
    result.will_narrow = should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);

bool should_narrow_hrtf_spatial_image_preflight(HrtfPanPath path, float distance_attenuation,
        .should_narrow_spatial_image();
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain);


bool can_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
        .canApplyCoupling();

    result.skipped = !should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);

    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    float distance_attenuation, float occlusion_gain) {
    return preflight_hrtf_attenuation_coupling(
        resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener), distance_attenuation,

    HrtfAttenuationCouplingPreflight out;
    out.path = path;
    out.bypass_pan_path = should_skip_hrtf_pan_path(path);
    out.unity_attenuation = is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    out.spatial_blend =
    out.unity_spatial_blend = is_unity_hrtf_spatial_blend(out.spatial_blend);

    float occlusion_gain, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfPanPathPreflight pan = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight_hrtf_attenuation_coupling(pan.path, distance_attenuation, occlusion_gain,
                                               coupling, params);

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    default:

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             HrtfAttenuationCouplingRejectReason& outReason) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        outReason = HrtfAttenuationCouplingRejectReason::BypassPath;
        return false;
        outReason = HrtfAttenuationCouplingRejectReason::UnityAttenuation;

    outReason = HrtfAttenuationCouplingRejectReason::None;
    return true;

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         HrtfAttenuationCouplingRejectReason* reason) {
    HrtfAttenuationCouplingRejectReason localReason = HrtfAttenuationCouplingRejectReason::None;
    const bool ok =
        try_preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                localReason);
    if (reason != nullptr) {
        *reason = localReason;
    return ok;
                                             HrtfAttenuationCouplingPreflightRejectReason* reason) {
    if (!preflight_hrtf_pan_path(path)) {
            *reason = HrtfAttenuationCouplingPreflightRejectReason::BypassPath;
            *reason = HrtfAttenuationCouplingPreflightRejectReason::UnityAttenuation;
        *reason = HrtfAttenuationCouplingPreflightRejectReason::None;

    return try_preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                   nullptr);

}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::PanBypassed:
        return "PanBypassed";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    default:
        return "Unknown";
    }
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        return HrtfAttenuationCouplingRejectReason::PanBypassed;
    }
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

namespace {

HrtfAttenuationCouplingPreflight make_hrtf_attenuation_coupling_preflight(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.bypass_path = should_skip_hrtf_attenuation_coupling(path);
    preflight.unity_attenuation =
        !preflight.bypass_path
        && should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.spatial_blend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.reject_reason =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain,
                                                  coupling, params);
    preflight.skipped = preflight.reject_reason != HrtfAttenuationCouplingRejectReason::None;
    return preflight;
}

} // namespace

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    return make_hrtf_attenuation_coupling_preflight(path, distance_attenuation, occlusion_gain,
                                                    coupling, params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                        float occlusion_gain,
                                        HrtfAttenuationCouplingRejectReason* reason,
                                        const HrtfAttenuationCoupling& coupling,
                                        const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight =
        make_hrtf_attenuation_coupling_preflight(path, distance_attenuation, occlusion_gain,
                                                 coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reject_reason;
    }
    return preflight.can_apply_coupling();
}

HrtfAttenuationCouplingSkipReason classify_hrtf_attenuation_coupling_skip(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        return HrtfAttenuationCouplingSkipReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingSkipReason::UnityAttenuation;
    }
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return HrtfAttenuationCouplingSkipReason::UnitySpatialBlend;
    }
    return HrtfAttenuationCouplingSkipReason::None;
}

bool hrtf_attenuation_coupling_skip_reason_is_blocking(HrtfAttenuationCouplingSkipReason reason) {
    return reason != HrtfAttenuationCouplingSkipReason::None;
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                       float occlusion_gain,
                                       HrtfAttenuationCouplingSkipReason* reason,
                                       const HrtfAttenuationCoupling& coupling,
                                       const BinauralPanParams& params) {
    const HrtfAttenuationCouplingSkipReason skip =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !hrtf_attenuation_coupling_skip_reason_is_blocking(skip);
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.reason =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    preflight.skipped = hrtf_attenuation_coupling_skip_reason_is_blocking(preflight.reason);
    return preflight;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.path = path;
    preflight.distance_attenuation = clamp_hrtf_attenuation(distance_attenuation);
    preflight.occlusion_gain = clamp_hrtf_attenuation(occlusion_gain);
    preflight.bypass_path = should_skip_hrtf_attenuation_coupling(path);
    preflight.unity_attenuation =
        is_unity_hrtf_attenuation(preflight.distance_attenuation, preflight.occlusion_gain);
    preflight.spatial_blend = compute_hrtf_spatial_blend(preflight.distance_attenuation,
                                                         preflight.occlusion_gain, coupling, params);
    preflight.unity_spatial_blend = is_unity_hrtf_spatial_blend(preflight.spatial_blend);

    if (preflight.bypass_path) {
        preflight.skip_reason = HrtfAttenuationCouplingSkipReason::BypassPath;
    } else if (preflight.unity_attenuation || preflight.unity_spatial_blend) {
        preflight.skip_reason = HrtfAttenuationCouplingSkipReason::UnityAttenuation;
    }
    return preflight;
}

bool hrtf_attenuation_coupling_skips_for_reason(HrtfPanPath path, float distance_attenuation,
                                                float occlusion_gain,
                                                HrtfAttenuationCouplingSkipReason expected,
                                                const HrtfAttenuationCoupling& coupling,
                                                const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                             params)
               .skip_reason
        == expected;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight;
    preflight.bypass_path = should_skip_hrtf_attenuation_coupling(path);
    preflight.unity_attenuation =
        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.spatial_blend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.would_narrow = should_narrow_hrtf_spatial_image(path, distance_attenuation,
                                                              occlusion_gain);
    return preflight;
}

bool can_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                               params)
        .can_apply_coupling();
}

HrtfAttenuationCouplingPreflight preflightHrtfAttenuationCoupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight;
    preflight.bypassPath = should_skip_hrtf_attenuation_coupling(path);
    preflight.unityAttenuation =
        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    return preflight;
}

bool canApplyHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                     float occlusion_gain) {
    return should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight result;
    result.path = path;
    result.bypass_pan = is_hrtf_pan_path_bypass(path);
    result.unity_attenuation = is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    result.spatial_blend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    result.unity_spatial_blend = is_unity_hrtf_spatial_blend(result.spatial_blend);
    return result;
}

bool can_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                               params)
        .can_apply_coupling();
}

HrtfAttenuationCouplingSkipReason classify_hrtf_attenuation_coupling_skip(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        return HrtfAttenuationCouplingSkipReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingSkipReason::UnityAttenuation;
    }
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return HrtfAttenuationCouplingSkipReason::UnitySpatialBlend;
    }
    return HrtfAttenuationCouplingSkipReason::None;
}

bool hrtf_attenuation_coupling_skip_reason_is_blocking(HrtfAttenuationCouplingSkipReason reason) {
    return reason != HrtfAttenuationCouplingSkipReason::None;
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                       float occlusion_gain,
                                       HrtfAttenuationCouplingSkipReason* reason,
                                       const HrtfAttenuationCoupling& coupling,
                                       const BinauralPanParams& params) {
    const HrtfAttenuationCouplingSkipReason skip =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !hrtf_attenuation_coupling_skip_reason_is_blocking(skip);
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.reason =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    preflight.skipped = hrtf_attenuation_coupling_skip_reason_is_blocking(preflight.reason);
    return preflight;
}

HrtfAttenuationCouplingSkipReason classify_hrtf_attenuation_coupling_skip(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        return HrtfAttenuationCouplingSkipReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingSkipReason::UnityAttenuation;
    }
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return HrtfAttenuationCouplingSkipReason::UnitySpatialBlend;
    }
    return HrtfAttenuationCouplingSkipReason::None;
}

bool hrtf_attenuation_coupling_skip_reason_is_blocking(HrtfAttenuationCouplingSkipReason reason) {
    return reason != HrtfAttenuationCouplingSkipReason::None;
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                       float occlusion_gain,
                                       HrtfAttenuationCouplingSkipReason* reason,
                                       const HrtfAttenuationCoupling& coupling,
                                       const BinauralPanParams& params) {
    const HrtfAttenuationCouplingSkipReason skip =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !hrtf_attenuation_coupling_skip_reason_is_blocking(skip);
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.reason =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    preflight.skipped = hrtf_attenuation_coupling_skip_reason_is_blocking(preflight.reason);
    return preflight;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::PanBypassed:
        return "PanBypassed";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    default:
        return "Unknown";
    }
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        return HrtfAttenuationCouplingRejectReason::PanBypassed;
    }
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

namespace {

HrtfAttenuationCouplingPreflight make_hrtf_attenuation_coupling_preflight(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.bypass_path = should_skip_hrtf_attenuation_coupling(path);
    preflight.unity_attenuation =
        !preflight.bypass_path
        && should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.spatial_blend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.reject_reason =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain,
                                                  coupling, params);
    preflight.skipped = preflight.reject_reason != HrtfAttenuationCouplingRejectReason::None;
    return preflight;
}

} // namespace

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    return make_hrtf_attenuation_coupling_preflight(path, distance_attenuation, occlusion_gain,
                                                    coupling, params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                        float occlusion_gain,
                                        HrtfAttenuationCouplingRejectReason* reason,
                                        const HrtfAttenuationCoupling& coupling,
                                        const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight =
        make_hrtf_attenuation_coupling_preflight(path, distance_attenuation, occlusion_gain,
                                                 coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reject_reason;
    }
    return preflight.can_apply_coupling();
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    HrtfBinauralPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuation = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.panPath.skipped;
    return preflight;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params);
}

HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, const HrtfIrStub& ir,
                                                       float distance_attenuation, float occlusion_gain,
                                                       const HrtfAttenuationCoupling& coupling,
                                                       const BinauralPanParams& params) {
    HrtfBinauralPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath.path = path;
    preflight.panPath.emptyIr = is_empty_hrtf_ir(ir);
    preflight.panPath.skipped = should_skip_hrtf_pan_path(path);
    preflight.attenuation =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.panPath.skipped;
    return preflight;
}

HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, float distance_attenuation,
                                                       float occlusion_gain,
                                                       const HrtfAttenuationCoupling& coupling,
                                                       const BinauralPanParams& params) {
    return preflight_hrtf_binaural_for_path(path, make_empty_hrtf_ir(), distance_attenuation,
                                            occlusion_gain, coupling, params);
}

bool can_apply_binaural_hrtf_pan(const HrtfBinauralPreflight& preflight) {
    return preflight.can_spatial_pan();
}

bool can_convolve_binaural_hrtf(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();
}

bool can_narrow_binaural_hrtf_spatial_image(const HrtfBinauralPreflight& preflight) {
    return preflight.can_narrow();
}

bool should_skip_hrtf_attenuation_coupling_preflight(const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.should_skip();
}

bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
                                                     float occlusion_gain) {
    return classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain)
        != HrtfAttenuationCouplingRejectReason::None;
}

bool should_skip_hrtf_attenuation_coupling_preflight(const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.should_skip();
}

float compute_hrtf_distance_factor(float distance_attenuation,
                                   const BinauralPanParams& params) {
    const float atten = clamp_hrtf_attenuation(distance_attenuation);
    return params.min_spatial_blend + (1.f - params.min_spatial_blend) * atten;
}

void apply_spatial_blend(BinauralPanGains& gains, float blend) {
    const float clamped = std::clamp(blend, 0.f, 1.f);
    const float centre = 0.5f * (gains.left + gains.right);
    gains.left = centre + (gains.left - centre) * clamped;
    gains.right = centre + (gains.right - centre) * clamped;
    clamp_binaural_pan_gains(gains);
}

bool is_unity_hrtf_spatial_blend(float blend, float epsilon) {
    return blend >= 1.f - epsilon;
}

void apply_hrtf_spatial_blend_guarded(BinauralPanGains& gains, float blend) {
    if (is_unity_hrtf_spatial_blend(blend)) {
        return;
    }
    apply_spatial_blend(gains, blend);
}

bool should_skip_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                    const HrtfAttenuationCoupling& coupling,
                                    const BinauralPanParams& params) {
    return is_unity_hrtf_spatial_blend(
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params));
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    default:
        return "Unknown";
    }
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.path = path;
    preflight.bypassed = is_hrtf_pan_path_bypass(path);
    preflight.unityAttenuation = is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.rejectReason =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    preflight.skipped = !should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);
    return preflight;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    float occlusion_gain, const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    const HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                               params);
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

HrtfBinauralPanPreflight preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener,
                                                     float distance_attenuation, float occlusion_gain,
                                                     const HrtfAttenuationCoupling& coupling,
                                                     const BinauralPanParams& params) {
    HrtfBinauralPanPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuationCoupling = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    return preflight;
}

void apply_hrtf_distance_factor(BinauralPanGains& gains, float distance_attenuation,
                                const BinauralPanParams& params) {
    apply_hrtf_spatial_blend_guarded(gains,
                                     compute_hrtf_distance_factor(distance_attenuation, params));
}

float compute_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    const float distance_blend =
        compute_hrtf_distance_factor(clamp_hrtf_attenuation(distance_attenuation), params);
    const float occlusion_blend =
        compute_hrtf_distance_factor(clamp_hrtf_attenuation(occlusion_gain), params);
    const float weight = clamp_hrtf_attenuation_coupling_weight(coupling.occlusion_weight);
    return distance_blend * (1.f - weight) + occlusion_blend * weight;
}

bool is_fully_spatial_hrtf_blend(float spatial_blend, float epsilon) {
    return spatial_blend >= 1.f - epsilon;

bool should_skip_hrtf_attenuation_coupling(float distance_attenuation, float occlusion_gain,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return is_fully_spatial_hrtf_blend(
    const float weight = clamp_hrtf_occlusion_coupling_weight(coupling.occlusion_weight);

bool should_skip_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,

    return is_unity_hrtf_spatial_blend(
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params));
float compute_hrtf_spatial_blend_for_path(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
    if (!should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain)) {
float compute_hrtf_spatial_blend_guarded(HrtfPanPath path, float distance_attenuation,
    if (should_preserve_hrtf_spatial_image(path, distance_attenuation, occlusion_gain)) {
        return 1.f;
    }
    return compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
bool HrtfAttenuationCouplingPreflight::can_apply_coupling() const {
    return !skip_coupling && should_narrow;

bool HrtfAttenuationCouplingPreflight::ready_for_stub() const {
    return skip_coupling || !should_narrow || can_apply_coupling();

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.path = path;
    preflight.distance_attenuation = clamp_hrtf_attenuation(distance_attenuation);
    preflight.occlusion_gain = clamp_hrtf_attenuation(occlusion_gain);
    preflight.coupling_weight = clamp_hrtf_attenuation_coupling_weight(coupling.occlusion_weight);
    preflight.unity_attenuation =
        is_unity_hrtf_attenuation(preflight.distance_attenuation, preflight.occlusion_gain);
    preflight.skip_coupling = should_skip_hrtf_attenuation_coupling(path);
    preflight.should_narrow =
        should_narrow_hrtf_spatial_image(path, preflight.distance_attenuation, preflight.occlusion_gain);
    preflight.spatial_blend =
        compute_hrtf_spatial_blend(preflight.distance_attenuation, preflight.occlusion_gain, coupling,
                                   params);
    return preflight;

bool HrtfSpatialPanPreflight::can_apply_spatial_pan() const {
    return pan.can_apply_spatial_pan();

bool HrtfSpatialPanPreflight::can_apply_attenuation_coupling() const {
    return coupling.can_apply_coupling();

bool HrtfSpatialPanPreflight::ready_for_stub() const {
    return pan.ready_for_stub() && coupling.ready_for_stub();

HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                                   const Vec3& rel_listener,
                                                   float distance_attenuation, float occlusion_gain,
    HrtfSpatialPanPreflight preflight{};
    preflight.pan = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.coupling = preflight_hrtf_attenuation_coupling(
        preflight.pan.path, distance_attenuation, occlusion_gain, coupling, params);
    HrtfAttenuationCouplingPreflight preflight;
    preflight.bypassPath = should_skip_hrtf_attenuation_coupling(path);
    preflight.unityAttenuation =
        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.unitySpatialBlend = is_unity_hrtf_spatial_blend(preflight.spatialBlend);
    preflight.distanceAttenuation = clamp_hrtf_attenuation(distance_attenuation);
    preflight.occlusionGain = clamp_hrtf_attenuation(occlusion_gain);
        is_unity_hrtf_attenuation(preflight.distanceAttenuation, preflight.occlusionGain);
        compute_hrtf_spatial_blend(preflight.distanceAttenuation, preflight.occlusionGain, coupling,
    preflight.skipped =
        !should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);
    if (should_skip_hrtf_attenuation_coupling(path)) {
        preflight.skipped = true;
        preflight.reason = HrtfAttenuationCouplingSkipReason::BypassPath;
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        preflight.reason = HrtfAttenuationCouplingSkipReason::UnityAttenuation;

bool should_skip_hrtf_attenuation_coupling_apply(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                             params)
        .skipped;
        preflight.reject = HrtfAttenuationCouplingPreflightReject::BypassPath;

    preflight.applyCoupling = true;
        preflight.reject = HrtfAttenuationCouplingPreflightReject::UnityAttenuation;

    preflight.narrowImage = true;
    preflight.reject = HrtfAttenuationCouplingPreflightReject::None;

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             HrtfAttenuationCouplingPreflightReject* reject,
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
    if (reject != nullptr) {
        *reject = preflight.reject;
    return preflight.narrowImage;
    preflight.bypass_path = is_hrtf_pan_path_bypass(path);
    preflight.unity_attenuation = is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.skipped = preflight.bypass_path || preflight.unity_attenuation
        || is_unity_hrtf_spatial_blend(preflight.spatial_blend);

bool can_narrow_hrtf_spatial_image_preflight(HrtfPanPath path, float distance_attenuation,
        .can_narrow();

HrtfGuardedPanPreflight preflight_hrtf_guarded_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfGuardedPanPreflight preflight;
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.pan_path = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
        preflight.pan_path.path, distance_attenuation, occlusion_gain, coupling, params);



}

void apply_hrtf_attenuation_coupling(BinauralPanGains& gains, float distance_attenuation,
                                     float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                     const BinauralPanParams& params) {
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return;
    }
    apply_hrtf_spatial_blend_guarded(
        gains, compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params));

void apply_hrtf_attenuation_coupling_guarded(BinauralPanGains& gains, bool hrtf_enabled,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    apply_hrtf_attenuation_coupling_for_path(
        gains, resolve_hrtf_pan_path(hrtf_enabled, rel_listener), distance_attenuation,
        occlusion_gain, coupling, params);

                                             const HrtfIrStub& ir, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
        gains, resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener), distance_attenuation,
    if (should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain)) {
    if (should_skip_hrtf_attenuation_coupling_mapping(distance_attenuation, occlusion_gain)) {
    apply_spatial_blend(gains,
                        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling,
                                                   params));
}

void apply_hrtf_attenuation_coupling_for_path(BinauralPanGains& gains, HrtfPanPath path,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    if (!can_narrow_hrtf_spatial_image(
            preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                coupling, params))) {
    if (!should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain)) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
    if (should_skip_hrtf_attenuation_coupling_for_inputs(path, distance_attenuation,
                                                         occlusion_gain)) {
    if (!preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain)) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (preflight.skipped) {
    if (!preflight.narrowImage) {
        return;
    }
    apply_hrtf_attenuation_coupling(gains, distance_attenuation, occlusion_gain, coupling, params);
    if (should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain, coupling,
                                            params)) {
    apply_spatial_blend(gains,
                        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling,
                                                   params));
    if (!should_apply_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain)) {
                        compute_hrtf_spatial_blend_guarded(path, distance_attenuation, occlusion_gain,
                                                         coupling, params));
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const Vec3& rel_listener,
                                                    float distance_attenuation,
                                                    float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
    const HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, HrtfIrStub{}, rel_listener);
    BinauralPanGains pan = compute_binaural_pan_gains_for_path(path, rel_listener, params);
    if (!should_skip_hrtf_pan_path(path)
        && !should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain, coupling,
                                                  params)) {
        apply_hrtf_attenuation_coupling(pan, distance_attenuation, occlusion_gain, coupling, params);
    }
    return pan;
    return compute_binaural_pan_gains_coupled_for_path(
        resolve_hrtf_pan_path(hrtf_enabled, rel_listener), rel_listener, distance_attenuation,
        occlusion_gain, coupling, params);
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
    return compute_binaural_pan_gains_coupled_for_path(
        preflight_hrtf_pan_path(hrtf_enabled, preflight_hrtf_ir(ir), rel_listener), rel_listener,
        distance_attenuation, occlusion_gain, coupling, params);
    if (!can_apply_binaural_hrtf_pan(preflight)) {
        return make_centre_binaural_pan_gains();
    }

    BinauralPanGains pan =
        compute_binaural_pan_gains_for_path(preflight.panPath.path, rel_listener, params);
    if (can_narrow_binaural_hrtf_spatial_image(preflight)) {
        apply_hrtf_attenuation_coupling(pan, distance_attenuation, occlusion_gain, coupling, params);
    return pan;
}

BinauralPanGains compute_binaural_pan_gains_coupled_for_path(HrtfPanPath path,
                                                              const Vec3& rel_listener,
                                                              float distance_attenuation,
                                                              float occlusion_gain,
                                                              const HrtfAttenuationCoupling& coupling,
                                                              const BinauralPanParams& params) {
    BinauralPanGains pan = compute_binaural_pan_gains_for_path(path, rel_listener, params);
    if (can_narrow_binaural_hrtf_spatial_image(preflight_hrtf_binaural_for_path(
            path, distance_attenuation, occlusion_gain, coupling, params))) {
        apply_hrtf_attenuation_coupling(pan, distance_attenuation, occlusion_gain, coupling, params);
    }
    return pan;

namespace {

HrtfBinauralRejectReason map_hrtf_ir_reject_reason(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return HrtfBinauralRejectReason::None;
    case HrtfIrRejectReason::NullSamples:
        return HrtfBinauralRejectReason::NullSamples;
    case HrtfIrRejectReason::ZeroLength:
        return HrtfBinauralRejectReason::ZeroLength;
    case HrtfIrRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;

HrtfBinauralRejectReason map_hrtf_pan_path_reject_reason(HrtfPanPathRejectReason reason) {
    case HrtfPanPathRejectReason::None:
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;

HrtfBinauralRejectReason map_hrtf_attenuation_coupling_reject_reason(
    HrtfAttenuationCouplingRejectReason reason) {
    case HrtfAttenuationCouplingRejectReason::None:
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;

void populate_hrtf_binaural_reject_reasons(HrtfBinauralPreflight& preflight) {
    preflight.reason = map_hrtf_pan_path_reject_reason(preflight.panPath.reason);
    if (preflight.reason != HrtfBinauralRejectReason::None) {
        preflight.convolutionReason = preflight.reason;
    } else {
        preflight.convolutionReason = map_hrtf_ir_reject_reason(preflight.ir.reason);
    preflight.narrowingReason =
        map_hrtf_attenuation_coupling_reject_reason(preflight.attenuationCoupling.reason);

} // namespace

const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::NullSamples:
        return "NullSamples";
    case HrtfBinauralRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    return "Unknown";

HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight) {
    return map_hrtf_pan_path_reject_reason(preflight.panPath.reason);

HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason pan_reason = map_hrtf_pan_path_reject_reason(preflight.panPath.reason);
    if (pan_reason != HrtfBinauralRejectReason::None) {
        return pan_reason;
    return map_hrtf_ir_reject_reason(preflight.ir.reason);

HrtfBinauralRejectReason hrtf_binaural_narrowing_reject_reason(
    return map_hrtf_attenuation_coupling_reject_reason(preflight.attenuationCoupling.reason);

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(preflight) == expected;

bool hrtf_binaural_rejects_for_convolution_reason(const HrtfBinauralPreflight& preflight,
    return hrtf_binaural_convolution_reject_reason(preflight) == expected;

bool hrtf_binaural_rejects_for_narrowing_reason(const HrtfBinauralPreflight& preflight,
    return hrtf_binaural_narrowing_reject_reason(preflight) == expected;

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
    HrtfBinauralPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuationCoupling = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    populate_hrtf_binaural_reject_reasons(preflight);
    return preflight;

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params);

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, const HrtfIrStub& ir,
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                  occlusion_gain, coupling, params);

                                              const Vec3& source_position, float distance_attenuation,
                                              float occlusion_gain, const HrtfAttenuationCoupling& coupling,
    return preflight_hrtf_binaural(hrtf_enabled, listener, source_position, make_empty_hrtf_ir(),

bool can_apply_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight) {
    return preflight.can_spatial_pan();

bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();

bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight) {
    return preflight.can_narrow_spatial_image();

bool should_skip_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.is_bypass();

bool should_skip_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight) {
    return preflight.should_skip_convolution();

bool uses_ild_itd_stub_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.uses_ild_itd_stub();

BinauralPanGains compute_binaural_pan_gains_from_preflight(const HrtfBinauralPreflight& preflight,
    (void)coupling;
    BinauralPanGains pan =
        compute_binaural_pan_gains_for_path(preflight.path(), rel_listener, params);
    if (preflight.can_narrow_spatial_image()) {
        apply_hrtf_spatial_blend_guarded(pan, preflight.attenuationCoupling.spatialBlend);
    const HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (!should_skip_hrtf_pan_path(path)
        && !should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain, coupling,
                                                  params)) {

void apply_binaural_pan_to_sample_from_preflight(float mono, const HrtfBinauralPreflight& preflight,
                                                 const Vec3& rel_listener, float attenuation,
                                                 float& left, float& right,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    if (preflight.should_skip()) {
        apply_centre_binaural_pan_to_sample(mono, attenuation, left, right);
        return;
    const BinauralPanGains pan =
        compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
    apply_binaural_pan_to_sample(mono, pan, attenuation, left, right);
    preflight.panPath.path = path;
    preflight.panPath.skipped = should_skip_hrtf_pan_path(path);
        path, distance_attenuation, occlusion_gain, coupling, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
        return "none";
    case HrtfBinauralRejectReason::PanBypass:
        return "pan_bypass";
    case HrtfBinauralRejectReason::IrFallback:
        return "ir_fallback";
    case HrtfBinauralRejectReason::AttenuationSkipped:
        return "attenuation_skipped";
    return "unknown";

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.is_bypass()) {
        return HrtfBinauralRejectReason::PanBypass;
    if (!preflight.can_convolve()) {
        return HrtfBinauralRejectReason::IrFallback;
    if (!preflight.can_narrow_spatial_image()) {
        return HrtfBinauralRejectReason::AttenuationSkipped;

        return "hrtf_disabled";
        return "co_located";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
        return "malformed_ir";
        return "bypass_path";
        return "unity_attenuation";

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    (void)ir;
    const HrtfPanPathRejectReason pan_reject =
        classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    (void)ir;
    const HrtfPanPathRejectReason pan_reject =
        classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    (void)ir;
    const HrtfPanPathRejectReason pan_reject =
        classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    (void)ir;
    const HrtfPanPathRejectReason pan_reject =
        classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    const HrtfPanPathRejectReason pan_reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    switch (pan_reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(hrtf_enabled, rel_listener) == expected;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    (void)ir;
    const HrtfPanPathRejectReason pan_reject =
        classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    }
    return "unknown";
}

HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    switch (hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener)) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(hrtf_enabled, rel_listener) == expected;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    HrtfBinauralPreflight preflight{};
    preflight.reason = hrtf_binaural_reject_reason(hrtf_enabled, rel_listener);
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuationCoupling = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    preflight.reason = classify_hrtf_binaural_reject(preflight);
    preflight.reason = classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener);
    return preflight;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params);
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, const HrtfIrStub& ir,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, coupling, params);
}

                                              const Vec3& source_position, float distance_attenuation,
                                              float occlusion_gain, const HrtfAttenuationCoupling& coupling,
    return preflight_hrtf_binaural(hrtf_enabled, listener, source_position, make_empty_hrtf_ir(),
                                   distance_attenuation, occlusion_gain, coupling, params);
bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfBinauralRejectReason* reason,
    const HrtfBinauralRejectReason reject =
        classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = reject;
    return reject == HrtfBinauralRejectReason::None;

bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, reason, coupling, params);

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  HrtfBinauralRejectReason& reason,
                                   occlusion_gain, &reason, coupling, params);

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural(hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain,
                                   &reason, coupling, params);












const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    return "unknown";

HrtfBinauralRejectReason hrtf_binaural_reject_reason(HrtfPanPathRejectReason pan_reason) {
    switch (pan_reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;

HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    return hrtf_binaural_reject_reason(hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener));

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(hrtf_enabled, rel_listener) == expected;
}

bool can_apply_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight) {
    return preflight.can_spatial_pan();
}

bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();
}

bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight) {
    return preflight.can_narrow_spatial_image();
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    preflight = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                        occlusion_gain, coupling, params);
    return preflight.can_spatial_pan();
}

bool should_skip_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.rejectReason() != HrtfBinauralRejectReason::None;
}

bool is_consistent_hrtf_binaural_preflight(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.path == HrtfPanPath::Convolution) {
        return preflight.ir.can_convolve();
    return true;

bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();

bool should_skip_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight) {
    return preflight.should_skip_convolution();

bool has_empty_hrtf_ir(const HrtfBinauralPreflight& preflight) {
    return preflight.has_empty_ir();

bool uses_ild_itd_stub_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.uses_ild_itd_stub();

bool should_apply_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight) {
    return preflight.can_narrow_spatial_image();

bool should_skip_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight) {
    return !preflight.can_narrow_spatial_image();

bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight) {
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan_path_preflight(hrtf_enabled, rel_listener);





    return preflight.reason != HrtfBinauralRejectReason::None;
}

BinauralPanGains compute_binaural_pan_gains_from_preflight(const HrtfBinauralPreflight& preflight,
                                                           const Vec3& rel_listener,
                                                           const HrtfAttenuationCoupling& coupling,
                                                           const BinauralPanParams& params) {
    (void)coupling;
    BinauralPanGains pan =
        compute_binaural_pan_gains_for_path(preflight.path(), rel_listener, params);
    if (preflight.can_narrow_spatial_image()) {
        apply_hrtf_spatial_blend_guarded(pan, preflight.attenuationCoupling.spatialBlend);
    }
    return pan;
}

void apply_binaural_pan_to_sample_from_preflight(float mono, const HrtfBinauralPreflight& preflight,
                                                 const Vec3& rel_listener, float attenuation,
                                                 float& left, float& right,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    if (preflight.should_skip()) {
        apply_centre_binaural_pan_to_sample(mono, attenuation, left, right);
        return;
    }
    const BinauralPanGains pan =
        compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
    apply_binaural_pan_to_sample(mono, pan, attenuation, left, right);
}

} // namespace fuse::audio
