#pragma once
// WP-7.3 gates: the tiny path-tracing scenes shared by test_rp_pathtrace_cpu.cpp and test_rp_pathtrace.cpp. Every
// scene is a WP-1.1 GpuScene (CPU-only mode in the CPU gates, the real GPU scene on Lavapipe) of WP-1.2 meshlet
// meshes, so the GPU (BLAS / TLAS of the same decoded vertices) and the CPU reference trace the same triangles.
//
//   Converge  ground (Lambert) + back wall (Lambert) + a GGX-metal block + a half-metal block, lit by a one-sided
//             emissive panel (8 traced triangles, each a WP-7.1 light-tree emitter mapped by the emitter map) and a
//             point light (NEE only), dim constant sky
//   Furnace   one floating convex box (Lambert, albedo `furnaceAlbedo`), no light, sky = 1: every camera path sees
//             albedo x 1 on the box (a convex surface never sees itself) and 1 on the sky
//   Analytic  a large Lambert ground (albedo 0.5) under a one-sided square emitter (side 2, height 1.5, radiance 3):
//             the point below the emitter's centre reflects rho Le F, F the point-to-parallel-rectangle form factor
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/pathtrace/pathtrace.hpp>
#include <fuse/renderer/pathtrace/pt_reference.hpp>
#include <fuse/renderer/restir/restir.hpp>

#include <cmath>
#include <vector>

namespace pt_test {

using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using namespace fuse::renderer;
using namespace fuse::renderer::pathtrace;

enum class SceneKind { Converge, Furnace, Analytic };

struct Box {
    f32 lo[3];
    f32 hi[3];
};

inline gpu_scene::GpuTransform boxTransform(const Box& b) {
    gpu_scene::GpuTransform t{};
    for (u32 k = 0; k < 3u; ++k) {
        t.rows[k][k] = (b.hi[k] - b.lo[k]) * 0.5f;
        t.rows[k][3] = (b.hi[k] + b.lo[k]) * 0.5f;
    }
    return t;
}

/// A downward-facing square panel of side `side` centred at (cx, y, cz): the +y plane mesh rotated 180 degrees
/// about x (so e1 x e2 of every triangle points down).
inline gpu_scene::GpuTransform panelTransform(f32 cx, f32 y, f32 cz, f32 side) {
    gpu_scene::GpuTransform t{};
    t.rows[0][0] = side;
    t.rows[1][1] = -1.f;
    t.rows[2][2] = -side;
    t.rows[0][3] = cx;
    t.rows[1][3] = y;
    t.rows[2][3] = cz;
    return t;
}

inline Material::GPUMaterial material(f32 r, f32 g, f32 b, f32 metallic, f32 roughness) {
    Material::GPUMaterial m{};
    m.baseColor = {r, g, b, metallic};
    m.roughnessEmissive = {roughness, 0.f, 0.f, 0.f};
    return m;
}

inline Material::GPUMaterial emissive(f32 r, f32 g, f32 b) {
    Material::GPUMaterial m{};
    m.baseColor = {0.f, 0.f, 0.f, 0.f};
    m.roughnessEmissive = {1.f, r, g, b};
    m.emissiveIntensity = 1.f;
    return m;
}

struct World {
    SceneKind kind = SceneKind::Converge;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<light_tree::LightTreeLight> lights;
    std::vector<light_tree::EmissiveTriangleRef> triangleRefs;
    std::vector<restir::RestirLight> table;
    light_tree::LightTree tree;
    std::vector<PtEmitterRef> emitterRefs;
    std::vector<u32> emitterMap;
    PtCpuScene cpu;
    PtCamera camera{};
    u32 width = 0;
    u32 height = 0;
    f32 sky[3] = {0.f, 0.f, 0.f};
    f32 furnaceAlbedo = 1.f;
    /// Analytic: expected radiance of the centre pixel.
    f64 analytic = 0.0;
};

/// Column-major Vulkan view-projection (y down, forward z/w in [0, 1]) of a look-at camera.
inline void lookAt(PtCamera& c, const f64 eye[3], const f64 at[3], f64 fovY, f64 aspect, f64 zNear, f64 zFar) {
    f64 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f64 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f64& v : f) {
        v /= fl;
    }
    const f64 up[3] = {std::fabs(f[1]) > 0.999 ? 0.0 : 0.0, std::fabs(f[1]) > 0.999 ? 0.0 : 1.0, std::fabs(f[1]) > 0.999 ? 1.0 : 0.0};
    f64 s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f64 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f64& v : s) {
        v /= sl;
    }
    const f64 u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    f64 view[16] = {s[0], u[0], -f[0], 0.0, s[1], u[1], -f[1], 0.0, s[2], u[2], -f[2], 0.0,
                    -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                    f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2], 1.0};
    const f64 t = 1.0 / std::tan(fovY * 0.5);
    f64 proj[16] = {};
    proj[0] = t / aspect;
    proj[5] = -t;
    proj[10] = zFar / (zNear - zFar);
    proj[11] = -1.0;
    proj[14] = zNear * zFar / (zNear - zFar);
    for (u32 col = 0; col < 4u; ++col) {
        for (u32 row = 0; row < 4u; ++row) {
            f64 v = 0.0;
            for (u32 k = 0; k < 4u; ++k) {
                v += proj[k * 4u + row] * view[col * 4u + k];
            }
            c.viewProj[col * 4u + row] = static_cast<f32>(v);
        }
    }
    for (u32 k = 0; k < 3u; ++k) {
        c.position[k] = static_cast<f32>(eye[k]);
        c.forward[k] = static_cast<f32>(f[k]);
    }
}

/// Point-to-parallel-rectangle form factor, the point below one corner (sides a, b at distance c).
inline f64 cornerFormFactor(f64 a, f64 b, f64 c) {
    const f64 x = a / c;
    const f64 y = b / c;
    const f64 sx = std::sqrt(1.0 + x * x);
    const f64 sy = std::sqrt(1.0 + y * y);
    return (x / sx * std::atan(y / sx) + y / sy * std::atan(x / sy)) / (2.0 * 3.14159265358979323846);
}

/// Adds the scene to `scene` (initialised, CPU-only or GPU; meshes 0.. added here) and builds the lights, the
/// emitter map and the CPU reference scene. `scene.commit()` is the caller's.
inline bool buildWorld(World& w, gpu_scene::GpuScene& scene, SceneKind kind, u32 width, u32 height, f32 furnaceAlbedo = 1.f) {
    w.kind = kind;
    w.width = width;
    w.height = height;
    w.furnaceAlbedo = furnaceAlbedo;
    w.meshes.resize(3);
    if (!mr_test::build(mr_test::plane(2, 1.f, 1.f), w.meshes[0]) || !mr_test::build(mr_test::box(), w.meshes[1]) ||
        !mr_test::build(mr_test::plane(2, 1.f, 1.f), w.meshes[2])) {
        return false;
    }
    for (u32 i = 0; i < 3u; ++i) {
        if (scene.addMeshletMesh(w.meshes[i]) != i) {
            return false;
        }
    }
    auto addInstance = [&](u32 mesh, u32 materialRow, const gpu_scene::GpuTransform& xf) -> u32 {
        gpu_scene::InstanceDesc d{};
        d.mesh = mesh;
        d.material = materialRow;
        d.transform = xf;
        const gpu_scene::InstanceHandle h = scene.addInstance(d);
        return h.valid() ? h.slot : fuse::renderer::gpu_scene::kInvalidIndex;
    };
    auto scaled = [](f32 sx, f32 sz, f32 tx, f32 ty, f32 tz) {
        gpu_scene::GpuTransform t{};
        t.rows[0][0] = sx;
        t.rows[2][2] = sz;
        t.rows[0][3] = tx;
        t.rows[1][3] = ty;
        t.rows[2][3] = tz;
        return t;
    };
    f64 eye[3] = {0.0, 1.3, 2.2};
    f64 at[3] = {0.0, 0.7, -1.6};
    f64 fov = 1.0;
    w.lights.clear();
    w.triangleRefs.clear();
    w.table.clear();
    if (kind == SceneKind::Converge) {
        scene.setMaterial(0, material(0.7f, 0.68f, 0.62f, 0.f, 1.f));    // ground
        scene.setMaterial(1, material(0.6f, 0.6f, 0.5f, 0.f, 1.f));      // back wall
        scene.setMaterial(2, material(0.95f, 0.64f, 0.54f, 1.f, 0.45f)); // GGX metal block
        scene.setMaterial(3, material(0.3f, 0.55f, 0.8f, 0.5f, 0.3f));   // half-metal block
        scene.setMaterial(4, emissive(8.f, 7.5f, 6.5f));                 // light panel
        if (addInstance(0, 0, scaled(8.f, 8.f, 0.f, 0.f, -1.f)) == gpu_scene::kInvalidIndex ||
            addInstance(1, 1, boxTransform(Box{{-3.f, 0.f, -3.4f}, {3.f, 3.f, -3.f}})) == gpu_scene::kInvalidIndex ||
            addInstance(1, 2, boxTransform(Box{{-1.4f, 0.f, -2.2f}, {-0.4f, 1.1f, -1.2f}})) == gpu_scene::kInvalidIndex ||
            addInstance(1, 3, boxTransform(Box{{0.3f, 0.f, -1.9f}, {1.3f, 0.6f, -0.9f}})) == gpu_scene::kInvalidIndex) {
            return false;
        }
        const gpu_scene::GpuTransform panel = panelTransform(0.f, 2.4f, -1.6f, 1.2f);
        const u32 slot = addInstance(2, 4, panel);
        if (slot == gpu_scene::kInvalidIndex) {
            return false;
        }
        light_tree::appendEmissiveTriangles(w.meshes[2], panel, 8.f, false, w.lights, &w.triangleRefs, slot);
        const f32 rgb[3] = {8.f, 7.5f, 6.5f};
        for (const light_tree::LightTreeLight& l : w.lights) {
            w.table.push_back(restir::makeRestirLight(l, rgb));
        }
        const f32 pos[3] = {1.8f, 1.6f, -0.5f};
        light_tree::LightTreeLight point = light_tree::makePointLight(pos, 1.5f);
        w.lights.push_back(point);
        const f32 prgb[3] = {1.5f, 1.2f, 0.9f};
        w.table.push_back(restir::makeRestirLight(point, prgb));
        w.sky[0] = 0.05f;
        w.sky[1] = 0.06f;
        w.sky[2] = 0.08f;
    } else if (kind == SceneKind::Furnace) {
        scene.setMaterial(0, material(furnaceAlbedo, furnaceAlbedo, furnaceAlbedo, 0.f, 1.f));
        if (addInstance(1, 0, boxTransform(Box{{-0.6f, 0.2f, -1.6f}, {0.6f, 1.2f, -0.8f}})) == gpu_scene::kInvalidIndex) {
            return false;
        }
        w.sky[0] = w.sky[1] = w.sky[2] = 1.f;
    } else {
        scene.setMaterial(0, material(0.5f, 0.5f, 0.5f, 0.f, 1.f));
        scene.setMaterial(1, emissive(3.f, 3.f, 3.f));
        if (addInstance(0, 0, scaled(40.f, 40.f, 0.f, 0.f, 0.f)) == gpu_scene::kInvalidIndex) {
            return false;
        }
        const gpu_scene::GpuTransform panel = panelTransform(0.f, 1.5f, 0.f, 2.f);
        const u32 slot = addInstance(2, 1, panel);
        if (slot == gpu_scene::kInvalidIndex) {
            return false;
        }
        light_tree::appendEmissiveTriangles(w.meshes[2], panel, 3.f, false, w.lights, &w.triangleRefs, slot);
        const f32 rgb[3] = {3.f, 3.f, 3.f};
        for (const light_tree::LightTreeLight& l : w.lights) {
            w.table.push_back(restir::makeRestirLight(l, rgb));
        }
        // A low camera looking at the ground point below the emitter's centre (the panel is not in the way).
        eye[0] = 3.0;
        eye[1] = 0.4;
        eye[2] = 0.0;
        at[0] = 0.0;
        at[1] = 0.0;
        at[2] = 0.0;
        fov = 0.002;
        w.analytic = 0.5 * 3.0 * 4.0 * cornerFormFactor(1.0, 1.0, 1.5);
    }
    lookAt(w.camera, eye, at, fov, static_cast<f64>(width) / height, 0.1, 60.0);
    if (!w.lights.empty() && !w.tree.build(w.lights)) {
        return false;
    }
    ptEmitterRefsFromTree(w.tree, w.triangleRefs, w.emitterRefs);
    if (!buildPtEmitterMap(scene, w.emitterRefs.data(), static_cast<u32>(w.emitterRefs.size()), w.emitterMap)) {
        return false;
    }
    const u16* vpos[3] = {w.meshes[0].positions.data(), w.meshes[1].positions.data(), w.meshes[2].positions.data()};
    if (!w.cpu.build(scene, vpos, 3u)) {
        return false;
    }
    w.cpu.setLights(w.tree.emitters().data(), w.table.data(), static_cast<u32>(w.table.size()), w.emitterMap.data(),
                    static_cast<u32>(w.emitterMap.size()), scene.instanceHighWater());
    return true;
}

inline PtSettings baseSettings(const World& w) {
    PtSettings s{};
    for (u32 k = 0; k < 3u; ++k) {
        s.sky[k] = w.sky[k];
    }
    s.maxBounces = 5;
    s.rrStartBounce = 3;
    return s;
}

} // namespace pt_test
