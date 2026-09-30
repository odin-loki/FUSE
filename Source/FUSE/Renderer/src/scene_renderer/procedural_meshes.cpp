// E02 procedural meshes: see include/fuse/renderer/scene_renderer/procedural_meshes.hpp.
#include <fuse/renderer/scene_renderer/procedural_meshes.hpp>

#include <fuse/renderer/geometry/meshlet_builder.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::renderer::scene_renderer {

namespace {

constexpr f32 kPi = 3.14159265358979f;

struct Source {
    std::vector<f32> positions;
    std::vector<f32> normals;
    std::vector<f32> uvs;
    std::vector<u32> indices;
    void vertex(f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
        positions.insert(positions.end(), {x, y, z});
        normals.insert(normals.end(), {nx, ny, nz});
        uvs.insert(uvs.end(), {u, v});
    }
};

void cube(Source& m, f32 e) {
    struct Face {
        f32 n[3];
        f32 u[3];
        f32 v[3];
    };
    const Face faces[6] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                           {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                           {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (const Face& fc : faces) {
        const u32 base = static_cast<u32>(m.positions.size() / 3u);
        for (u32 c = 0; c < 4u; ++c) {
            const f32 su = (c == 1u || c == 2u) ? 1.f : -1.f;
            const f32 sv = (c >= 2u) ? 1.f : -1.f;
            m.vertex(e * (fc.n[0] + su * fc.u[0] + sv * fc.v[0]), e * (fc.n[1] + su * fc.u[1] + sv * fc.v[1]),
                     e * (fc.n[2] + su * fc.u[2] + sv * fc.v[2]), fc.n[0], fc.n[1], fc.n[2], su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
        }
        m.indices.insert(m.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    }
}

void plane(Source& m, u32 n, f32 size, f32 tiles) {
    for (u32 j = 0; j <= n; ++j) {
        for (u32 i = 0; i <= n; ++i) {
            const f32 u = static_cast<f32>(i) / static_cast<f32>(n);
            const f32 v = static_cast<f32>(j) / static_cast<f32>(n);
            m.vertex((u - 0.5f) * size, 0.f, (v - 0.5f) * size, 0.f, 1.f, 0.f, u * tiles, v * tiles);
        }
    }
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const u32 a = j * (n + 1u) + i;
            const u32 b = a + n + 1u;
            m.indices.insert(m.indices.end(), {a, b, a + 1u, a + 1u, b, b + 1u});
        }
    }
}

void sphere(Source& m, u32 rings, u32 segments, f32 radius) {
    for (u32 r = 0; r <= rings; ++r) {
        const f32 theta = kPi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 phi = 2.f * kPi * static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 nx = std::sin(theta) * std::cos(phi);
            const f32 ny = std::cos(theta);
            const f32 nz = std::sin(theta) * std::sin(phi);
            m.vertex(radius * nx, radius * ny, radius * nz, nx, ny, nz, static_cast<f32>(s) / static_cast<f32>(segments),
                     static_cast<f32>(r) / static_cast<f32>(rings));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1u) + s;
            const u32 b = a + segments + 1u;
            if (r != 0u) {
                m.indices.insert(m.indices.end(), {a, b, a + 1u});
            }
            if (r + 1u != rings) {
                m.indices.insert(m.indices.end(), {a + 1u, b, b + 1u});
            }
        }
    }
}

} // namespace

bool buildProceduralMesh(const ProceduralMeshDesc& desc, geometry::MeshletMesh& out, f32* localMin, f32* localMax,
                         std::string* error) {
    Source m;
    f32 lo[3] = {0.f, 0.f, 0.f};
    f32 hi[3] = {0.f, 0.f, 0.f};
    switch (desc.shape) {
    case ProceduralShape::Cube: {
        if (!(desc.halfExtent > 0.f)) {
            if (error != nullptr) {
                *error = "cube: halfExtent must be > 0";
            }
            return false;
        }
        cube(m, desc.halfExtent);
        for (u32 a = 0; a < 3u; ++a) {
            lo[a] = -desc.halfExtent;
            hi[a] = desc.halfExtent;
        }
        break;
    }
    case ProceduralShape::Plane: {
        if (!(desc.size > 0.f) || desc.segments == 0u) {
            if (error != nullptr) {
                *error = "plane: size must be > 0 and segments >= 1";
            }
            return false;
        }
        plane(m, desc.segments, desc.size, desc.uvTiles);
        lo[0] = lo[2] = -0.5f * desc.size;
        hi[0] = hi[2] = 0.5f * desc.size;
        break;
    }
    case ProceduralShape::Sphere: {
        if (!(desc.radius > 0.f) || desc.segments < 3u || desc.rings < 2u) {
            if (error != nullptr) {
                *error = "sphere: radius must be > 0, segments >= 3, rings >= 2";
            }
            return false;
        }
        sphere(m, desc.rings, desc.segments, desc.radius);
        for (u32 a = 0; a < 3u; ++a) {
            lo[a] = -desc.radius;
            hi[a] = desc.radius;
        }
        break;
    }
    }
    geometry::MeshletSource s{};
    s.positions = m.positions.data();
    s.normals = m.normals.data();
    s.uvs = m.uvs.data();
    s.vertex_count = static_cast<u32>(m.positions.size() / 3u);
    s.indices = m.indices.data();
    s.index_count = static_cast<u32>(m.indices.size());
    geometry::MeshletBuildOptions options{};
    options.backend = kernel::Backend::CpuReference;
    if (!geometry::build_meshlets(s, options, out, error)) {
        return false;
    }
    if (localMin != nullptr && localMax != nullptr) {
        for (u32 a = 0; a < 3u; ++a) {
            localMin[a] = lo[a];
            localMax[a] = hi[a];
        }
    }
    return true;
}

} // namespace fuse::renderer::scene_renderer
