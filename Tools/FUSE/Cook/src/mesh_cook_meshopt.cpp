// Asset plan W0.2 (docs/plans/FUSE_ASSET_PLAN.md §1.4, §5.1): meshoptimizer in the mesh cook —
// discrete LOD chains, the WP-1.2 meshlet table and the WP-5.2 cluster DAG (built by the renderer's
// own libraries, so the FMSH tables are exactly what the renderer cooks), the meshopt vertex / index
// codecs, and the FMSH v2 section table that stores them.

#include "mesh_cook_meshopt.hpp"

#include <fuse/asset/detail/fmsh_layout.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
#include <meshoptimizer.h>
#endif
#if defined(FUSE_COOK_HAS_GEOMETRY_DAG)
#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#endif

namespace fuse::cook {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// Section ids: shared with the runtime reader (fuse/asset/detail/fmsh_layout.hpp, UNI-U7-ASSET-1).
constexpr u32 kLodt = asset::detail::kSectionLodt;
constexpr u32 kLodr = asset::detail::kSectionLodr;
constexpr u32 kLodi = asset::detail::kSectionLodi;
constexpr u32 kLodz = asset::detail::kSectionLodz;
constexpr u32 kMlth = asset::detail::kSectionMlth;
constexpr u32 kSubm = asset::detail::kSectionSubm;
constexpr u32 kMshl = asset::detail::kSectionMshl;
constexpr u32 kMvrt = asset::detail::kSectionMvrt;
constexpr u32 kMtri = asset::detail::kSectionMtri;
constexpr u32 kDagh = asset::detail::kSectionDagh;
constexpr u32 kDmsh = asset::detail::kSectionDmsh;
constexpr u32 kDmvr = asset::detail::kSectionDmvr;
constexpr u32 kDmtr = asset::detail::kSectionDmtr;
constexpr u32 kDgrp = asset::detail::kSectionDgrp;
constexpr u32 kDgmb = asset::detail::kSectionDgmb;
constexpr u32 kDclk = asset::detail::kSectionDclk;

// ---- little-endian writer (the reader is in fuse_asset) -----------------------------------------------------------------

struct Writer {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void u16v(u32 v) {
        out.push_back(static_cast<u8>(v & 0xFFu));
        out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    }
    void u32v(u32 v) {
        for (u32 s = 0; s < 32u; s += 8u) {
            out.push_back(static_cast<u8>((v >> s) & 0xFFu));
        }
    }
    void f32v(f32 v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u32v(bits);
    }
    void f32x3(const f32 v[3]) {
        for (u32 i = 0; i < 3u; ++i) {
            f32v(v[i]);
        }
    }
    void record(const CookedMeshlet& r) { // renderer MSHL / DMSH element, 96 bytes
        u32v(r.vertex_offset);
        u32v(r.triangle_offset);
        u8v(static_cast<u8>(r.vertex_count));
        u8v(static_cast<u8>(r.triangle_count));
        u16v(r.submesh);
        f32x3(r.center);
        f32v(r.radius);
        f32x3(r.cone_apex);
        f32x3(r.cone_axis);
        f32v(r.cone_cutoff);
        for (std::int8_t v : r.cone_axis_s8) {
            u8v(static_cast<u8>(v));
        }
        u8v(static_cast<u8>(r.cone_cutoff_s8));
        f32x3(r.aabb_min);
        f32x3(r.aabb_max);
        u32v(0u);
        u32v(0u);
        u32v(0u);
    }
    void bounds(const ClusterDagTable::Bounds& b) {
        f32x3(b.center);
        f32v(b.radius);
        f32v(b.error);
    }
};

void pad4(std::vector<u8>& out) {
    while (out.size() % 4u != 0u) {
        out.push_back(0u);
    }
}

void begin_section(std::vector<u8>& out, u32 id, u32 element_bytes, usize element_count) {
    Writer w{out};
    w.u32v(id);
    w.u32v(element_bytes);
    w.u32v(static_cast<u32>(element_count));
}

void write_u32s(std::vector<u8>& out, u32 id, const std::vector<u32>& values) {
    begin_section(out, id, 4u, values.size());
    Writer w{out};
    for (u32 v : values) {
        w.u32v(v);
    }
}

#if defined(FUSE_COOK_HAS_GEOMETRY_DAG)
CookedMeshlet to_cooked(const renderer::geometry::MeshletRecord& r) {
    CookedMeshlet c;
    c.vertex_offset = r.vertex_offset;
    c.triangle_offset = r.triangle_offset;
    c.vertex_count = r.vertex_count;
    c.triangle_count = r.triangle_count;
    c.submesh = r.submesh;
    std::memcpy(c.center, r.center, sizeof(c.center));
    c.radius = r.radius;
    std::memcpy(c.cone_apex, r.cone_apex, sizeof(c.cone_apex));
    std::memcpy(c.cone_axis, r.cone_axis, sizeof(c.cone_axis));
    c.cone_cutoff = r.cone_cutoff;
    for (u32 i = 0; i < 3u; ++i) {
        c.cone_axis_s8[i] = r.cone_axis_s8[i];
    }
    c.cone_cutoff_s8 = r.cone_cutoff_s8;
    std::memcpy(c.aabb_min, r.aabb_min, sizeof(c.aabb_min));
    std::memcpy(c.aabb_max, r.aabb_max, sizeof(c.aabb_max));
    return c;
}

ClusterDagTable::Bounds to_cooked(const renderer::geometry::dag::DagLodBounds& b) {
    ClusterDagTable::Bounds c;
    std::memcpy(c.center, b.center, sizeof(c.center));
    c.radius = b.radius;
    c.error = b.error;
    return c;
}
#endif

} // namespace

// ---- availability / helpers --------------------------------------------------------------------------

bool mesh_optimizer_available() {
#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
    return true;
#else
    return false;
#endif
}

bool mesh_meshlets_available() {
#if defined(FUSE_COOK_HAS_GEOMETRY_DAG)
    return true;
#else
    return false;
#endif
}

f32 lod_switch_distance(f32 error, f32 fov_y_radians, f32 viewport_height_px, f32 pixels) {
    // projected size in pixels = error * viewport_height / (2 * tan(fov / 2) * distance)
    const f32 denom = 2.f * std::tan(fov_y_radians * 0.5f) * pixels;
    return denom > 0.f ? error * viewport_height_px / denom : 0.f;
}

// ---- LODs ---------------------------------------------------------------------------------------------

bool build_mesh_lods(CookedMesh& mesh, const MeshLodOptions& options, std::string* error) {
#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
    const u32 vertexCount = mesh.vertex_count();
    if (vertexCount == 0u || mesh.indices.size() % 3u != 0u || mesh.normals.size() != static_cast<usize>(vertexCount) * 3u) {
        set_error(error, "lod build: mesh has no vertices, a partial triangle or no normals");
        return false;
    }
    for (u32 index : mesh.indices) {
        if (index >= vertexCount) {
            set_error(error, "lod build: index out of range");
            return false;
        }
    }
    for (const CookedMesh::Submesh& sub : mesh.submeshes) {
        if (static_cast<u64>(sub.index_offset) + sub.index_count > mesh.indices.size() || sub.index_count % 3u != 0u) {
            set_error(error, "lod build: submesh range invalid");
            return false;
        }
    }
    for (f32 ratio : options.ratios) {
        if (!(ratio > 0.f && ratio < 1.f)) {
            set_error(error, "lod build: ratios must lie in (0, 1)");
            return false;
        }
    }
    if (!(options.min_reduction >= 0.f && options.min_reduction < 1.f) || !(options.normal_weight >= 0.f)) {
        set_error(error, "lod build: min_reduction must lie in [0, 1), normal_weight >= 0");
        return false;
    }

    const f32 scale = meshopt_simplifyScale(mesh.positions.data(), vertexCount, 12u);
    const f32 weights[3] = {options.normal_weight, options.normal_weight, options.normal_weight};
    const unsigned int flags = options.lock_border ? static_cast<unsigned int>(meshopt_SimplifyLockBorder) : 0u;

    std::vector<MeshLod> lods;
    std::vector<u32> lodIndices;
    u64 previousTriangles = mesh.indices.size() / 3u;
    f32 previousError = 0.f;
    std::vector<u32> scratch;
    for (f32 ratio : options.ratios) {
        MeshLod lod;
        lod.target_ratio = ratio;
        std::vector<u32> levelIndices;
        f32 levelError = 0.f;
        for (const CookedMesh::Submesh& sub : mesh.submeshes) {
            MeshLod::Range range;
            range.index_offset = static_cast<u32>(lodIndices.size() + levelIndices.size());
            if (sub.index_count > 0u) {
                // Always simplify LOD 0 (best quality per level); the error is measured against LOD 0.
                const u32* source = mesh.indices.data() + sub.index_offset;
                const usize target = static_cast<usize>(static_cast<f64>(sub.index_count / 3u) * ratio) * 3u;
                scratch.assign(sub.index_count, 0u);
                f32 resultError = 0.f;
                usize count = 0;
                if (options.normal_weight > 0.f) {
                    count = meshopt_simplifyWithAttributes(scratch.data(), source, sub.index_count, mesh.positions.data(),
                                                           vertexCount, 12u, mesh.normals.data(), 12u, weights, 3u,
                                                           nullptr, target, 1.f, flags, &resultError);
                } else {
                    count = meshopt_simplify(scratch.data(), source, sub.index_count, mesh.positions.data(), vertexCount,
                                             12u, target, 1.f, flags, &resultError);
                }
                scratch.resize(count);
                meshopt_optimizeVertexCache(scratch.data(), scratch.data(), count, vertexCount);
                levelIndices.insert(levelIndices.end(), scratch.begin(), scratch.end());
                levelError = std::max(levelError, resultError * scale);
                range.index_count = static_cast<u32>(count);
            }
            lod.ranges.push_back(range);
        }
        const u64 triangles = levelIndices.size() / 3u;
        // §5.3 asset_budget_mesh: every level must cut >= min_reduction of the previous one.
        if (triangles == 0u ||
            static_cast<f64>(triangles) > static_cast<f64>(previousTriangles) * (1.0 - static_cast<f64>(options.min_reduction))) {
            break;
        }
        // A coarser level never reports a smaller error than the finer one (both are bounds vs LOD 0).
        lod.error = std::max(levelError, previousError);
        previousError = lod.error;
        previousTriangles = triangles;
        lodIndices.insert(lodIndices.end(), levelIndices.begin(), levelIndices.end());
        lods.push_back(std::move(lod));
    }
    mesh.lods = std::move(lods);
    mesh.lod_indices = std::move(lodIndices);
    return true;
#else
    (void)mesh;
    (void)options;
    set_error(error, "lod build: meshoptimizer is not linked into this build");
    return false;
#endif
}

// ---- meshlets + cluster DAG (renderer WP-1.2 / WP-5.2 builders) --------------------------------------

bool build_mesh_meshlets(CookedMesh& mesh, bool with_dag, std::string* error) {
#if defined(FUSE_COOK_HAS_GEOMETRY_DAG)
    namespace geo = renderer::geometry;
    const u32 vertexCount = mesh.vertex_count();
    if (mesh.normals.size() != static_cast<usize>(vertexCount) * 3u || mesh.uvs.size() != static_cast<usize>(vertexCount) * 2u) {
        set_error(error, "meshlet build: FMSH streams disagree with the vertex count");
        return false;
    }
    // Same source as the WP-1.2 sidecar cook (geometry/cook/meshlet_cook_hook.cpp,
    // build_meshlets_from_cooked): positions, normals, uv0, submeshes; default options.
    geo::MeshletSource source;
    source.positions = mesh.positions.data();
    source.normals = mesh.normals.data();
    source.uvs = mesh.uvs.data();
    source.vertex_count = vertexCount;
    source.indices = mesh.indices.data();
    source.index_count = static_cast<u32>(mesh.indices.size());
    for (const CookedMesh::Submesh& sub : mesh.submeshes) {
        source.submeshes.push_back({sub.index_offset, sub.index_count, sub.material_index});
    }
    geo::MeshletBuildOptions options;
    options.keep_source_vertex_map = true;
    geo::MeshletMesh built;
    if (!geo::build_meshlets(source, options, built, error)) {
        return false;
    }
    if (built.source_vertices.size() != built.vertex_count()) {
        set_error(error, "meshlet build: renderer cook produced no source vertex map");
        return false;
    }
    auto to_fmsh_vertex = [&](u32 v) { return built.source_vertices[v]; };

    MeshletTable table;
    table.max_vertices = built.max_vertices;
    table.max_triangles = built.max_triangles;
    for (const geo::SubmeshRange& s : built.submeshes) {
        table.submeshes.push_back({s.meshlet_offset, s.meshlet_count, s.material_index, s.triangle_count});
    }
    for (const geo::MeshletRecord& r : built.meshlets) {
        table.meshlets.push_back(to_cooked(r));
    }
    table.vertices.reserve(built.meshlet_vertices.size());
    for (u32 v : built.meshlet_vertices) {
        table.vertices.push_back(to_fmsh_vertex(v));
    }
    table.triangles = built.meshlet_triangles;

    ClusterDagTable dagTable;
    if (with_dag) {
        geo::dag::ClusterDag dag;
        if (!geo::dag::build_cluster_dag(built, geo::dag::DagBuildOptions{}, dag, error)) {
            return false;
        }
        dagTable.leaf_cluster_count = dag.leaf_cluster_count;
        dagTable.level_count = dag.level_count;
        for (const geo::MeshletRecord& r : dag.lod_clusters) {
            dagTable.lod_clusters.push_back(to_cooked(r));
        }
        dagTable.lod_vertices.reserve(dag.lod_meshlet_vertices.size());
        for (u32 v : dag.lod_meshlet_vertices) {
            dagTable.lod_vertices.push_back(to_fmsh_vertex(v));
        }
        dagTable.lod_triangles = dag.lod_meshlet_triangles;
        for (const geo::dag::DagGroup& g : dag.groups) {
            ClusterDagTable::Group c;
            c.bounds = to_cooked(g.bounds);
            c.member_offset = g.member_offset;
            c.member_count = g.member_count;
            c.child_offset = g.child_offset;
            c.child_count = g.child_count;
            c.depth = g.depth;
            c.submesh = g.submesh;
            c.reserved = g.reserved;
            dagTable.groups.push_back(c);
        }
        dagTable.group_members = dag.group_members;
        for (const geo::dag::DagClusterLink& l : dag.links) {
            dagTable.links.push_back({to_cooked(l.self), to_cooked(l.parent), l.group, l.refined});
        }
    }
    mesh.meshlets = std::move(table);
    mesh.cluster_dag = std::move(dagTable);
    return true;
#else
    (void)mesh;
    (void)with_dag;
    set_error(error, "meshlet build: the renderer geometry libraries are not linked into this build");
    return false;
#endif
}

namespace detail {

// ---- meshopt codec -------------------------------------------------------------------------------------

bool meshopt_codec_available() { return mesh_optimizer_available(); }

void meshopt_encode_vertices(const u8* raw, usize count, u32 element_bytes, std::vector<u8>& out) {
    out.clear();
#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
    const u32 stride = (element_bytes + 3u) & ~3u;
    std::vector<u8> padded(count * stride, 0u);
    for (usize i = 0; i < count; ++i) {
        std::memcpy(padded.data() + i * stride, raw + i * element_bytes, element_bytes);
    }
    out.resize(meshopt_encodeVertexBufferBound(count, stride));
    // Explicit level 2 / format version 1: output never depends on meshopt_encodeVertexVersion state.
    out.resize(meshopt_encodeVertexBufferLevel(out.data(), out.size(), padded.data(), count, stride, 2, 1));
#else
    (void)raw;
    (void)count;
    (void)element_bytes;
#endif
}

void meshopt_encode_indices(const u32* indices, usize count, u32 vertex_count, std::vector<u8>& out) {
    out.clear();
#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
    // Index codec format: the library default (v1; nothing in FUSE calls meshopt_encodeIndexVersion).
    // The triangle codec may rotate a triangle's corners (winding kept). FMSH never changes the
    // index list, so it is used only when it decodes to exactly `indices`; otherwise the index
    // sequence codec (lossless for any order) is. The blob's first byte tells them apart (0xE_ / 0xD_).
    if (count % 3u == 0u) {
        out.resize(meshopt_encodeIndexBufferBound(count, vertex_count));
        out.resize(meshopt_encodeIndexBuffer(out.data(), out.size(), indices, count));
        std::vector<u32> check(count + 3u, 0u);
        if (meshopt_decodeIndexBuffer(check.data(), count, 4u, out.data(), out.size()) == 0 &&
            (count == 0u || std::memcmp(check.data(), indices, count * sizeof(u32)) == 0)) {
            return;
        }
    }
    out.resize(meshopt_encodeIndexSequenceBound(count, vertex_count));
    out.resize(meshopt_encodeIndexSequence(out.data(), out.size(), indices, count));
#else
    (void)indices;
    (void)count;
    (void)vertex_count;
#endif
}

// ---- sections ------------------------------------------------------------------------------------------

bool has_sections(const CookedMesh& mesh) {
    return !mesh.lods.empty() || !mesh.meshlets.empty() || !mesh.cluster_dag.empty();
}

void write_sections(const CookedMesh& mesh, bool codec, std::vector<u8>& out) {
    Writer w{out};
    const usize countAt = out.size();
    w.u32v(0u);
    u32 count = 0;
    if (!mesh.lods.empty()) {
        begin_section(out, kLodt, 16u, mesh.lods.size());
        for (const MeshLod& lod : mesh.lods) {
            w.f32v(lod.error);
            w.f32v(lod.target_ratio);
            w.u32v(0u);
            w.u32v(0u);
        }
        begin_section(out, kLodr, 8u, mesh.lods.size() * mesh.submeshes.size());
        for (const MeshLod& lod : mesh.lods) {
            for (usize s = 0; s < mesh.submeshes.size(); ++s) {
                const MeshLod::Range range = s < lod.ranges.size() ? lod.ranges[s] : MeshLod::Range{};
                w.u32v(range.index_offset);
                w.u32v(range.index_count);
            }
        }
        if (codec) {
            std::vector<u8> encoded;
            meshopt_encode_indices(mesh.lod_indices.data(), mesh.lod_indices.size(), mesh.vertex_count(), encoded);
            begin_section(out, kLodz, 1u, encoded.size());
            out.insert(out.end(), encoded.begin(), encoded.end());
            pad4(out);
        } else {
            write_u32s(out, kLodi, mesh.lod_indices);
        }
        count += 3u;
    }
    if (!mesh.meshlets.empty()) {
        const MeshletTable& t = mesh.meshlets;
        begin_section(out, kMlth, 16u, 1u);
        w.u32v(t.max_vertices);
        w.u32v(t.max_triangles);
        w.u32v(0u);
        w.u32v(0u);
        begin_section(out, kSubm, 16u, t.submeshes.size());
        for (const MeshletTable::SubmeshRange& s : t.submeshes) {
            w.u32v(s.meshlet_offset);
            w.u32v(s.meshlet_count);
            w.u32v(s.material_index);
            w.u32v(s.triangle_count);
        }
        begin_section(out, kMshl, 96u, t.meshlets.size());
        for (const CookedMeshlet& r : t.meshlets) {
            w.record(r);
        }
        write_u32s(out, kMvrt, t.vertices);
        write_u32s(out, kMtri, t.triangles);
        count += 5u;
    }
    if (!mesh.cluster_dag.empty()) {
        const ClusterDagTable& d = mesh.cluster_dag;
        begin_section(out, kDagh, 32u, 1u);
        w.u32v(d.leaf_cluster_count);
        w.u32v(static_cast<u32>(d.lod_clusters.size()));
        w.u32v(static_cast<u32>(d.groups.size()));
        w.u32v(d.level_count);
        w.u32v(static_cast<u32>(d.lod_vertices.size()));
        w.u32v(static_cast<u32>(d.lod_triangles.size()));
        w.u32v(0u);
        w.u32v(0u);
        begin_section(out, kDmsh, 96u, d.lod_clusters.size());
        for (const CookedMeshlet& r : d.lod_clusters) {
            w.record(r);
        }
        write_u32s(out, kDmvr, d.lod_vertices);
        write_u32s(out, kDmtr, d.lod_triangles);
        begin_section(out, kDgrp, 48u, d.groups.size());
        for (const ClusterDagTable::Group& g : d.groups) {
            w.bounds(g.bounds);
            w.u32v(g.member_offset);
            w.u32v(g.member_count);
            w.u32v(g.child_offset);
            w.u32v(g.child_count);
            w.u32v(g.depth);
            w.u32v(g.submesh);
            w.u32v(g.reserved);
        }
        write_u32s(out, kDgmb, d.group_members);
        begin_section(out, kDclk, 48u, d.links.size());
        for (const ClusterDagTable::Link& l : d.links) {
            w.bounds(l.self);
            w.bounds(l.parent);
            w.u32v(l.group);
            w.u32v(l.refined);
        }
        count += 7u;
    }
    for (u32 i = 0; i < 4u; ++i) {
        out[countAt + i] = static_cast<u8>((count >> (i * 8u)) & 0xFFu);
    }
}

// read_sections moved to the runtime asset library (Source/FUSE/Asset/src/cooked_mesh_reader.cpp).

} // namespace detail

} // namespace fuse::cook
