// B5.12 gate rows for B5.4 clustered deferred shading (CPU reference of the cull + shade kernels):
//  1. Clustered light culler assigns zero lights to clusters with no light overlap — the rebuilt
//     light grid (offset, count) + flat light list is compared against an independent brute force.
//  2. 1000 point lights — deferred shading correct, no light leaking. Point lights carry no
//     shadowing in the spec, so "no leaking" means: a light contributes exactly zero outside its
//     range sphere (windowed falloff), lights behind a wall thicker than their range add exactly
//     zero to the visible side, and the cluster lists never omit a light that reaches a pixel
//     (clustered shading is bit-identical to looping all lights).
//  3. Cull + shade < 3 ms at 1080p is GPU-only; the CPU cull time for 1000 lights is reported.

#include <fuse/core/init.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_shading.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using namespace fuse::renderer;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// Independent double-precision camera math (column-major 4x4, same layout as fuse::Camera).

struct Mat4d {
    double m[16] = {};
};

Mat4d lookAtD(const Vec3& eye, const Vec3& forward, const Vec3& upHint) {
    double f[3] = {forward.x, forward.y, forward.z};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) {
        v /= fl;
    }
    double s[3] = {f[1] * upHint.z - f[2] * upHint.y, f[2] * upHint.x - f[0] * upHint.z,
                   f[0] * upHint.y - f[1] * upHint.x};
    const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (double& v : s) {
        v /= sl;
    }
    const double u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4d r{};
    r.m[0] = s[0];
    r.m[4] = s[1];
    r.m[8] = s[2];
    r.m[1] = u[0];
    r.m[5] = u[1];
    r.m[9] = u[2];
    r.m[2] = -f[0];
    r.m[6] = -f[1];
    r.m[10] = -f[2];
    r.m[12] = -(s[0] * eye.x + s[1] * eye.y + s[2] * eye.z);
    r.m[13] = -(u[0] * eye.x + u[1] * eye.y + u[2] * eye.z);
    r.m[14] = f[0] * eye.x + f[1] * eye.y + f[2] * eye.z;
    r.m[15] = 1.0;
    return r;
}

/// Reversed-Z infinite-far projection (identical to fuse::Camera's perspectiveReversedZ).
Mat4d reversedZInfiniteD(double fovY, double aspect, double nearZ) {
    const double f = 1.0 / std::tan(fovY * 0.5);
    Mat4d r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[11] = -1.0;
    r.m[14] = nearZ;
    return r;
}

/// Standard [0,1] finite projection (right-handed, -Z forward).
Mat4d standardZD(double fovY, double aspect, double nearZ, double farZ) {
    const double f = 1.0 / std::tan(fovY * 0.5);
    Mat4d r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = farZ / (nearZ - farZ);
    r.m[11] = -1.0;
    r.m[14] = (farZ * nearZ) / (nearZ - farZ);
    return r;
}

void mulD(const Mat4d& a, const double v[4], double out[4]) {
    for (int row = 0; row < 4; ++row) {
        out[row] = a.m[0 * 4 + row] * v[0] + a.m[1 * 4 + row] * v[1] + a.m[2 * 4 + row] * v[2] +
                   a.m[3 * 4 + row] * v[3];
    }
}

/// General 4x4 inverse by Gauss-Jordan elimination with partial pivoting.
bool invertD(const Mat4d& in, Mat4d& out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = in.m[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-15) {
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
            if (r == col) {
                continue;
            }
            const double factor = a[r][col];
            for (int c = 0; c < 8; ++c) {
                a[r][c] -= factor * a[col][c];
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[c * 4 + r] = a[r][c + 4];
        }
    }
    return true;
}

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3d transformPointD(const Mat4d& m, const Vec3& p) {
    const double v[4] = {p.x, p.y, p.z, 1.0};
    double o[4];
    mulD(m, v, o);
    return {o[0] / o[3], o[1] / o[3], o[2] / o[3]};
}

/// Independent closed sphere-vs-AABB test in double precision.
bool sphereAabbD(const Vec3d& c, double r, const ClusterAABB& box) {
    if (r < 0.0) {
        return false;
    }
    auto axis = [](double v, double lo, double hi) {
        if (v < lo) {
            return lo - v;
        }
        if (v > hi) {
            return v - hi;
        }
        return 0.0;
    };
    const double dx = axis(c.x, box.minP.x, box.maxP.x);
    const double dy = axis(c.y, box.minP.y, box.maxP.y);
    const double dz = axis(c.z, box.minP.z, box.maxP.z);
    return dx * dx + dy * dy + dz * dz <= r * r;
}

// ---------------------------------------------------------------------------------------------
// Fixtures.

ClusterCameraDesc makeGateCamera() {
    ClusterCameraDesc camera{};
    camera.position = {3.f, 4.f, 15.f};
    camera.forward = {0.3f, -0.2f, -1.f};
    camera.up = {0.f, 1.f, 0.f};
    camera.fovYRadians = 1.04719755f;
    camera.nearPlane = 0.1f;
    camera.farPlane = 200.f;
    camera.screenWidth = 1920;
    camera.screenHeight = 1080;
    camera.reversedZ = true;
    return camera;
}

ClusterDesc makeGateDesc() {
    ClusterDesc desc{};
    desc.tilesX = 16;
    desc.tilesY = 9;
    desc.slicesZ = 24;
    desc.maxLightsPerCluster = 256;
    return desc;
}

/// 1000 point lights: most inside the view volume, plus deliberate edge cases (behind the camera,
/// straddling the near plane, beyond the far plane, far off to the side, zero radius).
std::vector<PointLightInput> makeGateLights(const ClusterCameraDesc& camera, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    const f32 tanY = std::tan(camera.fovYRadians * 0.5f);
    const f32 tanX = tanY * camera.aspect();

    std::vector<PointLightInput> lights;
    lights.reserve(1000);
    auto addView = [&](const Vec3& view, f32 radius) {
        PointLightInput light{};
        light.position = cluster_math::viewToWorld(camera, view);
        light.radius = radius;
        light.color = {0.2f + 0.8f * unit(rng), 0.2f + 0.8f * unit(rng), 0.2f + 0.8f * unit(rng)};
        light.intensity = 1.f + 4.f * unit(rng);
        lights.push_back(light);
    };

    for (u32 i = 0; i < 800u; ++i) {
        // Depth biased toward the camera (like real scenes), lateral spread slightly past the frustum.
        const f32 depth = 0.5f + 80.f * unit(rng) * unit(rng);
        const f32 x = (unit(rng) * 2.4f - 1.2f) * tanX * depth;
        const f32 y = (unit(rng) * 2.4f - 1.2f) * tanY * depth;
        addView({x, y, -depth}, 0.3f + 2.7f * unit(rng));
    }
    for (u32 i = 0; i < 50u; ++i) { // Behind the camera; some reach past the near plane.
        addView({unit(rng) * 4.f - 2.f, unit(rng) * 4.f - 2.f, 0.2f + 3.f * unit(rng)}, 0.2f + 3.f * unit(rng));
    }
    for (u32 i = 0; i < 50u; ++i) { // Straddling the near plane.
        addView({unit(rng) * 0.4f - 0.2f, unit(rng) * 0.4f - 0.2f, -camera.nearPlane + 0.2f * unit(rng) - 0.1f},
                0.05f + 0.5f * unit(rng));
    }
    for (u32 i = 0; i < 50u; ++i) { // Around / beyond the far plane.
        const f32 depth = camera.farPlane - 5.f + 30.f * unit(rng);
        addView({(unit(rng) * 2.f - 1.f) * tanX * depth, (unit(rng) * 2.f - 1.f) * tanY * depth, -depth},
                1.f + 8.f * unit(rng));
    }
    for (u32 i = 0; i < 49u; ++i) { // Well outside the frustum laterally.
        const f32 depth = 5.f + 50.f * unit(rng);
        const f32 side = unit(rng) < 0.5f ? -1.f : 1.f;
        addView({side * (tanX * depth + 5.f + 10.f * unit(rng)), 0.f, -depth}, 0.5f + 3.f * unit(rng));
    }
    addView({0.f, 0.f, -10.f}, 0.f); // Zero-radius light: reaches nothing, occupies nothing.
    return lights;
}

struct GpuContext {
    std::unique_ptr<VulkanBootstrap> bootstrap;
    BindlessDescriptors bindless{};
    ResourceManager resources;

    bool init() {
        VulkanBootstrapDesc bootstrapDesc{};
        bootstrapDesc.instance.enableValidation = false;
        bootstrapDesc.createSwapchain = false;
        bootstrap = VulkanBootstrap::create(bootstrapDesc);
        if (bootstrap == nullptr) {
            return false;
        }
        bindless.init(*bootstrap->device());
        return resources.init(*bootstrap->device(), bindless);
    }

    void destroy() {
        resources.destroy();
        if (bootstrap != nullptr) {
            bindless.destroy(*bootstrap->device());
        }
    }
};

/// Brute-force per-cluster membership: every light whose range sphere touches the cluster AABB.
/// A light with zero range has an identically-zero falloff and belongs to no cluster.
std::vector<std::vector<u32>> bruteForceClusterLists(const std::vector<ClusterAABB>& aabbs,
                                                     const ClusterCameraDesc& camera,
                                                     const std::vector<PointLightInput>& lights) {
    const Mat4d view = lookAtD(camera.position, camera.forward, camera.up);
    std::vector<Vec3d> centers;
    centers.reserve(lights.size());
    for (const PointLightInput& light : lights) {
        centers.push_back(transformPointD(view, light.position));
    }
    std::vector<std::vector<u32>> lists(aabbs.size());
    for (usize c = 0; c < aabbs.size(); ++c) {
        for (u32 i = 0; i < static_cast<u32>(lights.size()); ++i) {
            if (lights[i].radius > 0.f && sphereAabbD(centers[i], lights[i].radius, aabbs[c])) {
                lists[c].push_back(i);
            }
        }
    }
    return lists;
}

// ---------------------------------------------------------------------------------------------
// Cluster AABB build vs independent reversed-Z unprojection.

void testClusterAabbsMatchReversedZUnprojection() {
    const ClusterCameraDesc camera = makeGateCamera();
    const ClusterDesc desc = makeGateDesc();

    const Mat4d view = lookAtD(camera.position, camera.forward, camera.up);
    const Mat4d proj = reversedZInfiniteD(camera.fovYRadians, camera.aspect(), camera.nearPlane);
    Mat4d invProj{};
    expectTrue(invertD(proj, invProj), "reversed-Z projection invertible");

    // worldToView agrees with a lookAt view matrix.
    std::mt19937 rng(7u);
    std::uniform_real_distribution<f32> coord(-50.f, 50.f);
    double maxViewErr = 0.0;
    for (u32 i = 0; i < 256u; ++i) {
        const Vec3 p{coord(rng), coord(rng), coord(rng)};
        const Vec3 engine = cluster_math::worldToView(camera, p);
        const Vec3d ref = transformPointD(view, p);
        maxViewErr = std::max({maxViewErr, std::fabs(engine.x - ref.x), std::fabs(engine.y - ref.y),
                               std::fabs(engine.z - ref.z)});
        const Vec3 back = cluster_math::viewToWorld(camera, engine);
        maxViewErr = std::max({maxViewErr, std::fabs(back.x - p.x) * 0.1, std::fabs(back.y - p.y) * 0.1,
                               std::fabs(back.z - p.z) * 0.1});
    }
    expectTrue(maxViewErr < 1e-3, "worldToView/viewToWorld match lookAt view matrix");

    // Device depth convention matches the projection matrices.
    double maxDepthErr = 0.0;
    const Mat4d stdProj = standardZD(camera.fovYRadians, camera.aspect(), camera.nearPlane, camera.farPlane);
    ClusterCameraDesc stdCamera = camera;
    stdCamera.reversedZ = false;
    for (f32 d : {0.1f, 0.37f, 1.f, 4.5f, 17.f, 90.f, 199.f}) {
        const double v[4] = {0.0, 0.0, -static_cast<double>(d), 1.0};
        double clip[4];
        mulD(proj, v, clip);
        maxDepthErr = std::max<double>(maxDepthErr,
                               std::fabs(cluster_math::deviceDepthFromViewDepth(d, camera) - clip[2] / clip[3]));
        maxDepthErr = std::max<double>(maxDepthErr,
                               std::fabs(cluster_math::viewDepthFromDeviceDepth(static_cast<f32>(clip[2] / clip[3]),
                                                                                camera) -
                                         d) /
                                   d);
        mulD(stdProj, v, clip);
        maxDepthErr = std::max<double>(maxDepthErr,
                               std::fabs(cluster_math::deviceDepthFromViewDepth(d, stdCamera) - clip[2] / clip[3]));
        maxDepthErr = std::max<double>(
            maxDepthErr,
            std::fabs(cluster_math::viewDepthFromDeviceDepth(static_cast<f32>(clip[2] / clip[3]), stdCamera) - d) /
                d);
    }
    expectTrue(maxDepthErr < 1e-3, "device depth <-> view depth matches reversed-Z and standard projections");
    expectTrue(cluster_math::viewDepthFromDeviceDepth(0.f, camera) == 0.f, "reversed-Z cleared depth is sky");
    expectTrue(cluster_math::viewDepthFromDeviceDepth(1.f, stdCamera) == 0.f, "standard-Z cleared depth is sky");

    // Cluster AABBs == bounds of 8 corners unprojected through inverse(P) at the slice depths.
    std::vector<ClusterAABB> aabbs;
    cluster_math::buildClusterAabbs(desc, camera, aabbs);
    expectTrue(aabbs.size() == desc.clusterCount(), "one AABB per cluster");

    double maxRelErr = 0.0;
    const double ratio = static_cast<double>(camera.farPlane) / camera.nearPlane;
    for (u32 z = 0; z < desc.slicesZ; ++z) {
        const double d0 = camera.nearPlane * std::pow(ratio, static_cast<double>(z) / desc.slicesZ);
        const double d1 = camera.nearPlane * std::pow(ratio, static_cast<double>(z + 1u) / desc.slicesZ);
        for (u32 y = 0; y < desc.tilesY; ++y) {
            for (u32 x = 0; x < desc.tilesX; ++x) {
                Vec3d lo{1e30, 1e30, 1e30};
                Vec3d hi{-1e30, -1e30, -1e30};
                for (double d : {d0, d1}) {
                    for (u32 cx = x; cx <= x + 1u; ++cx) {
                        for (u32 cy = y; cy <= y + 1u; ++cy) {
                            const double ndc[4] = {2.0 * cx / desc.tilesX - 1.0, 1.0 - 2.0 * cy / desc.tilesY,
                                                   camera.nearPlane / d, 1.0};
                            double v[4];
                            mulD(invProj, ndc, v);
                            const Vec3d p{v[0] / v[3], v[1] / v[3], v[2] / v[3]};
                            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                        }
                    }
                }
                const ClusterAABB& box = aabbs[ClusterGridLayout::clusterIndex(x, y, z, desc)];
                const double scale = std::max(1.0, d1);
                maxRelErr = std::max({maxRelErr, std::fabs(box.minP.x - lo.x) / scale,
                                      std::fabs(box.minP.y - lo.y) / scale, std::fabs(box.minP.z - lo.z) / scale,
                                      std::fabs(box.maxP.x - hi.x) / scale, std::fabs(box.maxP.y - hi.y) / scale,
                                      std::fabs(box.maxP.z - hi.z) / scale});
            }
        }
    }
    std::printf("  cluster AABB vs reversed-Z unprojection: max rel err %.3g\n", maxRelErr);
    expectTrue(maxRelErr < 1e-5, "cluster AABBs match reversed-Z inverse-projection corners");

    // Screen/depth -> cluster mapping lands in a cluster whose AABB contains the point.
    u32 outside = 0;
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    for (u32 i = 0; i < 20000u; ++i) {
        const f32 sx = unit(rng);
        const f32 sy = unit(rng);
        const f32 depth = camera.nearPlane * std::pow(camera.farPlane / camera.nearPlane, unit(rng));
        u32 idx = 0;
        if (!ClusterGridLayout::mapScreenDepthToClusterIndex(sx, sy, depth, desc, camera, idx)) {
            continue;
        }
        const Vec3 p = cluster_math::viewPositionFromScreen(sx, sy, depth, camera);
        const ClusterAABB& box = aabbs[idx];
        const f32 tol = 1e-4f * std::max(1.f, depth);
        if (p.x < box.minP.x - tol || p.x > box.maxP.x + tol || p.y < box.minP.y - tol || p.y > box.maxP.y + tol ||
            p.z < box.minP.z - tol || p.z > box.maxP.z + tol) {
            ++outside;
        }
    }
    expectTrue(outside == 0u, "screen/depth cluster mapping consistent with cluster AABBs");
}

// ---------------------------------------------------------------------------------------------
// Gate row 1: light_grid vs brute force; empty clusters have count 0.

void testCullMatchesBruteForce(GpuContext& gpu) {
    const ClusterCameraDesc camera = makeGateCamera();
    const ClusterDesc desc = makeGateDesc();
    const std::vector<PointLightInput> lights = makeGateLights(camera, 1234u);
    expectTrue(lights.size() == 1000u, "gate scene has 1000 point lights");

    ClusteredLightCuller culler;
    culler.init(desc, gpu.resources);
    expectTrue(culler.isReady(), "culler ready for gate cull");
    culler.cullLights(lights, {}, camera);

    const ClusterGridSoA& grid = culler.gridSoA();
    const u32 clusterCount = desc.clusterCount();
    expectTrue(grid.grid.size() == clusterCount, "light_grid has one (offset,count) per cluster");
    expectTrue(ClusterLightGridLayout::validateContiguousOffsets(grid, clusterCount),
               "light_grid offsets contiguous over flat light list");
    expectTrue(culler.stats().lightsDroppedOverflow == 0u, "no overflow at 256 lights per cluster");

    const std::vector<std::vector<u32>> expected = bruteForceClusterLists(grid.aabbs, camera, lights);

    u32 mismatchedClusters = 0;
    u32 emptyClusters = 0;
    u32 emptyWithLights = 0;
    u64 pairs = 0;
    u32 maxPerCluster = 0;
    std::vector<u32> actual;
    for (u32 c = 0; c < clusterCount; ++c) {
        cluster_util::lookupClusterLights(grid, c, actual);
        if (actual != expected[c]) {
            ++mismatchedClusters;
        }
        if (expected[c].empty()) {
            ++emptyClusters;
            if (grid.grid[c].count != 0u) {
                ++emptyWithLights;
            }
        }
        pairs += expected[c].size();
        maxPerCluster = std::max(maxPerCluster, static_cast<u32>(expected[c].size()));
    }
    std::printf("  1000 lights / %u clusters: %llu (cluster,light) pairs, %u empty clusters, max %u per cluster, "
                "%u mismatches vs brute force\n",
                clusterCount, static_cast<unsigned long long>(pairs), emptyClusters, maxPerCluster,
                mismatchedClusters);
    expectTrue(mismatchedClusters == 0u, "every cluster list == brute-force sphere/AABB set");
    expectTrue(emptyWithLights == 0u, "clusters with no light overlap have count 0");
    expectTrue(emptyClusters > 0u && emptyClusters < clusterCount, "scene has both empty and lit clusters");
    expectTrue(grid.lightList.size() == pairs, "flat light list holds exactly the overlapping pairs");
    expectTrue(culler.stats().lightsCulled == pairs, "stats report overlapping pair count");

    // Lights that cannot reach the grid appear nowhere.
    std::vector<u8> referenced(lights.size(), 0u);
    for (u32 idx : grid.lightList) {
        referenced[idx] = 1u;
    }
    expectTrue(referenced[lights.size() - 1u] == 0u, "zero-radius light assigned to no cluster");

    // No false negatives on the true frustum cells: sample points inside each cell; any light
    // whose range reaches the point must be in that cell's list.
    std::mt19937 rng(99u);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    u32 missed = 0;
    u32 samples = 0;
    for (u32 c = 0; c < clusterCount; ++c) {
        u32 tx = 0;
        u32 ty = 0;
        u32 tz = 0;
        ClusterGridLayout::decodeClusterIndex(c, desc, tx, ty, tz);
        const f32 d0 = ClusterSliceLayout::computeSliceNearZ(tz, desc, camera);
        const f32 d1 = ClusterSliceLayout::computeSliceFarZ(tz, desc, camera);
        cluster_util::lookupClusterLights(grid, c, actual);
        for (u32 s = 0; s < 6u; ++s) {
            const f32 sx = (static_cast<f32>(tx) + 0.02f + 0.96f * unit(rng)) / desc.tilesX;
            const f32 sy = (static_cast<f32>(ty) + 0.02f + 0.96f * unit(rng)) / desc.tilesY;
            const f32 depth = d0 + (d1 - d0) * (0.02f + 0.96f * unit(rng));
            const Vec3 world =
                cluster_math::viewToWorld(camera, cluster_math::viewPositionFromScreen(sx, sy, depth, camera));
            ++samples;
            for (u32 i = 0; i < static_cast<u32>(lights.size()); ++i) {
                if ((lights[i].position - world).length() < lights[i].radius &&
                    !std::binary_search(actual.begin(), actual.end(), i)) {
                    ++missed;
                }
            }
        }
    }
    std::printf("  %u in-cell samples: %u lights reaching a sample but missing from its cluster\n", samples, missed);
    expectTrue(missed == 0u, "no light reaching a cluster cell is missing from its list");

    culler.destroy();
}

// ---------------------------------------------------------------------------------------------
// Overflow: lists keep the lowest indices, drops are counted exactly.

void testOverflowHandling(GpuContext& gpu) {
    const ClusterCameraDesc camera = makeGateCamera();
    ClusterDesc desc = makeGateDesc();
    desc.maxLightsPerCluster = 8;
    const std::vector<PointLightInput> lights = makeGateLights(camera, 1234u);

    ClusteredLightCuller culler;
    culler.init(desc, gpu.resources);
    culler.cullLights(lights, {}, camera);
    const ClusterGridSoA& grid = culler.gridSoA();
    const std::vector<std::vector<u32>> expected = bruteForceClusterLists(grid.aabbs, camera, lights);

    u32 mismatched = 0;
    u64 expectedDropped = 0;
    u32 expectedAtCapacity = 0;
    std::vector<u32> actual;
    for (u32 c = 0; c < desc.clusterCount(); ++c) {
        const std::vector<u32>& full = expected[c];
        const usize keep = std::min<usize>(full.size(), desc.maxLightsPerCluster);
        const std::vector<u32> truncated(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(keep));
        cluster_util::lookupClusterLights(grid, c, actual);
        if (actual != truncated) {
            ++mismatched;
        }
        if (full.size() > desc.maxLightsPerCluster) {
            expectedDropped += full.size() - desc.maxLightsPerCluster;
            ++expectedAtCapacity;
        }
    }
    std::printf("  overflow @8/cluster: %u clusters over capacity, %llu pairs dropped\n", expectedAtCapacity,
                static_cast<unsigned long long>(expectedDropped));
    expectTrue(expectedAtCapacity > 0u, "overflow scenario actually overflows");
    expectTrue(mismatched == 0u, "overflowed clusters keep the lowest-index lights");
    expectTrue(culler.stats().lightsDroppedOverflow == expectedDropped, "dropped pair count exact");
    expectTrue(culler.stats().clustersAtCapacity == expectedAtCapacity, "clusters-at-capacity count exact");
    expectTrue(cluster_util::countClustersAtCapacity(grid, desc.clusterCount(), desc.maxLightsPerCluster) >=
                   expectedAtCapacity,
               "capacity helper sees every overflowed cluster as full");
    expectTrue(cluster_util::validateGridPopulation(grid, desc.clusterCount()), "overflowed grid population valid");

    // Dense cluster at the default 256 cap: 300 co-located lights.
    ClusteredLightCuller dense;
    dense.init(makeGateDesc(), gpu.resources);
    std::vector<PointLightInput> stack(300u);
    for (PointLightInput& light : stack) {
        light.position = cluster_math::viewToWorld(camera, {0.f, 0.f, -20.f});
        light.radius = 0.25f;
    }
    dense.cullLights(stack, {}, camera);
    const ClusterGridSoA& denseGrid = dense.gridSoA();
    u32 maxCount = 0;
    for (const ClusterGridEntry& entry : denseGrid.grid) {
        maxCount = std::max(maxCount, entry.count);
    }
    expectTrue(maxCount == 256u, "dense cluster clamps to 256 lights");
    expectTrue(dense.stats().lightsDroppedOverflow == 44u * dense.stats().clustersAtCapacity,
               "each full cluster drops exactly the 44 excess lights");
    expectTrue(dense.stats().clustersAtCapacity > 0u, "dense scene reports clusters at capacity");
    dense.destroy();
    culler.destroy();
}

// ---------------------------------------------------------------------------------------------
// Gate row 2: 1000-light deferred shading equals the all-lights reference; no leaking.

struct Box {
    Vec3 lo;
    Vec3 hi;
    Vec3 albedo;
};

struct SceneHit {
    f32 t = 0.f;
    Vec3 normal{};
    Vec3 albedo{};
};

bool intersectBox(const Vec3& o, const Vec3& d, const Box& b, SceneHit& hit) {
    f32 tNear = -1e30f;
    f32 tFar = 1e30f;
    int axisNear = -1;
    f32 signNear = 0.f;
    const f32 oa[3] = {o.x, o.y, o.z};
    const f32 da[3] = {d.x, d.y, d.z};
    const f32 lo[3] = {b.lo.x, b.lo.y, b.lo.z};
    const f32 hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(da[a]) < 1e-12f) {
            if (oa[a] < lo[a] || oa[a] > hi[a]) {
                return false;
            }
            continue;
        }
        f32 t0 = (lo[a] - oa[a]) / da[a];
        f32 t1 = (hi[a] - oa[a]) / da[a];
        f32 sign = -1.f;
        if (t0 > t1) {
            std::swap(t0, t1);
            sign = 1.f;
        }
        if (t0 > tNear) {
            tNear = t0;
            axisNear = a;
            signNear = sign;
        }
        tFar = std::min(tFar, t1);
    }
    if (tNear > tFar || tNear <= 0.f || axisNear < 0) {
        return false;
    }
    hit.t = tNear;
    hit.normal = {axisNear == 0 ? signNear : 0.f, axisNear == 1 ? signNear : 0.f, axisNear == 2 ? signNear : 0.f};
    hit.albedo = b.albedo;
    return true;
}

struct WallScene {
    std::vector<Box> boxes;
    static constexpr f32 kWallFront = -10.f;
    static constexpr f32 kWallBack = -12.f;

    WallScene() {
        boxes.push_back({{-60.f, -1.f, -60.f}, {60.f, 0.f, 30.f}, {0.7f, 0.7f, 0.7f}});                 // Floor slab.
        boxes.push_back({{-60.f, 0.f, kWallBack}, {60.f, 30.f, kWallFront}, {0.8f, 0.6f, 0.5f}});        // Wall.
        boxes.push_back({{-6.f, 0.f, -4.f}, {-4.f, 5.f, -2.f}, {0.4f, 0.6f, 0.9f}});                     // Pillar.
        boxes.push_back({{3.f, 0.f, -7.f}, {5.f, 2.5f, -5.f}, {0.9f, 0.9f, 0.3f}});                      // Crate.
    }

    bool trace(const Vec3& o, const Vec3& d, SceneHit& best) const {
        bool any = false;
        for (const Box& box : boxes) {
            SceneHit hit{};
            if (intersectBox(o, d, box, hit) && (!any || hit.t < best.t)) {
                best = hit;
                any = true;
            }
        }
        return any;
    }
};

ClusterCameraDesc makeRoomCamera(u32 width, u32 height) {
    ClusterCameraDesc camera{};
    camera.position = {0.f, 3.f, 12.f};
    camera.forward = {0.1f, -0.15f, -1.f};
    camera.fovYRadians = 1.04719755f;
    camera.nearPlane = 0.1f;
    camera.farPlane = 200.f;
    camera.screenWidth = width;
    camera.screenHeight = height;
    camera.reversedZ = true;
    return camera;
}

struct SyntheticGBuffer {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> depth;
    std::vector<Vec3> normals;
    std::vector<Vec3> albedo;

    DeferredGBufferView view() const {
        DeferredGBufferView v{};
        v.width = width;
        v.height = height;
        v.deviceDepth = depth.data();
        v.normals = normals.data();
        v.albedo = albedo.data();
        return v;
    }
};

/// Ray-cast the wall scene into a reversed-Z G-buffer (what the G-buffer pass would write).
SyntheticGBuffer rasterizeScene(const WallScene& scene, const ClusterCameraDesc& camera) {
    SyntheticGBuffer gb{};
    gb.width = camera.screenWidth;
    gb.height = camera.screenHeight;
    const usize count = static_cast<usize>(gb.width) * gb.height;
    gb.depth.assign(count, 0.f);
    gb.normals.assign(count, Vec3{});
    gb.albedo.assign(count, Vec3{});
    const Vec3 forward = camera.forward.normalized();
    for (u32 py = 0; py < gb.height; ++py) {
        for (u32 px = 0; px < gb.width; ++px) {
            const f32 sx = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(gb.width);
            const f32 sy = (static_cast<f32>(py) + 0.5f) / static_cast<f32>(gb.height);
            const Vec3 target =
                cluster_math::viewToWorld(camera, cluster_math::viewPositionFromScreen(sx, sy, 1.f, camera));
            const Vec3 dir = (target - camera.position).normalized();
            SceneHit hit{};
            if (!scene.trace(camera.position, dir, hit)) {
                continue;
            }
            const usize i = static_cast<usize>(py) * gb.width + px;
            const f32 viewDepth = hit.t * dir.dot(forward);
            gb.depth[i] = cluster_math::deviceDepthFromViewDepth(viewDepth, camera);
            gb.normals[i] = hit.normal;
            gb.albedo[i] = hit.albedo;
        }
    }
    return gb;
}

std::vector<PointLightInput> makeRoomLights(u32 frontCount, u32 behindCount, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    std::vector<PointLightInput> lights;
    auto make = [&](f32 zLo, f32 zHi, bool behind) {
        PointLightInput light{};
        light.radius = 0.5f + 2.f * unit(rng);
        f32 z = zLo + (zHi - zLo) * unit(rng);
        if (behind) {
            // Keep the whole range sphere strictly behind the wall's front face.
            z = std::min(z, WallScene::kWallFront - light.radius - 0.05f);
        }
        light.position = {-20.f + 40.f * unit(rng), 0.2f + 7.f * unit(rng), z};
        light.color = {0.2f + 0.8f * unit(rng), 0.2f + 0.8f * unit(rng), 0.2f + 0.8f * unit(rng)};
        light.intensity = 1.f + 4.f * unit(rng);
        lights.push_back(light);
    };
    for (u32 i = 0; i < frontCount; ++i) {
        make(-9.8f, 10.f, false);
    }
    for (u32 i = 0; i < behindCount; ++i) {
        make(-40.f, -10.f, true);
    }
    return lights;
}

void testDeferredShading1000Lights(GpuContext& gpu) {
    const WallScene scene;
    const ClusterCameraDesc camera = makeRoomCamera(384, 216);
    const SyntheticGBuffer gb = rasterizeScene(scene, camera);
    const ClusterDesc desc = makeGateDesc();

    const std::vector<PointLightInput> lights = makeRoomLights(700u, 300u, 4242u);
    expectTrue(lights.size() == 1000u, "room scene has 1000 point lights");

    ClusteredLightCuller culler;
    culler.init(desc, gpu.resources);
    culler.cullLights(lights, {}, camera);
    expectTrue(culler.stats().lightsDroppedOverflow == 0u, "room scene fits the per-cluster cap");

    std::vector<Vec3> clustered;
    std::vector<Vec3> reference;
    const DeferredShadeStats cs =
        clustered_shading::shadeDeferredFrame(gb.view(), desc, camera, culler.gridSoA(), lights, true, clustered);
    const DeferredShadeStats rs =
        clustered_shading::shadeDeferredFrame(gb.view(), desc, camera, culler.gridSoA(), lights, false, reference);

    u32 bitMismatches = 0;
    u32 litPixels = 0;
    double maxAbs = 0.0;
    for (usize i = 0; i < clustered.size(); ++i) {
        if (std::memcmp(&clustered[i], &reference[i], sizeof(Vec3)) != 0) {
            ++bitMismatches;
        }
        maxAbs = std::max<double>({maxAbs, std::fabs(clustered[i].x - reference[i].x),
                           std::fabs(clustered[i].y - reference[i].y), std::fabs(clustered[i].z - reference[i].z)});
        if (reference[i].x + reference[i].y + reference[i].z > 0.f) {
            ++litPixels;
        }
    }
    std::printf("  deferred shade %ux%u, 1000 lights: %u shaded px, %u lit px, %u bit mismatches (max |d| %.3g), "
                "light evals clustered %llu vs all-lights %llu (%.1fx fewer)\n",
                camera.screenWidth, camera.screenHeight, cs.shadedPixels, litPixels, bitMismatches, maxAbs,
                static_cast<unsigned long long>(cs.lightEvaluations),
                static_cast<unsigned long long>(rs.lightEvaluations),
                cs.lightEvaluations > 0u ? static_cast<double>(rs.lightEvaluations) / cs.lightEvaluations : 0.0);
    expectTrue(cs.shadedPixels == rs.shadedPixels && cs.shadedPixels > gb.width * gb.height / 2u,
               "most pixels hit geometry and are shaded");
    expectTrue(litPixels > cs.shadedPixels / 10u, "1000 lights visibly light the scene");
    expectTrue(bitMismatches == 0u, "clustered shading bit-identical to shading with all lights");
    expectTrue(cs.lightEvaluations * 10u < rs.lightEvaluations, "clusters cut light evaluations by >10x");

    // Per pixel: every light missing from the pixel's cluster contributes exactly zero there.
    u32 leaks = 0;
    std::vector<u32> list;
    for (u32 py = 0; py < gb.height; py += 2u) {
        for (u32 px = 0; px < gb.width; px += 2u) {
            const usize i = static_cast<usize>(py) * gb.width + px;
            const f32 sx = (static_cast<f32>(px) + 0.5f) / gb.width;
            const f32 sy = (static_cast<f32>(py) + 0.5f) / gb.height;
            DeferredSurfaceSample surface{};
            f32 viewDepth = 0.f;
            u32 cluster = 0;
            if (!clustered_shading::reconstructWorldPosition(camera, sx, sy, gb.depth[i], surface.worldPos,
                                                             viewDepth) ||
                !ClusterGridLayout::mapScreenDepthToClusterIndex(sx, sy, viewDepth, desc, camera, cluster)) {
                continue;
            }
            surface.normal = gb.normals[i];
            surface.albedo = gb.albedo[i];
            cluster_util::lookupClusterLights(culler.gridSoA(), cluster, list);
            for (u32 l = 0; l < static_cast<u32>(lights.size()); ++l) {
                if (std::binary_search(list.begin(), list.end(), l)) {
                    continue;
                }
                const Vec3 c = clustered_shading::pointLightContribution(lights[l], surface);
                const f32 dist = (lights[l].position - surface.worldPos).length();
                if (c.x != 0.f || c.y != 0.f || c.z != 0.f || dist < lights[l].radius) {
                    ++leaks;
                }
            }
        }
    }
    expectTrue(leaks == 0u, "lights outside a pixel's cluster list contribute exactly zero there");

    // Wall: the 300 lights behind the wall must add exactly nothing to the visible side.
    const std::vector<PointLightInput> behind(lights.begin() + 700, lights.end());
    ClusteredLightCuller behindCuller;
    behindCuller.init(desc, gpu.resources);
    behindCuller.cullLights(behind, {}, camera);
    std::vector<Vec3> behindClustered;
    std::vector<Vec3> behindReference;
    clustered_shading::shadeDeferredFrame(gb.view(), desc, camera, behindCuller.gridSoA(), behind, true,
                                          behindClustered);
    clustered_shading::shadeDeferredFrame(gb.view(), desc, camera, behindCuller.gridSoA(), behind, false,
                                          behindReference);
    u32 leakedPixels = 0;
    u32 wallPixels = 0;
    for (usize i = 0; i < behindClustered.size(); ++i) {
        const Vec3& a = behindClustered[i];
        const Vec3& b = behindReference[i];
        if (a.x != 0.f || a.y != 0.f || a.z != 0.f || b.x != 0.f || b.y != 0.f || b.z != 0.f) {
            ++leakedPixels;
        }
        if (gb.normals[i].z > 0.5f && gb.albedo[i].x == 0.8f) {
            ++wallPixels;
        }
    }
    // Sanity: those lights are real — they do light the hidden back face of the wall.
    u32 backFaceLit = 0;
    for (const PointLightInput& light : behind) {
        DeferredSurfaceSample back{};
        back.worldPos = {light.position.x, light.position.y, WallScene::kWallBack};
        back.normal = {0.f, 0.f, -1.f};
        const Vec3 c = clustered_shading::pointLightContribution(light, back);
        if (c.x + c.y + c.z > 0.f) {
            ++backFaceLit;
        }
    }
    std::printf("  wall test: %u visible wall px, %u px lit by the 300 lights behind the wall, %u of those lights "
                "light the hidden back face\n",
                wallPixels, leakedPixels, backFaceLit);
    expectTrue(wallPixels > 1000u, "wall front face covers a large part of the frame");
    expectTrue(leakedPixels == 0u, "no light leaks through the wall (clustered and all-lights reference)");
    expectTrue(backFaceLit > 0u, "behind-wall lights do reach the hidden back face");

    // Falloff window: exactly zero at and beyond range, positive inside.
    expectTrue(clustered_shading::pointLightFalloff(2.f, 2.f) == 0.f, "falloff exactly 0 at range");
    expectTrue(clustered_shading::pointLightFalloff(2.0001f, 2.f) == 0.f, "falloff 0 beyond range");
    expectTrue(clustered_shading::pointLightFalloff(std::nextafter(2.f, 0.f), 2.f) >= 0.f, "falloff non-negative");
    expectTrue(clustered_shading::pointLightFalloff(1.f, 2.f) > 0.f, "falloff positive inside range");
    expectTrue(clustered_shading::pointLightFalloff(0.5f, 0.f) == 0.f, "zero-range light contributes nothing");
    bool monotonic = true;
    f32 previous = clustered_shading::pointLightFalloff(0.01f, 5.f);
    for (u32 i = 2; i <= 500u; ++i) {
        const f32 value = clustered_shading::pointLightFalloff(0.01f * static_cast<f32>(i), 5.f);
        monotonic = monotonic && value <= previous;
        previous = value;
    }
    expectTrue(monotonic, "falloff monotonically decreasing to zero at range");

    behindCuller.destroy();
    culler.destroy();
}

// ---------------------------------------------------------------------------------------------
// Gate row 3 (GPU timing is hardware-only): report CPU cull time for 1000 lights at 1080p.

void testCullTiming(GpuContext& gpu) {
    const ClusterCameraDesc camera = makeGateCamera();
    const ClusterDesc desc = makeGateDesc();
    const std::vector<PointLightInput> lights = makeGateLights(camera, 1234u);

    ClusteredLightCuller culler;
    culler.init(desc, gpu.resources);
    culler.cullLights(lights, {}, camera); // Warm-up (allocations).

    using Clock = std::chrono::steady_clock;
    std::vector<double> samples;
    for (u32 i = 0; i < 15u; ++i) {
        const auto t0 = Clock::now();
        culler.cullLights(lights, {}, camera);
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());

    // Cluster-major brute force (every cluster x every light) for comparison.
    std::vector<std::vector<u32>> brute(desc.clusterCount());
    const auto b0 = Clock::now();
    for (u32 c = 0; c < desc.clusterCount(); ++c) {
        for (u32 l = 0; l < static_cast<u32>(lights.size()); ++l) {
            if (cluster_math::sphereIntersectsAabb(cluster_math::worldToView(camera, lights[l].position),
                                                   lights[l].radius, culler.gridSoA().aabbs[c])) {
                brute[c].push_back(l);
            }
        }
    }
    const auto b1 = Clock::now();

    // One 1080p CPU clustered shade pass (reference only; the gate budget is for the GPU kernel).
    const WallScene scene;
    const ClusterCameraDesc room = makeRoomCamera(1920, 1080);
    const SyntheticGBuffer gb = rasterizeScene(scene, room);
    const std::vector<PointLightInput> roomLights = makeRoomLights(700u, 300u, 4242u);
    culler.cullLights(roomLights, {}, room);
    std::vector<Vec3> radiance;
    const auto s0 = Clock::now();
    const DeferredShadeStats shade =
        clustered_shading::shadeDeferredFrame(gb.view(), desc, room, culler.gridSoA(), roomLights, true, radiance);
    const auto s1 = Clock::now();

    std::printf("  CPU cull 1000 lights -> 16x9x24 clusters (1080p camera): median %.3f ms, min %.3f ms "
                "(cluster x light brute force %.1f ms)\n",
                samples[samples.size() / 2u], samples.front(),
                std::chrono::duration<double, std::milli>(b1 - b0).count());
    std::printf("  CPU clustered deferred shade 1920x1080, 1000 lights (single thread): %.1f ms, %llu light evals\n",
                std::chrono::duration<double, std::milli>(s1 - s0).count(),
                static_cast<unsigned long long>(shade.lightEvaluations));
    std::printf("  GPU gate (cull + shade < 3 ms @1080p): hardware-only, not measurable on CI\n");
    expectTrue(!samples.empty() && samples.front() > 0.0, "cull timing recorded");

    culler.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();

    testClusterAabbsMatchReversedZUnprojection();

    GpuContext gpu;
    expectTrue(gpu.init(), "resource manager ready for clustered gates");
    testCullMatchesBruteForce(gpu);
    testOverflowHandling(gpu);
    testDeferredShading1000Lights(gpu);
    testCullTiming(gpu);
    gpu.destroy();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b5_clustered_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b5_clustered_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
