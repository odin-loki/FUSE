// B7.5 gate row (docs/plans/FUSE_MASTER_PLAN.md): "Terrain-SVO cave correctly renders below terrain
// surface — SVO ray march transitions from heightfield".
//
// Deterministic CPU render: every pixel of a pinhole camera is traced through `Terrain::ray_cast`
// (the combined heightfield + SVO cave ray march) over a sloped terrain with one cave breaking the
// surface and two caves buried below it. Each pixel is compared with an independent exact
// reference: a voxel DDA over the analytic scene (the planar heightfield intersected with the
// complement of the carved voxels, carved by the same voxel-centre rule the terrain uses), which
// gives the exact first hit, which surface it lies on (heightfield vs cave wall) and how long the
// ray stays inside rock there. Asserts:
//  - per-pixel surface id and depth agree with the reference (depth within 2 cm);
//  - rays that enter through the cave mouth are handed from the heightfield to the SVO and hit
//    cave walls, and there are plenty of them;
//  - buried caves never show through the surface;
//  - no seam / gap pixels: no pixel where the reference hits rock but the march sees sky or
//    something behind it, in particular on the heightfield/cave boundary in image space.
// The image (render | reference | error mask) is written as a PPM next to the test binary.
// The GPU (Vulkan) terrain + SVO render of the same scene remains a manual visual check.

#include <fuse/core/init.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/terrain.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef FUSE_TERRAIN_TEST_OUTPUT_DIR
#define FUSE_TERRAIN_TEST_OUTPUT_DIR "."
#endif

namespace {

using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::u32;
using fuse::u8;
using namespace fuse::terrain;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

#if defined(FUSE_TERRAIN_HAS_SVO) && FUSE_TERRAIN_HAS_SVO

enum class Surface : u8 { Sky = 0, Heightfield = 1, Cave = 2 };

// Scene: planar heightfield h(x, z) = kH0 + kSlopeX x + kSlopeZ z (bilinear sampling reproduces a
// plane exactly), 1 m texels and 1 m cave voxels.
constexpr f64 kH0 = 36.0;
constexpr f64 kSlopeX = 0.04;
constexpr f64 kSlopeZ = 0.02;
constexpr u32 kResolution = 257;
constexpr f32 kWorldSize = 256.f;
constexpr f32 kMaxHeight = 64.f;
constexpr u32 kSvoDepth = 8;

struct CaveSphere {
    vec3 centre;
    f32 radius;
};

f64 planeHeight(f64 x, f64 z) {
    return kH0 + kSlopeX * x + kSlopeZ * z;
}

/// Reference scene: the analytic plane and the carved voxel set, independent of the SVO and march.
struct ReferenceScene {
    vec3 origin{};
    f32 voxel = 1.f;
    std::vector<CaveSphere> caves;

    /// Same voxel-centre rule and f32 arithmetic as `TerrainCaves::carve_sphere`.
    [[nodiscard]] bool carved(s32 x, s32 y, s32 z) const {
        const vec3 c{origin.x + (static_cast<f32>(x) + 0.5f) * voxel, origin.y + (static_cast<f32>(y) + 0.5f) * voxel,
                     origin.z + (static_cast<f32>(z) + 0.5f) * voxel};
        for (const CaveSphere& cave : caves) {
            const vec3 d = c - cave.centre;
            if (d.dot(d) <= cave.radius * cave.radius) {
                return true;
            }
        }
        return false;
    }
};

struct ReferenceHit {
    Surface surface = Surface::Sky;
    f64 t = 0.0;
    f64 rockRun = 0.0;     ///< distance the ray stays inside rock from the hit
    bool crossedPlaneInCave = false; ///< ray passed below the heightfield inside cave air first
};

/// Exact first hit along the ray against {y <= plane} minus the carved voxels, by voxel DDA.
ReferenceHit traceReference(const ReferenceScene& scene, vec3 o, vec3 d, f64 tMax) {
    const f64 origin[3] = {o.x, o.y, o.z};
    const f64 dir[3] = {d.x, d.y, d.z};
    const f64 gridOrigin[3] = {scene.origin.x, scene.origin.y, scene.origin.z};
    const f64 vs = scene.voxel;
    s32 cell[3];
    s32 stepDir[3];
    f64 tNext[3];
    f64 tDelta[3];
    for (int k = 0; k < 3; ++k) {
        const f64 local = (origin[k] - gridOrigin[k]) / vs;
        cell[k] = static_cast<s32>(std::floor(local));
        if (dir[k] > 0.0) {
            stepDir[k] = 1;
            tNext[k] = ((static_cast<f64>(cell[k]) + 1.0) - local) * vs / dir[k];
            tDelta[k] = vs / dir[k];
        } else if (dir[k] < 0.0) {
            stepDir[k] = -1;
            tNext[k] = (local - static_cast<f64>(cell[k])) * vs / -dir[k];
            tDelta[k] = vs / -dir[k];
        } else {
            stepDir[k] = 0;
            tNext[k] = 1e300;
            tDelta[k] = 1e300;
        }
    }
    // g(t) = y(t) - h(x(t), z(t)) is linear in t on the plane: rock where g <= 0.
    const f64 g0 = origin[1] - planeHeight(origin[0], origin[2]);
    const f64 gSlope = dir[1] - (kSlopeX * dir[0] + kSlopeZ * dir[2]);
    const auto g = [&](f64 t) { return g0 + gSlope * t; };

    ReferenceHit hit{};
    f64 t0 = 0.0;
    bool inRock = false;
    bool previousCarved = false;
    while (t0 < tMax) {
        const f64 t1 = std::min({tNext[0], tNext[1], tNext[2], tMax});
        const bool carved = scene.carved(cell[0], cell[1], cell[2]);
        if (!inRock) {
            if (carved) {
                if (g(t1) < 0.0 || g(t0) < 0.0) {
                    hit.crossedPlaneInCave = true;
                }
            } else if (g(t0) <= 0.0) {
                hit.surface = previousCarved ? Surface::Cave : Surface::Heightfield;
                hit.t = t0;
                inRock = true;
            } else if (gSlope < 0.0 && g(t1) <= 0.0) {
                hit.surface = Surface::Heightfield;
                hit.t = -g0 / gSlope;
                inRock = true;
            }
        } else {
            // Leaving rock: into carved air, or up through the plane.
            if (carved) {
                hit.rockRun = t0 - hit.t;
                return hit;
            }
            if (gSlope > 0.0 && g(t1) > 0.0) {
                hit.rockRun = -g0 / gSlope - hit.t;
                return hit;
            }
            if (t1 - hit.t > 4.0 * vs) {
                hit.rockRun = t1 - hit.t;
                return hit;
            }
        }
        previousCarved = carved;
        // Advance to the next cell.
        int axis = 0;
        if (tNext[1] < tNext[axis]) {
            axis = 1;
        }
        if (tNext[2] < tNext[axis]) {
            axis = 2;
        }
        t0 = t1;
        cell[axis] += stepDir[axis];
        tNext[axis] += tDelta[axis];
    }
    if (inRock) {
        hit.rockRun = tMax - hit.t;
    }
    return hit;
}

struct Rgb {
    u8 r, g, b;
};

Rgb shade(Surface surface, vec3 normal, f64 depth) {
    if (surface == Surface::Sky) {
        return {135, 180, 230};
    }
    const vec3 light = vec3{0.4f, 0.8f, -0.45f}.normalized();
    const f64 lambert = 0.35 + 0.65 * std::max(0.0, static_cast<f64>(std::fabs(normal.dot(light))));
    const f64 fog = std::clamp(1.0 - depth / 220.0, 0.35, 1.0);
    const f64 k = lambert * fog;
    return surface == Surface::Heightfield
               ? Rgb{static_cast<u8>(90 * k), static_cast<u8>(170 * k), static_cast<u8>(80 * k)}
               : Rgb{static_cast<u8>(210 * k), static_cast<u8>(130 * k), static_cast<u8>(60 * k)};
}

void testCaveRenderAgainstReference() {
    TerrainDesc desc{};
    desc.resolution = kResolution;
    desc.world_size = kWorldSize;
    desc.max_height = kMaxHeight;
    desc.chunk_resolution = 32;
    desc.has_svo_caves = true;
    desc.svo_depth = kSvoDepth;
    desc.async_loading = false;
    Terrain terrain{};
    terrain.init(desc);
    Heightfield& field = terrain.heightfield();
    const f32 texel = field.meters_per_texel();
    for (u32 z = 0; z < kResolution; ++z) {
        for (u32 x = 0; x < kResolution; ++x) {
            field.set_height(x, z, static_cast<f32>(planeHeight(x * texel, z * texel)));
        }
    }

    // One cave breaking the surface (top at y = 49 > h = 43.7) and two buried ones, the shallower
    // with only ~1.3 m of rock over it.
    const std::vector<CaveSphere> caves = {
        {{128.f, 40.f, 128.f}, 9.f},
        {{114.f, 30.f, 138.f}, 6.f},
        {{140.f, static_cast<f32>(planeHeight(140.0, 138.0)) - 5.f, 138.f}, 3.5f},
    };
    for (const CaveSphere& cave : caves) {
        terrain.carve_cave(cave.centre, cave.radius);
    }
    ReferenceScene scene;
    scene.origin = {0.f, kMaxHeight - kWorldSize, 0.f};
    scene.voxel = terrain.caves().voxel_size();
    scene.caves = caves;
    expectTrue(std::fabs(scene.voxel - kWorldSize / static_cast<f32>(1u << kSvoDepth)) < 1e-6f,
               "reference voxel grid matches the SVO leaf size");
    // Same step the hybrid march uses: a quarter of the finer of texel and voxel.
    const f64 marchStep = std::max(std::min(texel, scene.voxel) * 0.25f, 1e-3f);

    // Camera south of the cave mouth, looking down into it.
    constexpr u32 kWidth = 192;
    constexpr u32 kHeight = 128;
    // Pitched down ~50 degrees so every ray lands inside the terrain footprint (no horizon).
    const vec3 eye{128.f, 64.f, 106.f};
    const vec3 target{126.f, 40.f, 128.f};
    const vec3 forward = (target - eye).normalized();
    const vec3 worldUp{0.f, 1.f, 0.f};
    const auto cross = [](vec3 a, vec3 b) {
        return vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const vec3 right = cross(forward, worldUp).normalized();
    const vec3 up = cross(right, forward);
    const f32 tanHalf = std::tan(0.5f * 60.f * 3.14159265f / 180.f);
    const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);
    constexpr f32 kMaxDistance = 220.f;
    constexpr f64 kDepthTolerance = 0.02; // metres; the march bisects its bracket to ~1e-6 m

    const vec3 planeNormal = vec3{static_cast<f32>(-kSlopeX), 1.f, static_cast<f32>(-kSlopeZ)}.normalized();
    std::vector<Surface> renderId(kWidth * kHeight);
    std::vector<Surface> refId(kWidth * kHeight);
    std::vector<f64> renderDepth(kWidth * kHeight, 0.0);
    std::vector<f64> refDepth(kWidth * kHeight, 0.0);
    std::vector<vec3> renderNormal(kWidth * kHeight);
    std::vector<u8> sliver(kWidth * kHeight, 0u);
    std::vector<u8> bad(kWidth * kHeight, 0u);

    u32 mouthRays = 0;
    u32 mouthOk = 0;
    u32 caveRefPixels = 0;
    u32 slivers = 0;
    u32 sliversResolved = 0;
    u32 sliversOnSky = 0;
    u32 idMismatch = 0;
    u32 depthMismatch = 0;
    u32 gapPixels = 0;
    u32 surfaceNormalBad = 0;
    f64 maxDepthErr = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (u32 py = 0; py < kHeight; ++py) {
        for (u32 px = 0; px < kWidth; ++px) {
            const f32 sx = ((static_cast<f32>(px) + 0.5f) / static_cast<f32>(kWidth) * 2.f - 1.f) * tanHalf * aspect;
            const f32 sy = (1.f - (static_cast<f32>(py) + 0.5f) / static_cast<f32>(kHeight) * 2.f) * tanHalf;
            const vec3 dir = (forward + right * sx + up * sy).normalized();
            const u32 i = py * kWidth + px;

            vec3 hit{};
            vec3 normal{};
            f32 distance = 0.f;
            if (terrain.ray_cast(eye, dir, kMaxDistance, hit, normal, distance)) {
                const f32 surfaceY = field.sample_height(hit.x, hit.z);
                renderId[i] = hit.y < surfaceY - 1e-3f ? Surface::Cave : Surface::Heightfield;
                renderDepth[i] = distance;
                renderNormal[i] = normal;
            } else {
                renderId[i] = Surface::Sky;
            }

            const ReferenceHit ref = traceReference(scene, eye, dir, kMaxDistance);
            // Classify the reference hit the same way (below the surface = cave wall); a hit on a
            // voxel face flush with the surface counts as heightfield.
            Surface refSurface = ref.surface;
            if (refSurface == Surface::Cave) {
                const vec3 p = eye + dir * static_cast<f32>(ref.t);
                if (static_cast<f64>(p.y) >= planeHeight(p.x, p.z) - 1e-3) {
                    refSurface = Surface::Heightfield;
                }
            }
            refId[i] = refSurface;
            refDepth[i] = ref.t;
            // A rock sliver thinner than one march step can fall between samples; that is the
            // march's documented resolution, not a heightfield/SVO seam, so it is counted apart.
            if (refSurface != Surface::Sky && ref.rockRun < marchStep) {
                sliver[i] = 1u;
                ++slivers;
                const bool resolved = renderId[i] == refSurface && std::fabs(renderDepth[i] - ref.t) <= kDepthTolerance;
                sliversResolved += resolved ? 1u : 0u;
                // Missed slivers must still land on real geometry behind the lip, never on sky.
                sliversOnSky += renderId[i] == Surface::Sky ? 1u : 0u;
                continue;
            }
            if (refSurface == Surface::Cave) {
                ++caveRefPixels;
            }
            const bool depthOk = std::fabs(renderDepth[i] - ref.t) <= kDepthTolerance;
            if (refSurface != Surface::Sky &&
                (renderId[i] == Surface::Sky || renderDepth[i] > ref.t + kDepthTolerance)) {
                ++gapPixels; // saw sky or something behind the true surface
                bad[i] = 1u;
            }
            if (renderId[i] != refSurface) {
                ++idMismatch;
                bad[i] = 1u;
            } else if (refSurface != Surface::Sky) {
                maxDepthErr = std::max(maxDepthErr, std::fabs(renderDepth[i] - ref.t));
                if (!depthOk) {
                    ++depthMismatch;
                    bad[i] = 1u;
                }
            }
            if (refSurface == Surface::Heightfield && renderId[i] == Surface::Heightfield &&
                renderNormal[i].dot(planeNormal) < 0.999f) {
                ++surfaceNormalBad;
                bad[i] = 1u;
            }
            if (ref.crossedPlaneInCave && refSurface == Surface::Cave) {
                ++mouthRays;
                mouthOk += (renderId[i] == Surface::Cave && depthOk) ? 1u : 0u;
            }
        }
    }
    const f64 renderMs =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();

    // Seam check in image space: every pixel on the reference heightfield/cave boundary renders
    // the right surface at the right depth (no crack between the two representations).
    u32 seamPixels = 0;
    u32 seamBad = 0;
    for (u32 py = 0; py < kHeight; ++py) {
        for (u32 px = 0; px < kWidth; ++px) {
            const u32 i = py * kWidth + px;
            if (refId[i] == Surface::Sky || sliver[i] != 0u) {
                continue;
            }
            bool boundary = false;
            const s32 offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& o : offsets) {
                const s32 nx = static_cast<s32>(px) + o[0];
                const s32 ny = static_cast<s32>(py) + o[1];
                if (nx >= 0 && ny >= 0 && nx < static_cast<s32>(kWidth) && ny < static_cast<s32>(kHeight)) {
                    const Surface other = refId[static_cast<u32>(ny) * kWidth + static_cast<u32>(nx)];
                    boundary = boundary || (other != Surface::Sky && other != refId[i]);
                }
            }
            if (boundary) {
                ++seamPixels;
                seamBad += bad[i];
            }
        }
    }

    // Buried caves: no pixel may show them, and they must lie in view (their projected discs
    // overlap rendered heightfield pixels) for that to mean anything.
    u32 buriedInView = 0;
    for (size_t c = 1; c < caves.size(); ++c) {
        const vec3 toCave = caves[c].centre - eye;
        const f32 along = toCave.dot(forward);
        const f32 cx = toCave.dot(right) / (along * tanHalf * aspect);
        const f32 cy = toCave.dot(up) / (along * tanHalf);
        buriedInView += (along > 0.f && std::fabs(cx) < 0.9f && std::fabs(cy) < 0.9f) ? 1u : 0u;
    }

    const u32 pixels = kWidth * kHeight;
    std::printf("cave render %ux%u: %.0f ms, %u cave pixels (%u through the mouth, %u correct), %u seam pixels "
                "(%u bad)\n",
                kWidth, kHeight, renderMs, caveRefPixels, mouthRays, mouthOk, seamPixels, seamBad);
    std::printf("  vs reference: %u id mismatches, %u depth mismatches (max err %.4f m), %u gap pixels, %u "
                "sub-step slivers (%u still resolved), %u bad surface normals, %u buried caves in view\n",
                idMismatch, depthMismatch, maxDepthErr, gapPixels, slivers, sliversResolved, surfaceNormalBad,
                buriedInView);

    expectTrue(caveRefPixels > pixels / 20u, "cave interior covers a sizeable part of the image");
    expectTrue(mouthRays > pixels / 20u && mouthOk == mouthRays,
               "every ray entering the cave mouth transitions to the SVO and hits the cave wall");
    expectTrue(idMismatch == 0u, "per-pixel surface id (sky / heightfield / cave) matches the reference");
    expectTrue(depthMismatch == 0u, "per-pixel depth within 2 cm of the reference");
    expectTrue(gapPixels == 0u, "no gap pixels: nothing seen through rock");
    expectTrue(seamPixels > 50u && seamBad == 0u, "heightfield/cave boundary renders without seams");
    expectTrue(surfaceNormalBad == 0u, "heightfield pixels carry the surface normal");
    expectTrue(buriedInView == 2u, "both buried caves are inside the view (and stay hidden)");
    expectTrue(slivers < pixels / 100u, "sub-step rock slivers are rare (< 1% of pixels)");
    expectTrue(sliversOnSky == 0u, "a ray missing a sub-step rim sliver still hits the cave behind it");

    // Artifact: render | reference | error mask (red = mismatch, yellow = sub-step sliver).
    const std::string path = std::string(FUSE_TERRAIN_TEST_OUTPUT_DIR) + "/terrain_svo_cave_render.ppm";
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fprintf(file, "P6\n%u %u\n255\n", kWidth * 3u, kHeight);
        std::vector<Rgb> row(kWidth * 3u);
        for (u32 py = 0; py < kHeight; ++py) {
            for (u32 px = 0; px < kWidth; ++px) {
                const u32 i = py * kWidth + px;
                row[px] = shade(renderId[i], renderNormal[i], renderDepth[i]);
                const vec3 refNormal = refId[i] == Surface::Heightfield ? planeNormal : renderNormal[i];
                row[kWidth + px] = shade(refId[i], refNormal, refDepth[i]);
                row[2u * kWidth + px] = bad[i] != 0u      ? Rgb{255, 0, 0}
                                        : sliver[i] != 0u ? Rgb{255, 220, 0}
                                        : refId[i] == Surface::Cave ? Rgb{60, 60, 60}
                                                                    : Rgb{20, 20, 20};
            }
            std::fwrite(row.data(), sizeof(Rgb), row.size(), file);
        }
        std::fclose(file);
        std::printf("  wrote %s\n", path.c_str());
    } else {
        expectTrue(false, "write the cave render PPM artifact");
    }
    terrain.destroy();
}

#endif

} // namespace

int main() {
    fuse::core::initialize();
#if defined(FUSE_TERRAIN_HAS_SVO) && FUSE_TERRAIN_HAS_SVO
    testCaveRenderAgainstReference();
#else
    std::printf("fuse_b7_terrain_cave_render: skipped (built without fuse_scene SVO)\n");
#endif
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_b7_terrain_cave_render: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b7_terrain_cave_render: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
