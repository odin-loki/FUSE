#include "golden.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if defined(FUSE_RP_HARNESS_QUALITY_METRICS)
#include <fuse/renderer/quality/image_metrics.hpp>
#endif

namespace fuse::renderer::harness {

namespace {

f64 psnrOf(const ImageRgba8& a, const ImageRgba8& b) {
    f64 sum = 0.0;
    const usize n = static_cast<usize>(a.width) * a.height;
    for (usize i = 0; i < n; ++i) {
        for (usize c = 0; c < 3u; ++c) {
            const f64 d = (static_cast<f64>(a.pixels[i * 4u + c]) - b.pixels[i * 4u + c]) / 255.0;
            sum += d * d;
        }
    }
    const f64 mse = sum / static_cast<f64>(n * 3u);
    return mse == 0.0 ? std::numeric_limits<f64>::infinity() : 10.0 * std::log10(1.0 / mse);
}

#if defined(FUSE_RP_HARNESS_QUALITY_METRICS)
std::vector<math::Vec3> toVec3(const ImageRgba8& image) {
    std::vector<math::Vec3> out(static_cast<usize>(image.width) * image.height);
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = math::Vec3(image.pixels[i * 4u] / 255.f, image.pixels[i * 4u + 1u] / 255.f,
                            image.pixels[i * 4u + 2u] / 255.f);
    }
    return out;
}

/// SSIM / FLIP through the image-metrics library; NaN when it cannot evaluate.
f64 qualityMetric(GoldenMetric metric, const ImageRgba8& actual, const ImageRgba8& golden) {
    const std::vector<math::Vec3> a = toVec3(actual);
    const std::vector<math::Vec3> g = toVec3(golden);
    quality::QualityOptions options{};
    options.encoding = quality::QualityEncoding::Display;
    quality::ImageQualityEvaluator evaluator(options);
    const quality::QualityImage test{a.data(), actual.width, actual.height};
    const quality::QualityImage ref{g.data(), golden.width, golden.height};
    return metric == GoldenMetric::Ssim ? evaluator.ssim(test, ref) : evaluator.flip(test, ref);
}
#endif

} // namespace

const char* goldenMetricName(GoldenMetric metric) {
    switch (metric) {
    case GoldenMetric::Psnr: return "PSNR";
    case GoldenMetric::Ssim: return "SSIM";
    case GoldenMetric::Flip: return "FLIP";
    }
    return "?";
}

bool goldenQualityMetricsAvailable() {
#if defined(FUSE_RP_HARNESS_QUALITY_METRICS)
    return true;
#else
    return false;
#endif
}

bool goldenUpdateRequestedByEnv() {
    const char* v = std::getenv("FUSE_UPDATE_GOLDENS");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

GoldenResult compareImages(const ImageRgba8& actual, const ImageRgba8& golden, const GoldenSpec& spec,
                           ImageRgba8* diff) {
    GoldenResult r;
    if (!actual.valid() || !golden.valid() || actual.width != golden.width || actual.height != golden.height) {
        r.message = "size mismatch: actual " + std::to_string(actual.width) + "x" + std::to_string(actual.height) +
                    ", golden " + std::to_string(golden.width) + "x" + std::to_string(golden.height);
        r.differingPixels = std::max(actual.width * actual.height, golden.width * golden.height);
        return r;
    }
    if (diff != nullptr) {
        *diff = ImageRgba8(actual.width, actual.height);
    }
    const usize n = static_cast<usize>(actual.width) * actual.height;
    for (usize i = 0; i < n; ++i) {
        u32 worst = 0;
        for (usize c = 0; c < 4u; ++c) {
            const int d = std::abs(static_cast<int>(actual.pixels[i * 4u + c]) - golden.pixels[i * 4u + c]);
            worst = std::max(worst, static_cast<u32>(d));
        }
        r.maxChannelDiff = std::max(r.maxChannelDiff, worst);
        const bool differs = worst > spec.pixelTolerance;
        if (differs) {
            ++r.differingPixels;
        }
        if (diff != nullptr) {
            const u8* g = golden.pixels.data() + i * 4u;
            const u8 grey = static_cast<u8>((static_cast<u32>(g[0]) * 77u + g[1] * 150u + g[2] * 29u) >> 10); // /4 dim
            u8* d = diff->pixels.data() + i * 4u;
            if (differs) {
                d[0] = static_cast<u8>(std::min(255u, 128u + worst));
                d[1] = 0;
                d[2] = 0;
            } else if (worst > 0u) {
                d[0] = grey;
                d[1] = static_cast<u8>(std::min(255u, grey + 64u));
                d[2] = grey;
            } else {
                d[0] = d[1] = d[2] = grey;
            }
            d[3] = 255;
        }
    }
    r.psnr = psnrOf(actual, golden);

    GoldenMetric metric = spec.metric;
    f64 score = std::numeric_limits<f64>::quiet_NaN();
#if defined(FUSE_RP_HARNESS_QUALITY_METRICS)
    if (metric != GoldenMetric::Psnr) {
        score = qualityMetric(metric, actual, golden);
    }
#endif
    if (metric == GoldenMetric::Psnr || std::isnan(score)) {
        metric = GoldenMetric::Psnr;
        score = r.psnr;
        r.threshold = spec.metric == GoldenMetric::Psnr ? spec.threshold : spec.psnrFallbackDb;
    } else {
        r.threshold = spec.threshold;
    }
    r.metricUsed = metric;
    r.score = score;
    const bool metricOk = metric == GoldenMetric::Flip ? score <= r.threshold : score >= r.threshold;
    const bool pixelsOk = r.differingPixels <= spec.maxDifferingPixels;
    r.passed = metricOk && pixelsOk;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s %.4f (%s %.4f), PSNR %.2f dB, %u px > tol %u (budget %u), max diff %u",
                  goldenMetricName(metric), score, metric == GoldenMetric::Flip ? "<=" : ">=", r.threshold, r.psnr,
                  r.differingPixels, static_cast<u32>(spec.pixelTolerance), spec.maxDifferingPixels, r.maxChannelDiff);
    r.message = buffer;
    if (!metricOk) {
        r.message += " [metric over threshold]";
    }
    if (!pixelsOk) {
        r.message += " [pixel budget exceeded]";
    }
    return r;
}

GoldenStore::GoldenStore(std::string goldenDir, std::string artifactDir)
    : m_goldenDir(std::move(goldenDir)), m_artifactDir(std::move(artifactDir)), m_update(goldenUpdateRequestedByEnv()) {}

std::string GoldenStore::goldenPath(const std::string& name) const {
    return m_goldenDir + "/" + name + ".png";
}

GoldenResult GoldenStore::check(const std::string& name, const ImageRgba8& actual, const GoldenSpec& spec) {
    GoldenResult r;
    const std::string path = goldenPath(name);
    ImageRgba8 golden;
    std::string error;
    const bool haveGolden = readPng(path, golden, &error);

    if (m_update) {
        if (haveGolden) {
            r = compareImages(actual, golden, spec);
        } else {
            r.goldenMissing = true;
            r.message = "golden was missing";
        }
        const bool unchanged = haveGolden && r.differingPixels == 0u && r.maxChannelDiff == 0u;
        r.passed = true;
        if (!unchanged) {
            if (!ensureDirectory(m_goldenDir) || !writePng(path, actual)) {
                r.passed = false;
                r.message = "cannot write golden " + path;
            } else {
                r.updated = true;
            }
        }
    } else if (!haveGolden) {
        r.goldenMissing = true;
        r.message = "golden missing (" + error + "); run with FUSE_UPDATE_GOLDENS=1 to create it";
    } else {
        ImageRgba8 diff;
        r = compareImages(actual, golden, spec, &diff);
        if (!r.passed && ensureDirectory(m_artifactDir)) {
            r.actualPath = m_artifactDir + "/" + name + ".actual.png";
            r.diffPath = m_artifactDir + "/" + name + ".diff.png";
            if (!writePng(r.actualPath, actual)) {
                r.actualPath.clear();
            }
            if (!diff.valid() || !writePng(r.diffPath, diff)) {
                r.diffPath.clear();
            }
        }
    }
    r.name = name;
    r.goldenPath = path;
    m_results.push_back(r);
    return r;
}

void GoldenStore::printSummary() const {
    u32 failed = 0;
    u32 updated = 0;
    for (const GoldenResult& r : m_results) {
        const char* status = r.updated ? "UPDATED" : r.passed ? "PASS" : "FAIL";
        std::printf("golden %-8s %-24s %s\n", status, r.name.c_str(), r.message.c_str());
        if (!r.passed) {
            ++failed;
            if (!r.actualPath.empty()) {
                std::printf("               actual: %s\n", r.actualPath.c_str());
            }
            if (!r.diffPath.empty()) {
                std::printf("               diff:   %s\n", r.diffPath.c_str());
            }
        }
        if (r.updated) {
            ++updated;
        }
    }
    if (m_update) {
        std::printf("FUSE_UPDATE_GOLDENS: %u of %zu golden(s) rewritten in %s\n", updated, m_results.size(),
                    m_goldenDir.c_str());
        for (const GoldenResult& r : m_results) {
            if (!r.updated) {
                continue;
            }
            std::vector<u8> bytes;
            readFileBytes(r.goldenPath, bytes);
            std::printf("  %s (%zu bytes)%s\n", r.goldenPath.c_str(), bytes.size(),
                        bytes.size() > 64u * 1024u ? "  WARNING: over the 64 KiB golden budget" : "");
        }
        if (updated > 0u) {
            std::printf("  review the new images, then commit them (Tests/golden/renderer/README.md)\n");
        }
    } else if (failed > 0u) {
        std::printf("%u golden check(s) failed; FUSE_UPDATE_GOLDENS=1 regenerates after an intended change\n", failed);
    }
}

} // namespace fuse::renderer::harness
