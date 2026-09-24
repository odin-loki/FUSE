// WP-5.4 compute software rasteriser, CPU gates (stub-safe: only the reference kernels and the API's
// failure paths). Lavapipe gates: test_rp_swraster.cpp.
//
//   layout    record sizes / offsets, count words, the integer-range argument of swraster_kernel.hpp,
//             and the shader twins' constants (swraster_common.{glsl,slang}) == the C++ ones
//   fixed     ratio_round / ratio_floor_z (the exact integer perspective divide) == an f64 evaluation on
//             500k random inputs (inputs whose f64 value lies within 1e-6 of a rounding boundary are
//             skipped and counted) and on exact half-way cases (round half up); project_vertex domain
//   fill      top-left rule: a centre exactly on an edge belongs to exactly one of two triangles
//             sharing it (quads split along either diagonal and convex fans from different apexes
//             cover identical pixel sets, no pixel twice), both windings rasterise identically, top /
//             left edges are in and bottom / right edges out; depth is interpolated exactly at vertices
//   raster    the SW reference over every cluster of a far-away scene (micro triangles) == the WP-1.4
//             f64 raster reference (vis_reference.hpp) on every robust pixel (ids), no cluster demoted;
//             CpuReference == CpuParallel bit for bit
//   classify  every class occurs on a constructed scene (culled, SW, HW size / extent / clip); every
//             SW cluster's vertices project inside its corner rect (so the SW kernel never demotes
//             it); ForceHardware has no SW cluster, ForceSoftware's SW set contains Classify's;
//             threshold conversion; CpuReference == CpuParallel
//   api       SwRasterizer / capabilities fail cleanly without a device
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/swraster/swraster.hpp>
#include <fuse/renderer/swraster/swraster_kernel.hpp>
#include <fuse/renderer/swraster/swraster_reference.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::swraster;
using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::s64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using sw_kernel::SwVertex;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- math (column-major 4x4, Vulkan clip space, forward depth) --------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

/// Camera at the origin looking down -z (view = identity).
Mat4 camera(f32 aspect) {
    Mat4 v{};
    v.m[0] = v.m[5] = v.m[10] = v.m[15] = 1.f;
    return mul(perspective(1.1f, aspect, 0.5f, 120.f), v);
}

gpu_scene::GpuTransform place(f32 x, f32 y, f32 z, f32 s, f32 yaw = 0.f) {
    gpu_scene::GpuTransform t{};
    const f32 c = std::cos(yaw) * s;
    const f32 n = std::sin(yaw) * s;
    t.rows[0][0] = c;
    t.rows[0][2] = n;
    t.rows[1][1] = s;
    t.rows[2][0] = -n;
    t.rows[2][2] = c;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

/// A CPU-only GpuScene with three meshlet meshes (dense sphere, torus, box).
struct TestScene {
    gpu_scene::GpuScene scene;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<visbuffer::decode_kernel::MeshPositions> positions;
    SwSceneStorage storage;

    bool init() {
        gpu_scene::GpuSceneDesc d{};
        if (!scene.init(d)) {
            return false;
        }
        const vis_test::SourceMesh sources[3] = {vis_test::uvSphere(24, 40, 1.f), vis_test::torus(40, 20, 1.f, 0.35f),
                                                 vis_test::box()};
        meshes.resize(3);
        for (u32 i = 0; i < 3u; ++i) {
            if (!vis_test::build(sources[i], meshes[i]) || scene.addMeshletMesh(meshes[i]) != i) {
                return false;
            }
        }
        for (const geometry::MeshletMesh& m : meshes) {
            positions.push_back(visbuffer::decode_kernel::MeshPositions{m.positions.data(), m.vertex_count()});
        }
        return true;
    }
    const sw_kernel::SwSceneView& view() {
        storage.build(scene, meshes);
        return storage.view;
    }
    /// Classify records of every valid instance (32 meshlets each).
    std::vector<SwGroup> groups() const {
        std::vector<SwGroup> g;
        for (u32 i = 0; i < scene.instanceHighWater(); ++i) {
            const gpu_scene::GpuInstance inst = scene.instance(i);
            if ((inst.flags & gpu_scene::kInstanceValid) == 0u) {
                continue;
            }
            const u32 n = static_cast<u32>(meshes[inst.mesh].meshlets.size());
            for (u32 f = 0; f < n; f += kSwGroupSize) {
                g.push_back(SwGroup{i, f});
            }
        }
        return g;
    }
};

SwRasterConstants makeConstants(const Mat4& vp, u32 w, u32 h, u32 mode, f32 thresholdPx = 4.f, f32 clusterPx = 32.f) {
    SwRasterConstants c{};
    std::memcpy(c.viewProj, vp.m, sizeof(c.viewProj));
    c.width = w;
    c.height = h;
    c.mode = mode;
    swRasterThresholds(thresholdPx, clusterPx, c.triangleThreshold, c.maxClusterExtent);
    return c;
}

// --- layout ---------------------------------------------------------------------------------------
std::string readFile(const char* path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// "NAME <value>" (GLSL #define) or "NAME = <value>" (Slang static const): the value as an integer.
bool findConstant(const std::string& text, const std::string& name, long long& value) {
    usize at = 0;
    while ((at = text.find(name, at)) != std::string::npos) {
        usize p = at + name.size();
        if (p < text.size() && (std::isalnum(static_cast<unsigned char>(text[p])) != 0 || text[p] == '_')) {
            at = p;
            continue;
        }
        while (p < text.size() && (text[p] == ' ' || text[p] == '=')) {
            ++p;
        }
        if (p < text.size() && (text[p] == '-' || std::isdigit(static_cast<unsigned char>(text[p])) != 0)) {
            value = std::stoll(text.substr(p), nullptr, 0);
            return true;
        }
        at = p;
    }
    return false;
}

int runLayout() {
    expect(sizeof(SwRasterConstants) == 144u && sizeof(SwRasterPush) == 16u && sizeof(SwCluster) == 8u && sizeof(SwGroup) == 8u,
           "record sizes");
    expect(kSwCountClassify1 == kSwCountClassify0 + 3u && kSwCountSoftware1 == kSwCountSoftware0 + 3u &&
               kSwCountHardware1 == kSwCountHardware0 + 4u && kSwCountSwTriangles < kSwCountWords,
           "count word layout (dispatch x3, draw x4)");
    expect(kSwHwVertices == 372u && kSwMaxSpanSubpixels == 12288, "HW slots and span bound");
    // Integer ranges (swraster_kernel.hpp): |E| <= 2 * (span + 1 px)^2 < 2^31; z interpolation < 2^64.
    const f64 span = static_cast<f64>(kSwMaxSpanSubpixels + kSwSubpixelOne);
    expect(2.0 * span * span < 2147483648.0, "edge functions fit in 32 bits");
    expect(static_cast<f64>(kSwMaxClusterPixels) < static_cast<f64>(kSwMaxSpanPixels), "classifier bound < demotion bound");
    // Twins.
    struct Pair {
        const char* glsl;
        const char* slang;
        long long value;
    };
    const Pair pairs[] = {
        {"FUSE_SW_SUBPIXEL_BITS", "kFuseSwSubpixelBits", kSwSubpixelBits},
        {"FUSE_SW_SUBPIXEL_ONE", "kFuseSwSubpixelOne", kSwSubpixelOne},
        {"FUSE_SW_GROUP_SIZE", "kFuseSwGroupSize", kSwGroupSize},
        {"FUSE_SW_RASTER_THREADS", "kFuseSwRasterThreads", kSwRasterThreads},
        {"FUSE_SW_MAX_VERTICES", "kFuseSwMaxVertices", kSwMaxVertices},
        {"FUSE_SW_MAX_TRIANGLES", "kFuseSwMaxTriangles", kSwMaxTriangles},
        {"FUSE_SW_MAX_SPAN_SUBPIXELS", "kFuseSwMaxSpanSubpixels", kSwMaxSpanSubpixels},
        {"FUSE_SW_COORD_LIMIT", "kFuseSwCoordLimit", kSwCoordLimit},
        {"FUSE_SW_RESULT_SOFTWARE", "kFuseSwResultSoftware", kSwResultSoftware},
        {"FUSE_SW_RESULT_HARDWARE_FORCED", "kFuseSwResultHardwareForced", kSwResultHardwareForced},
        {"FUSE_SW_RESULT_OVERSIZE", "kFuseSwResultOversize", kSwResultOversize},
        {"FUSE_SW_MODE_FORCE_HARDWARE", "kFuseSwModeForceHardware", kSwModeForceHardware},
        {"FUSE_SW_FLAG_SKIP_HARDWARE", "kFuseSwFlagSkipHardware", kSwFlagSkipHardware},
        {"FUSE_SW_COUNT_HARDWARE0", "kFuseSwCountHardware0", kSwCountHardware0},
        {"FUSE_SW_COUNT_HW_REQUESTED0", "kFuseSwCountHwRequested0", kSwCountHwRequested0},
        {"FUSE_SW_COUNT_DEMOTED", "kFuseSwCountDemoted", kSwCountDemoted},
        {"FUSE_SW_COUNT_SW_TRIANGLES", "kFuseSwCountSwTriangles", kSwCountSwTriangles},
    };
    const std::string glsl = readFile(FUSE_SWRASTER_SHADER_DIR "/swraster_common.glsl");
    const std::string slang = readFile(FUSE_SWRASTER_SHADER_DIR "/swraster_common.slang");
    expect(!glsl.empty() && !slang.empty(), "shader twins readable");
    u32 bad = 0;
    for (const Pair& p : pairs) {
        long long a = -1, b = -1;
        if (!findConstant(glsl, p.glsl, a) || !findConstant(slang, p.slang, b) || a != p.value || b != p.value) {
            std::fprintf(stderr, "  constant %s / %s: C++ %lld, GLSL %lld, Slang %lld\n", p.glsl, p.slang, p.value, a, b);
            ++bad;
        }
    }
    expect(bad == 0u, "shader constants == C++");
    std::printf("layout: SwRasterConstants %zu B, push %zu B, %zu shader constants checked in both twins\n",
                sizeof(SwRasterConstants), sizeof(SwRasterPush), sizeof(pairs) / sizeof(pairs[0]));
    return 0;
}

// --- fixed ----------------------------------------------------------------------------------------
int runFixed() {
    std::mt19937 rng(54);
    std::uniform_real_distribution<f32> unit(-1.f, 1.f);
    std::uniform_int_distribution<int> expo(-30, 12);
    u32 checked = 0, skipped = 0, bad = 0, zChecked = 0, zSkipped = 0, zBad = 0;
    for (u32 i = 0; i < 500000u; ++i) {
        const f32 w = std::ldexp(0.5f + 0.5f * std::fabs(unit(rng)), expo(rng) / 4);
        const f32 x = unit(rng) * w * (i % 7u == 0u ? 40.f : 1.5f);
        const u32 scale = (64u + (i % 2048u)) << 7u;
        s64 q = 0;
        const bool ok = sw_kernel::ratio_round(x, w, scale, q);
        const f64 r = static_cast<f64>(x) * static_cast<f64>(scale) / static_cast<f64>(w) + 0.5;
        const f64 fl = std::floor(r);
        if (!ok) {
            bad += std::fabs(r) < 1e9 ? 1u : 0u;
            continue;
        }
        if (r - fl < 1e-6 || fl + 1.0 - r < 1e-6) {
            ++skipped;
            continue;
        }
        ++checked;
        if (static_cast<f64>(q) != fl) {
            if (bad < 4u) {
                std::fprintf(stderr, "  ratio_round(%.9g, %.9g, %u) = %lld, f64 %.3f\n", static_cast<f64>(x), static_cast<f64>(w), scale,
                             static_cast<long long>(q), r - 0.5);
            }
            ++bad;
        }
        // z / w in [0, 1].
        const f32 z = std::fabs(unit(rng)) * w;
        const u32 zq = sw_kernel::ratio_floor_z(z, w);
        const f64 zr = static_cast<f64>(z) * 4294967296.0 / static_cast<f64>(w);
        const f64 zf = std::min(std::floor(zr), 4294967295.0);
        if (zr - std::floor(zr) < 1e-5 || std::floor(zr) + 1.0 - zr < 1e-5) {
            ++zSkipped;
            continue;
        }
        ++zChecked;
        zBad += static_cast<f64>(zq) == zf ? 0u : 1u;
    }
    expect(bad == 0u, "ratio_round == f64 away from rounding boundaries");
    expect(zBad == 0u, "ratio_floor_z == f64 away from rounding boundaries");
    // Exact half-way cases: (k + 1/2) sub-pixels rounds up; -(k + 1/2) rounds towards +inf too.
    u32 halves = 0;
    for (u32 k = 0; k < 4096u; ++k) {
        const f32 num = (static_cast<f32>(k) + 0.5f) / 32768.f; // exact
        s64 up = 0, down = 0;
        const bool a = sw_kernel::ratio_round(num, 1.f, 256u << 7u, up);
        const bool b = sw_kernel::ratio_round(-num, 1.f, 256u << 7u, down);
        halves += (a && b && up == static_cast<s64>(k) + 1 && down == -static_cast<s64>(k)) ? 1u : 0u;
    }
    expect(halves == 4096u, "exact halves round up (floor(q + 1/2))");
    s64 q = 0;
    expect(!sw_kernel::ratio_round(1.f, 0.f, 32768u, q) && !sw_kernel::ratio_round(1.f, -1.f, 32768u, q) &&
               !sw_kernel::ratio_round(1.f, 1e-40f, 32768u, q) && !sw_kernel::ratio_round(1e30f, 1.f, 32768u, q),
           "ratio_round rejects den <= 0 / denormal / huge quotients");
    expect(sw_kernel::ratio_round(0.f, 1.f, 32768u, q) && q == 0 && sw_kernel::ratio_round(-0.f, 3.f, 32768u, q) && q == 0,
           "+-0 -> 0");
    expect(sw_kernel::ratio_floor_z(1.f, 1.f) == 0xFFFFFFFFu && sw_kernel::ratio_floor_z(0.f, 2.f) == 0u &&
               sw_kernel::ratio_floor_z(0.5f, 1.f) == 0x80000000u,
           "ratio_floor_z end points");
    // project_vertex: centre of a 256 x 192 target, w <= 0 / z outside [0, w] invalid.
    const SwVertex c = sw_kernel::project_vertex(visbuffer::decode_kernel::Clip{0.f, 0.f, 0.5f, 1.f}, 256u, 192u);
    expect(c.valid == 1u && c.x == 128 * 256 && c.y == 96 * 256 && c.z == 0x80000000u, "NDC origin -> target centre");
    const SwVertex corner = sw_kernel::project_vertex(visbuffer::decode_kernel::Clip{-2.f, 2.f, 0.f, 2.f}, 256u, 192u);
    expect(corner.valid == 1u && corner.x == 0 && corner.y == 192 * 256, "NDC (-1, 1) -> (0, height)");
    expect(sw_kernel::project_vertex({0.f, 0.f, 0.5f, 0.f}, 256u, 192u).valid == 0u &&
               sw_kernel::project_vertex({0.f, 0.f, -0.1f, 1.f}, 256u, 192u).valid == 0u &&
               sw_kernel::project_vertex({0.f, 0.f, 1.1f, 1.f}, 256u, 192u).valid == 0u,
           "behind / outside the depth range -> invalid");
    std::printf("fixed: ratio_round %u checked (%u within 1e-6 of a boundary skipped), ratio_floor_z %u checked (%u skipped), "
                "%u exact halves\n",
                checked, skipped, zChecked, zSkipped, halves);
    return 0;
}

// --- fill -----------------------------------------------------------------------------------------
constexpr u32 kFW = 48;
constexpr u32 kFH = 40;

SwVertex sv(s32 x, s32 y, u32 z = 0x40000000u) {
    SwVertex v{};
    v.x = x;
    v.y = y;
    v.z = z;
    v.valid = 1u;
    return v;
}

void rasterCount(const SwVertex& a, const SwVertex& b, const SwVertex& c, std::vector<u32>& count) {
    sw_kernel::raster_triangle(a, b, c, kFW, kFH, 0u, 0u, [&](u32 pixel, u64) { ++count[pixel]; });
}

int runFill() {
    std::mt19937 rng(7);
    // Vertices on pixel centres or on a coarse sub-pixel lattice: many centres lie exactly on edges.
    u32 quads = 0, quadBad = 0, fans = 0, fanBad = 0, windBad = 0, edgeHits = 0;
    for (u32 iter = 0; iter < 2000u; ++iter) {
        const bool centres = (iter & 1u) == 0u;
        // Convex quad: 4 points around a centre at increasing angles.
        const f64 cx = 24.0 * 256.0, cy = 20.0 * 256.0;
        SwVertex q[6];
        u32 n = 4u + iter % 3u;
        f64 base = std::uniform_real_distribution<f64>(0.0, 6.28)(rng);
        for (u32 k = 0; k < n; ++k) {
            const f64 ang = base + 6.2831853 * static_cast<f64>(k) / n;
            const f64 rad = 256.0 * (6.0 + 10.0 * std::uniform_real_distribution<f64>(0.0, 1.0)(rng));
            s32 x = static_cast<s32>(cx + rad * std::cos(ang));
            s32 y = static_cast<s32>(cy + rad * std::sin(ang));
            if (centres) {
                x = (x >> 8) * 256 + 128;
                y = (y >> 8) * 256 + 128;
            } else {
                x &= ~63;
                y &= ~63;
            }
            q[k] = sv(x, y);
        }
        // Fans from every apex must give identical coverage, each pixel at most once.
        std::vector<u32> first;
        bool convexOk = true;
        u32 doubles = 0;
        for (u32 apex = 0; apex < n; ++apex) {
            std::vector<u32> count(kFW * kFH, 0u);
            for (u32 k = 1; k + 1 < n; ++k) {
                rasterCount(q[apex], q[(apex + k) % n], q[(apex + k + 1) % n], count);
            }
            for (u32 v : count) {
                doubles += v > 1u ? 1u : 0u;
            }
            std::vector<u32> cov(count.size());
            for (usize i = 0; i < count.size(); ++i) {
                cov[i] = count[i] != 0u ? 1u : 0u;
            }
            if (apex == 0u) {
                first = cov;
            } else if (cov != first) {
                convexOk = false;
            }
        }
        // The rounding to the lattice can make the polygon non-convex; only count convex ones.
        bool convex = true;
        for (u32 k = 0; k < n; ++k) {
            const SwVertex& a = q[k];
            const SwVertex& b = q[(k + 1) % n];
            const SwVertex& c = q[(k + 2) % n];
            const s64 cr = static_cast<s64>(b.x - a.x) * (c.y - b.y) - static_cast<s64>(b.y - a.y) * (c.x - b.x);
            convex = convex && cr > 0;
        }
        if (convex) {
            ++(n == 4u ? quads : fans);
            if (!convexOk || doubles != 0u) {
                ++(n == 4u ? quadBad : fanBad);
            }
        }
        // Both windings rasterise the same set.
        std::vector<u32> a(kFW * kFH, 0u), b(kFW * kFH, 0u);
        rasterCount(q[0], q[1], q[2], a);
        rasterCount(q[0], q[2], q[1], b);
        windBad += a == b ? 0u : 1u;
        // Edge hits: centres with E == 0 on some edge of the first triangle (statistic).
        for (u32 py = 0; py < kFH; ++py) {
            for (u32 px = 0; px < kFW; ++px) {
                const s64 x = px * 256 + 128, y = py * 256 + 128;
                for (u32 e = 0; e < 3u; ++e) {
                    const SwVertex& u = q[e];
                    const SwVertex& v = q[(e + 1) % 3u];
                    edgeHits += static_cast<s64>(v.x - u.x) * (y - u.y) - static_cast<s64>(v.y - u.y) * (x - u.x) == 0 ? 1u : 0u;
                }
            }
        }
    }
    expect(quads > 100u && fans > 100u && edgeHits > 1000u, "enough convex quads / fans with exact edge hits");
    expect(quadBad == 0u && fanBad == 0u, "shared edges: every centre exactly once (watertight, no double coverage)");
    expect(windBad == 0u, "both windings cover the same pixels");
    // Explicit rule: axis-aligned square with corners on pixel centres (2,2)..(6,6):
    // top row (y = 2) and left column (x = 2) in, bottom row (y = 6) and right column (x = 6) out.
    {
        std::vector<u32> count(kFW * kFH, 0u);
        const s32 lo = 2 * 256 + 128, hi = 6 * 256 + 128;
        rasterCount(sv(lo, lo), sv(hi, lo), sv(hi, hi), count);
        rasterCount(sv(lo, lo), sv(hi, hi), sv(lo, hi), count);
        u32 inside = 0, wrong = 0;
        for (u32 y = 0; y < kFH; ++y) {
            for (u32 x = 0; x < kFW; ++x) {
                const bool want = x >= 2u && x < 6u && y >= 2u && y < 6u;
                inside += want ? 1u : 0u;
                wrong += (count[y * kFW + x] == (want ? 1u : 0u)) ? 0u : 1u;
            }
        }
        expect(inside == 16u && wrong == 0u, "top-left rule: top / left edges in, bottom / right edges out");
    }
    // Depth: a centre exactly on a vertex gets that vertex's z (>> 8).
    {
        u64 word = ~0ull;
        const s32 c0 = 10 * 256 + 128;
        sw_kernel::raster_triangle(sv(c0, c0, 0x12345678u), sv(c0 + 2048, c0, 0x7FFFFFFFu), sv(c0, c0 + 2048, 0x10000000u), kFW, kFH,
                                   3u, 9u, [&](u32 pixel, u64 w) {
                                       if (pixel == 10u * kFW + 10u) {
                                           word = w;
                                       }
                                   });
        expect(visbuffer::vis64_depth_bits(word) == (0x12345678u >> 8u) && visbuffer::vis64_instance(word) == 3u &&
                   visbuffer::vis64_triangle(word) == 9u,
               "depth exact at a vertex; ids packed");
    }
    std::printf("fill: %u convex quads, %u convex fans (4..6 apexes each), %u exact edge hits, 0 double / missing coverage\n", quads,
                fans, edgeHits);
    return 0;
}

// --- raster ---------------------------------------------------------------------------------------
int runRaster() {
    TestScene t;
    expect(t.init(), "scene");
    constexpr u32 kW = 256, kH = 192;
    std::mt19937 rng(99);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 i = 0; i < 300u; ++i) {
        gpu_scene::InstanceDesc d{};
        d.mesh = i % 3u;
        const f32 z = -30.f - 40.f * (u(rng) * 0.5f + 0.5f);
        d.transform = place(u(rng) * 0.55f * -z, u(rng) * 0.4f * -z, z, 0.5f + 0.4f * (u(rng) * 0.5f + 0.5f), u(rng) * 3.f);
        t.scene.addInstance(d);
    }
    const Mat4 vp = camera(static_cast<f32>(kW) / kH);
    const sw_kernel::SwSceneView& view = t.view();
    // Every meshlet of every instance, straight to the SW kernel.
    std::vector<SwCluster> clusters;
    for (const SwGroup& g : t.groups()) {
        const u32 n = static_cast<u32>(t.meshes[t.scene.instance(g.instance).mesh].meshlets.size());
        for (u32 m = g.firstMeshlet; m < n && m < g.firstMeshlet + kSwGroupSize; ++m) {
            clusters.push_back(SwCluster{g.instance, m});
        }
    }
    const SwRasterConstants c = makeConstants(vp, kW, kH, kSwModeForceSoftware);
    std::vector<u64> words, wordsPar;
    std::vector<SwCluster> demoted, demotedPar;
    SwRasterReferenceStats st{};
    swraster_raster_reference(view, c, {clusters.data(), static_cast<u32>(clusters.size())}, words, demoted, &st);
    swraster_raster_reference(view, c, {clusters.data(), static_cast<u32>(clusters.size())}, wordsPar, demotedPar, nullptr,
                              fuse::kernel::Backend::CpuParallel);
    expect(words == wordsPar, "CpuReference == CpuParallel (atomicMin order independent)");
    expect(demoted.empty(), "no cluster of the far scene is demoted");
    // Against the WP-1.4 f64 reference on robust pixels.
    std::vector<visbuffer::RasterRefPixel> ref;
    visbuffer::RasterRefStats rs{};
    visbuffer::raster_reference(visbuffer::vis_scene_view(t.scene, t.positions), vp.m, kW, kH, visbuffer::RasterRefOptions{}, ref,
                                &rs);
    // Depth: the SW plane goes through the SNAPPED vertices (1/256 px), the reference's through the
    // exact ones; on these far, sloped surfaces that moves the depth by a few 2^-24 quanta.
    constexpr s64 kDepthQuanta = 64;
    u32 robust = 0, robustCovered = 0, bad = 0, covered = 0, depthBad = 0;
    s64 depthMax = 0;
    for (u32 p = 0; p < kW * kH; ++p) {
        covered += visbuffer::vis64_valid(words[p]) ? 1u : 0u;
        if (ref[p].robust == 0u) {
            continue;
        }
        ++robust;
        const visbuffer::VisSample s = visbuffer::vis64_unpack(words[p]);
        robustCovered += ref[p].instance != visbuffer::kVisInvalid ? 1u : 0u;
        if (s.instance != ref[p].instance || s.triangle != ref[p].triangle) {
            if (bad < 4u) {
                std::fprintf(stderr, "  pixel (%u, %u): SW (%u, %u), reference (%u, %u)\n", p % kW, p / kW, s.instance, s.triangle,
                             ref[p].instance, ref[p].triangle);
            }
            ++bad;
        }
        // Depth of the same surface: the reference's f64 depth quantised within 1 quantum.
        if (s.instance == ref[p].instance && s.instance != visbuffer::kVisInvalid) {
            const s64 q = static_cast<s64>(visbuffer::vis64_depth_bits(words[p]));
            const s64 r = static_cast<s64>(std::floor(static_cast<f64>(ref[p].depth) * 16777216.0));
            const s64 diff = q > r ? q - r : r - q;
            depthMax = std::max(depthMax, diff);
            depthBad += diff > kDepthQuanta ? 1u : 0u;
        }
    }
    expect(robustCovered > 2000u, "enough robust covered pixels");
    expect(bad == 0u, "SW reference == the WP-1.4 f64 raster reference on every robust pixel (ids)");
    expect(depthBad == 0u, "SW depth within kDepthQuanta of the f64 reference (same surface)");
    std::printf("raster: %zu clusters, %u triangles, %u covered pixels; %u robust (%u covered) pixels == f64 reference, "
                "depth within %lld quanta, %zu demoted\n",
                clusters.size(), st.triangles, covered, robust, robustCovered, static_cast<long long>(depthMax), demoted.size());
    return 0;
}

// --- classify -------------------------------------------------------------------------------------
int runClassify() {
    TestScene t;
    expect(t.init(), "scene");
    constexpr u32 kW = 256, kH = 192;
    std::mt19937 rng(5);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    auto add = [&](u32 mesh, const gpu_scene::GpuTransform& xf) {
        gpu_scene::InstanceDesc d{};
        d.mesh = mesh;
        d.transform = xf;
        t.scene.addInstance(d);
    };
    for (u32 i = 0; i < 200u; ++i) { // far: micro triangles
        const f32 z = -25.f - 50.f * (u(rng) * 0.5f + 0.5f);
        add(i % 3u, place(u(rng) * 0.5f * -z, u(rng) * 0.35f * -z, z, 0.6f, u(rng) * 3.f));
    }
    add(0, place(0.f, 0.f, -3.f, 1.f));          // near, large: HW extent
    add(1, place(1.f, 0.3f, -0.6f, 1.f));        // crossing the near plane: HW clip
    add(0, place(0.f, 0.f, 20.f, 1.f));          // behind the camera: culled
    add(2, place(60.f, 0.f, -10.f, 1.f));        // off-screen right: culled
    add(0, place(-2.f, -1.f, -12.f, 1.f, 0.4f)); // mid distance: mixed
    const Mat4 vp = camera(static_cast<f32>(kW) / kH);
    const sw_kernel::SwSceneView& view = t.view();
    const std::vector<SwGroup> groups = t.groups();
    const fuse::kernel::Span<const SwGroup> gs{groups.data(), static_cast<u32>(groups.size())};
    std::vector<u32> classify, classifyPar, force, hard;
    const SwRasterConstants cc = makeConstants(vp, kW, kH, kSwModeClassify, 2.f);
    swraster_classify_reference(view, cc, gs, classify);
    swraster_classify_reference(view, cc, gs, classifyPar, fuse::kernel::Backend::CpuParallel);
    swraster_classify_reference(view, makeConstants(vp, kW, kH, kSwModeForceSoftware, 2.f), gs, force);
    swraster_classify_reference(view, makeConstants(vp, kW, kH, kSwModeForceHardware, 2.f), gs, hard);
    expect(classify == classifyPar, "CpuReference == CpuParallel");
    u32 hist[kSwResultCount] = {};
    u32 notSubset = 0, hardSw = 0, cullDiff = 0, outside = 0, swClusters = 0, spanMax = 0;
    for (usize i = 0; i < classify.size(); ++i) {
        ++hist[classify[i] < kSwResultCount ? classify[i] : 0u];
        notSubset += classify[i] == kSwResultSoftware && force[i] != kSwResultSoftware ? 1u : 0u;
        hardSw += hard[i] == kSwResultSoftware ? 1u : 0u;
        cullDiff += (classify[i] == kSwResultCulled) != (hard[i] == kSwResultCulled) ? 1u : 0u;
        if (force[i] != kSwResultSoftware) {
            continue;
        }
        // Every vertex of an SW-safe cluster projects validly, the cluster spans <= the demotion bound.
        const SwGroup g = groups[i / kSwGroupSize];
        const u32 m = g.firstMeshlet + static_cast<u32>(i % kSwGroupSize);
        const gpu_scene::GpuInstance inst = t.scene.instance(g.instance);
        const gpu_scene::GpuMesh& mesh = view.meshes[inst.mesh];
        const sw_kernel::SwMeshGeometry& geo = view.geometry[inst.mesh];
        const gpu_scene::GpuMeshlet& ml = geo.meshlets[m];
        s32 minX = 0x7FFFFFFF, maxX = -0x7FFFFFFF, minY = 0x7FFFFFFF, maxY = -0x7FFFFFFF;
        for (u32 v = 0; v < sw_kernel::meshlet_vertex_count(ml); ++v) {
            visbuffer::decode_kernel::Clip clip{};
            const bool ok = sw_kernel::meshlet_vertex_clip(mesh, geo, ml, v, view.transforms[g.instance], cc.viewProj, clip);
            const SwVertex sv0 = sw_kernel::project_vertex(clip, kW, kH);
            if (!ok || sv0.valid == 0u) {
                ++outside;
                continue;
            }
            minX = std::min(minX, sv0.x);
            maxX = std::max(maxX, sv0.x);
            minY = std::min(minY, sv0.y);
            maxY = std::max(maxY, sv0.y);
        }
        ++swClusters;
        spanMax = std::max<u32>(spanMax, static_cast<u32>(std::max(maxX - minX, maxY - minY)));
    }
    std::printf("classify: %zu meshlet slots: none %u, culled %u, SW %u, HW size %u, HW extent %u, HW clip %u, oversize %u; "
                "ForceSoftware %u SW clusters, largest vertex span %.2f px\n",
                classify.size(), hist[kSwResultNone], hist[kSwResultCulled], hist[kSwResultSoftware], hist[kSwResultHardwareSize],
                hist[kSwResultHardwareExtent], hist[kSwResultHardwareClip], hist[kSwResultOversize], swClusters,
                static_cast<f64>(spanMax) / 256.0);
    expect(hist[kSwResultCulled] > 0u && hist[kSwResultSoftware] > 0u && hist[kSwResultHardwareSize] > 0u &&
               hist[kSwResultHardwareExtent] > 0u && hist[kSwResultHardwareClip] > 0u,
           "every class occurs");
    expect(notSubset == 0u, "ForceSoftware's SW set contains Classify's");
    expect(hardSw == 0u && cullDiff == 0u, "ForceHardware: no SW cluster, the same culled set");
    expect(outside == 0u && spanMax <= kSwMaxClusterPixels * 256u + 256u, "SW clusters' vertices project inside the rect bound");
    // Thresholds.
    u32 a = 0, b = 0;
    swRasterThresholds(4.f, 32.f, a, b);
    expect(a == 1024u && b == 8192u, "4 px / 32 px -> 1024 / 8192 sub-pixels");
    swRasterThresholds(1000.f, 1000.f, a, b);
    expect(a == 64u * 256u && b == kSwMaxClusterPixels * 256u, "clamped to 64 px / kSwMaxClusterPixels");
    swRasterThresholds(-1.f, std::nanf(""), a, b);
    expect(a == 0u && b == 0u, "negative / NaN -> 0");
    return 0;
}

// --- api ------------------------------------------------------------------------------------------
int runApi() {
    SwRasterizer sw;
    SwRasterDesc d{};
    expect(!sw.init(d) && !sw.valid(), "init fails without a device");
    expect(!querySwRasterCapabilities(nullptr).usable, "no capabilities without a device");
    SwRasterFrameDesc f{};
    culling::InstanceCuller culler;
    expect(!sw.beginFrame(1, f, culler), "beginFrame fails when not initialised");
    fuse::renderer::rg::Graph graph;
    const SwRasterGraphRefs refs = sw.importInto(graph);
    expect(!refs.counts.valid(), "no imports when not initialised");
    std::printf("api: init / beginFrame / importInto fail cleanly without a device (%s)\n", querySwRasterCapabilities(nullptr).reason);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "layout" || suite == "all") {
        runLayout();
    }
    if (suite == "fixed" || suite == "all") {
        runFixed();
    }
    if (suite == "fill" || suite == "all") {
        runFill();
    }
    if (suite == "raster" || suite == "all") {
        runRaster();
    }
    if (suite == "classify" || suite == "all") {
        runClassify();
    }
    if (suite == "api" || suite == "all") {
        runApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
