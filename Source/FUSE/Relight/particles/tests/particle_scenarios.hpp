// FUSE Relight RL-3.6 tests: the fixture particle systems built in code (shared by the CPU gates and the Lavapipe
// gates). Tests/relight/fixtures/particles/scenes/mod.usda authors the same systems as USD ParticleSystemAPI prims; the
// CPU gate checks that the USD path (descFromUsd and the RL-3.2 record path) produces descriptions with the same
// hash, so the GPU gate (which does not link the USD reader) runs exactly the fixture systems.
//
//   fountain  mesh_<H> replacement system: Poisson 600/s, 0.5..1 s, 200 cm/s along the quad normal, gravity
//             -980 cm/s^2, colour gradients and size curves (Bezier tangents with tangent times), hideEmitter; the
//             emitter moves along x (initialVelocityFromMotion 0.5)
//   sparks    mat_<H> system: constant count (rate >= max 64), 0.25..0.5 s, 300 cm/s in a 45 degree cone, drag,
//             motion trail, random flips, velocity-aligned billboards
//   smoke     mat_<H> system: bursts of 0.25 s at 480/s, 2..3 s, turbulence, attractor, drag, maxVelocity curves
//             (x/z 30 cm/s, y 80 cm/s), cylindrical billboards, rotation-speed curve, initial rotation deviation
#pragma once

#include <fuse/relight/particles/curve_bake.hpp>
#include <fuse/relight/particles/particle_desc.hpp>
#include <fuse/relight/particles/particle_system.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace rl_particles_test {

using namespace fuse::relight::particles;

struct EmitterData {
    std::vector<float> positions;
    std::vector<std::uint32_t> colors;
    std::vector<float> texcoords;
    std::vector<std::uint32_t> indices;
    EmitterMesh view() const { return EmitterMesh{positions, {}, colors, texcoords, indices}; }
};

/// A size x size quad in the XZ plane at y = 0 (normal +Y), 2 x 2 cells, vertex colours and texcoords.
inline EmitterData quadEmitter(float size) {
    EmitterData e;
    for (int j = 0; j <= 2; ++j) {
        for (int i = 0; i <= 2; ++i) {
            const float u = static_cast<float>(i) * 0.5f, v = static_cast<float>(j) * 0.5f;
            e.positions.insert(e.positions.end(), {(u - 0.5f) * size, 0.f, (v - 0.5f) * size});
            const std::uint32_t r = static_cast<std::uint32_t>(i * 127), g = static_cast<std::uint32_t>(j * 127);
            e.colors.push_back(r | (g << 8) | (200u << 16) | (255u << 24));
            e.texcoords.insert(e.texcoords.end(), {u, v});
        }
    }
    for (std::uint32_t j = 0; j < 2; ++j) {
        for (std::uint32_t i = 0; i < 2; ++i) {
            const std::uint32_t a = j * 3 + i, b = a + 1, c = a + 3, d = a + 4;
            // Counter-clockwise seen from +Y: normal +Y.
            e.indices.insert(e.indices.end(), {a, c, b, b, c, d});
        }
    }
    return e;
}

inline FloatCurve curve(std::vector<float> times, std::vector<float> values) {
    FloatCurve c;
    c.times = std::move(times);
    c.values = std::move(values);
    return c;
}

inline Channel bake2(const FloatCurve& x, const FloatCurve& y, float dx, float dy) {
    std::vector<float> bx, by;
    const bool hx = bakeFloatCurve(x, bx, kCurveResolution, dx);
    const bool hy = bakeFloatCurve(y, by, kCurveResolution, dy);
    Channel out;
    combineChannels({&bx, &by}, {hx, hy}, {dx, dy}, out);
    return out;
}
inline Channel bake1(const FloatCurve& x, float dx) {
    std::vector<float> bx;
    const bool hx = bakeFloatCurve(x, bx, kCurveResolution, dx);
    Channel out;
    combineChannels({&bx}, {hx}, {dx}, out);
    return out;
}
inline Channel bake3(const FloatCurve& x, const FloatCurve& y, const FloatCurve& z, float d) {
    std::vector<float> bx, by, bz;
    const bool hx = bakeFloatCurve(x, bx, kCurveResolution, d);
    const bool hy = bakeFloatCurve(y, by, kCurveResolution, d);
    const bool hz = bakeFloatCurve(z, bz, kCurveResolution, d);
    Channel out;
    combineChannels({&bx, &by, &bz}, {hx, hy, hz}, {d, d, d}, out);
    return out;
}
inline Channel gradient(std::vector<float> times, std::vector<std::array<float, 4>> values) {
    ColorGradient g;
    g.times = std::move(times);
    g.values = std::move(values);
    std::vector<std::array<float, 4>> out;
    bakeColorGradient(g, out);
    return out;
}

/// mesh_<H> of the fixture (scenes/mod.usda): /RootNode/meshes/mesh_00000000F0074A10.
inline ParticleSystemDesc fountainDesc() {
    ParticleSystemDesc d = schemaDefaultDesc();
    GpuSystemDesc& g = d.gpu;
    g.maxNumParticles = 1000;
    g.spawnRatePerSecond = 600.f;
    g.minTimeToLive = 0.5f;
    g.maxTimeToLive = 1.f;
    g.initialVelocityFromNormal = 200.f;
    g.initialVelocityFromMotion = 0.5f;
    g.gravityForce = -980.f;
    g.flags = kFlagHideEmitter | kFlagUseSpawnTexcoords;
    d.minColor = gradient({0.f, 0.5f, 1.f}, {{1.f, 1.f, 1.f, 1.f}, {1.f, 0.5f, 0.25f, 1.f}, {1.f, 0.f, 0.f, 0.f}});
    d.maxColor = gradient({0.f, 1.f}, {{0.5f, 0.75f, 1.f, 1.f}, {0.f, 0.f, 1.f, 0.f}});
    FloatCurve sx = curve({0.f, 1.f}, {4.f, 12.f});
    sx.inTangentTypes = {TangentType::Auto, TangentType::Auto};
    sx.outTangentTypes = {TangentType::Auto, TangentType::Auto};
    sx.inTangentValues = {0.f, -2.f};
    sx.outTangentValues = {3.f, 0.f};
    sx.inTangentTimes = {0.f, -0.25f};
    sx.outTangentTimes = {0.25f, 0.f};
    FloatCurve sy = curve({0.f, 0.5f, 1.f}, {4.f, 8.f, 6.f});
    sy.outTangentTypes = {TangentType::Linear, TangentType::Step, TangentType::Linear};
    d.minSize = bake2(sx, sy, 10.f, 10.f);
    d.maxSize = bake2(curve({0.f, 1.f}, {8.f, 16.f}), FloatCurve{}, 10.f, 10.f);
    return d;
}

/// mat_<H> of the fixture: /RootNode/Looks/mat_00000000005A4B50.
inline ParticleSystemDesc sparksDesc() {
    ParticleSystemDesc d = schemaDefaultDesc();
    GpuSystemDesc& g = d.gpu;
    g.maxNumParticles = 64;
    g.spawnRatePerSecond = 64.f;
    g.minTimeToLive = 0.25f;
    g.maxTimeToLive = 0.5f;
    g.initialVelocityFromNormal = 300.f;
    g.initialVelocityConeAngleDegrees = 45.f;
    g.dragCoefficient = 1.5f;
    g.motionTrailMultiplier = 2.f;
    g.randomFlipAxis = static_cast<std::uint32_t>(RandomFlipAxis::Both);
    g.flags = kFlagEnableMotionTrail | kFlagAlignParticlesToVelocity;
    d.minColor = {{1.f, 0.75f, 0.25f, 1.f}, {1.f, 0.25f, 0.f, 0.5f}};
    d.maxColor = {{1.f, 1.f, 0.5f, 1.f}, {1.f, 0.5f, 0.f, 0.5f}};
    d.minSize = {{2.f, 2.f, 0.f, 0.f}, {1.f, 1.f, 0.f, 0.f}};
    d.maxSize = {{3.f, 3.f, 0.f, 0.f}, {1.f, 1.f, 0.f, 0.f}};
    return d;
}

/// mat_<H> of the fixture: /RootNode/Looks/mat_000000000005E0CE.
inline ParticleSystemDesc smokeDesc() {
    ParticleSystemDesc d = schemaDefaultDesc();
    GpuSystemDesc& g = d.gpu;
    g.maxNumParticles = 2000;
    g.spawnRatePerSecond = 480.f;
    g.spawnBurstDuration = 0.25f;
    g.minTimeToLive = 2.f;
    g.maxTimeToLive = 3.f;
    g.initialVelocityFromNormal = 50.f;
    g.initialVelocityConeAngleDegrees = 20.f;
    g.initialRotationDeviationDegrees = 90.f;
    g.turbulenceForce = 20.f;
    g.turbulenceFrequency = 0.0625f;
    g.attractorPosition[0] = 0.f;
    g.attractorPosition[1] = 300.f;
    g.attractorPosition[2] = 0.f;
    g.attractorForce = 50.f;
    g.attractorRadius = 100.f;
    g.dragCoefficient = 0.5f;
    g.billboardType = static_cast<std::uint32_t>(BillboardType::FaceCameraUpAxisLocked);
    g.flags = kFlagUseTurbulence;
    d.minColor = {{0.5f, 0.5f, 0.5f, 0.75f}, {0.25f, 0.25f, 0.25f, 0.f}};
    d.maxColor = {{0.75f, 0.75f, 0.75f, 0.75f}, {0.5f, 0.5f, 0.5f, 0.f}};
    d.minSize = {{20.f, 20.f, 0.f, 0.f}, {60.f, 60.f, 0.f, 0.f}};
    d.maxSize = {{30.f, 30.f, 0.f, 0.f}, {90.f, 90.f, 0.f, 0.f}};
    d.minRotationSpeed = bake1(curve({0.f, 1.f}, {0.5f, -0.5f}), 0.f);
    d.maxRotationSpeed = bake1(curve({0.f, 1.f}, {1.f, 0.f}), 0.f);
    d.maxVelocity = bake3(curve({0.f, 1.f}, {30.f, 30.f}), curve({0.f, 0.5f, 1.f}, {80.f, 60.f, 40.f}), curve({0.f, 1.f}, {30.f, 30.f}), -1.f);
    return d;
}

struct Scenario {
    std::string name;
    ParticleSystemDesc desc;
    EmitterData emitter;
    std::uint64_t materialKey = 0;
    /// Emitter world transform at frame f (row-major 3x4).
    Mat34 (*world)(std::uint32_t frame) = nullptr;
};

inline Mat34 translation(float x, float y, float z) { return Mat34{1.f, 0.f, 0.f, x, 0.f, 1.f, 0.f, y, 0.f, 0.f, 1.f, z}; }
inline Mat34 fountainWorld(std::uint32_t f) {
    return translation(20.f * std::sin(static_cast<float>(f) * 0.05f), 0.f, 0.f);
}
inline Mat34 sparksWorld(std::uint32_t) { return translation(-150.f, 20.f, 0.f); }
inline Mat34 smokeWorld(std::uint32_t) { return translation(150.f, 0.f, 0.f); }

inline std::vector<Scenario> scenarios() {
    return {
        {"fountain", fountainDesc(), quadEmitter(100.f), 0xF0074A10ull, &fountainWorld},
        {"sparks", sparksDesc(), quadEmitter(20.f), 0x005A4B50ull, &sparksWorld},
        {"smoke", smokeDesc(), quadEmitter(40.f), 0x0005E0CEull, &smokeWorld},
    };
}

inline constexpr float kDt = 1.f / 60.f;

/// Column-major camera at (0, 60, 500) looking down -Z (view = world axes), and its perspective projection.
inline FrameInput frameInput(std::uint32_t frame) {
    FrameInput in;
    in.frameIndex = frame;
    in.deltaTimeSecs = kDt;
    in.absoluteTimeSecs = static_cast<double>(frame) * static_cast<double>(kDt);
    in.viewToWorld = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 60.f, 500.f, 1.f};
    // proj * worldToView; worldToView = translate(-camPos). Perspective 60 degrees, aspect 16:9, near 1.
    const float f = 1.f / std::tan(0.5f * 1.0471975512f);
    const float aspect = 16.f / 9.f;
    std::array<float, 16> proj{f / aspect, 0.f, 0.f, 0.f, 0.f, f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 1.f, 0.f};
    std::array<float, 16> view{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -60.f, -500.f, 1.f};
    std::array<float, 16> m{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += proj[k * 4 + r] * view[c * 4 + k];
            }
            m[c * 4 + r] = s;
        }
    }
    in.prevWorldToProjection = m;
    in.renderWidth = 1280;
    in.renderHeight = 720;
    in.up = {0.f, 1.f, 0.f};
    in.sceneScale = 1.f;
    return in;
}

inline ManagerConfig smallConfig(std::uint32_t framesInFlight) {
    ManagerConfig c;
    c.maxSystems = 8;
    c.particleCapacity = 4096;
    c.vertexCapacity = 4096 * 8;
    c.maxSpawnContexts = 64;
    c.geometryVertexCapacity = 1024;
    c.geometryIndexCapacity = 4096;
    c.framesInFlight = framesInFlight;
    return c;
}

/// Drives `scenarios` for frames [0, frames): per frame beginFrame, one spawn per scenario, simulate; `perFrame`
/// is called after simulate with the frame number.
struct Driver {
    std::vector<Scenario> list;
    std::vector<EmitterMeshId> meshes;
    std::vector<std::uint64_t> hashes;

    explicit Driver(std::vector<Scenario> s) : list(std::move(s)) {}

    bool registerMeshes(ParticleSystemManager& m) {
        meshes.clear();
        hashes.clear();
        for (const Scenario& s : list) {
            meshes.push_back(m.registerMesh(s.emitter.view()));
            hashes.push_back(hashDesc(s.desc));
            if (meshes.back() == kInvalidMesh) {
                return false;
            }
        }
        return true;
    }

    template <typename F>
    bool run(ParticleSystemManager& m, ParticleBackend& backend, std::uint32_t first, std::uint32_t frames, F&& perFrame) {
        for (std::uint32_t f = first; f < first + frames; ++f) {
            m.beginFrame(frameInput(f), backend);
            for (std::size_t i = 0; i < list.size(); ++i) {
                SpawnRequest r;
                r.desc = &list[i].desc;
                r.descHash = hashes[i];
                r.materialKey = list[i].materialKey;
                r.mesh = meshes[i];
                r.objectToWorld = list[i].world(f);
                r.prevObjectToWorld = list[i].world(f == 0 ? 0 : f - 1);
                m.spawn(r);
            }
            if (!m.simulate(backend)) {
                return false;
            }
            perFrame(f);
        }
        return true;
    }
};

} // namespace rl_particles_test
