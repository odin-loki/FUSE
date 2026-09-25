// Gate: "SDF normals are smooth at surface — no faceting visible at any zoom level".
//
// Numeric proof on the renderer's SDF normal evaluation (`fuse::compute::ray_march_scene_normal`, the normal the
// CPU reference ray marcher writes into its output surface). For a sphere, an edge-rounded box and a smooth-union
// blend of two spheres, surface points are sampled along a curve at progressively finer spacing (zoom 1x, 10x,
// 100x, 1000x) and, for every adjacent pair, the angular change of the engine normal is compared with the angular
// change of a double-precision reference normal at the same points (= local curvature x spacing):
//   * no steps     : engine change <= 1.2 x reference change + noise floor,
//   * no plateaus  : engine change >= 0.8 x reference change - noise floor,
//   * bounded      : reference change <= kappa_max x spacing (curvature bound of the shape),
//   * accuracy     : every engine normal is within a few f32 ulps (rad) of the reference.
// The noise floor (4e-7 rad) is ~3 f32 ulps at unit scale, i.e. 4 orders of magnitude below one 8-bit shading
// quantum (1/255), so "passes" here is far stricter than "no faceting visible".
//
// Part A evaluates exact surface points; part B goes end-to-end through `launch_ray_march_cpu` (pixel rays through
// a zoomed camera looking at the feature 30 degrees off-normal), additionally checking that consecutive hit points
// advance regularly (no hit-position plateaus that would quantise the normal).
//
// Sensitivity: the previous normal (central differences with h = max(min_dist, 1e-4 t)) is run through the same
// metric at 1000x and must be flagged — proving the metric detects quantised / stepped normals.
//
// There is no GPU SDF ray-march pass yet (Compute/kernels/ray_march.cu is a stub kernel and no Vulkan SDF shader
// exists), so no GPU readback is performed; this gate covers the CPU reference normal function only.

#include <fuse/compute/ray_march.hpp>
#include <fuse/math/sdf.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::compute::RayMarchParams;
using fuse::compute::SdfObject;
using fuse::compute::SdfPrimitiveType;
using fuse::math::Vec3;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct D3 {
    double x = 0, y = 0, z = 0;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 unit(D3 a) { return a * (1.0 / len(a)); }
D3 toD(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 toF(D3 v) { return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)}; }
double angleBetween(D3 a, D3 b) { return std::atan2(len(cross(a, b)), dot(a, b)); }

using RefSdf = std::function<double(D3)>;

double refSphere(D3 p, D3 c, double r) { return len(p - c) - r; }
double refBox(D3 p, D3 h) {
    const D3 q{std::abs(p.x) - h.x, std::abs(p.y) - h.y, std::abs(p.z) - h.z};
    const D3 o{std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)};
    return len(o) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0);
}
double refSmoothUnion(double a, double b, double k) {
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

/// Double-precision reference normal (central differences at h = 1e-7: truncation ~1e-14, rounding ~1e-9).
D3 refNormal(const RefSdf& f, D3 p) {
    const double h = 1e-7;
    return unit(D3{f(p + D3{h, 0, 0}) - f(p - D3{h, 0, 0}), f(p + D3{0, h, 0}) - f(p - D3{0, h, 0}),
                   f(p + D3{0, 0, h}) - f(p - D3{0, 0, h})});
}

struct Scene {
    const char* name = "";
    std::vector<SdfObject> objects;
    RefSdf ref;
    double kappaMax = 1.0;
    /// Feature angles (in the z = 0 slice, around the origin) the zoom windows are centred on.
    std::vector<double> featureThetas;
    /// Part B camera tilt sign per feature (chosen so the 1x pixel row stays on the object, no silhouette).
    std::vector<double> featureTilts;
    RayMarchParams params() const {
        RayMarchParams p{};
        p.objects = objects.data();
        p.object_count = static_cast<u32>(objects.size());
        return p;
    }
};

Scene makeSphere() {
    Scene s{};
    s.name = "sphere";
    SdfObject o{};
    o.params = {1.f, 0.f, 0.f};
    o.type = static_cast<u32>(SdfPrimitiveType::Sphere);
    o.alpha = 0.f;
    s.objects.push_back(o);
    s.ref = [](D3 p) { return refSphere(p, {}, 1.0); };
    s.kappaMax = 1.0;
    s.featureThetas = {0.7, 2.1};
    return s;
}

Scene makeRoundedBox() {
    Scene s{};
    s.name = "rounded box";
    SdfObject o{};
    o.params = {1.f, 0.6f, 0.8f};
    o.rounding = 0.2f;
    o.type = static_cast<u32>(SdfPrimitiveType::Box);
    o.alpha = 0.f;
    s.objects.push_back(o);
    s.ref = [](D3 p) {
        return refBox(p, {1.0 - 0.2, static_cast<double>(0.6f) - 0.2, static_cast<double>(0.8f) - 0.2}) - 0.2;
    };
    s.kappaMax = 1.0 / 0.2;
    // Edge-rounding arc (+x+y edge), both arc/face tangent seams, and a flat face (kappa = 0).
    s.featureThetas = {std::atan2(0.5, 0.9), std::atan2(0.4, 1.0), std::atan2(0.6, 0.8), 0.2};
    s.featureTilts = {1.0, 1.0, -1.0, 1.0};
    return s;
}

Scene makeSmoothUnion() {
    Scene s{};
    s.name = "smooth union";
    SdfObject a{};
    a.position = {-0.5f, 0.f, 0.f};
    a.params = {0.7f, 0.f, 0.f};
    a.type = static_cast<u32>(SdfPrimitiveType::Sphere);
    a.alpha = 0.f;
    SdfObject b = a;
    b.position = {0.5f, 0.f, 0.f};
    b.alpha = 0.4f;
    s.objects = {a, b};
    s.ref = [](D3 p) {
        const double r = static_cast<double>(0.7f);
        return refSmoothUnion(refSphere(p, {-0.5, 0, 0}, r), refSphere(p, {0.5, 0, 0}, r), static_cast<double>(0.4f));
    };
    s.kappaMax = 12.0; // concave blend fillet
    // Blend seam (x = 0) and the blend-region boundary where the fillet meets the sphere (curvature jump).
    s.featureThetas = {0.5 * 3.14159265358979, 1.2, 1.9};
    return s;
}

/// Surface point in the z = 0 slice along direction theta from the origin (bisection in double).
D3 surfacePoint(const RefSdf& f, double theta) {
    const D3 dir{std::cos(theta), std::sin(theta), 0.0};
    double lo = 0.0;
    double hi = 3.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (f(dir * mid) < 0.0 ? lo : hi) = mid;
    }
    return dir * (0.5 * (lo + hi));
}

struct Metrics {
    int pairs = 0;
    int steps = 0;
    int plateaus = 0;
    int curvatureViolations = 0;
    int accuracyViolations = 0;
    double maxNormalError = 0.0;
};

constexpr double kNoiseFloor = 4e-7;
constexpr double kAccuracyBound = 5e-7;

/// Compares the angular change between consecutive engine normals with the reference change at the same points.
Metrics measure(const Scene& scene, const std::vector<Vec3>& points, const std::vector<Vec3>& normals) {
    Metrics m{};
    std::vector<D3> ref(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        ref[i] = refNormal(scene.ref, toD(points[i]));
        const double err = angleBetween(unit(toD(normals[i])), ref[i]);
        m.maxNormalError = std::max(m.maxNormalError, err);
        if (!(err <= kAccuracyBound)) {
            ++m.accuracyViolations;
        }
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double engine = angleBetween(unit(toD(normals[i])), unit(toD(normals[i + 1])));
        const double expected = angleBetween(ref[i], ref[i + 1]);
        const double spacing = len(toD(points[i + 1]) - toD(points[i]));
        ++m.pairs;
        if (!(engine <= 1.2 * expected + kNoiseFloor)) {
            ++m.steps;
        }
        if (!(engine >= 0.8 * expected - kNoiseFloor)) {
            ++m.plateaus;
        }
        if (!(expected <= scene.kappaMax * spacing * 1.05 + 1e-9)) {
            ++m.curvatureViolations;
        }
    }
    return m;
}

bool clean(const Metrics& m) {
    return m.pairs > 0 && m.steps == 0 && m.plateaus == 0 && m.curvatureViolations == 0 && m.accuracyViolations == 0;
}

void report(const char* part, const Scene& scene, double zoom, double feature, const Metrics& m) {
    std::printf("%s %-12s zoom %6.0fx theta %.3f: pairs %d steps %d plateaus %d kappa-viol %d acc-viol %d "
                "max normal err %.2e rad\n",
                part, scene.name, zoom, feature, m.pairs, m.steps, m.plateaus, m.curvatureViolations,
                m.accuracyViolations, m.maxNormalError);
}

constexpr double kZooms[] = {1.0, 10.0, 100.0, 1000.0};
constexpr int kSamples = 64;
/// 1x spacing: ~1 px of a 1000 px view framing a ~2 unit object.
constexpr double kBaseSpacing = 2e-3;

/// The normal the ray marcher used before this gate: central differences, h = max(min_dist, 1e-4 max(t, 1)).
Vec3 legacyFiniteDifferenceNormal(const RayMarchParams& params, Vec3 p, f32 t) {
    const f32 h = std::max(params.min_dist, 1e-4f * std::max(t, 1.f));
    return fuse::math::SDF::finiteDifferenceNormal(
        [&params](Vec3 q) { return fuse::compute::ray_march_scene_distance(params, q); }, p, h);
}

/// Part A: exact surface points at spacing kBaseSpacing / zoom, normal function evaluated directly.
void partA(const Scene& scene, int& legacyFlagged) {
    const RayMarchParams params = scene.params();
    for (double feature : scene.featureThetas) {
        for (double zoom : kZooms) {
            const double radius = len(surfacePoint(scene.ref, feature));
            const double dTheta = kBaseSpacing / zoom / radius;
            std::vector<Vec3> points;
            std::vector<Vec3> normals;
            std::vector<Vec3> legacy;
            for (int i = 0; i < kSamples; ++i) {
                const Vec3 p = toF(surfacePoint(scene.ref, feature + (i - kSamples / 2) * dTheta));
                points.push_back(p);
                normals.push_back(fuse::compute::ray_march_scene_normal(params, p));
                legacy.push_back(legacyFiniteDifferenceNormal(params, p, 3.f));
            }
            const Metrics m = measure(scene, points, normals);
            report("A", scene, zoom, feature, m);
            expectTrue(clean(m), "part A: engine normal continuous (no steps/plateaus, curvature-bounded, accurate)");
            if (zoom == 1000.0) {
                const Metrics lm = measure(scene, points, legacy);
                std::printf("  legacy FD normal @1000x: steps %d plateaus %d max err %.2e rad\n", lm.steps,
                            lm.plateaus, lm.maxNormalError);
                legacyFlagged += clean(lm) ? 0 : 1;
            }
        }
    }
}

/// Part B: end-to-end through launch_ray_march_cpu — a 64 x 1 pixel row of a camera 3 units away, looking at the
/// feature 30 degrees off its normal, with horizontal field of view 0.1 / zoom rad (1x: ~5e-3 world units / px).
void partB(const Scene& scene) {
    for (std::size_t f = 0; f < scene.featureThetas.size(); ++f) {
        const double feature = scene.featureThetas[f];
        const D3 target = surfacePoint(scene.ref, feature);
        const D3 n = refNormal(scene.ref, target);
        const double tilt = 0.5236 * (f < scene.featureTilts.size() ? scene.featureTilts[f] : 1.0);
        const D3 view{n.x * std::cos(tilt) - n.y * std::sin(tilt), n.x * std::sin(tilt) + n.y * std::cos(tilt), 0.0};
        const D3 camPos = target + view * 3.0;
        const D3 forward = view * -1.0;
        const D3 up{0.0, 0.0, 1.0};
        const D3 right = unit(cross(forward, up));
        for (double zoom : kZooms) {
            constexpr u32 kWidth = 64;
            RayMarchParams params = scene.params();
            params.width = kWidth;
            params.height = 1;
            params.cam_pos = toF(camPos);
            params.cam_forward = toF(forward);
            params.cam_right = toF(right);
            params.cam_up = toF(up);
            params.fov_rad = static_cast<f32>(0.1 / zoom / kWidth);
            std::vector<f32> depth(kWidth);
            std::vector<fuse::math::Vec4> out(kWidth);
            params.depth_surface = depth.data();
            params.output_surface = out.data();
            expectTrue(fuse::compute::launch_ray_march_cpu(params), "part B: launch_ray_march_cpu accepted params");

            // Recompute hit positions exactly as the launcher does.
            const Vec3 fwd = params.cam_forward.normalized();
            const Vec3 rgt = params.cam_right.normalized();
            const f32 tanHalf = std::tan(0.5f * params.fov_rad);
            const f32 aspect = static_cast<f32>(kWidth);
            std::vector<Vec3> points;
            std::vector<Vec3> normals;
            bool allHit = true;
            for (u32 x = 0; x < kWidth; ++x) {
                const f32 ndcX = 2.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth) - 1.f;
                const Vec3 dir = (fwd + rgt * (ndcX * tanHalf * aspect)).normalized();
                allHit = allHit && depth[x] >= 0.f;
                points.push_back(params.cam_pos + dir * depth[x]);
                normals.push_back(Vec3{out[x].x, out[x].y, out[x].z});
            }
            expectTrue(allHit, "part B: every pixel of the zoomed row hits the surface");
            const Metrics m = measure(scene, points, normals);

            // Hit-point regularity: consecutive hit spacing varies smoothly (no position plateaus / jumps).
            int irregular = 0;
            for (std::size_t i = 0; i + 2 < points.size(); ++i) {
                const double a = len(toD(points[i + 1]) - toD(points[i]));
                const double b = len(toD(points[i + 2]) - toD(points[i + 1]));
                if (!(b <= 1.5 * a && a <= 1.5 * b)) {
                    ++irregular;
                }
            }
            report("B", scene, zoom, feature, m);
            if (irregular != 0) {
                std::printf("  irregular hit spacing: %d\n", irregular);
            }
            expectTrue(clean(m), "part B: ray-marched normals continuous at this zoom");
            expectTrue(irregular == 0, "part B: hit points advance regularly (no plateaus in hit position)");
        }
    }
}

} // namespace

int main() {
    const Scene scenes[] = {makeSphere(), makeRoundedBox(), makeSmoothUnion()};
    int legacyFlagged = 0;
    for (const Scene& scene : scenes) {
        partA(scene, legacyFlagged);
    }
    for (const Scene& scene : scenes) {
        partB(scene);
    }
    std::printf("legacy FD normal flagged in %d zoom-1000x windows\n", legacyFlagged);
    expectTrue(legacyFlagged > 0, "sensitivity: metric flags the legacy finite-difference normal at 1000x");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_b2_sdf_normal_smoothness: OK\n");
    return 0;
}
