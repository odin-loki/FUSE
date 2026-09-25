// FUSE asset plan W0.8: CPU gates for the golden-render metric (runs in every tree, stub included).
//
//   fuse_content_golden_flip [--golden-dir D]
//
//   1. FLIP of an image against itself is exactly 0 (no pixel > 0), and repeated evaluation is
//      bit-identical;
//   2. FLIP sanity: known perturbations of a synthetic test card land in calibrated ranges —
//      one changed pixel (> 0, tiny mean), a +8-code brightness shift (small), noise of growing
//      amplitude (strictly increasing), black vs white (large);
//   3. cross-check with FUSE's own LDR-FLIP (renderer quality/image_metrics) when the harness links it:
//      the two implementations agree on the perturbations within 1e-4 mean FLIP;
//   4. the committed goldens (Samples/content_golden/golden/<scene>_<setup>.png) exist, decode and are
//      960 x 540 opaque images;
//   5. the magma heat map of a zero map is uniform and of a non-zero map is not.
#include "flip_metric.hpp"
#include "golden.hpp"
#include "image_io.hpp"
#include "reference_scenes.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef FUSE_CONTENT_GOLDEN_DIR
#define FUSE_CONTENT_GOLDEN_DIR "Samples/content_golden/golden"
#endif

namespace {

using namespace fuse::content_golden;
using fuse::renderer::harness::ImageRgba8;
using fuse::u32;
using fuse::u8;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    std::printf("%s: %s\n", condition ? "ok  " : "FAIL", message.c_str());
    if (!condition) {
        ++g_failures;
    }
}

u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

u8 clampByte(int v) { return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/// Deterministic test card: vertical gradient, eight coloured discs, a fine checker strip.
ImageRgba8 testCard(u32 w, u32 h) {
    ImageRgba8 img(w, h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = img.at(x, y);
            int r = static_cast<int>(40 + 120 * y / h), g = static_cast<int>(60 + 100 * y / h), b = 150;
            for (u32 d = 0; d < 8u; ++d) {
                const int cx = static_cast<int>(w / 16u + d * w / 8u), cy = static_cast<int>(h / 2u);
                const int dx = static_cast<int>(x) - cx, dy = static_cast<int>(y) - cy;
                if (dx * dx + dy * dy < static_cast<int>((w / 20u) * (w / 20u))) {
                    r = static_cast<int>(hash32(d * 3u + 0u) & 255u);
                    g = static_cast<int>(hash32(d * 3u + 1u) & 255u);
                    b = static_cast<int>(hash32(d * 3u + 2u) & 255u);
                }
            }
            if (y < h / 8u) {
                const int v = ((x / 2u) + (y / 2u)) % 2u ? 230 : 25;
                r = g = b = v;
            }
            p[0] = clampByte(r);
            p[1] = clampByte(g);
            p[2] = clampByte(b);
            p[3] = 255u;
        }
    }
    return img;
}

ImageRgba8 addNoise(const ImageRgba8& src, int amplitude, u32 seed) {
    ImageRgba8 out = src;
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        if (i % 4u == 3u) {
            continue;
        }
        const int n = static_cast<int>(hash32(static_cast<u32>(i) * 2654435761u + seed) % static_cast<u32>(2 * amplitude + 1)) - amplitude;
        out.pixels[i] = clampByte(out.pixels[i] + n);
    }
    return out;
}

double flipMean(const ImageRgba8& ref, const ImageRgba8& test) {
    return computeFlip(ref.pixels.data(), test.pixels.data(), ref.width, ref.height).mean;
}

void checkInRange(const char* what, double v, double lo, double hi) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: mean FLIP %.6f in [%.6f, %.6f]", what, v, lo, hi);
    expect(v >= lo && v <= hi, buf);
}

} // namespace

int main(int argc, char** argv) {
    std::string goldenDir = FUSE_CONTENT_GOLDEN_DIR;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--golden-dir") == 0) {
            goldenDir = argv[++i];
        }
    }
    const u32 w = 320u, h = 180u;
    const ImageRgba8 card = testCard(w, h);
    std::printf("FLIP pixels per degree: %.4f\n", static_cast<double>(flipDefaultPixelsPerDegree()));

    // 1. identity + repeatability.
    const FlipResult same = computeFlip(card.pixels.data(), card.pixels.data(), w, h);
    expect(same.valid && same.mean == 0.0 && same.nonZero == 0u && same.max == 0.f, "identical images: FLIP == 0 exactly");
    ImageRgba8 onePixel = card;
    onePixel.at(w / 2u, h / 3u)[0] = static_cast<u8>(255u - onePixel.at(w / 2u, h / 3u)[0]);
    const FlipResult a = computeFlip(card.pixels.data(), onePixel.pixels.data(), w, h);
    const FlipResult b = computeFlip(card.pixels.data(), onePixel.pixels.data(), w, h);
    expect(a.map == b.map && a.mean == b.mean, "repeated evaluation is bit-identical");

    // 2. sanity ranges (calibrated on this card; FLIP 1.7, default PPD).
    checkInRange("one inverted pixel", a.mean, 1e-7, 1e-3);
    expect(a.max > 0.1f, "one inverted pixel: per-pixel FLIP peak > 0.1");
    ImageRgba8 brighter = card;
    for (std::size_t i = 0; i < brighter.pixels.size(); ++i) {
        if (i % 4u != 3u) {
            brighter.pixels[i] = clampByte(brighter.pixels[i] + 8);
        }
    }
    checkInRange("+8 code brightness shift", flipMean(card, brighter), 0.05, 0.3);
    const double n4 = flipMean(card, addNoise(card, 4, 1u));
    const double n16 = flipMean(card, addNoise(card, 16, 2u));
    const double n64 = flipMean(card, addNoise(card, 64, 3u));
    checkInRange("noise +-4", n4, 0.001, 0.06);
    checkInRange("noise +-64", n64, 0.1, 0.9);
    expect(n4 < n16 && n16 < n64, "FLIP increases with noise amplitude");
    ImageRgba8 black(w, h), white(w, h);
    for (std::size_t i = 0; i < black.pixels.size(); ++i) {
        black.pixels[i] = (i % 4u == 3u) ? 255u : 0u;
        white.pixels[i] = 255u;
    }
    checkInRange("black vs white", flipMean(black, white), 0.7, 1.0);
    expect(!computeFlip(nullptr, card.pixels.data(), w, h).valid, "null input rejected");

    // 3. cross-check against FUSE's own LDR-FLIP implementation.
    if (fuse::renderer::harness::goldenQualityMetricsAvailable()) {
        fuse::renderer::harness::GoldenSpec spec;
        spec.metric = fuse::renderer::harness::GoldenMetric::Flip;
        spec.threshold = 1.0;
        spec.pixelTolerance = 255u;
        spec.maxDifferingPixels = w * h;
        const ImageRgba8 perturbed[] = {brighter, addNoise(card, 16, 2u), onePixel};
        for (const ImageRgba8& p : perturbed) {
            const auto r = fuse::renderer::harness::compareImages(p, card, spec);
            const double vendored = flipMean(card, p);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "vendored FLIP %.6f vs FUSE image_metrics FLIP %.6f (|d| <= 1e-4)", vendored, r.score);
            expect(r.metricUsed == fuse::renderer::harness::GoldenMetric::Flip && std::fabs(vendored - r.score) <= 1e-4, buf);
        }
    } else {
        std::printf("NOTE: renderer image-metrics library not linked; FUSE FLIP cross-check skipped\n");
    }

    // 4. committed goldens.
    for (const std::string& scene : referenceSceneNames()) {
        for (const LightingSetup& setup : lightingSetups()) {
            const std::string path = goldenDir + "/" + scene + "_" + setup.name + ".png";
            ImageRgba8 img;
            std::string err;
            const bool ok = fuse::renderer::harness::readPng(path, img, &err);
            bool opaque = ok;
            for (std::size_t i = 3; ok && i < img.pixels.size(); i += 4u) {
                opaque = opaque && img.pixels[i] == 255u;
            }
            expect(ok && img.width == kGoldenWidth && img.height == kGoldenHeight && opaque,
                   "golden " + path + " decodes as an opaque 960x540 image" + (ok ? "" : " (" + err + ")"));
        }
    }

    // 5. heat map.
    const std::vector<u8> zero = flipHeatmap(same.map, w, h);
    bool uniform = true;
    for (std::size_t i = 4; i < zero.size(); ++i) {
        uniform = uniform && zero[i] == zero[i % 4u];
    }
    const std::vector<u8> hot = flipHeatmap(a.map, w, h);
    expect(uniform && hot != zero && hot.size() == static_cast<std::size_t>(w) * h * 4u, "magma heat map");

    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
