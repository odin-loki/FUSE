// WP-5.3 test helpers shared by the CPU gates (test_rp_geometry_streaming_cpu.cpp) and the Lavapipe gates
// (test_rp_geometry_streaming.cpp): a procedural closed mesh + its DAG + page file, the scripted camera,
// and the "only resident pages" watertightness / area check of a cut.
#pragma once

#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry_streaming/cluster_page_file.hpp>
#include <fuse/renderer/geometry_streaming/stream_cut_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stream_test {

using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace geometry = fuse::renderer::geometry;
namespace dag = fuse::renderer::geometry::dag;
namespace gs = fuse::renderer::geometry_streaming;

/// Subdivided icosahedron with radial bumps, outward CCW, closed (every edge shared by two triangles).
struct SourceMesh {
    std::vector<f32> positions;
    std::vector<u32> indices;
};

inline SourceMesh make_bumpy_sphere(u32 subdivisions, f32 radius, f32 bump) {
    const f32 t = 1.61803398875f;
    std::vector<f32> p = {-1, t, 0, 1, t, 0, -1, -t, 0, 1, -t, 0, 0, -1, t, 0, 1, t,
                          0, -1, -t, 0, 1, -t, t, 0, -1, t, 0, 1, -t, 0, -1, -t, 0, 1};
    std::vector<u32> idx = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                            3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (u32 s = 0; s < subdivisions; ++s) {
        std::map<std::pair<u32, u32>, u32> mid;
        auto midpoint = [&](u32 a, u32 b) {
            const std::pair<u32, u32> key{std::min(a, b), std::max(a, b)};
            auto it = mid.find(key);
            if (it != mid.end()) {
                return it->second;
            }
            const u32 id = static_cast<u32>(p.size() / 3u);
            for (u32 k = 0; k < 3u; ++k) {
                p.push_back(0.5f * (p[a * 3u + k] + p[b * 3u + k]));
            }
            mid.emplace(key, id);
            return id;
        };
        std::vector<u32> next;
        next.reserve(idx.size() * 4u);
        for (usize i = 0; i < idx.size(); i += 3u) {
            const u32 a = idx[i], b = idx[i + 1u], c = idx[i + 2u];
            const u32 ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        idx.swap(next);
    }
    SourceMesh m;
    for (usize v = 0; v < p.size() / 3u; ++v) {
        f64 d[3] = {p[v * 3u], p[v * 3u + 1u], p[v * 3u + 2u]};
        const f64 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (f64& x : d) {
            x /= len;
        }
        const f64 r = radius * (1.0 + bump * std::sin(5.0 * d[0] + 1.0) * std::sin(7.0 * d[1]) * std::sin(3.0 * d[2] + 2.0));
        for (u32 k = 0; k < 3u; ++k) {
            m.positions.push_back(static_cast<f32>(r * d[k]));
        }
    }
    m.indices = idx;
    f64 vol = 0.0;
    for (usize i = 0; i < idx.size(); i += 3u) {
        const f32* a = &m.positions[idx[i] * 3u];
        const f32* b = &m.positions[idx[i + 1u] * 3u];
        const f32* c = &m.positions[idx[i + 2u] * 3u];
        vol += static_cast<f64>(a[0]) * (b[1] * c[2] - b[2] * c[1]) - static_cast<f64>(a[1]) * (b[0] * c[2] - b[2] * c[0]) +
               static_cast<f64>(a[2]) * (b[0] * c[1] - b[1] * c[0]);
    }
    if (vol < 0.0) {
        for (usize i = 0; i < m.indices.size(); i += 3u) {
            std::swap(m.indices[i + 1u], m.indices[i + 2u]);
        }
    }
    return m;
}

struct StreamAsset {
    dag::ClusterDagMesh mesh;
    gs::ClusterPageFile pages;
    std::vector<gs::StreamClusterInfo> info;
    /// cluster -> (page, index of its StreamCluster in the page)
    std::vector<std::pair<u32, u32>> where;
    f64 source_area = 0.0;
};

inline bool build_asset(const SourceMesh& src, u32 pageBytes, StreamAsset& out, std::string* error) {
    geometry::MeshletSource s{};
    s.positions = src.positions.data();
    s.vertex_count = static_cast<u32>(src.positions.size() / 3u);
    s.indices = src.indices.data();
    s.index_count = static_cast<u32>(src.indices.size());
    if (!dag::build_cluster_dag_mesh(s, geometry::MeshletBuildOptions{}, dag::DagBuildOptions{}, out.mesh, error)) {
        return false;
    }
    gs::PageBuildOptions po{};
    po.page_bytes = pageBytes;
    if (!gs::build_cluster_pages(out.mesh, po, out.pages, error)) {
        return false;
    }
    gs::build_stream_cluster_info(out.mesh.dag, out.pages, out.info);
    out.where.assign(out.mesh.dag.cluster_count(), {gs::kPageNone, 0u});
    for (u32 p = 0; p < out.pages.page_count(); ++p) {
        gs::PageView v{};
        gs::view_page(out.pages.page_payload(p), out.pages.pages[p].payload_bytes, v);
        for (u32 c = 0; c < v.header->cluster_count; ++c) {
            out.where[v.clusters[c].cluster] = {p, c};
        }
    }
    return true;
}

/// Camera of the scripted fly-through at frame f of `frames` around a sphere of radius `r`: approach
/// from far away, skim the surface (half an orbit at 3% of the radius above it, dipping to 1%), climb
/// out, then hold still (the last `hold` frames).
inline void flythrough_camera(u32 f, u32 frames, u32 hold, f32 r, f32 out[3]) {
    const u32 moving = frames - hold;
    const f32 t = static_cast<f32>(std::min(f, moving - 1u)) / static_cast<f32>(moving - 1u);
    f32 dist = 0.f, angle = 0.f, lift = 0.f;
    if (t < 0.25f) {
        const f32 u = t / 0.25f;
        dist = r * (6.f - 4.97f * u); // 6 r -> 1.03 r
        angle = 0.f;
    } else if (t < 0.75f) {
        const f32 u = (t - 0.25f) / 0.5f;
        dist = r * (1.03f - 0.02f * std::sin(u * 3.14159265f));
        angle = u * 3.14159265f;
        lift = 0.35f * std::sin(u * 6.2831853f);
    } else {
        const f32 u = (t - 0.75f) / 0.25f;
        dist = r * (1.03f + 2.f * u);
        angle = 3.14159265f;
        lift = 0.f;
    }
    out[0] = dist * std::cos(angle) * std::cos(lift);
    out[1] = dist * std::sin(lift);
    out[2] = dist * std::sin(angle) * std::cos(lift);
}

inline dag::cut_kernel::DagView make_view(const f32 camera[3], f32 threshold) {
    // 60 degree vertical FOV, 720 px viewport, near 0.01.
    return dag::cut_kernel::make_dag_view(camera, 1.f / std::tan(0.5f * 1.04719755f), 720.f, threshold, 0.01f);
}

/// Undirected edge between two quantised positions (48-bit keys), exact equality.
struct EdgeKey {
    u64 lo = 0, hi = 0;
    bool operator==(const EdgeKey& o) const { return lo == o.lo && hi == o.hi; }
};
struct EdgeKeyHash {
    usize operator()(const EdgeKey& k) const {
        u64 h = k.lo * 0x9E3779B97F4A7C15ull;
        h ^= (k.hi + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2));
        return static_cast<usize>(h ^ (h >> 29));
    }
};

/// Result of checking a cut against page data only.
struct CutCheck {
    bool pages_ok = true;      ///< every drawn cluster lives in a resident page and is found there
    u64 open_edges = 0;        ///< directed edges without their reverse (0 = watertight, closed source)
    f64 area = 0.0;
    u32 clusters = 0;
    u64 triangles = 0;
};

/// `slotData(page)` returns the bytes the pool slot of a resident page holds (nullptr: not resident).
/// Every triangle is decoded from those bytes alone (StreamCluster -> refs -> positions).
template <typename SlotFn>
CutCheck check_cut(const StreamAsset& a, const std::vector<u32>& cut, SlotFn slotData) {
    CutCheck r;
    std::unordered_map<EdgeKey, int, EdgeKeyHash> edges;
    edges.reserve(1u << 16);
    const geometry::QuantParams& q = a.pages.quant;
    for (u32 c = 0; c < cut.size(); ++c) {
        if (cut[c] == 0u) {
            continue;
        }
        ++r.clusters;
        const u32 page = a.info[c].member_page;
        const u8* data = slotData(page);
        gs::PageView v{};
        if (data == nullptr || !gs::view_page(data, a.pages.page_bytes, v) || v.header->page != page) {
            r.pages_ok = false;
            continue;
        }
        const u32 idx = a.where[c].second;
        if (idx >= v.header->cluster_count || v.clusters[idx].cluster != c) {
            r.pages_ok = false;
            continue;
        }
        const geometry::MeshletRecord& rec = v.clusters[idx].record;
        for (u32 t = 0; t < rec.triangle_count; ++t) {
            const u32 packed = v.triangles[rec.triangle_offset + t];
            u64 key[3];
            f32 pos[3][3];
            for (u32 k = 0; k < 3u; ++k) {
                const u32 local = v.refs[rec.vertex_offset + geometry::triangle_index(packed, k)];
                const u16* vp = v.positions + static_cast<usize>(local) * 4u;
                key[k] = static_cast<u64>(vp[0]) | (static_cast<u64>(vp[1]) << 16) | (static_cast<u64>(vp[2]) << 32);
                gs::decode_page_position(q, vp, pos[k]);
            }
            ++r.triangles;
            const f64 e1[3] = {static_cast<f64>(pos[1][0]) - pos[0][0], static_cast<f64>(pos[1][1]) - pos[0][1],
                               static_cast<f64>(pos[1][2]) - pos[0][2]};
            const f64 e2[3] = {static_cast<f64>(pos[2][0]) - pos[0][0], static_cast<f64>(pos[2][1]) - pos[0][1],
                               static_cast<f64>(pos[2][2]) - pos[0][2]};
            const f64 cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
            r.area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
            for (u32 k = 0; k < 3u; ++k) {
                const u64 x = key[k], y = key[(k + 1u) % 3u];
                if (x == y) {
                    continue;
                }
                edges[EdgeKey{std::min(x, y), std::max(x, y)}] += x < y ? 1 : -1;
            }
        }
    }
    for (const auto& [k, n] : edges) {
        (void)k;
        r.open_edges += static_cast<u64>(n < 0 ? -n : n);
    }
    return r;
}

} // namespace stream_test
