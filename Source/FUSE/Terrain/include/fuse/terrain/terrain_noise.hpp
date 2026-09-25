#pragma once

#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

namespace fuse::terrain {

/// Fractal value-noise parameters used by `Terrain::generate` (B7.5).
///
/// Octave `i` places lattice points every `base_cell_texels >> i` texels with amplitude
/// `persistence^i`; lattice values are interpolated with the C2 quintic fade
/// `6t^5 - 15t^4 + 10t^3`, so the surface is continuous in height, slope and curvature
/// across lattice cells (no grid seams). The normalised sum is mapped to [0, max_height].
struct TerrainNoiseParams {
    u32 base_cell_texels = 8;
    u32 octaves = 1;
    f32 persistence = 0.5f;
};

/// Smallest lattice cell an octave may use; finer octaves would approach per-texel white noise.
inline constexpr u32 kMinNoiseCellTexels = 4;
inline constexpr u32 kMaxNoiseOctaves = 6;

/// Noise parameters derived from the heightfield resolution (base cell = pow2(resolution) / 8).
[[nodiscard]] TerrainNoiseParams make_terrain_noise_params(const TerrainDesc& desc);

/// Lattice value in [-1, 1] for octave `octave` at integer lattice coordinate (ix, iz).
[[nodiscard]] f32 terrain_lattice_value(u64 seed, u32 octave, u32 ix, u32 iz);

/// Reference single-texel evaluation of the generated height in metres (matches `Terrain::generate`).
[[nodiscard]] f32 terrain_noise_height(u64 seed, const TerrainNoiseParams& params, f32 max_height, u32 x, u32 z);

/// Fill `out` (resolution x resolution, row-major z*res+x) with generated heights in metres.
void generate_terrain_heights(u64 seed, const TerrainNoiseParams& params, u32 resolution, f32 max_height,
                              f32* out);

} // namespace fuse::terrain
