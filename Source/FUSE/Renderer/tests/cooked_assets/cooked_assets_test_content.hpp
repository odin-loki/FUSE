#pragma once

// E06 (AP-RT-COOKED) gates: deterministic source content shared by the CPU gates (test_cooked_assets_cpu.cpp) and the
// Lavapipe gates (test_cooked_assets_vk.cpp): a UV sphere with analytic normals / tangents, a smooth sRGB albedo and a
// tangent-space normal map from an analytic height field, plus the in-memory cook of them (Tools/FUSE/Cook library)
// read back through the runtime readers (fuse_asset), exactly what a .fusetex / .fusemesh file on disk yields.

#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/asset/cooked_texture.hpp>
#include <fuse/types.hpp>

#if defined(FUSE_CA_HAS_COOK)
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/cook/texture_cook.hpp>
#endif

#include <cmath>
#include <string>
#include <vector>

namespace ca_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u8;
using fuse::usize;

inline constexpr f64 kPi = 3.14159265358979323846;

/// UV sphere of radius `radius` (stacks x slices quads, seam and poles duplicated), one submesh, FMSH v2 streams:
/// positions, normals, uv0, tangents (+U direction, w = +1).
inline fuse::asset::CookedMesh makeSphere(u32 stacks, u32 slices, f32 radius) {
    fuse::asset::CookedMesh m;
    for (u32 i = 0; i <= stacks; ++i) {
        const f64 v = static_cast<f64>(i) / stacks;
        const f64 theta = v * kPi; // 0 at +Y
        for (u32 j = 0; j <= slices; ++j) {
            const f64 u = static_cast<f64>(j) / slices;
            const f64 phi = u * 2.0 * kPi;
            const f64 n[3] = {std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
            for (u32 a = 0; a < 3u; ++a) {
                m.positions.push_back(static_cast<f32>(n[a] * radius));
                m.normals.push_back(static_cast<f32>(n[a]));
            }
            m.uvs.push_back(static_cast<f32>(u * 2.0)); // the textures repeat twice around
            m.uvs.push_back(static_cast<f32>(v));
            // d(position)/d(phi) direction (+U); at the poles use the neighbouring direction.
            const f64 t[3] = {-std::sin(phi), 0.0, -std::cos(phi)};
            m.tangents.push_back(static_cast<f32>(t[0]));
            m.tangents.push_back(static_cast<f32>(t[1]));
            m.tangents.push_back(static_cast<f32>(t[2]));
            m.tangents.push_back(1.f);
        }
    }
    const u32 row = slices + 1u;
    for (u32 i = 0; i < stacks; ++i) {
        for (u32 j = 0; j < slices; ++j) {
            const u32 a = i * row + j;
            const u32 b = a + row;
            if (i != 0u) {
                m.indices.insert(m.indices.end(), {a, b, a + 1u});
            }
            if (i + 1u != stacks) {
                m.indices.insert(m.indices.end(), {a + 1u, b, b + 1u});
            }
        }
    }
    fuse::asset::CookedMesh::Submesh s{};
    s.index_count = static_cast<u32>(m.indices.size());
    m.submeshes.push_back(s);
    for (u32 a = 0; a < 3u; ++a) {
        m.bounds_min[a] = -radius;
        m.bounds_max[a] = radius;
    }
    return m;
}

inline u32 packRgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8u) | (b << 16u) | (a << 24u); }

inline u32 toByte(f64 v) {
    const f64 c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return static_cast<u32>(std::lround(c * 255.0));
}

/// Smooth, colourful, tileable sRGB albedo (packed RGBA8 words, alpha = a smooth "height" for layer blends).
inline std::vector<u32> makeAlbedo(u32 size) {
    std::vector<u32> t(static_cast<usize>(size) * size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f64 u = (x + 0.5) / size * 2.0 * kPi;
            const f64 v = (y + 0.5) / size * 2.0 * kPi;
            const f64 r = 0.55 + 0.35 * std::sin(u * 2.0) * std::cos(v);
            const f64 g = 0.45 + 0.3 * std::cos(u + v * 2.0);
            const f64 b = 0.35 + 0.25 * std::sin(v * 3.0 - u);
            const f64 h = 0.5 + 0.45 * std::sin(u) * std::sin(v * 2.0);
            t[static_cast<usize>(y) * size + x] = packRgba(toByte(r), toByte(g), toByte(b), toByte(h));
        }
    }
    return t;
}

/// Tangent-space (+Y up, OpenGL) normal map of the height h(u, v) = 0.5 sin(2 pi 2u) sin(2 pi 2v) / (4 pi), encoded
/// n * 0.5 + 0.5 (linear RGBA8 words, alpha 255).
inline std::vector<u32> makeNormalMap(u32 size) {
    std::vector<u32> t(static_cast<usize>(size) * size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f64 u = (x + 0.5) / size * 2.0 * kPi * 2.0;
            const f64 v = (y + 0.5) / size * 2.0 * kPi * 2.0;
            const f64 dx = 0.5 * std::cos(u) * std::sin(v);
            const f64 dy = 0.5 * std::sin(u) * std::cos(v);
            f64 n[3] = {-dx, -dy, 1.0};
            const f64 l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (f64& c : n) {
                c /= l;
            }
            t[static_cast<usize>(y) * size + x] =
                packRgba(toByte(n[0] * 0.5 + 0.5), toByte(n[1] * 0.5 + 0.5), toByte(n[2] * 0.5 + 0.5), 255u);
        }
    }
    return t;
}

inline std::vector<u8> wordsToBytes(const std::vector<u32>& words) {
    std::vector<u8> out(words.size() * 4u);
    for (usize i = 0; i < words.size(); ++i) {
        for (u32 c = 0; c < 4u; ++c) {
            out[i * 4u + c] = static_cast<u8>((words[i] >> (8u * c)) & 255u);
        }
    }
    return out;
}

#if defined(FUSE_CA_HAS_COOK)
/// Cooks RGBA8 words in memory, serialises the `.fusetex` and parses it back with the runtime reader.
inline bool cookTexture(const std::vector<u32>& words, u32 width, u32 height, u32 layers,
                        const fuse::cook::TextureCookOptions& options, fuse::asset::CookedTexture& out,
                        std::vector<u8>* bytes = nullptr, std::string* error = nullptr) {
    fuse::cook::TextureSource src;
    src.width = width;
    src.height = height;
    src.layers = layers;
    src.rgba8 = wordsToBytes(words);
    fuse::asset::CookedTexture cooked;
    const fuse::cook::CookStubWriteResult r = fuse::cook::cook_texture_image(src, options, cooked);
    if (!r.ok) {
        if (error != nullptr) {
            *error = r.note;
        }
        return false;
    }
    const std::vector<u8> blob = fuse::cook::serialize_cooked_texture(cooked);
    if (bytes != nullptr) {
        *bytes = blob;
    }
    return fuse::asset::parse_cooked_texture(blob.data(), blob.size(), out, error);
}

/// FMSH v2 with the WP-1.2 meshlet table (+ WP-5.2 DAG when `dag`), serialised and read back with the runtime reader.
inline bool cookMesh(fuse::asset::CookedMesh mesh, bool meshlets, bool dag, fuse::asset::CookedMesh& out,
                     std::vector<u8>* bytes = nullptr, std::string* error = nullptr) {
    if (meshlets && !fuse::cook::build_mesh_meshlets(mesh, dag, error)) {
        return false;
    }
    const std::vector<u8> blob = fuse::cook::serialize_cooked_mesh(mesh);
    if (bytes != nullptr) {
        *bytes = blob;
    }
    return fuse::asset::deserialize_cooked_mesh(blob.data(), blob.size(), out, error);
}
#endif

} // namespace ca_test
