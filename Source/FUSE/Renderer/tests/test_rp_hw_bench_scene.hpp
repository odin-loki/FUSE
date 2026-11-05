#pragma once
// Procedural stand-ins for the plan §8 reference scenes (docs/unification/RENDERER-HW-RUN.md, "Stand-in scenes"),
// sized like the real assets so the 1440p budget harness times representative work until Sponza / Bistro are fetched.
//
//   sponza   Crytek-Sponza-like atrium: 40 x 24 m courtyard, two storeys of colonnades (2 x 2 x 12 columns),
//            arches, galleries, enclosing walls, 6 hanging banners (transparent), sun + 8 local lights; 244 instances,
//            0.55M triangles, 24 materials (Sponza: 262k triangles, 25 materials, 1 sun; the stand-in errs heavy).
//   bistro   Amazon-Lumberyard-Bistro-exterior-like street: 160 m street, 2 x 24 facades of 6-14 m height with
//            window / balcony / awning detail, 96 trees (sphere canopies), 120 street lamps, café tables, glass
//            store fronts (transparent); 3.92M triangles, 2,748 instances, 40 materials, sun + `lights` local
//            lights (default 256 string lights; the P2 scenario uses 4,096) (Bistro exterior: ~2.8M triangles).
// Everything is deterministic (fixed LCG); meshes are WP-1.2 meshlet meshes (mr_test builders) shared by instances.
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material/material.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace hw_scene {

using fuse::f32;
using fuse::u32;
using fuse::u64;
namespace gs = fuse::renderer::gpu_scene;

enum class Kind : u32 { Sponza = 0, Bistro = 1 };

inline const char* kindName(Kind k) { return k == Kind::Sponza ? "sponza" : "bistro"; }

struct Lcg {
    u64 s = 0x9E3779B97F4A7C15ull;
    f32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((s >> 40) & 0xFFFFFFu) / 16777216.f;
    }
    f32 range(f32 a, f32 b) { return a + (b - a) * next(); }
};

/// Mesh ids of Desc::meshes.
enum MeshId : u32 { kBox = 0, kSphere, kSphereHi, kPlane, kTorus, kMeshCount };

struct Instance {
    u32 mesh = kBox;
    u32 material = 0;
    f32 center[3] = {0.f, 0.f, 0.f};
    f32 half[3] = {1.f, 1.f, 1.f}; ///< per-axis scale (the unit meshes span [-1, 1])
    bool transparent = false;
    f32 opacity = 1.f;
};

struct Desc {
    Kind kind = Kind::Sponza;
    std::vector<mr_test::SourceMesh> meshes; ///< kMeshCount
    std::vector<fuse::renderer::Material> materials;
    std::vector<Instance> instances;
    std::vector<gs::GpuLight> lights; ///< [0] = the sun (directional)
    f32 toSun[3] = {0.35f, 0.85f, 0.25f};
    f32 eye[3] = {0.f, 2.f, 10.f};
    f32 target[3] = {0.f, 2.f, 0.f};
    f32 bounds[2][3] = {{-20.f, 0.f, -12.f}, {20.f, 16.f, 12.f}}; ///< DDGI / fog extent
    u64 triangles = 0; ///< source triangles over all instances
};

inline fuse::renderer::Material mat(f32 r, f32 g, f32 b, f32 rough, f32 metal = 0.f) {
    fuse::renderer::Material m{};
    m.baseColor = fuse::math::Vec3{r, g, b};
    m.roughness = rough;
    m.metallic = metal;
    return m;
}

inline u64 meshTriangles(const mr_test::SourceMesh& m) { return m.indices.size() / 3u; }

inline void addMeshes(Desc& d) {
    d.meshes.resize(kMeshCount);
    d.meshes[kBox] = mr_test::box();
    d.meshes[kSphere] = mr_test::uvSphere(24, 48, 1.f);     // 2.2k triangles
    d.meshes[kSphereHi] = mr_test::uvSphere(64, 128, 1.f);  // 16k triangles
    d.meshes[kPlane] = mr_test::plane(64, 2.f, 8.f);        // 8k triangles
    d.meshes[kTorus] = mr_test::torus(48, 24, 1.f, 0.18f);  // 2.3k triangles
}

inline void add(Desc& d, u32 mesh, u32 material, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, bool transparent = false,
                f32 opacity = 1.f) {
    Instance i{};
    i.mesh = mesh;
    i.material = material;
    i.center[0] = cx;
    i.center[1] = cy;
    i.center[2] = cz;
    i.half[0] = hx;
    i.half[1] = hy;
    i.half[2] = hz;
    i.transparent = transparent;
    i.opacity = opacity;
    d.instances.push_back(i);
}

inline gs::GpuLight pointLight(f32 x, f32 y, f32 z, f32 range, f32 r, f32 g, f32 b, f32 intensity) {
    gs::GpuLight l{};
    l.type = static_cast<u32>(gs::GpuLightType::Point);
    l.position[0] = x;
    l.position[1] = y;
    l.position[2] = z;
    l.range = range;
    l.color[0] = r;
    l.color[1] = g;
    l.color[2] = b;
    l.intensity = intensity;
    return l;
}

inline gs::GpuLight sunLight(const f32 toSun[3]) {
    gs::GpuLight sun{};
    sun.type = static_cast<u32>(gs::GpuLightType::Directional);
    const f32 l = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
    for (u32 k = 0; k < 3u; ++k) {
        sun.direction[k] = -toSun[k] / l;
    }
    sun.color[0] = 1.f;
    sun.color[1] = 0.95f;
    sun.color[2] = 0.85f;
    sun.intensity = 3.f;
    return sun;
}

// --- Sponza stand-in --------------------------------------------------------------------------------------
inline Desc makeSponza() {
    Desc d{};
    d.kind = Kind::Sponza;
    addMeshes(d);
    Lcg rng;
    // materials: 0 floor, 1 walls, 2 columns, 3 arches, 4 gallery floor, 5 roof trim, 6-11 banners, 12-23 props
    d.materials.push_back(mat(0.55f, 0.52f, 0.47f, 0.8f));
    d.materials.push_back(mat(0.70f, 0.64f, 0.55f, 0.9f));
    d.materials.push_back(mat(0.66f, 0.62f, 0.58f, 0.6f));
    d.materials.push_back(mat(0.60f, 0.55f, 0.50f, 0.7f));
    d.materials.push_back(mat(0.45f, 0.40f, 0.35f, 0.75f));
    d.materials.push_back(mat(0.35f, 0.30f, 0.28f, 0.6f, 0.3f));
    const f32 banner[6][3] = {{0.7f, 0.1f, 0.1f}, {0.1f, 0.5f, 0.2f}, {0.1f, 0.2f, 0.7f},
                              {0.7f, 0.6f, 0.1f}, {0.5f, 0.1f, 0.5f}, {0.1f, 0.6f, 0.6f}};
    for (const auto& c : banner) {
        d.materials.push_back(mat(c[0], c[1], c[2], 0.9f));
    }
    for (u32 i = 0; i < 12u; ++i) {
        d.materials.push_back(mat(rng.range(0.2f, 0.8f), rng.range(0.2f, 0.7f), rng.range(0.15f, 0.6f), rng.range(0.2f, 0.9f),
                                  i % 4u == 0u ? 0.8f : 0.f));
    }
    // floor (4 x 3 plane tiles), outer walls, roof trim
    for (u32 i = 0; i < 4u; ++i) {
        for (u32 j = 0; j < 3u; ++j) {
            add(d, kPlane, 0, -15.f + 10.f * static_cast<f32>(i), 0.f, -8.f + 8.f * static_cast<f32>(j), 5.f, 1.f, 4.f);
        }
    }
    add(d, kBox, 1, 0.f, 8.f, -12.5f, 20.f, 8.f, 0.5f);
    add(d, kBox, 1, 0.f, 8.f, 12.5f, 20.f, 8.f, 0.5f);
    add(d, kBox, 1, -20.5f, 8.f, 0.f, 0.5f, 8.f, 12.f);
    add(d, kBox, 1, 20.5f, 8.f, 0.f, 0.5f, 8.f, 12.f);
    add(d, kBox, 5, 0.f, 15.6f, -11.5f, 20.f, 0.4f, 1.f);
    add(d, kBox, 5, 0.f, 15.6f, 11.5f, 20.f, 0.4f, 1.f);
    // two storeys of colonnades on both long sides: 12 columns each (high-poly shafts + capitals), arches between
    for (u32 storey = 0; storey < 2u; ++storey) {
        const f32 base = storey == 0u ? 0.f : 7.f;
        const f32 h = storey == 0u ? 3.f : 2.5f;
        for (u32 side = 0; side < 2u; ++side) {
            const f32 z = side == 0u ? -7.f : 7.f;
            add(d, kPlane, 4, 0.f, base + 2.f * h + 0.3f, side == 0u ? -9.5f : 9.5f, 18.f, 1.f, 2.5f); // gallery floor
            add(d, kBox, 4, 0.f, base + 2.f * h + 0.15f, side == 0u ? -9.5f : 9.5f, 18.f, 0.15f, 2.5f);
            for (u32 c = 0; c < 12u; ++c) {
                const f32 x = -16.5f + 3.f * static_cast<f32>(c);
                add(d, kSphere, 2, x, base + h, z, 0.35f, h, 0.35f);            // shaft
                add(d, kTorus, 3, x, base + 2.f * h - 0.1f, z, 0.45f, 0.45f, 0.45f); // capital
                add(d, kBox, 3, x, base + 0.15f, z, 0.5f, 0.15f, 0.5f);            // plinth
                if (c < 11u) {
                    add(d, kTorus, 3, x + 1.5f, base + 2.f * h - 0.2f, z, 1.5f, 1.2f, 0.3f); // arch
                }
            }
        }
    }
    // hanging banners (transparent cloth) and props (vases / urns)
    for (u32 b = 0; b < 6u; ++b) {
        add(d, kPlane, 6u + b, -12.5f + 5.f * static_cast<f32>(b), 9.f, b % 2u == 0u ? -6.2f : 6.2f, 1.f, 1.f, 2.5f, true, 0.7f);
    }
    for (u32 p = 0; p < 24u; ++p) {
        const f32 x = rng.range(-17.f, 17.f);
        const f32 z = rng.range(-5.f, 5.f);
        const f32 s = rng.range(0.3f, 0.6f);
        add(d, kSphere, 12u + p % 12u, x, s, z, s, s * 1.3f, s);
    }
    d.lights.push_back(sunLight(d.toSun));
    for (u32 i = 0; i < 8u; ++i) {
        d.lights.push_back(pointLight(-14.f + 4.f * static_cast<f32>(i), 2.5f, i % 2u == 0u ? -5.f : 5.f, 8.f, 1.f, 0.7f, 0.4f, 6.f));
    }
    d.eye[0] = -16.f;
    d.eye[1] = 1.8f;
    d.eye[2] = 0.5f;
    d.target[0] = 10.f;
    d.target[1] = 4.f;
    d.target[2] = -1.f;
    d.bounds[0][0] = -20.f;
    d.bounds[0][1] = 0.f;
    d.bounds[0][2] = -12.f;
    d.bounds[1][0] = 20.f;
    d.bounds[1][1] = 16.f;
    d.bounds[1][2] = 12.f;
    return d;
}

// --- Bistro stand-in ---------------------------------------------------------------------------------------
inline Desc makeBistro(u32 localLights) {
    Desc d{};
    d.kind = Kind::Bistro;
    addMeshes(d);
    Lcg rng;
    // materials 0 road, 1 sidewalk, 2-17 facades, 18-25 windows / frames, 26-29 awnings, 30 foliage, 31 bark,
    // 32 lamp metal, 33 lamp glass (emissive), 34 glass (transparent), 35-39 furniture
    d.materials.push_back(mat(0.12f, 0.12f, 0.13f, 0.6f));
    d.materials.push_back(mat(0.45f, 0.43f, 0.40f, 0.8f));
    for (u32 i = 0; i < 16u; ++i) {
        d.materials.push_back(mat(rng.range(0.4f, 0.85f), rng.range(0.35f, 0.75f), rng.range(0.3f, 0.65f), rng.range(0.6f, 0.95f)));
    }
    for (u32 i = 0; i < 8u; ++i) {
        d.materials.push_back(mat(rng.range(0.1f, 0.4f), rng.range(0.1f, 0.4f), rng.range(0.1f, 0.4f), 0.3f, i < 4u ? 0.9f : 0.f));
    }
    for (u32 i = 0; i < 4u; ++i) {
        d.materials.push_back(mat(rng.range(0.4f, 0.9f), rng.range(0.1f, 0.5f), rng.range(0.1f, 0.4f), 0.85f));
    }
    d.materials.push_back(mat(0.12f, 0.35f, 0.1f, 0.9f));
    d.materials.push_back(mat(0.25f, 0.18f, 0.12f, 0.9f));
    d.materials.push_back(mat(0.08f, 0.08f, 0.08f, 0.35f, 1.f));
    {
        fuse::renderer::Material m = mat(1.f, 0.9f, 0.7f, 0.2f);
        m.emissiveColor = fuse::math::Vec3{1.f, 0.8f, 0.5f};
        m.emissiveIntensity = 4.f;
        d.materials.push_back(m);
    }
    d.materials.push_back(mat(0.6f, 0.75f, 0.8f, 0.05f));
    for (u32 i = 0; i < 5u; ++i) {
        d.materials.push_back(mat(rng.range(0.2f, 0.6f), rng.range(0.15f, 0.4f), rng.range(0.1f, 0.3f), 0.5f, i == 0u ? 1.f : 0.f));
    }
    constexpr f32 kLen = 80.f; // street half length
    for (u32 i = 0; i < 10u; ++i) {
        const f32 x = -kLen + 8.f + 16.f * static_cast<f32>(i);
        add(d, kPlane, 0, x, 0.f, 0.f, 8.f, 1.f, 5.f);
        add(d, kBox, 1, x, 0.1f, -6.5f, 8.f, 0.1f, 1.5f);
        add(d, kBox, 1, x, 0.1f, 6.5f, 8.f, 0.1f, 1.5f);
    }
    // facades: 2 sides x 24 buildings; per building: shell, 3-5 storeys x 4 windows (frame box + sill + glass),
    // balconies (torus railings), awnings on the ground floor, a cornice (high-poly sphere ornaments)
    for (u32 side = 0; side < 2u; ++side) {
        const f32 sz = side == 0u ? -1.f : 1.f;
        for (u32 b = 0; b < 24u; ++b) {
            const f32 w = 3.3f;
            const f32 x = -kLen + w + 2.f * w * static_cast<f32>(b) + 0.05f;
            const u32 storeys = 3u + static_cast<u32>(rng.next() * 3.f);
            const f32 h = 3.2f * static_cast<f32>(storeys) * 0.5f;
            const f32 depth = 5.f;
            const f32 front = sz * 8.f;
            const u32 facadeMat = 2u + static_cast<u32>(rng.next() * 16.f) % 16u;
            add(d, kBox, facadeMat, x, h, front + sz * depth, w, h, depth);
            add(d, kBox, 5u + facadeMat % 3u, x, 2.f * h + 0.2f, front + sz * 0.3f, w + 0.1f, 0.2f, 0.4f);
            for (u32 o = 0; o < 4u; ++o) {
                add(d, kSphere, 18u + o % 8u, x - 2.4f + 1.6f * static_cast<f32>(o), 2.f * h + 0.55f, front + sz * 0.2f, 0.18f, 0.25f,
                    0.18f);
            }
            for (u32 s = 0; s < storeys; ++s) {
                const f32 y = 1.6f + 3.2f * static_cast<f32>(s);
                for (u32 wi = 0; wi < 4u; ++wi) {
                    const f32 wx = x - 2.4f + 1.6f * static_cast<f32>(wi);
                    if (s == 0u) {
                        add(d, kBox, 34, wx, 1.2f, front - sz * 0.02f, 0.7f, 1.1f, 0.03f, true, 0.35f); // store-front glass
                        continue;
                    }
                    add(d, kBox, 18u + (wi + s) % 8u, wx, y, front - sz * 0.05f, 0.55f, 0.85f, 0.08f);
                    add(d, kBox, 18u + (wi + s + 3u) % 8u, wx, y - 0.95f, front - sz * 0.15f, 0.65f, 0.06f, 0.15f);
                    if ((wi + s + b) % 3u == 0u) {
                        add(d, kTorus, 32, wx, y - 0.6f, front - sz * 0.45f, 0.6f, 0.6f, 0.35f); // balcony railing
                        add(d, kBox, 32, wx, y - 0.95f, front - sz * 0.45f, 0.65f, 0.05f, 0.4f);
                    }
                }
            }
            add(d, kPlane, 26u + b % 4u, x, 3.4f, front - sz * 1.2f, w * 0.95f, 1.f, 1.2f); // awning
        }
    }
    // trees (canopy = high-poly sphere + trunk), street lamps (pole + globe), café tables and chairs
    for (u32 t = 0; t < 96u; ++t) {
        const f32 x = -kLen + 1.7f * static_cast<f32>(t) + 0.8f;
        const f32 z = t % 2u == 0u ? -5.6f : 5.6f;
        const f32 s = rng.range(1.2f, 1.8f);
        add(d, kSphereHi, 30, x, 2.6f + s, z, s, s * 0.9f, s);
        add(d, kSphere, 31, x, 1.4f, z, 0.12f, 1.4f, 0.12f);
    }
    for (u32 l = 0; l < 120u; ++l) {
        const f32 x = -kLen + 1.33f * static_cast<f32>(l) + 0.4f;
        const f32 z = l % 2u == 0u ? -4.9f : 4.9f;
        add(d, kSphere, 32, x, 1.8f, z, 0.05f, 1.8f, 0.05f);
        add(d, kSphere, 33, x, 3.75f, z, 0.2f, 0.25f, 0.2f);
    }
    for (u32 c = 0; c < 64u; ++c) {
        const f32 x = rng.range(-kLen + 2.f, kLen - 2.f);
        const f32 z = c % 2u == 0u ? rng.range(-6.8f, -5.6f) : rng.range(5.6f, 6.8f);
        add(d, kSphere, 35u + c % 5u, x, 0.75f, z, 0.4f, 0.04f, 0.4f);
        add(d, kSphere, 35u + (c + 1u) % 5u, x, 0.38f, z, 0.04f, 0.37f, 0.04f);
        for (u32 k = 0; k < 2u; ++k) {
            add(d, kBox, 35u + (c + 2u) % 5u, x + (k == 0u ? -0.6f : 0.6f), 0.45f, z, 0.2f, 0.45f, 0.2f);
        }
    }
    d.toSun[0] = 0.5f;
    d.toSun[1] = 0.6f;
    d.toSun[2] = 0.35f;
    d.lights.push_back(sunLight(d.toSun));
    // local lights: string lights zig-zagging over the street (3-7 m range), every 4th a spot pointing down
    for (u32 i = 0; i < localLights; ++i) {
        const f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(localLights);
        const f32 x = -kLen + 2.f * kLen * t;
        const u32 strand = i % 8u;
        const f32 z = -5.f + 10.f * rng.next();
        const f32 y = 3.5f + 2.5f * static_cast<f32>(strand) / 8.f + 0.3f * rng.next();
        gs::GpuLight l = pointLight(x, y, z, rng.range(3.f, 7.f), rng.range(0.8f, 1.f), rng.range(0.55f, 0.85f), rng.range(0.3f, 0.6f),
                                    rng.range(1.f, 3.f));
        if (i % 4u == 3u) {
            l.type = static_cast<u32>(gs::GpuLightType::Spot);
            l.direction[0] = 0.f;
            l.direction[1] = -1.f;
            l.direction[2] = 0.f;
            l.cosInner = 0.9f;
            l.cosOuter = 0.7f;
        }
        d.lights.push_back(l);
    }
    d.eye[0] = -55.f;
    d.eye[1] = 1.7f;
    d.eye[2] = 1.5f;
    d.target[0] = 20.f;
    d.target[1] = 3.5f;
    d.target[2] = -1.f;
    d.bounds[0][0] = -kLen;
    d.bounds[0][1] = 0.f;
    d.bounds[0][2] = -14.f;
    d.bounds[1][0] = kLen;
    d.bounds[1][1] = 18.f;
    d.bounds[1][2] = 14.f;
    return d;
}

inline void countTriangles(Desc& d) {
    d.triangles = 0;
    for (const Instance& i : d.instances) {
        d.triangles += meshTriangles(d.meshes[i.mesh]);
    }
}

inline gs::GpuTransform transformOf(const Instance& i) {
    gs::GpuTransform t{};
    for (u32 k = 0; k < 3u; ++k) {
        t.rows[k][k] = i.half[k];
        t.rows[k][3] = i.center[k];
    }
    return t;
}

} // namespace hw_scene
