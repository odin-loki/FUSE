// WP-5.3 cluster page file: see include/fuse/renderer/geometry_streaming/cluster_page_file.hpp.
#include <fuse/renderer/geometry_streaming/cluster_page_file.hpp>

#include <fuse/renderer/geometry/meshlet_format.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace fuse::renderer::geometry_streaming {

using geometry::MeshletMesh;
using geometry::dag::ClusterDag;
using geometry::dag::ClusterDagMesh;
using geometry::dag::DagGroup;

namespace {

u64 align16(u64 v) { return (v + 15u) & ~u64{15u}; }

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

u32 spread10(u32 v) {
    v &= 0x3FFu;
    v = (v | (v << 16)) & 0x030000FFu;
    v = (v | (v << 8)) & 0x0300F00Fu;
    v = (v | (v << 4)) & 0x030C30C3u;
    v = (v | (v << 2)) & 0x09249249u;
    return v;
}

/// Payload size of a page with the given counts (sections 16-aligned).
u64 payload_bytes(u64 clusters, u64 refs, u64 triangles, u64 vertices) {
    return sizeof(PagePayloadHeader) + clusters * sizeof(StreamCluster) + align16(refs * 4u) + align16(triangles * 4u) +
           align16(vertices * 8u) + align16(vertices * 4u);
}

struct PageBuilder {
    std::vector<u32> groups;
    std::unordered_map<u32, u32> local; // mesh vertex -> page vertex
    u64 clusters = 0, refs = 0, triangles = 0;

    void clear() {
        groups.clear();
        local.clear();
        clusters = refs = triangles = 0;
    }
};

} // namespace

u64 cluster_links_hash(const ClusterDag& dag) {
    return geometry::meshlet_fnv1a64(reinterpret_cast<const u8*>(dag.links.data()), dag.links.size() * sizeof(dag.links[0]));
}

bool build_cluster_pages(const ClusterDagMesh& mesh, const PageBuildOptions& options, ClusterPageFile& out, std::string* error) {
    out = ClusterPageFile{};
    const ClusterDag& dag = mesh.dag;
    const MeshletMesh& base = mesh.base;
    if (options.page_bytes < 1024u || options.page_bytes % 16u != 0u) {
        return fail(error, "page_bytes must be a multiple of 16 and >= 1024");
    }
    const u32 clusterCount = dag.cluster_count();
    const u32 groupCount = static_cast<u32>(dag.groups.size());
    if (groupCount == 0u || dag.links.size() != clusterCount || dag.leaf_cluster_count != base.meshlets.size()) {
        return fail(error, "not a cluster DAG mesh (no groups / link count mismatch)");
    }
    // 1. Group order: terminal groups, then by depth descending; each class by submesh, then Morton code.
    f32 lo[3] = {0.f, 0.f, 0.f}, hi[3] = {0.f, 0.f, 0.f};
    for (u32 g = 0; g < groupCount; ++g) {
        for (u32 a = 0; a < 3u; ++a) {
            const f32 c = dag.groups[g].bounds.center[a];
            lo[a] = g == 0u ? c : std::min(lo[a], c);
            hi[a] = g == 0u ? c : std::max(hi[a], c);
        }
    }
    std::vector<u32> morton(groupCount);
    for (u32 g = 0; g < groupCount; ++g) {
        u32 code = 0;
        for (u32 a = 0; a < 3u; ++a) {
            const f32 extent = hi[a] - lo[a];
            const f32 t = extent > 0.f ? (dag.groups[g].bounds.center[a] - lo[a]) / extent : 0.f;
            const u32 q = static_cast<u32>(std::clamp(t, 0.f, 1.f) * 1023.f + 0.5f);
            code |= spread10(q) << a;
        }
        morton[g] = code;
    }
    std::vector<u32> order(groupCount);
    for (u32 g = 0; g < groupCount; ++g) {
        order[g] = g;
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const DagGroup& ga = dag.groups[a];
        const DagGroup& gb = dag.groups[b];
        const bool ta = ga.child_count == 0u, tb = gb.child_count == 0u;
        if (ta != tb) {
            return ta;
        }
        if (!ta && ga.depth != gb.depth) {
            return ga.depth > gb.depth;
        }
        if (ga.submesh != gb.submesh) {
            return ga.submesh < gb.submesh;
        }
        if (morton[a] != morton[b]) {
            return morton[a] < morton[b];
        }
        return a < b;
    });

    // 2. Greedy packing.
    out.leaf_cluster_count = dag.leaf_cluster_count;
    out.cluster_count = clusterCount;
    out.group_count = groupCount;
    out.page_bytes = options.page_bytes;
    out.links_hash = cluster_links_hash(dag);
    out.quant = base.quant;
    out.group_page.assign(groupCount, kPageNone);
    std::vector<std::vector<u32>> pageGroups;
    PageBuilder page;
    bool pageTerminal = false;
    std::vector<u32> fresh;
    auto close = [&]() {
        if (!page.groups.empty()) {
            pageGroups.push_back(page.groups);
        }
        page.clear();
    };
    for (u32 g : order) {
        const DagGroup& grp = dag.groups[g];
        const bool terminal = grp.child_count == 0u;
        u64 clusters = 0, refs = 0, tris = 0;
        fresh.clear();
        for (u32 m = 0; m < grp.member_count; ++m) {
            const geometry::dag::ClusterRef ref = geometry::dag::cluster_ref(mesh, dag.group_members[grp.member_offset + m]);
            ++clusters;
            refs += ref.record->vertex_count;
            tris += ref.record->triangle_count;
            for (u32 v = 0; v < ref.record->vertex_count; ++v) {
                fresh.push_back(ref.vertices[v]);
            }
        }
        std::sort(fresh.begin(), fresh.end());
        fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());
        const u64 alone = payload_bytes(clusters, refs, tris, fresh.size());
        if (alone > options.page_bytes) {
            return fail(error, "group " + std::to_string(g) + " needs " + std::to_string(alone) + " bytes > page_bytes " +
                                   std::to_string(options.page_bytes));
        }
        u64 newVerts = 0;
        for (u32 v : fresh) {
            newVerts += page.local.count(v) == 0u ? 1u : 0u;
        }
        const u64 joined = payload_bytes(page.clusters + clusters, page.refs + refs, page.triangles + tris, page.local.size() + newVerts);
        if (!page.groups.empty() && (terminal != pageTerminal || joined > options.page_bytes)) {
            close();
        }
        if (page.groups.empty()) {
            pageTerminal = terminal;
        }
        page.groups.push_back(g);
        page.clusters += clusters;
        page.refs += refs;
        page.triangles += tris;
        for (u32 v : fresh) {
            page.local.emplace(v, static_cast<u32>(page.local.size()));
        }
    }
    close();

    // 3. Page records, payloads, dependencies.
    const u32 pageCount = static_cast<u32>(pageGroups.size());
    for (u32 p = 0; p < pageCount; ++p) {
        for (u32 g : pageGroups[p]) {
            out.group_page[g] = p;
        }
    }
    out.pages.resize(pageCount);
    std::vector<u32> deps;
    std::unordered_map<u32, u32> local;
    std::vector<u32> localVerts;
    for (u32 p = 0; p < pageCount; ++p) {
        ClusterPageEntry& e = out.pages[p];
        const bool terminal = dag.groups[pageGroups[p][0]].child_count == 0u;
        e.flags = terminal ? kPageFlagCoarse : 0u;
        if (terminal) {
            out.coarse_page_count = p + 1u;
        }
        e.group_offset = static_cast<u32>(out.page_groups.size());
        e.group_count = static_cast<u32>(pageGroups[p].size());
        e.min_depth = 0xFFFFFFFFu;
        e.max_depth = 0u;
        deps.clear();
        local.clear();
        localVerts.clear();
        std::vector<StreamCluster> clusters;
        std::vector<u32> refs, tris;
        for (u32 g : pageGroups[p]) {
            out.page_groups.push_back(g);
            const DagGroup& grp = dag.groups[g];
            e.min_depth = std::min(e.min_depth, grp.depth);
            e.max_depth = std::max(e.max_depth, grp.depth);
            for (u32 c = 0; c < grp.child_count; ++c) {
                const u32 q = out.group_page[dag.links[grp.child_offset + c].group];
                if (q != p) {
                    deps.push_back(q);
                }
            }
            for (u32 m = 0; m < grp.member_count; ++m) {
                const u32 id = dag.group_members[grp.member_offset + m];
                const geometry::dag::ClusterRef ref = geometry::dag::cluster_ref(mesh, id);
                StreamCluster sc{};
                sc.record = *ref.record;
                sc.record.vertex_offset = static_cast<u32>(refs.size());
                sc.record.triangle_offset = static_cast<u32>(tris.size());
                sc.cluster = id;
                clusters.push_back(sc);
                for (u32 v = 0; v < ref.record->vertex_count; ++v) {
                    const u32 mv = ref.vertices[v];
                    auto it = local.find(mv);
                    if (it == local.end()) {
                        it = local.emplace(mv, static_cast<u32>(localVerts.size())).first;
                        localVerts.push_back(mv);
                    }
                    refs.push_back(it->second);
                }
                tris.insert(tris.end(), ref.triangles, ref.triangles + ref.record->triangle_count);
            }
        }
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        for (u32 q : deps) {
            if (q >= p) {
                return fail(error, "internal: dependency on a later page (layout order broken)");
            }
        }
        e.dep_offset = static_cast<u32>(out.page_deps.size());
        e.dep_count = static_cast<u32>(deps.size());
        out.page_deps.insert(out.page_deps.end(), deps.begin(), deps.end());
        e.cluster_count = static_cast<u32>(clusters.size());

        PagePayloadHeader h{};
        h.page = p;
        h.cluster_count = e.cluster_count;
        h.vertex_count = static_cast<u32>(localVerts.size());
        h.ref_count = static_cast<u32>(refs.size());
        h.triangle_count = static_cast<u32>(tris.size());
        u64 cursor = sizeof(PagePayloadHeader);
        h.clusters_offset = static_cast<u32>(cursor);
        cursor += clusters.size() * sizeof(StreamCluster);
        h.refs_offset = static_cast<u32>(cursor);
        cursor = align16(cursor + refs.size() * 4u);
        h.triangles_offset = static_cast<u32>(cursor);
        cursor = align16(cursor + tris.size() * 4u);
        h.positions_offset = static_cast<u32>(cursor);
        cursor = align16(cursor + localVerts.size() * 8u);
        h.source_offset = static_cast<u32>(cursor);
        cursor = align16(cursor + localVerts.size() * 4u);
        h.total_bytes = static_cast<u32>(cursor);
        if (cursor > options.page_bytes) {
            return fail(error, "internal: page payload over capacity");
        }
        e.payload_offset = out.payload.size();
        e.payload_bytes = h.total_bytes;
        out.payload.resize(out.payload.size() + cursor, 0u);
        u8* dst = out.payload.data() + e.payload_offset;
        std::memcpy(dst, &h, sizeof(h));
        if (!clusters.empty()) {
            std::memcpy(dst + h.clusters_offset, clusters.data(), clusters.size() * sizeof(StreamCluster));
        }
        if (!refs.empty()) {
            std::memcpy(dst + h.refs_offset, refs.data(), refs.size() * 4u);
        }
        if (!tris.empty()) {
            std::memcpy(dst + h.triangles_offset, tris.data(), tris.size() * 4u);
        }
        for (u32 v = 0; v < localVerts.size(); ++v) {
            std::memcpy(dst + h.positions_offset + v * 8u, &base.positions[static_cast<usize>(localVerts[v]) * 4u], 8u);
            std::memcpy(dst + h.source_offset + v * 4u, &localVerts[v], 4u);
        }
    }
    std::string why;
    if (!validate_cluster_page_layout(out, &why)) {
        return fail(error, "internal: built layout invalid: " + why);
    }
    return true;
}

bool view_page(const u8* data, usize bytes, PageView& out) {
    out = PageView{};
    if (data == nullptr || bytes < sizeof(PagePayloadHeader) || reinterpret_cast<uintptr_t>(data) % 16u != 0u) {
        return false;
    }
    const auto* h = reinterpret_cast<const PagePayloadHeader*>(data);
    if (h->magic != kPagePayloadMagic || h->total_bytes > bytes || h->clusters_offset != sizeof(PagePayloadHeader)) {
        return false;
    }
    const u64 clustersEnd = h->clusters_offset + static_cast<u64>(h->cluster_count) * sizeof(StreamCluster);
    const u64 refsEnd = h->refs_offset + static_cast<u64>(h->ref_count) * 4u;
    const u64 trisEnd = h->triangles_offset + static_cast<u64>(h->triangle_count) * 4u;
    const u64 posEnd = h->positions_offset + static_cast<u64>(h->vertex_count) * 8u;
    const u64 srcEnd = h->source_offset + static_cast<u64>(h->vertex_count) * 4u;
    if (h->refs_offset != clustersEnd || h->triangles_offset != align16(refsEnd) || h->positions_offset != align16(trisEnd) ||
        h->source_offset != align16(posEnd) || h->total_bytes != align16(srcEnd)) {
        return false;
    }
    out.header = h;
    out.clusters = reinterpret_cast<const StreamCluster*>(data + h->clusters_offset);
    out.refs = reinterpret_cast<const u32*>(data + h->refs_offset);
    out.triangles = reinterpret_cast<const u32*>(data + h->triangles_offset);
    out.positions = reinterpret_cast<const u16*>(data + h->positions_offset);
    out.source_vertices = reinterpret_cast<const u32*>(data + h->source_offset);
    return true;
}

bool validate_cluster_page_layout(const ClusterPageFile& f, std::string* error) {
    const u32 pageCount = f.page_count();
    if (pageCount == 0u || f.group_count == 0u || f.cluster_count < f.leaf_cluster_count || f.page_bytes < 1024u ||
        f.page_bytes % 16u != 0u) {
        return fail(error, "empty file or bad counts");
    }
    if (f.page_groups.size() != f.group_count || f.group_page.size() != f.group_count || f.coarse_page_count == 0u ||
        f.coarse_page_count > pageCount) {
        return fail(error, "group tables / coarse page count");
    }
    std::vector<u8> groupSeen(f.group_count, 0u);
    std::vector<u8> clusterSeen(f.cluster_count, 0u);
    u64 groupCursor = 0, depCursor = 0, payloadCursor = 0;
    u64 clusters = 0;
    for (u32 p = 0; p < pageCount; ++p) {
        const ClusterPageEntry& e = f.pages[p];
        const std::string at = "page " + std::to_string(p) + ": ";
        const bool coarse = p < f.coarse_page_count;
        if (e.group_offset != groupCursor || e.group_count == 0u || e.group_offset + static_cast<u64>(e.group_count) > f.group_count) {
            return fail(error, at + "group range does not tile");
        }
        groupCursor += e.group_count;
        if (e.dep_offset != depCursor || e.dep_offset + static_cast<u64>(e.dep_count) > f.page_deps.size()) {
            return fail(error, at + "dependency range does not tile");
        }
        depCursor += e.dep_count;
        if (e.flags != (coarse ? kPageFlagCoarse : 0u) || (coarse && e.dep_count != 0u) || e.reserved != 0u ||
            e.min_depth > e.max_depth) {
            return fail(error, at + "flags / coarse dependencies / depth range");
        }
        for (u32 i = 0; i < e.dep_count; ++i) {
            const u32 q = f.page_deps[e.dep_offset + i];
            if (q >= p || (i > 0u && q <= f.page_deps[e.dep_offset + i - 1u])) {
                return fail(error, at + "dependencies must be earlier pages, ascending, unique");
            }
        }
        for (u32 i = 0; i < e.group_count; ++i) {
            const u32 g = f.page_groups[e.group_offset + i];
            if (g >= f.group_count || groupSeen[g] != 0u || f.group_page[g] != p) {
                return fail(error, at + "group listed twice / out of range / group_page mismatch");
            }
            groupSeen[g] = 1u;
        }
        if (e.payload_offset != payloadCursor || e.payload_offset % 16u != 0u || e.payload_bytes > f.page_bytes ||
            e.payload_offset + e.payload_bytes > f.payload.size()) {
            return fail(error, at + "payload range");
        }
        payloadCursor = align16(e.payload_offset + e.payload_bytes);
        PageView v{};
        if (!view_page(f.payload.data() + e.payload_offset, e.payload_bytes, v) || v.header->page != p ||
            v.header->cluster_count != e.cluster_count || v.header->total_bytes != e.payload_bytes) {
            return fail(error, at + "payload header");
        }
        for (u32 c = 0; c < e.cluster_count; ++c) {
            const StreamCluster& sc = v.clusters[c];
            const MeshletRecord& r = sc.record;
            if (sc.cluster >= f.cluster_count || clusterSeen[sc.cluster] != 0u || r.vertex_count == 0u || r.triangle_count == 0u ||
                r.vertex_offset + static_cast<u64>(r.vertex_count) > v.header->ref_count ||
                r.triangle_offset + static_cast<u64>(r.triangle_count) > v.header->triangle_count) {
                return fail(error, at + "cluster record " + std::to_string(c));
            }
            clusterSeen[sc.cluster] = 1u;
            for (u32 k = 0; k < r.vertex_count; ++k) {
                if (v.refs[r.vertex_offset + k] >= v.header->vertex_count) {
                    return fail(error, at + "vertex ref out of range");
                }
            }
            for (u32 t = 0; t < r.triangle_count; ++t) {
                const u32 packed = v.triangles[r.triangle_offset + t];
                if ((packed >> 24) != 0u || geometry::triangle_index(packed, 0) >= r.vertex_count ||
                    geometry::triangle_index(packed, 1) >= r.vertex_count || geometry::triangle_index(packed, 2) >= r.vertex_count) {
                    return fail(error, at + "micro-index out of range");
                }
            }
        }
        clusters += e.cluster_count;
    }
    if (groupCursor != f.group_count || depCursor != f.page_deps.size() || clusters != f.cluster_count ||
        payloadCursor != f.payload.size()) {
        return fail(error, "tables do not tile (groups / dependencies / clusters / payload)");
    }
    return true;
}

bool validate_cluster_page_file(const ClusterPageFile& f, const ClusterDagMesh& mesh, std::string* error) {
    if (!validate_cluster_page_layout(f, error)) {
        return false;
    }
    const ClusterDag& dag = mesh.dag;
    if (f.cluster_count != dag.cluster_count() || f.group_count != dag.groups.size() ||
        f.leaf_cluster_count != dag.leaf_cluster_count || f.links_hash != cluster_links_hash(dag) ||
        std::memcmp(&f.quant, &mesh.base.quant, sizeof(f.quant)) != 0) {
        return fail(error, "file does not belong to this DAG (counts / links hash / quantisation)");
    }
    for (u32 p = 0; p < f.page_count(); ++p) {
        const ClusterPageEntry& e = f.pages[p];
        const std::string at = "page " + std::to_string(p) + ": ";
        std::vector<u32> deps;
        u32 expectedClusters = 0;
        bool terminal = false, nonTerminal = false;
        for (u32 i = 0; i < e.group_count; ++i) {
            const DagGroup& g = dag.groups[f.page_groups[e.group_offset + i]];
            terminal = terminal || g.child_count == 0u;
            nonTerminal = nonTerminal || g.child_count != 0u;
            expectedClusters += g.member_count;
            for (u32 c = 0; c < g.child_count; ++c) {
                const u32 q = f.group_page[dag.links[g.child_offset + c].group];
                if (q != p) {
                    deps.push_back(q);
                }
            }
        }
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        if (deps.size() != e.dep_count || !std::equal(deps.begin(), deps.end(), f.page_deps.begin() + e.dep_offset) ||
            (terminal && nonTerminal) || terminal != ((e.flags & kPageFlagCoarse) != 0u) || expectedClusters != e.cluster_count) {
            return fail(error, at + "dependencies / coarse flag / cluster count differ from the DAG");
        }
        PageView v{};
        view_page(f.page_payload(p), e.payload_bytes, v);
        for (u32 c = 0; c < e.cluster_count; ++c) {
            const StreamCluster& sc = v.clusters[c];
            if (f.group_page[dag.links[sc.cluster].group] != p) {
                return fail(error, at + "cluster stored outside its group's page");
            }
            const geometry::dag::ClusterRef ref = geometry::dag::cluster_ref(mesh, sc.cluster);
            MeshletRecord r = sc.record;
            r.vertex_offset = ref.record->vertex_offset;
            r.triangle_offset = ref.record->triangle_offset;
            if (std::memcmp(&r, ref.record, sizeof(r)) != 0 ||
                std::memcmp(v.triangles + sc.record.triangle_offset, ref.triangles, ref.record->triangle_count * 4u) != 0) {
                return fail(error, at + "cluster record / triangles differ from the DAG");
            }
            for (u32 k = 0; k < r.vertex_count; ++k) {
                const u32 local = v.refs[sc.record.vertex_offset + k];
                const u32 mv = ref.vertices[k];
                if (v.source_vertices[local] != mv ||
                    std::memcmp(v.positions + static_cast<usize>(local) * 4u, &mesh.base.positions[static_cast<usize>(mv) * 4u], 8u) != 0) {
                    return fail(error, at + "vertex data differs from the DAG mesh");
                }
            }
        }
    }
    return true;
}

// --- serialisation -----------------------------------------------------------------------------------
namespace {

struct FileHeader {
    u32 magic;
    u16 major;
    u16 minor;
    u32 header_bytes;
    u32 page_count;
    u32 group_count;
    u32 cluster_count;
    u32 leaf_cluster_count;
    u32 coarse_page_count;
    u32 dep_count;
    u32 page_bytes;
    u64 links_hash;
    s32 exponent[3];
    f32 offset[3];
    f32 step[3];
    u32 pad0;
    u64 table_offset;
    u64 payload_offset;
    u64 payload_bytes;
    u32 reserved[4];
};
static_assert(sizeof(FileHeader) == kClusterPageHeaderBytes, "FCPG header layout");

template <typename T>
void put(std::vector<u8>& out, const T* data, usize count) {
    const usize at = out.size();
    out.resize(at + count * sizeof(T));
    if (count > 0u) {
        std::memcpy(out.data() + at, data, count * sizeof(T));
    }
}

} // namespace

std::vector<u8> serialize_cluster_page_file(const ClusterPageFile& f) {
    FileHeader h{};
    h.magic = kClusterPageMagic;
    h.major = kClusterPageVersionMajor;
    h.minor = kClusterPageVersionMinor;
    h.header_bytes = kClusterPageHeaderBytes;
    h.page_count = f.page_count();
    h.group_count = f.group_count;
    h.cluster_count = f.cluster_count;
    h.leaf_cluster_count = f.leaf_cluster_count;
    h.coarse_page_count = f.coarse_page_count;
    h.dep_count = static_cast<u32>(f.page_deps.size());
    h.page_bytes = f.page_bytes;
    h.links_hash = f.links_hash;
    for (u32 a = 0; a < 3u; ++a) {
        h.exponent[a] = f.quant.exponent[a];
        h.offset[a] = f.quant.offset[a];
        h.step[a] = f.quant.step[a];
    }
    h.table_offset = kClusterPageHeaderBytes;
    const u64 tables = static_cast<u64>(h.page_count) * sizeof(ClusterPageEntry) + f.page_groups.size() * 4u + f.page_deps.size() * 4u +
                       f.group_page.size() * 4u;
    h.payload_offset = align16(h.table_offset + tables);
    h.payload_bytes = f.payload.size();
    std::vector<u8> out;
    out.reserve(static_cast<usize>(h.payload_offset + h.payload_bytes + 8u));
    put(out, &h, 1u);
    put(out, f.pages.data(), f.pages.size());
    put(out, f.page_groups.data(), f.page_groups.size());
    put(out, f.page_deps.data(), f.page_deps.size());
    put(out, f.group_page.data(), f.group_page.size());
    out.resize(static_cast<usize>(h.payload_offset), 0u);
    put(out, f.payload.data(), f.payload.size());
    const u64 hash = geometry::meshlet_fnv1a64(out.data(), out.size());
    put(out, &hash, 1u);
    return out;
}

bool parse_cluster_page_file(const u8* data, usize size, ClusterPageFile& out, std::string* error) {
    out = ClusterPageFile{};
    if (data == nullptr || size < kClusterPageHeaderBytes + 8u) {
        return fail(error, "truncated");
    }
    FileHeader h{};
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kClusterPageMagic) {
        return fail(error, "bad magic");
    }
    if (h.major != kClusterPageVersionMajor) {
        return fail(error, "unsupported version");
    }
    if (h.header_bytes != kClusterPageHeaderBytes || h.pad0 != 0u || h.reserved[0] != 0u || h.reserved[1] != 0u ||
        h.reserved[2] != 0u || h.reserved[3] != 0u || h.table_offset != kClusterPageHeaderBytes) {
        return fail(error, "bad header");
    }
    const u64 tables = static_cast<u64>(h.page_count) * sizeof(ClusterPageEntry) + static_cast<u64>(h.group_count) * 8u +
                       static_cast<u64>(h.dep_count) * 4u;
    if (h.payload_offset != align16(h.table_offset + tables) || h.payload_offset > size ||
        h.payload_bytes > size - h.payload_offset || h.payload_offset + h.payload_bytes + 8u != size) {
        return fail(error, "truncated or bad section sizes");
    }
    u64 stored = 0;
    std::memcpy(&stored, data + size - 8u, 8u);
    if (stored != geometry::meshlet_fnv1a64(data, size - 8u)) {
        return fail(error, "checksum mismatch");
    }
    out.leaf_cluster_count = h.leaf_cluster_count;
    out.cluster_count = h.cluster_count;
    out.group_count = h.group_count;
    out.page_bytes = h.page_bytes;
    out.coarse_page_count = h.coarse_page_count;
    out.links_hash = h.links_hash;
    for (u32 a = 0; a < 3u; ++a) {
        out.quant.exponent[a] = h.exponent[a];
        out.quant.offset[a] = h.offset[a];
        out.quant.step[a] = h.step[a];
    }
    const u8* cursor = data + h.table_offset;
    out.pages.resize(h.page_count);
    std::memcpy(out.pages.data(), cursor, out.pages.size() * sizeof(ClusterPageEntry));
    cursor += out.pages.size() * sizeof(ClusterPageEntry);
    out.page_groups.resize(h.group_count);
    std::memcpy(out.page_groups.data(), cursor, out.page_groups.size() * 4u);
    cursor += out.page_groups.size() * 4u;
    out.page_deps.resize(h.dep_count);
    std::memcpy(out.page_deps.data(), cursor, out.page_deps.size() * 4u);
    cursor += out.page_deps.size() * 4u;
    out.group_page.resize(h.group_count);
    std::memcpy(out.group_page.data(), cursor, out.group_page.size() * 4u);
    out.payload.assign(data + h.payload_offset, data + h.payload_offset + h.payload_bytes);
    std::string why;
    if (!validate_cluster_page_layout(out, &why)) {
        out = ClusterPageFile{};
        return fail(error, "invalid layout: " + why);
    }
    return true;
}

bool write_cluster_page_file(const std::string& path, const ClusterPageFile& file, std::string* error) {
    const std::vector<u8> bytes = serialize_cluster_page_file(file);
    std::ofstream s(path, std::ios::binary | std::ios::trunc);
    if (!s || !s.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return fail(error, "cannot write " + path);
    }
    return true;
}

bool load_cluster_page_file(const std::string& path, ClusterPageFile& out, std::string* error) {
    std::ifstream s(path, std::ios::binary);
    if (!s) {
        return fail(error, "cannot open " + path);
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
    return parse_cluster_page_file(bytes.data(), bytes.size(), out, error);
}

bool cluster_page_file_equal(const ClusterPageFile& a, const ClusterPageFile& b) {
    return a.leaf_cluster_count == b.leaf_cluster_count && a.cluster_count == b.cluster_count && a.group_count == b.group_count &&
           a.page_bytes == b.page_bytes && a.coarse_page_count == b.coarse_page_count && a.links_hash == b.links_hash &&
           std::memcmp(&a.quant, &b.quant, sizeof(a.quant)) == 0 && a.pages.size() == b.pages.size() &&
           (a.pages.empty() || std::memcmp(a.pages.data(), b.pages.data(), a.pages.size() * sizeof(ClusterPageEntry)) == 0) &&
           a.page_groups == b.page_groups && a.page_deps == b.page_deps && a.group_page == b.group_page && a.payload == b.payload;
}

void build_stream_cluster_info(const ClusterDag& dag, const ClusterPageFile& file, std::vector<StreamClusterInfo>& out) {
    out.assign(dag.links.size(), StreamClusterInfo{});
    for (usize c = 0; c < dag.links.size(); ++c) {
        const geometry::dag::DagClusterLink& l = dag.links[c];
        out[c].member_page = l.group < file.group_page.size() ? file.group_page[l.group] : kPageNone;
        out[c].producer_page =
            l.refined != geometry::dag::kDagNoGroup && l.refined < file.group_page.size() ? file.group_page[l.refined] : kPageNone;
    }
}

} // namespace fuse::renderer::geometry_streaming
