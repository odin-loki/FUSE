// WP-5.2 cluster DAG + LOD gates (CPU only; runs in the stub build). Suites (argv[1]):
//   build        procedural set: DAG validates, leaves are the WP-1.2 meshlets, LOD clusters within
//                limits with culling bounds containing their decoded vertices, children only use
//                vertices of their group's members, triangle count shrinks per level, error and
//                LOD spheres monotonic up every DAG edge (with the cook slack), bad input rejected
//   crack        for every simplified group: boundary(members) == boundary(children) as directed
//                edge chains over exact quantised positions (vertex-identical shared boundaries);
//                only the source mesh's own open border may differ; includes groups next to
//                terminal ("stuck") groups and submesh (material) borders
//   cut          geometry_dag_cut over random, near-surface, inside, far and adversarial (exact
//                tie) views: the cut is watertight (boundary chain == source open border only),
//                area within the source-area bounds, acceptance monotone up the DAG (0 violations),
//                triangle count monotone in the threshold, threshold extremes give leaves / roots,
//                CpuReference == CpuParallel
//   format       FMLT 1.1 bit-exact round trip; the 1.0 reader reads the full-detail mesh from a
//                DAG file; every truncation / sampled byte flip rejected; unknown chunks and newer
//                minors accepted, major 2 rejected; duplicate / missing / resized DAG chunks, count
//                and header mismatches and 14 semantic corruptions rejected with the right error
//   determinism  same bytes twice, across CpuReference / CpuParallel (0, 2, 4 workers), and from a
//                parsed base mesh

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/geometry/dag/cluster_dag.hpp>
#include <fuse/renderer/geometry/dag/fmlt_chunks.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace geo = fuse::renderer::geometry;
namespace dag = fuse::renderer::geometry::dag;
namespace cut = fuse::renderer::geometry::dag::cut_kernel;
namespace kernel = fuse::kernel;
namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

bool expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
    return condition;
}

struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed * 2654435761u + 1u) {}
    u32 next_u32() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    f32 unit() { return static_cast<f32>(next_u32() >> 8) / 16777216.f; }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

// ---- procedural meshes ------------------------------------------------------------------------------

struct TestMesh {
    std::string name;
    std::vector<f32> positions, normals, uvs;
    std::vector<u32> indices;
    std::vector<geo::MeshletSourceSubmesh> submeshes;
    bool closed = false; ///< closed by position: every cut must have an empty boundary
    bool expect_levels = true; ///< large enough that the DAG must have more than one level
    bool reducible = true;     ///< the coarsest cut must be well below the full-detail triangle count

    u32 vertex_count() const { return static_cast<u32>(positions.size() / 3u); }
    geo::MeshletSource source() const {
        geo::MeshletSource s;
        s.positions = positions.data();
        s.normals = normals.empty() ? nullptr : normals.data();
        s.uvs = uvs.empty() ? nullptr : uvs.data();
        s.vertex_count = vertex_count();
        s.indices = indices.data();
        s.index_count = static_cast<u32>(indices.size());
        s.submeshes = submeshes;
        return s;
    }
};

void push_vertex(TestMesh& m, f32 x, f32 y, f32 z, f32 u, f32 v) {
    m.positions.insert(m.positions.end(), {x, y, z});
    m.uvs.insert(m.uvs.end(), {u, v});
}

/// Subdivided icosahedron (no seams), outward CCW; optional radial bumps.
TestMesh make_icosphere(u32 subdivisions, f32 radius, const f32 center[3], f32 bump, const std::string& name) {
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
        for (usize i = 0; i < idx.size(); i += 3u) {
            const u32 a = idx[i], b = idx[i + 1u], c = idx[i + 2u];
            const u32 ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        idx.swap(next);
    }
    TestMesh m;
    m.name = name;
    m.closed = true;
    for (usize v = 0; v < p.size() / 3u; ++v) {
        f64 d[3] = {p[v * 3u], p[v * 3u + 1u], p[v * 3u + 2u]};
        const f64 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (f64& x : d) {
            x /= len;
        }
        const f64 r = radius * (1.0 + bump * std::sin(5.0 * d[0] + 1.0) * std::sin(7.0 * d[1]) * std::sin(3.0 * d[2] + 2.0));
        push_vertex(m, center[0] + static_cast<f32>(r * d[0]), center[1] + static_cast<f32>(r * d[1]),
                    center[2] + static_cast<f32>(r * d[2]), static_cast<f32>(0.5 + 0.5 * d[0]), static_cast<f32>(0.5 + 0.5 * d[1]));
    }
    m.indices = idx;
    // Orient outward: positive signed volume.
    f64 vol = 0.0;
    for (usize i = 0; i < idx.size(); i += 3u) {
        const f32* a = &m.positions[idx[i] * 3u];
        const f32* b = &m.positions[idx[i + 1u] * 3u];
        const f32* c = &m.positions[idx[i + 2u] * 3u];
        vol += static_cast<f64>(a[0] - center[0]) * ((b[1] - center[1]) * (c[2] - center[2]) - (b[2] - center[2]) * (c[1] - center[1])) -
               static_cast<f64>(a[1] - center[1]) * ((b[0] - center[0]) * (c[2] - center[2]) - (b[2] - center[2]) * (c[0] - center[0])) +
               static_cast<f64>(a[2] - center[2]) * ((b[0] - center[0]) * (c[1] - center[1]) - (b[1] - center[1]) * (c[0] - center[0]));
    }
    if (vol < 0.0) {
        for (usize i = 0; i < m.indices.size(); i += 3u) {
            std::swap(m.indices[i + 1u], m.indices[i + 2u]);
        }
    }
    return m;
}

/// UV sphere: seam column and pole rows are duplicated vertices (closed by position).
TestMesh make_uv_sphere(u32 rings, u32 segments) {
    TestMesh m;
    m.name = "uv_sphere";
    m.closed = true;
    const f32 pi = 3.14159265358979f;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 theta = pi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 phi = 2.f * pi * static_cast<f32>(s % segments) / static_cast<f32>(segments);
            f32 x = std::sin(theta) * std::cos(phi), y = std::cos(theta), z = std::sin(theta) * std::sin(phi);
            if (r == 0u || r == rings) {
                x = 0.f;
                z = 0.f;
            }
            push_vertex(m, x, y, z, static_cast<f32>(s) / static_cast<f32>(segments), static_cast<f32>(r) / static_cast<f32>(rings));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1u) + s, b = a + 1u, c = a + segments + 1u, d = c + 1u;
            if (r != 0u) {
                m.indices.insert(m.indices.end(), {a, b, c});
            }
            if (r + 1u != rings) {
                m.indices.insert(m.indices.end(), {b, d, c});
            }
        }
    }
    return m;
}

TestMesh make_torus(u32 major, u32 minor) {
    TestMesh m;
    m.name = "torus";
    m.closed = true;
    const f32 pi = 3.14159265358979f, R = 1.f, r = 0.3f;
    for (u32 i = 0; i <= major; ++i) {
        const f32 u = 2.f * pi * static_cast<f32>(i % major) / static_cast<f32>(major);
        for (u32 j = 0; j <= minor; ++j) {
            const f32 v = 2.f * pi * static_cast<f32>(j % minor) / static_cast<f32>(minor);
            push_vertex(m, (R + r * std::cos(v)) * std::cos(u), r * std::sin(v), (R + r * std::cos(v)) * std::sin(u),
                        static_cast<f32>(i) / static_cast<f32>(major), static_cast<f32>(j) / static_cast<f32>(minor));
        }
    }
    for (u32 i = 0; i < major; ++i) {
        for (u32 j = 0; j < minor; ++j) {
            const u32 a = i * (minor + 1u) + j, b = a + 1u, c = a + minor + 1u, d = c + 1u;
            m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
        }
    }
    return m;
}

/// Hard-edged cube, one submesh (material) per face, each face subdivided n x n and gently domed so
/// simplification has non-zero error; face borders are shared only by position across submeshes.
TestMesh make_cube_submeshes(u32 n) {
    TestMesh m;
    m.name = "cube6";
    m.closed = true;
    const f32 axes[6][3][3] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (u32 f = 0; f < 6u; ++f) {
        const f32* nrm = axes[f][0];
        const f32* tu = axes[f][1];
        const f32* tv = axes[f][2];
        const u32 base = m.vertex_count();
        for (u32 y = 0; y <= n; ++y) {
            for (u32 x = 0; x <= n; ++x) {
                const f32 s = -1.f + 2.f * static_cast<f32>(x) / static_cast<f32>(n);
                const f32 t = -1.f + 2.f * static_cast<f32>(y) / static_cast<f32>(n);
                const f32 dome = 1.f + 0.15f * (1.f - s * s) * (1.f - t * t);
                push_vertex(m, nrm[0] * dome + s * tu[0] + t * tv[0], nrm[1] * dome + s * tu[1] + t * tv[1],
                            nrm[2] * dome + s * tu[2] + t * tv[2], (s + 1.f) * 0.5f, (t + 1.f) * 0.5f);
            }
        }
        geo::MeshletSourceSubmesh sub{static_cast<u32>(m.indices.size()), 0u, 10u + f};
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                const u32 a = base + y * (n + 1u) + x, b = a + 1u, c = a + n + 1u, d = c + 1u;
                m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
            }
        }
        sub.index_count = static_cast<u32>(m.indices.size()) - sub.index_offset;
        m.submeshes.push_back(sub);
    }
    return m;
}

/// Open grid in XZ; `wave` > 0 adds height so simplification error is non-zero.
TestMesh make_grid(u32 nx, u32 ny, f32 wave, const std::string& name) {
    TestMesh m;
    m.name = name;
    for (u32 y = 0; y <= ny; ++y) {
        for (u32 x = 0; x <= nx; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(nx), fy = static_cast<f32>(y) / static_cast<f32>(ny);
            push_vertex(m, fx * 4.f, wave * std::sin(fx * 9.f) * std::cos(fy * 7.f), fy * 4.f, fx, fy);
        }
    }
    for (u32 y = 0; y < ny; ++y) {
        for (u32 x = 0; x < nx; ++x) {
            const u32 a = y * (nx + 1u) + x, b = a + 1u, c = a + nx + 1u, d = c + 1u;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

/// Wavy grid whose left third is faceted (every triangle has its own vertices, so every vertex
/// there is a multi-way attribute seam the simplifier must keep): groups there get stuck at an early
/// level while the smooth part keeps simplifying next to them.
TestMesh make_grid_faceted(u32 nx, u32 ny) {
    TestMesh m = make_grid(nx, ny, 0.25f, "grid_faceted");
    const std::vector<f32> pos = m.positions;
    std::vector<u32> idx;
    for (usize i = 0; i < m.indices.size(); i += 3u) {
        const u32 a = m.indices[i];
        if (a % (nx + 1u) < nx / 3u) {
            for (u32 c = 0; c < 3u; ++c) {
                const u32 v = m.indices[i + c];
                idx.push_back(m.vertex_count());
                push_vertex(m, pos[v * 3u], pos[v * 3u + 1u], pos[v * 3u + 2u], static_cast<f32>(i % 7u) * 0.1f + static_cast<f32>(c) * 0.01f,
                            static_cast<f32>(i % 11u) * 0.05f);
            }
        } else {
            idx.insert(idx.end(), {m.indices[i], m.indices[i + 1u], m.indices[i + 2u]});
        }
    }
    m.indices = idx;
    m.reducible = false;
    return m;
}

TestMesh make_two_spheres() {
    const f32 c0[3] = {-2.f, 0.f, 0.f};
    const f32 c1[3] = {2.5f, 0.5f, 0.f};
    TestMesh a = make_icosphere(3, 1.f, c0, 0.05f, "a");
    const TestMesh b = make_icosphere(3, 1.5f, c1, 0.05f, "b");
    const u32 offset = a.vertex_count();
    a.positions.insert(a.positions.end(), b.positions.begin(), b.positions.end());
    a.uvs.insert(a.uvs.end(), b.uvs.begin(), b.uvs.end());
    for (u32 i : b.indices) {
        a.indices.push_back(i + offset);
    }
    a.name = "two_spheres";
    return a;
}

TestMesh make_single_triangle() {
    TestMesh m;
    m.name = "triangle";
    m.expect_levels = false;
    push_vertex(m, 0.f, 0.f, 0.f, 0.f, 0.f);
    push_vertex(m, 1.f, 0.f, 0.f, 1.f, 0.f);
    push_vertex(m, 0.f, 1.f, 0.f, 0.f, 1.f);
    m.indices = {0u, 1u, 2u};
    return m;
}

std::vector<TestMesh> procedural_set() {
    const f32 origin[3] = {0.f, 0.f, 0.f};
    const f32 far[3] = {100000.f, -2500.5f, 31337.25f};
    std::vector<TestMesh> set;
    set.push_back(make_icosphere(5, 1.f, origin, 0.f, "icosphere5"));
    set.push_back(make_icosphere(4, 3.f, far, 0.12f, "bumpy_far"));
    set.push_back(make_uv_sphere(48, 96));
    set.push_back(make_torus(160, 48));
    set.push_back(make_cube_submeshes(24));
    set.push_back(make_grid(64, 64, 0.25f, "grid_wavy"));
    set.push_back(make_grid(48, 48, 0.f, "grid_flat"));
    set.push_back(make_grid_faceted(72, 48));
    set.push_back(make_two_spheres());
    TestMesh small = make_icosphere(1, 1.f, origin, 0.f, "icosphere1");
    small.expect_levels = false;
    set.push_back(small);
    set.push_back(make_single_triangle());
    return set;
}

// ---- built mesh + helpers ------------------------------------------------------------------------------

struct Built {
    std::string name;
    bool closed = false;
    bool expect_levels = true;
    bool reducible = true;
    dag::ClusterDagMesh mesh;
    geo::DecodedVertices decoded;
    std::vector<u64> key;       ///< per cooked vertex: exact quantised position (x | y << 16 | z << 32)
    std::set<u64> open_border;  ///< keys on the full-detail surface's open border
    f64 source_area = 0.0;
    f32 center[3] = {0.f, 0.f, 0.f};
    f32 radius = 0.f;
};

using Chain = std::map<std::pair<u64, u64>, s32>;

void chain_edge(Chain& ch, u64 a, u64 b, s32 sign) {
    if (a == b) {
        return;
    }
    if (a < b) {
        ch[{a, b}] += sign;
    } else {
        ch[{b, a}] -= sign;
    }
}

void chain_cluster(Chain& ch, const Built& b, u32 id, s32 sign) {
    const dag::ClusterRef ref = dag::cluster_ref(b.mesh, id);
    for (u32 t = 0; t < ref.record->triangle_count; ++t) {
        const u32 packed = ref.triangles[t];
        const u64 k0 = b.key[ref.vertices[geo::triangle_index(packed, 0)]];
        const u64 k1 = b.key[ref.vertices[geo::triangle_index(packed, 1)]];
        const u64 k2 = b.key[ref.vertices[geo::triangle_index(packed, 2)]];
        chain_edge(ch, k0, k1, sign);
        chain_edge(ch, k1, k2, sign);
        chain_edge(ch, k2, k0, sign);
    }
}

/// Non-zero entries of the chain whose endpoints are not both on the source's open border.
usize chain_violations(const Chain& ch, const std::set<u64>& border) {
    usize bad = 0;
    for (const auto& [edge, count] : ch) {
        if (count != 0 && (border.count(edge.first) == 0u || border.count(edge.second) == 0u)) {
            ++bad;
        }
    }
    return bad;
}

f64 cluster_area(const Built& b, u32 id) {
    const dag::ClusterRef ref = dag::cluster_ref(b.mesh, id);
    f64 area = 0.0;
    for (u32 t = 0; t < ref.record->triangle_count; ++t) {
        const u32 packed = ref.triangles[t];
        const f32* p0 = &b.decoded.positions[ref.vertices[geo::triangle_index(packed, 0)] * 3u];
        const f32* p1 = &b.decoded.positions[ref.vertices[geo::triangle_index(packed, 1)] * 3u];
        const f32* p2 = &b.decoded.positions[ref.vertices[geo::triangle_index(packed, 2)] * 3u];
        const f64 e1[3] = {static_cast<f64>(p1[0]) - p0[0], static_cast<f64>(p1[1]) - p0[1], static_cast<f64>(p1[2]) - p0[2]};
        const f64 e2[3] = {static_cast<f64>(p2[0]) - p0[0], static_cast<f64>(p2[1]) - p0[1], static_cast<f64>(p2[2]) - p0[2]};
        const f64 c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        area += 0.5 * std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    }
    return area;
}

dag::DagBuildOptions test_options(kernel::Backend backend) {
    dag::DagBuildOptions o;
    o.group_size = 8u; // more levels on the small test meshes than the default 16
    o.backend = backend;
    return o;
}

bool build(const TestMesh& m, kernel::Backend backend, Built& out, const dag::DagBuildOptions* options = nullptr) {
    out = Built{};
    out.name = m.name;
    out.closed = m.closed;
    out.expect_levels = m.expect_levels;
    out.reducible = m.reducible;
    geo::MeshletBuildOptions mo;
    mo.backend = backend;
    const dag::DagBuildOptions o = options != nullptr ? *options : test_options(backend);
    std::string error;
    if (!expect(dag::build_cluster_dag_mesh(m.source(), mo, o, out.mesh, &error), m.name + ": DAG build (" + error + ")")) {
        return false;
    }
    geo::decode_vertices(out.mesh.base, out.decoded, kernel::Backend::CpuReference);
    const u32 n = out.mesh.base.vertex_count();
    out.key.resize(n);
    f32 lo[3] = {0.f, 0.f, 0.f}, hi[3] = {0.f, 0.f, 0.f};
    for (u32 v = 0; v < n; ++v) {
        const u16* q = &out.mesh.base.positions[v * 4u];
        out.key[v] = static_cast<u64>(q[0]) | (static_cast<u64>(q[1]) << 16) | (static_cast<u64>(q[2]) << 32);
        for (u32 a = 0; a < 3u; ++a) {
            const f32 x = out.decoded.positions[v * 3u + a];
            lo[a] = v == 0u ? x : std::min(lo[a], x);
            hi[a] = v == 0u ? x : std::max(hi[a], x);
        }
    }
    f64 r2 = 0.0;
    for (u32 a = 0; a < 3u; ++a) {
        out.center[a] = 0.5f * (lo[a] + hi[a]);
        r2 += 0.25 * (static_cast<f64>(hi[a]) - lo[a]) * (static_cast<f64>(hi[a]) - lo[a]);
    }
    out.radius = static_cast<f32>(std::sqrt(r2));
    Chain leaves;
    for (u32 c = 0; c < out.mesh.dag.leaf_cluster_count; ++c) {
        chain_cluster(leaves, out, c, 1);
        out.source_area += cluster_area(out, c);
    }
    for (const auto& [edge, count] : leaves) {
        if (count != 0) {
            out.open_border.insert(edge.first);
            out.open_border.insert(edge.second);
        }
    }
    return true;
}

std::vector<Built> build_set(kernel::Backend backend) {
    std::vector<Built> set;
    for (const TestMesh& m : procedural_set()) {
        Built b;
        if (build(m, backend, b)) {
            set.push_back(std::move(b));
        }
    }
    return set;
}

// ---- build ------------------------------------------------------------------------------------------

void suite_build() {
    fuse::kernel::reset_kernel_stats();
    for (const Built& b : build_set(kernel::Backend::CpuReference)) {
        const dag::ClusterDag& d = b.mesh.dag;
        const geo::MeshletMesh& base = b.mesh.base;
        std::string why;
        expect(dag::validate_cluster_dag(base, d, &why), b.name + ": DAG validates (" + why + ")");
        expect(d.leaf_cluster_count == base.meshlets.size(), b.name + ": leaves are the WP-1.2 meshlets");
        expect(b.closed == b.open_border.empty(), b.name + ": closed-by-position flag matches the leaf surface");
        const std::vector<dag::DagLevelStats> levels = dag::dag_level_stats(b.mesh);
        std::string hist;
        for (const dag::DagLevelStats& l : levels) {
            hist += " " + std::to_string(l.clusters) + "c/" + std::to_string(l.triangles) + "t";
        }
        std::printf("  %-12s levels %u groups %zu lod clusters %zu (%s )\n", b.name.c_str(), d.level_count, d.groups.size(),
                    d.lod_clusters.size(), hist.c_str());
        if (b.expect_levels) {
            expect(d.level_count >= 3u, b.name + ": at least 3 DAG levels");
        }
        // Triangles shrink per simplified group.
        bool shrinks = true;
        bool subset = true;
        bool limits = true;
        bool bounds = true;
        for (const dag::DagGroup& g : d.groups) {
            if (g.child_count == 0u) {
                continue;
            }
            u64 memberTris = 0, childTris = 0;
            std::set<u32> memberVerts;
            for (u32 i = g.member_offset; i < g.member_offset + g.member_count; ++i) {
                const dag::ClusterRef ref = dag::cluster_ref(b.mesh, d.group_members[i]);
                memberTris += ref.record->triangle_count;
                memberVerts.insert(ref.vertices, ref.vertices + ref.record->vertex_count);
            }
            for (u32 c = g.child_offset; c < g.child_offset + g.child_count; ++c) {
                const dag::ClusterRef ref = dag::cluster_ref(b.mesh, c);
                childTris += ref.record->triangle_count;
                for (u32 i = 0; i < ref.record->vertex_count; ++i) {
                    subset = subset && memberVerts.count(ref.vertices[i]) != 0u;
                }
            }
            shrinks = shrinks && static_cast<f64>(childTris) <= 0.85 * static_cast<f64>(memberTris);
        }
        for (const geo::MeshletRecord& r : d.lod_clusters) {
            limits = limits && r.vertex_count <= geo::kMeshletMaxVertices && r.triangle_count <= geo::kMeshletMaxTriangles;
            for (u32 i = 0; i < r.vertex_count; ++i) {
                const f32* p = &b.decoded.positions[d.lod_meshlet_vertices[r.vertex_offset + i] * 3u];
                f64 d2 = 0.0;
                for (u32 a = 0; a < 3u; ++a) {
                    const f64 dd = static_cast<f64>(p[a]) - r.center[a];
                    d2 += dd * dd;
                    bounds = bounds && p[a] >= r.aabb_min[a] && p[a] <= r.aabb_max[a];
                }
                bounds = bounds && d2 <= static_cast<f64>(r.radius) * r.radius;
            }
        }
        expect(shrinks, b.name + ": every simplified group keeps <= 85% of its members' triangles");
        expect(subset, b.name + ": children use only their group members' vertices (no new vertices)");
        expect(limits, b.name + ": LOD clusters within 64 v / 124 t");
        expect(bounds, b.name + ": LOD cluster spheres and AABBs contain their decoded vertices");
        // Error monotonic up every DAG edge, spheres nested with the cook slack.
        const dag::DagBuildOptions o = test_options(kernel::Backend::CpuReference);
        bool errorMono = true;
        bool sphereMono = true;
        bool rootsTerminal = true;
        u32 finiteGroups = 0;
        for (u32 c = 0; c < d.cluster_count(); ++c) {
            const dag::DagClusterLink& l = d.links[c];
            const dag::DagGroup& g = d.groups[l.group];
            if (g.bounds.error == dag::kDagErrorTerminal) {
                continue;
            }
            errorMono = errorMono && g.bounds.error >= l.self.error &&
                        (l.self.error == 0.f || static_cast<f64>(g.bounds.error) >=
                                                    static_cast<f64>(l.self.error) * (1.0 + 0.999 * o.error_slack));
            f64 d2 = 0.0;
            for (u32 a = 0; a < 3u; ++a) {
                const f64 dd = static_cast<f64>(g.bounds.center[a]) - l.self.center[a];
                d2 += dd * dd;
            }
            sphereMono = sphereMono && (std::sqrt(d2) + l.self.radius) * (1.0 + 0.999 * o.sphere_slack) <= g.bounds.radius;
        }
        for (const dag::DagGroup& g : d.groups) {
            finiteGroups += g.bounds.error != dag::kDagErrorTerminal ? 1u : 0u;
        }
        // Every path ends in a terminal group: walking up from any cluster terminates.
        for (u32 c = 0; c < d.cluster_count() && rootsTerminal; ++c) {
            u32 id = c;
            f32 last = 0.f;
            for (u32 steps = 0; steps < 1000u; ++steps) {
                const dag::DagGroup& g = d.groups[d.links[id].group];
                if (g.bounds.error < last) {
                    rootsTerminal = false;
                }
                last = g.bounds.error;
                if (g.child_count == 0u) {
                    break;
                }
                id = g.child_offset; // any child: its group is one level up
                if (steps == 999u) {
                    rootsTerminal = false;
                }
            }
        }
        expect(errorMono, b.name + ": group error >= every member's error (x (1 + error_slack))");
        expect(sphereMono, b.name + ": group LOD sphere contains every member's LOD sphere (with sphere_slack)");
        expect(rootsTerminal, b.name + ": error non-decreasing along DAG paths, every path ends in a terminal group");
        if (b.expect_levels) {
            expect(finiteGroups > 0u, b.name + ": has simplified groups");
        }
    }
    // Bad input.
    {
        const f32 zero[3] = {0.f, 0.f, 0.f};
        const TestMesh m = make_icosphere(2, 1.f, zero, 0.f, "bad");
        geo::MeshletMesh base;
        std::string error;
        expect(geo::build_meshlets(m.source(), geo::MeshletBuildOptions{}, base, &error), "bad input: base builds");
        dag::ClusterDag out;
        dag::DagBuildOptions o;
        o.group_size = 1u;
        expect(!dag::build_cluster_dag(base, o, out, &error), "bad input: group_size 1 rejected");
        o = dag::DagBuildOptions{};
        o.simplify_ratio = 1.f;
        expect(!dag::build_cluster_dag(base, o, out, &error), "bad input: simplify_ratio 1 rejected");
        o = dag::DagBuildOptions{};
        o.sphere_slack = -1.f;
        expect(!dag::build_cluster_dag(base, o, out, &error), "bad input: negative slack rejected");
        geo::MeshletMesh broken = base;
        broken.meshlet_vertices[0] = base.vertex_count();
        expect(!dag::build_cluster_dag(broken, dag::DagBuildOptions{}, out, &error), "bad input: invalid base mesh rejected");
    }
}

// ---- crack ------------------------------------------------------------------------------------------

void suite_crack() {
    u64 groupsChecked = 0;
    u64 groupsNextToTerminal = 0;
    u64 groupsOnSubmeshBorder = 0;
    for (const Built& b : build_set(kernel::Backend::CpuParallel)) {
        const dag::ClusterDag& d = b.mesh.dag;
        // Vertices of clusters in terminal groups, by the depth the terminal group was formed at.
        std::map<u64, u32> terminalDepth; // key -> lowest depth of a terminal group using it
        for (const dag::DagGroup& g : d.groups) {
            if (g.child_count != 0u) {
                continue;
            }
            for (u32 i = g.member_offset; i < g.member_offset + g.member_count; ++i) {
                const dag::ClusterRef ref = dag::cluster_ref(b.mesh, d.group_members[i]);
                for (u32 v = 0; v < ref.record->vertex_count; ++v) {
                    auto [it, inserted] = terminalDepth.emplace(b.key[ref.vertices[v]], g.depth);
                    if (!inserted) {
                        it->second = std::min(it->second, g.depth);
                    }
                }
            }
        }
        std::map<u64, std::set<u32>> submeshOf;
        for (u32 c = 0; c < d.leaf_cluster_count; ++c) {
            const dag::ClusterRef ref = dag::cluster_ref(b.mesh, c);
            for (u32 v = 0; v < ref.record->vertex_count; ++v) {
                submeshOf[b.key[ref.vertices[v]]].insert(ref.record->submesh);
            }
        }
        usize badGroups = 0;
        usize badEdges = 0;
        bool identical = true;
        for (const dag::DagGroup& g : d.groups) {
            if (g.child_count == 0u) {
                continue;
            }
            ++groupsChecked;
            Chain members, children, diff;
            for (u32 i = g.member_offset; i < g.member_offset + g.member_count; ++i) {
                chain_cluster(members, b, d.group_members[i], 1);
                chain_cluster(diff, b, d.group_members[i], 1);
            }
            for (u32 c = g.child_offset; c < g.child_offset + g.child_count; ++c) {
                chain_cluster(children, b, c, 1);
                chain_cluster(diff, b, c, -1);
            }
            const usize bad = chain_violations(diff, b.open_border);
            badEdges += bad;
            badGroups += bad != 0u ? 1u : 0u;
            // Vertex-identical: the boundary vertex sets (off the open border) agree exactly.
            std::set<u64> mv, cv;
            bool touchesTerminal = false;
            bool touchesSubmesh = false;
            for (const auto& [edge, count] : members) {
                if (count != 0) {
                    for (u64 k : {edge.first, edge.second}) {
                        if (b.open_border.count(k) == 0u) {
                            mv.insert(k);
                        }
                        auto it = terminalDepth.find(k);
                        touchesTerminal = touchesTerminal || (it != terminalDepth.end() && it->second < g.depth);
                        touchesSubmesh = touchesSubmesh || submeshOf[k].size() > 1u;
                    }
                }
            }
            for (const auto& [edge, count] : children) {
                if (count != 0) {
                    for (u64 k : {edge.first, edge.second}) {
                        if (b.open_border.count(k) == 0u) {
                            cv.insert(k);
                        }
                    }
                }
            }
            identical = identical && mv == cv;
            groupsNextToTerminal += touchesTerminal ? 1u : 0u;
            groupsOnSubmeshBorder += touchesSubmesh ? 1u : 0u;
        }
        expect(badGroups == 0u, b.name + ": crack test: " + std::to_string(badGroups) + " group(s) / " +
                                    std::to_string(badEdges) + " boundary edge(s) differ between members and children");
        expect(identical, b.name + ": crack test: shared boundary vertex sets identical (exact quantised positions)");
    }
    std::printf("  crack test: %llu simplified groups, %llu next to an earlier terminal group, %llu on a submesh border\n",
                static_cast<unsigned long long>(groupsChecked), static_cast<unsigned long long>(groupsNextToTerminal),
                static_cast<unsigned long long>(groupsOnSubmeshBorder));
    expect(groupsChecked > 100u, "crack test covers > 100 simplified groups");
    expect(groupsOnSubmeshBorder > 0u, "crack test covers groups on submesh (material) borders");
    expect(groupsNextToTerminal > 0u, "crack test covers groups next to earlier terminal (stuck) groups");
}

// ---- cut --------------------------------------------------------------------------------------------

struct CutCheck {
    u64 triangles = 0;
    f64 area = 0.0;
    usize boundary_violations = 0;
    u32 monotone_violations = 0;
    u32 levels_in_cut = 0;
    std::vector<u32> ids;
};

CutCheck check_cut(const Built& b, const cut::DagView& view, kernel::Backend backend) {
    CutCheck r;
    const dag::ClusterDag& d = b.mesh.dag;
    dag::select_dag_cut(d, view, r.ids, backend);
    Chain ch;
    std::set<u32> levels;
    for (u32 c : r.ids) {
        chain_cluster(ch, b, c, 1);
        r.area += cluster_area(b, c);
        r.triangles += dag::cluster_ref(b.mesh, c).record->triangle_count;
        levels.insert(dag::cluster_level(d, c));
    }
    r.levels_in_cut = static_cast<u32>(levels.size());
    r.boundary_violations = chain_violations(ch, b.open_border);
    for (u32 c = 0; c < d.cluster_count(); ++c) {
        // Monotone: a group accepted as coarse enough implies every member's producer is too.
        if (cut::lod_acceptable(d.links[c].parent, view) && !cut::lod_acceptable(d.links[c].self, view)) {
            ++r.monotone_violations;
        }
    }
    return r;
}

void suite_cut() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4u);
    fuse::kernel::reset_kernel_stats();
    const f32 projY = 1.7320508f; // 60 degree vertical fov
    const f32 height = 1080.f;
    u64 views = 0;
    u32 mixedViews = 0;
    for (const Built& b : build_set(kernel::Backend::CpuParallel)) {
        const dag::ClusterDag& d = b.mesh.dag;
        Rng rng(0xC0FFEEu ^ static_cast<u32>(b.name.size() * 131u));
        const f32 znear = b.radius * 1e-3f;
        bool watertight = true, areaOk = true, mono = true, parity = true, triMono = true;
        f64 minRatio = 1e30, maxRatio = 0.0;
        std::string firstBad;
        auto run = [&](const cut::DagView& view, const std::string& what) {
            ++views;
            const CutCheck r = check_cut(b, view, kernel::Backend::CpuParallel);
            std::vector<u32> refIds;
            dag::select_dag_cut(d, view, refIds, kernel::Backend::CpuReference);
            parity = parity && refIds == r.ids;
            mono = mono && r.monotone_violations == 0u;
            if (r.boundary_violations != 0u) {
                watertight = false;
                firstBad = firstBad.empty() ? what + ": " + std::to_string(r.boundary_violations) + " open edges" : firstBad;
            }
            // Source-area bounds: simplification may shrink / grow the surface a little, never
            // anywhere near a missing (<) or double-covered (>) cluster.
            const f64 ratio = r.area / b.source_area;
            minRatio = std::min(minRatio, ratio);
            maxRatio = std::max(maxRatio, ratio);
            if (!(ratio > 0.7 && ratio < 1.2) || r.ids.empty()) {
                areaOk = false;
                firstBad = firstBad.empty() ? what + ": area ratio " + std::to_string(ratio) : firstBad;
            }
            mixedViews += r.levels_in_cut >= 2u ? 1u : 0u;
            return r;
        };
        for (u32 i = 0; i < 48u; ++i) {
            f32 dir[3] = {rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)};
            const f32 len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-6f;
            const f32 distances[8] = {0.3f, 0.9f, 1.02f, 1.3f, 2.f, 6.f, 40.f, 3000.f};
            const f32 dist = b.radius * distances[i % 8u];
            f32 cam[3];
            for (u32 a = 0; a < 3u; ++a) {
                cam[a] = b.center[a] + dir[a] / len * dist;
            }
            u64 lastTris = ~0ull;
            for (f32 t : {0.25f, 1.f, 4.f, 32.f}) {
                const CutCheck r = run(cut::make_dag_view(cam, projY, height, t, znear), "view " + std::to_string(i));
                triMono = triMono && r.triangles <= lastTris;
                lastTris = r.triangles;
            }
        }
        // Adversarial: camera just outside a group's LOD sphere, threshold at the exact tie of that group.
        u32 ties = 0;
        for (u32 gi = 0; gi < d.groups.size() && ties < 48u; gi += 1u + static_cast<u32>(d.groups.size() / 48u)) {
            const dag::DagGroup& g = d.groups[gi];
            if (g.child_count == 0u || !(g.bounds.error > 0.f)) {
                continue;
            }
            f32 cam[3];
            const f32 dirs[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f}};
            const f32 k = 1.f + 0.5f * static_cast<f32>(ties % 5u);
            for (u32 a = 0; a < 3u; ++a) {
                cam[a] = g.bounds.center[a] + dirs[ties % 3u][a] * g.bounds.radius * k;
            }
            cut::DagView v = cut::make_dag_view(cam, projY, height, 1.f, znear);
            const f32 dx = g.bounds.center[0] - cam[0], dy = g.bounds.center[1] - cam[1], dz = g.bounds.center[2] - cam[2];
            const f32 dd = std::sqrt(dx * dx + dy * dy + dz * dz) - g.bounds.radius;
            v.threshold = g.bounds.error * v.error_scale / (dd > v.znear ? dd : v.znear);
            (void)run(v, "tie group " + std::to_string(gi));
            ++ties;
        }
        // Extremes: threshold 0 -> exactly the leaves (when every simplified group has error > 0);
        // huge threshold -> exactly the members of terminal groups.
        {
            bool allPositive = true;
            for (const dag::DagGroup& g : d.groups) {
                allPositive = allPositive && (g.child_count == 0u || g.bounds.error > 0.f);
            }
            const f32 cam[3] = {b.center[0], b.center[1] + 2.f * b.radius, b.center[2]};
            const CutCheck fine = run(cut::make_dag_view(cam, projY, height, 0.f, znear), "threshold 0");
            if (allPositive) {
                bool leavesOnly = fine.ids.size() == d.leaf_cluster_count;
                for (usize i = 0; i < fine.ids.size() && leavesOnly; ++i) {
                    leavesOnly = fine.ids[i] == i;
                }
                expect(leavesOnly, b.name + ": threshold 0 selects exactly the full-detail meshlets");
                expect(std::fabs(fine.area - b.source_area) <= 1e-9 * b.source_area, b.name + ": full-detail cut area == source area");
            }
            const CutCheck coarse = run(cut::make_dag_view(cam, projY, height, 3.0e38f, znear), "threshold max");
            std::vector<u32> roots;
            for (const dag::DagGroup& g : d.groups) {
                if (g.child_count == 0u) {
                    roots.insert(roots.end(), d.group_members.begin() + g.member_offset,
                                 d.group_members.begin() + g.member_offset + g.member_count);
                }
            }
            std::sort(roots.begin(), roots.end());
            expect(coarse.ids == roots, b.name + ": huge threshold selects exactly the terminal groups' members");
            if (b.expect_levels && b.reducible) {
                expect(coarse.triangles * 4u <= fine.triangles || fine.triangles <= 4u * geo::kMeshletMaxTriangles,
                       b.name + ": coarsest cut has <= 1/4 of the full-detail triangles");
            }
        }
        std::printf("  %-12s cut area / source area in [%.4f, %.4f]\n", b.name.c_str(), minRatio, maxRatio);
        expect(watertight, b.name + ": every cut is watertight (boundary only on the source's open border) " + firstBad);
        expect(areaOk, b.name + ": every cut's area within [0.7, 1.2] of the source area " + firstBad);
        expect(mono, b.name + ": acceptance monotone up the DAG for every view (incl. exact ties)");
        expect(triMono, b.name + ": cut triangle count non-increasing in the threshold");
        expect(parity, b.name + ": CpuReference cut == CpuParallel cut");
    }
    std::printf("  cut: %llu views, %u with clusters from >= 2 levels\n", static_cast<unsigned long long>(views), mixedViews);
    expect(mixedViews > 50u, "cut: many views select a mixed-level cut");
    fuse::kernel::KernelStats stats{};
    expect(fuse::kernel::find_kernel_stats(cut::kName, stats) && stats.launches > 0u && stats.items > 0u,
           "kernel stats recorded for geometry_dag_cut");
    scheduler.shutdown();
}

// ---- format -----------------------------------------------------------------------------------------

bool parse_code(const std::vector<u8>& bytes, geo::MeshletFormatError expected, const std::string& what) {
    dag::ClusterDagMesh out;
    geo::MeshletFormatError code = geo::MeshletFormatError::None;
    std::string error;
    const bool ok = dag::parse_cluster_dag_mesh(bytes.data(), bytes.size(), out, &error, &code);
    return expect(!ok && code == expected, "format: " + what + " rejected as " + geo::meshlet_format_error_name(expected) +
                                               " (got " + geo::meshlet_format_error_name(code) + ": " + error + ")");
}

/// Split, edit at chunk level, reassemble (with a valid checksum).
template <typename Edit>
std::vector<u8> edit_chunks(const std::vector<u8>& bytes, Edit edit) {
    dag::FmltFile f;
    (void)dag::split_fmlt(bytes.data(), bytes.size(), f);
    edit(f);
    return dag::assemble_fmlt(f);
}

bool dag_equal_ignoring_minor(const dag::ClusterDagMesh& a, const dag::ClusterDagMesh& b) {
    dag::ClusterDagMesh x = a;
    x.base.version_minor = b.base.version_minor;
    return dag::cluster_dag_mesh_equal(x, b);
}

void set_u32(std::vector<u8>& payload, usize offset, u32 v) {
    for (u32 s = 0; s < 4u; ++s) {
        payload[offset + s] = static_cast<u8>((v >> (8u * s)) & 0xFFu);
    }
}

void suite_format() {
    Built bumpy, tri;
    const f32 origin[3] = {0.f, 0.f, 0.f};
    if (!build(make_icosphere(4, 1.f, origin, 0.1f, "bumpy"), kernel::Backend::CpuParallel, bumpy) ||
        !build(make_single_triangle(), kernel::Backend::CpuParallel, tri)) {
        return;
    }
    for (const Built* b : {&bumpy, &tri}) {
        const std::vector<u8> bytes = dag::serialize_cluster_dag_mesh(b->mesh);
        dag::ClusterDagMesh parsed;
        std::string error;
        expect(dag::parse_cluster_dag_mesh(bytes.data(), bytes.size(), parsed, &error), b->name + ": format: parse (" + error + ")");
        expect(dag::cluster_dag_mesh_equal(parsed, b->mesh), b->name + ": format: parsed == built (bitwise)");
        expect(dag::serialize_cluster_dag_mesh(parsed) == bytes, b->name + ": format: re-serialize is bit-exact");
        expect(parsed.base.version_minor == dag::kClusterDagFormatVersionMinor, b->name + ": format: header minor version 1");
        // The WP-1.2 1.0 reader reads the full-detail mesh and skips the DAG chunks.
        geo::MeshletMesh plain;
        expect(geo::parse_meshlet_mesh(bytes.data(), bytes.size(), plain, &error),
               b->name + ": format: WP-1.2 reader accepts FMLT 1.1 (" + error + ")");
        expect(geo::meshlet_mesh_equal(plain, b->mesh.base), b->name + ": format: WP-1.2 reader sees the full-detail mesh");
        // A plain 1.0 file has no DAG.
        geo::MeshletMesh v10 = b->mesh.base;
        v10.version_minor = 0u;
        parse_code(geo::serialize_meshlet_mesh(v10), geo::MeshletFormatError::MissingChunk, b->name + " FMLT 1.0 without DAG");
        // Every truncation (sampled for large files) and random bit flips.
        u32 truncRejected = 0, truncTotal = 0;
        const usize step = bytes.size() > 8192u ? bytes.size() / 2048u : 1u;
        for (usize len = 0; len < bytes.size(); len += step) {
            dag::ClusterDagMesh out;
            ++truncTotal;
            truncRejected += dag::parse_cluster_dag_mesh(bytes.data(), len, out) ? 0u : 1u;
        }
        expect(truncRejected == truncTotal, b->name + ": format: every truncation rejected (" + std::to_string(truncTotal) + ")");
        Rng rng(77u);
        u32 flipRejected = 0;
        for (u32 i = 0; i < 400u; ++i) {
            std::vector<u8> c = bytes;
            c[rng.next_u32() % c.size()] ^= static_cast<u8>(1u << (rng.next_u32() % 8u));
            dag::ClusterDagMesh out;
            flipRejected += dag::parse_cluster_dag_mesh(c.data(), c.size(), out) ? 0u : 1u;
        }
        expect(flipRejected == 400u, b->name + ": format: 400 random bit flips rejected");
    }

    const dag::ClusterDagMesh& ref = bumpy.mesh;
    const std::vector<u8> bytes = dag::serialize_cluster_dag_mesh(ref);
    auto accepts = [&](const std::vector<u8>& b, const std::string& what) {
        dag::ClusterDagMesh out;
        geo::MeshletMesh plain;
        std::string error;
        const bool ok = dag::parse_cluster_dag_mesh(b.data(), b.size(), out, &error);
        expect(ok && dag_equal_ignoring_minor(out, ref), "format: " + what + " accepted by the DAG reader (" + error + ")");
        expect(geo::parse_meshlet_mesh(b.data(), b.size(), plain, &error), "format: " + what + " accepted by the WP-1.2 reader");
    };
    const u32 zzzz = dag::fmlt_fourcc('Z', 'Z', 'Z', 'Z');
    const u32 dagh = dag::fmlt_fourcc('D', 'A', 'G', 'H');
    const u32 dgrp = dag::fmlt_fourcc('D', 'G', 'R', 'P');
    const u32 dclk = dag::fmlt_fourcc('D', 'C', 'L', 'K');
    const u32 dmsh = dag::fmlt_fourcc('D', 'M', 'S', 'H');
    accepts(edit_chunks(bytes, [&](dag::FmltFile& f) { f.chunks.push_back({zzzz, 3u, 5u, std::vector<u8>(15u, 0xABu)}); }),
            "unknown chunk at the end");
    accepts(edit_chunks(bytes, [&](dag::FmltFile& f) {
                const s32 at = dag::fmlt_find_chunk(f, dgrp);
                f.chunks.insert(f.chunks.begin() + at, {zzzz, 16u, 2u, std::vector<u8>(32u, 0x5Au)});
                f.chunks.insert(f.chunks.begin() + 2, {zzzz + 1u, 1u, 1u, std::vector<u8>(1u, 0x01u)});
            }),
            "unknown chunks between DAG and 1.0 chunks");
    accepts(edit_chunks(bytes, [&](dag::FmltFile& f) { dag::fmlt_set_version_minor(f, 7u); }), "minor version 7");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { dag::fmlt_set_version_major(f, 2u); }),
               geo::MeshletFormatError::UnsupportedVersion, "major version 2");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { dag::fmlt_set_version_minor(f, 0u); }), geo::MeshletFormatError::BadHeader,
               "DAG chunks in a minor-0 file");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { f.chunks.push_back(f.chunks[static_cast<usize>(dag::fmlt_find_chunk(f, dgrp))]); }),
               geo::MeshletFormatError::DuplicateChunk, "duplicate DGRP");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { f.chunks.erase(f.chunks.begin() + dag::fmlt_find_chunk(f, dclk)); }),
               geo::MeshletFormatError::MissingChunk, "missing DCLK");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) {
                   dag::FmltChunk& c = f.chunks[static_cast<usize>(dag::fmlt_find_chunk(f, dgrp))];
                   c.element_bytes = 24u;
                   c.element_count *= 2u;
               }),
               geo::MeshletFormatError::BadElementSize, "DGRP with 24-byte elements");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) {
                   dag::FmltChunk& c = f.chunks[static_cast<usize>(dag::fmlt_find_chunk(f, dmsh))];
                   c.element_count -= 1u;
                   c.payload.resize(c.payload.size() - 96u);
               }),
               geo::MeshletFormatError::BadElementSize, "DMSH count below DAGH");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { set_u32(f.chunks[static_cast<usize>(dag::fmlt_find_chunk(f, dagh))].payload, 28u, 1u); }),
               geo::MeshletFormatError::BadHeader, "DAGH reserved field");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { set_u32(f.chunks[static_cast<usize>(dag::fmlt_find_chunk(f, dagh))].payload, 0u, 1u); }),
               geo::MeshletFormatError::BadElementSize, "DAGH leaf count mismatch");
    parse_code(edit_chunks(bytes, [&](dag::FmltFile& f) { set_u32(f.chunks[static_cast<usize>(dag::fmlt_find_chunk(f, dmsh))].payload, 84u, 9u); }),
               geo::MeshletFormatError::Invalid, "DMSH reserved field");

    // Semantic corruptions: serialise a broken DAG (the writer does not validate), expect Invalid.
    const dag::ClusterDag& d = ref.dag;
    u32 lodChild = 0; // an LOD cluster whose group is not terminal
    u32 simplifiedGroup = 0;
    for (u32 c = d.leaf_cluster_count; c < d.cluster_count(); ++c) {
        if (d.groups[d.links[c].group].child_count != 0u) {
            lodChild = c;
            break;
        }
    }
    for (u32 g = 0; g < d.groups.size(); ++g) {
        if (d.groups[g].child_count != 0u) {
            simplifiedGroup = g;
            break;
        }
    }
    expect(lodChild != 0u, "format: reference DAG has an LOD cluster inside a simplified group");
    auto corrupt = [&](const std::string& what, auto mutate) {
        dag::ClusterDagMesh m = ref;
        mutate(m.dag);
        parse_code(dag::serialize_cluster_dag_mesh(m), geo::MeshletFormatError::Invalid, what);
        std::string why;
        expect(!dag::validate_cluster_dag(m.base, m.dag, &why), "validate: " + what + " rejected");
    };
    const auto setGroupBounds = [](dag::ClusterDag& x, u32 g, const dag::DagLodBounds& b) {
        x.groups[g].bounds = b;
        for (u32 c = 0; c < x.links.size(); ++c) {
            if (x.links[c].group == g) {
                x.links[c].parent = b;
            }
            if (x.links[c].refined == g) {
                x.links[c].self = b;
            }
        }
    };
    corrupt("error not monotonic (group below its member)", [&](dag::ClusterDag& x) {
        const u32 g = x.links[lodChild].group;
        dag::DagLodBounds b = x.groups[g].bounds;
        b.error = x.links[lodChild].self.error * 0.5f;
        setGroupBounds(x, g, b);
    });
    corrupt("group sphere does not contain a member sphere", [&](dag::ClusterDag& x) {
        const u32 g = x.links[lodChild].group;
        dag::DagLodBounds b = x.groups[g].bounds;
        b.radius = x.links[lodChild].self.radius * 0.5f;
        setGroupBounds(x, g, b);
    });
    corrupt("link parent differs from the group", [&](dag::ClusterDag& x) { x.links[lodChild].parent.error += 1.f; });
    corrupt("link self differs from the producer", [&](dag::ClusterDag& x) { x.links[lodChild].self.radius += 1.f; });
    corrupt("leaf self error non-zero", [&](dag::ClusterDag& x) { x.links[0].self.error = 1.f; });
    corrupt("cluster in two groups", [&](dag::ClusterDag& x) { x.group_members[1] = x.group_members[0]; });
    corrupt("link group wrong", [&](dag::ClusterDag& x) { x.links[0].group = (x.links[0].group + 1u) % static_cast<u32>(x.groups.size()); });
    corrupt("link refined wrong", [&](dag::ClusterDag& x) { x.links[lodChild].refined = dag::kDagNoGroup; });
    corrupt("terminal group with children", [&](dag::ClusterDag& x) {
        dag::DagLodBounds b = x.groups[simplifiedGroup].bounds;
        b.error = dag::kDagErrorTerminal;
        setGroupBounds(x, simplifiedGroup, b);
    });
    corrupt("simplified group with non-finite error", [&](dag::ClusterDag& x) {
        dag::DagLodBounds b = x.groups[simplifiedGroup].bounds;
        b.error = std::numeric_limits<f32>::infinity();
        setGroupBounds(x, simplifiedGroup, b);
    });
    corrupt("child range gap", [&](dag::ClusterDag& x) { x.groups[simplifiedGroup].child_offset += 1u; });
    corrupt("LOD micro-index out of range", [&](dag::ClusterDag& x) { x.lod_meshlet_triangles[0] = geo::pack_triangle(0u, 1u, 250u); });
    corrupt("LOD vertex index out of range", [&](dag::ClusterDag& x) { x.lod_meshlet_vertices[0] = 0xFFFFFFu; });
    corrupt("level_count wrong", [&](dag::ClusterDag& x) { x.level_count += 1u; });
    corrupt("producer depth not below the consumer", [&](dag::ClusterDag& x) { x.groups[x.links[lodChild].refined].depth = 1000u; });

    // Files.
    const fs::path dir = fs::temp_directory_path() / "fuse_rp_cluster_dag";
    const std::string path = (dir / "bumpy.fusemeshlet").string();
    std::string error;
    dag::ClusterDagMesh loaded;
    expect(dag::write_cluster_dag_file(path, ref, &error) && dag::load_cluster_dag_file(path, loaded, &error) &&
               dag::cluster_dag_mesh_equal(loaded, ref),
           "format: file write / load round trip (" + error + ")");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- determinism ------------------------------------------------------------------------------------

void suite_determinism() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (const TestMesh& m : procedural_set()) {
        scheduler.shutdown();
        Built ref;
        if (!build(m, kernel::Backend::CpuReference, ref)) {
            continue;
        }
        const std::vector<u8> refBytes = dag::serialize_cluster_dag_mesh(ref.mesh);
        Built again;
        if (build(m, kernel::Backend::CpuReference, again)) {
            expect(dag::serialize_cluster_dag_mesh(again.mesh) == refBytes, m.name + ": same bytes on a second build");
        }
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            Built par;
            if (build(m, kernel::Backend::CpuParallel, par)) {
                expect(dag::serialize_cluster_dag_mesh(par.mesh) == refBytes,
                       m.name + ": CpuParallel (" + std::to_string(workers) + " workers) bytes == CpuReference bytes");
            }
        }
        // DAG built from a parsed base == DAG built from the in-memory base.
        geo::MeshletMesh parsed;
        const std::vector<u8> baseBytes = geo::serialize_meshlet_mesh(ref.mesh.base);
        std::string error;
        dag::ClusterDag fromParsed;
        expect(geo::parse_meshlet_mesh(baseBytes.data(), baseBytes.size(), parsed, &error) &&
                   dag::build_cluster_dag(parsed, test_options(kernel::Backend::CpuParallel), fromParsed, &error),
               m.name + ": DAG from the parsed base builds (" + error + ")");
        dag::ClusterDagMesh viaParsed{parsed, fromParsed};
        expect(dag::serialize_cluster_dag_mesh(viaParsed) == refBytes, m.name + ": DAG from the parsed base == DAG from the build");
    }
    scheduler.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "build" || suite == "all") {
        suite_build();
    }
    if (suite == "crack" || suite == "all") {
        suite_crack();
    }
    if (suite == "cut" || suite == "all") {
        suite_cut();
    }
    if (suite == "format" || suite == "all") {
        suite_format();
    }
    if (suite == "determinism" || suite == "all") {
        suite_determinism();
    }
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d cluster DAG check(s) failed (suite %s)\n", g_failures, g_checks, suite.c_str());
        return EXIT_FAILURE;
    }
    std::printf("cluster DAG gates passed: %d checks (suite %s)\n", g_checks, suite.c_str());
    return EXIT_SUCCESS;
}
