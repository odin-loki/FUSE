#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_effects_job.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/cuda_jobs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/ssfx/hbao.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi.hpp>
#include <fuse/ssfx/ssr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::math::Vec3;
namespace ssfx = fuse::ssfx;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

fuse::compute::SSAOParams makeSsaoParams() {
    fuse::compute::SSAOParams params{};
    params.width = 16;
    params.height = 16;
    params.strength = 1.5f;
    params.directions = 8;
    params.steps_per_dir = 4;
    params.enable_blur = true;
    params.blur_depth_threshold = 0.001f;
    params.blur_normal_threshold = 0.95f;
    params.contact_depth_scale = 0.05f;
    params.contact_normal_power = 2.f;
    return params;
}

fuse::compute::SSRParams makeSsrParams() {
    fuse::compute::SSRParams params{};
    params.width = 16;
    params.height = 16;
    params.max_steps = 64;
    params.use_hiz = true;
    params.contact_hardening = true;
    params.contact_distance = 0.5f;
    params.contact_roughness_floor = 0.02f;
    params.contact_harden_exponent = 2.f;
    return params;
}

fuse::compute::SSGIParams makeSsgiParams() {
    fuse::compute::SSGIParams params{};
    params.width = 16;
    params.height = 16;
    params.max_bounces = 2;
    params.intensity = 1.f;
    return params;
}

// ---------------------------------------------------------------------------------------------------------
// Analytic scenes (world space, +Y up) rasterised into host G-buffers in the Compute surface layout:
// linear view depth + engine view-space normals (+Y up, -Z forward).
// ---------------------------------------------------------------------------------------------------------

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kFovY = 60.f * kPi / 180.f;
constexpr u32 kWidth = 160;
constexpr u32 kHeight = 120;

struct WorldCamera {
    Vec3 position{};
    Vec3 right{1.f, 0.f, 0.f};
    Vec3 down{0.f, -1.f, 0.f};
    Vec3 forward{0.f, 0.f, 1.f};

    static WorldCamera pitchedDown(const Vec3& position, f32 pitch) {
        WorldCamera c{};
        c.position = position;
        c.forward = Vec3{0.f, -std::sin(pitch), std::cos(pitch)};
        c.down = Vec3{0.f, -std::cos(pitch), -std::sin(pitch)};
        return c;
    }
    /// ssfx view space (+Y down, +Z forward) <-> world.
    Vec3 viewToWorldDir(const Vec3& v) const { return right * v.x + down * v.y + forward * v.z; }
    Vec3 worldToViewDir(const Vec3& w) const { return Vec3{w.dot(right), w.dot(down), w.dot(forward)}; }
    /// Engine view space (+Y up, -Z forward).
    Vec3 worldToEngineViewDir(const Vec3& w) const { return Vec3{w.dot(right), -w.dot(down), -w.dot(forward)}; }
};

struct Hit {
    bool hit = false;
    f32 t = 0.f;
    Vec3 normal{};
    Vec3 color{};
    int surface = -1;
};

/// Axis-aligned rectangle in the plane `axis == value`, bounded on the other two axes; two-sided.
struct Rect {
    int axis = 1;
    f32 value = 0.f;
    f32 lo[3]{-1e30f, -1e30f, -1e30f};
    f32 hi[3]{1e30f, 1e30f, 1e30f};
    Vec3 color{};
};

void intersectRect(const Vec3& o, const Vec3& d, const Rect& r, int id, Hit& best) {
    const f32 os[3] = {o.x, o.y, o.z};
    const f32 ds[3] = {d.x, d.y, d.z};
    if (std::fabs(ds[r.axis]) < 1e-9f) {
        return;
    }
    const f32 t = (r.value - os[r.axis]) / ds[r.axis];
    if (t <= 1e-4f || (best.hit && t >= best.t)) {
        return;
    }
    for (int a = 0; a < 3; ++a) {
        const f32 c = os[a] + ds[a] * t;
        if (a != r.axis && (c < r.lo[a] || c > r.hi[a])) {
            return;
        }
    }
    const f32 sign = ds[r.axis] < 0.f ? 1.f : -1.f;
    best.hit = true;
    best.t = t;
    best.normal = Vec3{r.axis == 0 ? sign : 0.f, r.axis == 1 ? sign : 0.f, r.axis == 2 ? sign : 0.f};
    best.color = r.color;
    best.surface = id;
}

struct Scene {
    std::vector<Rect> rects;
    Hit trace(const Vec3& o, const Vec3& d) const {
        Hit best{};
        for (size_t i = 0; i < rects.size(); ++i) {
            intersectRect(o, d, rects[i], static_cast<int>(i), best);
        }
        return best;
    }
};

struct GBuffer {
    ssfx::SsfxCamera camera{};
    WorldCamera world{};
    std::vector<f32> depth;
    std::vector<Vec3> engineNormals;
    std::vector<Vec3> color;
    std::vector<int> surface;
    std::vector<Vec3> worldPos;
    f32 proj[16]{};

    u32 index(u32 x, u32 y) const { return y * camera.width + x; }
};

void fillProjection(f32 (&out)[16]) {
    const fuse::math::Mat4 proj = fuse::math::perspective(kFovY * 180.f / kPi,
                                                          static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                                          200.f);
    for (int i = 0; i < 16; ++i) {
        out[i] = proj.data[static_cast<size_t>(i)];
    }
}

GBuffer rasterize(const Scene& scene, const WorldCamera& world) {
    GBuffer g{};
    g.world = world;
    fillProjection(g.proj);
    // Intrinsics as documented for the Compute surfaces (column-major perspective, rows grow downwards), so the
    // direct ssfx reference sees bit-identical inputs.
    g.camera.width = kWidth;
    g.camera.height = kHeight;
    g.camera.fx = 0.5f * static_cast<f32>(kWidth) * g.proj[0];
    g.camera.fy = 0.5f * static_cast<f32>(kHeight) * g.proj[5];
    g.camera.cx = 0.5f * static_cast<f32>(kWidth) * (1.f - g.proj[8]);
    g.camera.cy = 0.5f * static_cast<f32>(kHeight) * (1.f + g.proj[9]);
    g.camera.near_z = g.proj[14] / g.proj[10];
    const u32 count = kWidth * kHeight;
    g.depth.assign(count, 0.f);
    g.engineNormals.assign(count, Vec3{});
    g.color.assign(count, Vec3{});
    g.surface.assign(count, -1);
    g.worldPos.assign(count, Vec3{});
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            // View ray with z == 1: the hit parameter is the linear view depth.
            const Vec3 viewDir = g.camera.rayDirection(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f);
            const Hit h = scene.trace(world.position, world.viewToWorldDir(viewDir));
            const u32 i = g.index(x, y);
            if (h.hit) {
                g.depth[i] = h.t;
                g.engineNormals[i] = world.worldToEngineViewDir(h.normal);
                g.color[i] = h.color;
                g.surface[i] = h.surface;
                g.worldPos[i] = world.position + world.viewToWorldDir(viewDir) * h.t;
            }
        }
    }
    return g;
}

/// The same G-buffer seen directly through the shared ssfx reference (ssfx normals = +Y down, +Z forward).
struct SsfxViewStorage {
    std::vector<Vec3> normals;
    ssfx::SsfxGBufferView view{};
};

SsfxViewStorage ssfxView(const GBuffer& g) {
    SsfxViewStorage s{};
    s.normals.resize(g.engineNormals.size());
    for (size_t i = 0; i < s.normals.size(); ++i) {
        s.normals[i] = Vec3{g.engineNormals[i].x, -g.engineNormals[i].y, -g.engineNormals[i].z};
    }
    s.view.camera = g.camera;
    s.view.depth = g.depth.data();
    s.view.normals = s.normals.data();
    return s;
}

void tangentBasis(const Vec3& n, Vec3& t, Vec3& b) {
    const Vec3 helper = std::fabs(n.x) < 0.9f ? Vec3{1.f, 0.f, 0.f} : Vec3{0.f, 1.f, 0.f};
    t = fuse::math::cross(helper, n).normalized();
    b = fuse::math::cross(n, t);
}

/// Cosine-weighted visibility against the true geometry within `radius` (stratified 48x48 rays).
f32 trueGeometryVisibility(const Scene& scene, const Vec3& p, const Vec3& n, f32 radius) {
    Vec3 t{};
    Vec3 b{};
    tangentBasis(n, t, b);
    const u32 k = 48;
    u32 visible = 0;
    for (u32 i = 0; i < k; ++i) {
        for (u32 j = 0; j < k; ++j) {
            const f32 u1 = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(k);
            const f32 u2 = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(k);
            const f32 r = std::sqrt(u1);
            const f32 phi = 2.f * kPi * u2;
            const Vec3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(1.f - u1);
            const Hit h = scene.trace(p + n * 1e-4f, dir);
            if (!h.hit || h.t > radius) {
                ++visible;
            }
        }
    }
    return static_cast<f32>(visible) / static_cast<f32>(k * k);
}

f32 luminance(const Vec3& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// ---------------------------------------------------------------------------------------------------------
// SSAO — routed to the shared HBAO reference; 90 degree corner vs true-geometry cosine AO
// ---------------------------------------------------------------------------------------------------------

constexpr f32 kStepZ = 3.f;
constexpr f32 kStepHeight = 1.6f;
enum StepSurface { kFloor = 0, kRiser = 1, kTop = 2 };

Scene stepScene() {
    Scene s{};
    Rect floor{};
    floor.axis = 1;
    floor.value = 0.f;
    floor.hi[2] = kStepZ;
    floor.color = Vec3{0.5f, 0.5f, 0.5f};
    Rect riser{};
    riser.axis = 2;
    riser.value = kStepZ;
    riser.lo[1] = 0.f;
    riser.hi[1] = kStepHeight;
    Rect top{};
    top.axis = 1;
    top.value = kStepHeight;
    top.lo[2] = kStepZ;
    s.rects = {floor, riser, top};
    return s;
}

fuse::compute::SSAOParams ssaoForGBuffer(const GBuffer& g, std::vector<f32>& out) {
    fuse::compute::SSAOParams params = makeSsaoParams();
    params.width = kWidth;
    params.height = kHeight;
    for (int i = 0; i < 16; ++i) {
        params.proj[i] = g.proj[i];
    }
    params.depth_surface = const_cast<f32*>(g.depth.data());
    params.normal_surface = const_cast<Vec3*>(g.engineNormals.data());
    out.assign(kWidth * kHeight, -1.f);
    params.ao_out_surface = out.data();
    params.radius = 1.f;
    params.bias = 0.02f;
    params.directions = 16;
    params.steps_per_dir = 32;
    params.max_radius_px = 128.f;
    params.strength = 1.f;
    params.enable_blur = false;
    return params;
}

void testSsaoCornerMatchesTrueGeometry() {
    const Scene scene = stepScene();
    const WorldCamera world = WorldCamera::pitchedDown(Vec3{0.f, 2.2f, 0.f}, 30.f * kPi / 180.f);
    const GBuffer g = rasterize(scene, world);

    // math::perspective intrinsics == the pinhole camera with the same vertical FOV.
    const ssfx::SsfxCamera fovCamera = ssfx::SsfxCamera::fromVerticalFov(kWidth, kHeight, kFovY);
    expectNear(g.camera.fx / fovCamera.fx, 1.f, 1e-5f, "projection fx matches the vertical FOV");
    expectNear(g.camera.fy / fovCamera.fy, 1.f, 1e-5f, "projection fy matches the vertical FOV");
    expectNear(g.camera.cx, fovCamera.cx, 1e-4f, "projection principal point x is centred");
    expectNear(g.camera.cy, fovCamera.cy, 1e-4f, "projection principal point y is centred");
    expectNear(g.camera.near_z, 0.05f, 1e-5f, "projection near plane recovered");

    std::vector<f32> ao;
    fuse::compute::SSAOParams params = ssaoForGBuffer(g, ao);
    expectTrue(fuse::compute::launch_ssao_cpu(params), "SSAO CPU pass on the step scene succeeds");

    // Routing: identical to the shared HBAO reference on the ssfx-convention view (projection -> intrinsics and
    // engine -> ssfx normal conversion are exact).
    const SsfxViewStorage sv = ssfxView(g);
    ssfx::HbaoParams hbao{};
    hbao.radius = params.radius;
    hbao.bias = params.bias;
    hbao.directions = params.directions;
    hbao.steps_per_dir = params.steps_per_dir;
    hbao.max_radius_px = params.max_radius_px;
    hbao.strength = 1.f;
    std::vector<f32> reference(kWidth * kHeight);
    ssfx::computeHbaoCpu(sv.view, hbao, reference.data());
    f32 maxRouteDiff = 0.f;
    for (size_t i = 0; i < ao.size(); ++i) {
        maxRouteDiff = std::max(maxRouteDiff, std::fabs(ao[i] - reference[i]));
    }

    // Physical check: floor pixels approaching the concave corner vs true-geometry cosine-weighted AO.
    const u32 column = kWidth / 2;
    f32 worstVsTrue = 0.f;
    f32 closestDist = 1e9f;
    f32 closestVis = 1.f;
    u32 samples = 0;
    f32 prevVis = -1.f;
    bool monotonic = true;
    for (u32 y = 0; y < kHeight; ++y) {
        const u32 i = g.index(column, y);
        if (g.surface[i] != kFloor) {
            continue;
        }
        const f32 distCorner = kStepZ - g.worldPos[i].z;
        if (distCorner > 1.2f) {
            continue;
        }
        const f32 truth = trueGeometryVisibility(scene, g.worldPos[i], Vec3{0.f, 1.f, 0.f}, params.radius);
        worstVsTrue = std::max(worstVsTrue, std::fabs(ao[i] - truth));
        ++samples;
        if (distCorner < closestDist) {
            closestDist = distCorner;
            closestVis = ao[i];
        }
        // Rows run from the corner towards the camera: visibility rises with distance from the corner.
        if (prevVis >= 0.f && ao[i] < prevVis - 0.005f) {
            monotonic = false;
        }
        prevVis = ao[i];
    }
    std::printf("[compute] SSAO step scene: route |compute - ssfx| max %.2e; %u floor samples, closest %.3f m from "
                "corner -> %.3f (analytic 0.5); max |SSAO - true-geometry AO| %.3f\n",
                maxRouteDiff, samples, closestDist, closestVis, worstVsTrue);
    expectTrue(maxRouteDiff <= 1e-6f, "launch_ssao_cpu equals the shared HBAO reference");
    expectTrue(samples >= 8u, "SSAO corner walk has enough floor samples");
    expectNear(closestVis, 0.5f, 0.08f, "SSAO ~0.5 visibility at the 90 degree corner line");
    expectTrue(worstVsTrue < 0.08f, "SSAO within 0.08 of true-geometry cosine AO near the corner");
    expectTrue(monotonic, "SSAO occlusion decays monotonically away from the corner");

    // Centre sample equals the unblurred full-frame value; strength is the visibility exponent.
    const u32 centre = g.index(kWidth / 2, kHeight / 2);
    expectNear(fuse::compute::ssao_center_sample(params), ao[centre], 1e-6f, "SSAO centre sample == frame value");
    fuse::compute::SSAOParams strong = params;
    strong.strength = 1.5f;
    expectNear(fuse::compute::ssao_center_sample(strong), std::pow(ao[centre], 1.5f), 1e-5f,
               "SSAO strength is the visibility exponent");

    // Depth-reconstructed normals (no normal surface) keep the corner behaviour.
    std::vector<f32> aoRecon;
    fuse::compute::SSAOParams recon = ssaoForGBuffer(g, aoRecon);
    recon.normal_surface = nullptr;
    expectTrue(fuse::compute::launch_ssao_cpu(recon), "SSAO with reconstructed normals succeeds");
    f32 worstRecon = 0.f;
    for (u32 y = 0; y < kHeight; ++y) {
        const u32 i = g.index(column, y);
        if (g.surface[i] == kFloor && kStepZ - g.worldPos[i].z <= 1.2f && kStepZ - g.worldPos[i].z > 0.1f) {
            worstRecon = std::max(worstRecon, std::fabs(aoRecon[i] - ao[i]));
        }
    }
    std::printf("[compute] SSAO reconstructed normals: max |recon - gbuffer normals| %.3f\n", worstRecon);
    expectTrue(worstRecon < 0.05f, "SSAO with depth-reconstructed normals tracks G-buffer normals");

    // Cross-bilateral blur: preserves the physically based result (coplanar taps only).
    std::vector<f32> aoBlur;
    fuse::compute::SSAOParams blurred = ssaoForGBuffer(g, aoBlur);
    blurred.enable_blur = true;
    expectTrue(fuse::compute::launch_ssao_cpu(blurred), "SSAO with blur succeeds");
    f32 worstBlurVsTrue = 0.f;
    for (u32 y = 0; y < kHeight; ++y) {
        const u32 i = g.index(column, y);
        if (g.surface[i] != kFloor || kStepZ - g.worldPos[i].z > 1.2f) {
            continue;
        }
        const f32 truth = trueGeometryVisibility(scene, g.worldPos[i], Vec3{0.f, 1.f, 0.f}, params.radius);
        worstBlurVsTrue = std::max(worstBlurVsTrue, std::fabs(aoBlur[i] - truth));
    }
    std::printf("[compute] SSAO blurred: max |SSAO - true-geometry AO| %.3f\n", worstBlurVsTrue);
    expectTrue(worstBlurVsTrue < 0.08f, "blurred SSAO stays within 0.08 of true-geometry AO");

    // Flat plane: unoccluded everywhere, blurred or not.
    Scene flat{};
    Rect floorOnly{};
    floorOnly.axis = 1;
    flat.rects = {floorOnly};
    const GBuffer gf = rasterize(flat, world);
    std::vector<f32> aoFlat;
    fuse::compute::SSAOParams flatParams = ssaoForGBuffer(gf, aoFlat);
    flatParams.enable_blur = true;
    expectTrue(fuse::compute::launch_ssao_cpu(flatParams), "SSAO flat plane succeeds");
    f32 minFlat = 1.f;
    for (size_t i = 0; i < aoFlat.size(); ++i) {
        minFlat = std::min(minFlat, aoFlat[i]);
    }
    std::printf("[compute] SSAO flat plane: min visibility %.4f\n", minFlat);
    expectTrue(minFlat > 0.99f, "SSAO flat plane is unoccluded");
}

// ---------------------------------------------------------------------------------------------------------
// SSR — routed to the shared SSR reference; mirror floor vs analytic reflection
// ---------------------------------------------------------------------------------------------------------

constexpr f32 kWallZ = 12.f;

Scene ssrScene(bool includeFloor) {
    Scene s{};
    // Wall made of coloured vertical bands so a wrong hit position shows up as a wrong colour.
    const Vec3 bands[4] = {{0.9f, 0.2f, 0.1f}, {0.1f, 0.8f, 0.2f}, {0.2f, 0.3f, 0.9f}, {0.9f, 0.9f, 0.2f}};
    for (int b = 0; b < 8; ++b) {
        Rect wall{};
        wall.axis = 2;
        wall.value = kWallZ;
        wall.lo[0] = -8.f + 2.f * static_cast<f32>(b);
        wall.hi[0] = wall.lo[0] + 2.f;
        wall.lo[1] = 0.f;
        wall.color = bands[b % 4];
        s.rects.push_back(wall);
    }
    if (includeFloor) {
        Rect floor{};
        floor.axis = 1;
        floor.hi[2] = kWallZ;
        floor.color = Vec3{0.3f, 0.3f, 0.3f};
        s.rects.push_back(floor);
    }
    return s;
}

void testSsrMirrorFloor() {
    const Scene scene = ssrScene(true);
    const Scene reflected = ssrScene(false);
    const int floorId = static_cast<int>(scene.rects.size()) - 1;
    const WorldCamera world = WorldCamera::pitchedDown(Vec3{0.f, 1.5f, 0.f}, 10.f * kPi / 180.f);
    const GBuffer g = rasterize(scene, world);

    std::vector<fuse::math::Vec4> out(kWidth * kHeight);
    fuse::compute::SSRParams params = makeSsrParams();
    params.width = kWidth;
    params.height = kHeight;
    for (int i = 0; i < 16; ++i) {
        params.proj[i] = g.proj[i];
    }
    params.depth_surface = const_cast<f32*>(g.depth.data());
    params.normal_surface = const_cast<Vec3*>(g.engineNormals.data());
    params.scene_color_surface = const_cast<Vec3*>(g.color.data());
    params.ssr_out_surface = out.data();
    expectTrue(fuse::compute::launch_ssr_cpu(params), "SSR CPU pass on the mirror floor succeeds");

    // Routing: identical to the shared SSR reference.
    const SsfxViewStorage sv = ssfxView(g);
    ssfx::SsrParams ref{};
    ref.max_steps = params.max_steps;
    ref.stride_px = params.ray_step_size;
    ref.thickness = params.thickness;
    ref.max_distance = params.max_distance;
    ref.fade_screen_edge = params.fade_screen_edge;
    ref.refine_steps = params.refine_steps;
    std::vector<Vec3> refColor(kWidth * kHeight);
    std::vector<f32> refConf(kWidth * kHeight);
    ssfx::computeSsrCpu(sv.view, g.color.data(), ref, nullptr, refColor.data(), refConf.data());
    f32 maxRouteDiff = 0.f;
    for (size_t i = 0; i < out.size(); ++i) {
        const Vec3 c{out[i].x, out[i].y, out[i].z};
        maxRouteDiff = std::max(maxRouteDiff, (c - refColor[i]).length() + std::fabs(out[i].w - refConf[i]));
    }

    // Physical check: reflected colour vs the analytic mirror ray, over screen-visible true hits.
    u32 valid = 0;
    u32 hits = 0;
    u32 good = 0;
    f32 errSum = 0.f;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const u32 i = g.index(x, y);
            if (g.surface[i] != floorId) {
                continue;
            }
            const Vec3 pw = g.worldPos[i];
            const Vec3 incident = (pw - world.position).normalized();
            const Vec3 mirror{incident.x, -incident.y, incident.z};
            const Hit truth = reflected.trace(pw, mirror);
            if (!truth.hit || truth.t > params.max_distance) {
                continue;
            }
            const Vec3 hitView = world.worldToViewDir(pw + mirror * truth.t - world.position);
            f32 hx = 0.f;
            f32 hy = 0.f;
            if (!g.camera.project(hitView, hx, hy) || !g.camera.inside(hx, hy)) {
                continue;
            }
            const u32 hi = g.index(static_cast<u32>(hx), static_cast<u32>(hy));
            // Screen space only reflects what the camera sees; skip band boundaries (colour edges).
            if (g.surface[hi] != truth.surface || std::fabs(g.depth[hi] - hitView.z) > 0.01f * hitView.z) {
                continue;
            }
            const f32 bandPos = std::fmod(pw.x + mirror.x * truth.t + 8.f, 2.f);
            if (bandPos < 0.1f || bandPos > 1.9f) {
                continue;
            }
            ++valid;
            if (out[i].w <= 0.f) {
                continue;
            }
            ++hits;
            const Vec3 c{out[i].x, out[i].y, out[i].z};
            const f32 err = (c - truth.color).length() / truth.color.length();
            errSum += err;
            if (err <= 0.05f) {
                ++good;
            }
        }
    }
    const f32 coverage = valid > 0 ? static_cast<f32>(hits) / static_cast<f32>(valid) : 0.f;
    const f32 meanErr = hits > 0 ? errSum / static_cast<f32>(hits) : 1.f;
    const f32 goodFrac = hits > 0 ? static_cast<f32>(good) / static_cast<f32>(hits) : 0.f;
    std::printf("[compute] SSR mirror floor: route |compute - ssfx| max %.2e; valid %u, hits %u (coverage %.1f%%), "
                "mean rel colour err %.2f%%, within 5%%: %.1f%%\n",
                maxRouteDiff, valid, hits, 100.f * coverage, 100.f * meanErr, 100.f * goodFrac);
    expectTrue(maxRouteDiff <= 1e-6f, "launch_ssr_cpu equals the shared SSR reference");
    expectTrue(valid > 500u, "SSR scene has enough screen-visible mirror hits");
    expectTrue(coverage > 0.9f, "SSR finds >= 90% of screen-visible mirror reflections");
    expectTrue(meanErr < 0.05f, "SSR mean reflected colour error < 5% vs analytic mirror");
    expectTrue(goodFrac > 0.95f, "SSR: > 95% of hits reflect the correct wall band");

    // Centre sample: floor pixel reflecting the wall — luminance x confidence of the frame value.
    const u32 centre = g.index(kWidth / 2, kHeight / 2);
    expectTrue(g.surface[centre] == floorId, "SSR centre pixel lies on the mirror floor");
    const f32 centreSample = fuse::compute::ssr_center_sample(params);
    expectNear(centreSample, luminance(Vec3{out[centre].x, out[centre].y, out[centre].z}) * out[centre].w, 1e-6f,
               "SSR centre sample == luminance x confidence of the frame value");
    expectTrue(centreSample > 0.1f, "SSR centre sample reflects the wall");

    // Roughness: mirror (0) keeps full confidence, 1 is not traced, 0.5 halves it beyond the contact distance.
    std::vector<f32> roughness(kWidth * kHeight, 0.f);
    std::vector<fuse::math::Vec4> outRough(kWidth * kHeight);
    fuse::compute::SSRParams rough = params;
    rough.roughness_surface = roughness.data();
    rough.ssr_out_surface = outRough.data();
    expectTrue(fuse::compute::launch_ssr_cpu(rough), "SSR with roughness 0 succeeds");
    f32 maxMirrorDiff = 0.f;
    for (size_t i = 0; i < out.size(); ++i) {
        maxMirrorDiff = std::max(maxMirrorDiff, std::fabs(outRough[i].w - out[i].w));
    }
    expectTrue(maxMirrorDiff == 0.f, "SSR roughness 0 keeps mirror confidence");
    std::fill(roughness.begin(), roughness.end(), 0.5f);
    expectTrue(fuse::compute::launch_ssr_cpu(rough), "SSR with roughness 0.5 succeeds");
    f32 maxHalfErr = 0.f;
    for (size_t i = 0; i < out.size(); ++i) {
        maxHalfErr = std::max(maxHalfErr, std::fabs(outRough[i].w - 0.5f * out[i].w));
    }
    expectTrue(maxHalfErr < 1e-6f, "SSR roughness 0.5 halves confidence for distant hits");
    std::fill(roughness.begin(), roughness.end(), 1.f);
    expectTrue(fuse::compute::launch_ssr_cpu(rough), "SSR with roughness 1 succeeds");
    f32 roughSum = 0.f;
    for (const fuse::math::Vec4& v : outRough) {
        roughSum += v.w;
    }
    expectTrue(roughSum == 0.f, "SSR skips fully rough pixels");
}

// ---------------------------------------------------------------------------------------------------------
// SSGI — one-bounce gather vs the exact differential-area-to-polygon form factor (Lambert)
// ---------------------------------------------------------------------------------------------------------

constexpr f32 kPanelZ = 5.f;
constexpr f32 kPanelHalfWidth = 1.f;
constexpr f32 kPanelHeight = 1.5f;
const Vec3 kPanelRadiance{1.f, 0.6f, 0.3f};
constexpr f32 kFloorAlbedo = 0.5f;

/// Form factor from a differential area at `p` with normal `n` to the polygon (Lambert's formula).
f32 polygonFormFactor(const Vec3& p, const Vec3& n, const Vec3* verts, u32 count) {
    f32 sum = 0.f;
    for (u32 i = 0; i < count; ++i) {
        const Vec3 a = (verts[i] - p).normalized();
        const Vec3 b = (verts[(i + 1) % count] - p).normalized();
        const Vec3 c = fuse::math::cross(a, b);
        const f32 len = c.length();
        if (len < 1e-9f) {
            continue;
        }
        const f32 angle = std::atan2(len, a.dot(b));
        sum += angle * n.dot(c * (1.f / len));
    }
    return std::fabs(sum) / (2.f * kPi);
}

Scene panelScene() {
    Scene s{};
    Rect floor{};
    floor.axis = 1;
    floor.color = Vec3{};
    Rect panel{};
    panel.axis = 2;
    panel.value = kPanelZ;
    panel.lo[0] = -kPanelHalfWidth;
    panel.hi[0] = kPanelHalfWidth;
    panel.lo[1] = 0.f;
    panel.hi[1] = kPanelHeight;
    panel.color = kPanelRadiance;
    s.rects = {floor, panel};
    return s;
}

/// The gather's own estimator (its `ssgiSampleDirection` rays around the ssfx view-space normal) traced against the
/// true geometry: what SSGI would return if the depth buffer were the scene.
Vec3 trueGeometryGather(const Scene& scene, const GBuffer& g, u32 idx, u32 sampleSqrt, f32 maxDistance) {
    const Vec3 en = g.engineNormals[idx];
    Vec3 n{en.x, -en.y, -en.z};
    const Vec3 pView = g.world.worldToViewDir(g.worldPos[idx] - g.world.position);
    if (n.dot(pView) > 0.f) {
        n = n * -1.f;
    }
    Vec3 sum{};
    for (u32 i = 0; i < sampleSqrt; ++i) {
        for (u32 j = 0; j < sampleSqrt; ++j) {
            const Vec3 dirView = ssfx::ssgiSampleDirection(n, idx % kWidth, idx / kWidth, i, j, sampleSqrt);
            const Vec3 dir = g.world.viewToWorldDir(dirView);
            const Hit h = scene.trace(g.worldPos[idx], dir);
            if (h.hit && h.t <= maxDistance && h.normal.dot(dir) < 0.f) {
                sum = sum + h.color;
            }
        }
    }
    return sum * (1.f / static_cast<f32>(sampleSqrt * sampleSqrt));
}

void testSsgiPanelFormFactor() {
    const Scene scene = panelScene();
    const WorldCamera world = WorldCamera::pitchedDown(Vec3{0.f, 1.8f, 0.f}, 25.f * kPi / 180.f);
    const GBuffer g = rasterize(scene, world);
    std::vector<Vec3> albedo(kWidth * kHeight, Vec3{kFloorAlbedo, kFloorAlbedo, kFloorAlbedo});

    std::vector<Vec3> out(kWidth * kHeight);
    fuse::compute::SSGIParams params = makeSsgiParams();
    params.width = kWidth;
    params.height = kHeight;
    for (int i = 0; i < 16; ++i) {
        params.proj[i] = g.proj[i];
    }
    params.depth_surface = const_cast<f32*>(g.depth.data());
    params.normal_surface = const_cast<Vec3*>(g.engineNormals.data());
    params.albedo_surface = albedo.data();
    params.scene_color_surface = const_cast<Vec3*>(g.color.data());
    params.ssgi_out_surface = out.data();
    params.max_bounces = 1;
    params.sample_sqrt = 8;
    params.max_steps = 256;
    params.thickness = 0.15f;
    params.max_distance = 20.f;
    expectTrue(fuse::compute::launch_ssgi_cpu(params), "SSGI CPU pass on the panel scene succeeds");

    // Routing: identical to the shared SSGI reference.
    const SsfxViewStorage sv = ssfxView(g);
    ssfx::SsgiParams ref{};
    ref.sample_sqrt = params.sample_sqrt;
    ref.bounces = params.max_bounces;
    ref.max_steps = params.max_steps;
    ref.stride_px = params.ray_step_size;
    ref.thickness = params.thickness;
    ref.max_distance = params.max_distance;
    ref.intensity = params.intensity;
    std::vector<Vec3> refOut(kWidth * kHeight);
    ssfx::computeSsgiCpu(sv.view, g.color.data(), albedo.data(), ref, refOut.data());
    f32 maxRouteDiff = 0.f;
    for (size_t i = 0; i < out.size(); ++i) {
        maxRouteDiff = std::max(maxRouteDiff, (out[i] - refOut[i]).length());
    }

    // (1) Screen-space tracing vs the same estimator traced against the true geometry, and vs the exact form
    // factor (Lambert's polygon formula) summed over the region.
    const Vec3 panel[4] = {{-kPanelHalfWidth, 0.f, kPanelZ},
                           {kPanelHalfWidth, 0.f, kPanelZ},
                           {kPanelHalfWidth, kPanelHeight, kPanelZ},
                           {-kPanelHalfWidth, kPanelHeight, kPanelZ}};
    u32 samples = 0;
    f32 sumAbs = 0.f;
    f32 sumSigned = 0.f;
    f32 sumTruth = 0.f;
    f32 sumAnalytic = 0.f;
    f32 worstAbs = 0.f;
    f32 worstChroma = 0.f;
    f32 peak = 0.f;
    for (u32 y = 0; y < kHeight; y += 2) {
        for (u32 x = 0; x < kWidth; x += 2) {
            const u32 i = g.index(x, y);
            if (g.surface[i] != 0) {
                continue;
            }
            const f32 dz = kPanelZ - g.worldPos[i].z;
            if (dz < 0.2f || dz > 3.f) {
                continue;
            }
            const f32 truth =
                kFloorAlbedo * luminance(trueGeometryGather(scene, g, i, params.sample_sqrt, params.max_distance));
            const f32 got = luminance(out[i]);
            ++samples;
            sumAbs += std::fabs(got - truth);
            sumSigned += got - truth;
            sumTruth += truth;
            sumAnalytic += kFloorAlbedo * luminance(kPanelRadiance) *
                           polygonFormFactor(g.worldPos[i], Vec3{0.f, 1.f, 0.f}, panel, 4);
            peak = std::max(peak, truth);
            worstAbs = std::max(worstAbs, std::fabs(got - truth));
            // Colour bleeding keeps the panel's chromaticity (red / green ratio).
            if (out[i].y > 1e-4f) {
                worstChroma =
                    std::max(worstChroma, std::fabs(out[i].x / out[i].y - kPanelRadiance.x / kPanelRadiance.y) /
                                              (kPanelRadiance.x / kPanelRadiance.y));
            }
        }
    }
    // Per pixel, a ray grazing a panel edge or passing just behind it (thickness) flips between hit and miss, so
    // each pixel can be off by a few of its 64 rays. The residual energy loss (~2% at 160x120) is rays that cross
    // the panel's silhouette between two march samples (nearest-depth taps see sky) — a one-pixel effect.
    const f32 meanRel = sumTruth > 0.f ? sumAbs / sumTruth : 1.f;
    const f32 bias = sumTruth > 0.f ? sumSigned / sumTruth : 1.f;
    const f32 analyticBias = sumAnalytic > 0.f ? (sumTruth + sumSigned - sumAnalytic) / sumAnalytic : 1.f;

    // (2) Converged gather at the centre pixel vs the exact form factor (Lambert's polygon formula).
    const u32 centre = g.index(kWidth / 2, kHeight / 2);
    expectTrue(g.surface[centre] == 0 && kPanelZ - g.worldPos[centre].z > 0.5f,
               "SSGI centre pixel lies on the floor in front of the panel");
    const f32 formFactor = polygonFormFactor(g.worldPos[centre], Vec3{0.f, 1.f, 0.f}, panel, 4);
    const f32 analytic = kFloorAlbedo * luminance(kPanelRadiance) * formFactor;
    fuse::compute::SSGIParams converged = params;
    converged.sample_sqrt = 64;
    const f32 centreConverged = fuse::compute::ssgi_center_sample(converged);
    const f32 centreRel = std::fabs(centreConverged - analytic) / analytic;
    // The estimator itself (same 4096 directions against the true geometry) converges to the form factor.
    const f32 estimatorTruth = kFloorAlbedo * luminance(trueGeometryGather(scene, g, centre, 64u, params.max_distance));
    const f32 estimatorRel = std::fabs(estimatorTruth - analytic) / analytic;
    std::printf("[compute] SSGI panel scene: route |compute - ssfx| max %.2e; %u floor samples 0.2-3 m from the "
                "panel (64 rays): screen vs true-geometry gather bias %+.2f%%, mean |err| %.2f%% (of mean), worst %.4f "
                "(peak %.4f), region energy vs form factor %+.2f%%, chroma drift %.2e; centre %.2f m from the panel, 4096 rays: screen %.5f, true-geometry "
                "%.5f, form factor %.5f (F = %.4f; screen %.2f%%, estimator %.2f%%)\n",
                maxRouteDiff, samples, 100.f * bias, 100.f * meanRel, worstAbs, peak, 100.f * analyticBias, worstChroma,
                kPanelZ - g.worldPos[centre].z, centreConverged, estimatorTruth, analytic, formFactor,
                100.f * centreRel, 100.f * estimatorRel);
    expectTrue(maxRouteDiff <= 1e-6f, "launch_ssgi_cpu equals the shared SSGI reference");
    expectTrue(samples > 100u, "SSGI panel scene has enough floor samples");
    expectTrue(std::fabs(bias) < 0.04f, "SSGI screen-space gather has < 4% energy bias vs the true-geometry gather");
    expectTrue(std::fabs(analyticBias) < 0.03f, "SSGI region energy within 3% of the analytic form factors");
    expectTrue(meanRel < 0.15f, "SSGI screen-space gather mean per-pixel error < 15% at 64 rays");
    expectTrue(worstAbs < 0.2f * peak, "SSGI screen-space gather worst error < 20% of the peak");
    expectTrue(worstChroma < 1e-4f, "SSGI preserves the emitter chromaticity");
    expectTrue(estimatorRel < 0.01f, "SSGI estimator converges to the analytic form factor (< 1%)");
    expectTrue(centreRel < 0.04f, "SSGI converged screen-space gather within 4% of the analytic form factor");

    // Centre sample equals the frame value; intensity is linear; an unlit scene gathers nothing.
    expectNear(fuse::compute::ssgi_center_sample(params), luminance(out[centre]), 1e-6f,
               "SSGI centre sample == frame value");
    fuse::compute::SSGIParams doubled = params;
    doubled.intensity = 2.f;
    expectNear(fuse::compute::ssgi_center_sample(doubled), 2.f * luminance(out[centre]), 1e-5f,
               "SSGI intensity scales linearly");
    std::vector<Vec3> dark(kWidth * kHeight, Vec3{});
    fuse::compute::SSGIParams unlit = params;
    unlit.scene_color_surface = dark.data();
    expectNear(fuse::compute::ssgi_center_sample(unlit), 0.f, 0.f, "SSGI of an unlit scene is zero");

    // Two bounces: a lit floor re-lights the (reflective) panel, which then adds light back to the floor.
    std::vector<Vec3> lit = g.color;
    for (size_t i = 0; i < lit.size(); ++i) {
        if (g.surface[i] == 0) {
            lit[i] = Vec3{0.2f, 0.2f, 0.2f};
        }
    }
    std::vector<Vec3> albedo2(kWidth * kHeight, Vec3{0.8f, 0.8f, 0.8f});
    std::vector<Vec3> one(kWidth * kHeight);
    std::vector<Vec3> two(kWidth * kHeight);
    fuse::compute::SSGIParams bounce = params;
    bounce.sample_sqrt = 4;
    bounce.scene_color_surface = lit.data();
    bounce.albedo_surface = albedo2.data();
    bounce.ssgi_out_surface = one.data();
    expectTrue(fuse::compute::launch_ssgi_cpu(bounce), "SSGI one bounce succeeds");
    bounce.max_bounces = 2;
    bounce.ssgi_out_surface = two.data();
    expectTrue(fuse::compute::launch_ssgi_cpu(bounce), "SSGI two bounces succeeds");
    f32 minGain = 1e9f;
    f32 floorGain = 0.f;
    u32 floorCount = 0;
    for (size_t i = 0; i < one.size(); ++i) {
        minGain = std::min(minGain, luminance(two[i]) - luminance(one[i]));
        if (g.surface[i] == 0 && luminance(one[i]) > 0.01f) {
            floorGain += luminance(two[i]) / luminance(one[i]);
            ++floorCount;
        }
    }
    floorGain = floorCount > 0 ? floorGain / static_cast<f32>(floorCount) : 0.f;
    std::printf("[compute] SSGI second bounce: min gain %.2e, mean floor gain x%.3f\n", minGain, floorGain);
    expectTrue(minGain >= -1e-6f, "SSGI second bounce never removes light");
    expectTrue(floorGain > 1.01f, "SSGI second bounce adds panel-reflected floor light");
    expectNear(fuse::compute::ssgi_center_sample(bounce), luminance(two[centre]), 1e-6f,
               "SSGI multi-bounce centre sample == frame value");
}

void testScreenSpaceEffectsInfo() {
    const fuse::compute::ScreenSpaceEffectsInfo info = fuse::compute::screen_space_effects_info();
    expectTrue(info.valid, "screen-space effects info valid");
#if defined(FUSE_HAS_CUDA)
    expectTrue(info.mode == fuse::compute::ScreenSpaceEffectsMode::Cuda, "CUDA backend active");
#else
    expectTrue(info.mode == fuse::compute::ScreenSpaceEffectsMode::CpuReference,
               "CPU reference mode without toolkit");
    expectTrue(!fuse::jobs::cudaJobsAvailable(), "cuda jobs unavailable without toolkit");
#endif
}

void testSsaoContactWeighting() {
    const fuse::compute::SSAOParams params = makeSsaoParams();

    expectNear(fuse::compute::ssao_contact_ao_weight(0.f, 1.f, params), 1.f, 0.001f,
               "aligned contact yields full AO weight");
    expectTrue(fuse::compute::ssao_contact_ao_weight(params.contact_depth_scale, 1.f, params) <
                   fuse::compute::ssao_contact_ao_weight(0.f, 1.f, params),
               "large depth delta reduces contact AO weight");
    expectTrue(fuse::compute::ssao_contact_ao_weight(0.f, 0.f, params) <
                   fuse::compute::ssao_contact_ao_weight(0.f, 1.f, params),
               "dissimilar normals reduce contact AO weight");
}

void testSsaoBlurWeight() {
    const fuse::compute::SSAOParams params = makeSsaoParams();

    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.f, 1.f, 1.f, params), 1.f, 0.001f,
               "identical depth/normal yields full blur weight");
    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.01f, 1.f, 1.f, params), 0.f, 0.001f,
               "large depth delta rejects blur tap");
    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.f, 1.f, 0.5f, params), 0.f, 0.001f,
               "dissimilar normals reject blur tap");

    fuse::compute::SSAOParams disabledBlur = params;
    disabledBlur.enable_blur = false;
    expectNear(fuse::compute::ssao_blur_weight(1.f, 1.f, 1.f, 1.f, disabledBlur), 0.f, 0.001f,
               "disabled blur yields zero weight");
}

void testSsrContactHardening() {
    const fuse::compute::SSRParams params = makeSsrParams();

    expectNear(fuse::compute::ssr_contact_harden_roughness(0.f, 0.8f, params), 0.02f, 0.001f,
               "contact hit snaps roughness to floor");
    expectNear(fuse::compute::ssr_contact_harden_roughness(params.contact_distance, 0.8f, params), 0.8f,
               0.001f, "distant hit preserves material roughness");

    fuse::compute::SSRParams disabled = params;
    disabled.contact_hardening = false;
    expectNear(fuse::compute::ssr_contact_harden_roughness(0.f, 0.8f, disabled), 0.8f, 0.001f,
               "disabled contact hardening preserves roughness");
}

void testSsrScreenEdgeFade() {
    const fuse::compute::SSRParams params = makeSsrParams();

    expectNear(fuse::compute::ssr_screen_edge_fade(0.5f, 0.5f, params), 1.f, 0.001f,
               "center pixel has full edge fade");
    expectNear(fuse::compute::ssr_screen_edge_fade(0.f, 0.5f, params), 0.f, 0.001f,
               "screen border fades to zero");

    fuse::compute::SSRParams noFade = params;
    noFade.fade_screen_edge = 0.f;
    expectNear(fuse::compute::ssr_screen_edge_fade(0.f, 0.f, noFade), 1.f, 0.001f,
               "zero fade width disables edge attenuation");
}

void testParamValidation() {
    expectTrue(fuse::compute::validate_ssao_params(makeSsaoParams()), "default SSAO params valid");

    fuse::compute::SSAOParams invalidSsao = makeSsaoParams();
    invalidSsao.directions = 0;
    expectTrue(!fuse::compute::validate_ssao_params(invalidSsao), "zero SSAO directions rejected");

    invalidSsao = makeSsaoParams();
    invalidSsao.blur_normal_threshold = 1.5f;
    expectTrue(!fuse::compute::validate_ssao_params(invalidSsao), "SSAO blur normal threshold clamped");

    expectTrue(fuse::compute::validate_ssr_params(makeSsrParams()), "default SSR params valid");

    fuse::compute::SSRParams invalidSsr = makeSsrParams();
    invalidSsr.contact_distance = 0.f;
    expectTrue(!fuse::compute::validate_ssr_params(invalidSsr), "zero SSR contact distance rejected");

    invalidSsr = makeSsrParams();
    invalidSsr.max_steps = 0;
    expectTrue(!fuse::compute::validate_ssr_params(invalidSsr), "zero SSR max steps rejected");

    expectTrue(fuse::compute::validate_ssgi_params(makeSsgiParams()), "default SSGI params valid");

    fuse::compute::SSGIParams invalidSsgi = makeSsgiParams();
    invalidSsgi.thickness = 0.f;
    expectTrue(!fuse::compute::validate_ssgi_params(invalidSsgi), "zero SSGI thickness rejected");

    invalidSsgi = makeSsgiParams();
    invalidSsgi.intensity = -1.f;
    expectTrue(!fuse::compute::validate_ssgi_params(invalidSsgi), "negative SSGI intensity rejected");
}

/// Step scene frame with all surfaces bound, for the launcher / job plumbing tests.
struct LaunchFixture {
    GBuffer g;
    std::vector<Vec3> albedo;
    std::vector<f32> ao;
    std::vector<fuse::math::Vec4> ssr;
    std::vector<Vec3> ssgi;
    fuse::compute::SSAOParams ssaoParams{};
    fuse::compute::SSRParams ssrParams{};
    fuse::compute::SSGIParams ssgiParams{};

    LaunchFixture() : g(rasterize(stepScene(), WorldCamera::pitchedDown(Vec3{0.f, 2.2f, 0.f}, 0.5f))) {
        albedo.assign(kWidth * kHeight, Vec3{0.5f, 0.5f, 0.5f});
        ssaoParams = ssaoForGBuffer(g, ao);
        ssaoParams.directions = 8;
        ssaoParams.steps_per_dir = 4;
        ssr.assign(kWidth * kHeight, fuse::math::Vec4{-1.f, -1.f, -1.f, -1.f});
        ssgi.assign(kWidth * kHeight, Vec3{-1.f, -1.f, -1.f});
        ssrParams = makeSsrParams();
        ssgiParams = makeSsgiParams();
        ssgiParams.max_bounces = 1;
        ssrParams.width = kWidth;
        ssrParams.height = kHeight;
        ssgiParams.width = kWidth;
        ssgiParams.height = kHeight;
        for (int i = 0; i < 16; ++i) {
            ssrParams.proj[i] = g.proj[i];
            ssgiParams.proj[i] = g.proj[i];
        }
        ssrParams.depth_surface = g.depth.data();
        ssrParams.normal_surface = g.engineNormals.data();
        ssrParams.scene_color_surface = g.color.data();
        ssrParams.ssr_out_surface = ssr.data();
        ssgiParams.depth_surface = g.depth.data();
        ssgiParams.normal_surface = g.engineNormals.data();
        ssgiParams.scene_color_surface = g.color.data();
        ssgiParams.albedo_surface = albedo.data();
        ssgiParams.ssgi_out_surface = ssgi.data();
    }

    /// Every output pixel was written (the sentinels are negative, real outputs are not).
    bool allWritten() const {
        for (size_t i = 0; i < ao.size(); ++i) {
            if (ao[i] < 0.f || ssr[i].w < 0.f || ssgi[i].x < 0.f) {
                return false;
            }
        }
        return true;
    }
};

void testLaunchScreenSpaceEffects() {
    LaunchFixture f{};
#if !defined(FUSE_HAS_CUDA)
    // Without CUDA the host launchers run the CPU reference on host surfaces.
    expectTrue(fuse::compute::launch_ssao(f.ssaoParams), "launch_ssao succeeds");
    expectTrue(fuse::compute::launch_ssr(f.ssrParams), "launch_ssr succeeds");
    expectTrue(fuse::compute::launch_ssgi(f.ssgiParams), "launch_ssgi succeeds");
    expectTrue(f.allWritten(), "host launchers write every output pixel");
    const std::vector<f32> ao = f.ao;
    expectTrue(fuse::compute::launch_ssao_cpu(f.ssaoParams) && ao == f.ao, "launch_ssao == launch_ssao_cpu");
#endif
    expectTrue(fuse::compute::launch_ssao_cpu(f.ssaoParams), "launch_ssao_cpu succeeds");
    expectTrue(fuse::compute::launch_ssr_cpu(f.ssrParams), "launch_ssr_cpu succeeds");
    expectTrue(fuse::compute::launch_ssgi_cpu(f.ssgiParams), "launch_ssgi_cpu succeeds");
    expectTrue(f.allWritten(), "CPU launchers write every output pixel");

    fuse::compute::SSAOParams invalidSsao = f.ssaoParams;
    invalidSsao.width = 0;
    expectTrue(!fuse::compute::launch_ssao_cpu(invalidSsao), "launch_ssao_cpu rejects invalid params");
    invalidSsao = f.ssaoParams;
    invalidSsao.depth_surface = nullptr;
    expectTrue(!fuse::compute::launch_ssao_cpu(invalidSsao), "launch_ssao_cpu rejects a missing depth surface");
    invalidSsao = f.ssaoParams;
    invalidSsao.ao_out_surface = nullptr;
    expectTrue(!fuse::compute::launch_ssao_cpu(invalidSsao), "launch_ssao_cpu rejects a missing output");
    invalidSsao = f.ssaoParams;
    std::fill(std::begin(invalidSsao.proj), std::end(invalidSsao.proj), 0.f);
    expectTrue(!fuse::compute::launch_ssao_cpu(invalidSsao), "launch_ssao_cpu rejects a non-perspective proj");

    fuse::compute::SSRParams invalidSsr = f.ssrParams;
    invalidSsr.ray_step_size = 0.f;
    expectTrue(!fuse::compute::launch_ssr_cpu(invalidSsr), "launch_ssr_cpu rejects invalid params");
    invalidSsr = f.ssrParams;
    invalidSsr.scene_color_surface = nullptr;
    expectTrue(!fuse::compute::launch_ssr_cpu(invalidSsr), "launch_ssr_cpu rejects a missing scene colour");

    fuse::compute::SSGIParams invalidSsgi = f.ssgiParams;
    invalidSsgi.sample_sqrt = 0;
    expectTrue(!fuse::compute::launch_ssgi_cpu(invalidSsgi), "launch_ssgi_cpu rejects zero samples");
    invalidSsgi = f.ssgiParams;
    invalidSsgi.ssgi_out_surface = nullptr;
    expectTrue(!fuse::compute::launch_ssgi_cpu(invalidSsgi), "launch_ssgi_cpu rejects a missing output");

    // Without surfaces the centre samples report "no geometry" instead of a made-up constant.
    expectNear(fuse::compute::ssao_center_sample(makeSsaoParams()), 1.f, 0.f, "SSAO centre sample without depth");
    expectNear(fuse::compute::ssr_center_sample(makeSsrParams()), 0.f, 0.f, "SSR centre sample without surfaces");
    expectNear(fuse::compute::ssgi_center_sample(makeSsgiParams()), 0.f, 0.f, "SSGI centre sample without surfaces");
}

void testSubmitScreenSpaceJobs() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    LaunchFixture f{};

    fuse::jobs::JobCounter ssaoCounter(1);
    fuse::compute::SSAOJobDesc ssaoJob{};
    ssaoJob.params = f.ssaoParams;
    ssaoJob.counter = &ssaoCounter;
    fuse::compute::submit_ssao_job(std::move(ssaoJob));
    ssaoCounter.wait();
    expectTrue(ssaoCounter.isComplete(), "SSAO job signals counter");

    fuse::jobs::JobCounter ssrCounter(1);
    fuse::compute::SSRJobDesc ssrJob{};
    ssrJob.params = f.ssrParams;
    ssrJob.counter = &ssrCounter;
    fuse::compute::submit_ssr_job(std::move(ssrJob));
    ssrCounter.wait();
    expectTrue(ssrCounter.isComplete(), "SSR job signals counter");

    fuse::jobs::JobCounter ssgiCounter(1);
    fuse::compute::SSGIJobDesc ssgiJob{};
    ssgiJob.params = f.ssgiParams;
    ssgiJob.counter = &ssgiCounter;
    fuse::compute::submit_ssgi_job(std::move(ssgiJob));
    ssgiCounter.wait();
    expectTrue(ssgiCounter.isComplete(), "SSGI job signals counter");

    scheduler.shutdown();

#if !defined(FUSE_HAS_CUDA)
    // CPU lane: the jobs ran the reference passes into the host surfaces.
    expectTrue(f.allWritten(), "screen-space jobs write every output pixel");
    std::vector<f32> direct;
    fuse::compute::SSAOParams again = ssaoForGBuffer(f.g, direct);
    again.directions = 8;
    again.steps_per_dir = 4;
    expectTrue(fuse::compute::launch_ssao_cpu(again) && direct == f.ao, "SSAO job output == direct CPU pass");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testScreenSpaceEffectsInfo();
    testSsaoCornerMatchesTrueGeometry();
    testSsrMirrorFloor();
    testSsgiPanelFormFactor();
    testSsaoContactWeighting();
    testSsaoBlurWeight();
    testSsrContactHardening();
    testSsrScreenEdgeFade();
    testParamValidation();
    testLaunchScreenSpaceEffects();
    testSubmitScreenSpaceJobs();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
