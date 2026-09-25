#pragma once
// WP-1.5 gates: procedural textured meshlet meshes, material textures (with CPU mip chains) and the
// materials shared by test_rp_material_resolve_cpu.cpp and test_rp_material_resolve.cpp.
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/material/material.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace mr_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u8;

constexpr f32 kPi = 3.14159265358979f;

struct SourceMesh {
    std::vector<f32> positions;
    std::vector<f32> normals;
    std::vector<f32> uvs;
    std::vector<u32> indices;
    std::vector<fuse::renderer::geometry::MeshletSourceSubmesh> submeshes;
    void vertex(f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
        positions.insert(positions.end(), {x, y, z});
        normals.insert(normals.end(), {nx, ny, nz});
        uvs.insert(uvs.end(), {u, v});
    }
};

/// UV sphere; the seam column is duplicated so UVs are continuous on every triangle.
inline SourceMesh uvSphere(u32 rings, u32 segments, f32 radius) {
    SourceMesh m;
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
    return m;
}

/// Torus with duplicated seams (continuous UVs, 2 x 1 tiling around).
inline SourceMesh torus(u32 major, u32 minor, f32 R, f32 r) {
    SourceMesh m;
    for (u32 i = 0; i <= major; ++i) {
        const f32 u = 2.f * kPi * static_cast<f32>(i) / static_cast<f32>(major);
        for (u32 j = 0; j <= minor; ++j) {
            const f32 v = 2.f * kPi * static_cast<f32>(j) / static_cast<f32>(minor);
            const f32 nx = std::cos(v) * std::cos(u);
            const f32 ny = std::sin(v);
            const f32 nz = std::cos(v) * std::sin(u);
            m.vertex((R + r * std::cos(v)) * std::cos(u), r * std::sin(v), (R + r * std::cos(v)) * std::sin(u), nx, ny, nz,
                     2.f * static_cast<f32>(i) / static_cast<f32>(major), static_cast<f32>(j) / static_cast<f32>(minor));
        }
    }
    for (u32 i = 0; i < major; ++i) {
        for (u32 j = 0; j < minor; ++j) {
            const u32 a = i * (minor + 1u) + j;
            const u32 b = (i + 1u) * (minor + 1u) + j;
            m.indices.insert(m.indices.end(), {a, b, b + 1u, a, b + 1u, a + 1u});
        }
    }
    return m;
}

/// Cube [-1, 1]^3 with per-face normals and UVs; two submeshes: faces +-x, +-y (material 0), +-z (material 1).
inline SourceMesh box() {
    SourceMesh m;
    struct Face {
        f32 n[3];
        f32 u[3];
        f32 v[3];
    };
    const Face faces[6] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                           {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                           {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (u32 f = 0; f < 6u; ++f) {
        const Face& fc = faces[f];
        const u32 base = static_cast<u32>(m.positions.size() / 3u);
        for (u32 c = 0; c < 4u; ++c) {
            const f32 su = (c == 1u || c == 2u) ? 1.f : -1.f;
            const f32 sv = (c >= 2u) ? 1.f : -1.f;
            m.vertex(fc.n[0] + su * fc.u[0] + sv * fc.v[0], fc.n[1] + su * fc.u[1] + sv * fc.v[1],
                     fc.n[2] + su * fc.u[2] + sv * fc.v[2], fc.n[0], fc.n[1], fc.n[2], su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
        }
        m.indices.insert(m.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    }
    m.submeshes.push_back({0u, 24u, 0u});
    m.submeshes.push_back({24u, 12u, 1u});
    return m;
}

/// Ground grid in the xz plane (normal +y), UVs tiled `tiles` times.
inline SourceMesh plane(u32 n, f32 size, f32 tiles) {
    SourceMesh m;
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
    return m;
}

inline bool build(const SourceMesh& src, fuse::renderer::geometry::MeshletMesh& out) {
    fuse::renderer::geometry::MeshletSource s{};
    s.positions = src.positions.data();
    s.normals = src.normals.data();
    s.uvs = src.uvs.data();
    s.vertex_count = static_cast<u32>(src.positions.size() / 3u);
    s.indices = src.indices.data();
    s.index_count = static_cast<u32>(src.indices.size());
    s.submeshes = src.submeshes;
    fuse::renderer::geometry::MeshletBuildOptions options{};
    options.backend = fuse::kernel::Backend::CpuReference;
    std::string error;
    if (!fuse::renderer::geometry::build_meshlets(s, options, out, &error)) {
        std::fprintf(stderr, "build_meshlets: %s\n", error.c_str());
        return false;
    }
    return true;
}

// --- textures -------------------------------------------------------------------------------------
/// RGBA8 UNORM texture with a full mip chain (level 0 first), texels as floats in [0, 1].
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    std::vector<std::vector<f32>> levels; ///< 4 floats per texel, already quantised to unorm8
};

inline f32 q8(f32 v) { return std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f; }

/// Level 0 from `fn(u, v, rgba)` at texel centres, then 2 x 2 box-filtered levels down to 1 x 1.
template <typename Fn>
TextureData makeTexture(u32 size, Fn fn) {
    TextureData t;
    t.width = size;
    t.height = size;
    std::vector<f32> level(static_cast<size_t>(size) * size * 4u);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 c[4] = {0.f, 0.f, 0.f, 1.f};
            fn((static_cast<f32>(x) + 0.5f) / static_cast<f32>(size), (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size), c);
            for (u32 k = 0; k < 4u; ++k) {
                level[(y * size + x) * 4u + k] = q8(c[k]);
            }
        }
    }
    t.levels.push_back(level);
    for (u32 s = size / 2u; s >= 1u; s /= 2u) {
        const std::vector<f32>& prev = t.levels.back();
        std::vector<f32> next(static_cast<size_t>(s) * s * 4u);
        for (u32 y = 0; y < s; ++y) {
            for (u32 x = 0; x < s; ++x) {
                for (u32 k = 0; k < 4u; ++k) {
                    const u32 ps = s * 2u;
                    const f32 sum = prev[((2u * y) * ps + 2u * x) * 4u + k] + prev[((2u * y) * ps + 2u * x + 1u) * 4u + k] +
                                    prev[((2u * y + 1u) * ps + 2u * x) * 4u + k] +
                                    prev[((2u * y + 1u) * ps + 2u * x + 1u) * 4u + k];
                    next[(y * s + x) * 4u + k] = q8(sum * 0.25f);
                }
            }
        }
        t.levels.push_back(std::move(next));
        if (s == 1u) {
            break;
        }
    }
    return t;
}

/// Every level L is the constant L / 16: a trilinear sample returns lod / 16, so the G-buffer albedo
/// of this texture reads back the LOD the sampler selected (8-bit: 16 / 255 LOD resolution).
inline TextureData makeLodProbe(u32 size) {
    TextureData t;
    t.width = size;
    t.height = size;
    u32 l = 0;
    for (u32 s = size; s >= 1u; s /= 2u, ++l) {
        const f32 v = static_cast<f32>(l) / 16.f;
        std::vector<f32> level(static_cast<size_t>(s) * s * 4u);
        for (size_t i = 0; i < level.size(); i += 4u) {
            level[i] = v;
            level[i + 1u] = v;
            level[i + 2u] = v;
            level[i + 3u] = 1.f;
        }
        t.levels.push_back(std::move(level));
        if (s == 1u) {
            break;
        }
    }
    return t;
}

/// Bilinear sample (wrap addressing) of level `l` at `uv`, channel `k`.
inline f32 bilinear(const TextureData& t, u32 l, f32 u, f32 v, u32 k) {
    const u32 w = std::max(1u, t.width >> l);
    const u32 h = std::max(1u, t.height >> l);
    const f32 x = u * static_cast<f32>(w) - 0.5f;
    const f32 y = v * static_cast<f32>(h) - 0.5f;
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const f32 ax = x - fx;
    const f32 ay = y - fy;
    auto texel = [&](int ix, int iy) {
        const u32 cx = static_cast<u32>(((ix % static_cast<int>(w)) + static_cast<int>(w)) % static_cast<int>(w));
        const u32 cy = static_cast<u32>(((iy % static_cast<int>(h)) + static_cast<int>(h)) % static_cast<int>(h));
        return t.levels[l][(cy * w + cx) * 4u + k];
    };
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    return (texel(ix, iy) * (1.f - ax) + texel(ix + 1, iy) * ax) * (1.f - ay) +
           (texel(ix, iy + 1) * (1.f - ax) + texel(ix + 1, iy + 1) * ax) * ay;
}

/// Per level L: the largest change of any channel of a sample when the LOD moves from level L to
/// L + 1, max over a dense UV grid of |bilinear(L) - bilinear(L+1)|. A trilinear sample at LOD l is
/// a blend of floor(l) and floor(l) + 1, so moving the LOD by at most one mip changes it by at most
/// the largest of these over the levels the two LODs touch: the texture-side bound that "LOD within
/// one mip" puts on a G-buffer channel.
inline std::vector<f32> adjacentMipBounds(const TextureData& t) {
    std::vector<f32> bounds;
    for (u32 l = 0; l + 1u < t.levels.size(); ++l) {
        f32 bound = 0.f;
        const u32 w = std::max(1u, t.width >> l);
        const u32 n = std::min(4u * w, 256u);
        for (u32 j = 0; j < n; ++j) {
            for (u32 i = 0; i < n; ++i) {
                const f32 u = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
                const f32 v = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(n);
                for (u32 k = 0; k < 4u; ++k) {
                    bound = std::max(bound, std::fabs(bilinear(t, l, u, v, k) - bilinear(t, l + 1u, u, v, k)));
                }
            }
        }
        bounds.push_back(bound);
    }
    return bounds;
}

/// The bound for a pixel whose LOD lies in [lodLo, lodHi] (either path may be up to one mip away).
inline f32 lodWindowBound(const std::vector<f32>& bounds, f32 lodLo, f32 lodHi) {
    if (bounds.empty()) {
        return 0.f;
    }
    const int last = static_cast<int>(bounds.size()) - 1;
    const int lo = std::clamp(static_cast<int>(std::floor(lodLo)) - 1, 0, last);
    const int hi = std::clamp(static_cast<int>(std::ceil(lodHi)) + 1, 0, last);
    f32 b = 0.f;
    for (int l = lo; l <= hi; ++l) {
        b = std::max(b, bounds[static_cast<size_t>(l)]);
    }
    return b;
}

/// Textures of the test scene (index order = MaterialTextures slots).
enum TextureId : u32 { kTexAlbedo = 0, kTexRoughness, kTexNormal, kTexLodProbe, kTexEmissive, kTexAo, kTexMetallic, kTexCount };

inline std::vector<TextureData> makeTextures() {
    std::vector<TextureData> t(kTexCount);
    t[kTexAlbedo] = makeTexture(64, [](f32 u, f32 v, f32* c) {
        c[0] = 0.55f + 0.35f * std::sin(2.f * kPi * u);
        c[1] = 0.55f + 0.35f * std::cos(2.f * kPi * v);
        c[2] = 0.5f + 0.3f * std::sin(2.f * kPi * (u + v));
    });
    t[kTexRoughness] = makeTexture(64, [](f32, f32 v, f32* c) {
        c[0] = 0.5f + 0.4f * std::sin(2.f * kPi * v);
        c[1] = c[0];
        c[2] = c[0];
    });
    t[kTexNormal] = makeTexture(64, [](f32 u, f32 v, f32* c) {
        const f32 k = 0.35f;
        const f32 dx = -k * std::cos(2.f * kPi * u) * std::sin(2.f * kPi * v);
        const f32 dy = -k * std::sin(2.f * kPi * u) * std::cos(2.f * kPi * v);
        const f32 len = std::sqrt(dx * dx + dy * dy + 1.f);
        c[0] = dx / len * 0.5f + 0.5f;
        c[1] = dy / len * 0.5f + 0.5f;
        c[2] = 1.f / len * 0.5f + 0.5f;
    });
    t[kTexLodProbe] = makeLodProbe(128);
    t[kTexEmissive] = makeTexture(64, [](f32 u, f32 v, f32* c) {
        c[0] = 0.6f + 0.3f * std::sin(2.f * kPi * (u - v));
        c[1] = 0.4f + 0.2f * std::cos(2.f * kPi * u);
        c[2] = 0.3f;
    });
    t[kTexAo] = makeTexture(64, [](f32 u, f32 v, f32* c) {
        c[0] = 0.7f + 0.25f * std::sin(2.f * kPi * u) * std::sin(2.f * kPi * v);
        c[1] = c[0];
        c[2] = c[0];
    });
    t[kTexMetallic] = makeTexture(64, [](f32 u, f32, f32* c) {
        c[0] = 0.5f + 0.45f * std::cos(2.f * kPi * u);
        c[1] = c[0];
        c[2] = c[0];
    });
    return t;
}

// --- materials ------------------------------------------------------------------------------------
enum MaterialId : u32 {
    kMatFlat = 0,       ///< constants only (bin Flat)
    kMatTextured,       ///< albedo + roughness textures (bin Textured)
    kMatNormalMapped,   ///< albedo + normal map (bin NormalMapped)
    kMatLodProbe,       ///< the LOD probe as albedo (bin Textured)
    kMatEmissive,       ///< flat emissive, shading model Emissive (bin Flat)
    kMatAoEmissiveTex,  ///< AO + emissive + metallic textures, clear coat (bin Textured)
    kMatBoxSide,        ///< box submesh 1: metallic texture + normal map, cloth (bin NormalMapped)
    kMatCount,
};

/// `tex[i]` = the shader handle of texture i (bindless sampled image; any value != ~0 in CPU-only tests).
inline std::vector<fuse::renderer::Material::GPUMaterial> makeMaterials(const u32 tex[kTexCount]) {
    using fuse::renderer::Material;
    std::vector<Material::GPUMaterial> m(kMatCount);
    auto base = [](Material::GPUMaterial& g, f32 r, f32 gr, f32 b, f32 metal, f32 rough, u32 shading) {
        g.baseColor = {r, gr, b, metal};
        g.roughnessEmissive = {rough, 0.f, 0.f, 0.f};
        g.shadingModel = shading;
    };
    base(m[kMatFlat], 0.8f, 0.3f, 0.2f, 0.f, 0.6f, 0u);
    base(m[kMatTextured], 1.f, 1.f, 1.f, 0.1f, 0.9f, 0u);
    m[kMatTextured].baseColorTexIdx = tex[kTexAlbedo];
    m[kMatTextured].roughnessTexIdx = tex[kTexRoughness];
    base(m[kMatNormalMapped], 0.9f, 0.9f, 0.9f, 0.f, 0.4f, 0u);
    m[kMatNormalMapped].baseColorTexIdx = tex[kTexAlbedo];
    m[kMatNormalMapped].normalTexIdx = tex[kTexNormal];
    m[kMatNormalMapped].normalStrength = 0.8f;
    m[kMatNormalMapped].flags = fuse::renderer::MaterialFlagBits::kHasNormalMap;
    base(m[kMatLodProbe], 1.f, 1.f, 1.f, 0.f, 0.5f, 0u);
    m[kMatLodProbe].baseColorTexIdx = tex[kTexLodProbe];
    base(m[kMatEmissive], 0.2f, 0.2f, 0.2f, 0.f, 0.5f, 2u);
    m[kMatEmissive].roughnessEmissive = {0.5f, 1.f, 0.6f, 0.2f};
    m[kMatEmissive].emissiveIntensity = 3.f;
    base(m[kMatAoEmissiveTex], 0.5f, 0.7f, 0.9f, 1.f, 0.3f, 4u);
    m[kMatAoEmissiveTex].aoTexIdx = tex[kTexAo];
    m[kMatAoEmissiveTex].emissiveTexIdx = tex[kTexEmissive];
    m[kMatAoEmissiveTex].metallicTexIdx = tex[kTexMetallic];
    m[kMatAoEmissiveTex].roughnessEmissive = {0.3f, 0.5f, 0.5f, 0.5f};
    m[kMatAoEmissiveTex].emissiveIntensity = 2.f;
    m[kMatAoEmissiveTex].clearCoatBlock = {1.f, 0.05f, 0.f, 0.f};
    m[kMatAoEmissiveTex].flags = fuse::renderer::MaterialFlagBits::kHasAoMap | fuse::renderer::MaterialFlagBits::kHasMetallicMap;
    base(m[kMatBoxSide], 0.6f, 0.6f, 0.3f, 1.f, 0.7f, 5u);
    m[kMatBoxSide].metallicTexIdx = tex[kTexMetallic];
    m[kMatBoxSide].normalTexIdx = tex[kTexNormal];
    m[kMatBoxSide].normalStrength = 1.f;
    m[kMatBoxSide].clothBlock = {0.9f, 0.8f, 0.7f, 0.f};
    return m;
}

} // namespace mr_test
