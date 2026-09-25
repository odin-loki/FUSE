// WP-6.3 screen-space fallback: CPU gates (no device; also run in the stub tree).
//
//   layout         SsfxFrameConstants / SsfxPush sizes; the GLSL and Slang mirrors of SsfxFrameConstants
//                  (struct SxFrame in shaders/ssfx/sx_common.*) have the same fields, order and offsets;
//                  SsfxBufferLayout sections are 256-aligned, disjoint and inside the buffer.
//   gtao_analytic  the new GTAO kernel against analytic references (camera + G-buffer from test_rp_ssfx_gpu_scene):
//                  a plane (fronto-parallel and oblique, jitter on / off) gives exactly 1; at the crease of a
//                  dihedral wedge (a corner / crease; alpha = 60, 90, 120, 150 degrees) GTAO tends to the analytic
//                  (1 - cos alpha) / 2 (pixels next to the crease and the d -> 0 limit); the error falls with the
//                  slice count; GTAO agrees with the HBAO oracle and with the brute-force hemisphere
//                  reference (ssaoHemisphereReferenceVisibility) on the room scene; the polynomial acos / cos
//                  are within 1e-6 of libm.
//   gtao_parity    CpuReference == CpuParallel bit for bit (0, 2, 4 workers, odd extent), kernel stats
//                  ("screen_space_gtao", item count), a GPU request without a device falls back to CpuParallel.
//   reference      prepare_pixel (linear depth == the WP-2.1 shade's formula, octahedral decode within 1e-3 rad
//                  of the analytic normal, the view / ssfx convention), compose_pixel rules (AO re-weights only
//                  the ambient term, SSR x confidence x Schlick, SSGI added, sky untouched), reference_frame
//                  CpuReference == CpuParallel.
//   api            resolve_constants (flags, clamps, slice table, camera intrinsics), stub / no-device behaviour.
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/ssfx/gtao_kernel.hpp>
#include <fuse/ssfx/hbao.hpp>
#include <fuse/ssfx/hbao_kernel.hpp>

#include "test_rp_ssfx_gpu_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::ssfx_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace kernel = fuse::kernel;
namespace ssfx = fuse::ssfx;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define SX_FIELD(n) Field{#n, offsetof(SsfxFrameConstants, n)}
const Field kFields[] = {
    SX_FIELD(prepared), SX_FIELD(normals), SX_FIELD(radiance), SX_FIELD(albedo), SX_FIELD(diffuse), SX_FIELD(ao),
    SX_FIELD(ssr), SX_FIELD(gi), SX_FIELD(bounce0), SX_FIELD(bounce1), SX_FIELD(dump), SX_FIELD(sky),
    SX_FIELD(width), SX_FIELD(height), SX_FIELD(inputDepth), SX_FIELD(inputNormal), SX_FIELD(inputAlbedo),
    SX_FIELD(inputRoughMetal), SX_FIELD(inputLit), Field{"output_", offsetof(SsfxFrameConstants, output)},
    SX_FIELD(flags), SX_FIELD(reserved1), SX_FIELD(fx), SX_FIELD(fy), SX_FIELD(cx), SX_FIELD(cy), SX_FIELD(nearZ),
    SX_FIELD(nearPlane), SX_FIELD(farPlane), SX_FIELD(reserved2), SX_FIELD(viewRot), SX_FIELD(ambient),
    SX_FIELD(aoRadius), SX_FIELD(aoFalloff), SX_FIELD(aoBias), SX_FIELD(aoStrength), SX_FIELD(aoMaxRadiusPx),
    SX_FIELD(aoSlices), SX_FIELD(aoSteps), SX_FIELD(aoFrame), SX_FIELD(aoSliceCos), SX_FIELD(aoSliceSin),
    SX_FIELD(ssrMaxSteps), SX_FIELD(ssrRefineSteps), SX_FIELD(ssrStride), SX_FIELD(ssrThickness),
    SX_FIELD(ssrMaxDistance), SX_FIELD(ssrFadeEdge), SX_FIELD(ssrContactDistance), SX_FIELD(ssrContactFloor),
    SX_FIELD(ssrContactExponent), SX_FIELD(reserved3), SX_FIELD(giSampleSqrt), SX_FIELD(giBounces),
    SX_FIELD(giMaxSteps), SX_FIELD(giRefineSteps), SX_FIELD(giStride), SX_FIELD(giThickness), SX_FIELD(giMaxDistance),
    SX_FIELD(giIntensity),
};
#undef SX_FIELD

/// Parses `struct SxFrame { ... };` of a shader source: (name, std430 / scalar offset) per field.
bool parseShaderStruct(const std::string& path, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const size_t begin = text.find("struct SxFrame {");
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + 16, end - begin - 16));
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

void testLayout() {
    expect(sizeof(SsfxFrameConstants) == 592u, "SsfxFrameConstants is 592 bytes");
    expect(sizeof(SsfxPush) == 32u, "SsfxPush is 32 bytes (<= 128 guaranteed push-constant bytes)");
    expect(kSsfxMaxSlices == ssfx::gtao_kernel::kMaxSlices, "slice table size == gtao_kernel::kMaxSlices");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_SSFX_SHADER_DIR) + "/sx_common." + lang;
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, offsets, size, names);
        expect(parsed, "sx_common struct SxFrame parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == sizeof(SsfxFrameConstants);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(),
                             offsets[i], kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: sx_common.%s SxFrame %zu fields, %zu bytes\n", lang, offsets.size(), size);
        expect(same, "shader SxFrame == SsfxFrameConstants (names, order, offsets, size)");
    }
    for (const auto& extent : {std::pair<u32, u32>{1u, 1u}, {61u, 37u}, {256u, 192u}, {1920u, 1080u}}) {
        const u32 w = extent.first;
        const u32 h = extent.second;
        const SsfxBufferLayout l = SsfxBufferLayout::compute(w, h);
        const u64 n = static_cast<u64>(w) * h;
        struct Section {
            u64 at, bytes;
        };
        const Section sections[] = {{l.prepared, n * 16}, {l.normals, n * 16}, {l.radiance, n * 16}, {l.albedo, n * 16},
                                    {l.diffuse, n * 16},  {l.ao, n * 4},       {l.ssr, n * 16},      {l.gi, n * 16},
                                    {l.bounce[0], n * 16}, {l.bounce[1], n * 16}};
        bool ok = true;
        const usize count = sizeof(sections) / sizeof(sections[0]);
        for (usize i = 0; i < count; ++i) {
            ok = ok && sections[i].at % 256u == 0u && sections[i].at + sections[i].bytes <= l.workBytes;
            for (usize j = i + 1; j < count; ++j) {
                ok = ok && (sections[i].at + sections[i].bytes <= sections[j].at ||
                            sections[j].at + sections[j].bytes <= sections[i].at);
            }
        }
        expect(ok, "work sections are 256-aligned, disjoint and inside the buffer");
        std::printf("layout: %ux%u work buffer %.2f MiB (%.1f B / pixel)\n", w, h,
                    static_cast<f64>(l.workBytes) / (1024.0 * 1024.0), static_cast<f64>(l.workBytes) / static_cast<f64>(n));
    }
}

// --- helpers -----------------------------------------------------------------------------------------
struct CpuFrame {
    SsfxFrameConstants c{};
    SsfxPreparedFrame prepared;
    std::vector<f32> depth;
    std::vector<Vec3> normals;
    ssfx::SsfxGBufferView view{};
};

bool makeFrame(const ssfx_test::GBuffer& g, const ssfx_test::Camera& cam, const SsfxGpuSettings& settings,
               const f32 (&ambient)[3], CpuFrame& f) {
    if (!resolve_constants(settings, ssfx_test::cameraDesc(cam), ambient, g.width, g.height, f.c)) {
        return false;
    }
    ssfx_test::prepareCpu(f.c, g, f.prepared);
    const usize n = static_cast<usize>(g.width) * g.height;
    f.depth.resize(n);
    for (usize i = 0; i < n; ++i) {
        f.depth[i] = f.prepared.prepared[i].x;
    }
    f.normals = f.prepared.normal;
    f.view = prepared_view(f.c, f.depth, f.normals);
    return true;
}

// --- gtao_analytic ---------------------------------------------------------------------------------------
void testPolynomials() {
    f64 acosErr = 0.0;
    f64 cosErr = 0.0;
    for (u32 i = 0; i <= 200000u; ++i) {
        const f32 x = -1.f + 2.f * static_cast<f32>(i) / 200000.f;
        acosErr = std::max(acosErr, std::fabs(static_cast<f64>(ssfx::gtao_kernel::acos_poly(x)) - std::acos(static_cast<f64>(x))));
        const f32 a = -6.28f + 12.56f * static_cast<f32>(i) / 200000.f;
        cosErr = std::max(cosErr, std::fabs(static_cast<f64>(ssfx::gtao_kernel::cos_poly(a)) - std::cos(static_cast<f64>(a))));
    }
    std::printf("gtao polynomials: max |acos_poly - acos| = %.3g on [-1, 1], max |cos_poly - cos| = %.3g on [-2pi, 2pi]\n",
                acosErr, cosErr);
    expect(acosErr <= 1e-6 && cosErr <= 1e-6, "polynomial acos / cos within 1e-6 of libm");
}

void testPlanes() {
    const f32 ambient[3] = {0.3f, 0.3f, 0.3f};
    f32 worst = 0.f;
    u32 pixels = 0;
    for (const f32 lateral : {0.f, 1.5f, 3.f}) {
        ssfx_test::WedgeScene plane;
        ssfx_test::buildWedge(plane, 96, 72, 180.f, 70.f, 3.f, 20.f, lateral);
        for (const bool jitter : {false, true}) {
            SsfxGpuSettings s{};
            s.gtao.radius = 2.f;
            s.gtao.slices = 8;
            s.gtao.steps = 12;
            s.gtao.falloff = 0.3f;
            s.gtao.jitter = jitter;
            s.gtao.frame = 7;
            CpuFrame f;
            expect(makeFrame(plane.g, plane.camera, s, ambient, f), "plane frame");
            std::vector<f32> ao(static_cast<usize>(96) * 72, -1.f);
            expect(computeGtaoCpu(f.view, s.gtao, ao.data()), "GTAO on a plane");
            for (usize i = 0; i < ao.size(); ++i) {
                if (plane.face[i] != 0u) {
                    worst = std::max(worst, std::fabs(1.f - ao[i]));
                    ++pixels;
                }
            }
        }
    }
    std::printf("gtao plane: %u pixels (fronto-parallel + 2 oblique views, jitter off / on): max |1 - AO| = %.3g\n",
                pixels, static_cast<f64>(worst));
    expect(pixels > 10000u && worst <= 1e-5f, "a plane gives AO 1 (within 1e-5)");
}

/// GTAO on a wedge of opening `alphaDeg`, binned by the pixel's distance to the crease in pixel footprints
/// (the camera is 3 m from the crease, 90 degree vertical field of view). A horizon-based estimate on a finite
/// screen sees face B only up to the screen edge (and up to the radius), so its error grows with the distance
/// d to the crease (~ d / screen extent) and vanishes at the crease: the gate is on the pixels nearest the
/// crease and on the d -> 0 limit of a least-squares line through the pixels within 4 footprints.
struct WedgeResult {
    f64 nearMean = 0.0;   ///< pixels within 1 footprint of the crease
    u32 nearCount = 0;
    f64 intercept = 0.0;  ///< AO extrapolated to d = 0 (least squares over d <= 4 footprints)
    f64 slope = 0.0;      ///< per footprint
    f64 hbaoNearMean = 0.0;
};

/// Every statistic below reads only the pixels within 4 footprints of the crease (a ~3 % band of the frame), so
/// only those are evaluated: per pixel, through the same gtao_kernel::pixel_visibility / hbaoPixelVisibility that
/// the full-frame launches run (bit-identical; `checkLaunch` re-proves it against computeGtaoCpu). Evaluating the
/// full 256^2 frame with 32 slices x 192 steps plus the 64-direction HBAO oracle took ~45 s per wedge in Debug.
WedgeResult wedgeAo(f32 alphaDeg, u32 slices, u32 size, bool withHbao, bool checkLaunch = false) {
    ssfx_test::WedgeScene w;
    ssfx_test::buildWedge(w, size, size, alphaDeg, 90.f, 3.f, 25.f);
    SsfxGpuSettings s{};
    s.gtao.radius = 1000.f;
    s.gtao.max_radius_px = 512.f;
    s.gtao.slices = slices;
    s.gtao.steps = 192;
    const f32 ambient[3] = {0.f, 0.f, 0.f};
    CpuFrame f;
    makeFrame(w.g, w.camera, s, ambient, f);
    const f64 foot = 2.0 * 3.0 / static_cast<f64>(size);
    const auto inBand = [&](usize i) { return w.face[i] != 0u && w.creaseDistance[i] / foot <= 4.0; };
    ssfx::HbaoParams hp{};
    hp.radius = 1000.f;
    hp.bias = 0.02f;
    hp.directions = 2u * slices;
    hp.steps_per_dir = 192;
    hp.max_radius_px = 512.f;
    const ssfx::gtao_kernel::Params gp = ssfx::gtao_kernel::make_params(f.view, s.gtao, nullptr);
    std::vector<f32> ao(static_cast<usize>(size) * size, -1.f);
    std::vector<f32> hbao;
    if (withHbao) {
        hbao.assign(ao.size(), -1.f);
    }
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const usize i = static_cast<usize>(y) * size + x;
            if (!inBand(i)) {
                continue;
            }
            ao[i] = ssfx::gtao_kernel::pixel_visibility(gp, x, y);
            if (withHbao) {
                hbao[i] = ssfx::hbaoPixelVisibility(f.view, hp, x, y);
            }
        }
    }
    if (checkLaunch) {
        std::vector<f32> full(ao.size(), -1.f);
        expect(computeGtaoCpu(f.view, s.gtao, full.data()), "wedge: full-frame GTAO launch");
        bool same = true;
        for (usize i = 0; i < ao.size(); ++i) {
            if (inBand(i) && std::memcmp(&ao[i], &full[i], sizeof(f32)) != 0) {
                same = false;
            }
        }
        expect(same, "wedge: per-pixel GTAO == the full-frame launch bit for bit on the crease band");
    }
    WedgeResult r{};
    f64 sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    u32 n = 0;
    f64 hsum = 0.0;
    for (usize i = 0; i < ao.size(); ++i) {
        if (w.face[i] == 0u) {
            continue;
        }
        const f64 d = w.creaseDistance[i] / foot;
        if (d < 1.0) {
            r.nearMean += ao[i];
            hsum += hbao.empty() ? 0.0 : hbao[i];
            ++r.nearCount;
        }
        if (d <= 4.0) {
            sx += d;
            sy += ao[i];
            sxx += d * d;
            sxy += d * ao[i];
            ++n;
        }
    }
    if (r.nearCount > 0u) {
        r.nearMean /= r.nearCount;
        r.hbaoNearMean = hsum / r.nearCount;
    }
    if (n > 1u) {
        r.slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        r.intercept = (sy - r.slope * sx) / n;
    }
    return r;
}

void testWedges() {
    for (const f32 alpha : {60.f, 90.f, 120.f, 150.f}) {
        const f64 expected = ssfx_test::wedgeVisibility(alpha);
        const WedgeResult r = wedgeAo(alpha, 32, 256, true);
        std::printf("gtao wedge alpha %5.1f: analytic (1 - cos a) / 2 = %.4f; GTAO (32 slices, 256^2) within 1 footprint "
                    "of the crease %.4f (%u px), d -> 0 limit %.4f (slope %.4f / footprint); HBAO oracle %.4f\n",
                    static_cast<f64>(alpha), expected, r.nearMean, r.nearCount, r.intercept, r.slope, r.hbaoNearMean);
        expect(r.nearCount >= 256u, "wedge: pixels next to the crease");
        expect(std::fabs(r.intercept - expected) <= 0.015, "wedge: GTAO d -> 0 limit within 0.015 of (1 - cos alpha) / 2");
        expect(std::fabs(r.nearMean - expected) <= 0.02, "wedge: GTAO next to the crease within 0.02 of (1 - cos alpha) / 2");
        expect(r.slope >= 0.0, "wedge: the error grows away from the crease (screen / horizon truncation)");
        expect(std::fabs(r.hbaoNearMean - r.nearMean) <= 0.02, "wedge: GTAO agrees with the HBAO oracle (within 0.02)");
    }
    // Slice quadrature convergence (90 degrees, 192^2).
    f64 first = 0.0;
    f64 last = 0.0;
    for (const u32 slices : {2u, 4u, 8u, 16u, 32u}) {
        const WedgeResult r = wedgeAo(90.f, slices, 192, false, /*checkLaunch=*/slices == 2u);
        const f64 err = std::fabs(r.intercept - 0.5);
        std::printf("gtao wedge 90 (192^2): %2u slices, d -> 0 limit %.4f |err| %.4f\n", slices, r.intercept, err);
        if (slices == 2u) {
            first = err;
        }
        last = err;
    }
    expect(last <= first && last <= 0.015, "wedge: 32 slices at least as close as 2 slices, within 0.015");
}

void testRoomVsOracles() {
    ssfx_test::RoomScene room;
    ssfx_test::buildRoom(room, 96, 72);
    SsfxGpuSettings s{};
    s.gtao.radius = 1.f;
    s.gtao.slices = 16;
    s.gtao.steps = 16;
    s.gtao.max_radius_px = 64.f;
    CpuFrame f;
    expect(makeFrame(room.g, room.camera, s, room.ambient, f), "room frame");
    const usize n = f.depth.size();
    std::vector<f32> gtao(n, -1.f);
    std::vector<f32> hbao(n, -1.f);
    expect(computeGtaoCpu(f.view, s.gtao, gtao.data()), "room GTAO");
    ssfx::HbaoParams hp{};
    hp.radius = 1.f;
    hp.bias = 0.02f;
    hp.directions = 32;
    hp.steps_per_dir = 16;
    hp.max_radius_px = 64.f;
    expect(ssfx::computeHbaoCpu(f.view, hp, hbao.data()), "room HBAO");
    f64 sumHbao = 0.0;
    f64 sumRef = 0.0;
    u32 geometry = 0;
    u32 occluded = 0;
    u32 refCount = 0;
    for (u32 y = 0; y < 72u; ++y) {
        for (u32 x = 0; x < 96u; ++x) {
            const usize i = static_cast<usize>(y) * 96u + x;
            if (f.depth[i] <= 0.f) {
                expect(gtao[i] == 1.f, "sky pixels have AO 1");
                continue;
            }
            ++geometry;
            occluded += gtao[i] < 0.9f ? 1u : 0u;
            sumHbao += std::fabs(static_cast<f64>(gtao[i]) - hbao[i]);
            if ((x % 3u) == 0u && (y % 3u) == 0u) {
                const f32 ref = ssfx::ssaoHemisphereReferenceVisibility(f.view, x, y, 1.f, 16u, 48u);
                sumRef += std::fabs(static_cast<f64>(gtao[i]) - ref);
                ++refCount;
            }
        }
    }
    const f64 meanHbao = sumHbao / geometry;
    const f64 meanRef = sumRef / refCount;
    std::printf("gtao room (96x72, 16 slices): %u geometry pixels, %u occluded (< 0.9); mean |GTAO - HBAO| = %.4f; "
                "mean |GTAO - brute-force hemisphere| = %.4f (%u pixels)\n",
                geometry, occluded, meanHbao, meanRef, refCount);
    expect(occluded > 100u, "room: the creases and boxes occlude");
    expect(meanHbao <= 0.03, "room: GTAO mean |diff| to the HBAO oracle <= 0.03");
    expect(meanRef <= 0.05, "room: GTAO mean |diff| to the brute-force hemisphere reference <= 0.05");
}

// --- gtao_parity ---------------------------------------------------------------------------------------------
void testGtaoParity() {
    ssfx_test::RoomScene room;
    ssfx_test::buildRoom(room, 61, 37);
    SsfxGpuSettings s{};
    s.gtao.jitter = true;
    s.gtao.frame = 3;
    s.gtao.falloff = 0.25f;
    CpuFrame f;
    expect(makeFrame(room.g, room.camera, s, room.ambient, f), "parity frame");
    const usize n = f.depth.size();
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    std::vector<f32> reference(n, -1.f);
    kernel::reset_kernel_stats();
    const kernel::LaunchResult r = kernel::launch(kernel::Backend::CpuReference, ssfx::gtao_kernel::make_launch(f.view),
                                                  ssfx::gtao_kernel::Kernel{},
                                                  ssfx::gtao_kernel::make_params(f.view, s.gtao, reference.data()));
    expect(r.ok && r.backend == kernel::Backend::CpuReference && r.items == n, "CpuReference launch");
    kernel::KernelStats stats{};
    expect(kernel::find_kernel_stats("screen_space_gtao", stats) && stats.launches == 1u && stats.items == n,
           "kernel stats under \"screen_space_gtao\" with the item count");
    for (const u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        std::vector<f32> parallel(n, -2.f);
        expect(computeGtaoCpu(f.view, s.gtao, parallel.data(), kernel::Backend::CpuParallel), "CpuParallel launch");
        const bool same = std::memcmp(reference.data(), parallel.data(), n * sizeof(f32)) == 0;
        std::printf("gtao parity: CpuReference == CpuParallel bit for bit at %u workers: %s\n", workers,
                    same ? "yes" : "NO");
        expect(same, "CpuReference == CpuParallel bit for bit");
    }
    for (const kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::VulkanCompute}) {
        if (kernel::backend_available(gpu)) {
            continue;
        }
        std::vector<f32> out(n, -3.f);
        const kernel::LaunchResult fb =
            kernel::launch(gpu, ssfx::gtao_kernel::make_launch(f.view), ssfx::gtao_kernel::Kernel{},
                           ssfx::gtao_kernel::make_params(f.view, s.gtao, out.data()));
        expect(fb.ok && fb.backend == kernel::Backend::CpuParallel &&
                   std::memcmp(reference.data(), out.data(), n * sizeof(f32)) == 0,
               "a GPU request without a device falls back to CpuParallel (same bits)");
    }
    scheduler.shutdown();
    u32 occluded = 0;
    for (usize i = 0; i < n; ++i) {
        occluded += reference[i] < 0.95f ? 1u : 0u;
    }
    expect(occluded > 20u, "parity frame has occlusion");
}

// --- reference -----------------------------------------------------------------------------------------------
void testReference() {
    ssfx_test::RoomScene room;
    ssfx_test::buildRoom(room, 64, 48);
    SsfxGpuSettings s{};
    s.ssgi_params.sample_sqrt = 3;
    s.ssgi_params.bounces = 2;
    CpuFrame f;
    expect(makeFrame(room.g, room.camera, s, room.ambient, f), "reference frame");
    const usize n = f.depth.size();
    // prepare_pixel: linear depth and normals against the analytic scene.
    f64 depthErr = 0.0;
    f64 normalErr = 0.0;
    u32 sky = 0;
    const f32* m = room.camera.view.data.data();
    for (usize i = 0; i < n; ++i) {
        const Vec4& p = f.prepared.prepared[i];
        if (room.g.viewZ[i] <= 0.f) {
            sky += p.x == 0.f ? 1u : 0u;
            continue;
        }
        depthErr = std::max(depthErr, std::fabs(static_cast<f64>(p.x) - room.g.viewZ[i]) / room.g.viewZ[i]);
        const Vec3 w = room.g.worldN[i];
        const Vec3 ve{m[0] * w.x + m[4] * w.y + m[8] * w.z, m[1] * w.x + m[5] * w.y + m[9] * w.z,
                      m[2] * w.x + m[6] * w.y + m[10] * w.z};
        const Vec3 expected{ve.x, -ve.y, -ve.z};
        const f32 cosA = std::min(1.f, f.prepared.normal[i].normalized().dot(expected));
        normalErr = std::max(normalErr, static_cast<f64>(std::acos(cosA)));
        expect(p.y == room.g.rt2[i].x && p.z == room.g.rt2[i].y && p.w == room.g.rt0[i].w, "roughness / metallic / AO");
    }
    std::printf("reference: prepare max relative depth error %.3g (z/w -> linear), max normal error %.3g rad, "
                "%u sky pixels -> depth 0\n",
                depthErr, normalErr, sky);
    expect(depthErr <= 1e-4 && normalErr <= 1e-3 && sky > 50u, "prepare_pixel: depth and normals");

    // compose_pixel rules on hand-made inputs.
    SsfxFrameConstants c = f.c;
    c.ambient[0] = 0.5f;
    c.ambient[1] = 0.25f;
    c.ambient[2] = 1.f;
    const Vec4 prep{3.f, 0.2f, 0.f, 0.8f};
    const Vec3 albedo{0.5f, 0.5f, 0.5f};
    const Vec3 lit{1.f, 1.f, 1.f};
    const Vec3 nrm{0.f, 0.f, -1.f};
    const u32 cx = static_cast<u32>(c.cx);
    const u32 cy = static_cast<u32>(c.cy);
    c.flags = kSsfxFlagAo;
    Vec4 o = compose_pixel(c, cx, cy, prep, nrm, albedo, lit, 1.f, 1.f, Vec4{}, Vec3{});
    expect(o.x == 1.f && o.y == 1.f && o.z == 1.f && o.w == 1.f, "AO 1 leaves the lit image unchanged");
    o = compose_pixel(c, cx, cy, prep, nrm, albedo, lit, 1.f, 0.f, Vec4{}, Vec3{});
    expect(std::fabs(o.x - (1.f - 0.5f * 0.5f * 0.8f)) < 1e-6f && std::fabs(o.z - (1.f - 1.f * 0.5f * 0.8f)) < 1e-6f,
           "AO 0 removes exactly ambient x albedo x material AO");
    c.flags = kSsfxFlagSsr;
    o = compose_pixel(c, cx, cy, prep, nrm, albedo, lit, 1.f, 0.f, Vec4{2.f, 2.f, 2.f, 0.5f}, Vec3{});
    // Near-normal incidence at the centre: F ~ F0 = 0.04 (dielectric).
    expect(std::fabs(o.x - (1.f + 2.f * 0.5f * 0.04f)) < 2e-4f, "SSR adds reflection x confidence x F0 at normal incidence");
    o = compose_pixel(c, cx, cy, Vec4{0.f, 0.2f, 0.f, 0.8f}, nrm, albedo, lit, 0.f, 0.f, Vec4{2.f, 2.f, 2.f, 1.f}, Vec3{});
    expect(o.x == 1.f && o.w == 0.f, "sky pixels are untouched");
    c.flags = kSsfxFlagSsgi;
    o = compose_pixel(c, cx, cy, prep, nrm, albedo, lit, 1.f, 0.f, Vec4{}, Vec3{0.1f, 0.2f, 0.3f});
    expect(o.x == 1.1f && o.y == 1.2f && std::fabs(o.z - 1.3f) < 1e-6f, "SSGI adds the indirect radiance");
    c.flags = kSsfxFlagAo | kSsfxFlagMultiBounce;
    o = compose_pixel(c, cx, cy, prep, nrm, albedo, lit, 1.f, 0.5f, Vec4{}, Vec3{});
    const f32 mb = ssfx::gtao_kernel::multi_bounce(0.5f, 0.5f);
    expect(mb >= 0.5f && std::fabs(o.x - (1.f + 0.5f * 0.5f * 0.8f * (mb - 1.f))) < 1e-6f, "multi-bounce AO");

    // Full reference frame: CpuReference == CpuParallel, every effect contributes.
    SsfxReferenceFrame a;
    SsfxReferenceFrame b;
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    expect(reference_frame(s, f.c, f.prepared, a, kernel::Backend::CpuReference), "reference_frame CpuReference");
    scheduler.initialize(2);
    expect(reference_frame(s, f.c, f.prepared, b, kernel::Backend::CpuParallel), "reference_frame CpuParallel");
    scheduler.shutdown();
    expect(std::memcmp(a.composed.data(), b.composed.data(), n * sizeof(Vec4)) == 0 &&
               std::memcmp(a.ao.data(), b.ao.data(), n * sizeof(f32)) == 0 &&
               std::memcmp(a.ssr.data(), b.ssr.data(), n * sizeof(Vec4)) == 0 &&
               std::memcmp(a.gi.data(), b.gi.data(), n * sizeof(Vec3)) == 0,
           "reference_frame CpuReference == CpuParallel bit for bit");
    u32 aoLess = 0;
    u32 hits = 0;
    u32 lit2 = 0;
    for (usize i = 0; i < n; ++i) {
        aoLess += a.ao[i] < 0.9f ? 1u : 0u;
        hits += a.ssr[i].w > 0.f ? 1u : 0u;
        lit2 += a.gi[i].x > 0.f ? 1u : 0u;
    }
    std::printf("reference frame 64x48: %u occluded, %u SSR hits, %u pixels with indirect light\n", aoLess, hits, lit2);
    expect(aoLess > 20u && hits > 100u && lit2 > 100u, "reference frame exercises AO, SSR hits and SSGI");
}

// --- sky fallback (kSsfxFlagSky) -------------------------------------------------------------------------------
Vec3 testSky(const void*, const Vec3& w) { return Vec3{0.2f + 0.5f * std::max(0.f, w.y), 0.3f + 0.1f * w.x, 0.4f + 0.2f * w.z}; }

void testSkyFallback() {
    ssfx_test::RoomScene room;
    ssfx_test::buildRoom(room, 64, 48);
    SsfxGpuSettings s{};
    s.ssgi_params.sample_sqrt = 3;
    s.ssgi_params.bounces = 2;
    CpuFrame off;
    expect(makeFrame(room.g, room.camera, s, room.ambient, off), "frame without the sky fallback");
    expect((off.c.flags & kSsfxFlagSky) == 0u, "skyFallback off: no kSsfxFlagSky");
    s.skyFallback = true;
    s.ssgiSkyFallback = true;
    CpuFrame f;
    expect(makeFrame(room.g, room.camera, s, room.ambient, f), "frame with the sky fallback");
    expect((f.c.flags & kSsfxFlagSky) != 0u && (f.c.flags & kSsfxFlagSkyGi) != 0u, "skyFallback / ssgiSkyFallback on: kSsfxFlagSky / SkyGi");
    SsfxGpuSettings none = s;
    none.ssr = false;
    none.ssgi = false;
    SsfxFrameConstants cn{};
    expect(resolve_constants(none, ssfx_test::cameraDesc(room.camera), room.ambient, 64, 48, cn) &&
               (cn.flags & (kSsfxFlagSky | kSsfxFlagSkyGi)) == 0u,
           "no SSR / SSGI: no sky flags");
    SsfxGpuSettings ssrOnly = s;
    ssrOnly.ssgiSkyFallback = false;
    SsfxFrameConstants cs{};
    expect(resolve_constants(ssrOnly, ssfx_test::cameraDesc(room.camera), room.ambient, 64, 48, cs) &&
               (cs.flags & kSsfxFlagSky) != 0u && (cs.flags & kSsfxFlagSkyGi) == 0u,
           "skyFallback alone: SSR only");
    const usize n = f.depth.size();

    // sky_world_dir inverts prepare_pixel's world -> ssfx-view rotation (the view normals map back to the world ones).
    f64 dirErr = 0.0;
    for (usize i = 0; i < n; ++i) {
        if (room.g.viewZ[i] <= 0.f) {
            continue;
        }
        const Vec3 w = sky_world_dir(f.c, f.prepared.normal[i]);
        const Vec3 e = room.g.worldN[i];
        dirErr = std::max({dirErr, static_cast<f64>(std::fabs(w.x - e.x)), static_cast<f64>(std::fabs(w.y - e.y)),
                           static_cast<f64>(std::fabs(w.z - e.z))});
    }
    expect(dirErr < 5e-3, "sky_world_dir == the world direction of an ssfx view direction");

    // sky_exit: a ray straight up the view (-Y ssfx) from a floor pixel leaves the screen; one into the floor does not.
    u32 upExits = 0;
    u32 downExits = 0;
    u32 floorPx = 0;
    for (u32 y = 24; y < 48; y += 3) {
        for (u32 x = 2; x < 64; x += 5) {
            if (f.view.depthAt(x, y) <= 0.f) {
                continue;
            }
            ++floorPx;
            upExits += sky_exit(f.view, f.c.ssrMaxDistance, x, y, Vec3{0.f, -1.f, 0.f}) ? 1u : 0u;
            downExits += sky_exit(f.view, 1e-5f, x, y, Vec3{0.f, 1.f, 0.f}) ? 1u : 0u;
        }
    }
    expect(floorPx > 20u && upExits == floorPx && downExits == 0u, "sky_exit: off-screen end -> sky; zero-length ray -> no sky");

    const SsfxSkySource sky{&testSky, nullptr};
    SsfxReferenceFrame a;
    SsfxReferenceFrame b;
    expect(reference_frame(s, off.c, off.prepared, a, kernel::Backend::CpuReference), "reference without the sky");
    expect(reference_frame(s, f.c, f.prepared, b, kernel::Backend::CpuReference, sky), "reference with the sky");
    SsfxReferenceFrame c0;
    expect(reference_frame(s, f.c, f.prepared, c0, kernel::Backend::CpuReference, SsfxSkySource{}) &&
               std::memcmp(c0.ssr.data(), a.ssr.data(), n * sizeof(Vec4)) == 0 &&
               std::memcmp(c0.gi.data(), a.gi.data(), n * sizeof(Vec3)) == 0,
           "kSsfxFlagSky without a sky source == no fallback");
    u32 kept = 0;
    u32 skySsr = 0;
    u32 bad = 0;
    u32 giMore = 0;
    u32 giLess = 0;
    const ssfx::SsrParams sp = ssfx::ssr_kernel::clamp_params(s.ssr_params);
    for (u32 y = 0; y < 48u; ++y) {
        for (u32 x = 0; x < 64u; ++x) {
            const usize i = static_cast<usize>(y) * 64u + x;
            const bool same = std::memcmp(&a.ssr[i], &b.ssr[i], sizeof(Vec4)) == 0;
            const f32 r = f.prepared.prepared[i].y;
            if (f.view.depthAt(x, y) <= 0.f || !(r < 1.f) ||
                ssfx::ssr_kernel::trace_pixel(f.view, f.prepared.radiance.data(), sp, x, y).hit) {
                kept += same ? 1u : 0u;
                bad += same ? 0u : 1u; // sky pixels, rough surfaces and hits: unchanged
                continue;
            }
            const Vec3 dir = pixel_reflection(f.view, x, y);
            if (!sky_exit(f.view, sp.max_distance, x, y, dir)) {
                bad += same ? 0u : 1u; // a miss over geometry stays a miss
                continue;
            }
            ++skySsr;
            const Vec3 L = testSky(nullptr, sky_world_dir(f.c, dir.normalized()));
            const f32 fade = ssfx::ssr_kernel::gloss_fade(s.contact, sp.max_distance, r);
            bad += (b.ssr[i].x == L.x && b.ssr[i].y == L.y && b.ssr[i].z == L.z && b.ssr[i].w == fade) ? 0u : 1u;
        }
    }
    for (usize i = 0; i < n; ++i) {
        giMore += b.gi[i].x > a.gi[i].x ? 1u : 0u;
        giLess += b.gi[i].x < a.gi[i].x - 1e-6f ? 1u : 0u;
    }
    std::printf("sky fallback 64x48: SSR %u pixels kept (hits), %u take the sky (%u bad); SSGI brighter at %u pixels, darker %u\n",
                kept, skySsr, bad, giMore, giLess);
    expect(skySsr > 50u && bad == 0u, "SSR misses that leave the screen / reach the sky take the sky radiance, hits are unchanged");
    expect(giMore > 100u && giLess == 0u, "SSGI gathers the sky on its missed rays (never less light)");
}

// --- api -----------------------------------------------------------------------------------------------------
void testApi() {
    ssfx_test::Camera cam;
    cam.width = 64;
    cam.height = 48;
    cam.eye = Vec3{0.f, 1.f, 5.f};
    cam.target = Vec3{0.f, 0.f, 0.f};
    cam.build();
    SsfxGpuSettings s{};
    s.gtao.slices = 99;
    s.gtao.steps = 0;
    s.ssr_params.max_steps = 0;
    s.ssgi_params.sample_sqrt = 100;
    s.ssgi_params.bounces = 3;
    s.multiBounce = true;
    const f32 ambient[3] = {0.1f, 0.2f, 0.3f};
    SsfxFrameConstants c{};
    expect(resolve_constants(s, ssfx_test::cameraDesc(cam), ambient, 64, 48, c), "resolve_constants");
    expect(c.aoSlices == 32u && c.aoSteps == 1u && c.ssrMaxSteps == 1u && c.giSampleSqrt == 64u && c.giBounces == 3u,
           "kernel parameters are clamped like the CPU kernels");
    expect((c.flags & (kSsfxFlagAo | kSsfxFlagSsr | kSsfxFlagSsgi | kSsfxFlagMultiBounce | kSsfxFlagSsrRoughness |
                       kSsfxFlagSsrContact)) ==
               (kSsfxFlagAo | kSsfxFlagSsr | kSsfxFlagSsgi | kSsfxFlagMultiBounce | kSsfxFlagSsrRoughness |
                kSsfxFlagSsrContact) &&
               (c.flags & kSsfxFlagReversedZ) == 0u,
           "flags");
    const ssfx::gtao_kernel::Params g = ssfx::gtao_kernel::make_params(ssfx::SsfxGBufferView{}, s.gtao, nullptr);
    expect(std::memcmp(c.aoSliceCos, g.slice_cos, sizeof(c.aoSliceCos)) == 0 &&
               std::memcmp(c.aoSliceSin, g.slice_sin, sizeof(c.aoSliceSin)) == 0,
           "slice table == gtao_kernel::make_params");
    const f32 focal = 0.5f * 48.f * cam.proj.data[5];
    expect(std::fabs(c.fy - focal) < 1e-4f && std::fabs(c.cx - 32.f) < 1e-4f && std::fabs(c.cy - 24.f) < 1e-4f &&
               std::fabs(c.nearZ - cam.nearPlane) < 1e-4f,
           "camera intrinsics from the projection");
    expect(c.ambient[2] == 0.3f && c.viewRot[0] == cam.view.data[0] && c.viewRot[10] == cam.view.data[10],
           "ambient + view rotation");
    SsfxCameraDesc bad = ssfx_test::cameraDesc(cam);
    bad.proj[11] = 0.f; // orthographic
    expect(!resolve_constants(s, bad, ambient, 64, 48, c), "a non-perspective projection is rejected");
    expect(!querySsfxCapabilities(nullptr).ssfx, "no device: not capable");
    SsfxGpu gpu;
    expect(!gpu.init(SsfxGpuDesc{}) && !gpu.valid(), "init without a device fails cleanly");
    ssfx_test::RoomScene room;
    ssfx_test::buildRoom(room, 8, 8);
    SsfxFrameImages none{};
    expect(!gpu.beginFrame(1, s, ssfx_test::cameraDesc(room.camera), ambient, none), "beginFrame before init fails");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "gtao_analytic") {
        testPolynomials();
        testPlanes();
        testWedges();
        testRoomVsOracles();
    }
    if (all || suite == "gtao_parity") {
        testGtaoParity();
    }
    if (all || suite == "reference") {
        testReference();
    }
    if (all || suite == "sky") {
        testSkyFallback();
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
