#pragma once

#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/math.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// Chunk edges in grid order: -X, +X, -Z, +Z.
enum class ChunkEdge : u8 {
    NegX = 0,
    PosX = 1,
    NegZ = 2,
    PosZ = 3,
};

inline constexpr u32 kChunkEdgeCount = 4;

/// Inputs for one CPU chunk mesh (reference of the vertex-shader displacement, also used for collision).
struct ChunkMeshDesc {
    ivec2 chunk_coord{};
    f32 chunk_size = 0.f;       ///< World metres per chunk edge
    u32 chunk_resolution = 1;   ///< LOD 0 quads per chunk edge
    u32 lod = 0;
    /// LOD of the resident neighbour across each edge (`ChunkEdge` order). Edges whose neighbour is
    /// coarser are stitched onto the neighbour's vertices so shared edges are crack-free.
    u32 neighbor_lod[kChunkEdgeCount] = {0, 0, 0, 0};
};

/// Displaced chunk grid: (cells + 1)^2 vertices row-major (z outer, x inner) and a triangle list.
struct ChunkMesh {
    ivec2 chunk_coord{};
    u32 lod = 0;
    u32 cells_per_edge = 0;
    u32 neighbor_lod[kChunkEdgeCount] = {0, 0, 0, 0};
    std::vector<vec3> positions;
    std::vector<vec3> normals;
    std::vector<u32> indices;

    [[nodiscard]] u32 vertex_index(u32 ix, u32 iz) const { return iz * (cells_per_edge + 1u) + ix; }
    [[nodiscard]] bool empty() const { return positions.empty(); }
};

/// Quads per chunk edge at `lod`: halves per level while the count stays an exact divisor.
[[nodiscard]] u32 lod_cells_per_edge(u32 chunk_resolution, u32 lod);

/// Build the displaced, edge-stitched mesh for one chunk by sampling `field` at vertex positions.
void build_chunk_mesh(const Heightfield& field, const ChunkMeshDesc& desc, ChunkMesh& out);

/// Height of the mesh's edge polyline at parameter t in [0, 1] (t runs along +X or +Z).
[[nodiscard]] f32 chunk_mesh_edge_height(const ChunkMesh& mesh, ChunkEdge edge, f32 t);

} // namespace fuse::terrain
