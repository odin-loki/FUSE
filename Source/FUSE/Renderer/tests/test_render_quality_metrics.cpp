// Gate for the image-quality / temporal metrics library (fuse/renderer/quality/):
//   - Known values: identical images -> PSNR +inf, SSIM 1 (Gaussian 11x11 and 8x8 box), FLIP 0; alternating
//     +/-d noise -> PSNR = 10 log10(1 / d^2) exactly; uniform noise -> analytic E[MSE] = a^2 / 3; constant
//     offset -> the closed-form SSIM luminance term; alternating temporal flicker -> tPSNR = 10 log10(1 / (4 d^2)).
//   - Reference implementations on deterministic 8-bit image pairs (generated identically here and in Python):
//     mean LDR-FLIP from NVIDIA's official `flip_evaluator` 1.7 (github.com/NVlabs/flip, BSD-3), SSIM from
//     scikit-image 0.25 `structural_similarity(gaussian_weights=True, sigma=1.5, use_sample_covariance=False,
//     data_range=1)` on Rec. 601 luma, the 8x8 box SSIM and PSNR from NumPy (float64).
//   - Published FLIP pair (images/reference.png + test.png of the FLIP repository, mean LDR-FLIP 0.159691 with
//     flip_evaluator 1.7): checked when FUSE_FLIP_REFERENCE_DIR points at those images (not vendored: 9.5 MB).
//   - CpuReference == CpuParallel (0/2/4 workers) bit-identical scores and maps; kernel stats names.

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/quality/image_metrics.hpp>
#include <fuse/renderer/quality/quality_kernels.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_QUALITY_TEST_HAS_STB)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#endif

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u8;
using fuse::math::Vec3;
namespace kernel = fuse::kernel;
namespace q = fuse::renderer::quality;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f64 value, f64 expected, f64 tolerance, const char* message) {
    if (!(std::fabs(value - expected) <= tolerance)) {
        std::fprintf(stderr, "FAIL: %s (got %.7f, expected %.7f +/- %g)\n", message, value, expected, tolerance);
        ++g_failures;
    }
}

q::QualityImage view(const std::vector<Vec3>& pixels, u32 w, u32 h) { return {pixels.data(), w, h}; }

// ---------------------------------------------------------------------------------------------------------
// Deterministic 8-bit test pairs (mirrors the Python generator used for the reference values).
// ---------------------------------------------------------------------------------------------------------

constexpr u32 kW = 96;
constexpr u32 kH = 64;

u32 hash(u32 x, u32 y, u32 c, u32 seed) {
    u32 h = (x * 73856093u) ^ (y * 19349663u) ^ ((c + 1u) * 83492791u) ^ (seed * 2654435761u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

using Bytes = std::vector<int>; // kW * kH * 3

Bytes patternBytes() {
    Bytes img(kW * kH * 3u);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            for (u32 c = 0; c < 3u; ++c) {
                int v = static_cast<int>((x * 2u + y * 3u + c * 40u) % 256u);
                if (((x / 8u) + (y / 8u)) % 2u != 0u) {
                    v = 255 - v;
                }
                const int dx = static_cast<int>(x) - 60;
                const int dy = static_cast<int>(y) - 30;
                if (dx * dx + dy * dy < 15 * 15) {
                    v = (v + 97 * static_cast<int>(c + 1u)) % 256;
                }
                img[(y * kW + x) * 3u + c] = v;
            }
        }
    }
    return img;
}

int at(const Bytes& b, int x, int y, u32 c) {
    x = x < 0 ? 0 : (x >= static_cast<int>(kW) ? static_cast<int>(kW) - 1 : x);
    y = y < 0 ? 0 : (y >= static_cast<int>(kH) ? static_cast<int>(kH) - 1 : y);
    return b[(static_cast<u32>(y) * kW + static_cast<u32>(x)) * 3u + c];
}

std::vector<Vec3> toImage(const Bytes& b) {
    std::vector<Vec3> img(kW * kH);
    for (u32 i = 0; i < kW * kH; ++i) {
        img[i] = {static_cast<f32>(b[i * 3u]) / 255.f, static_cast<f32>(b[i * 3u + 1u]) / 255.f,
                  static_cast<f32>(b[i * 3u + 2u]) / 255.f};
    }
    return img;
}

struct Pair {
    const char* name;
    Bytes ref;
    Bytes test;
    f64 flip;
    f64 psnr;
    f64 ssim;
    f64 ssim_box8;
};

std::vector<Pair> referencePairs() {
    const Bytes ref = patternBytes();
    Bytes noise = ref;
    Bytes shift = ref;
    Bytes blur = ref;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            for (u32 c = 0; c < 3u; ++c) {
                const u32 i = (y * kW + x) * 3u + c;
                int v = ref[i] + static_cast<int>(hash(x, y, c, 7u) % 41u) - 20;
                noise[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
                shift[i] = x == 0u ? ref[i] : ref[i - 3u];
                int s = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        s += at(ref, static_cast<int>(x) + dx, static_cast<int>(y) + dy, c);
                    }
                }
                blur[i] = s / 9;
            }
        }
    }
    Bytes constRef(kW * kH * 3u, 128);
    Bytes constTest = constRef;
    for (u32 i = 0; i < kW * kH; ++i) {
        constTest[i * 3u] = 140;
        constTest[i * 3u + 1u] = 120;
    }
    // Reference values: flip_evaluator 1.7 (LDR, default 67.02 ppd), NumPy PSNR, scikit-image SSIM, NumPy 8x8 SSIM.
    return {
        {"noise", ref, noise, 0.1112260, 26.791101, 0.9436566, 0.9590561},
        {"shift", ref, shift, 0.2636731, 13.723533, 0.6519340, 0.6925669},
        {"const", constRef, constTest, 0.2014159, 29.721381, 0.9999622, 0.9999622},
        {"blur", ref, blur, 0.2492682, 17.448147, 0.7815793, 0.8199607},
    };
}

// ---------------------------------------------------------------------------------------------------------

void testKnownValues() {
    q::ImageQualityEvaluator eval{};
    constexpr u32 w = 64;
    constexpr u32 h = 48;
    std::vector<Vec3> a(w * h);
    for (u32 i = 0; i < w * h; ++i) {
        a[i] = {static_cast<f32>(hash(i, 0u, 0u, 1u) % 256u) / 255.f, static_cast<f32>(hash(i, 0u, 1u, 1u) % 256u) / 255.f,
                static_cast<f32>(hash(i, 0u, 2u, 1u) % 256u) / 255.f};
    }
    q::PsnrResult pr{};
    expectTrue(eval.psnr(view(a, w, h), view(a, w, h), pr) && pr.mse == 0.0 && std::isinf(pr.psnr) && pr.psnr > 0.0,
               "identical images: PSNR = +inf");
    expectNear(eval.ssim(view(a, w, h), view(a, w, h)), 1.0, 1e-6, "identical images: SSIM (Gaussian 11x11) = 1");
    expectNear(eval.ssim(view(a, w, h), view(a, w, h), q::SsimWindow::Box8), 1.0, 1e-6, "identical images: SSIM (8x8) = 1");
    expectTrue(eval.flip(view(a, w, h), view(a, w, h)) == 0.0, "identical images: FLIP = 0 exactly");

    // Alternating +/- d around 0.5: MSE = d^2 exactly.
    const f32 d = 0.05f;
    std::vector<Vec3> flat(w * h, Vec3{0.5f, 0.5f, 0.5f});
    std::vector<Vec3> alt(w * h);
    for (u32 i = 0; i < w * h; ++i) {
        const f32 s = ((i + i / w) & 1u) != 0u ? d : -d;
        alt[i] = {0.5f + s, 0.5f + s, 0.5f + s};
    }
    eval.psnr(view(alt, w, h), view(flat, w, h), pr);
    expectNear(pr.psnr, 10.0 * std::log10(1.0 / (static_cast<f64>(d) * d)), 1e-3, "alternating +/-d: PSNR = 10 log10(1/d^2)");

    // Uniform noise in [-a, a]: E[MSE] = a^2 / 3 (256x256 samples, 3 channels).
    constexpr u32 nw = 256;
    constexpr u32 nh = 256;
    const f32 amp = 0.1f;
    std::vector<Vec3> base(nw * nh, Vec3{0.5f, 0.5f, 0.5f});
    std::vector<Vec3> noisy(nw * nh);
    for (u32 i = 0; i < nw * nh; ++i) {
        const auto u = [&](u32 c) { return (static_cast<f32>(hash(i, 3u, c, 11u) & 0xffffu) + 0.5f) / 65536.f * 2.f - 1.f; };
        noisy[i] = {0.5f + amp * u(0u), 0.5f + amp * u(1u), 0.5f + amp * u(2u)};
    }
    eval.psnr(view(noisy, nw, nh), view(base, nw, nh), pr);
    const f64 analytic = 10.0 * std::log10(3.0 / (static_cast<f64>(amp) * amp));
    expectNear(pr.psnr, analytic, 0.05, "uniform noise: PSNR within 0.05 dB of 10 log10(3 / a^2)");

    // Constant images c1, c2: SSIM = (2 c1 c2 + C1) / (c1^2 + c2^2 + C1) (structure term = 1).
    std::vector<Vec3> c1(w * h, Vec3{0.5f, 0.5f, 0.5f});
    std::vector<Vec3> c2(w * h, Vec3{0.6f, 0.6f, 0.6f});
    const f64 C1 = 0.0001;
    const f64 expected = (2.0 * 0.5 * 0.6 + C1) / (0.25 + 0.36 + C1);
    expectNear(eval.ssim(view(c2, w, h), view(c1, w, h)), expected, 2e-5, "constant offset: closed-form SSIM");
    expectNear(eval.ssim(view(c2, w, h), view(c1, w, h), q::SsimWindow::Box8), expected, 2e-5, "constant offset: 8x8 SSIM");

    // Linear inputs are exposed, clamped and sRGB encoded before scoring.
    std::vector<Vec3> linA(w * h);
    std::vector<Vec3> linB(w * h);
    std::vector<Vec3> dispA(w * h);
    std::vector<Vec3> dispB(w * h);
    for (u32 i = 0; i < w * h; ++i) {
        linA[i] = a[i] * 0.5f;
        linB[i] = alt[i] * 0.5f;
        const auto enc = [](const Vec3& v) {
            return Vec3{q::srgb_encode(v.x * 2.f), q::srgb_encode(v.y * 2.f), q::srgb_encode(v.z * 2.f)};
        };
        dispA[i] = enc(linA[i]);
        dispB[i] = enc(linB[i]);
    }
    q::ImageQualityEvaluator linear{{q::QualityEncoding::Linear, 2.f, kernel::Backend::CpuParallel}};
    q::PsnrResult pl{};
    q::PsnrResult pd{};
    linear.psnr(view(linA, w, h), view(linB, w, h), pl);
    eval.psnr(view(dispA, w, h), view(dispB, w, h), pd);
    expectNear(pl.psnr, pd.psnr, 1e-4, "Linear encoding == exposure + sRGB display encoding");
    expectNear(linear.flip(view(linA, w, h), view(linB, w, h)), eval.flip(view(dispA, w, h), view(dispB, w, h)), 1e-6,
               "Linear encoding FLIP == display FLIP");

    // FLIP pixels-per-degree default.
    expectNear(q::flipDefaultPixelsPerDegree(), 67.0206451, 1e-4, "FLIP default ppd = 0.7 * 3840 / 0.7 * pi / 180");
    const q::FlipFilters filters = q::make_flip_filters(q::flipDefaultPixelsPerDegree());
    expectTrue(filters.spatial_radius == 10 && filters.feature_radius == 9, "FLIP filter radii at 67 ppd (10, 9)");
}

void testReferencePairs() {
    q::ImageQualityEvaluator eval{};
    for (const Pair& pair : referencePairs()) {
        const std::vector<Vec3> r = toImage(pair.ref);
        const std::vector<Vec3> t = toImage(pair.test);
        char label[160];
        const f64 flip = eval.flip(view(t, kW, kH), view(r, kW, kH));
        std::printf("  pair %-6s FLIP %.7f (flip_evaluator %.7f)", pair.name, flip, pair.flip);
        std::snprintf(label, sizeof(label), "pair %s: mean LDR-FLIP matches flip_evaluator 1.7", pair.name);
        expectNear(flip, pair.flip, 2e-5, label);
        // FLIP is symmetric.
        std::snprintf(label, sizeof(label), "pair %s: FLIP symmetric", pair.name);
        expectNear(eval.flip(view(r, kW, kH), view(t, kW, kH)), flip, 1e-6, label);

        q::PsnrResult pr{};
        eval.psnr(view(t, kW, kH), view(r, kW, kH), pr);
        std::snprintf(label, sizeof(label), "pair %s: PSNR matches NumPy", pair.name);
        expectNear(pr.psnr, pair.psnr, 1e-3, label);

        const f64 ssim = eval.ssim(view(t, kW, kH), view(r, kW, kH));
        const f64 box = eval.ssim(view(t, kW, kH), view(r, kW, kH), q::SsimWindow::Box8);
        std::printf(" | PSNR %.4f | SSIM %.6f (skimage %.6f) | SSIM8 %.6f\n", pr.psnr, ssim, pair.ssim, box);
        std::snprintf(label, sizeof(label), "pair %s: SSIM (Gaussian) matches scikit-image", pair.name);
        expectNear(ssim, pair.ssim, 1e-4, label);
        std::snprintf(label, sizeof(label), "pair %s: SSIM (8x8 box) matches NumPy", pair.name);
        expectNear(box, pair.ssim_box8, 1e-4, label);
    }
}

void testPublishedFlipPair() {
    const char* dir = std::getenv("FUSE_FLIP_REFERENCE_DIR");
#if defined(FUSE_QUALITY_TEST_HAS_STB)
    if (dir == nullptr || dir[0] == '\0') {
        std::printf("  published FLIP pair: skipped (set FUSE_FLIP_REFERENCE_DIR to github.com/NVlabs/flip/images)\n");
        return;
    }
    const auto load = [&](const char* name, int& w, int& h) {
        const std::string path = std::string(dir) + "/" + name;
        int n = 0;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 3);
        std::vector<Vec3> img;
        if (data != nullptr) {
            img.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            for (std::size_t i = 0; i < img.size(); ++i) {
                img[i] = {data[i * 3] / 255.f, data[i * 3 + 1] / 255.f, data[i * 3 + 2] / 255.f};
            }
            stbi_image_free(data);
        }
        return img;
    };
    int rw = 0, rh = 0, tw = 0, th = 0;
    const std::vector<Vec3> ref = load("reference.png", rw, rh);
    const std::vector<Vec3> test = load("test.png", tw, th);
    expectTrue(!ref.empty() && !test.empty() && rw == tw && rh == th, "published FLIP pair loads");
    if (ref.empty() || test.empty()) {
        return;
    }
    q::ImageQualityEvaluator eval{};
    const f64 flip = eval.flip({test.data(), static_cast<u32>(tw), static_cast<u32>(th)},
                               {ref.data(), static_cast<u32>(rw), static_cast<u32>(rh)});
    std::printf("  published FLIP pair (%dx%d): mean LDR-FLIP %.6f (flip_evaluator 1.7: 0.159691)\n", rw, rh, flip);
    expectNear(flip, 0.1596912, 2e-5, "published FLIP pair matches flip_evaluator");
#else
    (void)dir;
    std::printf("  published FLIP pair: skipped (built without stb_image)\n");
#endif
}

void testTemporalAndMasked() {
    q::ImageQualityEvaluator eval{};
    constexpr u32 w = 48;
    constexpr u32 h = 32;
    std::vector<Vec3> r0(w * h);
    std::vector<Vec3> r1(w * h);
    for (u32 i = 0; i < w * h; ++i) {
        const f32 v = 0.9f * static_cast<f32>(i % w) / static_cast<f32>(w); // t0 = r0 + d stays unclamped
        r0[i] = {v, 0.4f, 0.6f};
        r1[i] = {0.9f * v + 0.05f, 0.4f, 0.6f}; // the reference itself changes
    }
    q::TemporalResult tr{};
    expectTrue(eval.temporal(view(r0, w, h), view(r1, w, h), view(r0, w, h), view(r1, w, h), true, tr) &&
                   std::isinf(tr.tpsnr) && tr.flip_excess == 0.0,
               "test == reference sequence: tPSNR = +inf, temporal FLIP excess = 0");
    // Test flickers by +/- d on top of the reference: temporal change error = 2 d everywhere.
    const f32 d = 0.04f;
    std::vector<Vec3> t0(w * h);
    std::vector<Vec3> t1(w * h);
    for (u32 i = 0; i < w * h; ++i) {
        t0[i] = r0[i] + Vec3{d, d, d};
        t1[i] = r1[i] - Vec3{d, d, d};
    }
    eval.temporal(view(t0, w, h), view(t1, w, h), view(r0, w, h), view(r1, w, h), true, tr);
    expectNear(tr.tpsnr, 10.0 * std::log10(1.0 / (4.0 * d * d)), 1e-3, "alternating flicker: tPSNR = 10 log10(1/(4 d^2))");
    expectTrue(tr.flip_excess > 0.01 && tr.test_change_flip > tr.ref_change_flip, "flicker raises temporal FLIP excess");

    // Masked (ghosting / disocclusion) error: +delta inside the mask only.
    const f32 delta = 0.2f;
    std::vector<u8> mask(w * h, 0u);
    std::vector<Vec3> ghost = r0;
    u32 inside = 0;
    for (u32 y = 8; y < 16; ++y) {
        for (u32 x = 10; x < 30; ++x) {
            mask[y * w + x] = 1u;
            ghost[y * w + x] = Vec3{0.3f + delta, 0.3f + delta, 0.3f + delta};
            r0[y * w + x] = Vec3{0.3f, 0.3f, 0.3f};
            ++inside;
        }
    }
    q::MaskedErrorResult mr{};
    expectTrue(eval.maskedError(view(ghost, w, h), view(r0, w, h), mask.data(), mr) && mr.pixels == inside,
               "masked error counts mask pixels");
    expectNear(mr.mean_abs_luma, delta, 1e-5, "masked error: mean |luma| = delta");
    expectNear(mr.rms, delta, 1e-5, "masked error: RMS = delta");
    expectNear(mr.fraction_above, 1.0, 0.0, "masked error: all pixels above 0.1");

    // Trail mask: pixels the object covered before but not now.
    std::vector<u32> idsPrev(w * h, 0u);
    std::vector<u32> idsCur(w * h, 0u);
    idsPrev[5] = 7u;
    idsPrev[6] = 7u;
    idsCur[6] = 7u;
    const u32* history[1] = {idsPrev.data()};
    std::vector<u8> trail(w * h, 0u);
    q::buildTrailMask(history, 1u, idsCur.data(), 7u, w, h, trail.data());
    u32 trailCount = 0;
    for (u8 v : trail) {
        trailCount += v;
    }
    expectTrue(trail[5] == 1u && trail[6] == 0u && trailCount == 1u, "trail mask = covered before, not now");
    std::vector<u8> dilated(w * h, 0u);
    q::dilateMask(trail.data(), w, h, 1u, dilated.data());
    u32 dilatedCount = 0;
    for (u8 v : dilated) {
        dilatedCount += v;
    }
    expectTrue(dilatedCount == 6u, "dilateMask radius 1 at the top edge covers 3x2 pixels");
    std::vector<Vec3> bad(w * h, Vec3{0.f, 0.f, 0.f});
    expectTrue(!q::imageHasNonFinite(bad.data(), bad.size()), "finite image");
    bad[3].y = std::nanf("");
    expectTrue(q::imageHasNonFinite(bad.data(), bad.size()), "NaN detected");
}

void testParityAndStats() {
    const std::vector<Pair> pairs = referencePairs();
    const std::vector<Vec3> r = toImage(pairs[3].ref);
    const std::vector<Vec3> t = toImage(pairs[3].test);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    kernel::reset_kernel_stats();
    q::ImageQualityEvaluator refEval{{q::QualityEncoding::Display, 1.f, kernel::Backend::CpuReference}};
    q::PsnrResult rp{};
    refEval.psnr(view(t, kW, kH), view(r, kW, kH), rp);
    const f64 rs = refEval.ssim(view(t, kW, kH), view(r, kW, kH));
    const f64 rf = refEval.flip(view(t, kW, kH), view(r, kW, kH));
    const std::vector<f32> refMap = refEval.lastFlipMap();

    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(q::kFlipFilterYName, stats) && stats.launches == 1u && stats.items == kW * kH &&
                   stats.last_backend == kernel::Backend::CpuReference,
               "flip_filter_y stats recorded (items = pixels, CpuReference)");
    expectTrue(kernel::find_kernel_stats(q::kSsimName, stats) && stats.items == (kW - 10u) * (kH - 10u),
               "quality_ssim stats: one item per valid 11x11 window");
    expectTrue(kernel::find_kernel_stats(q::kPrepareName, stats) && stats.launches == 2u, "quality_prepare launched");
    expectTrue(kernel::find_kernel_stats(q::kFlipConvertName, stats) && kernel::find_kernel_stats(q::kFlipFilterXName, stats),
               "flip_convert / flip_filter_x recorded");

    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        q::ImageQualityEvaluator par{{q::QualityEncoding::Display, 1.f, kernel::Backend::CpuParallel}};
        q::PsnrResult pp{};
        par.psnr(view(t, kW, kH), view(r, kW, kH), pp);
        const f64 ps = par.ssim(view(t, kW, kH), view(r, kW, kH));
        const f64 pf = par.flip(view(t, kW, kH), view(r, kW, kH));
        const bool mapSame = std::memcmp(refMap.data(), par.lastFlipMap().data(), refMap.size() * sizeof(f32)) == 0;
        char label[128];
        std::snprintf(label, sizeof(label), "CpuReference == CpuParallel bit-exact PSNR/SSIM/FLIP + map (%u workers)", workers);
        expectTrue(pp.mse == rp.mse && ps == rs && pf == rf && mapSame, label);
    }
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        q::ImageQualityEvaluator gpu{{q::QualityEncoding::Display, 1.f, kernel::Backend::Cuda}};
        const f64 gf = gpu.flip(view(t, kW, kH), view(r, kW, kH));
        const kernel::LaunchRecord last = kernel::last_launch();
        expectTrue(gf == rf && last.ok && last.requested == kernel::Backend::Cuda && last.backend == kernel::Backend::CpuParallel,
                   "Cuda request without a device falls back to CpuParallel with the same FLIP");
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testKnownValues();
    testReferencePairs();
    testPublishedFlipPair();
    testTemporalAndMasked();
    testParityAndStats();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d quality metric check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Render quality metric gates passed\n");
    return EXIT_SUCCESS;
}
