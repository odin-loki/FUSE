#include <fuse/audio/binaural_pan.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::should_use_hrtf_ir(valid), "valid IR enables convolution path");
    expectTrue(fuse::audio::should_use_hrtf_ir(valid) == fuse::audio::has_hrtf_ir(valid),
               "should_use_hrtf_ir matches has_hrtf_ir");
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
               "for_path convolution stub matches ILD/ITD until IR wired");
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
}

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
    expectTrue(
        !fuse::audio::should_apply_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),
        "bypass path skips attenuation coupling");
}

void testResolveHrtfPanPathWithoutIr() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, offset)
                   == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload selects ILD/ITD stub");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(false, offset)
                   == fuse::audio::HrtfPanPath::Bypass,
               "no-IR overload bypasses when disabled");
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, fuse::audio::Vec3{})
                   == fuse::audio::HrtfPanPath::Bypass,
               "no-IR overload bypasses co-located source");
}

void testClampHrtfAttenuation() {
    expectNear(fuse::audio::clamp_hrtf_attenuation(-0.5f), 0.f, 1e-5f,
               "negative attenuation clamps to zero");
    expectNear(fuse::audio::clamp_hrtf_attenuation(1.5f), 1.f, 1e-5f,
               "above-unity attenuation clamps to one");
    expectNear(fuse::audio::clamp_hrtf_attenuation(0.4f), 0.4f, 1e-5f,
               "in-range attenuation is preserved");
}

void testAttenuationCouplingForPath() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);

    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        narrowed, fuse::audio::HrtfPanPath::IldItdStub, 0.1f, 0.2f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed)
                   < fuse::audio::compute_pan_spread(wide),
               "for_path coupling narrows spatial image");

    fuse::audio::BinauralPanGains bypassed = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        bypassed, fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f);
    expectNear(bypassed.left, wide.left, 1e-5f, "bypass for_path coupling leaves gains unchanged");
    expectNear(bypassed.right, wide.right, 1e-5f,
               "bypass for_path coupling leaves gains unchanged");

    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled_for_path(
            fuse::audio::HrtfPanPath::Convolution, offset, 0.2f, 0.3f);
    fuse::audio::BinauralPanGains manual =
        fuse::audio::compute_binaural_pan_gains_for_path(fuse::audio::HrtfPanPath::Convolution,
                                                         offset);
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        manual, fuse::audio::HrtfPanPath::Convolution, 0.2f, 0.3f);
    expectNear(coupled.left, manual.left, 1e-5f,
               "coupled_for_path matches manual path gains + coupling");
    expectNear(coupled.right, manual.right, 1e-5f,
               "coupled_for_path matches manual path gains + coupling");
}

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
}

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
}

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
    expectTrue(fuse::audio::should_bypass_hrtf_pan(true, co_located)
                   == fuse::audio::should_skip_hrtf_pan(true, co_located),
               "should_bypass_hrtf_pan matches should_skip_hrtf_pan");
}

void testBypassPanPathAndAttenuationSkipGuards() {
    expectTrue(fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass),
               "bypass path predicate alias");
    expectTrue(fuse::audio::is_bypass_hrtf_pan_path(fuse::audio::HrtfPanPath::Bypass)
                   == fuse::audio::is_hrtf_pan_path_bypass(fuse::audio::HrtfPanPath::Bypass),
               "bypass aliases agree");
    expectTrue(
        fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),
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

void testHrtfIrPreflightGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.empty_ir, "preflight marks empty IR");
    expectTrue(empty_preflight.null_samples, "preflight marks null samples on empty IR");
    expectTrue(empty_preflight.zero_length, "preflight marks zero length on empty IR");
    expectTrue(!empty_preflight.can_use_convolution(), "empty IR preflight rejects convolution");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    const fuse::audio::HrtfIrPreflight null_preflight =
        fuse::audio::preflight_hrtf_ir(null_samples_nonzero_length);
    expectTrue(null_preflight.null_samples, "preflight marks null samples");
    expectTrue(!null_preflight.zero_length, "preflight preserves non-zero length field");
    expectTrue(null_preflight.empty_ir, "null samples preflight is still empty IR");
    expectTrue(!null_preflight.can_use_convolution(),
               "null samples preflight rejects convolution");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid =
        fuse::audio::make_hrtf_ir_stub(samples, static_cast<fuse::u32>(2));
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.empty_ir, "valid IR preflight is not empty");
    expectTrue(!valid_preflight.null_samples, "valid IR preflight has samples");
    expectTrue(!valid_preflight.zero_length, "valid IR preflight has non-zero length");
    expectTrue(valid_preflight.can_use_convolution(), "valid IR preflight enables convolution");
    expectTrue(valid_preflight.can_use_convolution() == fuse::audio::has_hrtf_ir(valid),
               "preflight can_use_convolution matches has_hrtf_ir");
}

void testHrtfPanPathPreflightGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight convolution =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(!convolution.hrtf_disabled, "enabled convolution preflight is not disabled");
    expectTrue(!convolution.co_located, "offset source is not co-located");
    expectTrue(!convolution.empty_ir, "valid IR preflight is not empty");
    expectTrue(convolution.path == fuse::audio::HrtfPanPath::Convolution,
               "valid IR preflight selects convolution");
    expectTrue(convolution.can_apply_spatial_pan(), "convolution preflight applies spatial pan");
    expectTrue(convolution.uses_convolution(), "convolution preflight uses convolution");
    expectTrue(!convolution.uses_ild_itd_stub(), "convolution preflight is not ILD/ITD stub");

    const fuse::audio::HrtfPanPathPreflight ild_stub =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(ild_stub.empty_ir, "empty IR preflight is empty");
    expectTrue(ild_stub.path == fuse::audio::HrtfPanPath::IldItdStub,
               "empty IR preflight selects ILD/ITD stub");
    expectTrue(ild_stub.can_apply_spatial_pan(), "ILD stub preflight applies spatial pan");
    expectTrue(ild_stub.uses_ild_itd_stub(), "ILD stub preflight uses ILD/ITD stub");
    expectTrue(!ild_stub.uses_convolution(), "ILD stub preflight does not use convolution");

    const fuse::audio::HrtfPanPathPreflight bypass_disabled =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass_disabled.hrtf_disabled, "disabled preflight marks HRTF disabled");
    expectTrue(bypass_disabled.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled preflight bypasses pan");
    expectTrue(!bypass_disabled.can_apply_spatial_pan(),
               "disabled preflight skips spatial pan");

    const fuse::audio::HrtfPanPathPreflight bypass_co_located =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(bypass_co_located.co_located, "co-located preflight marks co-located source");
    expectTrue(bypass_co_located.path == fuse::audio::HrtfPanPath::Bypass,
               "co-located preflight bypasses pan");
    expectTrue(!bypass_co_located.can_apply_spatial_pan(),
               "co-located preflight skips spatial pan");

    const fuse::audio::HrtfPanPathPreflight no_ir =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir.empty_ir, "no-IR overload preflight is empty");
    expectTrue(no_ir.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload preflight selects ILD/ITD stub");
    expectTrue(no_ir.path == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "no-IR preflight path matches resolve_hrtf_pan_path");
}

void testHrtfAttenuationCouplingPreflightGuards() {
    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.2f);
    expectTrue(bypass.bypass_path, "bypass coupling preflight marks bypass path");
    expectTrue(!bypass.spatial_path, "bypass coupling preflight is not spatial");
    expectTrue(!bypass.unity_attenuation, "reduced attenuation is non-unity");
    expectTrue(!bypass.can_apply_coupling(), "bypass coupling preflight rejects coupling");
    expectTrue(!bypass.should_narrow(), "bypass coupling preflight does not narrow");

    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(!unity.bypass_path, "spatial unity preflight is not bypass");
    expectTrue(unity.spatial_path, "spatial unity preflight is spatial");
    expectTrue(unity.unity_attenuation, "unity preflight marks unity attenuation");
    expectTrue(!unity.can_apply_coupling(), "unity preflight rejects coupling");
    expectTrue(!unity.should_narrow(), "unity preflight does not narrow");
    expectTrue(!fuse::audio::should_narrow_hrtf_spatial_image(fuse::audio::HrtfPanPath::Convolution,
                                                              1.f, 1.f),
               "preflight should_narrow matches should_narrow_hrtf_spatial_image");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.8f);
    expectTrue(!narrowed.bypass_path, "spatial narrowed preflight is not bypass");
    expectTrue(narrowed.spatial_path, "spatial narrowed preflight is spatial");
    expectTrue(!narrowed.unity_attenuation, "reduced attenuation preflight is non-unity");
    expectTrue(narrowed.can_apply_coupling(), "spatial narrowed preflight applies coupling");
    expectTrue(narrowed.should_narrow(), "spatial narrowed preflight narrows image");
    expectTrue(narrowed.can_apply_coupling()
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.8f),
               "preflight can_apply_coupling matches should_narrow_hrtf_spatial_image");
}

void testBinauralPanGainSampleHelpers() {
    const fuse::audio::BinauralPanGains centre = fuse::audio::make_centre_binaural_pan_gains();
    expectNear(fuse::audio::compute_binaural_pan_energy(centre), 0.5f, 1e-5f,
               "centre pan energy is 0.5 under equal-power stub");

    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
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
    testAttenuationCouplingForPath();
    testMakeHrtfIrStubFactory();
    testHrtfPanPathBypassGuards();
    testCoLocatedHrtfGuards();
    testBypassPanPathAndAttenuationSkipGuards();
    testUnityHrtfAttenuationGuards();
    testClampHrtfAttenuationCouplingWeight();
    testCoupledPanVec3UsesCoupledForPath();
    testHrtfIrPreflightGuards();
    testHrtfPanPathPreflightGuards();
    testHrtfAttenuationCouplingPreflightGuards();
    testBinauralPanGainSampleHelpers();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("test_binaural_hrtf: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "test_binaural_hrtf: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
