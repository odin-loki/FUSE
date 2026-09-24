#pragma once
// WP-3.2 gates: shared helpers of test_rp_vsm_raster_cpu.cpp and test_rp_vsm_raster.cpp. Analytic
// worlds (a ground plane y = 0 and axis-aligned boxes) that are also GpuScene meshes (the WP-1.5 test
// meshes: mr_test::plane, mr_test::box), a Vulkan-convention camera, double-precision ray casting and
// the B5 CSM seam scene (tests/test_b5_shadows_gates.cpp) the seam gates port to the VSM.
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vsmr_test {

using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::u32;
using fuse::u64;

constexpr f64 kPi = 3.14159265358979323846;

// --- double vectors ---------------------------------------------------------------------------------
struct D3 {
    f64 x = 0.0, y = 0.0, z = 0.0;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline D3 normalize(D3 a) {
    const f64 l = std::sqrt(dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

struct Box {
    D3 lo, hi;
};

/// Slab test: entry distance in [tMin, tMax], or < 0 when missed.
inline f64 rayBox(const D3& o, const D3& d, const Box& b, f64 tMin, f64 tMax) {
    const f64 os[3] = {o.x, o.y, o.z};
    const f64 ds[3] = {d.x, d.y, d.z};
    const f64 lo[3] = {b.lo.x, b.lo.y, b.lo.z};
    const f64 hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    f64 t0 = tMin;
    f64 t1 = tMax;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ds[i]) < 1e-15) {
            if (os[i] < lo[i] || os[i] > hi[i]) {
                return -1.0;
            }
            continue;
        }
        f64 ta = (lo[i] - os[i]) / ds[i];
        f64 tb = (hi[i] - os[i]) / ds[i];
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) {
            return -1.0;
        }
    }
    return t0;
}

struct Lcg {
    unsigned long long state = 0x9E3779B97F4A7C15ull;
    f64 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f64>((state >> 11) & ((1ull << 53) - 1ull)) / static_cast<f64>(1ull << 53);
    }
};

// --- matrices (column-major, Vulkan clip space, forward depth) ----------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 4; ++k) {
                s += static_cast<f64>(a.m[k * 4 + row]) * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = static_cast<f32>(s);
        }
    }
    return r;
}

inline Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

inline Mat4 lookAt(const D3& eye, const D3& at) {
    const D3 f = normalize(at - eye);
    const D3 upRef = std::fabs(f.y) > 0.999 ? D3{0, 0, 1} : D3{0, 1, 0};
    const D3 s = normalize(cross(f, upRef));
    const D3 u = cross(s, f);
    Mat4 v{};
    v.m[0] = static_cast<f32>(s.x);
    v.m[4] = static_cast<f32>(s.y);
    v.m[8] = static_cast<f32>(s.z);
    v.m[1] = static_cast<f32>(u.x);
    v.m[5] = static_cast<f32>(u.y);
    v.m[9] = static_cast<f32>(u.z);
    v.m[2] = static_cast<f32>(-f.x);
    v.m[6] = static_cast<f32>(-f.y);
    v.m[10] = static_cast<f32>(-f.z);
    v.m[12] = static_cast<f32>(-dot(s, eye));
    v.m[13] = static_cast<f32>(-dot(u, eye));
    v.m[14] = static_cast<f32>(dot(f, eye));
    v.m[15] = 1.f;
    return v;
}

/// Gauss-Jordan inverse in double (column-major).
inline bool invert(const f32 in[16], f64 out[16]) {
    f64 a[4][8];
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            a[r][c] = in[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (u32 c = 0; c < 4; ++c) {
        u32 piv = c;
        for (u32 r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) {
                piv = r;
            }
        }
        if (std::fabs(a[piv][c]) < 1e-30) {
            return false;
        }
        for (u32 k = 0; k < 8; ++k) {
            std::swap(a[c][k], a[piv][k]);
        }
        const f64 d = a[c][c];
        for (u32 k = 0; k < 8; ++k) {
            a[c][k] /= d;
        }
        for (u32 r = 0; r < 4; ++r) {
            if (r != c) {
                const f64 f = a[r][c];
                for (u32 k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            out[c * 4 + r] = a[r][c + 4];
        }
    }
    return true;
}

inline D3 transformH(const f64 m[16], f64 x, f64 y, f64 z) {
    const f64 w = m[3] * x + m[7] * y + m[11] * z + m[15];
    return {(m[0] * x + m[4] * y + m[8] * z + m[12]) / w, (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
            (m[2] * x + m[6] * y + m[10] * z + m[14]) / w};
}

struct Camera {
    D3 eye{};
    D3 at{};
    f32 fovY = 1.0472f; ///< 60 degrees (the B5 seam camera)
    f32 zNear = 0.1f;
    f32 zFar = 200.f;
    u32 width = 320;
    u32 height = 180;
    Mat4 viewProj{};
    Mat4 invViewProjF{};
    f64 invViewProj[16] = {};

    void build() {
        viewProj = mul(perspective(fovY, static_cast<f32>(width) / static_cast<f32>(height), zNear, zFar), lookAt(eye, at));
        invert(viewProj.m, invViewProj);
        for (u32 i = 0; i < 16u; ++i) {
            invViewProjF.m[i] = static_cast<f32>(invViewProj[i]);
        }
    }
    /// World ray through the centre of pixel (x, y).
    void ray(u32 x, u32 y, D3& o, D3& d) const {
        const f64 nx = (x + 0.5) * 2.0 / width - 1.0;
        const f64 ny = (y + 0.5) * 2.0 / height - 1.0;
        const D3 a = transformH(invViewProj, nx, ny, 0.0);
        const D3 b = transformH(invViewProj, nx, ny, 1.0);
        o = a;
        d = normalize(b - a);
    }
    f64 depthOf(const D3& p) const {
        const f32* m = viewProj.m;
        const f64 z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        const f64 w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        return z / w;
    }
    f32 pixelSpread() const { return 2.f * std::tan(fovY * 0.5f) / static_cast<f32>(height); }
};

// --- analytic world == GpuScene ---------------------------------------------------------------------
/// Ground plane y = 0 (a square of `groundSize`) plus axis-aligned boxes, each also a GpuScene instance
/// (mesh 0 = mr_test::plane, mesh 1 = mr_test::box scaled / translated: exact axis-aligned boxes).
struct World {
    f32 groundSize = 420.f;
    std::vector<Box> boxes;
    std::vector<fuse::u32> boxSlots;

    enum Hit : u32 { kMiss = 0, kGround = 1, kBox = 2 };

    /// Nearest hit along a ray (t > tMin).
    Hit cast(const D3& o, const D3& d, f64 tMin, f64 tMax, f64& t, u32* boxIndex = nullptr) const {
        Hit hit = kMiss;
        t = tMax;
        if (std::fabs(d.y) > 1e-15) {
            const f64 tg = -o.y / d.y;
            const D3 p = o + d * tg;
            if (tg > tMin && tg < t && std::fabs(p.x) <= groundSize * 0.5 && std::fabs(p.z) <= groundSize * 0.5) {
                t = tg;
                hit = kGround;
            }
        }
        for (u32 i = 0; i < boxes.size(); ++i) {
            const f64 tb = rayBox(o, d, boxes[i], tMin, t);
            if (tb >= 0.0 && tb < t) {
                t = tb;
                hit = kBox;
                if (boxIndex != nullptr) {
                    *boxIndex = i;
                }
            }
        }
        return hit;
    }
    bool occluded(const D3& p, const D3& towardLight, f64 maxT = 1e9) const {
        for (const Box& b : boxes) {
            if (rayBox(p, towardLight, b, 1e-7, maxT) >= 0.0) {
                return true;
            }
        }
        return false;
    }

    static fuse::renderer::gpu_scene::GpuTransform boxTransform(const Box& b) {
        fuse::renderer::gpu_scene::GpuTransform t{};
        t.rows[0][0] = static_cast<f32>((b.hi.x - b.lo.x) * 0.5);
        t.rows[1][1] = static_cast<f32>((b.hi.y - b.lo.y) * 0.5);
        t.rows[2][2] = static_cast<f32>((b.hi.z - b.lo.z) * 0.5);
        t.rows[0][3] = static_cast<f32>((b.hi.x + b.lo.x) * 0.5);
        t.rows[1][3] = static_cast<f32>((b.hi.y + b.lo.y) * 0.5);
        t.rows[2][3] = static_cast<f32>((b.hi.z + b.lo.z) * 0.5);
        return t;
    }
};

/// The meshes every world uses (mesh 0 ground, mesh 1 box).
inline bool buildMeshes(std::vector<fuse::renderer::geometry::MeshletMesh>& meshes, f32 groundSize) {
    meshes.resize(2);
    return mr_test::build(mr_test::plane(8, groundSize, 8.f), meshes[0]) && mr_test::build(mr_test::box(), meshes[1]);
}

/// Adds the world's instances (ground first, then the boxes in order).
inline bool addInstances(fuse::renderer::gpu_scene::GpuScene& scene, World& w, u32 material = 0xFFFFFFFFu) {
    using namespace fuse::renderer::gpu_scene;
    InstanceDesc ground{};
    ground.mesh = 0;
    ground.material = material;
    if (!scene.addInstance(ground).valid()) {
        return false;
    }
    w.boxSlots.clear();
    for (const Box& b : w.boxes) {
        InstanceDesc id{};
        id.mesh = 1;
        id.material = material;
        id.transform = World::boxTransform(b);
        const InstanceHandle h = scene.addInstance(id);
        if (!h.valid()) {
            return false;
        }
        w.boxSlots.push_back(h.slot);
    }
    return true;
}

// --- the B5 CSM seam scene (tests/test_b5_shadows_gates.cpp) ----------------------------------------
/// Ground y = 0 plus a long wall along the view axis through every cascade (here: every clipmap level).
inline World seamWorld() {
    World w;
    w.boxes.push_back(Box{{-0.5, 0.0, -180.0}, {0.5, 4.0, 15.0}});
    return w;
}
inline D3 seamSunTravel() { return normalize(D3{1.0, -1.2, -0.3}); }
inline Camera seamCamera() {
    Camera c;
    c.eye = {0.0, 6.0, 20.0};
    c.at = c.eye + normalize(D3{0.0, -0.15, -1.0});
    c.build();
    return c;
}

/// B5's analyticClear: the analytic answer is the same everywhere within `margin` around a ground point
/// (away from shadow edges) and the point is not next to the wall's footprint.
inline bool analyticClear(const World& w, const D3& p, const D3& toLight, f64 margin) {
    const bool centre = w.occluded(p, toLight);
    const D3 offsets[8] = {{margin, 0, 0}, {-margin, 0, 0}, {0, 0, margin}, {0, 0, -margin},
                           {margin, 0, margin}, {-margin, 0, margin}, {margin, 0, -margin}, {-margin, 0, -margin}};
    for (const D3& o : offsets) {
        if (w.occluded(p + o, toLight) != centre) {
            return false;
        }
    }
    for (const Box& b : w.boxes) {
        if (p.x > b.lo.x - margin && p.x < b.hi.x + margin && p.z > b.lo.z - margin && p.z < b.hi.z + margin) {
            return false;
        }
    }
    return true;
}

} // namespace vsmr_test
