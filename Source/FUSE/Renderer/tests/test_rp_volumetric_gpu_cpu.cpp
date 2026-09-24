// WP-8.1 froxel fog: CPU gates (stub-safe; the Lavapipe gates are test_rp_volumetric_gpu.cpp).
//   layout     FogFrameConstants / FogVolume / FogPush sizes; the GLSL and Slang mirrors (fog_common.*) field by
//              field (names, order, std430 offsets); work-buffer sections aligned and disjoint
//   oracle     alignment with the existing CPU oracles: the slice table == B5 FroxelSliceLayout (and == the
//              WP-2.1 clustered slice_near_z for the same range and count), froxel_index == B5
//              FroxelGridLayout::froxelIndex, the global medium == B5 sample_volumetric_fog_density bit for bit,
//              an aligned grid puts every froxel centre in cluster (x / 2, y / 2, z / 2)
//   lights     in-scattering through the WP-2.1 cluster lists == over every light (bit for bit), point falloff /
//              spot cone == the WP-2.1 CPU kernel's, Henyey-Greenstein normalisation
//   analytic   homogeneous medium: transmittance e^{-sigma d} and ambient in-scattering albedo L (1 - T) at every
//              slice boundary; height fog transmittance vs the B5 closed-form optical depth; apply interpolation
//   temporal   the CPU pipeline over many frames: static camera converges to the jitter-period mean and its
//              temporal variance falls below epsilon; moving camera: ghosting / trail metrics below thresholds and
//              far below a same-froxel (no reprojection) history
//   api        settings / constant resolution, jitter, FroxelFog without a device
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_kernel.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_reference.hpp>
#include <fuse/renderer/volumetric/volumetric_fog.hpp>

#include "test_rp_volumetric_gpu_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::volumetric_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace fk = fuse::renderer::volumetric_gpu::fog_kernel;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool sameBits(f32 a, f32 b) { return std::memcmp(&a, &b, 4u) == 0; }

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define FOG_FIELD(n) Field{#n, offsetof(FogFrameConstants, n)}
const Field kFields[] = {
    FOG_FIELD(lighting), FOG_FIELD(shadows), FOG_FIELD(current), FOG_FIELD(historyPrev), FOG_FIELD(historyCur),
    FOG_FIELD(integrated), FOG_FIELD(dump), FOG_FIELD(reserved0), FOG_FIELD(gridX), FOG_FIELD(gridY), FOG_FIELD(gridZ),
    FOG_FIELD(froxelCount), FOG_FIELD(width), FOG_FIELD(height), FOG_FIELD(inputDepth), FOG_FIELD(inputLit),
    Field{"output_", offsetof(FogFrameConstants, output)}, FOG_FIELD(flags), FOG_FIELD(frameIndex),
    FOG_FIELD(volumeCount), FOG_FIELD(position), FOG_FIELD(nearPlane), FOG_FIELD(right), FOG_FIELD(farPlane),
    FOG_FIELD(up), FOG_FIELD(tanX), FOG_FIELD(back), FOG_FIELD(tanY), FOG_FIELD(prevPosition), FOG_FIELD(reserved1),
    FOG_FIELD(prevRight), FOG_FIELD(prevTanX), FOG_FIELD(prevUp), FOG_FIELD(prevTanY), FOG_FIELD(prevBack),
    FOG_FIELD(reserved2), FOG_FIELD(jitter), FOG_FIELD(temporalAlpha), FOG_FIELD(density), FOG_FIELD(heightFalloff),
    FOG_FIELD(baseHeight), FOG_FIELD(anisotropy), FOG_FIELD(albedo), FOG_FIELD(invWidth), FOG_FIELD(ambient),
    FOG_FIELD(depthNear), FOG_FIELD(depthFar), FOG_FIELD(invGridX), FOG_FIELD(invGridY), FOG_FIELD(invHeight),
    FOG_FIELD(sliceDepth), FOG_FIELD(volumes),
};
#undef FOG_FIELD
#define VOL_FIELD(n) Field{#n, offsetof(FogVolume, n)}
const Field kVolumeFields[] = {VOL_FIELD(center), VOL_FIELD(shape),  VOL_FIELD(halfExtent), VOL_FIELD(density),
                               VOL_FIELD(albedo), VOL_FIELD(edge),   VOL_FIELD(reserved)};
#undef VOL_FIELD

/// Parses `struct <name> {` of a shader source: (name, std430 offset) per field. Nested struct arrays count
/// as 64-byte FogVolume elements (the only nested type).
bool parseShaderStruct(const std::string& path, const std::string& structName, std::vector<size_t>& offsets,
                       size_t& size, std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const std::string head = "struct " + structName + " {";
    const size_t begin = text.find(head);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + head.size(), end - begin - head.size()));
    std::string line;
    size_t offset = 0;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        size_t count = 1;
        const size_t bracket = decl.find('[');
        if (bracket != std::string::npos) {
            count = static_cast<size_t>(std::stoul(decl.substr(bracket + 1)));
            decl = decl.substr(0, bracket);
        }
        size_t bytes = 4u;
        size_t align = 4u;
        if (type == "uint64_t") {
            bytes = align = 8u;
        } else if (type.find("Volume") != std::string::npos) {
            bytes = 64u;
            align = 16u;
        }
        offset = (offset + align - 1u) / align * align;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = offset;
    return true;
}

void checkStruct(const char* file, const char* structName, const Field* fields, size_t fieldCount, size_t cppSize) {
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_FOG_SHADER_DIR) + "/" + file + "." + lang;
        const std::string name = std::string(std::strcmp(lang, "glsl") == 0 ? "FuseFog" : "Fog") + structName;
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, name, offsets, size, names);
        expect(parsed, "fog_common struct parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == cppSize;
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == fields[i].name && offsets[i] == fields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(), offsets[i],
                             fields[i].name, fields[i].offset);
            }
        }
        std::printf("layout: %s.%s %s %zu fields, %zu bytes\n", file, lang, name.c_str(), offsets.size(), size);
        expect(same, "shader struct == C++ record (names, order, offsets, size)");
    }
}

void testLayout() {
    expect(sizeof(FogFrameConstants) == 1360u, "FogFrameConstants is 1360 bytes");
    expect(sizeof(FogVolume) == 64u, "FogVolume is 64 bytes");
    expect(sizeof(FogPush) == 16u, "FogPush is 16 bytes");
    checkStruct("fog_common", "Frame", kFields, sizeof(kFields) / sizeof(kFields[0]), sizeof(FogFrameConstants));
    checkStruct("fog_common", "Volume", kVolumeFields, sizeof(kVolumeFields) / sizeof(kVolumeFields[0]), sizeof(FogVolume));
    for (u32 n : {1u, 27648u, 160u * 90u * 64u, 256u * 256u * 128u}) {
        const FogBufferLayout l = FogBufferLayout::compute(n);
        const u64 bytes = static_cast<u64>(n) * 16u;
        const u64 at[4] = {l.current, l.history[0], l.history[1], l.integrated};
        bool ok = true;
        for (u32 i = 0; i < 4u; ++i) {
            ok = ok && at[i] % 256u == 0u && at[i] + bytes <= l.workBytes;
            for (u32 j = i + 1u; j < 4u; ++j) {
                ok = ok && (at[i] + bytes <= at[j] || at[j] + bytes <= at[i]);
            }
        }
        expect(ok, "work sections are 256-aligned, disjoint and inside the buffer");
        std::printf("layout: %u froxels -> work buffer %.2f MiB\n", n, static_cast<f64>(l.workBytes) / (1024.0 * 1024.0));
    }
}

// --- helpers -----------------------------------------------------------------------------------------
struct Frame {
    FogFrameConstants c{};
    lighting_gpu::LightingFrameConstants lighting{};
    std::vector<gpu_scene::GpuLight> lights;
    FogLightLists lists;
    fk::LightView view{};
};

bool makeFrame(const FroxelFogSettings& s, const ClusterCameraDesc& cam, const ClusterCameraDesc* prev, bool history,
               u32 frameIndex, Frame& f, bool withLights = true) {
    if (!resolveFogConstants(s, cam, prev, history, frameIndex, 64, 36, f.c)) {
        return false;
    }
    f.lights = fog_test::lights();
    const u32 count = static_cast<u32>(f.lights.size());
    f.lighting = makeLightingView(fog_test::clusters(), cam, count);
    oracleFogLights(fog_test::clusters(), cam, f.lights.data(), count, f.lists);
    f.view = makeLightView(&f.lighting, f.lights.data(), f.lists);
    if (withLights) {
        f.c.flags |= kFogFlagLights;
    }
    return true;
}

// --- oracle ------------------------------------------------------------------------------------------
void testOracle() {
    const ClusterCameraDesc cam = fog_test::camera(0, 0, 256, 144);
    // Slice table == B5 FroxelSliceLayout (within the B5 grid limits) and == clustered slice_near_z.
    u32 sliceMismatch = 0;
    u32 sliceChecks = 0;
    for (u32 slices : {8u, 24u, 48u, 64u, 128u}) {
        FroxelFogSettings s = fog_test::settings();
        s.gridZ = slices;
        s.farPlane = fog_test::kFar; // the full camera range: the B5 layout spans [near, far]
        FogFrameConstants c{};
        expect(resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c), "resolve");
        FroxelGridDesc b5{};
        b5.tilesX = 16;
        b5.tilesY = 9;
        b5.slicesZ = slices;
        FroxelCameraDesc b5cam{};
        b5cam.nearPlane = cam.nearPlane;
        b5cam.farPlane = cam.farPlane;
        for (u32 z = 0; z < slices; ++z) {
            ++sliceChecks;
            const f32 n = FroxelSliceLayout::computeSliceNearZ(z, b5, b5cam);
            const f32 f = FroxelSliceLayout::computeSliceFarZ(z, b5, b5cam);
            const f32 cn = clustered_kernel::slice_near_z(z, slices, cam.nearPlane, cam.farPlane);
            if (!sameBits(c.sliceDepth[z], n) || !sameBits(c.sliceDepth[z], cn) ||
                (z + 1u < slices && !sameBits(c.sliceDepth[z + 1u], f))) {
                ++sliceMismatch;
            }
        }
    }
    std::printf("oracle: slice table vs B5 FroxelSliceLayout + clustered slice_near_z: %u / %u slices differ\n",
                sliceMismatch, sliceChecks);
    expect(sliceMismatch == 0u, "slice table == B5 FroxelSliceLayout == clustered slice_near_z (bit for bit)");

    // froxel_index == B5 FroxelGridLayout::froxelIndex.
    {
        FroxelFogSettings s = fog_test::settings();
        s.gridX = 32;
        s.gridY = 18;
        s.gridZ = 64;
        FogFrameConstants c{};
        resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c);
        FroxelGridDesc b5{};
        b5.tilesX = 32;
        b5.tilesY = 18;
        b5.slicesZ = 64;
        u32 bad = 0;
        for (u32 y = 0; y < 18u; ++y) {
            for (u32 x = 0; x < 32u; ++x) {
                for (u32 z = 0; z < 64u; ++z) {
                    bad += fk::froxel_index(c, x, y, z) != FroxelGridLayout::froxelIndex(x, y, z, b5) ? 1u : 0u;
                }
            }
        }
        expect(bad == 0u, "froxel_index == B5 FroxelGridLayout::froxelIndex");
    }

    // Global medium == B5 sample_volumetric_fog_density (no local volumes).
    {
        FroxelFogSettings s = fog_test::settings();
        s.volumeCount = 0;
        FogFrameConstants c{};
        resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c);
        std::mt19937 rng(7);
        std::uniform_real_distribution<f32> u(-20.f, 20.f);
        u32 bad = 0;
        for (u32 i = 0; i < 20000u; ++i) {
            const Vec3 p{u(rng), u(rng) * 0.5f, u(rng)};
            Vec3 sc{};
            const f32 e = fk::medium(c, p, sc);
            const f32 ref = sample_volumetric_fog_density(s.medium, p);
            bad += sameBits(e, ref) && sameBits(sc.x, s.medium.fog_color.x * ref) ? 0u : 1u;
        }
        std::printf("oracle: medium vs B5 sample_volumetric_fog_density: %u / 20000 points differ\n", bad);
        expect(bad == 0u, "global medium == B5 sample_volumetric_fog_density (bit for bit)");
    }

    // Aligned grid: froxel (x, y, z) centre -> cluster (x / 2, y / 2, z / 2) of the WP-2.1 16 x 9 x 24 grid
    // when the fog spans the clustered range with twice the resolution.
    {
        FroxelFogSettings s = fog_test::settings();
        s.gridX = 32;
        s.gridY = 18;
        s.gridZ = 48;
        s.farPlane = fog_test::kFar;
        FogFrameConstants c{};
        resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c);
        const lighting_gpu::LightingFrameConstants f = makeLightingView(fog_test::clusters(), cam, 1);
        u32 bad = 0;
        u32 unmapped = 0;
        for (u32 y = 0; y < c.gridY; ++y) {
            for (u32 x = 0; x < c.gridX; ++x) {
                for (u32 z = 0; z < c.gridZ; ++z) {
                    u32 cluster = 0;
                    if (!fk::light_cluster(f, fk::froxel_point(c, x, y, z, 0.5f, 0.5f, 0.5f), cluster)) {
                        ++unmapped;
                        continue;
                    }
                    bad += cluster != clustered_kernel::cluster_index(x / 2u, y / 2u, z / 2u, 16u, 24u) ? 1u : 0u;
                }
            }
        }
        std::printf("oracle: aligned 32x18x48 froxels in 16x9x24 clusters: %u misplaced, %u unmapped of %u\n", bad, unmapped,
                    c.froxelCount);
        expect(bad == 0u && unmapped == 0u, "an aligned grid puts every froxel centre in its parent cluster");
    }
}

// --- lights ------------------------------------------------------------------------------------------
void testLights() {
    // Per-light terms == the WP-2.1 CPU kernel.
    std::mt19937 rng(11);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    u32 bad = 0;
    for (u32 i = 0; i < 100000u; ++i) {
        const f32 d = u(rng) * 12.f;
        const f32 r = u(rng) * 10.f;
        bad += sameBits(fk::point_falloff(d, r), clustered_kernel::point_light_falloff(d, r)) ? 0u : 1u;
        const f32 ca = u(rng) * 2.f - 1.f;
        const f32 ci = 0.5f + 0.5f * u(rng);
        const f32 co = ci - 0.4f * u(rng);
        bad += sameBits(fk::spot_cone(ca, ci, co), lighting_gpu::spot_cone(ca, ci, co)) ? 0u : 1u;
    }
    expect(bad == 0u, "point falloff / spot cone == the WP-2.1 CPU kernel (bit for bit)");

    // Henyey-Greenstein integrates to 1 over the sphere.
    for (f32 g : {-0.7f, 0.f, 0.4f, 0.9f}) {
        const u32 n = 200000;
        f64 sum = 0.0;
        for (u32 i = 0; i < n; ++i) {
            const f32 mu = -1.f + 2.f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
            sum += fk::phase_hg(g, mu);
        }
        const f64 integral = sum * 2.0 / n * 2.0 * 3.14159265358979;
        std::printf("lights: HG g=%.1f integral %.6f\n", g, integral);
        expect(std::fabs(integral - 1.0) < 2e-3, "Henyey-Greenstein phase is normalised");
    }

    // Cluster lists == every light: 3 cameras (the lists are the oracle's, == WP-2.1's GPU lists).
    for (u32 k = 0; k < 3u; ++k) {
        const ClusterCameraDesc cam = fog_test::camera(k * 20u, 1, 256, 144);
        Frame f;
        FroxelFogSettings s = fog_test::settings();
        s.farPlane = fog_test::kFar;
        makeFrame(s, cam, nullptr, false, k, f);
        std::vector<Vec4> listed;
        injectReference(f.c, f.view, listed);
        // Brute force: every cluster lists every point / spot slot.
        FogLightLists all = f.lists;
        std::vector<u32> slots;
        for (u32 i = 0; i < f.lights.size(); ++i) {
            const u32 t = f.lights[i].type;
            if (t == static_cast<u32>(gpu_scene::GpuLightType::Point) || t == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
                slots.push_back(i);
            }
        }
        for (usize c = 0; c * 2u < all.grid.size(); ++c) {
            all.grid[c * 2u] = 0u;
            all.grid[c * 2u + 1u] = static_cast<u32>(slots.size());
        }
        all.lightList = slots;
        const fk::LightView brute = makeLightView(&f.lighting, f.lights.data(), all);
        std::vector<Vec4> every;
        injectReference(f.c, brute, every);
        FogLightLists directionalOnly{};
        directionalOnly.directional = f.lists.directional;
        const fk::LightView sunOnly = makeLightView(&f.lighting, f.lights.data(), directionalOnly);
        std::vector<Vec4> direct;
        injectReference(f.c, sunOnly, direct);
        u32 differ = 0;
        u32 lit = 0;
        f64 maxRel = 0.0;
        for (usize i = 0; i < listed.size(); ++i) {
            const bool same = sameBits(listed[i].x, every[i].x) && sameBits(listed[i].y, every[i].y) &&
                              sameBits(listed[i].z, every[i].z) && sameBits(listed[i].w, every[i].w);
            differ += same ? 0u : 1u;
            if (!same) {
                maxRel = std::max(maxRel, std::fabs(static_cast<f64>(listed[i].x) - every[i].x) / std::max(1e-9f, std::fabs(every[i].x)));
            }
            lit += listed[i].x != direct[i].x ? 1u : 0u;
        }
        std::printf("lights: camera %u: %zu cluster-list entries, %u / %zu froxels lit by cluster-listed lights; cluster lists vs every "
                    "light: %u differ (max rel %.2e)\n",
                    k, f.lists.lightList.size(), lit, listed.size(), differ, maxRel);
        expect(differ == 0u, "in-scattering through the cluster lists == over every light (bit for bit)");
        expect(lit > listed.size() / 50u, "a meaningful share of froxels is lit by local lights");
    }
}

// --- analytic ----------------------------------------------------------------------------------------
void testAnalytic() {
    const ClusterCameraDesc cam = fog_test::camera(0, 0, 256, 144);
    // Homogeneous medium, ambient only: T(d) = e^{-sigma |ray| d}, L = albedo ambient (1 - T).
    for (f32 sigma : {0.0005f, 0.02f, 0.3f}) {
        FroxelFogSettings s = fog_test::settings();
        s.volumeCount = 0;
        s.medium.density = sigma;
        s.medium.height_falloff = 0.f;
        s.temporal = false;
        FogFrameConstants c{};
        resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c);
        const fk::LightView none{};
        std::vector<Vec4> cur;
        std::vector<Vec4> integ;
        injectReference(c, none, cur);
        integrateReference(c, cur, integ);
        f64 maxT = 0.0;
        f64 maxL = 0.0;
        for (u32 y = 0; y < c.gridY; ++y) {
            for (u32 x = 0; x < c.gridX; ++x) {
                const f64 scale = fk::column_ray_scale(c, x, y);
                for (u32 z = 0; z < c.gridZ; ++z) {
                    const Vec4 v = integ[fk::froxel_index(c, x, y, z)];
                    const f64 T = std::exp(-static_cast<f64>(sigma) * scale * c.sliceDepth[z + 1u]);
                    const f64 L = static_cast<f64>(c.albedo[0]) * c.ambient[0] * (1.0 - T);
                    maxT = std::max(maxT, std::fabs(v.w - T) / T);
                    maxL = std::max(maxL, std::fabs(v.x - L) / std::max(L, 1e-12));
                }
            }
        }
        std::printf("analytic: homogeneous sigma=%.4f: transmittance max rel %.2e, in-scattering max rel %.2e\n", sigma, maxT,
                    maxL);
        expect(maxT < 2e-5, "homogeneous transmittance == e^{-sigma d} (2e-5 relative)");
        expect(maxL < 2e-4, "homogeneous in-scattering == albedo ambient (1 - T) (2e-4 relative)");
    }
    // Height fog: transmittance along each column centre ray vs e^{-(B5 closed-form optical depth)}; midpoint
    // samples (no jitter) of a smooth medium: second-order quadrature error only.
    {
        FroxelFogSettings s = fog_test::settings();
        s.volumeCount = 0;
        s.temporal = false;
        s.medium.density = 0.08f;
        s.medium.height_falloff = 0.3f;
        FogFrameConstants c{};
        resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c);
        const fk::LightView none{};
        std::vector<Vec4> cur;
        std::vector<Vec4> integ;
        injectReference(c, none, cur);
        integrateReference(c, cur, integ);
        f64 maxErr = 0.0;
        for (u32 y = 0; y < c.gridY; ++y) {
            for (u32 x = 0; x < c.gridX; ++x) {
                const f32 sx = (static_cast<f32>(x) + 0.5f) * c.invGridX;
                const f32 sy = (static_cast<f32>(y) + 0.5f) * c.invGridY;
                const Vec3 far = fk::fog_world(c, sx, sy, 1.f);
                const Vec3 origin{c.position[0], c.position[1], c.position[2]};
                const Vec3 dir{far.x - origin.x, far.y - origin.y, far.z - origin.z};
                const f32 len = dir.length();
                for (u32 z = 0; z < c.gridZ; ++z) {
                    const f32 dist = c.sliceDepth[z + 1u] * len;
                    const f64 T = std::exp(-static_cast<f64>(analytic_volumetric_fog_optical_depth(s.medium, origin, dir, dist)));
                    maxErr = std::max(maxErr, std::fabs(integ[fk::froxel_index(c, x, y, z)].w - T));
                }
            }
        }
        std::printf("analytic: height fog transmittance vs B5 closed form: max abs %.2e\n", maxErr);
        expect(maxErr < 2e-3, "height-fog transmittance == e^{-B5 analytic optical depth} (2e-3 abs, midpoint rule)");
    }
    // Apply: at a slice boundary the lookup returns the stored value; sky = the whole range; depth 0 = identity.
    {
        FroxelFogSettings s = fog_test::settings();
        s.temporal = false;
        FogFrameConstants c{};
        resolveFogConstants(s, cam, nullptr, false, 0, 64, 36, c);
        std::vector<Vec4> integ(c.froxelCount);
        for (u32 i = 0; i < c.froxelCount; ++i) {
            integ[i] = Vec4{static_cast<f32>(i % 97u) * 0.01f, 0.5f, 0.25f, 1.f - static_cast<f32>(i % 13u) * 0.05f};
        }
        const u32 x = 5;
        const u32 y = 7;
        const f32 sx = (static_cast<f32>(x) + 0.5f) * c.invGridX;
        const f32 sy = (static_cast<f32>(y) + 0.5f) * c.invGridY;
        bool ok = true;
        for (u32 z = 0; z < c.gridZ; ++z) {
            const Vec4 v = fk::sample_integrated(c, integ.data(), sx, sy, c.sliceDepth[z + 1u]);
            const Vec4 ref = integ[fk::froxel_index(c, x, y, z)];
            ok = ok && std::fabs(v.x - ref.x) <= 1e-6f && std::fabs(v.w - ref.w) <= 1e-6f;
        }
        const Vec4 zero = fk::sample_integrated(c, integ.data(), sx, sy, 0.f);
        const Vec4 sky = fk::sample_integrated(c, integ.data(), sx, sy, 1e9f);
        const Vec4 last = integ[fk::froxel_index(c, x, y, c.gridZ - 1u)];
        expect(ok, "apply lookup at slice boundaries == the stored boundary values");
        expect(zero.x == 0.f && zero.w == 1.f, "apply lookup at depth 0 == no fog");
        expect(sky.x == last.x && sky.w == last.w, "apply lookup beyond the range == the whole range");
    }
}

// --- temporal ----------------------------------------------------------------------------------------
struct Pipeline {
    std::vector<Vec4> history[2];
    u32 parity = 0;
};

/// One CPU frame: inject -> temporal -> integrate; returns this frame's history.
const std::vector<Vec4>& runFrame(Pipeline& p, const Frame& f) {
    std::vector<Vec4> cur;
    injectReference(f.c, f.view, cur);
    std::vector<Vec4>& prev = p.history[p.parity];
    std::vector<Vec4>& next = p.history[p.parity ^ 1u];
    if (prev.size() != cur.size()) {
        prev.assign(cur.size(), Vec4{});
    }
    temporalReference(f.c, cur, prev, next);
    p.parity ^= 1u;
    return next;
}

void testTemporal() {
    constexpr u32 kW = 256;
    constexpr u32 kH = 144;
    const FroxelFogSettings s = fog_test::settings();
    // Static camera: the history is a linear filter (weight 1 - alpha) of a periodic input (the jitter period),
    // so it converges geometrically to a periodic steady state: the distance between frames one period apart
    // falls as (1 - alpha)^16 per period, the period mean equals the jitter-period mean of the samples, and the
    // remaining fluctuation inside a period (the temporal variance) is bounded by alpha x the in-froxel spread.
    {
        Pipeline p;
        const ClusterCameraDesc cam = fog_test::camera(0, 0, kW, kH);
        fog_test::VarianceWindow window;
        fog_test::VarianceWindow visible;
        std::vector<std::vector<Vec4>> ring(kFogJitterPeriod);
        std::vector<f64> phase;
        constexpr u32 kFrames = 256;
        for (u32 frame = 0; frame < kFrames; ++frame) {
            Frame f;
            makeFrame(s, cam, &cam, frame > 0u, frame, f);
            const std::vector<Vec4>& h = runFrame(p, f);
            std::vector<Vec4>& slot = ring[frame % kFogJitterPeriod];
            if (frame >= kFogJitterPeriod) {
                phase.push_back(fog_test::relL1(h, slot));
            }
            slot = h;
            if (frame >= kFrames - kFogJitterPeriod) {
                window.add(h);
                std::vector<Vec4> integ;
                integrateReference(f.c, h, integ);
                visible.add(integ);
            }
        }
        Frame f;
        makeFrame(s, cam, nullptr, false, 0, f);
        std::vector<Vec4> periodMean;
        jitterPeriodMeanReference(f.c, f.view, periodMean);
        std::vector<Vec4> truth;
        froxelAverageReference(f.c, f.view, 4, truth);
        const f64 toPeriod = fog_test::relL1(window.mean(), periodMean);
        const f64 toTruth = fog_test::relL1(window.mean(), truth);
        const f64 stdev = window.relativeStd();
        const f64 visibleStd = visible.relativeStd();
        std::printf("temporal static (CPU): distance to the frame one jitter period earlier %.2e (frame 16) -> %.2e "
                    "(frame 64) -> %.2e (frame %u); relative std over the last period: froxels %.2e, integrated "
                    "in-scattering %.2e; period mean vs jitter-period mean %.2e, vs 4^3-sample froxel mean %.2e\n",
                    phase[0], phase[48], phase.back(), kFrames - 1u, stdev, visibleStd, toPeriod, toTruth);
        expect(phase.back() < 1e-4 * phase[0], "static camera: converges (period-to-period distance falls by 1e4)");
        expect(visibleStd < 1e-2, "static camera: temporal variance of the integrated fog below 1e-2 (relative std)");
        expect(stdev < 4e-2, "static camera: temporal variance of the froxels below 4e-2 (relative std)");
        expect(toPeriod < 1e-4, "static camera: history converges to the jitter-period mean (1e-4)");
        expect(toTruth < 2e-2, "static camera: converged fog within 2% of the froxel mean (jitter bias)");
    }
    // Moving camera: reprojected history vs the same-froxel control, against each frame's froxel mean.
    {
        f64 metric[2] = {0.0, 0.0};
        f64 trail[2] = {0.0, 0.0};
        f64 floor = 0.0;
        u32 samples = 0;
        for (u32 mode = 0; mode < 2u; ++mode) {
            FroxelFogSettings ms = s;
            ms.reproject = mode == 0u;
            Pipeline p;
            ClusterCameraDesc prev{};
            constexpr u32 kFrames = 96;
            samples = 0;
            for (u32 frame = 0; frame < kFrames; ++frame) {
                const ClusterCameraDesc cam = fog_test::camera(frame, 1, kW, kH);
                Frame f;
                makeFrame(ms, cam, &prev, frame > 0u, frame, f);
                const std::vector<Vec4>& h = runFrame(p, f);
                prev = cam;
                if (frame >= 48u && frame % 8u == 0u) {
                    std::vector<Vec4> truth;
                    froxelAverageReference(f.c, f.view, 2, truth);
                    metric[mode] += fog_test::relL1(h, truth);
                    trail[mode] += fog_test::trailEnergy(h, truth);
                    if (mode == 0u) {
                        std::vector<Vec4> periodMean;
                        jitterPeriodMeanReference(f.c, f.view, periodMean);
                        floor += fog_test::relL1(periodMean, truth);
                    }
                    ++samples;
                }
            }
        }
        for (u32 m = 0; m < 2u; ++m) {
            metric[m] /= samples;
            trail[m] /= samples;
        }
        floor /= samples;
        std::printf("temporal moving (CPU, %u samples): ghosting L1 vs the froxel mean %.4f (reprojected) / %.4f "
                    "(same-froxel history); trail energy %.4f / %.4f; converged-static floor %.4f\n",
                    samples, metric[0], metric[1], trail[0], trail[1], floor);
        expect(metric[0] < 0.08, "moving camera: ghosting metric (relative L1 to the froxel mean) below 0.08");
        expect(trail[0] < 0.04, "moving camera: trail energy below 0.04");
        expect(metric[0] < 0.5 * metric[1], "reprojection at least halves the ghosting of a same-froxel history");
    }
}

// --- api ---------------------------------------------------------------------------------------------
void testApi() {
    const ClusterCameraDesc cam = fog_test::camera(0, 0, 256, 144);
    FroxelFogSettings s = fog_test::settings();
    FogFrameConstants c{};
    expect(resolveFogConstants(s, cam, nullptr, false, 3, 64, 36, c), "resolve");
    expect(c.froxelCount == 32u * 18u * 48u && c.farPlane == 48.f && c.nearPlane == fog_test::kNear, "grid / range");
    expect((c.flags & kFogFlagHistory) == 0u, "no history without a previous camera");
    expect(c.sliceDepth[0] == c.nearPlane && c.sliceDepth[c.gridZ] == c.farPlane, "slice table spans the range");
    bool increasing = true;
    for (u32 z = 0; z < c.gridZ; ++z) {
        increasing = increasing && c.sliceDepth[z] < c.sliceDepth[z + 1u];
    }
    expect(increasing, "slice table increases");
    FogFrameConstants h{};
    expect(resolveFogConstants(s, cam, &cam, true, 3, 64, 36, h) && (h.flags & kFogFlagHistory) != 0u &&
               (h.flags & kFogFlagReproject) != 0u,
           "history + reprojection with a previous camera");
    s.reproject = false;
    expect(resolveFogConstants(s, cam, &cam, true, 3, 64, 36, h) && (h.flags & kFogFlagReproject) == 0u,
           "reprojection off");
    FroxelFogSettings e = fog_test::settings();
    e.gridZ = 0;
    expect(!resolveFogConstants(e, cam, nullptr, false, 0, 64, 36, h), "empty grid rejected");
    ClusterCameraDesc bad = cam;
    bad.nearPlane = 0.f;
    expect(!resolveFogConstants(fog_test::settings(), bad, nullptr, false, 0, 64, 36, h), "invalid camera rejected");
    FroxelFogSettings big = fog_test::settings();
    big.gridX = 5000;
    big.gridY = 5000;
    big.gridZ = 5000;
    expect(resolveFogConstants(big, cam, nullptr, false, 0, 64, 36, h) && h.gridX == kFogMaxGridX &&
               h.gridY == kFogMaxGridY && h.gridZ == kFogMaxSlices,
           "grid clamped to the limits");
    FroxelFogSettings clampAniso = fog_test::settings();
    clampAniso.medium.anisotropy = 3.f;
    clampAniso.medium.density = -1.f;
    expect(resolveFogConstants(clampAniso, cam, nullptr, false, 0, 64, 36, h) && h.anisotropy == fk::kMaxAnisotropy &&
               h.density == 0.f,
           "anisotropy / density clamped");
    // Jitter: Halton, period 16, inside [0, 1); disabled -> centre.
    bool inside = true;
    f32 j0[3];
    f32 j16[3];
    fogJitter(0, true, j0);
    fogJitter(16, true, j16);
    for (u32 f = 0; f < 64u; ++f) {
        f32 j[3];
        fogJitter(f, true, j);
        for (f32 v : j) {
            inside = inside && v >= 0.f && v < 1.f;
        }
    }
    f32 jc[3];
    fogJitter(5, false, jc);
    expect(inside && j0[0] == 0.5f && j0[1] == halton(1, 3) && j16[2] == j0[2], "jitter: Halton 2 / 3 / 5, period 16");
    expect(jc[0] == 0.5f && jc[1] == 0.5f && jc[2] == 0.5f, "jitter off: froxel centre");
    expect(!queryFogCapabilities(nullptr).fog, "no device: not capable");
    FroxelFog fog;
    expect(!fog.init(FroxelFogDesc{}) && !fog.valid(), "init without a device fails cleanly");
    expect(!fog.beginFrame(1, fog_test::settings(), FogFrameDesc{}), "beginFrame before init fails");
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
    if (all || suite == "lights") {
        testLights();
    }
    if (all || suite == "analytic") {
        testAnalytic();
    }
    if (all || suite == "temporal") {
        testTemporal();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
