#include <fuse/audio/binaural_pan.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testHrtfPanPathResolution() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f, 0.f};
    const fuse::audio::HrtfIrStub valid{samples, 2};

    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, valid, offset)
                   == fuse::audio::HrtfPanPath::Convolution,
               "valid IR selects convolution path");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, empty, offset)
                   == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR falls back to ILD/ITD stub");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(false, valid, offset)
                   == fuse::audio::HrtfPanPath::Bypass,
               "disabled HRTF bypasses pan");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, valid, fuse::audio::Vec3{})
                   == fuse::audio::HrtfPanPath::Bypass,
               "co-located source bypasses pan");
}

void testShouldUseHrtfIrAlias() {
    const fuse::audio::HrtfIrStub empty{};
    expectTrue(!fuse::audio::should_use_hrtf_ir(empty), "empty IR cannot use convolution");
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty), "default IR stub is empty");
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty) == !fuse::audio::has_hrtf_ir(empty),
               "is_empty_hrtf_ir is inverse of has_hrtf_ir");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::should_use_hrtf_ir(valid), "valid IR enables convolution path");
    expectTrue(fuse::audio::should_use_hrtf_ir(valid) == fuse::audio::has_hrtf_ir(valid),
               "should_use_hrtf_ir matches has_hrtf_ir");
    expectTrue(!fuse::audio::is_empty_hrtf_ir(valid), "valid IR is not empty");

    const fuse::audio::HrtfIrStub null_with_length{nullptr, 4};
    expectTrue(fuse::audio::is_empty_hrtf_ir(null_with_length),
               "null samples with non-zero length remain empty");
}

void testHrtfPanPathBypassGuards() {
    expectTrue(fuse::audio::is_hrtf_pan_bypassed(fuse::audio::HrtfPanPath::Bypass),
               "Bypass path is bypassed");
    expectTrue(!fuse::audio::is_hrtf_pan_bypassed(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub path is active");
    expectTrue(!fuse::audio::is_hrtf_pan_bypassed(fuse::audio::HrtfPanPath::Convolution),
               "convolution path is active");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "should_skip mirrors is_hrtf_pan_bypassed for Bypass");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub path is not skipped");
}

void testItdAndElevationHelpers() {
    const fuse::audio::BinauralPanParams params;
    expectNear(fuse::audio::compute_itd_from_azimuth(0.f, params), 0.f, 1e-5f,
               "ahead azimuth has zero ITD");
    expectNear(fuse::audio::compute_itd_from_azimuth(1.5707963f, params), params.max_itd_seconds,
               1e-5f, "right azimuth reaches max ITD stub");
    expectNear(fuse::audio::compute_itd_from_azimuth(-1.5707963f, params), -params.max_itd_seconds,
               1e-5f, "left azimuth reaches negative max ITD stub");

    expectNear(fuse::audio::compute_elevation_factor(0.f, params), 1.f, 1e-5f,
               "horizon elevation keeps unity factor");
    const float above_factor =
        fuse::audio::compute_elevation_factor(1.5707963f, params);
    expectTrue(above_factor < 1.f, "directly above attenuates via elevation rolloff");
    expectNear(fuse::audio::compute_elevation_factor(-1.5707963f, params), above_factor, 1e-5f,
               "elevation factor is symmetric for above/below");
}

void testIrAwareGuardedPan() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::BinauralPanGains empty_ir =
        fuse::audio::compute_binaural_pan_gains_guarded(true, empty, offset);
    const fuse::audio::BinauralPanGains valid_ir =
        fuse::audio::compute_binaural_pan_gains_guarded(true, valid, offset);
    expectTrue(fuse::audio::compute_pan_spread(empty_ir) > 0.5f,
               "empty IR still produces lateral ILD/ITD stub pan");
    expectNear(empty_ir.left, valid_ir.left, 1e-4f,
               "convolution path stub matches ILD/ITD until IR wired");
    expectNear(empty_ir.right, valid_ir.right, 1e-4f,
               "convolution path stub matches ILD/ITD until IR wired");

    const fuse::audio::BinauralPanGains bypass =
        fuse::audio::compute_binaural_pan_gains_guarded(false, valid, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass), "IR-aware guard bypasses when disabled");
}

void testSpatialBlendHelper() {
    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    const float wide_spread = fuse::audio::compute_pan_spread(wide);

    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_spatial_blend(narrowed, 0.25f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed) < wide_spread,
               "spatial blend narrows pan spread");

    fuse::audio::BinauralPanGains centre = wide;
    fuse::audio::apply_spatial_blend(centre, 0.f);
    expectTrue(fuse::audio::is_centre_panned(centre), "zero blend collapses to centre");

    fuse::audio::BinauralPanGains preserved = wide;
    fuse::audio::apply_spatial_blend(preserved, 1.f);
    expectNear(preserved.left, wide.left, 1e-5f, "unity blend preserves left gain");
    expectNear(preserved.right, wide.right, 1e-5f, "unity blend preserves right gain");
}

void testLerpBinauralPanGains() {
    const fuse::audio::BinauralPanGains centre = fuse::audio::make_centre_binaural_pan_gains();
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});

    const fuse::audio::BinauralPanGains midpoint =
        fuse::audio::lerp_binaural_pan_gains(centre, wide, 0.5f);
    expectTrue(fuse::audio::compute_pan_spread(midpoint) > 0.f,
               "lerp midpoint has non-zero spread");
    expectTrue(fuse::audio::compute_pan_spread(midpoint) < fuse::audio::compute_pan_spread(wide),
               "lerp midpoint is narrower than hard-right pan");

    const fuse::audio::BinauralPanGains at_start =
        fuse::audio::lerp_binaural_pan_gains(centre, wide, 0.f);
    expectNear(at_start.left, centre.left, 1e-5f, "lerp t=0 matches start");
    expectNear(at_start.right, centre.right, 1e-5f, "lerp t=0 matches start");

    const fuse::audio::BinauralPanGains at_end =
        fuse::audio::lerp_binaural_pan_gains(centre, wide, 1.f);
    expectNear(at_end.left, wide.left, 1e-5f, "lerp t=1 matches end");
    expectNear(at_end.right, wide.right, 1e-5f, "lerp t=1 matches end");
}

void testCoupledPanOneShot() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};

    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, offset, 0.15f, 0.2f);
    fuse::audio::BinauralPanGains manual =
        fuse::audio::compute_binaural_pan_gains_guarded(true, offset);
    fuse::audio::apply_hrtf_attenuation_coupling(manual, 0.15f, 0.2f);
    expectNear(coupled.left, manual.left, 1e-5f, "coupled helper matches manual distance coupling");
    expectNear(coupled.right, manual.right, 1e-5f,
               "coupled helper matches manual distance coupling");

    const fuse::audio::BinauralPanGains ir_coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 1.f, 0.1f);
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);
    expectTrue(fuse::audio::compute_pan_spread(ir_coupled)
                   < fuse::audio::compute_pan_spread(wide),
               "IR-aware coupled pan narrows under occlusion");

    const fuse::audio::BinauralPanGains bypassed =
        fuse::audio::compute_binaural_pan_gains_coupled(false, empty, offset, 0.1f, 0.1f);
    expectTrue(fuse::audio::is_centre_panned(bypassed),
               "coupled helper bypasses coupling when HRTF disabled");
}

void testPanPathForPathHelper() {
    const fuse::audio::Vec3 offset{0.f, 0.f, 5.f};
    const fuse::audio::BinauralPanGains bypass =
        fuse::audio::compute_binaural_pan_gains_for_path(fuse::audio::HrtfPanPath::Bypass, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass), "for_path bypass returns centre");

    const fuse::audio::BinauralPanGains stub =
        fuse::audio::compute_binaural_pan_gains_for_path(fuse::audio::HrtfPanPath::IldItdStub,
                                                         offset);
    const fuse::audio::BinauralPanGains direct =
        fuse::audio::compute_binaural_pan_gains(offset);
    expectNear(stub.left, direct.left, 1e-5f, "for_path ILD stub matches direct gains");
    expectNear(stub.right, direct.right, 1e-5f, "for_path ILD stub matches direct gains");

    const fuse::audio::BinauralPanGains convolution =
        fuse::audio::compute_binaural_pan_gains_for_path(fuse::audio::HrtfPanPath::Convolution,
                                                         offset);
    expectNear(convolution.left, direct.left, 1e-5f,
               "for_path convolution stub matches ILD/ITD until IR wired");
    expectNear(convolution.right, direct.right, 1e-5f,
}

void testEmptyHrtfIrFactoryAndAlias() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty), "factory returns empty IR");
    expectTrue(!fuse::audio::has_hrtf_ir(empty), "factory IR has no samples");
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty) == !fuse::audio::has_hrtf_ir(empty),
               "is_empty_hrtf_ir is inverse of has_hrtf_ir");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    expectTrue(fuse::audio::is_empty_hrtf_ir(null_samples_nonzero_length),
               "null samples with non-zero length is treated as empty");

void testHrtfPanPathPredicateHelpers() {
    expectTrue(fuse::audio::is_spatial_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub path is spatial");
    expectTrue(fuse::audio::is_spatial_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution),
               "convolution path is spatial");
    expectTrue(!fuse::audio::is_spatial_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "bypass path is not spatial");

    expectTrue(fuse::audio::hrtf_pan_path_uses_convolution(fuse::audio::HrtfPanPath::Convolution),
               "convolution path uses IR");
    expectTrue(!fuse::audio::hrtf_pan_path_uses_convolution(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub does not use IR");

    expectTrue(
        fuse::audio::should_apply_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub),
        "spatial path applies attenuation coupling");
        !fuse::audio::should_apply_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),
        "bypass path skips attenuation coupling");

void testResolveHrtfPanPathWithoutIr() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, offset)
                   == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload selects ILD/ITD stub");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(false, offset)
                   == fuse::audio::HrtfPanPath::Bypass,
               "no-IR overload bypasses when disabled");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, fuse::audio::Vec3{})
               "no-IR overload bypasses co-located source");

void testClampHrtfAttenuation() {
    expectNear(fuse::audio::clamp_hrtf_attenuation(-0.5f), 0.f, 1e-5f,
               "negative attenuation clamps to zero");
    expectNear(fuse::audio::clamp_hrtf_attenuation(1.5f), 1.f, 1e-5f,
               "above-unity attenuation clamps to one");
    expectNear(fuse::audio::clamp_hrtf_attenuation(0.4f), 0.4f, 1e-5f,
               "in-range attenuation is preserved");

void testCoLocatedHrtfGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectNear(fuse::audio::hrtf_co_located_epsilon(), 1e-5f, 1e-8f,
               "co-located epsilon is exposed");
    expectTrue(fuse::audio::is_co_located_hrtf_source(co_located),
               "zero offset is co-located");
    expectTrue(!fuse::audio::is_co_located_hrtf_source(offset),
               "separated offset is not co-located");
    expectTrue(fuse::audio::should_skip_hrtf_pan(false, offset),
               "disabled HRTF skips pan");
    expectTrue(fuse::audio::should_skip_hrtf_pan(true, co_located),
               "co-located source skips pan");
    expectTrue(!fuse::audio::should_skip_hrtf_pan(true, offset),
               "enabled separated source does not skip pan");
    expectTrue(fuse::audio::should_apply_hrtf_pan(true, offset)
                   == !fuse::audio::should_skip_hrtf_pan(true, offset),
               "should_apply_hrtf_pan is inverse of should_skip_hrtf_pan");
}

void testHrtfIrStubFactory() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_hrtf_ir_stub(nullptr, 0);
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty), "null factory IR is empty");

    const float samples[] = {0.25f, -0.25f};
    const fuse::audio::HrtfIrStub valid =
        fuse::audio::make_hrtf_ir_stub(samples, static_cast<fuse::u32>(2));
    expectTrue(fuse::audio::has_hrtf_ir(valid), "factory IR with samples is valid");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, valid, fuse::audio::Vec3{5.f, 0.f, 0.f})
                   == fuse::audio::HrtfPanPath::Convolution,
               "factory IR selects convolution path");
}

void testBypassPanPathHelpers() {
    expectTrue(fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "bypass path predicate");
    expectTrue(!fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub is not bypass");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),
               "bypass skips attenuation coupling");
    expectTrue(
        !fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution),
        "convolution path does not skip attenuation coupling");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass)
                   == !fuse::audio::should_apply_hrtf_attenuation_coupling(
                          fuse::audio::HrtfPanPath::Bypass),
               "skip/apply attenuation coupling are inverses on bypass");
}

void testUnityHrtfAttenuationGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_attenuation(1.f, 1.f),
               "unity attenuation at full gain");
    expectTrue(fuse::audio::is_unity_hrtf_attenuation(1.5f, 2.f),
               "out-of-range attenuation clamps to unity");
    expectTrue(!fuse::audio::is_unity_hrtf_attenuation(0.5f, 1.f),
               "reduced distance attenuation is non-unity");

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);
    fuse::audio::BinauralPanGains unchanged = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        unchanged, fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f);
    expectNear(unchanged.left, wide.left, 1e-5f,
               "unity attenuation skips spatial narrowing");
    expectNear(unchanged.right, wide.right, 1e-5f,
               "unity attenuation skips spatial narrowing");

    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::Bypass,
                                                              0.1f, 0.1f),
               "bypass never narrows spatial image");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f),
               "unity attenuation does not narrow spatial image");
    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f),
               "reduced distance attenuation narrows spatial image");
}

void testClampHrtfAttenuationCouplingWeight() {
    expectNear(fuse::audio::clamp_hrtf_attenuation_coupling_weight(-0.2f), 0.f, 1e-5f,
               "negative coupling weight clamps to zero");
    expectNear(fuse::audio::clamp_hrtf_attenuation_coupling_weight(1.5f), 1.f, 1e-5f,
               "above-unity coupling weight clamps to one");

    const fuse::audio::HrtfAttenuationCoupling distance_only{.occlusion_weight = 0.f};
    const fuse::audio::HrtfAttenuationCoupling occlusion_only{.occlusion_weight = 1.f};
    expectNear(fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f, distance_only), 0.4f, 1e-5f,
               "zero occlusion weight uses distance attenuation only");
    expectNear(fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f, occlusion_only), 0.85f, 1e-5f,
               "unity occlusion weight uses occlusion gain only");
}

void testCoupledPanVec3UsesCoupledForPath() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, offset, 0.3f, 0.4f);
    const fuse::audio::BinauralPanGains via_path =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::resolve_hrtf_pan_path(true, offset), offset, 0.3f, 0.4f);
    expectNear(coupled.left, via_path.left, 1e-5f,
               "Vec3 coupled helper matches coupled_for_path");
    expectNear(coupled.right, via_path.right, 1e-5f,
               "Vec3 coupled helper matches coupled_for_path");
}

void testShouldSkipHrtfPanGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    expectTrue(fuse::audio::should_skip_hrtf_pan(false, offset),
               "disabled HRTF skips pan");
    expectTrue(fuse::audio::should_skip_hrtf_pan(true, fuse::audio::Vec3{}),
               "co-located source skips pan");
    expectTrue(!fuse::audio::should_skip_hrtf_pan(true, offset),
               "enabled offset does not skip pan");
    expectTrue(fuse::audio::should_apply_hrtf_pan(true, offset)
                   == !fuse::audio::should_skip_hrtf_pan(true, offset),
               "should_apply is inverse of should_skip");
}

void testEmptyIrConvolutionSkipGuards() {
void testHrtfIrPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.emptyIr, "preflight marks empty IR");
    expectTrue(!empty_preflight.malformedIr, "canonical empty IR is not malformed");
    expectTrue(!empty_preflight.can_convolute(), "empty IR preflight cannot convolute");
    expectTrue(empty_preflight.canConvolution == empty_preflight.can_convolute(),
               "canConvolution matches can_convolute");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(valid_preflight.can_convolute(), "valid IR preflight can convolute");
    expectTrue(!valid_preflight.emptyIr, "valid IR preflight is not empty");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.malformedIr, "preflight marks malformed IR");
    expectTrue(!malformed_preflight.can_convolute(), "malformed IR cannot convolute");

void testShouldSkipHrtfConvolution() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::should_skip_hrtf_convolution(empty),
               "empty IR skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(empty)
                   == fuse::audio::is_empty_hrtf_ir(empty),
               "skip convolution matches is_empty");

    const float samples[] = {0.25f};
                   == fuse::audio::should_fallback_hrtf_to_ild_itd_stub(empty),
               "skip convolution matches ILD fallback alias");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(!fuse::audio::should_skip_hrtf_convolution(valid),
               "valid IR does not skip convolution");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(fuse::audio::is_nonnull_zero_length_hrtf_ir(zero_length),
               "non-null zero-length IR is recognised");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(zero_length),
               "non-null zero-length IR skips convolution");

void testHrtfPanPathBypassAndIldStubPredicates() {
    expectTrue(fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "bypass path predicate");
    expectTrue(!fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub is not bypass");
    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub path predicate");
    expectTrue(!fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::Convolution),
               "convolution path is not ILD stub");

    expectTrue(
        fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),
        "bypass skips attenuation coupling");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution),
               "convolution path applies attenuation coupling");
    expectTrue(fuse::audio::should_apply_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub)
                   == !fuse::audio::should_skip_hrtf_attenuation_coupling(
                          fuse::audio::HrtfPanPath::IldItdStub),
               "should_apply coupling is inverse of should_skip");

void testSpatialBlendSkipGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(1.f), "unity blend at one");
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(0.99999f), "near-unity blend");
    expectTrue(!fuse::audio::is_unity_hrtf_spatial_blend(0.5f), "partial blend is not unity");

    expectTrue(fuse::audio::should_skip_hrtf_spatial_blend(1.f, 1.f),
               "full attenuation skips spatial narrowing");
    expectTrue(!fuse::audio::should_skip_hrtf_spatial_blend(0.f, 0.f),
               "zero attenuation requires spatial narrowing");

    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    const float wide_spread = fuse::audio::compute_pan_spread(wide);

    fuse::audio::BinauralPanGains guarded = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(guarded, 1.f);
    expectNear(guarded.left, wide.left, 1e-5f, "guarded unity blend preserves left");
    expectNear(guarded.right, wide.right, 1e-5f, "guarded unity blend preserves right");

    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(narrowed, 0.25f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed) < wide_spread,
               "guarded partial blend narrows image");

void testClampHrtfOcclusionCouplingWeight() {
    expectNear(fuse::audio::clamp_hrtf_occlusion_coupling_weight(-0.2f), 0.f, 1e-5f,
               "negative coupling weight clamps to zero");
    expectNear(fuse::audio::clamp_hrtf_occlusion_coupling_weight(1.5f), 1.f, 1e-5f,
               "above-unity coupling weight clamps to one");
    expectNear(fuse::audio::clamp_hrtf_occlusion_coupling_weight(0.6f), 0.6f, 1e-5f,
               "in-range coupling weight is preserved");

void testAttenuationCouplingForPath() {
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);

    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        narrowed, fuse::audio::HrtfPanPath::IldItdStub, 0.1f, 0.2f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed)
                   < fuse::audio::compute_pan_spread(wide),
               "for_path coupling narrows spatial image");

    fuse::audio::BinauralPanGains bypassed = wide;
        bypassed, fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f);
    expectNear(bypassed.left, wide.left, 1e-5f, "bypass for_path coupling leaves gains unchanged");
    expectNear(bypassed.right, wide.right, 1e-5f,
               "bypass for_path coupling leaves gains unchanged");

    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::HrtfPanPath::Convolution, offset, 0.2f, 0.3f);
    fuse::audio::BinauralPanGains manual =
        manual, fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f);
    expectNear(coupled.left, manual.left, 1e-5f,
               "coupled_for_path matches manual path gains + coupling");
    expectNear(coupled.right, manual.right, 1e-5f,

void testMakeHrtfIrStubFactory() {
    const float samples[] = {0.25f, -0.5f};
    const fuse::audio::HrtfIrStub valid =
        fuse::audio::make_hrtf_ir_stub(samples, static_cast<fuse::u32>(2));
    expectTrue(fuse::audio::has_hrtf_ir(valid), "factory preserves valid IR");
    expectTrue(!fuse::audio::is_empty_hrtf_ir(valid), "factory valid IR is not empty");

    const fuse::audio::HrtfIrStub null_samples =
        fuse::audio::make_hrtf_ir_stub(nullptr, 8);
    expectTrue(fuse::audio::is_empty_hrtf_ir(null_samples),
               "factory null samples returns empty IR");

    const fuse::audio::HrtfIrStub zero_length =
        fuse::audio::make_hrtf_ir_stub(samples, 0);
    expectTrue(fuse::audio::is_empty_hrtf_ir(zero_length),
               "factory zero length returns empty IR");
    expectTrue(fuse::audio::make_hrtf_ir_stub(samples, 0).length == 0,
               "factory zero length clears length field");

void testHrtfPanPathBypassGuards() {
    expectTrue(fuse::audio::is_hrtf_pan_path_bypass(fuse::audio::HrtfPanPath::Bypass),
               "bypass path is flagged as bypass");
    expectTrue(!fuse::audio::is_hrtf_pan_path_bypass(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub is not bypass");
    expectTrue(fuse::audio::should_skip_hrtf_spatial_pan(fuse::audio::HrtfPanPath::Bypass),
               "bypass path skips spatial pan");
    expectTrue(!fuse::audio::should_skip_hrtf_spatial_pan(fuse::audio::HrtfPanPath::Convolution),
               "convolution path does not skip spatial pan");

    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_itd_stub(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub path predicate");
    expectTrue(!fuse::audio::hrtf_pan_path_uses_ild_itd_stub(fuse::audio::HrtfPanPath::Convolution),
               "convolution path is not ILD/ITD stub");

    const fuse::audio::Vec3 offset{3.f, 0.f, 0.f};
    expectTrue(fuse::audio::should_bypass_hrtf_pan(false, offset),
               "disabled HRTF should bypass pan");
    expectTrue(!fuse::audio::should_bypass_hrtf_pan(true, offset),
               "enabled offset source should not bypass pan");
    expectTrue(fuse::audio::should_bypass_hrtf_pan(true, fuse::audio::Vec3{}),
               "co-located source should bypass pan");
    expectTrue(fuse::audio::should_bypass_hrtf_pan(false, offset)
                   == !fuse::audio::should_apply_hrtf_pan(false, offset),
               "should_bypass_hrtf_pan inverts should_apply_hrtf_pan");

void testCoLocatedHrtfGuards() {
    const fuse::audio::Vec3 co_located{};

    expectNear(fuse::audio::hrtf_co_located_epsilon(), 1e-5f, 1e-8f,
               "co-located epsilon is exposed");
    expectTrue(fuse::audio::is_co_located_hrtf_source(co_located),
               "zero offset is co-located");
    expectTrue(!fuse::audio::is_co_located_hrtf_source(offset),
               "separated offset is not co-located");
    expectTrue(fuse::audio::should_skip_hrtf_pan(true, co_located),
               "enabled separated source does not skip pan");
               "should_apply_hrtf_pan is inverse of should_skip_hrtf_pan");
    expectTrue(fuse::audio::should_bypass_hrtf_pan(true, co_located)
                   == fuse::audio::should_skip_hrtf_pan(true, co_located),
               "should_bypass_hrtf_pan matches should_skip_hrtf_pan");

void testBypassPanPathAndAttenuationSkipGuards() {
               "bypass path predicate alias");
    expectTrue(fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass)
                   == fuse::audio::is_hrtf_pan_path_bypass(fuse::audio::HrtfPanPath::Bypass),
               "bypass aliases agree");
        !fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution),
        "convolution path does not skip attenuation coupling");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass)
                   == !fuse::audio::should_apply_hrtf_attenuation_coupling(
                          fuse::audio::HrtfPanPath::Bypass),
               "skip/apply attenuation coupling are inverses on bypass");

void testUnityHrtfAttenuationGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_attenuation(1.f, 1.f),
               "unity attenuation at full gain");
    expectTrue(fuse::audio::is_unity_hrtf_attenuation(1.5f, 2.f),
               "out-of-range attenuation clamps to unity");
    expectTrue(!fuse::audio::is_unity_hrtf_attenuation(0.5f, 1.f),
               "reduced distance attenuation is non-unity");

    fuse::audio::BinauralPanGains unchanged = wide;
        unchanged, fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f);
    expectNear(unchanged.left, wide.left, 1e-5f,
               "unity attenuation skips spatial narrowing");
    expectNear(unchanged.right, wide.right, 1e-5f,

    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::Bypass,
                                                              0.1f, 0.1f),
               "bypass never narrows spatial image");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f),
               "unity attenuation does not narrow spatial image");
    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f),
               "reduced distance attenuation narrows spatial image");

void testClampHrtfAttenuationCouplingWeight() {
    expectNear(fuse::audio::clamp_hrtf_attenuation_coupling_weight(-0.2f), 0.f, 1e-5f,
    expectNear(fuse::audio::clamp_hrtf_attenuation_coupling_weight(1.5f), 1.f, 1e-5f,

    const fuse::audio::HrtfAttenuationCoupling distance_only{.occlusion_weight = 0.f};
    const fuse::audio::HrtfAttenuationCoupling occlusion_only{.occlusion_weight = 1.f};
    expectNear(fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f, distance_only), 0.4f, 1e-5f,
               "zero occlusion weight uses distance attenuation only");
    expectNear(fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f, occlusion_only), 0.85f, 1e-5f,
               "unity occlusion weight uses occlusion gain only");

void testCoupledPanVec3UsesCoupledForPath() {
        fuse::audio::compute_binaural_pan_gains_coupled(true, offset, 0.3f, 0.4f);
    const fuse::audio::BinauralPanGains via_path =
            fuse::audio::resolve_hrtf_pan_path(true, offset), offset, 0.3f, 0.4f);
    expectNear(coupled.left, via_path.left, 1e-5f,
               "Vec3 coupled helper matches coupled_for_path");
    expectNear(coupled.right, via_path.right, 1e-5f,

void testShouldSkipHrtfConvolution() {
                   == !fuse::audio::should_use_hrtf_ir(empty),
               "should_skip_hrtf_convolution inverts should_use_hrtf_ir");

    const float samples[] = {0.5f};

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::is_nonnull_zero_length_hrtf_ir(malformed),
               "non-null zero-length IR is malformed");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(malformed),
               "malformed IR skips convolution");
    expectTrue(fuse::audio::is_empty_hrtf_ir(malformed),
               "malformed IR is treated as empty");
}

void testHrtfPanPathPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight enabled =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(!enabled.hrtfDisabled, "enabled preflight clears hrtfDisabled");
    expectTrue(!enabled.coLocated, "offset source is not co-located");
    expectTrue(enabled.emptyIr, "empty IR flagged in pan preflight");
    expectTrue(enabled.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR selects ILD/ITD stub in preflight");
    expectTrue(enabled.can_spatial_pan(), "ILD/ITD stub can spatial pan");
    expectTrue(!enabled.should_skip_pan(), "spatial path does not skip pan");

    const fuse::audio::HrtfPanPathPreflight convolution =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(!convolution.emptyIr, "valid IR clears emptyIr flag");
    expectTrue(convolution.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR selects convolution in preflight");
    expectTrue(fuse::audio::should_use_hrtf_convolution_path(true, valid, offset),
               "convolution path helper agrees with preflight");

    const fuse::audio::HrtfPanPathPreflight bypass =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass.hrtfDisabled, "disabled HRTF flagged in preflight");
    expectTrue(bypass.should_skip_pan(), "disabled preflight skips pan");
    expectTrue(fuse::audio::should_fallback_hrtf_to_ild_itd_stub(true, empty, offset),
               "empty IR fallback helper agrees with preflight");

void testHrtfPanPathSkipAliases() {
    expectTrue(fuse::audio::is_hrtf_pan_bypassed(fuse::audio::HrtfPanPath::Bypass),
               "bypass path is flagged as bypassed");
    expectTrue(!fuse::audio::is_hrtf_pan_bypassed(fuse::audio::HrtfPanPath::Convolution),
               "convolution path is not bypassed");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "should_skip_hrtf_pan_path on bypass");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass)
                   == fuse::audio::is_hrtf_pan_bypassed(fuse::audio::HrtfPanPath::Bypass),
               "skip alias matches bypassed predicate");

    const fuse::audio::HrtfPanPath path =
        fuse::audio::resolve_hrtf_pan_path(false, offset);
    expectTrue(fuse::audio::should_skip_hrtf_pan_path(path),
               "disabled HRTF resolves to skippable pan path");

void testUnitySpatialBlendGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(1.f),
               "unity blend at 1.0");
    expectTrue(fuse::audio::is_convolution_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution),
               "convolution path alias predicate");
    expectTrue(fuse::audio::is_convolution_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution)
                   == fuse::audio::hrtf_pan_path_uses_convolution(
                          fuse::audio::HrtfPanPath::Convolution),
               "convolution alias matches uses_convolution");
    expectTrue(fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub applies spatial pan");
    expectTrue(fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::IldItdStub)
                   == !fuse::audio::should_skip_hrtf_spatial_pan(
                          fuse::audio::HrtfPanPath::IldItdStub),
               "apply spatial pan is inverse of skip");
}

void testHrtfAttenuationCouplingPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
        fuse::audio::resolve_hrtf_pan_path(true, offset);

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(path, 1.f, 1.f);
    expectTrue(unity.unityAttenuation, "unity scalars flagged in coupling preflight");
    expectTrue(unity.unitySpatialBlend, "unity spatial blend flagged in preflight");
    expectTrue(unity.should_skip_coupling(), "unity coupling preflight skips coupling");
    expectTrue(!unity.can_couple(), "unity coupling preflight cannot couple");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(path, 0.2f, 1.f);
    expectTrue(!narrowed.unityAttenuation, "reduced distance is non-unity attenuation");
    expectTrue(!narrowed.unitySpatialBlend, "reduced distance narrows spatial blend");
    expectTrue(narrowed.can_couple(), "non-unity coupling preflight can couple");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                       0.1f);
    expectTrue(bypass.bypassPath, "bypass path flagged in coupling preflight");
    expectTrue(bypass.should_skip_coupling(), "bypass coupling preflight skips coupling");

    expectTrue(fuse::audio::is_unity_hrtf_distance_attenuation(1.5f),
               "above-unity distance attenuation is unity");
    expectTrue(fuse::audio::is_unity_hrtf_occlusion_gain(1.f),
               "full occlusion gain is unity");
    expectTrue(!fuse::audio::is_unity_hrtf_occlusion_gain(0.5f),
               "reduced occlusion gain is non-unity");

    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(1.f), "unity blend at 1.0");
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(1.00001f),
               "above-unity blend treated as unity");
    expectTrue(!fuse::audio::is_unity_hrtf_spatial_blend(0.99f),
               "sub-unity blend is not unity");

    expectTrue(fuse::audio::should_skip_hrtf_spatial_blend(1.f, 1.f),
               "full distance and occlusion skip spatial narrowing");
    expectTrue(!fuse::audio::should_skip_hrtf_spatial_blend(0.2f, 1.f),
               "reduced distance attenuation warrants narrowing");
    expectTrue(!fuse::audio::should_skip_hrtf_spatial_blend(1.f, 0.1f),
               "reduced occlusion gain warrants narrowing");

void testApplyHrtfSpatialBlendGuarded() {

    fuse::audio::apply_hrtf_spatial_blend_guarded(unchanged, 1.f);
               "unity guarded blend leaves left gain unchanged");
               "unity guarded blend leaves right gain unchanged");

}

    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    const float wide_spread = fuse::audio::compute_pan_spread(wide);

    fuse::audio::BinauralPanGains unchanged = wide;
    expectNear(unchanged.left, wide.left, 1e-5f,
    expectNear(unchanged.right, wide.right, 1e-5f,

    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(narrowed, 0.25f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed) < wide_spread,
               "sub-unity guarded blend narrows pan spread");

    fuse::audio::BinauralPanGains coupling_unchanged = wide;
    fuse::audio::apply_hrtf_attenuation_coupling(coupling_unchanged, 1.f, 1.f);
    expectNear(coupling_unchanged.left, wide.left, 1e-5f,
               "unity attenuation coupling is a no-op via spatial blend guard");
    expectNear(coupling_unchanged.right, wide.right, 1e-5f,

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::EmptyIr),
               "canonical empty IR rejects with empty_ir reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::EmptyIr),
                           "empty_ir") == 0,
               "empty_ir reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::EmptyIr),
               "valid IR does not reject as empty");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects with malformed_ir reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir") == 0,
               "malformed_ir reject reason label");
}

void testHrtfIrPreflight() {
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
               "preflight carries empty-IR reject reason");
    expectTrue(empty_preflight.emptyIr, "preflight marks empty IR");
    expectTrue(empty_preflight.nullSamples, "preflight marks null samples");
    expectTrue(empty_preflight.zeroLength, "preflight marks zero length");
    expectTrue(!empty_preflight.malformedIr, "canonical empty IR is not malformed");
    expectTrue(!empty_preflight.can_convolve(), "empty IR preflight cannot convolve");
    expectTrue(!fuse::audio::can_convolve_hrtf_ir(empty_preflight),
               "can_convolve_hrtf_ir mirrors preflight");
    const fuse::audio::HrtfIrPreflight empty =
        fuse::audio::preflight_hrtf_ir(fuse::audio::make_empty_hrtf_ir());
    expectTrue(empty.empty_ir, "preflight marks factory empty IR");
    expectTrue(empty.null_samples, "preflight marks null samples on empty IR");
    expectTrue(empty.zero_length, "preflight marks zero length on empty IR");
    expectTrue(!empty.can_use_convolution(), "preflight rejects convolution on empty IR");
    expectTrue(empty.should_fallback_to_ild_itd(), "preflight expects ILD/ITD fallback");
void testHrtfIrPreflightGuards() {
    expectTrue(empty.null_samples, "empty IR preflight marks null samples");
    expectTrue(empty.zero_length, "empty IR preflight marks zero length");
    expectTrue(empty.isEmpty(), "empty IR preflight reports empty");
    expectTrue(!empty.canUseConvolution(), "empty IR preflight rejects convolution");
    expectTrue(!fuse::audio::can_use_hrtf_ir_for_convolution(fuse::audio::make_empty_hrtf_ir()),
               "can_use_hrtf_ir_for_convolution rejects empty IR");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(valid_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight has no reject reason");
    expectTrue(!valid_preflight.emptyIr, "valid IR is not empty");
    expectTrue(!valid_preflight.nullSamples, "valid IR has samples");
    expectTrue(!valid_preflight.zeroLength, "valid IR has non-zero length");
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(fuse::audio::can_convolve_hrtf_ir(valid_preflight)
                   == fuse::audio::should_use_hrtf_ir(valid),
               "preflight can_convolve matches should_use_hrtf_ir");

    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR preflight carries malformed reject reason");
    expectTrue(malformed_preflight.malformedIr, "preflight marks malformed IR");
    expectTrue(malformed_preflight.emptyIr, "malformed IR is treated as empty");
    expectTrue(!malformed_preflight.can_convolve(), "malformed IR cannot convolve");

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects with hrtf_disabled reason");
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects with co_located reason");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located") == 0,
               "co_located pan-path reject reason label");
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR preflight carries malformed reject reason");
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples reason");
    expectTrue(valid_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight has none reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::NullSamples),
               "hrtf_ir_rejects_for_reason matches empty IR");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::MalformedIr)) == "malformed_ir",
               "IR reject reason label for malformed_ir");
}

void testHrtfPanPathPreflight() {
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "ILD/ITD stub path has no pan-path reject reason");
    expectTrue(stub_preflight.can_spatial_pan(), "empty IR still spatial-pans via ILD/ITD");
    expectTrue(!stub_preflight.can_convolve(), "empty IR preflight does not convolve");
    expectTrue(stub_preflight.emptyIr, "preflight marks empty IR");
    expectTrue(stub_preflight.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR preflight selects ILD/ITD stub");
    expectTrue(!stub_preflight.skipped, "ILD/ITD stub path is not skipped");

    const fuse::audio::HrtfPanPathPreflight conv_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(conv_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(conv_preflight.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR preflight selects convolution path");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF preflight carries hrtf_disabled reject reason");
    expectTrue(disabled_preflight.hrtfDisabled, "preflight marks disabled HRTF");
    expectTrue(disabled_preflight.skipped, "disabled HRTF preflight is skipped");
    expectTrue(!disabled_preflight.can_spatial_pan(), "disabled HRTF cannot spatial-pan");
    expectTrue(disabled_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled HRTF preflight selects bypass");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located preflight carries co_located reject reason");
    expectTrue(co_located_preflight.coLocated, "preflight marks co-located source");
    expectTrue(co_located_preflight.skipped, "co-located preflight is skipped");
    expectTrue(!fuse::audio::can_apply_spatial_hrtf_pan(co_located_preflight),
               "co-located source cannot spatial-pan");

    const fuse::audio::HrtfPanPathPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir_preflight.path == fuse::audio::HrtfPanPath::IldItdStub,
    expectTrue(no_ir_preflight.path
                   == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "no-IR preflight path matches resolve_hrtf_pan_path");

void testHrtfBinauralCompositePreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.can_spatial_pan(), "composite preflight spatial-pans via ILD/ITD");
    expectTrue(!stub_preflight.can_convolve(), "composite preflight does not convolve empty IR");
    expectTrue(stub_preflight.can_narrow(), "composite preflight narrows under attenuation");
    expectTrue(stub_preflight.panPath.path == fuse::audio::HrtfPanPath::IldItdStub,
               "composite preflight selects ILD/ITD stub path");
    expectTrue(stub_preflight.ir.emptyIr, "composite preflight carries empty IR diagnostics");
    expectTrue(stub_preflight.attenuation.spatialBlend < 1.f,
               "composite preflight exposes sub-unity spatial blend");
    expectTrue(fuse::audio::can_apply_binaural_hrtf_pan(stub_preflight),
               "can_apply_binaural_hrtf_pan mirrors composite can_spatial_pan");
    expectTrue(!fuse::audio::can_convolve_binaural_hrtf(stub_preflight),
               "can_convolve_binaural_hrtf mirrors composite can_convolve");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.can_convolve(), "valid IR composite preflight can convolve");
    expectTrue(fuse::audio::can_convolve_binaural_hrtf(conv_preflight)
                   == fuse::audio::should_use_hrtf_ir(valid),
               "composite can_convolve matches should_use_hrtf_ir on valid path");
    expectTrue(!conv_preflight.can_narrow(), "unity attenuation composite preflight cannot narrow");
    expectTrue(!fuse::audio::can_narrow_binaural_hrtf_spatial_image(conv_preflight),
               "can_narrow mirrors composite at unity attenuation");

    const fuse::audio::HrtfBinauralPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(disabled_preflight.skipped, "disabled HRTF composite preflight is skipped");
    expectTrue(!disabled_preflight.can_spatial_pan(), "disabled HRTF cannot spatial-pan");
    expectTrue(!fuse::audio::can_apply_binaural_hrtf_pan(disabled_preflight),
               "can_apply rejects disabled composite preflight");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.panPath.coLocated,
               "composite preflight marks co-located source");
    expectTrue(co_located_preflight.skipped, "co-located composite preflight is skipped");
    expectTrue(!co_located_preflight.can_narrow(),
               "co-located composite preflight cannot narrow");

    const fuse::audio::HrtfBinauralPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.2f, 0.3f);
    expectTrue(no_ir_preflight.panPath.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR composite overload selects ILD/ITD stub");
    expectTrue(no_ir_preflight.panPath.path
                   == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "no-IR composite path matches resolve_hrtf_pan_path");

    const fuse::audio::HrtfBinauralPreflight for_path_preflight =
        fuse::audio::preflight_hrtf_binaural_for_path(fuse::audio::HrtfPanPath::Convolution,
                                                      valid, 0.2f, 0.3f);
    expectTrue(for_path_preflight.can_convolve(), "for_path composite preflight can convolve");
    expectTrue(for_path_preflight.can_narrow(), "for_path composite preflight can narrow");
    expectTrue(for_path_preflight.attenuation.spatialBlend < 1.f,
               "for_path composite exposes spatial blend");

    const fuse::audio::HrtfBinauralPreflight bypass_for_path =
        fuse::audio::preflight_hrtf_binaural_for_path(fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f);
    expectTrue(bypass_for_path.skipped, "for_path bypass composite preflight is skipped");
    expectTrue(!bypass_for_path.can_spatial_pan(), "for_path bypass cannot spatial-pan");

    const fuse::audio::BinauralPanGains via_composite =
        fuse::audio::compute_binaural_pan_gains_coupled(true, valid, offset, 0.2f, 0.3f);
    const fuse::audio::BinauralPanGains via_path =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::HrtfPanPath::Convolution, offset, 0.2f, 0.3f);
    expectNear(via_composite.left, via_path.left, 1e-5f,
               "IR-aware coupled helper matches for_path on convolution path");
    expectNear(via_composite.right, via_path.right, 1e-5f,

    const fuse::audio::BinauralPanGains bypassed =
        fuse::audio::compute_binaural_pan_gains_coupled(false, valid, offset, 0.1f, 0.1f);
    expectTrue(fuse::audio::is_centre_panned(bypassed),
               "composite coupled helper bypasses to centre when disabled");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects coupling with bypass_path reason");
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects coupling with unity_attenuation reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation on spatial path has no coupling reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                           "unity_attenuation") == 0,
               "unity_attenuation coupling reject reason label");
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial stub path has none pan reject reason");
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF pan preflight carries hrtf_disabled reject reason");
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located pan preflight carries co_located reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(false, offset,
                                                             fuse::audio::HrtfPanPathRejectReason::
                                                                 HrtfDisabled),
               "hrtf_pan_path_rejects_for_reason matches disabled HRTF");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingPreflight() {

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation preflight carries unity_attenuation reject reason");
    expectTrue(unity_preflight.unityAttenuation, "preflight marks unity attenuation");
    expectTrue(unity_preflight.skipped, "unity attenuation preflight is skipped");
    expectTrue(!unity_preflight.can_narrow(), "unity attenuation cannot narrow");
    expectTrue(!fuse::audio::can_narrow_hrtf_spatial_image(unity_preflight),
               "can_narrow mirrors preflight");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass,
                                                         0.1f, 0.1f);
    expectTrue(bypass_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path preflight carries bypass_path reject reason");
    expectTrue(bypass_preflight.bypassPath, "preflight marks bypass path");
    expectTrue(bypass_preflight.skipped, "bypass path preflight is skipped");
    expectTrue(!bypass_preflight.can_narrow(), "bypass path cannot narrow");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight has no coupling reject reason");
    expectTrue(narrow_preflight.can_narrow(), "reduced attenuation can narrow");
    expectTrue(narrow_preflight.spatialBlend < 1.f, "preflight exposes sub-unity spatial blend");
    expectTrue(narrow_preflight.can_narrow()
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "preflight can_narrow matches should_narrow_hrtf_spatial_image");

    fuse::audio::BinauralPanGains via_preflight = wide;
        via_preflight, fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f);
    fuse::audio::BinauralPanGains manual = wide;
    fuse::audio::apply_hrtf_attenuation_coupling(manual, 0.2f, 0.3f);
    expectNear(via_preflight.left, manual.left, 1e-5f,
               "for_path coupling via preflight matches manual coupling");
    expectNear(via_preflight.right, manual.right, 1e-5f,

    expectNear(bypassed.left, wide.left, 1e-5f,
               "bypass preflight leaves gains unchanged");
void testSpatialBlendForPathGuards() {
    expectNear(fuse::audio::compute_hrtf_spatial_blend_for_path(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f),
               1.f, 1e-5f, "bypass path returns unity spatial blend");
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f),
               1.f, 1e-5f, "unity attenuation on spatial path returns unity blend");
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.8f),
               fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f), 1e-5f,
               "non-unity spatial path matches base spatial blend");

void testAttenuationCouplingGuardedHelpers() {

    fuse::audio::apply_hrtf_attenuation_coupling_guarded(guarded, true, offset, 0.2f, 0.3f);
        manual, fuse::audio::resolve_hrtf_pan_path(true, offset), 0.2f, 0.3f);
    expectNear(guarded.left, manual.left, 1e-5f,
               "guarded coupling matches manual path coupling");
    expectNear(guarded.right, manual.right, 1e-5f,

    fuse::audio::BinauralPanGains ir_guarded = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_guarded(ir_guarded, true, empty, offset, 0.2f,
                                                         0.3f);
    expectNear(ir_guarded.left, guarded.left, 1e-5f,
               "IR-aware guarded coupling matches empty-IR path");
    expectNear(ir_guarded.right, guarded.right, 1e-5f,

    fuse::audio::apply_hrtf_attenuation_coupling_guarded(bypassed, false, offset, 0.1f, 0.1f);
    expectNear(bypassed.left, wide.left, 1e-5f, "disabled HRTF skips guarded coupling");
    expectNear(bypassed.right, wide.right, 1e-5f, "disabled HRTF skips guarded coupling");

void testGuardedBinauralPanSampleApply() {

    float left = 0.f;
    float right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_for_path(fuse::audio::HrtfPanPath::Bypass, 1.f, wide,
                                                        0.5f, left, right);
    expectNear(left, 0.5f, 1e-5f, "for_path bypass applies centre mono to left");
    expectNear(right, 0.5f, 1e-5f, "for_path bypass applies centre mono to right");

    left = 0.f;
    right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_for_path(fuse::audio::HrtfPanPath::IldItdStub, 1.f,
                                                       wide, 0.5f, left, right);
    expectNear(left, 0.5f * wide.left, 1e-5f, "for_path spatial stub scales left");
    expectNear(right, 0.5f * wide.right, 1e-5f, "for_path spatial stub scales right");

    fuse::audio::apply_guarded_binaural_pan_to_sample(false, offset, 1.f, wide, 0.25f, left,
                                                      right);
    expectNear(left, 0.25f, 1e-5f, "guarded sample apply bypasses to centre when disabled");
    expectNear(right, 0.25f, 1e-5f, "guarded sample apply bypasses to centre when disabled");

    fuse::audio::apply_guarded_binaural_pan_to_sample(true, fuse::audio::Vec3{}, 1.f, wide, 0.25f,
                                                      left, right);
    expectNear(left, 0.25f, 1e-5f, "guarded sample apply bypasses co-located source");
    expectNear(right, 0.25f, 1e-5f, "guarded sample apply bypasses co-located source");
void testShouldSkipHrtfIrConvolution() {
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty),
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty)
               "should_skip_hrtf_ir_convolution matches is_empty_hrtf_ir");

    expectTrue(!fuse::audio::should_skip_hrtf_ir_convolution(valid),
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(valid)
                   == !fuse::audio::should_use_hrtf_ir(valid),
               "should_skip_hrtf_ir_convolution inverts should_use_hrtf_ir");

void testShouldFallbackHrtfToIldItdStub() {

    expectTrue(fuse::audio::should_fallback_hrtf_to_ild_itd_stub(empty),
               "empty IR falls back to ILD/ITD stub");
    expectTrue(fuse::audio::should_fallback_hrtf_to_ild_itd_stub(empty)
                   == fuse::audio::should_skip_hrtf_ir_convolution(empty),
               "fallback alias matches skip convolution on empty IR");

    expectTrue(!fuse::audio::should_fallback_hrtf_to_ild_itd_stub(valid),
               "valid IR does not fall back to ILD/ITD stub");

    expectTrue(fuse::audio::should_fallback_hrtf_to_ild_itd_stub(true, empty, offset),
               "enabled separated source with empty IR falls back");
    expectTrue(!fuse::audio::should_fallback_hrtf_to_ild_itd_stub(true, valid, offset),
               "enabled separated source with valid IR does not fall back");
    expectTrue(!fuse::audio::should_fallback_hrtf_to_ild_itd_stub(false, empty, offset),
               "disabled HRTF does not fall back to ILD/ITD stub");
    expectTrue(!fuse::audio::should_fallback_hrtf_to_ild_itd_stub(true, empty, fuse::audio::Vec3{}),
               "co-located source does not fall back to ILD/ITD stub");

void testShouldUseHrtfConvolutionPath() {

    expectTrue(!fuse::audio::should_use_hrtf_convolution_path(true, empty, offset),
               "empty IR does not select convolution path");
    expectTrue(fuse::audio::should_use_hrtf_convolution_path(true, valid, offset),
               "valid IR selects convolution path");
    expectTrue(!fuse::audio::should_use_hrtf_convolution_path(false, valid, offset),
               "disabled HRTF skips convolution path");
    expectTrue(fuse::audio::should_use_hrtf_convolution_path(true, valid, offset)
                   == (fuse::audio::resolve_hrtf_pan_path(true, valid, offset)
                       == fuse::audio::HrtfPanPath::Convolution),
               "convolution path guard matches resolve_hrtf_pan_path");

void testShouldApplyHrtfSpatialPan() {
    expectTrue(fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub applies spatial pan");
    expectTrue(fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::Convolution),
               "convolution path applies spatial pan");
    expectTrue(!fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::Bypass),
               "bypass path does not apply spatial pan");
    expectTrue(fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::Bypass)
                   == !fuse::audio::should_skip_hrtf_spatial_pan(fuse::audio::HrtfPanPath::Bypass),
               "should_apply_hrtf_spatial_pan inverts should_skip_hrtf_spatial_pan");

void testIsConvolutionHrtfPanPathAlias() {
    expectTrue(fuse::audio::is_convolution_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution),
               "convolution path alias");
    expectTrue(!fuse::audio::is_convolution_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub is not convolution alias");
    expectTrue(fuse::audio::is_convolution_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution)
                   == fuse::audio::hrtf_pan_path_uses_convolution(
               "is_convolution_hrtf_pan_path matches hrtf_pan_path_uses_convolution");

void testPerScalarUnityHrtfAttenuationGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_distance_attenuation(1.f),
               "unity distance attenuation");
    expectTrue(fuse::audio::is_unity_hrtf_distance_attenuation(1.5f),
               "above-unity distance clamps to unity");
    expectTrue(!fuse::audio::is_unity_hrtf_distance_attenuation(0.5f),
               "reduced distance is non-unity");

    expectTrue(fuse::audio::is_unity_hrtf_occlusion_gain(1.f), "unity occlusion gain");
    expectTrue(!fuse::audio::is_unity_hrtf_occlusion_gain(0.3f),
               "reduced occlusion is non-unity");

               "combined unity matches per-scalar unity");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(1.f, 1.f),
               "should_skip matches combined unity");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(0.5f, 1.f),
               "reduced distance skips unity coupling guard");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(1.f, 0.4f),
               "reduced occlusion skips unity coupling guard");

void testApplyHrtfAttenuationCouplingUnityEarlyOut() {

    fuse::audio::apply_hrtf_attenuation_coupling(unchanged, 1.f, 1.f);
               "apply_hrtf_attenuation_coupling early-outs at unity");

    expectTrue(fuse::audio::should_apply_hrtf_distance_coupling(0.2f),
               "reduced distance warrants distance coupling");
    expectTrue(!fuse::audio::should_apply_hrtf_distance_coupling(1.f),
               "unity distance skips distance coupling");
    expectTrue(fuse::audio::should_apply_hrtf_occlusion_coupling(0.1f),
               "reduced occlusion warrants occlusion coupling");
    expectTrue(!fuse::audio::should_apply_hrtf_occlusion_coupling(1.f),
               "unity occlusion skips occlusion coupling");
void testEmptyIrFallbackAndNormalizeGuards() {
    expectTrue(fuse::audio::should_fallback_to_ild_itd_stub(empty),
               "empty IR should fall back to ILD/ITD stub");
    expectTrue(fuse::audio::should_fallback_to_ild_itd_stub(empty)
               "fallback predicate matches is_empty_hrtf_ir");
    expectTrue(fuse::audio::hrtf_ir_stub_sample_count(empty) == 0,
               "empty IR reports zero sample count");

    expectTrue(!fuse::audio::should_fallback_to_ild_itd_stub(valid),
    expectTrue(fuse::audio::hrtf_ir_stub_sample_count(valid) == 2,
               "valid IR reports sample count");

    const fuse::audio::HrtfIrStub invalid{nullptr, 4};
    const fuse::audio::HrtfIrStub normalized = fuse::audio::normalize_hrtf_ir_stub(invalid);
    expectTrue(fuse::audio::is_empty_hrtf_ir(normalized),
               "normalize coerces invalid IR to empty");
    expectTrue(fuse::audio::normalize_hrtf_ir_stub(valid).samples == valid.samples,
               "normalize preserves valid IR samples");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, normalized, fuse::audio::Vec3{5.f, 0.f, 0.f})
                   == fuse::audio::HrtfPanPath::IldItdStub,
               "normalized empty IR resolves to ILD/ITD stub");

void testApplyBinauralPanForPathToSample() {

    fuse::audio::apply_binaural_pan_for_path_to_sample(fuse::audio::HrtfPanPath::IldItdStub, 1.f,
    expectNear(left, 0.5f * wide.left, 1e-5f, "spatial path applies pan gains to left");
    expectNear(right, 0.5f * wide.right, 1e-5f, "spatial path applies pan gains to right");

    fuse::audio::apply_binaural_pan_for_path_to_sample(fuse::audio::HrtfPanPath::Bypass, 1.f, wide,
    expectNear(left, 0.5f, 1e-5f, "bypass path applies centre mono to left");
    expectNear(right, 0.5f, 1e-5f, "bypass path applies centre mono to right");

void testNonUnityAndPreserveSpatialImageGuards() {
    expectTrue(fuse::audio::is_non_unity_hrtf_attenuation(0.5f, 1.f),
    expectTrue(!fuse::audio::is_non_unity_hrtf_attenuation(1.f, 1.f),
               "unity attenuation is not non-unity");
    expectTrue(fuse::audio::is_non_unity_hrtf_attenuation(1.f, 1.f)
                   == !fuse::audio::is_unity_hrtf_attenuation(1.f, 1.f),
               "non-unity is inverse of unity attenuation");

    expectTrue(fuse::audio::should_preserve_hrtf_spatial_image(
               "unity attenuation preserves spatial image");
    expectTrue(!fuse::audio::should_preserve_hrtf_spatial_image(
               "reduced distance attenuation does not preserve spatial image");
               "bypass always preserves spatial image");
                   fuse::audio::HrtfPanPath::IldItdStub, 0.3f, 0.4f)
                   == !fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::IldItdStub, 0.3f, 0.4f),
               "preserve is inverse of narrow spatial image");

void testCombinedAttenuationCouplingPredicate() {
               "combined predicate applies coupling for spatial non-unity attenuation");
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(
               "combined predicate skips unity attenuation");
               "combined predicate skips bypass path");
                   fuse::audio::HrtfPanPath::IldItdStub, 0.5f, 0.5f)
                          fuse::audio::HrtfPanPath::IldItdStub, 0.5f, 0.5f),
               "combined predicate matches narrow spatial image guard");

void testGuardedSpatialBlendHelper() {
    expectNear(fuse::audio::compute_hrtf_spatial_blend_guarded(
                   fuse::audio::HrtfPanPath::Bypass, 0.f, 0.f),
               1.f, 1e-5f, "guarded blend preserves unity on bypass");
               1.f, 1e-5f, "guarded blend preserves unity at full attenuation");

    const float guarded =
        fuse::audio::compute_hrtf_spatial_blend_guarded(fuse::audio::HrtfPanPath::IldItdStub,
                                                        0.2f, 0.8f);
    const float direct = fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f);
    expectNear(guarded, direct, 1e-5f,
               "guarded blend matches direct blend for spatial non-unity attenuation");
void testEmptyIrConvolutionGuards() {

    expectTrue(!fuse::audio::should_apply_hrtf_ir_convolution(empty),
               "empty IR cannot apply convolution");
    expectTrue(fuse::audio::should_apply_hrtf_ir_convolution(valid),
               "valid IR applies convolution");
               "skip convolution matches is_empty_hrtf_ir");
    expectTrue(fuse::audio::should_apply_hrtf_ir_convolution(valid)
               "apply convolution matches should_use_hrtf_ir");

void testTryResolveHrtfPanPath() {

    fuse::audio::HrtfPanPath path = fuse::audio::HrtfPanPath::Bypass;
    fuse::audio::HrtfPanRejectReason reason = fuse::audio::HrtfPanRejectReason::None;

    expectTrue(fuse::audio::try_resolve_hrtf_pan_path(true, valid, offset, path, reason),
               "separated source with valid IR resolves");
    expectTrue(path == fuse::audio::HrtfPanPath::Convolution,
    expectTrue(reason == fuse::audio::HrtfPanRejectReason::None,
               "spatial path has no reject reason");

    expectTrue(fuse::audio::try_resolve_hrtf_pan_path(true, empty, offset, path, reason),
               "empty IR still resolves to spatial stub");
    expectTrue(path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR selects ILD/ITD stub");
               "ILD stub path has no reject reason");

    expectTrue(!fuse::audio::try_resolve_hrtf_pan_path(false, valid, offset, path, reason),
               "disabled HRTF rejects pan path");
    expectTrue(path == fuse::audio::HrtfPanPath::Bypass, "disabled resolves to bypass");
    expectTrue(reason == fuse::audio::HrtfPanRejectReason::Disabled,
               "disabled sets reject reason");

    expectTrue(!fuse::audio::try_resolve_hrtf_pan_path(true, valid, co_located, path, reason),
               "co-located source rejects pan path");
    expectTrue(path == fuse::audio::HrtfPanPath::Bypass, "co-located resolves to bypass");
    expectTrue(reason == fuse::audio::HrtfPanRejectReason::CoLocated,
               "co-located sets reject reason");

    expectTrue(std::strcmp(fuse::audio::hrtf_pan_reject_reason_label(
                               fuse::audio::HrtfPanRejectReason::Disabled),
                           "Disabled")
                   == 0,
               "reject reason label for disabled");

void testHrtfPanPreflight() {

    const fuse::audio::HrtfPanPreflight spatial =
        fuse::audio::preflight_hrtf_pan(true, valid, offset);
    expectTrue(spatial.can_apply_spatial_pan(), "spatial preflight can apply pan");
    expectTrue(!spatial.skip_convolution(), "valid IR does not skip convolution");
    expectTrue(spatial.ready_for_stub_mix(), "spatial preflight is stub-ready");
    expectTrue(spatial.has_valid_ir, "spatial preflight records valid IR");
    expectTrue(!spatial.co_located, "spatial preflight is not co-located");

    const fuse::audio::HrtfPanPreflight empty_ir =
        fuse::audio::preflight_hrtf_pan(true, fuse::audio::make_empty_hrtf_ir(), offset);
    expectTrue(empty_ir.can_apply_spatial_pan(), "empty IR preflight is spatial");
    expectTrue(empty_ir.skip_convolution(), "empty IR preflight skips convolution");
    expectTrue(!empty_ir.has_valid_ir, "empty IR preflight has no valid IR");

    const fuse::audio::HrtfPanPreflight bypass =
        fuse::audio::preflight_hrtf_pan(false, valid, offset);
    expectTrue(!bypass.can_apply_spatial_pan(), "disabled preflight cannot apply spatial pan");
    expectTrue(bypass.reject_reason == fuse::audio::HrtfPanRejectReason::Disabled,
               "disabled preflight records reject reason");
    expectTrue(bypass.ready_for_stub_mix(), "bypass preflight is stub-ready");

    const fuse::audio::HrtfPanPreflight no_ir =
        fuse::audio::preflight_hrtf_pan(true, offset);
    expectTrue(no_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR preflight selects ILD/ITD stub");
    expectTrue(no_ir.skip_convolution(), "no-IR preflight skips convolution");

    const fuse::audio::HrtfPanPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan(true, valid, co_located);
    expectTrue(co_located_preflight.co_located, "co-located preflight records co-location");
    expectTrue(co_located_preflight.reject_reason == fuse::audio::HrtfPanRejectReason::CoLocated,
               "co-located preflight records reject reason");

void testAttenuationCouplingPreflight() {

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
    expectTrue(unity.unity_attenuation, "unity preflight records unity attenuation");
    expectTrue(unity.skip_coupling, "unity preflight skips coupling");
    expectTrue(!unity.can_narrow_image(), "unity preflight cannot narrow image");
    expectTrue(!unity.ready_for_coupling(), "unity preflight is not ready for coupling");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation is non-unity");
    expectTrue(!narrowed.skip_coupling, "reduced attenuation does not skip coupling");
    expectTrue(narrowed.can_narrow_image(), "reduced attenuation can narrow image");
    expectTrue(narrowed.ready_for_coupling(), "reduced attenuation is ready for coupling");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass.skip_coupling, "bypass path skips coupling preflight");
    expectTrue(!bypass.can_narrow_image(), "bypass path cannot narrow image");

    const fuse::audio::HrtfAttenuationCouplingPreflight coupled =
        fuse::audio::preflight_hrtf_coupled_pan(true, valid, offset, 0.25f, 0.5f);
    expectTrue(coupled.path == fuse::audio::HrtfPanPath::Convolution,
               "coupled preflight carries convolution path");
    expectTrue(coupled.ready_for_coupling(), "coupled preflight is ready for coupling");

    const fuse::audio::HrtfAttenuationCouplingPreflight coupled_no_ir =
        fuse::audio::preflight_hrtf_coupled_pan(true, offset, 1.f, 1.f);
    expectTrue(coupled_no_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR coupled preflight selects ILD stub");
    expectTrue(coupled_no_ir.skip_coupling, "unity no-IR coupled preflight skips coupling");

    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_mapping(1.f, 1.f),
               "mapping skip alias for unity attenuation");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_mapping(1.f, 1.f)
                   == fuse::audio::is_unity_hrtf_attenuation(1.f, 1.f),
               "mapping skip matches is_unity_hrtf_attenuation");

void testAttenuationCouplingMappingSkipGuard() {
               "direct coupling early-outs on unity attenuation");
    expectTrue(empty_preflight.empty_ir, "preflight marks empty IR");
    expectTrue(empty_preflight.null_samples, "preflight marks null samples on empty IR");
    expectTrue(empty_preflight.zero_length, "preflight marks zero length on empty IR");
    expectTrue(!empty_preflight.can_use_convolution(), "empty IR preflight rejects convolution");
    expectTrue(empty_preflight.empty_ir, "empty IR preflight marks empty IR");
    expectTrue(empty_preflight.null_samples, "empty IR preflight marks null samples");
    expectTrue(empty_preflight.zero_length, "empty IR preflight marks zero length");
    expectTrue(!empty_preflight.has_samples, "empty IR preflight has no samples");
    expectTrue(!empty_preflight.can_use_convolution(), "empty IR preflight cannot use convolution");
    expectTrue(empty_preflight.should_use_ild_itd_stub(),
               "empty IR preflight falls back to ILD/ITD stub");
    expectTrue(empty_preflight.should_skip(), "empty IR preflight should skip convolution");

    expectTrue(!valid_preflight.empty_ir, "valid IR preflight is not empty");
    expectTrue(!valid_preflight.null_samples, "valid IR preflight has samples pointer");
    expectTrue(!valid_preflight.zero_length, "valid IR preflight has non-zero length");
    expectTrue(valid_preflight.has_samples, "valid IR preflight has samples");
    expectTrue(valid_preflight.can_use_convolution(), "valid IR preflight can use convolution");
    expectTrue(!valid_preflight.should_use_ild_itd_stub(),
               "valid IR preflight does not force ILD/ITD stub");
    expectTrue(!valid_preflight.should_skip(), "valid IR preflight does not skip convolution");
void testPreflightHrtfIrGuards() {
    expectTrue(empty_preflight.empty_ir, "preflight marks factory empty IR");
    expectTrue(!empty_preflight.can_use_convolution(), "preflight blocks convolution on empty IR");
    expectTrue(empty_preflight.empty_ir == !fuse::audio::has_hrtf_ir(empty),
               "preflight empty_ir matches has_hrtf_ir");

    expectTrue(!valid_preflight.empty_ir, "preflight accepts valid IR");
    expectTrue(!valid_preflight.null_samples, "preflight clears null_samples on valid IR");
    expectTrue(!valid_preflight.zero_length, "preflight clears zero_length on valid IR");
    expectTrue(valid_preflight.can_use_convolution(), "preflight enables convolution on valid IR");
    expectTrue(!valid_preflight.should_fallback_to_ild_itd(),
               "preflight does not fallback on valid IR");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight =
        fuse::audio::preflight_hrtf_ir(null_samples_nonzero_length);
    expectTrue(null_preflight.null_samples, "preflight marks null samples");
    expectTrue(!null_preflight.zero_length, "preflight preserves non-zero length field");
    expectTrue(null_preflight.empty_ir, "null samples preflight is still empty IR");
    expectTrue(!null_preflight.can_use_convolution(),
               "null samples preflight rejects convolution");

    expectTrue(!valid_preflight.null_samples, "valid IR preflight has samples");
    expectTrue(valid_preflight.can_use_convolution(), "valid IR preflight enables convolution");
    expectTrue(valid_preflight.can_use_convolution() == fuse::audio::has_hrtf_ir(valid),
               "preflight can_use_convolution matches has_hrtf_ir");

void testHrtfPanPathPreflightGuards() {

    const fuse::audio::HrtfPanPathPreflight convolution =
    expectTrue(!convolution.hrtf_disabled, "enabled convolution preflight is not disabled");
    expectTrue(!convolution.co_located, "offset source is not co-located");
    expectTrue(!convolution.empty_ir, "valid IR preflight is not empty");
    expectTrue(convolution.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR preflight selects convolution");
    expectTrue(convolution.can_apply_spatial_pan(), "convolution preflight applies spatial pan");
    expectTrue(convolution.uses_convolution(), "convolution preflight uses convolution");
    expectTrue(!convolution.uses_ild_itd_stub(), "convolution preflight is not ILD/ITD stub");

    const fuse::audio::HrtfPanPathPreflight ild_stub =
    expectTrue(ild_stub.empty_ir, "empty IR preflight is empty");
    expectTrue(ild_stub.path == fuse::audio::HrtfPanPath::IldItdStub,
    expectTrue(ild_stub.can_apply_spatial_pan(), "ILD stub preflight applies spatial pan");
    expectTrue(ild_stub.uses_ild_itd_stub(), "ILD stub preflight uses ILD/ITD stub");
    expectTrue(!ild_stub.uses_convolution(), "ILD stub preflight does not use convolution");

    const fuse::audio::HrtfPanPathPreflight bypass_disabled =
    expectTrue(bypass_disabled.hrtf_disabled, "disabled preflight marks HRTF disabled");
    expectTrue(bypass_disabled.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight bypasses pan");
    expectTrue(!bypass_disabled.can_apply_spatial_pan(),
               "disabled preflight skips spatial pan");

    const fuse::audio::HrtfPanPathPreflight bypass_co_located =
    expectTrue(bypass_co_located.co_located, "co-located preflight marks co-located source");
    expectTrue(bypass_co_located.path == fuse::audio::HrtfPanPath::Bypass,
               "co-located preflight bypasses pan");
    expectTrue(!bypass_co_located.can_apply_spatial_pan(),
               "co-located preflight skips spatial pan");

    const fuse::audio::HrtfPanPathPreflight no_ir =
    expectTrue(no_ir.empty_ir, "no-IR overload preflight is empty");
               "no-IR overload preflight selects ILD/ITD stub");
    expectTrue(no_ir.path == fuse::audio::resolve_hrtf_pan_path(true, offset),

void testHrtfAttenuationCouplingPreflightGuards() {
                                                         0.2f);
    expectTrue(bypass.bypass_path, "bypass coupling preflight marks bypass path");
    expectTrue(!bypass.spatial_path, "bypass coupling preflight is not spatial");
    expectTrue(!bypass.unity_attenuation, "reduced attenuation is non-unity");
    expectTrue(!bypass.can_apply_coupling(), "bypass coupling preflight rejects coupling");
    expectTrue(!bypass.should_narrow(), "bypass coupling preflight does not narrow");

    expectTrue(!unity.bypass_path, "spatial unity preflight is not bypass");
    expectTrue(unity.spatial_path, "spatial unity preflight is spatial");
    expectTrue(unity.unity_attenuation, "unity preflight marks unity attenuation");
    expectTrue(!unity.can_apply_coupling(), "unity preflight rejects coupling");
    expectTrue(!unity.should_narrow(), "unity preflight does not narrow");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::Convolution,
                                                              1.f, 1.f),
               "preflight should_narrow matches should_narrow_hrtf_spatial_image");

    expectTrue(!narrowed.bypass_path, "spatial narrowed preflight is not bypass");
    expectTrue(narrowed.spatial_path, "spatial narrowed preflight is spatial");
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation preflight is non-unity");
    expectTrue(narrowed.can_apply_coupling(), "spatial narrowed preflight applies coupling");
    expectTrue(narrowed.should_narrow(), "spatial narrowed preflight narrows image");
    expectTrue(narrowed.can_apply_coupling()
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.8f),
               "preflight can_apply_coupling matches should_narrow_hrtf_spatial_image");
    expectTrue(null_preflight.empty_ir, "null samples preflight is empty");
    expectTrue(null_preflight.null_samples, "null samples preflight marks null pointer");
    expectTrue(!null_preflight.zero_length, "null samples preflight keeps non-zero length flag");
    expectTrue(null_preflight.should_skip(), "null samples preflight skips convolution");

    expectTrue(empty_preflight.should_fallback_to_stub(),

    expectTrue(!valid_preflight.empty_ir, "preflight marks valid IR");
    expectTrue(!valid_preflight.should_fallback_to_stub(),
               "valid IR preflight does not fall back to stub");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight = fuse::audio::preflight_hrtf_ir(null_samples);


               "pan-path preflight selects convolution for valid IR");
    expectTrue(!convolution.hrtf_disabled, "pan-path preflight keeps HRTF enabled");
    expectTrue(!convolution.co_located, "pan-path preflight marks separated source");
    expectTrue(!convolution.empty_ir, "pan-path preflight marks non-empty IR");
    expectTrue(!convolution.bypass, "pan-path preflight is not bypass");
    expectTrue(convolution.uses_convolution, "pan-path preflight uses convolution");
    expectTrue(!convolution.uses_ild_itd_stub, "pan-path preflight does not use ILD/ITD stub");
    expectTrue(convolution.can_apply_spatial_pan(), "pan-path preflight can apply spatial pan");
    expectTrue(!convolution.should_skip(), "pan-path preflight does not skip spatial pan");

               "pan-path preflight selects ILD/ITD stub for empty IR");
    expectTrue(ild_stub.empty_ir, "pan-path preflight marks empty IR");
    expectTrue(ild_stub.uses_ild_itd_stub, "pan-path preflight uses ILD/ITD stub");
    expectTrue(!ild_stub.uses_convolution, "pan-path preflight does not use convolution");
    expectTrue(ild_stub.can_apply_spatial_pan(), "ILD/ITD stub preflight can apply spatial pan");

    const fuse::audio::HrtfPanPathPreflight bypass =
    expectTrue(bypass.path == fuse::audio::HrtfPanPath::Bypass,
               "pan-path preflight bypasses when HRTF disabled");
    expectTrue(bypass.hrtf_disabled, "pan-path preflight marks disabled HRTF");
    expectTrue(bypass.bypass, "pan-path preflight is bypass");
    expectTrue(!bypass.can_apply_spatial_pan(), "bypass preflight cannot apply spatial pan");
    expectTrue(bypass.should_skip(), "bypass preflight skips spatial pan");

    expectTrue(co_located_preflight.co_located, "pan-path preflight marks co-located source");
    expectTrue(co_located_preflight.bypass, "co-located preflight bypasses pan");
    expectTrue(co_located_preflight.should_skip(), "co-located preflight skips spatial pan");

               "no-IR pan-path preflight selects ILD/ITD stub");
    expectTrue(no_ir.empty_ir, "no-IR pan-path preflight treats IR as empty");


    expectTrue(narrowed.path == fuse::audio::HrtfPanPath::Convolution,
               "coupling preflight preserves pan path");
    expectTrue(!narrowed.bypass_path, "coupling preflight is not bypass path");
    expectTrue(!narrowed.unity_attenuation, "coupling preflight marks non-unity attenuation");
    expectTrue(!narrowed.skipped, "coupling preflight does not skip narrowing");
    expectTrue(narrowed.can_narrow(), "coupling preflight can narrow spatial image");
    expectTrue(narrowed.can_apply_coupling(), "coupling preflight can apply coupling");
    expectTrue(!narrowed.should_skip(), "coupling preflight does not skip");

        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 1.f,
                                                        1.f);
    expectTrue(unity.unity_attenuation, "unity coupling preflight marks unity attenuation");
    expectTrue(unity.skipped, "unity coupling preflight skips narrowing");
    expectTrue(!unity.can_narrow(), "unity coupling preflight cannot narrow");
    expectTrue(unity.should_skip(), "unity coupling preflight should skip");

    expectTrue(bypass.skipped, "bypass coupling preflight skips narrowing");
    expectTrue(!bypass.can_apply_coupling(), "bypass coupling preflight cannot apply coupling");

    const fuse::audio::HrtfAttenuationCouplingPreflight ir_coupled =
        fuse::audio::preflight_hrtf_attenuation_coupling(true, empty, offset, 0.15f, 0.2f);
    expectTrue(ir_coupled.path == fuse::audio::HrtfPanPath::IldItdStub,
               "IR-aware coupling preflight resolves ILD/ITD stub path");
    expectTrue(!ir_coupled.bypass_path, "IR-aware coupling preflight is spatial");
    expectTrue(!ir_coupled.skipped, "IR-aware coupling preflight narrows under attenuation");

    const fuse::audio::HrtfAttenuationCouplingPreflight disabled =
        fuse::audio::preflight_hrtf_attenuation_coupling(false, valid, offset, 0.1f, 0.1f);
    expectTrue(disabled.bypass_path, "disabled HRTF coupling preflight bypasses path");
    expectTrue(disabled.skipped, "disabled HRTF coupling preflight skips narrowing");

    const fuse::audio::HrtfAttenuationCouplingPreflight full_unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(true, valid, offset, 1.5f, 2.f);
    expectTrue(full_unity.unity_attenuation,
               "out-of-range attenuation preflight clamps to unity");
    expectTrue(full_unity.skipped, "unity IR-aware coupling preflight skips narrowing");
    expectTrue(null_preflight.empty_ir, "preflight treats null samples as empty IR");


    expectTrue(convolution.can_apply_spatial_pan(), "preflight accepts spatial convolution path");
    expectTrue(convolution.will_use_convolution(), "preflight marks convolution path");
    expectTrue(!convolution.should_skip(), "preflight does not skip convolution path");
               "preflight path matches resolve_hrtf_pan_path");

        fuse::audio::preflight_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), offset);
    expectTrue(ild_stub.empty_ir, "preflight marks empty IR on ILD/ITD stub path");
    expectTrue(ild_stub.ild_itd_stub, "preflight marks ILD/ITD stub path");
    expectTrue(!ild_stub.convolution, "preflight clears convolution on empty IR");
    expectTrue(ild_stub.can_apply_spatial_pan(), "preflight keeps ILD/ITD stub spatial");

    const fuse::audio::HrtfPanPathPreflight disabled =
    expectTrue(disabled.hrtf_disabled, "preflight marks disabled HRTF");
    expectTrue(disabled.bypass, "preflight marks bypass when disabled");
    expectTrue(disabled.should_skip(), "preflight skips disabled path");
    expectTrue(!disabled.can_apply_spatial_pan(), "preflight rejects spatial pan when disabled");

    expectTrue(co_located_preflight.co_located, "preflight marks co-located source");
    expectTrue(co_located_preflight.bypass, "preflight bypasses co-located source");
    expectTrue(co_located_preflight.should_skip(), "preflight skips co-located source");

    expectTrue(no_ir.ild_itd_stub, "no-IR overload selects ILD/ITD stub");
    expectTrue(no_ir.empty_ir, "no-IR overload marks empty IR");
               "no-IR preflight matches resolve_hrtf_pan_path overload");

    expectTrue(bypass.bypass_path, "preflight marks bypass path");
    expectTrue(!bypass.will_narrow, "preflight does not narrow on bypass path");
    expectTrue(bypass.should_skip(), "preflight skips coupling on bypass path");
    expectTrue(!bypass.can_apply_coupling(), "preflight rejects coupling on bypass path");

    expectTrue(unity.unity_attenuation, "preflight marks unity attenuation");
    expectTrue(!unity.will_narrow, "preflight does not narrow at unity attenuation");
    expectTrue(unity.should_skip(), "preflight skips coupling at unity attenuation");

    expectTrue(narrowed.can_apply_coupling(), "preflight applies coupling on spatial path");
    expectTrue(narrowed.will_narrow, "preflight narrows under reduced attenuation");
    expectTrue(!narrowed.should_skip(), "preflight does not skip non-unity spatial coupling");
    expectTrue(narrowed.will_narrow
               "preflight will_narrow matches should_narrow_hrtf_spatial_image");
    expectTrue(disabled.should_bypass(), "disabled preflight bypasses pan");
    expectTrue(!disabled.can_apply_spatial_pan(), "disabled preflight skips spatial pan");
    expectTrue(disabled.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight resolves to bypass path");

    expectTrue(co_located_preflight.should_bypass(), "co-located preflight bypasses pan");

    const fuse::audio::HrtfPanPathPreflight empty_ir =
    expectTrue(empty_ir.empty_ir, "preflight marks empty IR");
    expectTrue(empty_ir.should_use_ild_itd_stub(), "empty IR preflight uses ILD/ITD stub");
    expectTrue(empty_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR preflight resolves to ILD/ITD stub");

    expectTrue(convolution.has_valid_ir, "valid IR preflight has valid IR");
    expectTrue(convolution.should_use_convolution(), "valid IR preflight uses convolution");
               "valid IR preflight resolves to convolution path");

    expectTrue(no_ir.empty_ir, "no-IR overload preflight treats IR as empty");
    expectTrue(no_ir.should_use_ild_itd_stub(), "no-IR overload preflight uses ILD/ITD stub");

    expectTrue(bypass.should_skip_coupling(), "bypass preflight skips coupling");
    expectTrue(!bypass.should_narrow(), "bypass preflight does not narrow");
    expectTrue(!bypass.can_apply_coupling(), "bypass preflight cannot apply coupling");

    expectTrue(unity.spatial_path, "unity preflight is still spatial path");
    expectTrue(unity.should_skip_coupling(), "unity preflight skips coupling");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
                                                         0.2f, 1.f);
    expectTrue(narrow.spatial_path, "narrow preflight is spatial path");
    expectTrue(!narrow.unity_attenuation, "narrow preflight has non-unity attenuation");
    expectTrue(narrow.should_narrow(), "narrow preflight should narrow image");
    expectTrue(narrow.can_apply_coupling(), "narrow preflight can apply coupling");
    expectTrue(narrow.should_narrow()
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 1.f),
               "preflight should_narrow matches combined guard");

void testHrtfSpatialPanPreflight() {

    const fuse::audio::HrtfSpatialPanPreflight bypassed =
        fuse::audio::preflight_hrtf_spatial_pan(false, valid, offset, 0.2f, 0.3f);
    expectTrue(!bypassed.can_apply_spatial_pan(), "combined preflight bypass skips spatial pan");
    expectTrue(!bypassed.should_narrow_spatial_image(),
               "combined preflight bypass does not narrow");

    const fuse::audio::HrtfSpatialPanPreflight stub_narrow =
        fuse::audio::preflight_hrtf_spatial_pan(true, empty, offset, 0.15f, 0.25f);
    expectTrue(stub_narrow.ir.should_fallback_to_stub(),
               "combined preflight empty IR falls back to stub");
    expectTrue(stub_narrow.pan.should_use_ild_itd_stub(),
               "combined preflight empty IR uses ILD/ITD stub");
    expectTrue(stub_narrow.can_apply_spatial_pan(), "combined preflight stub path is spatial");
    expectTrue(stub_narrow.should_narrow_spatial_image(),
               "combined preflight narrows under attenuation");

    const fuse::audio::HrtfSpatialPanPreflight convolution_unity =
        fuse::audio::preflight_hrtf_spatial_pan(true, valid, offset, 1.f, 1.f);
    expectTrue(convolution_unity.ir.can_use_convolution(),
               "combined preflight valid IR can use convolution");
    expectTrue(convolution_unity.pan.should_use_convolution(),
               "combined preflight uses convolution path");
    expectTrue(!convolution_unity.should_narrow_spatial_image(),
               "combined preflight unity attenuation skips narrowing");

    expectTrue(empty_preflight.null_samples, "empty factory IR has null samples");
    expectTrue(empty_preflight.zero_length, "empty factory IR has zero length");
    expectTrue(empty_preflight.is_empty(), "empty factory IR preflight is empty");
    expectTrue(!empty_preflight.has_valid_ir(), "empty factory IR is not valid");

    const fuse::audio::HrtfIrPreflight valid_preflight =
        fuse::audio::preflight_hrtf_ir(samples, static_cast<fuse::u32>(2));
    expectTrue(!valid_preflight.null_samples, "valid IR has non-null samples");
    expectTrue(!valid_preflight.zero_length, "valid IR has non-zero length");
    expectTrue(valid_preflight.has_valid_ir(), "valid IR preflight passes");
    expectTrue(fuse::audio::can_use_hrtf_ir_preflight(fuse::audio::HrtfIrStub{samples, 2}),
               "can_use_hrtf_ir_preflight matches valid IR");

    const fuse::audio::HrtfIrPreflight null_length_preflight =
        fuse::audio::preflight_hrtf_ir(nullptr, 4);
    expectTrue(null_length_preflight.null_samples, "null samples flagged in preflight");
    expectTrue(null_length_preflight.is_empty(), "null samples with length is empty");

    const fuse::audio::HrtfIrPreflight zero_length_preflight =
        fuse::audio::preflight_hrtf_ir(samples, 0);
    expectTrue(zero_length_preflight.zero_length, "zero length flagged in preflight");
    expectTrue(zero_length_preflight.is_empty(), "zero length IR is empty");
    expectTrue(fuse::audio::preflight_hrtf_ir(empty).is_empty()
               "preflight empty matches is_empty_hrtf_ir");
    expectTrue(fuse::audio::can_use_hrtf_ir_preflight(fuse::audio::HrtfIrStub{samples, 2})
                   == fuse::audio::has_hrtf_ir(fuse::audio::HrtfIrStub{samples, 2}),
               "can_use_hrtf_ir_preflight matches has_hrtf_ir");


    const fuse::audio::HrtfPanPathPreflight convolution_preflight =
    expectTrue(convolution_preflight.will_use_convolution(), "valid IR selects convolution");
    expectTrue(convolution_preflight.can_apply_spatial_pan(), "convolution path is spatial");
    expectTrue(!convolution_preflight.empty_ir, "valid IR is not empty");
    expectTrue(convolution_preflight.path == fuse::audio::HrtfPanPath::Convolution,
    expectTrue(convolution_preflight.path
                   == fuse::audio::resolve_hrtf_pan_path(true, valid, offset),
               "preflight path agrees with resolver");

    expectTrue(stub_preflight.will_use_ild_itd_stub(), "empty IR selects ILD/ITD stub");
    expectTrue(stub_preflight.empty_ir, "empty IR flagged in pan-path preflight");
    expectTrue(stub_preflight.ir_preflight.is_empty(), "nested IR preflight is empty");

    expectTrue(disabled_preflight.will_bypass(), "disabled HRTF bypasses pan");
    expectTrue(disabled_preflight.hrtf_disabled, "disabled flag set in preflight");
    expectTrue(!disabled_preflight.can_apply_spatial_pan(), "disabled path cannot spatial pan");

    expectTrue(co_located_preflight.will_bypass(), "co-located source bypasses pan");
    expectTrue(co_located_preflight.co_located, "co-located flag set in preflight");

    expectTrue(no_ir_preflight.will_use_ild_itd_stub(), "no-IR overload selects ILD/ITD stub");
    expectTrue(no_ir_preflight.empty_ir, "no-IR overload marks empty IR");

    expectTrue(fuse::audio::can_apply_hrtf_spatial_pan_preflight(true, valid, offset),
               "can_apply spatial pan preflight for valid offset");
    expectTrue(!fuse::audio::can_apply_hrtf_spatial_pan_preflight(false, valid, offset),
               "can_apply spatial pan preflight rejects disabled HRTF");
    expectTrue(fuse::audio::can_apply_hrtf_spatial_pan_preflight(true, valid, offset)
                   == fuse::audio::should_apply_hrtf_pan(true, offset),
               "can_apply spatial pan preflight matches should_apply_hrtf_pan");

    expectTrue(bypass_preflight.bypass_path, "bypass path flagged in coupling preflight");
    expectTrue(!bypass_preflight.should_apply_coupling(), "bypass skips coupling");
    expectTrue(!bypass_preflight.should_narrow_spatial_image(), "bypass does not narrow");
    expectTrue(bypass_preflight.should_skip_coupling(), "bypass should_skip_coupling");

    expectTrue(unity_preflight.unity_attenuation, "unity attenuation flagged");
    expectTrue(unity_preflight.should_apply_coupling(), "spatial path applies coupling");
    expectTrue(!unity_preflight.should_narrow_spatial_image(), "unity does not narrow");
    expectTrue(unity_preflight.should_skip_coupling(), "unity skips coupling application");

    expectTrue(narrow_preflight.should_narrow_spatial_image(), "reduced attenuation narrows");
    expectTrue(!narrow_preflight.should_skip_coupling(), "non-unity spatial path applies coupling");
    expectNear(narrow_preflight.clamped_distance_attenuation, 0.2f, 1e-5f,
               "preflight clamps distance attenuation");
    expectNear(narrow_preflight.clamped_occlusion_gain, 0.8f, 1e-5f,
               "preflight clamps occlusion gain");

    const fuse::audio::HrtfAttenuationCouplingPreflight clamped_preflight =
                                                         -0.5f, 1.5f);
    expectNear(clamped_preflight.clamped_distance_attenuation, 0.f, 1e-5f,
               "negative distance attenuation clamped in preflight");
    expectNear(clamped_preflight.clamped_occlusion_gain, 1.f, 1e-5f,
               "above-unity occlusion gain clamped in preflight");
    expectTrue(!clamped_preflight.unity_attenuation,
               "clamped zero distance attenuation is non-unity");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_clamped_preflight =
                                                         2.f, 1.5f);
    expectTrue(unity_clamped_preflight.unity_attenuation,
               "out-of-range values that clamp to unity are flagged");

    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image_preflight(
                   fuse::audio::HrtfPanPath::Convolution, 0.3f, 1.f),
               "narrow preflight convenience matches reduced distance");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image_preflight(
                   fuse::audio::HrtfPanPath::Bypass, 0.3f, 1.f),
               "narrow preflight convenience rejects bypass");
        fuse::audio::should_narrow_hrtf_spatial_image_preflight(
            fuse::audio::HrtfPanPath::Convolution, 0.3f, 1.f)
            == fuse::audio::should_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::Convolution,
                                                             0.3f, 1.f),
        "narrow preflight convenience matches should_narrow_hrtf_spatial_image");
void testEmptyIrPreflightGuards() {

    const fuse::audio::HrtfIrStub normalized_empty =
    expectTrue(fuse::audio::is_empty_hrtf_ir(normalized_empty),
               "preflight_hrtf_ir collapses invalid IR to empty");
    expectTrue(fuse::audio::preflight_hrtf_ir(valid).length == 2,
               "preflight_hrtf_ir preserves valid IR length");

               "skip convolution is inverse of should_use_hrtf_ir");

               "fallback alias matches is_empty_hrtf_ir");

void testPanPathPreflightGuards() {

    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, valid, offset)
                   == fuse::audio::HrtfPanPath::Convolution,
               "preflight selects convolution for valid IR");
    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, empty, offset)
               "preflight selects ILD/ITD stub for empty IR");
    expectTrue(fuse::audio::preflight_hrtf_pan_path(false, valid, offset)
                   == fuse::audio::HrtfPanPath::Bypass,
               "preflight bypasses when HRTF disabled");
    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, offset)
               "no-IR preflight matches resolve overload");

               "convolution path predicate for valid IR");
               "convolution path predicate false for empty IR");
    expectTrue(!fuse::audio::should_use_hrtf_convolution_path(true, valid, co_located),
               "convolution path predicate false when co-located");

               "spatial pan applies on convolution path");
               "spatial pan skipped on bypass path");
    expectTrue(fuse::audio::should_apply_hrtf_spatial_pan(fuse::audio::HrtfPanPath::IldItdStub)
                   == fuse::audio::is_spatial_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "spatial pan alias matches is_spatial_hrtf_pan_path");

void testAttenuationCouplingPreflightGuards() {
    expectNear(fuse::audio::hrtf_unity_attenuation_epsilon(), 1e-5f, 1e-8f,
               "unity attenuation epsilon is exposed");

    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(
               "preflight coupling runs for spatial path with reduced attenuation");
    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.8f),
               "preflight coupling skips bypass path");
               "preflight coupling skips unity attenuation");
               "preflight coupling matches should_narrow_hrtf_spatial_image");

    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_for_inputs(
               "skip coupling for bypass path");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_for_inputs(
                   fuse::audio::HrtfPanPath::Convolution, 0.1f, 0.1f),
               "do not skip coupling for spatial path with reduced attenuation");
               "skip coupling for unity attenuation");
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.5f)
                   == !fuse::audio::preflight_hrtf_attenuation_coupling(
                          fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.5f),
               "skip coupling is inverse of preflight coupling");
    expectTrue(!valid_preflight.null_samples, "valid IR preflight clears null_samples");
    expectTrue(!valid_preflight.zero_length, "valid IR preflight clears zero_length");
    expectTrue(valid_preflight.canUseConvolution(), "valid IR preflight accepts convolution");
    expectTrue(fuse::audio::can_use_hrtf_ir_for_convolution(valid)
               "can_use_hrtf_ir_for_convolution matches should_use_hrtf_ir");

    const fuse::audio::HrtfIrPreflight null_only =
    expectTrue(null_only.null_samples, "null samples flagged even with non-zero length");
    expectTrue(!null_only.zero_length, "non-zero length field preserved in preflight");
    expectTrue(null_only.isEmpty(), "null samples with length still treated as empty");

    const fuse::audio::HrtfIrStub zero_length_valid_ptr{samples, 0};
    const fuse::audio::HrtfIrPreflight zero_length =
        fuse::audio::preflight_hrtf_ir(zero_length_valid_ptr);
    expectTrue(!zero_length.null_samples, "valid pointer with zero length is not null_samples");
    expectTrue(zero_length.zero_length, "zero length flagged when pointer is valid");
    expectTrue(zero_length.isEmpty(), "zero length IR is empty");
    expectNear(bypassed.right, wide.right, 1e-5f,
               "bypass preflight leaves gains unchanged");

    expectTrue(unity_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::
                                             UnityAttenuation,
               "unity attenuation preflight carries unity_attenuation reject reason");
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::
                                               BypassPath,
               "bypass path preflight carries bypass_path reject reason");
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path has none attenuation reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "attenuation coupling rejects bypass path");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "attenuation reject reason label for unity_attenuation");
}

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(!disabled.canApplyPan(), "disabled HRTF preflight rejects pan");
               "disabled HRTF resolves to bypass");

    expectTrue(!co_located_preflight.canApplyPan(), "co-located preflight rejects pan");
    expectTrue(co_located_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "co-located source resolves to bypass");

    expectTrue(empty_ir.canApplyPan(), "empty IR still applies ILD/ITD stub pan");
               "empty IR resolves to ILD/ITD stub");

    expectTrue(!convolution.empty_ir, "valid IR preflight clears empty_ir");
    expectTrue(convolution.canApplyPan(), "valid IR preflight accepts pan");
               "valid IR resolves to convolution path");
    expectTrue(fuse::audio::can_apply_hrtf_spatial_pan(convolution.path),
               "can_apply_hrtf_spatial_pan accepts convolution path");
    expectTrue(!fuse::audio::can_apply_hrtf_spatial_pan(disabled.path),
               "can_apply_hrtf_spatial_pan rejects bypass path");

               "no-IR overload selects ILD/ITD stub");

    expectTrue(bypass.bypass_path, "coupling preflight marks bypass path");
    expectTrue(!bypass.canApplyCoupling(), "bypass path rejects coupling");
    expectTrue(!fuse::audio::can_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                             0.1f),
               "can_narrow rejects bypass path");

        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution, 1.f,
    expectTrue(unity.unity_attenuation, "coupling preflight marks unity attenuation");
    expectTrue(!unity.canApplyCoupling(), "unity attenuation rejects coupling");
    expectTrue(!fuse::audio::can_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f),
               "can_narrow rejects unity attenuation");

    expectTrue(!narrowed.bypass_path, "spatial path clears bypass_path");
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation clears unity flag");
    expectTrue(narrowed.canApplyCoupling(), "spatial + reduced attenuation accepts coupling");
    expectTrue(fuse::audio::can_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::IldItdStub,
                                                          0.2f, 0.8f),
               "can_narrow matches preflight canApplyCoupling");
                                                          0.2f, 0.8f)
               "can_narrow matches should_narrow_hrtf_spatial_image");


    expectTrue(!null_preflight.zero_length, "preflight keeps zero_length false when length > 0");

void testPreflightHrtfPanPathGuards() {

    expectTrue(convolution.can_apply_spatial_pan(), "preflight enables spatial pan for valid IR");
    expectTrue(convolution.uses_convolution(), "preflight selects convolution for valid IR");
    expectTrue(!convolution.empty_ir, "preflight clears empty_ir for valid IR");
    expectTrue(convolution.path == fuse::audio::resolve_hrtf_pan_path(true, valid, offset),

    expectTrue(ild_stub.can_apply_spatial_pan(), "preflight enables spatial pan for empty IR");
    expectTrue(ild_stub.uses_ild_itd_stub(), "preflight selects ILD/ITD stub for empty IR");
    expectTrue(ild_stub.empty_ir, "preflight marks empty IR");
    expectTrue(!ild_stub.hrtf_disabled, "preflight clears hrtf_disabled when enabled");

    expectTrue(bypass.is_bypass(), "preflight bypasses when HRTF disabled");
    expectTrue(bypass.hrtf_disabled, "preflight marks hrtf_disabled");
    expectTrue(!bypass.can_apply_spatial_pan(), "preflight blocks spatial pan when disabled");

    expectTrue(co_located_preflight.is_bypass(), "preflight bypasses co-located source");

    expectTrue(no_ir.uses_ild_itd_stub(), "no-IR overload selects ILD/ITD stub");
               "no-IR preflight path matches resolve overload");

void testPreflightHrtfAttenuationCouplingGuards() {

    expectTrue(!narrowed.skipped, "preflight does not skip narrowed convolution path");
    expectTrue(narrowed.can_narrow_spatial_image(), "preflight can narrow under attenuation");
    expectTrue(narrowed.can_apply_coupling(), "preflight can_apply_coupling matches can_narrow");
    expectTrue(!narrowed.bypass_path, "preflight clears bypass_path on convolution");
    expectTrue(!narrowed.unity_attenuation, "preflight clears unity_attenuation when narrowed");
    expectTrue(narrowed.can_narrow_spatial_image()
                          fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f),

    expectTrue(unity.skipped, "preflight skips unity attenuation");
    expectTrue(!unity.can_apply_coupling(), "preflight blocks coupling at unity gain");

    expectTrue(bypass.skipped, "preflight skips bypass path");
    expectTrue(!bypass.can_narrow_spatial_image(), "preflight cannot narrow on bypass");

    const fuse::audio::HrtfAttenuationCouplingPreflight ir_aware =
        fuse::audio::preflight_hrtf_attenuation_coupling(true, empty, offset, 0.15f, 0.25f);
    const fuse::audio::HrtfAttenuationCouplingPreflight via_path =
        fuse::audio::preflight_hrtf_attenuation_coupling(
            fuse::audio::resolve_hrtf_pan_path(true, empty, offset), 0.15f, 0.25f);
    expectTrue(ir_aware.skipped == via_path.skipped,
               "IR-aware preflight skip matches path preflight");
    expectTrue(ir_aware.path == via_path.path, "IR-aware preflight path matches resolved path");

    expectTrue(disabled.skipped, "preflight skips coupling when HRTF disabled");
    expectTrue(disabled.bypass_path, "disabled HRTF resolves to bypass path");

               "unity attenuation coupling is a no-op via spatial blend guard");
void testPreflightHrtfIr() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.emptyIr, "empty IR preflight marks empty");
    expectTrue(!empty_preflight.malformedIr, "canonical empty IR is not malformed");
    expectTrue(!empty_preflight.hasValidIr, "empty IR preflight has no valid IR");
    expectTrue(empty_preflight.shouldSkipConvolution(), "empty IR skips convolution");
    expectTrue(!empty_preflight.canUseConvolution(), "empty IR cannot use convolution");
void testHrtfIrPreflightGuards() {
    expectTrue(!empty_preflight.ready, "empty IR preflight is not ready");
    expectTrue(empty_preflight.skipConvolution, "empty IR preflight skips convolution");
    expectTrue(empty_preflight.reject == fuse::audio::HrtfIrPreflightReject::NullSamples,
               "empty IR preflight rejects null samples");
void testHrtfIrPreflight() {
    expectTrue(empty_preflight.empty, "preflight marks empty IR");
    expectTrue(empty_preflight.null_samples, "preflight marks null samples");
    expectTrue(empty_preflight.zero_length, "preflight marks zero length");
    expectTrue(empty_preflight.skips_convolution(), "empty IR skips convolution");
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "empty IR rejects for zero length");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::ZeroLength),
               "reject helper matches zero-length reason");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.emptyIr, "valid IR preflight is not empty");
    expectTrue(!valid_preflight.malformedIr, "valid IR preflight is not malformed");
    expectTrue(valid_preflight.hasValidIr, "valid IR preflight has samples");
    expectTrue(valid_preflight.canUseConvolution(), "valid IR can use convolution");
    expectTrue(!valid_preflight.shouldSkipConvolution(), "valid IR does not skip convolution");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight = fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.emptyIr, "malformed IR preflight is empty");
    expectTrue(malformed_preflight.malformedIr, "malformed IR preflight is flagged");
    expectTrue(malformed_preflight.shouldSkipConvolution(), "malformed IR skips convolution");
}

void testPreflightHrtfPanPath() {
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    expectTrue(valid_preflight.ready, "valid IR preflight is ready");
    expectTrue(!valid_preflight.skipConvolution, "valid IR preflight does not skip convolution");
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    const fuse::audio::HrtfIrPreflight zero_preflight = fuse::audio::preflight_hrtf_ir(zero_length);
    expectTrue(zero_preflight.reject == fuse::audio::HrtfIrPreflightReject::ZeroLength,
               "zero-length IR preflight rejects zero length");

    fuse::audio::HrtfIrPreflightReject reject = fuse::audio::HrtfIrPreflightReject::None;
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, &reject),
               "try_preflight_hrtf_ir succeeds for valid IR");
    expectTrue(reject == fuse::audio::HrtfIrPreflightReject::None,
               "valid IR try_preflight reject is None");
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, &reject),
               "try_preflight_hrtf_ir fails for empty IR");
    expectTrue(reject == fuse::audio::HrtfIrPreflightReject::NullSamples,
               "empty IR try_preflight reports null samples");

void testHrtfPanPathPreflightGuards() {
}

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrStub empty{};

    const fuse::audio::HrtfPanPathPreflight disabled =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(disabled.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled HRTF pan preflight selects bypass");
    expectTrue(disabled.reject == fuse::audio::HrtfPanPathPreflightReject::Disabled,
               "disabled HRTF pan preflight reports Disabled");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "co-located pan preflight selects bypass");
    expectTrue(co_located_preflight.reject == fuse::audio::HrtfPanPathPreflightReject::CoLocated,
               "co-located pan preflight reports CoLocated");

    const fuse::audio::HrtfPanPathPreflight convolution =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(convolution.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR preflight selects convolution");
    expectTrue(convolution.spatial, "convolution preflight is spatial");
    expectTrue(convolution.usesConvolution, "convolution preflight uses IR");
    expectTrue(!convolution.bypassed, "convolution preflight is not bypassed");
    expectTrue(convolution.canApplySpatialPan(), "convolution preflight can apply spatial pan");
               "valid IR pan preflight selects convolution");
    expectTrue(convolution.is_spatial(), "convolution pan preflight is spatial");
    expectTrue(!convolution.skipConvolution, "convolution pan preflight does not skip convolution");

    const fuse::audio::HrtfPanPathPreflight stub =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR preflight selects ILD/ITD stub");
    expectTrue(stub.usesIldItdStub, "empty IR preflight uses ILD/ITD stub");
    expectTrue(!stub.usesConvolution, "empty IR preflight does not use convolution");
    expectTrue(stub.canApplySpatialPan(), "ILD/ITD stub preflight can apply spatial pan");

    expectTrue(disabled.hrtfDisabled, "disabled HRTF preflight is flagged");
    expectTrue(disabled.bypassed, "disabled HRTF preflight bypasses pan");
    expectTrue(disabled.shouldSkipSpatialPan(), "disabled HRTF skips spatial pan");

    expectTrue(co_located_preflight.coLocated, "co-located preflight is flagged");
    expectTrue(co_located_preflight.bypassed, "co-located preflight bypasses pan");
               "empty IR pan preflight selects ILD/ITD stub");
    expectTrue(stub.skipConvolution, "ILD/ITD stub pan preflight skips convolution");

    const fuse::audio::HrtfPanPathPreflight no_ir =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload preflight selects ILD/ITD stub");
    expectTrue(no_ir.usesIldItdStub, "no-IR overload preflight uses ILD/ITD stub");

void testPreflightHrtfAttenuationCoupling() {
    const fuse::audio::HrtfAttenuationCoupling coupling{};
    const fuse::audio::BinauralPanParams params;

    const fuse::audio::HrtfAttenuationCouplingPreflight spatial =
            fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, coupling, params);
    expectTrue(!spatial.bypassPath, "spatial path preflight is not bypass");
    expectTrue(!spatial.unityAttenuation, "reduced attenuation is non-unity");
    expectTrue(!spatial.skipped, "spatial path with reduced attenuation is not skipped");
    expectTrue(spatial.canApplyCoupling(), "spatial path can apply coupling");
    expectTrue(spatial.spatialBlend < 1.f, "reduced attenuation lowers spatial blend");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
            fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f, coupling, params);
    expectTrue(unity.unityAttenuation, "unity attenuation preflight is flagged");
    expectTrue(unity.skipped, "unity attenuation preflight skips coupling");
    expectTrue(unity.shouldSkipCoupling(), "unity attenuation should skip coupling");
    expectNear(unity.spatialBlend, 1.f, 1e-5f, "unity attenuation keeps full spatial blend");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
            fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, coupling, params);
    expectTrue(bypass.bypassPath, "bypass path preflight is flagged");
    expectTrue(bypass.skipped, "bypass path preflight skips coupling");
    expectTrue(!bypass.canApplyCoupling(), "bypass path cannot apply coupling");

    expectNear(spatial.distanceAttenuation, 0.2f, 1e-5f,
               "preflight clamps distance attenuation into range");
    expectNear(spatial.occlusionGain, 0.3f, 1e-5f, "preflight clamps occlusion gain into range");
    expectTrue(empty_preflight.null_samples, "empty IR preflight marks null samples");
    expectTrue(empty_preflight.zero_length, "empty IR preflight marks zero length");
    expectTrue(empty_preflight.empty_ir(), "empty IR preflight reports empty");
    expectTrue(empty_preflight.should_skip_convolution(), "empty IR preflight skips convolution");
    expectTrue(empty_preflight.should_skip_convolution()
                   == fuse::audio::should_skip_hrtf_convolution(empty),
               "preflight skip matches should_skip_hrtf_convolution");

    const float samples[] = {0.5f};
    expectTrue(!valid_preflight.empty_ir(), "valid IR preflight is not empty");
    expectTrue(valid_preflight.can_convolve() == fuse::audio::has_hrtf_ir(valid),
               "preflight can_convolve matches has_hrtf_ir");

    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.malformed_ir, "malformed IR preflight flags non-null zero length");
    expectTrue(malformed_preflight.should_skip_convolution(),
               "malformed IR preflight skips convolution");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight =
        fuse::audio::preflight_hrtf_ir(null_samples_nonzero_length);
    expectTrue(null_preflight.null_samples, "null samples flagged in preflight");
    expectTrue(null_preflight.empty_ir(), "null samples with length treated as empty");


    expectTrue(convolution.can_apply_spatial_pan(), "convolution preflight applies spatial pan");
    expectTrue(convolution.skip_reason == fuse::audio::HrtfPanSkipReason::None,
               "spatial convolution has no skip reason");
               "preflight path matches resolve_hrtf_pan_path");

    const fuse::audio::HrtfPanPathPreflight ild_stub =
    expectTrue(ild_stub.uses_ild_itd_stub(), "empty IR preflight selects ILD/ITD stub");
    expectTrue(ild_stub.empty_ir, "ILD stub preflight marks empty IR");
    expectTrue(!ild_stub.should_skip_attenuation_coupling(),
               "ILD stub preflight does not skip attenuation coupling");

    expectTrue(disabled.should_skip_pan(), "disabled HRTF preflight skips pan");
    expectTrue(disabled.hrtf_disabled, "disabled HRTF preflight marks disabled flag");
    expectTrue(disabled.skip_reason == fuse::audio::HrtfPanSkipReason::HrtfDisabled,
               "disabled HRTF preflight reports HrtfDisabled reason");
    expectTrue(disabled.should_skip_spatial_pan()
                   == fuse::audio::should_skip_hrtf_spatial_pan(disabled.path),
               "preflight skip matches should_skip_hrtf_spatial_pan");

    expectTrue(co_located_preflight.co_located, "co-located preflight marks co-located flag");
    expectTrue(co_located_preflight.skip_reason == fuse::audio::HrtfPanSkipReason::CoLocated,
               "co-located preflight reports CoLocated reason");
    expectTrue(co_located_preflight.should_skip_attenuation_coupling(),
               "co-located preflight skips attenuation coupling");

    expectTrue(no_ir.uses_ild_itd_stub(), "no-IR overload preflight selects ILD/ITD stub");
    expectTrue(no_ir.path == fuse::audio::resolve_hrtf_pan_path(true, offset),


        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass.bypass_pan_path, "bypass path preflight marks bypass");
    expectTrue(bypass.should_skip_coupling(), "bypass path preflight skips coupling");
    expectTrue(!bypass.should_narrow_spatial_image(),
               "bypass path preflight does not narrow spatial image");

        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity.unity_attenuation, "unity attenuation preflight marks unity gain");
    expectTrue(unity.unity_spatial_blend, "unity attenuation preflight yields unity blend");
    expectTrue(unity.should_skip_coupling(), "unity attenuation preflight skips coupling");
    expectTrue(unity.should_skip_coupling()
                   == fuse::audio::should_skip_hrtf_spatial_blend(1.f, 1.f),
               "preflight skip matches should_skip_hrtf_spatial_blend");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation preflight is non-unity");
    expectTrue(narrowed.should_narrow_spatial_image(),
               "reduced attenuation preflight narrows spatial image");
    expectTrue(narrowed.should_narrow_spatial_image()
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          narrowed.path, 0.2f, 0.3f),
               "preflight narrow matches should_narrow_hrtf_spatial_image");
    expectTrue(narrowed.spatial_blend < 1.f, "reduced attenuation preflight blend is below unity");
    expectTrue(narrowed.can_couple(), "reduced attenuation preflight can couple");

    const fuse::audio::HrtfAttenuationCouplingPreflight bundled =
        fuse::audio::preflight_hrtf_attenuation_coupling(true, valid, offset, 0.15f, 0.25f);
    const fuse::audio::HrtfAttenuationCouplingPreflight manual =
            fuse::audio::resolve_hrtf_pan_path(true, valid, offset), 0.15f, 0.25f);
    expectNear(bundled.spatial_blend, manual.spatial_blend, 1e-5f,
               "bundled preflight matches path-only preflight blend");
    expectTrue(bundled.can_couple() == manual.can_couple(),
               "bundled preflight can_couple matches path-only preflight");

    const fuse::audio::HrtfAttenuationCouplingPreflight disabled_bundle =
        fuse::audio::preflight_hrtf_attenuation_coupling(false, empty, offset, 0.1f, 0.1f);
    expectTrue(disabled_bundle.bypass_pan_path,
               "disabled HRTF bundled preflight bypasses pan path");
    expectTrue(disabled_bundle.should_skip_coupling(),
               "disabled HRTF bundled preflight skips coupling");
void testPreflightHrtfIrConvolution() {
    fuse::audio::HrtfIrPreflightRejectReason reason =
        fuse::audio::HrtfIrPreflightRejectReason::None;

    expectTrue(!fuse::audio::preflight_hrtf_ir_convolution(empty),
               "empty IR fails convolution preflight");
    expectTrue(!fuse::audio::try_preflight_hrtf_ir_convolution(empty, &reason),
               "try_preflight rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfIrPreflightRejectReason::NullSamples,
               "empty IR reports null samples reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_ir_convolution(null_samples_nonzero_length, &reason),
               "null samples with length fails preflight");
               "null samples reports NullSamples reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_ir_convolution(malformed, &reason),
               "zero-length IR fails preflight");
    expectTrue(reason == fuse::audio::HrtfIrPreflightRejectReason::ZeroLength,
               "zero-length IR reports ZeroLength reject reason");

    expectTrue(fuse::audio::preflight_hrtf_ir_convolution(valid),
               "valid IR passes convolution preflight");
    expectTrue(fuse::audio::try_preflight_hrtf_ir_convolution(valid, &reason),
               "try_preflight accepts valid IR");
    expectTrue(reason == fuse::audio::HrtfIrPreflightRejectReason::None,
               "valid IR reports None reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir_convolution(valid)
                   == fuse::audio::should_use_hrtf_ir(valid),
               "preflight matches should_use_hrtf_ir on valid IR");
    expectTrue(!fuse::audio::preflight_hrtf_ir_convolution(empty)
               "preflight inverts should_skip_hrtf_convolution");

    fuse::audio::HrtfPanPreflightRejectReason reason =
        fuse::audio::HrtfPanPreflightRejectReason::None;

    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, offset),
               "enabled separated source passes pan preflight");
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, offset, &reason),
               "try_preflight accepts enabled separated source");
    expectTrue(reason == fuse::audio::HrtfPanPreflightRejectReason::None,
               "enabled separated source reports None reject reason");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(false, offset),
               "disabled HRTF fails pan preflight");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, offset, &reason),
               "try_preflight rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfPanPreflightRejectReason::Disabled,
               "disabled HRTF reports Disabled reject reason");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(true, co_located),
               "co-located source fails pan preflight");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(true, co_located, &reason),
               "try_preflight rejects co-located source");
    expectTrue(reason == fuse::audio::HrtfPanPreflightRejectReason::CoLocated,
               "co-located source reports CoLocated reject reason");

    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, offset)
                   == fuse::audio::should_apply_hrtf_pan(true, offset),
               "preflight matches should_apply_hrtf_pan");
    expectTrue(fuse::audio::preflight_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD/ITD stub path passes path preflight");
    expectTrue(fuse::audio::preflight_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution),
               "convolution path passes path preflight");
    expectTrue(!fuse::audio::preflight_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "bypass path fails path preflight");
    expectTrue(fuse::audio::preflight_hrtf_pan_path(fuse::audio::HrtfPanPath::Convolution)
                   == fuse::audio::is_spatial_hrtf_pan_path(
                          fuse::audio::HrtfPanPath::Convolution),
               "path preflight matches is_spatial_hrtf_pan_path");

    fuse::audio::HrtfAttenuationCouplingPreflightRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::None;

    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "spatial path with reduced attenuation passes coupling preflight");
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f, &reason),
               "try_preflight accepts reduced distance attenuation");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::None,
               "reduced attenuation reports None reject reason");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f),
               "bypass path fails coupling preflight");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, &reason),
               "try_preflight rejects bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::BypassPath,
               "bypass path reports BypassPath reject reason");

               "unity attenuation fails coupling preflight");
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f, &reason),
               "try_preflight rejects unity attenuation");
    expectTrue(
        reason == fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::UnityAttenuation,
        "unity attenuation reports UnityAttenuation reject reason");

                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f)
                          fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f),
               "preflight matches should_narrow_hrtf_spatial_image");

    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);
    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        narrowed, fuse::audio::HrtfPanPath::IldItdStub, 0.15f, 0.2f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed)
                   < fuse::audio::compute_pan_spread(wide),
               "preflight-guarded coupling still narrows spatial image");

    fuse::audio::BinauralPanGains bypassed = wide;
        bypassed, fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f);
    expectNear(bypassed.left, wide.left, 1e-5f,
               "preflight-guarded coupling skips bypass path");
    expectNear(bypassed.right, wide.right, 1e-5f,
    expectTrue(empty_preflight.empty_ir, "empty IR preflight marks empty_ir");
    expectTrue(!empty_preflight.can_convolve(), "empty IR preflight cannot convolve");
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "default empty IR reports NullSamples");

    expectTrue(valid_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight reason is None");

    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight = fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.malformed, "zero-length IR preflight marks malformed");
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "zero-length IR preflight reports ZeroLength");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &reason),
               "bool preflight_hrtf_ir passes for valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None, "bool preflight reason is None");
    expectTrue(!fuse::audio::preflight_hrtf_ir(empty, &reason),
               "bool preflight_hrtf_ir rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "bool preflight reports NullSamples for empty IR");

    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                           "ZeroLength") == 0,
               "IR reject reason label for ZeroLength");


                               fuse::audio::HrtfIrRejectReason::ZeroLength),
}

void testPreflightHrtfPanPath() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const float samples[] = {1.f};

    const fuse::audio::HrtfPanPathPreflight convolution =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(convolution.can_spatial_pan(), "convolution path passes spatial preflight");
    expectTrue(convolution.can_convolve(), "convolution path can convolve");
    expectTrue(convolution.uses_convolution, "convolution path uses convolution");
    expectTrue(convolution.reject_reason == fuse::audio::HrtfPanPathRejectReason::None,
               "convolution path has no reject reason");

    const fuse::audio::HrtfPanPathPreflight stub =
        fuse::audio::preflight_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), offset);
    expectTrue(stub.can_spatial_pan(), "ILD/ITD stub passes spatial preflight");
    expectTrue(stub.uses_ild_itd_stub, "empty IR preflight selects ILD/ITD stub");
    expectTrue(!stub.can_convolve(), "ILD/ITD stub cannot convolve");

    const fuse::audio::HrtfPanPathPreflight disabled =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(!disabled.can_spatial_pan(), "disabled HRTF fails spatial preflight");
    expectTrue(disabled.bypassed, "disabled HRTF is bypassed");
    expectTrue(disabled.reject_reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF reports HrtfDisabled");

    const fuse::audio::HrtfPanPathPreflight co_located =
        fuse::audio::preflight_hrtf_pan_path(true, valid, fuse::audio::Vec3{});
    expectTrue(!co_located.can_spatial_pan(), "co-located source fails spatial preflight");
    expectTrue(co_located.reject_reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source reports CoLocated");

    const fuse::audio::HrtfPanPathPreflight no_ir =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir.uses_ild_itd_stub, "no-IR overload selects ILD/ITD stub");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, valid, offset, &reason),
               "bool pan-path preflight passes for spatial source");
    expectTrue(!fuse::audio::preflight_hrtf_pan_path(false, valid, offset, &reason),
               "bool pan-path preflight rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "bool pan-path preflight reports HrtfDisabled");

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "classify pan-path reject for disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, fuse::audio::Vec3{})
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "classify pan-path reject for co-located source");

    const fuse::audio::HrtfPanPath spatial_path =
        fuse::audio::resolve_hrtf_pan_path(true, offset);



        fuse::audio::preflight_hrtf_attenuation_coupling(spatial_path, 0.2f, 0.3f);
    expectTrue(narrowed.can_apply_coupling(), "reduced attenuation passes coupling preflight");
    expectTrue(narrowed.can_narrow(), "reduced attenuation can narrow");
    expectTrue(narrowed.spatial_blend < 1.f, "reduced attenuation yields sub-unity blend");
    expectTrue(narrowed.reject_reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight reason is None");

        fuse::audio::preflight_hrtf_attenuation_coupling(spatial_path, 1.f, 1.f);
    expectTrue(!unity.can_apply_coupling(), "unity attenuation skips coupling preflight");
    expectTrue(unity.unity_attenuation, "unity attenuation flagged in preflight");
    expectTrue(unity.reject_reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypassed =
    expectTrue(!bypassed.can_apply_coupling(), "bypass path skips coupling preflight");
    expectTrue(bypassed.bypass_path, "bypass path flagged in preflight");
    expectTrue(bypassed.reject_reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::PanBypassed,
               "bypass path reports PanBypassed");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(spatial_path, 0.5f, 0.5f, &reason),
               "bool coupling preflight passes for reduced attenuation");
                   fuse::audio::HrtfPanPath::Bypass, 0.5f, 0.5f, &reason),
               "bool coupling preflight rejects bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::PanBypassed,
               "bool coupling preflight reports PanBypassed");

    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
               "classify coupling reject for bypass path");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(spatial_path, 1.f, 1.f)
               "classify coupling reject for unity attenuation");

    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation") == 0,
               "coupling reject reason label for UnityAttenuation");
               "no-IR pan preflight selects ILD/ITD stub");

    fuse::audio::HrtfPanPathPreflightReject reject =
        fuse::audio::HrtfPanPathPreflightReject::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, &reject),
               "try_preflight_hrtf_pan_path succeeds for spatial source");
    expectTrue(reject == fuse::audio::HrtfPanPathPreflightReject::None,
               "spatial pan try_preflight reject is None");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, &reject),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reject == fuse::audio::HrtfPanPathPreflightReject::Disabled,
               "disabled pan try_preflight reports Disabled");

    expectTrue(convolution.path
                   == fuse::audio::resolve_hrtf_pan_path(true, valid, offset),
               "pan preflight path matches resolve_hrtf_pan_path");

void testHrtfAttenuationCouplingPreflightGuards() {
}

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass.can_skip(), "bypass attenuation preflight can skip coupling");
    expectTrue(bypass.reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::BypassPath,
               "bypass attenuation preflight reports BypassPath");

            fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f);
    expectTrue(unity.can_skip(), "unity attenuation preflight can skip narrowing");
    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(
    expectTrue(
        unity.reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::UnityAttenuation,
        "unity attenuation preflight reports UnityAttenuation");
    expectNear(unity.spatialBlend, 1.f, 1e-5f,
               "unity attenuation preflight spatial blend is unity");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(
            fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f);
    expectTrue(narrowed.narrowImage, "reduced attenuation preflight narrows image");
    expectTrue(!narrowed.can_skip(), "reduced attenuation preflight cannot skip");
    expectTrue(narrowed.reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::None,
               "narrowing preflight reject is None");
    expectTrue(narrowed.spatialBlend < 1.f, "narrowing preflight spatial blend is sub-unity");

    fuse::audio::HrtfAttenuationCouplingPreflightReject reject =
        fuse::audio::HrtfAttenuationCouplingPreflightReject::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.15f, 1.f, &reject),
               "try_preflight attenuation coupling succeeds for reduced distance");
    expectTrue(reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::None,
               "narrowing try_preflight reject is None");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.15f, 1.f, &reject),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::BypassPath,
               "bypass try_preflight reports BypassPath");

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    fuse::audio::BinauralPanGains via_preflight =
        fuse::audio::compute_binaural_pan_gains_for_path(fuse::audio::HrtfPanPath::IldItdStub,
                                                         offset);
    fuse::audio::BinauralPanGains manual =
        via_preflight, fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f);
        fuse::audio::compute_binaural_pan_gains_for_path(fuse::audio::HrtfPanPath::IldItdStub,
                                                         offset);
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        manual, fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f);
    expectNear(via_preflight.left, manual.left, 1e-5f,
               "for_path coupling via preflight matches manual path");
    expectNear(via_preflight.right, manual.right, 1e-5f,
    expectTrue(empty_preflight.emptyIr, "IR preflight marks empty IR");
    expectTrue(!empty_preflight.can_use_convolution(), "empty IR cannot use convolution");
    expectTrue(empty_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::Empty,
               "empty IR reject reason is Empty");

    expectTrue(valid_preflight.can_use_convolution(), "valid IR passes convolution preflight");
    expectTrue(valid_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reject reason is None");

    expectTrue(malformed_preflight.malformedIr, "malformed IR flagged in preflight");
    expectTrue(malformed_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::Malformed,
               "malformed IR reject reason is Malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::Empty;
    expectTrue(!fuse::audio::preflight_hrtf_ir(empty, &reason), "bool IR preflight rejects empty");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::Empty, "bool IR preflight reports Empty");
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &reason), "bool IR preflight accepts valid");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None, "bool IR preflight reports None");


    const fuse::audio::HrtfPanPathPreflight spatial =
    expectTrue(spatial.can_apply_spatial_pan(), "spatial offset passes pan-path preflight");
    expectTrue(spatial.usesConvolution, "valid IR selects convolution in preflight");
    expectTrue(spatial.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path has no reject reason");

    const fuse::audio::HrtfPanPathPreflight empty_ir =
    expectTrue(empty_ir.can_apply_spatial_pan(), "empty IR still applies spatial ILD/ITD stub");
    expectTrue(empty_ir.usesIldItdStub, "empty IR selects ILD/ITD stub in preflight");
    expectTrue(!empty_ir.usesConvolution, "empty IR does not select convolution in preflight");

    expectTrue(!disabled.can_apply_spatial_pan(), "disabled HRTF fails pan-path preflight");
    expectTrue(disabled.bypassed, "disabled HRTF is bypassed in preflight");
    expectTrue(disabled.rejectReason == fuse::audio::HrtfPanPathRejectReason::Disabled,
               "disabled HRTF reject reason is Disabled");

    expectTrue(!co_located.can_apply_spatial_pan(), "co-located source fails pan-path preflight");
    expectTrue(co_located.rejectReason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located reject reason is CoLocated");

    expectTrue(no_ir.usesIldItdStub, "no-IR overload selects ILD/ITD stub in preflight");

    fuse::audio::HrtfPanPathRejectReason reason =
        fuse::audio::HrtfPanPathRejectReason::Disabled;
               "bool pan-path preflight accepts spatial source");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "bool pan-path preflight reports None for spatial source");


    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
    expectTrue(narrow.can_narrow(), "reduced attenuation passes coupling preflight");
    expectTrue(!narrow.skipped, "narrowing is not skipped under reduced attenuation");
    expectTrue(narrow.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing reject reason is None");
    expectTrue(narrow.spatialBlend < 1.f, "preflight reports sub-unity spatial blend");

    expectTrue(!unity.can_narrow(), "unity attenuation fails coupling preflight");
    expectTrue(unity.skipped, "unity attenuation skips narrowing");
    expectTrue(unity.unityAttenuation, "unity attenuation flagged in preflight");
    expectTrue(unity.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reject reason is UnityAttenuation");

    expectTrue(!bypass.can_narrow(), "bypass path fails coupling preflight");
    expectTrue(bypass.bypassed, "bypass path flagged in preflight");
    expectTrue(bypass.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass reject reason is BypassPath");

    const fuse::audio::HrtfAttenuationCouplingPreflight resolved =
    expectTrue(resolved.can_narrow(), "resolved coupling preflight can narrow");
    expectTrue(resolved.path == fuse::audio::HrtfPanPath::Convolution,
               "resolved coupling preflight carries convolution path");

        fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath;
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, &reason),
               "bool coupling preflight accepts narrowing");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "bool coupling preflight reports None when narrowing applies");

void testHrtfBinauralPanCombinedPreflight() {

    const fuse::audio::HrtfBinauralPanPreflight spatial =
        fuse::audio::preflight_hrtf_binaural_pan(true, valid, offset, 0.2f, 0.3f);
    expectTrue(spatial.can_apply_spatial_pan(), "combined preflight allows spatial pan");
    expectTrue(spatial.can_apply_attenuation_coupling(), "combined preflight allows coupling");
    expectTrue(spatial.ir.can_use_convolution(), "combined preflight IR is valid");
    expectTrue(spatial.panPath.usesConvolution, "combined preflight selects convolution path");

    const fuse::audio::HrtfBinauralPanPreflight bypass =
        fuse::audio::preflight_hrtf_binaural_pan(false, valid, offset, 0.1f, 0.1f);
    expectTrue(!bypass.can_apply_spatial_pan(), "combined preflight blocks spatial when disabled");
    expectTrue(!bypass.can_apply_attenuation_coupling(),
               "combined preflight blocks coupling when bypassed");
    expectTrue(bypass.panPath.rejectReason == fuse::audio::HrtfPanPathRejectReason::Disabled,
               "combined preflight reports disabled pan-path reject");

    const fuse::audio::HrtfBinauralPanPreflight empty_ir =
        fuse::audio::preflight_hrtf_binaural_pan(true, fuse::audio::make_empty_hrtf_ir(), offset,
    expectTrue(empty_ir.can_apply_spatial_pan(), "empty IR still spatial via ILD/ITD stub");
    expectTrue(!empty_ir.can_apply_attenuation_coupling(),
               "unity attenuation skips coupling in combined preflight");
    expectTrue(!empty_ir.ir.can_use_convolution(), "combined preflight marks empty IR");

void testHrtfPreflightRejectReasonLabels() {
                               fuse::audio::HrtfIrRejectReason::Empty),
                           "Empty")
                   == 0,
               "IR reject reason label for Empty");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
               "pan-path reject reason label for CoLocated");
                               fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
                           "BypassPath")
               "coupling reject reason label for BypassPath");
    expectTrue(valid_preflight.can_convolve(), "valid IR passes preflight");
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid,
                                                        fuse::audio::HrtfIrRejectReason::NullSamples),
               "valid IR does not reject for null samples");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight = fuse::audio::preflight_hrtf_ir(null_samples);
    expectTrue(null_preflight.null_samples, "null samples flagged");
    expectTrue(null_preflight.skips_convolution(), "null samples skip convolution");
    expectTrue(null_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "null samples reject for null pointer");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(null_samples,
               "reject helper matches null samples");

    expectTrue(malformed_preflight.malformed, "non-null zero-length is malformed");
               "malformed IR rejects for zero length");
    expectTrue(malformed_preflight.skips_convolution() == fuse::audio::should_skip_hrtf_convolution(malformed),
    const fuse::audio::HrtfIrPreflight empty =
        fuse::audio::preflight_hrtf_ir(fuse::audio::make_empty_hrtf_ir());
    expectTrue(empty.null_samples, "empty IR preflight marks null samples");
    expectTrue(empty.zero_length, "empty IR preflight marks zero length");
    expectTrue(!empty.malformed_stub, "factory empty IR is not malformed");
    expectTrue(!empty.has_valid_ir, "empty IR preflight has no valid IR");
    expectTrue(empty.should_skip_convolution(), "empty IR preflight skips convolution");
    expectTrue(!fuse::audio::can_convolve_hrtf_ir(fuse::audio::make_empty_hrtf_ir()),
               "can_convolve_hrtf_ir rejects empty IR");

    expectTrue(valid_preflight.has_valid_ir, "valid IR preflight enables convolution");
    expectTrue(fuse::audio::can_convolve_hrtf_ir(valid), "can_convolve_hrtf_ir accepts valid IR");
    expectTrue(valid_preflight.can_convolve()
               "preflight can_convolve matches should_use_hrtf_ir");

    expectTrue(malformed_preflight.malformed_stub, "malformed IR preflight marks zero length");

    const fuse::audio::HrtfIrStub null_nonzero{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_nonzero_preflight =
        fuse::audio::preflight_hrtf_ir(null_nonzero);
    expectTrue(null_nonzero_preflight.null_samples, "null samples flagged in preflight");
    expectTrue(null_nonzero_preflight.should_skip_convolution(),
               "null samples with length skips convolution");
void testEmptyHrtfIrPreflight() {
    const fuse::audio::EmptyHrtfIrPreflight empty_preflight =
        fuse::audio::preflightEmptyHrtfIr(empty);
    expectTrue(empty_preflight.nullSamples, "empty IR preflight marks null samples");
    expectTrue(empty_preflight.zeroLength, "empty IR preflight marks zero length");
    expectTrue(!empty_preflight.nonnullZeroLength, "factory empty IR is not malformed");
    expectTrue(!empty_preflight.hasIr(), "empty IR preflight has no IR");
    expectTrue(empty_preflight.shouldSkipConvolution(), "empty IR preflight skips convolution");
    expectTrue(fuse::audio::canUseHrtfIr(empty) == empty_preflight.hasIr(),
               "canUseHrtfIr mirrors preflight hasIr");

    const fuse::audio::EmptyHrtfIrPreflight valid_preflight =
        fuse::audio::preflightEmptyHrtfIr(valid);
    expectTrue(valid_preflight.hasIr(), "valid IR preflight has samples");
    expectTrue(!valid_preflight.shouldSkipConvolution(),
               "valid IR preflight does not skip convolution");
    expectTrue(fuse::audio::canUseHrtfIr(valid), "canUseHrtfIr accepts valid IR");

    const fuse::audio::EmptyHrtfIrPreflight malformed_preflight =
        fuse::audio::preflightEmptyHrtfIr(malformed);
    expectTrue(malformed_preflight.nonnullZeroLength,
               "malformed IR preflight marks non-null zero length");
    expectTrue(malformed_preflight.zeroLength, "malformed IR preflight marks zero length");
    expectTrue(!malformed_preflight.hasIr(), "malformed IR preflight has no IR");

    const fuse::audio::EmptyHrtfIrPreflight null_preflight =
        fuse::audio::preflightEmptyHrtfIr(null_samples_nonzero_length);
    expectTrue(null_preflight.nullSamples, "null samples preflight marks null samples");
    expectTrue(!null_preflight.hasIr(), "null samples with length still has no IR");
    expectTrue(empty.is_empty(), "empty IR preflight is empty");
    expectTrue(!empty.can_convolve(), "empty IR preflight cannot convolve");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.null_samples, "valid IR preflight has samples");
    expectTrue(!valid_preflight.zero_length, "valid IR preflight has non-zero length");
    expectTrue(!valid_preflight.malformed, "valid IR preflight is not malformed");
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(fuse::audio::can_convolve_hrtf_ir(valid) == fuse::audio::has_hrtf_ir(valid),
               "can_convolve_hrtf_ir matches has_hrtf_ir");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.malformed, "non-null zero-length IR is malformed");
    expectTrue(malformed_preflight.is_empty(), "malformed IR preflight is empty");
    expectTrue(!malformed_preflight.can_convolve(), "malformed IR cannot convolve");
}

void testHrtfPanPathPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(disabled.bypassed(), "disabled HRTF bypasses pan");
    expectTrue(disabled.reject_reason == fuse::audio::HrtfPanPathRejectReason::Disabled,
               "disabled reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(false, offset,
                                                             fuse::audio::HrtfPanPathRejectReason::Disabled),
               "reject helper matches disabled");

    expectTrue(co_located_preflight.co_located, "co-located source flagged");
    expectTrue(co_located_preflight.bypassed(), "co-located source bypasses pan");
    expectTrue(co_located_preflight.reject_reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located reject reason");

    expectTrue(stub.empty_ir, "empty IR flagged on pan preflight");
    expectTrue(stub.uses_ild_itd_stub(), "empty IR selects ILD/ITD stub");
    expectTrue(stub.can_spatial_pan(), "ILD/ITD stub is spatial");
    expectTrue(stub.reject_reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");

    expectTrue(convolution.uses_convolution(), "valid IR selects convolution");
    expectTrue(convolution.path == fuse::audio::resolve_hrtf_pan_path(true, valid, offset),

    expectTrue(no_ir.uses_ild_itd_stub(), "no-IR overload selects ILD/ITD stub");
    expectTrue(no_ir.empty_ir, "no-IR overload marks empty IR");

void testHrtfAttenuationCouplingPreflight() {

                                                          0.2f);
    expectTrue(bypass.bypass_path, "bypass path flagged");
    expectTrue(bypass.skips_coupling(), "bypass path skips coupling");
    expectTrue(bypass.skip_reason == fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath,
               "bypass skip reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_skips_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.2f,
                   fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath),
               "skip helper matches bypass");

    expectTrue(unity.unity_attenuation, "unity attenuation flagged");
    expectTrue(unity.unity_spatial_blend, "unity spatial blend flagged");
    expectTrue(unity.skips_coupling(), "unity attenuation skips coupling");
    expectTrue(unity.skip_reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity skip reason");

    expectTrue(narrowed.should_narrow(), "reduced attenuation warrants narrowing");
    expectTrue(narrowed.can_apply_coupling(), "reduced attenuation can apply coupling");
    expectTrue(narrowed.skip_reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "narrowing preflight has no skip reason");
    expectTrue(narrowed.spatial_blend < 1.f, "reduced attenuation lowers spatial blend");

    fuse::audio::BinauralPanGains manual = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(manual,
                                                         fuse::audio::HrtfPanPath::Convolution, 0.2f,
                                                         0.3f);
    expectTrue(narrowed.should_narrow()
                          fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f),
               "preflight should_narrow matches combined guard");
    expectTrue(fuse::audio::compute_pan_spread(manual) < fuse::audio::compute_pan_spread(wide),
               "preflight narrowing path still narrows pan spread when applied");
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};

    const fuse::audio::HrtfPanPathPreflight disabled =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(disabled.hrtf_disabled, "preflight marks disabled HRTF");
    expectTrue(disabled.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled HRTF resolves to bypass");
    expectTrue(disabled.should_skip_spatial_pan(), "disabled HRTF skips spatial pan");
    expectTrue(!fuse::audio::can_apply_hrtf_spatial_pan(false, valid, offset),
               "can_apply_hrtf_spatial_pan rejects disabled HRTF");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.co_located, "preflight marks co-located source");
    expectTrue(co_located_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "co-located source resolves to bypass");

        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(empty_ir.empty_ir, "preflight marks empty IR");
    expectTrue(empty_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR resolves to ILD/ITD stub");
    expectTrue(empty_ir.uses_ild_itd_stub(), "empty IR uses ILD/ITD stub path");
    expectTrue(empty_ir.can_apply_spatial_pan(), "empty IR still applies spatial pan");
    expectTrue(fuse::audio::can_apply_hrtf_spatial_pan(true, empty, offset),
               "can_apply_hrtf_spatial_pan accepts empty IR with offset");

    const fuse::audio::HrtfPanPathPreflight convolution =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(!convolution.empty_ir, "valid IR clears empty_ir flag");
    expectTrue(convolution.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR resolves to convolution");
    expectTrue(convolution.uses_convolution(), "valid IR uses convolution path");
               "preflight path matches resolve_hrtf_pan_path");

    const fuse::audio::HrtfPanPathPreflight no_ir =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload selects ILD/ITD stub");
    expectTrue(fuse::audio::can_apply_hrtf_spatial_pan(true, offset),
               "can_apply_hrtf_spatial_pan accepts no-IR spatial path");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                        0.1f);
    expectTrue(bypass.bypass_path, "bypass path flagged in coupling preflight");
    expectTrue(bypass.should_skip_coupling(), "bypass path skips coupling");
    expectTrue(!bypass.would_narrow, "bypass path does not narrow spatial image");
    expectTrue(!fuse::audio::can_apply_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f),
               "can_apply_hrtf_attenuation_coupling rejects bypass");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity.unity_attenuation, "unity attenuation flagged in preflight");
    expectTrue(unity.should_skip_coupling(), "unity attenuation skips coupling");
    expectTrue(!unity.would_narrow, "unity attenuation does not narrow");
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(unity.spatial_blend),
               "unity attenuation yields unity spatial blend");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrowed.bypass_path, "spatial path clears bypass flag");
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation is non-unity");
    expectTrue(narrowed.would_narrow, "reduced attenuation would narrow image");
    expectTrue(narrowed.can_apply_coupling(), "spatial non-unity path can apply coupling");
    expectTrue(fuse::audio::can_apply_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "can_apply_hrtf_attenuation_coupling accepts spatial narrowing");
    expectTrue(narrowed.would_narrow
                   == fuse::audio::should_narrow_hrtf_spatial_image(
               "preflight would_narrow matches should_narrow_hrtf_spatial_image");
    expectTrue(narrowed.can_apply_coupling()
                   == !fuse::audio::should_skip_hrtf_spatial_blend(0.2f, 0.3f),
               "preflight can_apply_coupling inverts should_skip_hrtf_spatial_blend");

    const fuse::audio::HrtfPanPathPreflight bypass =
        fuse::audio::preflightHrtfPanPath(false, valid, offset);
    expectTrue(bypass.hrtfDisabled, "disabled HRTF preflight marks hrtfDisabled");
    expectTrue(bypass.shouldSkipSpatialPan(), "disabled HRTF preflight skips spatial pan");
    expectTrue(bypass.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled HRTF preflight resolves to bypass");

        fuse::audio::preflightHrtfPanPath(true, valid, co_located);
    expectTrue(co_located_preflight.coLocated, "co-located preflight marks coLocated");
    expectTrue(co_located_preflight.shouldSkipSpatialPan(),
               "co-located preflight skips spatial pan");

    const fuse::audio::HrtfPanPathPreflight stub =
        fuse::audio::preflightHrtfPanPath(true, empty, offset);
    expectTrue(stub.emptyIr, "empty IR preflight marks emptyIr");
    expectTrue(stub.canApplySpatialPan(), "empty IR preflight still applies spatial pan");
    expectTrue(!stub.canUseConvolution(), "empty IR preflight does not use convolution");
    expectTrue(stub.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR preflight resolves to ILD/ITD stub");

        fuse::audio::preflightHrtfPanPath(true, valid, offset);
    expectTrue(convolution.canUseConvolution(), "valid IR preflight can use convolution");
               "valid IR preflight resolves to convolution");

        fuse::audio::preflightHrtfPanPath(true, offset);
    expectTrue(no_ir.emptyIr, "no-IR overload preflight marks emptyIr");
               "no-IR overload preflight resolves to ILD/ITD stub");

    expectTrue(fuse::audio::canApplyHrtfPanPath(fuse::audio::HrtfPanPath::Convolution),
               "canApplyHrtfPanPath accepts convolution");
    expectTrue(!fuse::audio::canApplyHrtfPanPath(fuse::audio::HrtfPanPath::Bypass),
               "canApplyHrtfPanPath rejects bypass");
    expectTrue(fuse::audio::canApplyHrtfPanPath(convolution.path)
                   == convolution.canApplySpatialPan(),
               "canApplyHrtfPanPath mirrors preflight canApplySpatialPan");

        fuse::audio::preflightHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
    expectTrue(bypass.bypassPath, "bypass path preflight marks bypassPath");
    expectTrue(!bypass.canApplyCoupling(), "bypass path preflight cannot apply coupling");
    expectTrue(!bypass.shouldNarrowSpatialImage(),
               "bypass path preflight does not narrow spatial image");

        fuse::audio::preflightHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::Convolution, 1.f,
                                                      1.f);
    expectTrue(unity.unityAttenuation, "unity attenuation preflight marks unityAttenuation");
    expectTrue(!unity.canApplyCoupling(), "unity attenuation preflight cannot apply coupling");
               "unity attenuation preflight reports unity spatial blend");

        fuse::audio::preflightHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::IldItdStub, 0.2f,
    expectTrue(!narrowed.bypassPath, "spatial path preflight is not bypass");
    expectTrue(!narrowed.unityAttenuation, "reduced attenuation preflight is non-unity");
    expectTrue(narrowed.canApplyCoupling(), "reduced attenuation preflight can apply coupling");
    expectTrue(narrowed.shouldNarrowSpatialImage(),
               "reduced attenuation preflight narrows spatial image");
    expectTrue(narrowed.spatialBlend < 1.f, "reduced attenuation preflight blend is below unity");

    expectTrue(fuse::audio::canApplyHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::Convolution,
                                                            0.2f, 1.f),
               "canApplyHrtfAttenuationCoupling accepts reduced distance attenuation");
    expectTrue(!fuse::audio::canApplyHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::Bypass,
                                                              0.2f, 0.3f),
               "canApplyHrtfAttenuationCoupling rejects bypass path");
    expectTrue(fuse::audio::canApplyHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                            0.2f, 0.3f)
                   == narrowed.canApplyCoupling(),
               "canApplyHrtfAttenuationCoupling mirrors preflight canApplyCoupling");

    expectTrue(convolution.should_convolve(), "preflight selects convolution with valid IR");
    expectTrue(convolution.can_spatialize(), "convolution path can spatialize");
    expectTrue(convolution.has_valid_ir, "convolution preflight marks valid IR");
    expectTrue(!convolution.empty_ir, "convolution preflight is not empty IR");

    expectTrue(stub.should_stub(), "preflight selects ILD/ITD stub for empty IR");
    expectTrue(stub.empty_ir, "stub preflight marks empty IR");
    expectTrue(!stub.has_valid_ir, "stub preflight has no valid IR");

    expectTrue(disabled.is_bypassed(), "preflight bypasses when HRTF disabled");
    expectTrue(disabled.hrtf_disabled, "disabled preflight marks hrtf_disabled");

    expectTrue(co_located_preflight.is_bypassed(), "preflight bypasses co-located source");
    expectTrue(co_located_preflight.co_located, "co-located preflight marks co_located");

    expectTrue(no_ir.should_stub(), "no-IR overload preflight selects stub");
    expectTrue(no_ir.path == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "no-IR preflight path matches resolve");

    expectTrue(fuse::audio::can_spatialize_hrtf_pan(true, offset),
               "can_spatialize_hrtf_pan accepts separated source");
    expectTrue(!fuse::audio::can_spatialize_hrtf_pan(false, offset),
               "can_spatialize_hrtf_pan rejects disabled HRTF");
    expectTrue(fuse::audio::can_spatialize_hrtf_pan(true, offset)
                   == fuse::audio::should_apply_hrtf_pan(true, offset),
               "can_spatialize_hrtf_pan matches should_apply_hrtf_pan");

    expectTrue(bypass.bypass_pan, "coupling preflight marks bypass pan");
    expectTrue(!bypass.can_apply_coupling(), "bypass path cannot apply coupling");
    expectTrue(!bypass.should_narrow(), "bypass path should not narrow");

        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 1.f,
    expectTrue(unity.unity_attenuation, "unity attenuation preflight marks full gain");
    expectTrue(unity.unity_spatial_blend, "unity attenuation yields unity spatial blend");
    expectTrue(!unity.can_apply_coupling(), "unity attenuation skips coupling");
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f)
                          fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f),
               "can_apply_hrtf_attenuation_coupling matches should_narrow at unity");

    expectTrue(narrowed.should_narrow(), "reduced attenuation should narrow");
    expectTrue(!narrowed.unity_spatial_blend, "reduced attenuation is not unity blend");
    expectTrue(narrowed.spatial_blend < 1.f, "reduced attenuation yields sub-unity blend");
    expectTrue(narrowed.path == fuse::audio::HrtfPanPath::Convolution,
               "coupling preflight preserves pan path");

                   fuse::audio::HrtfPanPath::Convolution, 0.15f, 0.25f),
                   fuse::audio::HrtfPanPath::Bypass, 0.15f, 0.25f),
               "can_apply_hrtf_attenuation_coupling rejects bypass path");


               "for_path coupling via preflight matches manual path");

void testBinauralPanGainSampleHelpers() {
    const fuse::audio::BinauralPanGains centre = fuse::audio::make_centre_binaural_pan_gains();
    expectNear(fuse::audio::compute_binaural_pan_energy(centre), 0.5f, 1e-5f,
               "centre pan energy is 0.5 under equal-power stub");

    expectTrue(fuse::audio::has_nonzero_itd(wide), "lateral pan has non-zero ITD stub");
    expectTrue(!fuse::audio::has_nonzero_itd(centre), "centre pan has zero ITD");

    fuse::audio::BinauralPanGains scaled = wide;
    fuse::audio::scale_binaural_pan_gains(scaled, 0.5f);
    expectNear(scaled.left, wide.left * 0.5f, 1e-5f, "scale halves left gain");
    expectNear(scaled.right, wide.right * 0.5f, 1e-5f, "scale halves right gain");

    float left = 0.f;
    float right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample(1.f, wide, 0.5f, left, right);
    expectNear(left, 0.5f * wide.left, 1e-5f, "apply_binaural_pan_to_sample scales left");
    expectNear(right, 0.5f * wide.right, 1e-5f, "apply_binaural_pan_to_sample scales right");

    left = 0.f;
    right = 0.f;
    fuse::audio::apply_centre_binaural_pan_to_sample(1.f, 0.25f, left, right);
    expectNear(left, 0.25f, 1e-5f, "centre pan applies equal attenuated mono to left");
    expectNear(right, 0.25f, 1e-5f, "centre pan applies equal attenuated mono to right");

void testHrtfIrPreflightSkipAlias() {
    expectTrue(empty_preflight.should_skip_convolution(), "empty IR preflight skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight),
               "should_skip_hrtf_ir_convolution mirrors struct method");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight)
                   == !fuse::audio::can_convolve_hrtf_ir(empty_preflight),
               "skip/convolve IR preflight predicates are inverses");

    expectTrue(!valid_preflight.should_skip_convolution(), "valid IR preflight does not skip convolution");
    expectTrue(!fuse::audio::should_skip_hrtf_ir_convolution(valid_preflight),
               "valid IR should_skip_hrtf_ir_convolution is false");

void testHrtfPanPathPreflightPredicateAliases() {

    expectTrue(stub_preflight.uses_ild_itd_stub(), "stub preflight selects ILD/ITD path");
    expectTrue(!stub_preflight.should_skip(), "stub preflight is not skipped");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_pan_path(stub_preflight),
               "uses_ild_itd_stub_hrtf_pan_path mirrors struct method");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(stub_preflight),
               "should_skip_hrtf_pan_path_preflight false on stub path");
    expectTrue(fuse::audio::can_convolve_hrtf_pan_path(stub_preflight)
                   == stub_preflight.can_convolve(),
               "can_convolve_hrtf_pan_path mirrors struct method");

    expectTrue(fuse::audio::can_convolve_hrtf_pan_path(conv_preflight),
               "convolution pan-path preflight can convolve");
    expectTrue(!conv_preflight.uses_ild_itd_stub(), "convolution preflight does not use ILD/ITD stub");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF pan-path preflight is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(bypass_preflight),
               "should_skip_hrtf_pan_path_preflight true on bypass");
    expectTrue(!fuse::audio::can_apply_spatial_hrtf_pan(bypass_preflight),
               "bypass pan-path preflight cannot spatial-pan");

void testHrtfAttenuationCouplingPreflightSkipAlias() {
    expectTrue(unity_preflight.should_skip(), "unity attenuation coupling preflight is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(unity_preflight),
               "should_skip_hrtf_attenuation_coupling_preflight mirrors struct method");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(unity_preflight)
                   == !fuse::audio::can_narrow_hrtf_spatial_image(unity_preflight),
               "skip/can_narrow coupling preflight predicates are inverses");

    expectTrue(!narrow_preflight.should_skip(), "reduced attenuation coupling preflight is not skipped");
    expectTrue(fuse::audio::can_narrow_hrtf_spatial_image(narrow_preflight),
               "can_narrow_hrtf_spatial_image mirrors struct method");

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfBinauralPreflightPredicateAliases() {

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.uses_ild_itd_stub(), "composite stub path uses ILD/ITD");
    expectTrue(!stub_preflight.should_skip(), "composite stub path is not skipped");
    expectTrue(stub_preflight.should_skip_convolution(), "composite stub path skips convolution");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_binaural(stub_preflight),
               "uses_ild_itd_stub_hrtf_binaural mirrors struct method");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_convolution(stub_preflight),
               "should_skip_hrtf_binaural_convolution mirrors struct method");
    expectTrue(fuse::audio::can_convolve_hrtf_binaural(stub_preflight)
               "can_convolve_hrtf_binaural mirrors struct method");
    expectTrue(fuse::audio::can_narrow_hrtf_binaural_spatial_image(stub_preflight)
                   == stub_preflight.can_narrow_spatial_image(),
               "can_narrow_hrtf_binaural_spatial_image mirrors struct method");
    expectTrue(fuse::audio::can_apply_hrtf_binaural_pan(stub_preflight)
                   == stub_preflight.can_spatial_pan(),
               "can_apply_hrtf_binaural_pan mirrors can_spatial_pan");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(stub_preflight)
                   == stub_preflight.should_skip(),
               "should_skip_hrtf_binaural mirrors should_skip");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(fuse::audio::can_convolve_hrtf_binaural(conv_preflight),
               "valid IR composite preflight can convolve via alias");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_convolution(conv_preflight),
               "valid IR composite preflight does not skip convolution");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.should_skip(), "composite bypass preflight should_skip");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass_preflight),
               "should_skip_hrtf_binaural true on bypass");
    expectTrue(bypass_preflight.should_skip_convolution(),
               "bypass composite preflight skips convolution");

void testListenerAwareHrtfBinauralPreflight() {
    fuse::audio::AudioListener listener{};
    listener.position = fuse::audio::Vec3{0.f, 0.f, 0.f};
    listener.forward = fuse::audio::Vec3{0.f, 0.f, -1.f};
    listener.up = fuse::audio::Vec3{0.f, 1.f, 0.f};
    const fuse::audio::Vec3 source_position{5.f, 0.f, -5.f};

    const fuse::audio::HrtfBinauralPreflight listener_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, 0.25f, 0.35f);
    const fuse::audio::Vec3 rel_listener =
        fuse::audio::to_listener_space(source_position - listener.position,
                                       fuse::audio::compute_listener_basis(listener));
    const fuse::audio::HrtfBinauralPreflight offset_preflight =
        fuse::audio::preflight_hrtf_binaural(true, rel_listener, 0.25f, 0.35f);
    expectTrue(listener_preflight.panPath.path == offset_preflight.panPath.path,
               "listener preflight path matches listener-local preflight");
    expectTrue(listener_preflight.can_spatial_pan() == offset_preflight.can_spatial_pan(),
               "listener preflight can_spatial_pan matches offset preflight");
    expectTrue(listener_preflight.can_narrow_spatial_image()
                   == offset_preflight.can_narrow_spatial_image(),
               "listener preflight narrowing matches offset preflight");

    const fuse::audio::HrtfBinauralPreflight listener_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, valid, 1.f, 1.f);
    const fuse::audio::HrtfBinauralPreflight offset_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, rel_listener, 1.f, 1.f);
    expectTrue(listener_ir_preflight.can_convolve() == offset_ir_preflight.can_convolve(),
               "listener IR preflight can_convolve matches offset preflight");

void testApplyBinauralPanToSampleFromPreflight() {

    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, stub_preflight, offset, 0.5f,
                                                             left, right);
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 0.2f, 0.3f);
    expectNear(left, 0.5f * coupled.left, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");
    expectNear(right, 0.5f * coupled.right, 1e-5f,

        fuse::audio::preflight_hrtf_binaural(false, empty, offset, 0.1f, 0.1f);
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, bypass_preflight, offset, 0.25f,
    expectNear(left, 0.25f, 1e-5f, "bypass preflight applies centre pan to left");
    expectNear(right, 0.25f, 1e-5f, "bypass preflight applies centre pan to right");

void testGuardedPanRoutesThroughCompositePreflight() {

    const fuse::audio::BinauralPanGains guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, offset);
    const fuse::audio::BinauralPanGains direct =
        fuse::audio::compute_binaural_pan_gains_for_path(
            fuse::audio::resolve_hrtf_pan_path(true, offset), offset);
    expectNear(guarded.left, direct.left, 1e-5f,
               "guarded pan still matches for_path on valid stub path");
    expectNear(guarded.right, direct.right, 1e-5f,

    const fuse::audio::BinauralPanGains ir_guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, valid, offset);
    const fuse::audio::BinauralPanGains ir_direct =
            fuse::audio::resolve_hrtf_pan_path(true, valid, offset), offset);
    expectNear(ir_guarded.left, ir_direct.left, 1e-5f,
               "IR-aware guarded pan still matches for_path on valid path");
    expectNear(ir_guarded.right, ir_direct.right, 1e-5f,

    const fuse::audio::BinauralPanGains bypass_guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(false, valid, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass_guarded),
               "guarded bypass still returns centre pan");

void testHrtfBinauralPreflight() {

    expectTrue(stub_preflight.ir.emptyIr, "composite preflight carries empty-IR diagnostics");
    expectTrue(stub_preflight.panPath.path == fuse::audio::HrtfPanPath::IldItdStub,
               "composite preflight selects ILD/ITD stub for empty IR");
    expectTrue(stub_preflight.can_spatial_pan(), "composite preflight allows spatial pan on stub path");
    expectTrue(!stub_preflight.can_convolve(), "composite preflight cannot convolve with empty IR");
    expectTrue(stub_preflight.can_narrow_spatial_image(),
               "reduced attenuation narrows via composite preflight");
    expectTrue(!stub_preflight.is_bypass(), "enabled offset source is not bypassed");
    expectTrue(fuse::audio::can_apply_hrtf_binaural_pan(stub_preflight),
    expectTrue(!fuse::audio::should_skip_hrtf_binaural(stub_preflight),
               "should_skip_hrtf_binaural false on spatial stub path");

    expectTrue(conv_preflight.can_convolve(), "valid IR composite preflight can convolve");
    expectTrue(!conv_preflight.can_narrow_spatial_image(),
               "unity attenuation skips narrowing in composite preflight");
    expectTrue(conv_preflight.attenuationCoupling.unityAttenuation,
               "composite preflight marks unity attenuation");

    expectTrue(bypass_preflight.is_bypass(), "disabled HRTF composite preflight is bypass");
    expectTrue(!fuse::audio::can_apply_hrtf_binaural_pan(bypass_preflight),
               "bypass composite preflight cannot spatial-pan");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.panPath.coLocated,
               "composite preflight marks co-located source");
    expectTrue(co_located_preflight.is_bypass(), "co-located composite preflight bypasses");

    const fuse::audio::HrtfBinauralPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.25f, 0.35f);
    expectTrue(no_ir_preflight.panPath.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR composite overload selects ILD/ITD stub");
    expectTrue(no_ir_preflight.path()
               "composite path matches resolve_hrtf_pan_path");

    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(stub_preflight, offset);
    expectNear(from_preflight.left, coupled.left, 1e-5f,
               "from_preflight matches coupled helper on stub path");
    expectNear(from_preflight.right, coupled.right, 1e-5f,

    const fuse::audio::BinauralPanGains bypass_gains =
        fuse::audio::compute_binaural_pan_gains_from_preflight(bypass_preflight, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass_gains),
               "from_preflight returns centre pan on bypass");

    const fuse::audio::BinauralPanGains unity_gains =
        fuse::audio::compute_binaural_pan_gains_from_preflight(conv_preflight, offset);
    expectNear(unity_gains.left, wide.left, 1e-5f,
               "unity composite preflight preserves lateral gains");
    expectNear(unity_gains.right, wide.right, 1e-5f,

    const fuse::audio::BinauralPanGains vec3_coupled =
    const fuse::audio::HrtfBinauralPreflight vec3_preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.3f, 0.4f);
    const fuse::audio::BinauralPanGains vec3_from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(vec3_preflight, offset);
    expectNear(vec3_coupled.left, vec3_from_preflight.left, 1e-5f,
               "Vec3 coupled helper routes through composite preflight");
    expectNear(vec3_coupled.right, vec3_from_preflight.right, 1e-5f,

void testHrtfIrRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty)
                   == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "canonical empty IR reports ZeroLength reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::ZeroLength),
               "empty IR rejects for ZeroLength");

    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid,
                                                        fuse::audio::HrtfIrRejectReason::NullSamples),
               "valid IR does not reject for NullSamples");

    expectTrue(fuse::audio::hrtf_ir_reject_reason(zero_length)
               "zero-length factory IR reports ZeroLength reject reason");

    expectTrue(fuse::audio::hrtf_ir_reject_reason(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR reports MalformedIr reject reason");

    expectTrue(fuse::audio::hrtf_ir_reject_reason(null_samples_nonzero_length)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "null samples with non-zero length reports NullSamples reject reason");

    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "IR preflight carries ZeroLength reject reason");
    expectTrue(empty_preflight.reason
                   == fuse::audio::hrtf_ir_reject_reason(empty),
               "IR preflight reason matches diagnose helper");

void testHrtfIrRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::None),
                           "None")
                   == 0,
               "None IR reject reason name");
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "MalformedIr")
               "MalformedIr reject reason name");

void testHrtfPanPathRejectReasonGuards() {

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF reports HrtfDisabled reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects for HrtfDisabled");

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source reports CoLocated reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");

        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.reason
               "pan-path preflight carries HrtfDisabled reject reason");
                   == fuse::audio::hrtf_pan_path_reject_reason(false, offset),
               "pan-path preflight reason matches diagnose helper");

void testHrtfPanPathRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
               "CoLocated pan-path reject reason name");

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path reports BypassPath coupling reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects for BypassPath");

                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation reject reason");
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation on spatial path has no coupling reject reason");

    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries None reject reason");
    expectTrue(narrow_preflight.reason
                   == fuse::audio::hrtf_attenuation_coupling_reject_reason(
               "coupling preflight reason matches diagnose helper");

void testHrtfAttenuationCouplingRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation")
               "UnityAttenuation coupling reject reason name");

void testHrtfBinauralRejectReasonGuards() {

    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub path has no primary pan reject reason");
    expectTrue(stub_preflight.convolutionReason
                   == fuse::audio::HrtfBinauralRejectReason::ZeroLength,
               "empty IR composite preflight carries ZeroLength convolution reason");
    expectTrue(stub_preflight.narrowingReason == fuse::audio::HrtfBinauralRejectReason::None,
               "reduced attenuation composite preflight has no narrowing reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_convolution_reason(
                   stub_preflight, fuse::audio::HrtfBinauralRejectReason::ZeroLength),
               "composite rejects for ZeroLength convolution reason");
    expectTrue(!fuse::audio::hrtf_binaural_rejects_for_reason(
                   stub_preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "enabled stub path does not reject for HrtfDisabled");

    expectTrue(conv_preflight.convolutionReason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite preflight has no convolution reject reason");
    expectTrue(conv_preflight.narrowingReason
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite preflight carries UnityAttenuation narrowing reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_narrowing_reason(
                   conv_preflight, fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "composite rejects for UnityAttenuation narrowing reason");
    expectTrue(fuse::audio::hrtf_binaural_convolution_reject_reason(conv_preflight)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "convolution diagnose helper matches preflight field");

    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite preflight carries HrtfDisabled primary reason");
    expectTrue(bypass_preflight.convolutionReason
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite preflight propagates HrtfDisabled to convolution reason");
    expectTrue(bypass_preflight.narrowingReason == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "disabled composite preflight carries BypassPath narrowing reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   bypass_preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "composite rejects for HrtfDisabled primary reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(bypass_preflight)
                   == bypass_preflight.reason,
               "composite diagnose helper matches preflight primary reason");

    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight carries CoLocated primary reason");
                   co_located_preflight, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "composite rejects for CoLocated primary reason");

    const fuse::audio::HrtfBinauralPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_binaural(true, malformed, offset, 1.f, 1.f);
    expectTrue(malformed_preflight.convolutionReason
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite preflight carries MalformedIr convolution reason");
                   malformed_preflight, fuse::audio::HrtfBinauralRejectReason::MalformedIr),
               "composite rejects for MalformedIr convolution reason");

void testHrtfBinauralRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::BypassPath),
                           "BypassPath")
               "BypassPath composite reject reason name");
                               fuse::audio::HrtfBinauralRejectReason::MalformedIr),
               "MalformedIr composite reject reason name");
    const fuse::audio::BinauralPanGains conv =
    expectNear(conv.left, direct.left, 1e-5f,
               "for_path convolution stub matches ILD until IR wired");
    expectNear(conv.right, direct.right, 1e-5f,

void testAttenuationCouplingEarlyOut() {
    const float wide_left = wide.left;
    const float wide_right = wide.right;

    expectTrue(fuse::audio::is_fully_spatial_hrtf_blend(1.f),
               "unity spatial blend is fully spatial");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(1.f, 1.f),
               "unity distance and occlusion skip coupling");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(0.2f, 1.f),
               "attenuated distance still couples");

    fuse::audio::apply_hrtf_attenuation_coupling(wide, 1.f, 1.f);
    expectNear(wide.left, wide_left, 1e-5f, "early-out coupling preserves left gain");
    expectNear(wide.right, wide_right, 1e-5f, "early-out coupling preserves right gain");

        fuse::audio::compute_binaural_pan_gains_coupled(true, fuse::audio::Vec3{5.f, 0.f, 0.f},
    expectNear(coupled.left, wide_left, 1e-5f,
               "coupled helper early-outs when fully spatial");
    expectNear(coupled.right, wide_right, 1e-5f,

void testBinauralGainHelpers() {
    const fuse::audio::BinauralPanGains pan =
    const fuse::audio::BinauralStereoSample out =
        fuse::audio::apply_binaural_pan_gains(1.f, pan, 0.5f);
    expectNear(out.left, 0.5f * pan.left, 1e-5f, "apply_binaural_pan_gains scales left");
    expectNear(out.right, 0.5f * pan.right, 1e-5f, "apply_binaural_pan_gains scales right");

    float left = 0.1f;
    float right = 0.2f;
    fuse::audio::accumulate_binaural_pan(1.f, pan, 0.5f, left, right);
    expectNear(left, 0.1f + 0.5f * pan.left, 1e-5f, "accumulate_binaural_pan adds left");
    expectNear(right, 0.2f + 0.5f * pan.right, 1e-5f, "accumulate_binaural_pan adds right");

    fuse::audio::BinauralPanGains scaled = pan;
    const fuse::audio::BinauralPanGains copied =
        fuse::audio::scale_binaural_pan_gains_copy(pan, 0.5f);
    expectNear(scaled.left, copied.left, 1e-5f, "scale copy matches in-place scale");
    expectNear(scaled.right, copied.right, 1e-5f, "scale copy matches in-place scale");
    expectNear(scaled.itd_seconds, pan.itd_seconds, 1e-5f, "scale preserves ITD");
}

void testCoLocatedHrtfGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectNear(fuse::audio::hrtf_co_located_epsilon(), 1e-5f, 1e-8f,
               "co-located epsilon is exposed");
    expectTrue(fuse::audio::is_co_located_hrtf_source(co_located),
               "zero offset is co-located");
    expectTrue(!fuse::audio::is_co_located_hrtf_source(offset),
               "offset source is not co-located");
    expectTrue(fuse::audio::should_bypass_hrtf_pan(true, co_located),
               "co-located source bypasses pan");
    expectTrue(!fuse::audio::should_bypass_hrtf_pan(true, offset),
               "offset source does not bypass pan");
}

void testEmptyIrConvolutionSkipGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::should_skip_hrtf_convolution(empty),
               "empty IR skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(empty)
                   == fuse::audio::is_empty_hrtf_ir(empty),
               "skip convolution matches is_empty");

    const float samples[] = {0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(!fuse::audio::should_skip_hrtf_convolution(valid),
               "valid IR does not skip convolution");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(fuse::audio::is_nonnull_zero_length_hrtf_ir(zero_length),
               "non-null zero-length IR is recognised");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(zero_length),
               "non-null zero-length IR skips convolution");
}

void testAttenuationCouplingSkipGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_attenuation(1.f, 1.f),
               "unity distance and occlusion");
    expectTrue(!fuse::audio::is_unity_hrtf_attenuation(0.5f, 1.f),
               "partial distance is not unity attenuation");

    expectTrue(
        fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),
        "bypass skips attenuation coupling");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution),
               "convolution path applies attenuation coupling");
    expectTrue(fuse::audio::should_apply_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub)
                   == !fuse::audio::should_skip_hrtf_attenuation_coupling(
                          fuse::audio::HrtfPanPath::IldItdStub),
               "should_apply coupling is inverse of should_skip");

    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "spatial path with low attenuation narrows image");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f),
               "bypass path never narrows image");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f),
               "unity attenuation skips narrowing");
}

void testSpatialBlendSkipGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(1.f), "unity blend at one");
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(0.99999f), "near-unity blend");
    expectTrue(!fuse::audio::is_unity_hrtf_spatial_blend(0.5f), "partial blend is not unity");

    expectTrue(fuse::audio::should_skip_hrtf_spatial_blend(1.f, 1.f),
               "full attenuation skips spatial narrowing");
    expectTrue(!fuse::audio::should_skip_hrtf_spatial_blend(0.f, 0.f),
               "zero attenuation requires spatial narrowing");

    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    const float wide_spread = fuse::audio::compute_pan_spread(wide);

    fuse::audio::BinauralPanGains guarded = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(guarded, 1.f);
    expectNear(guarded.left, wide.left, 1e-5f, "guarded unity blend preserves left");
    expectNear(guarded.right, wide.right, 1e-5f, "guarded unity blend preserves right");

    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(narrowed, 0.25f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed) < wide_spread,
               "guarded partial blend narrows image");
}

void testClampHrtfOcclusionCouplingWeight() {
    expectNear(fuse::audio::clamp_hrtf_occlusion_coupling_weight(-0.2f), 0.f, 1e-5f,
               "negative coupling weight clamps to zero");
    expectNear(fuse::audio::clamp_hrtf_occlusion_coupling_weight(1.5f), 1.f, 1e-5f,
               "above-unity coupling weight clamps to one");
    expectNear(fuse::audio::clamp_hrtf_occlusion_coupling_weight(0.6f), 0.6f, 1e-5f,
               "in-range coupling weight is preserved");
}

void testHrtfIrPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.is_empty, "empty IR preflight marks empty");
    expectTrue(!empty_preflight.has_samples, "empty IR preflight has no samples");
    expectTrue(!empty_preflight.length_ok, "empty IR preflight length not ok");
    expectTrue(empty_preflight.should_fallback_to_ild_itd(), "empty IR falls back to ILD/ITD");
    expectTrue(!empty_preflight.can_use_convolution(), "empty IR cannot use convolution");
    expectTrue(empty_preflight.ready_for_stub(), "empty IR preflight is stub-ready");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid =
        fuse::audio::make_hrtf_ir_stub(samples, static_cast<fuse::u32>(2));
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.is_empty, "valid IR preflight is not empty");
    expectTrue(valid_preflight.has_samples, "valid IR preflight has samples");
    expectTrue(valid_preflight.length_ok, "valid IR preflight length ok");
    expectTrue(valid_preflight.can_use_convolution(), "valid IR can use convolution");
    expectTrue(!valid_preflight.should_fallback_to_ild_itd(), "valid IR does not fallback");
    expectTrue(valid_preflight.ready_for_stub(), "valid IR preflight is stub-ready");
    expectTrue(valid_preflight.ir_length == 2u, "valid IR preflight carries length");

    const fuse::audio::HrtfIrStub null_length{samples, 0};
    const fuse::audio::HrtfIrPreflight zero_length = fuse::audio::preflight_hrtf_ir(null_length);
    expectTrue(zero_length.is_empty, "zero-length IR preflight is empty");
    expectTrue(zero_length.has_samples, "zero-length IR still reports sample pointer");
    expectTrue(!zero_length.length_ok, "zero-length IR fails length check");
}

void testHrtfPanPathPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();

    const fuse::audio::HrtfPanPathPreflight convolution =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(convolution.can_apply_spatial_pan(), "valid IR offset can apply spatial pan");
    expectTrue(convolution.uses_convolution, "valid IR selects convolution path");
    expectTrue(!convolution.skip_spatial_pan, "convolution path does not skip spatial pan");
    expectTrue(convolution.ready_for_stub(), "convolution preflight is stub-ready");
    expectTrue(convolution.path == fuse::audio::HrtfPanPath::Convolution,
               "preflight path matches resolve_hrtf_pan_path");

    const fuse::audio::HrtfPanPathPreflight ild_stub =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(ild_stub.can_apply_spatial_pan(), "empty IR offset can apply ILD/ITD stub pan");
    expectTrue(ild_stub.uses_ild_itd_stub, "empty IR selects ILD/ITD stub");
    expectTrue(!ild_stub.uses_convolution, "empty IR does not select convolution");
    expectTrue(ild_stub.ir.should_fallback_to_ild_itd(), "pan preflight IR falls back");

    const fuse::audio::HrtfPanPathPreflight bypass =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass.should_bypass(), "disabled HRTF bypasses pan");
    expectTrue(bypass.skip_spatial_pan, "disabled HRTF skips spatial pan");
    expectTrue(!bypass.can_apply_spatial_pan(), "disabled HRTF cannot apply spatial pan");
    expectTrue(bypass.ready_for_stub(), "bypass preflight is stub-ready");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.co_located, "zero offset is co-located");
    expectTrue(co_located_preflight.should_bypass(), "co-located source bypasses pan");
    expectTrue(co_located_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "co-located preflight selects bypass");

    const fuse::audio::HrtfPanPathPreflight no_ir =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir.uses_ild_itd_stub, "no-IR overload selects ILD/ITD stub");
    expectTrue(no_ir.ir.is_empty, "no-IR overload reports empty IR preflight");
}

void testHrtfAttenuationCouplingPreflight() {
    const fuse::audio::HrtfAttenuationCouplingPreflight spatial =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 0.2f,
                                                         0.3f);
    expectTrue(!spatial.skip_coupling, "spatial path does not skip coupling preflight");
    expectTrue(spatial.should_narrow, "reduced attenuation should narrow");
    expectTrue(spatial.can_apply_coupling(), "spatial path can apply coupling");
    expectTrue(spatial.ready_for_stub(), "spatial coupling preflight is stub-ready");
    expectTrue(spatial.spatial_blend < 1.f, "reduced attenuation lowers spatial blend");
    expectNear(spatial.distance_attenuation, 0.2f, 1e-5f,
               "preflight clamps distance attenuation in range");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass.skip_coupling, "bypass path skips coupling preflight");
    expectTrue(!bypass.can_apply_coupling(), "bypass path cannot apply coupling");
    expectTrue(bypass.ready_for_stub(), "bypass coupling preflight is stub-ready");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity.unity_attenuation, "unity attenuation flagged");
    expectTrue(!unity.should_narrow, "unity attenuation does not narrow");
    expectTrue(!unity.can_apply_coupling(), "unity attenuation skips coupling application");
    expectTrue(unity.ready_for_stub(), "unity coupling preflight is stub-ready");

    const fuse::audio::HrtfAttenuationCoupling distance_only{.occlusion_weight = 0.f};
    const fuse::audio::HrtfAttenuationCouplingPreflight distance_blend =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 0.2f,
                                                         0.8f, distance_only);
    expectNear(distance_blend.spatial_blend, 0.4f, 1e-5f,
               "preflight spatial blend respects distance-only weight");
}

void testHrtfSpatialPanPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();

    const fuse::audio::HrtfSpatialPanPreflight active =
        fuse::audio::preflight_hrtf_spatial_pan(true, empty, offset, 0.25f, 0.35f);
    expectTrue(active.can_apply_spatial_pan(), "active spatial pan preflight can pan");
    expectTrue(active.can_apply_attenuation_coupling(), "active spatial pan can couple");
    expectTrue(active.ready_for_stub(), "active spatial pan preflight is stub-ready");
    expectTrue(active.pan.uses_ild_itd_stub, "combined preflight carries pan path");
    expectTrue(active.coupling.should_narrow, "combined preflight carries coupling state");

    const fuse::audio::HrtfSpatialPanPreflight bypass =
        fuse::audio::preflight_hrtf_spatial_pan(false, empty, offset, 0.1f, 0.1f);
    expectTrue(!bypass.can_apply_spatial_pan(), "disabled HRTF cannot apply spatial pan");
    expectTrue(!bypass.can_apply_attenuation_coupling(), "disabled HRTF skips coupling");
    expectTrue(bypass.ready_for_stub(), "disabled spatial pan preflight is stub-ready");

    const fuse::audio::HrtfSpatialPanPreflight unity =
        fuse::audio::preflight_hrtf_spatial_pan(true, empty, offset, 1.f, 1.f);
    expectTrue(unity.can_apply_spatial_pan(), "unity spatial pan still applies pan");
    expectTrue(!unity.can_apply_attenuation_coupling(), "unity attenuation skips coupling");
    expectTrue(unity.ready_for_stub(), "unity spatial pan preflight is stub-ready");
}

void testHrtfIrPreflightGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.empty_ir, "preflight marks empty IR");
    expectTrue(empty_preflight.null_samples, "preflight marks null samples on empty IR");
    expectTrue(empty_preflight.zero_length, "preflight marks zero length on empty IR");
    expectTrue(empty_preflight.should_fallback_to_stub(), "empty IR preflight falls back to stub");
    expectTrue(!empty_preflight.can_use_convolution(), "empty IR preflight cannot use convolution");
    expectTrue(!fuse::audio::can_use_hrtf_convolution(empty),
               "can_use_hrtf_convolution mirrors preflight on empty IR");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid =
        fuse::audio::make_hrtf_ir_stub(samples, static_cast<fuse::u32>(2));
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.empty_ir, "preflight marks valid IR as non-empty");
    expectTrue(!valid_preflight.null_samples, "preflight clears null_samples on valid IR");
    expectTrue(!valid_preflight.zero_length, "preflight clears zero_length on valid IR");
    expectTrue(valid_preflight.length == 2u, "preflight reports IR length");
    expectTrue(valid_preflight.can_use_convolution(), "valid IR preflight can use convolution");
    expectTrue(fuse::audio::can_use_hrtf_convolution(valid),
               "can_use_hrtf_convolution mirrors preflight on valid IR");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight = fuse::audio::preflight_hrtf_ir(null_samples);
    expectTrue(null_preflight.empty_ir, "preflight treats null samples as empty");
    expectTrue(null_preflight.null_samples, "preflight marks null samples");
    expectTrue(!null_preflight.zero_length, "preflight sees non-zero length with null samples");
}

void testHrtfPanPathPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.path == fuse::audio::HrtfPanPath::IldItdStub,
               "preflight selects ILD/ITD stub for empty IR");
    expectTrue(stub_preflight.empty_ir, "preflight marks empty IR on stub path");
    expectTrue(stub_preflight.spatial, "stub path preflight is spatial");
    expectTrue(stub_preflight.uses_ild_itd_stub, "stub path preflight uses ILD/ITD stub");
    expectTrue(!stub_preflight.uses_convolution, "stub path preflight does not use convolution");
    expectTrue(stub_preflight.can_apply_spatial_pan(), "stub path can apply spatial pan");
    expectTrue(!stub_preflight.should_skip_spatial_pan(), "stub path does not skip spatial pan");

    const fuse::audio::HrtfPanPathPreflight convolution_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(convolution_preflight.path == fuse::audio::HrtfPanPath::Convolution,
               "preflight selects convolution for valid IR");
    expectTrue(!convolution_preflight.empty_ir, "convolution preflight marks non-empty IR");
    expectTrue(convolution_preflight.uses_convolution, "convolution preflight uses IR path");
    expectTrue(fuse::audio::can_apply_hrtf_spatial_pan(true, valid, offset),
               "can_apply_hrtf_spatial_pan accepts valid IR offset source");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "preflight bypasses when HRTF disabled");
    expectTrue(bypass_preflight.hrtf_disabled, "preflight marks disabled HRTF");
    expectTrue(bypass_preflight.bypass, "disabled preflight is bypass");
    expectTrue(bypass_preflight.should_skip_spatial_pan(), "disabled preflight skips spatial pan");
    expectTrue(!bypass_preflight.can_apply_spatial_pan(),
               "disabled preflight cannot apply spatial pan");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.co_located, "preflight marks co-located source");
    expectTrue(co_located_preflight.bypass, "co-located preflight bypasses pan");
    expectTrue(!fuse::audio::can_apply_hrtf_spatial_pan(true, valid, co_located),
               "can_apply_hrtf_spatial_pan rejects co-located source");

    const fuse::audio::HrtfPanPathPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir_preflight.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload preflight selects ILD/ITD stub");
    expectTrue(no_ir_preflight.empty_ir, "no-IR overload preflight marks empty IR");
}

void testHrtfAttenuationCouplingPreflightGuards() {
    const fuse::audio::HrtfAttenuationCouplingPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass_preflight.bypass_path, "coupling preflight marks bypass path");
    expectTrue(bypass_preflight.skipped, "bypass path skips attenuation coupling");
    expectTrue(!bypass_preflight.would_narrow, "bypass path does not narrow spatial image");
    expectTrue(!bypass_preflight.can_apply(), "bypass preflight cannot apply coupling");
    expectTrue(bypass_preflight.should_skip(), "bypass preflight should skip");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.unity_attenuation, "unity preflight marks full gain");
    expectTrue(unity_preflight.skipped, "unity attenuation skips coupling");
    expectTrue(!unity_preflight.would_narrow, "unity attenuation does not narrow");
    expectTrue(!fuse::audio::can_apply_hrtf_attenuation_coupling_narrowing(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f),
               "narrowing convenience rejects unity gain");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrow_preflight.bypass_path, "spatial path is not bypass");
    expectTrue(!narrow_preflight.unity_attenuation, "reduced gain is non-unity");
    expectTrue(narrow_preflight.would_narrow, "reduced gain would narrow spatial image");
    expectTrue(!narrow_preflight.skipped, "narrowing preflight is not skipped");
    expectTrue(narrow_preflight.can_apply(), "narrowing preflight can apply coupling");
    expectTrue(narrow_preflight.distance_attenuation == 0.2f,
               "preflight clamps in-range distance attenuation");
    expectTrue(narrow_preflight.occlusion_gain == 0.3f,
               "preflight clamps in-range occlusion gain");
    expectTrue(fuse::audio::can_apply_hrtf_attenuation_coupling_narrowing(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "narrowing convenience accepts spatial path with reduced gain");

    const fuse::audio::HrtfAttenuationCouplingPreflight clamped_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         -0.5f, 1.5f);
    expectTrue(clamped_preflight.distance_attenuation == 0.f,
               "preflight clamps negative distance attenuation");
    expectTrue(clamped_preflight.occlusion_gain == 1.f,
               "preflight clamps above-unity occlusion gain");
    expectTrue(clamped_preflight.would_narrow,
               "clamped non-unity distance attenuation still narrows");
}

void testPreflightHrtfIrConvolution() {
    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::EmptyIr;

    expectTrue(fuse::audio::preflight_hrtf_ir_convolution(valid, &reason),
               "valid IR passes convolution preflight");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight reason is None");
    expectTrue(fuse::audio::preflight_hrtf_ir_convolution(valid)
                   == fuse::audio::should_use_hrtf_ir(valid),
               "preflight matches should_use_hrtf_ir on valid IR");

    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(!fuse::audio::try_preflight_hrtf_ir_convolution(empty, reason),
               "empty IR fails convolution preflight");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
               "empty IR preflight reason is EmptyIr");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    expectTrue(!fuse::audio::preflight_hrtf_ir_convolution(null_samples, &reason),
               "null samples fail convolution preflight");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "null samples preflight reason is NullSamples");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(!fuse::audio::preflight_hrtf_ir_convolution(zero_length, &reason),
               "zero-length IR fails convolution preflight");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "zero-length preflight reason is ZeroLength");
    expectTrue(fuse::audio::preflight_hrtf_ir_convolution(zero_length)
                   == !fuse::audio::should_skip_hrtf_convolution(zero_length),
               "preflight inverts should_skip_hrtf_convolution");

    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "NullSamples")
                   == 0,
               "IR reject reason label is readable");
}

void testPreflightHrtfSpatialPan() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    fuse::audio::HrtfPanPath path = fuse::audio::HrtfPanPath::Bypass;
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::Disabled;

    expectTrue(fuse::audio::try_preflight_hrtf_spatial_pan(true, offset, path, reason),
               "enabled offset source passes spatial pan preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial pan preflight reason is None on success");
    expectTrue(path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR preflight resolves ILD/ITD stub path");

    expectTrue(!fuse::audio::preflight_hrtf_spatial_pan(false, offset, &path, &reason),
               "disabled HRTF fails spatial pan preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::Disabled,
               "disabled preflight reason is Disabled");
    expectTrue(path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight resolves bypass path");

    expectTrue(!fuse::audio::preflight_hrtf_spatial_pan(true, fuse::audio::Vec3{}, &path, &reason),
               "co-located source fails spatial pan preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located preflight reason is CoLocated");

    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::preflight_hrtf_spatial_pan(true, valid, offset, &path, &reason),
               "IR-aware preflight passes for valid offset source");
    expectTrue(path == fuse::audio::HrtfPanPath::Convolution,
               "IR-aware preflight resolves convolution path");
    expectTrue(fuse::audio::preflight_hrtf_spatial_pan(true, offset)
                   == fuse::audio::should_apply_hrtf_pan(true, offset),
               "Vec3 preflight matches should_apply_hrtf_pan");

    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
                   == 0,
               "pan-path reject reason label is readable");
}

void testPreflightHrtfAttenuationCoupling() {
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath;

    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, reason),
               "spatial path with reduced attenuation passes coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "coupling preflight reason is None on success");
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f)
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f),
               "preflight matches should_narrow_hrtf_spatial_image");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, &reason),
               "bypass path fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass coupling preflight reason is BypassPath");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f, &reason),
               "unity attenuation fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation preflight reason is UnityAttenuation");

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfPanPath path =
        fuse::audio::resolve_hrtf_pan_path(true, offset);
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(path, 0.15f, 0.2f)
                   == !fuse::audio::should_skip_hrtf_spatial_blend(0.15f, 0.2f),
               "coupling preflight aligns with spatial blend skip guard on valid path");

    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation")
                   == 0,
               "attenuation-coupling reject reason label is readable");
}

void testHrtfIrPreflightGuards() {
    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(valid_preflight.can_convolve(), "valid IR passes preflight");
    expectTrue(!valid_preflight.rejected, "valid IR is not rejected");
    expectTrue(valid_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(!empty_preflight.can_convolve(), "empty IR fails preflight");
    expectTrue(empty_preflight.rejected, "empty IR is rejected");
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR rejects null samples");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(zero_length)
                   == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "zero-length IR reports zero-length reject reason");
    expectTrue(fuse::audio::is_malformed_hrtf_ir(zero_length),
               "zero-length IR is malformed");
    expectTrue(fuse::audio::should_fallback_to_ild_itd_stub(zero_length),
               "zero-length IR falls back to ILD/ITD stub");
    expectTrue(fuse::audio::should_fallback_to_ild_itd_stub(zero_length)
                   == fuse::audio::should_skip_hrtf_convolution(zero_length),
               "fallback alias matches convolution skip");
}

void testHrtfPanPathPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight spatial =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(spatial.can_spatialize(), "enabled offset source can spatialize");
    expectTrue(!spatial.skipped, "enabled offset source is not skipped");
    expectTrue(spatial.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR selects convolution in preflight");

    const fuse::audio::HrtfPanPathPreflight disabled =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(!disabled.can_spatialize(), "disabled HRTF cannot spatialize");
    expectTrue(disabled.skipped, "disabled HRTF is skipped");
    expectTrue(disabled.skip_reason == fuse::audio::HrtfPanPathSkipReason::Disabled,
               "disabled HRTF reports disabled skip reason");

    const fuse::audio::HrtfPanPathPreflight co_located =
        fuse::audio::preflight_hrtf_pan_path(true, valid, fuse::audio::Vec3{});
    expectTrue(!co_located.can_spatialize(), "co-located source cannot spatialize");
    expectTrue(co_located.skip_reason == fuse::audio::HrtfPanPathSkipReason::CoLocated,
               "co-located source reports co-located skip reason");

    const fuse::audio::HrtfPanPathPreflight no_ir =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR preflight selects ILD/ITD stub");
    expectTrue(fuse::audio::should_fallback_to_ild_itd_pan_path(no_ir.path),
               "ILD/ITD stub path uses fallback predicate");
    expectTrue(!fuse::audio::should_fallback_to_ild_itd_pan_path(spatial.path),
               "convolution path does not use ILD/ITD fallback predicate");
}

void testHrtfAttenuationCouplingPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(!bypass.can_apply(), "bypass path skips attenuation coupling");
    expectTrue(bypass.skipped, "bypass preflight is skipped");
    expectTrue(bypass.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath,
               "bypass preflight reports bypass reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 1.f,
                                                         1.f);
    expectTrue(!unity.can_apply(), "unity attenuation skips coupling");
    expectTrue(unity.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity preflight reports unity reason");
    expectNear(unity.spatial_blend, 1.f, 1e-5f, "unity preflight exposes full spatial blend");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         0.2f, 0.3f);
    expectTrue(narrow.can_apply(), "reduced attenuation passes coupling preflight");
    expectTrue(narrow.spatial_blend < 1.f, "narrowing preflight reports sub-unity blend");

    fuse::audio::BinauralPanGains via_preflight = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        via_preflight, fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f);
    fuse::audio::BinauralPanGains manual = wide;
    fuse::audio::apply_hrtf_attenuation_coupling(manual, 0.2f, 0.3f);
    expectNear(via_preflight.left, manual.left, 1e-5f,
               "preflight-wired for_path coupling matches manual coupling");
    expectNear(via_preflight.right, manual.right, 1e-5f,
               "preflight-wired for_path coupling matches manual coupling");

    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_apply(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f),
               "skip apply alias rejects bypass path");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_apply(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f),
               "skip apply alias accepts spatial narrowing");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_apply(
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f),
               "skip apply alias rejects unity attenuation");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_apply(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == !fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "skip apply alias matches should_narrow guard");
}

void testHrtfIrPreflightGuards() {
    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};

    fuse::audio::HrtfIrSkipReason reason = fuse::audio::HrtfIrSkipReason::NullSamples;
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &reason),
               "valid IR passes preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::None,
               "valid IR preflight reason is None");

    const fuse::audio::HrtfIrPreflight valid_preflight =
        fuse::audio::preflight_hrtf_ir_stub(valid);
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(!valid_preflight.skipped, "valid IR preflight is not skipped");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    expectTrue(!fuse::audio::preflight_hrtf_ir(null_samples, &reason),
               "null samples fail preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::NullSamples,
               "null samples report NullSamples reason");
    expectTrue(fuse::audio::classify_hrtf_ir_skip(null_samples)
                   == fuse::audio::HrtfIrSkipReason::NullSamples,
               "classify matches null samples");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(!fuse::audio::preflight_hrtf_ir(zero_length, &reason),
               "zero length fails preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::ZeroLength,
               "zero length reports ZeroLength reason");
    expectTrue(fuse::audio::hrtf_ir_skip_reason_is_blocking(reason),
               "ZeroLength is blocking");

    const fuse::audio::HrtfIrPreflight empty_preflight =
        fuse::audio::preflight_hrtf_ir_stub(fuse::audio::make_empty_hrtf_ir());
    expectTrue(empty_preflight.skipped, "empty IR preflight is skipped");
    expectTrue(!empty_preflight.can_convolve(), "empty IR preflight cannot convolve");
}

void testHrtfPanPathPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    fuse::audio::HrtfPanPathSkipReason reason = fuse::audio::HrtfPanPathSkipReason::Disabled;
    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, valid, offset, &reason),
               "enabled offset passes pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::None,
               "enabled offset preflight reason is None");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(false, valid, offset, &reason),
               "disabled HRTF fails pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::Disabled,
               "disabled HRTF reports Disabled reason");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(true, valid, fuse::audio::Vec3{}, &reason),
               "co-located source fails pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::CoLocated,
               "co-located source reports CoLocated reason");
    expectTrue(fuse::audio::hrtf_pan_path_skip_reason_is_blocking(reason),
               "CoLocated is blocking");

    const fuse::audio::HrtfPanPathPreflight spatial =
        fuse::audio::preflight_hrtf_pan_path_guarded(true, valid, offset);
    expectTrue(spatial.can_spatial_pan(), "spatial preflight can pan");
    expectTrue(spatial.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR resolves to convolution in preflight");

    const fuse::audio::HrtfPanPathPreflight bypass =
        fuse::audio::preflight_hrtf_pan_path_guarded(false, valid, offset);
    expectTrue(bypass.skipped, "disabled preflight is skipped");
    expectTrue(bypass.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight resolves to bypass");
    expectTrue(!bypass.can_spatial_pan(), "disabled preflight cannot spatial pan");

    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub alias predicate");
    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub)
                   == fuse::audio::hrtf_pan_path_uses_ild_itd_stub(
                          fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub alias matches ild_itd_stub predicate");
}

void testHrtfAttenuationCouplingPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfPanPath stub_path =
        fuse::audio::resolve_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), offset);

    fuse::audio::HrtfAttenuationCouplingSkipReason reason =
        fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath;
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 0.2f, 0.3f, &reason),
               "reduced attenuation passes coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "reduced attenuation preflight reason is None");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f, &reason),
               "bypass path fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath,
               "bypass path reports BypassPath reason");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 1.f, 1.f, &reason),
               "unity attenuation fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
        fuse::audio::preflight_hrtf_attenuation_coupling_for_path(stub_path, 0.15f, 0.25f);
    expectTrue(narrow.can_narrow(), "reduced attenuation preflight can narrow");
    expectTrue(!narrow.skipped, "reduced attenuation preflight is not skipped");
    expectTrue(narrow.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "reduced attenuation struct reason is None");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling_for_path(stub_path, 1.f, 1.f);
    expectTrue(unity.skipped, "unity attenuation preflight is skipped");
    expectTrue(!unity.can_narrow(), "unity attenuation preflight cannot narrow");
    expectTrue(unity.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity struct reason is UnityAttenuation");

    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_skip(stub_path, 0.2f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "distance-only reduction classifies as narrowable");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_skip_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath),
               "BypassPath is blocking");
    expectTrue(!fuse::audio::hrtf_attenuation_coupling_skip_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingSkipReason::None),
               "None is not blocking");

    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image(stub_path, 0.2f, 0.3f)
                   == fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 0.2f, 0.3f),
               "should_narrow matches coupling preflight on valid path");
}

void testHrtfIrPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.empty_ir, "preflight marks empty IR");
    expectTrue(empty_preflight.null_samples, "preflight marks null samples");
    expectTrue(empty_preflight.zero_length, "preflight marks zero length");
    expectTrue(!empty_preflight.malformed, "factory empty IR is not malformed");
    expectTrue(!empty_preflight.can_convolve(), "empty IR cannot convolve");
    expectTrue(empty_preflight.should_skip(), "empty IR preflight skips convolution");
    expectTrue(!fuse::audio::can_convolve_hrtf_ir(empty),
               "can_convolve_hrtf_ir rejects empty IR");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.empty_ir, "valid IR is not empty");
    expectTrue(!valid_preflight.null_samples, "valid IR has samples");
    expectTrue(!valid_preflight.zero_length, "valid IR has non-zero length");
    expectTrue(valid_preflight.can_convolve(), "valid IR can convolve");
    expectTrue(fuse::audio::can_convolve_hrtf_ir(valid),
               "can_convolve_hrtf_ir accepts valid IR");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.malformed, "preflight marks malformed IR");
    expectTrue(malformed_preflight.empty_ir, "malformed IR is treated as empty");
    expectTrue(malformed_preflight.should_skip(), "malformed IR skips convolution");

    const fuse::audio::HrtfIrStub null_nonzero{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight =
        fuse::audio::preflight_hrtf_ir(null_nonzero);
    expectTrue(null_preflight.null_samples, "null samples flagged");
    expectTrue(null_preflight.empty_ir, "null samples with length still empty");
}

void testHrtfPanPathPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(!stub_preflight.hrtf_disabled, "enabled HRTF is not disabled");
    expectTrue(!stub_preflight.co_located, "offset source is not co-located");
    expectTrue(stub_preflight.empty_ir, "empty IR flagged in pan preflight");
    expectTrue(stub_preflight.uses_ild_itd_stub(), "empty IR selects ILD/ITD stub");
    expectTrue(stub_preflight.can_apply_spatial_pan(), "ILD/ITD stub is spatial");
    expectTrue(fuse::audio::can_apply_hrtf_pan_path(true, empty, offset),
               "can_apply_hrtf_pan_path accepts ILD/ITD stub path");

    const fuse::audio::HrtfPanPathPreflight convolution_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(!convolution_preflight.empty_ir, "valid IR is not empty in pan preflight");
    expectTrue(convolution_preflight.uses_convolution(), "valid IR selects convolution path");
    expectTrue(convolution_preflight.can_apply_spatial_pan(),
               "convolution path applies spatial pan");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(disabled_preflight.hrtf_disabled, "disabled HRTF flagged");
    expectTrue(disabled_preflight.should_skip(), "disabled HRTF bypasses pan");
    expectTrue(!fuse::audio::can_apply_hrtf_pan_path(false, valid, offset),
               "can_apply_hrtf_pan_path rejects disabled HRTF");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.co_located, "co-located source flagged");
    expectTrue(co_located_preflight.should_skip(), "co-located source bypasses pan");

    const fuse::audio::HrtfPanPathPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir_preflight.empty_ir, "no-IR overload treats IR as empty");
    expectTrue(no_ir_preflight.uses_ild_itd_stub(), "no-IR overload selects ILD/ITD stub");
    expectTrue(no_ir_preflight.path == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "preflight path matches resolve_hrtf_pan_path");
}

void testHrtfAttenuationCouplingPreflight() {
    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass.bypass_path, "bypass path flagged");
    expectTrue(bypass.skipped, "bypass path skips coupling");
    expectTrue(!bypass.can_narrow(), "bypass path cannot narrow");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution, 1.f,
                                                         1.f);
    expectTrue(unity.unity_attenuation, "unity attenuation flagged");
    expectTrue(unity.skipped, "unity attenuation skips coupling");
    expectTrue(!unity.can_narrow(), "unity attenuation cannot narrow");
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(unity.spatial_blend),
               "unity attenuation yields unity spatial blend");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                          0.2f, 0.3f);
    expectTrue(!narrowed.bypass_path, "spatial path is not bypass");
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation is non-unity");
    expectTrue(narrowed.can_narrow(), "reduced attenuation can narrow");
    expectTrue(!narrowed.should_skip(), "reduced attenuation does not skip coupling");
    expectTrue(fuse::audio::can_narrow_hrtf_spatial_image_preflight(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "can_narrow preflight alias accepts narrowing case");
    expectTrue(fuse::audio::can_narrow_hrtf_spatial_image_preflight(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f)
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f),
               "preflight alias matches should_narrow on bypass");
    expectTrue(fuse::audio::can_narrow_hrtf_spatial_image_preflight(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f)
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f),
               "preflight alias matches should_narrow on spatial path");
}

void testHrtfGuardedPanPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfGuardedPanPreflight stub =
        fuse::audio::preflight_hrtf_guarded_pan(true, empty, offset, 0.25f, 0.4f);
    expectTrue(stub.ir.empty_ir, "bundled preflight carries empty IR state");
    expectTrue(stub.pan_path.uses_ild_itd_stub(), "bundled preflight carries ILD/ITD path");
    expectTrue(stub.can_compute_spatial_pan(), "ILD/ITD stub can compute spatial pan");
    expectTrue(stub.coupling.can_narrow(), "bundled coupling can narrow under attenuation");
    expectTrue(!stub.should_skip_coupling(), "non-unity attenuation does not skip coupling");

    const fuse::audio::HrtfGuardedPanPreflight bypass =
        fuse::audio::preflight_hrtf_guarded_pan(false, valid, offset, 0.1f, 0.1f);
    expectTrue(!bypass.can_compute_spatial_pan(), "disabled HRTF cannot compute spatial pan");
    expectTrue(bypass.should_skip_coupling(), "bypass path skips coupling");
    expectTrue(bypass.pan_path.hrtf_disabled, "bundled pan preflight marks disabled HRTF");

    const fuse::audio::HrtfGuardedPanPreflight unity =
        fuse::audio::preflight_hrtf_guarded_pan(true, valid, offset, 1.f, 1.f);
    expectTrue(unity.pan_path.uses_convolution(), "valid IR selects convolution in bundle");
    expectTrue(unity.should_skip_coupling(), "unity attenuation skips bundled coupling");
}

void testHrtfIrPreflightGuards() {
    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};

    fuse::audio::HrtfIrSkipReason reason = fuse::audio::HrtfIrSkipReason::NullSamples;
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &reason),
               "valid IR passes preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::None,
               "valid IR preflight reason is None");

    const fuse::audio::HrtfIrPreflight valid_preflight =
        fuse::audio::preflight_hrtf_ir_stub(valid);
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(!valid_preflight.skipped, "valid IR preflight is not skipped");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    expectTrue(!fuse::audio::preflight_hrtf_ir(null_samples, &reason),
               "null samples fail preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::NullSamples,
               "null samples report NullSamples reason");
    expectTrue(fuse::audio::classify_hrtf_ir_skip(null_samples)
                   == fuse::audio::HrtfIrSkipReason::NullSamples,
               "classify matches null samples");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(!fuse::audio::preflight_hrtf_ir(zero_length, &reason),
               "zero length fails preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::ZeroLength,
               "zero length reports ZeroLength reason");
    expectTrue(fuse::audio::hrtf_ir_skip_reason_is_blocking(reason),
               "ZeroLength is blocking");

    const fuse::audio::HrtfIrPreflight empty_preflight =
        fuse::audio::preflight_hrtf_ir_stub(fuse::audio::make_empty_hrtf_ir());
    expectTrue(empty_preflight.skipped, "empty IR preflight is skipped");
    expectTrue(!empty_preflight.can_convolve(), "empty IR preflight cannot convolve");
}

void testHrtfPanPathPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    fuse::audio::HrtfPanPathSkipReason reason = fuse::audio::HrtfPanPathSkipReason::Disabled;
    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, valid, offset, &reason),
               "enabled offset passes pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::None,
               "enabled offset preflight reason is None");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(false, valid, offset, &reason),
               "disabled HRTF fails pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::Disabled,
               "disabled HRTF reports Disabled reason");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(true, valid, fuse::audio::Vec3{}, &reason),
               "co-located source fails pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::CoLocated,
               "co-located source reports CoLocated reason");
    expectTrue(fuse::audio::hrtf_pan_path_skip_reason_is_blocking(reason),
               "CoLocated is blocking");

    const fuse::audio::HrtfPanPathPreflight spatial =
        fuse::audio::preflight_hrtf_pan_path_guarded(true, valid, offset);
    expectTrue(spatial.can_spatial_pan(), "spatial preflight can pan");
    expectTrue(spatial.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR resolves to convolution in preflight");

    const fuse::audio::HrtfPanPathPreflight bypass =
        fuse::audio::preflight_hrtf_pan_path_guarded(false, valid, offset);
    expectTrue(bypass.skipped, "disabled preflight is skipped");
    expectTrue(bypass.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight resolves to bypass");
    expectTrue(!bypass.can_spatial_pan(), "disabled preflight cannot spatial pan");

    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub alias predicate");
    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub)
                   == fuse::audio::hrtf_pan_path_uses_ild_itd_stub(
                          fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub alias matches ild_itd_stub predicate");
}

void testHrtfAttenuationCouplingPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfPanPath stub_path =
        fuse::audio::resolve_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), offset);

    fuse::audio::HrtfAttenuationCouplingSkipReason reason =
        fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath;
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 0.2f, 0.3f, &reason),
               "reduced attenuation passes coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "reduced attenuation preflight reason is None");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f, &reason),
               "bypass path fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath,
               "bypass path reports BypassPath reason");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 1.f, 1.f, &reason),
               "unity attenuation fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
        fuse::audio::preflight_hrtf_attenuation_coupling_for_path(stub_path, 0.15f, 0.25f);
    expectTrue(narrow.can_narrow(), "reduced attenuation preflight can narrow");
    expectTrue(!narrow.skipped, "reduced attenuation preflight is not skipped");
    expectTrue(narrow.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "reduced attenuation struct reason is None");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling_for_path(stub_path, 1.f, 1.f);
    expectTrue(unity.skipped, "unity attenuation preflight is skipped");
    expectTrue(!unity.can_narrow(), "unity attenuation preflight cannot narrow");
    expectTrue(unity.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity struct reason is UnityAttenuation");

    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_skip(stub_path, 0.2f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "distance-only reduction classifies as narrowable");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_skip_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath),
               "BypassPath is blocking");
    expectTrue(!fuse::audio::hrtf_attenuation_coupling_skip_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingSkipReason::None),
               "None is not blocking");

    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image(stub_path, 0.2f, 0.3f)
                   == fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 0.2f, 0.3f),
               "should_narrow matches coupling preflight on valid path");
}

void testHrtfIrPreflightGuards() {
    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};

    fuse::audio::HrtfIrSkipReason reason = fuse::audio::HrtfIrSkipReason::NullSamples;
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &reason),
               "valid IR passes preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::None,
               "valid IR preflight reason is None");

    const fuse::audio::HrtfIrPreflight valid_preflight =
        fuse::audio::preflight_hrtf_ir_stub(valid);
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(!valid_preflight.skipped, "valid IR preflight is not skipped");

    const fuse::audio::HrtfIrStub null_samples{nullptr, 4};
    expectTrue(!fuse::audio::preflight_hrtf_ir(null_samples, &reason),
               "null samples fail preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::NullSamples,
               "null samples report NullSamples reason");
    expectTrue(fuse::audio::classify_hrtf_ir_skip(null_samples)
                   == fuse::audio::HrtfIrSkipReason::NullSamples,
               "classify matches null samples");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(!fuse::audio::preflight_hrtf_ir(zero_length, &reason),
               "zero length fails preflight");
    expectTrue(reason == fuse::audio::HrtfIrSkipReason::ZeroLength,
               "zero length reports ZeroLength reason");
    expectTrue(fuse::audio::hrtf_ir_skip_reason_is_blocking(reason),
               "ZeroLength is blocking");

    const fuse::audio::HrtfIrPreflight empty_preflight =
        fuse::audio::preflight_hrtf_ir_stub(fuse::audio::make_empty_hrtf_ir());
    expectTrue(empty_preflight.skipped, "empty IR preflight is skipped");
    expectTrue(!empty_preflight.can_convolve(), "empty IR preflight cannot convolve");
}

void testHrtfPanPathPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    fuse::audio::HrtfPanPathSkipReason reason = fuse::audio::HrtfPanPathSkipReason::Disabled;
    expectTrue(fuse::audio::preflight_hrtf_pan_path(true, valid, offset, &reason),
               "enabled offset passes pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::None,
               "enabled offset preflight reason is None");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(false, valid, offset, &reason),
               "disabled HRTF fails pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::Disabled,
               "disabled HRTF reports Disabled reason");

    expectTrue(!fuse::audio::preflight_hrtf_pan_path(true, valid, fuse::audio::Vec3{}, &reason),
               "co-located source fails pan-path preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathSkipReason::CoLocated,
               "co-located source reports CoLocated reason");
    expectTrue(fuse::audio::hrtf_pan_path_skip_reason_is_blocking(reason),
               "CoLocated is blocking");

    const fuse::audio::HrtfPanPathPreflight spatial =
        fuse::audio::preflight_hrtf_pan_path_guarded(true, valid, offset);
    expectTrue(spatial.can_spatial_pan(), "spatial preflight can pan");
    expectTrue(spatial.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR resolves to convolution in preflight");

    const fuse::audio::HrtfPanPathPreflight bypass =
        fuse::audio::preflight_hrtf_pan_path_guarded(false, valid, offset);
    expectTrue(bypass.skipped, "disabled preflight is skipped");
    expectTrue(bypass.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight resolves to bypass");
    expectTrue(!bypass.can_spatial_pan(), "disabled preflight cannot spatial pan");

    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub alias predicate");
    expectTrue(fuse::audio::hrtf_pan_path_uses_ild_stub(fuse::audio::HrtfPanPath::IldItdStub)
                   == fuse::audio::hrtf_pan_path_uses_ild_itd_stub(
                          fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub alias matches ild_itd_stub predicate");
}

void testHrtfAttenuationCouplingPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfPanPath stub_path =
        fuse::audio::resolve_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), offset);

    fuse::audio::HrtfAttenuationCouplingSkipReason reason =
        fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath;
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 0.2f, 0.3f, &reason),
               "reduced attenuation passes coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "reduced attenuation preflight reason is None");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.2f, 0.3f, &reason),
               "bypass path fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath,
               "bypass path reports BypassPath reason");

    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 1.f, 1.f, &reason),
               "unity attenuation fails coupling preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
        fuse::audio::preflight_hrtf_attenuation_coupling_for_path(stub_path, 0.15f, 0.25f);
    expectTrue(narrow.can_narrow(), "reduced attenuation preflight can narrow");
    expectTrue(!narrow.skipped, "reduced attenuation preflight is not skipped");
    expectTrue(narrow.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "reduced attenuation struct reason is None");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling_for_path(stub_path, 1.f, 1.f);
    expectTrue(unity.skipped, "unity attenuation preflight is skipped");
    expectTrue(!unity.can_narrow(), "unity attenuation preflight cannot narrow");
    expectTrue(unity.reason == fuse::audio::HrtfAttenuationCouplingSkipReason::UnityAttenuation,
               "unity struct reason is UnityAttenuation");

    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_skip(stub_path, 0.2f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingSkipReason::None,
               "distance-only reduction classifies as narrowable");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_skip_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingSkipReason::BypassPath),
               "BypassPath is blocking");
    expectTrue(!fuse::audio::hrtf_attenuation_coupling_skip_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingSkipReason::None),
               "None is not blocking");

    expectTrue(fuse::audio::should_narrow_hrtf_spatial_image(stub_path, 0.2f, 0.3f)
                   == fuse::audio::preflight_hrtf_attenuation_coupling(stub_path, 0.2f, 0.3f),
               "should_narrow matches coupling preflight on valid path");
}

void testHrtfIrPreflightSkipAlias() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.should_skip_convolution(),
               "empty IR preflight skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight),
               "should_skip_hrtf_ir_convolution mirrors preflight");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.should_skip_convolution(),
               "valid IR preflight does not skip convolution");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(valid_preflight)
                   == !fuse::audio::can_convolve_hrtf_ir(valid_preflight),
               "skip alias inverts can_convolve on IR preflight");
}

void testPanPathPreflightApplyHelper() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);

    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_pan_path_preflight(stub_preflight, offset);
    const fuse::audio::BinauralPanGains guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, empty, offset);
    expectNear(from_preflight.left, guarded.left, 1e-5f,
               "from_pan_path_preflight matches guarded helper");
    expectNear(from_preflight.right, guarded.right, 1e-5f,
               "from_pan_path_preflight matches guarded helper");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, empty, offset);
    const fuse::audio::BinauralPanGains bypass =
        fuse::audio::compute_binaural_pan_gains_from_pan_path_preflight(bypass_preflight, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass),
               "bypass pan-path preflight returns centre pan");
}

void testHrtfBinauralPreflightCompositeAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.has_empty_ir(), "composite has_empty_ir on empty stub");
    expectTrue(stub_preflight.should_skip_convolution(),
               "composite should_skip_convolution on empty IR");
    expectTrue(fuse::audio::has_empty_hrtf_ir(stub_preflight),
               "has_empty_hrtf_ir mirrors composite method");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_convolution(stub_preflight),
               "should_skip_hrtf_binaural_convolution on empty IR");
    expectTrue(!fuse::audio::can_convolve_hrtf_binaural(stub_preflight),
               "can_convolve_hrtf_binaural false on empty IR");
    expectTrue(fuse::audio::should_apply_hrtf_attenuation_coupling(stub_preflight),
               "composite should_apply coupling when narrowing is warranted");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(stub_preflight),
               "composite should not skip coupling when narrowing is warranted");
    expectTrue(fuse::audio::can_narrow_hrtf_binaural_spatial_image(stub_preflight),
               "can_narrow alias mirrors can_narrow_spatial_image");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(stub_preflight),
               "empty IR stub path is internally consistent");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(!conv_preflight.has_empty_ir(), "valid IR composite is not empty");
    expectTrue(fuse::audio::can_convolve_hrtf_binaural(conv_preflight),
               "valid IR composite can convolve");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_convolution(conv_preflight),
               "valid IR composite does not skip convolution");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(conv_preflight),
               "convolution path with valid IR is consistent");
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(conv_preflight),
               "unity attenuation composite skips coupling");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(conv_preflight),
               "unity attenuation composite skips coupling via skip alias");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(bypass_preflight),
               "bypass composite skips attenuation coupling");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(bypass_preflight),
               "bypass with valid IR remains consistent");
}

void testCoupledForPathUsesCompositePreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::HrtfPanPath::IldItdStub, offset, 0.2f, 0.3f);
    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.2f, 0.3f);
    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(preflight, offset);
    expectNear(coupled.left, from_preflight.left, 1e-5f,
               "coupled_for_path routes through composite preflight apply");
    expectNear(coupled.right, from_preflight.right, 1e-5f,
               "coupled_for_path routes through composite preflight apply");
}

void testHrtfIrPreflightSkipAlias() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.should_skip_convolution(), "empty IR preflight skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight),
               "should_skip_hrtf_ir_convolution mirrors struct method");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight)
                   == !fuse::audio::can_convolve_hrtf_ir(empty_preflight),
               "skip/convolve IR preflight predicates are inverses");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.should_skip_convolution(),
               "valid IR preflight does not skip convolution");
}

void testHrtfPanPathPreflightAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.uses_ild_itd_stub(), "empty IR pan-path uses ILD/ITD stub");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_pan_path(stub_preflight),
               "uses_ild_itd_stub alias mirrors struct method");
    expectTrue(!stub_preflight.should_skip(), "ILD/ITD stub path is not skipped");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(stub_preflight),
               "should_skip pan-path preflight false on stub path");
    expectTrue(!fuse::audio::can_convolve_hrtf_pan_path(stub_preflight),
               "empty IR pan-path cannot convolve");

    const fuse::audio::HrtfPanPathPreflight conv_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(fuse::audio::can_convolve_hrtf_pan_path(conv_preflight),
               "valid IR pan-path can convolve");
    expectTrue(!conv_preflight.uses_ild_itd_stub(), "convolution path does not use ILD/ITD stub");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF pan-path is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(bypass_preflight),
               "should_skip pan-path preflight true on bypass");
}

void testPanPathPreflightApplyHelper() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);

    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_pan_path_preflight(stub_preflight, offset);
    const fuse::audio::BinauralPanGains guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, empty, offset);
    expectNear(from_preflight.left, guarded.left, 1e-5f,
               "from_pan_path_preflight matches guarded helper");
    expectNear(from_preflight.right, guarded.right, 1e-5f,
               "from_pan_path_preflight matches guarded helper");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, empty, offset);
    const fuse::audio::BinauralPanGains bypass =
        fuse::audio::compute_binaural_pan_gains_from_pan_path_preflight(bypass_preflight, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass),
               "bypass pan-path preflight returns centre pan");
}

void testHrtfAttenuationCouplingPreflightSkipAlias() {
    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.should_skip(), "unity attenuation coupling preflight is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(unity_preflight),
               "should_skip attenuation coupling preflight mirrors struct method");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrow_preflight.should_skip(), "reduced attenuation coupling is not skipped");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(narrow_preflight),
               "should_skip false when narrowing is warranted");
}

void testHrtfBinauralPreflightCompositeAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.has_empty_ir(), "composite has_empty_ir on empty stub");
    expectTrue(stub_preflight.should_skip_convolution(),
               "composite should_skip_convolution on empty IR");
    expectTrue(stub_preflight.uses_ild_itd_stub(), "empty IR composite uses ILD/ITD stub");
    expectTrue(fuse::audio::has_empty_hrtf_ir(stub_preflight),
               "has_empty_hrtf_ir mirrors composite method");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_binaural(stub_preflight),
               "uses_ild_itd_stub_hrtf_binaural mirrors composite method");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_convolution(stub_preflight),
               "should_skip_hrtf_binaural_convolution on empty IR");
    expectTrue(!fuse::audio::can_convolve_hrtf_binaural(stub_preflight),
               "can_convolve_hrtf_binaural false on empty IR");
    expectTrue(fuse::audio::should_apply_hrtf_attenuation_coupling(stub_preflight),
               "composite should_apply coupling when narrowing is warranted");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(stub_preflight),
               "composite should not skip coupling when narrowing is warranted");
    expectTrue(fuse::audio::can_narrow_hrtf_binaural_spatial_image(stub_preflight),
               "can_narrow alias mirrors can_narrow_spatial_image");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(stub_preflight),
               "empty IR stub path is internally consistent");
    expectTrue(!stub_preflight.should_skip(), "enabled offset stub path is not skipped");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(!conv_preflight.has_empty_ir(), "valid IR composite is not empty");
    expectTrue(fuse::audio::can_convolve_hrtf_binaural(conv_preflight),
               "valid IR composite can convolve");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_convolution(conv_preflight),
               "valid IR composite does not skip convolution");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(conv_preflight),
               "convolution path with valid IR is consistent");
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(conv_preflight),
               "unity attenuation composite skips coupling");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(conv_preflight),
               "unity attenuation composite skips coupling via skip alias");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF composite is skipped");
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(bypass_preflight),
               "bypass composite skips attenuation coupling");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(bypass_preflight),
               "bypass with valid IR remains consistent");
}

void testCoupledForPathUsesCompositePreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::HrtfPanPath::IldItdStub, offset, 0.2f, 0.3f);
    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.2f, 0.3f);
    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(preflight, offset);
    expectNear(coupled.left, from_preflight.left, 1e-5f,
               "coupled_for_path routes through composite preflight apply");
    expectNear(coupled.right, from_preflight.right, 1e-5f,
               "coupled_for_path routes through composite preflight apply");
}

void testListenerAwareBinauralPreflight() {
    fuse::audio::AudioListener listener{};
    listener.position = fuse::audio::Vec3{};
    listener.forward = fuse::audio::Vec3{0.f, 0.f, -1.f};
    listener.up = fuse::audio::Vec3{0.f, 1.f, 0.f};
    const fuse::audio::Vec3 source_position{5.f, 0.f, 0.f};

    const fuse::audio::HrtfBinauralPreflight listener_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, 0.2f, 0.3f);
    const fuse::audio::HrtfBinauralPreflight offset_preflight =
        fuse::audio::preflight_hrtf_binaural(true, source_position, 0.2f, 0.3f);
    expectTrue(listener_preflight.path() == offset_preflight.path(),
               "listener-aware preflight matches listener-local overload");
    expectTrue(listener_preflight.can_spatial_pan() == offset_preflight.can_spatial_pan(),
               "listener-aware spatial-pan predicate matches offset overload");

    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfBinauralPreflight ir_listener_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, valid, 1.f, 1.f);
    expectTrue(ir_listener_preflight.can_convolve(),
               "listener-aware IR preflight can convolve with valid IR");
}

void testApplyBinauralPanToSampleFromPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    float left = 0.f;
    float right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, stub_preflight, offset, 0.5f,
                                                             left, right);
    const fuse::audio::BinauralPanGains expected =
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 0.2f, 0.3f);
    expectNear(left, 0.5f * expected.left, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");
    expectNear(right, 0.5f * expected.right, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    left = 0.f;
    right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, bypass_preflight, offset, 0.25f,
                                                             left, right);
    expectNear(left, 0.25f, 1e-5f, "bypass preflight sample apply uses centre pan left");
    expectNear(right, 0.25f, 1e-5f, "bypass preflight sample apply uses centre pan right");
}

void testHrtfIrRejectReasonClassifiers() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR classifies as null samples");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "null samples label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(zero_length)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR classifies as malformed");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::MalformedIr)) == "malformed_ir",
               "malformed IR label");

    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::should_skip_hrtf_ir_preflight(valid),
               "valid IR does not skip IR preflight");

    fuse::audio::HrtfIrPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, preflight),
               "try_preflight succeeds for valid IR");
    expectTrue(preflight.ok(), "valid IR preflight ok()");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight reason is none");
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, preflight),
               "try_preflight fails for empty IR");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR try_preflight carries null_samples reason");
}

void testHrtfPanPathRejectReasonClassifiers() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF classifies as hrtf_disabled");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source classifies as co_located");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan reject reason");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "co-located label");

    fuse::audio::HrtfPanPathPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, empty, offset, preflight),
               "try_preflight pan path succeeds for spatial stub");
    expectTrue(preflight.ok(), "spatial stub pan preflight ok()");
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial stub pan preflight reason is none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, empty, offset, preflight),
               "try_preflight pan path fails when disabled");
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled pan preflight carries hrtf_disabled reason");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(false, offset),
               "disabled pan path preflight is skippable");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(true, offset),
               "enabled offset pan path preflight is not skippable");
}

void testHrtfAttenuationCouplingRejectReasonClassifiers() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path classifies as bypass_path");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation classifies as unity_attenuation");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "unity attenuation label");

    fuse::audio::HrtfAttenuationCouplingPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, preflight),
               "try_preflight coupling succeeds for narrowed path");
    expectTrue(preflight.ok(), "narrowing coupling preflight ok()");
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing coupling preflight reason is none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, preflight),
               "try_preflight coupling fails on bypass path");
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass coupling preflight carries bypass_path reason");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f),
               "unity attenuation coupling preflight is skippable");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 1.f),
               "reduced distance attenuation coupling preflight is not skippable");
}

void testHrtfBinauralRejectReasonClassifiers() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::IrFallback,
               "empty IR composite preflight selects ir_fallback");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(stub_preflight)
                   == fuse::audio::HrtfBinauralRejectReason::IrFallback,
               "classify mirrors composite ir_fallback");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::AttenuationSkipped,
               "unity attenuation composite preflight selects attenuation_skipped");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::AttenuationSkipped))
                   == "attenuation_skipped",
               "attenuation_skipped label");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::PanBypass,
               "disabled HRTF composite preflight selects pan_bypass");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_preflight(false, offset),
               "disabled composite preflight is skippable");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_preflight(true, offset),
               "enabled offset composite preflight is not skippable");

    fuse::audio::HrtfBinauralPreflight try_preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.3f, 0.4f,
                                                        try_preflight),
               "try_preflight composite succeeds for spatial path");
    expectTrue(try_preflight.ok(), "spatial composite preflight ok()");
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "full spatial composite preflight reason is none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f,
                                                         try_preflight),
               "try_preflight composite fails for co-located source");
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::PanBypass,
               "co-located composite try_preflight carries pan_bypass reason");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfIrPreflightSkipAlias() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.should_skip_convolution(), "empty IR preflight skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight),
               "should_skip_hrtf_ir_convolution mirrors struct method");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty_preflight)
                   == !fuse::audio::can_convolve_hrtf_ir(empty_preflight),
               "skip/convolve IR preflight predicates are inverses");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.should_skip_convolution(),
               "valid IR preflight does not skip convolution");
}

void testHrtfPanPathPreflightAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.uses_ild_itd_stub(), "empty IR pan-path uses ILD/ITD stub");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_pan_path(stub_preflight),
               "uses_ild_itd_stub alias mirrors struct method");
    expectTrue(!stub_preflight.should_skip(), "ILD/ITD stub path is not skipped");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(stub_preflight),
               "should_skip pan-path preflight false on stub path");
    expectTrue(!fuse::audio::can_convolve_hrtf_pan_path(stub_preflight),
               "empty IR pan-path cannot convolve");

    const fuse::audio::HrtfPanPathPreflight conv_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(fuse::audio::can_convolve_hrtf_pan_path(conv_preflight),
               "valid IR pan-path can convolve");
    expectTrue(!conv_preflight.uses_ild_itd_stub(), "convolution path does not use ILD/ITD stub");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF pan-path is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(bypass_preflight),
               "should_skip pan-path preflight true on bypass");
}

void testPanPathPreflightApplyHelper() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);

    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_pan_path_preflight(stub_preflight, offset);
    const fuse::audio::BinauralPanGains guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, empty, offset);
    expectNear(from_preflight.left, guarded.left, 1e-5f,
               "from_pan_path_preflight matches guarded helper");
    expectNear(from_preflight.right, guarded.right, 1e-5f,
               "from_pan_path_preflight matches guarded helper");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, empty, offset);
    const fuse::audio::BinauralPanGains bypass =
        fuse::audio::compute_binaural_pan_gains_from_pan_path_preflight(bypass_preflight, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass),
               "bypass pan-path preflight returns centre pan");
}

void testHrtfAttenuationCouplingPreflightSkipAlias() {
    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.should_skip(), "unity attenuation coupling preflight is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(unity_preflight),
               "should_skip attenuation coupling preflight mirrors struct method");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrow_preflight.should_skip(), "reduced attenuation coupling is not skipped");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(narrow_preflight),
               "should_skip false when narrowing is warranted");
}

void testHrtfBinauralPreflightCompositeAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.has_empty_ir(), "composite has_empty_ir on empty stub");
    expectTrue(stub_preflight.should_skip_convolution(),
               "composite should_skip_convolution on empty IR");
    expectTrue(stub_preflight.uses_ild_itd_stub(), "empty IR composite uses ILD/ITD stub");
    expectTrue(fuse::audio::has_empty_hrtf_ir(stub_preflight),
               "has_empty_hrtf_ir mirrors composite method");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_binaural(stub_preflight),
               "uses_ild_itd_stub_hrtf_binaural mirrors composite method");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_convolution(stub_preflight),
               "should_skip_hrtf_binaural_convolution on empty IR");
    expectTrue(!fuse::audio::can_convolve_hrtf_binaural(stub_preflight),
               "can_convolve_hrtf_binaural false on empty IR");
    expectTrue(fuse::audio::should_apply_hrtf_attenuation_coupling(stub_preflight),
               "composite should_apply coupling when narrowing is warranted");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(stub_preflight),
               "composite should not skip coupling when narrowing is warranted");
    expectTrue(fuse::audio::can_narrow_hrtf_binaural_spatial_image(stub_preflight),
               "can_narrow alias mirrors can_narrow_spatial_image");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(stub_preflight),
               "empty IR stub path is internally consistent");
    expectTrue(!stub_preflight.should_skip(), "enabled offset stub path is not skipped");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(!conv_preflight.has_empty_ir(), "valid IR composite is not empty");
    expectTrue(fuse::audio::can_convolve_hrtf_binaural(conv_preflight),
               "valid IR composite can convolve");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_convolution(conv_preflight),
               "valid IR composite does not skip convolution");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(conv_preflight),
               "convolution path with valid IR is consistent");
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(conv_preflight),
               "unity attenuation composite skips coupling");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(conv_preflight),
               "unity attenuation composite skips coupling via skip alias");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF composite is skipped");
    expectTrue(!fuse::audio::should_apply_hrtf_attenuation_coupling(bypass_preflight),
               "bypass composite skips attenuation coupling");
    expectTrue(fuse::audio::is_consistent_hrtf_binaural_preflight(bypass_preflight),
               "bypass with valid IR remains consistent");
}

void testCoupledForPathUsesCompositePreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::HrtfPanPath::IldItdStub, offset, 0.2f, 0.3f);
    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.2f, 0.3f);
    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(preflight, offset);
    expectNear(coupled.left, from_preflight.left, 1e-5f,
               "coupled_for_path routes through composite preflight apply");
    expectNear(coupled.right, from_preflight.right, 1e-5f,
               "coupled_for_path routes through composite preflight apply");
}

void testListenerAwareBinauralPreflight() {
    fuse::audio::AudioListener listener{};
    listener.position = fuse::audio::Vec3{};
    listener.forward = fuse::audio::Vec3{0.f, 0.f, -1.f};
    listener.up = fuse::audio::Vec3{0.f, 1.f, 0.f};
    const fuse::audio::Vec3 source_position{5.f, 0.f, 0.f};

    const fuse::audio::HrtfBinauralPreflight listener_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, 0.2f, 0.3f);
    const fuse::audio::HrtfBinauralPreflight offset_preflight =
        fuse::audio::preflight_hrtf_binaural(true, source_position, 0.2f, 0.3f);
    expectTrue(listener_preflight.path() == offset_preflight.path(),
               "listener-aware preflight matches listener-local overload");
    expectTrue(listener_preflight.can_spatial_pan() == offset_preflight.can_spatial_pan(),
               "listener-aware spatial-pan predicate matches offset overload");

    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfBinauralPreflight ir_listener_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, valid, 1.f, 1.f);
    expectTrue(ir_listener_preflight.can_convolve(),
               "listener-aware IR preflight can convolve with valid IR");
}

void testApplyBinauralPanToSampleFromPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    float left = 0.f;
    float right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, stub_preflight, offset, 0.5f,
                                                             left, right);
    const fuse::audio::BinauralPanGains expected =
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 0.2f, 0.3f);
    expectNear(left, 0.5f * expected.left, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");
    expectNear(right, 0.5f * expected.right, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    left = 0.f;
    right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, bypass_preflight, offset, 0.25f,
                                                             left, right);
    expectNear(left, 0.25f, 1e-5f, "bypass preflight sample apply uses centre pan left");
    expectNear(right, 0.25f, 1e-5f, "bypass preflight sample apply uses centre pan right");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfIrRejectReasonPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty) == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::NullSamples),
               "rejects_for_reason matches null_samples on empty IR");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::EmptyIr),
               "valid IR does not reject for empty_ir");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR rejects as malformed");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "rejects_for_reason matches malformed_ir");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(null_samples_nonzero_length)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "null samples with non-zero length rejects as null_samples");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight carries empty-IR reject reason");
    expectTrue(!empty_preflight.can_convolve(), "preflight can_convolve mirrors reject reason");

    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::MalformedIr)) == "malformed_ir",
               "reject reason label for malformed_ir");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "rejects_for_reason matches hrtf_disabled");

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "rejects_for_reason matches co_located");

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");

    const fuse::audio::HrtfPanPathPreflight spatial_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(spatial_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial preflight carries none reject reason");
    expectTrue(spatial_preflight.can_spatial_pan(), "spatial preflight can_spatial_pan mirrors reason");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "bypass preflight carries hrtf_disabled reject reason");
    expectTrue(!bypass_preflight.can_spatial_pan(), "bypass preflight cannot spatial-pan");

    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "rejects_for_reason matches bypass_path");

    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "rejects_for_reason matches unity_attenuation");

    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowable preflight carries none reject reason");
    expectTrue(narrow_preflight.can_narrow(), "narrowable preflight can_narrow mirrors reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity preflight carries unity_attenuation reject reason");
    expectTrue(!unity_preflight.can_narrow(), "unity preflight cannot narrow");

    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath)) == "bypass_path",
               "reject reason label for bypass_path");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};

    expectTrue(fuse::audio::hrtf_binaural_reject_reason(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite rejects disabled HRTF");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "composite rejects_for_reason matches hrtf_disabled");

    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite rejects co-located source");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "composite rejects_for_reason matches co_located");

    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "enabled offset source has no composite reject reason");

    const fuse::audio::HrtfBinauralPreflight spatial_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(spatial_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial composite preflight carries none reject reason");
    expectTrue(!spatial_preflight.is_bypass(), "spatial composite preflight is not bypassed");
    expectTrue(spatial_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite bundles empty-IR reject reason");
    expectTrue(spatial_preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "composite bundles pan-path reject reason");
    expectTrue(spatial_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "composite bundles attenuation-coupling reject reason");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, empty, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "bypass composite preflight carries hrtf_disabled reject reason");
    expectTrue(bypass_preflight.is_bypass(), "bypass composite preflight is bypassed");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass_preflight),
               "should_skip_hrtf_binaural mirrors composite reject reason");

    const fuse::audio::HrtfBinauralPreflight unity_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 1.f, 1.f);
    expectTrue(unity_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reject reason preserved in composite preflight");
    expectTrue(unity_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "unity attenuation does not set composite bypass reject reason");

    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled)) == "hrtf_disabled",
               "composite reject reason label for hrtf_disabled");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfRejectReasonPreflights() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    const fuse::audio::HrtfIrStub malformed{samples, 0};

    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::None),
               "valid IR has no reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::NullSamples),
               "empty IR rejects with null_samples");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects with malformed_ir");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "IR reject reason name for malformed_ir");

    const fuse::audio::HrtfIrPreflight ir_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(ir_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "IR preflight reason is none for valid IR");
    expectTrue(ir_preflight.can_convolve() == (ir_preflight.reason == fuse::audio::HrtfIrRejectReason::None),
               "IR preflight can_convolve mirrors reject reason");

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, offset, fuse::audio::HrtfPanPathRejectReason::None),
               "enabled offset source has no pan-path reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects pan path");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "pan-path reject reason name for co_located");

    const fuse::audio::HrtfPanPathPreflight pan_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(pan_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "pan-path preflight reason is none on spatial path");
    expectTrue(pan_preflight.can_spatial_pan()
                   == (pan_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None),
               "pan-path can_spatial_pan mirrors reject reason");

    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects narrowing");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "spatial path with reduced attenuation has no coupling reject reason");
    expectTrue(std::strcmp(
                   fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                       fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                   "unity_attenuation")
                   == 0,
               "attenuation-coupling reject reason name for unity_attenuation");

    const fuse::audio::HrtfAttenuationCouplingPreflight coupling_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 0.2f,
                                                         0.3f);
    expectTrue(coupling_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "coupling preflight reason is none when narrowing applies");
    expectTrue(coupling_preflight.can_narrow()
                   == (coupling_preflight.reason
                       == fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "coupling can_narrow mirrors reject reason");

    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, offset, fuse::audio::HrtfBinauralRejectReason::None),
               "enabled offset source has no composite reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF rejects composite binaural");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located source rejects composite binaural");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                           "hrtf_disabled")
                   == 0,
               "composite reject reason name for hrtf_disabled");

    const fuse::audio::HrtfBinauralPreflight composite_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(composite_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite preflight reason is none on spatial stub path");
    expectTrue(!composite_preflight.is_bypass(),
               "composite is_bypass false when reject reason is none");
    expectTrue(composite_preflight.is_bypass()
                   == (composite_preflight.reason != fuse::audio::HrtfBinauralRejectReason::None),
               "composite is_bypass mirrors reject reason");
    expectTrue(static_cast<fuse::u8>(composite_preflight.panPath.reason)
                   == static_cast<fuse::u8>(composite_preflight.reason),
               "composite pan-path reject reason matches composite reason");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite preflight marks disabled reject reason");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass_preflight),
               "should_skip_hrtf_binaural true when composite reject reason is set");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtfIrRejectReason(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with NullSamples");
    expectTrue(fuse::audio::hrtfIrRejectsForReason(empty,
                                                     fuse::audio::HrtfIrRejectReason::NullSamples),
               "hrtfIrRejectsForReason matches NullSamples on empty IR");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtfIrRejectReason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtfIrRejectsForReason(valid,
                                                     fuse::audio::HrtfIrRejectReason::NullSamples),
               "valid IR does not reject for NullSamples");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtfIrRejectReason(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR rejects as MalformedIr");
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonName(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "MalformedIr") == 0,
               "malformed IR reject reason label");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight_hrtf_ir carries reject reason");
    expectTrue(!empty_preflight.can_convolve(), "reject reason blocks convolution");
    expectTrue(empty_preflight.can_convolve()
                   == (empty_preflight.reason == fuse::audio::HrtfIrRejectReason::None),
               "can_convolve mirrors reject reason");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtfPanPathRejectReason(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::hrtfPanPathRejectReason(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::hrtfPanPathRejectReason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject");

    expectTrue(fuse::audio::hrtfPanPathRejectsForReason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "hrtfPanPathRejectsForReason matches disabled HRTF");
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonName(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated") == 0,
               "co-located pan-path reject reason label");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "preflight_hrtf_pan_path carries reject reason");
    expectTrue(!disabled_preflight.can_spatial_pan(), "reject reason blocks spatial pan");
    expectTrue(disabled_preflight.skipped, "skipped flag mirrors reject reason");

    const fuse::audio::HrtfPanPathPreflight spatial_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(spatial_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial pan path has no reject reason");
    expectTrue(spatial_preflight.can_spatial_pan(), "no reject reason allows spatial pan");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectReason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectReason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects narrowing");
    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectReason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "spatial path with reduced attenuation has no reject reason");

    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectsForReason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "hrtfAttenuationCouplingRejectsForReason matches bypass path");
    expectTrue(std::strcmp(fuse::audio::hrtfAttenuationCouplingRejectReasonName(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation") == 0,
               "unity attenuation coupling reject reason label");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "preflight_hrtf_attenuation_coupling carries reject reason");
    expectTrue(narrow_preflight.can_narrow(), "no reject reason allows narrowing");
    expectTrue(narrow_preflight.can_narrow()
                   == (narrow_preflight.reason
                       == fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "can_narrow mirrors reject reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation preflight carries reject reason");
    expectTrue(!unity_preflight.can_narrow(), "unity reject reason blocks narrowing");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "enabled offset composite preflight has no spatial reject");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite carries empty-IR convolution reject");
    expectTrue(fuse::audio::hrtfBinauralConvolutionRejectReason(stub_preflight)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "convolution reject accessor mirrors IR sub-preflight");
    expectTrue(stub_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "composite narrowing proceeds when attenuation is reduced");
    expectTrue(fuse::audio::hrtfBinauralNarrowingRejectReason(stub_preflight)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing reject accessor mirrors coupling sub-preflight");
    expectTrue(!stub_preflight.is_bypass(), "no spatial reject means not bypassed");

    const fuse::audio::HrtfBinauralPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite preflight carries spatial reject");
    expectTrue(fuse::audio::hrtfBinauralRejectReason(disabled_preflight)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "hrtfBinauralRejectReason accessor matches preflight reason");
    expectTrue(fuse::audio::hrtfBinauralRejectsForReason(
                   disabled_preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "hrtfBinauralRejectsForReason matches disabled HRTF");
    expectTrue(disabled_preflight.is_bypass(), "spatial reject marks bypass");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(disabled_preflight),
               "should_skip_hrtf_binaural mirrors spatial reject reason");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight carries spatial reject");
    expectTrue(std::strcmp(fuse::audio::hrtfBinauralRejectReasonName(
                               fuse::audio::HrtfBinauralRejectReason::CoLocated),
                           "CoLocated") == 0,
               "co-located composite reject reason label");

    const fuse::audio::HrtfBinauralPreflight unity_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(unity_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "unity attenuation does not spatially bypass composite preflight");
    expectTrue(unity_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "composite carries unity attenuation narrowing reject");
    expectTrue(!unity_preflight.can_narrow_spatial_image(),
               "unity attenuation blocks narrowing in composite preflight");
    expectTrue(unity_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no convolution reject in composite preflight");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF rejects composite binaural with hrtf_disabled reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located source rejects composite binaural with co_located reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "enabled offset source has no composite binaural reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                           "hrtf_disabled") == 0,
               "hrtf_disabled composite reject reason label");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::NullSamples),
               "empty IR rejects with null_samples reason");
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty) == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason is null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::NullSamples),
               "valid IR does not reject for null_samples");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects with malformed_ir reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "malformed_ir label");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(preflight.rejected, "malformed IR preflight is rejected");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR preflight carries reject reason");
    expectTrue(!preflight.can_convolve(), "malformed IR preflight cannot convolve");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects with hrtf_disabled reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects with co_located reason");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.rejected, "disabled pan-path preflight is rejected");
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled pan-path preflight carries reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "co_located label");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects coupling with bypass_path reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects coupling with unity_attenuation reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution, 1.f,
                                                         1.f);
    expectTrue(unity_preflight.rejected, "unity coupling preflight is rejected");
    expectTrue(unity_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity coupling preflight carries reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
                           "bypass_path")
                   == 0,
               "bypass_path label");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, valid, offset, 0.1f, 0.1f,
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF composite rejects with hrtf_disabled");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, valid, co_located, 0.1f, 0.1f,
                   fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located composite rejects with co_located");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, empty, offset, 0.2f, 0.3f,
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "empty IR composite reports empty_ir reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, valid, offset, 1.f, 1.f,
                   fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "unity attenuation composite reports unity_attenuation reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(!stub_preflight.rejected, "empty IR stub path is not spatially rejected");
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite preflight carries empty_ir reason on stub path");
    expectTrue(stub_preflight.ir.rejected, "composite IR sub-preflight is rejected");
    expectTrue(!stub_preflight.panPath.rejected, "composite pan sub-preflight is not rejected");
    expectTrue(stub_preflight.can_spatial_pan(), "empty IR composite still spatial-pans");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.rejected, "disabled composite preflight is spatially rejected");
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite preflight carries hrtf_disabled reason");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass_preflight),
               "should_skip_hrtf_binaural matches spatial bypass");

    const fuse::audio::HrtfBinauralPreflight unity_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(!unity_preflight.rejected, "unity attenuation does not spatially reject");
    expectTrue(unity_preflight.reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity composite preflight carries unity_attenuation reason");
    expectTrue(unity_preflight.attenuationCoupling.rejected,
               "unity coupling sub-preflight is rejected");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "empty_ir")
                   == 0,
               "empty_ir composite label");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::NullSamples),
               "canonical empty IR rejects with NullSamples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "MalformedIr") == 0,
               "MalformedIr reject reason has stable label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir(valid).reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight carries None reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects with MalformedIr");
    expectTrue(fuse::audio::preflight_hrtf_ir(malformed).reason
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR preflight carries MalformedIr reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir(malformed).can_convolve()
                   == (fuse::audio::preflight_hrtf_ir(malformed).reason
                       == fuse::audio::HrtfIrRejectReason::None),
               "preflight can_convolve mirrors IR reject reason");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects with HrtfDisabled");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects with CoLocated");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated") == 0,
               "CoLocated pan-path reject reason has stable label");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled pan-path preflight carries HrtfDisabled reject reason");
    expectTrue(disabled_preflight.can_spatial_pan()
                   == (disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None),
               "pan-path preflight can_spatial_pan mirrors reject reason");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects attenuation coupling with BypassPath");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects coupling with UnityAttenuation");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation") == 0,
               "UnityAttenuation coupling reject reason has stable label");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries None reject reason");
    expectTrue(narrow_preflight.can_narrow()
                   == (narrow_preflight.reason
                       == fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "coupling preflight can_narrow mirrors reject reason");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite preflight rejects with EmptyIr");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite preflight carries empty-IR reject reason");
    expectTrue(stub_preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial stub path has no pan-path reject reason");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite preflight rejects with UnityAttenuation");
    expectTrue(conv_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR composite preflight has no IR reject reason");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite preflight rejects with HrtfDisabled");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, valid, offset, 0.1f, 0.1f,
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "hrtf_binaural_rejects_for_reason matches disabled composite");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight rejects with CoLocated");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfBinauralPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_binaural(true, malformed, offset, 0.2f, 0.3f);
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite preflight rejects with MalformedIr");

    const fuse::audio::HrtfBinauralPreflight ideal_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f);
    expectTrue(ideal_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path with reduced attenuation has no composite reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "EmptyIr") == 0,
               "EmptyIr composite reject reason has stable label");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, valid, offset, 0.2f, 0.3f)
                   == ideal_preflight.reason,
               "hrtf_binaural_reject_reason matches composite preflight");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes null_samples reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(!preflight.ok(), "empty IR preflight is not ok");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "IR reject reason label for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source passes pan-path reject classify");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path leaves reject reason at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF exposes hrtf_disabled reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan path");
    expectTrue(preflight.ok(), "ILD/ITD stub pan path preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "pan-path reject reason label for co_located");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation passes coupling reject classify");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path leaves coupling reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight attenuation coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path exposes bypass_path coupling reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries none coupling reason");
    expectTrue(preflight.ok(), "narrowing coupling preflight is ok");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
                   == "unity_attenuation",
               "coupling reject reason label for unity_attenuation");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite classify marks co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR still passes composite classify — ILD/ITD stub is valid");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path has no composite reject");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling classify maps bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling classify maps unity attenuation");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path leaves reject at none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite exposes hrtf_disabled reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "empty IR composite preflight keeps none reject — ILD/ITD is valid");
    expectTrue(stub_preflight.ok(), "empty IR composite preflight is ok for spatial pan");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason lives in IR sub-preflight");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR composite still allows ILD/ITD spatial pan");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",
               "composite reject reason label for empty_ir");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::NullSamples),
               "canonical empty IR rejects with null_samples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir") == 0,
               "malformed_ir reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "non-null zero-length IR is malformed");
    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "preflight carries malformed_ir reject reason");
    expectTrue(!malformed_preflight.can_convolve(), "malformed IR preflight cannot convolve");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan reject reason");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "pan preflight carries hrtf_disabled reject reason");
    expectTrue(!disabled_preflight.can_spatial_pan(),
               "disabled pan preflight cannot spatial-pan");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects coupling narrowing");
    expectTrue(std::strcmp(
                   fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                       fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                   "unity_attenuation") == 0,
               "unity_attenuation reject reason label");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");
    expectTrue(narrow_preflight.can_narrow(), "reduced attenuation can narrow");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite preflight marks empty_ir reject reason");
    expectTrue(stub_preflight.can_spatial_pan(),
               "empty IR still spatial-pans via ILD/ITD stub");
    expectTrue(!stub_preflight.can_convolve(), "empty IR composite cannot convolve");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(stub_preflight)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite reject reason resolver matches empty_ir");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR with unity attenuation has no composite reject reason");
    expectTrue(conv_preflight.can_convolve(), "valid IR composite can convolve");
    expectTrue(conv_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation coupling reject reason on sub-preflight");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   bypass_preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF composite rejects with hrtf_disabled");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                           "hrtf_disabled") == 0,
               "hrtf_disabled composite reject reason label");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   co_located_preflight, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located composite rejects with co_located");

    expectTrue(fuse::audio::hrtf_binaural_reject_reason_from_ir(
                   fuse::audio::HrtfIrRejectReason::MalformedIr)
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "IR malformed maps to composite malformed_ir");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_from_coupling(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "coupling unity maps to composite unity_attenuation");
}

void testHrtfIrRejectReason() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with NullSamples");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                         fuse::audio::HrtfIrRejectReason::NullSamples),
               "hrtf_ir_rejects_for_reason matches NullSamples");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR rejects with MalformedIr");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "MalformedIr")
                   == 0,
               "malformed IR reject reason name");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "IR preflight carries reject reason");
}

void testHrtfPanPathRejectReason() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "pan-path rejects_for_reason matches disabled");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "pan-path preflight carries reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
                   == 0,
               "co-located pan-path reject reason name");
}

void testHrtfAttenuationCouplingRejectReason() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "coupling rejects_for_reason matches bypass");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling narrowing");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "coupling preflight carries reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation")
                   == 0,
               "unity attenuation coupling reject reason name");
}

void testHrtfBinauralRejectReason() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, empty, offset, 0.2f, 0.3f)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite rejects convolution with EmptyIr");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, valid, offset, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite rejects narrowing");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(false, valid, offset, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite rejects with HrtfDisabled");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, valid, offset, 0.1f, 0.1f,
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "composite rejects_for_reason matches disabled");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, valid, co_located, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite rejects with CoLocated");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, malformed, offset, 0.2f, 0.3f)
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite rejects with MalformedIr");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 0.25f, 0.35f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path with reduced attenuation has no composite reject");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "EmptyIr")
                   == 0,
               "empty IR composite reject reason name");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite preflight carries reject reason");
    expectTrue(stub_preflight.can_spatial_pan(),
               "EmptyIr composite reject still allows spatial pan");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(
        fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::NullSamples),
        "empty IR rejects for NullSamples");
    expectTrue(
        std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                        fuse::audio::HrtfIrRejectReason::NullSamples),
                    "NullSamples") == 0,
        "IR reject reason name resolves NullSamples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects for MalformedIr");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.rejected, "empty IR preflight is rejected");
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR preflight carries NullSamples reason");

    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.rejected, "valid IR preflight is not rejected");
    expectTrue(valid_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight reason is None");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects for HrtfDisabled");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects for CoLocated");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");
    expectTrue(
        std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                        fuse::audio::HrtfPanPathRejectReason::CoLocated),
                    "CoLocated") == 0,
        "pan-path reject reason name resolves CoLocated");

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.rejected, "disabled pan-path preflight is rejected");
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled pan-path preflight carries HrtfDisabled reason");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), co_located);
    expectTrue(co_located_preflight.rejected, "co-located pan-path preflight is rejected");
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located pan-path preflight carries CoLocated reason");

    const fuse::audio::HrtfPanPathPreflight spatial_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(), offset);
    expectTrue(!spatial_preflight.rejected, "spatial stub pan-path preflight is not rejected");
    expectTrue(spatial_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial stub pan-path preflight reason is None");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects attenuation coupling for BypassPath");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects coupling for UnityAttenuation");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");
    expectTrue(
        std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                        fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                    "UnityAttenuation") == 0,
        "coupling reject reason name resolves UnityAttenuation");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(bypass_preflight.rejected, "bypass coupling preflight is rejected");
    expectTrue(bypass_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass coupling preflight carries BypassPath reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.rejected, "unity coupling preflight is rejected");
    expectTrue(unity_preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity coupling preflight carries UnityAttenuation reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrow_preflight.rejected, "narrowable coupling preflight is not rejected");
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowable coupling preflight reason is None");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF composite rejects for HrtfDisabled");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located composite rejects for CoLocated");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "enabled offset composite has no reject reason");
    expectTrue(
        std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                        fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                    "HrtfDisabled") == 0,
        "composite reject reason name resolves HrtfDisabled");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.rejected, "disabled composite preflight is rejected");
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite preflight carries HrtfDisabled reason");
    expectTrue(!bypass_preflight.ir.rejected,
               "valid IR is not rejected on disabled composite path");
    expectTrue(bypass_preflight.attenuationCoupling.rejected,
               "composite bundles coupling reject diagnostics on bypass path");
    expectTrue(bypass_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass composite coupling reject reason is BypassPath");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.rejected, "co-located composite preflight is rejected");
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight carries CoLocated reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(!stub_preflight.rejected, "spatial stub composite preflight is not rejected");
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub composite preflight reason is None");
    expectTrue(stub_preflight.ir.rejected, "empty IR is rejected for convolution in composite");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite IR reject reason is NullSamples for empty stub");
    expectTrue(!stub_preflight.attenuationCoupling.rejected,
               "reduced attenuation coupling is not rejected in composite");
    expectTrue(stub_preflight.can_spatial_pan(), "spatial stub composite can spatial-pan");
    expectTrue(!stub_preflight.can_convolve(), "empty IR composite cannot convolve");
    expectTrue(stub_preflight.can_narrow_spatial_image(),
               "reduced attenuation composite can narrow spatial image");
}

void testHrtfIrRejectReasons() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR classifies as null_samples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "null_samples")
                   == 0,
               "null_samples reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR classifies as malformed_ir");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "malformed_ir reject reason label");

    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reports no reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir_convolution(valid),
               "valid IR convolution preflight succeeds");
    expectTrue(!fuse::audio::preflight_hrtf_ir_convolution(empty),
               "empty IR convolution preflight fails");
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_is_blocking(fuse::audio::HrtfIrRejectReason::None),
               "none IR reject reason is non-blocking");
    expectTrue(fuse::audio::hrtf_ir_reject_reason_is_blocking(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR reject reason is blocking");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::preflight_hrtf_ir_convolution(empty, &reason),
               "empty IR convolution preflight writes reject reason");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR convolution preflight reason is null_samples");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "IR preflight carries reject reason");
}

void testHrtfPanPathRejectReasons() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_spatial_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF spatial reject reason");
    expectTrue(fuse::audio::classify_hrtf_pan_path_spatial_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located spatial reject reason");
    expectTrue(fuse::audio::classify_hrtf_pan_path_spatial_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no spatial reject reason");

    expectTrue(fuse::audio::classify_hrtf_pan_path_convolution_reject(true, empty, offset)
                   == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "empty IR convolution pan-path reject reason");
    expectTrue(fuse::audio::classify_hrtf_pan_path_convolution_reject(true, valid, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "valid IR convolution pan-path has no reject reason");

    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::EmptyIr),
                           "empty_ir")
                   == 0,
               "empty_ir pan-path reject reason label");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason_is_blocking_spatial(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located blocks spatial pan");
    expectTrue(!fuse::audio::hrtf_pan_path_reject_reason_is_blocking_spatial(
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr),
               "empty IR does not block spatial pan");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason_is_blocking_convolution(
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr),
               "empty IR blocks convolution routing");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::preflight_hrtf_spatial_pan_path(true, offset, &reason),
               "spatial pan-path preflight succeeds on offset source");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial pan-path preflight reports none on success");
    expectTrue(!fuse::audio::preflight_hrtf_convolution_pan_path(true, empty, offset, &reason),
               "convolution pan-path preflight fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "convolution pan-path preflight reports empty_ir");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.spatialRejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "pan-path preflight spatial reject is none on stub path");
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "pan-path preflight convolution reject is empty_ir");
}

void testHrtfAttenuationCouplingRejectReasons() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path coupling reject reason");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation coupling reject reason");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject reason");

    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "unity_attenuation")
                   == 0,
               "unity_attenuation coupling reject reason label");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path coupling reject is blocking");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(!fuse::audio::preflight_hrtf_attenuation_coupling_narrowing(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, &reason),
               "coupling narrowing preflight fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "coupling narrowing preflight reports bypass_path");
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling_narrowing(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, &reason),
               "coupling narrowing preflight succeeds with reduced attenuation");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "coupling narrowing preflight reports none on success");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "attenuation coupling preflight carries none reject reason");
}

void testHrtfBinauralRejectReasons() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const float malformed_samples[] = {0.5f};
    const fuse::audio::HrtfIrStub malformed{malformed_samples, 0};

    expectTrue(fuse::audio::classify_hrtf_binaural_spatial_reject(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite spatial reject for disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_spatial_reject(true, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite spatial reject for co-located source");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite convolution reject for empty IR");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, malformed, offset)
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "composite convolution reject for malformed IR");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "composite convolution reject none for valid IR");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite coupling reject for bypass path");

    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "co_located composite reject reason label");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_blocking_spatial(
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF blocks composite spatial pan");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_blocking_convolution(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "empty IR blocks composite convolution");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_is_blocking_convolution(
                   fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "unity attenuation does not block convolution");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_blocking_coupling(
                   fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "unity attenuation blocks composite coupling");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::preflight_hrtf_binaural_spatial(true, offset, &reason),
               "composite spatial preflight succeeds on offset source");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite spatial preflight reports none on success");
    expectTrue(!fuse::audio::preflight_hrtf_binaural_convolution(true, empty, offset, &reason),
               "composite convolution preflight fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite convolution preflight reports empty_ir");
    expectTrue(fuse::audio::preflight_hrtf_binaural_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, &reason),
               "composite coupling preflight succeeds with reduced attenuation");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite coupling preflight reports none on success");

    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(preflight.spatialRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite preflight spatial reject is none on stub path");
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite preflight convolution reject is empty_ir");
    expectTrue(preflight.couplingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite preflight coupling reject is none with reduced attenuation");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.spatialRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite preflight spatial reject is hrtf_disabled on bypass");
    expectTrue(bypass_preflight.convolutionRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite preflight convolution reject is hrtf_disabled on bypass");
    expectTrue(bypass_preflight.couplingRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "composite preflight coupling reject is bypass_path on bypass");
}

void testHrtfIrRejectReasonGuards() {
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::None),
                           "None")
                   == 0,
               "None IR reject reason has stable label");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "NullSamples")
                   == 0,
               "NullSamples IR reject reason has stable label");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "MalformedIr")
                   == 0,
               "MalformedIr IR reject reason has stable label");

    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                      fuse::audio::HrtfIrRejectReason::NullSamples),
               "canonical empty IR reports NullSamples reject reason");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(
                   null_samples_nonzero_length, fuse::audio::HrtfIrRejectReason::NullSamples),
               "null samples with non-zero length reports NullSamples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                      fuse::audio::HrtfIrRejectReason::MalformedIr),
               "non-null zero-length IR reports MalformedIr");

    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::None),
               "valid IR reports None reject reason");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR preflight carries NullSamples reason");
    expectTrue(!empty_preflight.can_convolve(), "NullSamples preflight cannot convolve");

    fuse::audio::HrtfIrPreflight out{};
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, out, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir writes NullSamples reject reason");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, out, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir writes None on valid IR");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
                           "HrtfDisabled")
                   == 0,
               "HrtfDisabled pan-path reject reason has stable label");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
                   == 0,
               "CoLocated pan-path reject reason has stable label");

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF reports HrtfDisabled reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source reports CoLocated reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, offset, fuse::audio::HrtfPanPathRejectReason::None),
               "enabled offset source reports None reject reason");

    const fuse::audio::HrtfIrStub empty{};
    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "ILD/ITD stub path preflight reason is None");
    expectTrue(stub_preflight.can_spatial_pan(), "None pan-path reason can spatial-pan");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, empty, offset);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF pan-path preflight carries HrtfDisabled reason");
    expectTrue(bypass_preflight.should_skip(), "HrtfDisabled pan-path preflight is skipped");

    fuse::audio::HrtfPanPathPreflight out{};
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, empty, offset, out, reason),
               "try_preflight_hrtf_pan_path succeeds on spatial stub path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "try_preflight_hrtf_pan_path writes None on spatial path");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, empty, offset, out, reason),
               "try_preflight_hrtf_pan_path fails when HRTF disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "try_preflight_hrtf_pan_path writes HrtfDisabled on bypass");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
                           "BypassPath")
                   == 0,
               "BypassPath coupling reject reason has stable label");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation")
                   == 0,
               "UnityAttenuation coupling reject reason has stable label");

    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path reports BypassPath coupling reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation reports UnityAttenuation coupling reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "reduced attenuation reports None coupling reject reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries None coupling reason");
    expectTrue(narrow_preflight.can_narrow(), "None coupling reason can narrow");

    fuse::audio::HrtfAttenuationCouplingPreflight out{};
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f, out, reason),
               "try_preflight attenuation coupling succeeds when narrowing applies");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "try_preflight coupling writes None when narrowing applies");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, out, reason),
               "try_preflight coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "try_preflight coupling writes BypassPath on bypass");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "EmptyIr")
                   == 0,
               "EmptyIr composite reject reason has stable label");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::MalformedIr),
                           "MalformedIr")
                   == 0,
               "MalformedIr composite reject reason has stable label");

    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, empty, offset, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "empty IR composite reports EmptyIr reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, valid, offset, fuse::audio::HrtfBinauralRejectReason::None),
               "valid IR composite reports None reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, valid, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF composite reports HrtfDisabled reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, valid, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located composite reports CoLocated reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, offset, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "no-IR composite overload reports EmptyIr");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, malformed, offset, fuse::audio::HrtfBinauralRejectReason::MalformedIr),
               "malformed IR composite reports MalformedIr reject reason");

    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_bypass(
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "HrtfDisabled is a bypass composite reject reason");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_is_bypass(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "EmptyIr is not a bypass composite reject reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_blocks_convolution(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "EmptyIr blocks convolution");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_blocks_convolution(
                   fuse::audio::HrtfBinauralRejectReason::None),
               "None does not block convolution");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite preflight carries EmptyIr reason on stub path");
    expectTrue(!stub_preflight.should_skip(), "EmptyIr composite preflight is not bypassed");
    expectTrue(stub_preflight.should_skip_convolution(),
               "EmptyIr composite preflight skips convolution");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite preflight carries None reason");

    fuse::audio::HrtfBinauralPreflight out{};
    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, out, reason),
               "try_preflight composite succeeds on valid spatial path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "try_preflight composite writes None on valid path");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, out, reason),
               "try_preflight composite fails on bypass");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "try_preflight composite writes HrtfDisabled on bypass");
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f, out, reason),
               "try_preflight composite succeeds on ILD/ITD stub path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "try_preflight composite writes EmptyIr on stub path");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::EmptyIr,
               "empty IR reports EmptyIr reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::EmptyIr),
                           "empty_ir")
                   == 0,
               "empty_ir label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reports None reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::None),
                           "none")
                   == 0,
               "none label");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR reports MalformedIr reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                       fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects for MalformedIr");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
               "IR preflight carries reject reason");
    expectTrue(preflight.can_convolve()
                   == (preflight.reason == fuse::audio::HrtfIrRejectReason::None),
               "can_convolve tracks reject reason");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source reports None pan-path reject");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF reports HrtfDisabled pan-path reject");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source reports CoLocated pan-path reject");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "co_located label");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "pan-path rejects for HrtfDisabled");

    const fuse::audio::HrtfIrStub empty{};
    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "pan-path preflight carries reject reason on valid path");
    expectTrue(preflight.can_spatial_pan()
                   == (preflight.reason == fuse::audio::HrtfPanPathRejectReason::None),
               "can_spatial_pan tracks pan-path reject reason");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation reports None coupling reject");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path reports BypassPath coupling reject");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation coupling reject");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "unity_attenuation")
                   == 0,
               "unity_attenuation label");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "coupling rejects for BypassPath");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "coupling preflight carries reject reason on valid path");
    expectTrue(preflight.can_narrow()
                   == (preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "can_narrow tracks coupling reject reason");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, empty, offset, 0.2f, 0.3f)
                   == fuse::audio::HrtfBinauralRejectReason::ConvolutionEmptyIr,
               "empty IR composite reports ConvolutionEmptyIr");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, offset, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::NarrowingUnityAttenuation,
               "unity attenuation composite reports NarrowingUnityAttenuation");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, valid, offset, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::PanBypassDisabled,
               "disabled HRTF composite reports PanBypassDisabled");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, valid, co_located, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::PanBypassCoLocated,
               "co-located composite reports PanBypassCoLocated");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::PanBypassDisabled),
                           "pan_bypass_disabled")
                   == 0,
               "pan_bypass_disabled label");
    expectTrue(fuse::audio::is_blocking_hrtf_binaural_reject_reason(
                   fuse::audio::HrtfBinauralRejectReason::PanBypassDisabled),
               "PanBypassDisabled is blocking");
    expectTrue(!fuse::audio::is_blocking_hrtf_binaural_reject_reason(
                   fuse::audio::HrtfBinauralRejectReason::ConvolutionEmptyIr),
               "ConvolutionEmptyIr is not blocking");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, empty, offset, 0.2f, 0.3f,
                   fuse::audio::HrtfBinauralRejectReason::ConvolutionEmptyIr),
               "composite rejects for ConvolutionEmptyIr");

    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid composite path reports None reject reason");
    expectTrue(preflight.ir.reason == fuse::audio::HrtfIrRejectReason::None,
               "composite carries IR reject reason");
    expectTrue(preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "composite carries pan-path reject reason");
    expectTrue(preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "composite carries coupling reject reason");
}

void testHrtfIrRejectReasonGuards() {
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::None),
                           "none")
                   == 0,
               "None IR reject label");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "MalformedIr IR reject label");

    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR classifies as null_samples");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR classifies as none");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR classifies as malformed_ir");

    fuse::audio::HrtfIrPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, preflight),
               "try_preflight_hrtf_ir passes for valid IR");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight reason is None");
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, preflight),
               "try_preflight_hrtf_ir rejects empty IR");
    expectTrue(fuse::audio::hrtf_ir_preflight_rejects_for_reason(
                   preflight, fuse::audio::HrtfIrRejectReason::NullSamples),
               "empty IR preflight rejects for null_samples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_skip_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF classifies as hrtf_disabled");
    expectTrue(fuse::audio::classify_hrtf_pan_path_skip_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source classifies as co_located");
    expectTrue(fuse::audio::classify_hrtf_pan_path_skip_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no skip reject");

    expectTrue(fuse::audio::classify_hrtf_pan_path_convolution_reject(true, empty, offset)
                   == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "empty IR classifies as empty_ir for convolution");
    expectTrue(fuse::audio::classify_hrtf_pan_path_convolution_reject(true, valid, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "valid IR has no convolution reject");
    expectTrue(fuse::audio::classify_hrtf_pan_path_convolution_reject(false, valid, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF blocks convolution before IR check");

    fuse::audio::HrtfPanPathPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, preflight),
               "try_preflight_hrtf_pan_path passes for valid spatial path");
    expectTrue(preflight.skipReason == fuse::audio::HrtfPanPathRejectReason::None,
               "valid spatial path has no skip reason");
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "valid IR has no convolution reject reason");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, preflight),
               "try_preflight_hrtf_pan_path rejects disabled HRTF");
    expectTrue(fuse::audio::hrtf_pan_path_preflight_skips_for_reason(
                   preflight, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF pan-path preflight skips for hrtf_disabled");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path classifies as bypass_path");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation classifies as unity_attenuation");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject");

    fuse::audio::HrtfAttenuationCouplingPreflight preflight{};
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, preflight),
               "try_preflight rejects bypass path");
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass coupling preflight reason is bypass_path");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_preflight_rejects_for_reason(
                   preflight, fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass coupling preflight rejects for bypass_path");

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f, preflight),
               "try_preflight rejects unity attenuation");
    expectTrue(preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity coupling preflight reason is unity_attenuation");

    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, preflight),
               "try_preflight passes reduced attenuation on spatial path");
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight reason is None on valid path");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "empty_ir")
                   == 0,
               "EmptyIr composite reject label");

    expectTrue(fuse::audio::classify_hrtf_binaural_skip_reject(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite skip classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite convolution classify marks empty IR");
    expectTrue(fuse::audio::classify_hrtf_binaural_narrowing_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite narrowing classify marks unity attenuation");

    fuse::audio::HrtfBinauralPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, preflight),
               "try_preflight_hrtf_binaural passes spatial stub path");
    expectTrue(preflight.skipReason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub composite skip reason is None");
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite convolution reason is None");
    expectTrue(preflight.narrowingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "reduced attenuation composite narrowing reason is None");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, preflight),
               "try_preflight_hrtf_binaural rejects disabled HRTF");
    expectTrue(fuse::audio::hrtf_binaural_preflight_skips_for_reason(
                   preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "composite preflight skips for hrtf_disabled");
    expectTrue(fuse::audio::hrtf_binaural_preflight_convolution_rejects_for_reason(
                   preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "composite convolution rejects for hrtf_disabled");

    const fuse::audio::HrtfBinauralPreflight empty_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 1.f, 1.f);
    expectTrue(fuse::audio::hrtf_binaural_preflight_convolution_rejects_for_reason(
                   empty_ir_preflight, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "empty IR composite convolution rejects for empty_ir");
    expectTrue(empty_ir_preflight.convolutionRejectReason
                   == fuse::audio::classify_hrtf_binaural_convolution_reject(true, empty, offset),
               "composite convolution reason matches classifier");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(fuse::audio::hrtf_binaural_preflight_skips_for_reason(
                   co_located_preflight, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located composite preflight skips for co_located");

    const fuse::audio::HrtfBinauralPreflight unity_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(fuse::audio::hrtf_binaural_preflight_narrowing_rejects_for_reason(
                   unity_preflight, fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "unity attenuation composite narrowing rejects for unity_attenuation");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "null_samples")
                   == 0,
               "null_samples reject label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_is_blocking(fuse::audio::HrtfIrRejectReason::None),
               "none IR reject reason is not blocking");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed_ir");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "malformed_ir reject label");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "empty IR try_preflight rejects convolution");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight empty IR reports null_samples");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "valid IR try_preflight allows convolution");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR try_preflight reports none");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight struct carries IR reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &reason),
               "optional-reason preflight returns true for valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "optional-reason preflight reports none for valid IR");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
                           "hrtf_disabled")
                   == 0,
               "hrtf_disabled pan-path reject label");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "co_located pan-path reject label");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, empty, offset, reason),
               "spatial offset passes pan-path try_preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial offset pan-path try_preflight reports none");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, empty, offset, reason),
               "disabled HRTF fails pan-path try_preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF pan-path try_preflight reports hrtf_disabled");

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "ILD/ITD stub path preflight reason is none");
    expectTrue(!fuse::audio::hrtf_pan_path_reject_reason_is_blocking(stub_preflight.reason),
               "ILD/ITD stub pan-path reject is not blocking");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
                           "bypass_path")
                   == 0,
               "bypass_path coupling reject label");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "unity_attenuation")
                   == 0,
               "unity_attenuation coupling reject label");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    const fuse::audio::HrtfAttenuationCoupling coupling{};
    const fuse::audio::BinauralPanParams params{};
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, coupling, params, reason),
               "reduced attenuation passes coupling try_preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation coupling try_preflight reports none");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, coupling, params, reason),
               "bypass path fails coupling try_preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass coupling try_preflight reports bypass_path");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight struct carries none reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity_attenuation coupling reject is blocking");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfAttenuationCoupling coupling{};
    const fuse::audio::BinauralPanParams params{};

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "enabled offset composite has no reject");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite rejects with hrtf_disabled");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                           "hrtf_disabled")
                   == 0,
               "composite hrtf_disabled reject label");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite rejects with co_located");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f, coupling,
                                                        params, reason),
               "stub path composite try_preflight allows spatial pan");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "stub path composite try_preflight reports none");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f,
                                                         coupling, params, reason),
               "disabled composite try_preflight rejects spatial pan");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite try_preflight reports hrtf_disabled");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite preflight struct carries none reject reason");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite bundles empty-IR reject reason");
    expectTrue(stub_preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "composite bundles pan-path reject reason");
    expectTrue(stub_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "composite bundles coupling reject reason");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_is_blocking(stub_preflight.reason),
               "stub composite reject reason is not blocking");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "bypass composite preflight carries hrtf_disabled reason");
    expectTrue(fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f, coupling,
                                                    params, &reason),
               "valid IR unity attenuation composite preflight allows spatial pan");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite optional-reason preflight reports none");
}

void testHrtfIrRejectReasonPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "null_samples reject label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_is_blocking(fuse::audio::HrtfIrRejectReason::None),
               "None is not blocking");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR is malformed_ir");
    expectTrue(fuse::audio::hrtf_ir_reject_reason_is_blocking(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed_ir is blocking");

    fuse::audio::HrtfIrPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, preflight),
               "try_preflight_hrtf_ir passes on valid IR");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight carries None reason on valid IR");
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, preflight),
               "try_preflight_hrtf_ir rejects empty IR");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight carries null_samples on empty IR");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan reject");

    fuse::audio::HrtfPanPathPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, preflight),
               "try_preflight pan path passes on valid spatial source");
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial pan preflight reason is None");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, preflight),
               "try_preflight pan path rejects disabled HRTF");
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled pan preflight carries hrtf_disabled reason");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
               "co_located pan reject label");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    fuse::audio::HrtfAttenuationCouplingPreflight preflight{};
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects narrowing");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject");

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f, preflight),
               "try_preflight coupling rejects unity attenuation");
    expectTrue(preflight.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity coupling preflight carries unity_attenuation reason");
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, preflight),
               "try_preflight coupling passes when narrowing applies");
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing coupling preflight reason is None");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath)) == "bypass_path",
               "bypass_path coupling reject label");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_pan_reject(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite pan reject for disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite convolution reject for empty IR");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR has no composite convolution reject");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, valid, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located source blocks convolution via pan reject");
    expectTrue(fuse::audio::classify_hrtf_binaural_attenuation_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite attenuation reject for unity gain");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.panRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "stub path has no pan reject reason");
    expectTrue(stub_preflight.convolutionRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "stub path carries empty_ir convolution reject");
    expectTrue(stub_preflight.attenuationRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "reduced attenuation has no composite attenuation reject");

    const fuse::audio::HrtfBinauralPreflight unity_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(unity_preflight.attenuationRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity composite preflight carries unity_attenuation reject");

    fuse::audio::HrtfBinauralPreflight try_preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f,
                                                        try_preflight),
               "try_preflight composite passes on spatial source");
    expectTrue(try_preflight.panRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "try_preflight composite pan reject is None");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.2f, 0.3f,
                                                         try_preflight),
               "try_preflight composite rejects disabled HRTF");
    expectTrue(try_preflight.panRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "try_preflight composite carries hrtf_disabled pan reject");

    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_pan_blocking(
                   fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co_located is pan-blocking");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_convolution_blocking(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "empty_ir is convolution-blocking");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_is_convolution_blocking(
                   fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "unity_attenuation does not block convolution");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::MalformedIr)) == "malformed_ir",
               "malformed_ir composite reject label");
}

void testHrtfIrRejectReasonPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR classifies as NullSamples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "NullSamples")
                   == 0,
               "NullSamples reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reports no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_is_blocking(
                   fuse::audio::HrtfIrRejectReason::None),
               "None IR reject reason is not blocking");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR classifies as MalformedIr");
    expectTrue(fuse::audio::hrtf_ir_reject_reason_is_blocking(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "MalformedIr reject reason is blocking");

    fuse::audio::HrtfIrPreflight preflight{};
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, preflight, reason),
               "try_preflight_hrtf_ir rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir carries NullSamples reason");
    expectTrue(preflight.rejectReason == reason,
               "preflight struct carries IR reject reason");

    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, preflight, reason),
               "try_preflight_hrtf_ir accepts valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR try_preflight reports None");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject reason");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF classifies as HrtfDisabled");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source classifies as CoLocated");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
                   == 0,
               "CoLocated pan-path reject reason label");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason_is_blocking(
                   fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "HrtfDisabled pan-path reject reason is blocking");

    fuse::audio::HrtfPanPathPreflight preflight{};
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, preflight, reason),
               "try_preflight_hrtf_pan_path accepts spatial path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial path try_preflight reports None");
    expectTrue(preflight.rejectReason == reason,
               "preflight struct carries pan-path reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, preflight, reason),
               "try_preflight_hrtf_pan_path rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF try_preflight carries HrtfDisabled reason");

    const fuse::audio::HrtfPanPathPreflight empty_ir_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(empty_ir_preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR does not reject pan-path preflight");
    expectTrue(empty_ir_preflight.can_spatial_pan(),
               "empty IR still spatial-pans via ILD/ITD stub");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path classifies as BypassPath");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation classifies as UnityAttenuation");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation on spatial path has no reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation")
                   == 0,
               "UnityAttenuation coupling reject reason label");
    expectTrue(!fuse::audio::hrtf_attenuation_coupling_reject_reason_is_blocking(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "None coupling reject reason is not blocking");

    fuse::audio::HrtfAttenuationCouplingPreflight preflight{};
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, preflight, reason),
               "try_preflight coupling accepts narrowing path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path try_preflight reports None");
    expectTrue(preflight.rejectReason == reason,
               "preflight struct carries coupling reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, preflight, reason),
               "try_preflight coupling rejects bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path try_preflight carries BypassPath reason");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_binaural_pan_reject(true, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "composite pan reject is None on valid path");
    expectTrue(fuse::audio::classify_hrtf_binaural_pan_reject(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite pan reject maps HrtfDisabled");
    expectTrue(fuse::audio::classify_hrtf_binaural_pan_reject(true, co_located)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "composite pan reject maps CoLocated");

    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, empty, offset)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite convolution reject maps EmptyIr");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite convolution reject is None");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(false, valid, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF blocks convolution before empty-IR check");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(true, malformed, offset)
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "composite convolution reject maps MalformedIr");

    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(true, offset, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "composite coupling reject maps UnityAttenuation");
    expectTrue(fuse::audio::classify_hrtf_binaural_coupling_reject(false, offset, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF blocks coupling before attenuation check");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "EmptyIr")
                   == 0,
               "EmptyIr composite reject reason label");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_blocking(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "EmptyIr composite reject reason is blocking");

    const fuse::audio::HrtfBinauralPreflight composite =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(composite.panRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite preflight carries None pan reject on stub path");
    expectTrue(composite.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite preflight carries EmptyIr convolution reject");
    expectTrue(composite.couplingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "composite preflight carries None coupling reject on reduced attenuation");

    const fuse::audio::HrtfBinauralPreflight unity =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(unity.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite convolution reject is None");
    expectTrue(unity.couplingRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite coupling reject is UnityAttenuation");

    fuse::audio::HrtfBinauralPreflight out{};
    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, out,
                                                        reason),
               "try_preflight composite accepts spatial stub path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub try_preflight reports None pan reject");
    expectTrue(out.panRejectReason == reason,
               "try_preflight composite carries pan reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, out,
                                                         reason),
               "try_preflight composite rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF try_preflight carries HrtfDisabled reason");
}

void testHrtfIrRejectReasonPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR rejects with null_samples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "null_samples")
                   == 0,
               "null_samples label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::None),
               "valid IR rejects_for_reason None");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR rejects with malformed_ir");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "malformed_ir label");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight carries IR reject reason");
    expectTrue(!preflight.can_convolve(), "reject reason blocks convolution");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir reports null_samples");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir succeeds on valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir clears reason on success");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "co_located label");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "rejects_for_reason matches disabled HRTF");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial pan-path preflight has no reject reason");
    expectTrue(preflight.can_spatial_pan(), "no reject reason allows spatial pan");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, offset, reason),
               "try_preflight pan path succeeds on valid offset");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "try_preflight pan path clears reason on success");
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, offset, reason),
               "try_preflight pan path fails when disabled");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "try_preflight pan path reports hrtf_disabled");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "unity_attenuation")
                   == 0,
               "unity_attenuation label");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "coupling rejects_for_reason matches bypass");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight has no reject reason");
    expectTrue(preflight.can_narrow(), "no reject reason allows narrowing");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 0.25f, 0.35f, reason),
               "try_preflight coupling succeeds on reduced attenuation");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "try_preflight coupling clears reason on success");
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight coupling fails on bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "try_preflight coupling reports bypass_path");
}

void testHrtfBinauralCompositeRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.spatialPanReject == fuse::audio::HrtfBinauralRejectReason::None,
               "stub path has no spatial-pan reject");
    expectTrue(stub_preflight.convolutionReject == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite rejects convolution");
    expectTrue(stub_preflight.narrowingReject == fuse::audio::HrtfBinauralRejectReason::None,
               "reduced attenuation has no narrowing reject");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(stub_preflight)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "classify convolution reject on empty IR");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "empty_ir")
                   == 0,
               "empty_ir composite label");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.convolutionReject == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite allows convolution");
    expectTrue(conv_preflight.narrowingReject
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite rejects narrowing");
    expectTrue(fuse::audio::classify_hrtf_binaural_narrowing_reject(conv_preflight)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "classify narrowing reject on unity attenuation");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.spatialPanReject
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite rejects spatial pan");
    expectTrue(bypass_preflight.convolutionReject
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite rejects convolution");
    expectTrue(bypass_preflight.narrowingReject
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "bypass composite rejects narrowing via bypass_path");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.spatialPanReject
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite rejects spatial pan");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::CoLocated),
                           "co_located")
                   == 0,
               "co_located composite label");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfBinauralPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_binaural(true, malformed, offset, 1.f, 1.f);
    expectTrue(malformed_preflight.convolutionReject
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite rejects convolution");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural_spatial_pan(
                   true, valid, offset, 0.2f, 0.3f, reason),
               "try_preflight composite spatial pan succeeds on valid path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "try_preflight composite spatial pan clears reason");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural_convolution(
                   true, empty, offset, 1.f, 1.f, reason),
               "try_preflight composite convolution fails on empty IR");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "try_preflight composite convolution reports empty_ir");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural_narrowing(
                   true, valid, offset, 1.f, 1.f, reason),
               "try_preflight composite narrowing fails on unity attenuation");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "try_preflight composite narrowing reports unity_attenuation");
}

void testHrtfIrRejectReasonPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;

    expectTrue(!fuse::audio::preflight_hrtf_ir_ready(empty, &reason),
               "empty IR fails IR-ready preflight");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reports null_samples reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(reason), "null_samples") == 0,
               "null_samples reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "valid IR passes try_preflight_hrtf_ir");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reports no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_blocks_convolution(reason),
               "none IR reject reason does not block convolution");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(malformed, reason),
               "malformed IR fails try_preflight_hrtf_ir");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR reports malformed_ir reject reason");
    expectTrue(fuse::audio::hrtf_ir_reject_reason_blocks_convolution(reason),
               "malformed_ir blocks convolution");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(fuse::audio::classify_hrtf_ir_reject(preflight) == preflight.reason,
               "classify_hrtf_ir_reject matches stored preflight reason");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;

    expectTrue(fuse::audio::preflight_hrtf_pan_path_ready(true, valid, offset, &reason),
               "enabled offset source passes pan-path ready preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial pan path reports no reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "disabled HRTF fails pan-path try preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF reports hrtf_disabled reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(reason), "hrtf_disabled")
                   == 0,
               "hrtf_disabled pan-path reject reason label");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason_blocks_spatial_pan(reason),
               "hrtf_disabled blocks spatial pan");

    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(true, valid, co_located, reason),
               "co-located source fails pan-path try preflight");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source reports co_located reject reason");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(preflight) == preflight.reason,
               "classify_hrtf_pan_path_reject matches stored preflight reason");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;

    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling_ready(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, {}, {}, &reason),
               "reduced attenuation passes coupling-ready preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing path reports no coupling reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "bypass path fails coupling try preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path reports bypass_path coupling reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(reason),
                           "bypass_path")
                   == 0,
               "bypass_path coupling reject reason label");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason_blocks_narrowing(reason),
               "bypass_path blocks narrowing");

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f, reason),
               "unity attenuation fails coupling try preflight");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports unity_attenuation coupling reject reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub, 0.2f,
                                                         0.3f);
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(preflight) == preflight.reason,
               "classify_hrtf_attenuation_coupling_reject matches stored preflight reason");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;

    expectTrue(fuse::audio::preflight_hrtf_binaural_ready(true, empty, offset, 0.2f, 0.3f, {}, {},
                                                          &reason),
               "stub path passes composite binaural ready preflight");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite preflight reports empty_ir reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(reason), "empty_ir") == 0,
               "empty_ir composite reject reason label");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_blocks_spatial_pan(reason),
               "empty_ir does not block spatial pan");

    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f, reason),
               "valid IR with unity attenuation passes composite try preflight");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation reports unity_attenuation composite reject reason");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_blocks_spatial_pan(reason),
               "unity_attenuation does not block spatial pan");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, reason),
               "disabled HRTF fails composite try preflight");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF reports hrtf_disabled composite reject reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_blocks_spatial_pan(reason),
               "hrtf_disabled blocks spatial pan");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_blocks_convolution(reason),
               "hrtf_disabled blocks convolution");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f,
                                                         reason),
               "co-located source fails composite try preflight");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located source reports co_located composite reject reason");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(stub_preflight) == stub_preflight.reason,
               "classify_hrtf_binaural_reject matches stored composite reason");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(stub_preflight)
                   == fuse::audio::HrtfBinauralConvolutionRejectReason::EmptyIr,
               "composite convolution reject reason is empty_ir on stub path");
    expectTrue(std::strcmp(
                   fuse::audio::hrtf_binaural_convolution_reject_reason_label(
                       fuse::audio::HrtfBinauralConvolutionRejectReason::EmptyIr),
                   "empty_ir")
                   == 0,
               "empty_ir convolution reject reason label");
    expectTrue(fuse::audio::classify_hrtf_binaural_narrowing_reject(stub_preflight)
                   == fuse::audio::HrtfBinauralNarrowingRejectReason::None,
               "reduced attenuation composite narrowing reject reason is none");

    const fuse::audio::HrtfBinauralPreflight unity_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(fuse::audio::classify_hrtf_binaural_narrowing_reject(unity_preflight)
                   == fuse::audio::HrtfBinauralNarrowingRejectReason::UnityAttenuation,
               "unity attenuation composite narrowing reject reason");
    expectTrue(std::strcmp(
                   fuse::audio::hrtf_binaural_narrowing_reject_reason_label(
                       fuse::audio::HrtfBinauralNarrowingRejectReason::UnityAttenuation),
                   "unity_attenuation")
                   == 0,
               "unity_attenuation narrowing reject reason label");
}

void testHrtfIrRejectReasonPreflight() {
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::None),
                           "none")
                   == 0,
               "IR reject label for none");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir")
                   == 0,
               "IR reject label for malformed_ir");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_is_blocking(
                   fuse::audio::HrtfIrRejectReason::None),
               "none is not blocking");

    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR rejects with null_samples");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR rejects with malformed_ir");
    expectTrue(fuse::audio::hrtf_ir_reject_reason_is_blocking(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed_ir is blocking");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "preflight carries IR reject reason");
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty_preflight)
                   == empty_preflight.rejectReason,
               "classify from preflight matches stored reason");

    fuse::audio::HrtfIrRejectReason rejectReason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, rejectReason),
               "try_preflight_hrtf_ir fails on empty IR");
    expectTrue(rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir returns reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir(valid, &rejectReason),
               "preflight_hrtf_ir succeeds on valid IR");
    expectTrue(rejectReason == fuse::audio::HrtfIrRejectReason::None,
               "preflight_hrtf_ir clears reject reason on valid IR");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};

    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                               fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
                           "hrtf_disabled")
                   == 0,
               "pan-path reject label for hrtf_disabled");

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan-path reject");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF rejects pan path");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source rejects pan path");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, empty, offset);
    expectTrue(bypass_preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "pan-path preflight stores reject reason");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(bypass_preflight)
                   == bypass_preflight.rejectReason,
               "classify from pan-path preflight matches stored reason");

    fuse::audio::HrtfPanPathRejectReason rejectReason =
        fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, empty, offset, rejectReason),
               "try_preflight_hrtf_pan_path fails when disabled");
    expectTrue(rejectReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "try_preflight_hrtf_pan_path returns reject reason");
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, empty, offset, rejectReason),
               "try_preflight_hrtf_pan_path succeeds on valid path");
    expectTrue(rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "try_preflight_hrtf_pan_path clears reject reason on valid path");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "unity_attenuation")
                   == 0,
               "attenuation reject label for unity_attenuation");

    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path rejects attenuation coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation has no coupling reject");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.rejectReason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight has no reject reason");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(narrow_preflight)
                   == narrow_preflight.rejectReason,
               "classify from coupling preflight matches stored reason");

    const fuse::audio::HrtfAttenuationCoupling coupling{};
    const fuse::audio::BinauralPanParams params{};
    fuse::audio::HrtfAttenuationCouplingRejectReason rejectReason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, coupling, params, rejectReason),
               "try_preflight attenuation coupling fails on bypass");
    expectTrue(rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "try_preflight attenuation coupling returns bypass reject");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfAttenuationCoupling coupling{};
    const fuse::audio::BinauralPanParams params{};

    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "empty_ir")
                   == 0,
               "composite reject label for empty_ir");

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.spatialPanRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub path has no spatial-pan reject");
    expectTrue(stub_preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite rejects convolution");
    expectTrue(stub_preflight.narrowingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "reduced attenuation does not reject narrowing");
    expectTrue(fuse::audio::classify_hrtf_binaural_convolution_reject(stub_preflight)
                   == stub_preflight.convolutionRejectReason,
               "classify convolution reject matches stored reason");
    expectTrue(fuse::audio::classify_hrtf_binaural_narrowing_reject(stub_preflight)
                   == stub_preflight.narrowingRejectReason,
               "classify narrowing reject matches stored reason");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite can convolve");
    expectTrue(conv_preflight.narrowingRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite rejects narrowing");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.spatialPanRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite rejects spatial pan");
    expectTrue(bypass_preflight.convolutionRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite rejects convolution");
    expectTrue(bypass_preflight.narrowingRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "bypass composite rejects narrowing via bypass path");
    expectTrue(fuse::audio::classify_hrtf_binaural_spatial_pan_reject(bypass_preflight)
                   == bypass_preflight.spatialPanRejectReason,
               "classify spatial-pan reject matches stored reason");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.spatialPanRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite rejects spatial pan");
    expectTrue(co_located_preflight.narrowingRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::BypassPath,
               "co-located composite rejects narrowing via bypass path");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfBinauralPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_binaural(true, malformed, offset, 1.f, 1.f);
    expectTrue(malformed_preflight.convolutionRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite rejects convolution");

    const fuse::audio::HrtfBinauralPreflight bypass_narrow_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f);
    expectTrue(bypass_narrow_preflight.can_narrow_spatial_image(),
               "bypass + reduced attenuation still narrows when spatial path active");
    expectTrue(bypass_narrow_preflight.narrowingRejectReason
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "active spatial path with reduced attenuation has no narrowing reject");

    fuse::audio::HrtfBinauralRejectReason rejectReason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f, coupling,
                                                        params, rejectReason),
               "try_preflight composite succeeds on valid path");
    expectTrue(rejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "try_preflight composite clears reject reason on valid path");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, coupling,
                                                         params, rejectReason),
               "try_preflight composite fails when disabled");
    expectTrue(rejectReason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "try_preflight composite returns spatial-pan reject reason");
    expectTrue(!fuse::audio::hrtf_binaural_reject_reason_is_blocking(
                   fuse::audio::HrtfBinauralRejectReason::None),
               "composite none is not blocking");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason_is_blocking(
                   fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "composite co_located is blocking");
}

void testHrtfBinauralPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub composite preflight has no bypass reject reason");
    expectTrue(stub_preflight.ir.emptyIr, "composite preflight carries empty-IR diagnostics");
    expectTrue(stub_preflight.panPath.path == fuse::audio::HrtfPanPath::IldItdStub,
               "composite preflight selects ILD/ITD stub for empty IR");
    expectTrue(stub_preflight.can_spatial_pan(), "composite preflight allows spatial pan on stub path");
    expectTrue(!stub_preflight.can_convolve(), "composite preflight cannot convolve with empty IR");
    expectTrue(stub_preflight.can_narrow_spatial_image(),
               "reduced attenuation narrows via composite preflight");
    expectTrue(!stub_preflight.is_bypass(), "enabled offset source is not bypassed");
    expectTrue(fuse::audio::can_apply_hrtf_binaural_pan(stub_preflight),
               "can_apply_hrtf_binaural_pan mirrors can_spatial_pan");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural(stub_preflight),
               "should_skip_hrtf_binaural false on spatial stub path");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.can_convolve(), "valid IR composite preflight can convolve");
    expectTrue(!conv_preflight.can_narrow_spatial_image(),
               "unity attenuation skips narrowing in composite preflight");
    expectTrue(conv_preflight.attenuationCoupling.unityAttenuation,
               "composite preflight marks unity attenuation");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite preflight carries hrtf_disabled reject reason");
    expectTrue(bypass_preflight.is_bypass(), "disabled HRTF composite preflight is bypass");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass_preflight),
               "should_skip_hrtf_binaural true on bypass");
    expectTrue(!fuse::audio::can_apply_hrtf_binaural_pan(bypass_preflight),
               "bypass composite preflight cannot spatial-pan");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight carries co_located reject reason");
    expectTrue(co_located_preflight.panPath.coLocated,
               "composite preflight marks co-located source");
    expectTrue(co_located_preflight.is_bypass(), "co-located composite preflight bypasses");

    const fuse::audio::HrtfBinauralPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.25f, 0.35f);
    expectTrue(no_ir_preflight.panPath.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR composite overload selects ILD/ITD stub");
    expectTrue(no_ir_preflight.path()
                   == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "composite path matches resolve_hrtf_pan_path");

    const fuse::audio::BinauralPanGains from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(stub_preflight, offset);
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 0.2f, 0.3f);
    expectNear(from_preflight.left, coupled.left, 1e-5f,
               "from_preflight matches coupled helper on stub path");
    expectNear(from_preflight.right, coupled.right, 1e-5f,
               "from_preflight matches coupled helper on stub path");

    const fuse::audio::BinauralPanGains bypass_gains =
        fuse::audio::compute_binaural_pan_gains_from_preflight(bypass_preflight, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass_gains),
               "from_preflight returns centre pan on bypass");

    const fuse::audio::BinauralPanGains unity_gains =
        fuse::audio::compute_binaural_pan_gains_from_preflight(conv_preflight, offset);
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);
    expectNear(unity_gains.left, wide.left, 1e-5f,
               "unity composite preflight preserves lateral gains");
    expectNear(unity_gains.right, wide.right, 1e-5f,
               "unity composite preflight preserves lateral gains");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(
                   empty, fuse::audio::HrtfIrRejectReason::NullSamples),
               "canonical empty IR rejects with null_samples");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::NullSamples),
                           "null_samples") == 0,
               "null_samples reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(fuse::audio::preflight_hrtf_ir(valid).reason
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR preflight reason is none");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
                   malformed, fuse::audio::HrtfIrRejectReason::MalformedStub),
               "non-null zero-length IR rejects as malformed_stub");
    expectTrue(fuse::audio::preflight_hrtf_ir(malformed).reason
                   == fuse::audio::HrtfIrRejectReason::MalformedStub,
               "malformed IR preflight carries reject reason");
    expectTrue(!fuse::audio::preflight_hrtf_ir(malformed).can_convolve(),
               "malformed IR preflight cannot convolve");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   false, offset, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects with hrtf_disabled");
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects with co_located");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source has no pan reject reason");

    const fuse::audio::HrtfPanPathPreflight disabled =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled pan preflight carries reject reason");
    expectTrue(!disabled.can_spatial_pan(), "disabled pan preflight cannot spatial-pan");

    const fuse::audio::HrtfIrStub empty{};
    const fuse::audio::HrtfPanPathPreflight stub =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "empty IR still spatial-pans without pan reject reason");
    expectTrue(stub.can_spatial_pan(), "empty IR pan preflight can spatial-pan");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "co_located") == 0,
               "co_located pan reject reason label");

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects attenuation coupling");
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity attenuation rejects coupling narrowing");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.2f,
                                                         0.3f);
    expectTrue(bypass.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass coupling preflight carries reject reason");
    expectTrue(!bypass.can_narrow(), "bypass coupling preflight cannot narrow");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrowed.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "spatial non-unity coupling has no reject reason");
    expectTrue(narrowed.can_narrow(), "spatial non-unity coupling can narrow");
    expectTrue(std::strcmp(
                   fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                   "unity_attenuation") == 0,
               "unity_attenuation coupling reject reason label");

void testHrtfBinauralRejectReasonPreflight() {

    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   false, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "composite reject reason marks disabled HRTF");
                   true, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "composite reject reason marks co-located source");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, offset)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "enabled offset source has no composite reject reason");

    const fuse::audio::HrtfBinauralPreflight stub =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub.rejectReason() == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub path has no composite reject reason");
    expectTrue(stub.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite preflight carries empty-IR reject reason");
    expectTrue(stub.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "composite preflight clears pan reject on stub path");
    expectTrue(stub.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "composite preflight allows narrowing with reduced attenuation");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural(stub),
               "composite should_skip false on spatial stub path");

    const fuse::audio::HrtfBinauralPreflight bypass =
        fuse::audio::preflight_hrtf_binaural(false, empty, offset, 0.1f, 0.1f);
    expectTrue(bypass.rejectReason() == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite preflight reject reason mirrors disabled pan path");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass),
               "composite should_skip true on disabled HRTF");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                           "hrtf_disabled") == 0,
               "hrtf_disabled composite reject reason label");

    const float samples[] = {1.f};
    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.rejectReason()
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight carries co_located reject reason");
    expectTrue(co_located_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "co-located composite skips attenuation coupling via bypass path");

    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
               "hrtf_ir_rejects_for_reason matches classify");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(zero_length)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR rejects as malformed_ir");

    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid) == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir") == 0,
               "IR reject reason label for malformed_ir");

    fuse::audio::HrtfIrPreflight preflight{};
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, preflight, reason),
               "try_preflight_hrtf_ir rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir exposes reject reason");
    expectTrue(preflight.rejectReason == reason, "preflight carries reject reason");

    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, preflight, reason),
               "try_preflight_hrtf_ir accepts valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR try_preflight clears reject reason");


    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "pan path rejects_for_reason matches classify");

    fuse::audio::HrtfPanPathPreflight preflight{};
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, empty, offset, preflight, reason),
               "try_preflight_hrtf_pan_path rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "try_preflight exposes pan-path reject reason");
    expectTrue(preflight.convolveRejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "pan-path preflight carries convolve reject reason for empty IR");

    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, empty, offset, preflight, reason),
               "try_preflight accepts spatial stub path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "spatial stub path has no pan-path reject reason");
    expectTrue(preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "preflight carries cleared pan-path reject reason");

    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
               "bypass path rejects coupling with bypass_path");
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation rejects coupling");
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "reduced attenuation has no coupling reject reason");

    fuse::audio::HrtfAttenuationCouplingPreflight preflight{};
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, preflight, reason),
               "try_preflight rejects bypass coupling");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "try_preflight exposes coupling reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
               "coupling reject reason label for unity_attenuation");

    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, preflight, reason),
               "try_preflight accepts narrowing coupling");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing coupling clears reject reason");

void testHrtfBinauralRejectReasonGuards() {

    expectTrue(fuse::audio::classify_hrtf_binaural_reject(false, offset)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "composite classify marks disabled HRTF");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(true, co_located)
               "composite classify marks co-located source");
               "composite rejects_for_reason matches classify");

    fuse::audio::HrtfBinauralPreflight preflight =
    expectTrue(preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub composite preflight has no bypass reject reason");
    expectTrue(preflight.ir.rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite preflight carries IR reject reason");
    expectTrue(preflight.panPath.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
               "composite preflight carries pan-path reject reason");
    expectTrue(preflight.attenuationCoupling.rejectReason
               "composite preflight carries coupling reject reason");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, empty, offset, 0.1f, 0.1f,
                                                         preflight, reason),
               "try_preflight composite rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "try_preflight composite exposes bypass reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                               fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "composite reject reason label for co_located");
    const fuse::audio::BinauralPanGains vec3_coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, offset, 0.3f, 0.4f);
    const fuse::audio::HrtfBinauralPreflight vec3_preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.3f, 0.4f);
    const fuse::audio::BinauralPanGains vec3_from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(vec3_preflight, offset);
    expectNear(vec3_coupled.left, vec3_from_preflight.left, 1e-5f,
               "Vec3 coupled helper routes through composite preflight");
    expectNear(vec3_coupled.right, vec3_from_preflight.right, 1e-5f,

    expectTrue(stub_preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::None,
               "spatial stub composite preflight has none reject reason");
    expectTrue(bypass_preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite preflight carries hrtf_disabled reject reason");
    expectTrue(co_located_preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(bypass_preflight)
                   == bypass_preflight.rejectReason,
               "hrtf_binaural_reject_reason mirrors stored rejectReason");
                   bypass_preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "hrtf_binaural_rejects_for_reason matches disabled bypass");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(stub_preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(stub_preflight.attenuationCoupling.reason
               "composite preflight carries attenuation reject reason on narrow path");
    expectTrue(conv_preflight.attenuationCoupling.reason
               "unity composite preflight carries unity attenuation reject reason");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled)) == "hrtf_disabled",
               "composite reject reason label for hrtf_disabled");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty)
                   == fuse::audio::HrtfIrRejectReason::EmptyIr,
               "empty IR reports EmptyIr reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::EmptyIr),
               "empty IR rejects for EmptyIr");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reports None reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::None),
               "valid IR rejects for None");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR reports MalformedIr reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "malformed_ir") == 0,
               "MalformedIr reject reason has stable label");

    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "IR preflight carries reject reason");
    expectTrue(!preflight.can_convolve(), "malformed IR preflight cannot convolve");
    expectTrue(preflight.can_convolve()
                   == (preflight.reason == fuse::audio::HrtfIrRejectReason::None),
               "can_convolve mirrors reject reason");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(false, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF reports HrtfDisabled reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located source reports CoLocated reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "enabled offset source reports None reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(
                   true, offset, fuse::audio::HrtfPanPathRejectReason::None),
               "enabled offset rejects for None");

    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "pan-path preflight carries reject reason");
    expectTrue(!preflight.can_spatial_pan(), "disabled pan-path preflight cannot spatial-pan");
    expectTrue(preflight.can_spatial_pan()
                   == (preflight.reason == fuse::audio::HrtfPanPathRejectReason::None),
               "can_spatial_pan mirrors reject reason");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path reports BypassPath reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation on spatial path reports None reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "unity_attenuation") == 0,
               "UnityAttenuation reject reason has stable label");

    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries None reject reason");
    expectTrue(preflight.can_narrow(), "reduced attenuation preflight can narrow");
    expectTrue(preflight.can_narrow()
                   == (preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "can_narrow mirrors reject reason");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, empty, offset, 0.2f, 0.3f)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite reports EmptyIr reject reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, valid, offset, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::None,
               "valid IR composite reports None reject reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(false, valid, offset, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite reports HrtfDisabled reject reason");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, valid, co_located, 0.1f, 0.1f)
                   == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite reports CoLocated reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   true, empty, offset, 0.2f, 0.3f,
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "composite rejects for EmptyIr on stub path");

    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite preflight carries primary reject reason");
    expectTrue(preflight.ir.reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
               "composite preflight carries IR reject reason");
    expectTrue(preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "stub path keeps None pan-path reject reason");
    expectTrue(preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation keeps None coupling reject reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   preflight, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "preflight rejects for EmptyIr");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                           "empty_ir") == 0,
               "EmptyIr composite reject reason has stable label");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid convolution path composite reason is None");
    expectTrue(conv_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation stays in coupling sub-preflight");
}

void testHrtfIrRejectReasonPreflights() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::classify_hrtf_ir_reject(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason is NullSamples");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::NullSamples),
               "hrtf_ir_rejects_for_reason matches NullSamples");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reject reason is None");
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonLabel(
                   fuse::audio::HrtfIrRejectReason::None),
               "none") == 0,
               "none IR reject reason label");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "malformed IR reject reason is MalformedIr");
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonLabel(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed_ir") == 0,
               "malformed_ir reject reason label");

    fuse::audio::HrtfIrPreflight preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, preflight),
               "try_preflight_hrtf_ir passes for valid IR");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir reason is None on success");
    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, preflight),
               "try_preflight_hrtf_ir fails for empty IR");
    expectTrue(preflight.rejected(), "empty IR preflight is rejected");
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "try_preflight_hrtf_ir reason is NullSamples on empty IR");
}

void testHrtfPanPathRejectReasonPreflights() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, empty, offset)
                   == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "empty IR pan-path reject reason is EmptyIr");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, valid, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "valid IR pan-path reject reason is None");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(false, valid, offset)
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF pan-path reject reason is HrtfDisabled");
    expectTrue(fuse::audio::classify_hrtf_pan_path_reject(true, valid, co_located)
                   == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located pan-path reject reason is CoLocated");
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonLabel(
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr),
               "empty_ir") == 0,
               "empty_ir pan-path reject reason label");

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "pan-path preflight carries empty-IR reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(true, empty, offset,
                                                             fuse::audio::HrtfPanPathRejectReason::EmptyIr),
               "hrtf_pan_path_rejects_for_reason matches EmptyIr");

    const fuse::audio::HrtfPanPathPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir_preflight.reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "no-IR pan-path preflight reject reason is EmptyIr");
}

void testHrtfAttenuationCouplingRejectReasonPreflights() {
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path coupling reject reason is BypassPath");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation coupling reject reason is UnityAttenuation");
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation coupling reject reason is None");
    expectTrue(std::strcmp(fuse::audio::hrtfAttenuationCouplingRejectReasonLabel(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity_attenuation") == 0,
               "unity_attenuation coupling reject reason label");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowable coupling preflight reject reason is None");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "hrtf_attenuation_coupling_rejects_for_reason matches BypassPath");
}

void testHrtfBinauralRejectReasonPreflights() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "composite stub preflight reject reason is EmptyIr");
    expectTrue(stub_preflight.rejected(), "empty-IR composite preflight is rejected");
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(stub_preflight)
                   == stub_preflight.reason,
               "classify_hrtf_binaural_reject matches preflight reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   stub_preflight, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "hrtf_binaural_rejects_for_reason matches EmptyIr");

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite reject reason is UnityAttenuation");
    expectTrue(std::strcmp(fuse::audio::hrtfBinauralRejectReasonLabel(
                   fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "unity_attenuation") == 0,
               "unity_attenuation composite reject reason label");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled HRTF composite reject reason is HrtfDisabled");

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite reject reason is CoLocated");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfBinauralPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_binaural(true, malformed, offset, 0.2f, 0.3f);
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite reject reason is MalformedIr");

    const fuse::audio::HrtfBinauralPreflight valid_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f);
    expectTrue(valid_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path composite reject reason is None");
    expectTrue(!valid_preflight.rejected(), "valid spatial composite preflight is not rejected");

    fuse::audio::HrtfBinauralPreflight try_preflight{};
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f,
                                                        try_preflight),
               "try_preflight_hrtf_binaural passes for valid spatial path");
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "try_preflight_hrtf_binaural reason is None on success");
    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f,
                                                         try_preflight),
               "try_preflight_hrtf_binaural fails for bypass");
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "try_preflight_hrtf_binaural reason is HrtfDisabled on bypass");
}

void testHrtfIrRejectReasonPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "canonical empty IR reports null_samples reject reason");
    expectTrue(std::string(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
               "null_samples reject reason label");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid)
                   == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reports no reject reason");
    expectTrue(!fuse::audio::is_blocking_hrtf_ir_reject_reason(
                   fuse::audio::HrtfIrRejectReason::None),
               "none IR reject reason is not blocking");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed,
                                                      fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed IR rejects for malformed_ir reason");
    expectTrue(fuse::audio::is_blocking_hrtf_ir_reject_reason(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "malformed_ir reject reason is blocking");

    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::try_preflight_hrtf_ir(valid, reason);
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "try_preflight_hrtf_ir reports none on valid IR");
    expectTrue(preflight.reason == reason, "try_preflight mirrors struct reason field");
    expectTrue(preflight.can_convolve(), "valid IR try_preflight can convolve");
}

void testHrtfPanPathRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, empty, offset)
                   == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "empty IR reports empty_ir pan-path reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_reject_reason(true, valid, offset)
                   == fuse::audio::HrtfPanPathRejectReason::None,
               "valid IR reports no pan-path reject reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(false, valid, offset,
                                                             fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
               "disabled HRTF rejects for hrtf_disabled reason");
    expectTrue(fuse::audio::hrtf_pan_path_rejects_for_reason(true, valid, co_located,
                                                             fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co-located source rejects for co_located reason");
    expectTrue(fuse::audio::is_blocking_hrtf_pan_path_reject_reason(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "co_located pan-path reject reason is blocking");
    expectTrue(!fuse::audio::is_blocking_hrtf_pan_path_reject_reason(
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr),
               "empty_ir pan-path reject reason is not bypass-blocking");

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    const fuse::audio::HrtfPanPathPreflight preflight =
        fuse::audio::try_preflight_hrtf_pan_path(true, valid, offset, reason);
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "try_preflight pan path reports none on valid path");
    expectTrue(preflight.reason == reason, "pan-path try_preflight mirrors struct reason");
    expectTrue(std::string(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr)) == "empty_ir",
               "empty_ir pan-path reject reason label");
}

void testHrtfAttenuationCouplingRejectReasonPreflight() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path reports bypass_path coupling reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports unity_attenuation coupling reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::None),
               "reduced attenuation reports no coupling reject reason");
    expectTrue(fuse::audio::is_blocking_hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
               "unity_attenuation coupling reject reason is blocking");

    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
        fuse::audio::try_preflight_hrtf_attenuation_coupling(
            fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, reason);
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "try_preflight coupling reports none when narrowing applies");
    expectTrue(preflight.reason == reason, "coupling try_preflight mirrors struct reason");
    expectTrue(preflight.can_narrow(), "reduced attenuation try_preflight can narrow");
    expectTrue(std::string(fuse::audio::hrtf_attenuation_coupling_reject_reason_label(
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath)) == "bypass_path",
               "bypass_path coupling reject reason label");
}

void testHrtfBinauralRejectReasonPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, empty, offset, 0.2f, 0.3f)
                   == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite reject reason is empty_ir");
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, valid, offset, 1.f, 1.f)
                   == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite reject reason is unity_attenuation");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(false, valid, offset, 0.1f, 0.1f,
                                                             fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "disabled HRTF composite rejects for hrtf_disabled");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(true, valid, co_located, 0.1f, 0.1f,
                                                             fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "co-located composite rejects for co_located");
    expectTrue(fuse::audio::is_blocking_hrtf_binaural_reject_reason(
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
               "hrtf_disabled composite reject reason blocks spatial pan");
    expectTrue(fuse::audio::is_blocking_hrtf_binaural_convolution_reject_reason(
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
               "empty_ir composite reject reason blocks convolution");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_binaural_reject_reason(true, malformed, offset, 0.2f, 0.3f)
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite reject reason is malformed_ir");

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    const fuse::audio::HrtfBinauralPreflight preflight =
        fuse::audio::try_preflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason);
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "valid spatial path with reduced attenuation reports none composite reject reason");
    expectTrue(preflight.reason == reason, "composite try_preflight mirrors struct reason");
    expectTrue(preflight.can_spatial_pan(), "none composite reject reason allows spatial pan");
    expectTrue(preflight.can_convolve(), "valid IR composite try_preflight can convolve");
    expectTrue(std::string(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::MalformedIr)) == "malformed_ir",
               "malformed_ir composite reject reason label");

    fuse::audio::HrtfBinauralRejectReason stub_reason = fuse::audio::HrtfBinauralRejectReason::None;
    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::try_preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f, stub_reason);
    expectTrue(stub_reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite try_preflight reports empty_ir");
    expectTrue(stub_preflight.panPath.reason
                   == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "composite preflight carries pan-path reject reason");
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "composite preflight carries IR reject reason");
    expectTrue(stub_preflight.attenuationCoupling.reason
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation coupling reject reason remains none on stub path");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHrtfPanPathResolution();
    testShouldUseHrtfIrAlias();
    testItdAndElevationHelpers();
    testIrAwareGuardedPan();
    testSpatialBlendHelper();
    testLerpBinauralPanGains();
    testCoupledPanOneShot();
    testPanPathForPathHelper();
    testEmptyHrtfIrFactoryAndAlias();
    testHrtfPanPathPredicateHelpers();
    testResolveHrtfPanPathWithoutIr();
    testClampHrtfAttenuation();
    testCoLocatedHrtfGuards();
    testHrtfIrStubFactory();
    testBypassPanPathHelpers();
    testUnityHrtfAttenuationGuards();
    testClampHrtfAttenuationCouplingWeight();
    testCoupledPanVec3UsesCoupledForPath();
    testShouldSkipHrtfPanGuards();
    testEmptyIrConvolutionSkipGuards();
    testHrtfPanPathBypassAndIldStubPredicates();
    testSpatialBlendSkipGuards();
    testClampHrtfOcclusionCouplingWeight();
    testAttenuationCouplingForPath();
    testMakeHrtfIrStubFactory();
    testHrtfPanPathBypassGuards();
    testCoLocatedHrtfGuards();
    testBypassPanPathAndAttenuationSkipGuards();
    testUnityHrtfAttenuationGuards();
    testClampHrtfAttenuationCouplingWeight();
    testCoupledPanVec3UsesCoupledForPath();
    testShouldSkipHrtfConvolution();
    testHrtfPanPathSkipAliases();
    testUnitySpatialBlendGuards();
    testApplyHrtfSpatialBlendGuarded();
    testHrtfIrRejectReasonGuards();
    testHrtfIrPreflight();
    testHrtfIrRejectReasonPreflight();
    testHrtfPanPathPreflight();
    testHrtfPanPathRejectReasonPreflight();
    testHrtfAttenuationCouplingPreflight();
    testHrtfAttenuationCouplingRejectReasonPreflight();
    testBinauralPanGainSampleHelpers();
    testHrtfIrPreflightSkipAlias();
    testHrtfPanPathPreflight();
    testHrtfBinauralCompositePreflight();
    testPanPathPreflightApplyHelper();
    testHrtfPanPathRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfAttenuationCouplingPreflight();
    testSpatialBlendForPathGuards();
    testAttenuationCouplingGuardedHelpers();
    testGuardedBinauralPanSampleApply();
    testShouldSkipHrtfIrConvolution();
    testShouldFallbackHrtfToIldItdStub();
    testShouldUseHrtfConvolutionPath();
    testShouldApplyHrtfSpatialPan();
    testIsConvolutionHrtfPanPathAlias();
    testPerScalarUnityHrtfAttenuationGuards();
    testApplyHrtfAttenuationCouplingUnityEarlyOut();
    testEmptyIrFallbackAndNormalizeGuards();
    testApplyBinauralPanForPathToSample();
    testNonUnityAndPreserveSpatialImageGuards();
    testCombinedAttenuationCouplingPredicate();
    testGuardedSpatialBlendHelper();
    testEmptyIrConvolutionGuards();
    testTryResolveHrtfPanPath();
    testHrtfPanPreflight();
    testAttenuationCouplingPreflight();
    testAttenuationCouplingMappingSkipGuard();
    testHrtfIrPreflightGuards();
    testHrtfPanPathPreflightGuards();
    testHrtfAttenuationCouplingPreflightGuards();
    testHrtfSpatialPanPreflight();
    testEmptyIrPreflightGuards();
    testPanPathPreflightGuards();
    testAttenuationCouplingPreflightGuards();
    testPreflightHrtfIrGuards();
    testPreflightHrtfPanPathGuards();
    testPreflightHrtfAttenuationCouplingGuards();
    testPreflightHrtfIr();
    testPreflightHrtfIrConvolution();
    testPreflightHrtfPanPath();
    testPreflightHrtfAttenuationCoupling();
    testHrtfBinauralPanCombinedPreflight();
    testHrtfPreflightRejectReasonLabels();
    testEmptyHrtfIrPreflight();
    testBinauralPanGainSampleHelpers();
    testHrtfPanPathPreflightPredicateAliases();
    testHrtfAttenuationCouplingPreflightSkipAlias();
    testHrtfIrRejectReasonGuards();
    testHrtfPanPathRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfBinauralRejectReasonGuards();
    testHrtfBinauralPreflightPredicateAliases();
    testHrtfIrRejectReasonGuards();
    testHrtfPanPathRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfBinauralRejectReasonGuards();
    testListenerAwareHrtfBinauralPreflight();
    testApplyBinauralPanToSampleFromPreflight();
    testGuardedPanRoutesThroughCompositePreflight();
    testHrtfBinauralRejectReasonGuards();
    testHrtfIrRejectReasons();
    testHrtfPanPathRejectReasons();
    testHrtfAttenuationCouplingRejectReasons();
    testHrtfBinauralRejectReasons();
    testHrtfIrRejectReasonGuards();
    testHrtfPanPathRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfIrRejectReasonPreflight();
    testHrtfPanPathRejectReasonPreflight();
    testHrtfAttenuationCouplingRejectReasonPreflight();
    testHrtfBinauralRejectReasonPreflight();
    testHrtfBinauralCompositeRejectReasonPreflight();
    testHrtfBinauralPreflight();
    testHrtfIrRejectReasonGuards();
    testHrtfIrRejectReasonName();
    testHrtfPanPathRejectReasonGuards();
    testHrtfPanPathRejectReasonName();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonName();
    testHrtfBinauralRejectReasonGuards();
    testHrtfBinauralRejectReasonName();
    testAttenuationCouplingEarlyOut();
    testBinauralGainHelpers();
    testCoLocatedHrtfGuards();
    testEmptyIrConvolutionSkipGuards();
    testAttenuationCouplingSkipGuards();
    testSpatialBlendSkipGuards();
    testClampHrtfOcclusionCouplingWeight();
    testHrtfIrPreflight();
    testHrtfPanPathPreflight();
    testHrtfAttenuationCouplingPreflight();
    testHrtfSpatialPanPreflight();
    testHrtfIrPreflightGuards();
    testHrtfPanPathPreflightGuards();
    testHrtfAttenuationCouplingPreflightGuards();
    testPreflightHrtfIrConvolution();
    testPreflightHrtfSpatialPan();
    testPreflightHrtfAttenuationCoupling();
    testHrtfGuardedPanPreflight();
    testHrtfBinauralPreflightCompositeAliases();
    testCoupledForPathUsesCompositePreflight();
    testHrtfIrRejectReasonPreflight();
    testHrtfPanPathRejectReasonPreflight();
    testHrtfAttenuationCouplingRejectReasonPreflight();
    testHrtfBinauralRejectReasonPreflight();
    testBinauralPanGainSampleHelpers();
    testHrtfIrPreflightSkipAlias();
    testHrtfPanPathPreflightAliases();
    testPanPathPreflightApplyHelper();
    testHrtfAttenuationCouplingPreflightSkipAlias();
    testListenerAwareBinauralPreflight();
    testApplyBinauralPanToSampleFromPreflight();
    testHrtfIrRejectReasonClassifiers();
    testHrtfPanPathRejectReasonClassifiers();
    testHrtfAttenuationCouplingRejectReasonClassifiers();
    testHrtfBinauralRejectReasonClassifiers();
    testHrtfRejectReasonPreflights();
    testHrtfIrRejectReason();
    testHrtfPanPathRejectReason();
    testHrtfAttenuationCouplingRejectReason();
    testHrtfBinauralRejectReason();
    testHrtfBinauralPreflight();
    testHrtfIrRejectReasonPreflights();
    testHrtfPanPathRejectReasonPreflights();
    testHrtfAttenuationCouplingRejectReasonPreflights();
    testHrtfBinauralRejectReasonPreflights();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("test_binaural_hrtf: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "test_binaural_hrtf: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
