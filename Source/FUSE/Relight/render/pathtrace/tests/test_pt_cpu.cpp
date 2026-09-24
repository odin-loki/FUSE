// FUSE Relight RL-5.1 CPU gates: the CPU reference path tracer (render/pathtrace/pt_reference.hpp) over the
// single-source core (kernels/pt_reference_core.h) against analytic cases, and its invariants.
//
//   furnace      white furnace: albedo-1 Lambertian, rough metal, rough dielectric and glass spheres under a uniform
//                sky of radiance 1 render 1 (per pixel within 4 sigma + 1%, the sphere mean within 1%); sky pixels
//                are exactly the sky.
//   lambert_sky  a Lambertian (RL-4.3 opaque: albedo-scaled diffuse under dielectric GGX) plane under a uniform sky:
//                L = sky x E(mu_o), the directional albedo integrated by an independent quadrature of bsdfEval.
//   direct       direct illumination (maxBounces 1) of a plane: a delta distant light (closed form: bsdfEval x E), a
//                sphere light, a one-sided rect light and an emissive-triangle panel (the exact integrand, integrated by
//                deterministic quadrature over the light) - each with NEE + MIS, per pixel within 4 sigma + 0.5%.
//   mis          NEE only, BSDF sampling only and MIS agree (8x8 block means within 4 sigma) on a glossy scene with
//                sphere, rect and emissive-triangle lights.
//   rr           Russian roulette on / off agree (block means within 4 sigma) in the Cornell box.
//   psr          mirror panel: PSR moves the G-buffer to the reflected surface (normal, instance, PSR length 1, mirror
//                tint in the albedo), radiance bit-identical with PSR on / off, demodulation exact.
//   alpha        legacy alpha: a blended unlit layer gives alpha A + (1 - alpha) B exactly; alpha test (GREATER 0.5
//                on a vertex-alpha ramp) shows the layer behind where the test fails, also for shadow rays.
//   portal       a ray portal shows exactly what a camera at its partner sees.
//   capture      the RL-1.8 capture loader on a written capture (meshes, transforms, sphere light, camera), rendered.
//   determinism  identical runs are bit-identical; CpuReference == CpuParallel.
//   zero_alloc   steady-state frames: PtCompiledScene::update (transforms, lights, cameras; light-set refit, reference
//                BVH refit) makes no heap allocation (the CPU reference render's own count is reported: the WP-6.0 CPU
//                BVH oracle allocates per ray through std::function callbacks).
//
// Usage: fuse_relight_pt_tests <suite>|all. Exit 0 pass, 1 fail.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/pt_capture_scene.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/render/material/bsdf_host.hpp>

#include "pt_reference_kernels.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

// --- allocation counter (every thread: CpuParallel runs on the job workers) --------------------------------------
namespace {
std::atomic<bool> g_count{false};
std::atomic<unsigned long long> g_allocations{0};
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (g_count.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace pt_test;
using fuse::u32;
using fuse::u64;
namespace opt = fuse::relight::options;
namespace kernel = fuse::kernel;
namespace ptk = fuse::relight::ptk;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
void check(bool condition, const std::string& message) { check(condition, message.c_str()); }

/// rtx.sceneScale belongs to scene/instances (linked in the runtime); the light set reads it by name.
struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

bool compile(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    check(ok, "compile: " + error);
    return ok;
}

bool render(const pt::PtCompiledScene& c, const pt::PtSettings& st, u32 w, u32 h, u32 spp, pt::PtReferenceImage& img,
            u32 seed = 1u, kernel::Backend backend = kernel::Backend::CpuParallel, u32 sampleBase = 0u) {
    img.resize(w, h);
    const bool ok = pt::renderReference(c, st, w, h, seed, sampleBase, spp, img, backend);
    check(ok, "renderReference");
    return ok;
}

double lum(const pt::PtReferenceImage& img, u32 x, u32 y) {
    return 0.2126 * img.mean(x, y, 0) + 0.7152 * img.mean(x, y, 1) + 0.0722 * img.mean(x, y, 2);
}

// --- ray helpers for the analytic expectations ------------------------------------------------------------------------
struct V3 {
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }

/// Primary ray direction of pixel centre (x, y) (pt_reference_core.h ptPrimaryDirection, RH camera).
V3 primaryDir(const pt::PtCamera& c, u32 w, u32 h, u32 x, u32 y) {
    const V3 f = norm({c.forward[0], c.forward[1], c.forward[2]});
    const V3 up0{c.up[0], c.up[1], c.up[2]};
    const V3 r = norm(cross(f, up0));
    const V3 u = norm(cross(r, f));
    const double ty = std::tan(0.5 * c.fovY);
    const double tx = ty * double(w) / double(h);
    const double nx = ((x + 0.5) / w) * 2.0 - 1.0;
    const double ny = 1.0 - ((y + 0.5) / h) * 2.0;
    return norm(f + r * (nx * tx) + u * (ny * ty));
}

/// BSDF of material m evaluated in world space at a surface with normal n (the core's frame: rlBasis-like).
bk::float3 evalWorld(const bk::BsdfMaterial& m, V3 n, V3 wo, V3 wi) {
    const float* lut = fuse::relight::render::material::sharedAlbedoLut().data();
    // Any orthonormal frame gives the same value for an isotropic BSDF.
    const V3 t = std::fabs(n.x) < 0.9 ? norm(cross(n, {1, 0, 0})) : norm(cross(n, {0, 1, 0}));
    const V3 b = cross(n, t);
    const bk::float3 o(float(dot(wo, t)), float(dot(wo, b)), float(dot(wo, n)));
    const bk::float3 i(float(dot(wi, t)), float(dot(wi, b)), float(dot(wi, n)));
    return bk::bsdfEval(lut, m, o, i);
}

// --- furnace ----------------------------------------------------------------------------------------------------------

void suiteFurnace() {
    struct Case {
        const char* name;
        pt::PtMaterial m;
        double tol;
    };
    pt::PtMaterial glass;
    glass.bsdf.model = bk::kBsdfModelTranslucent;
    glass.bsdf.ior = 1.5f;
    glass.bsdf.transmittance = bk::float3(1.f, 1.f, 1.f);
    const Case cases[] = {{"lambert", lambert(1.f, 1.f, 1.f, 0.8f), 0.01},
                          {"metal", metal(1.f, 1.f, 1.f, 0.5f), 0.01},
                          {"dielectric", lambert(1.f, 1.f, 1.f, 0.3f), 0.01},
                          {"glass", glass, 0.02}};
    for (const Case& cs : cases) {
        pt::PtScene s;
        s.materials = {cs.m};
        s.meshes.push_back(sphere(1.f, 24, 32, 0));
        s.instances.push_back(pt::PtInstance{});
        s.sky[0] = s.sky[1] = s.sky[2] = 1.f;
        s.camera = camera(0.f, 0.f, 4.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 40.f);
        pt::PtCompiledScene c;
        if (!compile(s, c)) {
            continue;
        }
        pt::PtSettings st;
        st.maxBounces = 48;
        st.rrStart = 6;
        pt::PtReferenceImage img;
        if (!render(c, st, 24, 24, 256, img)) {
            continue;
        }
        double sum = 0.0;
        u32 n = 0, bad = 0;
        for (u32 y = 0; y < 24; ++y) {
            for (u32 x = 0; x < 24; ++x) {
                const double m = img.mean(x, y, 1);
                const double se = std::sqrt(img.variance(x, y, 1) / img.samples(x, y));
                if (std::fabs(m - 1.0) > 4.0 * se + cs.tol) {
                    ++bad;
                }
                // Pixels whose centre ray hits the unit sphere (camera at z = 4): the sphere mean.
                const V3 d = primaryDir(s.camera, 24, 24, x, y);
                const V3 o{0, 0, 4};
                const double b = dot(o, d), c2 = dot(o, o) - 1.0;
                if (b * b - c2 > 0.02) {
                    sum += m;
                    ++n;
                }
            }
        }
        const double mean = n ? sum / n : 0.0;
        std::printf("furnace %-10s sphere pixels %u, mean %.5f, failing pixels %u\n", cs.name, n, mean, bad);
        check(n > 100u, std::string("furnace ") + cs.name + ": the sphere is in view");
        check(std::fabs(mean - 1.0) <= cs.tol, std::string("furnace ") + cs.name + ": sphere mean within tolerance");
        check(bad == 0u, std::string("furnace ") + cs.name + ": every pixel within 4 sigma + tolerance");
    }
}

// --- Lambert under a uniform sky -------------------------------------------------------------------------------------

void suiteLambertSky() {
    const float rho = 0.4f;
    pt::PtScene s;
    s.materials = {lambert(rho, rho, rho, 0.6f)};
    const float c0[3] = {0.f, 0.f, 0.f}, u[3] = {0.f, 0.f, 20.f}, v[3] = {20.f, 0.f, 0.f};
    s.meshes.push_back(quad(c0, u, v, 0));
    s.instances.push_back(pt::PtInstance{});
    const float L = 2.f;
    s.sky[0] = s.sky[1] = s.sky[2] = L;
    s.camera = camera(0.f, 4.f, 3.f, 0.f, -0.8f, -0.6f, 0.f, 1.f, 0.f, 30.f);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    pt::PtSettings st;
    st.flags &= ~pt::kPtFlagJitter; // pixel centres: the expectation is per view direction
    st.maxBounces = 4;
    pt::PtReferenceImage img;
    const u32 W = 16, H = 16;
    if (!render(c, st, W, H, 4096, img)) {
        return;
    }
    // E(mu_o): the directional albedo of the plane's material, by a stratified cosine quadrature of bsdfEval.
    u32 bad = 0, n = 0;
    double worst = 0.0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const V3 d = primaryDir(s.camera, W, H, x, y);
            const V3 wo = d * -1.0;
            const V3 nrm{0, 1, 0};
            double e0 = 0.0;
            const u32 N = 256;
            for (u32 i = 0; i < N; ++i) {
                for (u32 j = 0; j < N; ++j) {
                    const double u1 = (i + 0.5) / N, u2 = (j + 0.5) / N;
                    const double r = std::sqrt(u1), phi = 2.0 * kPi * u2;
                    const double ct = std::sqrt(std::max(0.0, 1.0 - u1));
                    const V3 wi{r * std::cos(phi), ct, r * std::sin(phi)};
                    const bk::float3 f = evalWorld(s.materials[0].bsdf, nrm, wo, wi);
                    e0 += double(f.y) / ct * kPi; // eval is projected; pdf = ct / pi
                }
            }
            e0 /= double(N) * N;
            const double expected = L * e0; // the directional albedo of the plane's BSDF (quadrature of bsdfEval)
            const double m = img.mean(x, y, 1);
            const double se = std::sqrt(img.variance(x, y, 1) / img.samples(x, y));
            const double err = std::fabs(m - expected);
            worst = std::max(worst, err / expected);
            if (err > 4.0 * se + 0.005 * expected) {
                ++bad;
                std::fprintf(stderr, "  lambert_sky pixel (%u,%u): %.6f expected %.6f (se %.6f, mu_o %.4f)\n", x, y, m,
                             expected, se, -d.y);
            }
            ++n;
        }
    }
    std::printf("lambert_sky: %u pixels, worst relative error %.4f, failing %u\n", n, worst, bad);
    check(bad == 0u, "lambert_sky: L = sky x directional albedo under the uniform sky");
}

// --- direct lighting closed forms -----------------------------------------------------------------------------------

struct DirectCase {
    const char* name;
    std::vector<lk::RlLight> lights;
    bool panel = false; ///< emissive-triangle panel
};

void suiteDirect() {
    const float rho = 0.6f;
    const V3 nrm{0, 1, 0};
    // Emitters: a delta distant light, a sphere light, a one-sided rect light, a two-sided emissive quad panel.
    const lk::float3 E(1.5f, 1.5f, 1.5f);
    std::vector<DirectCase> cases;
    cases.push_back({"distant", {rl::makeDistantLight(lk::float3(0.3f, -1.f, -0.2f), 0.f, E)}, false});
    cases.push_back({"sphere", {rl::makeSphereLight(lk::float3(0.4f, 1.2f, -0.3f), 0.3f, lk::float3(8.f, 8.f, 8.f))},
                     false});
    cases.push_back({"rect",
                     {rl::makeRectLight(lk::float3(-0.3f, 1.0f, 0.2f), lk::float3(0.4f, 0.f, 0.f),
                                        lk::float3(0.f, 0.f, 0.3f), lk::float3(6.f, 6.f, 6.f))}, // lit side -y
                     false});
    cases.push_back({"panel", {}, true});
    for (const DirectCase& dc : cases) {
        pt::PtScene s;
        s.materials = {lambert(rho, rho, rho, 0.5f), emitter(5.f, 5.f, 5.f)};
        const float c0[3] = {0.f, 0.f, 0.f}, u[3] = {0.f, 0.f, 10.f}, v[3] = {10.f, 0.f, 0.f};
        s.meshes.push_back(quad(c0, u, v, 0));
        s.instances.push_back(pt::PtInstance{});
        const float pc[3] = {0.2f, 0.9f, 0.1f}, pu[3] = {0.25f, 0.f, 0.f}, pv[3] = {0.f, 0.f, 0.35f};
        if (dc.panel) {
            s.meshes.push_back(quad(pc, pu, pv, 1));
            pt::PtInstance inst;
            inst.mesh = 1;
            s.instances.push_back(inst);
        }
        s.lights = dc.lights;
        s.camera = camera(0.f, 2.5f, 2.5f, 0.f, -0.7f, -0.7f, 0.f, 1.f, 0.f, 35.f);
        pt::PtCompiledScene c;
        if (!compile(s, c)) {
            continue;
        }
        pt::PtSettings st;
        st.flags &= ~pt::kPtFlagJitter;
        st.maxBounces = 1; // direct lighting only
        const u32 W = 12, H = 12;
        pt::PtReferenceImage img;
        if (!render(c, st, W, H, 1024, img)) {
            continue;
        }
        const lk::RlLight L = dc.panel ? lk::RlLight{} : dc.lights[0];
        u32 bad = 0, lit = 0;
        double worst = 0.0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const V3 d = primaryDir(s.camera, W, H, x, y);
                const V3 o{s.camera.origin[0], s.camera.origin[1], s.camera.origin[2]};
                const double t = -o.y / d.y;
                if (!(t > 0.0)) {
                    continue;
                }
                const V3 p = o + d * t;
                if (std::fabs(p.x) > 9.0 || std::fabs(p.z) > 9.0) {
                    continue;
                }
                if (!dc.panel && L.kind == lk::kRlKindSphere) {
                    const V3 oc = o - V3{L.position.x, L.position.y, L.position.z};
                    const double bb = dot(oc, d), cc = dot(oc, oc) - double(L.radius) * L.radius;
                    if (bb * bb - cc > -1e-3) {
                        continue; // the camera sees the light itself here
                    }
                }
                if (dc.panel) {
                    const double tp = (pc[1] - o.y) / d.y;
                    const V3 q = o + d * tp;
                    if (std::fabs(q.x - pc[0]) <= pu[0] + 0.02 && std::fabs(q.z - pc[2]) <= pv[2] + 0.02) {
                        continue; // the camera sees the panel itself here
                    }
                }
                const V3 wo = d * -1.0;
                // Expected: integral over the emitter of eval x Le (quadrature; the delta light: one eval).
                double expected = 0.0;
                auto addArea = [&](V3 center, V3 hu, V3 hv, double le, bool twoSided, bool disk) {
                    const V3 ln = norm(cross(hu, hv));
                    const double area = 4.0 * std::sqrt(dot(cross(hu, hv), cross(hu, hv)));
                    const u32 N = 256;
                    double acc = 0.0;
                    for (u32 i = 0; i < N; ++i) {
                        for (u32 j = 0; j < N; ++j) {
                            const double a = (i + 0.5) / N * 2.0 - 1.0, b = (j + 0.5) / N * 2.0 - 1.0;
                            (void)disk;
                            const V3 q = center + hu * a + hv * b;
                            const V3 dq = q - p;
                            const double d2 = dot(dq, dq);
                            const V3 wi = norm(dq);
                            const double cl = -dot(wi, ln);
                            if (!(twoSided ? std::fabs(cl) > 0.0 : cl > 0.0) || dot(wi, nrm) <= 0.0) {
                                continue;
                            }
                            const bk::float3 f = evalWorld(s.materials[0].bsdf, nrm, wo, wi);
                            acc += double(f.y) * le * std::fabs(cl) / d2;
                        }
                    }
                    return acc * area / (double(N) * N);
                };
                if (dc.panel) {
                    expected = addArea({pc[0], pc[1], pc[2]}, {pu[0], pu[1], pu[2]}, {pv[0], pv[1], pv[2]}, 5.0, true,
                                       false);
                } else if (L.kind == lk::kRlKindDistant) {
                    const V3 wi = norm(V3{-L.u.x, -L.u.y, -L.u.z});
                    expected = double(evalWorld(s.materials[0].bsdf, nrm, wo, wi).y) * double(E.y);
                } else if (L.kind == lk::kRlKindSphere) {
                    // Uniform quadrature over the subtended cone.
                    const V3 cc{L.position.x, L.position.y, L.position.z};
                    const V3 axis = norm(cc - p);
                    const double dist = std::sqrt(dot(cc - p, cc - p));
                    const double cmax = std::sqrt(std::max(0.0, 1.0 - double(L.radius) * L.radius / (dist * dist)));
                    const V3 t0 = std::fabs(axis.x) < 0.9 ? norm(cross(axis, {1, 0, 0})) : norm(cross(axis, {0, 1, 0}));
                    const V3 t1 = cross(axis, t0);
                    const u32 N = 128;
                    double acc = 0.0;
                    for (u32 i = 0; i < N; ++i) {
                        for (u32 j = 0; j < N; ++j) {
                            const double ct = 1.0 - (i + 0.5) / N * (1.0 - cmax);
                            const double st2 = std::sqrt(std::max(0.0, 1.0 - ct * ct));
                            const double phi = 2.0 * kPi * (j + 0.5) / N;
                            const V3 wi = axis * ct + t0 * (st2 * std::cos(phi)) + t1 * (st2 * std::sin(phi));
                            if (dot(wi, nrm) <= 0.0) {
                                continue;
                            }
                            acc += double(evalWorld(s.materials[0].bsdf, nrm, wo, wi).y);
                        }
                    }
                    expected = acc * double(L.radiance.y) * 2.0 * kPi * (1.0 - cmax) / (double(N) * N);
                } else {
                    expected = addArea({L.position.x, L.position.y, L.position.z}, {L.u.x, L.u.y, L.u.z},
                                       {L.v.x, L.v.y, L.v.z}, double(L.radiance.y), false, false);
                }
                const double m = img.mean(x, y, 1);
                const double se = std::sqrt(img.variance(x, y, 1) / img.samples(x, y));
                const double err = std::fabs(m - expected);
                if (expected > 1e-3) {
                    ++lit;
                    worst = std::max(worst, err / expected);
                }
                if (err > 4.0 * se + 0.005 * expected + 1e-5) {
                    ++bad;
                    if (bad <= 3u) {
                        std::fprintf(stderr, "  %s pixel (%u,%u): %.6f expected %.6f (se %.6f)\n", dc.name, x, y, m,
                                     expected, se);
                    }
                }
            }
        }
        std::printf("direct %-8s lit pixels %u, worst relative error %.4f, failing %u\n", dc.name, lit, worst, bad);
        check(lit > 20u, std::string("direct ") + dc.name + ": the light reaches the plane");
        check(bad == 0u, std::string("direct ") + dc.name + ": within 4 sigma + 0.5% of the closed form");
    }
}

// --- estimator agreement --------------------------------------------------------------------------------------------

pt::PtScene glossyScene() {
    pt::PtScene s;
    s.materials = {lambert(0.6f, 0.55f, 0.5f, 0.7f), metal(0.9f, 0.7f, 0.5f, 0.3f), emitter(4.f, 3.f, 2.f)};
    const float c0[3] = {0.f, 0.f, 0.f}, u[3] = {0.f, 0.f, 4.f}, v[3] = {4.f, 0.f, 0.f};
    s.meshes.push_back(quad(c0, u, v, 0));
    s.meshes.push_back(sphere(0.5f, 12, 16, 1));
    const float pc[3] = {-1.f, 1.5f, -0.5f}, pu[3] = {0.4f, 0.f, 0.f}, pv[3] = {0.f, -0.2f, 0.3f};
    s.meshes.push_back(quad(pc, pu, pv, 2));
    s.instances.push_back(pt::PtInstance{});
    pt::PtInstance ball;
    ball.mesh = 1;
    ball.objectToWorld = translate(0.3f, 0.5f, 0.f);
    s.instances.push_back(ball);
    pt::PtInstance panel;
    panel.mesh = 2;
    s.instances.push_back(panel);
    s.lights.push_back(rl::makeSphereLight(lk::float3(1.2f, 1.6f, 0.8f), 0.2f, lk::float3(10.f, 10.f, 12.f)));
    s.lights.push_back(rl::makeRectLight(lk::float3(0.f, 2.2f, 0.f), lk::float3(0.5f, 0.f, 0.f),
                                         lk::float3(0.f, 0.f, -0.5f), lk::float3(3.f, 3.f, 3.f)));
    s.camera = camera(0.f, 2.f, 4.f, 0.f, -0.45f, -0.9f, 0.f, 1.f, 0.f, 45.f);
    return s;
}

void suiteMis() {
    pt::PtScene s = glossyScene();
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    const u32 W = 32, H = 32;
    std::vector<double> mean[3], var[3];
    const u32 spp[3] = {512, 2048, 512};
    const u32 flags[3] = {pt::kPtFlagsDefault & ~pt::kPtFlagBsdfLights,  // NEE only
                          pt::kPtFlagsDefault & ~pt::kPtFlagNee,         // BSDF sampling only
                          pt::kPtFlagsDefault};                          // MIS
    const char* names[3] = {"nee", "bsdf", "mis"};
    for (int k = 0; k < 3; ++k) {
        pt::PtSettings st;
        st.flags = flags[k];
        st.maxBounces = 3;
        pt::PtReferenceImage img;
        if (!render(c, st, W, H, spp[k], img, 7u + u32(k))) {
            return;
        }
        referenceStats(img, mean[k], var[k]);
    }
    for (int k = 0; k < 2; ++k) {
        const BlockResult r = compareBlocks(W, H, 8, mean[k], var[k], spp[k], mean[2], var[2], spp[2], 4.0, 1e-4);
        std::printf("mis: %s vs mis: %u blocks, worst z %.2f, failing %u (means %.4f / %.4f)\n", names[k], r.blocks,
                    r.worstZ, r.failing, r.meanA, r.meanB);
        check(r.failing == 0u, std::string("mis: ") + names[k] + " agrees with MIS");
    }
    // MIS has the lowest variance of the three on the glossy sphere / light reflections (sum over the image).
    double v[3] = {0.0, 0.0, 0.0};
    for (int k = 0; k < 3; ++k) {
        for (double x : var[k]) {
            v[k] += x;
        }
    }
    std::printf("mis: summed per-sample variance nee %.3f bsdf %.3f mis %.3f\n", v[0], v[1], v[2]);
    check(v[2] < v[1], "mis: lower variance than BSDF sampling alone");
}

void suiteRr() {
    pt::PtScene s = cornell(false);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    const u32 W = 24, H = 24;
    std::vector<double> mean[2], var[2];
    for (int k = 0; k < 2; ++k) {
        pt::PtSettings st;
        st.maxBounces = 6;
        st.rrStart = 2;
        if (k == 0) {
            st.flags &= ~pt::kPtFlagRussianRoulette;
        }
        pt::PtReferenceImage img;
        if (!render(c, st, W, H, 512, img, 11u + u32(k))) {
            return;
        }
        referenceStats(img, mean[k], var[k]);
    }
    const BlockResult r = compareBlocks(W, H, 8, mean[0], var[0], 512, mean[1], var[1], 512, 4.0, 1e-4);
    std::printf("rr: %u blocks, worst z %.2f, failing %u (means %.4f / %.4f)\n", r.blocks, r.worstZ, r.failing,
                r.meanA, r.meanB);
    check(r.failing == 0u, "rr: Russian roulette is unbiased");
}

// --- PSR --------------------------------------------------------------------------------------------------------------

void suitePsr() {
    // Camera looks at a mirror (tilted 45 degrees) that reflects a red Lambertian wall lit by a distant light.
    pt::PtScene s;
    s.materials = {metal(0.9f, 0.8f, 0.7f, 0.0f), lambert(0.8f, 0.1f, 0.1f), lambert(0.5f, 0.5f, 0.5f)};
    const float mc[3] = {0.f, 0.f, 0.f}, mu[3] = {0.7071f, 0.f, 0.7071f}, mv[3] = {0.f, 1.f, 0.f};
    s.meshes.push_back(quad(mc, mu, mv, 0)); // mirror, normal (-0.707, 0, 0.707) facing +z/-x
    const float wc[3] = {-3.f, 0.f, 0.f}, wu[3] = {0.f, 0.f, 3.f}, wv[3] = {0.f, 3.f, 0.f};
    s.meshes.push_back(quad(wc, wu, wv, 1)); // red wall at x = -3, normal +x
    s.instances.push_back(pt::PtInstance{});
    pt::PtInstance wall;
    wall.mesh = 1;
    s.instances.push_back(wall);
    s.lights.push_back(rl::makeDistantLight(lk::float3(-1.f, -0.5f, -0.2f), 0.05f, lk::float3(2.f, 2.f, 2.f)));
    s.camera = camera(0.f, 0.f, 5.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 20.f);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    const u32 W = 16, H = 16;
    pt::PtSettings on;
    on.flags &= ~pt::kPtFlagJitter;
    on.maxBounces = 3;
    pt::PtSettings off = on;
    off.flags &= ~pt::kPtFlagPsr;
    pt::PtReferenceImage a, b;
    if (!render(c, on, W, H, 64, a, 3u) || !render(c, off, W, H, 64, b, 3u)) {
        return;
    }
    u32 identical = 0, mirrorPixels = 0, psrOk = 0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const auto& pa = a.pixel(x, y);
            const auto& pb = b.pixel(x, y);
            if (std::memcmp(pa.sum, pb.sum, sizeof(pa.sum)) == 0) {
                ++identical;
            }
            if (pb.instance == c.slotOf(0)) { // PSR off: the G-buffer is the mirror
                ++mirrorPixels;
                const bool wallNormal = std::fabs(pa.normal[0] - 1.f) < 1e-3f && std::fabs(pa.normal[1]) < 1e-3f;
                const bool tint = pa.albedoD[0] > 2.f * pa.albedoD[1]; // the red wall behind the mirror's tint
                if (pa.psr == 1u && pa.instance == c.slotOf(1) && wallNormal && tint) {
                    ++psrOk;
                }
            }
        }
    }
    std::printf("psr: %u/%u pixels bit-identical radiance, mirror pixels %u, replaced %u\n", identical, W * H,
                mirrorPixels, psrOk);
    check(identical == W * H, "psr: radiance unchanged by PSR (bit for bit)");
    check(mirrorPixels > 40u, "psr: the mirror covers the view");
    check(psrOk == mirrorPixels, "psr: mirror pixels carry the reflected wall's G-buffer (normal, instance, PSR 1)");
    // Demodulation is exact per sample: emissive + diffuse x albedoD + specular x albedoS == radiance.
    pt::PtReferenceImage one;
    u32 exact = 0;
    for (u32 sIdx = 0; sIdx < 8u; ++sIdx) {
        if (!render(c, on, W, H, 1, one, 5u, kernel::Backend::CpuParallel, sIdx)) {
            return;
        }
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const auto& p = one.pixel(x, y);
                bool ok = true;
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const double re = p.emissive[ch] + p.diffuse[ch] * p.albedoD[ch] + p.specular[ch] * p.albedoS[ch];
                    ok = ok && std::fabs(re - p.sum[ch]) <= 1e-5 * (1.0 + std::fabs(p.sum[ch]));
                }
                exact += ok ? 1u : 0u;
            }
        }
    }
    check(exact == 8u * W * H, "psr: remodulated demodulation equals the radiance");
}

// --- legacy alpha ------------------------------------------------------------------------------------------------------

void suiteAlpha() {
    const u32 W = 16, H = 16;
    pt::PtSettings st;
    st.flags &= ~pt::kPtFlagJitter;
    st.maxBounces = 4;
    {
        // Blend: unlit red layer with vertex alpha 0.25 over an unlit blue wall.
        pt::PtScene s;
        pt::PtMaterial front = unlit(1.f, 0.f, 0.f);
        front.flags |= pt::kPtMatAlphaBlend | pt::kPtMatVertexColor;
        s.materials = {front, unlit(0.f, 0.f, 1.f)};
        const float fc[3] = {0.f, 0.f, 0.f}, fu[3] = {3.f, 0.f, 0.f}, fv[3] = {0.f, 3.f, 0.f};
        pt::PtMesh fm = quad(fc, fu, fv, 0);
        fm.colors.assign(16, 1.f);
        for (int k = 0; k < 4; ++k) {
            fm.colors[std::size_t(k) * 4u + 3u] = 0.25f;
        }
        s.meshes.push_back(fm);
        const float bc[3] = {0.f, 0.f, -2.f};
        s.meshes.push_back(quad(bc, fu, fv, 1));
        s.instances.push_back(pt::PtInstance{});
        pt::PtInstance back;
        back.mesh = 1;
        s.instances.push_back(back);
        s.camera = camera(0.f, 0.f, 4.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 30.f);
        pt::PtCompiledScene c;
        pt::PtReferenceImage img;
        if (compile(s, c) && render(c, st, W, H, 4, img)) {
            u32 ok = 0;
            for (u32 y = 0; y < H; ++y) {
                for (u32 x = 0; x < W; ++x) {
                    ok += std::fabs(img.mean(x, y, 0) - 0.25) < 1e-5 && std::fabs(img.mean(x, y, 2) - 0.75) < 1e-5 ? 1u
                                                                                                                 : 0u;
                }
            }
            std::printf("alpha blend: %u/%u pixels = 0.25 red + 0.75 blue\n", ok, W * H);
            check(ok == W * H, "alpha: blended unlit layer = alpha A + (1 - alpha) B");
        }
    }
    {
        // Test: a red unlit ramp (vertex alpha 0 on the left, 1 on the right), GREATER 0.5, over the blue wall.
        pt::PtScene s;
        pt::PtMaterial front = unlit(1.f, 0.f, 0.f);
        front.flags |= pt::kPtMatAlphaTest | pt::kPtMatVertexColor;
        front.alphaReference = 0.5f;
        front.alphaCompare = 4u;
        s.materials = {front, unlit(0.f, 0.f, 1.f)};
        const float fc[3] = {0.f, 0.f, 0.f}, fu[3] = {3.f, 0.f, 0.f}, fv[3] = {0.f, 3.f, 0.f};
        pt::PtMesh fm = quad(fc, fu, fv, 0);
        fm.colors = {1.f, 1.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        s.meshes.push_back(fm);
        const float bc[3] = {0.f, 0.f, -2.f};
        s.meshes.push_back(quad(bc, fu, fv, 1));
        s.instances.push_back(pt::PtInstance{});
        pt::PtInstance back;
        back.mesh = 1;
        s.instances.push_back(back);
        s.camera = camera(0.f, 0.f, 4.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 30.f);
        pt::PtCompiledScene c;
        pt::PtReferenceImage img;
        if (compile(s, c) && render(c, st, W, H, 1, img)) {
            u32 left = 0, right = 0;
            for (u32 y = 0; y < H; ++y) {
                for (u32 x = 0; x < W; ++x) {
                    if (x < W / 2 - 1) {
                        left += std::fabs(img.mean(x, y, 2) - 1.0) < 1e-6 && img.mean(x, y, 0) == 0.0 ? 1u : 0u;
                    } else if (x > W / 2) {
                        const bool kept = std::fabs(img.mean(x, y, 0) - 1.0) < 1e-6 && img.mean(x, y, 2) == 0.0;
                        right += kept ? 1u : 0u;
                        if (!kept) {
                            std::printf("  alpha test pixel (%u,%u): %g %g %g\n", x, y, img.mean(x, y, 0),
                                        img.mean(x, y, 1), img.mean(x, y, 2));
                        }
                    }
                }
            }
            std::printf("alpha test: left (cut) %u, right (kept) %u\n", left, right);
            check(left == (W / 2 - 1) * H, "alpha: failing texels show the layer behind");
            check(right == (W / 2 - 1) * H, "alpha: passing texels keep the layer");
        }
    }
    {
        // Shadow rays: a lit floor under an alpha-tested ramp: the cut half casts no shadow.
        pt::PtScene s;
        pt::PtMaterial cut = lambert(0.5f, 0.5f, 0.5f);
        cut.flags |= pt::kPtMatAlphaTest | pt::kPtMatVertexColor;
        cut.alphaReference = 0.5f;
        cut.alphaCompare = 4u;
        s.materials = {cut, lambert(0.5f, 0.5f, 0.5f)};
        const float fc[3] = {0.f, 1.f, 0.f}, fu[3] = {2.f, 0.f, 0.f}, fv[3] = {0.f, 0.f, -2.f};
        pt::PtMesh fm = quad(fc, fu, fv, 0);
        fm.colors = {1.f, 1.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f};
        s.meshes.push_back(fm);
        const float gc[3] = {0.f, 0.f, 0.f}, gu[3] = {0.f, 0.f, 3.f}, gv[3] = {3.f, 0.f, 0.f};
        s.meshes.push_back(quad(gc, gu, gv, 1));
        pt::PtInstance ramp;
        ramp.flags = pt::kPtInstanceShadow;
        s.instances.push_back(ramp);
        pt::PtInstance floor;
        floor.mesh = 1;
        s.instances.push_back(floor);
        s.lights.push_back(rl::makeDistantLight(lk::float3(0.f, -1.f, 0.f), 0.f, lk::float3(1.f, 1.f, 1.f)));
        s.camera = camera(0.f, 5.f, 0.001f, 0.f, -1.f, 0.f, 0.f, 0.f, -1.f, 30.f);
        pt::PtCompiledScene c;
        pt::PtReferenceImage img;
        pt::PtSettings one = st;
        one.maxBounces = 1;
        if (compile(s, c) && render(c, one, W, H, 1, img)) {
            // Floor texel lit (x < -0.3) vs shadowed (x > 0.3) under the ramp (x in [-2, 2]).
            u32 lit = 0, dark = 0, total = 0;
            for (u32 y = 4; y < H - 4; ++y) {
                for (u32 x = 0; x < W; ++x) {
                    const V3 d = primaryDir(s.camera, W, H, x, y);
                    const V3 o{0, 5, 0.001};
                    const V3 p = o + d * (-5.0 / d.y);
                    if (std::fabs(p.x) < 0.3 || std::fabs(p.x) > 1.8 || std::fabs(p.z) > 1.8) {
                        continue;
                    }
                    ++total;
                    const double m = img.mean(x, y, 1);
                    if (p.x < 0.0 && m > 0.05) {
                        ++lit;
                    }
                    if (p.x > 0.0 && m == 0.0) {
                        ++dark;
                    }
                }
            }
            std::printf("alpha shadows: %u floor pixels, lit through the cut %u, shadowed %u\n", total, lit, dark);
            check(total > 20u && lit + dark == total, "alpha: shadow rays skip alpha-tested texels");
        }
    }
}

// --- portals -----------------------------------------------------------------------------------------------------------

void suitePortal() {
    const u32 W = 16, H = 16;
    pt::PtSettings st;
    st.flags &= ~pt::kPtFlagJitter;
    st.maxBounces = 4;
    auto build = [&](bool withPortal, float camShift) {
        pt::PtScene s;
        pt::PtMaterial portal;
        portal.bsdf.model = bk::kBsdfModelPortal;
        portal.portal = 0;
        s.materials = {portal, unlit(0.1f, 0.8f, 0.2f), unlit(0.3f, 0.3f, 0.3f), unlit(0.9f, 0.6f, 0.1f)};
        const float u[3] = {1.f, 0.f, 0.f}, v[3] = {0.f, 1.f, 0.f};
        auto add = [&](const float c[3], const float* hu, const float* hv, u32 m) {
            pt::PtInstance inst;
            inst.mesh = static_cast<u32>(s.meshes.size());
            s.meshes.push_back(quad(c, hu, hv, m));
            s.instances.push_back(inst);
        };
        if (withPortal) {
            const float c[3] = {0.f, 0.f, 0.f};
            add(c, u, v, 0); // portal A at z = 0
        }
        const float bigU[3] = {6.f, 0.f, 0.f}, bigV[3] = {0.f, 6.f, 0.f};
        const float back[3] = {0.f, 0.f, -4.f};
        add(back, bigU, bigV, 2); // grey backdrop behind A
        // Behind the exit at x = 100: a green panel with an orange square on it.
        const float g[3] = {100.f, 0.f, -3.f};
        add(g, bigU, bigV, 1);
        const float o[3] = {100.3f, 0.2f, -2.9f}, ou[3] = {0.3f, 0.f, 0.f}, ov[3] = {0.f, 0.3f, 0.f};
        add(o, ou, ov, 3);
        s.portals.push_back(translate(100.f, 0.f, 0.f));
        s.camera = camera(camShift, 0.f, 4.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 30.f);
        return s;
    };
    pt::PtCompiledScene a, b;
    pt::PtReferenceImage ia, ib;
    if (!compile(build(true, 0.f), a) || !compile(build(false, 100.f), b) || !render(a, st, W, H, 1, ia) ||
        !render(b, st, W, H, 1, ib)) {
        return;
    }
    // Pixels whose primary ray crosses A (|x|, |y| < 1 at z = 0) see what the shifted camera sees.
    const pt::PtCamera cam = build(true, 0.f).camera;
    u32 through = 0, equal = 0, portalHits = 0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const V3 d = primaryDir(cam, W, H, x, y);
            const double t = -4.0 / d.z;
            const double px = d.x * t, py = d.y * t;
            if (std::fabs(px) > 0.95 || std::fabs(py) > 0.95) {
                continue;
            }
            ++through;
            bool same = true;
            for (u32 ch = 0; ch < 3u; ++ch) {
                same = same && std::fabs(ia.mean(x, y, ch) - ib.mean(x, y, ch)) < 1e-5;
            }
            equal += same ? 1u : 0u;
            portalHits += ia.pixel(x, y).psr == 1u ? 1u : 0u;
        }
    }
    std::printf("portal: %u pixels through the portal, %u equal to the partner view, %u with PSR 1\n", through, equal,
                portalHits);
    check(through > 20u && equal == through, "portal: the portal shows its partner's view");
    check(portalHits == through, "portal: the G-buffer is the surface behind the exit (PSR through the portal)");
}

// --- capture loader ----------------------------------------------------------------------------------------------------

void suiteCapture() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "fuse_rl_pt_capture";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "lights", ec);
    {
        std::ofstream f(dir / "lights" / "light_1.usda");
        f << "#usda 1.0\n(\n    defaultPrim = \"light_1\"\n)\n\n"
             "def SphereLight \"light_1\"\n{\n"
             "    color3f inputs:color = (1, 0.5, 0.25)\n    float inputs:intensity = 4\n"
             "    float inputs:radius = 0.25\n"
             "    matrix4d xformOp:transform.timeSamples = {\n        0: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), "
             "(0, 2, 0, 1) ),\n    }\n    uniform token[] xformOpOrder = [\"xformOp:transform\"]\n}\n";
    }
    {
        std::ofstream f(dir / "capture_test.usda");
        f << "#usda 1.0\n(\n    defaultPrim = \"RootNode\"\n    upAxis = \"Y\"\n)\n\n"
             "def \"RootNode\"\n{\n"
             "    def Xform \"lights\"\n    {\n"
             "        def SphereLight \"light_1\" (\n            prepend references = @./lights/light_1.usda@\n"
             "        )\n        {\n        }\n    }\n"
             "    def Xform \"instances\"\n    {\n"
             "        def Xform \"inst_0\"\n        {\n"
             "            matrix4d xformOp:transform.timeSamples = {\n"
             "                0: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, -1, 1) ),\n"
             "                1: ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 0, -1, 1) ),\n            }\n"
             "            uniform token[] xformOpOrder = [\"xformOp:transform\"]\n"
             "            def Mesh \"mesh\"\n            {\n"
             "                int[] faceVertexCounts = [4]\n                int[] faceVertexIndices = [0, 1, 2, 3]\n"
             "                point3f[] points = [(-2, 0, -2), (-2, 0, 2), (2, 0, 2), (2, 0, -2)]\n"
             "                normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (\n"
             "                    interpolation = \"vertex\"\n                )\n"
             "            }\n        }\n"
             "        def Xform \"inst_hidden\"\n        {\n            token visibility = \"invisible\"\n"
             "            def Mesh \"mesh\"\n            {\n"
             "                int[] faceVertexCounts = [3]\n                int[] faceVertexIndices = [0, 1, 2]\n"
             "                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n            }\n        }\n"
             "    }\n"
             "    def Xform \"cameras\"\n    {\n        def Camera \"Camera\"\n        {\n"
             "            float focalLength = 13.610671\n            float horizontalAperture = 20.955\n"
             "            matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 0.8, -0.6, 0), (0, 0.6, 0.8, 0), "
             "(0, 3, 4, 1) )\n"
             "            uniform token[] xformOpOrder = [\"xformOp:transform\"]\n        }\n    }\n}\n";
    }
    pt::PtScene s;
    pt::PtCaptureReport rep;
    std::string error;
    pt::PtCaptureOptions options;
    const bool ok = pt::loadCaptureScene((dir / "capture_test.usda").string(), options, s, &rep, &error);
    check(ok, "capture: load: " + error);
    if (!ok) {
        return;
    }
    std::printf("capture: meshes %u, triangles %u, lights %u, camera %d, warnings %zu\n", rep.meshes, rep.triangles,
                rep.lights, rep.camera ? 1 : 0, rep.warnings.size());
    for (const std::string& w : rep.warnings) {
        std::printf("  warning: %s\n", w.c_str());
    }
    check(rep.meshes == 1u && rep.triangles == 2u, "capture: the visible mesh only (fan-triangulated)");
    check(s.instances.size() == 1u && std::fabs(s.instances[0].objectToWorld[11] + 1.f) < 1e-6f &&
              std::fabs(s.instances[0].objectToWorld[3]) < 1e-6f,
          "capture: the earliest transform sample");
    check(rep.lights == 1u && s.lights.size() == 1u && s.lights[0].kind == lk::kRlKindSphere &&
              std::fabs(s.lights[0].position.y - 2.f) < 1e-5f && s.lights[0].radiance.x > s.lights[0].radiance.y,
          "capture: the sphere light (position, colour)");
    check(rep.camera && std::fabs(s.camera.origin[1] - 3.f) < 1e-5f && std::fabs(s.camera.forward[1] + 0.6f) < 1e-5f &&
              std::fabs(s.camera.forward[2] + 0.8f) < 1e-5f,
          "capture: the camera (origin, -Z forward)");
    const float expectedFov = 2.f * std::atan((20.955f / options.aspect) / (2.f * 13.610671f));
    check(std::fabs(s.camera.fovY - expectedFov) < 1e-5f, "capture: the vertical field of view");
    pt::PtCompiledScene c;
    pt::PtReferenceImage img;
    if (compile(s, c) && render(c, pt::PtSettings{}, 16, 12, 16, img)) {
        u32 lit = 0;
        for (u32 y = 0; y < 12; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                lit += lum(img, x, y) > 0.01 ? 1u : 0u;
            }
        }
        std::printf("capture: %u lit pixels\n", lit);
        check(lit > 20u, "capture: the captured plane is lit by the captured light");
    }
    fs::remove_all(dir, ec);
}

// --- determinism / zero allocation -------------------------------------------------------------------------------------

void suiteDeterminism() {
    pt::PtScene s = cornell(true);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    pt::PtSettings st;
    pt::PtReferenceImage a, b, r;
    if (!render(c, st, 24, 24, 8, a, 5u) || !render(c, st, 24, 24, 8, b, 5u) ||
        !render(c, st, 24, 24, 8, r, 5u, kernel::Backend::CpuReference)) {
        return;
    }
    u32 same = 0, sameRef = 0, nonzero = 0;
    for (u32 y = 0; y < 24; ++y) {
        for (u32 x = 0; x < 24; ++x) {
            same += std::memcmp(&a.pixel(x, y), &b.pixel(x, y), sizeof(ptk::PtReferencePixel)) == 0 ? 1u : 0u;
            sameRef += std::memcmp(&a.pixel(x, y), &r.pixel(x, y), sizeof(ptk::PtReferencePixel)) == 0 ? 1u : 0u;
            nonzero += a.pixel(x, y).sum[0] > 0.0 ? 1u : 0u;
        }
    }
    std::printf("determinism: repeat %u/576, CpuReference == CpuParallel %u/576, lit %u\n", same, sameRef, nonzero);
    check(same == 576u, "determinism: identical runs are bit-identical");
    check(sameRef == 576u, "determinism: CpuReference == CpuParallel");
    check(nonzero > 300u, "determinism: the box is lit");
}

unsigned long long g_updateAllocations = 0;
unsigned long long g_renderAllocations = 0;

void suiteZeroAlloc() {
    pt::PtScene s = cornell(true);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    pt::PtSettings st;
    pt::PtReferenceImage img;
    img.resize(16, 16);
    auto frame = [&](u32 f) {
        s.instances[6].objectToWorld[3] = -1.1f + 0.05f * float(f % 5u); // the metal sphere moves
        s.lights[0].position.x = 0.9f - 0.02f * float(f % 3u);
        s.hasPrevCamera = true;
        s.prevCamera = s.camera;
        const unsigned long long a0 = g_allocations.load();
        const bool updated = c.update(s);
        const unsigned long long a1 = g_allocations.load();
        img.clear();
        const bool rendered = pt::renderReference(c, st, 16, 16, f, 0u, 1u, img, kernel::Backend::CpuParallel);
        const unsigned long long a2 = g_allocations.load();
        g_updateAllocations += a1 - a0;
        g_renderAllocations += a2 - a1;
        return updated && rendered;
    };
    bool ok = true;
    for (u32 f = 0; f < 4u; ++f) {
        ok = ok && frame(f);
    }
    g_allocations.store(0);
    g_updateAllocations = 0;
    g_renderAllocations = 0;
    g_count.store(true);
    for (u32 f = 4; f < 20u; ++f) {
        ok = ok && frame(f);
    }
    g_count.store(false);
    std::printf("zero_alloc: 16 steady-state frames: PtCompiledScene::update %llu operator-new calls; reference render "
                "%llu (informational: the WP-6.0 CPU BVH oracle passes its per-ray callbacks as std::function)\n",
                g_updateAllocations, g_renderAllocations);
    check(ok, "zero_alloc: frames succeed");
    check(g_updateAllocations == 0u, "zero_alloc: no heap allocation in steady-state scene updates");
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"furnace", suiteFurnace}, {"lambert_sky", suiteLambertSky}, {"direct", suiteDirect},
                            {"mis", suiteMis},         {"rr", suiteRr},                  {"psr", suitePsr},
                            {"alpha", suiteAlpha},     {"portal", suitePortal},          {"capture", suiteCapture},
                            {"determinism", suiteDeterminism}, {"zero_alloc", suiteZeroAlloc}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            s.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_pt %s\n", suite.c_str());
    return 0;
}
