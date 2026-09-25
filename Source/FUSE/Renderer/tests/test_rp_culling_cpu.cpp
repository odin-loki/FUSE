// WP-1.3 culling CPU gates (no Vulkan; also run in the stub tree). The Lavapipe gates are in
// test_rp_culling.cpp.
//
//   hiz        hiz_dims shapes; every texel of every level of build_hiz_reference equals the max of
//              the depth pixels its footprint covers (texels outside the image count as 1), for
//              square, non-square, odd, 1x1 and 1-wide extents
//   frustum    world_sphere contains the transformed object sphere under random affine transforms
//              (rotation, non-uniform scale, shear); a frustum-culled sphere has no sampled point
//              inside the view volume
//   occlusion  synthetic depth buffers (random screen rectangles) + random spheres: every sphere the
//              Hi-Z test rejects has no sampled point in front of the depth buffer (no false
//              occlusion), and the test rejects a non-trivial share (not vacuous)
//   phases     two-phase protocol: ineligible slots -> None, occlusion off -> phase 1 draws all
//              frustum-visible, no history -> all candidates, far Hi-Z -> all Phase2Drawn, near
//              Hi-Z -> only near-plane crossers drawn; the parity rule flags boundary instances only
//   parity     CpuReference == CpuParallel bit for bit (Hi-Z and cull) at 0 / 2 / 4 workers
//   api        InstanceCuller::init fails cleanly without a device; record layouts
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/culling/cull_reference.hpp>
#include <fuse/renderer/culling/cull_types.hpp>
#include <fuse/renderer/culling/hiz_build_kernel.hpp>
#include <fuse/renderer/culling/instance_cull_kernel.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/geometry/meshlet_cull_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::culling;
using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace kernel = fuse::kernel;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- math (column-major 4x4, Vulkan clip space, forward depth) --------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

Mat4 lookAt(const f32 eye[3], const f32 at[3]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& v : s) {
        v /= sl;
    }
    const f32 u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -f[0];
    v.m[6] = -f[1];
    v.m[10] = -f[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    v.m[15] = 1.f;
    return v;
}

void clip(const Mat4& vp, const f32 p[3], f32 out[4]) {
    for (u32 r = 0; r < 4; ++r) {
        out[r] = vp.m[r] * p[0] + vp.m[4 + r] * p[1] + vp.m[8 + r] * p[2] + vp.m[12 + r];
    }
}

CullConstants makeConstants(const Mat4& vp, const Mat4& prevVp, u32 depthW, u32 depthH, u32 flags, u32 count) {
    CullConstants c{};
    std::memcpy(c.viewProj, vp.m, sizeof(c.viewProj));
    std::memcpy(c.prevViewProj, prevVp.m, sizeof(c.prevViewProj));
    const f32 origin[3] = {0.f, 0.f, 0.f};
    const geometry::cull_kernel::CullView view = geometry::cull_kernel::make_cull_view(vp.m, origin);
    std::memcpy(c.planes, view.planes, sizeof(c.planes));
    const hiz_kernel::HizDims d = hiz_kernel::hiz_dims(depthW, depthH);
    c.hizDim = d.dim0;
    c.hizMipCount = d.mipCount;
    c.hizScale[0] = d.scaleX;
    c.hizScale[1] = d.scaleY;
    c.flags = flags;
    c.instanceCount = count;
    return c;
}

gpu_scene::GpuTransform affine(std::mt19937& rng, f32 spread, f32 minScale, f32 maxScale, bool shear) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    std::uniform_real_distribution<f32> s(minScale, maxScale);
    // Random rotation from a normalised quaternion.
    f32 q[4] = {u(rng), u(rng), u(rng), u(rng)};
    const f32 ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) + 1e-6f;
    for (f32& v : q) {
        v /= ql;
    }
    const f32 x = q[0], y = q[1], z = q[2], w = q[3];
    const f32 r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                         {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                         {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    const f32 sc[3] = {s(rng), s(rng), s(rng)};
    const f32 sh = shear ? u(rng) * 0.5f : 0.f;
    gpu_scene::GpuTransform t{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 3; ++col) {
            t.rows[row][col] = r[row][col] * sc[col] + (col == 1 ? r[row][0] * sh : 0.f);
        }
        t.rows[row][3] = u(rng) * spread;
    }
    return t;
}

void apply(const gpu_scene::GpuTransform& t, const f32 p[3], f32 out[3]) {
    for (u32 r = 0; r < 3; ++r) {
        out[r] = t.rows[r][0] * p[0] + t.rows[r][1] * p[1] + t.rows[r][2] * p[2] + t.rows[r][3];
    }
}

/// Uniform point in the unit ball.
void ballPoint(std::mt19937& rng, f32 out[3]) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    do {
        out[0] = u(rng);
        out[1] = u(rng);
        out[2] = u(rng);
    } while (out[0] * out[0] + out[1] * out[1] + out[2] * out[2] > 1.f);
}

// --- hiz --------------------------------------------------------------------------------------------
int runHiz() {
    {
        const hiz_kernel::HizDims a = hiz_kernel::hiz_dims(256, 256);
        expect(a.dim0 == 128u && a.mipCount == 8u, "hiz_dims(256, 256) = 128 x 8 levels");
        const hiz_kernel::HizDims b = hiz_kernel::hiz_dims(1920, 1080);
        expect(b.dim0 == 1024u && b.mipCount == 11u && b.scaleX == 960.f && b.scaleY == 540.f, "hiz_dims(1920, 1080)");
        const hiz_kernel::HizDims c = hiz_kernel::hiz_dims(1, 1);
        expect(c.dim0 == 1u && c.mipCount == 1u, "hiz_dims(1, 1)");
        const hiz_kernel::HizDims d = hiz_kernel::hiz_dims(8192, 8192);
        expect(d.dim0 == 4096u && d.mipCount == kMaxHizMips, "hiz_dims(8192) = 13 levels");
        expect(hiz_kernel::hiz_dims(8193, 4).dim0 == 0u && hiz_kernel::hiz_dims(0, 4).dim0 == 0u, "bad extents rejected");
    }
    const u32 sizes[][2] = {{1, 1}, {2, 2}, {3, 1}, {1, 7}, {7, 5}, {64, 64}, {100, 37}, {256, 256}, {257, 129}, {1000, 3}};
    std::mt19937 rng(3);
    std::uniform_real_distribution<f32> depthDist(0.f, 1.f);
    u32 checked = 0;
    for (const auto& size : sizes) {
        const u32 w = size[0];
        const u32 h = size[1];
        std::vector<f32> depth(static_cast<usize>(w) * h);
        for (f32& v : depth) {
            v = depthDist(rng);
        }
        HizPyramid pyramid;
        expect(build_hiz_reference(depth.data(), w, h, pyramid), "build_hiz_reference");
        const hiz_kernel::HizDims dims = hiz_kernel::hiz_dims(w, h);
        expect(pyramid.dim0 == dims.dim0 && pyramid.mipCount == dims.mipCount, "pyramid shape");
        bool ok = true;
        for (u32 level = 0; level < pyramid.mipCount && ok; ++level) {
            const u32 dim = hiz_kernel::mip_dim(pyramid.dim0, level);
            const u32 span = 2u << level; // depth pixels per texel side
            for (u32 y = 0; y < dim && ok; ++y) {
                for (u32 x = 0; x < dim && ok; ++x) {
                    f32 expected = 0.f;
                    for (u32 py = y * span; py < (y + 1) * span; ++py) {
                        for (u32 px = x * span; px < (x + 1) * span; ++px) {
                            const f32 v = (px < w && py < h) ? depth[static_cast<usize>(py) * w + px] : 1.f;
                            expected = std::max(expected, v);
                        }
                    }
                    const f32 got = pyramid.levels[level][static_cast<usize>(y) * dim + x];
                    if (std::memcmp(&got, &expected, 4) != 0) {
                        std::fprintf(stderr, "  %ux%u level %u texel (%u,%u): %.9g != brute force %.9g\n", w, h, level, x,
                                     y, static_cast<double>(got), static_cast<double>(expected));
                        ok = false;
                    }
                    ++checked;
                }
            }
        }
        expect(ok, "every Hi-Z texel == max over its depth footprint");
    }
    std::printf("hiz: %zu extents, %u texels equal the brute-force footprint max\n", std::size(sizes), checked);
    return 0;
}

// --- frustum ----------------------------------------------------------------------------------------
int runFrustum() {
    std::mt19937 rng(5);
    // world_sphere contains the transformed object-space sphere.
    u32 samples = 0;
    f32 worstRatio = 0.f;
    for (u32 t = 0; t < 400; ++t) {
        const gpu_scene::GpuTransform xf = affine(rng, 50.f, 0.05f, 4.f, (t & 1u) != 0u);
        gpu_scene::GpuMesh mesh{};
        std::uniform_real_distribution<f32> u(-2.f, 2.f);
        mesh.boundsCenter[0] = u(rng);
        mesh.boundsCenter[1] = u(rng);
        mesh.boundsCenter[2] = u(rng);
        mesh.boundsRadius = 0.1f + std::fabs(u(rng));
        const cull_kernel::Sphere s = cull_kernel::world_sphere(xf, mesh, 1.f);
        for (u32 k = 0; k < 200; ++k) {
            f32 b[3];
            ballPoint(rng, b);
            if (k < 6) { // axis extremes
                b[0] = b[1] = b[2] = 0.f;
                b[k % 3] = (k < 3) ? 1.f : -1.f;
            }
            const f32 p[3] = {mesh.boundsCenter[0] + b[0] * mesh.boundsRadius, mesh.boundsCenter[1] + b[1] * mesh.boundsRadius,
                              mesh.boundsCenter[2] + b[2] * mesh.boundsRadius};
            f32 w[3];
            apply(xf, p, w);
            const f32 d = std::sqrt((w[0] - s.c[0]) * (w[0] - s.c[0]) + (w[1] - s.c[1]) * (w[1] - s.c[1]) +
                                    (w[2] - s.c[2]) * (w[2] - s.c[2]));
            worstRatio = std::max(worstRatio, d / s.r);
            ++samples;
        }
    }
    expect(worstRatio <= 1.f + 1e-5f, "world_sphere contains every transformed point of the object sphere");

    // Frustum test is conservative.
    u32 culled = 0;
    u32 tested = 0;
    for (u32 v = 0; v < 48; ++v) {
        std::uniform_real_distribution<f32> u(-30.f, 30.f);
        const f32 eye[3] = {u(rng), u(rng), u(rng)};
        const f32 at[3] = {u(rng), u(rng), u(rng)};
        const Mat4 vp = mul(perspective(0.9f + 0.02f * static_cast<f32>(v), 1.3f, 0.1f, 80.f), lookAt(eye, at));
        const CullConstants c = makeConstants(vp, vp, 64, 64, kCullFrustum, 0);
        for (u32 i = 0; i < 300; ++i) {
            cull_kernel::Sphere s{};
            s.c[0] = u(rng) * 2.f;
            s.c[1] = u(rng) * 2.f;
            s.c[2] = u(rng) * 2.f;
            s.r = 0.2f + std::fabs(u(rng)) * 0.3f;
            ++tested;
            if (cull_kernel::frustum_visible(c, s)) {
                continue;
            }
            ++culled;
            for (u32 k = 0; k < 400; ++k) {
                f32 b[3];
                ballPoint(rng, b);
                const f32 p[3] = {s.c[0] + b[0] * s.r, s.c[1] + b[1] * s.r, s.c[2] + b[2] * s.r};
                f32 cl[4];
                clip(vp, p, cl);
                const bool inside = cl[3] > 0.f && std::fabs(cl[0]) <= cl[3] && std::fabs(cl[1]) <= cl[3] && cl[2] >= 0.f &&
                                    cl[2] <= cl[3];
                if (inside) {
                    std::fprintf(stderr, "  view %u sphere %u: culled but a point is inside the frustum\n", v, i);
                    ++g_failures;
                    break;
                }
            }
        }
    }
    expect(culled > tested / 10u, "frustum test rejects a non-trivial share");
    std::printf("frustum: %u transformed samples (max dist / radius %.7f), %u / %u spheres culled, all conservative\n",
                samples, static_cast<double>(worstRatio), culled, tested);
    return 0;
}

// --- occlusion --------------------------------------------------------------------------------------
struct DepthScene {
    u32 w = 0;
    u32 h = 0;
    std::vector<f32> depth;
};

void rasterRects(std::mt19937& rng, DepthScene& d, u32 rects) {
    d.depth.assign(static_cast<usize>(d.w) * d.h, 1.f);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    for (u32 r = 0; r < rects; ++r) {
        const u32 x0 = static_cast<u32>(u(rng) * static_cast<f32>(d.w));
        const u32 y0 = static_cast<u32>(u(rng) * static_cast<f32>(d.h));
        const u32 x1 = std::min(d.w, x0 + 1u + static_cast<u32>(u(rng) * static_cast<f32>(d.w) * 0.6f));
        const u32 y1 = std::min(d.h, y0 + 1u + static_cast<u32>(u(rng) * static_cast<f32>(d.h) * 0.6f));
        const f32 z = 0.2f + 0.7f * u(rng);
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                f32& v = d.depth[static_cast<usize>(y) * d.w + x];
                v = std::min(v, z);
            }
        }
    }
}

int runOcclusion() {
    std::mt19937 rng(9);
    const u32 sizes[][2] = {{64, 64}, {200, 120}, {37, 23}, {256, 256}};
    u32 tested = 0;
    u32 occluded = 0;
    u32 samples = 0;
    for (const auto& size : sizes) {
        for (u32 view = 0; view < 6; ++view) {
            DepthScene d;
            d.w = size[0];
            d.h = size[1];
            rasterRects(rng, d, 12);
            HizPyramid pyramid;
            build_hiz_reference(d.depth.data(), d.w, d.h, pyramid);
            const cull_kernel::HizLevels levels = pyramid.view();
            const cull_kernel::HizFetchCpu fetch{&levels};
            const f32 eye[3] = {0.f, 0.f, 0.f};
            const f32 at[3] = {0.1f * static_cast<f32>(view), 0.f, -1.f};
            const Mat4 vp = mul(perspective(1.1f, static_cast<f32>(d.w) / static_cast<f32>(d.h), 0.5f, 50.f), lookAt(eye, at));
            const CullConstants c = makeConstants(vp, vp, d.w, d.h, kCullFrustum | kCullOcclusion | kCullHistoryValid, 0);
            std::uniform_real_distribution<f32> u(-1.f, 1.f);
            for (u32 i = 0; i < 400; ++i) {
                cull_kernel::Sphere s{};
                const f32 dist = 1.f + 20.f * (u(rng) * 0.5f + 0.5f);
                s.c[0] = 0.1f * static_cast<f32>(view) * dist + u(rng) * dist * 0.5f;
                s.c[1] = u(rng) * dist * 0.4f;
                s.c[2] = -dist;
                s.r = 0.05f + 0.6f * (u(rng) * 0.5f + 0.5f);
                if (!cull_kernel::frustum_visible(c, s)) {
                    continue;
                }
                ++tested;
                if (cull_kernel::hiz_visible(c.viewProj, s, c, fetch)) {
                    continue;
                }
                ++occluded;
                // Brute force: no point of the ball may be in front of the depth buffer.
                for (u32 k = 0; k < 600; ++k) {
                    f32 b[3];
                    ballPoint(rng, b);
                    const f32 p[3] = {s.c[0] + b[0] * s.r, s.c[1] + b[1] * s.r, s.c[2] + b[2] * s.r};
                    f32 cl[4];
                    clip(vp, p, cl);
                    ++samples;
                    if (!(cl[3] > 0.f)) {
                        continue;
                    }
                    const f32 nx = cl[0] / cl[3];
                    const f32 ny = cl[1] / cl[3];
                    const f32 nz = cl[2] / cl[3];
                    if (nx < -1.f || nx > 1.f || ny < -1.f || ny > 1.f || nz < 0.f || nz > 1.f) {
                        continue;
                    }
                    const u32 px = std::min(d.w - 1u, static_cast<u32>((nx * 0.5f + 0.5f) * static_cast<f32>(d.w)));
                    const u32 py = std::min(d.h - 1u, static_cast<u32>((ny * 0.5f + 0.5f) * static_cast<f32>(d.h)));
                    if (nz <= d.depth[static_cast<usize>(py) * d.w + px]) {
                        std::fprintf(stderr, "  %ux%u view %u sphere %u: occluded but a point is visible (z %.6f <= %.6f)\n",
                                     d.w, d.h, view, i, static_cast<double>(nz),
                                     static_cast<double>(d.depth[static_cast<usize>(py) * d.w + px]));
                        ++g_failures;
                        break;
                    }
                }
            }
        }
    }
    expect(occluded * 10u > tested, "the Hi-Z test rejects more than 10% (not vacuous)");
    std::printf("occlusion: %u / %u frustum-visible spheres occluded, %u ball samples: no false occlusion\n", occluded,
                tested, samples);
    return 0;
}

// --- phases -----------------------------------------------------------------------------------------
struct TestScene {
    gpu_scene::GpuScene scene;
    u32 hidden = 0;
};

void buildScene(TestScene& t, u32 n, u32 seed) {
    t.scene.init(gpu_scene::GpuSceneDesc{});
    gpu_scene::GpuMesh cube{};
    cube.boundsRadius = std::sqrt(3.f);
    cube.triangleCount = 12;
    t.scene.addMesh(cube);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 i = 0; i < n; ++i) {
        gpu_scene::InstanceDesc d{};
        d.mesh = (i % 97u == 5u) ? 7u : 0u; // a few instances with a missing mesh
        const f32 s = 0.2f + 0.3f * (u(rng) * 0.5f + 0.5f);
        d.transform.rows[0][0] = s;
        d.transform.rows[1][1] = s;
        d.transform.rows[2][2] = s;
        d.transform.rows[0][3] = u(rng) * 20.f;
        d.transform.rows[1][3] = u(rng) * 20.f;
        d.transform.rows[2][3] = -2.f - 30.f * (u(rng) * 0.5f + 0.5f);
        if (i % 50u == 7u) {
            d.flags &= ~static_cast<u32>(gpu_scene::kInstanceVisible);
            ++t.hidden;
        }
        t.scene.addInstance(d);
    }
    // One instance straddling the near plane.
    gpu_scene::InstanceDesc near{};
    near.mesh = 0;
    near.transform.rows[2][3] = -0.3f;
    t.scene.addInstance(near);
}

int runPhases() {
    TestScene t;
    buildScene(t, 3000, 21);
    const SceneSpans spans = scene_spans(t.scene);
    const f32 eye[3] = {0.f, 0.f, 0.f};
    const f32 at[3] = {0.f, 0.f, -1.f};
    const Mat4 vp = mul(perspective(1.2f, 1.f, 0.5f, 60.f), lookAt(eye, at));
    const u32 n = t.scene.instanceHighWater();
    HizPyramid farHiz, nearHiz;
    std::vector<f32> ones(64 * 64, 1.f), zeros(64 * 64, 0.f);
    build_hiz_reference(ones.data(), 64, 64, farHiz);
    build_hiz_reference(zeros.data(), 64, 64, nearHiz);
    std::vector<u32> frustumOnly, noOcclusion, noHistory, allNear, historyFar;
    cull_reference(spans, makeConstants(vp, vp, 64, 64, kCullFrustum, n), farHiz.view(), farHiz.view(), 1.f, noOcclusion);
    cull_reference(spans, makeConstants(vp, vp, 64, 64, kCullFrustum | kCullOcclusion, n), farHiz.view(), farHiz.view(),
                   1.f, noHistory);
    cull_reference(spans, makeConstants(vp, vp, 64, 64, kCullFrustum | kCullOcclusion, n), farHiz.view(), nearHiz.view(),
                   1.f, allNear);
    cull_reference(spans, makeConstants(vp, vp, 64, 64, kCullFrustum | kCullOcclusion | kCullHistoryValid, n),
                   farHiz.view(), farHiz.view(), 1.f, historyFar);
    u32 none = 0, frustumCulled = 0, visible = 0, nearDrawn = 0;
    bool ok = true;
    for (u32 i = 0; i < n; ++i) {
        const u32 a = noOcclusion[i];
        none += a == kResultNone;
        frustumCulled += a == kResultFrustumCulled;
        visible += a == kResultPhase1Drawn;
        ok = ok && (a == kResultNone || a == kResultFrustumCulled || a == kResultPhase1Drawn);
        // No history: every frustum-visible instance goes through phase 2; a far Hi-Z passes all.
        ok = ok && (a != kResultPhase1Drawn || noHistory[i] == kResultPhase2Drawn);
        ok = ok && (a == kResultPhase1Drawn || noHistory[i] == a);
        // A near (all-0) Hi-Z occludes everything except what crosses the near plane.
        if (allNear[i] == kResultPhase2Drawn) {
            ++nearDrawn;
        }
        ok = ok && (a != kResultPhase1Drawn || allNear[i] == kResultPhase2Drawn || allNear[i] == kResultOccluded);
        // History + far Hi-Z: everything visible is drawn in phase 1.
        ok = ok && (a != kResultPhase1Drawn || historyFar[i] == kResultPhase1Drawn);
    }
    expect(ok, "phase protocol invariants");
    u32 expectedNone = 0;
    for (u32 i = 0; i < 3000u; ++i) {
        expectedNone += (i % 97u == 5u || i % 50u == 7u) ? 1u : 0u;
    }
    expect(none == expectedNone, "hidden / meshless slots are None");
    expect(nearDrawn == 1u && allNear[n - 1u] == kResultPhase2Drawn, "near Hi-Z: only the near-plane crosser is drawn");
    expect(visible > 100u && frustumCulled > 100u, "scene has visible and frustum-culled instances");

    // Parity rule: ambiguous instances are rare, and a far-from-boundary scene has none.
    CullParityReference parity;
    cull_reference_parity(spans, makeConstants(vp, vp, 64, 64, kCullFrustum | kCullOcclusion | kCullHistoryValid, n),
                          farHiz.view(), nearHiz.view(), parity);
    expect(parity.ambiguousCount * 100u < n, "boundary (ambiguous) instances < 1%");
    std::printf("phases: %u slots: %u none, %u frustum-culled, %u visible; near Hi-Z draws %u; %u ambiguous at e = %g\n", n,
                none, frustumCulled, visible, nearDrawn, parity.ambiguousCount,
                static_cast<double>(cull_kernel::kParityEpsilon));
    return 0;
}

// --- parity -----------------------------------------------------------------------------------------
int runParity() {
    fuse::jobs::JobScheduler& jobs = fuse::jobs::JobScheduler::instance();
    TestScene t;
    buildScene(t, 20000, 33);
    const SceneSpans spans = scene_spans(t.scene);
    std::mt19937 rng(41);
    DepthScene d;
    d.w = 320;
    d.h = 200;
    rasterRects(rng, d, 20);
    const f32 eye[3] = {0.f, 0.f, 0.f};
    const f32 at[3] = {0.f, 0.f, -1.f};
    const f32 eye2[3] = {0.5f, 0.2f, 0.f};
    const Mat4 vp = mul(perspective(1.2f, 1.6f, 0.5f, 60.f), lookAt(eye, at));
    const Mat4 prev = mul(perspective(1.2f, 1.6f, 0.5f, 60.f), lookAt(eye2, at));
    const CullConstants c =
        makeConstants(vp, prev, d.w, d.h, kCullFrustum | kCullOcclusion | kCullHistoryValid, t.scene.instanceHighWater());
    HizPyramid refHiz;
    build_hiz_reference(d.depth.data(), d.w, d.h, refHiz, kernel::Backend::CpuReference);
    std::vector<u32> refResults;
    cull_reference(spans, c, refHiz.view(), refHiz.view(), 1.f, refResults, kernel::Backend::CpuReference);
    u32 occluded = 0;
    for (const u32 r : refResults) {
        occluded += r == kResultOccluded;
    }
    for (const u32 workers : {0u, 2u, 4u}) {
        jobs.shutdown();
        jobs.initialize(workers);
        HizPyramid parHiz;
        build_hiz_reference(d.depth.data(), d.w, d.h, parHiz, kernel::Backend::CpuParallel);
        bool same = parHiz.mipCount == refHiz.mipCount;
        for (u32 l = 0; l < refHiz.mipCount && same; ++l) {
            same = std::memcmp(parHiz.levels[l].data(), refHiz.levels[l].data(), refHiz.levels[l].size() * 4u) == 0;
        }
        expect(same, "Hi-Z: CpuParallel == CpuReference");
        std::vector<u32> parResults;
        cull_reference(spans, c, parHiz.view(), parHiz.view(), 1.f, parResults, kernel::Backend::CpuParallel);
        expect(parResults == refResults, "cull: CpuParallel == CpuReference");
    }
    jobs.shutdown();
    std::printf("parity: %u instances (%u occluded), Hi-Z %u levels: CpuParallel == CpuReference at 0/2/4 workers\n",
                static_cast<u32>(refResults.size()), occluded, refHiz.mipCount);
    return 0;
}

// --- api --------------------------------------------------------------------------------------------
int runApi() {
    InstanceCuller culler;
    expect(!culler.init(InstanceCullerDesc{}), "init without a device fails");
    expect(!culler.valid() && !culler.setResolution(64, 64), "an uninitialised culler refuses work");
    CullFrameDesc frame{};
    expect(!culler.beginFrame(1, frame), "beginFrame without init fails");
    rg::Graph graph;
    const CullGraphRefs refs = culler.importInto(graph);
    expect(!refs.args.valid() && !refs.hiz.valid(), "importInto without init imports nothing");
    expect(sizeof(CullConstants) == 352u && sizeof(DrawIndexedIndirectCommand) == 20u && sizeof(CullPush) == 32u &&
               sizeof(HizPush) == 32u,
           "record sizes");
    std::printf("api: uninitialised culler is inert; record layouts pinned\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (suite == "hiz" || suite == "all") {
        rc |= runHiz();
    }
    if (suite == "frustum" || suite == "all") {
        rc |= runFrustum();
    }
    if (suite == "occlusion" || suite == "all") {
        rc |= runOcclusion();
    }
    if (suite == "phases" || suite == "all") {
        rc |= runPhases();
    }
    if (suite == "parity" || suite == "all") {
        rc |= runParity();
    }
    if (suite == "api" || suite == "all") {
        rc |= runApi();
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
