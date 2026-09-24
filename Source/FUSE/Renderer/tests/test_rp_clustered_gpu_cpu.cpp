// WP-2.1 CPU gates (stub-safe): the GPU clustered lighting's records, the oracle adapters, the
// candidate-window argument the GPU light lists rest on, the BRDF / light terms and the "light.shade"
// reference kernel. Lavapipe gates: test_rp_clustered_gpu.cpp.
//
//   layout  record sizes / offsets, the frame-constant edge tables fit the grid limits, buffer sections
//           256-aligned and disjoint
//   oracle  makeOracleLights / translateToSlots on a table with free slots, directional lights, points
//           and spots; oracleLightGrid == a brute-force light-major assignment (sphere vs the oracle's
//           AABBs, ascending index, capacity); widening the candidate window (the GPU's
//           kSliceWindowPad = 2 instead of the oracle's 1, and a full-grid window) changes no list over
//           random cameras x 4,096 lights
//   brdf    N.L <= 0 -> 0; reciprocity; energy (albedo 1, dielectric) <= 1 over a hemisphere quadrature
//           at roughness 0.05..1; spot cone 1 inside the inner cone, 0 outside the outer, monotone;
//           point / spot contribution exactly 0 outside the range; directional independent of distance
//   shade   one pixel against an independent f64 evaluation (1e-5 relative); CpuReference ==
//           CpuParallel bit for bit; cluster-list shading == all-lights shading (lights outside a
//           cluster contribute exactly 0); sky / out-of-range pixels are (0, 0, 0, 0)
//   api     no device: queryLightingCapabilities / init fail cleanly
#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>

#include <fuse/renderer/lighting/clustered_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::lighting_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace kernel = fuse::kernel;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

gpu_scene::GpuLight pointLight(Vec3 p, f32 range, Vec3 color, f32 intensity) {
    gpu_scene::GpuLight l{};
    l.type = static_cast<u32>(gpu_scene::GpuLightType::Point);
    l.position[0] = p.x;
    l.position[1] = p.y;
    l.position[2] = p.z;
    l.range = range;
    l.color[0] = color.x;
    l.color[1] = color.y;
    l.color[2] = color.z;
    l.intensity = intensity;
    return l;
}

gpu_scene::GpuLight spotLight(Vec3 p, Vec3 dir, f32 range, f32 cosInner, f32 cosOuter, f32 intensity) {
    gpu_scene::GpuLight l = pointLight(p, range, {1.f, 0.9f, 0.8f}, intensity);
    l.type = static_cast<u32>(gpu_scene::GpuLightType::Spot);
    const Vec3 d = dir.normalized();
    l.direction[0] = d.x;
    l.direction[1] = d.y;
    l.direction[2] = d.z;
    l.cosInner = cosInner;
    l.cosOuter = cosOuter;
    return l;
}

gpu_scene::GpuLight directionalLight(Vec3 dir, f32 intensity) {
    gpu_scene::GpuLight l{};
    l.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    const Vec3 d = dir.normalized();
    l.direction[0] = d.x;
    l.direction[1] = d.y;
    l.direction[2] = d.z;
    l.intensity = intensity;
    return l;
}

ClusterCameraDesc makeCamera(Vec3 eye, Vec3 at, u32 w, u32 h) {
    ClusterCameraDesc c{};
    c.position = eye;
    c.forward = at - eye;
    c.up = {0.f, 1.f, 0.f};
    c.nearPlane = 0.3f;
    c.farPlane = 120.f;
    c.fovYRadians = 1.1f;
    c.screenWidth = w;
    c.screenHeight = h;
    c.reversedZ = false;
    return c;
}

/// Random scene lights: `points` points, then `spots` spots (slots in that order), + extras.
std::vector<gpu_scene::GpuLight> randomLights(std::mt19937& rng, u32 points, u32 spots, bool extras) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    std::vector<gpu_scene::GpuLight> lights;
    for (u32 i = 0; i < points + spots; ++i) {
        const Vec3 p{u(rng) * 16.f, u(rng) * 4.f, -4.f - 34.f * (u(rng) * 0.5f + 0.5f)};
        const f32 range = 0.3f + 2.7f * (u(rng) * 0.5f + 0.5f);
        if (i < points) {
            lights.push_back(pointLight(p, range, {1.f, 0.8f, 0.6f}, 1.f + u(rng)));
        } else {
            lights.push_back(spotLight(p, {u(rng), -1.f, u(rng)}, range, 0.9f, 0.7f, 2.f));
        }
    }
    if (extras) {
        lights[3].type = 0;                // free slot among the points
        lights[5].range = 0.f;             // no range: occupies no cluster
        lights.push_back(directionalLight({0.3f, -1.f, -0.2f}, 1.5f));
        lights.push_back(gpu_scene::GpuLight{}); // trailing free slot
    }
    return lights;
}

// --- layout -----------------------------------------------------------------------------------------
void testLayout() {
    expect(kMaxTilesX == ClusterDesc::kMaxTilesX && kMaxTilesY == ClusterDesc::kMaxTilesY &&
               kMaxSlicesZ == ClusterDesc::kMaxSlicesZ && kMaxLightsPerCluster == ClusterDesc::kMaxLightsPerCluster,
           "grid limits == ClusterDesc limits");
    const LightingFrameConstants c{};
    expect(sizeof(c.sliceDepth) / sizeof(f32) >= kMaxSlicesZ + 1u, "slice-depth table holds every edge");
    expect(sizeof(c.ndcX) / sizeof(f32) >= kMaxTilesX + 1u && sizeof(c.ndcY) / sizeof(f32) >= kMaxTilesY + 1u,
           "tile-edge tables hold every edge");
    expect(sizeof(GpuClusterAabb) == 32u && sizeof(GpuLightBounds) == 32u && sizeof(LightListHeader) == 64u &&
               sizeof(LightingPush) == 16u && sizeof(LightingFrameConstants) == 752u,
           "record sizes");
    expect(sizeof(ClusterGridEntry) == 8u, "grid entry == u32x2 (lists buffer grid section)");
    for (const u32 lights : {1u, 4096u, 5000u}) {
        const LightingBufferLayout l = LightingBufferLayout::compute(16u * 9u * 24u, 24u, 256u, lights);
        const u64 work[] = {l.aabbs, l.bounds, l.sliceCounts, l.sliceLights, l.clusterCounts, l.clusterDropped, l.clusterSlots,
                            l.workBytes};
        const u64 lists[] = {l.header, l.grid, l.directional, l.lightList, l.listsBytes};
        bool ok = true;
        for (usize i = 0; i + 1 < std::size(work); ++i) {
            ok = ok && work[i] % 256u == 0u && work[i] < work[i + 1];
        }
        for (usize i = 0; i + 1 < std::size(lists); ++i) {
            ok = ok && lists[i] % 256u == 0u && lists[i] < lists[i + 1];
        }
        ok = ok && l.clusterSlots + 16ull * 9u * 24u * 256u * 4u <= l.workBytes &&
             l.lightList + 16ull * 9u * 24u * 256u * 4u <= l.listsBytes &&
             l.sliceLights + 24ull * lights * 4u <= l.clusterCounts && l.bounds + u64{lights} * 32u <= l.sliceCounts;
        expect(ok, "buffer sections 256-aligned, ordered, sized");
    }
    std::printf("layout: frame constants %zu B, work %llu B, lists %llu B (16x9x24, cap 256, 4096 lights)\n",
                sizeof(LightingFrameConstants),
                static_cast<unsigned long long>(LightingBufferLayout::compute(3456u, 24u, 256u, 4096u).workBytes),
                static_cast<unsigned long long>(LightingBufferLayout::compute(3456u, 24u, 256u, 4096u).listsBytes));
}

// --- oracle -----------------------------------------------------------------------------------------
/// Light-major brute force over the oracle's AABBs: every cluster, every light in ascending index.
void bruteForceGrid(const ClusterDesc& desc, const ClusterCameraDesc& camera, const OracleLights& lights,
                    const std::vector<ClusterAABB>& aabbs, ClusterGridSoA& out) {
    const clustered_kernel::CameraView view = clustered_kernel::make_camera(camera);
    const u32 total = static_cast<u32>(lights.points.size() + lights.spots.size());
    const u32 cap = desc.maxLightsPerCluster > 0u ? std::min(desc.maxLightsPerCluster, total) : total;
    out.grid.assign(desc.clusterCount(), ClusterGridEntry{});
    out.lightList.clear();
    for (u32 c = 0; c < desc.clusterCount(); ++c) {
        out.grid[c].offset = static_cast<u32>(out.lightList.size());
        u32 count = 0;
        for (u32 i = 0; i < total; ++i) {
            const bool point = i < lights.points.size();
            const Vec3 p = point ? lights.points[i].position : lights.spots[i - lights.points.size()].position;
            const f32 r = point ? lights.points[i].radius : lights.spots[i - lights.points.size()].radius;
            if (!(r > 0.f) || !std::isfinite(r)) {
                continue;
            }
            if (clustered_kernel::sphere_intersects_aabb(clustered_kernel::world_to_view(view, p), r, aabbs[c]) && count < cap) {
                out.lightList.push_back(i);
                ++count;
            }
        }
        out.grid[c].count = count;
    }
}

/// The oracle's bounds -> bin -> cull with the candidate window widened by `pad` slices (pad 1 = the
/// oracle itself; the GPU uses kSliceWindowPad).
void widenedGrid(const ClusterDesc& desc, const ClusterCameraDesc& camera, const OracleLights& lights,
                 const std::vector<ClusterAABB>& aabbs, u32 pad, ClusterGridSoA& out) {
    const clustered_kernel::CameraView view = clustered_kernel::make_camera(camera);
    const u32 total = static_cast<u32>(lights.points.size() + lights.spots.size());
    std::vector<ClusterLightBounds> bounds(total);
    clustered_kernel::BoundsParams bp{};
    bp.grid = clustered_kernel::make_grid(desc);
    bp.camera = view;
    bp.point_lights = {lights.points.data(), static_cast<u32>(lights.points.size())};
    bp.spot_lights = {lights.spots.data(), static_cast<u32>(lights.spots.size())};
    bp.out_bounds = {bounds.data(), total};
    for (u32 i = 0; i < total; ++i) {
        kernel::LaunchIndex idx{};
        idx.linear = i;
        clustered_kernel::BoundsKernel{}(idx, bp);
        ClusterLightBounds& b = bounds[i];
        if (b.sliceLo <= b.sliceHi) {
            const u32 lo = b.sliceLo + 1u; // undo the oracle's pad 1 (clamped at 0 is fine: pad only grows)
            b.sliceLo = lo > pad ? lo - pad : 0u;
            b.sliceHi = std::min(b.sliceHi + pad - 1u, desc.slicesZ - 1u);
        }
    }
    const u32 cap = std::min(desc.maxLightsPerCluster, total);
    out.grid.assign(desc.clusterCount(), ClusterGridEntry{});
    out.lightList.clear();
    for (u32 c = 0; c < desc.clusterCount(); ++c) {
        const u32 slice = c % desc.slicesZ;
        out.grid[c].offset = static_cast<u32>(out.lightList.size());
        u32 count = 0;
        for (u32 i = 0; i < total; ++i) {
            if (slice < bounds[i].sliceLo || slice > bounds[i].sliceHi) {
                continue;
            }
            if (clustered_kernel::sphere_intersects_aabb(bounds[i].center, bounds[i].radius, aabbs[c]) && count < cap) {
                out.lightList.push_back(i);
                ++count;
            }
        }
        out.grid[c].count = count;
    }
}

bool sameGrid(const ClusterGridSoA& a, const ClusterGridSoA& b) {
    if (a.grid.size() != b.grid.size() || a.lightList != b.lightList) {
        return false;
    }
    for (usize i = 0; i < a.grid.size(); ++i) {
        if (a.grid[i].offset != b.grid[i].offset || a.grid[i].count != b.grid[i].count) {
            return false;
        }
    }
    return true;
}

void testOracle() {
    std::mt19937 rng(77);
    std::vector<gpu_scene::GpuLight> table = randomLights(rng, 20, 10, true);
    OracleLights o{};
    makeOracleLights(table.data(), static_cast<u32>(table.size()), o);
    expect(o.points.size() == 19u && o.spots.size() == 10u, "points / spots (free slot skipped)");
    expect(o.directional.size() == 1u && o.directional[0] == 30u, "directional slot listed");
    expect(o.slotOfIndex.size() == 29u && o.slotOfIndex[3] == 4u && o.slotOfIndex[19] == 20u, "index -> slot map");
    expect(o.monotone, "points below spots: monotone");
    std::swap(table[0], table[25]);
    OracleLights swapped{};
    makeOracleLights(table.data(), static_cast<u32>(table.size()), swapped);
    expect(!swapped.monotone, "a spot below a point: not monotone");

    ClusterGridSoA g{};
    g.grid = {{0u, 2u}, {2u, 1u}};
    g.lightList = {0u, 19u, 3u};
    ClusterGridSoA t{};
    translateToSlots(g, o, t);
    expect(t.lightList == std::vector<u32>{0u, 20u, 4u} && t.grid[1].offset == 2u, "translateToSlots");

    // oracleLightGrid == brute force; widening the window changes no list.
    ClusterDesc desc{};
    u32 cameras = 0;
    u64 entries = 0;
    u32 mismatchBrute = 0;
    u32 mismatchPad2 = 0;
    u32 mismatchFull = 0;
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 k = 0; k < 6u; ++k) {
        desc.maxLightsPerCluster = k % 3u == 2u ? 16u : 256u;
        std::vector<gpu_scene::GpuLight> lights = randomLights(rng, 2500, 1596, false);
        OracleLights ol{};
        makeOracleLights(lights.data(), static_cast<u32>(lights.size()), ol);
        const Vec3 eye{u(rng) * 3.f, 1.f + u(rng), 2.f + u(rng)};
        const ClusterCameraDesc cam = makeCamera(eye, {u(rng) * 4.f, -0.5f, -20.f}, 256, 192);
        ClusterGridSoA oracle{};
        oracleLightGrid(desc, cam, ol, oracle, kernel::Backend::CpuReference);
        ClusterGridSoA brute{};
        bruteForceGrid(desc, cam, ol, oracle.aabbs, brute);
        ClusterGridSoA pad2{};
        widenedGrid(desc, cam, ol, oracle.aabbs, kSliceWindowPad, pad2);
        ClusterGridSoA full{};
        widenedGrid(desc, cam, ol, oracle.aabbs, 64u, full);
        mismatchBrute += sameGrid(oracle, brute) ? 0u : 1u;
        mismatchPad2 += sameGrid(oracle, pad2) ? 0u : 1u;
        mismatchFull += sameGrid(oracle, full) ? 0u : 1u;
        ++cameras;
        entries += oracle.lightList.size();
        ClusterGridSoA parallel{};
        oracleLightGrid(desc, cam, ol, parallel, kernel::Backend::CpuParallel);
        expect(sameGrid(oracle, parallel), "oracleLightGrid CpuReference == CpuParallel");
    }
    std::printf("oracle: %u cameras x 4096 lights, %llu list entries; grids != brute force: %u, != pad-2 window: %u, "
                "!= full-grid window: %u\n",
                cameras, static_cast<unsigned long long>(entries), mismatchBrute, mismatchPad2, mismatchFull);
    expect(entries > 10000u, "the oracle scenes fill the grid");
    expect(mismatchBrute == 0u, "oracle grid == brute-force light-major assignment");
    expect(mismatchPad2 == 0u && mismatchFull == 0u, "a wider candidate window changes no list");
}

// --- brdf -------------------------------------------------------------------------------------------
Vec3 hemisphere(f64 theta, f64 phi) {
    return {static_cast<f32>(std::sin(theta) * std::cos(phi)), static_cast<f32>(std::sin(theta) * std::sin(phi)),
            static_cast<f32>(std::cos(theta))};
}

void testBrdf() {
    SurfaceSample s{};
    s.normal = {0.f, 0.f, 1.f};
    s.albedo = {1.f, 1.f, 1.f};
    s.metallic = 0.f;
    const Vec3 v = Vec3{0.3f, 0.f, 1.f}.normalized();
    expect(brdf_cos(s, v, Vec3{0.f, 0.f, -1.f}).x == 0.f && brdf_cos(s, v, Vec3{1.f, 0.f, 0.f}).x == 0.f,
           "N.L <= 0 gives 0");
    // Reciprocity: f(v, l) = f(l, v)  <=>  brdf_cos(v, l) / NoL == brdf_cos(l, v) / NoV.
    f64 worstRecip = 0.0;
    std::mt19937 rng(5);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    for (u32 i = 0; i < 2000u; ++i) {
        s.roughness = 0.05f + 0.95f * u(rng);
        s.metallic = u(rng);
        const Vec3 a = hemisphere(1.5 * u(rng), 6.28 * u(rng));
        const Vec3 b = hemisphere(1.5 * u(rng), 6.28 * u(rng));
        const f64 fab = brdf_cos(s, a, b).y / b.z;
        const f64 fba = brdf_cos(s, b, a).y / a.z;
        worstRecip = std::max(worstRecip, std::fabs(fab - fba) / std::max({std::fabs(fab), std::fabs(fba), 1e-6}));
    }
    expect(worstRecip < 1e-4, "BRDF reciprocity");
    // Energy: albedo 1 dielectric at normal-ish incidence, hemisphere quadrature of brdf_cos <= 1.
    s.metallic = 0.f;
    f64 maxEnergy = 0.0;
    f64 minEnergy = 10.0;
    for (const f32 rough : {0.05f, 0.25f, 0.5f, 0.75f, 1.f}) {
        s.roughness = rough;
        const Vec3 view = hemisphere(0.3, 0.0);
        constexpr u32 kTheta = 512;
        constexpr u32 kPhi = 256;
        f64 sum = 0.0;
        for (u32 i = 0; i < kTheta; ++i) {
            // Uniform in cos(theta): dω = d(cosθ) dφ.
            const f64 ct = (static_cast<f64>(i) + 0.5) / kTheta;
            const f64 theta = std::acos(ct);
            for (u32 j = 0; j < kPhi; ++j) {
                const f64 phi = 6.283185307179586 * (static_cast<f64>(j) + 0.5) / kPhi;
                sum += brdf_cos(s, view, hemisphere(theta, phi)).y;
            }
        }
        const f64 energy = sum * (1.0 / kTheta) * (6.283185307179586 / kPhi);
        maxEnergy = std::max(maxEnergy, energy);
        minEnergy = std::min(minEnergy, energy);
    }
    std::printf("brdf: reciprocity worst %.2e; dielectric albedo-1 energy in [%.3f, %.3f] over roughness 0.05..1 "
                "(no multi-scatter compensation yet)\n",
                worstRecip, minEnergy, maxEnergy);
    expect(maxEnergy <= 1.02 && minEnergy > 0.6, "energy bounded (<= 1 up to quadrature error)");
    // Spot cone.
    expect(spot_cone(0.95f, 0.9f, 0.7f) == 1.f && spot_cone(0.6f, 0.9f, 0.7f) == 0.f, "spot cone saturates");
    f32 prev = -1.f;
    bool monotone = true;
    for (u32 i = 0; i <= 100u; ++i) {
        const f32 c = spot_cone(0.6f + 0.004f * static_cast<f32>(i), 0.9f, 0.7f);
        monotone = monotone && c >= prev;
        prev = c;
    }
    expect(monotone, "spot cone monotone in cos");
    // Range and directional.
    s.roughness = 0.5f;
    s.position = {0.f, 0.f, 0.f};
    const gpu_scene::GpuLight p = pointLight({0.f, 0.f, 2.f}, 2.f, {1.f, 1.f, 1.f}, 5.f);
    const gpu_scene::GpuLight pIn = pointLight({0.f, 0.f, 1.99f}, 2.f, {1.f, 1.f, 1.f}, 5.f);
    const gpu_scene::GpuLight sp = spotLight({0.f, 0.f, 2.f}, {0.f, 0.f, -1.f}, 2.f, 0.9f, 0.7f, 5.f);
    expect(light_contribution(p, s, v).x == 0.f && light_contribution(sp, s, v).x == 0.f, "0 at d >= range");
    expect(light_contribution(pIn, s, v).x > 0.f, "> 0 inside the range");
    const gpu_scene::GpuLight spAway = spotLight({0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, 3.f, 0.9f, 0.7f, 5.f);
    expect(light_contribution(spAway, s, v).x == 0.f, "spot pointing away gives 0");
    const gpu_scene::GpuLight d = directionalLight({0.f, 0.f, -1.f}, 2.f);
    SurfaceSample far = s;
    far.position = {100.f, -50.f, 7.f};
    expect(light_contribution(d, s, v).x == light_contribution(d, far, v).x && light_contribution(d, s, v).x > 0.f,
           "directional independent of position");
    gpu_scene::GpuLight none{};
    expect(light_contribution(none, s, v).x == 0.f, "free slot contributes nothing");
}

// --- shade ------------------------------------------------------------------------------------------
struct SyntheticGBuffer {
    u32 w = 0, h = 0;
    std::vector<f32> depth;
    std::vector<Vec4> rt0, rt1, rt2, rt5;
};

/// A ground plane y = -1 under the camera, sky above; per-pixel material variation.
SyntheticGBuffer makeGBuffer(const ClusterCameraDesc& cam, u32 w, u32 h) {
    SyntheticGBuffer g{};
    g.w = w;
    g.h = h;
    g.depth.assign(static_cast<usize>(w) * h, 1.f);
    g.rt0.assign(g.depth.size(), Vec4{});
    g.rt1 = g.rt2 = g.rt5 = g.rt0;
    const clustered_kernel::CameraView view = clustered_kernel::make_camera(cam);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 sx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
            const f32 sy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
            // Ray through the pixel at view depth 1, intersected with y = -1.
            const Vec3 dir = clustered_kernel::view_to_world(view, clustered_kernel::view_position_from_screen(sx, sy, 1.f, view.tan_x, view.tan_y)) - view.position;
            const usize i = static_cast<usize>(y) * w + x;
            if (dir.y >= -1e-3f) {
                continue;
            }
            const f32 t = (-1.f - view.position.y) / dir.y; // view depth (dir has view depth 1)
            if (t > cam.farPlane * 0.9f) {
                continue;
            }
            const f32 n = cam.nearPlane;
            const f32 f = cam.farPlane;
            g.depth[i] = f * (t - n) / (t * (f - n));
            const Vec3 nrm = Vec3{0.1f * std::sin(0.3f * static_cast<f32>(x)), 1.f, 0.1f * std::cos(0.2f * static_cast<f32>(y))}.normalized();
            const f32 l1 = std::fabs(nrm.x) + std::fabs(nrm.y) + std::fabs(nrm.z);
            // Signed octahedral encode (gbuffer.glsl oct_encode_signed), AO in .w.
            const Vec3 q = nrm * (1.f / l1);
            g.rt0[i].w = 0.5f + 0.5f * static_cast<f32>((x + y) % 2u);
            if (q.z >= 0.f) {
                g.rt0[i].x = q.x;
                g.rt0[i].y = q.y;
            } else {
                g.rt0[i].x = (1.f - std::fabs(q.y)) * (q.x >= 0.f ? 1.f : -1.f);
                g.rt0[i].y = (1.f - std::fabs(q.x)) * (q.y >= 0.f ? 1.f : -1.f);
            }
            g.rt1[i] = {0.2f + 0.6f * sx, 0.5f, 0.8f - 0.5f * sy, 1.f};
            g.rt2[i] = {0.1f + 0.8f * sy, (x / 16u) % 3u == 0u ? 1.f : 0.f, 0.f, 0.f};
            g.rt5[i] = (x % 37u == 0u) ? Vec4{0.5f, 0.2f, 0.1f, 0.f} : Vec4{};
        }
    }
    return g;
}

ShadeReferenceDesc shadeDesc(const SyntheticGBuffer& g, const ClusterDesc& desc, const ClusterCameraDesc& cam,
                             const std::vector<gpu_scene::GpuLight>& lights, const ClusterGridSoA* grid,
                             const std::vector<u32>* directional) {
    ShadeReferenceDesc d{};
    d.width = g.w;
    d.height = g.h;
    d.desc = desc;
    d.camera = cam;
    d.ambient = {0.02f, 0.03f, 0.04f};
    d.gbuffer = GBufferTexels{g.depth.data(), g.rt0.data(), g.rt1.data(), g.rt2.data(), g.rt5.data()};
    d.lights = lights.data();
    d.lightCount = static_cast<u32>(lights.size());
    d.grid = grid;
    d.directional = directional;
    return d;
}

// Independent f64 evaluation of one light (the formulas of clustered_gpu_kernel.hpp).
struct D3 {
    f64 x = 0, y = 0, z = 0;
};
D3 d3(Vec3 v) { return {v.x, v.y, v.z}; }
f64 ddot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 dnorm(D3 a) {
    const f64 l = std::sqrt(ddot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

D3 referenceLight(const gpu_scene::GpuLight& light, D3 pos, D3 n, D3 v, D3 albedo, f64 rough, f64 metal) {
    D3 l{};
    f64 att = 1.0;
    if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
        l = dnorm({-light.direction[0], -light.direction[1], -light.direction[2]});
    } else {
        const D3 to{light.position[0] - pos.x, light.position[1] - pos.y, light.position[2] - pos.z};
        const f64 d = std::sqrt(ddot(to, to));
        if (d >= light.range) {
            return {};
        }
        const f64 r = d / light.range;
        const f64 w = std::clamp(1.0 - r * r * r * r, 0.0, 1.0);
        att = w * w / std::max(d * d, 1e-4);
        l = {to.x / d, to.y / d, to.z / d};
        if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
            const D3 axis = dnorm({light.direction[0], light.direction[1], light.direction[2]});
            const f64 t = std::clamp((-ddot(l, axis) - light.cosOuter) / std::max<f64>(light.cosInner - light.cosOuter, 1e-4), 0.0, 1.0);
            att *= t * t;
        }
    }
    const f64 nl = ddot(n, l);
    if (nl <= 0.0 || att == 0.0) {
        return {};
    }
    const D3 h = dnorm({v.x + l.x, v.y + l.y, v.z + l.z});
    const f64 nv = std::max(ddot(n, v), 1e-4);
    const f64 nh = std::max(ddot(n, h), 0.0);
    const f64 vh = std::max(ddot(v, h), 0.0);
    const f64 rr = std::clamp(rough, 0.045, 1.0);
    const f64 a2 = rr * rr * rr * rr;
    const f64 dd = (nh * a2 - nh) * nh + 1.0;
    const f64 D = a2 / (3.14159265358979 * dd * dd);
    const f64 vis = 0.5 / std::max(nl * std::sqrt(nv * nv * (1 - a2) + a2) + nv * std::sqrt(nl * nl * (1 - a2) + a2), 1e-5);
    const f64 f5 = std::pow(std::clamp(1.0 - vh, 0.0, 1.0), 5.0);
    auto ch = [&](f64 alb) {
        const f64 f0 = 0.04 + (alb - 0.04) * metal;
        const f64 F = f0 + (1 - f0) * f5;
        return ((1 - F) * (1 - metal) / 3.14159265358979 * alb + D * vis * F) * nl;
    };
    const f64 s = light.intensity * att;
    return {ch(albedo.x) * light.color[0] * s, ch(albedo.y) * light.color[1] * s, ch(albedo.z) * light.color[2] * s};
}

void testShade() {
    constexpr u32 kW = 96;
    constexpr u32 kH = 64;
    const ClusterCameraDesc cam = makeCamera({0.f, 1.f, 3.f}, {0.5f, -0.8f, -20.f}, kW, kH);
    const SyntheticGBuffer g = makeGBuffer(cam, kW, kH);
    ClusterDesc desc{};
    std::mt19937 rng(11);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    std::vector<gpu_scene::GpuLight> lights;
    for (u32 i = 0; i < 600u; ++i) {
        const Vec3 p{u(rng) * 10.f, -1.f + 1.5f * (u(rng) * 0.5f + 0.5f), -2.f - 30.f * (u(rng) * 0.5f + 0.5f)};
        lights.push_back(i < 400u ? pointLight(p, 1.f + 2.f * (u(rng) * 0.5f + 0.5f), {1.f, 0.7f, 0.5f}, 3.f)
                                  : spotLight(p, {u(rng) * 0.5f, -1.f, u(rng) * 0.5f}, 3.f, 0.95f, 0.8f, 6.f));
    }
    lights.push_back(directionalLight({0.4f, -1.f, -0.3f}, 0.7f));
    OracleLights o{};
    makeOracleLights(lights.data(), static_cast<u32>(lights.size()), o);
    ClusterGridSoA oracle{};
    oracleLightGrid(desc, cam, o, oracle);
    ClusterGridSoA grid{};
    translateToSlots(oracle, o, grid);

    std::vector<Vec4> ref;
    std::vector<Vec4> par;
    const u32 shaded = shadeReferenceFrame(shadeDesc(g, desc, cam, lights, &grid, &o.directional), ref, kernel::Backend::CpuReference);
    shadeReferenceFrame(shadeDesc(g, desc, cam, lights, &grid, &o.directional), par, kernel::Backend::CpuParallel);
    expect(std::memcmp(ref.data(), par.data(), ref.size() * sizeof(Vec4)) == 0, "CpuReference == CpuParallel bit for bit");
    u32 sky = 0;
    bool skyBlack = true;
    for (usize i = 0; i < ref.size(); ++i) {
        if (g.depth[i] >= 1.f) {
            ++sky;
            skyBlack = skyBlack && ref[i].x == 0.f && ref[i].y == 0.f && ref[i].z == 0.f && ref[i].w == 0.f;
        }
    }
    expect(shaded + sky == kW * kH && shaded > kW * kH / 3u && skyBlack, "sky pixels are (0,0,0,0), ground shaded");

    // Cluster lists == every light (every light outside a pixel's list contributes exactly 0).
    ClusterGridSoA all{};
    all.grid.assign(desc.clusterCount(), ClusterGridEntry{});
    for (u32 slot = 0; slot < lights.size(); ++slot) {
        if (lights[slot].type != static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
            all.lightList.push_back(slot);
        }
    }
    for (ClusterGridEntry& e : all.grid) {
        e = {0u, static_cast<u32>(all.lightList.size())};
    }
    std::vector<Vec4> brute;
    shadeReferenceFrame(shadeDesc(g, desc, cam, lights, &all, &o.directional), brute, kernel::Backend::CpuParallel);
    u32 bitEqual = 0;
    f64 worst = 0.0;
    for (usize i = 0; i < ref.size(); ++i) {
        bitEqual += std::memcmp(&ref[i], &brute[i], sizeof(Vec4)) == 0 ? 1u : 0u;
        const f64 scale = std::max({std::fabs(static_cast<f64>(brute[i].x)), std::fabs(static_cast<f64>(brute[i].y)),
                                    std::fabs(static_cast<f64>(brute[i].z)), 1e-3});
        worst = std::max({worst, std::fabs(ref[i].x - brute[i].x) / scale, std::fabs(ref[i].y - brute[i].y) / scale,
                          std::fabs(ref[i].z - brute[i].z) / scale});
    }
    std::printf("shade: %u of %u pixels shaded; cluster lists vs all lights: %u / %u pixels bit-identical, worst rel %.2e\n",
                shaded, kW * kH, bitEqual, kW * kH, worst);
    expect(bitEqual == kW * kH, "cluster-list shading == all-lights shading, bit for bit");

    // Independent f64 evaluation at a handful of pixels.
    const ShadeParams params = makeShadeParams(shadeDesc(g, desc, cam, lights, &all, &o.directional), nullptr);
    f64 worstF64 = 0.0;
    u32 checked = 0;
    for (u32 y = 2; y < kH; y += 7) {
        for (u32 x = 1; x < kW; x += 9) {
            const usize i = static_cast<usize>(y) * kW + x;
            if (g.depth[i] >= 1.f) {
                continue;
            }
            const f32 sx = (static_cast<f32>(x) + 0.5f) / kW;
            const f32 sy = (static_cast<f32>(y) + 0.5f) / kH;
            const f32 vd = clustered_kernel::view_depth_from_device_depth(g.depth[i], cam.nearPlane, cam.farPlane, false);
            const Vec3 pos = clustered_kernel::view_to_world(params.camera, clustered_kernel::view_position_from_screen(sx, sy, vd, params.camera.tan_x, params.camera.tan_y));
            const Vec3 n = oct_decode_signed(g.rt0[i].x, g.rt0[i].y);
            const D3 v = dnorm(d3(cam.position - pos));
            D3 sum{g.rt5[i].x + 0.02 * g.rt1[i].x * g.rt0[i].w, g.rt5[i].y + 0.03 * g.rt1[i].y * g.rt0[i].w,
                   g.rt5[i].z + 0.04 * g.rt1[i].z * g.rt0[i].w};
            for (const gpu_scene::GpuLight& l : lights) {
                const D3 c = referenceLight(l, d3(pos), d3(n), v, {g.rt1[i].x, g.rt1[i].y, g.rt1[i].z}, g.rt2[i].x, g.rt2[i].y);
                sum = {sum.x + c.x, sum.y + c.y, sum.z + c.z};
            }
            const f64 scale = std::max({std::fabs(sum.x), std::fabs(sum.y), std::fabs(sum.z), 1e-4});
            worstF64 = std::max({worstF64, std::fabs(ref[i].x - sum.x) / scale, std::fabs(ref[i].y - sum.y) / scale,
                                 std::fabs(ref[i].z - sum.z) / scale});
            ++checked;
        }
    }
    std::printf("shade: %u pixels vs an independent f64 evaluation: worst relative error %.2e\n", checked, worstF64);
    expect(checked > 20u && worstF64 < 1e-5, "reference kernel == f64 evaluation within 1e-5 relative");
}

// --- api --------------------------------------------------------------------------------------------
void testApi() {
    const LightingCapabilities caps = queryLightingCapabilities(nullptr);
    expect(!caps.lighting && caps.reason != nullptr, "no device: not usable");
    ClusteredLighting lighting;
    ClusteredLightingDesc d{};
    expect(!lighting.init(d) && !lighting.valid(), "init without a device fails");
    LightingFrameDesc f{};
    expect(!lighting.beginFrame(1, f), "beginFrame on an invalid instance fails");
    rg::Graph graph;
    const LightingGraphRefs refs = lighting.importInto(graph);
    expect(!refs.work.valid() && !refs.lists.valid() && !refs.output.valid(), "no refs from an invalid instance");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "oracle") {
        testOracle();
    }
    if (all || suite == "brdf") {
        testBrdf();
    }
    if (all || suite == "shade") {
        testShade();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
