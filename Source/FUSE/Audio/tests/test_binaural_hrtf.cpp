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

void testEmptyIrGuardEdgeCases() {
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty), "make_empty_hrtf_ir is empty");
    expectTrue(!fuse::audio::has_hrtf_ir(empty), "empty factory has no IR");

    const float samples[] = {1.f};
    const fuse::audio::HrtfIrStub null_samples_nonzero_length{nullptr, 1};
    expectTrue(fuse::audio::is_empty_hrtf_ir(null_samples_nonzero_length),
               "null samples with non-zero length is empty");
    expectTrue(!fuse::audio::should_use_hrtf_ir(null_samples_nonzero_length),
               "null samples cannot select convolution");

    const fuse::audio::HrtfIrStub zero_length{samples, 0};
    expectTrue(fuse::audio::is_empty_hrtf_ir(zero_length), "zero-length IR is empty");
    expectTrue(fuse::audio::is_empty_hrtf_ir(empty) == !fuse::audio::has_hrtf_ir(empty),
               "is_empty_hrtf_ir inverts has_hrtf_ir");
}

void testHrtfPanPathQueryHelpers() {
    expectTrue(!fuse::audio::is_hrtf_pan_path_spatial(fuse::audio::HrtfPanPath::Bypass),
               "bypass path is not spatial");
    expectTrue(fuse::audio::is_hrtf_pan_path_spatial(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub path is spatial");
    expectTrue(fuse::audio::is_hrtf_pan_path_spatial(fuse::audio::HrtfPanPath::Convolution),
               "convolution path is spatial");

    expectTrue(!fuse::audio::hrtf_pan_path_uses_convolution(fuse::audio::HrtfPanPath::IldItdStub),
               "ILD stub does not use convolution");
    expectTrue(fuse::audio::hrtf_pan_path_uses_convolution(fuse::audio::HrtfPanPath::Convolution),
               "convolution path uses convolution");

    const fuse::audio::Vec3 offset{3.f, 0.f, 0.f};
    expectTrue(fuse::audio::resolve_hrtf_pan_path(true, offset)
                   == fuse::audio::resolve_hrtf_pan_path(true, fuse::audio::make_empty_hrtf_ir(),
                                                         offset),
               "no-IR resolve matches empty IR stub");
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

void testResolvedPanPipeline() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();
    const float samples[] = {1.f, 0.f};
    const fuse::audio::HrtfIrStub valid{samples, 2};

    const fuse::audio::BinauralPanGains resolved =
        fuse::audio::compute_binaural_pan_gains_resolved(true, empty, offset);
    const fuse::audio::BinauralPanGains guarded =
        fuse::audio::compute_binaural_pan_gains_guarded(true, empty, offset);
    expectNear(resolved.left, guarded.left, 1e-5f, "resolved matches guarded for empty IR");
    expectNear(resolved.right, guarded.right, 1e-5f, "resolved matches guarded for empty IR");

    const fuse::audio::BinauralPanGains resolved_valid =
        fuse::audio::compute_binaural_pan_gains_resolved(true, valid, offset);
    expectNear(resolved_valid.left, resolved.left, 1e-4f,
               "convolution path stub matches ILD/ITD until IR wired");
}

void testOcclusionOnlyCoupling() {
    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    const float wide_spread = fuse::audio::compute_pan_spread(wide);

    fuse::audio::BinauralPanGains occluded = wide;
    fuse::audio::apply_hrtf_occlusion_coupling(occluded, 0.1f);
    expectTrue(fuse::audio::compute_pan_spread(occluded) < wide_spread,
               "occlusion-only coupling narrows pan spread");

    expectNear(fuse::audio::compute_hrtf_occlusion_blend(1.f), 1.f, 1e-5f,
               "unity occlusion keeps full spatial blend");
    expectNear(fuse::audio::compute_hrtf_occlusion_blend(0.f), 0.25f, 1e-5f,
               "zero occlusion reaches min spatial blend");
}

void testSpatialBlendWeightExtremes() {
    fuse::audio::HrtfAttenuationCoupling distance_only;
    distance_only.occlusion_weight = 0.f;
    fuse::audio::HrtfAttenuationCoupling occlusion_only;
    occlusion_only.occlusion_weight = 1.f;

    expectNear(fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f, distance_only),
               fuse::audio::compute_hrtf_distance_factor(0.2f), 1e-5f,
               "zero occlusion weight uses distance factor only");
    expectNear(fuse::audio::compute_hrtf_spatial_blend(0.2f, 0.8f, occlusion_only),
               fuse::audio::compute_hrtf_occlusion_blend(0.8f), 1e-5f,
               "unity occlusion weight uses occlusion factor only");
}

void testResolvedCoupledPan() {
    const fuse::audio::Vec3 offset{5.f, 0.f, 0.f};
    const fuse::audio::HrtfIrStub empty = fuse::audio::make_empty_hrtf_ir();

    const fuse::audio::BinauralPanGains resolved =
        fuse::audio::compute_binaural_pan_gains_resolved_coupled(true, empty, offset, 0.15f, 0.2f);
    const fuse::audio::BinauralPanGains coupled =
        fuse::audio::compute_binaural_pan_gains_coupled(true, empty, offset, 0.15f, 0.2f);
    expectNear(resolved.left, coupled.left, 1e-5f, "resolved coupled matches coupled helper");
    expectNear(resolved.right, coupled.right, 1e-5f, "resolved coupled matches coupled helper");

    const fuse::audio::BinauralPanGains bypassed =
        fuse::audio::compute_binaural_pan_gains_resolved_coupled(false, empty, offset, 0.1f, 0.1f);
    expectTrue(fuse::audio::is_centre_panned(bypassed),
               "resolved coupled bypasses coupling when HRTF disabled");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHrtfPanPathResolution();
    testShouldUseHrtfIrAlias();
    testEmptyIrGuardEdgeCases();
    testHrtfPanPathQueryHelpers();
    testItdAndElevationHelpers();
    testIrAwareGuardedPan();
    testSpatialBlendHelper();
    testLerpBinauralPanGains();
    testCoupledPanOneShot();
    testPanPathForPathHelper();
    testResolvedPanPipeline();
    testOcclusionOnlyCoupling();
    testSpatialBlendWeightExtremes();
    testResolvedCoupledPan();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("test_binaural_hrtf: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "test_binaural_hrtf: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
