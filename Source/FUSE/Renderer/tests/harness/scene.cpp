#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

namespace fuse::renderer::harness {

using math::Vec3;

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

u32 hashU32(u32 x, u32 seed) {
    u32 h = x * 0x9E3779B1u ^ seed;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

f32 hash01(u32 x, u32 seed) {
    return static_cast<f32>(hashU32(x, seed) >> 8) * (1.f / 16777216.f);
}

u32 floatBits(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, 4);
    return b;
}

Vec3 rotateY(const Vec3& v, f32 yaw) {
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}

/// Merges triangles into batches keyed by (material, exact normal bits) in first-use order.
class Builder {
public:
    explicit Builder(Scene& scene) : m_scene(scene) {}

    u32 material(const SurfaceMaterial& m) {
        m_scene.materials.push_back(m);
        return static_cast<u32>(m_scene.materials.size() - 1u);
    }

    void tri(u32 mat, const Vec3& n, const Vec3& a, const Vec3& b, const Vec3& c) {
        Batch& batch = batchFor(mat, n);
        batch.positions.push_back(a);
        batch.positions.push_back(b);
        batch.positions.push_back(c);
    }

    void quad(u32 mat, const Vec3& n, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
        tri(mat, n, a, b, c);
        tri(mat, n, a, c, d);
    }

    /// Box rotated by `yaw` around +Y through `center`. `skipBottom` drops the -Y face.
    void box(u32 mat, const Vec3& center, const Vec3& half, f32 yaw, bool skipBottom) {
        auto corner = [&](f32 sx, f32 sy, f32 sz) {
            return center + rotateY(Vec3(sx * half.x, sy * half.y, sz * half.z), yaw);
        };
        auto normal = [&](const Vec3& n) { return yaw == 0.f ? n : rotateY(n, yaw); };
        const Vec3 p000 = corner(-1, -1, -1), p100 = corner(1, -1, -1), p010 = corner(-1, 1, -1),
                   p110 = corner(1, 1, -1), p001 = corner(-1, -1, 1), p101 = corner(1, -1, 1),
                   p011 = corner(-1, 1, 1), p111 = corner(1, 1, 1);
        quad(mat, normal({0, 1, 0}), p010, p011, p111, p110);
        if (!skipBottom) {
            quad(mat, normal({0, -1, 0}), p000, p100, p101, p001);
        }
        quad(mat, normal({1, 0, 0}), p100, p110, p111, p101);
        quad(mat, normal({-1, 0, 0}), p000, p001, p011, p010);
        quad(mat, normal({0, 0, 1}), p001, p101, p111, p011);
        quad(mat, normal({0, 0, -1}), p000, p010, p110, p100);
    }

    Batch& screenRect(u32 mat, f32 x0, f32 y0, f32 x1, f32 y1, f32 depth) {
        m_scene.batches.emplace_back();
        Batch& b = m_scene.batches.back();
        b.material = mat;
        b.normal = Vec3(0.f, 0.f, -1.f);
        b.screenSpace = true;
        b.screenDepth = depth;
        b.positions = {{x0, y0, depth}, {x1, y0, depth}, {x1, y1, depth},
                       {x0, y0, depth}, {x1, y1, depth}, {x0, y1, depth}};
        return b;
    }

private:
    Batch& batchFor(u32 mat, const Vec3& n) {
        const auto key = std::make_tuple(mat, floatBits(n.x), floatBits(n.y), floatBits(n.z));
        const auto it = m_index.find(key);
        if (it != m_index.end()) {
            return m_scene.batches[it->second];
        }
        m_index.emplace(key, m_scene.batches.size());
        m_scene.batches.emplace_back();
        Batch& b = m_scene.batches.back();
        b.material = mat;
        b.normal = n;
        return b;
    }

    Scene& m_scene;
    std::map<std::tuple<u32, u32, u32, u32>, usize> m_index;
};

SurfaceMaterial matte(f32 r, f32 g, f32 b, f32 roughness = 0.8f) {
    SurfaceMaterial m;
    m.albedo = Vec3(r, g, b);
    m.roughness = roughness;
    return m;
}

SurfaceMaterial emitter(f32 r, f32 g, f32 b, u32 shadingModel = 1u) {
    SurfaceMaterial m;
    m.albedo = Vec3(0.f, 0.f, 0.f);
    m.emissive = Vec3(r, g, b);
    m.shadingModel = shadingModel;
    return m;
}

void fnv(u64& h, const void* data, usize size) {
    const auto* p = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
}

void fnvVec(u64& h, const Vec3& v) {
    const u32 bits[3] = {floatBits(v.x), floatBits(v.y), floatBits(v.z)};
    fnv(h, bits, sizeof(bits));
}

} // namespace

u64 Scene::triangleCount() const {
    u64 n = 0;
    for (const Batch& b : batches) {
        n += b.positions.size() / 3u;
    }
    return n;
}

u64 Scene::contentHash() const {
    u64 h = 0xCBF29CE484222325ull;
    fnv(h, name.data(), name.size());
    fnvVec(h, camera.eye);
    fnvVec(h, camera.target);
    for (const SurfaceMaterial& m : materials) {
        fnvVec(h, m.albedo);
        fnvVec(h, m.emissive);
        const u32 bits[4] = {floatBits(m.roughness), floatBits(m.metallic), floatBits(m.ao), m.shadingModel};
        fnv(h, bits, sizeof(bits));
    }
    for (const Batch& b : batches) {
        const u32 head[3] = {b.material, b.screenSpace ? 1u : 0u, floatBits(b.screenDepth)};
        fnv(h, head, sizeof(head));
        fnvVec(h, b.normal);
        for (const Vec3& p : b.positions) {
            fnvVec(h, p);
        }
    }
    return h;
}

Scene buildGBufferQuads() {
    Scene s;
    s.name = "gbuffer_quads";
    Builder b(s);
    SurfaceMaterial far = matte(0.2f, 0.4f, 0.6f, 0.3f);
    far.ao = 0.8f;
    SurfaceMaterial near = matte(0.9f, 0.1f, 0.3f, 0.7f);
    near.metallic = 1.f;
    near.ao = 0.5f;
    near.emissive = Vec3(0.2f, 0.1f, 0.05f);
    near.shadingModel = 3u;
    const u32 farMat = b.material(far);
    const u32 nearMat = b.material(near);
    // Near first, so the far quad must fail the depth test on the overlap.
    b.screenRect(nearMat, -0.5f, -0.5f, 1.f, 1.f, 0.25f).normal = Vec3(0.3f, -0.5f, 0.8f).normalized();
    b.screenRect(farMat, -1.f, -1.f, 0.5f, 0.5f, 0.75f).normal = Vec3(0.f, 1.f, 0.f);
    return s;
}

Scene buildCornellBox() {
    Scene s;
    s.name = "cornell_box";
    s.camera.eye = Vec3(0.f, 1.f, 3.4f);
    s.camera.target = Vec3(0.f, 1.f, 0.f);
    s.camera.fovYDegrees = 40.f;
    Builder b(s);
    const u32 white = b.material(matte(0.73f, 0.73f, 0.73f));
    const u32 red = b.material(matte(0.65f, 0.05f, 0.05f));
    const u32 green = b.material(matte(0.12f, 0.45f, 0.15f));
    const u32 light = b.material(emitter(8.f, 7.2f, 5.6f));
    SurfaceMaterial tallMat = matte(0.55f, 0.55f, 0.6f, 0.4f);
    tallMat.ao = 0.9f;
    const u32 tall = b.material(tallMat);
    SurfaceMaterial shortMat = matte(0.8f, 0.76f, 0.62f, 0.6f);
    shortMat.ao = 0.9f;
    const u32 shortBlock = b.material(shortMat);

    const Vec3 a(-1.f, 0.f, -1.f), c(1.f, 2.f, 1.f);
    b.quad(white, {0, 1, 0}, {a.x, 0, a.z}, {c.x, 0, a.z}, {c.x, 0, c.z}, {a.x, 0, c.z});    // floor
    b.quad(white, {0, -1, 0}, {a.x, 2, a.z}, {a.x, 2, c.z}, {c.x, 2, c.z}, {c.x, 2, a.z});   // ceiling
    b.quad(white, {0, 0, 1}, {a.x, 0, a.z}, {a.x, 2, a.z}, {c.x, 2, a.z}, {c.x, 0, a.z});    // back
    b.quad(red, {1, 0, 0}, {a.x, 0, a.z}, {a.x, 0, c.z}, {a.x, 2, c.z}, {a.x, 2, a.z});      // left
    b.quad(green, {-1, 0, 0}, {c.x, 0, a.z}, {c.x, 2, a.z}, {c.x, 2, c.z}, {c.x, 0, c.z});   // right
    b.quad(light, {0, -1, 0}, {-0.25f, 1.98f, -0.25f}, {-0.25f, 1.98f, 0.25f}, {0.25f, 1.98f, 0.25f},
           {0.25f, 1.98f, -0.25f});
    b.box(tall, {-0.35f, 0.6f, -0.3f}, {0.3f, 0.6f, 0.3f}, 0.4f, true);
    b.box(shortBlock, {0.4f, 0.3f, 0.35f}, {0.3f, 0.3f, 0.3f}, -0.3f, true);
    s.markers.push_back({"light_center", {0.f, 1.98f, 0.f}});
    return s;
}

Scene buildSphereField(u32 side, u32 stacks, u32 slices) {
    Scene s;
    s.name = "sphere_field";
    s.camera.eye = Vec3(0.f, 3.2f, 5.2f);
    s.camera.target = Vec3(0.f, 0.3f, 0.f);
    s.camera.fovYDegrees = 45.f;
    side = std::max(side, 1u);
    stacks = std::max(stacks, 2u);
    slices = std::max(slices, 3u);
    Builder b(s);
    const u32 ground = b.material(matte(0.45f, 0.45f, 0.42f));
    b.quad(ground, {0, 1, 0}, {-3.5f, 0, -3.5f}, {3.5f, 0, -3.5f}, {3.5f, 0, 3.5f}, {-3.5f, 0, 3.5f});

    constexpr f32 kRadius = 0.45f;
    constexpr f32 kSpacing = 1.2f;
    const f32 origin = -0.5f * kSpacing * static_cast<f32>(side - 1u);
    for (u32 j = 0; j < side; ++j) {
        for (u32 i = 0; i < side; ++i) {
            const u32 id = j * side + i;
            SurfaceMaterial m;
            m.albedo = Vec3(0.2f + 0.7f * hash01(id, 1u), 0.2f + 0.7f * hash01(id, 2u), 0.2f + 0.7f * hash01(id, 3u));
            m.roughness = side > 1u ? static_cast<f32>(i) / static_cast<f32>(side - 1u) : 0.5f;
            m.metallic = side > 1u ? static_cast<f32>(j) / static_cast<f32>(side - 1u) : 0.f;
            const u32 mat = b.material(m);
            const Vec3 center(origin + kSpacing * static_cast<f32>(i), kRadius, origin + kSpacing * static_cast<f32>(j));
            auto point = [&](u32 st, u32 sl) {
                const f32 theta = kPi * static_cast<f32>(st) / static_cast<f32>(stacks);
                const f32 phi = 2.f * kPi * static_cast<f32>(sl % slices) / static_cast<f32>(slices);
                return Vec3(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            };
            for (u32 st = 0; st < stacks; ++st) {
                for (u32 sl = 0; sl < slices; ++sl) {
                    const Vec3 d00 = point(st, sl), d01 = point(st, sl + 1u), d10 = point(st + 1u, sl),
                               d11 = point(st + 1u, sl + 1u);
                    const Vec3 n = (d00 + d01 + d10 + d11).normalized();
                    auto P = [&](const Vec3& d) { return center + d * kRadius; };
                    if (st == 0u) {
                        b.tri(mat, n, P(d00), P(d11), P(d10));
                    } else if (st + 1u == stacks) {
                        b.tri(mat, n, P(d00), P(d01), P(d10));
                    } else {
                        b.quad(mat, n, P(d00), P(d01), P(d11), P(d10));
                    }
                }
            }
        }
    }
    s.instances = side * side;
    return s;
}

Scene buildThinWall(f32 wallThickness) {
    Scene s;
    s.name = "thin_wall";
    // Off-axis so the wall's dark (+x) face is visible, not edge-on.
    s.camera.eye = Vec3(1.8f, 3.2f, 3.4f);
    s.camera.target = Vec3(0.f, 0.4f, 0.f);
    s.camera.fovYDegrees = 50.f;
    Builder b(s);
    const u32 floorMat = b.material(matte(0.6f, 0.6f, 0.6f));
    const u32 wallMat = b.material(matte(0.8f, 0.78f, 0.74f));
    const u32 thinMat = b.material(matte(0.85f, 0.75f, 0.55f));
    const u32 panel = b.material(emitter(10.f, 9.f, 7.f));
    constexpr f32 kH = 1.2f;
    b.quad(floorMat, {0, 1, 0}, {-2, 0, -1}, {2, 0, -1}, {2, 0, 1}, {-2, 0, 1});
    b.quad(wallMat, {0, 0, 1}, {-2, 0, -1}, {-2, kH, -1}, {2, kH, -1}, {2, 0, -1});  // back
    b.quad(wallMat, {1, 0, 0}, {-2, 0, -1}, {-2, 0, 1}, {-2, kH, 1}, {-2, kH, -1});  // left
    b.quad(wallMat, {-1, 0, 0}, {2, 0, -1}, {2, kH, -1}, {2, kH, 1}, {2, 0, 1});     // right
    const f32 t = std::max(wallThickness, 1e-4f);
    b.box(thinMat, {0.f, 0.5f * kH, 0.f}, {0.5f * t, 0.5f * kH, 1.f}, 0.f, true);
    b.quad(panel, {0, 0, 1}, {-1.6f, 0.3f, -0.99f}, {-1.6f, 0.9f, -0.99f}, {-0.4f, 0.9f, -0.99f},
           {-0.4f, 0.3f, -0.99f});
    s.markers.push_back({"lit_side", {-0.5f * t - 0.25f, 0.5f, 0.f}});
    s.markers.push_back({"dark_side", {0.5f * t + 0.25f, 0.5f, 0.f}});
    return s;
}

Scene buildInstanceGrid(u32 count) {
    Scene s;
    count = std::max(count, 1u);
    s.name = "instance_grid_" + std::to_string(count);
    s.camera.eye = Vec3(0.f, 6.f, 6.5f);
    s.camera.target = Vec3(0.f, 0.f, 0.f);
    s.camera.fovYDegrees = 50.f;
    s.instances = count;
    Builder b(s);
    const u32 ground = b.material(matte(0.35f, 0.36f, 0.4f));
    b.quad(ground, {0, 1, 0}, {-4.2f, 0, -4.2f}, {4.2f, 0, -4.2f}, {4.2f, 0, 4.2f}, {-4.2f, 0, 4.2f});
    const u32 mats[4] = {b.material(matte(0.8f, 0.3f, 0.2f, 0.4f)), b.material(matte(0.2f, 0.6f, 0.8f, 0.6f)),
                         b.material(matte(0.9f, 0.8f, 0.3f, 0.3f)), b.material(matte(0.5f, 0.8f, 0.4f, 0.7f))};
    u32 side = 1;
    while (side * side < count) {
        ++side;
    }
    const f32 cell = 8.f / static_cast<f32>(side);
    for (u32 i = 0; i < count; ++i) {
        const u32 gx = i % side;
        const u32 gz = i / side;
        const f32 half = cell * 0.35f;
        const f32 height = cell * (0.4f + 1.2f * hash01(i, 11u));
        const Vec3 center(-4.f + cell * (static_cast<f32>(gx) + 0.5f), 0.5f * height,
                          -4.f + cell * (static_cast<f32>(gz) + 0.5f));
        b.box(mats[hashU32(i, 7u) & 3u], center, {half, 0.5f * height, half}, 0.f, true);
    }
    return s;
}

void addHudOverlay(Scene& scene) {
    Builder b(scene);
    const u32 bar = b.material(emitter(0.05f, 0.07f, 0.1f, kHudShadingModel));
    const u32 glyph = b.material(emitter(0.9f, 0.9f, 0.85f, kHudShadingModel));
    const u32 healthBack = b.material(emitter(0.25f, 0.02f, 0.02f, kHudShadingModel));
    const u32 healthFill = b.material(emitter(0.95f, 0.1f, 0.1f, kHudShadingModel));
    const u32 cross = b.material(emitter(0.2f, 1.f, 0.3f, kHudShadingModel));
    const u32 frame = b.material(emitter(1.f, 0.8f, 0.2f, kHudShadingModel));
    // Layering inside the HUD uses depth (LESS test): smaller = in front.
    b.screenRect(bar, -1.f, -1.f, 1.f, -0.86f, 0.002f);
    for (u32 k = 0; k < 6u; ++k) {
        const f32 x = -0.95f + 0.09f * static_cast<f32>(k);
        const f32 h = 0.04f + 0.03f * hash01(k, 21u);
        b.screenRect(glyph, x, -0.96f, x + 0.06f, -0.96f + h, 0.001f);
    }
    b.screenRect(healthBack, -0.9f, 0.82f, -0.3f, 0.9f, 0.002f);
    b.screenRect(healthFill, -0.9f, 0.82f, -0.48f, 0.9f, 0.001f);
    b.screenRect(cross, -0.1f, -0.01f, 0.1f, 0.01f, 0.001f);
    b.screenRect(cross, -0.01f, -0.1f, 0.01f, 0.1f, 0.001f);
    constexpr f32 x0 = 0.55f, y0 = 0.5f, x1 = 0.95f, y1 = 0.95f, t = 0.03f;
    b.screenRect(frame, x0, y0, x1, y0 + t, 0.001f);
    b.screenRect(frame, x0, y1 - t, x1, y1, 0.001f);
    b.screenRect(frame, x0, y0, x0 + t, y1, 0.001f);
    b.screenRect(frame, x1 - t, y0, x1, y1, 0.001f);
}

Scene buildHudOverlay() {
    Scene s = buildCornellBox();
    s.name = "hud_overlay";
    addHudOverlay(s);
    return s;
}

bool buildSceneByName(const std::string& name, Scene& out) {
    if (name == "gbuffer_quads") {
        out = buildGBufferQuads();
    } else if (name == "cornell_box") {
        out = buildCornellBox();
    } else if (name == "sphere_field") {
        out = buildSphereField();
    } else if (name == "thin_wall") {
        out = buildThinWall();
    } else if (name == "hud_overlay") {
        out = buildHudOverlay();
    } else if (name.rfind("instance_grid_", 0) == 0) {
        std::string count = name.substr(14);
        u32 scale = 1;
        if (!count.empty() && (count.back() == 'k' || count.back() == 'K')) {
            scale = 1000;
            count.pop_back();
        }
        if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos || count.size() > 7u) {
            return false;
        }
        const u32 n = static_cast<u32>(std::strtoul(count.c_str(), nullptr, 10)) * scale;
        if (n == 0u || n > 1000000u) {
            return false;
        }
        out = buildInstanceGrid(n);
        out.name = name;
    } else {
        return false;
    }
    return true;
}

std::vector<std::string> sceneNames() {
    return {"gbuffer_quads",     "cornell_box",      "sphere_field",       "thin_wall",
            "instance_grid_1k",  "instance_grid_10k", "instance_grid_100k", "hud_overlay"};
}

ProjectedScene projectScene(const Scene& scene, f32 aspect) {
    ProjectedScene out;
    const Camera& cam = scene.camera;
    const Vec3 f = (cam.target - cam.eye).normalized();
    const Vec3 r = math::cross(f, cam.up).normalized();
    const Vec3 u = math::cross(r, f);
    const f32 fy = 1.f / std::tan(0.5f * cam.fovYDegrees * kPi / 180.f);
    const f32 fx = fy / aspect;
    const f32 n = cam.nearZ;
    const f32 fz = cam.farZ;
    usize total = 0;
    for (const Batch& b : scene.batches) {
        total += b.positions.size();
    }
    out.vertices.reserve(total * 3u);

    auto emit = [&](const Vec3& v) { // v = (xv, yv, depth) with depth >= near
        out.vertices.push_back(fx * v.x / v.z);
        out.vertices.push_back(-fy * v.y / v.z);
        out.vertices.push_back(fz * (v.z - n) / ((fz - n) * v.z));
    };

    for (const Batch& b : scene.batches) {
        out.firstVertex.push_back(static_cast<u32>(out.vertices.size() / 3u));
        const usize before = out.vertices.size();
        if (b.screenSpace) {
            for (const Vec3& p : b.positions) {
                out.vertices.push_back(p.x);
                out.vertices.push_back(p.y);
                out.vertices.push_back(b.screenDepth);
            }
        } else {
            for (usize t = 0; t + 2u < b.positions.size(); t += 3u) {
                Vec3 in[3];
                bool allInside = true;
                for (int k = 0; k < 3; ++k) {
                    const Vec3 d = b.positions[t + static_cast<usize>(k)] - cam.eye;
                    in[k] = Vec3(d.dot(r), d.dot(u), d.dot(f));
                    allInside = allInside && in[k].z >= n;
                }
                if (allInside) {
                    emit(in[0]);
                    emit(in[1]);
                    emit(in[2]);
                    continue;
                }
                // Sutherland-Hodgman against depth >= near; fan-triangulate the result.
                Vec3 poly[4];
                int count = 0;
                for (int k = 0; k < 3; ++k) {
                    const Vec3& a = in[k];
                    const Vec3& c = in[(k + 1) % 3];
                    const bool aIn = a.z >= n;
                    const bool cIn = c.z >= n;
                    if (aIn) {
                        poly[count++] = a;
                    }
                    if (aIn != cIn) {
                        const f32 s = (n - a.z) / (c.z - a.z);
                        poly[count++] = Vec3(a.x + (c.x - a.x) * s, a.y + (c.y - a.y) * s, n);
                    }
                }
                for (int k = 1; k + 1 < count; ++k) {
                    emit(poly[0]);
                    emit(poly[k]);
                    emit(poly[k + 1]);
                }
            }
        }
        out.vertexCount.push_back(static_cast<u32>((out.vertices.size() - before) / 3u));
    }
    return out;
}

} // namespace fuse::renderer::harness
