// UNI-U7-ASSET-1: runtime FMSH (`.fusemesh`) reader, moved from Tools/FUSE/Cook/src/mesh_cook.cpp and
// mesh_cook_meshopt.cpp (asset plan W0.1 / W0.2) so the runtime reads cooked meshes without the cook's
// importers and encoders. Behaviour is unchanged: the cook's FMSH gates (fuse_asset_fmsh_v2,
// fuse_asset_mesh_*) now exercise this code through the fuse::cook re-exports.

#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/asset/detail/fmsh_layout.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#if defined(FUSE_ASSET_HAS_MESHOPTIMIZER)
#include <meshoptimizer.h>
#endif

namespace fuse::asset {

bool mesh_codec_available() {
#if defined(FUSE_ASSET_HAS_MESHOPTIMIZER)
    return true;
#else
    return false;
#endif
}

using namespace detail; // NOLINT(google-build-using-namespace): FMSH layout constants

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

u32 get_u32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) | (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

u64 get_u64(const u8* data) {
    return static_cast<u64>(get_u32(data)) | (static_cast<u64>(get_u32(data + 4)) << 32);
}

f32 get_f32(const u8* data) {
    const u32 bits = get_u32(data);
    f32 value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}


u32 get_u16(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8);
}


f32 from_snorm16(u32 bits) {
    const s32 v = static_cast<s32>(static_cast<std::int16_t>(static_cast<u16>(bits)));
    return (std::max)(static_cast<f32>(v) / 32767.f, -1.f);
}


void oct_decode(u32 bx, u32 by, f32* n) {
    f32 x = from_snorm16(bx);
    f32 y = from_snorm16(by);
    const f32 z = 1.f - std::fabs(x) - std::fabs(y);
    const f32 t = (std::max)(-z, 0.f);
    x += x >= 0.f ? -t : t;
    y += y >= 0.f ? -t : t;
    const f32 len = std::sqrt(x * x + y * y + z * z);
    n[0] = x / len;
    n[1] = y / len;
    n[2] = z / len;
}


struct KnownSection {
    u32 id;
    u32 element_bytes;
};
constexpr KnownSection kKnownSections[] = {
    {kSectionLodt, 16u}, {kSectionLodr, 8u},  {kSectionLodi, 4u},  {kSectionLodz, 1u},  {kSectionMlth, 16u}, {kSectionSubm, 16u},
    {kSectionMshl, 96u}, {kSectionMvrt, 4u},  {kSectionMtri, 4u},  {kSectionDagh, 32u}, {kSectionDmsh, 96u}, {kSectionDmvr, 4u},
    {kSectionDmtr, 4u},  {kSectionDgrp, 48u}, {kSectionDgmb, 4u},  {kSectionDclk, 48u},
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


std::vector<u32> read_u32s(const SectionView& view) {
    std::vector<u32> values(view.element_count);
    Reader r{view.data};
    for (u32& v : values) {
        v = r.u32v();
    }
    return values;
}


bool meshopt_decode_vertices(const u8* encoded, usize encoded_bytes, usize count, u32 element_bytes,
                             std::vector<u8>& raw) {
#if defined(FUSE_ASSET_HAS_MESHOPTIMIZER)
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


bool meshopt_decode_indices(const u8* encoded, usize encoded_bytes, usize count, std::vector<u32>& out) {
#if defined(FUSE_ASSET_HAS_MESHOPTIMIZER)
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
    const bool anyLod = has(kSectionLodt) || has(kSectionLodr) || has(kSectionLodi) || has(kSectionLodz);
    if (anyLod) {
        if (!has(kSectionLodt) || !has(kSectionLodr) || has(kSectionLodi) == has(kSectionLodz)) {
            return fail("LOD sections incomplete");
        }
        const SectionView lodt = sections[kSectionLodt];
        const SectionView lodr = sections[kSectionLodr];
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
        if (has(kSectionLodi)) {
            if (sections[kSectionLodi].element_count != total) {
                return fail("LODI count disagrees with the LOD ranges");
            }
            out.lod_indices = read_u32s(sections[kSectionLodi]);
        } else {
            const SectionView lodz = sections[kSectionLodz];
            if (!mesh_codec_available() ||
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
    const u32 meshletIds[] = {kSectionMlth, kSectionSubm, kSectionMshl, kSectionMvrt, kSectionMtri};
    const u32 meshletPresent = static_cast<u32>(std::count_if(std::begin(meshletIds), std::end(meshletIds), has));
    if (meshletPresent != 0u && meshletPresent != 5u) {
        return fail("meshlet sections incomplete");
    }
    if (meshletPresent == 5u) {
        MeshletTable& t = out.meshlets;
        const SectionView mlth = sections[kSectionMlth];
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
        const SectionView subm = sections[kSectionSubm];
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
        const SectionView mshl = sections[kSectionMshl];
        t.meshlets.resize(mshl.element_count);
        Reader rm{mshl.data};
        for (CookedMeshlet& r : t.meshlets) {
            if (!rm.record(r)) {
                return fail("MSHL reserved field set");
            }
        }
        t.vertices = read_u32s(sections[kSectionMvrt]);
        t.triangles = read_u32s(sections[kSectionMtri]);
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
    const u32 dagIds[] = {kSectionDagh, kSectionDmsh, kSectionDmvr, kSectionDmtr, kSectionDgrp, kSectionDgmb, kSectionDclk};
    const u32 dagPresent = static_cast<u32>(std::count_if(std::begin(dagIds), std::end(dagIds), has));
    if (dagPresent != 0u && dagPresent != 7u) {
        return fail("cluster DAG sections incomplete");
    }
    if (dagPresent == 7u) {
        if (out.meshlets.empty()) {
            return fail("cluster DAG without meshlets");
        }
        ClusterDagTable& d = out.cluster_dag;
        const SectionView dagh = sections[kSectionDagh];
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
            lodCount != sections[kSectionDmsh].element_count || groupCount != sections[kSectionDgrp].element_count ||
            lodVertexCount != sections[kSectionDmvr].element_count || lodTriangleCount != sections[kSectionDmtr].element_count ||
            d.level_count == 0u || groupCount == 0u) {
            return fail("DAGH disagrees with the DAG sections");
        }
        d.lod_clusters.resize(lodCount);
        Reader rm{sections[kSectionDmsh].data};
        for (CookedMeshlet& r : d.lod_clusters) {
            if (!rm.record(r)) {
                return fail("DMSH reserved field set");
            }
        }
        d.lod_vertices = read_u32s(sections[kSectionDmvr]);
        d.lod_triangles = read_u32s(sections[kSectionDmtr]);
        if (!check_clusters(d.lod_clusters, d.lod_vertices, d.lod_triangles, out.meshlets.max_vertices,
                            out.meshlets.max_triangles, vertexCount, submeshCount, "DAG cluster", error)) {
            return false;
        }
        const u32 clusterCount = d.cluster_count();
        d.groups.resize(groupCount);
        Reader rg{sections[kSectionDgrp].data};
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
        d.group_members = read_u32s(sections[kSectionDgmb]);
        d.links.resize(sections[kSectionDclk].element_count);
        Reader rl{sections[kSectionDclk].data};
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


bool deserialize_v2(const u8* data, usize size, CookedMesh& out, std::string* error) {
    auto reject = [&](const std::string& message) {
        set_error(error, message);
        out = CookedMesh{};
        return false;
    };
    if (size < kFmshHeaderBytesV2 + kFmshTrailerBytes) {
        return reject("cooked mesh truncated");
    }
    if (get_u64(data + size - kFmshTrailerBytes) != fmsh_fnv1a64(data, size - kFmshTrailerBytes)) {
        return reject("cooked mesh checksum mismatch");
    }
    const u32 flags = get_u32(data + 8);
    const u32 vertexCount = get_u32(data + 12);
    const u32 indexCount = get_u32(data + 16);
    const u32 submeshCount = get_u32(data + 20);
    const u32 streamCount = get_u32(data + 48);
    const u32 slotCount = get_u32(data + 52);
    if ((flags & ~(kFmshFlagQuantizedPositions | kFmshFlagMeshoptCodec | kFmshFlagSections)) != 0u) {
        return reject("cooked mesh flags unsupported");
    }
    const bool codec = (flags & kFmshFlagMeshoptCodec) != 0u;
    if (codec && !mesh_codec_available()) {
        return reject("cooked mesh uses the meshopt codec, which this build lacks");
    }
    if (slotCount != 0u && slotCount != submeshCount) {
        return reject("cooked mesh material slot count must be 0 or the submesh count");
    }
    const usize payloadEnd = size - kFmshTrailerBytes;
    usize cursor = 24u;
    for (f32& value : out.bounds_min) {
        value = get_f32(data + cursor);
        cursor += 4u;
    }
    for (f32& value : out.bounds_max) {
        value = get_f32(data + cursor);
        cursor += 4u;
    }
    for (u32 axis = 0; axis < 3u; ++axis) {
        if (!std::isfinite(out.bounds_min[axis]) || !std::isfinite(out.bounds_max[axis]) ||
            out.bounds_min[axis] > out.bounds_max[axis]) {
            return reject("cooked mesh bounds invalid");
        }
    }
    cursor = kFmshHeaderBytesV2;
    auto need = [&](u64 bytes) { return static_cast<u64>(cursor) + bytes <= payloadEnd; };
    if (!need(static_cast<u64>(submeshCount) * kFmshSubmeshBytes)) {
        return reject("cooked mesh size mismatch");
    }
    out.submeshes.resize(submeshCount);
    for (CookedMesh::Submesh& submesh : out.submeshes) {
        submesh.index_offset = get_u32(data + cursor);
        submesh.index_count = get_u32(data + cursor + 4);
        submesh.vertex_offset = get_u32(data + cursor + 8);
        submesh.material_index = get_u32(data + cursor + 12);
        cursor += kFmshSubmeshBytes;
        if (static_cast<u64>(submesh.index_offset) + submesh.index_count > indexCount ||
            submesh.vertex_offset > vertexCount) {
            return reject("cooked mesh submesh range out of bounds");
        }
    }
    for (u32 slot = 0; slot < slotCount; ++slot) {
        if (!need(4u)) {
            return reject("cooked mesh size mismatch");
        }
        const u32 length = get_u32(data + cursor);
        cursor += 4u;
        const u64 padded = (static_cast<u64>(length) + 3u) & ~static_cast<u64>(3u);
        if (!need(padded)) {
            return reject("cooked mesh material slot name out of bounds");
        }
        out.material_slots.emplace_back(reinterpret_cast<const char*>(data + cursor), length);
        cursor += static_cast<usize>(padded);
    }

    const usize n = vertexCount;
    u32 seen = 0;
    MeshStreamFormat weightFormat = MeshStreamFormat::Unorm16x4;
    std::vector<u32> rawWeights;
    std::vector<u8> decodedStream;
    for (u32 s = 0; s < streamCount; ++s) {
        const usize entryBytes = codec ? 16u : 12u;
        if (!need(entryBytes)) {
            return reject("cooked mesh stream table truncated");
        }
        const u32 semanticRaw = get_u32(data + cursor);
        const u32 formatRaw = get_u32(data + cursor + 4);
        const u32 byteLength = get_u32(data + cursor + 8);
        const u32 encodedBytes = codec ? get_u32(data + cursor + 12) : 0u;
        cursor += entryBytes;
        if (semanticRaw < 1u || semanticRaw > 8u) {
            return reject("cooked mesh stream semantic " + std::to_string(semanticRaw) + " unknown");
        }
        if ((seen & (1u << semanticRaw)) != 0u) {
            return reject("cooked mesh stream semantic " + std::to_string(semanticRaw) + " duplicated");
        }
        seen |= 1u << semanticRaw;
        const auto semantic = static_cast<MeshStreamSemantic>(semanticRaw);
        const auto format = static_cast<MeshStreamFormat>(formatRaw);
        if (formatRaw < 1u || formatRaw > 9u || !stream_format_allowed(semantic, format)) {
            return reject("cooked mesh stream " + std::to_string(semanticRaw) + " has unsupported format " +
                          std::to_string(formatRaw));
        }
        const u64 expected = static_cast<u64>(n) * stream_element_bytes(format);
        const u64 stored = codec ? static_cast<u64>(encodedBytes) : expected;
        const u64 padded = (stored + 3u) & ~static_cast<u64>(3u);
        if (byteLength != expected || !need(padded)) {
            return reject("cooked mesh stream " + std::to_string(semanticRaw) + " size mismatch");
        }
        const u8* p = data + cursor;
        if (codec) {
            if (!meshopt_decode_vertices(p, encodedBytes, n, stream_element_bytes(format), decodedStream)) {
                return reject("cooked mesh stream " + std::to_string(semanticRaw) + " meshopt decode failed");
            }
            p = decodedStream.data();
        }
        switch (semantic) {
        case MeshStreamSemantic::Position:
            out.positions.resize(n * 3u);
            for (usize i = 0; i < n * 3u; ++i) {
                if (format == MeshStreamFormat::F32x3) {
                    out.positions[i] = get_f32(p + i * 4u);
                } else {
                    const u32 axis = static_cast<u32>(i % 3u);
                    const f32 t = static_cast<f32>(get_u16(p + i * 2u)) / 65535.f;
                    out.positions[i] = out.bounds_min[axis] + t * (out.bounds_max[axis] - out.bounds_min[axis]);
                }
            }
            break;
        case MeshStreamSemantic::Normal:
            out.normals.resize(n * 3u);
            for (usize v = 0; v < n; ++v) {
                if (format == MeshStreamFormat::F32x3) {
                    for (u32 d = 0; d < 3u; ++d) {
                        out.normals[v * 3u + d] = get_f32(p + (v * 3u + d) * 4u);
                    }
                } else {
                    oct_decode(get_u16(p + v * 4u), get_u16(p + v * 4u + 2u), &out.normals[v * 3u]);
                }
            }
            break;
        case MeshStreamSemantic::Uv0:
        case MeshStreamSemantic::Uv1: {
            std::vector<f32>& dst = semantic == MeshStreamSemantic::Uv0 ? out.uvs : out.uv1s;
            dst.resize(n * 2u);
            for (usize i = 0; i < n * 2u; ++i) {
                dst[i] = get_f32(p + i * 4u);
            }
            break;
        }
        case MeshStreamSemantic::Tangent:
            out.tangents.resize(n * 4u);
            for (usize v = 0; v < n; ++v) {
                const u32 sign = get_u16(p + v * 8u + 4u);
                if ((sign != 1u && sign != 0xFFFFu) || get_u16(p + v * 8u + 6u) != 0u) {
                    return reject("cooked mesh tangent sign invalid");
                }
                oct_decode(get_u16(p + v * 8u), get_u16(p + v * 8u + 2u), &out.tangents[v * 4u]);
                out.tangents[v * 4u + 3u] = sign == 1u ? 1.f : -1.f;
            }
            break;
        case MeshStreamSemantic::Color0:
            out.colors.assign(p, p + n * 4u);
            break;
        case MeshStreamSemantic::Joints0:
            out.joints.resize(n * 4u);
            for (usize i = 0; i < n * 4u; ++i) {
                out.joints[i] = static_cast<u16>(format == MeshStreamFormat::Uint8x4 ? p[i] : get_u16(p + i * 2u));
            }
            break;
        case MeshStreamSemantic::Weights0:
            weightFormat = format;
            rawWeights.resize(n * 4u);
            for (usize i = 0; i < n * 4u; ++i) {
                rawWeights[i] = format == MeshStreamFormat::Unorm8x4 ? p[i] : get_u16(p + i * 2u);
            }
            break;
        }
        cursor += static_cast<usize>(padded);
    }
    if ((seen & (1u << static_cast<u32>(MeshStreamSemantic::Position))) == 0u) {
        return reject("cooked mesh has no position stream");
    }
    const bool hasJoints = (seen & (1u << static_cast<u32>(MeshStreamSemantic::Joints0))) != 0u;
    const bool hasWeights = (seen & (1u << static_cast<u32>(MeshStreamSemantic::Weights0))) != 0u;
    if (hasJoints != hasWeights) {
        return reject("cooked mesh joints and weights must both be present or both absent");
    }
    if (hasWeights) {
        const u32 scale = weightFormat == MeshStreamFormat::Unorm8x4 ? 255u : 65535u;
        out.weights.resize(n * 4u);
        for (usize v = 0; v < n; ++v) {
            u32 sum = 0;
            for (u32 k = 0; k < 4u; ++k) {
                sum += rawWeights[v * 4u + k];
                out.weights[v * 4u + k] = static_cast<f32>(rawWeights[v * 4u + k]) / static_cast<f32>(scale);
            }
            if (sum != scale) {
                return reject("cooked mesh skin weights of vertex " + std::to_string(v) + " do not sum to 1");
            }
        }
    }
    if (out.normals.empty()) {
        out.normals.assign(n * 3u, 0.f);
    }
    if (out.uvs.empty()) {
        out.uvs.assign(n * 2u, 0.f);
    }
    if (codec) {
        if (!need(4u)) {
            return reject("cooked mesh size mismatch");
        }
        const u32 encodedBytes = get_u32(data + cursor);
        cursor += 4u;
        const u64 padded = (static_cast<u64>(encodedBytes) + 3u) & ~static_cast<u64>(3u);
        if (!need(padded) || indexCount % 3u != 0u) {
            return reject("cooked mesh size mismatch");
        }
        if (!meshopt_decode_indices(data + cursor, encodedBytes, indexCount, out.indices)) {
            return reject("cooked mesh index meshopt decode failed");
        }
        cursor += static_cast<usize>(padded);
    } else {
        if (static_cast<u64>(cursor) + static_cast<u64>(indexCount) * 4u > payloadEnd) {
            return reject("cooked mesh size mismatch");
        }
        out.indices.resize(indexCount);
        for (u32& index : out.indices) {
            index = get_u32(data + cursor);
            cursor += 4u;
        }
    }
    for (u32 index : out.indices) {
        if (index >= vertexCount) {
            return reject("cooked mesh index out of range");
        }
    }
    if ((flags & kFmshFlagSections) != 0u) {
        std::string why;
        usize consumed = 0;
        if (!read_sections(data + cursor, payloadEnd - cursor, consumed, codec, out, &why)) {
            return reject("cooked mesh " + why);
        }
        cursor += consumed;
    }
    if (cursor != payloadEnd) {
        return reject("cooked mesh size mismatch");
    }
    for (f32 value : out.positions) {
        if (!std::isfinite(value)) {
            return reject("cooked mesh position not finite");
        }
    }
    return true;
}

} // namespace

bool deserialize_cooked_mesh(const u8* data, usize size, CookedMesh& out, std::string* error) {
    out = CookedMesh{};
    if (data == nullptr || size < kFmshHeaderBytes + kFmshTrailerBytes) {
        set_error(error, "cooked mesh truncated");
        return false;
    }
    if (std::memcmp(data, kFmshMagic, sizeof(kFmshMagic)) != 0) {
        set_error(error, "cooked mesh magic mismatch");
        return false;
    }
    const u32 version = get_u32(data + 4);
    if (version == kCookedMeshVersion) {
        return deserialize_v2(data, size, out, error);
    }
    if (version != kCookedMeshVersionV1) {
        set_error(error, "cooked mesh version " + std::to_string(version) + " unsupported");
        return false;
    }
    const u32 vertexCount = get_u32(data + 12);
    const u32 indexCount = get_u32(data + 16);
    const u32 submeshCount = get_u32(data + 20);
    const u64 expected = kFmshHeaderBytes + static_cast<u64>(submeshCount) * kFmshSubmeshBytes +
                         static_cast<u64>(vertexCount) * 32u + static_cast<u64>(indexCount) * 4u + kFmshTrailerBytes;
    if (expected != size) {
        set_error(error, "cooked mesh size mismatch");
        return false;
    }
    if (get_u64(data + size - kFmshTrailerBytes) != fmsh_fnv1a64(data, size - kFmshTrailerBytes)) {
        set_error(error, "cooked mesh checksum mismatch");
        return false;
    }

    const u8* cursor = data + 24;
    for (f32& value : out.bounds_min) {
        value = get_f32(cursor);
        cursor += 4;
    }
    for (f32& value : out.bounds_max) {
        value = get_f32(cursor);
        cursor += 4;
    }
    out.submeshes.resize(submeshCount);
    for (CookedMesh::Submesh& submesh : out.submeshes) {
        submesh.index_offset = get_u32(cursor);
        submesh.index_count = get_u32(cursor + 4);
        submesh.vertex_offset = get_u32(cursor + 8);
        submesh.material_index = get_u32(cursor + 12);
        cursor += kFmshSubmeshBytes;
        if (static_cast<u64>(submesh.index_offset) + submesh.index_count > indexCount) {
            set_error(error, "cooked mesh submesh range out of bounds");
            out = CookedMesh{};
            return false;
        }
    }
    auto read_floats = [&](std::vector<f32>& dst, usize count) {
        dst.resize(count);
        for (f32& value : dst) {
            value = get_f32(cursor);
            cursor += 4;
        }
    };
    read_floats(out.positions, static_cast<usize>(vertexCount) * 3u);
    read_floats(out.normals, static_cast<usize>(vertexCount) * 3u);
    read_floats(out.uvs, static_cast<usize>(vertexCount) * 2u);
    out.indices.resize(indexCount);
    for (u32& index : out.indices) {
        index = get_u32(cursor);
        cursor += 4;
        if (index >= vertexCount) {
            set_error(error, "cooked mesh index out of range");
            out = CookedMesh{};
            return false;
        }
    }
    return true;
}

bool read_cooked_mesh_file(const std::string& path, CookedMesh& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cooked mesh unreadable");
        out = CookedMesh{};
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return deserialize_cooked_mesh(bytes.data(), bytes.size(), out, error);
}

} // namespace fuse::asset
