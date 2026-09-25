// FUSE Relight RL-5.6 terrain tests: the synthetic terrain shared by the CPU and Vulkan gates.
#pragma once

#include <fuse/relight/terrain/terrain_baker.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace terrain_test {

namespace tb = fuse::relight::terrain;
using fuse::u32;
using fuse::u64;

/// The golden of the synthetic terrain (RGBA8 FNV-1a over the 96 x 64 bake). Update only with a reviewed change of
/// the baker's rules (print with `golden`).
inline constexpr u64 kGolden = 0xd64a420d81721eb6ull;

struct Mesh {
    std::vector<float> positions, uvs, colors;
    std::vector<u32> indices;
};

/// Grid over [x0, x1] x [z0, z1], n x m cells, height h(x, z), uv = world xz x uvScale.
template <typename H> inline Mesh grid(float x0, float z0, float x1, float z1, u32 n, u32 m, float uvScale, H h, bool colors) {
    Mesh g;
    for (u32 j = 0; j <= m; ++j) {
        for (u32 i = 0; i <= n; ++i) {
            const float x = x0 + (x1 - x0) * float(i) / float(n);
            const float z = z0 + (z1 - z0) * float(j) / float(m);
            g.positions.insert(g.positions.end(), {x, h(x, z), z});
            g.uvs.insert(g.uvs.end(), {x * uvScale, z * uvScale});
            if (colors) {
                const float a = 0.5f + 0.5f * std::sin(0.7f * x) * std::cos(0.5f * z);
                g.colors.insert(g.colors.end(), {1.f, 0.9f, 0.8f, a});
            }
        }
    }
    for (u32 j = 0; j < m; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const u32 a = j * (n + 1u) + i, b = a + 1u, c = a + n + 1u, d = c + 1u;
            g.indices.insert(g.indices.end(), {a, b, d, a, d, c});
        }
    }
    return g;
}

inline std::vector<float> checker(u32 w, u32 h, const float c0[4], const float c1[4]) {
    std::vector<float> t(std::size_t(w) * h * 4u);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const float* c = ((x / 2u + y / 2u) % 2u) != 0u ? c1 : c0;
            std::memcpy(&t[(std::size_t(y) * w + x) * 4u], c, 4u * sizeof(float));
        }
    }
    return t;
}

inline tb::TerrainLayer layerOf(const Mesh& m, const tb::TerrainTexture* tex, tb::TerrainBlend blend) {
    tb::TerrainLayer L;
    L.positions = m.positions.data();
    L.uvs = m.uvs.data();
    L.colors = m.colors.empty() ? nullptr : m.colors.data();
    L.vertexCount = static_cast<u32>(m.positions.size() / 3u);
    L.indices = m.indices.data();
    L.indexCount = static_cast<u32>(m.indices.size());
    L.texture = tex;
    L.blend = blend;
    return L;
}

inline u64 hashRgba8(const std::vector<float>& img, std::size_t texels) {
    u64 h = 1469598103934665603ull;
    for (std::size_t i = 0; i < texels * 4u; ++i) {
        const float v = std::min(std::max(img[i], 0.f), 1.f);
        const unsigned char q = static_cast<unsigned char>(std::lround(v * 255.f));
        h ^= q;
        h *= 1099511628211ull;
    }
    return h;
}

struct Terrain {
    Mesh ground, road, lightmap, highlight, grass;
    std::vector<float> texBase, texRoad, texLight, texGrass;
    tb::TerrainTexture base, roadTex, light, grassTex;
    std::vector<tb::TerrainLayer> layers;
    tb::TerrainBakeDesc desc;
};

inline void makeTerrain(Terrain& t) {
    auto hills = [](float x, float z) { return 0.4f * std::sin(0.8f * x) + 0.3f * std::cos(0.6f * z); };
    auto above = [&](float x, float z) { return hills(x, z) + 0.01f; };
    t.ground = grid(-6.f, -4.f, 6.f, 4.f, 24, 16, 0.5f, hills, false);
    t.road = grid(-6.f, -0.8f, 6.f, 0.8f, 24, 2, 0.25f, above, true);
    t.lightmap = grid(-6.f, -4.f, 6.f, 4.f, 12, 8, 1.f / 12.f, above, false);
    t.highlight = grid(1.f, 1.f, 3.f, 3.f, 2, 2, 0.5f, above, false);
    t.grass = grid(-5.f, -3.5f, -1.f, -1.5f, 8, 4, 1.f, above, false);
    const float green[4] = {0.2f, 0.5f, 0.15f, 1.f}, dark[4] = {0.1f, 0.3f, 0.1f, 1.f};
    t.texBase = checker(16, 16, green, dark);
    const float asphalt[4] = {0.3f, 0.3f, 0.32f, 0.9f}, line[4] = {0.9f, 0.9f, 0.8f, 0.6f};
    t.texRoad = checker(8, 8, asphalt, line);
    t.texLight.resize(16u * 16u * 4u);
    for (u32 y = 0; y < 16u; ++y) {
        for (u32 x = 0; x < 16u; ++x) {
            const float l = 0.35f + 0.3f * float((x * 3u + y * 5u) % 16u) / 15.f;
            float* p = &t.texLight[(y * 16u + x) * 4u];
            p[0] = l;
            p[1] = l * 0.95f;
            p[2] = l * 0.9f;
            p[3] = 1.f;
        }
    }
    t.texGrass.resize(8u * 8u * 4u);
    for (u32 y = 0; y < 8u; ++y) {
        for (u32 x = 0; x < 8u; ++x) {
            float* p = &t.texGrass[(y * 8u + x) * 4u];
            p[0] = 0.3f;
            p[1] = 0.7f;
            p[2] = 0.2f;
            p[3] = ((x + y) % 3u) == 0u ? 1.f : 0.f;
        }
    }
    t.base = tb::TerrainTexture{16, 16, t.texBase.data()};
    t.roadTex = tb::TerrainTexture{8, 8, t.texRoad.data()};
    t.light = tb::TerrainTexture{16, 16, t.texLight.data()};
    t.grassTex = tb::TerrainTexture{8, 8, t.texGrass.data()};
    t.layers.clear();
    t.layers.push_back(layerOf(t.ground, &t.base, tb::TerrainBlend::Opaque));
    t.layers.push_back(layerOf(t.road, &t.roadTex, tb::TerrainBlend::Alpha));
    tb::TerrainLayer grass = layerOf(t.grass, &t.grassTex, tb::TerrainBlend::Opaque);
    grass.alphaTest = true;
    t.layers.push_back(grass);
    tb::TerrainLayer lm = layerOf(t.lightmap, &t.light, tb::TerrainBlend::Multiply2x);
    lm.uvTransform[2] = 0.5f; // lightmap uv offset
    lm.uvTransform[5] = 0.5f;
    t.layers.push_back(lm);
    t.layers.push_back(layerOf(t.highlight, nullptr, tb::TerrainBlend::Additive));
    t.layers.back().colors = nullptr;
    t.desc.minX = -6.f;
    t.desc.minZ = -4.f;
    t.desc.maxX = 6.f;
    t.desc.maxZ = 4.f;
    t.desc.width = 96;
    t.desc.height = 64;
}

} // namespace terrain_test
