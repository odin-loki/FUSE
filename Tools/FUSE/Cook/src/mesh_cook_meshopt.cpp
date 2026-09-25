// Asset plan W0.2 (docs/plans/FUSE_ASSET_PLAN.md §1.4, §5.1): meshoptimizer in the mesh cook —
// discrete LOD chains, the WP-1.2 meshlet table and the WP-5.2 cluster DAG (built by the renderer's
// own libraries, so the FMSH tables are exactly what the renderer cooks), the meshopt vertex / index
// codecs, and the FMSH v2 section table that stores them.

#include "mesh_cook_meshopt.hpp"

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

constexpr u32 fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

// Section ids (see mesh_cook.hpp). Meshlet / DAG ids and layouts are the renderer's FMLT chunks.
constexpr u32 kLodt = fourcc('L', 'O', 'D', 'T');
constexpr u32 kLodr = fourcc('L', 'O', 'D', 'R');
constexpr u32 kLodi = fourcc('L', 'O', 'D', 'I');
constexpr u32 kLodz = fourcc('L', 'O', 'D', 'Z');
constexpr u32 kMlth = fourcc('M', 'L', 'T', 'H');
constexpr u32 kSubm = fourcc('S', 'U', 'B', 'M');
constexpr u32 kMshl = fourcc('M', 'S', 'H', 'L');
constexpr u32 kMvrt = fourcc('M', 'V', 'R', 'T');
constexpr u32 kMtri = fourcc('M', 'T', 'R', 'I');
constexpr u32 kDagh = fourcc('D', 'A', 'G', 'H');
constexpr u32 kDmsh = fourcc('D', 'M', 'S', 'H');
constexpr u32 kDmvr = fourcc('D', 'M', 'V', 'R');
constexpr u32 kDmtr = fourcc('D', 'M', 'T', 'R');
constexpr u32 kDgrp = fourcc('D', 'G', 'R', 'P');
constexpr u32 kDgmb = fourcc('D', 'G', 'M', 'B');
constexpr u32 kDclk = fourcc('D', 'C', 'L', 'K');

struct KnownSection {
    u32 id;
    u32 element_bytes;
};
constexpr KnownSection kKnownSections[] = {
    {kLodt, 16u}, {kLodr, 8u},  {kLodi, 4u},  {kLodz, 1u},  {kMlth, 16u}, {kSubm, 16u},
    {kMshl, 96u}, {kMvrt, 4u},  {kMtri, 4u},  {kDagh, 32u}, {kDmsh, 96u}, {kDmvr, 4u},
    {kDmtr, 4u},  {kDgrp, 48u}, {kDgmb, 4u},  {kDclk, 48u},
};

constexpr u32 kDagTerminalBits = 0x7F7FFFFFu; // FLT_MAX

// ---- little-endian writer / reader -----------------------------------------------------------------

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

struct Reader {
    const u8* p;
    u8 u8v() { return *p++; }
    u32 u16v() {
        const u32 v = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
        p += 2;
        return v;
    }
    u32 u32v() {
        const u32 v = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
                      (static_cast<u32>(p[3]) << 24);
        p += 4;
        return v;
    }
    f32 f32v() {
        const u32 bits = u32v();
        f32 v = 0.f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void f32x3(f32 v[3]) {
        for (u32 i = 0; i < 3u; ++i) {
            v[i] = f32v();
        }
    }
    bool record(CookedMeshlet& r) { // false when a reserved word is set
        r.vertex_offset = u32v();
        r.triangle_offset = u32v();
        r.vertex_count = u8v();
        r.triangle_count = u8v();
        r.submesh = u16v();
        f32x3(r.center);
        r.radius = f32v();
        f32x3(r.cone_apex);
        f32x3(r.cone_axis);
        r.cone_cutoff = f32v();
        for (std::int8_t& v : r.cone_axis_s8) {
            v = static_cast<std::int8_t>(u8v());
        }
        r.cone_cutoff_s8 = static_cast<std::int8_t>(u8v());
        f32x3(r.aabb_min);
        f32x3(r.aabb_max);
        const u32 reserved = u32v() | u32v() | u32v();
        return reserved == 0u;
    }
    void bounds(ClusterDagTable::Bounds& b) {
        f32x3(b.center);
        b.radius = f32v();
        b.error = f32v();
    }
};

void pad4(std::vector<u8>& out) {
    while (out.size() % 4u != 0u) {
        out.push_back(0u);
    }
}

bool finite3(const f32 v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

bool meshlet_bounds_ok(const CookedMeshlet& r) {
    return finite3(r.center) && std::isfinite(r.radius) && r.radius >= 0.f && finite3(r.cone_apex) &&
           finite3(r.cone_axis) && std::isfinite(r.cone_cutoff) && finite3(r.aabb_min) && finite3(r.aabb_max) &&
           r.aabb_min[0] <= r.aabb_max[0] && r.aabb_min[1] <= r.aabb_max[1] && r.aabb_min[2] <= r.aabb_max[2];
}

bool dag_bounds_ok(const ClusterDagTable::Bounds& b) {
    u32 bits = 0;
    std::memcpy(&bits, &b.error, sizeof(bits));
    return finite3(b.center) && std::isfinite(b.radius) && b.radius >= 0.f &&
           ((std::isfinite(b.error) && b.error >= 0.f) || bits == kDagTerminalBits);
}

/// Packed meshlet records + vertex / micro-triangle arrays: offsets are running sums, counts within
/// limits, vertex indices < vertex_count, micro-indices < the meshlet's vertex count.
bool check_clusters(const std::vector<CookedMeshlet>& records, const std::vector<u32>& vertices,
                    const std::vector<u32>& triangles, u32 max_vertices, u32 max_triangles, u32 vertex_count,
                    u32 submesh_count, const char* what, std::string* error) {
    u64 vsum = 0;
    u64 tsum = 0;
    for (const CookedMeshlet& r : records) {
        if (r.vertex_offset != vsum || r.triangle_offset != tsum || r.vertex_count == 0u ||
            r.vertex_count > max_vertices || r.triangle_count == 0u || r.triangle_count > max_triangles ||
            r.submesh >= submesh_count || !meshlet_bounds_ok(r)) {
            set_error(error, std::string(what) + " record invalid");
            return false;
        }
        vsum += r.vertex_count;
        tsum += r.triangle_count;
        if (vsum > vertices.size() || tsum > triangles.size()) {
            set_error(error, std::string(what) + " record out of range");
            return false;
        }
        for (u32 t = 0; t < r.triangle_count; ++t) {
            const u32 packed = triangles[r.triangle_offset + t];
            if ((packed >> 24) != 0u || (packed & 0xFFu) >= r.vertex_count || ((packed >> 8) & 0xFFu) >= r.vertex_count ||
                ((packed >> 16) & 0xFFu) >= r.vertex_count) {
                set_error(error, std::string(what) + " micro-index out of range");
                return false;
            }
        }
    }
    if (vsum != vertices.size() || tsum != triangles.size()) {
        set_error(error, std::string(what) + " arrays do not match the records");
        return false;
    }
    for (u32 v : vertices) {
        if (v >= vertex_count) {
            set_error(error, std::string(what) + " vertex index out of range");
            return false;
        }
    }
    return true;
}

struct SectionView {
    u32 element_count = 0;
    const u8* data = nullptr;
};

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

std::vector<u32> read_u32s(const SectionView& view) {
    std::vector<u32> values(view.element_count);
    Reader r{view.data};
    for (u32& v : values) {
        v = r.u32v();
    }
    return values;
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

bool meshopt_decode_vertices(const u8* encoded, usize encoded_bytes, usize count, u32 element_bytes,
                             std::vector<u8>& raw) {
#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
    const u32 stride = (element_bytes + 3u) & ~3u;
    std::vector<u8> padded(count * stride + 4u, 0u); // +4: non-null destination for count == 0
    if (meshopt_decodeVertexBuffer(padded.data(), count, stride, encoded, encoded_bytes) != 0) {
        return false;
    }
    raw.assign(count * element_bytes + 4u, 0u);
    for (usize i = 0; i < count; ++i) {
        const u8* element = padded.data() + i * stride;
        for (u32 b = element_bytes; b < stride; ++b) {
            if (element[b] != 0u) {
                return false; // padding must be zero (the encoder writes zeros)
            }
        }
        std::memcpy(raw.data() + i * element_bytes, element, element_bytes);
    }
    raw.resize(count * element_bytes);
    return true;
#else
    (void)encoded;
    (void)encoded_bytes;
    (void)count;
    (void)element_bytes;
    (void)raw;
    return false;
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

bool meshopt_decode_indices(const u8* encoded, usize encoded_bytes, usize count, std::vector<u32>& out) {
#if defined(FUSE_COOK_HAS_MESHOPTIMIZER)
    if (count % 3u != 0u || encoded_bytes == 0u) {
        return false;
    }
    out.assign(count + 3u, 0u);
    const u32 kind = encoded[0] & 0xF0u;
    const int status = kind == 0xE0u   ? meshopt_decodeIndexBuffer(out.data(), count, 4u, encoded, encoded_bytes)
                       : kind == 0xD0u ? meshopt_decodeIndexSequence(out.data(), count, 4u, encoded, encoded_bytes)
                                       : -1;
    if (status != 0) {
        out.clear();
        return false;
    }
    out.resize(count);
    return true;
#else
    (void)encoded;
    (void)encoded_bytes;
    (void)count;
    out.clear();
    return false;
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

bool read_sections(const u8* data, usize size, usize& consumed, bool codec, CookedMesh& out, std::string* error) {
    (void)codec;
    auto fail = [&](const std::string& message) {
        set_error(error, "section table: " + message);
        return false;
    };
    if (size < 4u) {
        return fail("truncated");
    }
    Reader header{data};
    const u32 sectionCount = header.u32v();
    usize cursor = 4u;
    std::map<u32, SectionView> sections;
    for (u32 s = 0; s < sectionCount; ++s) {
        if (cursor + 12u > size) {
            return fail("truncated");
        }
        Reader r{data + cursor};
        const u32 id = r.u32v();
        const u32 elementBytes = r.u32v();
        const u32 elementCount = r.u32v();
        cursor += 12u;
        const u64 bytes = static_cast<u64>(elementBytes) * elementCount;
        const u64 padded = (bytes + 3u) & ~static_cast<u64>(3u);
        if (static_cast<u64>(cursor) + padded > size) {
            return fail("section out of bounds");
        }
        const KnownSection* known = nullptr;
        for (const KnownSection& k : kKnownSections) {
            if (k.id == id) {
                known = &k;
            }
        }
        if (known != nullptr) {
            if (known->element_bytes != elementBytes) {
                return fail("section element size mismatch");
            }
            if (!sections.emplace(id, SectionView{elementCount, data + cursor}).second) {
                return fail("section duplicated");
            }
        }
        cursor += static_cast<usize>(padded);
    }
    consumed = cursor;
    auto has = [&](u32 id) { return sections.count(id) != 0u; };
    const u32 vertexCount = out.vertex_count();
    const u32 submeshCount = static_cast<u32>(out.submeshes.size());

    // LODs.
    const bool anyLod = has(kLodt) || has(kLodr) || has(kLodi) || has(kLodz);
    if (anyLod) {
        if (!has(kLodt) || !has(kLodr) || has(kLodi) == has(kLodz)) {
            return fail("LOD sections incomplete");
        }
        const SectionView lodt = sections[kLodt];
        const SectionView lodr = sections[kLodr];
        if (static_cast<u64>(lodr.element_count) != static_cast<u64>(lodt.element_count) * submeshCount) {
            return fail("LODR count must be LOD count x submesh count");
        }
        Reader rt{lodt.data};
        Reader rr{lodr.data};
        u64 total = 0;
        out.lods.resize(lodt.element_count);
        for (MeshLod& lod : out.lods) {
            lod.error = rt.f32v();
            lod.target_ratio = rt.f32v();
            if ((rt.u32v() | rt.u32v()) != 0u || !std::isfinite(lod.error) || lod.error < 0.f ||
                !(lod.target_ratio > 0.f && lod.target_ratio <= 1.f)) {
                return fail("LOD record invalid");
            }
            lod.ranges.resize(submeshCount);
            for (MeshLod::Range& range : lod.ranges) {
                range.index_offset = rr.u32v();
                range.index_count = rr.u32v();
                if (range.index_offset != total || range.index_count % 3u != 0u) {
                    return fail("LOD ranges must tile the LOD indices in order");
                }
                total += range.index_count;
            }
        }
        if (total > 0xFFFFFFFFull) {
            return fail("LOD index count overflow");
        }
        if (has(kLodi)) {
            if (sections[kLodi].element_count != total) {
                return fail("LODI count disagrees with the LOD ranges");
            }
            out.lod_indices = read_u32s(sections[kLodi]);
        } else {
            const SectionView lodz = sections[kLodz];
            if (!meshopt_codec_available() ||
                !meshopt_decode_indices(lodz.data, lodz.element_count, static_cast<usize>(total), out.lod_indices)) {
                return fail("LODZ meshopt decode failed");
            }
        }
        for (u32 index : out.lod_indices) {
            if (index >= vertexCount) {
                return fail("LOD index out of range");
            }
        }
    }

    // Meshlets.
    const u32 meshletIds[] = {kMlth, kSubm, kMshl, kMvrt, kMtri};
    const u32 meshletPresent = static_cast<u32>(std::count_if(std::begin(meshletIds), std::end(meshletIds), has));
    if (meshletPresent != 0u && meshletPresent != 5u) {
        return fail("meshlet sections incomplete");
    }
    if (meshletPresent == 5u) {
        MeshletTable& t = out.meshlets;
        const SectionView mlth = sections[kMlth];
        if (mlth.element_count != 1u) {
            return fail("MLTH must have one element");
        }
        Reader rh{mlth.data};
        t.max_vertices = rh.u32v();
        t.max_triangles = rh.u32v();
        if ((rh.u32v() | rh.u32v()) != 0u || t.max_vertices == 0u || t.max_vertices > 255u || t.max_triangles == 0u ||
            t.max_triangles > 252u) {
            return fail("MLTH invalid");
        }
        const SectionView subm = sections[kSubm];
        if (subm.element_count != submeshCount) {
            return fail("SUBM must have one element per submesh");
        }
        Reader rs{subm.data};
        t.submeshes.resize(subm.element_count);
        for (MeshletTable::SubmeshRange& s : t.submeshes) {
            s.meshlet_offset = rs.u32v();
            s.meshlet_count = rs.u32v();
            s.material_index = rs.u32v();
            s.triangle_count = rs.u32v();
        }
        const SectionView mshl = sections[kMshl];
        t.meshlets.resize(mshl.element_count);
        Reader rm{mshl.data};
        for (CookedMeshlet& r : t.meshlets) {
            if (!rm.record(r)) {
                return fail("MSHL reserved field set");
            }
        }
        t.vertices = read_u32s(sections[kMvrt]);
        t.triangles = read_u32s(sections[kMtri]);
        if (!check_clusters(t.meshlets, t.vertices, t.triangles, t.max_vertices, t.max_triangles, vertexCount,
                            submeshCount, "meshlet", error)) {
            return false;
        }
        u64 meshletSum = 0;
        for (u32 s = 0; s < submeshCount; ++s) {
            const MeshletTable::SubmeshRange& range = t.submeshes[s];
            if (range.meshlet_offset != meshletSum || range.material_index != out.submeshes[s].material_index) {
                return fail("SUBM ranges must tile the meshlets in submesh order");
            }
            u64 triangles = 0;
            for (u32 m = range.meshlet_offset; m < range.meshlet_offset + range.meshlet_count && m < t.meshlets.size(); ++m) {
                if (t.meshlets[m].submesh != s) {
                    return fail("meshlet submesh disagrees with SUBM");
                }
                triangles += t.meshlets[m].triangle_count;
            }
            if (triangles != range.triangle_count || triangles * 3u != out.submeshes[s].index_count) {
                return fail("SUBM triangle count disagrees with the meshlets / submesh");
            }
            meshletSum += range.meshlet_count;
        }
        if (meshletSum != t.meshlets.size()) {
            return fail("SUBM ranges do not cover every meshlet");
        }
    }

    // Cluster DAG.
    const u32 dagIds[] = {kDagh, kDmsh, kDmvr, kDmtr, kDgrp, kDgmb, kDclk};
    const u32 dagPresent = static_cast<u32>(std::count_if(std::begin(dagIds), std::end(dagIds), has));
    if (dagPresent != 0u && dagPresent != 7u) {
        return fail("cluster DAG sections incomplete");
    }
    if (dagPresent == 7u) {
        if (out.meshlets.empty()) {
            return fail("cluster DAG without meshlets");
        }
        ClusterDagTable& d = out.cluster_dag;
        const SectionView dagh = sections[kDagh];
        if (dagh.element_count != 1u) {
            return fail("DAGH must have one element");
        }
        Reader rh{dagh.data};
        d.leaf_cluster_count = rh.u32v();
        const u32 lodCount = rh.u32v();
        const u32 groupCount = rh.u32v();
        d.level_count = rh.u32v();
        const u32 lodVertexCount = rh.u32v();
        const u32 lodTriangleCount = rh.u32v();
        if ((rh.u32v() | rh.u32v()) != 0u || d.leaf_cluster_count != out.meshlets.meshlets.size() ||
            lodCount != sections[kDmsh].element_count || groupCount != sections[kDgrp].element_count ||
            lodVertexCount != sections[kDmvr].element_count || lodTriangleCount != sections[kDmtr].element_count ||
            d.level_count == 0u || groupCount == 0u) {
            return fail("DAGH disagrees with the DAG sections");
        }
        d.lod_clusters.resize(lodCount);
        Reader rm{sections[kDmsh].data};
        for (CookedMeshlet& r : d.lod_clusters) {
            if (!rm.record(r)) {
                return fail("DMSH reserved field set");
            }
        }
        d.lod_vertices = read_u32s(sections[kDmvr]);
        d.lod_triangles = read_u32s(sections[kDmtr]);
        if (!check_clusters(d.lod_clusters, d.lod_vertices, d.lod_triangles, out.meshlets.max_vertices,
                            out.meshlets.max_triangles, vertexCount, submeshCount, "DAG cluster", error)) {
            return false;
        }
        const u32 clusterCount = d.cluster_count();
        d.groups.resize(groupCount);
        Reader rg{sections[kDgrp].data};
        for (ClusterDagTable::Group& g : d.groups) {
            rg.bounds(g.bounds);
            g.member_offset = rg.u32v();
            g.member_count = rg.u32v();
            g.child_offset = rg.u32v();
            g.child_count = rg.u32v();
            g.depth = rg.u32v();
            g.submesh = rg.u32v();
            g.reserved = rg.u32v();
        }
        d.group_members = read_u32s(sections[kDgmb]);
        d.links.resize(sections[kDclk].element_count);
        Reader rl{sections[kDclk].data};
        for (ClusterDagTable::Link& l : d.links) {
            rl.bounds(l.self);
            rl.bounds(l.parent);
            l.group = rl.u32v();
            l.refined = rl.u32v();
        }
        if (d.group_members.size() != clusterCount || d.links.size() != clusterCount) {
            return fail("DGMB / DCLK must have one entry per cluster");
        }
        std::vector<u32> groupOf(clusterCount, 0xFFFFFFFFu);
        u64 memberSum = 0;
        u64 childSum = d.leaf_cluster_count;
        for (u32 gi = 0; gi < groupCount; ++gi) {
            const ClusterDagTable::Group& g = d.groups[gi];
            if (g.member_offset != memberSum || g.member_count == 0u || g.reserved != 0u || g.submesh >= submeshCount ||
                !dag_bounds_ok(g.bounds)) {
                return fail("DGRP record invalid");
            }
            if (g.child_count == 0u ? g.child_offset != 0u : g.child_offset != childSum) {
                return fail("DGRP child ranges must tile the LOD clusters in group order");
            }
            childSum += g.child_count;
            memberSum += g.member_count;
            if (memberSum > clusterCount) {
                return fail("DGRP member range out of bounds");
            }
            for (u32 m = g.member_offset; m < g.member_offset + g.member_count; ++m) {
                const u32 cluster = d.group_members[m];
                if (cluster >= clusterCount || groupOf[cluster] != 0xFFFFFFFFu) {
                    return fail("every cluster must be a member of exactly one group");
                }
                groupOf[cluster] = gi;
            }
        }
        if (memberSum != clusterCount || childSum != clusterCount) {
            return fail("DGRP ranges do not cover every cluster");
        }
        for (u32 c = 0; c < clusterCount; ++c) {
            const ClusterDagTable::Link& l = d.links[c];
            const bool leaf = c < d.leaf_cluster_count;
            if (l.group != groupOf[c] || (leaf ? l.refined != 0xFFFFFFFFu : l.refined >= groupCount) ||
                !dag_bounds_ok(l.self) || !dag_bounds_ok(l.parent)) {
                return fail("DCLK record invalid");
            }
        }
    }
    return true;
}

} // namespace detail

} // namespace fuse::cook
