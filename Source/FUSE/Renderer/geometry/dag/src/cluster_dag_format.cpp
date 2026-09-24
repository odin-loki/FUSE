// WP-5.2 cluster DAG: FMLT 1.1 chunks, validation, cut evaluation (see cluster_dag.hpp).

#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry/dag/fmlt_chunks.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>

namespace fuse::renderer::geometry::dag {

namespace {

constexpr u32 kDagh = fmlt_fourcc('D', 'A', 'G', 'H');
constexpr u32 kDmsh = fmlt_fourcc('D', 'M', 'S', 'H');
constexpr u32 kDmvr = fmlt_fourcc('D', 'M', 'V', 'R');
constexpr u32 kDmtr = fmlt_fourcc('D', 'M', 'T', 'R');
constexpr u32 kDgrp = fmlt_fourcc('D', 'G', 'R', 'P');
constexpr u32 kDgmb = fmlt_fourcc('D', 'G', 'M', 'B');
constexpr u32 kDclk = fmlt_fourcc('D', 'C', 'L', 'K');

enum DagChunk : u32 { cDagh = 0, cDmsh, cDmvr, cDmtr, cDgrp, cDgmb, cDclk, cDagChunkKinds };
constexpr u32 kDagFourcc[cDagChunkKinds] = {kDagh, kDmsh, kDmvr, kDmtr, kDgrp, kDgmb, kDclk};
constexpr u32 kDagElementBytes[cDagChunkKinds] = {32u, 96u, 4u, 4u, 48u, 4u, 48u};

// ---- little-endian encode / decode ------------------------------------------------------------------

struct Writer {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void u16v(u16 v) {
        out.push_back(static_cast<u8>(v & 0xFFu));
        out.push_back(static_cast<u8>(v >> 8));
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
    void bounds(const DagLodBounds& b) {
        for (f32 c : b.center) {
            f32v(c);
        }
        f32v(b.radius);
        f32v(b.error);
    }
};

struct Reader {
    const u8* p;
    u8 u8v() { return *p++; }
    i8 i8v() { return static_cast<i8>(*p++); }
    u16 u16v() {
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
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
    DagLodBounds bounds() {
        DagLodBounds b{};
        for (f32& c : b.center) {
            c = f32v();
        }
        b.radius = f32v();
        b.error = f32v();
        return b;
    }
};

/// MSHL layout (meshlet_format.hpp), reused verbatim for DMSH.
void write_record(Writer& w, const MeshletRecord& r) {
    w.u32v(r.vertex_offset);
    w.u32v(r.triangle_offset);
    w.u8v(static_cast<u8>(r.vertex_count));
    w.u8v(static_cast<u8>(r.triangle_count));
    w.u16v(static_cast<u16>(r.submesh));
    for (f32 v : r.center) {
        w.f32v(v);
    }
    w.f32v(r.radius);
    for (f32 v : r.cone_apex) {
        w.f32v(v);
    }
    for (f32 v : r.cone_axis) {
        w.f32v(v);
    }
    w.f32v(r.cone_cutoff);
    for (i8 v : r.cone_axis_s8) {
        w.u8v(static_cast<u8>(v));
    }
    w.u8v(static_cast<u8>(r.cone_cutoff_s8));
    for (f32 v : r.aabb_min) {
        w.f32v(v);
    }
    for (f32 v : r.aabb_max) {
        w.f32v(v);
    }
    for (u32 i = 0; i < 3u; ++i) {
        w.u32v(0u);
    }
}

bool read_record(Reader& r, MeshletRecord& rec) {
    rec.vertex_offset = r.u32v();
    rec.triangle_offset = r.u32v();
    rec.vertex_count = r.u8v();
    rec.triangle_count = r.u8v();
    rec.submesh = r.u16v();
    for (f32& v : rec.center) {
        v = r.f32v();
    }
    rec.radius = r.f32v();
    for (f32& v : rec.cone_apex) {
        v = r.f32v();
    }
    for (f32& v : rec.cone_axis) {
        v = r.f32v();
    }
    rec.cone_cutoff = r.f32v();
    for (i8& v : rec.cone_axis_s8) {
        v = r.i8v();
    }
    rec.cone_cutoff_s8 = r.i8v();
    for (f32& v : rec.aabb_min) {
        v = r.f32v();
    }
    for (f32& v : rec.aabb_max) {
        v = r.f32v();
    }
    u32 reserved = 0;
    for (u32 i = 0; i < 3u; ++i) {
        reserved |= r.u32v();
    }
    return reserved == 0u;
}

FmltChunk make_chunk(u32 kind, u32 count) {
    FmltChunk c;
    c.fourcc = kDagFourcc[kind];
    c.element_bytes = kDagElementBytes[kind];
    c.element_count = count;
    c.payload.reserve(static_cast<usize>(count) * c.element_bytes);
    return c;
}

bool set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool finite3(const f32 v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

bool bounds_ok(const DagLodBounds& b) {
    return finite3(b.center) && std::isfinite(b.radius) && b.radius >= 0.f &&
           (b.error == kDagErrorTerminal || (std::isfinite(b.error) && b.error >= 0.f));
}

bool same_bounds(const DagLodBounds& a, const DagLodBounds& b) { return std::memcmp(&a, &b, sizeof(DagLodBounds)) == 0; }

/// Exact (f64) containment of sphere `inner` in sphere `outer`.
bool sphere_contains(const DagLodBounds& outer, const DagLodBounds& inner) {
    f64 d2 = 0.0;
    for (u32 a = 0; a < 3u; ++a) {
        const f64 d = static_cast<f64>(outer.center[a]) - static_cast<f64>(inner.center[a]);
        d2 += d * d;
    }
    const f64 reach = std::sqrt(d2) + static_cast<f64>(inner.radius);
    return reach <= static_cast<f64>(outer.radius);
}

bool validate_record(const MeshletRecord& r, u64 vertexCursor, u64 triangleCursor, const MeshletMesh& base,
                     const std::vector<u32>& vertices, const std::vector<u32>& triangles, std::string* error) {
    if (r.vertex_count < 1u || r.vertex_count > base.max_vertices || r.triangle_count < 1u ||
        r.triangle_count > base.max_triangles) {
        return set_error(error, "LOD cluster exceeds its vertex / triangle limits");
    }
    if (r.vertex_offset != vertexCursor || r.triangle_offset != triangleCursor) {
        return set_error(error, "LOD clusters are not packed in order");
    }
    if (vertexCursor + r.vertex_count > vertices.size() || triangleCursor + r.triangle_count > triangles.size()) {
        return set_error(error, "LOD cluster range past the end of DMVR / DMTR");
    }
    if (r.submesh >= base.submeshes.size()) {
        return set_error(error, "LOD cluster submesh out of range");
    }
    for (u32 i = 0; i < r.vertex_count; ++i) {
        if (vertices[r.vertex_offset + i] >= base.vertex_count()) {
            return set_error(error, "LOD cluster vertex index out of range");
        }
    }
    for (u32 t = r.triangle_offset; t < r.triangle_offset + r.triangle_count; ++t) {
        const u32 packed = triangles[t];
        if ((packed >> 24) != 0u || triangle_index(packed, 0) >= r.vertex_count ||
            triangle_index(packed, 1) >= r.vertex_count || triangle_index(packed, 2) >= r.vertex_count) {
            return set_error(error, "LOD cluster micro-index out of range");
        }
    }
    if (!finite3(r.center) || !std::isfinite(r.radius) || r.radius < 0.f || !finite3(r.cone_apex) ||
        !finite3(r.cone_axis) || !std::isfinite(r.cone_cutoff) || !finite3(r.aabb_min) || !finite3(r.aabb_max)) {
        return set_error(error, "LOD cluster bounds non-finite or negative");
    }
    for (u32 a = 0; a < 3u; ++a) {
        if (r.aabb_min[a] > r.aabb_max[a]) {
            return set_error(error, "LOD cluster AABB inverted");
        }
    }
    return true;
}

bool reject(ClusterDagMesh& out, std::string* error, MeshletFormatError* code, MeshletFormatError kind,
            const std::string& message) {
    out = ClusterDagMesh{};
    if (code != nullptr) {
        *code = kind;
    }
    return set_error(error, "fusemeshlet dag: " + message);
}

} // namespace

ClusterRef cluster_ref(const ClusterDagMesh& mesh, u32 id) {
    ClusterRef ref;
    if (id < mesh.dag.leaf_cluster_count) {
        const MeshletRecord& r = mesh.base.meshlets[id];
        ref.record = &r;
        ref.vertices = mesh.base.meshlet_vertices.data() + r.vertex_offset;
        ref.triangles = mesh.base.meshlet_triangles.data() + r.triangle_offset;
    } else {
        const MeshletRecord& r = mesh.dag.lod_clusters[id - mesh.dag.leaf_cluster_count];
        ref.record = &r;
        ref.vertices = mesh.dag.lod_meshlet_vertices.data() + r.vertex_offset;
        ref.triangles = mesh.dag.lod_meshlet_triangles.data() + r.triangle_offset;
    }
    return ref;
}

u32 cluster_level(const ClusterDag& dag, u32 id) {
    const u32 refined = dag.links[id].refined;
    return refined == kDagNoGroup ? 0u : dag.groups[refined].depth + 1u;
}

bool validate_cluster_dag(const MeshletMesh& base, const ClusterDag& dag, std::string* error) {
    auto bad = [&](const std::string& message) { return set_error(error, "fusemeshlet dag: " + message); };
    const u32 leaves = static_cast<u32>(base.meshlets.size());
    if (dag.leaf_cluster_count != leaves) {
        return bad("leaf cluster count differs from the MSHL meshlet count");
    }
    const u64 total64 = static_cast<u64>(leaves) + dag.lod_clusters.size();
    if (total64 >= kDagNoGroup) {
        return bad("too many clusters");
    }
    const u32 total = static_cast<u32>(total64);
    if (dag.links.size() != total || dag.group_members.size() != total) {
        return bad("DCLK / DGMB must have one entry per cluster");
    }
    if (dag.groups.size() >= kDagNoGroup || (total > 0u && dag.groups.empty())) {
        return bad("group table size invalid");
    }
    // LOD cluster records.
    std::string why;
    u64 vc = 0;
    u64 tc = 0;
    for (const MeshletRecord& r : dag.lod_clusters) {
        if (!validate_record(r, vc, tc, base, dag.lod_meshlet_vertices, dag.lod_meshlet_triangles, &why)) {
            return bad(why);
        }
        vc += r.vertex_count;
        tc += r.triangle_count;
    }
    if (vc != dag.lod_meshlet_vertices.size() || tc != dag.lod_meshlet_triangles.size()) {
        return bad("DMVR / DMTR hold entries no LOD cluster owns");
    }
    // Groups: member ranges tile DGMB, child ranges tile the LOD ids.
    std::vector<u32> memberOf(total, kDagNoGroup);
    std::vector<u32> producedBy(total, kDagNoGroup);
    u32 memberCursor = 0;
    u32 childCursor = leaves;
    for (u32 g = 0; g < dag.groups.size(); ++g) {
        const DagGroup& grp = dag.groups[g];
        if (grp.reserved != 0u) {
            return bad("group reserved field set");
        }
        if (!bounds_ok(grp.bounds)) {
            return bad("group bounds non-finite or negative");
        }
        if (grp.member_count == 0u || grp.member_offset != memberCursor ||
            static_cast<u64>(grp.member_offset) + grp.member_count > total) {
            return bad("group member ranges do not tile DGMB");
        }
        memberCursor += grp.member_count;
        if (grp.submesh >= base.submeshes.size()) {
            return bad("group submesh out of range");
        }
        const bool terminal = grp.bounds.error == kDagErrorTerminal;
        if (terminal != (grp.child_count == 0u)) {
            return bad("terminal group must have no children and error kDagErrorTerminal (and vice versa)");
        }
        if (grp.child_count == 0u) {
            if (grp.child_offset != 0u) {
                return bad("terminal group child_offset must be 0");
            }
        } else {
            if (grp.child_offset != childCursor || static_cast<u64>(grp.child_offset) + grp.child_count > total) {
                return bad("group child ranges do not tile the LOD clusters");
            }
            childCursor += grp.child_count;
            for (u32 c = grp.child_offset; c < grp.child_offset + grp.child_count; ++c) {
                producedBy[c] = g;
                if (dag.lod_clusters[c - leaves].submesh != grp.submesh) {
                    return bad("child cluster submesh differs from its group");
                }
            }
        }
        for (u32 i = grp.member_offset; i < grp.member_offset + grp.member_count; ++i) {
            const u32 c = dag.group_members[i];
            if (c >= total || memberOf[c] != kDagNoGroup) {
                return bad("cluster listed in more than one group (or id out of range)");
            }
            memberOf[c] = g;
            const u32 sub = c < leaves ? base.meshlets[c].submesh : dag.lod_clusters[c - leaves].submesh;
            if (sub != grp.submesh) {
                return bad("member cluster submesh differs from its group");
            }
        }
    }
    if (childCursor != total) {
        return bad("LOD clusters not produced by any group");
    }
    // Links, levels, monotonicity.
    u32 maxLevel = 0;
    for (u32 c = 0; c < total; ++c) {
        const DagClusterLink& link = dag.links[c];
        if (memberOf[c] == kDagNoGroup) {
            return bad("cluster is in no group");
        }
        if (link.group != memberOf[c] || link.refined != producedBy[c]) {
            return bad("cluster link group / refined disagree with the group table");
        }
        const DagGroup& grp = dag.groups[link.group];
        if (!same_bounds(link.parent, grp.bounds)) {
            return bad("cluster parent bounds differ from its group's bounds");
        }
        u32 level = 0;
        if (c < leaves) {
            const MeshletRecord& r = base.meshlets[c];
            DagLodBounds leaf{};
            for (u32 a = 0; a < 3u; ++a) {
                leaf.center[a] = r.center[a];
            }
            leaf.radius = r.radius;
            leaf.error = 0.f;
            if (!same_bounds(link.self, leaf)) {
                return bad("leaf self bounds must be the meshlet sphere with error 0");
            }
        } else {
            const DagGroup& producer = dag.groups[link.refined];
            if (!same_bounds(link.self, producer.bounds)) {
                return bad("cluster self bounds differ from its producer group's bounds");
            }
            level = producer.depth + 1u;
            if (producer.depth >= grp.depth) {
                return bad("producer group is not below the consuming group");
            }
        }
        if (level > grp.depth) {
            return bad("cluster level above its group's depth");
        }
        maxLevel = std::max(maxLevel, level);
        if (grp.bounds.error != kDagErrorTerminal) {
            if (grp.bounds.error < link.self.error) {
                return bad("error not monotonic: group error below a member's error");
            }
            if (!sphere_contains(grp.bounds, link.self)) {
                return bad("bounds not monotonic: group sphere does not contain a member's LOD sphere");
            }
        }
    }
    if (dag.level_count != (total == 0u ? 0u : maxLevel + 1u)) {
        return bad("level_count differs from 1 + the highest cluster level");
    }
    return true;
}

bool cluster_dag_mesh_equal(const ClusterDagMesh& a, const ClusterDagMesh& b) {
    auto same = [](const auto& x, const auto& y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return meshlet_mesh_equal(a.base, b.base) && a.dag.leaf_cluster_count == b.dag.leaf_cluster_count &&
           a.dag.level_count == b.dag.level_count && same(a.dag.lod_clusters, b.dag.lod_clusters) &&
           same(a.dag.lod_meshlet_vertices, b.dag.lod_meshlet_vertices) &&
           same(a.dag.lod_meshlet_triangles, b.dag.lod_meshlet_triangles) && same(a.dag.groups, b.dag.groups) &&
           same(a.dag.group_members, b.dag.group_members) && same(a.dag.links, b.dag.links);
}

std::vector<u8> serialize_cluster_dag_mesh(const ClusterDagMesh& mesh) {
    MeshletMesh base = mesh.base;
    base.version_minor = std::max<u16>(base.version_minor, kClusterDagFormatVersionMinor);
    const std::vector<u8> bytes = serialize_meshlet_mesh(base);
    FmltFile file;
    (void)split_fmlt(bytes.data(), bytes.size(), file);
    const ClusterDag& d = mesh.dag;

    FmltChunk dagh = make_chunk(cDagh, 1u);
    {
        Writer w{dagh.payload};
        w.u32v(d.leaf_cluster_count);
        w.u32v(static_cast<u32>(d.lod_clusters.size()));
        w.u32v(static_cast<u32>(d.groups.size()));
        w.u32v(d.level_count);
        w.u32v(static_cast<u32>(d.lod_meshlet_vertices.size()));
        w.u32v(static_cast<u32>(d.lod_meshlet_triangles.size()));
        w.u32v(0u);
        w.u32v(0u);
    }
    FmltChunk dmsh = make_chunk(cDmsh, static_cast<u32>(d.lod_clusters.size()));
    {
        Writer w{dmsh.payload};
        for (const MeshletRecord& r : d.lod_clusters) {
            write_record(w, r);
        }
    }
    FmltChunk dmvr = make_chunk(cDmvr, static_cast<u32>(d.lod_meshlet_vertices.size()));
    {
        Writer w{dmvr.payload};
        for (u32 v : d.lod_meshlet_vertices) {
            w.u32v(v);
        }
    }
    FmltChunk dmtr = make_chunk(cDmtr, static_cast<u32>(d.lod_meshlet_triangles.size()));
    {
        Writer w{dmtr.payload};
        for (u32 v : d.lod_meshlet_triangles) {
            w.u32v(v);
        }
    }
    FmltChunk dgrp = make_chunk(cDgrp, static_cast<u32>(d.groups.size()));
    {
        Writer w{dgrp.payload};
        for (const DagGroup& g : d.groups) {
            w.bounds(g.bounds);
            w.u32v(g.member_offset);
            w.u32v(g.member_count);
            w.u32v(g.child_offset);
            w.u32v(g.child_count);
            w.u32v(g.depth);
            w.u32v(g.submesh);
            w.u32v(g.reserved);
        }
    }
    FmltChunk dgmb = make_chunk(cDgmb, static_cast<u32>(d.group_members.size()));
    {
        Writer w{dgmb.payload};
        for (u32 v : d.group_members) {
            w.u32v(v);
        }
    }
    FmltChunk dclk = make_chunk(cDclk, static_cast<u32>(d.links.size()));
    {
        Writer w{dclk.payload};
        for (const DagClusterLink& l : d.links) {
            w.bounds(l.self);
            w.bounds(l.parent);
            w.u32v(l.group);
            w.u32v(l.refined);
        }
    }
    for (FmltChunk* c : {&dagh, &dmsh, &dmvr, &dmtr, &dgrp, &dgmb, &dclk}) {
        file.chunks.push_back(std::move(*c));
    }
    return assemble_fmlt(file);
}

bool parse_cluster_dag_mesh(const u8* data, usize size, ClusterDagMesh& out, std::string* error, MeshletFormatError* code) {
    out = ClusterDagMesh{};
    if (code != nullptr) {
        *code = MeshletFormatError::None;
    }
    // The 1.x reader validates header, table layout, checksum and the full-detail mesh, and skips
    // every chunk it does not know (ours included).
    if (!parse_meshlet_mesh(data, size, out.base, error, code)) {
        out = ClusterDagMesh{};
        return false;
    }
    FmltFile file;
    if (!split_fmlt(data, size, file, error)) {
        return reject(out, error, code, MeshletFormatError::BadChunkTable, "chunk table unreadable");
    }
    s32 index[cDagChunkKinds];
    u32 present = 0;
    for (u32 k = 0; k < cDagChunkKinds; ++k) {
        index[k] = -1;
        for (usize i = 0; i < file.chunks.size(); ++i) {
            if (file.chunks[i].fourcc != kDagFourcc[k]) {
                continue;
            }
            if (index[k] >= 0) {
                return reject(out, error, code, MeshletFormatError::DuplicateChunk, "DAG chunk appears twice");
            }
            index[k] = static_cast<s32>(i);
        }
        present += index[k] >= 0 ? 1u : 0u;
    }
    if (present == 0u) {
        return reject(out, error, code, MeshletFormatError::MissingChunk, "no DAG chunks (plain FMLT 1.0 mesh)");
    }
    if (present != cDagChunkKinds) {
        return reject(out, error, code, MeshletFormatError::MissingChunk, "DAG chunk missing");
    }
    if (out.base.version_minor < kClusterDagFormatVersionMinor) {
        return reject(out, error, code, MeshletFormatError::BadHeader, "DAG chunks in a file older than minor 1");
    }
    for (u32 k = 0; k < cDagChunkKinds; ++k) {
        if (file.chunks[static_cast<usize>(index[k])].element_bytes != kDagElementBytes[k]) {
            return reject(out, error, code, MeshletFormatError::BadElementSize, "DAG chunk element size wrong");
        }
    }
    const FmltChunk& dagh = file.chunks[static_cast<usize>(index[cDagh])];
    if (dagh.element_count != 1u) {
        return reject(out, error, code, MeshletFormatError::BadElementSize, "DAGH must have one element");
    }
    Reader h{dagh.payload.data()};
    const u32 leafCount = h.u32v();
    const u32 lodCount = h.u32v();
    const u32 groupCount = h.u32v();
    const u32 levelCount = h.u32v();
    const u32 lodVertexCount = h.u32v();
    const u32 lodTriangleCount = h.u32v();
    const u32 reserved0 = h.u32v();
    const u32 reserved1 = h.u32v();
    if ((reserved0 | reserved1) != 0u) {
        return reject(out, error, code, MeshletFormatError::BadHeader, "DAGH reserved fields set");
    }
    const u64 clusterCount = static_cast<u64>(leafCount) + lodCount;
    const u64 expected[cDagChunkKinds] = {1u, lodCount, lodVertexCount, lodTriangleCount, groupCount, clusterCount, clusterCount};
    for (u32 k = 0; k < cDagChunkKinds; ++k) {
        if (file.chunks[static_cast<usize>(index[k])].element_count != expected[k]) {
            return reject(out, error, code, MeshletFormatError::BadElementSize, "DAG chunk count disagrees with DAGH");
        }
    }
    ClusterDag& d = out.dag;
    d.leaf_cluster_count = leafCount;
    d.level_count = levelCount;
    {
        Reader r{file.chunks[static_cast<usize>(index[cDmsh])].payload.data()};
        d.lod_clusters.resize(lodCount);
        for (MeshletRecord& rec : d.lod_clusters) {
            if (!read_record(r, rec)) {
                return reject(out, error, code, MeshletFormatError::Invalid, "DMSH reserved field set");
            }
        }
    }
    auto read_u32s = [&](u32 kind, std::vector<u32>& dst) {
        const FmltChunk& c = file.chunks[static_cast<usize>(index[kind])];
        Reader r{c.payload.data()};
        dst.resize(c.element_count);
        for (u32& v : dst) {
            v = r.u32v();
        }
    };
    read_u32s(cDmvr, d.lod_meshlet_vertices);
    read_u32s(cDmtr, d.lod_meshlet_triangles);
    read_u32s(cDgmb, d.group_members);
    {
        Reader r{file.chunks[static_cast<usize>(index[cDgrp])].payload.data()};
        d.groups.resize(groupCount);
        for (DagGroup& g : d.groups) {
            g.bounds = r.bounds();
            g.member_offset = r.u32v();
            g.member_count = r.u32v();
            g.child_offset = r.u32v();
            g.child_count = r.u32v();
            g.depth = r.u32v();
            g.submesh = r.u32v();
            g.reserved = r.u32v();
        }
    }
    {
        Reader r{file.chunks[static_cast<usize>(index[cDclk])].payload.data()};
        d.links.resize(static_cast<usize>(clusterCount));
        for (DagClusterLink& l : d.links) {
            l.self = r.bounds();
            l.parent = r.bounds();
            l.group = r.u32v();
            l.refined = r.u32v();
        }
    }
    std::string why;
    if (!validate_cluster_dag(out.base, d, &why)) {
        return reject(out, error, code, MeshletFormatError::Invalid, why);
    }
    return true;
}

bool write_cluster_dag_file(const std::string& path, const ClusterDagMesh& mesh, std::string* error) {
    const std::vector<u8> bytes = serialize_cluster_dag_mesh(mesh);
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return set_error(error, "fusemeshlet dag: cannot open " + path + " for writing");
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        return set_error(error, "fusemeshlet dag: write failed for " + path);
    }
    return true;
}

bool load_cluster_dag_file(const std::string& path, ClusterDagMesh& out, std::string* error, MeshletFormatError* code) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return reject(out, error, code, MeshletFormatError::Truncated, "cannot read " + path);
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_cluster_dag_mesh(bytes.data(), bytes.size(), out, error, code);
}

void evaluate_dag_cut(const ClusterDag& dag, const cut_kernel::DagView& view, std::vector<u32>& out, kernel::Backend backend) {
    out.assign(dag.links.size(), cut_kernel::kNotInCut);
    if (dag.links.empty()) {
        return;
    }
    cut_kernel::Params p{};
    p.links = kernel::make_span(dag.links.data(), static_cast<u32>(dag.links.size()));
    p.view = view;
    p.out = kernel::make_span(out.data(), static_cast<u32>(out.size()));
    (void)kernel::launch(backend, cut_kernel::make_launch(static_cast<u32>(dag.links.size())), cut_kernel::Kernel{}, p);
}

void select_dag_cut(const ClusterDag& dag, const cut_kernel::DagView& view, std::vector<u32>& cluster_ids, kernel::Backend backend) {
    std::vector<u32> flags;
    evaluate_dag_cut(dag, view, flags, backend);
    cluster_ids.clear();
    for (u32 c = 0; c < flags.size(); ++c) {
        if (flags[c] == cut_kernel::kInCut) {
            cluster_ids.push_back(c);
        }
    }
}

std::vector<DagLevelStats> dag_level_stats(const ClusterDagMesh& mesh) {
    const ClusterDag& d = mesh.dag;
    std::vector<DagLevelStats> levels(d.level_count);
    for (u32 c = 0; c < d.cluster_count() && c < d.links.size(); ++c) {
        const u32 level = cluster_level(d, c);
        if (level >= levels.size()) {
            levels.resize(level + 1u);
        }
        levels[level].clusters += 1u;
        levels[level].triangles += cluster_ref(mesh, c).record->triangle_count;
    }
    for (const DagGroup& g : d.groups) {
        if (g.depth >= levels.size()) {
            levels.resize(g.depth + 1u);
        }
        levels[g.depth].groups += 1u;
        levels[g.depth].terminal_groups += g.child_count == 0u ? 1u : 0u;
    }
    return levels;
}

} // namespace fuse::renderer::geometry::dag
