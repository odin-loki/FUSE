// Screen-space radiance cascades (the WP-6.5 follow-up): CPU gates (no device; also run in the stub tree).
//
//   layout   SsrcFrameConstants / SsrcPush sizes; the GLSL and Slang mirrors (struct SrcFrame, the push block) have
//            the same fields, order and offsets; the cascade layout (2x spacing, 4x directions, 4x interval length,
//            t_n >= the screen diagonal, constant records per cascade); the direction table (unit directions,
//            solid angles summing to 4 pi per cascade, octahedral children); work-buffer sections; memory at 1080p.
//   f16      the records' software f32 -> f16 (round to nearest even) == GBufferQuantize::floatToHalf over a sweep
//            of f32 bit patterns; decode(encode(h)) == h for every finite half.
//   furnace  white furnace (every surface and the far field radiate L): indirect == diffuse x L on every geometry
//            pixel (bilinear fix and vanilla, s0 = 1 and 2); a lone plane: AO ~ 1, no self-occlusion.
//   error    the cascades vs the brute-force screen-space ray marcher on the same depth-buffer model (the room
//            scene of WP-6.3, 2 camera positions, sky off / on): relative L1 of the indirect diffuse and mean |AO|
//            error within the documented bounds; the bilinear fix beats vanilla.
//   leak     a sealed box (walls within the slab thickness) with a bright emitter strip outside: the indirect light
//            inside stays within the documented bound (bilinear fix, == brute force); vanilla leaks; with a slab
//            thinner than the walls' depth extent both the brute force and the cascades leak (the screen-space
//            limit, reported).
//   ssgi     the SSFX room scene: SSRC and WP-6.3 SSGI (16 / 64 / 256 rays) against a world-space ray-traced
//            one-bounce reference (analytic room geometry, off-screen included) and the brute-force screen-space
//            reference; error and cost in depth samples per pixel.
//   api      settings validation / clamps, camera + rotations, stub / no-device behaviour.
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/renderer/ssrc/ssrc_gpu.hpp>
#include <fuse/renderer/ssrc/ssrc_reference.hpp>
#include <fuse/ssfx/ssgi.hpp>
#include <fuse/ssfx/ssgi_kernel.hpp>
#include <fuse/ssfx/ssr_kernel.hpp>

#include "test_rp_ssfx_gpu_scene.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::ssrc;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace ssfx = fuse::ssfx;
namespace ssfx_gpu = fuse::renderer::ssfx_gpu;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

f64 seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - since).count();
}

// --- layout ----------------------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define SRC_FIELD(n) Field{#n, offsetof(SsrcFrameConstants, n)}
const Field kFields[] = {
    SRC_FIELD(geo), SRC_FIELD(lit), SRC_FIELD(diffuse), SRC_FIELD(albedo), SRC_FIELD(indirect), SRC_FIELD(records0),
    SRC_FIELD(records1), SRC_FIELD(dirs), SRC_FIELD(ddgiVolume), SRC_FIELD(dump), SRC_FIELD(reserved0),
    SRC_FIELD(reserved1), SRC_FIELD(width), SRC_FIELD(height), SRC_FIELD(inputDepth), SRC_FIELD(inputNormal),
    SRC_FIELD(inputAlbedo), SRC_FIELD(inputRoughMetal), SRC_FIELD(inputLit),
    Field{"output_", offsetof(SsrcFrameConstants, output)}, SRC_FIELD(flags), SRC_FIELD(cascades),
    SRC_FIELD(aoCascades), SRC_FIELD(reserved2), SRC_FIELD(fx), SRC_FIELD(fy), SRC_FIELD(cx), SRC_FIELD(cy),
    SRC_FIELD(nearZ), SRC_FIELD(nearPlane), SRC_FIELD(farPlane), SRC_FIELD(reserved3), SRC_FIELD(viewRot),
    SRC_FIELD(viewToWorld), SRC_FIELD(cameraWorld), SRC_FIELD(ambient), SRC_FIELD(sky), SRC_FIELD(stride),
    SRC_FIELD(thickness), SRC_FIELD(thicknessSlope), SRC_FIELD(maxDistance), SRC_FIELD(originBias),
    SRC_FIELD(intensity), SRC_FIELD(ddgiScale), SRC_FIELD(planeTolerance), SRC_FIELD(probesX), SRC_FIELD(probesY),
    SRC_FIELD(dirRes), SRC_FIELD(dirOffset), SRC_FIELD(spacing), SRC_FIELD(tStart), SRC_FIELD(tEnd),
    SRC_FIELD(reserved5),
};
#undef SRC_FIELD
#define PUSH_FIELD(n) Field{#n, offsetof(SsrcPush, n)}
const Field kPushFields[] = {PUSH_FIELD(frame),   PUSH_FIELD(upper), PUSH_FIELD(dst),     PUSH_FIELD(cascade),
                             PUSH_FIELD(count),   PUSH_FIELD(groupsX), PUSH_FIELD(reserved)};
#undef PUSH_FIELD

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// Parses the block opened by `opener` up to "}": (name, std430 / scalar offset) per field.
bool parseBlock(const std::string& text, const char* opener, std::vector<size_t>& offsets, size_t& size,
                std::vector<std::string>& names) {
    const size_t begin = text.find(opener);
    if (begin == std::string::npos) {
        return false;
    }
    const size_t end = text.find('}', begin);
    const size_t start = begin + std::strlen(opener);
    std::istringstream body(text.substr(start, end - start));
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
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = offset;
    return true;
}

template <usize N>
void checkBlock(const std::string& text, const char* opener, const Field (&fields)[N], size_t bytes, const char* what) {
    std::vector<size_t> offsets;
    std::vector<std::string> names;
    size_t size = 0;
    const bool parsed = parseBlock(text, opener, offsets, size, names);
    expect(parsed, "shader block parsed");
    if (!parsed) {
        return;
    }
    bool same = offsets.size() == N && size == bytes;
    for (size_t i = 0; same && i < N; ++i) {
        same = names[i] == fields[i].name && offsets[i] == fields[i].offset;
        if (!same) {
            std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", what, i, names[i].c_str(), offsets[i],
                         fields[i].name, fields[i].offset);
        }
    }
    std::printf("layout: %s: %zu fields, %zu bytes\n", what, offsets.size(), size);
    expect(same, "shader block == C++ record (names, order, offsets, size)");
}

void testLayout() {
    expect(sizeof(SsrcFrameConstants) == 608u, "SsrcFrameConstants is 608 bytes");
    expect(sizeof(SsrcPush) == 40u, "SsrcPush is 40 bytes (<= 128 guaranteed push-constant bytes)");
    const std::string dir = FUSE_RP_SSRC_SHADER_DIR;
    const std::string glsl = readFile(dir + "/src_common.glsl");
    const std::string slang = readFile(dir + "/src_common.slang");
    checkBlock(glsl, "struct SrcFrame {", kFields, sizeof(SsrcFrameConstants), "src_common.glsl SrcFrame");
    checkBlock(slang, "struct SrcFrame {", kFields, sizeof(SsrcFrameConstants), "src_common.slang SrcFrame");
    checkBlock(glsl, "uniform SrcPush {", kPushFields, sizeof(SsrcPush), "src_common.glsl SrcPush");
    checkBlock(slang, "struct SrcPush {", kPushFields, sizeof(SsrcPush), "src_common.slang SrcPush");

    struct Case {
        u32 w, h, s0, r0;
    };
    for (const Case& k : {Case{97, 71, 1, 4}, Case{128, 96, 1, 4}, Case{128, 96, 2, 4}, Case{1920, 1080, 1, 4},
                          Case{1920, 1080, 2, 4}, Case{3840, 2160, 2, 4}}) {
        SsrcSettings s{};
        s.probeSpacing0 = k.s0;
        s.dirRes0 = k.r0;
        SsrcFrameConstants c{};
        SsrcLayoutInfo info{};
        const bool ok = resolve_layout(s, k.w, k.h, c, &info);
        expect(ok && c.cascades >= 1u, "layout resolves");
        if (!ok) {
            continue;
        }
        const f64 diag = std::sqrt(static_cast<f64>(k.w) * k.w + static_cast<f64>(k.h) * k.h);
        bool shape = c.tStart[0] == 0.f && c.tEnd[c.cascades - 1u] >= diag;
        for (u32 i = 0; i < c.cascades; ++i) {
            shape = shape && c.spacing[i] == static_cast<f32>(k.s0 << i) && c.dirRes[i] == (k.r0 << i) &&
                    c.probesX[i] == (k.w + (k.s0 << i) - 1u) / (k.s0 << i);
            shape = shape && std::fabs((c.tEnd[i] - c.tStart[i]) - 2.f * k.s0 * std::pow(4.f, static_cast<f32>(i))) < 1e-3f;
            if (i > 0u) {
                shape = shape && c.tStart[i] == c.tEnd[i - 1u];
                // Records stay within the rounding of the probe grid (ceil) of cascade 0's.
                shape = shape && info.records[i] <= info.records[0] * 2u;
            }
        }
        expect(shape, "cascade layout: 2x spacing, 2x octahedral resolution (4x directions), 4x interval, reach");
        const SsrcBufferLayout b = SsrcBufferLayout::compute(info, k.w, k.h);
        const u64 n = static_cast<u64>(k.w) * k.h;
        const u64 sections[][2] = {{b.geo, n * 16}, {b.lit, n * 16}, {b.diffuse, n * 16}, {b.albedo, n * 16},
                                   {b.indirect, n * 16}, {b.records[0], info.maxRecords * 8}, {b.records[1], info.maxRecords * 8}};
        bool disjoint = true;
        for (usize i = 0; i < 7u; ++i) {
            disjoint = disjoint && sections[i][0] % 256u == 0u && sections[i][0] + sections[i][1] <= b.workBytes;
            for (usize j = i + 1u; j < 7u; ++j) {
                disjoint = disjoint && (sections[i][0] + sections[i][1] <= sections[j][0] ||
                                        sections[j][0] + sections[j][1] <= sections[i][0]);
            }
        }
        expect(disjoint, "work sections 256-aligned, disjoint, inside the buffer");
        std::printf("layout: %4ux%-4u s0 %u N0 %2u: %u cascades (t_n %.0f px >= diag %.0f), records/cascade %llu (total "
                    "%llu), work buffer %.1f MiB (%.1f B / pixel)\n",
                    k.w, k.h, k.s0, k.r0 * k.r0, c.cascades, static_cast<f64>(c.tEnd[c.cascades - 1u]), diag,
                    static_cast<unsigned long long>(info.records[0]), static_cast<unsigned long long>(info.totalRecords),
                    static_cast<f64>(b.workBytes) / (1024.0 * 1024.0), static_cast<f64>(b.workBytes) / static_cast<f64>(n));
    }
    // Direction table.
    SsrcSettings s{};
    SsrcFrameConstants c{};
    expect(resolve_layout(s, 256, 256, c), "256^2 layout");
    std::vector<f32> dirs;
    buildDirectionTable(c, dirs);
    bool unit = true;
    bool sums = true;
    f64 minOmega = 1e30;
    f64 maxOmega = 0.0;
    for (u32 i = 0; i < c.cascades; ++i) {
        f64 sum = 0.0;
        const u32 count = c.dirRes[i] * c.dirRes[i];
        for (u32 k = 0; k < count; ++k) {
            const f32* d = dirs.data() + (static_cast<usize>(c.dirOffset[i]) + k) * 4u;
            unit = unit && std::fabs(std::sqrt(static_cast<f64>(d[0]) * d[0] + static_cast<f64>(d[1]) * d[1] +
                                               static_cast<f64>(d[2]) * d[2]) - 1.0) < 1e-6;
            sum += d[3];
            if (i == 0u) {
                minOmega = std::min(minOmega, static_cast<f64>(d[3]));
                maxOmega = std::max(maxOmega, static_cast<f64>(d[3]));
            }
        }
        sums = sums && std::fabs(sum - 4.0 * std::numbers::pi) < 1e-4;
    }
    // Children: the solid angles of the 4 children of a texel sum to the parent's (the octahedral quadtree).
    bool children = true;
    for (u32 i = 0; i + 1u < c.cascades; ++i) {
        const u32 res = c.dirRes[i];
        const u32 res2 = c.dirRes[i + 1u];
        for (u32 k = 0; k < res * res; ++k) {
            f64 sum = 0.0;
            const u32 ku = k % res;
            const u32 kv = k / res;
            for (u32 j = 0; j < 4u; ++j) {
                const u32 ck = (2u * kv + (j >> 1u)) * res2 + 2u * ku + (j & 1u);
                sum += dirs[(static_cast<usize>(c.dirOffset[i + 1u]) + ck) * 4u + 3u];
            }
            const f64 parent = dirs[(static_cast<usize>(c.dirOffset[i]) + k) * 4u + 3u];
            children = children && std::fabs(sum - parent) <= 2e-3 * parent;
        }
    }
    std::printf("layout: direction table: %u cascades, unit directions %s, solid angles sum to 4 pi %s, cascade 0 "
                "texel solid angle %.4f .. %.4f sr, children sum to the parent %s\n",
                c.cascades, unit ? "yes" : "NO", sums ? "yes" : "NO", minOmega, maxOmega, children ? "yes" : "NO");
    expect(unit && sums && children, "direction table (unit, 4 pi, octahedral quadtree)");
}

// --- f16 -------------------------------------------------------------------------------------------------------
void testF16() {
    u64 checked = 0;
    u64 mismatches = 0;
    for (u64 bits = 0; bits <= 0xFFFFFFFFull; bits += 97u) {
        const f32 v = std::bit_cast<f32>(static_cast<u32>(bits));
        if (std::isnan(v) || std::fabs(v) >= 65504.f) {
            continue;
        }
        ++checked;
        if (f32ToF16(v) != fuse::renderer::GBufferQuantize::floatToHalf(v)) {
            ++mismatches;
        }
    }
    u32 roundTrip = 0;
    for (u32 h = 0; h < 0x10000u; ++h) {
        if (((h >> 10) & 0x1Fu) == 0x1Fu) {
            continue; // inf / NaN
        }
        const f32 f = f16ToF32(h);
        roundTrip += (f32ToF16(f) == h && f == fuse::renderer::GBufferQuantize::halfToFloat(static_cast<u16>(h))) ? 0u : 1u;
    }
    const bool clamp = f32ToF16(1e9f) == 0x7BFFu && f32ToF16(-1e9f) == 0xFBFFu;
    std::printf("f16: %llu f32 patterns vs GBufferQuantize::floatToHalf: %llu mismatches; %u half round-trip failures; "
                "clamp to +-65504 %s\n",
                static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatches), roundTrip,
                clamp ? "yes" : "NO");
    expect(mismatches == 0u && roundTrip == 0u && clamp, "software f16 == round to nearest even, exact decode");
}

// --- scenes ----------------------------------------------------------------------------------------------------
struct Frame {
    ssfx_test::Camera camera;
    f32 ambient[3] = {0.f, 0.f, 0.f};
    ssfx_test::GBuffer g;
    ssfx_gpu::SsfxPreparedFrame prepared;
    SsrcInputs in;
    ssfx_gpu::SsfxFrameConstants sc{};
};

/// The ssrc.prepare oracle (WP-6.3 prepare_pixel) over a ray-cast G-buffer.
void prepare(Frame& f) {
    ssfx_gpu::SsfxGpuSettings s{};
    expect(ssfx_gpu::resolve_constants(s, ssfx_test::cameraDesc(f.camera), f.ambient, f.g.width, f.g.height, f.sc),
           "ssfx constants");
    ssfx_test::prepareCpu(f.sc, f.g, f.prepared);
    inputsFromPrepared(f.prepared, f.in);
}

void roomFrame(Frame& f, u32 w, u32 h, u32 cameraFrame) {
    ssfx_test::RoomScene room;
    ssfx_test::buildRoom(room, w, h, cameraFrame);
    f.camera = room.camera;
    f.ambient[0] = room.ambient[0];
    f.ambient[1] = room.ambient[1];
    f.ambient[2] = room.ambient[2];
    f.g = room.g;
    prepare(f);
}

bool constantsFor(const Frame& f, const SsrcSettings& s, SsrcFrameConstants& c, SsrcLayoutInfo* info = nullptr) {
    return resolve_constants(s, ssfx_test::cameraDesc(f.camera), f.ambient, f.g.width, f.g.height, c, info);
}

f64 lum(const std::vector<f32>& img, usize i) {
    return (static_cast<f64>(img[i * 4u]) + img[i * 4u + 1u] + img[i * 4u + 2u]) / 3.0;
}

f64 maskedLumAll(const std::vector<f32>& img, const std::vector<u8>& mask) {
    f64 sum = 0.0;
    u32 n = 0;
    for (usize i = 0; i < mask.size(); ++i) {
        if (mask[i] != 0u) {
            sum += lum(img, i);
            ++n;
        }
    }
    return n != 0u ? sum / n : 0.0;
}

// --- furnace -------------------------------------------------------------------------------------------------------
/// A lone plane (the floor of an empty world) lit with `radiance`.
void planeFrame(Frame& p, f32 radiance) {
    p.camera.width = 96;
    p.camera.height = 64;
    p.camera.fovYDeg = 60.f;
    p.camera.nearPlane = 0.1f;
    p.camera.farPlane = 100.f;
    p.camera.eye = Vec3{0.f, 2.f, 4.f};
    p.camera.target = Vec3{0.f, 0.f, -2.f};
    p.camera.build();
    p.g.resize(96, 64);
    for (u32 y = 0; y < 64u; ++y) {
        for (u32 x = 0; x < 96u; ++x) {
            Vec3 viewDir{};
            const Vec3 d = p.camera.rayWorld(x, y, viewDir);
            ssfx_test::Hit hit{};
            ssfx_test::hitPlane(p.camera.eye, d, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, 0.f, 0.f}, 0u, hit);
            if (hit.t < 60.f) {
                ssfx_test::writePixel(p.g, p.camera, static_cast<usize>(y) * 96u + x, hit, viewDir, ssfx_test::Material{},
                                      Vec3{radiance, radiance, radiance});
            }
        }
    }
    prepare(p);
}

void testFurnace() {
    // White furnace: the plane and the far field radiate L, so every ray (hit or escaped) returns L and the gathered
    // E / pi must be exactly L: exercises the merge weights, the child averages and the gather normalisation.
    const f32 L = 0.75f; // exact in f16
    Frame p;
    planeFrame(p, L);
    const std::vector<u8> mask = geometryMask(p.in);
    struct Cfg {
        const char* name;
        bool fix;
        u32 s0;
        u32 r0;
    };
    for (const Cfg& cfg : {Cfg{"bilinear fix, s0 1, N0 16", true, 1u, 4u}, Cfg{"vanilla, s0 1, N0 16", false, 1u, 4u},
                           Cfg{"bilinear fix, s0 2, N0 16", true, 2u, 4u}, Cfg{"bilinear fix, s0 1, N0 64", true, 1u, 8u}}) {
        SsrcSettings s{};
        s.bilinearFix = cfg.fix;
        s.probeSpacing0 = cfg.s0;
        s.dirRes0 = cfg.r0;
        s.sky[0] = s.sky[1] = s.sky[2] = L;
        SsrcFrameConstants c{};
        expect(constantsFor(p, s, c), "constants");
        SsrcCpuSolver solver;
        std::vector<f32> ind;
        std::vector<f32> img;
        expect(solver.solve(c, p.in, nullptr, ind, img), "solve");
        f64 maxRel = 0.0;
        u32 pixels = 0;
        for (usize i = 0; i < mask.size(); ++i) {
            if (mask[i] == 0u) {
                continue;
            }
            ++pixels;
            for (u32 ch = 0; ch < 3u; ++ch) {
                const f64 expected = static_cast<f64>(p.in.diffuse[i * 4u + ch]) * L;
                maxRel = std::max(maxRel, std::fabs(ind[i * 4u + ch] - expected) / std::max(1e-6, expected));
            }
        }
        std::printf("furnace: %s: %u geometry pixels, max |indirect - diffuse x L| / (diffuse x L) = %.3g\n", cfg.name,
                    pixels, maxRel);
        expect(pixels > 1000u && maxRel <= 1e-5, "white furnace: indirect == diffuse x L");
    }
    // The lone plane, unlit, AO over every cascade: nothing occludes the upper hemisphere and nothing is received.
    Frame q;
    planeFrame(q, 1.f);
    SsrcSettings s{};
    s.aoCascades = 0u;
    SsrcFrameConstants c{};
    expect(constantsFor(q, s, c), "plane constants");
    SsrcCpuSolver solver;
    std::vector<f32> ind;
    std::vector<f32> img;
    expect(solver.solve(c, q.in, nullptr, ind, img), "plane solve");
    f64 aoSum = 0.0;
    f64 aoMin = 1.0;
    f64 eMax = 0.0;
    u32 n = 0;
    for (usize i = 0; i < mask.size(); ++i) {
        if (mask[i] != 0u) {
            aoSum += ind[i * 4u + 3u];
            aoMin = std::min(aoMin, static_cast<f64>(ind[i * 4u + 3u]));
            eMax = std::max(eMax, lum(ind, i));
            ++n;
        }
    }
    std::printf("furnace: lone plane lit 1 (%u px, AO over every cascade): mean AO %.4f, min %.4f, mean indirect %.4g, "
                "max %.4g\n",
                n, aoSum / n, aoMin, maskedLumAll(ind, mask), eMax);
    expect(n > 1000u && aoMin == 1.0 && eMax == 0.0, "a lone plane is unoccluded and receives nothing from itself");
}

// --- error vs brute force ------------------------------------------------------------------------------------------
void testError() {
    struct Run {
        u32 frame;
        bool sky;
    };
    for (const Run& run : {Run{0u, false}, Run{2u, false}, Run{0u, true}}) {
        Frame f;
        roomFrame(f, 128, 96, run.frame);
        const std::vector<u8> mask = geometryMask(f.in);
        SsrcSettings s{};
        if (run.sky) {
            s.sky[0] = 0.3f;
            s.sky[1] = 0.4f;
            s.sky[2] = 0.6f;
        }
        SsrcFrameConstants c{};
        SsrcLayoutInfo info{};
        expect(constantsFor(f, s, c, &info), "constants");
        auto t0 = std::chrono::steady_clock::now();
        std::vector<f32> bf;
        SsrcStats bfStats{};
        bruteForce(c, f.in, nullptr, 24u, bf, &bfStats);
        const f64 bfTime = seconds(t0);
        SsrcCpuSolver solver;
        std::vector<f32> fix;
        std::vector<f32> img;
        SsrcStats st{};
        t0 = std::chrono::steady_clock::now();
        expect(solver.solve(c, f.in, nullptr, fix, img, &st), "solve");
        const f64 rcTime = seconds(t0);
        const SsrcErrorMetrics mf = compareIndirect(fix, bf, mask);
        SsrcSettings sv = s;
        sv.bilinearFix = false;
        SsrcFrameConstants cv{};
        expect(constantsFor(f, sv, cv), "vanilla constants");
        std::vector<f32> van;
        SsrcStats stv{};
        expect(solver.solve(cv, f.in, nullptr, van, img, &stv), "vanilla solve");
        const SsrcErrorMetrics mv = compareIndirect(van, bf, mask);
        const f64 px = static_cast<f64>(mf.pixels);
        const f64 all = static_cast<f64>(f.in.width) * f.in.height;
        std::printf("error: room 128x96 camera %u, sky %s: %u cascades; brute force 576 rays/px (%.1f s, %.0f samples/px); "
                    "mean ref %.4f\n",
                    run.frame, run.sky ? "on" : "off", c.cascades, bfTime, static_cast<f64>(bfStats.samples) / px, mf.meanRef);
        std::printf("       bilinear fix: relL1 %.4f relRMSE %.4f, mean %.4f (bias %+.1f%%), mean |AO err| %.4f, %.0f samples/px "
                    "(%.2f s)\n",
                    mf.relL1, mf.relRmse, mf.meanTest, 100.0 * (mf.meanTest / mf.meanRef - 1.0), mf.aoMeanAbs,
                    static_cast<f64>(st.samples) / all, rcTime);
        std::printf("       vanilla     : relL1 %.4f relRMSE %.4f, mean |AO err| %.4f, %.0f samples/px\n", mv.relL1, mv.relRmse,
                    mv.aoMeanAbs, static_cast<f64>(stv.samples) / all);
        for (u32 i = 0; i < c.cascades; ++i) {
            std::printf("       cascade %u: spacing %.0f px, %u dirs, interval [%.0f, %.0f) px, %.1f samples/px\n", i,
                        static_cast<f64>(c.spacing[i]), c.dirRes[i] * c.dirRes[i], static_cast<f64>(c.tStart[i]),
                        static_cast<f64>(c.tEnd[i]), static_cast<f64>(st.samplesPerCascade[i]) / all);
        }
        expect(mf.relL1 <= (run.sky ? 0.03 : 0.10), "cascades vs brute force: relative L1 <= 0.10 (sky off) / 0.03 (sky on)");
        expect(mf.aoMeanAbs <= 0.03, "cascades vs brute force: mean |AO error| <= 0.03");
        expect(std::fabs(mf.meanTest - mf.meanRef) <= 0.05 * mf.meanRef, "cascades vs brute force: mean bias <= 5%");
    }
}

// --- leak ------------------------------------------------------------------------------------------------------------
struct LeakScene {
    Frame f;
    std::vector<u8> inside;
    std::vector<u8> outside;
};

void buildLeak(LeakScene& s, u32 w, u32 h) {
    Frame& f = s.f;
    f.camera.width = w;
    f.camera.height = h;
    f.camera.fovYDeg = 55.f;
    f.camera.nearPlane = 0.1f;
    f.camera.farPlane = 100.f;
    f.camera.eye = Vec3{-0.4f, 7.f, 1.5f};
    f.camera.target = Vec3{-0.6f, 0.f, 0.f};
    f.camera.build();
    f.g.resize(w, h);
    s.inside.assign(static_cast<usize>(w) * h, 0u);
    s.outside.assign(static_cast<usize>(w) * h, 0u);
    const ssfx_test::Material floorMat{Vec3{0.7f, 0.7f, 0.7f}, 0.8f, 0.f, 1.f};
    const ssfx_test::Material wallMat{Vec3{0.6f, 0.6f, 0.6f}, 0.8f, 0.f, 1.f};
    const ssfx_test::Material emitMat{Vec3{0.9f, 0.9f, 0.9f}, 0.8f, 0.f, 1.f};
    // Emissive ramp outside the left wall: rises from y = 0 at x = -1.8 to y = 0.35 at x = -2.2, facing the box and
    // the camera (normal ~ (0.66, 0.75, 0)); lower than the wall, so no straight ray from the box floor reaches it.
    const Vec3 rampN = Vec3{0.35f, 0.4f, 0.f}.normalized();
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const usize i = static_cast<usize>(y) * w + x;
            Vec3 viewDir{};
            const Vec3 d = f.camera.rayWorld(x, y, viewDir);
            const Vec3 o = f.camera.eye;
            ssfx_test::Hit hit{};
            ssfx_test::hitPlane(o, d, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, 0.f, 0.f}, 0u, hit);
            // Sealed box: walls 0.12 thick, 0.5 high around [-1.2, 1.2]^2.
            ssfx_test::hitBox(o, d, Vec3{-1.2f, 0.f, -1.2f}, Vec3{-1.08f, 0.5f, 1.2f}, 1u, hit);
            ssfx_test::hitBox(o, d, Vec3{1.08f, 0.f, -1.2f}, Vec3{1.2f, 0.5f, 1.2f}, 1u, hit);
            ssfx_test::hitBox(o, d, Vec3{-1.2f, 0.f, -1.2f}, Vec3{1.2f, 0.5f, -1.08f}, 1u, hit);
            ssfx_test::hitBox(o, d, Vec3{-1.2f, 0.f, 1.08f}, Vec3{1.2f, 0.5f, 1.2f}, 1u, hit);
            {
                const f32 denom = rampN.dot(d);
                if (std::fabs(denom) > 1e-8f) {
                    const f32 t = rampN.dot(Vec3{-1.8f, 0.f, 0.f} - o) / denom;
                    const Vec3 q = o + d * t;
                    if (t > 1e-4f && t < hit.t && q.x >= -2.2f && q.x <= -1.8f && std::fabs(q.z) <= 1.f) {
                        hit.t = t;
                        hit.n = denom < 0.f ? rampN : rampN * -1.f;
                        hit.material = 2u;
                    }
                }
            }
            if (hit.t >= 1e29f) {
                continue;
            }
            const Vec3 p = o + d * hit.t;
            const ssfx_test::Material& m = hit.material == 0u ? floorMat : (hit.material == 1u ? wallMat : emitMat);
            const Vec3 radiance = hit.material == 2u ? Vec3{20.f, 20.f, 20.f} : Vec3{0.f, 0.f, 0.f};
            ssfx_test::writePixel(f.g, f.camera, i, hit, viewDir, m, radiance);
            if (hit.material == 0u && std::fabs(p.x) < 0.95f && std::fabs(p.z) < 0.95f) {
                s.inside[i] = 1u;
            }
            if (hit.material == 0u && p.x > -1.75f && p.x < -1.25f && std::fabs(p.z) < 0.9f) {
                s.outside[i] = 1u;
            }
        }
    }
    prepare(f);
}

f64 maskedLum(const std::vector<f32>& img, const std::vector<u8>& mask, u32* count = nullptr) {
    f64 sum = 0.0;
    u32 n = 0;
    for (usize i = 0; i < mask.size(); ++i) {
        if (mask[i] != 0u) {
            sum += lum(img, i);
            ++n;
        }
    }
    if (count != nullptr) {
        *count = n;
    }
    return n != 0u ? sum / n : 0.0;
}

void testLeak() {
    LeakScene s;
    buildLeak(s, 128, 96);
    u32 insideCount = 0;
    u32 outsideCount = 0;
    maskedLum(s.f.in.lit, s.inside, &insideCount);
    maskedLum(s.f.in.lit, s.outside, &outsideCount);
    std::printf("leak: sealed box 128x96 seen from above (walls 0.12 thick, 0.5 high; emissive ramp <= 0.35 high "
                "outside, facing the box): %u inside / %u outside receiver pixels\n",
                insideCount, outsideCount);
    expect(insideCount > 300u && outsideCount > 100u, "leak scene has receivers");
    struct Cfg {
        const char* name;
        f32 thickness;
        bool fix;
        f32 tolerance;
        bool bruteForce;
    };
    f64 leaks[4] = {};
    f64 bfLeaks[4] = {};
    u32 idx = 0;
    for (const Cfg& cfg : {Cfg{"slab 1.0 >= wall, bilinear fix (default)", 1.0f, true, 0.f, true},
                           Cfg{"slab 1.0 >= wall, bilinear fix + plane test 0.02", 1.0f, true, 0.02f, false},
                           Cfg{"slab 1.0 >= wall, vanilla", 1.0f, false, 0.f, false},
                           Cfg{"slab 0.1 < wall height, bilinear fix", 0.1f, true, 0.f, true}}) {
        SsrcSettings st{};
        st.thickness = cfg.thickness;
        st.bilinearFix = cfg.fix;
        st.planeTolerance = cfg.tolerance;
        SsrcFrameConstants c{};
        expect(constantsFor(s.f, st, c), "constants");
        SsrcCpuSolver solver;
        std::vector<f32> ind;
        std::vector<f32> img;
        expect(solver.solve(c, s.f.in, nullptr, ind, img), "solve");
        const f64 in = maskedLum(ind, s.inside);
        const f64 out = maskedLum(ind, s.outside);
        f64 maxIn = 0.0;
        for (usize i = 0; i < s.inside.size(); ++i) {
            if (s.inside[i] != 0u) {
                maxIn = std::max(maxIn, lum(ind, i));
            }
        }
        leaks[idx] = out > 0.0 ? in / out : 1.0;
        std::printf("leak: %-46s inside mean %.5f (max %.4f), outside mean %.4f -> leak %.3f%%", cfg.name, in, maxIn, out,
                    100.0 * leaks[idx]);
        if (cfg.bruteForce) {
            std::vector<f32> bf;
            bruteForce(c, s.f.in, nullptr, 16u, bf);
            const f64 bfIn = maskedLum(bf, s.inside);
            const f64 bfOut = maskedLum(bf, s.outside);
            bfLeaks[idx] = bfOut > 0.0 ? bfIn / bfOut : 1.0;
            std::printf("; brute force inside %.5f outside %.4f -> %.3f%%", bfIn, bfOut, 100.0 * bfLeaks[idx]);
            if (idx == 0u) {
                expect(out > 0.05 && std::fabs(out - bfOut) <= 0.1 * bfOut, "outside light within 10% of brute force");
            }
        }
        std::printf("\n");
        ++idx;
    }
    expect(bfLeaks[0] <= 1e-4, "brute force: walls within the slab do not leak");
    expect(leaks[0] <= 1e-4, "SSRC (bilinear fix): leak through walls within the slab <= 0.01% of the outside light");
    expect(leaks[1] <= 1e-4, "bilinear fix + plane test: leak <= 0.01%");
    expect(leaks[2] > 1e-3 && leaks[2] > 10.0 * leaks[0], "vanilla leaks (> 0.1%, > 10x the bilinear fix)");
    expect(bfLeaks[3] > 1e-2 && leaks[3] > 1e-2 && leaks[3] <= 1.5 * bfLeaks[3] + 1e-2,
           "a slab thinner than the walls' depth extent leaks in both (screen-space limit), SSRC within 1.5x + 1% of "
           "the brute force");
}

// --- SSGI comparison ---------------------------------------------------------------------------------------------
/// World-space one-bounce reference of the room: cosine-weighted rays (raysSqrt^2) from each geometry pixel's analytic
/// surface point against the analytic room (off-screen included); hit radiance = the room's lit formula, misses 0.
void roomWorldReference(const ssfx_test::RoomScene& room, const SsrcInputs& in, u32 raysSqrt, std::vector<f32>& out) {
    const u32 w = room.g.width;
    const u32 h = room.g.height;
    out.assign(static_cast<usize>(w) * h * 4u, 0.f);
    const Vec3 sun = room.sunDir.normalized();
    auto shade = [&](const ssfx_test::Hit& hit) {
        const ssfx_test::Material& m = room.materials[hit.material];
        const f32 ndl = std::max(0.f, hit.n.dot(sun));
        const Vec3 a{ssfx_test::unorm8(m.albedo.x), ssfx_test::unorm8(m.albedo.y), ssfx_test::unorm8(m.albedo.z)};
        const f32 kd = 1.f - ssfx_test::unorm8(m.metallic);
        return Vec3{a.x * (room.ambient[0] * ssfx_test::half(m.ao) + kd * room.sunColor.x * ndl / ssfx_test::kPi),
                    a.y * (room.ambient[1] * ssfx_test::half(m.ao) + kd * room.sunColor.y * ndl / ssfx_test::kPi),
                    a.z * (room.ambient[2] * ssfx_test::half(m.ao) + kd * room.sunColor.z * ndl / ssfx_test::kPi)};
    };
    auto cast = [&](const Vec3& o, const Vec3& d, ssfx_test::Hit& hit) {
        ssfx_test::hitPlane(o, d, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, 0.f, 0.f}, 0u, hit);
        ssfx_test::hitPlane(o, d, Vec3{0.f, 0.f, 1.f}, Vec3{0.f, 0.f, -4.f}, 1u, hit);
        ssfx_test::hitPlane(o, d, Vec3{1.f, 0.f, 0.f}, Vec3{-3.5f, 0.f, 0.f}, 2u, hit);
        ssfx_test::hitBox(o, d, Vec3{-1.8f, 0.f, -2.2f}, Vec3{-0.6f, 1.6f, -1.2f}, 3u, hit);
        ssfx_test::hitBox(o, d, Vec3{0.8f, 0.f, -1.0f}, Vec3{1.7f, 0.9f, 0.1f}, 4u, hit);
        if (hit.t >= 1e29f) {
            return false;
        }
        const Vec3 p = o + d * hit.t;
        return !(p.y > 3.5f || p.y < -0.01f || p.x > 4.5f);
    };
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const usize i = static_cast<usize>(y) * w + x;
            out[i * 4u + 3u] = 1.f;
            if (room.g.viewZ[i] <= 0.f) {
                continue;
            }
            Vec3 viewDir{};
            const Vec3 d = room.camera.rayWorld(x, y, viewDir);
            ssfx_test::Hit primary{};
            if (!cast(room.camera.eye, d, primary)) {
                continue;
            }
            const Vec3 n = primary.n;
            const Vec3 p = room.camera.eye + d * primary.t + n * 1e-4f;
            f64 sr = 0.0;
            f64 sg = 0.0;
            f64 sb = 0.0;
            for (u32 a = 0; a < raysSqrt; ++a) {
                for (u32 b = 0; b < raysSqrt; ++b) {
                    const Vec3 dir = ssfx::ssgi_kernel::sample_direction(n, x, y, a, b, raysSqrt).normalized();
                    ssfx_test::Hit hit{};
                    if (cast(p, dir, hit) && hit.n.dot(dir) < 0.f) {
                        const Vec3 l = shade(hit);
                        sr += l.x;
                        sg += l.y;
                        sb += l.z;
                    }
                }
            }
            const f64 inv = 1.0 / (static_cast<f64>(raysSqrt) * raysSqrt);
            out[i * 4u] = static_cast<f32>(in.diffuse[i * 4u] * sr * inv);
            out[i * 4u + 1u] = static_cast<f32>(in.diffuse[i * 4u + 1u] * sg * inv);
            out[i * 4u + 2u] = static_cast<f32>(in.diffuse[i * 4u + 2u] * sb * inv);
        }
    }
}

/// WP-6.3 SSGI (computeSsgiCpu, one bounce) + its cost: depth samples of every march step and bisection step.
void ssgiRun(const Frame& f, u32 sampleSqrt, std::vector<f32>& out, f64& samplesPerPixel) {
    const usize n = static_cast<usize>(f.g.width) * f.g.height;
    std::vector<f32> depth(n);
    for (usize i = 0; i < n; ++i) {
        depth[i] = f.prepared.prepared[i].x;
    }
    const ssfx::SsfxGBufferView view = ssfx_gpu::prepared_view(f.sc, depth, f.prepared.normal);
    ssfx::SsgiParams params{};
    params.sample_sqrt = sampleSqrt;
    std::vector<Vec3> gi(n);
    expect(ssfx::computeSsgiCpu(view, f.prepared.radiance.data(), f.prepared.diffuse.data(), params, gi.data()),
           "computeSsgiCpu");
    out.assign(n * 4u, 0.f);
    for (usize i = 0; i < n; ++i) {
        out[i * 4u] = gi[i].x;
        out[i * 4u + 1u] = gi[i].y;
        out[i * 4u + 2u] = gi[i].z;
        out[i * 4u + 3u] = 1.f;
    }
    // Cost: re-trace every gather ray (ssgi_kernel::pixel_gather's loop) and count the depth lookups.
    const ssfx::SsgiParams p = ssfx::ssgi_kernel::clamp_params(params);
    const ssfx::SsrParams trace = ssfx::ssgi_kernel::trace_params(p);
    u64 samples = 0;
    for (u32 y = 0; y < f.g.height; ++y) {
        for (u32 x = 0; x < f.g.width; ++x) {
            if (view.depthAt(x, y) <= 0.f) {
                continue;
            }
            const Vec3 pos = view.positionAt(x, y);
            Vec3 nrm = view.normalAt(x, y).normalized();
            if (nrm.dot(pos) > 0.f) {
                nrm = nrm * -1.f;
            }
            for (u32 a = 0; a < p.sample_sqrt; ++a) {
                for (u32 b = 0; b < p.sample_sqrt; ++b) {
                    const Vec3 dir = ssfx::ssgi_kernel::sample_direction(nrm, x, y, a, b, p.sample_sqrt);
                    const ssfx::SsrHit hit = ssfx::ssr_kernel::trace_ray(view, f.prepared.radiance.data(), trace, x, y, dir);
                    samples += hit.steps_taken + (hit.hit ? p.refine_steps : 0u);
                }
            }
        }
    }
    samplesPerPixel = static_cast<f64>(samples) / static_cast<f64>(n);
}

void testSsgi() {
    ssfx_test::RoomScene room;
    constexpr u32 kW = 128;
    constexpr u32 kH = 96;
    ssfx_test::buildRoom(room, kW, kH, 0);
    Frame f;
    f.camera = room.camera;
    f.ambient[0] = room.ambient[0];
    f.ambient[1] = room.ambient[1];
    f.ambient[2] = room.ambient[2];
    f.g = room.g;
    prepare(f);
    const std::vector<u8> mask = geometryMask(f.in);
    const f64 pixels = static_cast<f64>(kW) * kH;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<f32> world;
    roomWorldReference(room, f.in, 32u, world);
    const f64 worldTime = seconds(t0);
    SsrcSettings s{};
    SsrcFrameConstants c{};
    expect(constantsFor(f, s, c), "constants");
    std::vector<f32> bf;
    SsrcStats bfStats{};
    t0 = std::chrono::steady_clock::now();
    bruteForce(c, f.in, nullptr, 32u, bf, &bfStats);
    const f64 bfTime = seconds(t0);
    std::printf("ssgi: room %ux%u camera 0 (open sky = 0 for every method, one diffuse bounce of the lit image)\n", kW, kH);
    std::printf("  references: world-space ray traced 1024 rays/px (analytic room, off-screen included; %.1f s), "
                "screen-space brute force 1024 rays/px (%.0f samples/px; %.1f s)\n",
                worldTime, static_cast<f64>(bfStats.samples) / pixels, bfTime);
    const SsrcErrorMetrics bfVsWorld = compareIndirect(bf, world, mask);
    std::printf("  %-34s %10s %10s %10s %10s %12s\n", "method", "L1 vs BF", "RMSE vs BF", "L1 vs world", "mean", "samples/px");
    std::printf("  %-34s %10s %10s %10.4f %10.4f %12.0f\n", "brute force (screen space, 1024)", "-", "-", bfVsWorld.relL1,
                bfVsWorld.meanTest, static_cast<f64>(bfStats.samples) / pixels);
    std::printf("  %-34s %10s %10s %10s %10.4f %12s\n", "world reference", "-", "-", "-", bfVsWorld.meanRef, "-");

    f64 ssgi16L1 = 0.0;
    f64 ssgi16Cost = 0.0;
    f64 ssgi64L1 = 0.0;
    f64 ssgi64Cost = 0.0;
    f64 ssgi256L1 = 0.0;
    for (const u32 sq : {4u, 8u, 16u}) {
        std::vector<f32> gi;
        f64 cost = 0.0;
        ssgiRun(f, sq, gi, cost);
        const SsrcErrorMetrics vb = compareIndirect(gi, bf, mask);
        const SsrcErrorMetrics vw = compareIndirect(gi, world, mask);
        char name[64];
        std::snprintf(name, sizeof(name), "WP-6.3 SSGI %u rays", sq * sq);
        std::printf("  %-34s %10.4f %10.4f %10.4f %10.4f %12.0f\n", name, vb.relL1, vb.relRmse, vw.relL1, vb.meanTest, cost);
        if (sq == 4u) {
            ssgi16L1 = vb.relL1;
            ssgi16Cost = cost;
        } else if (sq == 8u) {
            ssgi64L1 = vb.relL1;
            ssgi64Cost = cost;
        } else {
            ssgi256L1 = vb.relL1;
        }
    }
    f64 rcL1 = 0.0;
    f64 rcCost = 0.0;
    f64 rc2L1 = 0.0;
    f64 rc2Cost = 0.0;
    struct Cfg {
        const char* name;
        u32 s0;
        u32 r0;
    };
    for (const Cfg& cfg : {Cfg{"SSRC s0 1, N0 16 (default)", 1u, 4u}, Cfg{"SSRC s0 2, N0 16", 2u, 4u},
                           Cfg{"SSRC s0 1, N0 64", 1u, 8u}}) {
        SsrcSettings st{};
        st.probeSpacing0 = cfg.s0;
        st.dirRes0 = cfg.r0;
        SsrcFrameConstants cc{};
        expect(constantsFor(f, st, cc), "constants");
        SsrcCpuSolver solver;
        std::vector<f32> ind;
        std::vector<f32> img;
        SsrcStats stats{};
        expect(solver.solve(cc, f.in, nullptr, ind, img, &stats), "solve");
        const SsrcErrorMetrics vb = compareIndirect(ind, bf, mask);
        const SsrcErrorMetrics vw = compareIndirect(ind, world, mask);
        const f64 cost = static_cast<f64>(stats.samples) / pixels;
        std::printf("  %-34s %10.4f %10.4f %10.4f %10.4f %12.0f\n", cfg.name, vb.relL1, vb.relRmse, vw.relL1, vb.meanTest, cost);
        if (cfg.s0 == 1u && cfg.r0 == 4u) {
            rcL1 = vb.relL1;
            rcCost = cost;
        } else if (cfg.s0 == 2u) {
            rc2L1 = vb.relL1;
            rc2Cost = cost;
        }
    }
    expect(rcL1 < 0.5 * ssgi16L1 && rcL1 < ssgi256L1,
           "SSRC (default) error vs the screen-space reference < half of 16-ray SSGI's and below 256-ray SSGI's");
    expect(rc2L1 < ssgi64L1 && rc2Cost < ssgi64Cost, "SSRC s0 2 beats 64-ray SSGI at fewer depth samples");
    std::printf("  SSRC default vs SSGI: %.2fx lower L1 than SSGI-16 (at %.1fx its samples), SSGI-64 L1 %.4f\n",
                ssgi16L1 / std::max(rcL1, 1e-9), rcCost / std::max(ssgi16Cost, 1.0), ssgi64L1);
}

// --- api ---------------------------------------------------------------------------------------------------------
void testApi() {
    SsrcFrameConstants c{};
    SsrcSettings s{};
    expect(!resolve_layout(s, 0, 16, c), "empty extent rejected");
    s.probeSpacing0 = 3;
    expect(!resolve_layout(s, 16, 16, c), "non-power-of-two spacing rejected");
    s = SsrcSettings{};
    s.dirRes0 = 0;
    expect(!resolve_layout(s, 16, 16, c), "zero directions rejected");
    s = SsrcSettings{};
    s.stride = 0.01f;
    s.aoCascades = 99;
    s.cascadeCount = 3;
    s.bilinearFix = false;
    s.compose = true;
    expect(resolve_layout(s, 64, 64, c), "clamped settings resolve");
    expect(c.stride == 0.25f && c.cascades == 3u && c.aoCascades == 3u && c.flags == kSsrcFlagCompose,
           "stride / cascade / AO clamps and flags");
    Frame f;
    roomFrame(f, 48, 32, 0);
    SsrcFrameConstants rc{};
    s = SsrcSettings{};
    expect(constantsFor(f, s, rc), "constants");
    // viewToWorld o viewRot == I, cameraWorld == the eye.
    f64 err = 0.0;
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 col = 0; col < 3u; ++col) {
            f64 v = 0.0;
            for (u32 k = 0; k < 3u; ++k) {
                v += static_cast<f64>(rc.viewToWorld[r * 4u + k]) * rc.viewRot[col * 4u + k];
            }
            err = std::max(err, std::fabs(v - (r == col ? 1.0 : 0.0)));
        }
    }
    const f64 eyeErr = std::fabs(rc.cameraWorld[0] - f.camera.eye.x) + std::fabs(rc.cameraWorld[1] - f.camera.eye.y) +
                       std::fabs(rc.cameraWorld[2] - f.camera.eye.z);
    std::printf("api: viewToWorld x viewRot max |err| %.2g, camera position err %.2g, fx %.3f fy %.3f\n", err, eyeErr,
                static_cast<f64>(rc.fx), static_cast<f64>(rc.fy));
    expect(err < 1e-5 && eyeErr < 1e-4, "view <-> world rotations and camera position");
    ssfx_gpu::SsfxCameraDesc bad{};
    expect(!resolve_constants(s, bad, f.ambient, 48, 32, rc), "invalid projection rejected");
    SsrcGpu gpu;
    SsrcGpuDesc d{};
    expect(!gpu.init(d) && !gpu.valid(), "no device: init fails");
    expect(!querySsrcCapabilities(nullptr).ssrc, "no device: not capable");
}


} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "f16") {
        testF16();
    }
    if (all || suite == "furnace") {
        testFurnace();
    }
    if (all || suite == "error") {
        testError();
    }
    if (all || suite == "leak") {
        testLeak();
    }
    if (all || suite == "ssgi") {
        testSsgi();
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
