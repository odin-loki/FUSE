// FUSE Relight RL-5.1 tests: procedural scenes and statistics shared by the CPU and Vulkan gates.
#pragma once

#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace pt_test {

namespace pt = fuse::relight::render::pathtrace;
namespace bk = fuse::relight::bsdf;
namespace lk = fuse::relight::lightk;
namespace rl = fuse::relight::render::lights;
using fuse::u32;

constexpr float kPi = 3.14159265358979f;

/// Quad centred at c with half axes u, v (normal = u x v), two triangles.
inline pt::PtMesh quad(const float c[3], const float u[3], const float v[3], u32 material = 0) {
    pt::PtMesh m;
    const float s[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
    const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (const auto& k : s) {
        for (int a = 0; a < 3; ++a) {
            m.positions.push_back(c[a] + k[0] * u[a] + k[1] * v[a]);
        }
        for (int a = 0; a < 3; ++a) {
            m.normals.push_back(n[a] / nl);
        }
        m.uvs.push_back(0.5f + 0.5f * k[0]);
        m.uvs.push_back(0.5f - 0.5f * k[1]);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    m.material = material;
    return m;
}

inline pt::PtMesh sphere(float radius, u32 rings, u32 segments, u32 material = 0) {
    pt::PtMesh m;
    for (u32 r = 0; r <= rings; ++r) {
        const float th = kPi * float(r) / float(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const float ph = 2.f * kPi * float(s) / float(segments);
            const float n[3] = {std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            for (int a = 0; a < 3; ++a) {
                m.positions.push_back(radius * n[a]);
                m.normals.push_back(n[a]);
            }
            m.uvs.push_back(float(s) / float(segments));
            m.uvs.push_back(float(r) / float(rings));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1u) + s;
            const u32 b = a + segments + 1u;
            m.indices.insert(m.indices.end(), {a, a + 1u, b, a + 1u, b + 1u, b}); // outward winding
        }
    }
    m.material = material;
    return m;
}

inline pt::Mat34 translate(float x, float y, float z) {
    pt::Mat34 m = pt::identity34();
    m[3] = x;
    m[7] = y;
    m[11] = z;
    return m;
}

inline pt::PtMaterial lambert(float r, float g, float b, float roughness = 0.8f) {
    pt::PtMaterial m;
    m.bsdf.albedo = bk::float3(r, g, b);
    m.bsdf.roughness = roughness;
    m.bsdf.metallic = 0.f;
    return m;
}

inline pt::PtMaterial metal(float r, float g, float b, float roughness) {
    pt::PtMaterial m;
    m.bsdf.albedo = bk::float3(r, g, b);
    m.bsdf.roughness = roughness;
    m.bsdf.metallic = 1.f;
    return m;
}

inline pt::PtMaterial unlit(float r, float g, float b) {
    pt::PtMaterial m;
    m.bsdf.albedo = bk::float3(r, g, b);
    m.flags = pt::kPtMatUnlit;
    return m;
}

inline pt::PtMaterial emitter(float r, float g, float b) {
    pt::PtMaterial m = lambert(0.f, 0.f, 0.f);
    m.bsdf.emission = bk::float3(r, g, b);
    return m;
}

inline pt::PtCamera camera(float ox, float oy, float oz, float fx, float fy, float fz, float ux, float uy, float uz,
                           float fovDeg) {
    pt::PtCamera c;
    c.origin[0] = ox;
    c.origin[1] = oy;
    c.origin[2] = oz;
    c.forward[0] = fx;
    c.forward[1] = fy;
    c.forward[2] = fz;
    c.up[0] = ux;
    c.up[1] = uy;
    c.up[2] = uz;
    c.fovY = fovDeg * kPi / 180.f;
    return c;
}

/// A Cornell-style box (5 x 5 x 5, open towards +z where the camera sits) with an emissive ceiling panel (emissive
/// triangles), a sphere light, a rough metal sphere, a mirror panel (PSR), a glass sphere and a vertex-alpha-tested
/// panel: the synthetic parity scene of the GPU gates.
inline pt::PtScene cornell(bool glass = true) {
    pt::PtScene s;
    s.materials = {lambert(0.75f, 0.75f, 0.75f),                                        // 0 white
                   lambert(0.8f, 0.15f, 0.12f),                                         // 1 red
                   lambert(0.15f, 0.7f, 0.2f),                                          // 2 green
                   emitter(6.f, 5.5f, 4.5f),                                            // 3 ceiling panel
                   metal(0.95f, 0.8f, 0.55f, 0.35f),                                    // 4 rough gold
                   metal(0.92f, 0.92f, 0.95f, 0.02f)};                                  // 5 mirror
    pt::PtMaterial g;
    g.bsdf.model = bk::kBsdfModelTranslucent;
    g.bsdf.ior = 1.5f;
    g.bsdf.transmittance = bk::float3(0.9f, 0.95f, 1.0f);
    g.bsdf.mediumDistance = 1.f;
    s.materials.push_back(g);                                                           // 6 glass
    pt::PtMaterial cut = lambert(0.3f, 0.3f, 0.9f);
    cut.flags = pt::kPtMatVertexColor | pt::kPtMatAlphaTest;
    cut.alphaReference = 0.5f;
    cut.alphaCompare = 4u; // GREATER
    s.materials.push_back(cut);                                                         // 7 alpha-tested panel
    const float h = 2.5f;
    auto add = [&](pt::PtMesh m, const pt::Mat34& xf) {
        pt::PtInstance inst;
        inst.mesh = static_cast<u32>(s.meshes.size());
        inst.objectToWorld = xf;
        s.meshes.push_back(std::move(m));
        s.instances.push_back(inst);
    };
    {
        const float c[3] = {0.f, -h, 0.f}, u[3] = {0.f, 0.f, h}, v[3] = {h, 0.f, 0.f};
        add(quad(c, u, v, 0), pt::identity34()); // floor (normal +y)
    }
    {
        const float c[3] = {0.f, h, 0.f}, u[3] = {h, 0.f, 0.f}, v[3] = {0.f, 0.f, h};
        add(quad(c, u, v, 0), pt::identity34()); // ceiling (normal -y)
    }
    {
        const float c[3] = {0.f, 0.f, -h}, u[3] = {h, 0.f, 0.f}, v[3] = {0.f, h, 0.f};
        add(quad(c, u, v, 0), pt::identity34()); // back wall (normal +z)
    }
    {
        const float c[3] = {-h, 0.f, 0.f}, u[3] = {0.f, h, 0.f}, v[3] = {0.f, 0.f, h};
        add(quad(c, u, v, 1), pt::identity34()); // left wall red (normal +x)
    }
    {
        const float c[3] = {h, 0.f, 0.f}, u[3] = {0.f, 0.f, h}, v[3] = {0.f, h, 0.f};
        add(quad(c, u, v, 2), pt::identity34()); // right wall green (normal -x)
    }
    {
        const float c[3] = {0.f, h - 0.01f, 0.f}, u[3] = {0.7f, 0.f, 0.f}, v[3] = {0.f, 0.f, 0.7f};
        add(quad(c, u, v, 3), pt::identity34()); // emissive panel (normal -y)
    }
    add(sphere(0.8f, 12, 18, 4), translate(-1.1f, -h + 0.8f, -0.8f));
    {
        const float c[3] = {1.2f, -0.6f, -h + 0.05f}, u[3] = {0.9f, 0.f, 0.f}, v[3] = {0.f, 1.2f, 0.f};
        add(quad(c, u, v, 5), pt::identity34()); // mirror on the back wall
    }
    if (glass) {
        add(sphere(0.6f, 10, 14, 6), translate(1.0f, -h + 0.6f, 0.9f));
    }
    {
        const float c[3] = {-0.4f, 0.6f, 0.2f}, u[3] = {0.5f, 0.f, 0.f}, v[3] = {0.f, 0.5f, 0.f};
        pt::PtMesh m = quad(c, u, v, 7);
        m.colors = {1.f, 1.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        add(std::move(m), pt::identity34()); // alpha ramp: left half cut
    }
    s.lights.push_back(rl::makeSphereLight(lk::float3(0.9f, 1.4f, 0.6f), 0.25f, lk::float3(12.f, 10.f, 8.f)));
    s.camera = camera(0.f, 0.f, 9.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 38.f);
    return s;
}

/// Per-block comparison of two estimates of the same image: block means of a and b differ by at most k sigma
/// (standard errors of the two block means combined). Returns the worst |z| and counts failing blocks.
struct BlockResult {
    double worstZ = 0.0;
    u32 blocks = 0;
    u32 failing = 0;
    double meanA = 0.0;
    double meanB = 0.0;
};

/// `meanA/varA` per pixel and channel: mean radiance and per-sample variance of estimator A with `nA` samples.
inline BlockResult compareBlocks(u32 w, u32 h, u32 block, const std::vector<double>& meanA,
                                 const std::vector<double>& varA, double nA, const std::vector<double>& meanB,
                                 const std::vector<double>& varB, double nB, double k, double absTolerance) {
    BlockResult r;
    double sumA = 0.0, sumB = 0.0;
    for (u32 by = 0; by < h; by += block) {
        for (u32 bx = 0; bx < w; bx += block) {
            for (u32 c = 0; c < 3u; ++c) {
                double ma = 0.0, mb = 0.0, va = 0.0, vb = 0.0;
                u32 n = 0;
                for (u32 y = by; y < std::min(h, by + block); ++y) {
                    for (u32 x = bx; x < std::min(w, bx + block); ++x) {
                        const std::size_t i = (std::size_t(y) * w + x) * 3u + c;
                        ma += meanA[i];
                        mb += meanB[i];
                        va += varA[i] / nA;
                        vb += varB[i] / nB;
                        ++n;
                    }
                }
                ma /= n;
                mb /= n;
                const double se = std::sqrt((va + vb) / (double(n) * n));
                const double z = std::fabs(ma - mb) / std::max(se, 1e-12);
                const bool ok = std::fabs(ma - mb) <= k * se + absTolerance;
                r.worstZ = std::max(r.worstZ, z);
                ++r.blocks;
                if (!ok) {
                    ++r.failing;
                }
                sumA += ma;
                sumB += mb;
            }
        }
    }
    r.meanA = sumA / std::max(r.blocks, 1u);
    r.meanB = sumB / std::max(r.blocks, 1u);
    return r;
}

inline void referenceStats(const pt::PtReferenceImage& img, std::vector<double>& mean, std::vector<double>& var) {
    const u32 w = img.width(), h = img.height();
    mean.assign(std::size_t(w) * h * 3u, 0.0);
    var.assign(std::size_t(w) * h * 3u, 0.0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            for (u32 c = 0; c < 3u; ++c) {
                const std::size_t i = (std::size_t(y) * w + x) * 3u + c;
                mean[i] = img.mean(x, y, c);
                var[i] = img.variance(x, y, c);
            }
        }
    }
}

} // namespace pt_test
