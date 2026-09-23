#include <fuse/cook/bc7_encoder.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace fuse::cook {

namespace {

u64 fnv1a64(const u8* data, std::size_t size) {
    u64 hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr u32 kWeights2[4] = {0, 21, 43, 64};
constexpr u32 kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr u32 kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Two-subset partition masks (bit i set = texel i in subset 1) and the subset-1 anchor texel,
// from the BC7 format specification.
constexpr u16 kPartition2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
constexpr u8 kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};

enum class PBitMode : u8 { None, Unique, Shared };

/// Subset of the BC7 modes this encoder emits.
struct ModeSpec {
    u32 mode;
    u32 subsets;
    u32 colorBits;
    u32 alphaBits; // 0: alpha decodes as 255
    PBitMode pbits;
    u32 indexBits;
};

constexpr ModeSpec kMode1{1, 2, 6, 0, PBitMode::Shared, 3};
constexpr ModeSpec kMode6{6, 1, 7, 7, PBitMode::Unique, 4};
constexpr ModeSpec kMode7{7, 2, 5, 5, PBitMode::Unique, 2};

const ModeSpec* mode_spec(u32 mode) {
    switch (mode) {
    case 1:
        return &kMode1;
    case 6:
        return &kMode6;
    case 7:
        return &kMode7;
    default:
        return nullptr;
    }
}

const u32* weight_table(u32 indexBits) {
    return indexBits == 2u ? kWeights2 : (indexBits == 3u ? kWeights3 : kWeights4);
}

struct BitWriter {
    u8* block;
    u32 bit = 0;
    void put(u32 count, u32 value) {
        for (u32 i = 0; i < count; ++i, ++bit) {
            if ((value >> i) & 1u) {
                block[bit >> 3] |= static_cast<u8>(1u << (bit & 7u));
            }
        }
    }
};

struct BitReader {
    const u8* block;
    u32 bit = 0;
    u32 take(u32 count) {
        u32 value = 0;
        for (u32 i = 0; i < count; ++i, ++bit) {
            value |= static_cast<u32>((block[bit >> 3] >> (bit & 7u)) & 1u) << i;
        }
        return value;
    }
};

u8 unquantize(u32 code, u32 bits, u32 pbit, bool hasPbit) {
    u32 value = hasPbit ? ((code << 1) | pbit) : code;
    const u32 n = bits + (hasPbit ? 1u : 0u);
    value <<= (8u - n);
    value |= value >> n;
    return static_cast<u8>(value & 0xFFu);
}

u32 quantize(f32 value, u32 bits, u32 pbit, bool hasPbit) {
    const u32 maxCode = (1u << bits) - 1u;
    const f32 scaled = value * static_cast<f32>(maxCode) / 255.f;
    const i32 guess = static_cast<i32>(std::floor(scaled + 0.5f));
    u32 best = 0;
    f32 bestError = 1e30f;
    for (i32 candidate = guess - 1; candidate <= guess + 1; ++candidate) {
        if (candidate < 0 || candidate > static_cast<i32>(maxCode)) {
            continue;
        }
        const f32 error = std::fabs(static_cast<f32>(unquantize(static_cast<u32>(candidate), bits, pbit, hasPbit)) - value);
        if (error < bestError) {
            bestError = error;
            best = static_cast<u32>(candidate);
        }
    }
    return best;
}

u8 interpolate(u8 e0, u8 e1, u32 weight) {
    return static_cast<u8>(((64u - weight) * e0 + weight * e1 + 32u) >> 6);
}

struct SubsetFit {
    u32 code[2][4] = {};
    u32 p[2] = {};
    u8 indices[16] = {}; // in subset texel order
    u64 error = ~0ull;
};

struct Texels {
    u8 rgba[16][4];
    u32 count = 0;
};

void endpoint_colors(const ModeSpec& spec, const SubsetFit& fit, u8 out[2][4]) {
    const bool hasPbit = spec.pbits != PBitMode::None;
    for (u32 e = 0; e < 2u; ++e) {
        for (u32 channel = 0; channel < 3u; ++channel) {
            out[e][channel] = unquantize(fit.code[e][channel], spec.colorBits, fit.p[e], hasPbit);
        }
        out[e][3] = spec.alphaBits == 0u ? 255u : unquantize(fit.code[e][3], spec.alphaBits, fit.p[e], hasPbit);
    }
}

u64 assign_indices(const ModeSpec& spec, const Texels& texels, SubsetFit& fit) {
    u8 ends[2][4];
    endpoint_colors(spec, fit, ends);
    const u32 levels = 1u << spec.indexBits;
    const u32* weights = weight_table(spec.indexBits);
    u8 palette[16][4];
    for (u32 i = 0; i < levels; ++i) {
        for (u32 channel = 0; channel < 4u; ++channel) {
            palette[i][channel] = interpolate(ends[0][channel], ends[1][channel], weights[i]);
        }
    }
    u64 total = 0;
    for (u32 t = 0; t < texels.count; ++t) {
        u32 bestError = ~0u;
        u8 best = 0;
        for (u32 i = 0; i < levels; ++i) {
            u32 error = 0;
            for (u32 channel = 0; channel < 4u; ++channel) {
                const i32 d = static_cast<i32>(texels.rgba[t][channel]) - static_cast<i32>(palette[i][channel]);
                error += static_cast<u32>(d * d);
            }
            if (error < bestError) {
                bestError = error;
                best = static_cast<u8>(i);
            }
        }
        fit.indices[t] = best;
        total += bestError;
    }
    return total;
}

void try_endpoints(const ModeSpec& spec, const Texels& texels, const f32 lo[4], const f32 hi[4], SubsetFit& best) {
    const bool hasPbit = spec.pbits != PBitMode::None;
    const u32 combos = spec.pbits == PBitMode::Unique ? 4u : (spec.pbits == PBitMode::Shared ? 2u : 1u);
    for (u32 combo = 0; combo < combos; ++combo) {
        SubsetFit candidate;
        candidate.p[0] = spec.pbits == PBitMode::Unique ? (combo & 1u) : combo;
        candidate.p[1] = spec.pbits == PBitMode::Unique ? (combo >> 1) : combo;
        for (u32 e = 0; e < 2u; ++e) {
            const f32* src = e == 0u ? lo : hi;
            for (u32 channel = 0; channel < 3u; ++channel) {
                candidate.code[e][channel] = quantize(src[channel], spec.colorBits, candidate.p[e], hasPbit);
            }
            candidate.code[e][3] = spec.alphaBits == 0u ? 0u : quantize(src[3], spec.alphaBits, candidate.p[e], hasPbit);
        }
        candidate.error = assign_indices(spec, texels, candidate);
        if (candidate.error < best.error) {
            best = candidate;
        }
    }
}

/// Least-squares endpoints for fixed indices; returns false when the system is degenerate.
bool refine_endpoints(const ModeSpec& spec, const Texels& texels, const u8* indices, f32 lo[4], f32 hi[4]) {
    const u32* weights = weight_table(spec.indexBits);
    f32 a11 = 0.f;
    f32 a12 = 0.f;
    f32 a22 = 0.f;
    f32 r1[4] = {};
    f32 r2[4] = {};
    for (u32 t = 0; t < texels.count; ++t) {
        const f32 w = static_cast<f32>(weights[indices[t]]) / 64.f;
        const f32 s = 1.f - w;
        a11 += s * s;
        a12 += s * w;
        a22 += w * w;
        for (u32 channel = 0; channel < 4u; ++channel) {
            const f32 x = static_cast<f32>(texels.rgba[t][channel]);
            r1[channel] += s * x;
            r2[channel] += w * x;
        }
    }
    const f32 det = a11 * a22 - a12 * a12;
    if (std::fabs(det) < 1e-6f) {
        return false;
    }
    const f32 inv = 1.f / det;
    for (u32 channel = 0; channel < 4u; ++channel) {
        lo[channel] = std::clamp((a22 * r1[channel] - a12 * r2[channel]) * inv, 0.f, 255.f);
        hi[channel] = std::clamp((a11 * r2[channel] - a12 * r1[channel]) * inv, 0.f, 255.f);
    }
    return true;
}

/// Mean + dominant covariance axis (power iteration); returns the variance off that axis.
f32 principal_axis(const Texels& texels, u32 channels, f32 mean[4], f32 axis[4]) {
    for (u32 channel = 0; channel < 4u; ++channel) {
        mean[channel] = 0.f;
        axis[channel] = 0.f;
    }
    for (u32 t = 0; t < texels.count; ++t) {
        for (u32 channel = 0; channel < channels; ++channel) {
            mean[channel] += static_cast<f32>(texels.rgba[t][channel]);
        }
    }
    for (u32 channel = 0; channel < channels; ++channel) {
        mean[channel] /= static_cast<f32>(texels.count);
    }
    if (channels < 4u) {
        mean[3] = 255.f;
    }
    f32 cov[4][4] = {};
    for (u32 t = 0; t < texels.count; ++t) {
        f32 d[4] = {};
        for (u32 channel = 0; channel < channels; ++channel) {
            d[channel] = static_cast<f32>(texels.rgba[t][channel]) - mean[channel];
        }
        for (u32 i = 0; i < channels; ++i) {
            for (u32 j = 0; j < channels; ++j) {
                cov[i][j] += d[i] * d[j];
            }
        }
    }
    f32 trace = 0.f;
    u32 seed = 0;
    for (u32 channel = 0; channel < channels; ++channel) {
        trace += cov[channel][channel];
        if (cov[channel][channel] > cov[seed][seed]) {
            seed = channel;
        }
    }
    axis[seed] = 1.f;
    f32 lambda = cov[seed][seed];
    for (u32 iteration = 0; iteration < 8u; ++iteration) {
        f32 next[4] = {};
        for (u32 i = 0; i < channels; ++i) {
            for (u32 j = 0; j < channels; ++j) {
                next[i] += cov[i][j] * axis[j];
            }
        }
        const f32 length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]);
        if (length < 1e-8f) {
            break;
        }
        lambda = length;
        for (u32 i = 0; i < 4u; ++i) {
            axis[i] = next[i] / length;
        }
    }
    return std::max(0.f, trace - lambda);
}

SubsetFit fit_subset(const ModeSpec& spec, const Texels& texels) {
    const u32 channels = spec.alphaBits == 0u ? 3u : 4u;
    f32 mean[4];
    f32 axis[4];
    (void)principal_axis(texels, channels, mean, axis);
    f32 minProj = 0.f;
    f32 maxProj = 0.f;
    for (u32 t = 0; t < texels.count; ++t) {
        f32 proj = 0.f;
        for (u32 channel = 0; channel < channels; ++channel) {
            proj += (static_cast<f32>(texels.rgba[t][channel]) - mean[channel]) * axis[channel];
        }
        minProj = std::min(minProj, proj);
        maxProj = std::max(maxProj, proj);
    }
    f32 lo[4];
    f32 hi[4];
    for (u32 channel = 0; channel < 4u; ++channel) {
        lo[channel] = std::clamp(mean[channel] + axis[channel] * minProj, 0.f, 255.f);
        hi[channel] = std::clamp(mean[channel] + axis[channel] * maxProj, 0.f, 255.f);
    }
    SubsetFit best;
    try_endpoints(spec, texels, lo, hi, best);
    for (u32 iteration = 0; iteration < 2u && best.error > 0u; ++iteration) {
        if (!refine_endpoints(spec, texels, best.indices, lo, hi)) {
            break;
        }
        try_endpoints(spec, texels, lo, hi, best);
    }
    return best;
}

struct BlockEncoding {
    const ModeSpec* spec = nullptr;
    u32 partition = 0;
    SubsetFit fits[2];
    u8 subsetOf[16] = {};
    u64 error = ~0ull;
};

void split_texels(const u8* rgba4x4, u32 subsets, u32 partition, Texels out[2], u8 subsetOf[16]) {
    out[0].count = 0;
    out[1].count = 0;
    for (u32 texel = 0; texel < 16u; ++texel) {
        const u32 subset = subsets == 2u ? ((kPartition2[partition] >> texel) & 1u) : 0u;
        subsetOf[texel] = static_cast<u8>(subset);
        std::memcpy(out[subset].rgba[out[subset].count++], rgba4x4 + texel * 4u, 4);
    }
}

BlockEncoding encode_with_mode(const ModeSpec& spec, const u8* rgba4x4, u32 partition) {
    BlockEncoding encoding;
    encoding.spec = &spec;
    encoding.partition = partition;
    Texels texels[2];
    split_texels(rgba4x4, spec.subsets, partition, texels, encoding.subsetOf);
    encoding.error = 0;
    for (u32 subset = 0; subset < spec.subsets; ++subset) {
        encoding.fits[subset] = fit_subset(spec, texels[subset]);
        encoding.error += encoding.fits[subset].error;
    }
    return encoding;
}

void pack_block(const BlockEncoding& input, u8 block[16]) {
    const ModeSpec& spec = *input.spec;
    BlockEncoding encoding = input;
    const u32 maxIndex = (1u << spec.indexBits) - 1u;
    const u32 half = 1u << (spec.indexBits - 1u);

    // Per-texel indices in block order, then enforce the anchor rule (anchor MSB implicitly 0).
    u8 indices[16];
    u32 cursor[2] = {0, 0};
    for (u32 texel = 0; texel < 16u; ++texel) {
        const u32 subset = encoding.subsetOf[texel];
        indices[texel] = encoding.fits[subset].indices[cursor[subset]++];
    }
    for (u32 subset = 0; subset < spec.subsets; ++subset) {
        const u32 anchor = subset == 0u ? 0u : kAnchor2[encoding.partition];
        if (indices[anchor] < half) {
            continue;
        }
        SubsetFit& fit = encoding.fits[subset];
        for (u32 channel = 0; channel < 4u; ++channel) {
            std::swap(fit.code[0][channel], fit.code[1][channel]);
        }
        std::swap(fit.p[0], fit.p[1]);
        for (u32 texel = 0; texel < 16u; ++texel) {
            if (encoding.subsetOf[texel] == subset) {
                indices[texel] = static_cast<u8>(maxIndex - indices[texel]);
            }
        }
    }

    std::memset(block, 0, 16);
    BitWriter writer{block};
    writer.put(spec.mode + 1u, 1u << spec.mode);
    if (spec.subsets == 2u) {
        writer.put(6, encoding.partition);
    }
    for (u32 channel = 0; channel < 3u; ++channel) {
        for (u32 subset = 0; subset < spec.subsets; ++subset) {
            writer.put(spec.colorBits, encoding.fits[subset].code[0][channel]);
            writer.put(spec.colorBits, encoding.fits[subset].code[1][channel]);
        }
    }
    if (spec.alphaBits != 0u) {
        for (u32 subset = 0; subset < spec.subsets; ++subset) {
            writer.put(spec.alphaBits, encoding.fits[subset].code[0][3]);
            writer.put(spec.alphaBits, encoding.fits[subset].code[1][3]);
        }
    }
    if (spec.pbits == PBitMode::Unique) {
        for (u32 subset = 0; subset < spec.subsets; ++subset) {
            writer.put(1, encoding.fits[subset].p[0]);
            writer.put(1, encoding.fits[subset].p[1]);
        }
    } else if (spec.pbits == PBitMode::Shared) {
        for (u32 subset = 0; subset < spec.subsets; ++subset) {
            writer.put(1, encoding.fits[subset].p[0]);
        }
    }
    for (u32 texel = 0; texel < 16u; ++texel) {
        const bool anchor = texel == 0u || (spec.subsets == 2u && texel == kAnchor2[encoding.partition]);
        writer.put(anchor ? spec.indexBits - 1u : spec.indexBits, indices[texel]);
    }
}

/// Rank the 64 two-subset partitions by the variance left off each subset's principal axis.
void best_partitions(const u8* rgba4x4, u32 channels, u32 count, u32 out[]) {
    std::pair<f32, u32> scored[64];
    for (u32 partition = 0; partition < 64u; ++partition) {
        Texels texels[2];
        u8 subsetOf[16];
        split_texels(rgba4x4, 2u, partition, texels, subsetOf);
        f32 mean[4];
        f32 axis[4];
        const f32 residual = principal_axis(texels[0], channels, mean, axis) + principal_axis(texels[1], channels, mean, axis);
        scored[partition] = {residual, partition};
    }
    std::partial_sort(scored, scored + count, scored + 64);
    for (u32 i = 0; i < count; ++i) {
        out[i] = scored[i].second;
    }
}

u8 box_average(u32 a, u32 b, u32 c, u32 d) {
    return static_cast<u8>((a + b + c + d + 2u) / 4u);
}

} // namespace

u32 bc7_padded_dimension(u32 value) {
    if (value == 0) {
        return 4u;
    }
    return ((value + 3u) / 4u) * 4u;
}

void bc7_encode_block(const u8* rgba4x4, u8 block[16]) {
    BlockEncoding best = encode_with_mode(kMode6, rgba4x4, 0u);

    // Mode 6 is one RGBA line per block. Blocks it cannot represent well (edges, several distinct
    // colours) also try the two-subset modes 1 (opaque RGB) and 7 (RGBA) on the best partitions.
    constexpr u64 kPartitionTrialError = 16u * 12u;
    if (best.error > kPartitionTrialError) {
        bool opaque = true;
        for (u32 texel = 0; texel < 16u; ++texel) {
            opaque = opaque && rgba4x4[texel * 4u + 3u] == 255u;
        }
        constexpr u32 kTrials = 6;
        u32 partitions[kTrials];
        best_partitions(rgba4x4, opaque ? 3u : 4u, kTrials, partitions);
        for (u32 partition : partitions) {
            if (opaque) {
                const BlockEncoding mode1 = encode_with_mode(kMode1, rgba4x4, partition);
                if (mode1.error < best.error) {
                    best = mode1;
                }
            }
            const BlockEncoding mode7 = encode_with_mode(kMode7, rgba4x4, partition);
            if (mode7.error < best.error) {
                best = mode7;
            }
        }
    }
    pack_block(best, block);
}

bool bc7_decode_block(const u8 block[16], u8 rgba4x4[64]) {
    BitReader reader{block};
    u32 mode = 0;
    while (mode < 8u && reader.take(1) == 0u) {
        ++mode;
    }
    const ModeSpec* spec = mode_spec(mode);
    if (spec == nullptr) {
        return false;
    }
    const u32 partition = spec->subsets == 2u ? reader.take(6) : 0u;
    u32 code[2][2][4] = {};
    for (u32 channel = 0; channel < 3u; ++channel) {
        for (u32 subset = 0; subset < spec->subsets; ++subset) {
            code[subset][0][channel] = reader.take(spec->colorBits);
            code[subset][1][channel] = reader.take(spec->colorBits);
        }
    }
    if (spec->alphaBits != 0u) {
        for (u32 subset = 0; subset < spec->subsets; ++subset) {
            code[subset][0][3] = reader.take(spec->alphaBits);
            code[subset][1][3] = reader.take(spec->alphaBits);
        }
    }
    u32 pbits[2][2] = {};
    for (u32 subset = 0; subset < spec->subsets; ++subset) {
        if (spec->pbits == PBitMode::Unique) {
            pbits[subset][0] = reader.take(1);
            pbits[subset][1] = reader.take(1);
        } else if (spec->pbits == PBitMode::Shared) {
            pbits[subset][0] = pbits[subset][1] = reader.take(1);
        }
    }
    const bool hasPbit = spec->pbits != PBitMode::None;
    u8 ends[2][2][4];
    for (u32 subset = 0; subset < spec->subsets; ++subset) {
        for (u32 e = 0; e < 2u; ++e) {
            for (u32 channel = 0; channel < 3u; ++channel) {
                ends[subset][e][channel] = unquantize(code[subset][e][channel], spec->colorBits, pbits[subset][e], hasPbit);
            }
            ends[subset][e][3] = spec->alphaBits == 0u
                                     ? 255u
                                     : unquantize(code[subset][e][3], spec->alphaBits, pbits[subset][e], hasPbit);
        }
    }
    const u32* weights = weight_table(spec->indexBits);
    for (u32 texel = 0; texel < 16u; ++texel) {
        const u32 subset = spec->subsets == 2u ? ((kPartition2[partition] >> texel) & 1u) : 0u;
        const bool anchor = texel == 0u || (spec->subsets == 2u && texel == kAnchor2[partition]);
        const u32 index = reader.take(anchor ? spec->indexBits - 1u : spec->indexBits);
        for (u32 channel = 0; channel < 4u; ++channel) {
            rgba4x4[texel * 4u + channel] = interpolate(ends[subset][0][channel], ends[subset][1][channel], weights[index]);
        }
    }
    return reader.bit == 128u;
}

void bc7_encode_solid_block(u8 block[16], u8 r, u8 g, u8 b, u8 a) {
    u8 tile[64];
    for (u32 texel = 0; texel < 16u; ++texel) {
        tile[texel * 4u + 0] = r;
        tile[texel * 4u + 1] = g;
        tile[texel * 4u + 2] = b;
        tile[texel * 4u + 3] = a;
    }
    bc7_encode_block(tile, block);
}

void bc7_encode_dual_endpoint_block(const u8* rgba4x4, u8 block[16]) {
    bc7_encode_block(rgba4x4, block);
}

bool bc7_decode_dual_endpoint_block(const u8 block[16], u8 rgba4x4[64]) {
    return bc7_decode_block(block, rgba4x4);
}

bool bc7_decode_solid_block(const u8 block[16], u8& r, u8& g, u8& b, u8& a) {
    u8 texels[64];
    if (!bc7_decode_block(block, texels)) {
        return false;
    }
    r = texels[0];
    g = texels[1];
    b = texels[2];
    a = texels[3];
    return true;
}

Bc7RgbaImage synthesize_rgba_from_source(const std::string& source_path) {
    Bc7RgbaImage image;

    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return image;
    }

    std::vector<u8> bytes;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), size);
    }

    const u64 hash = fnv1a64(bytes.data(), bytes.size());
    image.width = bc7_padded_dimension(static_cast<u32>(32u + (hash & 0x3Fu)));
    image.height = bc7_padded_dimension(static_cast<u32>(32u + ((hash >> 8) & 0x3Fu)));
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4u);

    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            const u32 index = (y * image.width + x) * 4u;
            image.rgba[index + 0] = static_cast<u8>((hash + x * 17u + y * 3u) & 0xFFu);
            image.rgba[index + 1] = static_cast<u8>((hash >> 16) + x) & 0xFFu;
            image.rgba[index + 2] = static_cast<u8>((hash >> 32) + y) & 0xFFu;
            image.rgba[index + 3] = 255u;
        }
    }

    return image;
}

Bc7EncodeResult encode_bc7_rgba8(const u8* rgba, u32 width, u32 height, std::vector<u8>& outBlocks) {
    Bc7EncodeResult result;
    if (rgba == nullptr || width == 0 || height == 0) {
        result.note = "invalid rgba input";
        return result;
    }

    const u32 paddedWidth = bc7_padded_dimension(width);
    const u32 paddedHeight = bc7_padded_dimension(height);
    const u32 blocksX = paddedWidth / 4u;
    const u32 blocksY = paddedHeight / 4u;
    result.width = paddedWidth;
    result.height = paddedHeight;
    result.blockCount = blocksX * blocksY;
    result.byteCount = result.blockCount * 16u;

    outBlocks.resize(static_cast<std::size_t>(result.blockCount) * 16u);
    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            u8 tile[64];
            for (u32 y = 0; y < 4u; ++y) {
                for (u32 x = 0; x < 4u; ++x) {
                    const u32 sx = std::min(bx * 4u + x, width - 1u);
                    const u32 sy = std::min(by * 4u + y, height - 1u);
                    const std::size_t srcIndex = (static_cast<std::size_t>(sy) * width + sx) * 4u;
                    const u32 dstIndex = (y * 4u + x) * 4u;
                    tile[dstIndex + 0] = rgba[srcIndex + 0];
                    tile[dstIndex + 1] = rgba[srcIndex + 1];
                    tile[dstIndex + 2] = rgba[srcIndex + 2];
                    tile[dstIndex + 3] = rgba[srcIndex + 3];
                }
            }

            const std::size_t offset = (static_cast<std::size_t>(by) * blocksX + bx) * 16u;
            bc7_encode_block(tile, outBlocks.data() + offset);
        }
    }

    result.ok = true;
    result.note = "bc7 mode-6 block encoding";
    return result;
}

bool decode_bc7_rgba8(const u8* blocks, usize blockBytes, u32 width, u32 height, std::vector<u8>& outRgba) {
    if (blocks == nullptr || width == 0 || height == 0) {
        return false;
    }
    const u32 blocksX = bc7_padded_dimension(width) / 4u;
    const u32 blocksY = bc7_padded_dimension(height) / 4u;
    if (blockBytes < static_cast<usize>(blocksX) * blocksY * 16u) {
        return false;
    }
    outRgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            u8 texels[64];
            if (!bc7_decode_block(blocks + (static_cast<std::size_t>(by) * blocksX + bx) * 16u, texels)) {
                return false;
            }
            for (u32 y = 0; y < 4u; ++y) {
                for (u32 x = 0; x < 4u; ++x) {
                    const u32 px = bx * 4u + x;
                    const u32 py = by * 4u + y;
                    if (px >= width || py >= height) {
                        continue;
                    }
                    const std::size_t dst = (static_cast<std::size_t>(py) * width + px) * 4u;
                    for (u32 channel = 0; channel < 4u; ++channel) {
                        outRgba[dst + channel] = texels[(y * 4u + x) * 4u + channel];
                    }
                }
            }
        }
    }
    return true;
}

std::vector<Bc7RgbaImage> build_rgba_mip_chain(const u8* rgba, u32 width, u32 height) {
    std::vector<Bc7RgbaImage> chain;
    if (rgba == nullptr || width == 0 || height == 0) {
        return chain;
    }
    Bc7RgbaImage base;
    base.width = width;
    base.height = height;
    base.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4u);
    chain.push_back(std::move(base));
    while (chain.back().width > 1u || chain.back().height > 1u) {
        const Bc7RgbaImage& src = chain.back();
        Bc7RgbaImage dst;
        dst.width = std::max(1u, src.width / 2u);
        dst.height = std::max(1u, src.height / 2u);
        dst.rgba.resize(static_cast<std::size_t>(dst.width) * dst.height * 4u);
        for (u32 y = 0; y < dst.height; ++y) {
            for (u32 x = 0; x < dst.width; ++x) {
                const u32 x0 = std::min(x * 2u, src.width - 1u);
                const u32 x1 = std::min(x * 2u + 1u, src.width - 1u);
                const u32 y0 = std::min(y * 2u, src.height - 1u);
                const u32 y1 = std::min(y * 2u + 1u, src.height - 1u);
                for (u32 channel = 0; channel < 4u; ++channel) {
                    auto at = [&](u32 sx, u32 sy) {
                        return static_cast<u32>(src.rgba[(static_cast<std::size_t>(sy) * src.width + sx) * 4u + channel]);
                    };
                    dst.rgba[(static_cast<std::size_t>(y) * dst.width + x) * 4u + channel] =
                        box_average(at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1));
                }
            }
        }
        chain.push_back(std::move(dst));
    }
    return chain;
}

} // namespace fuse::cook
