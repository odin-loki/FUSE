// WP-6.0 acceleration structures, CPU gates (stub-safe). Lavapipe gates: test_rp_rt.cpp.
//
//   layout     AsInstance == VkAccelerationStructureInstanceKHR (64 bytes, field offsets), probe ray /
//              hit and push-constant layouts, shader constants (rt_common.glsl / .slang) == C++
//   pack       packRtInstance: free slots, missing BLAS, masks from instance flags, custom index, flags
//              byte, transform copied bit for bit
//   caps       the T2 rule (evaluateRtCapabilities) over synthetic RendererCaps: T2 with AS + ray query
//              + BDA usable; T0 / T1 / capped T2 / missing extension / invalid caps not, each with its
//              reason; queryRtCapabilities(nullptr) unusable
//   reference  the two-level spatial::BVH reference == brute force over every instance and triangle
//              (same (instance, primitive), t within 1e-9) on 4096 random rays incl. cull masks;
//              robust-ray fraction reported
//   refit      reference refit (moved instances, deformed mesh) == a fresh build on the same rays
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/renderer/rt/rt_types.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::rt;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- layout ----------------------------------------------------------------------------------------

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int runLayout() {
    static_assert(offsetof(AsInstance, transform) == 0u);
    static_assert(offsetof(AsInstance, customIndexMask) == 48u);
    static_assert(offsetof(AsInstance, sbtOffsetFlags) == 52u);
    static_assert(offsetof(AsInstance, blasAddress) == 56u);
    static_assert(offsetof(RtProbeRay, tMin) == 12u && offsetof(RtProbeRay, direction) == 16u && offsetof(RtProbeRay, tMax) == 28u);
    static_assert(offsetof(RtProbeHit, instance) == 4u && offsetof(RtProbeHit, primitive) == 12u && offsetof(RtProbeHit, u) == 16u &&
                  offsetof(RtProbeHit, flags) == 24u);
    static_assert(offsetof(RtDecodePush, vertexCount) == 16u && offsetof(RtDecodePush, quantOffset) == 20u &&
                  offsetof(RtDecodePush, quantStep) == 32u);
    static_assert(offsetof(RtInstancesPush, count) == 32u && offsetof(RtInstancesPush, blasCount) == 36u &&
                  offsetof(RtInstancesPush, inactiveBlas) == 40u && offsetof(RtInstancesPush, inactiveMask) == 48u);
    static_assert(offsetof(RtProbePush, count) == 24u && offsetof(RtProbePush, cullMask) == 28u && offsetof(RtProbePush, rayFlags) == 32u);
    static_assert(sizeof(RtDecodePush) <= 128u && sizeof(RtInstancesPush) <= 128u && sizeof(RtProbePush) <= 128u,
                  "push constants fit the guaranteed 128 bytes");
    const std::string dir = FUSE_RT_SHADER_DIR;
    const std::string glsl = readFile(dir + "/rt_common.glsl");
    const std::string slang = readFile(dir + "/rt_common.slang");
    expect(!glsl.empty() && !slang.empty(), "rt_common.glsl / .slang readable");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "#define FUSE_RT_INSTANCE_FLAGS %uu", kRtInstanceFlags);
    expect(glsl.find(buf) != std::string::npos, "GLSL instance flags == kRtInstanceFlags");
    std::snprintf(buf, sizeof(buf), "kFuseRtInstanceFlags = %uu", kRtInstanceFlags);
    expect(slang.find(buf) != std::string::npos, "Slang instance flags == kRtInstanceFlags");
    std::snprintf(buf, sizeof(buf), "#define FUSE_RT_WORKGROUP %u", kRtWorkgroupSize);
    expect(glsl.find(buf) != std::string::npos, "GLSL workgroup == kRtWorkgroupSize");
    const struct {
        const char* glslName;
        const char* slangName;
        u32 value;
    } masks[] = {{"FUSE_RT_MASK_VISIBLE", "kFuseRtMaskVisible", kRtMaskVisible},
                 {"FUSE_RT_MASK_SHADOW", "kFuseRtMaskShadow", kRtMaskShadow},
                 {"FUSE_RT_MASK_TRANSPARENT", "kFuseRtMaskTransparent", kRtMaskTransparent},
                 {"FUSE_RT_MASK_DEAD", "kFuseRtMaskDead", kRtMaskDead},
                 {"FUSE_RT_INSTANCE_VALID", "kFuseRtInstanceValid", gpu_scene::kInstanceValid},
                 {"FUSE_RT_INSTANCE_VISIBLE", "kFuseRtInstanceVisible", gpu_scene::kInstanceVisible},
                 {"FUSE_RT_INSTANCE_CAST_SHADOW", "kFuseRtInstanceCastShadow", gpu_scene::kInstanceCastShadow},
                 {"FUSE_RT_INSTANCE_TRANSPARENT", "kFuseRtInstanceTransparent", gpu_scene::kInstanceTransparent},
                 {"FUSE_RT_HIT", "kFuseRtHit", kRtHit},
                 {"FUSE_RT_FRONT_FACE", "kFuseRtFrontFace", kRtFrontFace}};
    for (const auto& m : masks) {
        std::snprintf(buf, sizeof(buf), "#define %s %uu", m.glslName, m.value);
        expect(glsl.find(buf) != std::string::npos, m.glslName);
        std::snprintf(buf, sizeof(buf), "%s = %uu", m.slangName, m.value);
        expect(slang.find(buf) != std::string::npos, m.slangName);
    }
    std::printf("layout: AsInstance 64 B, ray/hit 32 B, push 48 B, shader constants match\n");
    return 0;
}

// --- pack ------------------------------------------------------------------------------------------

int runPack() {
    const u64 table[3] = {0x1000u, 0u, 0x3000u};
    GpuTransform xf{};
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            xf.rows[r][c] = static_cast<f32>(r * 4u + c) * 0.37f - 1.f;
        }
    }
    GpuInstance free{};
    AsInstance a = packRtInstance(free, xf, 7u, table, 3u);
    expect((a.customIndexMask >> 24) == 0u && a.blasAddress == 0u && (a.customIndexMask & 0xFFFFFFu) == 7u, "free slot inactive");
    expect(std::memcmp(a.transform, xf.rows, sizeof(xf.rows)) == 0, "transform copied bit for bit");
    expect((a.sbtOffsetFlags >> 24) == kRtInstanceFlags && (a.sbtOffsetFlags & 0xFFFFFFu) == 0u, "flags byte, SBT offset 0");

    GpuInstance inst{};
    inst.mesh = 0;
    inst.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible | gpu_scene::kInstanceCastShadow;
    a = packRtInstance(inst, xf, 0x123456u, table, 3u);
    expect((a.customIndexMask >> 24) == (kRtMaskVisible | kRtMaskShadow) && a.blasAddress == 0x1000u &&
               (a.customIndexMask & 0xFFFFFFu) == 0x123456u,
           "visible + shadow instance");
    inst.mesh = 1; // no BLAS
    a = packRtInstance(inst, xf, 1u, table, 3u);
    expect((a.customIndexMask >> 24) == 0u && a.blasAddress == 0u, "mesh without BLAS inactive");
    inst.mesh = 5; // out of range
    a = packRtInstance(inst, xf, 1u, table, 3u);
    expect((a.customIndexMask >> 24) == 0u && a.blasAddress == 0u, "mesh out of range inactive");
    inst.mesh = 2;
    inst.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceCastShadow | gpu_scene::kInstanceTransparent;
    a = packRtInstance(inst, xf, 2u, table, 3u);
    expect((a.customIndexMask >> 24) == (kRtMaskShadow | kRtMaskTransparent) && a.blasAddress == 0x3000u, "shadow-only transparent");
    inst.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceTransparent;
    a = packRtInstance(inst, xf, 2u, table, 3u);
    expect((a.customIndexMask >> 24) == 0u && a.blasAddress == 0u, "neither visible nor casting: inactive");
    inst.flags = gpu_scene::kInstanceVisible; // not valid
    a = packRtInstance(inst, xf, 2u, table, 3u);
    expect((a.customIndexMask >> 24) == 0u, "invalid slot inactive");
    a = packRtInstance(inst, xf, 2u, table, 3u, 0x3000u, 0u);
    expect((a.customIndexMask >> 24) == 0u && a.blasAddress == 0u, "no dead mask: spec-inactive (0, 0)");
    a = packRtInstance(inst, xf, 2u, table, 3u, 0x3000u, kRtMaskDead);
    expect((a.customIndexMask >> 24) == kRtMaskDead && a.blasAddress == 0x3000u, "quirk: dead slot -> placeholder, dead mask");
    a = packRtInstance(free, xf, 9u, table, 3u, 0x3000u, kRtMaskDead);
    expect((a.customIndexMask >> 24) == kRtMaskDead && a.blasAddress == 0x3000u, "quirk: free slot -> placeholder");
    a = packRtInstance(free, xf, 9u, table, 3u, 0u, kRtMaskDead);
    expect((a.customIndexMask >> 24) == 0u && a.blasAddress == 0u, "quirk without any BLAS: spec-inactive");
    inst.mesh = 0;
    inst.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible;
    a = packRtInstance(inst, xf, 3u, table, 3u, 0x3000u, kRtMaskDead);
    expect((a.customIndexMask >> 24) == kRtMaskVisible && a.blasAddress == 0x1000u, "live slot unaffected by the quirk");
    expect((kRtMaskAll & kRtMaskDead) == 0u, "kRtMaskAll excludes the dead bit");
    // Decode matches the vis-buffer expression.
    GpuMesh mesh{};
    mesh.quantOffset[0] = -1.5f;
    mesh.quantOffset[1] = 2.f;
    mesh.quantOffset[2] = 0.25f;
    mesh.quantStep[0] = 1.f / 65535.f;
    mesh.quantStep[1] = 3.f / 65535.f;
    mesh.quantStep[2] = 0.5f / 65535.f;
    const fuse::u16 vpos[4] = {65535u, 0u, 32768u, 0u};
    f32 p[3];
    rtDecodePosition(mesh, vpos, 0u, p);
    expect(p[0] == -1.5f + 65535.f * (1.f / 65535.f) && p[1] == 2.f && p[2] == 0.25f + 32768.f * (0.5f / 65535.f), "decode");
    std::printf("pack: masks, inactive slots, custom index, flags and transform checked\n");
    return 0;
}

// --- caps ------------------------------------------------------------------------------------------

RendererCaps capsAt(RenderTier tier) {
    RendererCaps c{};
    c.valid = true;
    c.meetsT0 = true;
    c.tier = tier;
    c.hardwareTier = tier;
    c.bufferDeviceAddress = true;
    c.accelerationStructure = tier >= RenderTier::T2;
    c.rayQuery = tier >= RenderTier::T2;
    c.rayTracingPipeline = tier >= RenderTier::T3;
    return c;
}

int runCaps() {
    RtCapabilities r = evaluateRtCapabilities(capsAt(RenderTier::T2));
    expect(r.usable && std::strcmp(r.fallback, "ray-query") == 0, "T2 usable");
    r = evaluateRtCapabilities(capsAt(RenderTier::T3));
    expect(r.usable && r.rayTracingPipeline, "T3 usable");
    for (RenderTier t : {RenderTier::T0, RenderTier::T1}) {
        r = evaluateRtCapabilities(capsAt(t));
        expect(!r.usable && std::strstr(r.reason, "below T2") != nullptr, "T0 / T1 rejected (tier)");
        expect(std::strcmp(r.fallback, "global-sdf") == 0, "T0 / T1 fallback");
    }
    RendererCaps capped = capsAt(RenderTier::T1);
    capped.hardwareTier = RenderTier::T2;
    capped.tierCap = RenderTier::T1;
    r = evaluateRtCapabilities(capped);
    expect(!r.usable && std::strstr(r.reason, "capped") != nullptr, "T2 hardware capped to T1 rejected");
    RendererCaps noRq = capsAt(RenderTier::T2);
    noRq.rayQuery = false;
    r = evaluateRtCapabilities(noRq);
    expect(!r.usable && std::strstr(r.reason, "ray_query") != nullptr, "no ray query rejected");
    RendererCaps noAs = capsAt(RenderTier::T2);
    noAs.accelerationStructure = false;
    r = evaluateRtCapabilities(noAs);
    expect(!r.usable && std::strstr(r.reason, "acceleration_structure") != nullptr, "no AS rejected");
    RendererCaps noBda = capsAt(RenderTier::T2);
    noBda.bufferDeviceAddress = false;
    r = evaluateRtCapabilities(noBda);
    expect(!r.usable && std::strstr(r.reason, "bufferDeviceAddress") != nullptr, "no BDA rejected");
    RendererCaps below = capsAt(RenderTier::T2);
    below.meetsT0 = false;
    r = evaluateRtCapabilities(below);
    expect(!r.usable, "below-T0 escape rejected");
    r = evaluateRtCapabilities(RendererCaps{});
    expect(!r.usable, "invalid caps rejected");
    r = queryRtCapabilities(nullptr);
    expect(!r.usable, "null device rejected");
    std::printf("caps: T2/T3 usable; T0, T1, capped T2, no AS, no ray query, no BDA, <T0, invalid rejected\n");
    return 0;
}

// --- reference -------------------------------------------------------------------------------------

struct TestMesh {
    std::vector<f32> positions;
    std::vector<u32> indices;
};

TestMesh sphere(u32 rings, u32 segments, f32 radius) {
    TestMesh m;
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
            m.indices.insert(m.indices.end(), {a, b, a + 1u, a + 1u, b, b + 1u});
        }
    }
    return m;
}

TestMesh box() {
    TestMesh m;
    const f32 v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    for (const auto& p : v) {
        m.positions.insert(m.positions.end(), {p[0], p[1], p[2]});
    }
    m.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    return m;
}

GpuTransform place(f32 x, f32 y, f32 z, f32 s, f32 yaw) {
    GpuTransform t{};
    const f32 c = std::cos(yaw), sn = std::sin(yaw);
    t.rows[0][0] = c * s;
    t.rows[0][2] = sn * s;
    t.rows[1][1] = s * 0.8f;
    t.rows[2][0] = -sn * s;
    t.rows[2][2] = c * s;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

/// Brute force: every instance slot, every triangle, same double Moller-Trumbore (independent code).
RtRefHit bruteForce(const std::vector<TestMesh>& meshes, const std::vector<GpuInstance>& inst, const std::vector<GpuTransform>& xf,
                    const RtProbeRay& ray, u32 cullMask) {
    RtRefHit best{};
    for (u32 s = 0; s < inst.size(); ++s) {
        const u32 mask = rtInstanceMask(inst[s].flags, inst[s].mesh < meshes.size() ? 1u : 0u);
        if ((mask & cullMask) == 0u) {
            continue;
        }
        const TestMesh& mesh = meshes[inst[s].mesh];
        // Transform the triangles to world space instead of the ray (independent of the reference).
        for (u32 t = 0; t < mesh.indices.size() / 3u; ++t) {
            f64 w[3][3];
            for (u32 k = 0; k < 3u; ++k) {
                const f32* p = &mesh.positions[mesh.indices[t * 3u + k] * 3u];
                for (u32 r = 0; r < 3u; ++r) {
                    w[k][r] = static_cast<f64>(xf[s].rows[r][0]) * p[0] + static_cast<f64>(xf[s].rows[r][1]) * p[1] +
                              static_cast<f64>(xf[s].rows[r][2]) * p[2] + static_cast<f64>(xf[s].rows[r][3]);
                }
            }
            const f64 e1[3] = {w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2]};
            const f64 e2[3] = {w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2]};
            const f64 d[3] = {ray.direction[0], ray.direction[1], ray.direction[2]};
            const f64 p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const f64 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
            if (std::abs(det) < 1e-300) {
                continue;
            }
            const f64 sv[3] = {ray.origin[0] - w[0][0], ray.origin[1] - w[0][1], ray.origin[2] - w[0][2]};
            const f64 u = (sv[0] * p[0] + sv[1] * p[1] + sv[2] * p[2]) / det;
            const f64 q[3] = {sv[1] * e1[2] - sv[2] * e1[1], sv[2] * e1[0] - sv[0] * e1[2], sv[0] * e1[1] - sv[1] * e1[0]};
            const f64 v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
            if (u < 0.0 || v < 0.0 || u + v > 1.0) {
                continue;
            }
            const f64 th = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
            if (th < ray.tMin || th > ray.tMax || (best.hit && th >= best.t)) {
                continue;
            }
            best.hit = true;
            best.instance = s;
            best.primitive = t;
            best.t = th;
            best.u = u;
            best.v = v;
        }
    }
    return best;
}

struct RefScene {
    std::vector<TestMesh> meshes;
    std::vector<GpuInstance> inst;
    std::vector<GpuTransform> xf;
    RtReferenceScene ref;
};

void makeRefScene(RefScene& s) {
    s.meshes = {sphere(10, 16, 1.f), box(), sphere(6, 8, 0.7f)};
    for (u32 m = 0; m < s.meshes.size(); ++m) {
        s.ref.setMesh(m, s.meshes[m].positions.data(), static_cast<u32>(s.meshes[m].positions.size() / 3u), s.meshes[m].indices.data(),
                      static_cast<u32>(s.meshes[m].indices.size()));
    }
    std::mt19937 rng(6060);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 i = 0; i < 40u; ++i) {
        GpuInstance gi{};
        gi.mesh = i % 3u;
        gi.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible | gpu_scene::kInstanceCastShadow;
        if (i % 7u == 3u) {
            gi.flags = 0; // free slot
        } else if (i % 5u == 2u) {
            gi.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceCastShadow; // shadow only
        }
        s.inst.push_back(gi);
        s.xf.push_back(place(u(rng) * 8.f, u(rng) * 4.f, -10.f + u(rng) * 6.f, 0.5f + 0.5f * (u(rng) * 0.5f + 0.5f), u(rng) * 3.f));
    }
    s.ref.setInstances(s.inst.data(), s.xf.data(), static_cast<u32>(s.inst.size()));
}

std::vector<RtProbeRay> makeRays(u32 count, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    std::vector<RtProbeRay> rays(count);
    for (RtProbeRay& r : rays) {
        r.origin[0] = u(rng) * 2.f;
        r.origin[1] = u(rng) * 2.f;
        r.origin[2] = 4.f + u(rng);
        // Aim at a random point of the instance volume (about half the rays hit).
        f32 d[3] = {u(rng) * 8.f - r.origin[0], u(rng) * 4.f - r.origin[1], -10.f + u(rng) * 6.f - r.origin[2]};
        const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (u32 c = 0; c < 3u; ++c) {
            r.direction[c] = d[c] / len;
        }
        r.tMin = 0.f;
        r.tMax = 100.f;
    }
    return rays;
}

int runReference() {
    RefScene s;
    makeRefScene(s);
    const std::vector<RtProbeRay> rays = makeRays(4096, 17);
    u32 hits = 0, mismatches = 0, robust = 0;
    for (u32 i = 0; i < rays.size(); ++i) {
        const u32 cull = (i % 3u == 0u) ? kRtMaskVisible : kRtMaskAll;
        const RtRefHit a = s.ref.trace(rays[i], cull);
        const RtRefHit b = bruteForce(s.meshes, s.inst, s.xf, rays[i], cull);
        const bool same = a.hit == b.hit && (!a.hit || (a.instance == b.instance && a.primitive == b.primitive &&
                                                         std::abs(a.t - b.t) <= 1e-9 * (1.0 + std::abs(b.t))));
        if (!same) {
            ++mismatches;
            if (mismatches <= 5u) {
                std::fprintf(stderr, "  ray %u: ref (%d %u %u %.9f) brute (%d %u %u %.9f)\n", i, a.hit ? 1 : 0, a.instance, a.primitive, a.t,
                             b.hit ? 1 : 0, b.instance, b.primitive, b.t);
            }
        }
        hits += a.hit ? 1u : 0u;
        robust += s.ref.traceClassified(rays[i], cull, 1e-4).robust ? 1u : 0u;
    }
    std::printf("reference: %zu rays, %u hits, %u mismatches vs brute force, %u robust (eps 1e-4), %u traced instances\n",
                rays.size(), hits, mismatches, robust, s.ref.tracedInstances());
    // Exact ties between the two double computations (different transform order) are possible only on
    // shared edges; allow none outside non-robust rays.
    expect(hits > rays.size() / 4u && hits < rays.size(), "the rays both hit and miss");
    expect(mismatches * 1000u <= rays.size(), "reference == brute force (<= 0.1% edge ties)");
    expect(robust * 100u >= rays.size() * 95u, ">= 95% robust rays");
    return 0;
}

int runRefit() {
    RefScene s;
    makeRefScene(s);
    // Move every third instance, deform mesh 2 (same topology).
    for (u32 i = 0; i < s.xf.size(); i += 3u) {
        s.xf[i].rows[0][3] += 0.37f;
        s.xf[i].rows[1][3] -= 0.21f;
    }
    // Refit the reference in place, then compare with a fresh build of the same scene.
    RtReferenceScene fresh;
    std::vector<f32> deformed = s.meshes[2].positions;
    for (usize v = 0; v < deformed.size(); v += 3u) {
        deformed[v + 1] *= 1.3f;
        deformed[v + 0] += 0.1f * std::sin(deformed[v + 2] * 4.f);
    }
    expect(s.ref.updateMeshPositions(2, deformed.data(), static_cast<u32>(deformed.size() / 3u)), "mesh refit accepted");
    // The deformed mesh's bounds changed: the top level refits (same slots) like a TLAS UPDATE.
    expect(s.ref.updateInstances(s.inst.data(), s.xf.data(), static_cast<u32>(s.inst.size())), "top level refit (no rebuild)");
    s.meshes[2].positions = deformed;
    for (u32 m = 0; m < s.meshes.size(); ++m) {
        fresh.setMesh(m, s.meshes[m].positions.data(), static_cast<u32>(s.meshes[m].positions.size() / 3u), s.meshes[m].indices.data(),
                      static_cast<u32>(s.meshes[m].indices.size()));
    }
    fresh.setInstances(s.inst.data(), s.xf.data(), static_cast<u32>(s.inst.size()));
    const std::vector<RtProbeRay> rays = makeRays(4096, 23);
    u32 diff = 0, bruteDiff = 0;
    for (const RtProbeRay& r : rays) {
        const RtRefHit a = s.ref.trace(r);
        const RtRefHit b = fresh.trace(r);
        diff += (a.hit != b.hit || a.instance != b.instance || a.primitive != b.primitive || a.t != b.t) ? 1u : 0u;
        const RtRefHit c = bruteForce(s.meshes, s.inst, s.xf, r, kRtMaskAll);
        bruteDiff += (a.hit != c.hit || (a.hit && (a.instance != c.instance || a.primitive != c.primitive))) ? 1u : 0u;
    }
    std::printf("refit: refit vs rebuild %u differences, refit vs brute force %u (of %zu rays)\n", diff, bruteDiff, rays.size());
    expect(diff == 0u, "refit mesh BVH == rebuilt mesh BVH");
    expect(bruteDiff * 1000u <= rays.size(), "refit == brute force");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "layout";
    int rc = 0;
    if (suite == "layout") {
        rc = runLayout();
    } else if (suite == "pack") {
        rc = runPack();
    } else if (suite == "caps") {
        rc = runCaps();
    } else if (suite == "reference") {
        rc = runReference();
    } else if (suite == "refit") {
        rc = runRefit();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
