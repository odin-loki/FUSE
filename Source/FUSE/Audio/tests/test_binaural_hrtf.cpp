#include <fuse/audio/binaural_pan.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void testShouldSkipHrtfConvolution() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::should_skip_hrtf_convolution(empty),
               "empty IR skips convolution");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(empty)
                   == !fuse::audio::should_use_hrtf_ir(empty),
               "should_skip_hrtf_convolution inverts should_use_hrtf_ir");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(!fuse::audio::should_skip_hrtf_convolution(valid),
               "valid IR does not skip convolution");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::is_nonnull_zero_length_hrtf_ir(malformed),
               "non-null zero-length IR is malformed");
    expectTrue(fuse::audio::should_skip_hrtf_convolution(malformed),
               "malformed IR skips convolution");
    expectTrue(fuse::audio::is_empty_hrtf_ir(malformed),
               "malformed IR is treated as empty");
}

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

    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfPanPath path =
        fuse::audio::resolve_hrtf_pan_path(false, offset);
    expectTrue(fuse::audio::should_skip_hrtf_pan_path(path),
               "disabled HRTF resolves to skippable pan path");
}

void testUnitySpatialBlendGuards() {
    expectTrue(fuse::audio::is_unity_hrtf_spatial_blend(1.f),
               "unity blend at 1.0");
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
}

void testApplyHrtfSpatialBlendGuarded() {
    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    const float wide_spread = fuse::audio::compute_pan_spread(wide);

    fuse::audio::BinauralPanGains unchanged = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(unchanged, 1.f);
    expectNear(unchanged.left, wide.left, 1e-5f,
               "unity guarded blend leaves left gain unchanged");
    expectNear(unchanged.right, wide.right, 1e-5f,
               "unity guarded blend leaves right gain unchanged");

    fuse::audio::BinauralPanGains narrowed = wide;
    fuse::audio::apply_hrtf_spatial_blend_guarded(narrowed, 0.25f);
    expectTrue(fuse::audio::compute_pan_spread(narrowed) < wide_spread,
               "sub-unity guarded blend narrows pan spread");

    fuse::audio::BinauralPanGains coupling_unchanged = wide;
    fuse::audio::apply_hrtf_attenuation_coupling(coupling_unchanged, 1.f, 1.f);
    expectNear(coupling_unchanged.left, wide.left, 1e-5f,
               "unity attenuation coupling is a no-op via spatial blend guard");
    expectNear(coupling_unchanged.right, wide.right, 1e-5f,
               "unity attenuation coupling is a no-op via spatial blend guard");
}

void testHrtfIrPreflight() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.emptyIr, "preflight marks empty IR");
    expectTrue(empty_preflight.nullSamples, "preflight marks null samples");
    expectTrue(empty_preflight.zeroLength, "preflight marks zero length");
    expectTrue(!empty_preflight.malformedIr, "canonical empty IR is not malformed");
    expectTrue(!empty_preflight.can_convolve(), "empty IR preflight cannot convolve");
    expectTrue(!fuse::audio::can_convolve_hrtf_ir(empty_preflight),
               "can_convolve_hrtf_ir mirrors preflight");

    const float samples[] = {0.5f, -0.25f};
    const fuse::audio::HrtfIrStub valid{samples, 2};
    const fuse::audio::HrtfIrPreflight valid_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(!valid_preflight.emptyIr, "valid IR is not empty");
    expectTrue(!valid_preflight.nullSamples, "valid IR has samples");
    expectTrue(!valid_preflight.zeroLength, "valid IR has non-zero length");
    expectTrue(valid_preflight.can_convolve(), "valid IR preflight can convolve");
    expectTrue(fuse::audio::can_convolve_hrtf_ir(valid_preflight)
                   == fuse::audio::should_use_hrtf_ir(valid),
               "preflight can_convolve matches should_use_hrtf_ir");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(malformed_preflight.malformedIr, "preflight marks malformed IR");
    expectTrue(malformed_preflight.emptyIr, "malformed IR is treated as empty");
    expectTrue(!malformed_preflight.can_convolve(), "malformed IR cannot convolve");
}

void testHrtfPanPathPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
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
    expectTrue(disabled_preflight.hrtfDisabled, "preflight marks disabled HRTF");
    expectTrue(disabled_preflight.skipped, "disabled HRTF preflight is skipped");
    expectTrue(!disabled_preflight.can_spatial_pan(), "disabled HRTF cannot spatial-pan");
    expectTrue(disabled_preflight.path == fuse::audio::HrtfPanPath::Bypass,
               "disabled HRTF preflight selects bypass");

    const fuse::audio::HrtfPanPathPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, co_located);
    expectTrue(co_located_preflight.coLocated, "preflight marks co-located source");
    expectTrue(co_located_preflight.skipped, "co-located preflight is skipped");
    expectTrue(!fuse::audio::can_apply_spatial_hrtf_pan(co_located_preflight),
               "co-located source cannot spatial-pan");

    const fuse::audio::HrtfPanPathPreflight no_ir_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, offset);
    expectTrue(no_ir_preflight.path == fuse::audio::HrtfPanPath::IldItdStub,
               "no-IR overload selects ILD/ITD stub");
    expectTrue(no_ir_preflight.path
                   == fuse::audio::resolve_hrtf_pan_path(true, offset),
               "no-IR preflight path matches resolve_hrtf_pan_path");
}

void testHrtfAttenuationCouplingPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(offset);

    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.unityAttenuation, "preflight marks unity attenuation");
    expectTrue(unity_preflight.skipped, "unity attenuation preflight is skipped");
    expectTrue(!unity_preflight.can_narrow(), "unity attenuation cannot narrow");
    expectTrue(!fuse::audio::can_narrow_hrtf_spatial_image(unity_preflight),
               "can_narrow mirrors preflight");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass,
                                                         0.1f, 0.1f);
    expectTrue(bypass_preflight.bypassPath, "preflight marks bypass path");
    expectTrue(bypass_preflight.skipped, "bypass path preflight is skipped");
    expectTrue(!bypass_preflight.can_narrow(), "bypass path cannot narrow");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.can_narrow(), "reduced attenuation can narrow");
    expectTrue(narrow_preflight.spatialBlend < 1.f, "preflight exposes sub-unity spatial blend");
    expectTrue(narrow_preflight.can_narrow()
                   == fuse::audio::should_narrow_hrtf_spatial_image(
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "preflight can_narrow matches should_narrow_hrtf_spatial_image");

    fuse::audio::BinauralPanGains via_preflight = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        via_preflight, fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f);
    fuse::audio::BinauralPanGains manual = wide;
    fuse::audio::apply_hrtf_attenuation_coupling(manual, 0.2f, 0.3f);
    expectNear(via_preflight.left, manual.left, 1e-5f,
               "for_path coupling via preflight matches manual coupling");
    expectNear(via_preflight.right, manual.right, 1e-5f,
               "for_path coupling via preflight matches manual coupling");

    fuse::audio::BinauralPanGains bypassed = wide;
    fuse::audio::apply_hrtf_attenuation_coupling_for_path(
        bypassed, fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f);
    expectNear(bypassed.left, wide.left, 1e-5f,
               "bypass preflight leaves gains unchanged");
    expectNear(bypassed.right, wide.right, 1e-5f,
               "bypass preflight leaves gains unchanged");
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
    expectTrue(!valid_preflight.should_skip_convolution(), "valid IR preflight does not skip convolution");
    expectTrue(!fuse::audio::should_skip_hrtf_ir_convolution(valid_preflight),
               "valid IR should_skip_hrtf_ir_convolution is false");
}

void testHrtfPanPathPreflightPredicateAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(stub_preflight.uses_ild_itd_stub(), "stub preflight selects ILD/ITD path");
    expectTrue(!stub_preflight.should_skip(), "stub preflight is not skipped");
    expectTrue(fuse::audio::uses_ild_itd_stub_hrtf_pan_path(stub_preflight),
               "uses_ild_itd_stub_hrtf_pan_path mirrors struct method");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(stub_preflight),
               "should_skip_hrtf_pan_path_preflight false on stub path");
    expectTrue(fuse::audio::can_convolve_hrtf_pan_path(stub_preflight)
                   == stub_preflight.can_convolve(),
               "can_convolve_hrtf_pan_path mirrors struct method");

    const fuse::audio::HrtfPanPathPreflight conv_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, valid, offset);
    expectTrue(fuse::audio::can_convolve_hrtf_pan_path(conv_preflight),
               "convolution pan-path preflight can convolve");
    expectTrue(!conv_preflight.uses_ild_itd_stub(), "convolution preflight does not use ILD/ITD stub");

    const fuse::audio::HrtfPanPathPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, valid, offset);
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF pan-path preflight is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(bypass_preflight),
               "should_skip_hrtf_pan_path_preflight true on bypass");
    expectTrue(!fuse::audio::can_apply_spatial_hrtf_pan(bypass_preflight),
               "bypass pan-path preflight cannot spatial-pan");
}

void testHrtfAttenuationCouplingPreflightSkipAlias() {
    const fuse::audio::HrtfAttenuationCouplingPreflight unity_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Convolution,
                                                         1.f, 1.f);
    expectTrue(unity_preflight.should_skip(), "unity attenuation coupling preflight is skipped");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(unity_preflight),
               "should_skip_hrtf_attenuation_coupling_preflight mirrors struct method");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(unity_preflight)
                   == !fuse::audio::can_narrow_hrtf_spatial_image(unity_preflight),
               "skip/can_narrow coupling preflight predicates are inverses");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(!narrow_preflight.should_skip(), "reduced attenuation coupling preflight is not skipped");
    expectTrue(fuse::audio::can_narrow_hrtf_spatial_image(narrow_preflight),
               "can_narrow_hrtf_spatial_image mirrors struct method");
}

void testHrtfBinauralPreflightPredicateAliases() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

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
                   == stub_preflight.can_convolve(),
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
}

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

    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    const fuse::audio::HrtfBinauralPreflight listener_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, listener, source_position, valid, 1.f, 1.f);
    const fuse::audio::HrtfBinauralPreflight offset_ir_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, rel_listener, 1.f, 1.f);
    expectTrue(listener_ir_preflight.can_convolve() == offset_ir_preflight.can_convolve(),
               "listener IR preflight can_convolve matches offset preflight");
}

void testApplyBinauralPanToSampleFromPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
    float left = 0.f;
    float right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, stub_preflight, offset, 0.5f,
                                                             left, right);
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 0.2f, 0.3f);
    expectNear(left, 0.5f * coupled.left, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");
    expectNear(right, 0.5f * coupled.right, 1e-5f,
               "from_preflight sample apply matches coupled gains on stub path");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, empty, offset, 0.1f, 0.1f);
    left = 0.f;
    right = 0.f;
    fuse::audio::apply_binaural_pan_to_sample_from_preflight(1.f, bypass_preflight, offset, 0.25f,
                                                             left, right);
    expectNear(left, 0.25f, 1e-5f, "bypass preflight applies centre pan to left");
    expectNear(right, 0.25f, 1e-5f, "bypass preflight applies centre pan to right");
}

void testGuardedPanRoutesThroughCompositePreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::BinauralPanGains guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, offset);
    const fuse::audio::BinauralPanGains direct =
        fuse::audio::compute_binaural_pan_gains_for_path(
            fuse::audio::resolve_hrtf_pan_path(true, offset), offset);
    expectNear(guarded.left, direct.left, 1e-5f,
               "guarded pan still matches for_path on valid stub path");
    expectNear(guarded.right, direct.right, 1e-5f,
               "guarded pan still matches for_path on valid stub path");

    const fuse::audio::BinauralPanGains ir_guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, valid, offset);
    const fuse::audio::BinauralPanGains ir_direct =
        fuse::audio::compute_binaural_pan_gains_for_path(
            fuse::audio::resolve_hrtf_pan_path(true, valid, offset), offset);
    expectNear(ir_guarded.left, ir_direct.left, 1e-5f,
               "IR-aware guarded pan still matches for_path on valid path");
    expectNear(ir_guarded.right, ir_direct.right, 1e-5f,
               "IR-aware guarded pan still matches for_path on valid path");

    const fuse::audio::BinauralPanGains bypass_guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(false, valid, offset);
    expectTrue(fuse::audio::is_centre_panned(bypass_guarded),
               "guarded bypass still returns centre pan");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;

    expectTrue(!fuse::audio::try_preflight_hrtf_ir(empty, reason),
               "try_preflight_hrtf_ir rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "empty IR reject reason is NullSamples");
    expectTrue(fuse::audio::should_skip_hrtf_ir_preflight(empty),
               "should_skip_hrtf_ir_preflight true for empty IR");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::preflight_hrtf_ir_ready(valid),
               "preflight_hrtf_ir_ready accepts valid IR");
    expectTrue(fuse::audio::try_preflight_hrtf_ir(valid, reason),
               "try_preflight_hrtf_ir accepts valid IR");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
               "valid IR reject reason is None");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfIrPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(fuse::audio::classify_hrtf_ir_reject(malformed_preflight) ==
                   fuse::audio::HrtfIrRejectReason::MalformedIr,
               "classify_hrtf_ir_reject maps malformed IR");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_label(
                   fuse::audio::HrtfIrRejectReason::MalformedIr),
               "MalformedIr") == 0,
               "IR reject reason label for MalformedIr");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    expectTrue(!fuse::audio::preflight_hrtf_ir_ready(null_samples_nonzero_length, &reason),
               "preflight_hrtf_ir_ready rejects null samples");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
               "null samples reject reason is NullSamples");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_pan_path(true, empty, offset, reason),
               "try_preflight_hrtf_pan_path accepts ILD/ITD stub path");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
               "ILD/ITD stub path has no spatial reject reason");

    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(false, valid, offset, reason),
               "try_preflight_hrtf_pan_path rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "disabled HRTF reject reason is HrtfDisabled");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(false, valid, offset),
               "should_skip_hrtf_pan_path_preflight true when disabled");

    expectTrue(!fuse::audio::try_preflight_hrtf_pan_path(true, valid, co_located, reason),
               "try_preflight_hrtf_pan_path rejects co-located source");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "co-located reject reason is CoLocated");

    expectTrue(fuse::audio::preflight_hrtf_pan_path_ready(true, offset),
               "no-IR overload accepts spatial pan");
    expectTrue(!fuse::audio::preflight_hrtf_convolution_path_ready(true, empty, offset, &reason),
               "convolution path preflight rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "empty IR convolution reject reason is EmptyIr");
    expectTrue(fuse::audio::preflight_hrtf_convolution_path_ready(true, valid, offset),
               "convolution path preflight accepts valid IR");

    const fuse::audio::HrtfPanPathPreflight stub_preflight =
        fuse::audio::preflight_hrtf_pan_path(true, empty, offset);
    expectTrue(fuse::audio::classify_hrtf_pan_path_convolution_reject(stub_preflight) ==
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr,
               "classify_hrtf_pan_path_convolution_reject maps empty IR");
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_label(
                   fuse::audio::HrtfPanPathRejectReason::CoLocated),
               "CoLocated") == 0,
               "pan-path reject reason label for CoLocated");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f, reason),
               "try_preflight_hrtf_attenuation_coupling rejects bypass path");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path coupling reject reason is BypassPath");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f),
               "should_skip_hrtf_attenuation_coupling_preflight true on bypass");

    expectTrue(!fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f, reason),
               "try_preflight_hrtf_attenuation_coupling rejects unity attenuation");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation coupling reject reason is UnityAttenuation");

    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling_ready(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "reduced attenuation coupling preflight can narrow");
    expectTrue(fuse::audio::try_preflight_hrtf_attenuation_coupling(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f, reason),
               "try_preflight_hrtf_attenuation_coupling accepts reduced attenuation");
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing-ready coupling reject reason is None");

    const fuse::audio::HrtfAttenuationCouplingPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
                                                         0.1f);
    expectTrue(fuse::audio::classify_hrtf_attenuation_coupling_reject(bypass_preflight) ==
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "classify_hrtf_attenuation_coupling_reject maps bypass path");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(fuse::audio::try_preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f, reason),
               "try_preflight_hrtf_binaural accepts ILD/ITD stub path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
               "ILD/ITD composite reject reason is None");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_preflight(true, empty, offset, 0.2f, 0.3f),
               "should_skip_hrtf_binaural_preflight false on stub path");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, reason),
               "try_preflight_hrtf_binaural rejects disabled HRTF");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "disabled composite reject reason is HrtfDisabled");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f,
                                                           reason),
               "try_preflight_hrtf_binaural rejects co-located source");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite reject reason is CoLocated");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural_convolution(true, empty, offset, 1.f, 1.f,
                                                                     reason),
               "try_preflight_hrtf_binaural_convolution rejects empty IR");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "empty IR composite convolution reject reason is EmptyIr");
    expectTrue(fuse::audio::preflight_hrtf_binaural_convolution_ready(true, valid, offset, 1.f,
                                                                      1.f),
               "composite convolution preflight accepts valid IR");

    expectTrue(!fuse::audio::try_preflight_hrtf_binaural_narrowing(true, valid, offset, 1.f, 1.f,
                                                                   reason),
               "try_preflight_hrtf_binaural_narrowing rejects unity attenuation");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
               "unity attenuation composite narrowing reject reason is UnityAttenuation");
    expectTrue(fuse::audio::try_preflight_hrtf_binaural_narrowing(true, empty, offset, 0.2f, 0.3f,
                                                                  reason),
               "try_preflight_hrtf_binaural_narrowing accepts reduced attenuation");

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
    expectTrue(fuse::audio::classify_hrtf_binaural_reject(bypass_preflight) ==
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
               "classify_hrtf_binaural_reject maps disabled HRTF");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_label(
                   fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),
               "UnityAttenuation") == 0,
               "binaural reject reason label for UnityAttenuation");

    expectTrue(fuse::audio::preflight_hrtf_binaural_ready(true, offset, 0.3f, 0.4f) ==
                   fuse::audio::preflight_hrtf_binaural_ready(true, empty, offset, 0.3f, 0.4f),
               "Vec3 composite preflight ready mirrors IR-aware overload on stub path");
}

void testHrtfPreflightReadyMirrorsExistingPredicates() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    expectTrue(fuse::audio::preflight_hrtf_ir_ready(valid) ==
                   fuse::audio::can_convolve_hrtf_ir(fuse::audio::preflight_hrtf_ir(valid)),
               "preflight_hrtf_ir_ready mirrors can_convolve_hrtf_ir");
    expectTrue(fuse::audio::preflight_hrtf_pan_path_ready(true, empty, offset) ==
                   fuse::audio::can_apply_spatial_hrtf_pan(
                       fuse::audio::preflight_hrtf_pan_path(true, empty, offset)),
               "preflight_hrtf_pan_path_ready mirrors can_apply_spatial_hrtf_pan");
    expectTrue(fuse::audio::preflight_hrtf_convolution_path_ready(true, valid, offset) ==
                   fuse::audio::can_convolve_hrtf_pan_path(
                       fuse::audio::preflight_hrtf_pan_path(true, valid, offset)),
               "preflight_hrtf_convolution_path_ready mirrors can_convolve_hrtf_pan_path");
    expectTrue(fuse::audio::preflight_hrtf_attenuation_coupling_ready(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f) ==
                   fuse::audio::can_narrow_hrtf_spatial_image(
                       fuse::audio::preflight_hrtf_attenuation_coupling(
                           fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)),
               "preflight_hrtf_attenuation_coupling_ready mirrors can_narrow_hrtf_spatial_image");
    expectTrue(fuse::audio::preflight_hrtf_binaural_ready(true, empty, offset, 0.2f, 0.3f) ==
                   fuse::audio::can_apply_hrtf_binaural_pan(
                       fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f)),
               "preflight_hrtf_binaural_ready mirrors can_apply_hrtf_binaural_pan");
}

void testHrtfBinauralPreflight() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
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
    expectTrue(bypass_preflight.is_bypass(), "disabled HRTF composite preflight is bypass");
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass_preflight),
               "should_skip_hrtf_binaural true on bypass");
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

    const fuse::audio::BinauralPanGains vec3_coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, offset, 0.3f, 0.4f);
    const fuse::audio::HrtfBinauralPreflight vec3_preflight =
        fuse::audio::preflight_hrtf_binaural(true, offset, 0.3f, 0.4f);
    const fuse::audio::BinauralPanGains vec3_from_preflight =
        fuse::audio::compute_binaural_pan_gains_from_preflight(vec3_preflight, offset);
    expectNear(vec3_coupled.left, vec3_from_preflight.left, 1e-5f,
               "Vec3 coupled helper routes through composite preflight");
    expectNear(vec3_coupled.right, vec3_from_preflight.right, 1e-5f,
               "Vec3 coupled helper routes through composite preflight");
}

void testHrtfIrRejectReasonGuards() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty)
                   == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "canonical empty IR reports ZeroLength reject reason");
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty,
                                                       fuse::audio::HrtfIrRejectReason::ZeroLength),
               "empty IR rejects for ZeroLength");

    const float samples[] = {0.5f};
    const fuse::audio::HrtfIrStub valid{samples, 1};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(valid) == fuse::audio::HrtfIrRejectReason::None,
               "valid IR has no reject reason");
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid,
                                                        fuse::audio::HrtfIrRejectReason::NullSamples),
               "valid IR does not reject for NullSamples");

    const fuse::audio::HrtfIrStub zero_length =
        fuse::audio::make_hrtf_ir_stub(samples, 0);
    expectTrue(fuse::audio::hrtf_ir_reject_reason(zero_length)
                   == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "zero-length factory IR reports ZeroLength reject reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(malformed)
                   == fuse::audio::HrtfIrRejectReason::MalformedIr,
               "non-null zero-length IR reports MalformedIr reject reason");

    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 4};
    expectTrue(fuse::audio::hrtf_ir_reject_reason(null_samples_nonzero_length)
                   == fuse::audio::HrtfIrRejectReason::NullSamples,
               "null samples with non-zero length reports NullSamples reject reason");

    const fuse::audio::HrtfIrPreflight empty_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "IR preflight carries ZeroLength reject reason");
    expectTrue(empty_preflight.reason
                   == fuse::audio::hrtf_ir_reject_reason(empty),
               "IR preflight reason matches diagnose helper");
}

void testHrtfIrRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::None),
                           "None")
                   == 0,
               "None IR reject reason name");
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(
                               fuse::audio::HrtfIrRejectReason::MalformedIr),
                           "MalformedIr")
                   == 0,
               "MalformedIr reject reason name");
}

void testHrtfPanPathRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};

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

    const fuse::audio::HrtfPanPathPreflight disabled_preflight =
        fuse::audio::preflight_hrtf_pan_path(false, offset);
    expectTrue(disabled_preflight.reason
                   == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
               "pan-path preflight carries HrtfDisabled reject reason");
    expectTrue(disabled_preflight.reason
                   == fuse::audio::hrtf_pan_path_reject_reason(false, offset),
               "pan-path preflight reason matches diagnose helper");
}

void testHrtfPanPathRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_pan_path_reject_reason_name(
                               fuse::audio::HrtfPanPathRejectReason::CoLocated),
                           "CoLocated")
                   == 0,
               "CoLocated pan-path reject reason name");
}

void testHrtfAttenuationCouplingRejectReasonGuards() {
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
               "bypass path reports BypassPath coupling reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_rejects_for_reason(
                   fuse::audio::HrtfPanPath::Bypass, 0.1f, 0.1f,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
               "bypass path rejects for BypassPath");

    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::Convolution, 1.f, 1.f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
               "unity attenuation reports UnityAttenuation reject reason");
    expectTrue(fuse::audio::hrtf_attenuation_coupling_reject_reason(
                   fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f)
                   == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "reduced attenuation on spatial path has no coupling reject reason");

    const fuse::audio::HrtfAttenuationCouplingPreflight narrow_preflight =
        fuse::audio::preflight_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::IldItdStub,
                                                         0.2f, 0.3f);
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
               "narrowing preflight carries None reject reason");
    expectTrue(narrow_preflight.reason
                   == fuse::audio::hrtf_attenuation_coupling_reject_reason(
                          fuse::audio::HrtfPanPath::IldItdStub, 0.2f, 0.3f),
               "coupling preflight reason matches diagnose helper");
}

void testHrtfAttenuationCouplingRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_attenuation_coupling_reject_reason_name(
                               fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
                           "UnityAttenuation")
                   == 0,
               "UnityAttenuation coupling reject reason name");
}

void testHrtfBinauralRejectReasonGuards() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::Vec3 co_located{};
    const fuse::audio::HrtfIrStub empty{};
    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub valid{samples, 1};

    const fuse::audio::HrtfBinauralPreflight stub_preflight =
        fuse::audio::preflight_hrtf_binaural(true, empty, offset, 0.2f, 0.3f);
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

    const fuse::audio::HrtfBinauralPreflight conv_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, offset, 1.f, 1.f);
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

    const fuse::audio::HrtfBinauralPreflight bypass_preflight =
        fuse::audio::preflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f);
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

    const fuse::audio::HrtfBinauralPreflight co_located_preflight =
        fuse::audio::preflight_hrtf_binaural(true, valid, co_located, 0.1f, 0.1f);
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
               "co-located composite preflight carries CoLocated primary reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_reason(
                   co_located_preflight, fuse::audio::HrtfBinauralRejectReason::CoLocated),
               "composite rejects for CoLocated primary reason");

    const fuse::audio::HrtfIrStub malformed{samples, 0};
    const fuse::audio::HrtfBinauralPreflight malformed_preflight =
        fuse::audio::preflight_hrtf_binaural(true, malformed, offset, 1.f, 1.f);
    expectTrue(malformed_preflight.convolutionReason
                   == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
               "malformed IR composite preflight carries MalformedIr convolution reason");
    expectTrue(fuse::audio::hrtf_binaural_rejects_for_convolution_reason(
                   malformed_preflight, fuse::audio::HrtfBinauralRejectReason::MalformedIr),
               "composite rejects for MalformedIr convolution reason");
}

void testHrtfBinauralRejectReasonName() {
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::BypassPath),
                           "BypassPath")
                   == 0,
               "BypassPath composite reject reason name");
    expectTrue(std::strcmp(fuse::audio::hrtf_binaural_reject_reason_name(
                               fuse::audio::HrtfBinauralRejectReason::MalformedIr),
                           "MalformedIr")
                   == 0,
               "MalformedIr composite reject reason name");
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
    testShouldSkipHrtfConvolution();
    testHrtfPanPathSkipAliases();
    testUnitySpatialBlendGuards();
    testApplyHrtfSpatialBlendGuarded();
    testHrtfIrPreflight();
    testHrtfPanPathPreflight();
    testHrtfAttenuationCouplingPreflight();
    testBinauralPanGainSampleHelpers();
    testHrtfIrPreflightSkipAlias();
    testHrtfPanPathPreflightPredicateAliases();
    testHrtfAttenuationCouplingPreflightSkipAlias();
    testHrtfBinauralPreflightPredicateAliases();
    testListenerAwareHrtfBinauralPreflight();
    testApplyBinauralPanToSampleFromPreflight();
    testGuardedPanRoutesThroughCompositePreflight();
    testHrtfIrRejectReasonGuards();
    testHrtfPanPathRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfBinauralRejectReasonGuards();
    testHrtfPreflightReadyMirrorsExistingPredicates();
    testHrtfBinauralPreflight();
    testHrtfIrRejectReasonGuards();
    testHrtfIrRejectReasonName();
    testHrtfPanPathRejectReasonGuards();
    testHrtfPanPathRejectReasonName();
    testHrtfAttenuationCouplingRejectReasonGuards();
    testHrtfAttenuationCouplingRejectReasonName();
    testHrtfBinauralRejectReasonGuards();
    testHrtfBinauralRejectReasonName();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("test_binaural_hrtf: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "test_binaural_hrtf: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

// --- deepen additive from deepen-b72-hrtf-pan-guards-e91a ---
               "should_skip mirrors is_hrtf_pan_bypassed for Bypass");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path(fuse::audio::HrtfPanPath::IldItdStub),
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(1.f, 1.f),
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(0.2f, 1.f),

// --- deepen additive from deepen-b72-hrtf-pan-guards-6bae ---
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(fuse::audio::HrtfPanPath::Bypass),

// --- deepen additive from deepen-hrtf-pan-empty-ir-guards-91f9 ---
void testShouldSkipHrtfPanGuards() {
    expectTrue(fuse::audio::should_skip_hrtf_pan(true, fuse::audio::Vec3{}),
               "should_apply is inverse of should_skip");
void testEmptyIrConvolutionSkipGuards() {
    expectTrue(fuse::audio::should_skip_hrtf_convolution(zero_length),
               "should_apply coupling is inverse of should_skip");
void testSpatialBlendSkipGuards() {
    expectTrue(!fuse::audio::should_skip_hrtf_spatial_blend(0.f, 0.f),

// --- deepen additive from deepen-b72-hrtf-pan-empty-ir-guards-7a97 ---
               "should_skip matches should_bypass");

// --- deepen additive from deepen-b72-hrtf-pan-empty-ir-guards-3df6 ---
void testEmptyIrFallbackAndNormalizeGuards() {
void testNonUnityAndPreserveSpatialImageGuards() {
void testGuardedSpatialBlendHelper() {

// --- deepen additive from deepen-hrtf-guards-71c7 ---
void testEmptyIrConvolutionGuards() {
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty),
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(empty)
    fuse::audio::HrtfPanRejectReason reason = fuse::audio::HrtfPanRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfPanRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfPanRejectReason::Disabled,
    expectTrue(reason == fuse::audio::HrtfPanRejectReason::CoLocated,
                               fuse::audio::HrtfPanRejectReason::Disabled),
void testHrtfPanPreflight() {
    const fuse::audio::HrtfPanPreflight spatial =
    const fuse::audio::HrtfPanPreflight empty_ir =
    const fuse::audio::HrtfPanPreflight bypass =
    expectTrue(bypass.reject_reason == fuse::audio::HrtfPanRejectReason::Disabled,
    const fuse::audio::HrtfPanPreflight no_ir =
    const fuse::audio::HrtfPanPreflight co_located_preflight =
    expectTrue(co_located_preflight.reject_reason == fuse::audio::HrtfPanRejectReason::CoLocated,
void testAttenuationCouplingPreflight() {
    const fuse::audio::HrtfAttenuationCouplingPreflight unity =
    const fuse::audio::HrtfAttenuationCouplingPreflight narrowed =
    const fuse::audio::HrtfAttenuationCouplingPreflight bypass =
    const fuse::audio::HrtfAttenuationCouplingPreflight coupled =
    const fuse::audio::HrtfAttenuationCouplingPreflight coupled_no_ir =
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_mapping(1.f, 1.f),
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_mapping(1.f, 1.f)
void testAttenuationCouplingMappingSkipGuard() {
    testHrtfPanPreflight();
    testAttenuationCouplingPreflight();

// --- deepen additive from hrtf-preflight-guards-9323 ---
    const fuse::audio::HrtfIrPreflight zero_length = fuse::audio::preflight_hrtf_ir(null_length);
    const fuse::audio::HrtfPanPathPreflight convolution =
    const fuse::audio::HrtfPanPathPreflight ild_stub =
    const fuse::audio::HrtfPanPathPreflight bypass =
    const fuse::audio::HrtfPanPathPreflight no_ir =
    const fuse::audio::HrtfAttenuationCouplingPreflight spatial =
    const fuse::audio::HrtfAttenuationCouplingPreflight distance_blend =
void testHrtfSpatialPanPreflight() {
    const fuse::audio::HrtfSpatialPanPreflight active =
    const fuse::audio::HrtfSpatialPanPreflight bypass =
    const fuse::audio::HrtfSpatialPanPreflight unity =
    testHrtfSpatialPanPreflight();

// --- deepen additive from hrtf-preflight-guards-4c1e ---
void testHrtfIrPreflightGuards() {
    const fuse::audio::HrtfIrPreflight null_preflight = fuse::audio::preflight_hrtf_ir(null_samples);
void testHrtfPanPathPreflightGuards() {
    expectTrue(!stub_preflight.should_skip_spatial_pan(), "stub path does not skip spatial pan");
    const fuse::audio::HrtfPanPathPreflight convolution_preflight =
    expectTrue(bypass_preflight.should_skip_spatial_pan(), "disabled preflight skips spatial pan");
void testHrtfAttenuationCouplingPreflightGuards() {
    expectTrue(bypass_preflight.should_skip(), "bypass preflight should skip");
    const fuse::audio::HrtfAttenuationCouplingPreflight clamped_preflight =
    testHrtfIrPreflightGuards();
    testHrtfPanPathPreflightGuards();
    testHrtfAttenuationCouplingPreflightGuards();

// --- deepen additive from deepen-hrtf-preflight-guards-fa19 ---
    const fuse::audio::HrtfPanPathPreflight bypass_disabled =
    const fuse::audio::HrtfPanPathPreflight bypass_co_located =

// --- deepen additive from hrtf-preflight-guards-1a3b ---
    expectTrue(empty_preflight.should_skip(), "empty IR preflight should skip convolution");
    expectTrue(!valid_preflight.should_skip(), "valid IR preflight does not skip convolution");
    expectTrue(null_preflight.should_skip(), "null samples preflight skips convolution");
    expectTrue(!convolution.should_skip(), "pan-path preflight does not skip spatial pan");
    expectTrue(bypass.should_skip(), "bypass preflight skips spatial pan");
    expectTrue(co_located_preflight.should_skip(), "co-located preflight skips spatial pan");
    expectTrue(!narrowed.should_skip(), "coupling preflight does not skip");
    expectTrue(unity.should_skip(), "unity coupling preflight should skip");
    const fuse::audio::HrtfAttenuationCouplingPreflight ir_coupled =
    const fuse::audio::HrtfAttenuationCouplingPreflight disabled =
    const fuse::audio::HrtfAttenuationCouplingPreflight full_unity =

// --- deepen additive from hrtf-preflight-guards-69c4 ---
    const fuse::audio::HrtfIrPreflight empty =
    expectTrue(!convolution.should_skip(), "preflight does not skip convolution path");
    const fuse::audio::HrtfPanPathPreflight disabled =
    expectTrue(disabled.should_skip(), "preflight skips disabled path");
    expectTrue(co_located_preflight.should_skip(), "preflight skips co-located source");
    expectTrue(bypass.should_skip(), "preflight skips coupling on bypass path");
    expectTrue(unity.should_skip(), "preflight skips coupling at unity attenuation");
    expectTrue(!narrowed.should_skip(), "preflight does not skip non-unity spatial coupling");

// --- deepen additive from hrtf-preflight-guards-3b8d ---
    const fuse::audio::HrtfPanPathPreflight empty_ir =
    expectTrue(bypass.should_skip_coupling(), "bypass preflight skips coupling");
    expectTrue(unity.should_skip_coupling(), "unity preflight skips coupling");
    const fuse::audio::HrtfAttenuationCouplingPreflight narrow =
    const fuse::audio::HrtfSpatialPanPreflight bypassed =
    const fuse::audio::HrtfSpatialPanPreflight stub_narrow =
    const fuse::audio::HrtfSpatialPanPreflight convolution_unity =

// --- deepen additive from hrtf-preflight-guards-3b23 ---
    const fuse::audio::HrtfIrPreflight null_length_preflight =
    const fuse::audio::HrtfIrPreflight zero_length_preflight =
    expectTrue(bypass_preflight.should_skip_coupling(), "bypass should_skip_coupling");
    expectTrue(unity_preflight.should_skip_coupling(), "unity skips coupling application");
    expectTrue(!narrow_preflight.should_skip_coupling(), "non-unity spatial path applies coupling");
    const fuse::audio::HrtfAttenuationCouplingPreflight unity_clamped_preflight =

// --- deepen additive from deepen-hrtf-preflights-44d8 ---
void testEmptyIrPreflightGuards() {
    expectTrue(!fuse::audio::should_skip_hrtf_ir_convolution(valid),
void testPanPathPreflightGuards() {
void testAttenuationCouplingPreflightGuards() {
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_for_inputs(
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_for_inputs(
    testEmptyIrPreflightGuards();
    testPanPathPreflightGuards();
    testAttenuationCouplingPreflightGuards();

// --- deepen additive from deepen-hrtf-preflights-4398 ---
    const fuse::audio::HrtfIrPreflight null_only =

// --- deepen additive from deepen-b72-hrtf-preflights-564d ---
void testPreflightHrtfIrGuards() {
void testPreflightHrtfPanPathGuards() {
void testPreflightHrtfAttenuationCouplingGuards() {
    const fuse::audio::HrtfAttenuationCouplingPreflight ir_aware =
    const fuse::audio::HrtfAttenuationCouplingPreflight via_path =
    testPreflightHrtfIrGuards();
    testPreflightHrtfPanPathGuards();
    testPreflightHrtfAttenuationCouplingGuards();

// --- deepen additive from deepen-hrtf-preflight-guards-1cf6 ---
    const fuse::audio::HrtfPanPathPreflight enabled =
    expectTrue(!enabled.should_skip_pan(), "spatial path does not skip pan");
    expectTrue(bypass.should_skip_pan(), "disabled preflight skips pan");
    expectTrue(unity.should_skip_coupling(), "unity coupling preflight skips coupling");
    expectTrue(bypass.should_skip_coupling(), "bypass coupling preflight skips coupling");

// --- deepen additive from deepen-b72-hrtf-preflights-201d ---
void testPreflightHrtfIr() {
    const fuse::audio::HrtfIrPreflight malformed_preflight = fuse::audio::preflight_hrtf_ir(malformed);
void testPreflightHrtfPanPath() {
    const fuse::audio::HrtfPanPathPreflight stub =
void testPreflightHrtfAttenuationCoupling() {
    testPreflightHrtfIr();
    testPreflightHrtfPanPath();
    testPreflightHrtfAttenuationCoupling();

// --- deepen additive from deepen-hrtf-preflights-ccde ---
               "preflight skip matches should_skip_hrtf_convolution");
    expectTrue(malformed_preflight.should_skip_convolution(),
    expectTrue(!ild_stub.should_skip_attenuation_coupling(),
    expectTrue(disabled.should_skip_pan(), "disabled HRTF preflight skips pan");
    expectTrue(disabled.should_skip_spatial_pan()
               "preflight skip matches should_skip_hrtf_spatial_pan");
    expectTrue(co_located_preflight.should_skip_attenuation_coupling(),
    expectTrue(bypass.should_skip_coupling(), "bypass path preflight skips coupling");
    expectTrue(unity.should_skip_coupling(), "unity attenuation preflight skips coupling");
               "preflight skip matches should_skip_hrtf_spatial_blend");
    const fuse::audio::HrtfAttenuationCouplingPreflight bundled =
    const fuse::audio::HrtfAttenuationCouplingPreflight manual =
    const fuse::audio::HrtfAttenuationCouplingPreflight disabled_bundle =
    expectTrue(disabled_bundle.should_skip_coupling(),

// --- deepen additive from deepen-b72-hrtf-preflights-815c ---
void testPreflightHrtfIrConvolution() {
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::EmptyIr;
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
               "preflight inverts should_skip_hrtf_convolution");
void testPreflightHrtfSpatialPan() {
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::Disabled;
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::Disabled,
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    fuse::audio::HrtfAttenuationCouplingRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath;
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
    testPreflightHrtfIrConvolution();
    testPreflightHrtfSpatialPan();

// --- deepen additive from deepen-b72-hrtf-preflights-eec6 ---
    fuse::audio::HrtfIrPreflightRejectReason reason =
        fuse::audio::HrtfIrPreflightRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfIrPreflightRejectReason::NullSamples,
    expectTrue(reason == fuse::audio::HrtfIrPreflightRejectReason::ZeroLength,
    expectTrue(reason == fuse::audio::HrtfIrPreflightRejectReason::None,
    fuse::audio::HrtfPanPreflightRejectReason reason =
        fuse::audio::HrtfPanPreflightRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfPanPreflightRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfPanPreflightRejectReason::Disabled,
    expectTrue(reason == fuse::audio::HrtfPanPreflightRejectReason::CoLocated,
    fuse::audio::HrtfAttenuationCouplingPreflightRejectReason reason =
        fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::BypassPath,
        reason == fuse::audio::HrtfAttenuationCouplingPreflightRejectReason::UnityAttenuation,

// --- deepen additive from deepen-b72-hrtf-pan-empty-ir-guards-e7fc ---
    expectTrue(valid_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    const fuse::audio::HrtfPanPathPreflight spatial =
    const fuse::audio::HrtfPanPathPreflight co_located =
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_apply(
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_apply(

// --- deepen additive from deepen-hrtf-preflights-981f ---
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfIrRejectReason::ZeroLength,
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None, "bool preflight reason is None");
    expectTrue(convolution.reject_reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(disabled.reject_reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(co_located.reject_reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    fuse::audio::HrtfPanPathRejectReason reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(narrowed.reject_reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    const fuse::audio::HrtfAttenuationCouplingPreflight bypassed =
        fuse::audio::HrtfAttenuationCouplingRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfAttenuationCouplingRejectReason::PanBypassed,

// --- deepen additive from deepen-hrtf-b7-2-guards-9316 ---
    expectTrue(empty_preflight.reject == fuse::audio::HrtfIrPreflightReject::NullSamples,
    const fuse::audio::HrtfIrPreflight zero_preflight = fuse::audio::preflight_hrtf_ir(zero_length);
    expectTrue(zero_preflight.reject == fuse::audio::HrtfIrPreflightReject::ZeroLength,
    fuse::audio::HrtfIrPreflightReject reject = fuse::audio::HrtfIrPreflightReject::None;
    expectTrue(reject == fuse::audio::HrtfIrPreflightReject::None,
    expectTrue(reject == fuse::audio::HrtfIrPreflightReject::NullSamples,
    expectTrue(disabled.reject == fuse::audio::HrtfPanPathPreflightReject::Disabled,
    expectTrue(co_located_preflight.reject == fuse::audio::HrtfPanPathPreflightReject::CoLocated,
    fuse::audio::HrtfPanPathPreflightReject reject =
        fuse::audio::HrtfPanPathPreflightReject::None;
    expectTrue(reject == fuse::audio::HrtfPanPathPreflightReject::None,
    expectTrue(reject == fuse::audio::HrtfPanPathPreflightReject::Disabled,
    expectTrue(bypass.reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::BypassPath,
        unity.reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::UnityAttenuation,
    expectTrue(narrowed.reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::None,
    fuse::audio::HrtfAttenuationCouplingPreflightReject reject =
        fuse::audio::HrtfAttenuationCouplingPreflightReject::None;
    expectTrue(reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::None,
    expectTrue(reject == fuse::audio::HrtfAttenuationCouplingPreflightReject::BypassPath,

// --- deepen additive from deepen-b72-hrtf-preflight-guards-df8b ---
    expectTrue(empty_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::Empty,
    expectTrue(valid_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(malformed_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::Malformed,
    fuse::audio::HrtfIrRejectReason reason = fuse::audio::HrtfIrRejectReason::Empty;
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::Empty, "bool IR preflight reports Empty");
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::None, "bool IR preflight reports None");
    expectTrue(spatial.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(disabled.rejectReason == fuse::audio::HrtfPanPathRejectReason::Disabled,
    expectTrue(co_located.rejectReason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    expectTrue(narrow.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    expectTrue(unity.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
    expectTrue(bypass.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
    const fuse::audio::HrtfAttenuationCouplingPreflight resolved =
void testHrtfBinauralPanCombinedPreflight() {
    const fuse::audio::HrtfBinauralPanPreflight spatial =
    const fuse::audio::HrtfBinauralPanPreflight bypass =
    expectTrue(bypass.panPath.rejectReason == fuse::audio::HrtfPanPathRejectReason::Disabled,
    const fuse::audio::HrtfBinauralPanPreflight empty_ir =
void testHrtfPreflightRejectReasonLabels() {
                               fuse::audio::HrtfIrRejectReason::Empty),
    testHrtfBinauralPanCombinedPreflight();
    testHrtfPreflightRejectReasonLabels();

// --- deepen additive from deepen-hrtf-preflights-bb6d ---
    expectTrue(null_preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(malformed_preflight.skips_convolution() == fuse::audio::should_skip_hrtf_convolution(malformed),
    expectTrue(disabled.reject_reason == fuse::audio::HrtfPanPathRejectReason::Disabled,
                                                             fuse::audio::HrtfPanPathRejectReason::Disabled),
    expectTrue(co_located_preflight.reject_reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    expectTrue(stub.reject_reason == fuse::audio::HrtfPanPathRejectReason::None,

// --- deepen additive from b72-hrtf-preflight-guards-2202 ---
    expectTrue(empty.should_skip_convolution(), "empty IR preflight skips convolution");
    const fuse::audio::HrtfIrPreflight null_nonzero_preflight =
    expectTrue(null_nonzero_preflight.should_skip_convolution(),
    expectTrue(disabled.should_skip_spatial_pan(), "disabled HRTF skips spatial pan");
    expectTrue(bypass.should_skip_coupling(), "bypass path skips coupling");
    expectTrue(unity.should_skip_coupling(), "unity attenuation skips coupling");
               "preflight can_apply_coupling inverts should_skip_hrtf_spatial_blend");

// --- deepen additive from deepen-hrtf-preflights-0f68 ---
void testEmptyHrtfIrPreflight() {
    const fuse::audio::EmptyHrtfIrPreflight empty_preflight =
        fuse::audio::preflightEmptyHrtfIr(empty);
    const fuse::audio::EmptyHrtfIrPreflight valid_preflight =
        fuse::audio::preflightEmptyHrtfIr(valid);
    const fuse::audio::EmptyHrtfIrPreflight malformed_preflight =
        fuse::audio::preflightEmptyHrtfIr(malformed);
    const fuse::audio::EmptyHrtfIrPreflight null_preflight =
        fuse::audio::preflightEmptyHrtfIr(null_samples_nonzero_length);
        fuse::audio::preflightHrtfPanPath(false, valid, offset);
        fuse::audio::preflightHrtfPanPath(true, valid, co_located);
        fuse::audio::preflightHrtfPanPath(true, empty, offset);
        fuse::audio::preflightHrtfPanPath(true, valid, offset);
        fuse::audio::preflightHrtfPanPath(true, offset);
        fuse::audio::preflightHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::Bypass, 0.1f,
        fuse::audio::preflightHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::Convolution, 1.f,
        fuse::audio::preflightHrtfAttenuationCoupling(fuse::audio::HrtfPanPath::IldItdStub, 0.2f,
    testEmptyHrtfIrPreflight();

// --- deepen additive from deepen-b72-hrtf-preflight-guards-f1c9 ---
    expectTrue(empty_preflight.should_skip(), "empty IR preflight skips convolution");
    expectTrue(malformed_preflight.should_skip(), "malformed IR skips convolution");
    expectTrue(disabled_preflight.should_skip(), "disabled HRTF bypasses pan");
    expectTrue(co_located_preflight.should_skip(), "co-located source bypasses pan");
    expectTrue(!narrowed.should_skip(), "reduced attenuation does not skip coupling");
void testHrtfGuardedPanPreflight() {
    const fuse::audio::HrtfGuardedPanPreflight stub =
    expectTrue(!stub.should_skip_coupling(), "non-unity attenuation does not skip coupling");
    const fuse::audio::HrtfGuardedPanPreflight bypass =
    const fuse::audio::HrtfGuardedPanPreflight unity =
    expectTrue(unity.should_skip_coupling(), "unity attenuation skips bundled coupling");
    testHrtfGuardedPanPreflight();

// --- deepen additive from deepen-b7-2-hrtf-composite-preflights-84ae ---
void testHrtfBinauralCompositePreflight() {
    const fuse::audio::HrtfBinauralPreflight disabled_preflight =
    const fuse::audio::HrtfBinauralPreflight for_path_preflight =
    const fuse::audio::HrtfBinauralPreflight bypass_for_path =
    testHrtfBinauralCompositePreflight();

// --- deepen additive from deepen-b72-hrtf-preflights-9877 ---
               "should_skip_hrtf_ir_convolution mirrors preflight");
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution(valid_preflight)
void testPanPathPreflightApplyHelper() {
void testHrtfBinauralPreflightCompositeAliases() {
               "composite should_skip_convolution on empty IR");
               "should_skip_hrtf_binaural_convolution on empty IR");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling(stub_preflight),
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling(conv_preflight),
void testCoupledForPathUsesCompositePreflight() {
    const fuse::audio::HrtfBinauralPreflight preflight =
    testPanPathPreflightApplyHelper();
    testHrtfBinauralPreflightCompositeAliases();
    testCoupledForPathUsesCompositePreflight();

// --- deepen additive from deepen-b72-hrtf-preflights-0447 ---
void testHrtfPanPathPreflightAliases() {
    expectTrue(!stub_preflight.should_skip(), "ILD/ITD stub path is not skipped");
               "should_skip pan-path preflight false on stub path");
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF pan-path is skipped");
               "should_skip pan-path preflight true on bypass");
               "should_skip attenuation coupling preflight mirrors struct method");
    expectTrue(!narrow_preflight.should_skip(), "reduced attenuation coupling is not skipped");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_preflight(narrow_preflight),
               "should_skip false when narrowing is warranted");
    expectTrue(!stub_preflight.should_skip(), "enabled offset stub path is not skipped");
    expectTrue(bypass_preflight.should_skip(), "disabled HRTF composite is skipped");
void testListenerAwareBinauralPreflight() {
    const fuse::audio::HrtfBinauralPreflight ir_listener_preflight =
    testHrtfPanPathPreflightAliases();
    testListenerAwareBinauralPreflight();

// --- deepen additive from deepen-fuse-b7-2-hrtf-guards-708b ---
void testHrtfIrRejectReasonClassifiers() {
                   fuse::audio::HrtfIrRejectReason::NullSamples)) == "null_samples",
                   fuse::audio::HrtfIrRejectReason::MalformedIr)) == "malformed_ir",
    expectTrue(!fuse::audio::should_skip_hrtf_ir_preflight(valid),
    fuse::audio::HrtfIrPreflight preflight{};
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
void testHrtfPanPathRejectReasonClassifiers() {
                   fuse::audio::HrtfPanPathRejectReason::CoLocated)) == "co_located",
    fuse::audio::HrtfPanPathPreflight preflight{};
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(false, offset),
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(true, offset),
void testHrtfAttenuationCouplingRejectReasonClassifiers() {
                   fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation))
    fuse::audio::HrtfAttenuationCouplingPreflight preflight{};
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    expectTrue(preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
void testHrtfBinauralRejectReasonClassifiers() {
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::IrFallback,
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::AttenuationSkipped,
                   fuse::audio::HrtfBinauralRejectReason::AttenuationSkipped))
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfBinauralRejectReason::PanBypass,
    expectTrue(fuse::audio::should_skip_hrtf_binaural_preflight(false, offset),
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_preflight(true, offset),
    fuse::audio::HrtfBinauralPreflight try_preflight{};
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::PanBypass,
    testHrtfIrRejectReasonClassifiers();
    testHrtfPanPathRejectReasonClassifiers();
    testHrtfAttenuationCouplingRejectReasonClassifiers();
    testHrtfBinauralRejectReasonClassifiers();

// --- deepen additive from deepen-b72-hrtf-preflight-reasons-53ba ---
    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(empty);
    const fuse::audio::HrtfPanPathPreflight preflight =
    const fuse::audio::HrtfAttenuationCouplingPreflight preflight =
    fuse::audio::HrtfBinauralRejectReason reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
    expectTrue(stub_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr)) == "empty_ir",

// --- deepen additive from b7-2-hrtf-reject-reasons-1115 ---
void testHrtfIrRejectReasonPreflight() {
    expectTrue(fuse::audio::hrtf_ir_reject_reason(empty) == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::NullSamples),
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::EmptyIr),
void testHrtfPanPathRejectReasonPreflight() {
                   true, co_located, fuse::audio::HrtfPanPathRejectReason::CoLocated),
    const fuse::audio::HrtfPanPathPreflight spatial_preflight =
    expectTrue(spatial_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
void testHrtfAttenuationCouplingRejectReasonPreflight() {
                   fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath)) == "bypass_path",
void testHrtfBinauralRejectReasonPreflight() {
                   false, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                   true, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
    const fuse::audio::HrtfBinauralPreflight spatial_preflight =
    expectTrue(spatial_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(spatial_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(spatial_preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
               "should_skip_hrtf_binaural mirrors composite reject reason");
    const fuse::audio::HrtfBinauralPreflight unity_preflight =
    expectTrue(unity_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
                   fuse::audio::HrtfBinauralRejectReason::HrtfDisabled)) == "hrtf_disabled",
    testHrtfIrRejectReasonPreflight();
    testHrtfPanPathRejectReasonPreflight();
    testHrtfAttenuationCouplingRejectReasonPreflight();
    testHrtfBinauralRejectReasonPreflight();

// --- deepen additive from b72-hrtf-reject-reason-preflights-62b4 ---
                   malformed, fuse::audio::HrtfIrRejectReason::MalformedStub),
    expectTrue(disabled.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(stub.reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(bypass.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
    expectTrue(narrowed.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    const fuse::audio::HrtfBinauralPreflight stub =
    expectTrue(stub.rejectReason() == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(stub.ir.reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(stub.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(!fuse::audio::should_skip_hrtf_binaural(stub),
               "composite should_skip false on spatial stub path");
    const fuse::audio::HrtfBinauralPreflight bypass =
    expectTrue(bypass.rejectReason() == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::should_skip_hrtf_binaural(bypass),
               "composite should_skip true on disabled HRTF");

// --- deepen additive from deepen-hrtf-reject-reason-preflights-1e2a ---
void testHrtfRejectReasonPreflights() {
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::None),
    const fuse::audio::HrtfIrPreflight ir_preflight = fuse::audio::preflight_hrtf_ir(valid);
    expectTrue(ir_preflight.reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(ir_preflight.can_convolve() == (ir_preflight.reason == fuse::audio::HrtfIrRejectReason::None),
                   true, offset, fuse::audio::HrtfPanPathRejectReason::None),
    const fuse::audio::HrtfPanPathPreflight pan_preflight =
    expectTrue(pan_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
                   fuse::audio::HrtfAttenuationCouplingRejectReason::None),
    const fuse::audio::HrtfAttenuationCouplingPreflight coupling_preflight =
    expectTrue(coupling_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
                   true, offset, fuse::audio::HrtfBinauralRejectReason::None),
    const fuse::audio::HrtfBinauralPreflight composite_preflight =
    expectTrue(composite_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
               "should_skip_hrtf_binaural true when composite reject reason is set");
    testHrtfRejectReasonPreflights();

// --- deepen additive from deepen-hrtf-reject-reason-preflights-83b7 ---
    expectTrue(fuse::audio::classify_hrtf_ir_reject(valid) == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(preflight.convolveRejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(preflight.ir.rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(preflight.panPath.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,

// --- deepen additive from deepen-hrtf-reject-reason-preflights-cb75 ---
    expectTrue(fuse::audio::hrtfIrRejectReason(empty)
    expectTrue(fuse::audio::hrtfIrRejectsForReason(empty,
    expectTrue(fuse::audio::hrtfIrRejectReason(valid) == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(!fuse::audio::hrtfIrRejectsForReason(valid,
    expectTrue(fuse::audio::hrtfIrRejectReason(malformed)
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonName(
    expectTrue(fuse::audio::hrtfPanPathRejectReason(false, offset)
    expectTrue(fuse::audio::hrtfPanPathRejectReason(true, co_located)
    expectTrue(fuse::audio::hrtfPanPathRejectReason(true, offset)
    expectTrue(fuse::audio::hrtfPanPathRejectsForReason(
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonName(
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectReason(
    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectsForReason(
    expectTrue(std::strcmp(fuse::audio::hrtfAttenuationCouplingRejectReasonName(
    expectTrue(fuse::audio::hrtfBinauralConvolutionRejectReason(stub_preflight)
    expectTrue(fuse::audio::hrtfBinauralNarrowingRejectReason(stub_preflight)
    expectTrue(disabled_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::hrtfBinauralRejectReason(disabled_preflight)
               "hrtfBinauralRejectReason accessor matches preflight reason");
    expectTrue(fuse::audio::hrtfBinauralRejectsForReason(
                   disabled_preflight, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
    expectTrue(fuse::audio::should_skip_hrtf_binaural(disabled_preflight),
               "should_skip_hrtf_binaural mirrors spatial reject reason");
    expectTrue(std::strcmp(fuse::audio::hrtfBinauralRejectReasonName(
    expectTrue(unity_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::None,

// --- deepen additive from deepen-hrtf-reject-reason-preflights-8d13 ---
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(empty, fuse::audio::HrtfIrRejectReason::EmptyIr),
    expectTrue(empty_preflight.reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(co_located_preflight.reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-0339 ---
    expectTrue(unity_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::
    expectTrue(stub_preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(bypass_preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
    expectTrue(co_located_preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
    expectTrue(stub_preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-54bb ---
    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::preflight_hrtf_ir(malformed);
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
                   fuse::audio::HrtfBinauralRejectReason::EmptyIr),
    expectTrue(preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
    expectTrue(preflight.ir.reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
    expectTrue(preflight.panPath.reason == fuse::audio::HrtfPanPathRejectReason::None,
                   preflight, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,

// --- deepen additive from b72-hrtf-reject-reason-preflights-1e6e ---
    expectTrue(!fuse::audio::hrtf_ir_rejects_for_reason(valid, fuse::audio::HrtfIrRejectReason::NullSamples),
    expectTrue(stub_preflight.reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
               "should_skip_hrtf_binaural matches spatial bypass");
    expectTrue(unity_preflight.reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-db94 ---
    expectTrue(fuse::audio::preflight_hrtf_ir(valid).reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(conv_preflight.reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
    expectTrue(conv_preflight.ir.reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(malformed_preflight.reason == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
    const fuse::audio::HrtfBinauralPreflight ideal_preflight =
    expectTrue(ideal_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-5710 ---
void testHrtfIrRejectReason() {
void testHrtfPanPathRejectReason() {
void testHrtfAttenuationCouplingRejectReason() {
void testHrtfBinauralRejectReason() {
    testHrtfIrRejectReason();
    testHrtfPanPathRejectReason();
    testHrtfAttenuationCouplingRejectReason();
    testHrtfBinauralRejectReason();

// --- deepen additive from deepen-b72-hrtf-reject-reasons-a636 ---
void testHrtfIrRejectReasons() {
    expectTrue(!fuse::audio::hrtf_ir_reject_reason_is_blocking(fuse::audio::HrtfIrRejectReason::None),
    expectTrue(preflight.rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
void testHrtfPanPathRejectReasons() {
                               fuse::audio::HrtfPanPathRejectReason::EmptyIr),
    expectTrue(reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
    expectTrue(preflight.spatialRejectReason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
void testHrtfAttenuationCouplingRejectReasons() {
    expectTrue(preflight.rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
void testHrtfBinauralRejectReasons() {
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
    expectTrue(preflight.spatialRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
    expectTrue(preflight.couplingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(bypass_preflight.spatialRejectReason
    expectTrue(bypass_preflight.convolutionRejectReason
    expectTrue(bypass_preflight.couplingRejectReason
    testHrtfIrRejectReasons();
    testHrtfPanPathRejectReasons();
    testHrtfAttenuationCouplingRejectReasons();
    testHrtfBinauralRejectReasons();

// --- deepen additive from b72-hrtf-reject-reason-preflights-6c04 ---
                   null_samples_nonzero_length, fuse::audio::HrtfIrRejectReason::NullSamples),
    fuse::audio::HrtfIrPreflight out{};
    expectTrue(bypass_preflight.should_skip(), "HrtfDisabled pan-path preflight is skipped");
    fuse::audio::HrtfPanPathPreflight out{};
    fuse::audio::HrtfAttenuationCouplingPreflight out{};
                   true, empty, offset, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                   true, valid, offset, fuse::audio::HrtfBinauralRejectReason::None),
                   false, valid, offset, fuse::audio::HrtfBinauralRejectReason::HrtfDisabled),
                   true, valid, co_located, fuse::audio::HrtfBinauralRejectReason::CoLocated),
                   true, offset, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
                   true, malformed, offset, fuse::audio::HrtfBinauralRejectReason::MalformedIr),
    expectTrue(!stub_preflight.should_skip(), "EmptyIr composite preflight is not bypassed");
    fuse::audio::HrtfBinauralPreflight out{};

// --- deepen additive from b72-hrtf-reject-reason-preflights-d934 ---
void testHrtfIrRejectReasonPreflights() {
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonLabel(
void testHrtfPanPathRejectReasonPreflights() {
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonLabel(
    expectTrue(stub_preflight.reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
    expectTrue(no_ir_preflight.reason == fuse::audio::HrtfPanPathRejectReason::EmptyIr,
void testHrtfAttenuationCouplingRejectReasonPreflights() {
    expectTrue(std::strcmp(fuse::audio::hrtfAttenuationCouplingRejectReasonLabel(
void testHrtfBinauralRejectReasonPreflights() {
                   stub_preflight, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
    expectTrue(std::strcmp(fuse::audio::hrtfBinauralRejectReasonLabel(
    const fuse::audio::HrtfBinauralPreflight valid_preflight =
    expectTrue(valid_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(try_preflight.reason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,
    testHrtfIrRejectReasonPreflights();
    testHrtfPanPathRejectReasonPreflights();
    testHrtfAttenuationCouplingRejectReasonPreflights();
    testHrtfBinauralRejectReasonPreflights();

// --- deepen additive from deepen-b72-hrtf-reject-reasons-4c1f ---
    expectTrue(preflight.reason == fuse::audio::HrtfIrRejectReason::EmptyIr,
                               fuse::audio::HrtfBinauralRejectReason::PanBypassDisabled),
                   fuse::audio::HrtfBinauralRejectReason::ConvolutionEmptyIr),
    expectTrue(preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(preflight.ir.reason == fuse::audio::HrtfIrRejectReason::None,

// --- deepen additive from deepen-b7-2-hrtf-reject-preflights-f8d6 ---
                   preflight, fuse::audio::HrtfIrRejectReason::NullSamples),
    expectTrue(preflight.skipReason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfPanPathRejectReason::None,
                   preflight, fuse::audio::HrtfPanPathRejectReason::HrtfDisabled),
                   preflight, fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath),
    fuse::audio::HrtfBinauralPreflight preflight{};
    expectTrue(preflight.skipReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(preflight.narrowingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    const fuse::audio::HrtfBinauralPreflight empty_ir_preflight =
                   empty_ir_preflight, fuse::audio::HrtfBinauralRejectReason::EmptyIr),
    expectTrue(empty_ir_preflight.convolutionRejectReason
                   unity_preflight, fuse::audio::HrtfBinauralRejectReason::UnityAttenuation),

// --- deepen additive from deepen-b72-hrtf-reject-reasons-2fef ---
    const fuse::audio::HrtfIrPreflight preflight = fuse::audio::try_preflight_hrtf_ir(valid, reason);
                   fuse::audio::HrtfPanPathRejectReason::EmptyIr)) == "empty_ir",
                   fuse::audio::HrtfBinauralRejectReason::MalformedIr)) == "malformed_ir",
    fuse::audio::HrtfBinauralRejectReason stub_reason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(stub_reason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e7e2 ---
    expectTrue(stub_preflight.panRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(stub_preflight.convolutionRejectReason
    expectTrue(stub_preflight.attenuationRejectReason
    expectTrue(unity_preflight.attenuationRejectReason
    expectTrue(try_preflight.panRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(try_preflight.panRejectReason

// --- deepen additive from deepen-b7-2-hrtf-guards-219a ---
    const fuse::audio::HrtfPanPathPreflight empty_ir_preflight =
    expectTrue(empty_ir_preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
    const fuse::audio::HrtfBinauralPreflight composite =
    expectTrue(composite.panRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(composite.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
    expectTrue(composite.couplingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    const fuse::audio::HrtfBinauralPreflight unity =
    expectTrue(unity.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(unity.couplingRejectReason
    expectTrue(out.panRejectReason == reason,

// --- deepen additive from deepen-b72-hrtf-reject-preflights-e16c ---
void testHrtfBinauralCompositeRejectReasonPreflight() {
    expectTrue(stub_preflight.spatialPanReject == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(stub_preflight.convolutionReject == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
    expectTrue(stub_preflight.narrowingReject == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(conv_preflight.convolutionReject == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(conv_preflight.narrowingReject
    expectTrue(bypass_preflight.spatialPanReject
    expectTrue(bypass_preflight.narrowingReject
    expectTrue(co_located_preflight.spatialPanReject
    expectTrue(malformed_preflight.convolutionReject
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::UnityAttenuation,
    testHrtfBinauralCompositeRejectReasonPreflight();

// --- deepen additive from deepen-b72-hrtf-reject-reasons-494b ---
    expectTrue(reason == fuse::audio::HrtfIrRejectReason::MalformedIr,
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
                       fuse::audio::HrtfBinauralConvolutionRejectReason::EmptyIr),
                       fuse::audio::HrtfBinauralNarrowingRejectReason::UnityAttenuation),

// --- deepen additive from deepen-hrtf-b7-2-guards-64dd ---
    expectTrue(empty_preflight.rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
    fuse::audio::HrtfIrRejectReason rejectReason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(rejectReason == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(rejectReason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(bypass_preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    fuse::audio::HrtfPanPathRejectReason rejectReason =
    expectTrue(rejectReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
    fuse::audio::HrtfAttenuationCouplingRejectReason rejectReason =
    expectTrue(rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
    expectTrue(stub_preflight.spatialPanRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(stub_preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,
    expectTrue(stub_preflight.narrowingRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(conv_preflight.convolutionRejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(conv_preflight.narrowingRejectReason
    expectTrue(bypass_preflight.spatialPanRejectReason
    expectTrue(bypass_preflight.narrowingRejectReason
    expectTrue(co_located_preflight.spatialPanRejectReason
    expectTrue(co_located_preflight.narrowingRejectReason
    expectTrue(malformed_preflight.convolutionRejectReason
    const fuse::audio::HrtfBinauralPreflight bypass_narrow_preflight =
    expectTrue(bypass_narrow_preflight.narrowingRejectReason
    fuse::audio::HrtfBinauralRejectReason rejectReason = fuse::audio::HrtfBinauralRejectReason::None;
    expectTrue(rejectReason == fuse::audio::HrtfBinauralRejectReason::None,
    expectTrue(rejectReason == fuse::audio::HrtfBinauralRejectReason::HrtfDisabled,

// --- deepen additive from deepen-b7.2-hrtf-reject-reasons-a8a9 ---
    expectTrue(!fuse::audio::tryPreflightHrtfIr(empty, reason),
               "empty IR fails IR tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonLabel(reason), "null_samples") == 0,
    expectTrue(fuse::audio::hrtfIrRejectReasonIsBlocking(reason),
    expectTrue(fuse::audio::tryPreflightHrtfIr(valid, reason),
               "valid IR passes tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonLabel(reason), "none") == 0,
    expectTrue(!fuse::audio::hrtfIrRejectReasonIsBlocking(reason),
    expectTrue(fuse::audio::classifyHrtfIrReject(malformed)
               "classifyHrtfIrReject malformed_ir for non-null zero-length");
    expectTrue(!fuse::audio::tryPreflightHrtfIr(malformed, reason),
               "malformed IR fails tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfIrRejectReasonLabel(reason), "malformed_ir") == 0,
    expectTrue(preflight.rejectReason == fuse::audio::classifyHrtfIrReject(empty),
               "preflight rejectReason matches classifyHrtfIrReject");
    expectTrue(fuse::audio::tryPreflightHrtfPanPath(true, empty, offset, reason),
               "enabled offset source passes pan-path tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonLabel(reason), "none") == 0,
    expectTrue(!fuse::audio::hrtfPanPathRejectReasonIsBlocking(reason),
    expectTrue(!fuse::audio::tryPreflightHrtfPanPath(false, empty, offset, reason),
               "disabled HRTF fails pan-path tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonLabel(reason), "hrtf_disabled") == 0,
    expectTrue(!fuse::audio::tryPreflightHrtfPanPath(true, empty, co_located, reason),
               "co-located source fails pan-path tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfPanPathRejectReasonLabel(reason), "co_located") == 0,
    expectTrue(fuse::audio::hrtfPanPathRejectReasonIsBlocking(reason),
    expectTrue(fuse::audio::classifyHrtfPanPathReject(true, offset)
               "classifyHrtfPanPathReject none for enabled offset source");
    expectTrue(preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::tryPreflightHrtfAttenuationCoupling(
               "reduced attenuation passes coupling tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfAttenuationCouplingRejectReasonLabel(reason), "none") == 0,
    expectTrue(!fuse::audio::tryPreflightHrtfAttenuationCoupling(
               "bypass path fails coupling tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfAttenuationCouplingRejectReasonLabel(reason),
               "unity attenuation fails coupling tryPreflight");
    expectTrue(fuse::audio::hrtfAttenuationCouplingRejectReasonIsBlocking(reason),
               "preflight rejectReason matches classifyHrtfAttenuationCouplingReject");
    expectTrue(fuse::audio::tryPreflightHrtfBinaural(true, valid, offset, 0.2f, 0.3f, reason),
               "valid IR offset source passes composite tryPreflight");
    expectTrue(std::strcmp(fuse::audio::hrtfBinauralRejectReasonLabel(reason), "none") == 0,
    expectTrue(!fuse::audio::hrtfBinauralRejectReasonIsBypass(reason),
    expectTrue(!fuse::audio::hrtfBinauralRejectReasonBlocksConvolution(reason),
    expectTrue(!fuse::audio::tryPreflightHrtfBinaural(false, valid, offset, 0.1f, 0.1f, reason),
               "disabled HRTF fails composite tryPreflight");
    expectTrue(fuse::audio::hrtfBinauralRejectReasonIsBypass(reason),
    expectTrue(fuse::audio::hrtfBinauralRejectReasonBlocksConvolution(reason),
    expectTrue(!fuse::audio::tryPreflightHrtfBinaural(true, valid, co_located, 0.1f, 0.1f, reason),
               "co-located source fails composite tryPreflight");
    expectTrue(fuse::audio::tryPreflightHrtfBinaural(true, empty, offset, 0.2f, 0.3f, reason),
               "empty IR still passes composite tryPreflight for spatial pan");
    expectTrue(std::strcmp(fuse::audio::hrtfBinauralRejectReasonLabel(reason), "empty_ir") == 0,
    expectTrue(fuse::audio::classifyHrtfBinauralReject(true, malformed, offset, 0.2f, 0.3f)
               "classifyHrtfBinauralReject malformed_ir for non-null zero-length IR");
    expectTrue(fuse::audio::classifyHrtfBinauralReject(true, valid, offset, 1.f, 1.f)
               "classifyHrtfBinauralReject unity_attenuation when narrowing skipped");
    expectTrue(preflight.rejectReason == fuse::audio::HrtfBinauralRejectReason::EmptyIr,

// --- deepen additive from hrtf-b7-2-deepen-guards-653a ---
void testHrtfIrRejectReasonEnums() {
    expectTrue(fuse::audio::classifyHrtfIrReject(empty)
    expectTrue(fuse::audio::classifyHrtfIrReject(valid) == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(!fuse::audio::hrtfIrRejectReasonIsBlocking(fuse::audio::HrtfIrRejectReason::None),
    expectTrue(fuse::audio::classifyHrtfIrReject(null_samples_nonzero)
void testHrtfPanPathRejectReasonEnums() {
    expectTrue(fuse::audio::classifyHrtfPanPathReject(false, valid, offset)
    expectTrue(fuse::audio::classifyHrtfPanPathReject(true, valid, co_located)
    expectTrue(fuse::audio::classifyHrtfPanPathReject(true, empty, offset)
    expectTrue(fuse::audio::classifyHrtfPanPathReject(true, malformed, offset)
    expectTrue(fuse::audio::classifyHrtfPanPathReject(true, valid, offset)
void testHrtfAttenuationCouplingRejectReasonEnums() {
    expectTrue(fuse::audio::classifyHrtfAttenuationCouplingReject(
void testHrtfBinauralRejectReasonEnums() {
    expectTrue(!fuse::audio::hrtfBinauralRejectReasonIsBlocking(stub_preflight.reason),
    expectTrue(fuse::audio::hrtfBinauralRejectReasonIsBlocking(bypass_preflight.reason),
    expectTrue(fuse::audio::classifyHrtfBinauralReject(bypass_preflight)
               "classifyHrtfBinauralReject mirrors stored composite reason");
    testHrtfIrRejectReasonEnums();
    testHrtfPanPathRejectReasonEnums();
    testHrtfAttenuationCouplingRejectReasonEnums();
    testHrtfBinauralRejectReasonEnums();

// --- deepen additive from b72-hrtf-reject-reason-guards-3aaf ---
               "should_skip_hrtf_ir_convolution true for empty IR");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_spatial(true, offset),
               "should_skip_hrtf_pan_spatial false for enabled offset source");
    expectTrue(fuse::audio::should_skip_hrtf_pan_spatial(false, offset),
               "should_skip_hrtf_pan_spatial true when disabled");
    expectTrue(fuse::audio::should_skip_hrtf_pan_spatial(true, co_located),
               "should_skip_hrtf_pan_spatial true for co-located source");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_narrowing(
               "should_skip_hrtf_attenuation_narrowing true on bypass");

// --- deepen additive from hrtf-reject-reason-enums-2d4f ---
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(fuse::audio::HrtfIrRejectReason::None),
                   fuse::audio::hrtf_ir_reject_reason_name(fuse::audio::HrtfIrRejectReason::MalformedIr),
    expectTrue(stub_preflight.convolutionReason == fuse::audio::HrtfConvolutionRejectReason::NullSamples,
    expectTrue(conv_preflight.convolutionReason == fuse::audio::HrtfConvolutionRejectReason::None,
                                                                fuse::audio::HrtfConvolutionRejectReason::None),
                                                                fuse::audio::HrtfConvolutionRejectReason::NullSamples),
    expectTrue(bypass_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
                   stub_preflight, fuse::audio::HrtfConvolutionRejectReason::NullSamples),
                   conv_preflight, fuse::audio::HrtfConvolutionRejectReason::None),

// --- deepen additive from deepen-b72-hrtf-reject-reasons-0e3e ---
    expectTrue(!fuse::audio::tryPreflight_hrtf_ir(empty, reason),
               "tryPreflight_hrtf_ir rejects empty IR");
    expectTrue(fuse::audio::tryPreflight_hrtf_ir(valid, reason),
               "tryPreflight_hrtf_ir accepts valid IR");
    expectTrue(fuse::audio::tryPreflight_hrtf_pan_path(true, valid, offset, reason),
               "tryPreflight_hrtf_pan_path accepts enabled offset source");
    expectTrue(!fuse::audio::tryPreflight_hrtf_pan_path(false, valid, offset, reason),
               "tryPreflight_hrtf_pan_path rejects disabled HRTF");
    expectTrue(fuse::audio::tryPreflight_hrtf_attenuation_coupling(
               "tryPreflight_hrtf_attenuation_coupling accepts reduced attenuation");
    expectTrue(!fuse::audio::tryPreflight_hrtf_attenuation_coupling(
               "tryPreflight_hrtf_attenuation_coupling rejects bypass path");
    expectTrue(fuse::audio::tryPreflight_hrtf_binaural(true, valid, offset, 0.2f, 0.3f, reason),
               "tryPreflight_hrtf_binaural accepts enabled offset source");
    expectTrue(!fuse::audio::tryPreflight_hrtf_binaural(false, valid, offset, 0.1f, 0.1f, reason),
               "tryPreflight_hrtf_binaural rejects disabled HRTF");
void testRejectReasonMirrorsExistingPreflights() {
    const fuse::audio::HrtfIrPreflight ir_preflight = fuse::audio::preflight_hrtf_ir(empty);
    testRejectReasonMirrorsExistingPreflights();

// --- deepen additive from b72-hrtf-reject-reasons-b804 ---
    reason = fuse::audio::HrtfAttenuationCouplingRejectReason::None;

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e349 ---
    expectTrue(std::strcmp(fuse::audio::hrtf_ir_reject_reason_name(fuse::audio::HrtfIrRejectReason::NullSamples),
    expectTrue(fuse::audio::hrtf_ir_rejects_for_reason(malformed, fuse::audio::HrtfIrRejectReason::MalformedIr),
                                                             fuse::audio::HrtfPanPathRejectReason::CoLocatedSource),
    expectTrue(conv_preflight.reason == fuse::audio::HrtfPanPathRejectReason::None,
                   co_located_preflight, fuse::audio::HrtfBinauralRejectReason::CoLocatedSource),

// --- deepen additive from deepen-b72-hrtf-reject-reasons-cf7e ---
    expectTrue(stub_preflight.convolutionReason == fuse::audio::HrtfConvolutionRejectReason::EmptyIr,
                   fuse::audio::HrtfConvolutionRejectReason::EmptyIr),

// --- deepen additive from deepen-b72-hrtf-reject-reasons-69f7 ---
    expectTrue(null_preflight.reject == fuse::audio::HrtfIrRejectReason::NullSamples,
    expectTrue(preflight.reject == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    expectTrue(stub_preflight.convolveReject
    expectTrue(malformed_preflight.convolveReject
    expectTrue(conv_preflight.convolveReject == fuse::audio::HrtfBinauralConvolveRejectReason::None,
    expectTrue(bypass_preflight.convolveReject
    expectTrue(composite.convolveReject

// --- deepen additive from deepen-b72-hrtf-reject-reasons-d5f8 ---
    expectTrue(fuse::audio::should_skip_hrtf_ir_preflight(empty),
               "should_skip_hrtf_ir_preflight true for empty IR");
               "should_skip_hrtf_ir_preflight false for valid IR");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(true, empty, offset),
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_preflight(false, valid, offset),
               "should_skip_hrtf_pan_path_preflight true when disabled");
    expectTrue(fuse::audio::should_skip_hrtf_pan_convolution_preflight(true, empty, offset),
               "should_skip_hrtf_pan_convolution_preflight true for empty IR");
               "should_skip_hrtf_attenuation_coupling_preflight true on bypass");
               "should_skip_hrtf_attenuation_coupling_preflight false when narrowing applies");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_preflight(true, empty, offset, 0.2f, 0.3f),
               "should_skip_hrtf_binaural_preflight false on stub path");
void testHrtfRejectReasonMirrorsExistingPreflights() {
               "preflight_hrtf_binaural_ready mirrors should_skip_hrtf_binaural inverse");
    testHrtfRejectReasonMirrorsExistingPreflights();

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e6c1 ---
                   empty, fuse::audio::HrtfBinauralRejectReason::NullSamples),
    expectTrue(preflight.convolutionReason == fuse::audio::HrtfBinauralRejectReason::NullSamples,
    expectTrue(preflight.attenuationReason == fuse::audio::HrtfBinauralRejectReason::None,
        fuse::audio::hrtf_binaural_reject_reason_from_ir(fuse::audio::HrtfIrRejectReason::MalformedIr)
            fuse::audio::hrtf_binaural_reject_reason_name(fuse::audio::HrtfBinauralRejectReason::BypassPath),

// --- deepen additive from b72-hrtf-reject-reasons-9d6b ---
    expectTrue(stub_preflight.convolutionReason == fuse::audio::HrtfPanPathConvolutionRejectReason::EmptyIr,
    const fuse::audio::HrtfIrPreflight empty_ir_preflight = fuse::audio::preflight_hrtf_ir(empty);
    expectTrue(conv_preflight.convolutionReason == fuse::audio::HrtfPanPathConvolutionRejectReason::None,
    expectTrue(unity_preflight.reason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
                   unity_preflight, fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation),
    const fuse::audio::HrtfBinauralPreflight narrow_preflight =
    expectTrue(narrow_preflight.reason == fuse::audio::HrtfBinauralRejectReason::None,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-1b78 ---
    fuse::audio::HrtfPanPathRejectReason spatialReason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(spatialReason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(spatialReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(spatialReason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    fuse::audio::HrtfPanConvolutionRejectReason convReason =
        fuse::audio::HrtfPanConvolutionRejectReason::None;
    expectTrue(convReason == fuse::audio::HrtfPanConvolutionRejectReason::EmptyIr,
    expectTrue(convReason == fuse::audio::HrtfPanConvolutionRejectReason::None,

// --- deepen additive from deepen-b72-hrtf-reject-enums-0847 ---
    fuse::audio::HrtfIrRejectReason conv_reason = fuse::audio::HrtfIrRejectReason::None;
    expectTrue(conv_reason == fuse::audio::HrtfIrRejectReason::None,
    expectTrue(conv_reason == fuse::audio::HrtfIrRejectReason::NullSamples,
    fuse::audio::HrtfPanPathRejectReason spatial_reason =
    expectTrue(spatial_reason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(spatial_reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    fuse::audio::HrtfAttenuationCouplingRejectReason narrow_reason =
    expectTrue(narrow_reason == fuse::audio::HrtfAttenuationCouplingRejectReason::UnityAttenuation,
    expectTrue(narrow_reason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-d95a ---
    expectTrue(fuse::audio::should_skip_hrtf_ir_ready(empty),
               "should_skip_hrtf_ir_ready true for empty IR");
    reason = fuse::audio::HrtfIrRejectReason::NullSamples;
void testHrtfPanPathRejectReasonCoLocated() {
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_ready(true, valid, co_located),
               "should_skip_hrtf_pan_path_ready true when co-located");
    expectTrue(!fuse::audio::should_skip_hrtf_attenuation_coupling_ready(
    expectTrue(fuse::audio::should_skip_hrtf_binaural_ready(false, valid, offset, 0.1f, 0.1f),
               "should_skip_hrtf_binaural_ready true when disabled");
void testHrtfConvolutionRejectReasonGuards() {
    fuse::audio::HrtfConvolutionRejectReason reason =
        fuse::audio::HrtfConvolutionRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfConvolutionRejectReason::None,
    expectTrue(reason == fuse::audio::HrtfConvolutionRejectReason::EmptyIr,
    expectTrue(reason == fuse::audio::HrtfConvolutionRejectReason::MalformedIr,
    expectTrue(reason == fuse::audio::HrtfConvolutionRejectReason::HrtfDisabled,
    expectTrue(fuse::audio::should_skip_hrtf_convolution_ready(true, empty, offset),
               "should_skip_hrtf_convolution_ready true for empty IR");
void testHrtfPanPathRejectReasonNoIrOverload() {
    testHrtfPanPathRejectReasonCoLocated();
    testHrtfConvolutionRejectReasonGuards();
    testHrtfPanPathRejectReasonNoIrOverload();

// --- deepen additive from b7-2-hrtf-binaural-guards-cd83 ---
    reason = fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath;
               "preflight_hrtf_ir_ready inverts should_skip_hrtf_convolution on empty IR");

// --- deepen additive from b72-hrtf-reject-reason-guards-3841 ---
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::MalformedIr,
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::BypassPath,
    const fuse::audio::HrtfBinauralPreflight binaural_preflight =

// --- deepen additive from deepen-b72-hrtf-reject-reasons-0cb8 ---
    expectTrue(!fuse::audio::preflightHrtfIrReady(empty, &reason),
    expectTrue(fuse::audio::preflightHrtfIrReady(valid, &reason),
               "malformed IR fails tryPreflightHrtfIr");
    expectTrue(fuse::audio::classifyHrtfIrReject(fuse::audio::preflight_hrtf_ir(malformed))
               "classifyHrtfIrReject marks malformed IR");
    fuse::audio::HrtfPanPathRejectReason panReason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(fuse::audio::preflightHrtfPanPathReady(true, empty, offset, &panReason),
    expectTrue(panReason == fuse::audio::HrtfPanPathRejectReason::None,
    expectTrue(!fuse::audio::preflightHrtfPanPathReady(false, valid, offset, &panReason),
    expectTrue(panReason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(!fuse::audio::tryPreflightHrtfPanPath(true, valid, co_located, panReason),
               "co-located source fails tryPreflightHrtfPanPath");
    expectTrue(panReason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    fuse::audio::HrtfPanConvolutionRejectReason convolutionReason =
    expectTrue(fuse::audio::preflightHrtfPanConvolutionReady(true, valid, offset,
    expectTrue(convolutionReason == fuse::audio::HrtfPanConvolutionRejectReason::None,
    expectTrue(!fuse::audio::preflightHrtfPanConvolutionReady(true, empty, offset,
    expectTrue(convolutionReason == fuse::audio::HrtfPanConvolutionRejectReason::EmptyIr,
    expectTrue(std::strcmp(fuse::audio::hrtfPanConvolutionRejectReasonLabel(
                   fuse::audio::HrtfPanConvolutionRejectReason::EmptyIr),
    expectTrue(fuse::audio::preflightHrtfAttenuationCouplingReady(
    expectTrue(!fuse::audio::preflightHrtfAttenuationCouplingReady(
               "unity attenuation fails tryPreflightHrtfAttenuationCoupling");
               "classifyHrtfAttenuationCouplingReject marks bypass path");
    expectTrue(fuse::audio::preflightHrtfBinauralPanReady(true, empty, offset, 0.2f, 0.3f, {}, {},
    expectTrue(!fuse::audio::preflightHrtfBinauralPanReady(false, valid, offset, 0.1f, 0.1f, {}, {},
    expectTrue(!fuse::audio::preflightHrtfBinauralConvolutionReady(true, empty, offset, 1.f, 1.f, {},
    expectTrue(!fuse::audio::tryPreflightHrtfBinauralNarrowing(true, valid, co_located, 0.1f, 0.1f,
    expectTrue(!fuse::audio::preflightHrtfBinauralNarrowingReady(true, valid, offset, 1.f, 1.f, {},
    expectTrue(fuse::audio::classifyHrtfBinauralPanReject(stub_preflight)
               "classifyHrtfBinauralPanReject is None on stub path");
    expectTrue(fuse::audio::classifyHrtfBinauralConvolutionReject(stub_preflight)
               "classifyHrtfBinauralConvolutionReject is EmptyIr on stub path");
    expectTrue(fuse::audio::classifyHrtfBinauralNarrowingReject(stub_preflight)
               "classifyHrtfBinauralNarrowingReject is None when attenuation narrows");
    expectTrue(fuse::audio::preflightHrtfIrReady(valid)
               "preflightHrtfIrReady mirrors can_convolve_hrtf_ir on valid IR");
    expectTrue(fuse::audio::preflightHrtfPanPathReady(true, empty, offset)
               "preflightHrtfPanPathReady mirrors can_apply_spatial_hrtf_pan on stub path");
    expectTrue(fuse::audio::preflightHrtfPanConvolutionReady(true, valid, offset)
               "preflightHrtfPanConvolutionReady mirrors can_convolve_hrtf_pan_path");
               "preflightHrtfAttenuationCouplingReady mirrors can_narrow_hrtf_spatial_image");
    expectTrue(fuse::audio::preflightHrtfBinauralPanReady(true, empty, offset, 0.2f, 0.3f)
               "preflightHrtfBinauralPanReady mirrors can_apply_hrtf_binaural_pan");
    expectTrue(fuse::audio::preflightHrtfBinauralConvolutionReady(true, valid, offset, 1.f, 1.f)
               "preflightHrtfBinauralConvolutionReady mirrors can_convolve_hrtf_binaural");
    expectTrue(fuse::audio::preflightHrtfBinauralNarrowingReady(true, empty, offset, 0.2f, 0.3f)
               "preflightHrtfBinauralNarrowingReady mirrors can_narrow_hrtf_binaural_spatial_image");

// --- deepen additive from deepen-b72-hrtf-reject-reasons-4454 ---
               "should_skip_hrtf_ir_preflight true on empty IR");
               "should_skip_hrtf_ir_preflight false on valid IR");
    fuse::audio::HrtfPanPathRejectReason pan_reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(pan_reason == fuse::audio::HrtfPanPathRejectReason::None,
    pan_reason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(pan_reason == fuse::audio::HrtfPanPathRejectReason::HrtfDisabled,
    expectTrue(pan_reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    fuse::audio::HrtfConvolutionRejectReason conv_reason =
    expectTrue(conv_reason == fuse::audio::HrtfConvolutionRejectReason::None,
    conv_reason = fuse::audio::HrtfConvolutionRejectReason::None;
    expectTrue(conv_reason == fuse::audio::HrtfConvolutionRejectReason::EmptyIr,
    expectTrue(conv_reason == fuse::audio::HrtfConvolutionRejectReason::CoLocated,
    expectTrue(fuse::audio::should_skip_hrtf_convolution_preflight(true, empty, offset),
               "should_skip_hrtf_convolution_preflight true on empty IR");
    expectTrue(fuse::audio::should_skip_hrtf_spatial_pan_preflight(false, offset),
               "no-IR overload should_skip spatial pan when disabled");
    expectTrue(fuse::audio::should_skip_hrtf_narrowing_preflight(fuse::audio::HrtfPanPath::Bypass,
               "should_skip_hrtf_narrowing_preflight true on bypass");
    fuse::audio::HrtfBinauralConvolutionRejectReason conv_reason =
        fuse::audio::HrtfBinauralConvolutionRejectReason::None;
    expectTrue(conv_reason == fuse::audio::HrtfBinauralConvolutionRejectReason::EmptyIr,
    conv_reason = fuse::audio::HrtfBinauralConvolutionRejectReason::None;
    expectTrue(conv_reason == fuse::audio::HrtfBinauralConvolutionRejectReason::None,
    fuse::audio::HrtfBinauralNarrowingRejectReason narrow_reason =
        fuse::audio::HrtfBinauralNarrowingRejectReason::None;
    expectTrue(narrow_reason == fuse::audio::HrtfBinauralNarrowingRejectReason::None,
    narrow_reason = fuse::audio::HrtfBinauralNarrowingRejectReason::None;
    expectTrue(narrow_reason == fuse::audio::HrtfBinauralNarrowingRejectReason::UnityAttenuation,
    expectTrue(fuse::audio::should_skip_hrtf_binaural_preflight(false, offset, 0.1f, 0.1f),
               "should_skip_hrtf_binaural_preflight true when disabled");

// --- deepen additive from b72-hrtf-reject-reasons-9289 ---
               "should_skip_hrtf_pan_path_preflight false for spatial stub path");
    expectTrue(reason == fuse::audio::HrtfBinauralRejectReason::NullSamples,

// --- deepen additive from deepen-b7-2-hrtf-reject-reasons-364a ---
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_preflight(true, valid, offset),
               "should_skip_hrtf_pan_path_preflight false on valid path");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_spatial_preflight(false, valid, offset, 0.1f,
               "should_skip_hrtf_binaural_spatial_preflight true when disabled");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_convolution_preflight(true, empty, offset,
               "should_skip_hrtf_binaural_convolution_preflight true for empty IR");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_coupling_preflight(true, empty, offset,
               "should_skip_hrtf_binaural_coupling_preflight false when narrowing applies");

// --- deepen additive from deepen-b72-hrtf-reject-reasons-aba4 ---
void testHrtfIrRejectReasonEnumsAndPreflightWrappers() {
void testHrtfPanPathRejectReasonEnumsAndPreflightWrappers() {
    fuse::audio::HrtfPanPathRejectReason rejectReason = fuse::audio::HrtfPanPathRejectReason::None;
    expectTrue(rejectReason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
    expectTrue(stub_preflight.rejectReason == fuse::audio::HrtfPanPathRejectReason::None,
void testHrtfAttenuationCouplingRejectReasonEnumsAndPreflightWrappers() {
    expectTrue(rejectReason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
void testHrtfBinauralRejectReasonEnumsAndPreflightWrappers() {
    expectTrue(rejectReason == fuse::audio::HrtfBinauralRejectReason::CoLocated,
    testHrtfIrRejectReasonEnumsAndPreflightWrappers();
    testHrtfPanPathRejectReasonEnumsAndPreflightWrappers();
    testHrtfAttenuationCouplingRejectReasonEnumsAndPreflightWrappers();
    testHrtfBinauralRejectReasonEnumsAndPreflightWrappers();

// --- deepen additive from deepen-b7-2-hrtf-binaural-b048 ---
    expectTrue(!fuse::audio::should_skip_hrtf_ir_ready(valid),
               "should_skip_hrtf_ir_ready false for valid IR");
    expectTrue(fuse::audio::should_skip_hrtf_pan_path_ready(false, valid, offset),
               "should_skip_hrtf_pan_path_ready true when disabled");
    expectTrue(!fuse::audio::should_skip_hrtf_pan_path_ready(true, empty, offset),
               "should_skip_hrtf_pan_path_ready false on stub path");
    expectTrue(reason == fuse::audio::HrtfConvolutionRejectReason::NullSamples,
    expectTrue(reason == fuse::audio::HrtfConvolutionRejectReason::CoLocated,
                   fuse::audio::HrtfConvolutionRejectReason::HrtfDisabled),
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_ready(
               "should_skip_hrtf_attenuation_coupling_ready true on bypass");
               "should_skip_hrtf_attenuation_coupling_ready false when narrowing applies");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_ready(true, empty, offset, 0.2f, 0.3f),
               "should_skip_hrtf_binaural_ready false on stub path");

// --- deepen additive from deepen-b7-2-hrtf-reject-reasons-30ff ---
    expectTrue(fuse::audio::should_skip_hrtf_ir_convolution_preflight(empty),
               "should_skip_hrtf_ir_convolution_preflight true for empty IR");
    expectTrue(fuse::audio::should_skip_hrtf_pan_spatial_preflight(false, valid, offset),
               "should_skip_hrtf_pan_spatial_preflight true when disabled");
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_coupling_narrowing_preflight(
               "should_skip coupling narrowing true on bypass path");

// --- deepen additive from b7-2-hrtf-reject-reasons-62d8 ---
               "should_skip_hrtf_ir_ready true on empty IR");
               "should_skip_hrtf_ir_ready false on valid IR");

// --- deepen additive from deepen-b72-hrtf-reject-reasons-75f1 ---
void testHrtfPanConvolveRejectReasonGuards() {
    fuse::audio::HrtfPanConvolveRejectReason reason =
        fuse::audio::HrtfPanConvolveRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfPanConvolveRejectReason::None,
    reason = fuse::audio::HrtfPanConvolveRejectReason::None;
    expectTrue(reason == fuse::audio::HrtfPanConvolveRejectReason::EmptyIr,
    expectTrue(reason == fuse::audio::HrtfPanConvolveRejectReason::MalformedIr,
    expectTrue(fuse::audio::should_skip_hrtf_pan_convolve(true, empty, offset),
    const fuse::audio::HrtfPanPathPreflight malformed_preflight =
                   fuse::audio::HrtfPanConvolveRejectReason::EmptyIr),
    expectTrue(fuse::audio::should_skip_hrtf_binaural_ready(true, valid, co_located, 0.1f, 0.1f),
    testHrtfPanConvolveRejectReasonGuards();

// --- deepen additive from b7-2-hrtf-reject-reasons-6afb ---
               "should_skip coupling preflight true at unity attenuation");
    expectTrue(fuse::audio::should_skip_hrtf_binaural_preflight(false, valid, offset, 0.1f, 0.1f),
               "should_skip binaural preflight true when disabled");
    expectTrue(fuse::audio::should_skip_hrtf_convolution_preflight(true, empty, offset, 1.f, 1.f),
               "should_skip convolution preflight true on empty IR");
                   fuse::audio::HrtfConvolutionRejectReason::MalformedIr),
               "preflight_hrtf_binaural_ready mirrors should_skip inverse");

// --- deepen additive from deepen-b7-2-hrtf-reject-reasons-d1c9 ---
    expectTrue(!fuse::audio::should_skip_spatial_hrtf_pan_preflight(true, empty, offset),
               "should_skip_spatial_hrtf_pan_preflight false on valid stub path");
    expectTrue(fuse::audio::should_skip_spatial_hrtf_pan_preflight(false, valid, offset),
               "should_skip_spatial_hrtf_pan_preflight true when disabled");
    expectTrue(spatial_reason == fuse::audio::HrtfPanPathRejectReason::CoLocated,
               "should_skip_hrtf_convolution_preflight true for empty IR");
    expectTrue(conv_reason == fuse::audio::HrtfConvolutionRejectReason::MalformedIr,
    expectTrue(fuse::audio::should_skip_hrtf_attenuation_narrowing_preflight(
               "should_skip_hrtf_attenuation_narrowing_preflight true on bypass");
    expectTrue(!fuse::audio::should_skip_hrtf_binaural_spatial_preflight(true, empty, offset, 0.2f,
               "should_skip_hrtf_binaural_spatial_preflight false on stub path");

// --- deepen additive from deepen-b72-hrtf-reject-reasons-a4b4 ---
    expectTrue(fuse::audio::should_skip_hrtf_spatial_pan_preflight(false, valid, offset),
               "should_skip_hrtf_spatial_pan_preflight true when disabled");
    fuse::audio::HrtfPanPathConvolutionRejectReason convReason =
        fuse::audio::HrtfPanPathConvolutionRejectReason::None;
    expectTrue(convReason == fuse::audio::HrtfPanPathConvolutionRejectReason::None,
    expectTrue(convReason == fuse::audio::HrtfPanPathConvolutionRejectReason::NullSamples,
    expectTrue(convReason == fuse::audio::HrtfPanPathConvolutionRejectReason::HrtfDisabled,
               "should_skip_hrtf_binaural_preflight false on spatial stub path");
    fuse::audio::HrtfAttenuationCouplingRejectReason narrowReason =
    expectTrue(narrowReason == fuse::audio::HrtfAttenuationCouplingRejectReason::None,
    expectTrue(narrowReason == fuse::audio::HrtfAttenuationCouplingRejectReason::BypassPath,
