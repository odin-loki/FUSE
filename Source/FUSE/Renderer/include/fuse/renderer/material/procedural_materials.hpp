#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Per-point material sample (B5.3 procedural materials / SSBO row evaluation).
struct MaterialSample {
    fuse::math::Vec3 albedo{1.f, 1.f, 1.f};
    f32 roughness = 0.5f;
    f32 metallic = 0.f;
    fuse::math::Vec3 emissive{};      // emitted radiance (W / sr / m^2, linear)
    fuse::math::Vec3 normalOffset{};  // tangent-space normal perturbation
};

/// Procedural function ids stored in `Material::proceduralFnId` / GPU flag bits.
enum class ProceduralMaterialId : u32 {
    None = 0,
    Wood = 1,
    Metal = 2,
    Concrete = 3,
};

/// Lattice gradient noise, CPU reference of the procedural kernels.
/// Gradients come from an integer hash of the lattice cell (not a 256-entry permutation table), so the
/// field has no short period: no tiling within the 32-bit lattice range. Quintic fade => C2 continuous.
struct ProceduralNoise {
    static u32 hash(i32 x, i32 y, i32 z, u32 seed);
    /// Gradient noise in roughly [-1, 1]; exactly 0 on lattice points.
    static f32 perlin(const fuse::math::Vec3& p, u32 seed = 0u);
    /// Fractal sum of `octaves` perlin octaves (lacunarity 2, gain 0.5), normalised to roughly [-1, 1].
    static f32 fbm(const fuse::math::Vec3& p, u32 octaves, u32 seed = 0u);
};

/// Analytic materials — evaluated per world-space point, infinite resolution, no UVs (so no UV seams on
/// SDF surfaces). All outputs are continuous in position and clamped to physically valid ranges.
struct ProceduralMaterials {
    static MaterialSample wood(const fuse::math::Vec3& worldPos, u32 seed);
    static MaterialSample metal(const fuse::math::Vec3& worldPos, u32 seed);
    static MaterialSample concrete(const fuse::math::Vec3& worldPos, u32 seed);
    static MaterialSample evaluate(ProceduralMaterialId id, const fuse::math::Vec3& worldPos, u32 seed);
};

} // namespace fuse::renderer
