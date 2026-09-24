#pragma once
// WP-6.3 gates: analytic scenes shared by test_rp_ssfx_gpu_cpu.cpp (CPU) and test_rp_ssfx_gpu.cpp (Lavapipe).
//
//   Wedge (GTAO analytic reference): two half-planes meeting at a crease with opening angle alpha (through
//     the air); the camera sits inside the wedge on its bisector. A point on either face sees the other face
//     over a cross-section angle range, and the cosine-weighted visibility with an infinite radius is
//     (1 - cos alpha) / 2 for every point (the 2D cross-section of the cosine lobe: the fraction with
//     projected angle in [psi0, psi1] is (sin psi1 - sin psi0) / 2). alpha = 180 is a plane (visibility 1).
//   Room (parity scenes): floor, back wall, a red side wall, two boxes, open sky; simple Lambert + ambient lit
//     image; materials with different roughness / metallic.
// Both are ray cast per pixel centre (no raster): the G-buffer texels are exact functions of the geometry.
#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ssfx_test {

using fuse::f32;
using fuse::u16;
using fuse::u32;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;

constexpr f32 kPi = 3.14159265358979f;

// --- engine camera ------------------------------------------------------------------------------------
struct Camera {
    u32 width = 0;
    u32 height = 0;
    f32 fovYDeg = 60.f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 200.f;
    Vec3 eye{};
    Vec3 target{};
    Vec3 up{0.f, 1.f, 0.f};
    fuse::math::Mat4 view{};
    fuse::math::Mat4 proj{};

    void build() {
        view = fuse::math::lookAt(eye, target, up);
        proj = fuse::math::perspective(fovYDeg, static_cast<f32>(width) / static_cast<f32>(height), nearPlane, farPlane);
    }
    /// World-space ray through the centre of pixel (x, y) (row 0 = top); `viewDir` = engine view space.
    Vec3 rayWorld(u32 x, u32 y, Vec3& viewDir) const {
        const f32 ndcX = (2.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width)) - 1.f;
        const f32 ndcY = 1.f - (2.f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height));
        viewDir = Vec3{ndcX / proj.data[0], ndcY / proj.data[5], -1.f};
        // view^-1 rotation = transpose of the upper 3x3 (column-major data[col * 4 + row]).
        const f32* m = view.data.data();
        return Vec3{m[0] * viewDir.x + m[1] * viewDir.y + m[2] * viewDir.z,
                    m[4] * viewDir.x + m[5] * viewDir.y + m[6] * viewDir.z,
                    m[8] * viewDir.x + m[9] * viewDir.y + m[10] * viewDir.z};
    }
    /// Forward z/w device depth of an engine view-space depth (z_view < 0).
    f32 deviceDepth(f32 zView) const {
        const f32 clipZ = proj.data[10] * zView + proj.data[14];
        const f32 clipW = -zView;
        return clipZ / clipW;
    }
};

// --- G-buffer (CPU texel values exactly as the sampled images return them) ---------------------------
struct GBuffer {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> rt4;     ///< R32F device depth (1 = sky)
    std::vector<Vec4> rt0;    ///< RGBA16F: signed oct normal (half-exact), 0, material AO (half)
    std::vector<Vec4> rt1;    ///< RGBA8: albedo, 1
    std::vector<Vec4> rt2;    ///< RGBA8: roughness, metallic, 0, 0
    std::vector<Vec4> lit;    ///< RGBA16F lit radiance, alpha 1 (geometry) / 0 (sky)
    std::vector<f32> viewZ;   ///< positive linear view depth (analytic; 0 = sky)
    std::vector<Vec3> worldN; ///< analytic world normal
    void resize(u32 w, u32 h) {
        width = w;
        height = h;
        const usize n = static_cast<usize>(w) * h;
        rt4.assign(n, 1.f);
        rt0.assign(n, Vec4{});
        rt1.assign(n, Vec4{});
        rt2.assign(n, Vec4{});
        lit.assign(n, Vec4{});
        viewZ.assign(n, 0.f);
        worldN.assign(n, Vec3{});
    }
};

inline f32 half(f32 v) { return fuse::renderer::GBufferQuantize::toHalf(v); }
inline f32 unorm8(f32 v) { return fuse::renderer::GBufferQuantize::toUnorm8(v); }
inline u16 halfBits(f32 v) { return fuse::renderer::GBufferQuantize::floatToHalf(v); }

struct Material {
    Vec3 albedo{0.7f, 0.7f, 0.7f};
    f32 roughness = 0.8f;
    f32 metallic = 0.f;
    f32 ao = 1.f;
};

struct Hit {
    f32 t = 1e30f;
    Vec3 n{};
    u32 material = 0;
};

inline void hitPlane(const Vec3& o, const Vec3& d, const Vec3& n, const Vec3& p0, u32 material, Hit& best,
                     const Vec3* halfAxis = nullptr) {
    const f32 denom = n.dot(d);
    if (std::fabs(denom) < 1e-8f) {
        return;
    }
    const f32 t = n.dot(p0 - o) / denom;
    if (t <= 1e-4f || t >= best.t) {
        return;
    }
    if (halfAxis != nullptr && ((o + d * t) - p0).dot(*halfAxis) < 0.f) {
        return; // half-plane: only the side along halfAxis
    }
    best.t = t;
    best.n = denom < 0.f ? n : n * -1.f;
    best.material = material;
}

inline void hitBox(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi, u32 material, Hit& best) {
    f32 t0 = -1e30f;
    f32 t1 = 1e30f;
    u32 axis0 = 0;
    const f32 oo[3] = {o.x, o.y, o.z};
    const f32 dd[3] = {d.x, d.y, d.z};
    const f32 ll[3] = {lo.x, lo.y, lo.z};
    const f32 hh[3] = {hi.x, hi.y, hi.z};
    for (u32 a = 0; a < 3u; ++a) {
        if (std::fabs(dd[a]) < 1e-12f) {
            if (oo[a] < ll[a] || oo[a] > hh[a]) {
                return;
            }
            continue;
        }
        f32 ta = (ll[a] - oo[a]) / dd[a];
        f32 tb = (hh[a] - oo[a]) / dd[a];
        if (ta > tb) {
            std::swap(ta, tb);
        }
        if (ta > t0) {
            t0 = ta;
            axis0 = a;
        }
        t1 = std::min(t1, tb);
    }
    if (t0 > t1 || t0 <= 1e-4f || t0 >= best.t) {
        return;
    }
    Vec3 n{};
    const f32 s = dd[axis0] > 0.f ? -1.f : 1.f;
    if (axis0 == 0u) {
        n = Vec3{s, 0.f, 0.f};
    } else if (axis0 == 1u) {
        n = Vec3{0.f, s, 0.f};
    } else {
        n = Vec3{0.f, 0.f, s};
    }
    best.t = t0;
    best.n = n;
    best.material = material;
}

/// Writes one pixel of the G-buffer from a hit (world-space normal) and the lit radiance.
inline void writePixel(GBuffer& g, const Camera& cam, usize i, const Hit& hit, const Vec3& viewDir,
                       const Material& m, const Vec3& radiance) {
    const f32 zView = viewDir.z * hit.t; // viewDir.z = -1 (unnormalised ray), so zView = -t
    g.viewZ[i] = -zView;
    g.rt4[i] = cam.deviceDepth(zView);
    g.worldN[i] = hit.n;
    const fuse::math::Vec2 oct = fuse::renderer::GBufferEncoding::encodeNormalRgba16f(hit.n);
    g.rt0[i] = Vec4{oct.x, oct.y, 0.f, half(m.ao)};
    g.rt1[i] = Vec4{unorm8(m.albedo.x), unorm8(m.albedo.y), unorm8(m.albedo.z), 1.f};
    g.rt2[i] = Vec4{unorm8(m.roughness), unorm8(m.metallic), 0.f, 0.f};
    g.lit[i] = Vec4{half(radiance.x), half(radiance.y), half(radiance.z), 1.f};
}

// --- room -----------------------------------------------------------------------------------------------
struct RoomScene {
    Camera camera;
    f32 ambient[3] = {0.25f, 0.27f, 0.3f};
    Vec3 sunDir{0.35f, 0.8f, 0.45f}; ///< towards the light
    Vec3 sunColor{2.5f, 2.3f, 2.1f};
    Material materials[5];
    GBuffer g;
};

/// `frame` moves the camera a little (zero-alloc churn, multiple parity frames).
inline void buildRoom(RoomScene& s, u32 width, u32 height, u32 frame = 0) {
    s.camera.width = width;
    s.camera.height = height;
    s.camera.fovYDeg = 60.f;
    s.camera.nearPlane = 0.1f;
    s.camera.farPlane = 100.f;
    const f32 f = static_cast<f32>(frame);
    s.camera.eye = Vec3{0.6f + 0.15f * f, 2.2f + 0.05f * f, 6.5f};
    s.camera.target = Vec3{-0.3f, 0.6f, -2.f};
    s.camera.build();
    s.materials[0] = Material{Vec3{0.75f, 0.75f, 0.72f}, 0.12f, 0.f, 1.f};  // glossy floor
    s.materials[1] = Material{Vec3{0.8f, 0.8f, 0.8f}, 0.9f, 0.f, 0.9f};    // back wall
    s.materials[2] = Material{Vec3{0.85f, 0.12f, 0.1f}, 0.7f, 0.f, 1.f};   // red side wall
    s.materials[3] = Material{Vec3{0.2f, 0.75f, 0.25f}, 0.35f, 0.f, 0.8f}; // green box
    s.materials[4] = Material{Vec3{0.95f, 0.8f, 0.4f}, 0.25f, 1.f, 1.f};   // gold metal box
    s.g.resize(width, height);
    const Vec3 sun = s.sunDir.normalized();
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize i = static_cast<usize>(y) * width + x;
            Vec3 viewDir{};
            const Vec3 d = s.camera.rayWorld(x, y, viewDir);
            const Vec3 o = s.camera.eye;
            Hit hit{};
            hitPlane(o, d, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, 0.f, 0.f}, 0u, hit);
            hitPlane(o, d, Vec3{0.f, 0.f, 1.f}, Vec3{0.f, 0.f, -4.f}, 1u, hit);
            hitPlane(o, d, Vec3{1.f, 0.f, 0.f}, Vec3{-3.5f, 0.f, 0.f}, 2u, hit);
            hitBox(o, d, Vec3{-1.8f, 0.f, -2.2f}, Vec3{-0.6f, 1.6f, -1.2f}, 3u, hit);
            hitBox(o, d, Vec3{0.8f, 0.f, -1.0f}, Vec3{1.7f, 0.9f, 0.1f}, 4u, hit);
            if (hit.t >= 1e29f) {
                continue; // sky
            }
            const Vec3 p = o + d * hit.t;
            if (p.y > 3.5f || p.y < -0.01f || p.x > 4.5f || -(viewDir.z * hit.t) > s.camera.farPlane * 0.99f) {
                continue; // open ceiling / beyond the room: sky
            }
            const Material& m = s.materials[hit.material];
            const f32 ndl = std::max(0.f, hit.n.dot(sun));
            const Vec3 a{unorm8(m.albedo.x), unorm8(m.albedo.y), unorm8(m.albedo.z)};
            const f32 kd = 1.f - unorm8(m.metallic);
            const Vec3 radiance{a.x * (s.ambient[0] * half(m.ao) + kd * s.sunColor.x * ndl / kPi),
                                a.y * (s.ambient[1] * half(m.ao) + kd * s.sunColor.y * ndl / kPi),
                                a.z * (s.ambient[2] * half(m.ao) + kd * s.sunColor.z * ndl / kPi)};
            writePixel(s.g, s.camera, i, hit, viewDir, m, radiance);
        }
    }
}

// --- wedge ------------------------------------------------------------------------------------------------
struct WedgeScene {
    Camera camera;
    f32 alphaDeg = 90.f;
    Vec3 crease{};  ///< a point of the crease line (world)
    Vec3 axis{1.f, 0.f, 0.f};
    Vec3 faceA{};   ///< in-face direction away from the crease
    Vec3 faceB{};
    Vec3 normalA{};
    Vec3 normalB{};
    GBuffer g;
    std::vector<fuse::u8> face; ///< 0 = sky, 1 = A, 2 = B
    std::vector<f32> creaseDistance;
};

/// Wedge of opening `alphaDeg` (through the air) around the world X axis; the camera looks at the crease
/// from `distance` along the bisector (+ `lateral` across it, oblique views), tilted by `tiltDeg` about the
/// crease axis.
inline void buildWedge(WedgeScene& s, u32 width, u32 height, f32 alphaDeg, f32 fovYDeg = 90.f, f32 distance = 3.f,
                       f32 tiltDeg = 35.f, f32 lateral = 0.f) {
    s.alphaDeg = alphaDeg;
    const f32 half = 0.5f * alphaDeg * kPi / 180.f;
    const f32 tilt = tiltDeg * kPi / 180.f;
    // Bisector (world, in the YZ plane) pointing from the crease into the air.
    const Vec3 m{0.f, std::cos(tilt), std::sin(tilt)};
    const Vec3 side{0.f, -std::sin(tilt), std::cos(tilt)}; // perpendicular to m in the YZ plane
    s.crease = Vec3{0.f, 0.f, 0.f};
    s.faceA = m * std::cos(half) + side * std::sin(half);
    s.faceB = m * std::cos(half) - side * std::sin(half);
    s.normalA = (m - s.faceA * m.dot(s.faceA)).normalized();
    s.normalB = (m - s.faceB * m.dot(s.faceB)).normalized();
    s.camera.width = width;
    s.camera.height = height;
    s.camera.fovYDeg = fovYDeg;
    s.camera.nearPlane = 0.05f;
    s.camera.farPlane = 500.f;
    s.camera.eye = s.crease + m * distance + side * lateral;
    s.camera.target = s.crease;
    // Up: in the YZ plane (so the crease runs horizontally).
    s.camera.up = side;
    s.camera.build();
    s.g.resize(width, height);
    s.face.assign(static_cast<usize>(width) * height, 0u);
    s.creaseDistance.assign(static_cast<usize>(width) * height, 0.f);
    const Material mat{Vec3{0.6f, 0.6f, 0.6f}, 0.5f, 0.f, 1.f};
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize i = static_cast<usize>(y) * width + x;
            Vec3 viewDir{};
            const Vec3 d = s.camera.rayWorld(x, y, viewDir);
            const Vec3 o = s.camera.eye;
            Hit hit{};
            // Face planes contain the crease axis (X) and their in-face direction.
            const Vec3 nA = fuse::math::cross(s.axis, s.faceA).normalized();
            const Vec3 nB = fuse::math::cross(s.axis, s.faceB).normalized();
            hitPlane(o, d, nA, s.crease, 1u, hit, &s.faceA);
            hitPlane(o, d, nB, s.crease, 2u, hit, &s.faceB);
            if (hit.t >= 1e29f) {
                continue;
            }
            const Vec3 p = o + d * hit.t;
            if (-(viewDir.z * hit.t) > s.camera.farPlane * 0.99f) {
                continue;
            }
            hit.n = hit.material == 1u ? s.normalA : s.normalB;
            s.face[i] = static_cast<fuse::u8>(hit.material);
            const Vec3 rel = p - s.crease;
            s.creaseDistance[i] = (rel - s.axis * rel.dot(s.axis)).length();
            writePixel(s.g, s.camera, i, hit, viewDir, mat, Vec3{0.5f, 0.5f, 0.5f});
        }
    }
}

// --- CPU prepare (ssfx_gpu::prepare_pixel over a whole G-buffer) -------------------------------------------
inline fuse::renderer::ssfx_gpu::SsfxCameraDesc cameraDesc(const Camera& c) {
    fuse::renderer::ssfx_gpu::SsfxCameraDesc d{};
    for (u32 i = 0; i < 16u; ++i) {
        d.view[i] = c.view.data[i];
        d.proj[i] = c.proj.data[i];
    }
    d.nearPlane = c.nearPlane;
    d.farPlane = c.farPlane;
    d.reversedZ = false;
    return d;
}

inline void prepareCpu(const fuse::renderer::ssfx_gpu::SsfxFrameConstants& c, const GBuffer& g,
                       fuse::renderer::ssfx_gpu::SsfxPreparedFrame& out) {
    const usize n = static_cast<usize>(g.width) * g.height;
    out.width = g.width;
    out.height = g.height;
    out.prepared.resize(n);
    out.normal.resize(n);
    out.radiance.resize(n);
    out.litAlpha.resize(n);
    out.albedo.resize(n);
    out.diffuse.resize(n);
    for (usize i = 0; i < n; ++i) {
        const fuse::renderer::ssfx_gpu::PreparedPixel p =
            fuse::renderer::ssfx_gpu::prepare_pixel(c, g.rt4[i], g.rt0[i], g.rt1[i], g.rt2[i], g.lit[i]);
        out.prepared[i] = p.prepared;
        out.normal[i] = p.normal;
        out.radiance[i] = p.radiance;
        out.litAlpha[i] = p.litAlpha;
        out.albedo[i] = p.albedo;
        out.diffuse[i] = p.diffuse;
    }
}

inline f32 wedgeVisibility(f32 alphaDeg) { return 0.5f * (1.f - std::cos(alphaDeg * kPi / 180.f)); }

} // namespace ssfx_test
