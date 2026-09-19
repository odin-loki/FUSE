#include <fuse/audio/binaural_pan.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

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

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    return HrtfPanPathRejectReason::None;

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;

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
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    return HrtfIrRejectReason::ZeroLength;
}

const char* hrtfIrRejectReasonName(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";
}

HrtfIrRejectReason hrtfIrRejectReason(const HrtfIrStub& ir) {
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
    return HrtfIrRejectReason::None;
}

bool hrtfIrRejectsForReason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtfIrRejectReason(ir) == expected;
}

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (is_empty_hrtf_ir(ir)) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
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

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (is_empty_hrtf_ir(ir)) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::None;
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
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    }
    return "Unknown";
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
    return HrtfIrRejectReason::MalformedIr;
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
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
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
    }
    return "Unknown";
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

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
    }
    return "Unknown";
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

bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
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

bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !hrtf_ir_reject_reason_is_blocking(reject);
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
        return "None";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    default:
        return "Unknown";
    }
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::None:
    default:
        return "none";
    }
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

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(ir) == expected;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (is_empty_hrtf_ir(ir)) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(ir) == expected;
}

namespace {

HrtfBinauralRejectReason map_pan_path_skip_reject_to_binaural(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason map_pan_path_convolution_reject_to_binaural(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfPanPathRejectReason::EmptyIr:
        return HrtfBinauralRejectReason::EmptyIr;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason map_attenuation_coupling_reject_to_binaural(
    HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

} // namespace

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
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
    return HrtfIrRejectReason::NullSamples;
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

bool is_blocking_hrtf_ir_reject_reason(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
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

bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
}

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
    }
    return "Unknown";
}

bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
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
    default:
        return "unknown";
    }
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
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
    return HrtfIrRejectReason::NullSamples;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(ir) == expected;
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::EmptyIr;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
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

bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
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
    return HrtfIrRejectReason::NullSamples;
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::NullSamples;
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

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

bool hrtfIrRejectReasonIsBlocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
}

HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    return HrtfIrRejectReason::NullSamples;
}

bool tryPreflightHrtfIr(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    reason = classifyHrtfIrReject(ir);
    return !hrtfIrRejectReasonIsBlocking(reason);
}

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    }
    return "unknown";
}

bool hrtfIrRejectReasonIsBlocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
}

HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrStub& ir) {
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

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    }
    return "Unknown";
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
    return HrtfIrRejectReason::NullSamples;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
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
    }
    return "Unknown";
}

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::MalformedIr;
    }
    return HrtfIrRejectReason::ZeroLength;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

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
    }
    return "Unknown";
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
    return HrtfIrRejectReason::None;
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    return preflight.reason;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(preflight) == expected;
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::EmptyIr;
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
    return HrtfIrRejectReason::ZeroLength;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

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
    }
    return "Unknown";
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::EmptyIr:
        return "EmptyIr";
    }
    return "Unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::EmptyIr;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(preflight) == expected;
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples && preflight.zeroLength) {
        return HrtfIrRejectReason::EmptyIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::EmptyIr;
}

const char* hrtfIrRejectReasonName(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";
}

HrtfIrRejectReason hrtfIrRejectReason(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr && ir.length == 0) {
        return HrtfIrRejectReason::EmptyIr;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    return HrtfIrRejectReason::ZeroLength;
}

HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrPreflight& preflight) {
    return preflight.reason;
}

bool hrtfIrRejectsForReason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtfIrRejectReason(ir) == expected;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::None;
}

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
    default:
        return "Unknown";
    }
}

HrtfIrRejectReason classify_hrtf_ir_reject_reason(const HrtfIrStub& ir) {
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
    return HrtfIrRejectReason::None;
}

bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject_reason(ir);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfIrRejectReason::None;
}

bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir_convolution(ir, &reason);
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
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
    }
    return "Unknown";
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
    return HrtfIrRejectReason::None;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

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
    }
    return "Unknown";
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

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    }
    return "Unknown";
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    return preflight.reason;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(preflight) == expected;
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

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    }
    return "Unknown";
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
    }
    return "Unknown";
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

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::None:
    default:
        return "none";
    }
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    }
    return "Unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
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

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::None;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::EmptyIr:
        return "EmptyIr";
    }
    return "Unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::EmptyIr;
}

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "none";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::EmptyIr:
        return "empty_ir";
    }
    return "unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;
    }
    return HrtfIrRejectReason::None;
}

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight preflight{};
    preflight.reason = hrtf_ir_reject_reason(ir);
    preflight.reason = hrtfIrRejectReason(ir);
    preflight.rejected = preflight.reason != HrtfIrRejectReason::None;
    preflight.reason = classify_hrtf_ir_reject(ir);
    preflight.nullSamples = ir.samples == nullptr;
    preflight.zeroLength = ir.length == 0;
    preflight.malformedIr = preflight.reason == HrtfIrRejectReason::MalformedIr;
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    preflight.reason = classify_hrtf_ir_reject(ir);
    preflight.rejectReason = classify_hrtf_ir_reject(ir);
    preflight.malformedIr = preflight.reason == HrtfIrRejectReason::MalformedIr;
    preflight.emptyIr = preflight.reason != HrtfIrRejectReason::None;
    preflight.reason = hrtf_ir_reject_reason(ir);
    preflight.reason = classify_hrtf_ir_reject(preflight);
    preflight.rejectReason = classifyHrtfIrReject(ir);
    preflight.rejectReason = classify_hrtf_ir_reject_reason(ir);
    return preflight;
}

HrtfIrPreflight try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    const HrtfIrPreflight preflight = preflight_hrtf_ir(ir);
    reason = preflight.reason;

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight) {
    preflight = preflight_hrtf_ir(ir);
    return preflight.can_convolve();










bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    if (reason != nullptr) {
        *reason = reject;
    return reject == HrtfIrRejectReason::None;

        *reason = preflight.reason;

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir(ir, &reason);









bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight, HrtfIrRejectReason& reason) {
    reason = preflight.rejectReason;





bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& out, HrtfIrRejectReason& reason) {
    out = preflight_hrtf_ir(ir);
    reason = out.reason;
    return out.can_convolve();


bool hrtf_ir_preflight_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected) {
    return preflight.reason == expected;

    reason = out.rejectReason;


    preflight.reason = classifyHrtfIrReject(ir);

    const HrtfIrRejectReason reject = classifyHrtfIrReject(ir);


bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {

bool tryPreflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {

    preflight.reject = classify_hrtf_ir_reject(preflight);

        *reason = preflight.reject;

    return preflight_hrtf_ir_ready(ir, &reason);

const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::EmptyIr:
        return "EmptyIr";
    return "Unknown";

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    return HrtfIrRejectReason::None;




    preflight.reason = hrtfIrRejectReason(ir);

bool preflightHrtfIrReady(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
        *reason = classifyHrtfIrReject(preflight);

bool tryPreflightHrtfIr(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflightHrtfIrReady(ir, &reason);
    preflight.reason = classifyHrtfIrReject(preflight);

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
        return "none";
        return "null_samples";
        return "zero_length";
        return "malformed_ir";
        return "empty_ir";
    return "unknown";

    if (preflight.can_convolve()) {
    if (preflight.nullSamples && preflight.zeroLength) {
        return HrtfIrRejectReason::EmptyIr;

        *reason = classify_hrtf_ir_reject(preflight);


HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrPreflight& preflight) {






bool should_skip_hrtf_ir_ready(const HrtfIrStub& ir) {
    return !preflight_hrtf_ir_ready(ir);










const char* hrtfIrRejectReasonName(HrtfIrRejectReason reason) {


bool hrtfIrRejectsForReason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return classifyHrtfIrReject(preflight_hrtf_ir(ir)) == expected;




    if (preflight.emptyIr) {
















bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir) {

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
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    reason = classify_hrtf_ir_reject(ir);
    return reason == HrtfIrRejectReason::None;
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrPreflight preflight = preflight_hrtf_ir(ir);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_convolve();

    reason = preflight.reason;

const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason) {
const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
    case HrtfIrRejectReason::None:
    case HrtfIrRejectReason::NullSamples:
        return "null_samples";
    case HrtfIrRejectReason::ZeroLength:
        return "zero_length";
    case HrtfIrRejectReason::MalformedIr:
        return "malformed_ir";
    return "unknown";

HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrPreflight& preflight) {
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    return HrtfIrRejectReason::None;

bool tryCanConvolveHrtfIr(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    reason = classifyHrtfIrReject(preflight);

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
        *reason = classifyHrtfIrReject(preflight);
    return preflight;

    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    case HrtfPanPathRejectReason::EmptyIr:
        return "empty_ir";

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
bool hrtf_pan_path_reject_reason_is_blocking_spatial(HrtfPanPathRejectReason reason) {
    return reason == HrtfPanPathRejectReason::HrtfDisabled
        || reason == HrtfPanPathRejectReason::CoLocated;

bool hrtf_pan_path_reject_reason_is_blocking_convolution(HrtfPanPathRejectReason reason) {
    return reason != HrtfPanPathRejectReason::None;

HrtfPanPathRejectReason classify_hrtf_pan_path_spatial_reject(bool hrtf_enabled,
                                                              const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    return HrtfPanPathRejectReason::None;


HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(bool hrtf_enabled,
                                                                const HrtfIrStub& ir,
    const HrtfPanPathRejectReason spatial_reject =
        classify_hrtf_pan_path_spatial_reject(hrtf_enabled, rel_listener);
    if (spatial_reject != HrtfPanPathRejectReason::None) {
        return spatial_reject;
    if (is_empty_hrtf_ir(ir)) {
        return HrtfPanPathRejectReason::EmptyIr;

bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight) {
    return preflight.should_skip_convolution();








HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason) {
        return "None";
        return "HrtfDisabled";
        return "CoLocated";
    default:
        return "Unknown";


    case HrtfPanPathRejectReason::CoLocatedSource:
        return "CoLocatedSource";
        return "EmptyIr";

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
        return HrtfPanPathRejectReason::CoLocatedSource;

    return hrtf_pan_path_reject_reason(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, ir, rel_listener) == expected;

    return preflight_hrtf_ir_ready(ir, &reason);


    if (rel_listener.length() < kHrtfCoLocatedEpsilon) {

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
    return preflight.reason;


    (void)ir;
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);


const char* hrtf_pan_path_convolve_reject_reason_label(HrtfPanPathConvolveRejectReason reason) {
    case HrtfPanPathConvolveRejectReason::None:
    case HrtfPanPathConvolveRejectReason::HrtfDisabled:
    case HrtfPanPathConvolveRejectReason::CoLocated:
    case HrtfPanPathConvolveRejectReason::EmptyIr:
    case HrtfPanPathConvolveRejectReason::MalformedIr:
        return "MalformedIr";


HrtfPanPathConvolveRejectReason hrtf_pan_path_convolve_reject_reason(bool hrtf_enabled,
    const HrtfPanPathRejectReason pan_reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    if (pan_reason == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfPanPathConvolveRejectReason::HrtfDisabled;
    if (pan_reason == HrtfPanPathRejectReason::CoLocated) {
        return HrtfPanPathConvolveRejectReason::CoLocated;
    const HrtfIrRejectReason ir_reason = hrtf_ir_reject_reason(ir);
    if (ir_reason == HrtfIrRejectReason::MalformedIr) {
        return HrtfPanPathConvolveRejectReason::MalformedIr;
    if (ir_reason != HrtfIrRejectReason::None) {
        return HrtfPanPathConvolveRejectReason::EmptyIr;
    return HrtfPanPathConvolveRejectReason::None;


HrtfPanPathConvolveRejectReason classify_hrtf_pan_path_convolve_reject(
    const HrtfPanPathPreflight& preflight) {
    return preflight.convolveReason;

bool can_convolve_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject_reason = hrtf_ir_reject_reason(ir);
        *reason = reject_reason;
    return reject_reason == HrtfIrRejectReason::None;

    reason = hrtf_ir_reject_reason(ir);

bool is_co_located_hrtf_source(const Vec3& rel_listener) {
    return rel_listener.length() < kHrtfCoLocatedEpsilon;

bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !hrtf_enabled || is_co_located_hrtf_source(rel_listener);

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !should_skip_hrtf_pan(hrtf_enabled, rel_listener);

bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan(hrtf_enabled, rel_listener);



bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;









bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(ir) == expected;



const char* hrtfPanPathRejectReasonName(HrtfPanPathRejectReason reason) {

HrtfPanPathRejectReason hrtfPanPathRejectReason(bool hrtf_enabled, const Vec3& rel_listener) {

bool hrtfPanPathRejectsForReason(bool hrtf_enabled, const Vec3& rel_listener,
    return hrtfPanPathRejectReason(hrtf_enabled, rel_listener) == expected;


























bool preflight_hrtf_spatial_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfPanPathRejectReason* reason) {
    const HrtfPanPathRejectReason reject =
        *reason = reject;
    return !hrtf_pan_path_reject_reason_is_blocking_spatial(reject);

bool preflight_hrtf_convolution_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener,
        classify_hrtf_pan_path_convolution_reject(hrtf_enabled, ir, rel_listener);
    return !hrtf_pan_path_reject_reason_is_blocking_convolution(reject);

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const HrtfIrStub& ir,

    return classify_hrtf_pan_path_reject(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

    return classify_hrtf_pan_path_reject(hrtf_enabled, ir, rel_listener) == expected;


    return classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener) == expected;
    case HrtfPanPathRejectReason::MalformedIr:

HrtfPanPathRejectReason classify_hrtf_pan_path_skip_reject(bool hrtf_enabled, const Vec3& rel_listener) {

    const HrtfPanPathRejectReason skip_reason = classify_hrtf_pan_path_skip_reject(hrtf_enabled, rel_listener);
    if (skip_reason != HrtfPanPathRejectReason::None) {
        return skip_reason;

    const HrtfIrRejectReason ir_reason = classify_hrtf_ir_reject(ir);
    switch (ir_reason) {
        return HrtfPanPathRejectReason::MalformedIr;


bool is_blocking_hrtf_pan_path_reject_reason(HrtfPanPathRejectReason reason) {


bool hrtf_pan_path_reject_reason_is_blocking(HrtfPanPathRejectReason reason) {







const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    case HrtfIrRejectReason::EmptyIr:

bool hrtf_ir_reject_reason_blocks_convolution(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;



    if (preflight.hrtfDisabled) {
    if (preflight.coLocated) {



    if (preflight.can_spatial_pan()) {


bool hrtfPanPathRejectReasonIsBlocking(HrtfPanPathRejectReason reason) {

HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const Vec3& rel_listener) {

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathRejectReason& reason) {
    reason = classifyHrtfPanPathReject(hrtf_enabled, rel_listener);
    return !hrtfPanPathRejectReasonIsBlocking(reason);

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflightHrtfPanPath(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);


HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfIrRejectReason ir_reject = classifyHrtfIrReject(ir);
    if (ir_reject == HrtfIrRejectReason::MalformedIr) {
    if (ir_reject != HrtfIrRejectReason::None) {
        return "NullSamples";
        return "ZeroLength";

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;

        *reason = classify_hrtf_ir_reject(preflight);


bool preflight_hrtf_ir_convolution_ready(const HrtfIrPreflight& preflight,
                                       HrtfIrRejectReason* reason) {

bool preflight_hrtf_ir_convolution_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    return preflight_hrtf_ir_convolution_ready(preflight_hrtf_ir(ir), reason);

bool try_preflight_hrtf_ir_convolution(const HrtfIrPreflight& preflight, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir_convolution_ready(preflight, &reason);

bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir_convolution_ready(ir, &reason);

bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir) {
    return !preflight_hrtf_ir_convolution_ready(ir);

const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason) {
    case HrtfConvolutionRejectReason::None:
    case HrtfConvolutionRejectReason::HrtfDisabled:
    case HrtfConvolutionRejectReason::CoLocated:
    case HrtfConvolutionRejectReason::MalformedIr:
    case HrtfConvolutionRejectReason::NullSamples:

HrtfConvolutionRejectReason hrtf_convolution_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfPanPathRejectReason bypass = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    if (bypass == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfConvolutionRejectReason::HrtfDisabled;
    if (bypass == HrtfPanPathRejectReason::CoLocated) {
        return HrtfConvolutionRejectReason::CoLocated;
    const HrtfIrRejectReason irReason = hrtf_ir_reject_reason(ir);
    switch (irReason) {
        return HrtfConvolutionRejectReason::MalformedIr;
        return HrtfConvolutionRejectReason::NullSamples;
        return HrtfConvolutionRejectReason::None;

bool hrtf_convolution_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                         HrtfConvolutionRejectReason expected) {
    return hrtf_convolution_reject_reason(hrtf_enabled, ir, rel_listener) == expected;





bool hrtf_pan_path_rejects_for_reason(const HrtfPanPathPreflight& preflight,
    return classify_hrtf_pan_path_reject(preflight) == expected;

    if (!preflight.skipped) {

    if (preflight.can_convolve()) {



    return !preflight_hrtf_ir_ready(ir);

const char* hrtf_pan_path_convolution_reject_reason_label(HrtfPanPathConvolutionRejectReason reason) {
    case HrtfPanPathConvolutionRejectReason::None:
    case HrtfPanPathConvolutionRejectReason::HrtfDisabled:
    case HrtfPanPathConvolutionRejectReason::CoLocated:
    case HrtfPanPathConvolutionRejectReason::EmptyIr:
    case HrtfPanPathConvolutionRejectReason::MalformedIr:


HrtfPanPathConvolutionRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir_preflight) {
    if (preflight.skipped) {
            return HrtfPanPathConvolutionRejectReason::HrtfDisabled;
            return HrtfPanPathConvolutionRejectReason::CoLocated;
    if (ir_preflight.malformedIr) {
        return HrtfPanPathConvolutionRejectReason::MalformedIr;
    if (ir_preflight.emptyIr) {
        return HrtfPanPathConvolutionRejectReason::EmptyIr;
    return HrtfPanPathConvolutionRejectReason::None;


bool hrtf_pan_path_convolution_rejects_for_reason(
    const HrtfPanPathPreflight& preflight, const HrtfIrPreflight& ir_preflight,
    HrtfPanPathConvolutionRejectReason expected) {
    return classify_hrtf_pan_path_convolution_reject(preflight, ir_preflight) == expected;




HrtfPanPathRejectReason classifyHrtfPanPathReject(const HrtfPanPathPreflight& preflight) {








const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {




bool should_skip_hrtf_ir_ready(const HrtfIrStub& ir) {













HrtfPanPathRejectReason classify_hrtf_pan_path_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,

HrtfPanPathRejectReason classify_hrtf_pan_path_reject_reason(bool hrtf_enabled,
    return classify_hrtf_pan_path_reject_reason(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
        classify_hrtf_pan_path_reject_reason(hrtf_enabled, ir, rel_listener);
    return reject == HrtfPanPathRejectReason::None;

bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    return preflight_hrtf_spatial_pan(hrtf_enabled, ir, rel_listener, &reason);

bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_spatial_pan(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
    return try_preflight_hrtf_spatial_pan(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

    if (preflight.nullSamples && preflight.zeroLength) {








bool should_skip_hrtf_ir_convolution_preflight(const HrtfIrStub& ir) {








bool hrtf_ir_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected) {
    return classify_hrtf_ir_reject(preflight) == expected;

















HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrPreflight& preflight) {

    return preflight.reason == expected;


bool hrtf_pan_path_convolve_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                               HrtfPanPathConvolveRejectReason expected) {
    return hrtf_pan_path_convolve_reject_reason(hrtf_enabled, ir, rel_listener) == expected;






bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {

bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(preflight);
    return !hrtf_ir_reject_reason_is_blocking(reject);




    return preflight_hrtf_ir_convolution(ir, &reason);

bool preflight_hrtf_ir_convolve_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {

bool try_preflight_hrtf_ir_convolve(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir_convolve_ready(ir, &reason);








    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(preflight_hrtf_ir(ir));
    return reject == HrtfIrRejectReason::None;

    reason = classify_hrtf_ir_reject(preflight_hrtf_ir(ir));





    return classify_hrtf_ir_reject(preflight_hrtf_ir(ir)) == expected;










bool tryCanApplySpatialHrtfPan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    reason = classifyHrtfPanPathReject(preflight);
    return preflight.can_spatial_pan();

bool tryCanApplySpatialHrtfPan(bool hrtf_enabled, const Vec3& rel_listener,
    return tryCanApplySpatialHrtfPan(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
        *reason = classifyHrtfPanPathReject(preflight);

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);





    }
    return "unknown";

}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    preflight.reason = hrtfPanPathRejectReason(hrtf_enabled, rel_listener);
    preflight.rejected = preflight.reason != HrtfPanPathRejectReason::None;
    preflight.reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    preflight.convolutionReason = hrtf_convolution_reject_reason(hrtf_enabled, ir, rel_listener);
    preflight.reason = hrtf_pan_path_reject_reason(hrtf_enabled, ir, rel_listener);
    const HrtfIrPreflight ir_preflight = preflight_hrtf_ir(ir);
    preflight.hrtfDisabled = !hrtf_enabled;
    preflight.coLocated = is_co_located_hrtf_source(rel_listener);
    preflight.hrtfDisabled = preflight.reason == HrtfPanPathRejectReason::HrtfDisabled;
    preflight.coLocated = preflight.reason == HrtfPanPathRejectReason::CoLocated;
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    preflight.malformedIr = is_nonnull_zero_length_hrtf_ir(ir);
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
    preflight.reason = classify_hrtf_pan_path_reject(hrtf_enabled, ir, rel_listener);
    preflight.skipped = preflight.reason != HrtfPanPathRejectReason::None;
    preflight.rejectReason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    preflight.reason = classify_hrtf_pan_path_reject(preflight);
    preflight.reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    preflight.rejectReason = classifyHrtfPanPathReject(hrtf_enabled, rel_listener);
    preflight.reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    preflight.reject = classify_hrtf_pan_path_reject(preflight);
    preflight.rejectReason = classifyHrtfPanPathReject(preflight);
    preflight.convolutionRejectReason =
        classifyHrtfPanConvolutionReject(preflight, preflight_hrtf_ir(ir));
    preflight.rejectReason = classify_hrtf_pan_path_reject_reason(hrtf_enabled, ir, rel_listener);
    preflight.skipped = preflight.should_skip();
    preflight.reason = classifyHrtfPanPathReject(preflight);
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
    preflight.convolveRejectReason = preflight_hrtf_ir(ir).rejectReason;
    preflight.spatialRejectReason = classify_hrtf_pan_path_spatial_reject(hrtf_enabled, rel_listener);
        classify_hrtf_pan_path_convolution_reject(hrtf_enabled, ir, rel_listener);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathPreflight& out, HrtfPanPathRejectReason& reason) {
    out = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    reason = out.reason;
    return out.can_spatial_pan();
    preflight.skipReason = classify_hrtf_pan_path_skip_reject(hrtf_enabled, rel_listener);

                                 HrtfPanPathPreflight& preflight) {
    preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

bool hrtf_pan_path_preflight_skips_for_reason(const HrtfPanPathPreflight& preflight,
                                              HrtfPanPathRejectReason expected) {
    return preflight.skipReason == expected;

bool hrtf_pan_path_preflight_convolution_rejects_for_reason(const HrtfPanPathPreflight& preflight,
    return preflight.convolutionRejectReason == expected;
    preflight.reason = hrtf_pan_path_reject_reason(hrtf_enabled, ir, rel_listener);

HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                                 const Vec3& rel_listener,
                                                 HrtfPanPathRejectReason& reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    reason = preflight.reason;

HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return try_preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);
bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathRejectReason* reason) {

    preflight.rejectReason = classify_hrtf_pan_path_reject(preflight);

    const HrtfPanPathRejectReason reject = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    return reject == HrtfPanPathRejectReason::None;

    preflight.reason = classifyHrtfPanPathReject(hrtf_enabled, ir, rel_listener);

    const HrtfPanPathRejectReason reject =
        classifyHrtfPanPathReject(hrtf_enabled, ir, rel_listener);
    return !hrtfPanPathRejectReasonIsBlocking(reject);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener, &reason);

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
    if (preflight.hrtfDisabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (preflight.coLocated) {
        return HrtfPanPathRejectReason::CoLocated;
    return HrtfPanPathRejectReason::None;
    preflight.convolutionReason =
        classify_hrtf_pan_path_convolution_reject(preflight, ir_preflight);

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {

    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, &reason);


    const HrtfPanPathPreflight preflight =
        preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);


bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, &reason);
    preflight.reason = hrtfPanPathRejectReason(hrtf_enabled, rel_listener);

bool preflightHrtfPanPathReady(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
        *reason = classifyHrtfPanPathReject(preflight);

bool preflightHrtfPanPathReady(bool hrtf_enabled, const Vec3& rel_listener,
    return preflightHrtfPanPathReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    return preflightHrtfPanPathReady(hrtf_enabled, ir, rel_listener, &reason);

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
    return preflightHrtfPanPathReady(hrtf_enabled, rel_listener, &reason);
    preflight.convolveReason = hrtf_pan_path_convolve_reject_reason(hrtf_enabled, ir, rel_listener);

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
        *reason = classify_hrtf_pan_path_reject(preflight);


bool preflight_hrtf_pan_path_convolve_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                            HrtfPanPathConvolveRejectReason* reason) {
        *reason = classify_hrtf_pan_path_convolve_reject(preflight);

bool try_preflight_hrtf_pan_path_convolve(bool hrtf_enabled, const HrtfIrStub& ir,
                                          HrtfPanPathConvolveRejectReason& reason) {
    return preflight_hrtf_pan_path_convolve_ready(hrtf_enabled, ir, rel_listener, &reason);









bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener);

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












                                 HrtfPanPathPreflight& preflight, HrtfPanPathRejectReason& reason) {
    reason = preflight.rejectReason;
    return preflight.can_spatial_pan();







    return try_preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, preflight);
                                 HrtfPanPathPreflight& out, HrtfPanPathRejectReason& reason) {
    out = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    reason = out.rejectReason;
    return out.can_spatial_pan();



bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
        *reason = preflight.reason;

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    reason = preflight.reason;

bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);



    const HrtfPanPathPreflight preflight =
        preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
        *reason = preflight.reject;

                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, &reason);


    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, &reason);
bool hrtf_pan_path_matches_preflight(HrtfPanPath path, const HrtfPanPathPreflight& preflight) {
    return path == preflight.path;
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    return "unknown";

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
    if (!preflight.skipped) {
        return HrtfPanPathRejectReason::None;
    if (preflight.hrtfDisabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (preflight.coLocated) {
        return HrtfPanPathRejectReason::CoLocated;

        *reason = classify_hrtf_pan_path_reject(preflight);




const char* hrtf_pan_convolve_reject_reason_label(HrtfPanConvolveRejectReason reason) {
    case HrtfPanConvolveRejectReason::None:
    case HrtfPanConvolveRejectReason::HrtfDisabled:
    case HrtfPanConvolveRejectReason::CoLocated:
    case HrtfPanConvolveRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfPanConvolveRejectReason::MalformedIr:
        return "malformed_ir";

    if (preflight.can_spatial_pan()) {

HrtfPanConvolveRejectReason classify_hrtf_pan_convolve_reject(const HrtfPanPathPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfPanConvolveRejectReason::None;
        return HrtfPanConvolveRejectReason::HrtfDisabled;
        return HrtfPanConvolveRejectReason::CoLocated;
    if (preflight.malformedIr) {
        return HrtfPanConvolveRejectReason::MalformedIr;
    if (preflight.emptyIr) {
        return HrtfPanConvolveRejectReason::EmptyIr;





bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener);

bool preflight_hrtf_pan_convolve_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                       const Vec3& rel_listener,
                                       HrtfPanConvolveRejectReason* reason) {
        *reason = classify_hrtf_pan_convolve_reject(preflight);

bool try_preflight_hrtf_pan_convolve(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener, HrtfPanConvolveRejectReason& reason) {
    return preflight_hrtf_pan_convolve_ready(hrtf_enabled, ir, rel_listener, &reason);

bool should_skip_hrtf_pan_convolve(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_convolve_ready(hrtf_enabled, ir, rel_listener);






















bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,




bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = preflight.reason;
    return preflight.can_spatial_pan();
    }

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, &reason);
}



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

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener) == expected;
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason) {
    reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    return reason == HrtfPanPathRejectReason::None;
}

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return try_preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "none";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "co_located";
    return "unknown";

bool hrtf_pan_path_reject_reason_blocks_spatial_pan(HrtfPanPathRejectReason reason) {
    return reason != HrtfPanPathRejectReason::None;
        return "None";
        return "HrtfDisabled";
        return "CoLocated";
    return "Unknown";
    case HrtfPanPathRejectReason::EmptyIr:
        return "EmptyIr";

const char* hrtf_convolution_reject_reason_label(HrtfConvolutionRejectReason reason) {
    case HrtfConvolutionRejectReason::None:
    case HrtfConvolutionRejectReason::HrtfDisabled:
    case HrtfConvolutionRejectReason::CoLocated:
    case HrtfConvolutionRejectReason::EmptyIr:
    case HrtfConvolutionRejectReason::MalformedIr:
        return "MalformedIr";

const char* hrtf_pan_convolution_reject_reason_label(HrtfPanConvolutionRejectReason reason) {
    case HrtfPanConvolutionRejectReason::None:
    case HrtfPanConvolutionRejectReason::HrtfDisabled:
    case HrtfPanConvolutionRejectReason::CoLocated:
    case HrtfPanConvolutionRejectReason::EmptyIr:
    case HrtfPanConvolutionRejectReason::MalformedIr:
    default:

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
    if (preflight.hrtfDisabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (preflight.coLocated) {
        return HrtfPanPathRejectReason::CoLocated;
    return HrtfPanPathRejectReason::None;

HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& preflight) {
    if (preflight.emptyIr) {
        return HrtfPanPathRejectReason::EmptyIr;

HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfIrPreflight& ir,
                                                            const HrtfPanPathPreflight& panPath) {
    if (panPath.hrtfDisabled) {
        return HrtfConvolutionRejectReason::HrtfDisabled;
    if (panPath.coLocated) {
        return HrtfConvolutionRejectReason::CoLocated;
    if (ir.malformedIr) {
        return HrtfConvolutionRejectReason::MalformedIr;
    if (ir.emptyIr) {
        return HrtfConvolutionRejectReason::EmptyIr;
    return HrtfConvolutionRejectReason::None;
const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason) {

    if (!preflight.skipped) {

    if (preflight.can_spatial_pan()) {

    if (preflight.can_convolve()) {

HrtfPanPathRejectReason classify_hrtf_pan_path_spatial_reject(const HrtfPanPathPreflight& preflight) {



bool hrtf_pan_path_rejects_for_reason(const HrtfPanPathPreflight& preflight,
    return classify_hrtf_pan_path_reject(preflight) == expected;



    const HrtfPanPathRejectReason spatial_reject = classify_hrtf_pan_path_reject(preflight);
    if (spatial_reject != HrtfPanPathRejectReason::None) {
        return spatial_reject;

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = preflight.reason;
    return preflight.can_spatial_pan();
        *reason = classify_hrtf_pan_path_reject(preflight);

    const HrtfPanPathPreflight preflight =
        preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason) {

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, &reason);


bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {








    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, &reason);

bool preflight_hrtf_pan_spatial_ready(const HrtfPanPathPreflight& preflight,

bool preflight_hrtf_pan_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_pan_spatial_ready(
        preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener), reason);

bool preflight_hrtf_pan_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                            reason);

bool try_preflight_hrtf_pan_spatial(const HrtfPanPathPreflight& preflight,
    return preflight_hrtf_pan_spatial_ready(preflight, &reason);

bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_pan_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_spatial_ready(hrtf_enabled, rel_listener, &reason);

bool should_skip_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_spatial_ready(hrtf_enabled, rel_listener);





        *reason = classify_hrtf_pan_path_spatial_reject(preflight);














bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener);

bool preflight_hrtf_pan_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener,
        *reason = classify_hrtf_pan_path_convolution_reject(preflight);
    return preflight.can_convolve();

bool try_preflight_hrtf_pan_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_pan_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);

bool should_skip_hrtf_pan_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_convolution_ready(hrtf_enabled, ir, rel_listener);

bool preflight_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                      HrtfConvolutionRejectReason* reason) {
    const HrtfIrPreflight ir_preflight = preflight_hrtf_ir(ir);
    const HrtfPanPathPreflight pan_preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
        *reason = classify_hrtf_convolution_reject(ir_preflight, pan_preflight);
    return pan_preflight.can_convolve();

bool try_preflight_hrtf_convolution(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                    HrtfConvolutionRejectReason& reason) {
    return preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);

    reason = preflight.reason;


HrtfPanConvolutionRejectReason classify_hrtf_pan_convolution_reject(
    const HrtfPanPathPreflight& pan_preflight, const HrtfIrPreflight& ir_preflight) {
    if (pan_preflight.hrtfDisabled) {
        return HrtfPanConvolutionRejectReason::HrtfDisabled;
    if (pan_preflight.coLocated) {
        return HrtfPanConvolutionRejectReason::CoLocated;
    if (ir_preflight.malformedIr) {
        return HrtfPanConvolutionRejectReason::MalformedIr;
    if (ir_preflight.emptyIr) {
        return HrtfPanConvolutionRejectReason::EmptyIr;
    return HrtfPanConvolutionRejectReason::None;





                                          HrtfPanConvolutionRejectReason* reason) {
        *reason = classify_hrtf_pan_convolution_reject(pan_preflight, ir_preflight);

                                        HrtfPanConvolutionRejectReason& reason) {


bool preflight_hrtf_spatial_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,

bool preflight_hrtf_spatial_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_spatial_pan_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,

bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_spatial_pan_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_spatial_pan_ready(hrtf_enabled, rel_listener, &reason);

bool preflight_hrtf_pan_path_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               HrtfIrRejectReason* reason) {
        if (!preflight.can_convolve()) {
            *reason = classify_hrtf_ir_reject(preflight_hrtf_ir(ir));
        } else {
            *reason = HrtfIrRejectReason::None;

bool try_preflight_hrtf_pan_path_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, HrtfIrRejectReason& reason) {
    return preflight_hrtf_pan_path_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);




bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,

bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener) {





HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfPanPathPreflight& pan_path,
                                                             const HrtfIrPreflight& ir) {
    if (pan_path.can_convolve()) {
    if (pan_path.hrtfDisabled) {
    if (pan_path.coLocated) {





bool should_skip_hrtf_spatial_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_spatial_pan_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_spatial_pan_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_spatial_pan_ready(hrtf_enabled, rel_listener);

    const HrtfPanPathPreflight pan_path = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
        *reason = classify_hrtf_convolution_reject(pan_path, ir_preflight);
    return pan_path.can_convolve();

bool try_preflight_hrtf_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                    const Vec3& rel_listener, HrtfConvolutionRejectReason& reason) {

bool should_skip_hrtf_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener);

















    case HrtfConvolutionRejectReason::NullSamples:
        return "NullSamples";
    case HrtfConvolutionRejectReason::ZeroLength:
        return "ZeroLength";

    if (ir.nullSamples) {
        return HrtfConvolutionRejectReason::NullSamples;
    if (ir.zeroLength) {
        return HrtfConvolutionRejectReason::ZeroLength;

HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_convolution_reject(preflight.panPath, preflight.ir);

                                      const Vec3& rel_listener, HrtfConvolutionRejectReason* reason) {


bool should_skip_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,





bool should_skip_hrtf_pan_spatial_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_spatial_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_pan_spatial_preflight(bool hrtf_enabled, const Vec3& rel_listener) {







bool preflight_spatial_hrtf_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,

const char* hrtf_pan_path_convolution_reject_reason_label(HrtfPanPathConvolutionRejectReason reason) {
    case HrtfPanPathConvolutionRejectReason::None:
    case HrtfPanPathConvolutionRejectReason::HrtfDisabled:
    case HrtfPanPathConvolutionRejectReason::CoLocated:
    case HrtfPanPathConvolutionRejectReason::EmptyIr:
    case HrtfPanPathConvolutionRejectReason::NullSamples:
    case HrtfPanPathConvolutionRejectReason::ZeroLength:
    case HrtfPanPathConvolutionRejectReason::MalformedIr:


HrtfPanPathConvolutionRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& panPath, const HrtfIrPreflight& ir) {
    if (panPath.can_convolve()) {
        return HrtfPanPathConvolutionRejectReason::None;
        return HrtfPanPathConvolutionRejectReason::HrtfDisabled;
        return HrtfPanPathConvolutionRejectReason::CoLocated;
    switch (classify_hrtf_ir_reject(ir)) {
    case HrtfIrRejectReason::MalformedIr:
        return HrtfPanPathConvolutionRejectReason::MalformedIr;
    case HrtfIrRejectReason::NullSamples:
        return HrtfPanPathConvolutionRejectReason::NullSamples;
    case HrtfIrRejectReason::ZeroLength:
        return HrtfPanPathConvolutionRejectReason::ZeroLength;
    case HrtfIrRejectReason::EmptyIr:
        return HrtfPanPathConvolutionRejectReason::EmptyIr;
    case HrtfIrRejectReason::None:
        break;


bool preflight_spatial_hrtf_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_spatial_hrtf_pan_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,

bool try_preflight_spatial_hrtf_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_spatial_hrtf_pan_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_spatial_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_spatial_hrtf_pan_ready(hrtf_enabled, rel_listener, &reason);

bool should_skip_spatial_hrtf_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_spatial_hrtf_pan_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_spatial_hrtf_pan_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_spatial_hrtf_pan_ready(hrtf_enabled, rel_listener);









                                               HrtfPanPathConvolutionRejectReason* reason) {
    const HrtfPanPathPreflight panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    const HrtfIrPreflight irPreflight = preflight_hrtf_ir(ir);
        *reason = classify_hrtf_pan_path_convolution_reject(panPath, irPreflight);
    return panPath.can_convolve();

                                             HrtfPanPathConvolutionRejectReason& reason) {

bool should_skip_hrtf_pan_path_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_path_convolution_ready(hrtf_enabled, ir, rel_listener);

    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,


const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason) {

HrtfPanPathRejectReason classifyHrtfPanPathReject(const HrtfPanPathPreflight& preflight) {

        *reason = classifyHrtfPanPathReject(preflight);










HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    return preflight.path;

HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return resolve_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool resolve_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPath* path, HrtfPanPathRejectReason* reason) {
    if (path != nullptr) {
        *path = preflight.path;

bool resolve_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener, HrtfPanPath* path,
    return resolve_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, path,



HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
    if (is_co_located_hrtf_source(rel_listener)) {

    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(const HrtfPanPathPreflight& preflight) {
    return preflight.reason;

    return preflight.reason == expected;














bool hrtf_pan_path_reject_reason_is_blocking(HrtfPanPathRejectReason reason) {

bool preflight_spatial_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfPanPathRejectReason reject = classify_hrtf_pan_path_reject(preflight);
        *reason = reject;
    return !hrtf_pan_path_reject_reason_is_blocking(reject);

bool try_preflight_spatial_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_spatial_hrtf_pan_path(hrtf_enabled, ir, rel_listener, &reason);

bool preflight_spatial_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,




    return preflight_spatial_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);










    const HrtfPanPathRejectReason reject =
        classify_hrtf_pan_path_reject(preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener));
    return reject == HrtfPanPathRejectReason::None;


    reason = classify_hrtf_pan_path_reject(preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener));

        return "empty_ir";


    const HrtfPanPathRejectReason spatial_reject = classify_hrtf_pan_path_spatial_reject(preflight);

bool preflight_hrtf_pan_path_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,

bool preflight_hrtf_pan_path_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,

bool try_preflight_hrtf_pan_path_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_pan_path_spatial(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, rel_listener, &reason);



bool can_apply_spatial_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener,
    const HrtfPanPathRejectReason reject_reason =
        hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
        *reason = reject_reason;
    return reject_reason == HrtfPanPathRejectReason::None;

    reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
const char* hrtf_pan_spatial_reject_reason_label(HrtfPanSpatialRejectReason reason) {
    case HrtfPanSpatialRejectReason::None:
    case HrtfPanSpatialRejectReason::HrtfDisabled:
    case HrtfPanSpatialRejectReason::CoLocated:


HrtfPanSpatialRejectReason classify_hrtf_pan_spatial_reject(const HrtfPanPathPreflight& preflight) {
        return HrtfPanSpatialRejectReason::HrtfDisabled;
        return HrtfPanSpatialRejectReason::CoLocated;
    return HrtfPanSpatialRejectReason::None;


bool hrtf_pan_spatial_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfPanSpatialRejectReason expected) {
    return classify_hrtf_pan_spatial_reject(preflight_hrtf_pan_path(hrtf_enabled, rel_listener))
        == expected;

bool hrtf_pan_convolution_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                             HrtfPanConvolutionRejectReason expected) {
    return classify_hrtf_pan_convolution_reject(
               preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener))

                                      HrtfPanSpatialRejectReason* reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, rel_listener);
        *reason = classify_hrtf_pan_spatial_reject(preflight);


                                    HrtfPanSpatialRejectReason& reason) {

        *reason = classify_hrtf_pan_convolution_reject(preflight);







bool should_skip_hrtf_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,

bool should_skip_hrtf_pan_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
const char* hrtfPanPathRejectReasonName(HrtfPanPathRejectReason reason) {


bool hrtfPanPathRejectsForReason(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    return classifyHrtfPanPathReject(preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener))

bool preflightHrtfPanPathReady(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,

bool preflightHrtfPanPathReady(bool hrtf_enabled, const Vec3& rel_listener,
    return preflightHrtfPanPathReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    return preflightHrtfPanPathReady(hrtf_enabled, ir, rel_listener, &reason);

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
    return preflightHrtfPanPathReady(hrtf_enabled, rel_listener, &reason);
    }


bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason) {

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,



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

HrtfPanPathResolution resolve_hrtf_pan_path_diagnostic(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    HrtfPanPathResolution resolution{};
    resolution.preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    resolution.path = resolution.preflight.path;
    return resolution;
}

HrtfPanPathResolution resolve_hrtf_pan_path_diagnostic(bool hrtf_enabled, const Vec3& rel_listener) {
    return resolve_hrtf_pan_path_diagnostic(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason) {
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

const char* hrtfPanConvolutionRejectReasonLabel(HrtfPanConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfPanConvolutionRejectReason::None:
        return "none";
    case HrtfPanConvolutionRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfPanConvolutionRejectReason::CoLocated:
        return "co_located";
    case HrtfPanConvolutionRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfPanConvolutionRejectReason::EmptyIr:
        return "empty_ir";
    }
    return "unknown";
}

HrtfPanPathRejectReason classifyHrtfPanPathReject(const HrtfPanPathPreflight& preflight) {
    if (!preflight.skipped) {
        return HrtfPanPathRejectReason::None;
    }
    if (preflight.hrtfDisabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (preflight.coLocated) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

HrtfPanConvolutionRejectReason classifyHrtfPanConvolutionReject(const HrtfPanPathPreflight& panPath,
                                                               const HrtfIrPreflight& ir) {
    if (panPath.can_convolve()) {
        return HrtfPanConvolutionRejectReason::None;
    }
    if (panPath.hrtfDisabled) {
        return HrtfPanConvolutionRejectReason::HrtfDisabled;
    }
    if (panPath.coLocated) {
        return HrtfPanConvolutionRejectReason::CoLocated;
    }
    if (ir.malformedIr) {
        return HrtfPanConvolutionRejectReason::MalformedIr;
    }
    if (panPath.emptyIr || ir.emptyIr) {
        return HrtfPanConvolutionRejectReason::EmptyIr;
    }
    return HrtfPanConvolutionRejectReason::None;
}

bool preflightHrtfPanPathReady(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                               HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = preflight.rejectReason;
    }
    return preflight.can_spatial_pan();
}

bool preflightHrtfPanPathReady(bool hrtf_enabled, const Vec3& rel_listener,
                               HrtfPanPathRejectReason* reason) {
    return preflightHrtfPanPathReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);
}

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             HrtfPanPathRejectReason& reason) {
    return preflightHrtfPanPathReady(hrtf_enabled, ir, rel_listener, &reason);
}

bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
                             HrtfPanPathRejectReason& reason) {
    return preflightHrtfPanPathReady(hrtf_enabled, rel_listener, &reason);
}

bool preflightHrtfPanConvolutionReady(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener,
                                      HrtfPanConvolutionRejectReason* reason) {
    const HrtfPanPathPreflight panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = panPath.convolutionRejectReason;
    }
    return panPath.can_convolve();
}

bool tryPreflightHrtfPanConvolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                    const Vec3& rel_listener,
                                    HrtfPanConvolutionRejectReason& reason) {
    return preflightHrtfPanConvolutionReady(hrtf_enabled, ir, rel_listener, &reason);
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

bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPath& outPath, HrtfPanPathRejectReason& outReason) {
    outPath = resolve_hrtf_pan_path(hrtf_enabled, rel_listener);
    if (!hrtf_enabled) {
        outReason = HrtfPanPathRejectReason::Disabled;
        return false;
    if (is_co_located_hrtf_source(rel_listener)) {
        outReason = HrtfPanPathRejectReason::CoLocated;

    outReason = HrtfPanPathRejectReason::None;
    return true;

bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                HrtfPanPath* out_path, HrtfPanPathRejectReason* reason) {
    HrtfPanPath path = HrtfPanPath::Bypass;
    HrtfPanPathRejectReason localReason = HrtfPanPathRejectReason::None;
    const bool ok = try_preflight_hrtf_spatial_pan(hrtf_enabled, rel_listener, path, localReason);
    if (out_path != nullptr) {
        *out_path = path;
    if (reason != nullptr) {
        *reason = localReason;
    return ok;

bool preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                const Vec3& rel_listener, HrtfPanPath* out_path,
                                HrtfPanPathRejectReason* reason) {
    HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

            *reason = HrtfPanPathRejectReason::Disabled;
            *reason = HrtfPanPathRejectReason::CoLocated;

        *reason = HrtfPanPathRejectReason::None;

    case HrtfPanPathRejectReason::HrtfDisabled:
        return "HrtfDisabled";

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled,
        return HrtfPanPathRejectReason::HrtfDisabled;
        return HrtfPanPathRejectReason::CoLocated;
    return HrtfPanPathRejectReason::None;

namespace {

HrtfPanPathPreflight make_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfPanPathPreflight preflight{};
    preflight.bypassed = is_hrtf_pan_path_bypass(preflight.path);
    preflight.uses_convolution = hrtf_pan_path_uses_convolution(preflight.path);
    preflight.uses_ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(preflight.path);
    if (preflight.bypassed) {
        preflight.reject_reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);

} // namespace

    return make_hrtf_pan_path_preflight(hrtf_enabled, ir, rel_listener);

    return make_hrtf_pan_path_preflight(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    const HrtfPanPathPreflight preflight =
        make_hrtf_pan_path_preflight(hrtf_enabled, ir, rel_listener);
        *reason = preflight.reject_reason;
    return preflight.can_spatial_pan();


HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
        return HrtfPanPathRejectReason::Disabled;

    preflight.rejectReason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);


    const HrtfPanPathRejectReason reject = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
        *reason = reject;
    return reject == HrtfPanPathRejectReason::None;

    preflight.hrtf_disabled = !hrtf_enabled;
    preflight.co_located = is_co_located_hrtf_source(rel_listener);
    preflight.empty_ir = is_empty_hrtf_ir(ir);


bool can_apply_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener).can_apply_spatial_pan();









bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !hrtf_enabled || is_co_located_hrtf_source(rel_listener);

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, rel_listener);

bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan(hrtf_enabled, rel_listener);

bool is_hrtf_pan_bypassed(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
    return path == HrtfPanPath::Bypass;
bool is_hrtf_pan_path_bypass(HrtfPanPath path) {

bool is_bypass_hrtf_pan_path(HrtfPanPath path) {

bool should_skip_hrtf_spatial_pan(HrtfPanPath path) {

bool should_apply_hrtf_spatial_pan(HrtfPanPath path) {
    return !should_skip_hrtf_spatial_pan(path);


bool should_skip_hrtf_pan_path(HrtfPanPath path) {
    return is_hrtf_pan_bypassed(path);
    HrtfPanPathPreflight result;
    result.hrtf_disabled = !hrtf_enabled;
    result.co_located = is_co_located_hrtf_source(rel_listener);
    result.empty_ir = is_empty_hrtf_ir(ir);
    result.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return result;
    preflight.bypass = is_hrtf_pan_path_bypass(preflight.path);
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

    preflight.emptyIr = is_empty_hrtf_ir(ir);


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

    return !should_skip_hrtf_pan(hrtf_enabled, rel_listener);
    return hrtf_enabled && !is_co_located_hrtf_source(rel_listener);

bool hrtf_pan_path_uses_ild_stub(HrtfPanPath path) {


    result.bypass = is_hrtf_pan_path_bypass(result.path);
    result.spatial = is_spatial_hrtf_pan_path(result.path);
    result.uses_convolution = hrtf_pan_path_uses_convolution(result.path);
    result.uses_ild_itd_stub = hrtf_pan_path_uses_ild_itd_stub(result.path);


bool can_apply_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener) {

    result.ir_preflight = preflight_hrtf_ir(ir);
    result.empty_ir = result.ir_preflight.is_empty();


bool can_apply_hrtf_spatial_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPath preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {

    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);


    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener) == HrtfPanPath::Convolution;
bool can_apply_hrtf_spatial_pan(HrtfPanPath path) {
    return is_spatial_hrtf_pan_path(path);




bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPreflightRejectReason* reason) {
            *reason = HrtfPanPreflightRejectReason::Disabled;
            *reason = HrtfPanPreflightRejectReason::CoLocated;
        *reason = HrtfPanPreflightRejectReason::None;

bool preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return try_preflight_hrtf_pan_path(hrtf_enabled, rel_listener, nullptr);

bool preflight_hrtf_pan_path(HrtfPanPath path) {

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
        return "none";
        return "bypass_path";
        return "unity_attenuation";
    }
    return "unknown";

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(





















    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    (void)coupling;
    (void)params;

const char* hrtfAttenuationCouplingRejectReasonName(HrtfAttenuationCouplingRejectReason reason) {

HrtfAttenuationCouplingRejectReason hrtfAttenuationCouplingRejectReason(

bool hrtfAttenuationCouplingRejectsForReason(HrtfPanPath path, float distance_attenuation,
    return hrtfAttenuationCouplingRejectReason(path, distance_attenuation, occlusion_gain)












bool hrtf_attenuation_coupling_rejects_for_reason(
    HrtfAttenuationCouplingRejectReason expected, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain,
                                                   coupling, params)
        == HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
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
    return "unknown";

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    return HrtfAttenuationCouplingRejectReason::None;

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;

        return "None";
        return "BypassPath";
        return "UnityAttenuation";
    return "Unknown";







bool hrtf_attenuation_coupling_reject_reason_is_blocking(HrtfAttenuationCouplingRejectReason reason) {
    return reason != HrtfAttenuationCouplingRejectReason::None;


bool preflight_hrtf_attenuation_coupling_narrowing(HrtfPanPath path, float distance_attenuation,
                                                   HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    return !hrtf_attenuation_coupling_reject_reason_is_blocking(reject);



    default:



const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason) {

    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    (void)coupling;
    (void)params;

bool hrtf_attenuation_coupling_rejects_for_reason(
    HrtfAttenuationCouplingRejectReason expected, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain,
                                                     coupling, params)



    return classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain)





bool is_blocking_hrtf_attenuation_coupling_reject_reason(HrtfAttenuationCouplingRejectReason reason) {














    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
    if (preflight.unityAttenuation) {




    if (preflight.can_narrow()) {




bool hrtfAttenuationCouplingRejectReasonIsBlocking(HrtfAttenuationCouplingRejectReason reason) {

HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(

bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                         HrtfAttenuationCouplingRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
    reason = classifyHrtfAttenuationCouplingReject(path, distance_attenuation, occlusion_gain,
                                                   coupling, params);
    return !hrtfAttenuationCouplingRejectReasonIsBlocking(reason);
















    return preflight.reason;


    const HrtfAttenuationCouplingPreflight& preflight,
    return classify_hrtf_attenuation_coupling_reject(preflight) == expected;


    if (!preflight.skipped) {

const char* hrtf_attenuation_coupling_reject_reason_name(
    HrtfAttenuationCouplingRejectReason reason) {








bool hrtf_attenuation_coupling_rejects_for_reason(const HrtfAttenuationCouplingPreflight& preflight,



const char* hrtfAttenuationCouplingRejectReasonName(HrtfAttenuationCouplingRejectReason reason) {

HrtfAttenuationCouplingRejectReason hrtfAttenuationCouplingRejectReason(


bool hrtfAttenuationCouplingRejectsForReason(HrtfPanPath path, float distance_attenuation,
    return hrtfAttenuationCouplingRejectReason(path, distance_attenuation, occlusion_gain)




HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject_reason(

bool preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
                                          HrtfAttenuationCouplingRejectReason* reason,
        classify_hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain,
    return reject == HrtfAttenuationCouplingRejectReason::None;

bool try_preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_attenuation_narrowing(path, distance_attenuation, occlusion_gain, &reason,







}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
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

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.reason;
}

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;
}

bool hrtf_attenuation_coupling_rejects_for_reason(const HrtfAttenuationCouplingPreflight& preflight,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return classify_hrtf_attenuation_coupling_reject(preflight) == expected;
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

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
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
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
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
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    case HrtfAttenuationCouplingRejectReason::None:
    default:
        return "none";
    }
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
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
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
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
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    }
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::BypassPath;
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
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    }
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.reason =
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
        hrtfAttenuationCouplingRejectReason(path, distance_attenuation, occlusion_gain);
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain, coupling,
                                                params);
    preflight.rejected = preflight.reason != HrtfAttenuationCouplingRejectReason::None;
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    preflight.bypassPath = is_hrtf_pan_path_bypass(path);
    preflight.bypassPath =
        preflight.reason == HrtfAttenuationCouplingRejectReason::BypassPath;
    preflight.bypassPath = preflight.reason == HrtfAttenuationCouplingRejectReason::BypassPath;
    preflight.unityAttenuation =
        preflight.reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.bypassPath || preflight.unityAttenuation;
    preflight.reason = classify_hrtf_attenuation_coupling_reject(path, distance_attenuation,
                                                                 occlusion_gain);
    preflight.skipped = preflight.reason != HrtfAttenuationCouplingRejectReason::None;
    preflight.rejectReason =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain,
                                                  coupling, params);
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    preflight.reason = classify_hrtf_attenuation_coupling_reject(preflight);
    preflight.rejectReason = classifyHrtfAttenuationCouplingReject(
        path, distance_attenuation, occlusion_gain, coupling, params);
    preflight.reason =
        classifyHrtfAttenuationCouplingReject(path, distance_attenuation, occlusion_gain);
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
    preflight.reject = classify_hrtf_attenuation_coupling_reject(preflight);
        hrtfAttenuationCouplingRejectReason(path, distance_attenuation, occlusion_gain);
    preflight.reason = classifyHrtfAttenuationCouplingReject(preflight);
        classify_hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain,
    preflight.skipped = preflight.should_skip();
    return preflight;

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingPreflight& preflight, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    preflight = preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
    return preflight.can_narrow();
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                          float occlusion_gain,
                                          HrtfAttenuationCouplingRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    return reject == HrtfAttenuationCouplingRejectReason::None;

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                              HrtfAttenuationCouplingRejectReason& reason,
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, &reason,









    HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason& reason,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    reason = preflight.rejectReason;



bool can_narrow_hrtf_spatial_image(const HrtfAttenuationCouplingPreflight& preflight) {

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
    const HrtfPanPathPreflight pan = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return preflight_hrtf_attenuation_coupling(pan.path, distance_attenuation, occlusion_gain,

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    default:

                                             HrtfAttenuationCouplingRejectReason& outReason) {
    if (should_skip_hrtf_attenuation_coupling(path)) {
        outReason = HrtfAttenuationCouplingRejectReason::BypassPath;
        return false;
        outReason = HrtfAttenuationCouplingRejectReason::UnityAttenuation;

    outReason = HrtfAttenuationCouplingRejectReason::None;
    return true;

                                         HrtfAttenuationCouplingRejectReason* reason) {
    HrtfAttenuationCouplingRejectReason localReason = HrtfAttenuationCouplingRejectReason::None;
    const bool ok =
        try_preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                localReason);
        *reason = localReason;
    return ok;
                                             HrtfAttenuationCouplingPreflightRejectReason* reason) {
    if (!preflight_hrtf_pan_path(path)) {
            *reason = HrtfAttenuationCouplingPreflightRejectReason::BypassPath;
            *reason = HrtfAttenuationCouplingPreflightRejectReason::UnityAttenuation;
        *reason = HrtfAttenuationCouplingPreflightRejectReason::None;

    return try_preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                   nullptr);


    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::PanBypassed:
        return "PanBypassed";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
        return "Unknown";

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
        return HrtfAttenuationCouplingRejectReason::PanBypassed;
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    return HrtfAttenuationCouplingRejectReason::None;

namespace {

HrtfAttenuationCouplingPreflight make_hrtf_attenuation_coupling_preflight(
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.bypass_path = should_skip_hrtf_attenuation_coupling(path);
        !preflight.bypass_path
        && should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.spatial_blend =
    preflight.reject_reason =
    preflight.skipped = preflight.reject_reason != HrtfAttenuationCouplingRejectReason::None;

} // namespace

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    return make_hrtf_attenuation_coupling_preflight(path, distance_attenuation, occlusion_gain,

    const HrtfAttenuationCouplingPreflight preflight =
        make_hrtf_attenuation_coupling_preflight(path, distance_attenuation, occlusion_gain,
        *reason = preflight.reject_reason;
    return preflight.can_apply_coupling();

HrtfAttenuationCouplingSkipReason classify_hrtf_attenuation_coupling_skip(
        return HrtfAttenuationCouplingSkipReason::BypassPath;
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingSkipReason::UnityAttenuation;
        return HrtfAttenuationCouplingSkipReason::UnitySpatialBlend;
    return HrtfAttenuationCouplingSkipReason::None;

bool hrtf_attenuation_coupling_skip_reason_is_blocking(HrtfAttenuationCouplingSkipReason reason) {
    return reason != HrtfAttenuationCouplingSkipReason::None;

                                       HrtfAttenuationCouplingSkipReason* reason,
    const HrtfAttenuationCouplingSkipReason skip =
        classify_hrtf_attenuation_coupling_skip(path, distance_attenuation, occlusion_gain, coupling,
        *reason = skip;
    return !hrtf_attenuation_coupling_skip_reason_is_blocking(skip);

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(
    preflight.reason =
    preflight.skipped = hrtf_attenuation_coupling_skip_reason_is_blocking(preflight.reason);

        is_unity_hrtf_attenuation(preflight.distance_attenuation, preflight.occlusion_gain);
    preflight.spatial_blend = compute_hrtf_spatial_blend(preflight.distance_attenuation,
                                                         preflight.occlusion_gain, coupling, params);
    preflight.unity_spatial_blend = is_unity_hrtf_spatial_blend(preflight.spatial_blend);

    if (preflight.bypass_path) {
        preflight.skip_reason = HrtfAttenuationCouplingSkipReason::BypassPath;
    } else if (preflight.unity_attenuation || preflight.unity_spatial_blend) {
        preflight.skip_reason = HrtfAttenuationCouplingSkipReason::UnityAttenuation;

bool hrtf_attenuation_coupling_skips_for_reason(HrtfPanPath path, float distance_attenuation,
                                                HrtfAttenuationCouplingSkipReason expected,
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                             params)
               .skip_reason
        == expected;

        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.would_narrow = should_narrow_hrtf_spatial_image(path, distance_attenuation,

bool can_apply_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
        .can_apply_coupling();

HrtfAttenuationCouplingPreflight preflightHrtfAttenuationCoupling(
    preflight.bypassPath = should_skip_hrtf_attenuation_coupling(path);
    preflight.bypassPath = preflight.reason == HrtfAttenuationCouplingRejectReason::BypassPath;
    preflight.unityAttenuation =
        preflight.reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
                                                                 occlusion_gain, coupling, params);
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);

HrtfAttenuationCouplingPreflight try_preflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    reason = preflight.reason;

bool canApplyHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                     float occlusion_gain) {
    return should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);

    result.bypass_pan = is_hrtf_pan_path_bypass(path);
    result.spatial_blend =
    result.unity_spatial_blend = is_unity_hrtf_spatial_blend(result.spatial_blend);

















HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
    HrtfBinauralPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuation = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.panPath.skipped;

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params);

HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, const HrtfIrStub& ir,
    preflight.panPath.path = path;
    preflight.panPath.emptyIr = is_empty_hrtf_ir(ir);
    preflight.panPath.skipped = should_skip_hrtf_pan_path(path);
    preflight.attenuation =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling, params);

HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_binaural_for_path(path, make_empty_hrtf_ir(), distance_attenuation,

bool can_apply_binaural_hrtf_pan(const HrtfBinauralPreflight& preflight) {
    return preflight.can_spatial_pan();

bool can_convolve_binaural_hrtf(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();

bool can_narrow_binaural_hrtf_spatial_image(const HrtfBinauralPreflight& preflight) {


    preflight.skipped = preflight.should_skip();

    HrtfAttenuationCouplingPreflight& out, HrtfAttenuationCouplingRejectReason& reason,
    out = preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
    reason = out.reason;
    return out.can_narrow();
                                             HrtfAttenuationCouplingPreflight& preflight,

bool hrtf_attenuation_coupling_preflight_rejects_for_reason(
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason expected) {
    return preflight.reason == expected;

                                         const BinauralPanParams& params,
        *reason = preflight.reason;

                                             HrtfAttenuationCouplingRejectReason& reason) {
                                             params, &reason);
bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingPreflight& preflight, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    preflight = preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                    coupling, params);
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingPreflight& out, HrtfAttenuationCouplingRejectReason& reason,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    out = preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                              params);
    reason = out.rejectReason;
    return out.can_narrow();
}

bool preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params,
                                         HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfAttenuationCouplingRejectReason::None;
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfAttenuationCouplingRejectReason& reason) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                               params, &reason);
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
        classifyHrtfAttenuationCouplingReject(path, distance_attenuation, occlusion_gain);
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

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params,
    HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool tryPreflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    reason = preflight.reason;
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

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               float occlusion_gain,
                                               HrtfAttenuationCouplingRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reject;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(
        path, distance_attenuation, occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool preflightHrtfAttenuationCouplingReady(HrtfPanPath path, float distance_attenuation,
                                           float occlusion_gain,
                                           HrtfAttenuationCouplingRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classifyHrtfAttenuationCouplingReject(preflight);
    }
    return preflight.can_narrow();
}

bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         HrtfAttenuationCouplingRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflightHrtfAttenuationCouplingReady(path, distance_attenuation, occlusion_gain,
                                                 &reason, coupling, params);
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
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    }
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        *reason = classify_hrtf_attenuation_coupling_reject(preflight);
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason) {
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

HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    }
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool preflightHrtfAttenuationCouplingReady(HrtfPanPath path, float distance_attenuation,
                                           float occlusion_gain,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params,
                                           HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params,
                                         HrtfAttenuationCouplingRejectReason& reason) {
    return preflightHrtfAttenuationCouplingReady(path, distance_attenuation, occlusion_gain,
                                                 coupling, params, &reason);
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
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    }
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_attenuation_coupling_reject(preflight);
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool should_skip_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                                 float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    return !preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                      nullptr, coupling, params);
}

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

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
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

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.reason;
}

bool hrtf_attenuation_coupling_rejects_for_reason(const HrtfAttenuationCouplingPreflight& preflight,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return preflight.reason == expected;
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                          params);
    if (reason != nullptr) {
        *reason = classify_hrtf_attenuation_coupling_reject(preflight);
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
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

const char* hrtfAttenuationCouplingRejectReasonName(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool hrtfAttenuationCouplingRejectsForReason(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason expected,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return classifyHrtfAttenuationCouplingReject(
               preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                 coupling, params))
        == expected;
}

bool preflightHrtfAttenuationCouplingReady(HrtfPanPath path, float distance_attenuation,
                                           float occlusion_gain,
                                           HrtfAttenuationCouplingRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
                                         float occlusion_gain,
                                         HrtfAttenuationCouplingRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflightHrtfAttenuationCouplingReady(path, distance_attenuation, occlusion_gain,
                                                 &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               float occlusion_gain,
                                               HrtfAttenuationCouplingRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               float occlusion_gain,
                                               HrtfAttenuationCouplingRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params,
    HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     coupling, params, &reason);
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

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               float occlusion_gain,
                                               HrtfAttenuationCouplingRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                   &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               float occlusion_gain,
                                               HrtfAttenuationCouplingRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(
        path, distance_attenuation, occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
                                                      float occlusion_gain,
                                                      const HrtfAttenuationCoupling& coupling,
                                                      const BinauralPanParams& params) {
    return !preflight_hrtf_attenuation_coupling_ready(
        path, distance_attenuation, occlusion_gain, nullptr, coupling, params);
}

bool can_narrow_hrtf_spatial_image(const HrtfAttenuationCouplingPreflight& preflight) {
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
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    return "Unknown";

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    return HrtfAttenuationCouplingRejectReason::None;

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected,
                                                  const HrtfAttenuationCoupling& coupling,
                                                  const BinauralPanParams& params) {
    return classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain,
                                                     coupling, params) == expected;
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             HrtfAttenuationCouplingRejectReason& reason,
    (void)coupling;
    (void)params;
    reason = classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    return reason == HrtfAttenuationCouplingRejectReason::None;
const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "none";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    return "unknown";

bool hrtf_attenuation_coupling_reject_reason_blocks_narrowing(
    HrtfAttenuationCouplingRejectReason reason) {
    return reason != HrtfAttenuationCouplingRejectReason::None;

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                               const BinauralPanParams& params,
                                               HrtfAttenuationCouplingRejectReason* reason) {
bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params,
        return "None";
        return "BypassPath";
        return "UnityAttenuation";
    return "Unknown";

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;





                                               HrtfAttenuationCouplingRejectReason* reason,


    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,

    if (!preflight.skipped) {


bool preflight_hrtf_attenuation_narrowing_ready(

    default:


bool hrtf_attenuation_coupling_rejects_for_reason(const HrtfAttenuationCouplingPreflight& preflight,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return classify_hrtf_attenuation_coupling_reject(preflight) == expected;



bool preflight_hrtf_attenuation_coupling_narrow_ready(




    return classify_hrtf_attenuation_coupling_reject(
               preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                   coupling, params))
        == expected;
}

                                               float occlusion_gain,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {



HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(

bool tryCanNarrowHrtfSpatialImage(HrtfPanPath path, float distance_attenuation, float occlusion_gain,
                                  HrtfAttenuationCouplingRejectReason& reason,
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    reason = classifyHrtfAttenuationCouplingReject(preflight);
    return preflight.can_narrow();

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    return preflight.can_narrow();

    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     coupling, params, &reason);


const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {


    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
        *reason = classify_hrtf_attenuation_coupling_reject(preflight);

bool try_preflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    return preflight_hrtf_attenuation_coupling_ready(
        path, distance_attenuation, occlusion_gain, &reason, coupling, params);

    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason* reason) {

bool preflight_hrtf_attenuation_narrowing_ready(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_attenuation_narrowing_ready(
                                            params),
        reason);

bool try_preflight_hrtf_attenuation_narrowing(
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason& reason) {
    return preflight_hrtf_attenuation_narrowing_ready(preflight, &reason);

bool try_preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_attenuation_narrowing_ready(path, distance_attenuation, occlusion_gain,
                                                      &reason, coupling, params);

bool should_skip_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
    return !preflight_hrtf_attenuation_narrowing_ready(path, distance_attenuation, occlusion_gain,
                                                       nullptr, coupling, params);


        path, distance_attenuation, occlusion_gain, coupling, params, &reason);


    return !preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,






    reason = preflight.reason;





bool should_skip_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,


bool preflight_hrtf_narrowing_ready(HrtfPanPath path, float distance_attenuation, float occlusion_gain,

bool try_preflight_hrtf_narrowing(HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    return preflight_hrtf_narrowing_ready(path, distance_attenuation, occlusion_gain, &reason,
                                          coupling, params);

bool should_skip_hrtf_narrowing_preflight(HrtfPanPath path, float distance_attenuation,
    return !preflight_hrtf_narrowing_ready(path, distance_attenuation, occlusion_gain, nullptr,


bool should_skip_hrtf_attenuation_coupling_preflight(
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {










bool hrtf_attenuation_coupling_rejects_for_reason(
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason expected) {



bool should_skip_hrtf_attenuation_coupling_narrowing_preflight(




bool should_skip_hrtf_attenuation_narrowing_preflight(HrtfPanPath path, float distance_attenuation,


const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason) {

HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(

        *reason = classifyHrtfAttenuationCouplingReject(preflight);









bool try_preflight_hrtf_attenuation_coupling_narrow(
    return preflight_hrtf_attenuation_coupling_narrow_ready(path, distance_attenuation,
                                                          occlusion_gain, &reason, coupling, params);



bool hrtf_attenuation_coupling_reject_reason_is_blocking(HrtfAttenuationCouplingRejectReason reason) {

bool preflight_hrtf_spatial_narrowing(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain, const HrtfAttenuationCoupling& coupling,
    const HrtfAttenuationCouplingRejectReason reject =
        classify_hrtf_attenuation_coupling_reject(preflight);
        *reason = reject;
    return !hrtf_attenuation_coupling_reject_reason_is_blocking(reject);

bool try_preflight_hrtf_spatial_narrowing(HrtfPanPath path, float distance_attenuation,




                                          HrtfAttenuationCouplingRejectReason& reason) {
    return preflight_hrtf_spatial_narrowing(path, distance_attenuation, occlusion_gain, coupling,
                                            params, &reason);












    const HrtfAttenuationCouplingRejectReason reject = classify_hrtf_attenuation_coupling_reject(
                                            params));
    return reject == HrtfAttenuationCouplingRejectReason::None;

    reason = classify_hrtf_attenuation_coupling_reject(




bool can_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCouplingRejectReason reject_reason =
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
        *reason = reject_reason;
    return reject_reason == HrtfAttenuationCouplingRejectReason::None;

    reason = hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
    }

bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {

                                             const BinauralPanParams& params,

bool should_skip_hrtf_attenuation_coupling_inputs(HrtfPanPath path, float distance_attenuation,
    return preflight;
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

HrtfBinauralRejectBundle classify_hrtf_binaural_rejects(const HrtfBinauralPreflight& preflight) {
    HrtfBinauralRejectBundle rejects{};
    rejects.convolution = classify_hrtf_ir_reject(preflight.ir);
    rejects.spatialPan = classify_hrtf_pan_path_reject(preflight.panPath);
    rejects.narrowing = classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    return rejects;
}

bool hrtf_binaural_rejects_allow_spatial_pan(const HrtfBinauralRejectBundle& rejects) {
    return !rejects.spatial_pan_blocked();
}

bool hrtf_binaural_rejects_allow_convolution(const HrtfBinauralRejectBundle& rejects) {
    return !rejects.convolution_blocked() && !rejects.spatial_pan_blocked();
}

bool hrtf_binaural_rejects_allow_narrowing(const HrtfBinauralRejectBundle& rejects) {
    return !rejects.narrowing_blocked();
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
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    case HrtfBinauralRejectReason::None:
    default:
        return "none";
    }
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    switch (classify_hrtf_pan_path_reject(preflight.panPath)) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
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

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    switch (preflight.panPath.reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    default:
        return HrtfBinauralRejectReason::None;
    }
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::CoLocated;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::EmptyIr;
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

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params,
                                              HrtfBinauralRejectBundle* rejects) {
    HrtfBinauralPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuationCoupling = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    populate_hrtf_binaural_reject_reasons(preflight);
    if (rejects != nullptr) {
        *rejects = classify_hrtf_binaural_rejects(preflight);
    }
    preflight.reason = classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener);
    preflight.reason = classifyHrtfBinauralReject(preflight);
    preflight.reason = classify_hrtf_binaural_reject(preflight);
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

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                                        const Vec3& rel_listener) {
    (void)ir;
    const HrtfPanPathRejectReason pan_reject =
        classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    return HrtfBinauralRejectReason::None;

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain);
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;








const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
        return "None";
        return "HrtfDisabled";
        return "CoLocated";
    return "Unknown";

HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    const HrtfPanPathRejectReason pan_reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    switch (pan_reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
    case HrtfPanPathRejectReason::CoLocated:
    case HrtfPanPathRejectReason::None:
    default:

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(hrtf_enabled, rel_listener) == expected;





    switch (hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener)) {


const char* hrtfBinauralRejectReasonName(HrtfBinauralRejectReason reason) {

HrtfBinauralRejectReason hrtfBinauralRejectReason(const HrtfBinauralPreflight& preflight) {
    switch (preflight.panPath.reason) {

HrtfIrRejectReason hrtfBinauralConvolutionRejectReason(const HrtfBinauralPreflight& preflight) {
    return preflight.ir.reason;

HrtfAttenuationCouplingRejectReason hrtfBinauralNarrowingRejectReason(
    const HrtfBinauralPreflight& preflight) {
    return preflight.attenuationCoupling.reason;

bool hrtfBinauralRejectsForReason(const HrtfBinauralPreflight& preflight,
    return hrtfBinauralRejectReason(preflight) == expected;





namespace {

HrtfBinauralRejectReason resolve_hrtf_binaural_reject_reason(
    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener) {
    if (pan_reason == HrtfPanPathRejectReason::HrtfDisabled) {
    if (pan_reason == HrtfPanPathRejectReason::CoLocated) {
    const HrtfIrRejectReason ir_reason = hrtf_ir_reject_reason(ir);
    if (ir_reason == HrtfIrRejectReason::MalformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    if (ir_reason == HrtfIrRejectReason::EmptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;

} // namespace

HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    (void)distance_attenuation;
    (void)occlusion_gain;
    (void)coupling;
    (void)params;
    return resolve_hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener);

HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener,
    return hrtf_binaural_reject_reason(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, coupling, params);

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, float distance_attenuation,
                                      float occlusion_gain, HrtfBinauralRejectReason expected,
    return hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                       occlusion_gain, coupling, params)
        == expected;

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
    return preflight.reason == expected;


HrtfBinauralRejectReason map_pan_path_reject_to_binaural(HrtfPanPathRejectReason reason) {

HrtfBinauralRejectReason map_attenuation_coupling_reject_to_binaural(
    HrtfAttenuationCouplingRejectReason reason) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
    case HrtfAttenuationCouplingRejectReason::None:

    bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener, float distance_attenuation,
    float occlusion_gain, const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    if (pan_reason != HrtfPanPathRejectReason::None) {
        return map_pan_path_reject_to_binaural(pan_reason);
    if (hrtf_ir_reject_reason(ir) != HrtfIrRejectReason::None) {
    const HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    const HrtfAttenuationCouplingRejectReason coupling_reason =
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain, coupling,
                                               params);
    if (coupling_reason != HrtfAttenuationCouplingRejectReason::None) {
        return map_attenuation_coupling_reject_to_binaural(coupling_reason);



    return resolve_hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                               occlusion_gain, coupling, params);

    return resolve_hrtf_binaural_reject_reason(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                               distance_attenuation, occlusion_gain, coupling,



HrtfBinauralRejectReason map_ir_reject_to_binaural(HrtfIrRejectReason reason) {
    case HrtfIrRejectReason::MalformedIr:
    case HrtfIrRejectReason::NullSamples:
    case HrtfIrRejectReason::ZeroLength:
    case HrtfIrRejectReason::None:


HrtfBinauralRejectReason map_attenuation_reject_to_binaural(

HrtfBinauralRejectReason compute_hrtf_binaural_reject_reason(
    const HrtfIrPreflight& ir, const HrtfPanPathPreflight& pan_path,
    const HrtfAttenuationCouplingPreflight& attenuation) {
    const HrtfBinauralRejectReason pan_reject = map_pan_path_reject_to_binaural(pan_path.reason);
    if (pan_reject != HrtfBinauralRejectReason::None) {
        return pan_reject;
    const HrtfBinauralRejectReason ir_reject = map_ir_reject_to_binaural(ir.reason);
    if (ir_reject != HrtfBinauralRejectReason::None) {
        return ir_reject;
    return map_attenuation_reject_to_binaural(attenuation.reason);


        return "EmptyIr";
        return "MalformedIr";
        return "UnityAttenuation";





HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan(HrtfPanPathRejectReason reason) {

HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_ir(HrtfIrRejectReason reason) {

HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_coupling(

HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason pan_reason =
        hrtf_binaural_reject_reason_from_pan(preflight.panPath.reason);
    if (pan_reason != HrtfBinauralRejectReason::None) {
        return pan_reason;
    return preflight.reason;

    return hrtf_binaural_reject_reason(preflight) == expected;

        return "BypassPath";

                                                     float distance_attenuation,
                                                     float occlusion_gain) {
    const HrtfPanPathRejectReason pan_reason =
        hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);

    if (ir_reason != HrtfIrRejectReason::None) {

        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
    if (coupling_reason == HrtfAttenuationCouplingRejectReason::BypassPath) {
    if (coupling_reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {


                                      float occlusion_gain, HrtfBinauralRejectReason expected) {
                                       occlusion_gain)


    const HrtfPanPathRejectReason panReason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    switch (panReason) {



bool hrtf_binaural_reject_reason_is_blocking_spatial(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::HrtfDisabled
        || reason == HrtfBinauralRejectReason::CoLocated;

bool hrtf_binaural_reject_reason_is_blocking_convolution(HrtfBinauralRejectReason reason) {
    return reason != HrtfBinauralRejectReason::None
        && reason != HrtfBinauralRejectReason::BypassPath
        && reason != HrtfBinauralRejectReason::UnityAttenuation;

bool hrtf_binaural_reject_reason_is_blocking_coupling(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::BypassPath
        || reason == HrtfBinauralRejectReason::UnityAttenuation;

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(bool hrtf_enabled,
        classify_hrtf_pan_path_spatial_reject(hrtf_enabled, rel_listener);
    switch (pan_reject) {
    case HrtfPanPathRejectReason::EmptyIr:

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(bool hrtf_enabled,
                                                                  const HrtfIrStub& ir,
        classify_hrtf_pan_path_convolution_reject(hrtf_enabled, ir, rel_listener);
        if (is_nonnull_zero_length_hrtf_ir(ir)) {

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(HrtfPanPath path,
    switch (coupling_reject) {

bool preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfBinauralRejectReason* reason) {
    const HrtfBinauralRejectReason reject =
        classify_hrtf_binaural_spatial_reject(hrtf_enabled, rel_listener);
    if (reason != nullptr) {
        *reason = reject;
    return !hrtf_binaural_reject_reason_is_blocking_spatial(reject);

bool preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
        classify_hrtf_binaural_convolution_reject(hrtf_enabled, ir, rel_listener);
    return !hrtf_binaural_reject_reason_is_blocking_convolution(reject);

bool preflight_hrtf_binaural_coupling(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain, HrtfBinauralRejectReason* reason) {
        classify_hrtf_binaural_coupling_reject(path, distance_attenuation, occlusion_gain);
    return !hrtf_binaural_reject_reason_is_blocking_coupling(reject);






    return hrtf_binaural_reject_reason(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

                                      const Vec3& rel_listener, HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener) == expected;


bool hrtf_binaural_reject_reason_is_bypass(HrtfBinauralRejectReason reason) {

bool hrtf_binaural_reject_reason_blocks_convolution(HrtfBinauralRejectReason reason) {
    return hrtf_binaural_reject_reason_is_bypass(reason)
        || reason == HrtfBinauralRejectReason::EmptyIr
        || reason == HrtfBinauralRejectReason::MalformedIr;

    case HrtfBinauralRejectReason::PanBypassDisabled:
        return "pan_bypass_disabled";
    case HrtfBinauralRejectReason::PanBypassCoLocated:
        return "pan_bypass_co_located";
    case HrtfBinauralRejectReason::ConvolutionEmptyIr:
        return "convolution_empty_ir";
    case HrtfBinauralRejectReason::ConvolutionMalformedIr:
        return "convolution_malformed_ir";
    case HrtfBinauralRejectReason::NarrowingBypassPath:
        return "narrowing_bypass_path";
    case HrtfBinauralRejectReason::NarrowingUnityAttenuation:
        return "narrowing_unity_attenuation";

bool is_blocking_hrtf_binaural_reject_reason(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::PanBypassDisabled
        || reason == HrtfBinauralRejectReason::PanBypassCoLocated;

    const HrtfPanPathRejectReason panReject =
    if (panReject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::PanBypassDisabled;
    if (panReject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::PanBypassCoLocated;

    const HrtfIrRejectReason irReject = classify_hrtf_ir_reject(ir);
    if (irReject == HrtfIrRejectReason::MalformedIr) {
        return HrtfBinauralRejectReason::ConvolutionMalformedIr;
    if (irReject == HrtfIrRejectReason::EmptyIr) {
        return HrtfBinauralRejectReason::ConvolutionEmptyIr;

    const HrtfAttenuationCouplingRejectReason narrowingReject =
        classify_hrtf_attenuation_coupling_reject(
            resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener), distance_attenuation,
            occlusion_gain);
    if (narrowingReject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::NarrowingBypassPath;
    if (narrowingReject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::NarrowingUnityAttenuation;


    return classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener, distance_attenuation,


HrtfBinauralRejectReason classify_hrtf_binaural_skip_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    return map_pan_path_skip_reject_to_binaural(
        classify_hrtf_pan_path_skip_reject(hrtf_enabled, rel_listener));

    return map_pan_path_convolution_reject_to_binaural(
        classify_hrtf_pan_path_convolution_reject(hrtf_enabled, ir, rel_listener));

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(HrtfPanPath path,
    return map_attenuation_coupling_reject_to_binaural(
        classify_hrtf_attenuation_coupling_reject(path, distance_attenuation, occlusion_gain));


                                                     float distance_attenuation, float occlusion_gain) {
    if (!hrtf_enabled) {
    if (is_co_located_hrtf_source(rel_listener)) {


bool is_blocking_hrtf_binaural_convolution_reject_reason(HrtfBinauralRejectReason reason) {
    return is_blocking_hrtf_binaural_reject_reason(reason)
        || reason == HrtfBinauralRejectReason::MalformedIr
        || reason == HrtfBinauralRejectReason::EmptyIr;

                                       occlusion_gain) == expected;


bool hrtf_binaural_reject_reason_is_blocking(HrtfBinauralRejectReason reason) {
    return reason != HrtfBinauralRejectReason::None;

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    const HrtfPanPathRejectReason pan_reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);








void populate_hrtf_binaural_reject_reasons(HrtfBinauralPreflight& preflight) {
    preflight.panRejectReason =
        map_pan_path_reject_to_binaural(preflight.panPath.reason);
    if (preflight.panRejectReason != HrtfBinauralRejectReason::None) {
        preflight.convolutionRejectReason = preflight.panRejectReason;
    } else {
        preflight.convolutionRejectReason = map_ir_reject_to_binaural(preflight.ir.reason);
    if (preflight.attenuationCoupling.reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        preflight.attenuationRejectReason = HrtfBinauralRejectReason::UnityAttenuation;
        preflight.attenuationRejectReason = HrtfBinauralRejectReason::None;


HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(bool hrtf_enabled,
    return map_pan_path_reject_to_binaural(classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener));

    const HrtfBinauralRejectReason pan_reject = classify_hrtf_binaural_pan_reject(hrtf_enabled, rel_listener);
    return map_ir_reject_to_binaural(classify_hrtf_ir_reject(ir));

HrtfBinauralRejectReason classify_hrtf_binaural_attenuation_reject(
    const HrtfAttenuationCouplingRejectReason reject =
    if (reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {

bool hrtf_binaural_reject_reason_is_pan_blocking(HrtfBinauralRejectReason reason) {

bool hrtf_binaural_reject_reason_is_convolution_blocking(HrtfBinauralRejectReason reason) {
    return hrtf_binaural_reject_reason_is_pan_blocking(reason)




HrtfBinauralRejectReason map_pan_path_reject(HrtfPanPathRejectReason reason) {

HrtfBinauralRejectReason map_ir_reject(HrtfIrRejectReason reason) {

HrtfBinauralRejectReason map_coupling_reject(HrtfAttenuationCouplingRejectReason reason) {


    return map_pan_path_reject(classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener));

    const HrtfBinauralRejectReason pan_reject = classify_hrtf_binaural_pan_reject(hrtf_enabled,
                                                                                  rel_listener);
    return map_ir_reject(classify_hrtf_ir_reject(ir));

    bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation, float occlusion_gain) {
    const HrtfPanPath path = resolve_hrtf_pan_path(hrtf_enabled, rel_listener);
    return map_coupling_reject(





HrtfBinauralRejectReason map_coupling_reject_to_binaural(

    preflight.spatialPanReject =
    if (preflight.spatialPanReject != HrtfBinauralRejectReason::None) {
        preflight.convolutionReject = preflight.spatialPanReject;
        preflight.convolutionReject = map_ir_reject_to_binaural(preflight.ir.reason);
    preflight.narrowingReject =
        map_coupling_reject_to_binaural(preflight.attenuationCoupling.reason);


HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
    return preflight.spatialPanReject;

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    return preflight.convolutionReject;

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    return preflight.narrowingReject;

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
    if (preflight.panPath.coLocated) {
    if (preflight.ir.malformedIr) {
    if (preflight.ir.emptyIr) {
    if (preflight.attenuationCoupling.unityAttenuation) {

HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
        return HrtfBinauralConvolutionRejectReason::CoLocated;
        return HrtfBinauralConvolutionRejectReason::MalformedIr;
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    return HrtfBinauralConvolutionRejectReason::None;

HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralNarrowingRejectReason::BypassPath;
        return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
    return HrtfBinauralNarrowingRejectReason::None;







    preflight.spatialPanRejectReason =
        map_pan_path_reject_to_binaural(preflight.panPath.rejectReason);
    if (preflight.panPath.can_convolve()) {
        preflight.convolutionRejectReason = HrtfBinauralRejectReason::None;
    } else if (preflight.spatialPanRejectReason != HrtfBinauralRejectReason::None) {
        preflight.convolutionRejectReason = preflight.spatialPanRejectReason;
        preflight.convolutionRejectReason = map_ir_reject_to_binaural(preflight.ir.rejectReason);
    preflight.narrowingRejectReason =
        map_attenuation_reject_to_binaural(preflight.attenuationCoupling.rejectReason);


    return preflight.spatialPanRejectReason;

    return preflight.convolutionRejectReason;

    return preflight.narrowingRejectReason;




const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {

bool hrtfBinauralRejectReasonIsBypass(HrtfBinauralRejectReason reason) {

bool hrtfBinauralRejectReasonBlocksConvolution(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::EmptyIr
        || hrtfBinauralRejectReasonIsBypass(reason);

HrtfBinauralRejectReason classifyHrtfBinauralReject(
    float occlusion_gain, const HrtfAttenuationCoupling& coupling,
    const HrtfPanPathRejectReason pan_reject = classifyHrtfPanPathReject(hrtf_enabled, rel_listener);

    const HrtfIrRejectReason ir_reject = classifyHrtfIrReject(ir);
    if (ir_reject == HrtfIrRejectReason::MalformedIr) {
    if (ir_reject == HrtfIrRejectReason::NullSamples) {

    const HrtfAttenuationCouplingRejectReason atten_reject = classifyHrtfAttenuationCouplingReject(
    if (atten_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {


bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              HrtfBinauralRejectReason& reason,
    reason = classifyHrtfBinauralReject(hrtf_enabled, ir, rel_listener, distance_attenuation,
    return !hrtfBinauralRejectReasonIsBypass(reason);

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflightHrtfBinaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                    distance_attenuation, occlusion_gain, reason, coupling, params);


bool hrtfBinauralRejectReasonIsBlocking(HrtfBinauralRejectReason reason) {

HrtfBinauralRejectReason classifyHrtfBinauralReject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::HrtfDisabled) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::CoLocated) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::MalformedIr) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::EmptyIr) {
    if (preflight.attenuationCoupling.reason
        == HrtfAttenuationCouplingRejectReason::BypassPath) {
        == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {




HrtfConvolutionRejectReason hrtf_binaural_convolution_reject_reason(
    return preflight.panPath.convolutionReason;

bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                   HrtfConvolutionRejectReason expected) {
    return hrtf_binaural_convolution_reject_reason(preflight) == expected;

const char* hrtf_binaural_convolve_reject_reason_label(HrtfBinauralConvolveRejectReason reason) {
    case HrtfBinauralConvolveRejectReason::None:
    case HrtfBinauralConvolveRejectReason::Bypass:
        return "bypass";
    case HrtfBinauralConvolveRejectReason::EmptyIr:
    case HrtfBinauralConvolveRejectReason::NullSamples:
        return "null_samples";
    case HrtfBinauralConvolveRejectReason::ZeroLength:
        return "zero_length";
    case HrtfBinauralConvolveRejectReason::MalformedIr:

    switch (classify_hrtf_pan_path_reject(preflight.panPath)) {
    switch (preflight.panPath.reject) {


    const HrtfPanPathRejectReason pan_reason = classify_hrtf_pan_path_reject(preflight.panPath);

HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    if (!preflight.panPath.can_spatial_pan()) {
    return classify_hrtf_ir_reject(preflight.ir);

HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(
    return classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                     float occlusion_gain, HrtfBinauralRejectReason* reason,
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain,
                                coupling, params);
        *reason = classify_hrtf_binaural_reject(preflight);
    return preflight.can_spatial_pan();

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfBinauralRejectReason* reason,
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                  float occlusion_gain, HrtfBinauralRejectReason& reason,
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,





HrtfBinauralRejectReason composite_hrtf_binaural_reject_reason(
    const HrtfPanPathPreflight& panPath, const HrtfAttenuationCouplingPreflight& attenuationCoupling) {
    switch (panPath.reason) {
    case HrtfPanPathRejectReason::CoLocatedSource:
        return HrtfBinauralRejectReason::CoLocatedSource;
        break;
    switch (attenuationCoupling.reason) {


    case HrtfBinauralRejectReason::CoLocatedSource:
        return "CoLocatedSource";

    return composite_hrtf_binaural_reject_reason(preflight.panPath, preflight.attenuationCoupling);


const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason) {
    case HrtfConvolutionRejectReason::None:
    case HrtfConvolutionRejectReason::HrtfDisabled:
    case HrtfConvolutionRejectReason::CoLocated:
    case HrtfConvolutionRejectReason::EmptyIr:
    case HrtfConvolutionRejectReason::MalformedIr:


HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfBinauralPreflight& preflight) {
    return preflight.convolutionReason;


bool hrtf_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
    return classify_hrtf_convolution_reject(preflight) == expected;

    return classify_hrtf_binaural_reject(preflight) == expected;


HrtfBinauralRejectReason binaural_reject_from_pan_path(HrtfPanPathRejectReason reason) {

HrtfConvolutionRejectReason convolution_reject_reason_from_preflight(
        return HrtfConvolutionRejectReason::HrtfDisabled;
        return HrtfConvolutionRejectReason::CoLocated;
        return HrtfConvolutionRejectReason::MalformedIr;
        return HrtfConvolutionRejectReason::EmptyIr;
    return HrtfConvolutionRejectReason::None;


HrtfBinauralConvolveRejectReason classify_hrtf_binaural_convolve_reject(
        return HrtfBinauralConvolveRejectReason::None;
    if (preflight.is_bypass()) {
        return HrtfBinauralConvolveRejectReason::Bypass;
    switch (preflight.ir.reject) {
        return HrtfBinauralConvolveRejectReason::MalformedIr;
        return HrtfBinauralConvolveRejectReason::NullSamples;
        return HrtfBinauralConvolveRejectReason::ZeroLength;
    case HrtfIrRejectReason::EmptyIr:
        return HrtfBinauralConvolveRejectReason::EmptyIr;

    return preflight.panPath.reason == HrtfPanPathRejectReason::HrtfDisabled
        ? HrtfBinauralRejectReason::HrtfDisabled
        : preflight.panPath.reason == HrtfPanPathRejectReason::CoLocated
            ? HrtfBinauralRejectReason::CoLocated
            : HrtfBinauralRejectReason::None;
    case HrtfBinauralRejectReason::NullSamples:
        return "NullSamples";
    case HrtfBinauralRejectReason::ZeroLength:
        return "ZeroLength";

        return HrtfBinauralRejectReason::NullSamples;
        return HrtfBinauralRejectReason::ZeroLength;

HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan_path(HrtfPanPathRejectReason reason) {

HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_attenuation(

HrtfBinauralRejectReason hrtf_binaural_skip_reject_reason(bool hrtf_enabled,
    return hrtf_binaural_reject_reason_from_pan_path(
        hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener));

HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(const HrtfIrStub& ir) {
    return hrtf_binaural_reject_reason_from_ir(hrtf_ir_reject_reason(ir));

HrtfBinauralRejectReason hrtf_binaural_attenuation_reject_reason(
    return hrtf_binaural_reject_reason_from_attenuation(
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain));

bool hrtf_binaural_skip_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
    return hrtf_binaural_skip_reject_reason(hrtf_enabled, rel_listener) == expected;

bool hrtf_binaural_convolution_rejects_for_reason(const HrtfIrStub& ir,
    return hrtf_binaural_convolution_reject_reason(ir) == expected;

    if (preflight.panPath.skipped) {
    if (preflight.attenuationCoupling.skipped && preflight.attenuationCoupling.unityAttenuation) {



    const HrtfBinauralRejectReason pan_reject = classify_hrtf_binaural_reject(preflight);



    if (!preflight.should_skip()) {

HrtfBinauralRejectReason classify_hrtf_binaural_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
        classify_hrtf_pan_path_reject_reason(hrtf_enabled, ir, rel_listener);

HrtfBinauralRejectReason classify_hrtf_binaural_reject_reason(bool hrtf_enabled,
    return classify_hrtf_binaural_reject_reason(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);

bool preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
        classify_hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener);
    return reject == HrtfBinauralRejectReason::None;

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial(hrtf_enabled, ir, rel_listener, &reason);

    return preflight_hrtf_binaural_spatial(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfBinauralRejectReason& reason) {
    return try_preflight_hrtf_binaural_spatial(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                               reason);











HrtfBinauralRejectReason map_convolve_reject_to_binaural(HrtfPanPathConvolveRejectReason reason) {
    case HrtfPanPathConvolveRejectReason::HrtfDisabled:
    case HrtfPanPathConvolveRejectReason::CoLocated:
    case HrtfPanPathConvolveRejectReason::EmptyIr:
    case HrtfPanPathConvolveRejectReason::MalformedIr:
    case HrtfPanPathConvolveRejectReason::None:

    if (reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {



HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight) {
    return map_pan_path_reject_to_binaural(classify_hrtf_pan_path_reject(preflight.panPath));

HrtfBinauralRejectReason classify_hrtf_binaural_convolve_reject(
    return map_convolve_reject_to_binaural(
        classify_hrtf_pan_path_convolve_reject(preflight.panPath));

HrtfBinauralRejectReason classify_hrtf_binaural_narrow_reject(const HrtfBinauralPreflight& preflight) {
    return map_attenuation_reject_to_binaural(
        classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling));

    const HrtfBinauralRejectReason pan_reason = classify_hrtf_binaural_pan_reject(preflight);
    const HrtfBinauralRejectReason convolve_reason = classify_hrtf_binaural_convolve_reject(preflight);
    if (convolve_reason != HrtfBinauralRejectReason::None) {
        return convolve_reason;
    return classify_hrtf_binaural_narrow_reject(preflight);



HrtfBinauralRejectBundle classify_hrtf_binaural_rejects(const HrtfBinauralPreflight& preflight) {
    HrtfBinauralRejectBundle rejects{};
    rejects.convolution = classify_hrtf_ir_reject(preflight.ir);
    rejects.spatialPan = classify_hrtf_pan_path_reject(preflight.panPath);
    rejects.narrowing = classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    return rejects;

bool hrtf_binaural_rejects_allow_spatial_pan(const HrtfBinauralRejectBundle& rejects) {
    return !rejects.spatial_pan_blocked();

bool hrtf_binaural_rejects_allow_convolution(const HrtfBinauralRejectBundle& rejects) {
    return !rejects.convolution_blocked() && !rejects.spatial_pan_blocked();

bool hrtf_binaural_rejects_allow_narrowing(const HrtfBinauralRejectBundle& rejects) {
    return !rejects.narrowing_blocked();

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params,
                                              HrtfBinauralRejectBundle* rejects) {
    HrtfBinauralPreflight preflight{};
    preflight.reason = hrtf_binaural_reject_reason(hrtf_enabled, rel_listener);
    preflight.rejected = preflight.reason != HrtfBinauralRejectReason::None;
    preflight.reason = hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener);
    preflight.reason = classify_hrtf_binaural_reject(hrtf_enabled, rel_listener);
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuationCoupling = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    preflight.reason = classify_hrtf_binaural_reject(preflight);
    preflight.reason = classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener);
    preflight.rejectReason = classify_hrtf_binaural_reject(hrtf_enabled, rel_listener);
    preflight.reason = hrtfBinauralRejectReason(preflight);
    preflight.rejectReason = hrtf_binaural_reject_reason(preflight);
    preflight.reason = resolve_hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener);
    preflight.reason = resolve_hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener,
                                                           distance_attenuation, occlusion_gain,
                                                           coupling, params);
    preflight.rejected = preflight.panPath.rejected;
    preflight.reason =
        compute_hrtf_binaural_reject_reason(preflight.ir, preflight.panPath,
                                            preflight.attenuationCoupling);

    const HrtfBinauralRejectReason pan_reason =
        hrtf_binaural_reject_reason_from_pan(preflight.panPath.reason);
    if (pan_reason != HrtfBinauralRejectReason::None) {
        preflight.reason = pan_reason;
    } else {
        preflight.reason = hrtf_binaural_reject_reason_from_ir(preflight.ir.reason);
    }
    preflight.reason = hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain);
    preflight.spatialRejectReason =
        classify_hrtf_binaural_spatial_reject(hrtf_enabled, rel_listener);
    preflight.convolutionRejectReason =
        classify_hrtf_binaural_convolution_reject(hrtf_enabled, ir, rel_listener);
    preflight.couplingRejectReason = classify_hrtf_binaural_coupling_reject(
        preflight.panPath.path, distance_attenuation, occlusion_gain);
    preflight.reason = classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener,
    preflight.skipReason = classify_hrtf_binaural_skip_reject(hrtf_enabled, rel_listener);
    preflight.narrowingRejectReason = classify_hrtf_binaural_narrowing_reject(
    populate_hrtf_binaural_reject_reasons(preflight);
    preflight.panRejectReason = classify_hrtf_binaural_pan_reject(hrtf_enabled, rel_listener);
        hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain);
    preflight.rejectReason = classifyHrtfBinauralReject(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    preflight.reason = classifyHrtfBinauralReject(preflight);
        composite_hrtf_binaural_reject_reason(preflight.panPath, preflight.attenuationCoupling);
    preflight.reason = binaural_reject_from_pan_path(preflight.panPath.reason);
    preflight.convolutionReason = convolution_reject_reason_from_preflight(preflight);
    preflight.reject = classify_hrtf_binaural_reject(preflight);
    preflight.convolveReject = classify_hrtf_binaural_convolve_reject(preflight);
    preflight.skipReason = hrtf_binaural_reject_reason_from_pan_path(preflight.panPath.reason);
    preflight.convolutionReason = hrtf_binaural_reject_reason_from_ir(preflight.ir.reason);
    preflight.attenuationReason =
        hrtf_binaural_reject_reason_from_attenuation(preflight.attenuationCoupling.reason);
    preflight.rejectReason = classify_hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener);
    preflight.reason = classify_hrtf_binaural_pan_reject(preflight);
    preflight.convolveReason = classify_hrtf_binaural_convolve_reject(preflight);
    preflight.narrowReason = classify_hrtf_binaural_narrow_reject(preflight);
    if (rejects != nullptr) {
        *rejects = classify_hrtf_binaural_rejects(preflight);
    return preflight;

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& out, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    out = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                  occlusion_gain, coupling, params);
    reason = out.reason;
    return !out.should_skip();
                                 HrtfBinauralPreflight& preflight,
    preflight = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
    return preflight.can_spatial_pan();

bool hrtf_binaural_preflight_skips_for_reason(const HrtfBinauralPreflight& preflight,
                                              HrtfBinauralRejectReason expected) {
    return preflight.skipReason == expected;

bool hrtf_binaural_preflight_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
    return preflight.convolutionRejectReason == expected;

bool hrtf_binaural_preflight_narrowing_rejects_for_reason(const HrtfBinauralPreflight& preflight,
    return preflight.narrowingRejectReason == expected;

HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  const Vec3& rel_listener, float distance_attenuation,
                                                  float occlusion_gain, HrtfBinauralRejectReason& reason,
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    reason = preflight.reason;

HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                                  HrtfBinauralRejectReason& reason,
    return try_preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, reason, coupling,
                                       params);
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                             float distance_attenuation, float occlusion_gain,
                             const HrtfAttenuationCoupling& coupling,
                             const BinauralPanParams& params, HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    preflight = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                        occlusion_gain, coupling, params);
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return try_preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, preflight, coupling,
                                       params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& out, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    out = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                  occlusion_gain, coupling, params);
    reason = out.panRejectReason;
    return out.can_spatial_pan();
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params, HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    const HrtfBinauralRejectReason reject = preflight.spatialPanRejectReason;
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfBinauralRejectReason::None;
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, coupling, params, &reason);
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                            float distance_attenuation, float occlusion_gain,
                            HrtfBinauralRejectReason* reason,
                            const HrtfAttenuationCoupling& coupling,
                            const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return !hrtfBinauralRejectReasonIsBlocking(preflight.reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
                                               HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(preflight, &reason);
}

bool preflight_hrtf_binaural_coupling_ready(const HrtfBinauralPreflight& preflight,
                                            HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_coupling_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_coupling(const HrtfBinauralPreflight& preflight,
                                          HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_coupling_ready(preflight, &reason);
}

bool preflightHrtfBinauralReady(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                float distance_attenuation, float occlusion_gain,
                                HrtfBinauralRejectReason* reason,
                                const HrtfAttenuationCoupling& coupling,
                                const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralReject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflightHrtfBinauralReady(bool hrtf_enabled, const Vec3& rel_listener,
                                float distance_attenuation, float occlusion_gain,
                                HrtfBinauralRejectReason* reason,
                                const HrtfAttenuationCoupling& coupling,
                                const BinauralPanParams& params) {
    return preflightHrtfBinauralReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                      distance_attenuation, occlusion_gain, reason, coupling,
                                      params);
}

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return preflightHrtfBinauralReady(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                      occlusion_gain, &reason, coupling, params);
}

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return preflightHrtfBinauralReady(hrtf_enabled, rel_listener, distance_attenuation,
                                      occlusion_gain, &reason, coupling, params);
}

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classifyHrtfBinauralPanReject(const HrtfBinauralPreflight& preflight) {
    const HrtfPanPathRejectReason panReason = classifyHrtfPanPathReject(preflight.panPath);
    switch (panReason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classifyHrtfBinauralConvolutionReject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfPanConvolutionRejectReason convolutionReason =
        classifyHrtfPanConvolutionReject(preflight.panPath, preflight.ir);
    switch (convolutionReason) {
    case HrtfPanConvolutionRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanConvolutionRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanConvolutionRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfPanConvolutionRejectReason::EmptyIr:
        return HrtfBinauralRejectReason::EmptyIr;
    case HrtfPanConvolutionRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classifyHrtfBinauralNarrowingReject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfAttenuationCouplingRejectReason couplingReason =
        classifyHrtfAttenuationCouplingReject(preflight.attenuationCoupling);
    switch (couplingReason) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflightHrtfBinauralPanReady(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralPanReject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflightHrtfBinauralPanReady(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflightHrtfBinauralPanReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool tryPreflightHrtfBinauralPan(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason) {
    return preflightHrtfBinauralPanReady(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool tryPreflightHrtfBinauralPan(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params,
                                 HrtfBinauralRejectReason& reason) {
    return preflightHrtfBinauralPanReady(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool preflightHrtfBinauralConvolutionReady(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params,
                                           HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralConvolutionReject(preflight);
    }
    return preflight.can_convolve();
}

bool tryPreflightHrtfBinauralConvolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params,
                                         HrtfBinauralRejectReason& reason) {
    return preflightHrtfBinauralConvolutionReady(hrtf_enabled, ir, rel_listener,
                                                 distance_attenuation, occlusion_gain, coupling,
                                                 params, &reason);
}

bool preflightHrtfBinauralNarrowingReady(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params,
                                         HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralNarrowingReject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflightHrtfBinauralNarrowingReady(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params,
                                         HrtfBinauralRejectReason* reason) {
    return preflightHrtfBinauralNarrowingReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                               distance_attenuation, occlusion_gain, coupling,
                                               params, reason);
}

bool tryPreflightHrtfBinauralNarrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                       const Vec3& rel_listener, float distance_attenuation,
                                       float occlusion_gain,
                                       const HrtfAttenuationCoupling& coupling,
                                       const BinauralPanParams& params,
                                       HrtfBinauralRejectReason& reason) {
    return preflightHrtfBinauralNarrowingReady(hrtf_enabled, ir, rel_listener,
                                               distance_attenuation, occlusion_gain, coupling,
                                               params, &reason);
}

bool tryPreflightHrtfBinauralNarrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                       float distance_attenuation, float occlusion_gain,
                                       const HrtfAttenuationCoupling& coupling,
                                       const BinauralPanParams& params,
                                       HrtfBinauralRejectReason& reason) {
    return preflightHrtfBinauralNarrowingReady(hrtf_enabled, rel_listener, distance_attenuation,
                                               occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    preflight = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                        occlusion_gain, coupling, params);
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return try_preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, preflight, coupling,
                                       params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_pan_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolve_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain, HrtfBinauralRejectReason* reason,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolve_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolve(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float distance_attenuation,
                                          float occlusion_gain, HrtfBinauralRejectReason& reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolve_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                                  occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params,
                                              HrtfBinauralRejectBundle* rejects) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params,
                                   rejects);
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, const HrtfIrStub& ir,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params,
                                              HrtfBinauralRejectBundle* rejects) {
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, coupling, params);
                                  occlusion_gain, coupling, params, rejects);
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
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
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

HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight) {
    switch (preflight.panPath.reason) {
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

HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
    if (is_co_located_hrtf_source(rel_listener)) {

                                 HrtfBinauralPreflight& preflight, HrtfBinauralRejectReason& reason,
    preflight = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
    reason = preflight.rejectReason;
    return preflight.can_spatial_pan();
        break;

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
    return hrtf_binaural_reject_reason(preflight) == expected;
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener,
                                                     float distance_attenuation,
                                                     float occlusion_gain) {
                                   occlusion_gain)
        .reason;

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, float distance_attenuation,
                                      float occlusion_gain, HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(hrtf_enabled, ir, rel_listener, distance_attenuation,
        == expected;
    }

                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {

                                  float distance_attenuation, float occlusion_gain,
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,










bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
        *reason = preflight.reason;

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);

bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    reason = preflight.reason;

bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                      distance_attenuation, occlusion_gain, reason, coupling,
                                      params);





        *reason = preflight.reject;

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,


    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,

    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";

HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
    if (preflight.panPath.hrtfDisabled) {
    if (preflight.panPath.coLocated) {
    return HrtfBinauralRejectReason::BypassPath;

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
    if (preflight.should_skip()) {
        return classify_hrtf_binaural_pan_reject(preflight);
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    if (preflight.can_narrow_spatial_image()) {
    if (preflight.attenuationCoupling.bypassPath) {
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
        *reason = classify_hrtf_binaural_pan_reject(preflight);

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, rel_listener, distance_attenuation,

        return "None";
        return "HrtfDisabled";
        return "CoLocated";
    return "Unknown";

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
        *reason = classify_hrtf_binaural_reject(preflight);




bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);

bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,



    return !preflight.should_skip();


bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
        hrtf_enabled, listener, source_position, ir, distance_attenuation, occlusion_gain, coupling,

                                   const Vec3& source_position, float distance_attenuation,
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation, occlusion_gain,
                                         reason, coupling, params);




















                                              HrtfBinauralRejectBundle* rejects) {
                                   rejects);

                                 const BinauralPanParams& params, HrtfBinauralPreflight& preflight,
                                 HrtfBinauralRejectBundle& rejects) {

                                        occlusion_gain, coupling, params, &rejects);
    return hrtf_binaural_rejects_allow_spatial_pan(rejects);
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    const HrtfBinauralRejectReason reject =
        classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling,
                                  const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling,
                                  const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain,
                                   &reason, coupling, params);
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    const HrtfBinauralRejectReason reject =
        classify_hrtf_binaural_reject(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener, float distance_attenuation,
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling,
                                  const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                   occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason& reason,
                                  const HrtfAttenuationCoupling& coupling,
                                  const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain,
                                   &reason, coupling, params);
}

const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason hrtf_binaural_spatial_reject_reason(const HrtfBinauralPreflight& preflight) {
    switch (preflight.panPath.reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reason = hrtf_binaural_spatial_reject_reason(preflight);
    if (spatial_reason != HrtfBinauralRejectReason::None) {
        return spatial_reason;
    }
    switch (preflight.ir.reason) {
    case HrtfIrRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfIrRejectReason::NullSamples:
    case HrtfIrRejectReason::ZeroLength:
        return HrtfBinauralRejectReason::EmptyIr;
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason hrtf_binaural_attenuation_coupling_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    switch (preflight.attenuationCoupling.reason) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return hrtf_binaural_spatial_reject_reason(preflight);
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    default:
        return HrtfBinauralRejectReason::None;
    }
}

bool hrtf_binaural_rejects_for_spatial_reason(const HrtfBinauralPreflight& preflight,
                                              HrtfBinauralRejectReason expected) {
    return hrtf_binaural_spatial_reject_reason(preflight) == expected;
}

bool hrtf_binaural_rejects_for_convolution_reason(const HrtfBinauralPreflight& preflight,
                                                  HrtfBinauralRejectReason expected) {
    return hrtf_binaural_convolution_reject_reason(preflight) == expected;
}

bool hrtf_binaural_rejects_for_attenuation_coupling_reason(const HrtfBinauralPreflight& preflight,
                                                           HrtfBinauralRejectReason expected) {
    return hrtf_binaural_attenuation_coupling_reject_reason(preflight) == expected;
}

HrtfBinauralRejectReason HrtfBinauralPreflight::spatial_reject_reason() const {
    return hrtf_binaural_spatial_reject_reason(*this);
}

HrtfBinauralRejectReason HrtfBinauralPreflight::convolution_reject_reason() const {
    return hrtf_binaural_convolution_reject_reason(*this);
}

HrtfBinauralRejectReason HrtfBinauralPreflight::attenuation_coupling_reject_reason() const {
    return hrtf_binaural_attenuation_coupling_reject_reason(*this);
}

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {
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
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classifyHrtfBinauralReject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classifyHrtfBinauralConvolutionReject(const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason panReject = classifyHrtfBinauralReject(preflight);
    if (panReject != HrtfBinauralRejectReason::None) {
        return panReject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classifyHrtfBinauralNarrowingReject(const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason panReject = classifyHrtfBinauralReject(preflight);
    if (panReject != HrtfBinauralRejectReason::None) {
        return panReject;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    reason = classifyHrtfBinauralReject(preflight);
    return preflight.can_spatial_pan();
}

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return tryPreflightHrtfBinaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                    distance_attenuation, occlusion_gain, reason, coupling, params);
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params,
                                              HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralReject(preflight);
    }
    return preflight;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params,
                                              HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params, reason);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, reason);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation, occlusion_gain,
                                         coupling, params, reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
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

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool can_apply_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight) {
    return preflight.can_spatial_pan();
}

bool can_apply_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight,
                                 HrtfBinauralRejectReason* reason) {
    const HrtfBinauralRejectReason reject_reason = hrtf_binaural_spatial_reject_reason(preflight);
    if (reason != nullptr) {
        *reason = reject_reason;
    }
    return preflight.can_spatial_pan();
}

bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();
}

bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight,
                                HrtfBinauralRejectReason* reason) {
    const HrtfBinauralRejectReason reject_reason =
        hrtf_binaural_convolution_reject_reason(preflight);
    if (reason != nullptr) {
        *reason = reject_reason;
    }
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

bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight,
                                            HrtfBinauralRejectReason* reason) {
    const HrtfBinauralRejectReason reject_reason =
        hrtf_binaural_attenuation_coupling_reject_reason(preflight);
    if (reason != nullptr) {
        *reason = reject_reason;
    }
    return preflight.can_narrow_spatial_image();
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

bool hrtf_binaural_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfBinauralRejectReason expected) {
    return classify_hrtf_binaural_reject(hrtf_enabled, rel_listener) == expected;
}

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    case HrtfBinauralRejectReason::None:
    default:
        return "none";
    }
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return classify_hrtf_binaural_reject(preflight) == expected;
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight) {
    preflight = preflight_hrtf_ir(ir);
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralPreflight& preflight,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    preflight = preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                        occlusion_gain, coupling, params);
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason& reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    reason = classify_hrtf_binaural_spatial_pan_reject(preflight);
    return reason == HrtfBinauralRejectReason::None;
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain, HrtfBinauralRejectReason& reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    reason = classify_hrtf_binaural_convolution_reject(preflight);
    return reason == HrtfBinauralRejectReason::None;
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain, HrtfBinauralRejectReason& reason,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    reason = classify_hrtf_binaural_narrowing_reject(preflight);
    return reason == HrtfBinauralRejectReason::None;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolutionRejectReason::None:
        return "none";
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralConvolutionRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralConvolutionRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
        return "empty_ir";
    }
    return "unknown";
}

const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason) {
    switch (reason) {
    case HrtfBinauralNarrowingRejectReason::None:
        return "none";
    case HrtfBinauralNarrowingRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralNarrowingRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

bool hrtf_binaural_reject_reason_blocks_spatial_pan(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::HrtfDisabled
        || reason == HrtfBinauralRejectReason::CoLocated;
}

bool hrtf_binaural_reject_reason_blocks_convolution(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::HrtfDisabled
        || reason == HrtfBinauralRejectReason::CoLocated
        || reason == HrtfBinauralRejectReason::MalformedIr
        || reason == HrtfBinauralRejectReason::EmptyIr;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_ir_reject(preflight.ir);
}

HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_spatial_ready(const HrtfBinauralPreflight& preflight,
                                           HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_ready(
        preflight_hrtf_binaural(hrtf_enabled, rel_listener, 1.f, 1.f), reason);
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_ready(
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f), reason);
}

bool try_preflight_hrtf_binaural_spatial(const HrtfBinauralPreflight& preflight,
                                         HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(preflight, &reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
                                               HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_convolution_ready(
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f), reason);
}

bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(preflight, &reason);
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(
        preflight_hrtf_binaural(hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain,
                                coupling, params),
        reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain,
                                coupling, params),
        reason);
}

bool try_preflight_hrtf_binaural_narrowing(const HrtfBinauralPreflight& preflight,
                                           HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_narrowing_ready(preflight, &reason);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, &reason,
                                                   coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, &reason,
                                                   coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                       const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_pan_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                       HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                             reason);
}

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(const HrtfIrStub& ir,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfIrPreflight ir_preflight = preflight_hrtf_ir(ir);
    if (reason != nullptr) {
        if (ir_preflight.malformedIr) {
            *reason = HrtfBinauralRejectReason::MalformedIr;
        } else if (ir_preflight.emptyIr) {
            *reason = HrtfBinauralRejectReason::EmptyIr;
        } else {
            *reason = HrtfBinauralRejectReason::None;
        }
    }
    return ir_preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(const HrtfIrStub& ir,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(ir, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    const HrtfAttenuationCouplingPreflight coupling_preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        if (coupling_preflight.bypassPath) {
            *reason = HrtfBinauralRejectReason::BypassPath;
        } else if (coupling_preflight.unityAttenuation) {
            *reason = HrtfBinauralRejectReason::UnityAttenuation;
        } else {
            *reason = HrtfBinauralRejectReason::None;
        }
    }
    return coupling_preflight.can_narrow();
}

bool try_preflight_hrtf_binaural_narrowing(HrtfPanPath path, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(path, distance_attenuation, occlusion_gain,
                                                   coupling, params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(
    const HrtfBinauralPreflight& preflight) {
    switch (classify_hrtf_pan_path_reject(preflight.panPath)) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    switch (classify_hrtf_convolution_reject(preflight.ir, preflight.panPath)) {
    case HrtfConvolutionRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfConvolutionRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfConvolutionRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfConvolutionRejectReason::EmptyIr:
        return HrtfBinauralRejectReason::EmptyIr;
    case HrtfConvolutionRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    const HrtfBinauralPreflight& preflight) {
    switch (classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling)) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                               reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_coupling_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params,
                                            HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_coupling_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_coupling_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                            float distance_attenuation, float occlusion_gain,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params,
                                            HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_coupling_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                  distance_attenuation, occlusion_gain, coupling,
                                                  params, reason);
}

bool try_preflight_hrtf_binaural_coupling(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float distance_attenuation,
                                          float occlusion_gain, HrtfBinauralRejectReason& reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    return preflight_hrtf_binaural_coupling_ready(hrtf_enabled, ir, rel_listener,
                                                  distance_attenuation, occlusion_gain, coupling,
                                                  params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::NullSamples:
        return "NullSamples";
    case HrtfBinauralRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight) {
    const HrtfPanPathRejectReason pan_reason =
        classify_hrtf_pan_path_reject(preflight.panPath);
    switch (pan_reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfIrRejectReason ir_reason = classify_hrtf_ir_reject(preflight.ir);
    switch (ir_reason) {
    case HrtfIrRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfIrRejectReason::NullSamples:
        return HrtfBinauralRejectReason::NullSamples;
    case HrtfIrRejectReason::ZeroLength:
        return HrtfBinauralRejectReason::ZeroLength;
    case HrtfIrRejectReason::EmptyIr:
        return HrtfBinauralRejectReason::EmptyIr;
    case HrtfIrRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfAttenuationCouplingRejectReason coupling_reason =
        classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    switch (coupling_reason) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
        return HrtfBinauralRejectReason::None;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_pan_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, listener, source_position, ir, distance_attenuation, occlusion_gain, coupling,
        params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation,
                                         occlusion_gain, coupling, params, reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    reason = preflight.reason;
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return try_preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, reason, coupling,
                                       params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfIrRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = preflight.ir.reason;
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, HrtfIrRejectReason& reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    reason = preflight.ir.reason;
    return preflight.can_convolve();
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                   distance_attenuation, occlusion_gain, coupling,
                                                   params, reason);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, coupling,
                                                     params, &reason);
}

HrtfPanPathRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_pan_path_reject(preflight.panPath);
}

HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_ir_reject(preflight.ir);
}

HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
    return preflight_hrtf_spatial_pan_ready(hrtf_enabled, ir, rel_listener, reason);
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           HrtfPanPathRejectReason* reason) {
    return preflight_hrtf_spatial_pan_ready(hrtf_enabled, rel_listener, reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
    return try_preflight_hrtf_spatial_pan(hrtf_enabled, ir, rel_listener, reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfPanPathRejectReason& reason) {
    return try_preflight_hrtf_spatial_pan(hrtf_enabled, rel_listener, reason);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, HrtfIrRejectReason* reason) {
    return preflight_hrtf_pan_path_convolution_ready(hrtf_enabled, ir, rel_listener, reason);
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, HrtfIrRejectReason& reason) {
    return try_preflight_hrtf_pan_path_convolution(hrtf_enabled, ir, rel_listener, reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                   distance_attenuation, occlusion_gain, reason,
                                                   coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain,
                                           HrtfAttenuationCouplingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, &reason,
                                                   coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfAttenuationCouplingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, &reason, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolutionRejectReason::None:
        return "None";
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralConvolutionRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralConvolutionRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";
}

const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason) {
    switch (reason) {
    case HrtfBinauralNarrowingRejectReason::None:
        return "None";
    case HrtfBinauralNarrowingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralNarrowingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_pan_path_reject(preflight.panPath) == HrtfPanPathRejectReason::HrtfDisabled
               ? HrtfBinauralRejectReason::HrtfDisabled
           : classify_hrtf_pan_path_reject(preflight.panPath) == HrtfPanPathRejectReason::CoLocated
               ? HrtfBinauralRejectReason::CoLocated
               : HrtfBinauralRejectReason::None;
}

HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfConvolutionRejectReason reason =
        classify_hrtf_convolution_reject(preflight.panPath, preflight.ir);
    switch (reason) {
    case HrtfConvolutionRejectReason::None:
        return HrtfBinauralConvolutionRejectReason::None;
    case HrtfConvolutionRejectReason::HrtfDisabled:
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
    case HrtfConvolutionRejectReason::CoLocated:
        return HrtfBinauralConvolutionRejectReason::CoLocated;
    case HrtfConvolutionRejectReason::EmptyIr:
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    case HrtfConvolutionRejectReason::MalformedIr:
        return HrtfBinauralConvolutionRejectReason::MalformedIr;
    }
    return HrtfBinauralConvolutionRejectReason::EmptyIr;
}

HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfAttenuationCouplingRejectReason reason =
        classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return HrtfBinauralNarrowingRejectReason::None;
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralNarrowingRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
    }
    return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, listener, source_position, ir, distance_attenuation, occlusion_gain, coupling,
        params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation, occlusion_gain,
                                         reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                           occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                            occlusion_gain, nullptr, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
                                               HrtfBinauralConvolutionRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralConvolutionRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(preflight, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralNarrowingRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_narrowing(const HrtfBinauralPreflight& preflight,
                                           HrtfBinauralNarrowingRejectReason& reason) {
    return preflight_hrtf_binaural_narrowing_ready(preflight, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::NullSamples:
        return "NullSamples";
    case HrtfBinauralRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.can_spatial_pan()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::BypassPath;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.nullSamples) {
        return HrtfBinauralRejectReason::NullSamples;
    }
    if (preflight.ir.zeroLength) {
        return HrtfBinauralRejectReason::ZeroLength;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::EmptyIr;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_narrow_spatial_image()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::BypassPath;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                   distance_attenuation, occlusion_gain, coupling,
                                                   params, reason);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, coupling,
                                                   params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.can_spatial_pan()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::EmptyIr;
}

HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_narrow_spatial_image()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::UnityAttenuation;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 distance_attenuation, occlusion_gain, reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,
                                                 distance_attenuation, occlusion_gain, &reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                 occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_spatial_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                 const Vec3& rel_listener, float distance_attenuation,
                                                 float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,
                                                  distance_attenuation, occlusion_gain, nullptr,
                                                  coupling, params);
}

bool should_skip_hrtf_binaural_spatial_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                                 float distance_attenuation, float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                  occlusion_gain, nullptr, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool should_skip_hrtf_binaural_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener,
                                                     float distance_attenuation, float occlusion_gain,
                                                     const HrtfAttenuationCoupling& coupling,
                                                     const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                      distance_attenuation, occlusion_gain, nullptr,
                                                      coupling, params);
}

bool preflight_hrtf_binaural_coupling_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain, HrtfBinauralRejectReason* reason,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_coupling_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_coupling_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                            float distance_attenuation, float occlusion_gain,
                                            HrtfBinauralRejectReason* reason,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params) {
    return preflight_hrtf_binaural_coupling_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                  distance_attenuation, occlusion_gain, reason,
                                                  coupling, params);
}

bool try_preflight_hrtf_binaural_coupling(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float distance_attenuation,
                                          float occlusion_gain, HrtfBinauralRejectReason& reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    return preflight_hrtf_binaural_coupling_ready(hrtf_enabled, ir, rel_listener,
                                                  distance_attenuation, occlusion_gain, &reason,
                                                  coupling, params);
}

bool try_preflight_hrtf_binaural_coupling(bool hrtf_enabled, const Vec3& rel_listener,
                                          float distance_attenuation, float occlusion_gain,
                                          HrtfBinauralRejectReason& reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    return preflight_hrtf_binaural_coupling_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                  occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_coupling_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  const Vec3& rel_listener,
                                                  float distance_attenuation, float occlusion_gain,
                                                  const HrtfAttenuationCoupling& coupling,
                                                  const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_coupling_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, nullptr,
                                                   coupling, params);
}

bool should_skip_hrtf_binaural_coupling_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                                  float distance_attenuation, float occlusion_gain,
                                                  const HrtfAttenuationCoupling& coupling,
                                                  const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_coupling_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, nullptr, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                       const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_pan_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                       HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                             reason);
}

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                     HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(const HrtfIrStub& ir,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfIrPreflight ir_preflight = preflight_hrtf_ir(ir);
    if (reason != nullptr) {
        if (ir_preflight.malformedIr) {
            *reason = HrtfBinauralRejectReason::MalformedIr;
        } else if (ir_preflight.emptyIr) {
            *reason = HrtfBinauralRejectReason::EmptyIr;
        } else {
            *reason = HrtfBinauralRejectReason::None;
        }
    }
    return ir_preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(const HrtfIrStub& ir,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(ir, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(HrtfPanPath path, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    const HrtfAttenuationCouplingPreflight coupling_preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
    if (reason != nullptr) {
        if (coupling_preflight.bypassPath) {
            *reason = HrtfBinauralRejectReason::BypassPath;
        } else if (coupling_preflight.unityAttenuation) {
            *reason = HrtfBinauralRejectReason::UnityAttenuation;
        } else {
            *reason = HrtfBinauralRejectReason::None;
        }
    }
    return coupling_preflight.can_narrow();
}

bool try_preflight_hrtf_binaural_narrowing(HrtfPanPath path, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(path, distance_attenuation, occlusion_gain,
                                                   coupling, params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "none";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "none";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_pan_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_pan_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                        const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_pan_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool should_skip_hrtf_pan_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                const Vec3& rel_listener) {
    return !preflight_hrtf_pan_convolution_ready(hrtf_enabled, ir, rel_listener);
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 distance_attenuation, occlusion_gain, reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                                 occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                 occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_spatial_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                 const Vec3& rel_listener, float distance_attenuation,
                                                 float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                                  occlusion_gain, nullptr, coupling, params);
}

bool should_skip_hrtf_binaural_spatial_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                                 float distance_attenuation, float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                  occlusion_gain, nullptr, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool should_skip_hrtf_binaural_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener,
                                                     float distance_attenuation, float occlusion_gain,
                                                     const HrtfAttenuationCoupling& coupling,
                                                     const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                      distance_attenuation, occlusion_gain, nullptr,
                                                      coupling, params);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                   distance_attenuation, occlusion_gain, reason,
                                                   coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_narrowing_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                   const Vec3& rel_listener, float distance_attenuation,
                                                   float occlusion_gain,
                                                   const HrtfAttenuationCoupling& coupling,
                                                   const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                      distance_attenuation, occlusion_gain, nullptr,
                                                      coupling, params);
}

bool should_skip_hrtf_binaural_narrowing_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                                   float distance_attenuation, float occlusion_gain,
                                                   const HrtfAttenuationCoupling& coupling,
                                                   const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                    occlusion_gain, nullptr, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_spatial_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
        return spatial_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrow_reject(const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_spatial_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
        return spatial_reject;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 reason);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool preflight_hrtf_binaural_narrow_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float distance_attenuation,
                                          float occlusion_gain, HrtfBinauralRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrow_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrow_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                          float distance_attenuation, float occlusion_gain,
                                          HrtfBinauralRejectReason* reason,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrow_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                distance_attenuation, occlusion_gain, reason,
                                                coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                        const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_narrow(bool hrtf_enabled, const HrtfIrStub& ir,
                                        const Vec3& rel_listener, float distance_attenuation,
                                        float occlusion_gain, HrtfBinauralRejectReason& reason,
                                        const HrtfAttenuationCoupling& coupling,
                                        const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrow_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                                occlusion_gain, &reason, coupling, params);
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

const char* hrtf_convolution_reject_reason_label(HrtfConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfConvolutionRejectReason::None:
        return "none";
    case HrtfConvolutionRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfConvolutionRejectReason::CoLocated:
        return "co_located";
    case HrtfConvolutionRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfConvolutionRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfConvolutionRejectReason classify_hrtf_convolution_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfConvolutionRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfConvolutionRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfConvolutionRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfConvolutionRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfConvolutionRejectReason::EmptyIr;
    }
    return HrtfConvolutionRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, coupling, params,
                                       reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                       occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                       occlusion_gain, coupling, params, &reason);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float distance_attenuation,
                                          float occlusion_gain,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                        float distance_attenuation, float occlusion_gain,
                                        const HrtfAttenuationCoupling& coupling,
                                        const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);
}

bool preflight_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, float distance_attenuation,
                                      float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                      const BinauralPanParams& params,
                                      HrtfConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfConvolutionRejectReason& reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                            occlusion_gain, coupling, params, &reason);
}

bool should_skip_hrtf_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return !preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                             occlusion_gain, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::HrtfDisabled;
}

bool preflight_hrtf_binaural_spatial_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, make_empty_hrtf_ir(),
                                                     rel_listener, reason);
}

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, rel_listener, &reason);
}

bool should_skip_hrtf_binaural_spatial_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener) {
    return !preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, ir, rel_listener);
}

bool should_skip_hrtf_binaural_spatial_pan_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, rel_listener);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfPanPathConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_pan_path_convolution_reject(preflight.panPath, preflight.ir);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfPanPathConvolutionRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool should_skip_hrtf_binaural_convolution_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener) {
    return !preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                   distance_attenuation, occlusion_gain, reason,
                                                   coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain,
                                           HrtfAttenuationCouplingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, &reason,
                                                   coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfAttenuationCouplingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_narrowing_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                                   const Vec3& rel_listener,
                                                   float distance_attenuation, float occlusion_gain,
                                                   const HrtfAttenuationCoupling& coupling,
                                                   const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                      distance_attenuation, occlusion_gain, nullptr,
                                                      coupling, params);
}

bool should_skip_hrtf_binaural_narrowing_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                                   float distance_attenuation, float occlusion_gain,
                                                   const HrtfAttenuationCoupling& coupling,
                                                   const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                    occlusion_gain, nullptr, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return classify_hrtf_binaural_reject(preflight) == expected;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, reason, coupling,
                                       params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classifyHrtfBinauralPanReject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classifyHrtfBinauralConvolutionReject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason pan_reject = classifyHrtfBinauralPanReject(preflight);
    if (pan_reject != HrtfBinauralRejectReason::None) {
        return pan_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classifyHrtfBinauralNarrowingReject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_pan_ready(const HrtfBinauralPreflight& preflight,
                                       HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralPanReject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool try_preflight_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight,
                                     HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_pan_ready(preflight, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
                                               HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralConvolutionReject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(preflight, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(const HrtfBinauralPreflight& preflight,
                                             HrtfBinauralRejectReason* reason) {
    if (reason != nullptr) {
        *reason = classifyHrtfBinauralNarrowingReject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_narrowing(const HrtfBinauralPreflight& preflight,
                                           HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_narrowing_ready(preflight, &reason);
}

bool preflight_hrtf_binaural_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                       const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_pan_ready(
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f), reason);
}

bool try_preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_pan_ready(hrtf_enabled, ir, rel_listener, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason bypass = classify_hrtf_binaural_reject(preflight);
    if (bypass != HrtfBinauralRejectReason::None) {
        return bypass;
    }
    switch (classify_hrtf_ir_reject(preflight.ir)) {
    case HrtfIrRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfIrRejectReason::NullSamples:
    case HrtfIrRejectReason::ZeroLength:
        return HrtfBinauralRejectReason::EmptyIr;
    case HrtfIrRejectReason::None:
        break;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
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
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
    const HrtfBinauralPreflight& preflight) {
    if (!preflight.panPath.skipped) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.skipped) {
        if (preflight.panPath.hrtfDisabled) {
            return HrtfBinauralRejectReason::HrtfDisabled;
        }
        if (preflight.panPath.coLocated) {
            return HrtfBinauralRejectReason::CoLocated;
        }
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.nullSamples) {
        return HrtfBinauralRejectReason::NullSamples;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    if (preflight.ir.zeroLength) {
        return HrtfBinauralRejectReason::ZeroLength;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_narrow_spatial_image()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_binaural_spatial_pan_reject(preflight);
}

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return classify_hrtf_binaural_reject(preflight) == expected;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    default:
        return "Unknown";
    }
}

const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolutionRejectReason::None:
        return "None";
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralConvolutionRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralConvolutionRejectReason::MalformedIr:
        return "MalformedIr";
    default:
        return "Unknown";
    }
}

const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason) {
    switch (reason) {
    case HrtfBinauralNarrowingRejectReason::None:
        return "None";
    case HrtfBinauralNarrowingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralNarrowingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    default:
        return "Unknown";
    }
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralConvolutionRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralConvolutionRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    }
    return HrtfBinauralConvolutionRejectReason::None;
}

HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralNarrowingRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
    }
    return HrtfBinauralNarrowingRejectReason::None;
}

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return classify_hrtf_binaural_reject(preflight) == expected;
}

bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                  HrtfBinauralConvolutionRejectReason expected) {
    return classify_hrtf_binaural_convolution_reject(preflight) == expected;
}

bool hrtf_binaural_narrowing_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                HrtfBinauralNarrowingRejectReason expected) {
    return classify_hrtf_binaural_narrowing_reject(preflight) == expected;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener,
                                            HrtfBinauralConvolutionRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfBinauralNarrowingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain,
                                           HrtfBinauralNarrowingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, &reason,
                                                   coupling, params);
}

HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight) {
    return preflight.reason;
}

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return preflight.reason == expected;
}

HrtfIrRejectReason hrtf_binaural_convolution_reject_reason(const HrtfBinauralPreflight& preflight) {
    return preflight.ir.reason;
}

bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                  HrtfIrRejectReason expected) {
    return preflight.ir.reason == expected;
}

HrtfAttenuationCouplingRejectReason hrtf_binaural_attenuation_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    return preflight.attenuationCoupling.reason;
}

bool hrtf_binaural_attenuation_rejects_for_reason(
    const HrtfBinauralPreflight& preflight, HrtfAttenuationCouplingRejectReason expected) {
    return preflight.attenuationCoupling.reason == expected;
}

namespace {

HrtfBinauralRejectReason map_ir_reject_reason(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::NullSamples:
        return HrtfBinauralRejectReason::NullSamples;
    case HrtfIrRejectReason::ZeroLength:
        return HrtfBinauralRejectReason::ZeroLength;
    case HrtfIrRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    case HrtfIrRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason map_pan_path_reject_reason(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralRejectReason map_attenuation_coupling_reject_reason(
    HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

} // namespace

const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    switch (reason) {
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
    }
    return "Unknown";
}

HrtfIrRejectReason hrtf_binaural_ir_reject_reason(const HrtfBinauralPreflight& preflight) {
    return preflight.ir.reason;
}

HrtfPanPathRejectReason hrtf_binaural_pan_path_reject_reason(const HrtfBinauralPreflight& preflight) {
    return preflight.panPath.reason;
}

HrtfAttenuationCouplingRejectReason hrtf_binaural_attenuation_coupling_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    return preflight.attenuationCoupling.reason;
}

bool hrtf_binaural_rejects_ir_for_reason(const HrtfBinauralPreflight& preflight,
                                         HrtfIrRejectReason expected) {
    return preflight.ir.reason == expected;
}

bool hrtf_binaural_rejects_pan_path_for_reason(const HrtfBinauralPreflight& preflight,
                                               HrtfPanPathRejectReason expected) {
    return preflight.panPath.reason == expected;
}

bool hrtf_binaural_rejects_attenuation_coupling_for_reason(
    const HrtfBinauralPreflight& preflight, HrtfAttenuationCouplingRejectReason expected) {
    return preflight.attenuationCoupling.reason == expected;
}

HrtfBinauralRejectReason hrtf_binaural_pan_reject_reason(const HrtfBinauralPreflight& preflight) {
    return map_pan_path_reject_reason(preflight.panPath.reason);
}

HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    return map_ir_reject_reason(preflight.ir.reason);
}

HrtfBinauralRejectReason hrtf_binaural_narrowing_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    return map_attenuation_coupling_reject_reason(preflight.attenuationCoupling.reason);
}

bool hrtf_binaural_pan_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                          HrtfBinauralRejectReason expected) {
    return hrtf_binaural_pan_reject_reason(preflight) == expected;
}

bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                  HrtfBinauralRejectReason expected) {
    return hrtf_binaural_convolution_reject_reason(preflight) == expected;
}

bool hrtf_binaural_narrowing_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                 HrtfBinauralRejectReason expected) {
    return hrtf_binaural_narrowing_reject_reason(preflight) == expected;
}

bool hrtf_binaural_preflight_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                                HrtfBinauralRejectReason expected) {
    return preflight.reason == expected;
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_spatial_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
        return spatial_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 distance_attenuation, occlusion_gain, reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,
                                                 distance_attenuation, occlusion_gain, &reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                 occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
        return spatial_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, float distance_attenuation,
                                 float occlusion_gain, HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                    distance_attenuation, occlusion_gain, &reason,
                                                    coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_spatial_pan_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
        return spatial_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_spatial_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_pan_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                               HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, make_empty_hrtf_ir(),
                                                     rel_listener, reason);
}

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                             HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_spatial_pan_ready(hrtf_enabled, rel_listener, &reason);
}

bool preflight_hrtf_binaural_convolve_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener,
                                            HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolve(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener,
                                          HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_convolve_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                   distance_attenuation, occlusion_gain, coupling,
                                                   params, reason);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, coupling,
                                                   params, &reason);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolutionRejectReason::None:
        return "None";
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralConvolutionRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralConvolutionRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
        return "EmptyIr";
    }
    return "Unknown";
}

const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason) {
    switch (reason) {
    case HrtfBinauralNarrowingRejectReason::None:
        return "None";
    case HrtfBinauralNarrowingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralNarrowingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralConvolutionRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralConvolutionRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    }
    return HrtfBinauralConvolutionRejectReason::None;
}

HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralNarrowingRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
    }
    return HrtfBinauralNarrowingRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, listener, source_position, ir, distance_attenuation, occlusion_gain, coupling,
        params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation, occlusion_gain,
                                         reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain,
                                               HrtfBinauralConvolutionRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfBinauralConvolutionRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfBinauralNarrowingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain,
                                           HrtfBinauralNarrowingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                   distance_attenuation, occlusion_gain, &reason,
                                                   coupling, params);
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

const char* hrtf_binaural_convolve_reject_reason_label(HrtfBinauralConvolveRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolveRejectReason::None:
        return "none";
    case HrtfBinauralConvolveRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralConvolveRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralConvolveRejectReason::EmptyIr:
        return "empty_ir";
    case HrtfBinauralConvolveRejectReason::MalformedIr:
        return "malformed_ir";
    }
    return "unknown";
}

const char* hrtf_binaural_narrow_reject_reason_label(HrtfBinauralNarrowRejectReason reason) {
    switch (reason) {
    case HrtfBinauralNarrowRejectReason::None:
        return "none";
    case HrtfBinauralNarrowRejectReason::BypassPath:
        return "bypass_path";
    case HrtfBinauralNarrowRejectReason::UnityAttenuation:
        return "unity_attenuation";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    const HrtfPanPathRejectReason pan_reject = classify_hrtf_pan_path_reject(preflight.panPath);
    switch (pan_reject) {
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    case HrtfPanPathRejectReason::None:
    default:
        return HrtfBinauralRejectReason::None;
    }
}

HrtfBinauralConvolveRejectReason classify_hrtf_binaural_convolve_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralConvolveRejectReason::None;
    }
    const HrtfPanPathRejectReason pan_reject = classify_hrtf_pan_path_reject(preflight.panPath);
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralConvolveRejectReason::HrtfDisabled;
    }
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralConvolveRejectReason::CoLocated;
    }
    const HrtfIrRejectReason ir_reject = classify_hrtf_ir_reject(preflight.ir);
    if (ir_reject == HrtfIrRejectReason::MalformedIr) {
        return HrtfBinauralConvolveRejectReason::MalformedIr;
    }
    return HrtfBinauralConvolveRejectReason::EmptyIr;
}

HrtfBinauralNarrowRejectReason classify_hrtf_binaural_narrow_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfAttenuationCouplingRejectReason coupling_reject =
        classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    switch (coupling_reject) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralNarrowRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralNarrowRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
    default:
        return HrtfBinauralNarrowRejectReason::None;
    }
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralRejectReason reject = classify_hrtf_binaural_reject(
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain,
                                coupling, params));
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params,
                                 HrtfBinauralRejectReason& reason) {
    reason = classify_hrtf_binaural_reject(
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain,
                                coupling, params));
    return reason == HrtfBinauralRejectReason::None;
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params,
                                 HrtfBinauralRejectReason& reason) {
    return try_preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool preflight_hrtf_binaural_convolve_ready(const HrtfBinauralPreflight& preflight,
                                            HrtfBinauralConvolveRejectReason* reason) {
    const HrtfBinauralConvolveRejectReason reject = classify_hrtf_binaural_convolve_reject(preflight);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfBinauralConvolveRejectReason::None;
}

bool try_preflight_hrtf_binaural_convolve(const HrtfBinauralPreflight& preflight,
                                          HrtfBinauralConvolveRejectReason& reason) {
    reason = classify_hrtf_binaural_convolve_reject(preflight);
    return reason == HrtfBinauralConvolveRejectReason::None;
}

bool preflight_hrtf_binaural_narrow_ready(const HrtfBinauralPreflight& preflight,
                                            HrtfBinauralNarrowRejectReason* reason) {
    const HrtfBinauralNarrowRejectReason reject = classify_hrtf_binaural_narrow_reject(preflight);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == HrtfBinauralNarrowRejectReason::None;
}

bool try_preflight_hrtf_binaural_narrow(const HrtfBinauralPreflight& preflight,
                                          HrtfBinauralNarrowRejectReason& reason) {
    reason = classify_hrtf_binaural_narrow_reject(preflight);
    return reason == HrtfBinauralNarrowRejectReason::None;
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

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.can_spatial_pan()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::BypassPath;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralRejectReason::None;
    }
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_spatial_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
        return spatial_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::EmptyIr;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_narrow_spatial_image()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::BypassPath;
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_spatial_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason* reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 distance_attenuation, occlusion_gain, reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,
                                                 distance_attenuation, occlusion_gain, &reason,
                                                 coupling, params);
}

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         HrtfBinauralRejectReason& reason,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                 occlusion_gain, &reason, coupling, params);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain, HrtfBinauralRejectReason* reason,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason& reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain, HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfBinauralRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(),
                                                   rel_listener, distance_attenuation,
                                                   occlusion_gain, reason, coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           float occlusion_gain, HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, &reason,
                                                     coupling, params);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, &reason, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason pan_reject = classify_hrtf_binaural_reject(preflight);
    if (pan_reject != HrtfBinauralRejectReason::None) {
        return pan_reject;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.attenuationCoupling.bypassPath) {
        return HrtfBinauralRejectReason::BypassPath;
    }
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
                                   float distance_attenuation, float occlusion_gain,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, reason);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, float distance_attenuation,
                                   float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params,
                                   HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation,
                                         occlusion_gain, coupling, params, reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);
}

const char* hrtf_binaural_bypass_reject_reason_label(HrtfBinauralBypassRejectReason reason) {
    switch (reason) {
    case HrtfBinauralBypassRejectReason::None:
        return "None";
    case HrtfBinauralBypassRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralBypassRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolutionRejectReason::None:
        return "None";
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralConvolutionRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
        return "EmptyIr";
    }
    return "Unknown";
}

const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason) {
    switch (reason) {
    case HrtfBinauralNarrowingRejectReason::None:
        return "None";
    case HrtfBinauralNarrowingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralNarrowingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralBypassRejectReason classify_hrtf_binaural_bypass_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfPanSpatialRejectReason spatial =
        classify_hrtf_pan_spatial_reject(preflight.panPath);
    switch (spatial) {
    case HrtfPanSpatialRejectReason::HrtfDisabled:
        return HrtfBinauralBypassRejectReason::HrtfDisabled;
    case HrtfPanSpatialRejectReason::CoLocated:
        return HrtfBinauralBypassRejectReason::CoLocated;
    case HrtfPanSpatialRejectReason::None:
        return HrtfBinauralBypassRejectReason::None;
    }
    return HrtfBinauralBypassRejectReason::None;
}

HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfPanConvolutionRejectReason convolution =
        classify_hrtf_pan_convolution_reject(preflight.panPath);
    switch (convolution) {
    case HrtfPanConvolutionRejectReason::HrtfDisabled:
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
    case HrtfPanConvolutionRejectReason::CoLocated:
        return HrtfBinauralConvolutionRejectReason::CoLocated;
    case HrtfPanConvolutionRejectReason::EmptyIr:
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    case HrtfPanConvolutionRejectReason::None:
        return HrtfBinauralConvolutionRejectReason::None;
    }
    return HrtfBinauralConvolutionRejectReason::None;
}

HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfAttenuationCouplingRejectReason coupling =
        classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
    switch (coupling) {
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralNarrowingRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
    case HrtfAttenuationCouplingRejectReason::None:
        return HrtfBinauralNarrowingRejectReason::None;
    }
    return HrtfBinauralNarrowingRejectReason::None;
}

bool preflight_hrtf_binaural_bypass_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                          HrtfBinauralBypassRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_bypass_reject(preflight);
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_bypass_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener,
                                          HrtfBinauralBypassRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_bypass_reject(preflight);
    }
    return !preflight.should_skip();
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener,
                                               HrtfBinauralConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfBinauralNarrowingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             HrtfBinauralNarrowingRejectReason* reason,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_bypass(bool hrtf_enabled, const Vec3& rel_listener,
                                        HrtfBinauralBypassRejectReason& reason) {
    return preflight_hrtf_binaural_bypass_ready(hrtf_enabled, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener,
                                             HrtfBinauralConvolutionRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
                                           HrtfBinauralNarrowingRejectReason& reason,
                                           const HrtfAttenuationCoupling& coupling,
                                           const BinauralPanParams& params) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                                   occlusion_gain, &reason, coupling, params);
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

const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfBinauralConvolutionRejectReason::None:
        return "none";
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
        return "hrtf_disabled";
    case HrtfBinauralConvolutionRejectReason::CoLocated:
        return "co_located";
    case HrtfBinauralConvolutionRejectReason::MalformedIr:
        return "malformed_ir";
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
        return "empty_ir";
    }
    return "unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    const HrtfPanPathRejectReason pan_reason = classify_hrtf_pan_path_reject(preflight.panPath);
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

HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfBinauralConvolutionRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralConvolutionRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralConvolutionRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    }
    return HrtfBinauralConvolutionRejectReason::None;
}

HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_coupling_reject(
    const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params,
                                 HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params,
                                 HrtfBinauralRejectReason* reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,
                                         reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                           occlusion_gain, coupling, params, &reason);
}

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                               const Vec3& rel_listener, float distance_attenuation,
                                               float occlusion_gain,
                                               const HrtfAttenuationCoupling& coupling,
                                               const BinauralPanParams& params,
                                               HrtfBinauralConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params,
                                            HrtfBinauralConvolutionRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, coupling,
                                                     params, &reason);
}

bool preflight_hrtf_binaural_coupling_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                            const Vec3& rel_listener, float distance_attenuation,
                                            float occlusion_gain,
                                            const HrtfAttenuationCoupling& coupling,
                                            const BinauralPanParams& params,
                                            HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_coupling_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();
}

bool try_preflight_hrtf_binaural_coupling(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener, float distance_attenuation,
                                          float occlusion_gain,
                                          const HrtfAttenuationCoupling& coupling,
                                          const BinauralPanParams& params,
                                          HrtfAttenuationCouplingRejectReason& reason) {
    return preflight_hrtf_binaural_coupling_ready(hrtf_enabled, ir, rel_listener,
                                                  distance_attenuation, occlusion_gain, coupling,
                                                  params, &reason);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, float distance_attenuation,
                                         float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);
}

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
                                         float distance_attenuation, float occlusion_gain,
                                         const HrtfAttenuationCoupling& coupling,
                                         const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);
}

const char* hrtfBinauralRejectReasonName(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classifyHrtfBinauralReject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfIrRejectReason classifyHrtfBinauralConvolutionReject(const HrtfBinauralPreflight& preflight) {
    return preflight.ir.reason;
}

HrtfPanPathRejectReason classifyHrtfBinauralPanReject(const HrtfBinauralPreflight& preflight) {
    return preflight.panPath.reason;
}

HrtfAttenuationCouplingRejectReason classifyHrtfBinauralCouplingReject(
    const HrtfBinauralPreflight& preflight) {
    return preflight.attenuationCoupling.reason;
}

bool hrtfBinauralRejectsForReason(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                  float distance_attenuation, float occlusion_gain,
                                  HrtfBinauralRejectReason expected,
                                  const HrtfAttenuationCoupling& coupling,
                                  const BinauralPanParams& params) {
    return classifyHrtfBinauralReject(preflight_hrtf_binaural(
               hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling,
               params))
        == expected;
}

bool preflightHrtfBinauralReady(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                float distance_attenuation, float occlusion_gain,
                                HrtfBinauralRejectReason* reason,
                                const HrtfAttenuationCoupling& coupling,
                                const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return !preflight.should_skip();
}

bool preflightHrtfBinauralReady(bool hrtf_enabled, const Vec3& rel_listener,
                                float distance_attenuation, float occlusion_gain,
                                HrtfBinauralRejectReason* reason,
                                const HrtfAttenuationCoupling& coupling,
                                const BinauralPanParams& params) {
    return preflightHrtfBinauralReady(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                      distance_attenuation, occlusion_gain, reason, coupling,
                                      params);
}

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return preflightHrtfBinauralReady(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                      occlusion_gain, &reason, coupling, params);
}

bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener,
                              float distance_attenuation, float occlusion_gain,
                              HrtfBinauralRejectReason& reason,
                              const HrtfAttenuationCoupling& coupling,
                              const BinauralPanParams& params) {
    return preflightHrtfBinauralReady(hrtf_enabled, rel_listener, distance_attenuation,
                                      occlusion_gain, &reason, coupling, params);
}

const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::EmptyIr:
        return "EmptyIr";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
    }
    return HrtfBinauralRejectReason::None;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                       distance_attenuation, occlusion_gain, reason, coupling,
                                       params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
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

const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    case HrtfIrRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";

const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason) {
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    case HrtfPanPathRejectReason::None:
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";
    case HrtfPanPathRejectReason::EmptyIr:
    case HrtfPanPathRejectReason::MalformedIr:

const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason) {
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
    case HrtfAttenuationCouplingRejectReason::None:
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";

const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    case HrtfBinauralRejectReason::None:
    case HrtfBinauralRejectReason::HrtfDisabled:
    case HrtfBinauralRejectReason::CoLocated:
    case HrtfBinauralRejectReason::EmptyIr:
    case HrtfBinauralRejectReason::MalformedIr:
    case HrtfBinauralRejectReason::BypassPath:
    case HrtfBinauralRejectReason::UnityAttenuation:

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;

HrtfPanPathRejectReason classify_hrtf_pan_path_spatial_reject(const HrtfPanPathPreflight& preflight) {
    if (preflight.emptyIr) {
        return HrtfIrRejectReason::EmptyIr;

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
    if (preflight.can_spatial_pan()) {
        return HrtfPanPathRejectReason::None;
    if (preflight.hrtfDisabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    if (preflight.coLocated) {
        return HrtfPanPathRejectReason::CoLocated;

HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(const HrtfPanPathPreflight& preflight,
                                                                  const HrtfIrPreflight& ir) {
    if (ir.malformedIr) {
        return HrtfPanPathRejectReason::MalformedIr;
        return HrtfPanPathRejectReason::EmptyIr;

HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(
    const HrtfPanPathPreflight& preflight) {
    const HrtfPanPathRejectReason spatial_reject = classify_hrtf_pan_path_reject(preflight);
    if (spatial_reject != HrtfPanPathRejectReason::None) {
        return spatial_reject;
    if (!preflight.can_convolve() && preflight.emptyIr) {

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (preflight.can_narrow()) {
        return HrtfAttenuationCouplingRejectReason::None;
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;

HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {

const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason) {
    case HrtfConvolutionRejectReason::None:
    case HrtfConvolutionRejectReason::HrtfDisabled:
    case HrtfConvolutionRejectReason::CoLocated:
    case HrtfConvolutionRejectReason::EmptyIr:
    case HrtfConvolutionRejectReason::MalformedIr:

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
        return HrtfBinauralRejectReason::None;
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    return HrtfBinauralRejectReason::BypassPath;

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight) {

HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason spatial_reject = classify_hrtf_binaural_reject(preflight);
    if (spatial_reject != HrtfBinauralRejectReason::None) {
    if (preflight.ir.malformedIr) {
        return HrtfBinauralRejectReason::MalformedIr;
    if (preflight.ir.emptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(const HrtfBinauralPreflight& preflight) {

HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(
    if (preflight.can_narrow_spatial_image()) {
    if (preflight.attenuationCoupling.bypassPath) {
    if (preflight.attenuationCoupling.unityAttenuation) {
        return HrtfBinauralRejectReason::UnityAttenuation;

bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrPreflight preflight = preflight_hrtf_ir(ir);
    if (reason != nullptr) {
        *reason = classify_hrtf_ir_reject(preflight);
    return preflight.can_convolve();

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir_ready(ir, &reason);

bool should_skip_hrtf_ir_ready(const HrtfIrStub& ir) {
    return !preflight_hrtf_ir_ready(ir);

bool preflight_hrtf_pan_path_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener,
                                           HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
        *reason = classify_hrtf_pan_path_spatial_reject(preflight);
bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir) {

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
        *reason = classify_hrtf_pan_path_reject(preflight);
    return preflight.can_spatial_pan();

bool preflight_hrtf_pan_path_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 reason);

bool try_preflight_hrtf_pan_path_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_pan_path_spatial(bool hrtf_enabled, const Vec3& rel_listener,
                                         HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, rel_listener, &reason);

bool should_skip_hrtf_pan_path_spatial_input(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_pan_path_spatial_input(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_spatial_ready(hrtf_enabled, rel_listener);

bool preflight_hrtf_pan_path_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfPanPathPreflight pan_path = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    const HrtfIrPreflight ir_preflight = preflight_hrtf_ir(ir);
        *reason = classify_hrtf_pan_path_convolution_reject(pan_path, ir_preflight);
    return pan_path.can_convolve();

bool try_preflight_hrtf_pan_path_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_pan_path_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);

bool should_skip_hrtf_pan_path_convolution_input(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_path_convolution_ready(hrtf_enabled, ir, rel_listener);

bool preflight_hrtf_attenuation_narrowing_ready(HrtfPanPath path, float distance_attenuation,
                                                float occlusion_gain,
                                                const HrtfAttenuationCoupling& coupling,
                                                const BinauralPanParams& params,
                                                HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
        path, distance_attenuation, occlusion_gain, coupling, params);
bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, &reason);

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, &reason);

bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener);

bool preflight_hrtf_convolution_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
        *reason = classify_hrtf_pan_path_convolution_reject(preflight);

bool try_preflight_hrtf_convolution_path(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_convolution_path_ready(hrtf_enabled, ir, rel_listener, &reason);

bool should_skip_hrtf_convolution_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_convolution_path_ready(hrtf_enabled, ir, rel_listener);

bool preflight_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                            params);
        *reason = classify_hrtf_attenuation_coupling_reject(preflight);
    return preflight.can_narrow();

bool try_preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
                                              HrtfAttenuationCouplingRejectReason& reason,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_narrowing_ready(path, distance_attenuation, occlusion_gain,
                                                      coupling, params, &reason);

bool should_skip_hrtf_attenuation_narrowing_input(HrtfPanPath path, float distance_attenuation,
    return !preflight_hrtf_attenuation_narrowing_ready(path, distance_attenuation, occlusion_gain,
                                                       coupling, params);

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                           const Vec3& rel_listener, float distance_attenuation,
                                           HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
        *reason = classify_hrtf_binaural_spatial_reject(preflight);

bool preflight_hrtf_binaural_spatial_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                           float distance_attenuation, float occlusion_gain,
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                                 distance_attenuation, occlusion_gain, coupling,
                                                 params, reason);

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         float occlusion_gain, HrtfBinauralRejectReason& reason,
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,
                                                 params, &reason);

bool should_skip_hrtf_binaural_spatial_input(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,

bool preflight_hrtf_binaural_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,

bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
    return !preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
        *reason = classify_hrtf_binaural_reject(preflight);
    return !preflight.should_skip();

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, coupling, params,

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 HrtfBinauralRejectReason& reason,
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, coupling, params, &reason);

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                         float occlusion_gain, const HrtfAttenuationCoupling& coupling,
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, coupling, params);

bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener,
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,

        *reason = classify_hrtf_binaural_convolution_reject(preflight);

HrtfConvolutionRejectReason classify_hrtf_convolution_reject(
        return HrtfConvolutionRejectReason::None;
        return HrtfConvolutionRejectReason::HrtfDisabled;
        return HrtfConvolutionRejectReason::CoLocated;
        return HrtfConvolutionRejectReason::MalformedIr;
        return HrtfConvolutionRejectReason::EmptyIr;

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,

                                   HrtfBinauralRejectReason* reason,
                                         distance_attenuation, occlusion_gain, reason, coupling,

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
                                         occlusion_gain, reason, coupling, params);

                                   const Vec3& source_position, float distance_attenuation,
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation,

                                         occlusion_gain, &reason, coupling, params);


bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                          occlusion_gain, nullptr, coupling, params);

bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,

bool preflight_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                      HrtfConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
        *reason = classify_hrtf_convolution_reject(preflight);





HrtfPanPathRejectReason classify_hrtf_binaural_pan_reject(const HrtfBinauralPreflight& preflight) {
    return classify_hrtf_pan_path_reject(preflight.panPath);

HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.can_convolve()) {
        return classify_hrtf_ir_reject(preflight.ir);

HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(
    return classify_hrtf_attenuation_coupling_reject(preflight.attenuationCoupling);

bool preflight_hrtf_binaural_spatial_pan_ready(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, reason);

bool preflight_hrtf_binaural_spatial_pan_ready(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, reason);

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    return try_preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener, reason);

bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
    return try_preflight_hrtf_pan_path(hrtf_enabled, rel_listener, reason);

                                               HrtfIrRejectReason* reason) {
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, HrtfIrRejectReason& reason) {
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             float occlusion_gain,
                                             const HrtfAttenuationCoupling& coupling,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
                                             HrtfAttenuationCouplingRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    }
    return preflight.can_narrow_spatial_image();

bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         float occlusion_gain, HrtfBinauralRejectReason& reason,
                                         const BinauralPanParams& params) {
    return preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                                 occlusion_gain, coupling, params, &reason);

bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener, float distance_attenuation,
                                             const HrtfAttenuationCoupling& coupling,
    return preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                     distance_attenuation, occlusion_gain, coupling,
                                                     params, &reason);
}

bool should_skip_hrtf_binaural_convolution_input(bool hrtf_enabled, const HrtfIrStub& ir,
                                                 float occlusion_gain,
    return !preflight_hrtf_binaural_convolution_ready(hrtf_enabled, ir, rel_listener,
                                                      distance_attenuation, occlusion_gain,
                                                      coupling, params);

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const BinauralPanParams& params,
                                             HrtfBinauralRejectReason* reason) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_narrowing_reject(preflight);
    return preflight.can_narrow_spatial_image();

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,

bool should_skip_hrtf_binaural_narrowing_input(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, ir, rel_listener,
                                                     params);
bool try_preflight_hrtf_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                    const Vec3& rel_listener, HrtfConvolutionRejectReason& reason) {
    return preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);

bool should_skip_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                        const Vec3& rel_listener) {
    return !preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener);

bool should_skip_hrtf_binaural_spatial_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    return !preflight_hrtf_binaural_spatial_ready(hrtf_enabled, ir, rel_listener,

bool preflight_hrtf_binaural_narrowing_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                             float distance_attenuation, float occlusion_gain,
                                             HrtfAttenuationCouplingRejectReason* reason) {
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, make_empty_hrtf_ir(),
                                                   rel_listener, distance_attenuation,
                                                   occlusion_gain, coupling, params, reason);

                                           HrtfAttenuationCouplingRejectReason& reason) {

bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,
    return preflight_hrtf_binaural_narrowing_ready(hrtf_enabled, rel_listener, distance_attenuation,
}

} // namespace fuse::audio
