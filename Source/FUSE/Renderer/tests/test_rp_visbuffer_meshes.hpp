#pragma once
// WP-1.4 gates: procedural meshlet meshes shared by test_rp_visbuffer_cpu.cpp and test_rp_visbuffer.cpp.
#include <fuse/renderer/geometry/meshlet_builder.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vis_test {

using fuse::f32;
using fuse::u32;

struct SourceMesh {
    std::vector<f32> positions;
    std::vector<u32> indices;
};

inline SourceMesh uvSphere(u32 rings, u32 segments, f32 radius) {
    SourceMesh m;
    const f32 pi = 3.14159265358979f;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 theta = pi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 phi = 2.f * pi * static_cast<f32>(s) / static_cast<f32>(segments);
            m.positions.push_back(radius * std::sin(theta) * std::cos(phi));
            m.positions.push_back(radius * std::cos(theta));
            m.positions.push_back(radius * std::sin(theta) * std::sin(phi));
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
    return m;
}

inline SourceMesh torus(u32 major, u32 minor, f32 R, f32 r) {
    SourceMesh m;
    const f32 pi = 3.14159265358979f;
    for (u32 i = 0; i < major; ++i) {
        const f32 u = 2.f * pi * static_cast<f32>(i) / static_cast<f32>(major);
        for (u32 j = 0; j < minor; ++j) {
            const f32 v = 2.f * pi * static_cast<f32>(j) / static_cast<f32>(minor);
            m.positions.push_back((R + r * std::cos(v)) * std::cos(u));
            m.positions.push_back(r * std::sin(v));
            m.positions.push_back((R + r * std::cos(v)) * std::sin(u));
        }
    }
    for (u32 i = 0; i < major; ++i) {
        for (u32 j = 0; j < minor; ++j) {
            const u32 a = i * minor + j;
            const u32 b = ((i + 1u) % major) * minor + j;
            const u32 c = ((i + 1u) % major) * minor + (j + 1u) % minor;
            const u32 d = i * minor + (j + 1u) % minor;
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    }
    return m;
}

/// Unit cube [-1, 1]^3 (8 vertices, 12 triangles).
inline SourceMesh box() {
    SourceMesh m;
    m.positions = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1};
    m.indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2};
    return m;
}

inline bool build(const SourceMesh& src, fuse::renderer::geometry::MeshletMesh& out) {
    fuse::renderer::geometry::MeshletSource s{};
    s.positions = src.positions.data();
    s.vertex_count = static_cast<u32>(src.positions.size() / 3u);
    s.indices = src.indices.data();
    s.index_count = static_cast<u32>(src.indices.size());
    fuse::renderer::geometry::MeshletBuildOptions options{};
    options.backend = fuse::kernel::Backend::CpuReference;
    std::string error;
    if (!fuse::renderer::geometry::build_meshlets(s, options, out, &error)) {
        std::fprintf(stderr, "build_meshlets: %s\n", error.c_str());
        return false;
    }
    return true;
}

} // namespace vis_test
