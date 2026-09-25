// FUSE Relight RL-5.6: terrain baking (docs/plans/FUSE_REMIX_PORT_PLAN.md: the upstream inventory "Terrain baking (rasterizes
// terrain draws and decals into a baked texture)", §5.7 "Decals and terrain baking", the port table's row 29).
//
// The game draws terrain as several passes over the same ground - a base texture, alpha-blended decals and roads,
// lightmap / detail passes (D3D MODULATE / MODULATE2X) - which a path tracer cannot re-blend per hit. The baker
// rasterises every draw classified as terrain (RL-1.2 shouldBakeTerrain, rtx.terrainTextures) top-down into one
// cached RGBA texture over a world region (the horizontal plane of the world's up axis; FUSE: x / z with y up), in
// submission order with each draw's blend state, and the path tracer's terrain material samples it by world
// position (terrainLookup) instead of the draw's own texture. FUSE's own implementation of the concept as the plan
// describes it (no Remix source was used):
//
//   raster     per layer, every triangle's texels (texel centres inside the triangle's top-down projection, edges
//              inclusive), the highest surface wins where a layer overlaps itself (top-down depth test);
//   shading    colour = texture(uv') x vertex colour, uv' = the layer's 2x3 texture transform of the interpolated UV;
//              bilinear, repeat (the path tracer's sampler);
//   blend      Opaque (replace), Alpha (SRCALPHA / INVSRCALPHA), Additive (SRCALPHA / ONE), Multiply (DESTCOLOR / ZERO),
//              Multiply2x (DESTCOLOR / SRCCOLOR: D3D MODULATE2X lightmaps); an optional alpha test per layer;
//   cache      a 64-bit content hash of the region and every layer (geometry, texture contents, state): bakeCached()
//              re-bakes only when it changes.
// Steady-state bakes (no larger region / layer than before) make no heap allocation.
#pragma once

#include <fuse/types.hpp>

#include <span>
#include <vector>

namespace fuse::relight::terrain {

/// Linear RGBA float texture, row-major, top row first (v = 0).
struct TerrainTexture {
    u32 width = 0;
    u32 height = 0;
    const float* rgba = nullptr;
};

enum class TerrainBlend : u32 { Opaque = 0, Alpha = 1, Additive = 2, Multiply = 3, Multiply2x = 4 };

struct TerrainLayer {
    const float* positions = nullptr; ///< world xyz per vertex
    const float* uvs = nullptr;       ///< uv per vertex (null: 0)
    const float* colors = nullptr;    ///< linear rgba per vertex (null: white)
    u32 vertexCount = 0;
    const u32* indices = nullptr;     ///< triangle list
    u32 indexCount = 0;
    const TerrainTexture* texture = nullptr; ///< null: white
    TerrainBlend blend = TerrainBlend::Opaque;
    float uvTransform[6] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f}; ///< u' = [0] u + [1] v + [2], v' = [3] u + [4] v + [5]
    bool alphaTest = false;           ///< discard alpha <= alphaReference
    float alphaReference = 0.5f;
};

struct TerrainBakeDesc {
    float minX = 0.f;
    float minZ = 0.f;
    float maxX = 1.f;
    float maxZ = 1.f;
    u32 width = 64;  ///< texels along x
    u32 height = 64; ///< texels along z (row 0 at minZ)
    float clear[4] = {0.f, 0.f, 0.f, 0.f};
};

struct TerrainBakeStats {
    u32 bakes = 0;
    u32 cacheHits = 0;
    u64 texelsWritten = 0; ///< last bake
};

class TerrainBaker {
public:
    /// Pre-sizes the scratch for `texels` (optional).
    void reserve(u32 texels);
    /// Bakes into `out` (width x height x 4 floats; resized when smaller).
    bool bake(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers, std::vector<float>& out);
    /// bake() when the content hash differs from the last cached bake; returns true when it re-baked.
    bool bakeCached(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers, std::vector<float>& out);
    static u64 contentHash(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers);
    const TerrainBakeStats& stats() const { return m_stats; }
    void invalidate() { m_cachedHash = 0; m_cacheValid = false; }

private:
    std::vector<float> m_bestHeight;
    std::vector<u32> m_bestTriangle;
    std::vector<float> m_bestBary; ///< 2 per texel
    u64 m_cachedHash = 0;
    bool m_cacheValid = false;
    TerrainBakeStats m_stats{};
};

/// The baked colour at world (x, z): bilinear between texel centres, clamped to the region (the path tracer's terrain
/// lookup).
void terrainLookup(const TerrainBakeDesc& desc, const std::vector<float>& baked, float x, float z, float rgba[4]);

/// Bilinear, repeat sample (texel centres at (i + 0.5) / size).
void terrainSample(const TerrainTexture& t, float u, float v, float rgba[4]);

} // namespace fuse::relight::terrain
