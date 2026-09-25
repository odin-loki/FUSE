// WP-1.2 meshlet cook gates (CPU only; runs in the stub build). Suites (argv[1]):
//   codec   half / oct / position codec: exact decode contract, re-encode idempotence, error bounds
//   build   procedural mesh set: every triangle in exactly one meshlet (winding kept), limits,
//           bounds contain the decoded vertices, cone + frustum culling conservative, vertex fetch
//           order, determinism across CpuReference / CpuParallel (0, 2, 4 workers), kernel stats
//   format  bit-exact round trip; corrupt, truncated and version-wrong input rejected with the
//           right error; forward-compatible unknown chunks / minor versions accepted
//   cook    FMSH v1 hook: .fusemesh bytes unchanged by the hook, sidecar written, stale detection;
//           existing sample meshes (Templates/BaseGame primitives, assimp glTF samples) when assimp
//           is linked (otherwise that part is skipped and reported)

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/geometry/meshlet_bounds_kernel.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/geometry/meshlet_cook_hook.hpp>
#include <fuse/renderer/geometry/vertex_codec_kernel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
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
namespace codec = fuse::renderer::geometry::vertex_codec;
namespace cull = fuse::renderer::geometry::cull_kernel;
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
    std::vector<f32> positions, normals, uvs, tangents;
    std::vector<u32> indices;
    std::vector<geo::MeshletSourceSubmesh> submeshes;
    bool closed = false; ///< expect some cone culling from outside

    u32 vertex_count() const { return static_cast<u32>(positions.size() / 3u); }
    geo::MeshletSource source() const {
        geo::MeshletSource s;
        s.positions = positions.data();
        s.normals = normals.empty() ? nullptr : normals.data();
        s.uvs = uvs.empty() ? nullptr : uvs.data();
        s.tangents = tangents.empty() ? nullptr : tangents.data();
        s.vertex_count = vertex_count();
        s.indices = indices.data();
        s.index_count = static_cast<u32>(indices.size());
        s.submeshes = submeshes;
        return s;
    }
};

void push_vertex(TestMesh& m, f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
    m.positions.insert(m.positions.end(), {x, y, z});
    m.normals.insert(m.normals.end(), {nx, ny, nz});
    m.uvs.insert(m.uvs.end(), {u, v});
}

TestMesh make_grid(u32 nx, u32 ny) {
    TestMesh m;
    m.name = "grid" + std::to_string(nx) + "x" + std::to_string(ny);
    for (u32 y = 0; y <= ny; ++y) {
        for (u32 x = 0; x <= nx; ++x) {
            push_vertex(m, static_cast<f32>(x) * 0.1f, 0.f, static_cast<f32>(y) * 0.1f, 0.f, 1.f, 0.f,
                        static_cast<f32>(x) / static_cast<f32>(nx), static_cast<f32>(y) / static_cast<f32>(ny));
        }
    }
    for (u32 y = 0; y < ny; ++y) {
        for (u32 x = 0; x < nx; ++x) {
            const u32 a = y * (nx + 1u) + x, b = a + 1u, c = a + nx + 1u, d = c + 1u;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d}); // CCW seen from +Y
        }
    }
    return m;
}

TestMesh make_sphere(u32 rings, u32 segments, f32 radius, const f32 center[3], const char* name) {
    TestMesh m;
    m.name = name;
    m.closed = true;
    const f32 pi = 3.14159265358979f;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 theta = pi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 phi = 2.f * pi * static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 nx = std::sin(theta) * std::cos(phi), ny = std::cos(theta), nz = std::sin(theta) * std::sin(phi);
            push_vertex(m, center[0] + radius * nx, center[1] + radius * ny, center[2] + radius * nz, nx, ny, nz,
                        static_cast<f32>(s) / static_cast<f32>(segments), static_cast<f32>(r) / static_cast<f32>(rings));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1u) + s, b = a + 1u, c = a + segments + 1u, d = c + 1u;
            // Outward-facing CCW (normal = cross(p1 - p0, p2 - p0) points out).
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
        const f32 u = 2.f * pi * static_cast<f32>(i) / static_cast<f32>(major);
        for (u32 j = 0; j <= minor; ++j) {
            const f32 v = 2.f * pi * static_cast<f32>(j) / static_cast<f32>(minor);
            const f32 nx = std::cos(v) * std::cos(u), ny = std::sin(v), nz = std::cos(v) * std::sin(u);
            push_vertex(m, (R + r * std::cos(v)) * std::cos(u), r * std::sin(v), (R + r * std::cos(v)) * std::sin(u), nx, ny,
                        nz, static_cast<f32>(i) / static_cast<f32>(major), static_cast<f32>(j) / static_cast<f32>(minor));
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

/// Hard-edged cube, one submesh (material) per face, each face subdivided n x n.
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
                push_vertex(m, nrm[0] + s * tu[0] + t * tv[0], nrm[1] + s * tu[1] + t * tv[1], nrm[2] + s * tu[2] + t * tv[2],
                            nrm[0], nrm[1], nrm[2], (s + 1.f) * 0.5f, (t + 1.f) * 0.5f);
            }
        }
        geo::MeshletSourceSubmesh sub{static_cast<u32>(m.indices.size()), 0u, 10u + f};
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                const u32 a = base + y * (n + 1u) + x, b = a + 1u, c = a + n + 1u, d = c + 1u;
                m.indices.insert(m.indices.end(), {a, b, c, b, d, c}); // CCW around +normal (tu x tv = n)
            }
        }
        sub.index_count = static_cast<u32>(m.indices.size()) - sub.index_offset;
        m.submeshes.push_back(sub);
    }
    return m;
}

/// Random soup: unused vertices, degenerate and duplicated triangles, no normals / uvs.
TestMesh make_soup(u32 vertices, u32 triangles, u32 seed) {
    TestMesh m;
    m.name = "soup" + std::to_string(seed);
    Rng rng(seed);
    for (u32 v = 0; v < vertices; ++v) {
        m.positions.insert(m.positions.end(), {rng.range(-3.f, 3.f), rng.range(-1.f, 1.f), rng.range(-2.f, 5.f)});
    }
    for (u32 t = 0; t < triangles; ++t) {
        const u32 a = rng.next_u32() % (vertices - vertices / 8u);
        u32 b = rng.next_u32() % (vertices - vertices / 8u);
        const u32 c = rng.next_u32() % (vertices - vertices / 8u);
        if (t % 17u == 0u) {
            b = a; // degenerate
        }
        m.indices.insert(m.indices.end(), {a, b, c});
        if (t % 23u == 0u) {
            m.indices.insert(m.indices.end(), {a, b, c}); // duplicate
            ++t;
        }
    }
    return m;
}

/// One hub vertex shared by many triangles (vertex-limit stress), with source tangents.
TestMesh make_fan(u32 blades) {
    TestMesh m;
    m.name = "fan";
    push_vertex(m, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.5f, 0.5f);
    for (u32 i = 0; i <= blades; ++i) {
        const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(blades);
        push_vertex(m, std::cos(a), std::sin(a), 0.1f * std::sin(5.f * a), 0.f, 0.f, 1.f, 0.5f + 0.5f * std::cos(a),
                    0.5f + 0.5f * std::sin(a));
    }
    for (u32 i = 0; i < blades; ++i) {
        m.indices.insert(m.indices.end(), {0u, i + 1u, i + 2u});
    }
    for (u32 v = 0; v < m.vertex_count(); ++v) {
        m.tangents.insert(m.tangents.end(), {1.f, 0.f, 0.f, v % 2u == 0u ? 1.f : -1.f});
    }
    return m;
}

std::vector<TestMesh> procedural_set() {
    std::vector<TestMesh> set;
    set.push_back(make_grid(40, 30));
    const f32 origin[3] = {0.f, 0.f, 0.f};
    set.push_back(make_sphere(24, 48, 1.f, origin, "sphere"));
    const f32 far[3] = {100000.f, -2500.5f, 31337.25f};
    set.push_back(make_sphere(16, 32, 3.f, far, "sphere_far")); // far from the origin: offset stress
    const f32 tinyOrigin[3] = {1e-3f, 2e-3f, -1e-3f};
    set.push_back(make_sphere(12, 24, 1e-4f, tinyOrigin, "sphere_tiny"));
    set.push_back(make_torus(64, 24));
    set.push_back(make_cube_submeshes(9));
    set.push_back(make_soup(700, 1500, 7));
    set.push_back(make_fan(300));
    TestMesh tri;
    tri.name = "single_triangle";
    push_vertex(tri, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f); // zero normal -> regenerated
    push_vertex(tri, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f);
    push_vertex(tri, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
    tri.indices = {0u, 1u, 2u};
    set.push_back(tri);
    return set;
}

// ---- checks -----------------------------------------------------------------------------------------

using Tri = std::array<u32, 3>;

Tri canonical(u32 a, u32 b, u32 c) {
    // Rotate so the smallest index comes first; keeps the winding.
    if (b < a && b <= c) {
        return {b, c, a};
    }
    if (c < a && c < b) {
        return {c, a, b};
    }
    return {a, b, c};
}

void cross3(const f64 a[3], const f64 b[3], f64 out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// Column-major perspective * look-at (Vulkan clip z in [0, w]).
void make_view_proj(const f32 eye[3], const f32 target[3], f32 fovY, f32 aspect, f32 zn, f32 zf, f32 out[16]) {
    f32 fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    f32 len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    for (f32& v : fwd) {
        v /= len;
    }
    const f32 upHint[3] = {std::fabs(fwd[1]) > 0.99f ? 1.f : 0.f, std::fabs(fwd[1]) > 0.99f ? 0.f : 1.f, 0.f};
    f32 right[3] = {fwd[1] * upHint[2] - fwd[2] * upHint[1], fwd[2] * upHint[0] - fwd[0] * upHint[2],
                    fwd[0] * upHint[1] - fwd[1] * upHint[0]};
    len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (f32& v : right) {
        v /= len;
    }
    const f32 up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                       right[0] * fwd[1] - right[1] * fwd[0]};
    // view rows: right, up, -fwd
    f32 view[16] = {};
    for (u32 c = 0; c < 3u; ++c) {
        view[c * 4u + 0u] = right[c];
        view[c * 4u + 1u] = up[c];
        view[c * 4u + 2u] = -fwd[c];
    }
    view[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    view[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    view[14] = fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2];
    view[15] = 1.f;
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    f32 proj[16] = {};
    proj[0] = f / aspect;
    proj[5] = f;
    proj[10] = zf / (zn - zf);
    proj[11] = -1.f;
    proj[14] = zn * zf / (zn - zf);
    for (u32 c = 0; c < 4u; ++c) {
        for (u32 r = 0; r < 4u; ++r) {
            f32 acc = 0.f;
            for (u32 k = 0; k < 4u; ++k) {
                acc += proj[k * 4u + r] * view[c * 4u + k];
            }
            out[c * 4u + r] = acc;
        }
    }
}

/// Every gate on one built mesh. `srcPositions` etc. describe the source the mesh came from.
void check_built(const std::string& name, const geo::MeshletMesh& mesh, const f32* srcPositions, u32 srcVertexCount,
                 const u32* srcIndices, u32 srcIndexCount, const std::vector<geo::MeshletSourceSubmesh>& srcSubmeshes,
                 bool closed) {
    std::string why;
    expect(geo::validate_meshlet_mesh(mesh, &why), name + ": validates (" + why + ")");
    expect(mesh.max_vertices == 64u && mesh.max_triangles == 124u, name + ": default limits 64 / 124");
    expect(mesh.source_vertices.size() == mesh.vertex_count(), name + ": VSRC present");

    // Limits.
    u32 overVertices = 0, overTriangles = 0;
    for (const geo::MeshletRecord& r : mesh.meshlets) {
        overVertices += r.vertex_count > 64u ? 1u : 0u;
        overTriangles += r.triangle_count > 124u ? 1u : 0u;
    }
    expect(overVertices == 0u && overTriangles == 0u, name + ": every meshlet within 64 vertices / 124 triangles");

    // Every source triangle in exactly one meshlet (as a multiset per submesh, winding preserved).
    std::vector<geo::MeshletSourceSubmesh> subs = srcSubmeshes;
    if (subs.empty()) {
        subs.push_back({0u, srcIndexCount, 0u});
    }
    bool coverage = mesh.submeshes.size() == subs.size();
    for (usize s = 0; coverage && s < subs.size(); ++s) {
        std::map<Tri, s32> count;
        for (u32 i = subs[s].index_offset; i < subs[s].index_offset + subs[s].index_count; i += 3u) {
            ++count[canonical(srcIndices[i], srcIndices[i + 1u], srcIndices[i + 2u])];
        }
        const geo::SubmeshRange& range = mesh.submeshes[s];
        coverage = coverage && range.material_index == subs[s].material_index;
        for (u32 mi = range.meshlet_offset; mi < range.meshlet_offset + range.meshlet_count; ++mi) {
            const geo::MeshletRecord& r = mesh.meshlets[mi];
            for (u32 t = 0; t < r.triangle_count; ++t) {
                const u32 packed = mesh.meshlet_triangles[r.triangle_offset + t];
                u32 v[3];
                for (u32 c = 0; c < 3u; ++c) {
                    v[c] = mesh.source_vertices[mesh.meshlet_vertices[r.vertex_offset + geo::triangle_index(packed, c)]];
                }
                --count[canonical(v[0], v[1], v[2])];
            }
        }
        for (const auto& [tri, c] : count) {
            if (c != 0) {
                coverage = false;
            }
        }
    }
    expect(coverage, name + ": every source triangle is in exactly one meshlet (winding kept, per submesh)");
    expect(mesh.triangle_count() == srcIndexCount / 3u, name + ": triangle count preserved");

    // Vertex fetch order: cooked vertices are numbered in first-use order of the meshlet stream.
    u32 nextNew = 0;
    bool fetchOrdered = true;
    for (const geo::MeshletRecord& r : mesh.meshlets) {
        for (u32 t = 0; t < r.triangle_count; ++t) {
            for (u32 c = 0; c < 3u; ++c) {
                const u32 v = mesh.meshlet_vertices[r.vertex_offset + geo::triangle_index(mesh.meshlet_triangles[r.triangle_offset + t], c)];
                if (v == nextNew) {
                    ++nextNew;
                } else if (v > nextNew) {
                    fetchOrdered = false;
                }
            }
        }
    }
    expect(fetchOrdered && nextNew == mesh.vertex_count(), name + ": vertices in meshlet first-use (fetch) order, none unused");

    // Decode and check quantisation error against the source.
    geo::DecodedVertices dec;
    geo::decode_vertices(mesh, dec, kernel::Backend::CpuReference);
    f64 maxErr[3] = {0.0, 0.0, 0.0};
    for (u32 v = 0; v < mesh.vertex_count(); ++v) {
        const u32 src = mesh.source_vertices[v];
        expect(src < srcVertexCount, name + ": VSRC in range");
        for (u32 a = 0; a < 3u; ++a) {
            maxErr[a] = std::max(maxErr[a], std::fabs(static_cast<f64>(dec.positions[v * 3u + a]) - srcPositions[src * 3u + a]));
        }
    }
    bool errOk = true;
    for (u32 a = 0; a < 3u; ++a) {
        errOk = errOk && maxErr[a] <= 0.5 * mesh.quant.step[a];
    }
    expect(errOk, name + ": position error <= step / 2 per axis");

    // Bounds contain every decoded vertex; AABB is tight.
    bool sphereOk = true, boxOk = true;
    for (const geo::MeshletRecord& r : mesh.meshlets) {
        f32 lo[3] = {0.f, 0.f, 0.f}, hi[3] = {0.f, 0.f, 0.f};
        for (u32 i = 0; i < r.vertex_count; ++i) {
            const u32 v = mesh.meshlet_vertices[r.vertex_offset + i];
            f64 d2 = 0.0;
            for (u32 a = 0; a < 3u; ++a) {
                const f32 x = dec.positions[v * 3u + a];
                lo[a] = i == 0u ? x : std::min(lo[a], x);
                hi[a] = i == 0u ? x : std::max(hi[a], x);
                const f64 d = static_cast<f64>(x) - r.center[a];
                d2 += d * d;
            }
            sphereOk = sphereOk && d2 <= static_cast<f64>(r.radius) * r.radius;
        }
        for (u32 a = 0; a < 3u; ++a) {
            boxOk = boxOk && lo[a] == r.aabb_min[a] && hi[a] == r.aabb_max[a];
        }
    }
    expect(sphereOk, name + ": bounding spheres contain every decoded vertex");
    expect(boxOk, name + ": AABBs are the exact box of the decoded vertices");

    // Cone culling is conservative: a culled meshlet has no triangle facing the camera.
    f32 lo[3] = {dec.positions[0], dec.positions[1], dec.positions[2]}, hi[3] = {lo[0], lo[1], lo[2]};
    for (u32 v = 0; v < mesh.vertex_count(); ++v) {
        for (u32 a = 0; a < 3u; ++a) {
            lo[a] = std::min(lo[a], dec.positions[v * 3u + a]);
            hi[a] = std::max(hi[a], dec.positions[v * 3u + a]);
        }
    }
    const f32 extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-6f});
    Rng rng(0xC0FFEEu + static_cast<u32>(name.size()));
    u32 coneCulled = 0, s8Culled = 0, coneViolations = 0, s8Violations = 0, frustumViolations = 0, frustumCulled = 0;
    std::vector<u32> results;
    for (u32 trial = 0; trial < 48u; ++trial) {
        f32 eye[3];
        for (u32 a = 0; a < 3u; ++a) {
            const f32 mid = 0.5f * (lo[a] + hi[a]);
            eye[a] = mid + extent * (trial < 8u ? rng.range(-0.6f, 0.6f) : rng.range(-3.f, 3.f));
        }
        f32 target[3];
        for (u32 a = 0; a < 3u; ++a) {
            target[a] = 0.5f * (lo[a] + hi[a]) + extent * rng.range(-0.8f, 0.8f);
        }
        if (std::fabs(target[0] - eye[0]) + std::fabs(target[1] - eye[1]) + std::fabs(target[2] - eye[2]) < 1e-3f * extent) {
            target[2] += extent;
        }
        f32 vp[16];
        make_view_proj(eye, target, 0.9f, 1.3f, 0.01f * extent, 20.f * extent, vp);
        const cull::CullView view = cull::make_cull_view(vp, eye, cull::kTestFrustum | cull::kTestCone | cull::kTestConeS8);
        geo::cull_meshlets(mesh, view, results, kernel::Backend::CpuReference);
        for (usize mi = 0; mi < mesh.meshlets.size(); ++mi) {
            const geo::MeshletRecord& r = mesh.meshlets[mi];
            const bool byCone = (results[mi] & cull::kCulledCone) != 0u;
            const bool byS8 = (results[mi] & cull::kCulledConeS8) != 0u;
            const bool byFrustum = (results[mi] & cull::kCulledFrustum) != 0u;
            coneCulled += byCone ? 1u : 0u;
            s8Culled += byS8 ? 1u : 0u;
            frustumCulled += byFrustum ? 1u : 0u;
            if (byCone || byS8) {
                for (u32 t = 0; t < r.triangle_count; ++t) {
                    const u32 packed = mesh.meshlet_triangles[r.triangle_offset + t];
                    f64 p[3][3];
                    for (u32 c = 0; c < 3u; ++c) {
                        const u32 v = mesh.meshlet_vertices[r.vertex_offset + geo::triangle_index(packed, c)];
                        for (u32 a = 0; a < 3u; ++a) {
                            p[c][a] = dec.positions[v * 3u + a];
                        }
                    }
                    const f64 e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
                    const f64 e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                    f64 n[3];
                    cross3(e1, e2, n);
                    const f64 toEye[3] = {eye[0] - p[0][0], eye[1] - p[0][1], eye[2] - p[0][2]};
                    const f64 nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    const f64 el = std::sqrt(toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2]);
                    if (nl == 0.0 || el == 0.0) {
                        continue;
                    }
                    const f64 facing = (n[0] * toEye[0] + n[1] * toEye[1] + n[2] * toEye[2]) / (nl * el);
                    if (facing > 1e-4) {
                        coneViolations += byCone ? 1u : 0u;
                        s8Violations += byS8 ? 1u : 0u;
                    }
                }
            }
            if (byFrustum) {
                // No vertex of a frustum-culled meshlet may be clearly inside all six planes.
                for (u32 i = 0; i < r.vertex_count; ++i) {
                    const u32 v = mesh.meshlet_vertices[r.vertex_offset + i];
                    f64 minDist = 1e30;
                    for (const auto& plane : view.planes) {
                        minDist = std::min(minDist, static_cast<f64>(plane[0]) * dec.positions[v * 3u] +
                                                        static_cast<f64>(plane[1]) * dec.positions[v * 3u + 1u] +
                                                        static_cast<f64>(plane[2]) * dec.positions[v * 3u + 2u] + plane[3]);
                    }
                    frustumViolations += minDist > 1e-4 * extent ? 1u : 0u;
                }
            }
        }
    }
    expect(coneViolations == 0u, name + ": apex cone culling is conservative (" + std::to_string(coneViolations) +
                                     " front-facing triangles in culled meshlets, " + std::to_string(coneCulled) + " culls)");
    expect(s8Violations == 0u, name + ": snorm8 cone culling is conservative (" + std::to_string(s8Violations) +
                                   " violations, " + std::to_string(s8Culled) + " culls)");
    expect(frustumViolations == 0u, name + ": frustum culling is conservative (" + std::to_string(frustumViolations) +
                                        " inside vertices in culled meshlets, " + std::to_string(frustumCulled) + " culls)");
    if (closed) {
        expect(coneCulled > 0u, name + ": cone test culls something on a closed mesh");
    }
}

geo::MeshletMesh build_or_fail(const TestMesh& m, const geo::MeshletBuildOptions& options = {}) {
    geo::MeshletMesh out;
    std::string error;
    expect(geo::build_meshlets(m.source(), options, out, &error), m.name + ": builds (" + error + ")");
    return out;
}

// ---- suites -----------------------------------------------------------------------------------------

void suite_codec() {
    // Half: every finite half re-encodes to itself (except subnormals, which the encoder never emits).
    u32 halfMismatch = 0;
    for (u32 h = 0; h < 0x10000u; ++h) {
        const u32 e = (h >> 10) & 0x1Fu;
        if (e == 31u || (e == 0u && (h & 0x3FFu) != 0u)) {
            continue;
        }
        halfMismatch += codec::float_to_half(codec::half_to_float(static_cast<u16>(h))) != h ? 1u : 0u;
    }
    expect(halfMismatch == 0u, "half: decode -> encode is the identity on normal halves and zeros");
    expect(codec::float_to_half(1e-6f) == 0u && codec::float_to_half(-1e-6f) == 0x8000u, "half: tiny values flush to signed zero");
    expect(codec::float_to_half(4e-5f) == 0x0400u, "half: [2^-15, 2^-14) rounds to the smallest normal (never subnormal)");
    expect(codec::float_to_half(65520.f) == 0x7C00u && codec::float_to_half(65504.f) == 0x7BFFu, "half: overflow boundary");
    expect(codec::float_to_half(1.f + 1.f / 2048.f) == 0x3C00u && codec::float_to_half(1.f + 3.f / 2048.f) == 0x3C02u,
           "half: round half to even");
    // Nearest: random floats are within half an ulp of their half encoding.
    Rng rng(3u);
    u32 halfFar = 0;
    for (u32 i = 0; i < 200000u; ++i) {
        const f32 v = rng.range(-70000.f, 70000.f) * (i % 3u == 0u ? 1e-3f : 1.f);
        const u16 h = codec::float_to_half(v);
        if ((h & 0x7C00u) == 0x7C00u || std::fabs(v) < 6.2e-5f) {
            continue;
        }
        const f32 d = codec::half_to_float(h);
        const f32 up = codec::half_to_float(static_cast<u16>(h + 1u));
        const f32 down = codec::half_to_float(static_cast<u16>(h - 1u));
        halfFar += (std::fabs(v - d) > std::fabs(v - up) || std::fabs(v - d) > std::fabs(v - down)) ? 1u : 0u;
    }
    expect(halfFar == 0u, "half: encoder picks the nearest half");

    // Oct: decode -> re-encode is the identity for encoded directions; angular error bound.
    u32 octMismatch = 0;
    f64 worstDot = 1.0;
    for (u32 i = 0; i < 200000u; ++i) {
        f32 v[3] = {rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)};
        if (i < 6u) {
            v[0] = v[1] = v[2] = 0.f;
            v[i / 2u] = (i % 2u) != 0u ? -1.f : 1.f; // axes, including -Z (octahedron fold)
        }
        const f32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-3f) {
            continue;
        }
        const u32 q = codec::oct_encode(v[0], v[1], v[2]);
        f32 d[3];
        codec::oct_decode(q, d);
        worstDot = std::min(worstDot, static_cast<f64>(d[0] * v[0] + d[1] * v[1] + d[2] * v[2]) / len);
        octMismatch += codec::oct_encode(d[0], d[1], d[2]) != q ? 1u : 0u;
    }
    expect(octMismatch == 0u, "oct: decode -> encode is the identity (" + std::to_string(octMismatch) + " mismatches)");
    expect(worstDot > 0.999999, "oct snorm16: angular error below ~0.08 degrees (worst dot " + std::to_string(worstDot) + ")");
    f32 zero[3];
    codec::oct_decode(codec::oct_encode(0.f, 0.f, 0.f), zero);
    expect(zero[2] == 1.f, "oct: zero vector encodes +Z");
    expect(codec::snorm16_to_float(0x8000u) == -1.f && codec::snorm16_to_float(0x7FFFu) <= 1.f &&
               codec::snorm16_to_float(0u) == 0.f,
           "oct: snorm16 decode clamps to [-1, 1] like GLSL unpackSnorm2x16");

    // Positions: quant params satisfy the exact-decode contract and decode/encode round trips.
    struct Range {
        f32 lo, hi;
    };
    const Range ranges[] = {{-1.f, 1.f}, {0.f, 0.f}, {100000.f, 100006.f}, {-3e7f, 2e7f}, {1e-4f, 1.1e-4f}, {-5.f, -5.f},
                            {12345.678f, 12345.678f}, {-1e-30f, 1e-30f}};
    bool contractOk = true, roundTrip = true, errorOk = true;
    for (const Range& r : ranges) {
        const f32 lo[3] = {r.lo, r.lo, r.lo}, hi[3] = {r.hi, r.hi, r.hi};
        const fuse::renderer::geometry::QuantParams q = geo::compute_quant_params(lo, hi);
        const f64 k = static_cast<f64>(q.offset[0]) / q.step[0];
        contractOk = contractOk && q.step[0] == std::ldexp(1.f, q.exponent[0]) && k == std::floor(k) &&
                     std::fabs(k) + 65535.0 < 16777216.0 && q.offset[0] <= r.lo &&
                     static_cast<f64>(q.offset[0]) + 65535.0 * q.step[0] >= r.hi;
        for (u32 i = 0; i <= 64u; ++i) {
            const f32 p = r.lo + (r.hi - r.lo) * static_cast<f32>(i) / 64.f;
            const u16 code = codec::quantize_position(p, q.offset[0], q.exponent[0]);
            const f32 d = codec::dequantize_position(code, q.offset[0], q.step[0]);
            // Exactness of the decode: the f64 evaluation equals the f32 one.
            roundTrip = roundTrip && static_cast<f64>(d) == static_cast<f64>(q.offset[0]) + static_cast<f64>(code) * q.step[0] &&
                        codec::quantize_position(d, q.offset[0], q.exponent[0]) == code;
            errorOk = errorOk && std::fabs(static_cast<f64>(d) - p) <= 0.5 * q.step[0];
        }
    }
    expect(contractOk, "quant: step = 2^e, offset = k * step with |k| + 65535 < 2^24, range covered");
    expect(roundTrip, "quant: decode is exact (no rounding) and decode -> encode is the identity");
    expect(errorOk, "quant: error <= step / 2");
}

void suite_build() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    fuse::kernel::reset_kernel_stats();
    for (const TestMesh& m : procedural_set()) {
        scheduler.shutdown();
        geo::MeshletBuildOptions ref;
        ref.backend = kernel::Backend::CpuReference;
        const geo::MeshletMesh built = build_or_fail(m, ref);
        if (built.meshlets.empty()) {
            continue;
        }
        check_built(m.name, built, m.positions.data(), m.vertex_count(), m.indices.data(),
                    static_cast<u32>(m.indices.size()), m.submeshes, m.closed);
        const std::vector<u8> refBytes = geo::serialize_meshlet_mesh(built);
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            geo::MeshletBuildOptions par;
            par.backend = kernel::Backend::CpuParallel;
            const geo::MeshletMesh again = build_or_fail(m, par);
            expect(geo::serialize_meshlet_mesh(again) == refBytes,
                   m.name + ": CpuParallel (" + std::to_string(workers) + " workers) bytes == CpuReference bytes");
        }
        // Normal / tangent error against the source normal (where given).
        geo::DecodedVertices dec;
        geo::decode_vertices(built, dec, kernel::Backend::CpuReference);
        f64 worst = 1.0;
        for (u32 v = 0; v < built.vertex_count() && !m.normals.empty(); ++v) {
            const u32 s = built.source_vertices[v];
            const f64 sl = std::sqrt(static_cast<f64>(m.normals[s * 3u]) * m.normals[s * 3u] +
                                     static_cast<f64>(m.normals[s * 3u + 1u]) * m.normals[s * 3u + 1u] +
                                     static_cast<f64>(m.normals[s * 3u + 2u]) * m.normals[s * 3u + 2u]);
            if (sl < 1e-6) {
                continue;
            }
            worst = std::min(worst, (dec.normals[v * 3u] * m.normals[s * 3u] + dec.normals[v * 3u + 1u] * m.normals[s * 3u + 1u] +
                                     dec.normals[v * 3u + 2u] * m.normals[s * 3u + 2u]) / sl);
        }
        expect(worst > 0.99999, m.name + ": decoded normals within oct16 precision of the source");
        if (!m.uvs.empty()) {
            bool uvOk = true;
            for (u32 v = 0; v < built.vertex_count(); ++v) {
                const u32 s = built.source_vertices[v];
                for (u32 a = 0; a < 2u; ++a) {
                    uvOk = uvOk && dec.uvs[v * 2u + a] == codec::half_to_float(codec::float_to_half(m.uvs[s * 2u + a]));
                }
            }
            expect(uvOk, m.name + ": uv0 decodes to the half-rounded source value");
        }
        if (!m.tangents.empty()) {
            bool signOk = true;
            for (u32 v = 0; v < built.vertex_count(); ++v) {
                signOk = signOk && dec.tangents[v * 4u + 3u] == (m.tangents[built.source_vertices[v] * 4u + 3u] < 0.f ? -1.f : 1.f);
            }
            expect(signOk && (built.flags & geo::kMeshletFlagTangentsFromSource) != 0u, m.name + ": source tangent handedness kept");
        } else {
            // Generated tangents are unit and orthogonal to the normal (to oct precision).
            f64 worstOrtho = 0.0;
            for (u32 v = 0; v < built.vertex_count(); ++v) {
                worstOrtho = std::max(worstOrtho, std::fabs(static_cast<f64>(dec.normals[v * 3u]) * dec.tangents[v * 4u] +
                                                            static_cast<f64>(dec.normals[v * 3u + 1u]) * dec.tangents[v * 4u + 1u] +
                                                            static_cast<f64>(dec.normals[v * 3u + 2u]) * dec.tangents[v * 4u + 2u]));
            }
            expect(worstOrtho < 1e-3, m.name + ": generated tangents orthogonal to normals");
        }
    }
    scheduler.shutdown();

    // Custom limits are honoured too.
    TestMesh sphere = make_sphere(20, 40, 1.f, std::array<f32, 3>{0.f, 0.f, 0.f}.data(), "sphere_small_limits");
    geo::MeshletBuildOptions small;
    small.max_vertices = 32u;
    small.max_triangles = 32u;
    const geo::MeshletMesh smallMesh = build_or_fail(sphere, small);
    bool smallOk = !smallMesh.meshlets.empty() && smallMesh.max_vertices == 32u && smallMesh.max_triangles == 32u;
    for (const geo::MeshletRecord& r : smallMesh.meshlets) {
        smallOk = smallOk && r.vertex_count <= 32u && r.triangle_count <= 32u;
    }
    expect(smallOk, "custom limits 32 / 32 respected");

    // Invalid input and options are rejected.
    geo::MeshletMesh rejected;
    TestMesh bad = make_grid(4, 4);
    bad.indices[5] = 1000u;
    expect(!geo::build_meshlets(bad.source(), {}, rejected), "out-of-range index rejected");
    bad = make_grid(4, 4);
    bad.positions[7] = std::nanf("");
    expect(!geo::build_meshlets(bad.source(), {}, rejected), "NaN position rejected");
    bad = make_grid(4, 4);
    bad.uvs[3] = 1e6f;
    expect(!geo::build_meshlets(bad.source(), {}, rejected), "uv outside half range rejected");
    bad = make_grid(4, 4);
    bad.indices.pop_back();
    expect(!geo::build_meshlets(bad.source(), {}, rejected), "index count not a multiple of 3 rejected");
    geo::MeshletBuildOptions badLimits;
    badLimits.max_triangles = 126u;
    expect(!geo::build_meshlets(make_grid(4, 4).source(), badLimits, rejected), "max_triangles not a multiple of 4 rejected");
    badLimits = {};
    badLimits.max_vertices = 256u;
    expect(!geo::build_meshlets(make_grid(4, 4).source(), badLimits, rejected), "max_vertices above 255 rejected");

    // Kernel stats: the four single-source kernels ran under their names.
    for (const char* name : {codec::kEncodeName, codec::kDecodeName, fuse::renderer::geometry::bounds_kernel::kName, cull::kName}) {
        fuse::kernel::KernelStats stats{};
        expect(fuse::kernel::find_kernel_stats(name, stats) && stats.launches > 0u && stats.items > 0u,
               std::string("kernel stats recorded for ") + name);
    }
    // Cull kernel parity across backends.
    const geo::MeshletMesh torus = build_or_fail(make_torus(48, 16));
    const f32 eye[3] = {3.f, 1.5f, -2.f}, target[3] = {0.f, 0.f, 0.f};
    f32 vp[16];
    make_view_proj(eye, target, 1.f, 1.f, 0.05f, 50.f, vp);
    const cull::CullView view = cull::make_cull_view(vp, eye, cull::kTestFrustum | cull::kTestCone | cull::kTestConeS8);
    std::vector<u32> a, b;
    geo::cull_meshlets(torus, view, a, kernel::Backend::CpuReference);
    scheduler.initialize(4u);
    geo::cull_meshlets(torus, view, b, kernel::Backend::CpuParallel);
    scheduler.shutdown();
    expect(a == b && !a.empty(), "cull: CpuReference == CpuParallel (4 workers)");
    std::vector<u32> gpuFallback;
    geo::cull_meshlets(torus, view, gpuFallback, kernel::Backend::Cuda);
    expect(gpuFallback == a, "cull: a Cuda request without a device falls back with the same result");
}

// Helpers to corrupt a serialized file while keeping it well-formed where needed.
u64 read_u64(const std::vector<u8>& b, usize at) {
    u64 v = 0;
    for (u32 i = 0; i < 8u; ++i) {
        v |= static_cast<u64>(b[at + i]) << (8u * i);
    }
    return v;
}

void write_u32(std::vector<u8>& b, usize at, u32 v) {
    for (u32 i = 0; i < 4u; ++i) {
        b[at + i] = static_cast<u8>(v >> (8u * i));
    }
}

void resign(std::vector<u8>& b) {
    const u64 h = geo::meshlet_fnv1a64(b.data(), b.size() - 8u);
    for (u32 i = 0; i < 8u; ++i) {
        b[b.size() - 8u + i] = static_cast<u8>(h >> (8u * i));
    }
}

/// Table entry offset of the chunk with `fourcc` (or 0).
usize find_chunk_entry(const std::vector<u8>& b, const char* fourcc) {
    const u32 count = static_cast<u32>(b[16]) | (static_cast<u32>(b[17]) << 8);
    for (u32 c = 0; c < count; ++c) {
        const usize at = 64u + static_cast<usize>(c) * 32u;
        if (std::memcmp(&b[at], fourcc, 4) == 0) {
            return at;
        }
    }
    return 0u;
}

geo::MeshletFormatError parse_code(const std::vector<u8>& b) {
    geo::MeshletMesh m;
    geo::MeshletFormatError code = geo::MeshletFormatError::None;
    (void)geo::parse_meshlet_mesh(b.data(), b.size(), m, nullptr, &code);
    return code;
}

void suite_format() {
    for (const TestMesh& m : procedural_set()) {
        const geo::MeshletMesh built = build_or_fail(m);
        const std::vector<u8> bytes = geo::serialize_meshlet_mesh(built);
        geo::MeshletMesh parsed;
        std::string error;
        expect(geo::parse_meshlet_mesh(bytes.data(), bytes.size(), parsed, &error), m.name + ": parses (" + error + ")");
        expect(geo::meshlet_mesh_equal(built, parsed), m.name + ": parsed mesh is bit-identical to the built one");
        expect(geo::serialize_meshlet_mesh(parsed) == bytes, m.name + ": re-serialized bytes identical");
        // Decode of the parsed mesh == decode of the built mesh (bitwise).
        geo::DecodedVertices a, b;
        geo::decode_vertices(built, a, kernel::Backend::CpuReference);
        geo::decode_vertices(parsed, b, kernel::Backend::CpuReference);
        expect(std::memcmp(a.positions.data(), b.positions.data(), a.positions.size() * 4u) == 0 &&
                   std::memcmp(a.normals.data(), b.normals.data(), a.normals.size() * 4u) == 0,
               m.name + ": decoded streams bit-identical after the round trip");
    }

    const geo::MeshletMesh mesh = build_or_fail(make_cube_submeshes(6));
    const std::vector<u8> good = geo::serialize_meshlet_mesh(mesh);
    using E = geo::MeshletFormatError;
    expect(parse_code(good) == E::None, "baseline parses");

    // Every truncation fails.
    u32 truncAccepted = 0;
    for (usize len = 0; len < good.size(); len += (len < 256u ? 1u : 97u)) {
        std::vector<u8> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
        truncAccepted += parse_code(t) == E::None ? 1u : 0u;
    }
    expect(truncAccepted == 0u, "every truncated file rejected");
    // Every single-byte corruption fails (checksum), sampled across the file.
    u32 flipAccepted = 0;
    for (usize at = 0; at < good.size(); at += (at < 512u ? 1u : 31u)) {
        std::vector<u8> c = good;
        c[at] ^= 0x5Au;
        flipAccepted += parse_code(c) == E::None ? 1u : 0u;
    }
    expect(flipAccepted == 0u, "every single-byte corruption rejected");

    std::vector<u8> c = good;
    c[0] = 'X';
    expect(parse_code(c) == E::BadMagic, "bad magic -> BadMagic");
    // An FMSH v1 file is not a meshlet file.
    fuse::cook::CookedMesh fmsh;
    fmsh.positions = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    fmsh.normals = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    fmsh.uvs = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    fmsh.indices = {0u, 1u, 2u};
    fmsh.submeshes.push_back({0u, 3u, 0u, 0u});
    expect(parse_code(fuse::cook::serialize_cooked_mesh(fmsh)) == E::BadMagic, "FMSH bytes -> BadMagic");

    c = good;
    c[4] = 2u; // major 2
    resign(c);
    expect(parse_code(c) == E::UnsupportedVersion, "version 2.x -> UnsupportedVersion");
    c = good;
    c[4] = 0u; // major 0
    resign(c);
    expect(parse_code(c) == E::UnsupportedVersion, "version 0.x -> UnsupportedVersion");
    c = good;
    c[6] = 7u; // minor 7: newer minor, same major -> accepted
    resign(c);
    geo::MeshletMesh minor;
    expect(geo::parse_meshlet_mesh(c.data(), c.size(), minor) && minor.version_minor == 7u, "version 1.7 accepted (minor bump)");
    c = good;
    std::memcpy(&c[find_chunk_entry(c, "VSRC")], "XTRA", 4);
    resign(c);
    geo::MeshletMesh unknown;
    expect(geo::parse_meshlet_mesh(c.data(), c.size(), unknown) && unknown.source_vertices.empty(),
           "unknown chunk skipped (forward compatible), optional VSRC absent");
    c = good;
    std::memcpy(&c[find_chunk_entry(c, "VSRC")], "VUV0", 4);
    resign(c);
    expect(parse_code(c) == E::DuplicateChunk, "known chunk twice -> DuplicateChunk");
    c = good;
    std::memcpy(&c[find_chunk_entry(c, "VNRM")], "XNRM", 4);
    resign(c);
    expect(parse_code(c) == E::MissingChunk, "required chunk missing -> MissingChunk");
    c = good;
    write_u32(c, 20u, mesh.vertex_count() + 1u); // header vertex count disagrees with the streams
    resign(c);
    expect(parse_code(c) == E::BadElementSize, "header count mismatch -> BadElementSize");
    c = good;
    write_u32(c, 8u, 72u); // header_bytes
    resign(c);
    expect(parse_code(c) == E::BadHeader, "header size -> BadHeader");
    c = good;
    write_u32(c, 52u, 1u); // reserved
    resign(c);
    expect(parse_code(c) == E::BadHeader, "reserved header field -> BadHeader");
    c = good;
    {
        const usize entry = find_chunk_entry(c, "MTRI");
        write_u32(c, entry + 16u, static_cast<u32>(read_u64(c, entry + 16u) + 4u)); // misaligned offset
    }
    resign(c);
    expect(parse_code(c) == E::BadChunkTable, "misaligned chunk -> BadChunkTable");

    // Semantic corruption with a valid checksum.
    const usize mtri = static_cast<usize>(read_u64(good, find_chunk_entry(good, "MTRI") + 16u));
    const usize mshl = static_cast<usize>(read_u64(good, find_chunk_entry(good, "MSHL") + 16u));
    const usize qprm = static_cast<usize>(read_u64(good, find_chunk_entry(good, "QPRM") + 16u));
    const usize mvrt = static_cast<usize>(read_u64(good, find_chunk_entry(good, "MVRT") + 16u));
    c = good;
    write_u32(c, mtri, geo::pack_triangle(0u, 1u, 250u));
    resign(c);
    expect(parse_code(c) == E::Invalid, "micro-index past the meshlet's vertices -> Invalid");
    c = good;
    c[mshl + 9u] = 200u; // triangle_count of meshlet 0 above the 124 limit
    resign(c);
    expect(parse_code(c) == E::Invalid, "meshlet above the triangle limit -> Invalid");
    c = good;
    c[36u] = 0u;
    c[37u] = 1u; // max_vertices 256 > 255
    resign(c);
    expect(parse_code(c) == E::Invalid, "limit above the format maximum -> Invalid");
    c = good;
    write_u32(c, qprm + 24u, 0x3F400000u); // step[0] = 0.75, not a power of two
    resign(c);
    expect(parse_code(c) == E::Invalid, "quant step not 2^e -> Invalid");
    c = good;
    write_u32(c, mvrt, 0xFFFFFFu);
    resign(c);
    expect(parse_code(c) == E::Invalid, "meshlet vertex index out of range -> Invalid");
    c = good;
    write_u32(c, mshl + 24u, 0xBF800000u); // radius -1
    resign(c);
    expect(parse_code(c) == E::Invalid, "negative radius -> Invalid");
    c = good;
    write_u32(c, mshl + 84u, 1u); // reserved meshlet word
    resign(c);
    expect(parse_code(c) == E::Invalid, "reserved meshlet field -> Invalid");
    c = good;
    c[mshl + 10u] = 3u; // meshlet 0 claims another submesh
    resign(c);
    expect(parse_code(c) == E::Invalid, "meshlet outside its submesh range -> Invalid");

    // Files: write + load, error names.
    const fs::path dir = fs::temp_directory_path() / "fuse_rp_meshlet_format";
    fs::create_directories(dir);
    const std::string path = (dir / "cube.fusemeshlet").string();
    geo::MeshletMesh loaded;
    expect(geo::write_meshlet_file(path, mesh) && geo::load_meshlet_file(path, loaded) && geo::meshlet_mesh_equal(mesh, loaded),
           "file write + load round trip");
    expect(!geo::load_meshlet_file((dir / "missing.fusemeshlet").string(), loaded), "missing file rejected");
    expect(std::string(geo::meshlet_format_error_name(E::UnsupportedVersion)) == "unsupported-version", "error names");
    // Compare as paths: operator/ joins with '\\' on Windows while the input keeps its '/'.
    expect(fs::path(geo::meshlet_sidecar_path("a/b/rock.fusemesh")) == fs::path("a/b") / "rock.fusemeshlet", "sidecar path");
    fs::remove_all(dir);
}

std::vector<u8> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void check_cooked_fmsh(const std::string& name, const fs::path& fusemesh) {
    fuse::cook::CookedMesh cm;
    std::string error;
    if (!expect(fuse::cook::load_cooked_mesh(fusemesh.string(), cm, &error), name + ": FMSH v1 still loads (" + error + ")")) {
        return;
    }
    geo::MeshletMesh sidecar;
    if (!expect(geo::load_meshlet_file(geo::meshlet_sidecar_path(fusemesh.string()), sidecar, &error),
                name + ": sidecar loads (" + error + ")")) {
        return;
    }
    expect(geo::meshlet_sidecar_matches(fusemesh.string(), &error), name + ": sidecar matches its FMSH (" + error + ")");
    std::vector<geo::MeshletSourceSubmesh> subs;
    for (const auto& s : cm.submeshes) {
        subs.push_back({s.index_offset, s.index_count, s.material_index});
    }
    check_built(name, sidecar, cm.positions.data(), cm.vertex_count(), cm.indices.data(), static_cast<u32>(cm.indices.size()),
                subs, false);
    std::printf("  %s: %u vertices, %u triangles -> %zu meshlets (%zu submeshes)\n", name.c_str(), cm.vertex_count(),
                static_cast<u32>(cm.indices.size() / 3u), sidecar.meshlets.size(), sidecar.submeshes.size());
}

int suite_cook() {
    const fs::path dir = fs::temp_directory_path() / "fuse_rp_meshlet_cook";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Always: sidecar for an FMSH built in memory (no importer needed).
    const TestMesh torus = make_torus(40, 16);
    fuse::cook::CookedMesh cm;
    cm.positions = torus.positions;
    cm.normals = torus.normals;
    cm.uvs = torus.uvs;
    cm.indices = torus.indices;
    cm.submeshes.push_back({0u, static_cast<u32>(torus.indices.size()), 0u, 3u});
    const std::vector<u8> fmsh = fuse::cook::serialize_cooked_mesh(cm);
    const fs::path torusPath = dir / "torus.fusemesh";
    {
        std::ofstream out(torusPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(fmsh.data()), static_cast<std::streamsize>(fmsh.size()));
    }
    std::string error;
    expect(geo::cook_meshlet_sidecar(torusPath.string(), &error), "sidecar from an existing .fusemesh (" + error + ")");
    expect(read_file(torusPath) == fmsh, "FMSH v1 bytes untouched by the sidecar cook");
    check_cooked_fmsh("torus.fusemesh", torusPath);
    // Stale detection: re-cook the FMSH with different content, keep the old sidecar.
    cm.positions[0] += 0.5f;
    const std::vector<u8> fmsh2 = fuse::cook::serialize_cooked_mesh(cm);
    {
        std::ofstream out(torusPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(fmsh2.data()), static_cast<std::streamsize>(fmsh2.size()));
    }
    expect(!geo::meshlet_sidecar_matches(torusPath.string()), "stale sidecar detected (source hash)");
    // The post hook itself, called directly.
    std::string note;
    const fs::path hookOut = dir / "hook" / "torus.fusemesh";
    fs::create_directories(hookOut.parent_path());
    expect(geo::meshlet_cook_post_hook(cm, fmsh2, hookOut.string(), &note) && fs::exists(geo::meshlet_sidecar_path(hookOut.string())),
           "post hook writes the sidecar (" + note + ")");
    expect(geo::with_meshlet_sidecar().post_hook == &geo::meshlet_cook_post_hook && fuse::cook::MeshCookOptions{}.post_hook == nullptr,
           "hook is opt-in (default MeshCookOptions has none)");

#if defined(FUSE_HAS_ASSIMP)
    // OBJ through the real cook: FMSH bytes with the hook == FMSH bytes without it.
    const fs::path obj = dir / "quad.obj";
    {
        std::ofstream out(obj);
        out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0.5 0.5 0.3\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 0.5 0.5\n"
               "f 1/1 2/2 5/5\nf 2/2 3/3 5/5\nf 3/3 4/4 5/5\nf 4/4 1/1 5/5\n";
    }
    const fs::path plain = dir / "plain" / "quad.fusemesh";
    const fs::path hooked = dir / "hooked" / "quad.fusemesh";
    const auto r1 = fuse::cook::cook_mesh_file(obj.string(), plain.string());
    const auto r2 = fuse::cook::cook_mesh_file(obj.string(), hooked.string(), geo::with_meshlet_sidecar());
    expect(r1.ok && r2.ok, "OBJ cooks with and without the hook (" + r1.note + " | " + r2.note + ")");
    expect(read_file(plain) == read_file(hooked), "hook leaves the FMSH v1 bytes identical");
    expect(!fs::exists(geo::meshlet_sidecar_path(plain.string())) && fs::exists(geo::meshlet_sidecar_path(hooked.string())),
           "sidecar only when the hook is installed");
    check_cooked_fmsh("quad.obj", hooked);

    // Existing sample meshes in the repository.
    const fs::path root(FUSE_SOURCE_DIR);
    const char* samples[] = {
        "Templates/BaseGame/game/data/Prototyping/shapes/Primitives/CubePrimitive.fbx",
        "Templates/BaseGame/game/data/Prototyping/shapes/Primitives/SpherePrimitive.fbx",
        "Templates/BaseGame/game/data/Prototyping/shapes/Primitives/TorusPrimitive.fbx",
        "Templates/BaseGame/game/data/Prototyping/shapes/Primitives/ConePrimitive.fbx",
        "Templates/BaseGame/game/data/Prototyping/shapes/Primitives/CylinderPrimitive.fbx",
        "Templates/BaseGame/game/data/Prototyping/shapes/Primitives/ArrowPrimitive.fbx",
        "Templates/BaseGame/game/core/gameObjects/shapes/camera.fbx",
        "third_party/vendor/assimp/test/models/glTF2/BoxTextured-glTF/BoxTextured.gltf",
        "third_party/vendor/assimp/test/models/glTF/CesiumMilkTruck/CesiumMilkTruck.gltf",
    };
    u32 cooked = 0, present = 0;
    for (const char* rel : samples) {
        const fs::path src = root / rel;
        if (!fs::exists(src)) {
            continue;
        }
        ++present;
        const fs::path out = dir / "samples" / (src.stem().string() + ".fusemesh");
        const auto r = fuse::cook::cook_mesh_file(src.string(), out.string(), geo::with_meshlet_sidecar());
        if (!r.ok) {
            std::printf("  %s: not cooked (%s)\n", rel, r.note.c_str());
            continue;
        }
        ++cooked;
        check_cooked_fmsh(src.filename().string(), out);
    }
    expect(present == 0u || cooked > 0u, "at least one repository sample mesh cooks through the hook");
    std::printf("  repository samples: %u present, %u cooked with meshlets\n", present, cooked);
    fs::remove_all(dir);
    return 0;
#else
    fs::remove_all(dir);
    std::printf("  assimp not linked: importer-based cook and repository samples skipped\n");
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "codec" || suite == "all") {
        suite_codec();
    }
    if (suite == "build" || suite == "all") {
        suite_build();
    }
    if (suite == "format" || suite == "all") {
        suite_format();
    }
    if (suite == "cook" || suite == "all") {
        (void)suite_cook();
    }
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d meshlet cook check(s) failed (suite %s)\n", g_failures, g_checks, suite.c_str());
        return EXIT_FAILURE;
    }
    std::printf("meshlet cook gates passed: %d checks (suite %s)\n", g_checks, suite.c_str());
    return EXIT_SUCCESS;
}
