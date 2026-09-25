#include <fuse/cook/bcn_encoder.hpp>

#include <fuse/cook/bc7_encoder.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace fuse::cook {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// ---------------------------------------------------------------------------------------------
// Little-endian bit stream over one block.
// ---------------------------------------------------------------------------------------------
struct BitWriter {
    u8* data;
    u32 bit = 0;
    void put(u32 value, u32 count) {
        for (u32 i = 0; i < count; ++i, ++bit) {
            if ((value >> i) & 1u) {
                data[bit >> 3] = static_cast<u8>(data[bit >> 3] | (1u << (bit & 7u)));
            }
        }
    }
};

struct BitReader {
    const u8* data;
    u32 bit = 0;
    u32 get(u32 count) {
        u32 value = 0;
        for (u32 i = 0; i < count; ++i, ++bit) {
            value |= static_cast<u32>((data[bit >> 3] >> (bit & 7u)) & 1u) << i;
        }
        return value;
    }
};

// ---------------------------------------------------------------------------------------------
// Principal axis of up to 16 points in `dims` dimensions (power iteration, deterministic start).
// ---------------------------------------------------------------------------------------------
void principal_axis(const f32 (*points)[3], u32 count, u32 dims, f32 mean[3], f32 axis[3]) {
    for (u32 d = 0; d < 3u; ++d) {
        mean[d] = 0.f;
        axis[d] = 0.f;
    }
    for (u32 i = 0; i < count; ++i) {
        for (u32 d = 0; d < dims; ++d) {
            mean[d] += points[i][d];
        }
    }
    for (u32 d = 0; d < dims; ++d) {
        mean[d] /= static_cast<f32>(count);
    }
    f32 cov[3][3] = {};
    for (u32 i = 0; i < count; ++i) {
        f32 c[3] = {};
        for (u32 d = 0; d < dims; ++d) {
            c[d] = points[i][d] - mean[d];
        }
        for (u32 a = 0; a < dims; ++a) {
            for (u32 b = 0; b < dims; ++b) {
                cov[a][b] += c[a] * c[b];
            }
        }
    }
    f32 v[3] = {1.f, 1.f, 1.f};
    // Start from the largest-variance diagonal direction so a degenerate (1,1,1) start cannot stall.
    u32 largest = 0;
    for (u32 d = 1; d < dims; ++d) {
        if (cov[d][d] > cov[largest][largest]) {
            largest = d;
        }
    }
    for (u32 d = 0; d < dims; ++d) {
        v[d] = cov[largest][d];
    }
    for (u32 iter = 0; iter < 12u; ++iter) {
        f32 next[3] = {};
        for (u32 a = 0; a < dims; ++a) {
            for (u32 b = 0; b < dims; ++b) {
                next[a] += cov[a][b] * v[b];
            }
        }
        f32 len = 0.f;
        for (u32 d = 0; d < dims; ++d) {
            len += next[d] * next[d];
        }
        len = std::sqrt(len);
        if (len <= 1e-12f) {
            break;
        }
        for (u32 d = 0; d < dims; ++d) {
            v[d] = next[d] / len;
        }
    }
    f32 len = 0.f;
    for (u32 d = 0; d < dims; ++d) {
        len += v[d] * v[d];
    }
    len = std::sqrt(len);
    if (len > 1e-12f) {
        for (u32 d = 0; d < dims; ++d) {
            axis[d] = v[d] / len;
        }
    }
}

// Solve min Σ (a_i·E0 + b_i·E1 − x_i)² per channel. Returns false when the system is singular.
bool least_squares_endpoints(const f32 (*points)[3], const f32* weight0, u32 count, u32 dims, f32 e0[3],
                             f32 e1[3]) {
    f32 aa = 0.f;
    f32 ab = 0.f;
    f32 bb = 0.f;
    f32 ax[3] = {};
    f32 bx[3] = {};
    for (u32 i = 0; i < count; ++i) {
        const f32 a = weight0[i];
        const f32 b = 1.f - a;
        aa += a * a;
        ab += a * b;
        bb += b * b;
        for (u32 d = 0; d < dims; ++d) {
            ax[d] += a * points[i][d];
            bx[d] += b * points[i][d];
        }
    }
    const f32 det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-6f) {
        return false;
    }
    for (u32 d = 0; d < dims; ++d) {
        e0[d] = (ax[d] * bb - bx[d] * ab) / det;
        e1[d] = (bx[d] * aa - ax[d] * ab) / det;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// BC1
// ---------------------------------------------------------------------------------------------
struct Rgb565 {
    s32 c[3]; // r5 g6 b5
};

constexpr s32 kBc1Max[3] = {31, 63, 31};

u16 pack565(const Rgb565& e) {
    return static_cast<u16>((e.c[0] << 11) | (e.c[1] << 5) | e.c[2]);
}

void expand565(u16 packed, s32 out[3]) {
    const s32 r = (packed >> 11) & 31;
    const s32 g = (packed >> 5) & 63;
    const s32 b = packed & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

void bc1_palette(u16 c0, u16 c1, s32 palette[4][3]) {
    expand565(c0, palette[0]);
    expand565(c1, palette[1]);
    for (u32 d = 0; d < 3u; ++d) {
        if (c0 > c1) {
            palette[2][d] = (2 * palette[0][d] + palette[1][d]) / 3;
            palette[3][d] = (palette[0][d] + 2 * palette[1][d]) / 3;
        } else {
            palette[2][d] = (palette[0][d] + palette[1][d]) / 2;
            palette[3][d] = 0;
        }
    }
}

Rgb565 quantize565(const f32 value[3]) {
    Rgb565 e{};
    for (u32 d = 0; d < 3u; ++d) {
        const f32 scaled = std::clamp(value[d], 0.f, 255.f) * static_cast<f32>(kBc1Max[d]) / 255.f;
        e.c[d] = std::clamp(static_cast<s32>(std::lround(scaled)), 0, kBc1Max[d]);
    }
    return e;
}

/// Four-colour evaluation of an endpoint pair (order-independent): returns the squared error and
/// fills `packed0/packed1/indices` in block order (c0 > c1, or c0 == c1 with every index 0).
u64 bc1_evaluate(const f32 (*texels)[3], const Rgb565& a, const Rgb565& b, u16& packed0, u16& packed1,
                 u8 indices[16]) {
    u16 p0 = pack565(a);
    u16 p1 = pack565(b);
    if (p0 < p1) {
        std::swap(p0, p1);
    }
    s32 palette[4][3];
    bc1_palette(p0, p1, palette);
    const u32 entries = p0 == p1 ? 1u : 4u;
    u64 total = 0;
    for (u32 i = 0; i < 16u; ++i) {
        u64 best = std::numeric_limits<u64>::max();
        u8 bestIndex = 0;
        for (u32 k = 0; k < entries; ++k) {
            u64 err = 0;
            for (u32 d = 0; d < 3u; ++d) {
                const s64 diff = static_cast<s64>(std::lround(texels[i][d])) - palette[k][d];
                err += static_cast<u64>(diff * diff);
            }
            if (err < best) {
                best = err;
                bestIndex = static_cast<u8>(k);
            }
        }
        indices[i] = bestIndex;
        total += best;
    }
    packed0 = p0;
    packed1 = p1;
    return total;
}

// ---------------------------------------------------------------------------------------------
// BC4
// ---------------------------------------------------------------------------------------------
void bc4_palette(u32 r0, u32 r1, u32 palette[8]) {
    palette[0] = r0;
    palette[1] = r1;
    if (r0 > r1) {
        for (u32 k = 2; k < 8u; ++k) {
            palette[k] = ((8u - k) * r0 + (k - 1u) * r1 + 3u) / 7u;
        }
    } else {
        for (u32 k = 2; k < 6u; ++k) {
            palette[k] = ((6u - k) * r0 + (k - 1u) * r1 + 2u) / 5u;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
}

u64 bc4_evaluate(const u8 values[16], u32 r0, u32 r1, u8 indices[16]) {
    u32 palette[8];
    bc4_palette(r0, r1, palette);
    u64 total = 0;
    for (u32 i = 0; i < 16u; ++i) {
        u32 best = std::numeric_limits<u32>::max();
        u8 bestIndex = 0;
        for (u32 k = 0; k < 8u; ++k) {
            const s32 diff = static_cast<s32>(values[i]) - static_cast<s32>(palette[k]);
            const u32 err = static_cast<u32>(diff * diff);
            if (err < best) {
                best = err;
                bestIndex = static_cast<u8>(k);
            }
        }
        indices[i] = bestIndex;
        total += best;
    }
    return total;
}

void bc4_pack(u32 r0, u32 r1, const u8 indices[16], u8 block[8]) {
    std::memset(block, 0, 8);
    block[0] = static_cast<u8>(r0);
    block[1] = static_cast<u8>(r1);
    BitWriter writer{block, 16u};
    for (u32 i = 0; i < 16u; ++i) {
        writer.put(indices[i], 3u);
    }
}

// ---------------------------------------------------------------------------------------------
// BC6H (UF16, mode 11)
// ---------------------------------------------------------------------------------------------
constexpr u32 kBc6Weights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
constexpr u32 kBc6MaxHalf = 0x7BFFu; // largest finite half; UF16 finishes at (65535 * 31) >> 6

u32 bc6_unquantize10(u32 q) {
    if (q == 0u) {
        return 0u;
    }
    if (q >= 1023u) {
        return 0xFFFFu;
    }
    return ((q << 16) + 0x8000u) >> 10;
}

u32 bc6_finish(u32 interpolated) {
    return (interpolated * 31u) >> 6;
}

u32 bc6_quantize10(f32 target) {
    // unquantize(q) = 64q + 32 for 0 < q < 1023; try the rounded code and its neighbours.
    const s32 guess = static_cast<s32>(std::lround((target - 32.f) / 64.f));
    u32 best = 0;
    f32 bestErr = std::numeric_limits<f32>::max();
    for (s32 q = guess - 1; q <= guess + 1; ++q) {
        const u32 code = static_cast<u32>(std::clamp(q, 0, 1023));
        const f32 err = std::fabs(static_cast<f32>(bc6_unquantize10(code)) - target);
        if (err < bestErr) {
            bestErr = err;
            best = code;
        }
    }
    return best;
}

struct Bc6Endpoints {
    u32 a[3];
    u32 b[3];
};

u64 bc6_evaluate(const u32 texels[16][3], const Bc6Endpoints& e, u8 indices[16]) {
    u32 ua[3];
    u32 ub[3];
    for (u32 d = 0; d < 3u; ++d) {
        ua[d] = bc6_unquantize10(e.a[d]);
        ub[d] = bc6_unquantize10(e.b[d]);
    }
    u32 palette[16][3];
    for (u32 k = 0; k < 16u; ++k) {
        for (u32 d = 0; d < 3u; ++d) {
            palette[k][d] = bc6_finish(((64u - kBc6Weights[k]) * ua[d] + kBc6Weights[k] * ub[d] + 32u) >> 6);
        }
    }
    u64 total = 0;
    for (u32 i = 0; i < 16u; ++i) {
        u64 best = std::numeric_limits<u64>::max();
        u8 bestIndex = 0;
        for (u32 k = 0; k < 16u; ++k) {
            u64 err = 0;
            for (u32 d = 0; d < 3u; ++d) {
                const s64 diff = static_cast<s64>(texels[i][d]) - static_cast<s64>(palette[k][d]);
                err += static_cast<u64>(diff * diff);
            }
            if (err < best) {
                best = err;
                bestIndex = static_cast<u8>(k);
            }
        }
        indices[i] = bestIndex;
        total += best;
    }
    return total;
}

u16 sanitize_uf16(u16 h) {
    if ((h & 0x8000u) != 0u) {
        return 0; // negative (or -0 / -NaN): UF16 has no sign
    }
    if ((h & 0x7C00u) == 0x7C00u) {
        return (h & 0x03FFu) != 0u ? static_cast<u16>(0) : static_cast<u16>(kBc6MaxHalf); // NaN → 0, +Inf → max
    }
    return h;
}

} // namespace

// =============================================================================================
// Public API
// =============================================================================================

const char* bc_format_name(BcFormat format) {
    switch (format) {
    case BcFormat::BC1:
        return "BC1";
    case BcFormat::BC4:
        return "BC4";
    case BcFormat::BC5:
        return "BC5";
    case BcFormat::BC6H:
        return "BC6H";
    case BcFormat::BC7:
        return "BC7";
    }
    return "BC7";
}

bool parse_bc_format(const std::string& text, BcFormat& out) {
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static constexpr BcFormat kAll[] = {BcFormat::BC1, BcFormat::BC4, BcFormat::BC5, BcFormat::BC6H, BcFormat::BC7};
    for (BcFormat format : kAll) {
        if (upper == bc_format_name(format)) {
            out = format;
            return true;
        }
    }
    return false;
}

u32 bc_block_bytes(BcFormat format) {
    return (format == BcFormat::BC1 || format == BcFormat::BC4) ? 8u : 16u;
}

u32 bc_block_count(u32 width, u32 height) {
    return ((width + 3u) / 4u) * ((height + 3u) / 4u);
}

void bc1_encode_block(const u8 rgba[64], u8 block[8]) {
    f32 texels[16][3];
    for (u32 i = 0; i < 16u; ++i) {
        for (u32 d = 0; d < 3u; ++d) {
            texels[i][d] = static_cast<f32>(rgba[i * 4u + d]);
        }
    }
    f32 mean[3];
    f32 axis[3];
    principal_axis(texels, 16u, 3u, mean, axis);
    f32 lo = std::numeric_limits<f32>::max();
    f32 hi = -std::numeric_limits<f32>::max();
    for (u32 i = 0; i < 16u; ++i) {
        f32 t = 0.f;
        for (u32 d = 0; d < 3u; ++d) {
            t += (texels[i][d] - mean[d]) * axis[d];
        }
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    f32 e0[3];
    f32 e1[3];
    for (u32 d = 0; d < 3u; ++d) {
        e0[d] = mean[d] + axis[d] * hi;
        e1[d] = mean[d] + axis[d] * lo;
    }
    Rgb565 bestA = quantize565(e0);
    Rgb565 bestB = quantize565(e1);
    u16 p0 = 0;
    u16 p1 = 0;
    u8 indices[16];
    u64 bestErr = bc1_evaluate(texels, bestA, bestB, p0, p1, indices);

    // Least-squares refinement on the chosen indices (block order: palette[0] = the larger code).
    for (u32 iter = 0; iter < 2u && bestErr > 0u; ++iter) {
        static constexpr f32 kWeight0[4] = {1.f, 0.f, 2.f / 3.f, 1.f / 3.f};
        f32 w0[16];
        for (u32 i = 0; i < 16u; ++i) {
            w0[i] = kWeight0[indices[i]];
        }
        f32 r0[3];
        f32 r1[3];
        if (!least_squares_endpoints(texels, w0, 16u, 3u, r0, r1)) {
            break;
        }
        const Rgb565 a = quantize565(r0);
        const Rgb565 b = quantize565(r1);
        u16 q0 = 0;
        u16 q1 = 0;
        u8 trial[16];
        const u64 err = bc1_evaluate(texels, a, b, q0, q1, trial);
        if (err >= bestErr) {
            break;
        }
        bestErr = err;
        bestA = a;
        bestB = b;
        p0 = q0;
        p1 = q1;
        std::memcpy(indices, trial, 16);
    }

    // Greedy ±1 search on the six quantised components.
    for (u32 pass = 0; pass < 2u && bestErr > 0u; ++pass) {
        bool improved = false;
        for (u32 which = 0; which < 2u; ++which) {
            for (u32 d = 0; d < 3u; ++d) {
                for (s32 step : {-1, 1}) {
                    Rgb565 a = bestA;
                    Rgb565 b = bestB;
                    Rgb565& e = which == 0u ? a : b;
                    const s32 next = e.c[d] + step;
                    if (next < 0 || next > kBc1Max[d]) {
                        continue;
                    }
                    e.c[d] = next;
                    u16 q0 = 0;
                    u16 q1 = 0;
                    u8 trial[16];
                    const u64 err = bc1_evaluate(texels, a, b, q0, q1, trial);
                    if (err < bestErr) {
                        bestErr = err;
                        bestA = a;
                        bestB = b;
                        p0 = q0;
                        p1 = q1;
                        std::memcpy(indices, trial, 16);
                        improved = true;
                    }
                }
            }
        }
        if (!improved) {
            break;
        }
    }

    block[0] = static_cast<u8>(p0 & 0xFFu);
    block[1] = static_cast<u8>(p0 >> 8);
    block[2] = static_cast<u8>(p1 & 0xFFu);
    block[3] = static_cast<u8>(p1 >> 8);
    u32 bits = 0;
    for (u32 i = 0; i < 16u; ++i) {
        bits |= static_cast<u32>(indices[i]) << (i * 2u);
    }
    for (u32 i = 0; i < 4u; ++i) {
        block[4 + i] = static_cast<u8>((bits >> (i * 8u)) & 0xFFu);
    }
}

void bc1_decode_block(const u8 block[8], u8 rgba[64]) {
    const u16 c0 = static_cast<u16>(block[0] | (block[1] << 8));
    const u16 c1 = static_cast<u16>(block[2] | (block[3] << 8));
    s32 palette[4][3];
    bc1_palette(c0, c1, palette);
    const u32 bits = static_cast<u32>(block[4]) | (static_cast<u32>(block[5]) << 8) |
                     (static_cast<u32>(block[6]) << 16) | (static_cast<u32>(block[7]) << 24);
    for (u32 i = 0; i < 16u; ++i) {
        const u32 k = (bits >> (i * 2u)) & 3u;
        for (u32 d = 0; d < 3u; ++d) {
            rgba[i * 4u + d] = static_cast<u8>(palette[k][d]);
        }
        rgba[i * 4u + 3] = (c0 <= c1 && k == 3u) ? 0u : 255u;
    }
}

void bc4_encode_block(const u8 values[16], u8 block[8]) {
    u32 lo = 255;
    u32 hi = 0;
    for (u32 i = 0; i < 16u; ++i) {
        lo = std::min<u32>(lo, values[i]);
        hi = std::max<u32>(hi, values[i]);
    }
    u8 indices[16] = {};
    if (lo == hi) {
        bc4_pack(lo, lo, indices, block);
        return;
    }
    u32 bestR0 = hi;
    u32 bestR1 = lo;
    u64 bestErr = bc4_evaluate(values, hi, lo, indices);
    const u32 window = std::min<u32>(6u, (hi - lo) / 2u);
    for (u32 r0 = hi - window; r0 <= hi && bestErr > 0u; ++r0) {
        for (u32 r1 = lo; r1 <= lo + window && r1 < r0; ++r1) {
            u8 trial[16];
            const u64 err = bc4_evaluate(values, r0, r1, trial);
            if (err < bestErr) {
                bestErr = err;
                bestR0 = r0;
                bestR1 = r1;
                std::memcpy(indices, trial, 16);
            }
        }
    }
    // Six-value mode (r0 <= r1, explicit 0 and 255) when the block holds an extreme.
    if (bestErr > 0u && (lo == 0u || hi == 255u)) {
        u32 innerLo = 255;
        u32 innerHi = 0;
        for (u32 i = 0; i < 16u; ++i) {
            if (values[i] != 0u && values[i] != 255u) {
                innerLo = std::min<u32>(innerLo, values[i]);
                innerHi = std::max<u32>(innerHi, values[i]);
            }
        }
        if (innerLo > innerHi) {
            innerLo = innerHi = 0;
        }
        u8 trial[16];
        const u64 err = bc4_evaluate(values, innerLo, innerHi, trial);
        if (err < bestErr) {
            bestErr = err;
            bestR0 = innerLo;
            bestR1 = innerHi;
            std::memcpy(indices, trial, 16);
        }
    }
    bc4_pack(bestR0, bestR1, indices, block);
}

void bc4_decode_block(const u8 block[8], u8 values[16]) {
    u32 palette[8];
    bc4_palette(block[0], block[1], palette);
    BitReader reader{block, 16u};
    for (u32 i = 0; i < 16u; ++i) {
        values[i] = static_cast<u8>(palette[reader.get(3u)]);
    }
}

void bc5_encode_block(const u8 red[16], const u8 green[16], u8 block[16]) {
    bc4_encode_block(red, block);
    bc4_encode_block(green, block + 8);
}

void bc5_decode_block(const u8 block[16], u8 red[16], u8 green[16]) {
    bc4_decode_block(block, red);
    bc4_decode_block(block + 8, green);
}

void bc6h_encode_block(const u16 rgb[48], u8 block[16]) {
    u32 texels[16][3];
    f32 targets[16][3];
    for (u32 i = 0; i < 16u; ++i) {
        for (u32 d = 0; d < 3u; ++d) {
            texels[i][d] = std::min<u32>(sanitize_uf16(rgb[i * 3u + d]), kBc6MaxHalf);
            // Invert the UF16 finish ((x * 31) >> 6) into the interpolation domain.
            targets[i][d] = static_cast<f32>(texels[i][d]) * 64.f / 31.f;
        }
    }
    f32 mean[3];
    f32 axis[3];
    principal_axis(targets, 16u, 3u, mean, axis);
    f32 lo = std::numeric_limits<f32>::max();
    f32 hi = -std::numeric_limits<f32>::max();
    for (u32 i = 0; i < 16u; ++i) {
        f32 t = 0.f;
        for (u32 d = 0; d < 3u; ++d) {
            t += (targets[i][d] - mean[d]) * axis[d];
        }
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    Bc6Endpoints best{};
    for (u32 d = 0; d < 3u; ++d) {
        best.a[d] = bc6_quantize10(std::clamp(mean[d] + axis[d] * lo, 0.f, 65535.f));
        best.b[d] = bc6_quantize10(std::clamp(mean[d] + axis[d] * hi, 0.f, 65535.f));
    }
    u8 indices[16];
    u64 bestErr = bc6_evaluate(texels, best, indices);

    for (u32 iter = 0; iter < 2u && bestErr > 0u; ++iter) {
        f32 w0[16];
        for (u32 i = 0; i < 16u; ++i) {
            w0[i] = 1.f - static_cast<f32>(kBc6Weights[indices[i]]) / 64.f;
        }
        f32 ra[3];
        f32 rb[3];
        if (!least_squares_endpoints(targets, w0, 16u, 3u, ra, rb)) {
            break;
        }
        Bc6Endpoints trial{};
        for (u32 d = 0; d < 3u; ++d) {
            trial.a[d] = bc6_quantize10(std::clamp(ra[d], 0.f, 65535.f));
            trial.b[d] = bc6_quantize10(std::clamp(rb[d], 0.f, 65535.f));
        }
        u8 trialIdx[16];
        const u64 err = bc6_evaluate(texels, trial, trialIdx);
        if (err >= bestErr) {
            break;
        }
        bestErr = err;
        best = trial;
        std::memcpy(indices, trialIdx, 16);
    }
    for (u32 pass = 0; pass < 2u && bestErr > 0u; ++pass) {
        bool improved = false;
        for (u32 which = 0; which < 2u; ++which) {
            for (u32 d = 0; d < 3u; ++d) {
                for (s32 step : {-1, 1}) {
                    Bc6Endpoints trial = best;
                    u32& code = which == 0u ? trial.a[d] : trial.b[d];
                    const s32 next = static_cast<s32>(code) + step;
                    if (next < 0 || next > 1023) {
                        continue;
                    }
                    code = static_cast<u32>(next);
                    u8 trialIdx[16];
                    const u64 err = bc6_evaluate(texels, trial, trialIdx);
                    if (err < bestErr) {
                        bestErr = err;
                        best = trial;
                        std::memcpy(indices, trialIdx, 16);
                        improved = true;
                    }
                }
            }
        }
        if (!improved) {
            break;
        }
    }

    // Anchor texel 0 stores 3 index bits: its index must be < 8, otherwise swap the endpoints.
    if (indices[0] >= 8u) {
        std::swap(best.a, best.b);
        for (u8& index : indices) {
            index = static_cast<u8>(15u - index);
        }
    }

    std::memset(block, 0, 16);
    BitWriter writer{block, 0u};
    writer.put(0x03u, 5u); // mode 11
    for (u32 d = 0; d < 3u; ++d) {
        writer.put(best.a[d], 10u);
    }
    for (u32 d = 0; d < 3u; ++d) {
        writer.put(best.b[d], 10u);
    }
    writer.put(indices[0], 3u);
    for (u32 i = 1; i < 16u; ++i) {
        writer.put(indices[i], 4u);
    }
}

bool bc6h_decode_block(const u8 block[16], u16 rgb[48]) {
    BitReader reader{block, 0u};
    if (reader.get(5u) != 0x03u) {
        return false;
    }
    u32 a[3];
    u32 b[3];
    for (u32& value : a) {
        value = bc6_unquantize10(reader.get(10u));
    }
    for (u32& value : b) {
        value = bc6_unquantize10(reader.get(10u));
    }
    for (u32 i = 0; i < 16u; ++i) {
        const u32 index = reader.get(i == 0u ? 3u : 4u);
        const u32 w = kBc6Weights[index];
        for (u32 d = 0; d < 3u; ++d) {
            rgb[i * 3u + d] = static_cast<u16>(bc6_finish(((64u - w) * a[d] + w * b[d] + 32u) >> 6));
        }
    }
    return true;
}

bool encode_bc_image(BcFormat format, const BcSourceImage& image, std::vector<u8>& outBlocks, std::string* error) {
    const u32 width = image.width;
    const u32 height = image.height;
    if (width == 0u || height == 0u) {
        set_error(error, "encode: zero-size image");
        return false;
    }
    const usize texels = static_cast<usize>(width) * height;
    const bool hdr = format == BcFormat::BC6H;
    if (hdr ? image.rgba16f.size() < texels * 4u : image.rgba8.size() < texels * 4u) {
        set_error(error, std::string("encode: ") + bc_format_name(format) +
                             (hdr ? " needs an RGBA16F source" : " needs an RGBA8 source"));
        return false;
    }
    const u32 blocksX = (width + 3u) / 4u;
    const u32 blocksY = (height + 3u) / 4u;
    const u32 blockBytes = bc_block_bytes(format);
    outBlocks.assign(static_cast<usize>(blocksX) * blocksY * blockBytes, 0u);
    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            u8* dst = outBlocks.data() + (static_cast<usize>(by) * blocksX + bx) * blockBytes;
            u8 tile[64];
            u16 tileHalf[48];
            for (u32 y = 0; y < 4u; ++y) {
                for (u32 x = 0; x < 4u; ++x) {
                    const u32 sx = std::min(bx * 4u + x, width - 1u);
                    const u32 sy = std::min(by * 4u + y, height - 1u);
                    const usize src = (static_cast<usize>(sy) * width + sx) * 4u;
                    const u32 t = y * 4u + x;
                    if (hdr) {
                        for (u32 d = 0; d < 3u; ++d) {
                            tileHalf[t * 3u + d] = image.rgba16f[src + d];
                        }
                    } else {
                        for (u32 d = 0; d < 4u; ++d) {
                            tile[t * 4u + d] = image.rgba8[src + d];
                        }
                    }
                }
            }
            switch (format) {
            case BcFormat::BC1:
                bc1_encode_block(tile, dst);
                break;
            case BcFormat::BC4: {
                u8 red[16];
                for (u32 t = 0; t < 16u; ++t) {
                    red[t] = tile[t * 4u];
                }
                bc4_encode_block(red, dst);
                break;
            }
            case BcFormat::BC5: {
                u8 red[16];
                u8 green[16];
                for (u32 t = 0; t < 16u; ++t) {
                    red[t] = tile[t * 4u];
                    green[t] = tile[t * 4u + 1u];
                }
                bc5_encode_block(red, green, dst);
                break;
            }
            case BcFormat::BC6H:
                bc6h_encode_block(tileHalf, dst);
                break;
            case BcFormat::BC7:
                bc7_encode_block(tile, dst);
                break;
            }
        }
    }
    return true;
}

bool decode_bc_image(BcFormat format, const u8* blocks, usize blockBytes, u32 width, u32 height, BcSourceImage& out) {
    out = BcSourceImage{};
    if (blocks == nullptr || width == 0u || height == 0u) {
        return false;
    }
    const u32 blocksX = (width + 3u) / 4u;
    const u32 blocksY = (height + 3u) / 4u;
    const u32 bytesPerBlock = bc_block_bytes(format);
    if (blockBytes < static_cast<usize>(blocksX) * blocksY * bytesPerBlock) {
        return false;
    }
    out.width = width;
    out.height = height;
    const bool hdr = format == BcFormat::BC6H;
    if (hdr) {
        out.rgba16f.assign(static_cast<usize>(width) * height * 4u, 0u);
    } else {
        out.rgba8.assign(static_cast<usize>(width) * height * 4u, 0u);
    }
    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            const u8* src = blocks + (static_cast<usize>(by) * blocksX + bx) * bytesPerBlock;
            u8 tile[64] = {};
            u16 tileHalf[48] = {};
            switch (format) {
            case BcFormat::BC1:
                bc1_decode_block(src, tile);
                break;
            case BcFormat::BC4: {
                u8 red[16];
                bc4_decode_block(src, red);
                for (u32 t = 0; t < 16u; ++t) {
                    tile[t * 4u] = red[t];
                    tile[t * 4u + 3u] = 255u;
                }
                break;
            }
            case BcFormat::BC5: {
                u8 red[16];
                u8 green[16];
                bc5_decode_block(src, red, green);
                for (u32 t = 0; t < 16u; ++t) {
                    tile[t * 4u] = red[t];
                    tile[t * 4u + 1u] = green[t];
                    tile[t * 4u + 3u] = 255u;
                }
                break;
            }
            case BcFormat::BC6H:
                if (!bc6h_decode_block(src, tileHalf)) {
                    out = BcSourceImage{};
                    return false;
                }
                break;
            case BcFormat::BC7:
                if (!bc7_decode_block(src, tile)) {
                    out = BcSourceImage{};
                    return false;
                }
                break;
            }
            for (u32 y = 0; y < 4u; ++y) {
                for (u32 x = 0; x < 4u; ++x) {
                    const u32 px = bx * 4u + x;
                    const u32 py = by * 4u + y;
                    if (px >= width || py >= height) {
                        continue;
                    }
                    const usize dst = (static_cast<usize>(py) * width + px) * 4u;
                    const u32 t = y * 4u + x;
                    if (hdr) {
                        for (u32 d = 0; d < 3u; ++d) {
                            out.rgba16f[dst + d] = tileHalf[t * 3u + d];
                        }
                        out.rgba16f[dst + 3u] = 0x3C00u;
                    } else {
                        for (u32 d = 0; d < 4u; ++d) {
                            out.rgba8[dst + d] = tile[t * 4u + d];
                        }
                    }
                }
            }
        }
    }
    return true;
}

u16 float_to_half(f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 16) & 0x8000u;
    const u32 exponent = (bits >> 23) & 0xFFu;
    u32 mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu) {
        return static_cast<u16>(sign | 0x7C00u | (mantissa != 0u ? 0x200u : 0u));
    }
    const s32 e = static_cast<s32>(exponent) - 127 + 15;
    if (e >= 31) {
        return static_cast<u16>(sign | 0x7C00u);
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<u16>(sign);
        }
        mantissa |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 half = mantissa >> shift;
        const u32 rem = mantissa & ((1u << shift) - 1u);
        const u32 halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half & 1u) != 0u)) {
            ++half;
        }
        return static_cast<u16>(sign | half);
    }
    u32 half = (static_cast<u32>(e) << 10) | (mantissa >> 13);
    const u32 rem = mantissa & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) {
        ++half; // may carry into the exponent (and up to Inf), which is the correct rounding
    }
    return static_cast<u16>(sign | half);
}

f32 half_to_float(u16 value) {
    const u32 sign = (static_cast<u32>(value) & 0x8000u) << 16;
    const u32 exponent = (value >> 10) & 0x1Fu;
    u32 mantissa = value & 0x3FFu;
    u32 bits = 0;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            s32 e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0u);
            bits = sign | (static_cast<u32>(127 - 15 - e) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
    }
    f32 out = 0.f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

} // namespace fuse::cook
