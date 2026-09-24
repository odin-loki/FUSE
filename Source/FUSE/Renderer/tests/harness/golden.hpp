// WP-0.7 renderer test harness: golden-image comparison.
//
// A golden gate has two parts, and both must pass:
//   1. a perceptual / statistical metric against a per-scene threshold — FLIP (mean, lower is
//      better) or SSIM (higher is better) when the image-metrics library
//      (fuse/renderer/quality/image_metrics.hpp) is linked, else PSNR (higher is better);
//   2. a pixel budget: at most `maxDifferingPixels` pixels may have any channel differ by more than
//      `pixelTolerance` (0..255). Lavapipe is deterministic, so the default budget is 0 — a single
//      changed pixel fails the gate even when the mean metric barely moves.
//
// Update mode (env FUSE_UPDATE_GOLDENS=1, or GoldenStore::setUpdateMode) writes the actual image
// as the new golden instead of failing, and printSummary() lists every rewritten file.
// On failure the store writes <artifactDir>/<name>.actual.png and <name>.diff.png (golden dimmed to
// grey, differing pixels in red scaled by magnitude, pixels within tolerance in green-ish tint).
#pragma once

#include "image_io.hpp"

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::harness {

enum class GoldenMetric : u8 {
    Psnr = 0, ///< dB, pass when >= threshold (+inf for identical images).
    Ssim = 1, ///< Gaussian 11x11 mean SSIM on luma, pass when >= threshold.
    Flip = 2, ///< mean LDR-FLIP, pass when <= threshold.
};

const char* goldenMetricName(GoldenMetric metric);
/// True when the image-metrics library is linked (SSIM / FLIP available).
bool goldenQualityMetricsAvailable();

struct GoldenSpec {
    /// Preferred metric; falls back to PSNR (with `psnrFallbackDb`) when it is unavailable.
    GoldenMetric metric = GoldenMetric::Flip;
    f64 threshold = 0.01;
    f64 psnrFallbackDb = 40.0;
    u8 pixelTolerance = 2;
    u32 maxDifferingPixels = 0;
};

struct GoldenResult {
    std::string name;
    bool passed = false;
    bool updated = false;        ///< Update mode rewrote the golden.
    bool goldenMissing = false;
    GoldenMetric metricUsed = GoldenMetric::Psnr;
    f64 score = 0.0;             ///< Value of metricUsed.
    f64 threshold = 0.0;         ///< Threshold applied to metricUsed.
    f64 psnr = 0.0;              ///< Always computed (+inf when identical).
    u32 differingPixels = 0;     ///< Pixels with any channel |diff| > pixelTolerance.
    u32 maxChannelDiff = 0;
    std::string goldenPath;
    std::string actualPath;      ///< Written on failure.
    std::string diffPath;        ///< Written on failure.
    std::string message;
};

/// Pure comparison (no I/O). `diff` (optional) receives the visual diff image.
GoldenResult compareImages(const ImageRgba8& actual, const ImageRgba8& golden, const GoldenSpec& spec,
                           ImageRgba8* diff = nullptr);

class GoldenStore {
public:
    /// `goldenDir` holds <name>.png; failure artefacts go to `artifactDir` (created on demand).
    GoldenStore(std::string goldenDir, std::string artifactDir);

    /// Reads FUSE_UPDATE_GOLDENS at construction; this overrides it.
    void setUpdateMode(bool update) { m_update = update; }
    bool updateMode() const { return m_update; }
    const std::string& goldenDir() const { return m_goldenDir; }
    const std::string& artifactDir() const { return m_artifactDir; }

    std::string goldenPath(const std::string& name) const;

    /// Compares `actual` with <goldenDir>/<name>.png. In update mode writes it instead (and
    /// passes). A missing golden fails outside update mode.
    GoldenResult check(const std::string& name, const ImageRgba8& actual, const GoldenSpec& spec);

    const std::vector<GoldenResult>& results() const { return m_results; }
    /// One line per check plus the update-mode summary (files rewritten, bytes, storage hint).
    void printSummary() const;

private:
    std::string m_goldenDir;
    std::string m_artifactDir;
    bool m_update = false;
    std::vector<GoldenResult> m_results;
};

/// FUSE_UPDATE_GOLDENS is set to a non-empty value other than "0".
bool goldenUpdateRequestedByEnv();

} // namespace fuse::renderer::harness
