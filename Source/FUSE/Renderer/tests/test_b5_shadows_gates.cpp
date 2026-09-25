// B5.5 shadow gate proofs (CPU): cascaded shadow maps + SDF soft shadows.
//
// CI has no GPU, so each gate row is proven on the CPU against independent references:
//   * CSM cascades: split distances vs closed-form PSSM, every point of each frustum slice projects
//     inside its cascade map, boundary selection is consistent, and a shadow-map lookup of a
//     plane + wall scene (depth texels ray-cast from the inverted light matrix) matches an analytic
//     ray/box occlusion test on both sides of every cascade boundary (no seam).
//   * CSM stabilisation: sub-texel camera translation + rotation keeps shadow texel centres of static
//     world points on exact integer offsets (frame diff = 0); with stabilise=false they drift.
//   * SDF soft shadow: penumbra width on a receiver is linear in occluder distance and matches the
//     predicted width for the equivalent spherical light; per-pixel visibility matches a stratified
//     Monte Carlo cone (spherical light) reference with analytic ray/primitive intersection.
// Hardware-only row (not measurable here): "SDF shadows < 2 ms".

#include <fuse/core/init.hpp>
#include <fuse/renderer/shadow/csm.hpp>
#include <fuse/renderer/shadow/sdf_soft_shadow.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::math::Vec3;
using fuse::renderer::CascadeLightSpaceLayout;
using fuse::renderer::CascadeLightSpaceMatrices;
using fuse::renderer::CascadeShadowSampling;
using fuse::renderer::CascadedShadowMapDesc;
using fuse::renderer::CascadedShadowMapLayout;
using fuse::renderer::ShadowCameraParams;
using fuse::renderer::kCascadeCount;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------
// Independent double-precision helpers
// ---------------------------------------------------------------------------------------------

struct D3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

D3 d3(const Vec3& v) { return {v.x, v.y, v.z}; }
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 normalize(D3 a) {
    const double len = std::sqrt(dot(a, a));
    return {a.x / len, a.y / len, a.z / len};
}

/// Gauss-Jordan inverse of a column-major 4x4 matrix (double precision).
bool invert4(const std::array<f32, 16>& in, double out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = in[c * 4 + r];
            a[r][c + 4] = (r == c) ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-20) {
            return false;
        }
        for (int c = 0; c < 8; ++c) {
            std::swap(a[col][c], a[pivot][c]);
        }
        const double inv = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != col) {
                const double f = a[r][col];
                for (int c = 0; c < 8; ++c) {
                    a[r][c] -= f * a[col][c];
                }
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[c * 4 + r] = a[r][c + 4];
        }
    }
    return true;
}

D3 transformH(const double m[16], double x, double y, double z) {
    const double w = m[3] * x + m[7] * y + m[11] * z + m[15];
    return {(m[0] * x + m[4] * y + m[8] * z + m[12]) / w, (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
            (m[2] * x + m[6] * y + m[10] * z + m[14]) / w};
}

struct Box {
    D3 lo, hi;
};

/// Slab test; returns entry distance in (tMin, tMax) or a negative value when missed.
double rayBox(const D3& o, const D3& d, const Box& b, double tMin, double tMax) {
    const double os[3] = {o.x, o.y, o.z};
    const double ds[3] = {d.x, d.y, d.z};
    const double lo[3] = {b.lo.x, b.lo.y, b.lo.z};
    const double hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    double t0 = tMin;
    double t1 = tMax;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ds[i]) < 1e-15) {
            if (os[i] < lo[i] || os[i] > hi[i]) {
                return -1.0;
            }
            continue;
        }
        double ta = (lo[i] - os[i]) / ds[i];
        double tb = (hi[i] - os[i]) / ds[i];
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

double raySphere(const D3& o, const D3& d, const D3& c, double radius, double tMin) {
    const D3 oc = o - c;
    const double b = dot(oc, d);
    const double cc = dot(oc, oc) - radius * radius;
    const double disc = b * b - cc;
    if (disc < 0.0) {
        return -1.0;
    }
    const double s = std::sqrt(disc);
    const double t0 = -b - s;
    if (t0 > tMin) {
        return t0;
    }
    const double t1 = -b + s;
    return t1 > tMin ? t1 : -1.0;
}

struct Lcg {
    unsigned long long state = 0x9E3779B97F4A7C15ull;
    double next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>((state >> 11) & ((1ull << 53) - 1ull)) / static_cast<double>(1ull << 53);
    }
};

// ---------------------------------------------------------------------------------------------
// CSM scene: ground plane y = 0 + a long wall running along the view axis through all cascades.
// ---------------------------------------------------------------------------------------------

const Box kWall{{-0.5, 0.0, -180.0}, {0.5, 4.0, 15.0}};

ShadowCameraParams makeCsmCamera() {
    ShadowCameraParams camera{};
    camera.position = {0.f, 6.f, 20.f};
    camera.forward = Vec3{0.f, -0.15f, -1.f}.normalized();
    camera.nearPlane = 0.1f;
    camera.farPlane = 200.f;
    camera.fovDegrees = 60.f;
    camera.aspect = 16.f / 9.f;
    return camera;
}

const Vec3 kSunTravel = Vec3{1.f, -1.2f, -0.3f}.normalized();

/// Analytic occlusion: ray from the receiver towards the sun against the wall.
bool analyticShadowed(const D3& p) {
    const D3 toLight = d3(kSunTravel) * -1.0;
    return rayBox(p + toLight * 1e-6, toLight, kWall, 1e-6, 1e9) >= 0.0;
}

/// One cascade "rendered" as a depth map: texels are ray-cast lazily through the inverted light
/// view-projection (orthographic column per texel centre) against the plane + wall scene.
struct CpuCascadeMap {
    CascadeLightSpaceMatrices matrices{};
    double invViewProj[16]{};
    u32 resolution = 0;
    f32 texelWorld = 0.f;

    bool init(const CascadeLightSpaceMatrices& m, u32 res) {
        matrices = m;
        resolution = res;
        texelWorld = CascadeShadowSampling::texelWorldSize(m.orthoBounds, res);
        return m.valid && invert4(m.lightViewProj.data, invViewProj);
    }

    double texelDepth(int ix, int iy) const {
        const double ndcX = (ix + 0.5) / resolution * 2.0 - 1.0;
        const double ndcY = (iy + 0.5) / resolution * 2.0 - 1.0;
        const D3 a = transformH(invViewProj, ndcX, ndcY, 0.0);
        const D3 b = transformH(invViewProj, ndcX, ndcY, 1.0);
        const D3 d = b - a; // parameter s in [0,1] == NDC depth (orthographic)
        double best = 1.0;
        const double sBox = rayBox(a, d, kWall, 0.0, 1.0);
        if (sBox >= 0.0) {
            best = std::min(best, sBox);
        }
        if (std::fabs(d.y) > 1e-15) {
            const double sPlane = -a.y / d.y;
            if (sPlane >= 0.0 && sPlane <= 1.0) {
                best = std::min(best, sPlane);
            }
        }
        return best;
    }

    /// Nearest-texel shadow test with normal-offset + constant depth bias (the deferred lookup).
    bool shadowed(const D3& p, const D3& normal) const {
        const D3 biased = p + normal * (1.5 * texelWorld);
        const auto coord = CascadeShadowSampling::projectToCascade(
            matrices.lightViewProj, {static_cast<f32>(biased.x), static_cast<f32>(biased.y),
                                     static_cast<f32>(biased.z)},
            resolution);
        const int ix = std::clamp(static_cast<int>(std::floor(coord.texelX)), 0, static_cast<int>(resolution) - 1);
        const int iy = std::clamp(static_cast<int>(std::floor(coord.texelY)), 0, static_cast<int>(resolution) - 1);
        const double depthRange = matrices.orthoBounds.farPlane - matrices.orthoBounds.nearPlane;
        return coord.depth > texelDepth(ix, iy) + 0.02 / depthRange;
    }
};

bool buildCascadeMaps(const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera, CpuCascadeMap maps[]) {
    CascadeLightSpaceMatrices matrices[kCascadeCount]{};
    if (CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(desc, camera, kSunTravel, matrices) !=
        kCascadeCount) {
        return false;
    }
    for (u32 c = 0; c < kCascadeCount; ++c) {
        if (!maps[c].init(matrices[c], desc.resolution)) {
            return false;
        }
    }
    return true;
}

/// Ground point with the given view depth and world x (camera looks down -Z with zero x forward).
D3 groundPointAtViewDepth(const ShadowCameraParams& camera, double viewDepth, double x) {
    const D3 f = normalize(d3(camera.forward));
    const D3 c = d3(camera.position);
    const double z = c.z + (viewDepth - (x - c.x) * f.x - (0.0 - c.y) * f.y) / f.z;
    return {x, 0.0, z};
}

bool insideCameraFrustum(const ShadowCameraParams& camera, const D3& p) {
    const D3 f = normalize(d3(camera.forward));
    const D3 right = normalize(cross(f, {0.0, 1.0, 0.0}));
    const D3 up = cross(right, f);
    const D3 rel = p - d3(camera.position);
    const double z = dot(rel, f);
    if (z < camera.nearPlane || z > camera.farPlane) {
        return false;
    }
    const double tanHalf = std::tan(camera.fovDegrees * 0.5 * kPi / 180.0);
    return std::fabs(dot(rel, up)) <= z * tanHalf && std::fabs(dot(rel, right)) <= z * tanHalf * camera.aspect;
}

/// True when the analytic answer is the same everywhere within `margin` on the ground (away from edges).
bool analyticClear(const D3& p, double margin) {
    const bool center = analyticShadowed(p);
    const D3 offsets[8] = {{margin, 0, 0}, {-margin, 0, 0}, {0, 0, margin}, {0, 0, -margin},
                           {margin, 0, margin}, {-margin, 0, margin}, {margin, 0, -margin}, {-margin, 0, -margin}};
    for (const D3& o : offsets) {
        if (analyticShadowed(p + o) != center) {
            return false;
        }
    }
    // Skip receivers under the wall footprint.
    return !(p.x > kWall.lo.x - margin && p.x < kWall.hi.x + margin);
}

// ---------------------------------------------------------------------------------------------
// Gate: CSM correct across all 4 cascades, no seam
// ---------------------------------------------------------------------------------------------

void testCascadeSplitsMatchClosedForm() {
    const ShadowCameraParams camera = makeCsmCamera();
    const CascadedShadowMapDesc desc{};
    const double n = camera.nearPlane;
    const double f = camera.farPlane;
    const double fractions[4] = {0.05, 0.15, 0.4, 1.0};
    double maxErr = 0.0;
    for (u32 c = 0; c < kCascadeCount; ++c) {
        const double expectedFar = n + fractions[c] * (f - n);
        const double expectedNear = c == 0 ? n : n + fractions[c - 1] * (f - n);
        maxErr = std::max(maxErr, std::fabs(CascadedShadowMapLayout::computeCascadeFarZ(c, desc, camera) - expectedFar));
        maxErr = std::max(maxErr, std::fabs(CascadedShadowMapLayout::computeCascadeNearZ(c, desc, camera) - expectedNear));
    }
    expectTrue(maxErr < 1e-3, "CSMDesc split fractions map to near + frac * (far - near)");

    // Practical (PSSM) scheme: C_i = lambda * uniform + (1 - lambda) * logarithmic.
    fuse::renderer::CascadeSplitParams params{};
    params.scheme = fuse::renderer::CascadeSplitScheme::Practical;
    params.lambda = 0.6f;
    double practicalErr = 0.0;
    for (u32 c = 0; c < kCascadeCount; ++c) {
        const double t = static_cast<double>(c + 1) / kCascadeCount;
        const double expected = 0.6 * (n + (f - n) * t) + 0.4 * n * std::pow(f / n, t);
        practicalErr = std::max(practicalErr,
                                std::fabs(CascadedShadowMapLayout::computeSplitDistance(c, params, camera) - expected));
    }
    expectTrue(practicalErr < 1e-2, "practical split distances match closed-form PSSM");
    std::printf("[csm] split max error: fractions %.2e, practical %.2e\n", maxErr, practicalErr);
}

void testFrustumSlicesProjectInsideCascade() {
    const Vec3 suns[] = {kSunTravel, {0.f, -1.f, 0.f}, Vec3{0.3f, -0.4f, 0.8f}.normalized(),
                         Vec3{-0.9f, -0.1f, 0.2f}.normalized()};
    const Vec3 forwards[] = {Vec3{0.f, -0.15f, -1.f}.normalized(), {0.f, -1.f, 0.f},
                             Vec3{0.7f, 0.2f, 0.3f}.normalized()};
    u32 outside = 0;
    u32 samples = 0;
    double maxNdc = 0.0;
    for (int stabilise = 0; stabilise < 2; ++stabilise) {
        for (const Vec3& sun : suns) {
            for (const Vec3& fwd : forwards) {
                ShadowCameraParams camera = makeCsmCamera();
                camera.position = {13.7f, 4.2f, -8.9f};
                camera.forward = fwd;
                CascadedShadowMapDesc desc{};
                desc.stabilise = stabilise != 0;
                CascadeLightSpaceMatrices matrices[kCascadeCount]{};
                CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(desc, camera, sun, matrices);

                const D3 f = normalize(d3(camera.forward));
                const D3 upRef = std::fabs(f.y) > 0.999 ? D3{0, 0, 1} : D3{0, 1, 0};
                const D3 right = normalize(cross(f, upRef));
                const D3 up = cross(right, f);
                const double tanHalf = std::tan(camera.fovDegrees * 0.5 * kPi / 180.0);
                for (u32 c = 0; c < kCascadeCount; ++c) {
                    expectTrue(matrices[c].valid, "cascade matrices valid for every camera/sun");
                    const auto range = CascadedShadowMapLayout::computeCascadeRange(c, desc, camera);
                    constexpr int kSteps = 6;
                    for (int iz = 0; iz <= kSteps; ++iz) {
                        const double z = range.nearZ + (range.farZ - range.nearZ) * iz / kSteps;
                        for (int ix = 0; ix <= kSteps; ++ix) {
                            for (int iy = 0; iy <= kSteps; ++iy) {
                                const double sx = (2.0 * ix / kSteps - 1.0) * z * tanHalf * camera.aspect;
                                const double sy = (2.0 * iy / kSteps - 1.0) * z * tanHalf;
                                const D3 p = d3(camera.position) + f * z + right * sx + up * sy;
                                const auto coord = CascadeShadowSampling::projectToCascade(
                                    matrices[c].lightViewProj,
                                    {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)},
                                    desc.resolution);
                                ++samples;
                                maxNdc = std::max({maxNdc, std::fabs(coord.u * 2.0 - 1.0),
                                                   std::fabs(coord.v * 2.0 - 1.0)});
                                // Stabilised: strictly inside (one texel of padding). Tight AABB fit: frustum
                                // corners lie on the map border by construction, allow float rounding.
                                const double tol = stabilise ? 0.0 : 1e-5;
                                const bool inside = coord.u >= -tol && coord.u <= 1.0 + tol && coord.v >= -tol &&
                                                    coord.v <= 1.0 + tol && coord.depth >= -tol &&
                                                    coord.depth <= 1.0 + tol;
                                if (!inside) {
                                    ++outside;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("[csm] slice containment: %u/%u samples outside, max |ndc.xy| = %.4f\n", outside, samples, maxNdc);
    expectTrue(outside == 0u, "every frustum-slice point projects inside its cascade shadow map");
}

void testCascadeBoundarySelectionAndNoSeam() {
    const ShadowCameraParams camera = makeCsmCamera();
    CascadedShadowMapDesc desc{};
    fuse::renderer::CascadedShadowMapData data{};
    expectTrue(fuse::renderer::CascadeShadowDataLayout::populateCascadeShadowData(desc, camera, kSunTravel,
                                                                                 kCascadeCount, data) == 4u,
               "all four cascades populated");

    CpuCascadeMap maps[kCascadeCount];
    expectTrue(buildCascadeMaps(desc, camera, maps), "cpu cascade maps built");
    const D3 up{0.0, 1.0, 0.0};

    u32 totalMismatch = 0;
    u32 totalChecked = 0;
    for (u32 b = 0; b + 1 < kCascadeCount; ++b) {
        const double boundary = data.cascadeFarZ[b];
        const double margin = 3.0 * maps[b + 1].texelWorld; // coarser cascade texels
        u32 selectionErrors = 0;
        u32 mismatches = 0;
        u32 litNear = 0, shadowNear = 0, litFar = 0, shadowFar = 0;
        u32 checked = 0;
        for (int side = 0; side < 2; ++side) {
            for (int k = 1; k <= 4; ++k) {
                const double depth = boundary + (side == 0 ? -1.0 : 1.0) * 0.002 * k * boundary;
                for (double x = -3.0; x <= 12.0; x += 0.05) {
                    const D3 p = groundPointAtViewDepth(camera, depth, x);
                    if (!insideCameraFrustum(camera, p) || !analyticClear(p, margin)) {
                        continue;
                    }
                    const f32 viewDepth = CascadeShadowSampling::computeViewDepth(
                        camera, {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)});
                    const u32 selected = CascadeShadowSampling::selectCascade(data, viewDepth, kCascadeCount);
                    if (selected != b + static_cast<u32>(side)) {
                        ++selectionErrors;
                    }
                    const bool truth = analyticShadowed(p);
                    const bool inSelected = maps[selected].shadowed(p, up);
                    // Both cascades adjacent to the boundary must agree (no seam either way).
                    const bool inNear = maps[b].shadowed(p, up);
                    const bool inFar = maps[b + 1].shadowed(p, up);
                    ++checked;
                    if (inSelected != truth || inNear != truth || inFar != truth) {
                        ++mismatches;
                    }
                    if (side == 0) {
                        (inSelected ? shadowNear : litNear)++;
                    } else {
                        (inSelected ? shadowFar : litFar)++;
                    }
                }
            }
        }
        std::printf("[csm] boundary %u@%.2f: checked %u, mismatches %u, selection errors %u, "
                    "near lit/shadow %u/%u, far lit/shadow %u/%u\n",
                    b, boundary, checked, mismatches, selectionErrors, litNear, shadowNear, litFar, shadowFar);
        expectTrue(selectionErrors == 0u, "cascade selection flips exactly at the far-Z boundary");
        expectTrue(mismatches == 0u, "shadow answer identical to analytic on both sides of the boundary");
        expectTrue(litNear > 0u && shadowNear > 0u && litFar > 0u && shadowFar > 0u,
                   "shadow edge crosses the boundary (lit and shadowed samples on both sides)");
        totalMismatch += mismatches;
        totalChecked += checked;
    }

    // Whole-scene sweep through all four cascades.
    Lcg rng{};
    u32 perCascade[kCascadeCount]{};
    u32 sweepMismatch = 0;
    for (int i = 0; i < 20000; ++i) {
        const double depth = 1.0 + rng.next() * 195.0;
        const double x = -3.0 + rng.next() * 15.0;
        const D3 p = groundPointAtViewDepth(camera, depth, x);
        if (!insideCameraFrustum(camera, p)) {
            continue;
        }
        const f32 viewDepth = CascadeShadowSampling::computeViewDepth(
            camera, {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)});
        const u32 c = CascadeShadowSampling::selectCascade(data, viewDepth, kCascadeCount);
        if (c >= kCascadeCount || !analyticClear(p, 3.0 * maps[c].texelWorld)) {
            continue;
        }
        ++perCascade[c];
        if (maps[c].shadowed(p, up) != analyticShadowed(p)) {
            ++sweepMismatch;
        }
    }
    std::printf("[csm] sweep: per-cascade samples %u/%u/%u/%u, mismatches %u; boundary checks %u, mismatches %u\n",
                perCascade[0], perCascade[1], perCascade[2], perCascade[3], sweepMismatch, totalChecked, totalMismatch);
    expectTrue(perCascade[0] > 0 && perCascade[1] > 0 && perCascade[2] > 0 && perCascade[3] > 0,
               "sweep covers all four cascades");
    expectTrue(sweepMismatch == 0u, "CSM lookup matches analytic shadow in all four cascades");
}

// ---------------------------------------------------------------------------------------------
// Gate: stabilisation eliminates shimmer (frame diff)
// ---------------------------------------------------------------------------------------------

struct ShimmerResult {
    double maxFractionalDrift = 0.0;
    u32 changedPixels = 0;
    u32 comparedPixels = 0;
};

ShimmerResult measureShimmer(bool stabilise) {
    CascadedShadowMapDesc desc{};
    desc.stabilise = stabilise;
    const ShadowCameraParams base = makeCsmCamera();

    // Receiver "image": fixed world points on the ground around the shadow edge in cascade 1.
    std::vector<D3> receivers;
    for (double z = -2.0; z >= -8.0; z -= 0.173) {
        for (double x = 0.6; x <= 6.0; x += 0.0437) {
            receivers.push_back({x, 0.0, z});
        }
    }
    const u32 cascade = 1u;
    const D3 up{0.0, 1.0, 0.0};

    ShimmerResult result{};
    std::vector<char> previous;
    std::vector<double> texel0X, texel0Y;
    Lcg rng{};
    for (int frame = 0; frame < 24; ++frame) {
        ShadowCameraParams camera = base;
        // Sub-texel jitter + slow drift + small yaw/pitch (fractions of a texel / degree per frame).
        const f32 dx = static_cast<f32>(0.013 * frame + 0.004 * rng.next());
        const f32 dz = static_cast<f32>(-0.021 * frame + 0.004 * rng.next());
        camera.position = {base.position.x + dx, base.position.y + 0.003f * frame, base.position.z + dz};
        const double yaw = 0.07 * frame * kPi / 180.0;
        const double pitch = -0.15 + 0.0009 * frame;
        camera.forward = Vec3{static_cast<f32>(-std::sin(yaw)), static_cast<f32>(pitch),
                              static_cast<f32>(-std::cos(yaw))}.normalized();

        const CascadeLightSpaceMatrices m =
            CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(cascade, desc, camera, kSunTravel);
        CpuCascadeMap map;
        map.init(m, desc.resolution);

        std::vector<char> image(receivers.size());
        for (size_t i = 0; i < receivers.size(); ++i) {
            const D3& p = receivers[i];
            const auto coord = CascadeShadowSampling::projectToCascade(
                m.lightViewProj, {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)},
                desc.resolution);
            if (frame == 0) {
                texel0X.push_back(coord.texelX);
                texel0Y.push_back(coord.texelY);
            } else {
                const double ddx = coord.texelX - texel0X[i];
                const double ddy = coord.texelY - texel0Y[i];
                result.maxFractionalDrift = std::max({result.maxFractionalDrift, std::fabs(ddx - std::round(ddx)),
                                                      std::fabs(ddy - std::round(ddy))});
            }
            image[i] = map.shadowed(p, up) ? 1 : 0;
        }
        if (!previous.empty()) {
            for (size_t i = 0; i < image.size(); ++i) {
                ++result.comparedPixels;
                if (image[i] != previous[i]) {
                    ++result.changedPixels;
                }
            }
        }
        previous = std::move(image);
    }
    return result;
}

void testStabilisationEliminatesShimmer() {
    const ShimmerResult stable = measureShimmer(true);
    const ShimmerResult unstable = measureShimmer(false);
    std::printf("[csm] shimmer stabilise=true: max fractional texel drift %.2e, frame diff %u/%u pixels\n",
                stable.maxFractionalDrift, stable.changedPixels, stable.comparedPixels);
    std::printf("[csm] shimmer stabilise=false: max fractional texel drift %.3f, frame diff %u/%u pixels\n",
                unstable.maxFractionalDrift, unstable.changedPixels, unstable.comparedPixels);
    expectTrue(stable.maxFractionalDrift < 2e-3, "stabilised texel centres move by exact integer texel offsets");
    expectTrue(stable.changedPixels == 0u, "stabilised CSM frame diff is zero on a static scene");
    expectTrue(unstable.maxFractionalDrift > 0.05, "unstabilised texel grid drifts by sub-texel amounts");
    expectTrue(unstable.changedPixels > 0u, "unstabilised CSM shimmers (non-zero frame diff)");

    // Rotation invariance of the cascade extent (bounding sphere radius).
    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera = makeCsmCamera();
    f32 r0 = 0.f;
    CascadeLightSpaceLayout::computeCascadeBoundingSphere(2u, desc, camera, r0);
    camera.forward = Vec3{0.6f, 0.3f, -0.2f}.normalized();
    camera.position = {-40.f, 3.f, 7.f};
    f32 r1 = 0.f;
    CascadeLightSpaceLayout::computeCascadeBoundingSphere(2u, desc, camera, r1);
    expectTrue(r0 == r1, "cascade bounding sphere radius independent of camera pose");
}

// ---------------------------------------------------------------------------------------------
// SDF soft shadows
// ---------------------------------------------------------------------------------------------

f32 sdBox(const Vec3& p, const Vec3& center, const Vec3& halfExtent) {
    const Vec3 q{std::fabs(p.x - center.x) - halfExtent.x, std::fabs(p.y - center.y) - halfExtent.y,
                 std::fabs(p.z - center.z) - halfExtent.z};
    const Vec3 qa{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
    return qa.length() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.f);
}

f32 sdSphere(const Vec3& p, const Vec3& c, f32 r) { return (p - c).length() - r; }

f32 sdCapsule(const Vec3& p, const Vec3& a, const Vec3& b, f32 r) {
    const Vec3 pa = p - a;
    const Vec3 ba = b - a;
    const f32 h = std::clamp(pa.dot(ba) / ba.dot(ba), 0.f, 1.f);
    return (pa - ba * h).length() - r;
}

/// Exact half-line vs capsule test: minimum distance between the ray and the capsule axis segment
/// (closed-form segment/segment distance with a long ray segment) compared to the radius.
bool rayHitsCapsule(const D3& o, const D3& d, const D3& a, const D3& b, double radius) {
    const double rayLen = 1e3;
    const D3 d1 = d * rayLen;
    const D3 d2 = b - a;
    const D3 r = o - a;
    const double aa = dot(d1, d1), ee = dot(d2, d2), ff = dot(d2, r);
    const double c = dot(d1, r), bb = dot(d1, d2);
    const double denom = aa * ee - bb * bb;
    double s = denom > 1e-18 ? std::clamp((bb * ff - c * ee) / denom, 0.0, 1.0) : 0.0;
    double t = (bb * s + ff) / ee;
    if (t < 0.0) {
        t = 0.0;
        s = std::clamp(-c / aa, 0.0, 1.0);
    } else if (t > 1.0) {
        t = 1.0;
        s = std::clamp((bb - c) / aa, 0.0, 1.0);
    }
    const D3 diff = (o + d1 * s) - (a + d2 * t);
    return dot(diff, diff) < radius * radius;
}

/// Uniform-solid-angle direction inside a cone of half-angle `theta` around `axis` (stratified).
D3 coneDirection(const D3& axis, double theta, double u1, double u2) {
    const double cosTheta = 1.0 - u1 * (1.0 - std::cos(theta));
    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
    const double phi = 2.0 * kPi * u2;
    const D3 t = normalize(std::fabs(axis.x) > 0.5 ? cross(axis, {0, 1, 0}) : cross(axis, {1, 0, 0}));
    const D3 b = cross(axis, t);
    return normalize(t * (sinTheta * std::cos(phi)) + b * (sinTheta * std::sin(phi)) + axis * cosTheta);
}

struct LineFit {
    double slope = 0.0;
    double intercept = 0.0;
    double r2 = 0.0;
};

LineFit fitLine(const std::vector<double>& xs, const std::vector<double>& ys) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = static_cast<double>(xs.size());
    for (size_t i = 0; i < xs.size(); ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
    }
    LineFit fit{};
    fit.slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    fit.intercept = (sy - fit.slope * sx) / n;
    double ssRes = 0, ssTot = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        const double e = ys[i] - (fit.slope * xs[i] + fit.intercept);
        ssRes += e * e;
        ssTot += (ys[i] - sy / n) * (ys[i] - sy / n);
    }
    fit.r2 = 1.0 - ssRes / ssTot;
    return fit;
}

void testPenumbraWidthProportionalToOccluderDistance() {
    const f32 k = 16.f;
    const f32 thickness = 1.f;
    fuse::renderer::SdfSoftShadowParams params{};
    params.tMin = 0.01f;
    params.tMax = 50.f;
    params.penumbraK = k;
    params.maxSteps = 512u;
    params.stepScale = 0.25f;
    const f32 heights[] = {0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f};
    // Straight edge: visibility = disc fraction at r = k * x / D (small angle), D = distance from the
    // receiver to the silhouette edge. Overhead light + solid box (x < 0, y in [H, H + T]): the umbra
    // side is bounded by the bottom edge (D = H), the lit side by the top edge (D = H + T) — the same
    // geometry a physical disc light gives.
    auto invert = [](double target) {
        double lo = -1.0, hi = 1.0;
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            (fuse::renderer::sdfDiscVisibility(static_cast<f32>(mid)) < target ? lo : hi) = mid;
        }
        return 0.5 * (lo + hi);
    };
    const double r10 = invert(0.1);
    const double r90 = invert(0.9);

    std::vector<double> innerD, innerW, outerD, outerW, fullW;
    for (const f32 h : heights) {
        auto scene = [h, thickness](const Vec3& p) {
            return std::min(sdBox(p, {-20.f, h + 0.5f * thickness, 0.f}, {20.f, 0.5f * thickness, 20.f}), p.y);
        };
        auto shadowAt = [&](double x) {
            return fuse::renderer::sdfSoftShadow(scene, {static_cast<f32>(x), 0.f, 0.f}, {0.f, 1.f, 0.f}, params);
        };
        auto crossing = [&](double target) {
            double lo = -2.0 * (h + thickness) / k;
            double hi = 2.0 * (h + thickness) / k;
            for (int i = 0; i < 50; ++i) {
                const double mid = 0.5 * (lo + hi);
                (shadowAt(mid) < target ? lo : hi) = mid;
            }
            return 0.5 * (lo + hi);
        };
        const double x10 = crossing(0.1);
        const double x50 = crossing(0.5);
        const double x90 = crossing(0.9);
        innerD.push_back(h);
        innerW.push_back(x50 - x10);
        outerD.push_back(h + thickness);
        outerW.push_back(x90 - x50);
        fullW.push_back(x90 - x10);
        std::printf("[sdf] penumbra H=%.2f: x10 %.5f x50 %.5f x90 %.5f | inner/H %.5f outer/(H+T) %.5f\n", h, x10,
                    x50, x90, (x50 - x10) / h, (x90 - x50) / (h + thickness));
    }

    const LineFit inner = fitLine(innerD, innerW);
    const LineFit outer = fitLine(outerD, outerW);
    const double innerExpected = -r10 / k;
    const double outerExpected = r90 / k;
    std::printf("[sdf] umbra-side width vs distance: slope %.5f (expected %.5f), intercept %.2e, R^2 %.7f\n",
                inner.slope, innerExpected, inner.intercept, inner.r2);
    std::printf("[sdf] lit-side width vs distance:   slope %.5f (expected %.5f), intercept %.2e, R^2 %.7f\n",
                outer.slope, outerExpected, outer.intercept, outer.r2);
    expectTrue(inner.r2 > 0.9999 && outer.r2 > 0.9999, "penumbra width is linear in occluder distance");
    expectTrue(std::fabs(inner.intercept) < 0.01 * innerW.back() && std::fabs(outer.intercept) < 0.01 * outerW.back(),
               "penumbra width is proportional (zero width at zero distance)");
    // Umbra side: inside the occluder the clearance is the interior ball bound |h| / t, reached one
    // depth |h| past the silhouette edge, i.e. |x| / (H + |x|) instead of |x| / H — a +k^-1 * |r|
    // (~4.5% here) widening of the inner half. Lit side is exact up to the step-scale bound.
    expectTrue(std::fabs(inner.slope - innerExpected) < 0.05 * innerExpected &&
                   std::fabs(outer.slope - outerExpected) < 0.02 * outerExpected,
               "penumbra slope matches the equivalent spherical light (theta = 1/k)");
}

void testSdfShadowMatchesReferencePathTracer() {
    const f32 k = 12.f;
    const double theta = fuse::renderer::sdfPenumbraLightAngularRadius(k);
    const D3 toLight = normalize({0.3, 1.0, 0.2});
    // Solid, smooth occluders: a sphere and an oblique capsule (the SDF inner-penumbra estimate needs
    // occluders that are thick relative to the penumbra; see sdf_soft_shadow.hpp).
    const D3 sphereC{0.0, 1.5, 0.0};
    const double sphereR = 0.6;
    const D3 capA{1.3, 0.9, -0.9};
    const D3 capB{2.5, 1.3, 0.7};
    const double capR = 0.45;

    auto scene = [&](const Vec3& p) {
        const f32 s = sdSphere(p, {0.f, 1.5f, 0.f}, 0.6f);
        const f32 c = sdCapsule(p, {1.3f, 0.9f, -0.9f}, {2.5f, 1.3f, 0.7f}, 0.45f);
        return std::min(std::min(s, c), p.y);
    };

    fuse::renderer::SdfSoftShadowParams params{};
    params.tMin = 0.01f;
    params.tMax = 50.f;
    params.penumbraK = k;
    params.maxSteps = 512u;
    params.stepScale = 0.25f;

    constexpr int kRes = 64;
    constexpr int kStrata = 40; // 1600 stratified samples per pixel
    Lcg rng{};
    double maxErr = 0.0, sumErr = 0.0, maxErrDefault = 0.0;
    u32 penumbraPixels = 0, over5 = 0;
    for (int j = 0; j < kRes; ++j) {
        for (int i = 0; i < kRes; ++i) {
            const double x = -2.0 + 5.0 * (i + 0.5) / kRes;
            const double z = -2.0 + 4.0 * (j + 0.5) / kRes;
            const D3 p{x, 0.0, z};

            // Reference: Monte Carlo visibility of a uniform spherical light of angular radius theta.
            u32 visible = 0;
            for (int a = 0; a < kStrata; ++a) {
                for (int b = 0; b < kStrata; ++b) {
                    const D3 d = coneDirection(toLight, theta, (a + rng.next()) / kStrata, (b + rng.next()) / kStrata);
                    const bool hit = raySphere(p, d, sphereC, sphereR, 1e-6) > 0.0 || rayHitsCapsule(p, d, capA, capB, capR);
                    if (!hit) {
                        ++visible;
                    }
                }
            }
            const double reference = static_cast<double>(visible) / (kStrata * kStrata);
            const double sdf = fuse::renderer::sdfSoftShadow(
                scene, {static_cast<f32>(x), 0.f, static_cast<f32>(z)},
                {static_cast<f32>(toLight.x), static_cast<f32>(toLight.y), static_cast<f32>(toLight.z)}, params);
            // Luminance of a Lambertian receiver = E * cos * visibility; the error normalised to the
            // unshadowed luminance of the pixel is the visibility difference.
            const double err = std::fabs(sdf - reference);
            // Informational: spec-default march (step = h, 64 steps).
            const double sdfDefault = fuse::renderer::sdfSoftShadow(
                scene, {static_cast<f32>(x), 0.f, static_cast<f32>(z)},
                {static_cast<f32>(toLight.x), static_cast<f32>(toLight.y), static_cast<f32>(toLight.z)}, 0.01f, 50.f,
                k);
            maxErrDefault = std::max(maxErrDefault, std::fabs(sdfDefault - reference));
            maxErr = std::max(maxErr, err);
            sumErr += err;
            if (reference > 0.02 && reference < 0.98) {
                ++penumbraPixels;
            }
            if (err > 0.05) {
                if (over5 < 12) {
                    std::printf("  px (%.3f, %.3f) sdf %.4f ref %.4f\n", x, z, sdf, reference);
                }
                ++over5;
            }
        }
    }
    std::printf("[sdf] vs path tracer (%dx%d px, %d spp, theta=%.4f rad): max err %.4f, mean err %.5f, "
                "penumbra px %u, px > 5%% %u\n",
                kRes, kRes, kStrata * kStrata, theta, maxErr, sumErr / (kRes * kRes), penumbraPixels, over5);
    std::printf("[sdf] vs path tracer with default march (step scale 1, 64 steps): max err %.4f (informational)\n",
                maxErrDefault);
    expectTrue(penumbraPixels > 100u, "reference image contains a substantial penumbra region");
    expectTrue(maxErr <= 0.05, "SDF soft shadow within 5% luminance of reference path tracer per pixel");
}

void testSdfSoftShadowBasics() {
    auto scene = [](const Vec3& p) { return sdSphere(p, {0.f, 2.f, 0.f}, 0.5f); };
    const f32 lit = fuse::renderer::sdfSoftShadow(scene, {5.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.01f, 20.f, 16.f);
    const f32 umbra = fuse::renderer::sdfSoftShadow(scene, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.01f, 20.f, 16.f);
    expectTrue(lit > 0.999f, "unoccluded ray fully lit");
    expectTrue(umbra < 1e-3f, "ray through occluder centre fully shadowed");
    expectTrue(std::fabs(fuse::renderer::sdfDiscVisibility(0.f) - 0.5f) < 1e-6f, "edge through light centre = 50%");
}

} // namespace

int main() {
    fuse::core::initialize();

    testCascadeSplitsMatchClosedForm();
    testFrustumSlicesProjectInsideCascade();
    testCascadeBoundarySelectionAndNoSeam();
    testStabilisationEliminatesShimmer();
    testSdfSoftShadowBasics();
    testPenumbraWidthProportionalToOccluderDistance();
    testSdfShadowMatchesReferencePathTracer();
    std::printf("[hw-only] SDF shadows < 2 ms: requires GPU timing (CUDA events) — not measurable in CI\n");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b5_shadows_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b5_shadows_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
