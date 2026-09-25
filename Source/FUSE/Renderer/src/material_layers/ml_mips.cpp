// Asset plan W0.7 x WP-1.5: see include/fuse/renderer/material_layers/ml_mips.hpp.
#include <fuse/renderer/material_layers/ml_mips.hpp>

#include <fuse/renderer/material_layers/ml_reference.hpp>

#include <cmath>

namespace fuse::renderer::material_layers {

namespace {

u32 channel(u32 texel, u32 c) { return (texel >> (8u * c)) & 255u; }

/// Linear [0, 1] -> sRGB 8-bit (IEC 61966-2-1, f64, round to nearest).
u32 encodeSrgb8(f64 linear) {
    const f64 v = linear <= 0.0 ? 0.0 : (linear >= 1.0 ? 1.0 : linear);
    const f64 s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    const f64 q = std::floor(s * 255.0 + 0.5);
    return q <= 0.0 ? 0u : (q >= 255.0 ? 255u : static_cast<u32>(q));
}

void downsample(const u32* src, u32 w, u32 h, u32* dst, u32 dw, u32 dh, bool srgb, const f32* lut) {
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = x * 2u < w ? x * 2u : w - 1u;
            const u32 x1 = x * 2u + 1u < w ? x * 2u + 1u : w - 1u;
            const u32 y0 = y * 2u < h ? y * 2u : h - 1u;
            const u32 y1 = y * 2u + 1u < h ? y * 2u + 1u : h - 1u;
            const u32 t[4] = {src[y0 * w + x0], src[y0 * w + x1], src[y1 * w + x0], src[y1 * w + x1]};
            u32 out = 0u;
            for (u32 c = 0; c < 4u; ++c) {
                u32 q = 0u;
                if (srgb && c < 3u) {
                    f64 sum = 0.0;
                    for (const u32 v : t) {
                        sum += static_cast<f64>(lut[channel(v, c)]);
                    }
                    q = encodeSrgb8(sum * 0.25);
                } else {
                    u32 sum = 0u;
                    for (const u32 v : t) {
                        sum += channel(v, c);
                    }
                    q = (sum + 2u) >> 2u;
                }
                out |= q << (8u * c);
            }
            dst[y * dw + x] = out;
        }
    }
}

/// Bilinear sample of one level: the pool filter (ml_bilinear) on that level's texels.
MlF4 levelBilinear(const MlMipChains& c, const MlMipLevel& level, u32 flags, MlF2 uv) {
    MlView v{};
    v.texels = c.texels.data();
    v.lut = c.lut;
    MlTexture t{};
    t.offset = level.offset;
    t.width = level.width;
    t.height = level.height;
    t.flags = flags;
    return ml_bilinear(v, t, uv);
}

} // namespace

u64 MlMipChains::textureTexels(u32 t) const {
    u64 n = 0;
    for (u32 l = 0; l < levelCount[t]; ++l) {
        const MlMipLevel& level = levels[firstLevel[t] + l];
        n += static_cast<u64>(level.width) * level.height;
    }
    return n;
}

u32 ml_mip_level_count(u32 width, u32 height) {
    u32 m = width > height ? width : height;
    u32 n = 1u;
    while (m > 1u) {
        m >>= 1u;
        ++n;
    }
    return n;
}

void ml_build_mips(const MlLibrary& library, MlMipChains& out) {
    out = MlMipChains{};
    out.lut = library.lut();
    const std::vector<MlTexture>& textures = library.textures();
    u64 total = 0;
    for (const MlTexture& t : textures) {
        u32 w = t.width;
        u32 h = t.height;
        for (u32 l = 0; l < ml_mip_level_count(t.width, t.height); ++l) {
            total += static_cast<u64>(w) * h;
            w = w > 1u ? w / 2u : 1u;
            h = h > 1u ? h / 2u : 1u;
        }
    }
    out.texels.resize(static_cast<usize>(total));
    u32 cursor = 0;
    for (const MlTexture& t : textures) {
        out.firstLevel.push_back(static_cast<u32>(out.levels.size()));
        const u32 count = ml_mip_level_count(t.width, t.height);
        out.levelCount.push_back(count);
        u32 w = t.width;
        u32 h = t.height;
        for (u32 l = 0; l < count; ++l) {
            MlMipLevel level{cursor, w, h};
            if (l == 0u) {
                for (u32 i = 0; i < w * h; ++i) {
                    out.texels[cursor + i] = library.texels()[t.offset + i];
                }
            } else {
                const MlMipLevel& prev = out.levels.back();
                downsample(out.texels.data() + prev.offset, prev.width, prev.height, out.texels.data() + cursor, w, h,
                           (t.flags & kMlTexSrgb) != 0u, out.lut);
            }
            out.levels.push_back(level);
            cursor += w * h;
            w = w > 1u ? w / 2u : 1u;
            h = h > 1u ? h / 2u : 1u;
        }
    }
}

f32 ml_trilinear_lod(const MlMipChains& chains, u32 texture, MlF2 dx, MlF2 dy, f32 lodBias) {
    const MlMipLevel& base = chains.levels[chains.firstLevel[texture]];
    const f32 W = static_cast<f32>(base.width);
    const f32 H = static_cast<f32>(base.height);
    const f32 ax = dx.x * W;
    const f32 ay = dx.y * H;
    const f32 bx = dy.x * W;
    const f32 by = dy.y * H;
    const f32 rx = std::sqrt(ax * ax + ay * ay);
    const f32 ry = std::sqrt(bx * bx + by * by);
    const f32 rho = rx > ry ? rx : ry;
    return rho > 0.f ? std::log2(rho) + lodBias : -128.f;
}

MlF4 ml_trilinear(const MlMipChains& chains, u32 texture, u32 flags, MlF2 uv, MlF2 dx, MlF2 dy, f32 lodBias) {
    const u32 first = chains.firstLevel[texture];
    const u32 count = chains.levelCount[texture];
    f32 lambda = ml_trilinear_lod(chains, texture, dx, dy, lodBias);
    if (!(lambda > 0.f)) {
        return levelBilinear(chains, chains.levels[first], flags, uv);
    }
    const f32 top = static_cast<f32>(count - 1u);
    lambda = lambda < top ? lambda : top;
    const f32 d = std::floor(lambda);
    const f32 frac = lambda - d;
    const u32 lo = static_cast<u32>(d);
    const MlF4 a = levelBilinear(chains, chains.levels[first + lo], flags, uv);
    if (frac <= 0.f || lo + 1u >= count) {
        return a;
    }
    const MlF4 b = levelBilinear(chains, chains.levels[first + lo + 1u], flags, uv);
    return MlF4{ml_lerp(a.x, b.x, frac), ml_lerp(a.y, b.y, frac), ml_lerp(a.z, b.z, frac), ml_lerp(a.w, b.w, frac)};
}

MlF4 ml_trilinear_filter(const void* user, u32 texture, MlF2 uv, MlF2 dx, MlF2 dy) {
    const MlTrilinearFilter& f = *static_cast<const MlTrilinearFilter*>(user);
    const u32 flags = texture < f.textureCount ? f.textures[texture].flags : 0u;
    return ml_trilinear(*f.chains, texture, flags, uv, dx, dy, f.lodBias);
}

MlView ml_trilinear_view(const MlLibrary& library, const MlTrilinearFilter& filter) {
    MlView v = library.view();
    v.filter = &ml_trilinear_filter;
    v.filterUser = &filter;
    return v;
}

} // namespace fuse::renderer::material_layers
