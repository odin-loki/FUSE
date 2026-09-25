#include <fuse/terrain/terrain_noise.hpp>

#include <algorithm>
#include <vector>

namespace fuse::terrain {

namespace {

u32 next_pow2(u32 value) {
    u32 result = 1;
    while (result < value && result < (1u << 30)) {
        result <<= 1;
    }
    return result;
}

f32 quintic_fade(f32 t) {
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

f32 octave_amplitude_sum(const TerrainNoiseParams& params) {
    f32 total = 0.f;
    f32 amp = 1.f;
    for (u32 i = 0; i < params.octaves; ++i) {
        total += amp;
        amp *= params.persistence;
    }
    return total > 0.f ? total : 1.f;
}

u32 octave_cell(const TerrainNoiseParams& params, u32 octave) {
    return std::max(params.base_cell_texels >> octave, 1u);
}

} // namespace

TerrainNoiseParams make_terrain_noise_params(const TerrainDesc& desc) {
    TerrainNoiseParams params{};
    params.base_cell_texels = std::max(next_pow2(std::max(desc.resolution, 1u)) / 8u, 2u * kMinNoiseCellTexels);
    params.octaves = 0;
    for (u32 cell = params.base_cell_texels; cell >= kMinNoiseCellTexels && params.octaves < kMaxNoiseOctaves;
         cell >>= 1) {
        ++params.octaves;
    }
    params.octaves = std::max(params.octaves, 1u);
    return params;
}

f32 terrain_lattice_value(u64 seed, u32 octave, u32 ix, u32 iz) {
    u64 h = seed ^ (static_cast<u64>(octave + 1u) * 0xD6E8FEB86659FD93ull);
    h ^= static_cast<u64>(ix) * 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull;
    h ^= static_cast<u64>(iz) * 0x94D049BB133111EBull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    const f32 unit = static_cast<f32>(h & 0xFFFFFFull) / static_cast<f32>(0xFFFFFFull);
    return unit * 2.f - 1.f;
}

f32 terrain_noise_height(u64 seed, const TerrainNoiseParams& params, f32 max_height, u32 x, u32 z) {
    f32 sum = 0.f;
    f32 amp = 1.f;
    for (u32 octave = 0; octave < params.octaves; ++octave) {
        const u32 cell = octave_cell(params, octave);
        const u32 ix = x / cell;
        const u32 iz = z / cell;
        const f32 fx = quintic_fade(static_cast<f32>(x % cell) / static_cast<f32>(cell));
        const f32 fz = quintic_fade(static_cast<f32>(z % cell) / static_cast<f32>(cell));
        const f32 v00 = terrain_lattice_value(seed, octave, ix, iz);
        const f32 v10 = terrain_lattice_value(seed, octave, ix + 1, iz);
        const f32 v01 = terrain_lattice_value(seed, octave, ix, iz + 1);
        const f32 v11 = terrain_lattice_value(seed, octave, ix + 1, iz + 1);
        const f32 a = v00 + (v10 - v00) * fx;
        const f32 b = v01 + (v11 - v01) * fx;
        sum += amp * (a + (b - a) * fz);
        amp *= params.persistence;
    }
    const f32 normalised = sum / octave_amplitude_sum(params);
    return std::clamp(normalised * 0.5f + 0.5f, 0.f, 1.f) * max_height;
}

void generate_terrain_heights(u64 seed, const TerrainNoiseParams& params, u32 resolution, f32 max_height,
                              f32* out) {
    if (out == nullptr || resolution == 0) {
        return;
    }

    const usize texel_count = static_cast<usize>(resolution) * static_cast<usize>(resolution);
    std::fill(out, out + texel_count, 0.f);

    std::vector<f32> lattice;
    std::vector<f32> fade_x(resolution);
    std::vector<u32> cell_x(resolution);

    f32 amp = 1.f;
    for (u32 octave = 0; octave < params.octaves; ++octave) {
        const u32 cell = octave_cell(params, octave);
        const u32 lattice_dim = resolution / cell + 2u;
        lattice.resize(static_cast<usize>(lattice_dim) * lattice_dim);
        for (u32 iz = 0; iz < lattice_dim; ++iz) {
            for (u32 ix = 0; ix < lattice_dim; ++ix) {
                lattice[static_cast<usize>(iz) * lattice_dim + ix] = terrain_lattice_value(seed, octave, ix, iz);
            }
        }
        for (u32 x = 0; x < resolution; ++x) {
            cell_x[x] = x / cell;
            fade_x[x] = quintic_fade(static_cast<f32>(x % cell) / static_cast<f32>(cell));
        }

        for (u32 z = 0; z < resolution; ++z) {
            const u32 iz = z / cell;
            const f32 fz = quintic_fade(static_cast<f32>(z % cell) / static_cast<f32>(cell));
            const f32* row0 = lattice.data() + static_cast<usize>(iz) * lattice_dim;
            const f32* row1 = row0 + lattice_dim;
            f32* dst = out + static_cast<usize>(z) * resolution;
            for (u32 x = 0; x < resolution; ++x) {
                const u32 ix = cell_x[x];
                const f32 fx = fade_x[x];
                const f32 a = row0[ix] + (row0[ix + 1] - row0[ix]) * fx;
                const f32 b = row1[ix] + (row1[ix + 1] - row1[ix]) * fx;
                dst[x] += amp * (a + (b - a) * fz);
            }
        }
        amp *= params.persistence;
    }

    const f32 inv_total = 1.f / octave_amplitude_sum(params);
    for (usize i = 0; i < texel_count; ++i) {
        out[i] = std::clamp(out[i] * inv_total * 0.5f + 0.5f, 0.f, 1.f) * max_height;
    }
}

} // namespace fuse::terrain
