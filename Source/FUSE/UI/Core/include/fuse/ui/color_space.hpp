#pragma once
// Colour management basics: sRGB / Display P3 / Rec.2020 (linear <-> encoded, gamut matrices) and the HDR transfer
// functions (SMPTE ST 2084 "PQ", ARIB STD-B67 "HLG"). fuse::ui::Color is sRGB-encoded by convention; these convert it
// for a non-sRGB output (NativeOptions::colorSpace) or for linear-light maths. Header-only, <cmath> only.
#include "fuse/ui/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fuse::ui::color {

enum class Space : u8 { Srgb, DisplayP3, Rec2020 }; ///< all three share D65; P3 uses the sRGB transfer curve

/// IEC 61966-2-1 sRGB electro-optical transfer (encoded 0..1 -> linear 0..1); odd-extended for extended range.
inline f32 srgbToLinear(f32 v) {
    const f32 a = std::abs(v);
    const f32 l = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return std::copysign(l, v);
}
inline f32 linearToSrgb(f32 v) {
    const f32 a = std::abs(v);
    const f32 e = a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.f / 2.4f) - 0.055f;
    return std::copysign(e, v);
}
/// BT.2020 / BT.709 camera curve (used here for Rec.2020 SDR encoding).
inline f32 rec2020ToLinear(f32 v) {
    constexpr f32 al = 1.09929682680944f, be = 0.018053968510807f;
    return v < 4.5f * be ? v / 4.5f : std::pow((v + al - 1.f) / al, 1.f / 0.45f);
}
inline f32 linearToRec2020(f32 v) {
    constexpr f32 al = 1.09929682680944f, be = 0.018053968510807f;
    return v < be ? 4.5f * v : al * std::pow(v, 0.45f) - (al - 1.f);
}

/// SMPTE ST 2084 (PQ): absolute luminance in cd/m^2 (0..10000) <-> signal 0..1.
inline f32 pqEncode(f32 nits) {
    constexpr f32 m1 = 2610.f / 16384.f, m2 = 2523.f / 4096.f * 128.f, c1 = 3424.f / 4096.f, c2 = 2413.f / 4096.f * 32.f,
                  c3 = 2392.f / 4096.f * 32.f;
    const f32 y = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), m1);
    return std::pow((c1 + c2 * y) / (1.f + c3 * y), m2);
}
inline f32 pqDecode(f32 e) {
    constexpr f32 m1 = 2610.f / 16384.f, m2 = 2523.f / 4096.f * 128.f, c1 = 3424.f / 4096.f, c2 = 2413.f / 4096.f * 32.f,
                  c3 = 2392.f / 4096.f * 32.f;
    const f32 p = std::pow(std::clamp(e, 0.f, 1.f), 1.f / m2);
    return 10000.f * std::pow(std::max(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
}
/// ARIB STD-B67 (HLG) OETF: relative scene linear 0..1 <-> signal 0..1.
inline f32 hlgEncode(f32 l) {
    constexpr f32 a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    l = std::max(l, 0.f);
    return l <= 1.f / 12.f ? std::sqrt(3.f * l) : a * std::log(12.f * l - b) + c;
}
inline f32 hlgDecode(f32 e) {
    constexpr f32 a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::max(e, 0.f);
    return e <= 0.5f ? e * e / 3.f : (std::exp((e - c) / a) + b) / 12.f;
}

struct Mat3 {
    f32 m[9];
    void apply(f32& r, f32& g, f32& b) const {
        const f32 x = r, y = g, z = b;
        r = m[0] * x + m[1] * y + m[2] * z;
        g = m[3] * x + m[4] * y + m[5] * z;
        b = m[6] * x + m[7] * y + m[8] * z;
    }
};
/// Linear RGB -> CIE XYZ (D65) for each space; the inverse direction below.
inline const Mat3& toXyz(Space s) {
    static const Mat3 k[3] = {
        {{0.4123908f, 0.3575843f, 0.1804808f, 0.2126390f, 0.7151687f, 0.0721923f, 0.0193308f, 0.1191948f, 0.9505322f}},
        {{0.4865709f, 0.2656677f, 0.1982173f, 0.2289746f, 0.6917385f, 0.0792869f, 0.0000000f, 0.0451134f, 1.0439444f}},
        {{0.6369580f, 0.1446169f, 0.1688810f, 0.2627002f, 0.6779981f, 0.0593017f, 0.0000000f, 0.0280727f, 1.0609851f}}};
    return k[int(s)];
}
inline const Mat3& fromXyz(Space s) {
    static const Mat3 k[3] = {
        {{3.2409699f, -1.5373832f, -0.4986108f, -0.9692436f, 1.8759675f, 0.0415551f, 0.0556301f, -0.2039770f, 1.0569715f}},
        {{2.4934969f, -0.9313836f, -0.4027108f, -0.8294890f, 1.7626641f, 0.0236247f, 0.0358458f, -0.0761724f, 0.9568845f}},
        {{1.7166512f, -0.3556708f, -0.2533663f, -0.6666844f, 1.6164812f, 0.0157685f, 0.0176399f, -0.0427706f, 0.9421031f}}};
    return k[int(s)];
}
inline f32 decode(Space s, f32 v) { return s == Space::Rec2020 ? rec2020ToLinear(v) : srgbToLinear(v); }
inline f32 encode(Space s, f32 v) { return s == Space::Rec2020 ? linearToRec2020(v) : linearToSrgb(v); }

/// Linear-light colour (alpha untouched).
inline Color toLinear(Color c, Space s = Space::Srgb) { return {decode(s, c.r), decode(s, c.g), decode(s, c.b), c.a}; }
inline Color fromLinear(Color c, Space s = Space::Srgb) { return {encode(s, c.r), encode(s, c.g), encode(s, c.b), c.a}; }
/// Re-expresses an encoded colour of space `from` in space `to` (same appearance; out-of-gamut values are clipped
/// to 0..1 unless `clip` is false, e.g. for extended-range outputs).
inline Color convert(Color c, Space from, Space to, bool clip = true) {
    if (from == to) return c;
    Color l = toLinear(c, from);
    toXyz(from).apply(l.r, l.g, l.b);
    fromXyz(to).apply(l.r, l.g, l.b);
    if (clip) l = {std::clamp(l.r, 0.f, 1.f), std::clamp(l.g, 0.f, 1.f), std::clamp(l.b, 0.f, 1.f), l.a};
    return fromLinear(l, to);
}
/// Relative luminance (WCAG / BT.709 weights) of an sRGB colour, 0..1.
inline f32 luminance(Color c) {
    const Color l = toLinear(c);
    return 0.2126f * l.r + 0.7152f * l.g + 0.0722f * l.b;
}
/// WCAG 2 contrast ratio (1..21).
inline f32 contrastRatio(Color a, Color b) {
    const f32 la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}
/// An SDR sRGB colour as an HDR10 (Rec.2020 primaries, PQ) signal, with SDR white mapped to `sdrWhiteNits`.
inline Color toHdr10(Color c, f32 sdrWhiteNits = 203.f) {
    Color l = toLinear(c);
    toXyz(Space::Srgb).apply(l.r, l.g, l.b);
    fromXyz(Space::Rec2020).apply(l.r, l.g, l.b);
    return {pqEncode(std::max(l.r, 0.f) * sdrWhiteNits), pqEncode(std::max(l.g, 0.f) * sdrWhiteNits),
            pqEncode(std::max(l.b, 0.f) * sdrWhiteNits), c.a};
}

/// Packs canvas pixels (RGBA8, R in the low byte, sRGB-encoded) as A2B10G10R10 HDR10 words (Rec.2020 + PQ) for an
/// HDR10 swapchain: table-driven equivalent of toHdr10() (within 1-2 codes of 1023), for whole-frame conversion.
class Hdr10Packer {
public:
    explicit Hdr10Packer(f32 sdrWhiteNits = 203.f) {
        for (int i = 0; i < 256; ++i) m_lin[std::size_t(i)] = srgbToLinear(f32(i) / 255.f);
        const Mat3 &a = toXyz(Space::Srgb), &b = fromXyz(Space::Rec2020);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m_m.m[r * 3 + c] = b.m[r * 3] * a.m[c] + b.m[r * 3 + 1] * a.m[3 + c] + b.m[r * 3 + 2] * a.m[6 + c];
        // PQ table on sqrt(linear): dense near black where PQ is steep
        for (std::size_t i = 0; i < kSteps + 1; ++i) {
            const f32 t = f32(i) / f32(kSteps);
            m_pq[i] = u16(std::lround(std::clamp(pqEncode(t * t * sdrWhiteNits), 0.f, 1.f) * 1023.f));
        }
    }
    u32 pack(u32 px) const {
        f32 r = m_lin[px & 0xFFu], g = m_lin[(px >> 8) & 0xFFu], b = m_lin[(px >> 16) & 0xFFu];
        m_m.apply(r, g, b);
        const u32 a = (px >> 24) * 3u / 255u;
        return u32(q(r)) | (u32(q(g)) << 10) | (u32(q(b)) << 20) | (a << 30);
    }
    void pack(const u32* src, u32* dst, std::size_t n) const {
        for (std::size_t i = 0; i < n; ++i) dst[i] = pack(src[i]);
    }

private:
    static constexpr std::size_t kSteps = 4096;
    u16 q(f32 l) const {
        const f32 t = std::sqrt(std::clamp(l, 0.f, 1.f)) * f32(kSteps);
        return m_pq[std::size_t(t + 0.5f)];
    }
    std::array<f32, 256> m_lin{};
    std::array<u16, kSteps + 1> m_pq{};
    Mat3 m_m{};
};

} // namespace fuse::ui::color
