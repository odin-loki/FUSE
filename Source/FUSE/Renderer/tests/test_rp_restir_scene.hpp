#pragma once
// WP-7.2 gates: the tiny ReSTIR scene shared by test_rp_restir_cpu.cpp and test_rp_restir.cpp.
//
//   geometry   ground plane y = 0 (square of kGroundSize) + 4 axis-aligned boxes (two blocks, a back wall, a
//              floating slab under the light panel), each with its own albedo; the same boxes are WP-1.1 GPU-scene
//              instances in the Lavapipe gates (vsmr_test::World: mesh 0 plane, mesh 1 unit box)
//   lights     a ceiling panel of gx x gz cells, two emissive triangles each (50 x 100 -> 10 000 triangles),
//              facing down, per-triangle RGB radiance (a warm / cool gradient x a random factor, 1 % hot spots x 40);
//              the panel is light-only geometry (not traced), as ReSTIR's area lights are sampled, not hit
//   camera     looking down at the blocks from the front
// The analytic tracer (Tracer) is exact in double precision; it implements both the f32 RestirCpuScene of the
// CPU runner and the f64 RestirReferenceScene of the ground truth.
#include "test_rp_vsm_raster_common.hpp"

#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/restir/restir.hpp>
#include <fuse/renderer/restir/restir_reference.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace restir_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::renderer::light_tree::LightTree;
using fuse::renderer::light_tree::LightTreeLight;
using fuse::renderer::restir::RestirHitF;
using fuse::renderer::restir::RestirLight;
using fuse::renderer::restir::RestirSurfaceF;
using fuse::renderer::restir::RV3;
using vsmr_test::Box;
using vsmr_test::Camera;
using vsmr_test::D3;
using vsmr_test::World;

constexpr f32 kGroundSize = 24.f;
constexpr f32 kGroundAlbedo[3] = {0.7f, 0.68f, 0.62f};
constexpr f32 kPanelY = 3.2f;

struct Scene {
    World world;
    std::vector<std::array<f32, 3>> boxAlbedo;
    std::vector<LightTreeLight> lights;
    std::vector<RestirLight> table;
    LightTree tree;
    Camera camera;
};

inline void addBox(Scene& s, const Box& b, f32 r, f32 g, f32 bl) {
    s.world.boxes.push_back(b);
    s.boxAlbedo.push_back({r, g, bl});
}

/// The scene (gx x gz panel cells -> 2 gx gz emissive triangles) and a camera for `width` x `height`.
inline bool makeScene(Scene& s, u32 width, u32 height, u32 gx = 50, u32 gz = 100) {
    s = Scene{};
    s.world.groundSize = kGroundSize;
    addBox(s, Box{{-1.6, 0.0, -5.2}, {-0.4, 1.3, -4.0}}, 0.8f, 0.35f, 0.25f);
    addBox(s, Box{{0.6, 0.0, -3.4}, {2.0, 0.7, -2.4}}, 0.3f, 0.6f, 0.8f);
    addBox(s, Box{{-6.0, 0.0, -9.0}, {6.0, 4.0, -8.6}}, 0.6f, 0.6f, 0.5f);
    addBox(s, Box{{-0.8, 1.8, -3.9}, {0.8, 1.9, -2.7}}, 0.5f, 0.5f, 0.5f);
    vsmr_test::Lcg rng;
    const f32 x0 = -2.f;
    const f32 x1 = 2.f;
    const f32 z0 = -6.f;
    const f32 z1 = -1.f;
    const f32 dx = (x1 - x0) / static_cast<f32>(gx);
    const f32 dz = (z1 - z0) / static_cast<f32>(gz);
    s.lights.reserve(static_cast<size_t>(gx) * gz * 2u);
    s.table.reserve(s.lights.capacity());
    for (u32 j = 0; j < gz; ++j) {
        for (u32 i = 0; i < gx; ++i) {
            const f32 ax = x0 + dx * static_cast<f32>(i);
            const f32 bx = ax + dx;
            const f32 az = z0 + dz * static_cast<f32>(j);
            const f32 bz = az + dz;
            const f32 fx = static_cast<f32>(i) / static_cast<f32>(gx);
            const f32 fz = static_cast<f32>(j) / static_cast<f32>(gz);
            for (u32 t = 0; t < 2u; ++t) {
                f32 k = 0.5f + static_cast<f32>(rng.next());
                if (rng.next() < 0.01) {
                    k *= 40.f;
                }
                const f32 rgb[3] = {k * (1.5f + 1.0f * fx), k * (1.2f + 0.3f * fz), k * (0.8f + 1.2f * (1.f - fx))};
                LightTreeLight l{};
                if (t == 0u) {
                    const f32 v0[3] = {ax, kPanelY, az};
                    const f32 v1[3] = {bx, kPanelY, az};
                    const f32 v2[3] = {ax, kPanelY, bz};
                    l = fuse::renderer::light_tree::makeTriangleLight(v0, v1, v2, std::max(rgb[0], std::max(rgb[1], rgb[2])));
                } else {
                    const f32 v0[3] = {bx, kPanelY, az};
                    const f32 v1[3] = {bx, kPanelY, bz};
                    const f32 v2[3] = {ax, kPanelY, bz};
                    l = fuse::renderer::light_tree::makeTriangleLight(v0, v1, v2, std::max(rgb[0], std::max(rgb[1], rgb[2])));
                }
                l.source = static_cast<u32>(s.lights.size());
                s.lights.push_back(l);
                s.table.push_back(fuse::renderer::restir::makeRestirLight(l, rgb));
            }
        }
    }
    if (!s.tree.build(s.lights)) {
        return false;
    }
    s.camera.eye = {0.4, 2.3, 1.6};
    s.camera.at = {0.0, 0.4, -4.6};
    s.camera.width = width;
    s.camera.height = height;
    s.camera.zNear = 0.1f;
    s.camera.zFar = 60.f;
    s.camera.build();
    return true;
}

inline fuse::renderer::restir::RestirCamera restirCamera(const Camera& c) {
    fuse::renderer::restir::RestirCamera rc{};
    for (u32 i = 0; i < 16u; ++i) {
        rc.viewProj[i] = c.viewProj.m[i];
    }
    rc.position[0] = static_cast<f32>(c.eye.x);
    rc.position[1] = static_cast<f32>(c.eye.y);
    rc.position[2] = static_cast<f32>(c.eye.z);
    const D3 f = vsmr_test::normalize(c.at - c.eye);
    rc.forward[0] = static_cast<f32>(f.x);
    rc.forward[1] = static_cast<f32>(f.y);
    rc.forward[2] = static_cast<f32>(f.z);
    return rc;
}

/// Exact analytic tracer over the ground and the boxes.
class Tracer final : public fuse::renderer::restir::RestirCpuScene, public fuse::renderer::restir::RestirReferenceScene {
public:
    explicit Tracer(const Scene& scene) : m_scene(&scene) {}

    /// Closest hit in (tMin, tMax): t, the outward face normal, the albedo.
    bool hit(const D3& o, const D3& d, f64 tMin, f64 tMax, f64& t, D3& n, f32 albedo[3]) const {
        u32 box = 0;
        const World::Hit h = m_scene->world.cast(o, d, tMin, tMax, t, &box);
        if (h == World::kMiss) {
            return false;
        }
        if (h == World::kGround) {
            n = {0.0, 1.0, 0.0};
            for (u32 k = 0; k < 3u; ++k) {
                albedo[k] = kGroundAlbedo[k];
            }
            return true;
        }
        const Box& b = m_scene->world.boxes[box];
        const D3 p = o + d * t;
        const f64 dist[6] = {std::fabs(p.x - b.lo.x), std::fabs(p.x - b.hi.x), std::fabs(p.y - b.lo.y),
                             std::fabs(p.y - b.hi.y), std::fabs(p.z - b.lo.z), std::fabs(p.z - b.hi.z)};
        const D3 normals[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        u32 best = 0;
        for (u32 k = 1; k < 6u; ++k) {
            if (dist[k] < dist[best]) {
                best = k;
            }
        }
        n = normals[best];
        for (u32 k = 0; k < 3u; ++k) {
            albedo[k] = m_scene->boxAlbedo[box][k];
        }
        return true;
    }

    // RestirCpuScene (f32 rays)
    bool occluded(const RV3& o, const RV3& d, f32 tMax) const override {
        f64 t = 0.0;
        D3 n{};
        f32 a[3];
        return hit(D3{o.x, o.y, o.z}, D3{d.x, d.y, d.z}, 1e-7, tMax, t, n, a);
    }
    bool traceHit(const RV3& o, const RV3& d, f32 tMin, f32 tMax, RestirHitF& h) const override {
        f64 t = 0.0;
        D3 n{};
        f32 a[3];
        if (!hit(D3{o.x, o.y, o.z}, D3{d.x, d.y, d.z}, tMin, tMax, t, n, a)) {
            return false;
        }
        h.t = static_cast<f32>(t);
        h.normal = RV3{static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)};
        h.albedo = RV3{a[0], a[1], a[2]};
        return true;
    }
    // RestirReferenceScene (f64 rays)
    bool occluded(const f64 o[3], const f64 d[3], f64 tMax) const override {
        f64 t = 0.0;
        D3 n{};
        f32 a[3];
        return hit(D3{o[0], o[1], o[2]}, D3{d[0], d[1], d[2]}, 1e-7, tMax, t, n, a);
    }
    bool traceHit(const f64 o[3], const f64 d[3], f64 tMin, f64 tMax, f64& t, f64 normal[3], f64 albedo[3]) const override {
        D3 n{};
        f32 a[3];
        if (!hit(D3{o[0], o[1], o[2]}, D3{d[0], d[1], d[2]}, tMin, tMax, t, n, a)) {
            return false;
        }
        normal[0] = n.x;
        normal[1] = n.y;
        normal[2] = n.z;
        for (u32 k = 0; k < 3u; ++k) {
            albedo[k] = a[k];
        }
        return true;
    }

private:
    const Scene* m_scene;
};

/// Primary visible points of the camera (exact ray cast; f32 surfaces for the runner, f64 for the reference).
inline void castSurfaces(const Scene& s, std::vector<RestirSurfaceF>& out, std::vector<fuse::renderer::restir::RestirRefSurface>& ref) {
    const Camera& c = s.camera;
    Tracer tracer(s);
    out.assign(static_cast<size_t>(c.width) * c.height, RestirSurfaceF{});
    ref.assign(out.size(), fuse::renderer::restir::RestirRefSurface{});
    const D3 forward = vsmr_test::normalize(c.at - c.eye);
    for (u32 y = 0; y < c.height; ++y) {
        for (u32 x = 0; x < c.width; ++x) {
            D3 o{};
            D3 d{};
            c.ray(x, y, o, d);
            f64 t = 0.0;
            D3 n{};
            f32 albedo[3];
            if (!tracer.hit(o, d, 0.0, 1e3, t, n, albedo)) {
                continue;
            }
            const D3 p = o + d * t;
            // f32 surface, then the f64 reference at exactly the same (rounded) point
            RestirSurfaceF& sf = out[y * c.width + x];
            sf.p = RV3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
            sf.n = RV3{static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)};
            sf.depth = static_cast<f32>(vsmr_test::dot(p - c.eye, forward));
            fuse::renderer::restir::RestirRefSurface& rs = ref[y * c.width + x];
            rs.valid = true;
            rs.p[0] = sf.p.x;
            rs.p[1] = sf.p.y;
            rs.p[2] = sf.p.z;
            rs.n[0] = sf.n.x;
            rs.n[1] = sf.n.y;
            rs.n[2] = sf.n.z;
        }
    }
}

} // namespace restir_test
