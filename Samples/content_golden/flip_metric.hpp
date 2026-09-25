// FUSE asset plan W0.8 (docs/plans/FUSE_ASSET_PLAN.md §5.4): LDR-FLIP through the vendored NVIDIA FLIP
// reference implementation (Engine/lib/nvidia-flip/FLIP.h, BSD-3-Clause, pinned in its VERSION).
//
// Inputs are tightly packed sRGB-encoded RGBA8 images (alpha ignored), row 0 at the top, as the golden
// PNGs store them; they are decoded to linear RGB before FLIP (what FLIP's own tool does for LDR files).
// FLIP.h is included by flip_metric.cpp only, so no other translation unit sees its globals.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::content_golden {

/// FLIP's default viewing setup: 0.7 m from a 0.7 m wide, 3840 px monitor (67.02 pixels per degree).
float flipDefaultPixelsPerDegree();

struct FlipResult {
    bool valid = false;         ///< false for a size mismatch / empty image
    double mean = 0.0;          ///< mean LDR-FLIP over pixels, 0 for identical images (summed in double)
    float max = 0.f;            ///< largest per-pixel value
    std::uint32_t nonZero = 0;  ///< pixels with FLIP > 0
    std::vector<float> map;     ///< per-pixel FLIP in [0, 1], width * height
    std::string error;
};

/// LDR-FLIP of `test` against `reference` (both width * height * 4 bytes, sRGB).
FlipResult computeFlip(const std::uint8_t* reference, const std::uint8_t* test, std::uint32_t width,
                       std::uint32_t height, float pixelsPerDegree = flipDefaultPixelsPerDegree());

/// Magma heat map of a FLIP map (FLIP's own colour map, sRGB RGBA8, alpha 255) for failure artefacts.
std::vector<std::uint8_t> flipHeatmap(const std::vector<float>& map, std::uint32_t width, std::uint32_t height);

} // namespace fuse::content_golden
