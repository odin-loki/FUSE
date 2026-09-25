// WP-6.5 radiance cascades (research): CPU metric gates (stub-safe). Lavapipe gates: test_rp_rc_gpu.cpp.
//
//   layout   RcPush size and its GLSL / Slang mirrors (names, order, offsets); the cascade layout (2x spacing,
//            4x directions, 4x contiguous intervals reaching the scene diagonal, constant records per cascade
//            for power-of-two extents); the direction table (unit vectors, children inside the parent's cone)
//   energy   empty scene: every pixel == the sky radiance (|error| <= 1e-6; both merge modes, odd extents,
//            s_0 1 / 2, N_0 4 / 8, branching 4 / 2); an empty black scene gives exactly 0; a lone disk emitter:
//            bilinear-fix frame mean within 10% of the brute force for the default and the two tuned configs
//            (RC over-estimates a small emitter by a few %: the probe interpolation blurs a convex 1 / r falloff;
//            near / far field printed, see report.md)
//   error    rooms scene vs brute-force per-pixel ray tracing (1024 rays / pixel): relative L1 over the free
//            texels below the documented bound (bilinear fix <= 0.12, vanilla <= 0.18) and fix < vanilla
//   leak     a sealed box next to a bright emitter (walls 1 / 2 / 4 texels): ground truth 0 inside; bilinear fix
//            leaks <= 1e-6 of the outside mean, vanilla's leak is measured (and must exceed the fix's)
//   ringing  lone disk emitter: angular non-uniformity on circles (ray-shaped artefacts): fix <= brute force +
//            0.02 and <= vanilla
//   cost     rays / samples per pixel and records per cascade for 32^2 .. 256^2: branching 4 keeps the records
//            per cascade constant and its samples per pixel grow at most linearly with the extent (256^2 <= 5 x
//            64^2: ray length 4x per cascade, the top cascades dominate); branching 2 halves the records per
//            cascade and costs about the same per cascade (~ log extent; 256^2 <= 2.2 x 64^2: the lower cascades'
//            rays still end early on walls at 64^2); wall times printed only
//
// Metrics are printed in the form report.md quotes. Exit 0 = pass.
#include <fuse/renderer/research/rc/rc_reference.hpp>
#include <fuse/renderer/research/rc/rc_types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::research::rc;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

f64 msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t).count();
}

const f32 kSky[3] = {0.05f, 0.08f, 0.12f};

// --- layout ----------------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define RC_FIELD(n) Field{#n, offsetof(RcPush, n)}
const Field kFields[] = {
    RC_FIELD(scene),   RC_FIELD(dirs),         RC_FIELD(upper),        RC_FIELD(dst),     RC_FIELD(width),
    RC_FIELD(height),  RC_FIELD(probesX),      RC_FIELD(probesY),      RC_FIELD(dirCount), RC_FIELD(upperProbesX),
    RC_FIELD(upperProbesY), RC_FIELD(flags),   RC_FIELD(spacing),      RC_FIELD(upperSpacing), RC_FIELD(tStart),
    RC_FIELD(tEnd),    RC_FIELD(step),         RC_FIELD(invStep),      RC_FIELD(skyR),    RC_FIELD(skyG),
    RC_FIELD(skyB),    RC_FIELD(count),        RC_FIELD(branch),       RC_FIELD(invBranch),
};
#undef RC_FIELD

/// Parses the block starting at `opener` up to "}" of a shader source: (name, offset) per scalar field.
bool parseShaderStruct(const std::string& path, const char* opener, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const size_t begin = text.find(opener);
    if (begin == std::string::npos) {
        return false;
    }
    const size_t end = text.find('}', begin);
    if (end == std::string::npos) {
        return false;
    }
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
        if (decl.find('[') != std::string::npos) {
            return false; // arrays are not allowed in the push block (Slang std140)
        }
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes;
    }
    size = (offset + 7u) / 8u * 8u;
    return true;
}

void testLayout() {
    expect(sizeof(RcPush) == 112u, "RcPush is 112 bytes (<= 128 guaranteed push-constant bytes)");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    const char* openers[2] = {"uniform RcPushBlock {", "struct RcPush {"};
    const char* langs[2] = {"glsl", "slang"};
    for (u32 l = 0; l < 2u; ++l) {
        const std::string path = std::string(FUSE_RP_RC_SHADER_DIR) + "/rc_common." + langs[l];
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, openers[l], offsets, size, names);
        expect(parsed, "rc_common push block parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == sizeof(RcPush);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", langs[l], i, names[i].c_str(),
                             offsets[i], kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: rc_common.%s RcPush %zu fields, %zu bytes\n", langs[l], offsets.size(), size);
        expect(same, "shader RcPush == C++ RcPush (names, order, offsets, size)");
    }

    struct Case {
        u32 w, h, s0, n0;
        f32 r0;
        u32 b;
    };
    for (const Case c : {Case{128, 128, 1, 4, 1.0f, 4}, Case{97, 61, 1, 4, 1.0f, 4}, Case{256, 128, 2, 8, 2.0f, 4},
                         Case{1920, 1080, 2, 4, 2.0f, 4}, Case{1, 1, 1, 4, 1.0f, 4}, Case{128, 128, 1, 8, 1.0f, 2},
                         Case{1920, 1080, 1, 8, 1.0f, 2}}) {
        RcSettings s{};
        s.probeSpacing0 = c.s0;
        s.dirs0 = c.n0;
        s.interval0 = c.r0;
        s.branch = c.b;
        const f32 fb = static_cast<f32>(c.b);
        RcLayout l{};
        const bool ok = RcLayout::compute(s, c.w, c.h, l);
        expect(ok && l.cascades >= 1u, "layout computes");
        if (!ok) {
            continue;
        }
        const f64 diag = std::sqrt(static_cast<f64>(c.w) * c.w + static_cast<f64>(c.h) * c.h);
        expect(l.tEnd[l.cascades - 1u] >= diag || l.cascades == kRcMaxCascades, "top cascade reaches the diagonal");
        expect(l.cascades == 1u || l.tEnd[l.cascades - 2u] < diag, "no superfluous cascade");
        bool scaling = l.tStart[0] == 0.0f && l.tEnd[0] == c.r0 && l.spacing[0] == static_cast<f32>(c.s0);
        for (u32 i = 1; i < l.cascades; ++i) {
            scaling = scaling && l.spacing[i] == 2.0f * l.spacing[i - 1u] && l.dirs[i] == c.b * l.dirs[i - 1u] &&
                      l.tStart[i] == l.tEnd[i - 1u] && l.tEnd[i] - l.tStart[i] == fb * (l.tEnd[i - 1u] - l.tStart[i - 1u]) &&
                      l.dirOffset[i] == l.dirOffset[i - 1u] + l.dirs[i - 1u];
        }
        expect(scaling, "2x spacing, b x directions, contiguous b x intervals");
        const bool pow2 = (c.w & (c.w - 1u)) == 0u && (c.h & (c.h - 1u)) == 0u && c.b == 4u;
        if (pow2) {
            bool constant = true;
            for (u32 i = 0; i < l.cascades; ++i) {
                // Probe grids stop shrinking at 1 x 1, so only cascades with >= 1 probe per axis at full density.
                if (l.spacing[i] <= static_cast<f32>(std::min(c.w, c.h))) {
                    constant = constant && l.records[i] == l.records[0];
                }
            }
            expect(constant, "power-of-two extents: every cascade holds the same number of records");
        }
        std::printf("layout: %4ux%-4u b %u s0 %u N0 %u r0 %.0f: %2u cascades, records/cascade %llu (max %llu, total %llu, "
                    "%.1f per pixel), %.2f MiB work, reach %.0f\n",
                    c.w, c.h, c.b, c.s0, c.n0, static_cast<f64>(c.r0), l.cascades,
                    static_cast<unsigned long long>(l.records[0]), static_cast<unsigned long long>(l.maxRecords),
                    static_cast<unsigned long long>(l.totalRecords),
                    static_cast<f64>(l.totalRecords) / (static_cast<f64>(c.w) * c.h),
                    static_cast<f64>(2u * l.maxRecords * 16u + static_cast<u64>(c.w) * c.h * 16u) / (1024.0 * 1024.0),
                    static_cast<f64>(l.tEnd[l.cascades - 1u]));
        std::vector<f32> dirs;
        buildDirectionTable(l, dirs);
        bool unit = dirs.size() == static_cast<usize>(l.dirTotal) * 4u;
        bool cone = true;
        for (u32 i = 0; unit && i < l.cascades; ++i) {
            for (u32 k = 0; k < l.dirs[i]; ++k) {
                const f32* d = dirs.data() + (static_cast<usize>(l.dirOffset[i]) + k) * 4u;
                unit = unit && std::fabs(std::sqrt(static_cast<f64>(d[0]) * d[0] + static_cast<f64>(d[1]) * d[1]) - 1.0) < 1e-6;
                if (i + 1u < l.cascades) {
                    // Children 4k .. 4k + 3 lie within half a parent sector of the parent direction.
                    const f64 half = 3.14159265358979323846 / l.dirs[i];
                    for (u32 j = 0; j < c.b; ++j) {
                        const f32* e = dirs.data() + (static_cast<usize>(l.dirOffset[i + 1u]) + c.b * k + j) * 4u;
                        const f64 dotv = static_cast<f64>(d[0]) * e[0] + static_cast<f64>(d[1]) * e[1];
                        cone = cone && dotv >= std::cos(half) - 1e-6;
                    }
                }
            }
        }
        expect(unit, "direction table: unit vectors");
        expect(cone, "direction table: children inside the parent's cone");
    }
    RcSettings bad{};
    bad.probeSpacing0 = 3u;
    RcLayout l{};
    expect(!RcLayout::compute(bad, 64, 64, l), "non-power-of-two spacing rejected");
    expect(!RcLayout::compute(RcSettings{}, 0, 64, l), "empty extent rejected");
}

// --- energy ----------------------------------------------------------------------------------------------
void testEnergy() {
    RcCpuSolver solver;
    std::vector<f32> out;
    struct Case {
        u32 w, h, s0, n0, b;
    };
    f64 worst = 0.0;
    for (const Case c : {Case{64, 64, 1, 4, 4}, Case{97, 61, 1, 4, 4}, Case{97, 61, 2, 8, 4}, Case{40, 130, 2, 4, 4},
                         Case{97, 61, 1, 8, 2}}) {
        for (const bool fix : {false, true}) {
            RcScene scene;
            sceneEmpty(scene, c.w, c.h);
            RcSettings s{};
            s.probeSpacing0 = c.s0;
            s.dirs0 = c.n0;
            s.interval0 = static_cast<f32>(c.s0);
            s.bilinearFix = fix;
            s.branch = c.b;
            s.sky[0] = 1.0f;
            s.sky[1] = 0.5f;
            s.sky[2] = 0.25f;
            expect(solver.solve(scene, s, out), "solve");
            for (usize i = 0; i < out.size() / 4u; ++i) {
                for (u32 k = 0; k < 3u; ++k) {
                    worst = std::max(worst, std::fabs(static_cast<f64>(out[i * 4u + k]) - s.sky[k]) / s.sky[k]);
                }
            }
            s.sky[0] = s.sky[1] = s.sky[2] = 0.0f;
            expect(solver.solve(scene, s, out), "solve");
            bool zero = true;
            for (const f32 v : out) {
                zero = zero && (v == 0.0f || v == 1.0f);
            }
            expect(zero, "empty black scene: exactly 0");
        }
    }
    std::printf("energy: empty scene under a uniform sky, max relative deviation from the sky %.3g (20 runs)\n", worst);
    expect(worst <= 1e-6, "empty scene reproduces the sky within 1e-6");

    // A lone emitter in empty space: the frame mean (over free texels) against the brute force.
    RcScene disk;
    sceneDisk(disk, 64, 64, 3.0f);
    const f32 black[3] = {0.0f, 0.0f, 0.0f};
    std::vector<f32> ref;
    bruteForce(disk, 1024, 0.5f, black, ref);
    const std::vector<u8> mask = freeSpaceMask(disk);
    // Near field (centre distance < 8 texels, i.e. within 5 texels of the emitter's edge) and far field.
    std::vector<u8> near(mask.size(), 0u);
    std::vector<u8> far(mask.size(), 0u);
    for (u32 y = 0; y < disk.height; ++y) {
        for (u32 x = 0; x < disk.width; ++x) {
            const usize i = static_cast<usize>(y) * disk.width + x;
            const f64 dx = static_cast<f64>(x) + 0.5 - 32.0;
            const f64 dy = static_cast<f64>(y) + 0.5 - 32.0;
            const bool isNear = dx * dx + dy * dy < 64.0;
            near[i] = static_cast<u8>(mask[i] != 0u && isNear);
            far[i] = static_cast<u8>(mask[i] != 0u && !isNear);
        }
    }
    struct Config {
        u32 b, n0;
        f32 r0;
    };
    for (const Config c : {Config{4, 4, 1.0f}, Config{4, 16, 2.0f}, Config{2, 16, 4.0f}}) {
        for (const bool fix : {false, true}) {
            RcSettings s{};
            s.bilinearFix = fix;
            s.branch = c.b;
            s.dirs0 = c.n0;
            s.interval0 = c.r0;
            expect(solver.solve(disk, s, out), "solve");
            const f64 all = maskedMean(out, mask);
            const f64 allRef = maskedMean(ref, mask);
            const f64 n = maskedMean(out, near);
            const f64 nRef = maskedMean(ref, near);
            const f64 f = maskedMean(out, far);
            const f64 fRef = maskedMean(ref, far);
            std::printf("energy: disk emitter 64^2, b %u N0 %2u r0 %.0f %-12s frame mean RC / brute force %+.2f%%, near "
                        "field %+.2f%%, far field %+.2f%%\n",
                        c.b, c.n0, static_cast<f64>(c.r0), fix ? "bilinear-fix" : "vanilla",
                        100.0 * (all - allRef) / allRef, 100.0 * (n - nRef) / nRef, 100.0 * (f - fRef) / fRef);
            if (fix) {
                expect(std::fabs(all - allRef) <= 0.10 * allRef,
                       "disk emitter (bilinear fix): frame mean within 10% of the brute force");
            }
        }
    }
}

// --- error -----------------------------------------------------------------------------------------------
void testError() {
    constexpr u32 kSize = 64;
    constexpr u32 kRays = 1024;
    RcCpuSolver solver;
    std::vector<f32> out;
    std::vector<f32> ref;
    std::vector<f32> refHi;
    f64 worstFix = 0.0;
    f64 worstVanilla = 0.0;
    for (u32 variant = 0; variant < 2u; ++variant) {
        RcScene scene;
        sceneRooms(scene, kSize, kSize, variant * 2u);
        const std::vector<u8> mask = freeSpaceMask(scene);
        const auto t0 = std::chrono::steady_clock::now();
        RcStats bs{};
        bruteForce(scene, kRays, 0.5f, kSky, ref, &bs);
        const f64 bfMs = msSince(t0);
        if (variant == 0u) {
            // The ground truth's own angular error: 1024 vs 4096 rays.
            bruteForce(scene, 4096u, 0.5f, kSky, refHi);
            const RcErrorMetrics self = compareImages(ref, refHi, mask);
            std::printf("error: brute force 1024 vs 4096 rays / pixel: relL1 %.4f (the ground truth's own error)\n",
                        self.relL1);
        }
        struct Config {
            u32 s0, n0;
            bool fix;
            u32 b;
            f32 r0;
        };
        for (const Config c : {Config{1, 4, false, 4, 1.0f}, Config{1, 4, true, 4, 1.0f}, Config{1, 8, false, 4, 1.0f},
                               Config{1, 8, true, 4, 1.0f}, Config{2, 4, false, 4, 2.0f}, Config{2, 4, true, 4, 2.0f},
                               Config{1, 16, true, 4, 2.0f}, Config{1, 16, true, 2, 4.0f}}) {
            RcSettings s{};
            s.probeSpacing0 = c.s0;
            s.dirs0 = c.n0;
            s.interval0 = c.r0;
            s.branch = c.b;
            s.bilinearFix = c.fix;
            s.sky[0] = kSky[0];
            s.sky[1] = kSky[1];
            s.sky[2] = kSky[2];
            RcStats st{};
            const auto t1 = std::chrono::steady_clock::now();
            expect(solver.solve(scene, s, out, &st), "solve");
            const f64 ms = msSince(t1);
            const RcErrorMetrics m = compareImages(out, ref, mask);
            std::printf("error: rooms %u^2 v%u b %u s0 %u N0 %2u r0 %.0f %-12s relL1 %.4f relRMSE %.4f maxAbs %.3f mean %.4f/%.4f  "
                        "rays/px %.1f samples/px %.1f (brute force %.0f)  %.1f ms (brute force %.0f ms)\n",
                        kSize, variant * 2u, c.b, c.s0, c.n0, static_cast<f64>(c.r0), c.fix ? "bilinear-fix" : "vanilla", m.relL1, m.relRmse,
                        m.maxAbs, m.meanTest, m.meanRef, static_cast<f64>(st.rays) / (kSize * kSize),
                        static_cast<f64>(st.samples) / (kSize * kSize), static_cast<f64>(bs.samples) / (kSize * kSize),
                        ms, bfMs);
            if (c.s0 == 1u && c.n0 == 4u && c.b == 4u) {
                if (c.fix) {
                    worstFix = std::max(worstFix, m.relL1);
                } else {
                    worstVanilla = std::max(worstVanilla, m.relL1);
                }
            }
        }
    }
    std::printf("error: default config (s0 1, N0 4): worst relL1 bilinear fix %.4f (bound 0.12), vanilla %.4f "
                "(bound 0.18)\n",
                worstFix, worstVanilla);
    expect(worstFix <= 0.12, "bilinear fix: relative L1 vs brute force <= 0.12");
    expect(worstVanilla <= 0.18, "vanilla: relative L1 vs brute force <= 0.18");
    expect(worstFix < worstVanilla, "bilinear fix is more accurate than vanilla");
}

// --- leak ------------------------------------------------------------------------------------------------
void testLeak() {
    RcCpuSolver solver;
    std::vector<f32> out;
    for (const u32 wall : {1u, 2u, 4u}) {
        RcScene scene;
        std::vector<u8> inside;
        sceneLeakBox(scene, 128, 128, wall, inside);
        const std::vector<u8> free = freeSpaceMask(scene);
        f64 leak[2] = {0.0, 0.0};
        for (u32 fix = 0; fix < 2u; ++fix) {
            RcSettings s{};
            s.bilinearFix = fix == 1u;
            expect(solver.solve(scene, s, out), "solve");
            const f64 in = maskedMean(out, inside);
            std::vector<u8> outside = free;
            for (usize i = 0; i < outside.size(); ++i) {
                outside[i] = static_cast<u8>(outside[i] != 0u && inside[i] == 0u);
            }
            const f64 out_ = maskedMean(out, outside);
            leak[fix] = out_ > 0.0 ? in / out_ : 0.0;
            std::printf("leak: box wall %u, %-12s interior mean %.6f, exterior mean %.4f, leak ratio %.2e (ground "
                        "truth 0)\n",
                        wall, fix == 1u ? "bilinear-fix" : "vanilla", in, out_, leak[fix]);
        }
        expect(leak[1] <= 1e-6, "bilinear fix: no leak through a sealed wall");
        expect(leak[0] > leak[1], "vanilla leaks more than the bilinear fix");
    }
}

// --- ringing ---------------------------------------------------------------------------------------------
void testRinging() {
    constexpr u32 kSize = 96;
    RcScene scene;
    sceneDisk(scene, kSize, kSize, 3.0f);
    const f32 black[3] = {0.0f, 0.0f, 0.0f};
    std::vector<f32> ref;
    bruteForce(scene, 1024, 0.5f, black, ref);
    const f32 c = static_cast<f32>(kSize) * 0.5f;
    const f64 bf = ringNonUniformity(ref, scene, c, c, 6, 44);
    RcCpuSolver solver;
    std::vector<f32> out;
    f64 ring[2] = {0.0, 0.0};
    const std::vector<u8> mask = freeSpaceMask(scene);
    for (u32 fix = 0; fix < 2u; ++fix) {
        RcSettings s{};
        s.bilinearFix = fix == 1u;
        expect(solver.solve(scene, s, out), "solve");
        ring[fix] = ringNonUniformity(out, scene, c, c, 6, 44);
        const RcErrorMetrics m = compareImages(out, ref, mask);
        std::printf("ringing: disk %u^2, %-12s angular CV %.4f (brute force %.4f), relL1 %.4f\n", kSize,
                    fix == 1u ? "bilinear-fix" : "vanilla", ring[fix], bf, m.relL1);
    }
    expect(ring[1] <= bf + 0.02, "bilinear fix: ringing within 0.02 of the brute force's");
    expect(ring[1] <= ring[0], "bilinear fix rings no more than vanilla");
}

// --- cost ------------------------------------------------------------------------------------------------
void testCost() {
    RcCpuSolver solver;
    std::vector<f32> out;
    // [config]: b 4 (N0 4, r0 1) vanilla / fix, b 2 (N0 16, r0 4) vanilla / fix
    f64 samplesPerPixel[4][4] = {};
    const u32 sizes[4] = {32, 64, 128, 256};
    const char* names[4] = {"b4 vanilla", "b4 bilinear-fix", "b2 vanilla", "b2 bilinear-fix"};
    for (u32 si = 0; si < 4u; ++si) {
        const u32 n = sizes[si];
        RcScene scene;
        sceneRooms(scene, n, n, 1u);
        for (u32 cfg = 0; cfg < 4u; ++cfg) {
            const u32 fix = cfg & 1u;
            RcSettings s{};
            s.bilinearFix = fix == 1u;
            if (cfg >= 2u) {
                s.branch = 2u;
                s.dirs0 = 16u;
                s.interval0 = 4.0f;
            }
            RcStats st{};
            const auto t = std::chrono::steady_clock::now();
            expect(solver.solve(scene, s, out, &st), "solve");
            const f64 ms = msSince(t);
            const RcLayout& l = solver.layout();
            samplesPerPixel[cfg][si] = static_cast<f64>(st.samples) / (static_cast<f64>(n) * n);
            std::printf("cost: %3u^2 %-15s %2u cascades, records/cascade %llu, rays/px %.2f, samples/px %.1f "
                        "(per cascade:",
                        n, names[cfg], l.cascades,
                        static_cast<unsigned long long>(l.records[0]), static_cast<f64>(st.rays) / (static_cast<f64>(n) * n),
                        samplesPerPixel[cfg][si]);
            for (u32 i = 0; i < l.cascades; ++i) {
                std::printf(" %.1f", static_cast<f64>(st.samplesPerCascade[i]) / (static_cast<f64>(n) * n));
            }
            std::printf("), CPU %.1f ms\n", ms);
            bool scaling = true;
            for (u32 i = 0; i < l.cascades; ++i) {
                if (l.spacing[i] <= static_cast<f32>(n)) {
                    // b 4: constant records per cascade; b 2: records halve per cascade.
                    const u64 expected = cfg < 2u ? l.records[0] : l.records[0] >> i;
                    scaling = scaling && l.records[i] == expected;
                }
            }
            expect(scaling, "records per cascade: constant (b 4) / halving (b 2)");
        }
    }
    for (u32 cfg = 0; cfg < 4u; ++cfg) {
        const f64 growth = samplesPerPixel[cfg][3] / samplesPerPixel[cfg][1];
        std::printf("cost: %-15s samples/px growth 64^2 -> 256^2: x%.2f (4x the extent, 16x the pixels)\n", names[cfg],
                    growth);
        if (cfg < 2u) {
            expect(growth <= 5.0, "b 4: samples per pixel grow at most linearly with the extent");
        } else {
            expect(growth <= 2.2, "b 2: samples per pixel grow sub-linearly (~ log) with the extent");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    bool ran = false;
    if (all || suite == "layout") {
        testLayout();
        ran = true;
    }
    if (all || suite == "energy") {
        testEnergy();
        ran = true;
    }
    if (all || suite == "error") {
        testError();
        ran = true;
    }
    if (all || suite == "leak") {
        testLeak();
        ran = true;
    }
    if (all || suite == "ringing") {
        testRinging();
        ran = true;
    }
    if (all || suite == "cost") {
        testCost();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
