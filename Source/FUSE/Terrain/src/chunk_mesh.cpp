#include <fuse/terrain/chunk_mesh.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

u32 lod_cells_per_edge(u32 chunk_resolution, u32 lod) {
    // Halve only while exact so every coarser LOD's vertices are a subset of every finer LOD's.
    u32 cells = std::max(chunk_resolution, 1u);
    for (u32 level = 0; level < lod && (cells % 2u) == 0u; ++level) {
        cells /= 2u;
    }
    return cells;
}

namespace {

/// Edge vertex (ix, iz) for index `i` along `edge`.
void edge_vertex(ChunkEdge edge, u32 cells, u32 i, u32& ix, u32& iz) {
    switch (edge) {
    case ChunkEdge::NegX:
        ix = 0;
        iz = i;
        break;
    case ChunkEdge::PosX:
        ix = cells;
        iz = i;
        break;
    case ChunkEdge::NegZ:
        ix = i;
        iz = 0;
        break;
    case ChunkEdge::PosZ:
    default:
        ix = i;
        iz = cells;
        break;
    }
}

} // namespace

void build_chunk_mesh(const Heightfield& field, const ChunkMeshDesc& desc, ChunkMesh& out) {
    const u32 base_resolution = std::max(desc.chunk_resolution, 1u);
    const u32 cells = lod_cells_per_edge(base_resolution, desc.lod);
    const u32 step = base_resolution / cells; // LOD 0 texel-quads per LOD quad
    const u32 verts_per_edge = cells + 1u;
    const f32 texel_world = desc.chunk_size / static_cast<f32>(base_resolution);

    out.chunk_coord = desc.chunk_coord;
    out.lod = desc.lod;
    out.cells_per_edge = cells;
    for (u32 e = 0; e < kChunkEdgeCount; ++e) {
        out.neighbor_lod[e] = desc.neighbor_lod[e];
    }
    out.positions.resize(static_cast<usize>(verts_per_edge) * verts_per_edge);
    out.normals.resize(out.positions.size());

    // Global LOD-0 vertex indices keep shared edge positions bit-identical between neighbours.
    const s64 base_x = static_cast<s64>(desc.chunk_coord.x) * base_resolution;
    const s64 base_z = static_cast<s64>(desc.chunk_coord.y) * base_resolution;
    for (u32 iz = 0; iz < verts_per_edge; ++iz) {
        for (u32 ix = 0; ix < verts_per_edge; ++ix) {
            const f32 wx = static_cast<f32>(base_x + static_cast<s64>(ix) * step) * texel_world;
            const f32 wz = static_cast<f32>(base_z + static_cast<s64>(iz) * step) * texel_world;
            const usize index = out.vertex_index(ix, iz);
            out.positions[index] = {wx, field.sample_height(wx, wz), wz};
            out.normals[index] = field.sample_normal(wx, wz);
        }
    }

    // Stitch edges that border a coarser neighbour: vertices between the neighbour's vertices are
    // moved onto the neighbour's edge segment so no T-junction crack opens.
    for (u32 e = 0; e < kChunkEdgeCount; ++e) {
        const u32 neighbor_cells = lod_cells_per_edge(base_resolution, desc.neighbor_lod[e]);
        if (neighbor_cells >= cells) {
            continue;
        }
        const u32 ratio = cells / neighbor_cells;
        const ChunkEdge edge = static_cast<ChunkEdge>(e);
        for (u32 i = 0; i < verts_per_edge; ++i) {
            const u32 offset = i % ratio;
            if (offset == 0) {
                continue;
            }
            u32 ix = 0;
            u32 iz = 0;
            u32 ax = 0;
            u32 az = 0;
            u32 bx = 0;
            u32 bz = 0;
            edge_vertex(edge, cells, i, ix, iz);
            edge_vertex(edge, cells, i - offset, ax, az);
            edge_vertex(edge, cells, i - offset + ratio, bx, bz);
            const f32 ha = out.positions[out.vertex_index(ax, az)].y;
            const f32 hb = out.positions[out.vertex_index(bx, bz)].y;
            const f32 t = static_cast<f32>(offset) / static_cast<f32>(ratio);
            out.positions[out.vertex_index(ix, iz)].y = ha + (hb - ha) * t;
        }
    }

    out.indices.clear();
    out.indices.reserve(static_cast<usize>(cells) * cells * 6u);
    for (u32 iz = 0; iz < cells; ++iz) {
        for (u32 ix = 0; ix < cells; ++ix) {
            const u32 i00 = out.vertex_index(ix, iz);
            const u32 i10 = out.vertex_index(ix + 1, iz);
            const u32 i01 = out.vertex_index(ix, iz + 1);
            const u32 i11 = out.vertex_index(ix + 1, iz + 1);
            out.indices.insert(out.indices.end(), {i00, i01, i10, i10, i01, i11});
        }
    }
}

f32 chunk_mesh_edge_height(const ChunkMesh& mesh, ChunkEdge edge, f32 t) {
    if (mesh.empty() || mesh.cells_per_edge == 0) {
        return 0.f;
    }
    const f32 scaled = std::clamp(t, 0.f, 1.f) * static_cast<f32>(mesh.cells_per_edge);
    const u32 i0 = std::min(static_cast<u32>(scaled), mesh.cells_per_edge - 1u);
    const f32 frac = scaled - static_cast<f32>(i0);
    u32 ax = 0;
    u32 az = 0;
    u32 bx = 0;
    u32 bz = 0;
    edge_vertex(edge, mesh.cells_per_edge, i0, ax, az);
    edge_vertex(edge, mesh.cells_per_edge, i0 + 1u, bx, bz);
    const f32 ha = mesh.positions[mesh.vertex_index(ax, az)].y;
    const f32 hb = mesh.positions[mesh.vertex_index(bx, bz)].y;
    return ha + (hb - ha) * frac;
}

} // namespace fuse::terrain
