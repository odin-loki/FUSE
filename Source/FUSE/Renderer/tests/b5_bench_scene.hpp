#pragma once

// Shared 1080p benchmark frame for the B5 device gates (test_b5_clustered_device.cpp, test_b5_frame_bench.cpp):
// an analytic scene — a ground slab, a back wall, two side walls and 96 spheres resting on the ground (100 SDF
// objects, hard union, so the ray march and the SDF shadows see exactly this geometry) — ray-cast once on the host
// into the G-buffer surfaces every B5 pass consumes: reversed-Z device depth + world normals + albedo (clustered
// deferred shading), world positions (SDF shadows), linear view depth + engine view-space normals + scene colour +
// roughness (HBAO / SSR), plus 1000 point lights spread through the room.
//
// Host-only, header-only test code. Pixel rays use the clustered kernel's own camera math
// (clustered_kernel::view_position_from_screen / view_to_world), so the depth each pass reconstructs lands on the
// surface that was cast.

#include <fuse/compute/ray_march.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>
#include <vector>

namespace b5bench {

using fuse::f32;
using fuse::u32;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;

inline constexpr f32 kNearPlane = 0.1f;
inline constexpr f32 kFarPlane = 200.f;
inline constexpr f32 kFovYRadians = 1.04719755f; // 60 degrees

struct Box {
    Vec3 lo;
    Vec3 hi;
    Vec3 albedo;
};

struct Sphere {
    Vec3 center;
    f32 radius = 1.f;
    Vec3 albedo;
};

struct Scene {
    std::vector<Box> boxes;
    std::vector<Sphere> spheres;
};

/// 4 boxes (ground, back wall, side walls) + 96 spheres = 100 SDF objects.
inline Scene makeScene() {
    Scene s;
    s.boxes.push_back({{-20.f, -1.f, -42.f}, {20.f, 0.f, 10.f}, {0.7f, 0.7f, 0.7f}});
    s.boxes.push_back({{-20.f, 0.f, -42.f}, {20.f, 12.f, -40.f}, {0.6f, 0.6f, 0.7f}});
    s.boxes.push_back({{-21.f, 0.f, -42.f}, {-20.f, 12.f, 10.f}, {0.8f, 0.2f, 0.2f}});
    s.boxes.push_back({{20.f, 0.f, -42.f}, {21.f, 12.f, 10.f}, {0.2f, 0.8f, 0.2f}});
    for (u32 row = 0; row < 8u; ++row) {
        for (u32 col = 0; col < 12u; ++col) {
            const u32 i = row * 12u + col;
            const f32 r = 0.6f + 0.6f * static_cast<f32>((i * 7u) % 11u) / 10.f;
            Sphere sphere{};
            sphere.center = {-16.5f + 3.f * static_cast<f32>(col), r, -35.f + 4.f * static_cast<f32>(row)};
            sphere.radius = r;
            sphere.albedo = {0.3f + 0.6f * static_cast<f32>(i % 3u) / 2.f, 0.3f + 0.6f * static_cast<f32>((i / 3u) % 3u) / 2.f,
                             0.3f + 0.6f * static_cast<f32>((i / 9u) % 3u) / 2.f};
            s.spheres.push_back(sphere);
        }
    }
    return s;
}

/// The same geometry as SDF objects (boxes first, hard union: alpha 0).
inline std::vector<fuse::compute::SdfObject> sdfObjects(const Scene& s) {
    std::vector<fuse::compute::SdfObject> out;
    for (const Box& b : s.boxes) {
        fuse::compute::SdfObject o{};
        o.type = static_cast<u32>(fuse::compute::SdfPrimitiveType::Box);
        o.position = (b.lo + b.hi) * 0.5f;
        o.params = (b.hi - b.lo) * 0.5f;
        o.alpha = 0.f;
        out.push_back(o);
    }
    for (const Sphere& sp : s.spheres) {
        fuse::compute::SdfObject o{};
        o.type = static_cast<u32>(fuse::compute::SdfPrimitiveType::Sphere);
        o.position = sp.center;
        o.params = {sp.radius, 0.f, 0.f};
        o.alpha = 0.f;
        out.push_back(o);
    }
    return out;
}

inline fuse::renderer::ClusterCameraDesc makeCamera(u32 width, u32 height) {
    fuse::renderer::ClusterCameraDesc camera{};
    camera.position = {0.f, 4.f, 8.f};
    camera.forward = Vec3{0.f, -3.f, -28.f}.normalized();
    camera.up = {0.f, 1.f, 0.f};
    camera.nearPlane = kNearPlane;
    camera.farPlane = kFarPlane;
    camera.screenWidth = width;
    camera.screenHeight = height;
    camera.fovYRadians = kFovYRadians;
    camera.reversedZ = true;
    return camera;
}

/// Surfaces of one frame (row-major, row 0 = top).
struct Frame {
    u32 width = 0;
    u32 height = 0;
    fuse::renderer::ClusterCameraDesc camera{};
    fuse::renderer::clustered_kernel::CameraView view{};
    std::vector<f32> deviceDepth;  ///< reversed-Z (near / view depth); 0 = sky
    std::vector<f32> viewDepth;    ///< linear view depth (> 0), 0 = sky
    std::vector<Vec3> normals;     ///< world space
    std::vector<Vec3> viewNormals; ///< engine view space (+Y up, -Z forward)
    std::vector<Vec3> albedo;
    std::vector<Vec4> worldPos; ///< w = 1 on a surface, 0 on sky
    std::vector<Vec3> sceneColor; ///< simple lit colour (SSR input)
    std::vector<f32> roughness;
    u32 surfacePixels = 0;
};

inline bool raySphere(const Vec3& o, const Vec3& d, const Sphere& s, f32& t) {
    const Vec3 oc = o - s.center;
    const f32 b = oc.dot(d);
    const f32 c = oc.dot(oc) - s.radius * s.radius;
    const f32 disc = b * b - c;
    if (disc < 0.f) {
        return false;
    }
    const f32 root = std::sqrt(disc);
    f32 hit = -b - root;
    if (hit <= 1e-4f) {
        hit = -b + root;
    }
    if (hit <= 1e-4f) {
        return false;
    }
    t = hit;
    return true;
}

inline bool rayBox(const Vec3& o, const Vec3& d, const Box& b, f32& t, Vec3& normal) {
    f32 t0 = 1e-4f;
    f32 t1 = 1e30f;
    int axisHit = -1;
    f32 signHit = 0.f;
    const f32 origin[3] = {o.x, o.y, o.z};
    const f32 dir[3] = {d.x, d.y, d.z};
    const f32 lo[3] = {b.lo.x, b.lo.y, b.lo.z};
    const f32 hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dir[a]) < 1e-12f) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.f / dir[a];
        f32 ta = (lo[a] - origin[a]) * inv;
        f32 tb = (hi[a] - origin[a]) * inv;
        f32 sign = -1.f; // entering through the low face
        if (ta > tb) {
            std::swap(ta, tb);
            sign = 1.f;
        }
        if (ta > t0) {
            t0 = ta;
            axisHit = a;
            signHit = sign;
        }
        t1 = std::min(t1, tb);
        if (t0 > t1) {
            return false;
        }
    }
    if (axisHit < 0) {
        return false; // origin inside the box
    }
    t = t0;
    normal = {axisHit == 0 ? signHit : 0.f, axisHit == 1 ? signHit : 0.f, axisHit == 2 ? signHit : 0.f};
    return true;
}

/// Ray-casts the scene at `width` x `height` (one ray per pixel centre).
inline Frame makeFrame(const Scene& scene, u32 width, u32 height) {
    namespace ck = fuse::renderer::clustered_kernel;
    Frame f;
    f.width = width;
    f.height = height;
    f.camera = makeCamera(width, height);
    f.view = ck::make_camera(f.camera);
    const usize pixels = static_cast<usize>(width) * height;
    f.deviceDepth.assign(pixels, 0.f);
    f.viewDepth.assign(pixels, 0.f);
    f.normals.assign(pixels, Vec3{0.f, 1.f, 0.f});
    f.viewNormals.assign(pixels, Vec3{0.f, 0.f, 1.f});
    f.albedo.assign(pixels, Vec3{});
    f.worldPos.assign(pixels, Vec4{});
    f.sceneColor.assign(pixels, Vec3{0.2f, 0.3f, 0.5f});
    f.roughness.assign(pixels, 1.f);
    const Vec3 sun = Vec3{0.4f, 1.f, 0.3f}.normalized();
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 sx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 sy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
            const Vec3 viewDir = ck::view_position_from_screen(sx, sy, 1.f, f.view.tan_x, f.view.tan_y);
            const Vec3 dir = (ck::view_to_world(f.view, viewDir) - f.view.position).normalized();
            f32 best = 1e30f;
            Vec3 normal{};
            Vec3 albedo{};
            bool hit = false;
            for (const Box& b : scene.boxes) {
                f32 t = 0.f;
                Vec3 n{};
                if (rayBox(f.view.position, dir, b, t, n) && t < best) {
                    best = t;
                    normal = n;
                    albedo = b.albedo;
                    hit = true;
                }
            }
            for (const Sphere& s : scene.spheres) {
                f32 t = 0.f;
                if (raySphere(f.view.position, dir, s, t) && t < best) {
                    best = t;
                    normal = (f.view.position + dir * t - s.center).normalized();
                    albedo = s.albedo;
                    hit = true;
                }
            }
            const usize i = static_cast<usize>(y) * width + x;
            if (!hit) {
                continue;
            }
            const Vec3 p = f.view.position + dir * best;
            const Vec3 pv = ck::world_to_view(f.view, p);
            const f32 depth = -pv.z;
            if (!(depth > kNearPlane) || depth > kFarPlane) {
                continue;
            }
            f.deviceDepth[i] = kNearPlane / depth;
            f.viewDepth[i] = depth;
            f.normals[i] = normal;
            f.viewNormals[i] = {normal.dot(f.view.right), normal.dot(f.view.up), normal.dot(f.view.back)};
            f.albedo[i] = albedo;
            f.worldPos[i] = {p.x, p.y, p.z, 1.f};
            f.sceneColor[i] = albedo * (0.15f + std::max(normal.dot(sun), 0.f));
            f.roughness[i] = normal.y > 0.9f ? 0.1f : 0.6f; // glossy floor, rough walls / spheres
            ++f.surfacePixels;
        }
    }
    return f;
}

/// `count` point lights through the room (deterministic).
inline std::vector<fuse::renderer::PointLightInput> makeLights(u32 count, u32 seed = 20260929u) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u01(0.f, 1.f);
    std::vector<fuse::renderer::PointLightInput> lights(count);
    for (fuse::renderer::PointLightInput& l : lights) {
        l.position = {-19.f + 38.f * u01(rng), 0.3f + 5.7f * u01(rng), -39.f + 45.f * u01(rng)};
        l.color = {0.3f + 0.7f * u01(rng), 0.3f + 0.7f * u01(rng), 0.3f + 0.7f * u01(rng)};
        l.intensity = 1.f + 7.f * u01(rng);
        l.radius = 2.5f + 3.5f * u01(rng);
    }
    return lights;
}

} // namespace b5bench
