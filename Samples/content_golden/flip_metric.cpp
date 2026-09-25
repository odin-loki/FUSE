// FUSE asset plan W0.8: LDR-FLIP wrapper over the vendored NVIDIA FLIP header (see flip_metric.hpp).
#include "flip_metric.hpp"

#include <FLIP.h>

#include <algorithm>
#include <cmath>

namespace fuse::content_golden {

float flipDefaultPixelsPerDegree() { return FLIP::calculatePPD(0.7f, 3840.0f, 0.7f); }

FlipResult computeFlip(const std::uint8_t* reference, const std::uint8_t* test, std::uint32_t width,
                       std::uint32_t height, float pixelsPerDegree) {
    FlipResult out;
    if (reference == nullptr || test == nullptr || width == 0u || height == 0u) {
        out.error = "empty image";
        return out;
    }
    // sRGB8 -> linear through FLIP's own transfer function (256-entry table: bit-identical per code).
    float lut[256];
    for (int i = 0; i < 256; ++i) {
        lut[i] = FLIP::color3::sRGBToLinearRGB(static_cast<float>(i) / 255.0f);
    }
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<float> ref(n * 3u), tst(n * 3u);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < 3u; ++c) {
            ref[i * 3u + c] = lut[reference[i * 4u + c]];
            tst[i * 3u + c] = lut[test[i * 4u + c]];
        }
    }
    FLIP::image<FLIP::color3> referenceImage;
    FLIP::image<FLIP::color3> testImage;
    referenceImage.setPixels(ref.data(), static_cast<int>(width), static_cast<int>(height));
    testImage.setPixels(tst.data(), static_cast<int>(width), static_cast<int>(height));
    FLIP::image<float> errorMap(static_cast<int>(width), static_cast<int>(height), 0.0f);
    FLIP::Parameters parameters;
    parameters.PPD = pixelsPerDegree;
    FLIP::evaluate(referenceImage, testImage, /*useHDR*/ false, parameters, errorMap);

    out.map.assign(errorMap.getHostData(), errorMap.getHostData() + n);
    double sum = 0.0;
    for (float v : out.map) {
        sum += static_cast<double>(v);
        out.max = std::max(out.max, v);
        out.nonZero += v > 0.f ? 1u : 0u;
    }
    out.mean = sum / static_cast<double>(n);
    out.valid = std::isfinite(out.mean);
    if (!out.valid) {
        out.error = "non-finite FLIP";
    }
    return out;
}

std::vector<std::uint8_t> flipHeatmap(const std::vector<float>& map, std::uint32_t width, std::uint32_t height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> rgba(n * 4u, 255u);
    if (map.size() != n || n == 0u) {
        return rgba;
    }
    FLIP::image<float> src;
    src.setPixels(map.data(), static_cast<int>(width), static_cast<int>(height));
    FLIP::image<FLIP::color3> heat(static_cast<int>(width), static_cast<int>(height));
    heat.colorMap(src, FLIP::magmaMap);
    const FLIP::color3* px = heat.getHostData();
    for (std::size_t i = 0; i < n; ++i) {
        const float c[3] = {px[i].x, px[i].y, px[i].z};
        for (std::size_t k = 0; k < 3u; ++k) {
            rgba[i * 4u + k] = static_cast<std::uint8_t>(std::clamp(c[k], 0.f, 1.f) * 255.f + 0.5f);
        }
    }
    return rgba;
}

} // namespace fuse::content_golden
