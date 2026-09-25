// FUSE Relight RL-5.6 tests: scenes and helpers shared by the CPU and Vulkan gates.
#pragma once

#include <fuse/relight/render/volumetrics/volumetrics.hpp>

#include <fuse/relight/particles/particle_types.hpp>
#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vol_test {

namespace vol = fuse::relight::render::volumetrics;
namespace pt = fuse::relight::render::pathtrace;
namespace rl = fuse::relight::render::lights;
namespace lk = fuse::relight::lightk;
namespace pa = fuse::relight::particles;
using fuse::u32;

inline pt::PtCamera camera(float ox, float oy, float oz, float fx, float fy, float fz, float fovDeg) {
    pt::PtCamera c;
    c.origin[0] = ox;
    c.origin[1] = oy;
    c.origin[2] = oz;
    c.forward[0] = fx;
    c.forward[1] = fy;
    c.forward[2] = fz;
    c.fovY = fovDeg * 3.14159265f / 180.f;
    return c;
}

/// B8G8R8A8 (B in the low byte), from linear [0, 1] rgba.
inline u32 packColor(float r, float g, float b, float a) {
    auto q = [](float v) { return u32(std::lround(std::min(std::max(v, 0.f), 1.f) * 255.f)); };
    return q(b) | (q(g) << 8u) | (q(r) << 16u) | (q(a) << 24u);
}

/// A camera-facing (for a camera looking down -z) axis-aligned quad at (cx, cy, cz), half size s, in the RL-3.6
/// billboard strip order (kQuadOffsets: (-,+) (+,+) (-,-) (+,-)).
inline void addQuad(std::vector<pa::GpuParticleVertex>& out, float cx, float cy, float cz, float s, u32 color) {
    const float o[4][2] = {{-1.f, 1.f}, {1.f, 1.f}, {-1.f, -1.f}, {1.f, -1.f}};
    for (const auto& k : o) {
        pa::GpuParticleVertex v{};
        v.position[0] = cx + k[0] * s;
        v.position[1] = cy + k[1] * s;
        v.position[2] = cz;
        v.color = color;
        v.texcoord[0] = 0.5f + 0.5f * k[0];
        v.texcoord[1] = 0.5f - 0.5f * k[1];
        out.push_back(v);
    }
}

/// The volumetrics test lights: a sphere, a downward rect and a delta distant light.
inline void addTestLights(rl::RelightLightSet& set) {
    set.addLight(rl::makeSphereLight(lk::float3(0.8f, 2.2f, -2.f), 0.3f, lk::float3(30.f, 24.f, 18.f)), 1u);
    set.addLight(rl::makeRectLight(lk::float3(-1.5f, 3.f, -4.f), lk::float3(0.6f, 0.f, 0.f), lk::float3(0.f, 0.f, -0.4f),
                                   lk::float3(12.f, 14.f, 20.f)),
                 2u);
    set.addLight(rl::makeDistantLight(lk::float3(0.3f, -1.f, 0.2f), 0.f, lk::float3(0.8f, 0.8f, 0.7f)), 3u);
}

inline vol::VolFrameDesc baseDesc(u32 w, u32 h, u32 gx, u32 gy, u32 gz) {
    vol::VolFrameDesc d;
    d.camera = camera(0.f, 0.f, 4.f, 0.f, 0.f, -1.f, 50.f);
    d.prevCamera = d.camera;
    d.width = w;
    d.height = h;
    d.gridX = gx;
    d.gridY = gy;
    d.gridZ = gz;
    d.nearZ = 0.1f;
    d.farZ = 12.f;
    d.medium.density = 0.15f;
    d.medium.anisotropy = 0.3f;
    d.medium.albedo[0] = d.medium.albedo[1] = d.medium.albedo[2] = 0.9f;
    d.temporalAlpha = 0.1f;
    d.candidates = 4;
    d.flags = vol::kVolLights | vol::kVolFog | vol::kVolReproject;
    return d;
}

/// A shadowing scene for the path tracer's CPU reference BVH: a horizontal occluder at y = 1 over x < 0 and a sphere
/// light above the origin.
inline pt::PtScene shadowScene() {
    pt::PtScene s;
    pt::PtMaterial m;
    s.materials.push_back(m);
    pt::PtMesh q;
    const float c[3] = {-10.f, 1.f, 0.f}, u[3] = {0.f, 0.f, 20.f}, v[3] = {10.f, 0.f, 0.f};
    const float sgn[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
    for (const auto& k : sgn) {
        for (int a = 0; a < 3; ++a) {
            q.positions.push_back(c[a] + k[0] * u[a] + k[1] * v[a]);
        }
    }
    q.indices = {0, 1, 2, 0, 2, 3};
    s.meshes.push_back(q);
    pt::PtInstance inst;
    inst.mesh = 0;
    s.instances.push_back(inst);
    s.lights.push_back(rl::makeSphereLight(lk::float3(0.f, 3.f, -2.f), 0.2f, lk::float3(40.f, 40.f, 40.f)));
    s.camera = camera(0.f, 0.f, 4.f, 0.f, 0.f, -1.f, 60.f);
    s.prevCamera = s.camera;
    return s;
}

} // namespace vol_test
