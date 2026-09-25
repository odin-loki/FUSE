#include <fuse/renderer/postprocess/bloom.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

namespace {

using fuse::math::Vec3;

u32 clampIndex(s64 index, u32 size) {
    if (index < 0) {
        return 0u;
    }
    if (index >= static_cast<s64>(size)) {
        return size - 1u;
    }
    return static_cast<u32>(index);
}

// Separable 1D tent downsample along one axis: dst[i] = (src[2i-1] + 3 src[2i] + 3 src[2i+1] + src[2i+2]) / 8.
void downsampleAxis(const std::vector<Vec3>& src, u32 width, u32 height, bool horizontal, std::vector<Vec3>& out,
                    u32& outWidth, u32& outHeight) {
    outWidth = horizontal ? (width + 1u) / 2u : width;
    outHeight = horizontal ? height : (height + 1u) / 2u;
    out.assign(static_cast<size_t>(outWidth) * outHeight, Vec3{});
    constexpr f32 kWeights[4] = {1.f / 8.f, 3.f / 8.f, 3.f / 8.f, 1.f / 8.f};
    for (u32 y = 0; y < outHeight; ++y) {
        for (u32 x = 0; x < outWidth; ++x) {
            Vec3 sum{};
            for (s64 k = 0; k < 4; ++k) {
                if (horizontal) {
                    const u32 sx = clampIndex(2 * static_cast<s64>(x) - 1 + k, width);
                    sum = sum + src[static_cast<size_t>(y) * width + sx] * kWeights[k];
                } else {
                    const u32 sy = clampIndex(2 * static_cast<s64>(y) - 1 + k, height);
                    sum = sum + src[static_cast<size_t>(sy) * width + x] * kWeights[k];
                }
            }
            out[static_cast<size_t>(y) * outWidth + x] = sum;
        }
    }
}

// Separable 1D bilinear 2x upsample: even dst p=2i -> 1/4 src[i-1] + 3/4 src[i]; odd -> 3/4 src[i] + 1/4 src[i+1].
void upsampleAxis(const std::vector<Vec3>& src, u32 width, u32 height, bool horizontal, u32 dstSize,
                  std::vector<Vec3>& out) {
    const u32 outWidth = horizontal ? dstSize : width;
    const u32 outHeight = horizontal ? height : dstSize;
    const u32 srcSize = horizontal ? width : height;
    out.assign(static_cast<size_t>(outWidth) * outHeight, Vec3{});
    for (u32 y = 0; y < outHeight; ++y) {
        for (u32 x = 0; x < outWidth; ++x) {
            const u32 p = horizontal ? x : y;
            const s64 i = static_cast<s64>(p / 2u);
            const s64 other = (p % 2u == 0u) ? i - 1 : i + 1;
            const u32 a = clampIndex(i, srcSize);
            const u32 b = clampIndex(other, srcSize);
            const Vec3& va = horizontal ? src[static_cast<size_t>(y) * width + a] : src[static_cast<size_t>(a) * width + x];
            const Vec3& vb = horizontal ? src[static_cast<size_t>(y) * width + b] : src[static_cast<size_t>(b) * width + x];
            out[static_cast<size_t>(y) * outWidth + x] = va * 0.75f + vb * 0.25f;
        }
    }
}

} // namespace

void Bloom::init() {
    m_ready = true;
}

void Bloom::destroy() {
    m_ready = false;
}

f32 Bloom::luminance(const fuse::math::Vec3& rgb) {
    return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

f32 Bloom::thresholdResponse(f32 lum, const BloomParams& params) {
    const f32 knee = std::max(params.knee, 0.f);
    if (lum <= params.threshold - knee) {
        return 0.f;
    }
    if (knee > 0.f && lum < params.threshold + knee) {
        const f32 soft = lum - params.threshold + knee;
        return (soft * soft) / (4.f * knee);
    }
    return std::max(lum - params.threshold, 0.f);
}

fuse::math::Vec3 Bloom::extractBright(const fuse::math::Vec3& hdr, const BloomParams& params) {
    const f32 lum = luminance(hdr);
    const f32 response = thresholdResponse(lum, params);
    if (response <= 0.f || lum <= 0.f) {
        return {};
    }
    return hdr * (response / lum);
}

fuse::math::Vec3 Bloom::apply(const fuse::math::Vec3& hdr, const BloomParams& params) {
    const fuse::math::Vec3 bright = extractBright(hdr, params);
    return hdr + bright * params.intensity;
}

void Bloom::downsampleBox(const fuse::math::Vec3* src, u32 width, u32 height, fuse::math::Vec3* dst) {
    if (src == nullptr || dst == nullptr || width < 2u || height < 2u) {
        return;
    }

    const u32 dstWidth = width / 2u;
    const u32 dstHeight = height / 2u;
    for (u32 y = 0; y < dstHeight; ++y) {
        for (u32 x = 0; x < dstWidth; ++x) {
            const u32 srcX = x * 2u;
            const u32 srcY = y * 2u;
            const fuse::math::Vec3 a = src[srcY * width + srcX];
            const fuse::math::Vec3 b = src[srcY * width + srcX + 1u];
            const fuse::math::Vec3 c = src[(srcY + 1u) * width + srcX];
            const fuse::math::Vec3 d = src[(srcY + 1u) * width + srcX + 1u];
            dst[y * dstWidth + x] = (a + b + c + d) * 0.25f;
        }
    }
}

void Bloom::upsampleBox(const fuse::math::Vec3* src, u32 width, u32 height, fuse::math::Vec3* dst) {
    if (src == nullptr || dst == nullptr || width == 0u || height == 0u) {
        return;
    }

    const u32 dstWidth = width * 2u;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const fuse::math::Vec3 texel = src[y * width + x];
            dst[(y * 2u) * dstWidth + (x * 2u)] = texel;
            dst[(y * 2u) * dstWidth + (x * 2u + 1u)] = texel;
            dst[(y * 2u + 1u) * dstWidth + (x * 2u)] = texel;
            dst[(y * 2u + 1u) * dstWidth + (x * 2u + 1u)] = texel;
        }
    }
}

u32 bloom_level_count(u32 width, u32 height, const BloomParams& params) {
    const u32 maxLevels = std::max(params.mip_levels, 1u);
    u32 levels = 1u;
    while (levels < maxLevels && (width > 1u || height > 1u)) {
        width = (width + 1u) / 2u;
        height = (height + 1u) / 2u;
        ++levels;
    }
    return levels;
}

void bloom_prefilter(const fuse::math::Vec3* hdr, u32 width, u32 height, const BloomParams& params,
                     std::vector<fuse::math::Vec3>& out) {
    const size_t count = static_cast<size_t>(width) * height;
    out.assign(count, Vec3{});
    if (hdr == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = Bloom::extractBright(hdr[i], params);
    }
}

void bloom_downsample(const std::vector<fuse::math::Vec3>& src, u32 width, u32 height,
                      std::vector<fuse::math::Vec3>& out, u32& out_width, u32& out_height) {
    std::vector<Vec3> horizontal;
    u32 hw = 0;
    u32 hh = 0;
    downsampleAxis(src, width, height, true, horizontal, hw, hh);
    downsampleAxis(horizontal, hw, hh, false, out, out_width, out_height);
}

void bloom_upsample(const std::vector<fuse::math::Vec3>& src, u32 src_width, u32 src_height,
                    u32 dst_width, u32 dst_height, std::vector<fuse::math::Vec3>& out) {
    std::vector<Vec3> horizontal;
    upsampleAxis(src, src_width, src_height, true, dst_width, horizontal);
    upsampleAxis(horizontal, dst_width, src_height, false, dst_height, out);
}

void bloom_image(const fuse::math::Vec3* hdr, u32 width, u32 height, const BloomParams& params,
                 std::vector<fuse::math::Vec3>& out_bloom) {
    out_bloom.clear();
    if (hdr == nullptr || width == 0u || height == 0u) {
        return;
    }

    const u32 levels = bloom_level_count(width, height, params);
    std::vector<std::vector<Vec3>> down(levels);
    std::vector<u32> widths(levels);
    std::vector<u32> heights(levels);
    bloom_prefilter(hdr, width, height, params, down[0]);
    widths[0] = width;
    heights[0] = height;
    for (u32 level = 1; level < levels; ++level) {
        bloom_downsample(down[level - 1u], widths[level - 1u], heights[level - 1u], down[level], widths[level],
                         heights[level]);
    }

    const f32 scatter = std::clamp(params.scatter, 0.f, 1.f);
    std::vector<Vec3> up = down[levels - 1u];
    std::vector<Vec3> expanded;
    for (u32 level = levels - 1u; level > 0u; --level) {
        const u32 dst = level - 1u;
        bloom_upsample(up, widths[level], heights[level], widths[dst], heights[dst], expanded);
        up.resize(expanded.size());
        for (size_t i = 0; i < expanded.size(); ++i) {
            up[i] = down[dst][i] * (1.f - scatter) + expanded[i] * scatter;
        }
    }
    out_bloom = std::move(up);
}

void bloom_composite(const fuse::math::Vec3* hdr, u32 width, u32 height, const BloomParams& params,
                     std::vector<fuse::math::Vec3>& out) {
    std::vector<Vec3> bloom;
    bloom_image(hdr, width, height, params, bloom);
    out.assign(bloom.size(), Vec3{});
    for (size_t i = 0; i < bloom.size(); ++i) {
        out[i] = hdr[i] + bloom[i] * params.intensity;
    }
}

} // namespace fuse::renderer
