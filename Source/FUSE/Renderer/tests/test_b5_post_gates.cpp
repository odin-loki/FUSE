// B5.12 gate rows for B5.10 post-processing (CPU reference of the post passes):
//  1. Bloom only affects pixels above threshold — a black frame (or any frame with no pixel above
//     threshold - knee) produces exactly zero bloom; a single bright pixel produces a bloom that is
//     energy-bounded, mirror-symmetric and decays with distance; the soft knee is C0/C1 continuous.
//  2. DOF circle of confusion matches the thin-lens formula (checked against an independent
//     geometric derivation from image distances), is zero at the focus distance, negative in the
//     near field and positive in the far field, and converts to pixels via the sensor width.
//  3. Motion blur trails along the velocity vector: a moving dot smears only along its velocity,
//     trail length = |v| * shutter/360 (proportional, clamped), zero velocity is the identity.
//  4. ACES tonemap maps 0.18 grey to 0.18: the curve is the published ACES filmic rational fit
//     (verified against the constants in double precision); with the engine's mid-grey calibration
//     exposure (closed-form solve of the fit) scene 0.18 -> display-linear 0.18 (sRGB code 0.4614).
//  5. Film grain is temporally decorrelated: zero mean, variance intensity^2/3, frame N vs N+1
//     correlation ~0, spatially white, and the temporal average shows no fixed pattern.
//  6. Post chain order bloom -> DoF -> motion blur -> tonemap -> grade -> grain; auto exposure
//     compensates scene brightness back to mid grey.
// Bloom + DoF + motion blur + tonemap < 1 ms is a GPU timing row (hardware-only).

#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/dof.hpp>
#include <fuse/renderer/postprocess/motion_blur.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f64 actual, f64 expected, f64 epsilon, const char* message) {
    if (!(std::fabs(actual - expected) <= epsilon)) {
        std::fprintf(stderr, "FAIL: %s (expected %.7f, got %.7f)\n", message, expected, actual);
        ++g_failures;
    }
}

f64 lum(const Vec3& c) {
    return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
}

bool isExactZero(const Vec3& c) {
    return c.x == 0.f && c.y == 0.f && c.z == 0.f;
}

// ---------------------------------------------------------------------------------------------
// 1. Bloom

// Independent soft-knee reference (threshold/knee curve as published for HDR bloom prefilters):
// response = max(clamp(l - t + k, 0, 2k)^2 / 4k, l - t), zero at or below t - k.
f64 referenceKnee(f64 l, f64 t, f64 k) {
    if (k <= 0.0) {
        return std::max(l - t, 0.0);
    }
    if (l <= t - k) {
        return 0.0;
    }
    const f64 rq = std::clamp(l - t + k, 0.0, 2.0 * k);
    return std::max(rq * rq / (4.0 * k), l - t);
}

void testBloomSoftKnee() {
    BloomParams params{};
    params.threshold = 1.f;
    params.knee = 0.5f;
    f64 maxErr = 0.0;
    for (u32 i = 0; i <= 4000; ++i) {
        const f64 l = 4.0 * static_cast<f64>(i) / 4000.0;
        const f64 got = Bloom::thresholdResponse(static_cast<f32>(l), params);
        maxErr = std::max(maxErr, std::fabs(got - referenceKnee(static_cast<f32>(l), 1.0, 0.5)));
    }
    std::printf("bloom knee: max |response - reference| over [0,4] = %.3g\n", maxErr);
    expectTrue(maxErr < 1e-6, "soft knee matches the reference threshold curve");

    // C0 + C1 continuity at both knee ends (the pre-fix code jumped from knee to 2*knee at t + k).
    const f32 h = 1e-3f;
    for (const f32 edge : {params.threshold - params.knee, params.threshold + params.knee}) {
        const f32 below = Bloom::thresholdResponse(edge - h, params);
        const f32 at = Bloom::thresholdResponse(edge, params);
        const f32 above = Bloom::thresholdResponse(edge + h, params);
        expectTrue(std::fabs(above - below) < 2.5f * h, "soft knee is continuous at the knee edges");
        const f32 slopeLeft = (at - below) / h;
        const f32 slopeRight = (above - at) / h;
        expectTrue(std::fabs(slopeLeft - slopeRight) < 0.01f, "soft knee slope is continuous at the knee edges");
    }
    expectTrue(Bloom::thresholdResponse(params.threshold - params.knee, params) == 0.f,
               "response is exactly zero at threshold - knee");
    expectNear(Bloom::thresholdResponse(3.f, params), 2.0, 1e-6, "response is lum - threshold above the knee");

    BloomParams hard = params;
    hard.knee = 0.f;
    expectTrue(Bloom::thresholdResponse(1.f, hard) == 0.f, "hard threshold: exactly zero at threshold");
    expectNear(Bloom::thresholdResponse(1.25f, hard), 0.25, 1e-6, "hard threshold: linear above");
    const Vec3 colored{3.f, 1.f, 0.5f};
    const Vec3 bright = Bloom::extractBright(colored, params);
    expectNear(bright.x / bright.y, 3.0, 1e-5, "extractBright preserves chromaticity");
    expectNear(lum(bright), referenceKnee(lum(colored), 1.0, 0.5), 1e-5, "extractBright luminance = response");
}

std::vector<Vec3> makeFrame(u32 w, u32 h, const Vec3& fill = {}) {
    return std::vector<Vec3>(static_cast<size_t>(w) * h, fill);
}

void testBloomBelowThresholdIsExactlyZero() {
    const u32 w = 96;
    const u32 h = 64;
    BloomParams params{};
    std::vector<Vec3> bloom;

    const std::vector<Vec3> black = makeFrame(w, h);
    bloom_image(black.data(), w, h, params, bloom);
    bool allZero = bloom.size() == black.size();
    for (const Vec3& v : bloom) {
        allZero = allZero && isExactZero(v);
    }
    expectTrue(allZero, "black frame produces exactly zero bloom");

    // Random frame with every pixel at or below threshold - knee (soft knee) -> exactly zero.
    std::mt19937 rng(7u);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    std::vector<Vec3> dim = makeFrame(w, h);
    const f32 limit = params.threshold - params.knee;
    f64 maxLum = 0.0;
    for (Vec3& v : dim) {
        v = Vec3{unit(rng), unit(rng), unit(rng)};
        const f32 l = Bloom::luminance(v);
        if (l > limit) {
            v = v * (limit * 0.999f / l);
        }
        maxLum = std::max(maxLum, static_cast<f64>(Bloom::luminance(v)));
    }
    bloom_image(dim.data(), w, h, params, bloom);
    allZero = true;
    for (const Vec3& v : bloom) {
        allZero = allZero && isExactZero(v);
    }
    std::printf("bloom: sub-knee frame max luminance %.4f (cutoff %.4f) -> bloom exactly zero: %s\n", maxLum, limit,
                allZero ? "yes" : "no");
    expectTrue(allZero, "frame with no pixel above threshold - knee produces exactly zero bloom");

    std::vector<Vec3> composite;
    bloom_composite(dim.data(), w, h, params, composite);
    bool identity = true;
    for (size_t i = 0; i < dim.size(); ++i) {
        identity = identity && composite[i].x == dim[i].x && composite[i].y == dim[i].y && composite[i].z == dim[i].z;
    }
    expectTrue(identity, "bloom composite leaves a sub-threshold frame bit-identical");

    // Hard threshold: every pixel <= threshold -> zero.
    BloomParams hard = params;
    hard.knee = 0.f;
    std::vector<Vec3> atThreshold = makeFrame(w, h, Vec3{1.f, 1.f, 1.f});
    bloom_image(atThreshold.data(), w, h, hard, bloom);
    allZero = true;
    for (const Vec3& v : bloom) {
        allZero = allZero && isExactZero(v);
    }
    expectTrue(allZero, "hard threshold: frame at exactly threshold produces zero bloom");

    // Sub-threshold content does not change the bloom of a bright pixel (bit-identical).
    std::vector<Vec3> spotOnly = makeFrame(w, h);
    std::vector<Vec3> spotOnDim = dim;
    spotOnly[static_cast<size_t>(32) * w + 40] = Vec3{6.f, 6.f, 6.f};
    spotOnDim[static_cast<size_t>(32) * w + 40] = Vec3{6.f, 6.f, 6.f};
    std::vector<Vec3> bloomA;
    std::vector<Vec3> bloomB;
    bloom_image(spotOnly.data(), w, h, params, bloomA);
    bloom_image(spotOnDim.data(), w, h, params, bloomB);
    bool same = bloomA.size() == bloomB.size();
    for (size_t i = 0; same && i < bloomA.size(); ++i) {
        same = bloomA[i].x == bloomB[i].x && bloomA[i].y == bloomB[i].y && bloomA[i].z == bloomB[i].z;
    }
    expectTrue(same, "sub-threshold pixels contribute nothing to bloom");
}

void testBloomSinglePixel() {
    const u32 n = 128;
    BloomParams params{};
    params.mip_levels = 5;
    std::vector<Vec3> frame = makeFrame(n, n);
    const u32 cx = 64;
    const u32 cy = 64;
    frame[static_cast<size_t>(cy) * n + cx] = Vec3{10.f, 10.f, 10.f};

    std::vector<Vec3> bloom;
    bloom_image(frame.data(), n, n, params, bloom);
    f64 energy = 0.0;
    f64 minValue = 0.0;
    for (const Vec3& v : bloom) {
        energy += lum(v);
        minValue = std::min(minValue, static_cast<f64>(std::min({v.x, v.y, v.z})));
    }
    const f64 prefilterEnergy = referenceKnee(10.0, params.threshold, params.knee);
    std::printf("bloom single pixel: energy %.6f vs prefilter %.6f (ratio %.6f), levels %u\n", energy, prefilterEnergy,
                energy / prefilterEnergy, bloom_level_count(n, n, params));
    expectTrue(minValue >= 0.0, "bloom is non-negative");
    expectTrue(energy <= prefilterEnergy * (1.0 + 1e-4), "bloom energy bounded by the thresholded energy");
    expectTrue(energy >= prefilterEnergy * 0.99, "bloom conserves thresholded energy away from borders");

    // Decay with distance along 8 rays from the bright pixel.
    const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    bool monotone = true;
    for (const auto& d : dirs) {
        f64 previous = lum(bloom[static_cast<size_t>(cy) * n + cx]);
        for (int step = 1; step < 60; ++step) {
            const int x = static_cast<int>(cx) + d[0] * step;
            const int y = static_cast<int>(cy) + d[1] * step;
            const f64 value = lum(bloom[static_cast<size_t>(y) * n + static_cast<size_t>(x)]);
            monotone = monotone && value <= previous * (1.0 + 1e-6) + 1e-12;
            previous = value;
        }
    }
    expectTrue(monotone, "single-pixel bloom decays monotonically with distance");
    const f64 peak = lum(bloom[static_cast<size_t>(cy) * n + cx]);
    const f64 at8 = lum(bloom[static_cast<size_t>(cy) * n + cx + 8]);
    const f64 at32 = lum(bloom[static_cast<size_t>(cy) * n + cx + 32]);
    std::printf("bloom single pixel: peak %.5f, r=8 %.3g, r=32 %.3g\n", peak, at8, at32);
    expectTrue(at8 > 0.0, "bloom spreads beyond the source pixel");
    expectTrue(at32 < at8 && at32 < peak * 1e-2, "bloom falls off far from the source");

    // Mirror symmetry: a spot centred on the pyramid's symmetry point (2x2 block at the centre).
    std::vector<Vec3> centred = makeFrame(n, n);
    for (u32 y = n / 2 - 1; y <= n / 2; ++y) {
        for (u32 x = n / 2 - 1; x <= n / 2; ++x) {
            centred[static_cast<size_t>(y) * n + x] = Vec3{4.f, 4.f, 4.f};
        }
    }
    bloom_image(centred.data(), n, n, params, bloom);
    f64 maxAsym = 0.0;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f64 v = lum(bloom[static_cast<size_t>(y) * n + x]);
            const f64 mx = lum(bloom[static_cast<size_t>(y) * n + (n - 1 - x)]);
            const f64 my = lum(bloom[static_cast<size_t>(n - 1 - y) * n + x]);
            const f64 tr = lum(bloom[static_cast<size_t>(x) * n + y]);
            maxAsym = std::max({maxAsym, std::fabs(v - mx), std::fabs(v - my), std::fabs(v - tr)});
        }
    }
    std::printf("bloom centred spot: max mirror/transpose asymmetry %.3g\n", maxAsym);
    expectTrue(maxAsym < 1e-6, "centred bloom is symmetric under x/y mirror and transpose");

    // Operator equivariance: bloom(mirror(img)) == mirror(bloom(img)) for a random sparse frame.
    std::mt19937 rng(11u);
    std::uniform_int_distribution<u32> coord(0u, n - 1u);
    std::uniform_real_distribution<f32> energyDist(1.f, 20.f);
    std::vector<Vec3> sparse = makeFrame(n, n);
    for (u32 i = 0; i < 40; ++i) {
        sparse[static_cast<size_t>(coord(rng)) * n + coord(rng)] = Vec3{energyDist(rng), energyDist(rng), energyDist(rng)};
    }
    std::vector<Vec3> mirrored = makeFrame(n, n);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            mirrored[static_cast<size_t>(x) * n + (n - 1 - y)] = sparse[static_cast<size_t>(y) * n + x];
        }
    }
    std::vector<Vec3> bloomSparse;
    std::vector<Vec3> bloomMirrored;
    bloom_image(sparse.data(), n, n, params, bloomSparse);
    bloom_image(mirrored.data(), n, n, params, bloomMirrored);
    f64 maxEquiv = 0.0;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const Vec3& a = bloomSparse[static_cast<size_t>(y) * n + x];
            const Vec3& b = bloomMirrored[static_cast<size_t>(x) * n + (n - 1 - y)];
            maxEquiv = std::max({maxEquiv, static_cast<f64>(std::fabs(a.x - b.x)),
                                 static_cast<f64>(std::fabs(a.y - b.y)), static_cast<f64>(std::fabs(a.z - b.z))});
        }
    }
    expectTrue(maxEquiv < 1e-5, "bloom is equivariant under 90-degree rotation");
}

// ---------------------------------------------------------------------------------------------
// 2. Depth of field

// Independent geometric thin-lens reference: image distances v = 1 / (1/f - 1/s); a point at s2
// focuses at v2, the sensor sits at v1, the cone of half-angle A/2 over v2 gives blur A |v2 - v1| / v2.
f64 referenceCocMm(f64 focalMm, f64 fStop, f64 focusM, f64 objectM) {
    const f64 aperture = focalMm / fStop;
    const f64 s1 = focusM * 1000.0;
    const f64 s2 = objectM * 1000.0;
    const f64 v1 = 1.0 / (1.0 / focalMm - 1.0 / s1);
    const f64 v2 = 1.0 / (1.0 / focalMm - 1.0 / s2);
    return aperture * std::fabs(v2 - v1) / v2;
}

void testDofCircleOfConfusion() {
    struct Lens {
        f32 focal;
        f32 fStop;
        f32 focus;
    };
    const Lens lenses[] = {{50.f, 2.8f, 10.f}, {85.f, 1.4f, 3.f}, {24.f, 8.f, 2.f}, {35.f, 2.f, 1.f}, {200.f, 4.f, 50.f}};
    const f32 distances[] = {0.5f, 0.8f, 1.f, 1.5f, 2.f, 3.f, 5.f, 8.f, 10.f, 20.f, 50.f, 100.f, 1000.f};
    f64 maxRel = 0.0;
    bool signsOk = true;
    bool zeroAtFocus = true;
    for (const Lens& lens : lenses) {
        DOFParams params{};
        params.focal_length = lens.focal;
        params.f_stop = lens.fStop;
        params.focal_distance = lens.focus;
        zeroAtFocus = zeroAtFocus && dof_coc_diameter_mm(lens.focus, params) == 0.f &&
                      dof_coc_radius_px(lens.focus, params, 1920u) == 0.f;
        f64 previousFar = 0.0;
        for (const f32 s : distances) {
            if (s * 1000.f <= lens.focal * 1.01f) {
                continue;
            }
            const f64 got = dof_coc_diameter_mm(s, params);
            const f64 ref = referenceCocMm(lens.focal, lens.fStop, lens.focus, s);
            // Textbook form |A f (S1 - S2)| / (S2 (S1 - f)) evaluated in double.
            const f64 a = static_cast<f64>(lens.focal) / lens.fStop;
            const f64 textbook = std::fabs(a * lens.focal * (lens.focus * 1000.0 - s * 1000.0)) /
                                 (s * 1000.0 * (lens.focus * 1000.0 - lens.focal));
            if (ref > 1e-9) {
                maxRel = std::max({maxRel, std::fabs(std::fabs(got) - ref) / ref, std::fabs(textbook - ref) / ref});
            }
            if (s < lens.focus) {
                signsOk = signsOk && got < 0.0;
            } else if (s > lens.focus) {
                signsOk = signsOk && got > 0.0 && got >= previousFar;
                previousFar = got;
            }
        }
        // Far-field asymptote A f / (S1 - f).
        const f64 aperture = static_cast<f64>(lens.focal) / lens.fStop;
        const f64 asymptote = aperture * lens.focal / (lens.focus * 1000.0 - lens.focal);
        expectNear(dof_coc_diameter_mm(1e6f, params), asymptote, asymptote * 1e-3, "far CoC approaches A f / (S1 - f)");
    }
    std::printf("dof: max relative CoC error vs geometric thin lens = %.3g\n", maxRel);
    expectTrue(maxRel < 1e-5, "CoC matches thin-lens formula for test focal distances");
    expectTrue(signsOk, "CoC sign: near negative, far positive and growing with distance");
    expectTrue(zeroAtFocus, "CoC is exactly zero at the focus distance");

    // Pixel conversion via sensor width.
    DOFParams params{};
    const f32 radius = dof_coc_radius_px(5.f, params, 1920u);
    const f64 expected = 0.5 * referenceCocMm(50.0, 2.8, 10.0, 5.0) / 36.0 * 1920.0;
    std::printf("dof: 50mm f/2.8 focus 10m, object 5m -> CoC %.5f mm, radius %.4f px @1920 (ref %.4f)\n",
                referenceCocMm(50.0, 2.8, 10.0, 5.0), -radius, expected);
    expectNear(-radius, expected, expected * 1e-5, "CoC pixel radius = 0.5 CoC / sensor width * image width");
    DOFParams noNear = params;
    noNear.near_blur = false;
    expectTrue(dof_coc_radius_px(5.f, noNear, 1920u) == 0.f, "near_blur=false zeroes near-field CoC");
    expectTrue(dof_coc_radius_px(20.f, noNear, 1920u) > 0.f, "near_blur=false keeps far-field CoC");

    // Depth linearisation round-trips for the three depth conventions.
    const f32 nearPlane = 0.1f;
    const f32 farPlane = 500.f;
    for (const f32 z : {0.2f, 1.f, 7.5f, 42.f, 300.f}) {
        const f32 dRevInf = nearPlane / z;
        const f32 dRev = nearPlane * (farPlane - z) / (z * (farPlane - nearPlane));
        const f32 dFwd = farPlane * (z - nearPlane) / (z * (farPlane - nearPlane));
        expectNear(dof_linear_depth(dRevInf, nearPlane, 0.f, true), z, z * 1e-4, "reversed-Z infinite depth linearises");
        expectNear(dof_linear_depth(dRev, nearPlane, farPlane, true), z, z * 1e-3, "reversed-Z finite depth linearises");
        expectNear(dof_linear_depth(dFwd, nearPlane, farPlane, false), z, z * 1e-3, "forward depth linearises");
    }
}

void testDofPass() {
    const u32 w = 64;
    const u32 h = 64;
    DOFParams params{};
    std::mt19937 rng(3u);
    std::uniform_real_distribution<f32> unit(0.f, 2.f);
    std::vector<Vec3> color = makeFrame(w, h);
    for (Vec3& v : color) {
        v = Vec3{unit(rng), unit(rng), unit(rng)};
    }
    std::vector<f32> focusDepth(static_cast<size_t>(w) * h, params.focal_distance);
    std::vector<Vec3> out;
    dof_pass(color.data(), focusDepth.data(), w, h, params, out);
    bool identity = true;
    for (size_t i = 0; i < color.size(); ++i) {
        identity = identity && out[i].x == color[i].x && out[i].y == color[i].y && out[i].z == color[i].z;
    }
    expectTrue(identity, "DoF leaves an in-focus frame unchanged");

    // A point at an out-of-focus depth spreads into a disc of the thin-lens CoC radius.
    params.focal_length = 85.f;
    params.f_stop = 1.4f;
    const f32 objectDistance = 1.f;
    const f32 radius = std::fabs(dof_coc_radius_px(objectDistance, params, w));
    std::vector<Vec3> dot = makeFrame(w, h);
    dot[static_cast<size_t>(32) * w + 32] = Vec3{1.f, 1.f, 1.f};
    std::vector<f32> depth(static_cast<size_t>(w) * h, objectDistance);
    dof_pass(dot.data(), depth.data(), w, h, params, out);
    f64 maxDist = 0.0;
    f64 energy = 0.0;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f64 v = out[static_cast<size_t>(y) * w + x].x;
            if (v > 0.0) {
                maxDist = std::max(maxDist, std::hypot(static_cast<f64>(x) - 32.0, static_cast<f64>(y) - 32.0));
            }
            energy += v;
        }
    }
    std::printf("dof pass: CoC radius %.3f px, blurred footprint radius %.3f px, energy %.6f\n", radius, maxDist, energy);
    expectTrue(maxDist <= radius + 1e-4 && maxDist >= radius - 1.0, "DoF footprint radius matches CoC radius");
    expectNear(energy, 1.0, 1e-4, "DoF conserves energy");
}

// ---------------------------------------------------------------------------------------------
// 3. Motion blur

struct TrailStats {
    f64 minProj = 1e9;
    f64 maxProj = -1e9;
    f64 maxPerp = 0.0;
    u32 pixels = 0;
};

TrailStats blurDot(const Vec2& velocity, f32 shutter, f32 maxBlur, std::vector<Vec3>* outFrame = nullptr) {
    const u32 w = 96;
    const u32 h = 96;
    const u32 cx = 48;
    const u32 cy = 48;
    std::vector<Vec3> color = makeFrame(w, h);
    std::vector<Vec2> velocityBuffer(static_cast<size_t>(w) * h, Vec2{});
    std::vector<f32> depth(static_cast<size_t>(w) * h, 10.f);
    color[static_cast<size_t>(cy) * w + cx] = Vec3{1.f, 1.f, 1.f};
    velocityBuffer[static_cast<size_t>(cy) * w + cx] = velocity;
    depth[static_cast<size_t>(cy) * w + cx] = 1.f;

    MotionBlurParams params{};
    params.shutter_angle = shutter;
    params.max_blur_px = maxBlur;
    params.max_samples = 32;
    std::vector<Vec3> out;
    motion_blur_pass(color.data(), velocityBuffer.data(), depth.data(), w, h, params, out);

    const f64 len = std::hypot(velocity.x, velocity.y);
    const f64 dirX = velocity.x / len;
    const f64 dirY = velocity.y / len;
    TrailStats stats{};
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            if (out[static_cast<size_t>(y) * w + x].x <= 0.f) {
                continue;
            }
            const f64 dx = static_cast<f64>(x) - cx;
            const f64 dy = static_cast<f64>(y) - cy;
            const f64 proj = dx * dirX + dy * dirY;
            stats.minProj = std::min(stats.minProj, proj);
            stats.maxProj = std::max(stats.maxProj, proj);
            stats.maxPerp = std::max(stats.maxPerp, std::fabs(-dx * dirY + dy * dirX));
            ++stats.pixels;
        }
    }
    if (outFrame != nullptr) {
        *outFrame = out;
    }
    return stats;
}

void testMotionBlurTrail() {
    struct Case {
        Vec2 velocity;
        f32 shutter;
    };
    const Case cases[] = {{{16.f, 0.f}, 180.f}, {{8.f, 0.f}, 180.f},  {{0.f, 12.f}, 180.f}, {{-10.f, 0.f}, 360.f},
                          {{12.f, 12.f}, 180.f}, {{12.f, -5.f}, 360.f}, {{-7.f, 18.f}, 180.f}};
    // The reconstruction filter weights a swept pixel by a cone (1 - d / (L/2)), so a 1-px object leaves
    // a tent-shaped trail whose last lit pixel centre lies strictly inside the half length L/2.
    for (const Case& c : cases) {
        const TrailStats stats = blurDot(c.velocity, c.shutter, 64.f);
        const f64 expectedLength = std::hypot(c.velocity.x, c.velocity.y) * c.shutter / 360.0;
        const f64 half = expectedLength * 0.5;
        std::printf("motion blur v=(%.0f,%.0f) shutter %.0f: blur length %.2f px, lit span [%.2f, %.2f] along v, "
                    "max perpendicular %.2f px, %u px\n",
                    c.velocity.x, c.velocity.y, c.shutter, expectedLength, stats.minProj, stats.maxProj, stats.maxPerp,
                    stats.pixels);
        expectTrue(stats.maxPerp <= 0.75, "motion blur smears only along the velocity direction");
        expectTrue(stats.maxProj < half + 0.75 && stats.minProj > -half - 0.75,
                   "motion blur trail does not extend beyond |v| * shutter / 360");
        expectTrue(stats.maxProj >= half - 1.5 && stats.minProj <= -half + 1.5,
                   "motion blur trail reaches |v| * shutter / 360 (centred shutter)");
        expectTrue(std::fabs(stats.maxProj + stats.minProj) <= 1e-6, "motion blur trail is symmetric about the object");
    }

    // Reach (first unlit pixel centre along an axis) equals L/2 exactly for axis-aligned motion.
    const TrailStats slow = blurDot({8.f, 0.f}, 180.f, 64.f);
    const TrailStats fast = blurDot({24.f, 0.f}, 180.f, 64.f);
    const TrailStats wide = blurDot({8.f, 0.f}, 360.f, 64.f);
    const f64 slowReach = slow.maxProj + 1.0;
    const f64 fastReach = fast.maxProj + 1.0;
    const f64 wideReach = wide.maxProj + 1.0;
    std::printf("motion blur: reach |v|=8 @180 %.1f px, |v|=24 @180 %.1f px, |v|=8 @360 %.1f px\n", slowReach,
                fastReach, wideReach);
    expectNear(slowReach, 2.0, 1e-9, "reach = |v| * shutter / 720 (|v|=8, 180 deg)");
    expectNear(fastReach, 6.0, 1e-9, "trail length proportional to |v| (3x velocity -> 3x reach)");
    expectNear(wideReach, 4.0, 1e-9, "trail length proportional to shutter angle (360 deg -> 2x)");

    const TrailStats clamped = blurDot({200.f, 0.f}, 180.f, 20.f);
    std::printf("motion blur: |v|=200 clamped to 20 px -> reach %.1f px\n", clamped.maxProj + 1.0);
    expectNear(clamped.maxProj + 1.0, 10.0, 1e-9, "trail length clamped to max_blur_px");

    // Zero velocity is the identity (bit exact) on a random frame.
    const u32 w = 48;
    const u32 h = 40;
    std::mt19937 rng(5u);
    std::uniform_real_distribution<f32> unit(0.f, 4.f);
    std::vector<Vec3> color = makeFrame(w, h);
    for (Vec3& v : color) {
        v = Vec3{unit(rng), unit(rng), unit(rng)};
    }
    std::vector<Vec2> zero(static_cast<size_t>(w) * h, Vec2{});
    std::vector<Vec3> out;
    motion_blur_pass(color.data(), zero.data(), nullptr, w, h, MotionBlurParams{}, out);
    bool identity = true;
    for (size_t i = 0; i < color.size(); ++i) {
        identity = identity && out[i].x == color[i].x && out[i].y == color[i].y && out[i].z == color[i].z;
    }
    expectTrue(identity, "zero velocity motion blur is the identity");

    // Static background away from the trail is untouched even with non-black content.
    const u32 bw = 64;
    std::vector<Vec3> bg = makeFrame(bw, bw, Vec3{0.05f, 0.05f, 0.05f});
    std::vector<Vec2> vel(static_cast<size_t>(bw) * bw, Vec2{});
    bg[static_cast<size_t>(32) * bw + 32] = Vec3{2.f, 2.f, 2.f};
    vel[static_cast<size_t>(32) * bw + 32] = Vec2{0.f, 16.f};
    motion_blur_pass(bg.data(), vel.data(), nullptr, bw, bw, MotionBlurParams{}, out);
    bool offTrailUnchanged = true;
    bool onTrailChanged = true;
    for (u32 y = 0; y < bw; ++y) {
        for (u32 x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y) * bw + x;
            const bool onTrail = x == 32 && std::abs(static_cast<int>(y) - 32) <= 3;
            if (onTrail) {
                onTrailChanged = onTrailChanged && out[i].x != bg[i].x;
            } else if (x != 32) {
                offTrailUnchanged = offTrailUnchanged && out[i].x == bg[i].x;
            }
        }
    }
    expectTrue(offTrailUnchanged, "pixels off the velocity line are untouched");
    expectTrue(onTrailChanged, "pixels on the velocity line receive the trail");
}

// ---------------------------------------------------------------------------------------------
// 4. ACES tonemap calibration

// Published ACES filmic rational fit constants (a, b, c, d, e) = (2.51, 0.03, 2.43, 0.59, 0.14).
constexpr f64 kA = 2.51;
constexpr f64 kB = 0.03;
constexpr f64 kC = 2.43;
constexpr f64 kD = 0.59;
constexpr f64 kE = 0.14;

f64 referenceAces(f64 x) {
    return std::clamp(x * (kA * x + kB) / (x * (kC * x + kD) + kE), 0.0, 1.0);
}

f64 srgbOetf(f64 v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

void testAcesCalibration() {
    f64 maxErr = 0.0;
    f64 previous = -1.0;
    bool monotone = true;
    for (int i = 0; i <= 700; ++i) {
        const f64 x = std::pow(10.0, -4.0 + 7.0 * i / 700.0);
        const f64 got = aces_tonemap({static_cast<f32>(x), static_cast<f32>(x), static_cast<f32>(x)}).x;
        maxErr = std::max(maxErr, std::fabs(got - referenceAces(static_cast<f32>(x))));
        monotone = monotone && got >= previous;
        previous = got;
    }
    std::printf("aces: max |curve - published fit| over [1e-4, 1e3] = %.3g\n", maxErr);
    expectTrue(maxErr < 2e-6, "ACES curve matches the published rational fit");
    expectTrue(monotone, "ACES curve is monotone");
    expectTrue(aces_tonemap({0.f, 0.f, 0.f}).x == 0.f, "ACES maps black to black");
    expectTrue(aces_tonemap({100.f, 100.f, 100.f}).x == 1.f, "ACES saturates to 1");
    expectNear(aces_tonemap({0.18f, 0.18f, 0.18f}).x, 0.086724 / 0.324932, 2e-6, "un-exposed ACES(0.18) = 0.26690");
    expectNear(aces_tonemap({1.f, 1.f, 1.f}).x, 2.54 / 3.16, 1e-6, "ACES(1) = 0.80380");

    // Closed-form calibration: solve fit(x) = 0.18 -> (a - g c) x^2 + (b - g d) x - g e = 0.
    const f64 g = 0.18;
    const f64 qa = kA - g * kC;
    const f64 qb = kB - g * kD;
    const f64 qc = -g * kE;
    const f64 root = (-qb + std::sqrt(qb * qb - 4.0 * qa * qc)) / (2.0 * qa);
    const f64 referenceEv = std::log2(root / 0.18);
    const f64 engineEv = tone_mapper_mid_grey_calibration_ev(ToneMapper::ACES);
    std::printf("aces calibration: scene x* = %.6f, exposure bias %.5f EV (x%.5f), engine %.5f EV\n", root, referenceEv,
                std::exp2(referenceEv), engineEv);
    expectNear(engineEv, referenceEv, 1e-4, "ACES mid-grey calibration EV matches closed-form solve");

    // Reinhard: x / (1 + x) = g -> x = g / (1 - g); Neutral: 0 EV.
    expectNear(tone_mapper_mid_grey_calibration_ev(ToneMapper::Reinhard), std::log2((g / (1.0 - g)) / 0.18), 1e-4,
               "Reinhard mid-grey calibration EV matches closed form");
    expectTrue(tone_mapper_mid_grey_calibration_ev(ToneMapper::Neutral) == 0.f, "Neutral calibration is 0 EV");
    const f32 filmicEv = tone_mapper_mid_grey_calibration_ev(ToneMapper::Filmic);
    const f32 filmicX = 0.18f * std::exp2(filmicEv);
    expectNear(filmic_tonemap({filmicX, filmicX, filmicX}).x, 0.18, 1e-4, "Filmic calibration lands on 0.18");

    // Grade path: scene 0.18 -> display-linear 0.18 and sRGB code value OETF(0.18).
    ColorGradeParams grade{};
    grade.output_srgb = false;
    const Vec3 grey{0.18f, 0.18f, 0.18f};
    const Vec3 linear = apply_color_grade(grey, grade);
    grade.output_srgb = true;
    const Vec3 encoded = apply_color_grade(grey, grade);
    std::printf("aces grade: scene 0.18 -> display linear %.6f, sRGB-encoded %.6f (OETF(0.18) = %.6f)\n", linear.x,
                encoded.x, srgbOetf(0.18));
    expectNear(linear.x, 0.18, 1e-4, "ACES grade maps scene 0.18 to display-linear 0.18");
    expectNear(linear.y, 0.18, 1e-4, "ACES grade mid grey g");
    expectNear(linear.z, 0.18, 1e-4, "ACES grade mid grey b");
    expectNear(encoded.x, srgbOetf(0.18), 1e-4, "ACES grade mid grey sRGB code value");

    // Full stack: per-pixel and full-frame paths agree on the calibration.
    PostStack stack{};
    stack.init({});
    ColorGradeParams stackGrade{};
    stackGrade.output_srgb = false;
    stackGrade.vignette = 0.f;
    stack.setColorGradeParams(stackGrade);
    AutoExposureParams noAuto{};
    noAuto.enabled = false;
    stack.setAutoExposureParams(noAuto);
    expectNear(stack.processPixel(grey, 0u).x, 0.18, 1e-4, "post stack pixel maps 0.18 to 0.18");
    const u32 n = 16;
    std::vector<Vec3> frame = makeFrame(n, n, grey);
    std::vector<Vec3> out;
    PostFrameInput input{};
    input.hdr = frame.data();
    input.width = n;
    input.height = n;
    expectTrue(stack.processFrame(input, out), "post stack processes a mid-grey frame");
    f64 maxDev = 0.0;
    for (const Vec3& v : out) {
        maxDev = std::max(maxDev, std::fabs(static_cast<f64>(v.x) - 0.18));
    }
    expectTrue(maxDev < 1e-4, "post stack frame maps a 0.18 frame to 0.18 everywhere");

    // Calibration can be switched off (raw curve) for reference comparisons.
    ColorGradeParams raw{};
    raw.output_srgb = false;
    raw.calibrate_mid_grey = false;
    expectNear(apply_color_grade(grey, raw).x, referenceAces(0.18), 1e-5, "uncalibrated grade is the raw curve");

    // Auto exposure: a uniformly bright scene is metered and compensated back to mid grey.
    AutoExposureParams autoParams{};
    autoParams.enabled = true;
    autoParams.adaptation_speed_up = 100.f;
    autoParams.adaptation_speed_down = 100.f;
    stack.setAutoExposureParams(autoParams);
    const Vec3 bright{0.72f, 0.72f, 0.72f};
    const Vec3 samples[] = {bright, bright, bright, bright};
    f32 ev = 0.f;
    for (int i = 0; i < 10; ++i) {
        ev = stack.updateAutoExposure(samples, 4u, 0.1f);
    }
    const Vec3 compensated = stack.processPixel(bright, 0u);
    std::printf("auto exposure: scene 0.72 metered %.4f EV -> display %.5f\n", ev, compensated.x);
    expectNear(ev, 2.0, 1e-3, "auto exposure meters +2 EV for a 4x mid-grey scene");
    expectNear(compensated.x, 0.18, 1e-3, "auto exposure compensates a bright scene back to display 0.18");
    stack.destroy();
}

// ---------------------------------------------------------------------------------------------
// 5. Film grain

f64 correlation(const std::vector<f64>& a, const std::vector<f64>& b) {
    const size_t n = a.size();
    f64 ma = 0.0;
    f64 mb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<f64>(n);
    mb /= static_cast<f64>(n);
    f64 cov = 0.0;
    f64 va = 0.0;
    f64 vb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        cov += (a[i] - ma) * (b[i] - mb);
        va += (a[i] - ma) * (a[i] - ma);
        vb += (b[i] - mb) * (b[i] - mb);
    }
    return cov / std::sqrt(va * vb);
}

void testFilmGrain() {
    const u32 n = 256;
    const size_t count = static_cast<size_t>(n) * n;
    const f64 corrBound = 4.0 / std::sqrt(static_cast<f64>(count));
    const auto frameNoise = [&](u64 seed) {
        std::vector<f64> values(count);
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                values[static_cast<size_t>(y) * n + x] = film_grain_noise(seed, x, y);
            }
        }
        return values;
    };

    // Statistics of one frame and frame-to-frame correlation.
    f64 worstTemporal = 0.0;
    f64 worstMean = 0.0;
    f64 worstVarErr = 0.0;
    std::vector<f64> previous = frameNoise(1u);
    std::vector<f64> temporalSum(count, 0.0);
    const u32 frames = 64;
    for (u32 frame = 1; frame <= frames; ++frame) {
        const std::vector<f64> current = frame == 1u ? previous : frameNoise(frame);
        f64 mean = 0.0;
        f64 sq = 0.0;
        f64 minV = 1.0;
        f64 maxV = -1.0;
        for (size_t i = 0; i < count; ++i) {
            mean += current[i];
            sq += current[i] * current[i];
            temporalSum[i] += current[i];
            minV = std::min(minV, current[i]);
            maxV = std::max(maxV, current[i]);
        }
        mean /= static_cast<f64>(count);
        const f64 variance = sq / static_cast<f64>(count) - mean * mean;
        worstMean = std::max(worstMean, std::fabs(mean));
        worstVarErr = std::max(worstVarErr, std::fabs(variance * 3.0 - 1.0));
        expectTrue(minV >= -1.0 && maxV < 1.0, "grain sample in [-1, 1)");
        if (frame > 1u) {
            worstTemporal = std::max(worstTemporal, std::fabs(correlation(previous, current)));
        }
        previous = current;
    }
    std::printf("film grain: worst |mean| %.4g (bound %.4g), worst |3 var - 1| %.4g, worst |corr(N, N+1)| %.4g "
                "(bound %.4g)\n",
                worstMean, 4.0 * std::sqrt(1.0 / 3.0 / count), worstVarErr, worstTemporal, corrBound);
    expectTrue(worstMean < 4.0 * std::sqrt(1.0 / 3.0 / count), "grain mean ~ 0");
    expectTrue(worstVarErr < 0.03, "grain variance = 1/3 (uniform [-1, 1))");
    expectTrue(worstTemporal < corrBound, "grain frame N vs N+1 decorrelated");

    // No fixed pattern: the temporal average of T frames has variance (1/3)/T per pixel.
    f64 avgSq = 0.0;
    for (const f64 s : temporalSum) {
        const f64 m = s / frames;
        avgSq += m * m;
    }
    const f64 fixedPatternRatio = (avgSq / static_cast<f64>(count)) / (1.0 / 3.0 / frames);
    std::printf("film grain: temporal-average variance / white-noise expectation over %u frames = %.4f\n", frames,
                fixedPatternRatio);
    expectTrue(fixedPatternRatio > 0.9 && fixedPatternRatio < 1.1, "grain has no fixed pattern (averages out as 1/T)");

    // Spatial whiteness: neighbour correlations ~ 0.
    const std::vector<f64> base = frameNoise(12345u);
    const int lags[][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}, {2, 0}, {0, 2}};
    f64 worstSpatial = 0.0;
    for (const auto& lag : lags) {
        std::vector<f64> a;
        std::vector<f64> b;
        for (u32 y = 2; y + 2 < n; ++y) {
            for (u32 x = 2; x + 2 < n; ++x) {
                a.push_back(base[static_cast<size_t>(y) * n + x]);
                b.push_back(base[static_cast<size_t>(static_cast<int>(y) + lag[1]) * n +
                                 static_cast<size_t>(static_cast<int>(x) + lag[0])]);
            }
        }
        worstSpatial = std::max(worstSpatial, std::fabs(correlation(a, b)));
    }
    std::printf("film grain: worst spatial neighbour |corr| = %.4g\n", worstSpatial);
    expectTrue(worstSpatial < corrBound, "grain is spatially white");
    expectTrue(film_grain_noise(99u, 3u, 4u) == film_grain_noise(99u, 3u, 4u), "grain is deterministic per seed");

    // Through the post stack on a static grey frame: grain residual matches intensity^2 / 3,
    // differs per pixel (not a single value per colour) and decorrelates between frames.
    PostStack stack{};
    stack.init({});
    ColorGradeParams grade{};
    grade.output_srgb = false;
    grade.vignette = 0.f;
    grade.film_grain = 0.02f;
    stack.setColorGradeParams(grade);
    AutoExposureParams noAuto{};
    noAuto.enabled = false;
    stack.setAutoExposureParams(noAuto);
    const u32 m = 128;
    std::vector<Vec3> frame = makeFrame(m, m, Vec3{0.18f, 0.18f, 0.18f});
    PostFrameInput input{};
    input.hdr = frame.data();
    input.width = m;
    input.height = m;
    std::vector<Vec3> clean;
    std::vector<Vec3> grainA;
    std::vector<Vec3> grainB;
    input.frame_seed = 0u;
    stack.processFrame(input, clean);
    input.frame_seed = 1000u;
    stack.processFrame(input, grainA);
    input.frame_seed = 1001u;
    stack.processFrame(input, grainB);
    std::vector<f64> ra(clean.size());
    std::vector<f64> rb(clean.size());
    f64 sq = 0.0;
    f64 mean = 0.0;
    for (size_t i = 0; i < clean.size(); ++i) {
        ra[i] = static_cast<f64>(grainA[i].x) - clean[i].x;
        rb[i] = static_cast<f64>(grainB[i].x) - clean[i].x;
        mean += ra[i];
        sq += ra[i] * ra[i];
    }
    mean /= static_cast<f64>(clean.size());
    const f64 variance = sq / static_cast<f64>(clean.size()) - mean * mean;
    const f64 expectedVar = 0.02 * 0.02 / 3.0;
    const f64 stackCorr = correlation(ra, rb);
    std::printf("film grain via stack: residual mean %.3g, variance %.4g (expected %.4g), corr(N, N+1) %.4g\n", mean,
                variance, expectedVar, stackCorr);
    expectTrue(std::fabs(variance / expectedVar - 1.0) < 0.05, "stack grain variance = intensity^2 / 3");
    expectTrue(std::fabs(mean) < 4.0 * std::sqrt(expectedVar / clean.size()), "stack grain mean ~ 0");
    expectTrue(std::fabs(stackCorr) < 4.0 / std::sqrt(static_cast<f64>(clean.size())),
               "stack grain decorrelated between frames on a static frame");
    stack.destroy();
}

// ---------------------------------------------------------------------------------------------
// 6. Post chain order

void testPostChainOrder() {
    const auto& order = post_pass_order();
    const char* expected[] = {"bloom", "dof", "motion_blur", "tonemap", "color_grade", "film_grain"};
    for (u32 i = 0; i < kPostPassCount; ++i) {
        expectTrue(std::string(post_pass_name(order[i])) == expected[i], "canonical post pass order");
    }

    const u32 n = 64;
    PostStack stack{};
    stack.init({});
    ColorGradeParams grade{};
    grade.output_srgb = false;
    grade.vignette = 0.f;
    grade.film_grain = 0.f;
    stack.setColorGradeParams(grade);
    AutoExposureParams noAuto{};
    noAuto.enabled = false;
    stack.setAutoExposureParams(noAuto);
    BloomParams bloom{};
    bloom.intensity = 0.2f;
    stack.setBloomParams(bloom);

    std::vector<Vec3> hdr = makeFrame(n, n);
    hdr[static_cast<size_t>(32) * n + 32] = Vec3{8.f, 8.f, 8.f};
    std::vector<f32> depth(static_cast<size_t>(n) * n, stack.dofParams().focal_distance);
    std::vector<Vec2> velocity(static_cast<size_t>(n) * n, Vec2{});
    PostFrameInput input{};
    input.hdr = hdr.data();
    input.linear_depth_m = depth.data();
    input.velocity_px = velocity.data();
    input.width = n;
    input.height = n;
    input.frame_seed = 7u;
    grade.film_grain = 0.02f;
    stack.setColorGradeParams(grade);
    std::vector<Vec3> out;
    expectTrue(stack.processFrame(input, out), "post stack processes a full frame");
    const PostFrameStats stats = stack.lastFrameStats();
    bool orderOk = stats.executed_count == kPostPassCount;
    for (u32 i = 0; orderOk && i < kPostPassCount; ++i) {
        orderOk = stats.executed[i] == order[i];
    }
    expectTrue(orderOk, "processFrame runs bloom -> dof -> motion blur -> tonemap -> grade -> grain");

    // Bloom runs on HDR data before the tone map: the 8.0 pixel (tonemapped to <= 1) still blooms.
    grade.film_grain = 0.f;
    stack.setColorGradeParams(grade);
    input.frame_seed = 0u;
    stack.processFrame(input, out);
    const f32 halo = out[static_cast<size_t>(32) * n + 36].x;
    BloomParams noBloom = bloom;
    noBloom.intensity = 0.f;
    stack.setBloomParams(noBloom);
    std::vector<Vec3> outNoBloom;
    stack.processFrame(input, outNoBloom);
    std::printf("post order: halo 4 px from an HDR 8.0 pixel = %.5f (no bloom %.5f)\n", halo,
                outNoBloom[static_cast<size_t>(32) * n + 36].x);
    expectTrue(halo > 0.f && outNoBloom[static_cast<size_t>(32) * n + 36].x == 0.f,
               "bloom operates on HDR input before tone mapping");

    // Without depth / velocity the spatial passes are skipped but order is preserved.
    input.linear_depth_m = nullptr;
    input.velocity_px = nullptr;
    stack.processFrame(input, out);
    const PostFrameStats reduced = stack.lastFrameStats();
    expectTrue(reduced.executed_count == 3u && reduced.executed[0] == PostPass::Bloom &&
                   reduced.executed[1] == PostPass::ToneMap && reduced.executed[2] == PostPass::ColorGrade,
               "optional passes skipped without inputs, order preserved");
    stack.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();

    testBloomSoftKnee();
    testBloomBelowThresholdIsExactlyZero();
    testBloomSinglePixel();
    testDofCircleOfConfusion();
    testDofPass();
    testMotionBlurTrail();
    testAcesCalibration();
    testFilmGrain();
    testPostChainOrder();
    std::printf("hardware-only: Bloom + DoF + motion blur + tonemap < 1 ms (GPU timing row)\n");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b5_post_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b5_post_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
