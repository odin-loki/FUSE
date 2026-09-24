// WP-7.2 ReSTIR DI and GI, CPU gates (stub-safe; the Lavapipe gates are test_rp_restir.cpp).
//
// Scene (test_rp_restir_scene.hpp): ground + 4 boxes lit by a ceiling panel of 10 000 emissive triangles (WP-7.1
// light tree), 32 x 24 pixels. The runner is RestirCpu (the single-source kernel the GPU passes equal bit for bit);
// the ground truth is RestirReference (f64, independent code: power-proportional light sampling, cosine rays).
//
//   layout      every record's GLSL / Slang mirror (restir_common.{glsl,slang}) declares the same fields in the
//               same order at the same offsets; buffer layout sections 256-aligned and disjoint, ping-pong stages
//               never alias their input
//   api         settings sanitised, frame constants (flags, camera), light table, RestirGpu without a device fails
//               cleanly, the kernel's octahedral decode / cosine map / sequence determinism
//   unbiased    unbiased mode (Talbot MIS, visibility-tested targets; temporal + 1 spatial iteration, 8 frames):
//               the ensemble mean over independent runs == the reference within 3 sigma (image mean, DI and GI;
//               4 x 4 tiles within 3.5 sigma), sigma from the run-to-run spread and the reference's standard error
//   variance    per-pixel variance of the full ReSTIR chain < the RIS-only baseline (initial candidates +
//               visibility reuse for DI, one path sample for GI), both modes
//   biased      biased ("1 / M") mode: |mean - reference| / reference bounded (kBiasBound), and larger than the
//               unbiased mode's residual is allowed only up to that bound
//   reuse       temporal M-capping (M = 1, 2, ... 1 + mCap) and reprojection through the UV motion (a 3-pixel
//               shifted frame finds its history, an off-screen motion restarts every pixel)
//   zero_alloc  steady-state RestirCpu frames make no heap allocation (replaced operator new)
#include "test_rp_restir_scene.hpp"

#include <fuse/renderer/restir/restir.hpp>
#include <fuse/renderer/restir/restir_gpu.hpp>
#include <fuse/renderer/restir/restir_kernel.hpp>
#include <fuse/renderer/restir/restir_reference.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::restir;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using restir_test::Scene;
using restir_test::Tracer;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

u32 bits(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, 4);
    return b;
}

// Test sizes. The statistics hold at any run count (sigma comes from the run-to-run spread); Debug builds run
// fewer runs / reference samples to stay within the ctest timeout.
constexpr u32 kWidth = 48;
constexpr u32 kHeight = 36;
constexpr u32 kFrames = 8;
#if defined(NDEBUG)
constexpr u32 kRuns = 256;
constexpr u32 kRefSamples = 8192;
#else
constexpr u32 kRuns = 64;
constexpr u32 kRefSamples = 2048;
#endif
constexpr f64 kSigma = 3.0;      ///< image mean
constexpr f64 kTileSigma = 3.5;  ///< each of the 16 tiles (family-wise ~0.7 % at 3.5 sigma)
constexpr f64 kBiasBound = 0.08; ///< biased mode: |relative bias| of the image mean (DI and GI)

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define RS_FIELD(T, n) Field{#n, offsetof(T, n)}
const Field kLightFields[] = {RS_FIELD(RestirLight, radiance), RS_FIELD(RestirLight, kind),  RS_FIELD(RestirLight, cosInner),
                              RS_FIELD(RestirLight, cosOuter), RS_FIELD(RestirLight, flags), RS_FIELD(RestirLight, reserved)};
const Field kDiFields[] = {RS_FIELD(RestirDiReservoir, light), RS_FIELD(RestirDiReservoir, u1),
                           RS_FIELD(RestirDiReservoir, u2),    RS_FIELD(RestirDiReservoir, W),
                           RS_FIELD(RestirDiReservoir, M),     RS_FIELD(RestirDiReservoir, targetPdf),
                           RS_FIELD(RestirDiReservoir, reserved)};
const Field kGiFields[] = {RS_FIELD(RestirGiReservoir, position), RS_FIELD(RestirGiReservoir, W),
                           RS_FIELD(RestirGiReservoir, normal),   RS_FIELD(RestirGiReservoir, M),
                           RS_FIELD(RestirGiReservoir, radiance), RS_FIELD(RestirGiReservoir, targetPdf)};
const Field kHitFields[] = {RS_FIELD(RestirGiHitRecord, t),     RS_FIELD(RestirGiHitRecord, instance),
                            RS_FIELD(RestirGiHitRecord, primitive), RS_FIELD(RestirGiHitRecord, flags),
                            RS_FIELD(RestirGiHitRecord, direction), RS_FIELD(RestirGiHitRecord, reserved)};
const Field kFrameFields[] = {
    RS_FIELD(RestirFrameConstants, surfPos),         RS_FIELD(RestirFrameConstants, surfNormal),
    RS_FIELD(RestirFrameConstants, surfAlbedo),      RS_FIELD(RestirFrameConstants, diHistory),
    RS_FIELD(RestirFrameConstants, giHistory),       RS_FIELD(RestirFrameConstants, diSignal),
    RS_FIELD(RestirFrameConstants, giSignal),        RS_FIELD(RestirFrameConstants, depth),
    RS_FIELD(RestirFrameConstants, motion),          RS_FIELD(RestirFrameConstants, lightTree),
    RS_FIELD(RestirFrameConstants, lights),          RS_FIELD(RestirFrameConstants, tlas),
    RS_FIELD(RestirFrameConstants, scene),           RS_FIELD(RestirFrameConstants, invViewProj),
    RS_FIELD(RestirFrameConstants, cameraPosition),  RS_FIELD(RestirFrameConstants, width),
    RS_FIELD(RestirFrameConstants, cameraForward),   RS_FIELD(RestirFrameConstants, height),
    RS_FIELD(RestirFrameConstants, invWidth),        RS_FIELD(RestirFrameConstants, invHeight),
    RS_FIELD(RestirFrameConstants, frameIndex),      RS_FIELD(RestirFrameConstants, seed),
    RS_FIELD(RestirFrameConstants, flags),           RS_FIELD(RestirFrameConstants, lightCount),
    RS_FIELD(RestirFrameConstants, diCandidates),    RS_FIELD(RestirFrameConstants, diNeighbors),
    RS_FIELD(RestirFrameConstants, giNeighbors),     RS_FIELD(RestirFrameConstants, gbufferNormal),
    RS_FIELD(RestirFrameConstants, gbufferAlbedo),   RS_FIELD(RestirFrameConstants, gbufferDepth),
    RS_FIELD(RestirFrameConstants, diRadius),        RS_FIELD(RestirFrameConstants, giRadius),
    RS_FIELD(RestirFrameConstants, diMCap),          RS_FIELD(RestirFrameConstants, giMCap),
    RS_FIELD(RestirFrameConstants, normalThreshold), RS_FIELD(RestirFrameConstants, depthThreshold),
    RS_FIELD(RestirFrameConstants, normalBias),      RS_FIELD(RestirFrameConstants, viewBias),
    RS_FIELD(RestirFrameConstants, farDistance),     RS_FIELD(RestirFrameConstants, giJacobianClamp),
    RS_FIELD(RestirFrameConstants, giRayTMin),       RS_FIELD(RestirFrameConstants, cullMask)};
const Field kPushFields[] = {RS_FIELD(RestirPush, frame), RS_FIELD(RestirPush, src),  RS_FIELD(RestirPush, dst),
                             RS_FIELD(RestirPush, aux),   RS_FIELD(RestirPush, mode), RS_FIELD(RestirPush, iteration),
                             RS_FIELD(RestirPush, reserved)};
#undef RS_FIELD

std::string readText(const std::string& path) {
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// Parses `struct <name> {...};` of a shader source: (name, std430 / scalar offset) per field.
bool parseShaderStruct(const std::string& text, const std::string& name, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    const std::string key = "struct " + name + " {";
    const size_t begin = text.find(key);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + key.size(), end - begin - key.size()));
    std::string line;
    size_t offset = 0;
    size_t align = 4;
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
        align = std::max(align, bytes);
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = (offset + align - 1u) / align * align;
    return true;
}

template <size_t N>
void checkStruct(const std::string& text, const char* lang, const char* shaderName, const Field (&fields)[N], size_t cppSize) {
    std::vector<size_t> offsets;
    std::vector<std::string> names;
    size_t size = 0;
    const bool parsed = parseShaderStruct(text, shaderName, offsets, size, names);
    expect(parsed, "shader struct parsed");
    if (!parsed) {
        std::fprintf(stderr, "  %s: struct %s missing\n", lang, shaderName);
        return;
    }
    bool same = offsets.size() == N && size == cppSize;
    for (size_t i = 0; same && i < N; ++i) {
        same = names[i] == fields[i].name && offsets[i] == fields[i].offset;
        if (!same) {
            std::fprintf(stderr, "  %s %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, shaderName, i, names[i].c_str(),
                         offsets[i], fields[i].name, fields[i].offset);
        }
    }
    if (offsets.size() != N || size != cppSize) {
        std::fprintf(stderr, "  %s %s: %zu fields / %zu bytes vs C++ %zu / %zu\n", lang, shaderName, offsets.size(), size, N,
                     cppSize);
    }
    std::printf("layout: %s %s %zu fields, %zu bytes\n", lang, shaderName, offsets.size(), size);
    expect(same, "shader struct == C++ record (names, order, offsets, size)");
}

void testLayout() {
    const std::string dir = FUSE_RP_RESTIR_SHADER_DIR;
    for (const char* lang : {"glsl", "slang"}) {
        const std::string text = readText(dir + "/restir_common." + lang);
        expect(!text.empty(), "restir_common source readable");
        checkStruct(text, lang, "RestirLight", kLightFields, sizeof(RestirLight));
        checkStruct(text, lang, "RestirDiReservoir", kDiFields, sizeof(RestirDiReservoir));
        checkStruct(text, lang, "RestirGiReservoir", kGiFields, sizeof(RestirGiReservoir));
        checkStruct(text, lang, "RestirGiHitRecord", kHitFields, sizeof(RestirGiHitRecord));
        checkStruct(text, lang, "RestirFrame", kFrameFields, sizeof(RestirFrameConstants));
        checkStruct(text, lang, "RestirPush", kPushFields, sizeof(RestirPush));
    }
    for (const bool keep : {false, true}) {
        const RestirBufferLayout l = RestirBufferLayout::compute(37u, 23u, keep);
        const u64 pixels = 37u * 23u;
        bool aligned = true;
        for (u32 k = 0; k < 2u; ++k) {
            for (u64 v : {l.surfPos[k], l.surfNormal[k], l.surfAlbedo[k], l.diHistory[k], l.giHistory[k]}) {
                aligned = aligned && v % 256u == 0u;
            }
        }
        for (u32 s = 0; s < kRestirStages; ++s) {
            aligned = aligned && l.diStage[s] % 256u == 0u && l.giStage[s] % 256u == 0u;
            aligned = aligned && l.diStage[s] + pixels * sizeof(RestirDiReservoir) <= l.workBytes;
            aligned = aligned && l.giStage[s] + pixels * sizeof(RestirGiReservoir) <= l.workBytes;
        }
        expect(aligned, "buffer sections 256-aligned and inside the buffer");
        expect(l.giHistory[1] + pixels * sizeof(RestirGiReservoir) <= l.stateBytes &&
                   l.depth + pixels * 4u <= l.outputBytes && l.giSignal >= l.diSignal + pixels * 16u,
               "state / output sections disjoint");
        // No stage aliases the stage it reads: temporal reads initial, spatial 0 reads temporal or initial,
        // spatial i reads spatial i - 1.
        bool disjoint = l.diStage[1] != l.diStage[0] && l.diStage[2] != l.diStage[1] && l.diStage[2] != l.diStage[0];
        for (u32 s = 3; s < kRestirStages; ++s) {
            disjoint = disjoint && l.diStage[s] != l.diStage[s - 1u] && l.giStage[s] != l.giStage[s - 1u];
        }
        expect(disjoint, "no stage section aliases its input");
        if (keep) {
            bool distinct = true;
            for (u32 a = 0; a < kRestirStages; ++a) {
                for (u32 b = a + 1u; b < kRestirStages; ++b) {
                    distinct = distinct && l.diStage[a] != l.diStage[b] && l.giStage[a] != l.giStage[b];
                }
            }
            expect(distinct, "keepIntermediates: one section per stage");
        }
    }
}

// --- api -------------------------------------------------------------------------------------------------
void testApi() {
    RestirSettings wild{};
    wild.diCandidates = 1000;
    wild.diSpatialIterations = 99;
    wild.diNeighbors = 0;
    wild.giNeighbors = 99;
    wild.diRadius = -3.f;
    wild.diMCap = 0.f;
    wild.cullMask = 0xFFu;
    const RestirSettings s = restirSanitize(wild);
    expect(s.diCandidates == kRestirMaxCandidates && s.diSpatialIterations == kRestirMaxSpatial && s.diNeighbors == 1u &&
               s.giNeighbors == kRestirMaxNeighbors && s.diRadius == 1.f && s.diMCap == 1.f && s.cullMask == 0x7Fu,
           "restirSanitize clamps every field (cull mask never has the dead-slot bit)");

    Scene scene;
    expect(restir_test::makeScene(scene, kWidth, kHeight, 4, 4), "scene");
    RestirSettings settings{};
    settings.unbiased = true;
    RestirFrameParams params{};
    params.width = kWidth;
    params.height = kHeight;
    params.history = true;
    params.lightCount = 32;
    RestirFrameConstants c{};
    expect(buildRestirFrameConstants(settings, restir_test::restirCamera(scene.camera), params, c), "frame constants");
    expect((c.flags & kRestirFlagUnbiased) != 0u && (c.flags & kRestirFlagHistory) != 0u &&
               (c.flags & kRestirFlagVisibilityReuse) != 0u && (c.flags & kRestirFlagMotion) == 0u &&
               (c.flags & kRestirFlagGiShadeVisibility) != 0u,
           "frame flags");
    const f64 fl = std::sqrt(static_cast<f64>(c.cameraForward[0]) * c.cameraForward[0] +
                             static_cast<f64>(c.cameraForward[1]) * c.cameraForward[1] +
                             static_cast<f64>(c.cameraForward[2]) * c.cameraForward[2]);
    expect(std::fabs(fl - 1.0) < 1e-6, "camera forward normalised");
    params.width = 0;
    expect(!buildRestirFrameConstants(settings, restir_test::restirCamera(scene.camera), params, c), "zero extent rejected");
    // prepare reconstructs the camera's surfaces: pixel centre depth -> the analytic hit
    params.width = kWidth;
    expect(buildRestirFrameConstants(settings, restir_test::restirCamera(scene.camera), params, c), "frame constants");
    std::vector<RestirSurfaceF> surfaces;
    std::vector<RestirRefSurface> ref;
    restir_test::castSurfaces(scene, surfaces, ref);
    f64 worst = 0.0;
    u32 checked = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const RestirSurfaceF& t = surfaces[y * kWidth + x];
            if (!rsValid(t)) {
                continue;
            }
            const vsmr_test::D3 p{t.p.x, t.p.y, t.p.z};
            const f32 depth = static_cast<f32>(scene.camera.depthOf(p));
            // RT0 stores the signed octahedral normal (the G-buffer's encoding), here exactly for axis normals
            const f32 ox = t.n.x / (std::fabs(t.n.x) + std::fabs(t.n.y) + std::fabs(t.n.z));
            const f32 oy = t.n.y / (std::fabs(t.n.x) + std::fabs(t.n.y) + std::fabs(t.n.z));
            RestirSurfaceF out{};
            if (!rsPrepare(c, x, y, depth, t.n.z >= 0.f ? ox : (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f),
                           t.n.z >= 0.f ? oy : (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f), out)) {
                continue;
            }
            ++checked;
            const f64 dp = std::sqrt((out.p.x - t.p.x) * (out.p.x - t.p.x) + (out.p.y - t.p.y) * (out.p.y - t.p.y) +
                                     (out.p.z - t.p.z) * (out.p.z - t.p.z));
            worst = std::max(worst, dp / std::max(1e-3, static_cast<f64>(t.depth)));
            expect(std::fabs(rsDot(out.n, t.n) - 1.f) < 1e-6f, "prepare: octahedral normal decoded");
        }
    }
    std::printf("api: prepare reconstructs %u surfaces, worst |dp| / depth %.2e\n", checked, worst);
    expect(checked > kWidth * kHeight / 2u && worst < 2e-4, "prepare reconstruction");

    // cosine map: unit, in the hemisphere, deterministic
    const RV3 n = rsNormalize(RV3{0.3f, 0.8f, -0.5f}, RV3{0.f, 0.f, 1.f});
    bool unit = true;
    for (u32 i = 0; i < 256u; ++i) {
        f32 u = 0.f;
        f32 v = 0.f;
        rsSobol(i, 1234u, u, v);
        const RV3 d = rsCosineDirection(n, u, v);
        unit = unit && std::fabs(rsLength(d) - 1.f) < 1e-6f && rsDot(d, n) >= 0.f;
        const RV3 d2 = rsCosineDirection(n, u, v);
        unit = unit && bits(d.x) == bits(d2.x) && bits(d.y) == bits(d2.y) && bits(d.z) == bits(d2.z);
    }
    expect(unit, "cosine directions unit, above the surface, deterministic");
    u32 st = rsSeed(3, 4, 5, 6, 7);
    f32 mean = 0.f;
    for (u32 i = 0; i < 4096u; ++i) {
        const f32 r = rsNext(st);
        expect(r >= 0.f && r < 1.f, "rsNext in [0, 1)");
        mean += r;
    }
    expect(std::fabs(mean / 4096.f - 0.5f) < 0.02f, "rsNext mean");

    const RestirLight spot = makeRestirLight(
        fuse::renderer::light_tree::makeSpotLight({0.f, 2.f, 0.f}, {0.f, -1.f, 0.f}, 0.9f, 0.7f, 5.f), {1.f, 2.f, 3.f});
    expect(spot.cosInner == 0.9f && spot.cosOuter == 0.7f && spot.radiance[2] == 3.f, "makeRestirLight spot");
    expect(rsSpot(0.95f, 0.9f, 0.7f) == 1.f && rsSpot(0.6f, 0.9f, 0.7f) == 0.f && rsSpot(0.8f, 0.9f, 0.7f) > 0.49f &&
               rsSpot(0.8f, 0.9f, 0.7f) < 0.51f,
           "spot smoothstep");

    expect(!queryRestirCapabilities(nullptr).gpu, "no device: not capable");
    RestirGpu gpu;
    expect(!gpu.init(RestirGpuDesc{}) && !gpu.valid(), "RestirGpu::init without a device fails cleanly");
}

// --- statistics -------------------------------------------------------------------------------------------------
struct Ensemble {
    std::vector<f64> sum;
    std::vector<f64> sum2;
    std::vector<f64> runMean; ///< image mean of each run
    std::vector<f64> runTile; ///< 16 tile means of each run (4 x 4 tiles)
    u32 runs = 0;
};

struct Fixture {
    Scene scene;
    std::vector<RestirSurfaceF> surfaces;
    std::vector<RestirRefSurface> ref;
    std::vector<u8> valid;
    u32 validCount = 0;
    std::vector<f64> refDi;   ///< per-pixel reference luminance
    std::vector<f64> refDiSe; ///< its standard error
    std::vector<f64> refGi;
    std::vector<f64> refGiSe;
    bool references = false;
};

bool makeFixture(Fixture& f) {
    if (!restir_test::makeScene(f.scene, kWidth, kHeight)) {
        return false;
    }
    restir_test::castSurfaces(f.scene, f.surfaces, f.ref);
    f.valid.assign(f.surfaces.size(), 0u);
    for (usize p = 0; p < f.surfaces.size(); ++p) {
        f.valid[p] = rsValid(f.surfaces[p]) ? 1u : 0u;
        f.validCount += f.valid[p];
    }
    std::printf("scene: %zu emissive triangles, %u / %u pixels with a surface, light tree %u nodes (depth %u)\n",
                f.scene.lights.size(), f.validCount, kWidth * kHeight, f.scene.tree.stats().nodes, f.scene.tree.stats().maxDepth);
    return f.validCount > kWidth * kHeight / 2u;
}

void computeReferences(Fixture& f, const RestirSettings& settings) {
    if (f.references) {
        return;
    }
    RestirReference reference;
    expect(reference.build(f.scene.tree.view(), f.scene.table.data(), static_cast<u32>(f.scene.table.size())), "reference build");
    Tracer tracer(f.scene);
    RestirRefParams params{};
    params.normalBias = settings.normalBias;
    params.viewBias = settings.viewBias;
    params.camera[0] = static_cast<f32>(f.scene.camera.eye.x);
    params.camera[1] = static_cast<f32>(f.scene.camera.eye.y);
    params.camera[2] = static_cast<f32>(f.scene.camera.eye.z);
    params.giRayTMin = settings.giRayTMin;
    params.farDistance = settings.farDistance;
    const usize n = f.surfaces.size();
    f.refDi.assign(n, 0.0);
    f.refDiSe.assign(n, 0.0);
    f.refGi.assign(n, 0.0);
    f.refGiSe.assign(n, 0.0);
    for (usize p = 0; p < n; ++p) {
        if (f.valid[p] == 0u) {
            continue;
        }
        const RestirRefEstimate d = reference.direct(f.ref[p], tracer, params, kRefSamples, 1000u + p);
        const RestirRefEstimate g = reference.indirect(f.ref[p], tracer, params, kRefSamples, 777000u + p);
        f.refDi[p] = d.luminance;
        f.refDiSe[p] = d.stdError;
        f.refGi[p] = g.luminance;
        f.refGiSe[p] = g.stdError;
    }
    f.references = true;
}

/// Calls fn(pixel) for every pixel of tile t (4 x 4 tiles).
template <typename Fn>
void forTile(u32 t, Fn&& fn) {
    const u32 tw = kWidth / 4u;
    const u32 th = kHeight / 4u;
    const u32 tx = t % 4u;
    const u32 ty = t / 4u;
    for (u32 y = ty * th; y < (ty + 1u) * th; ++y) {
        for (u32 x = tx * tw; x < (tx + 1u) * tw; ++x) {
            fn(static_cast<usize>(y) * kWidth + x);
        }
    }
}

/// `runs` independent runs of `frames` frames; accumulates the last frame's per-pixel DI / GI luminance.
void runEnsemble(const Fixture& f, const RestirSettings& settings, u32 runs, u32 frames, Ensemble& di, Ensemble& gi) {
    const usize n = f.surfaces.size();
    for (Ensemble* e : {&di, &gi}) {
        e->sum.assign(n, 0.0);
        e->sum2.assign(n, 0.0);
        e->runMean.clear();
        e->runTile.clear();
        e->runs = runs;
    }
    RestirCpu cpu;
    Tracer tracer(f.scene);
    const RestirCamera cam = restir_test::restirCamera(f.scene.camera);
    for (u32 r = 0; r < runs; ++r) {
        cpu.reset();
        for (u32 frame = 0; frame < frames; ++frame) {
            const bool ok = cpu.runFrame(settings, cam, kWidth, kHeight, frame, 0xC0FFEEu + r * 7919u, f.surfaces.data(), nullptr,
                                         f.scene.tree.view(), f.scene.table.data(), static_cast<u32>(f.scene.table.size()), tracer);
            if (!ok) {
                expect(false, "RestirCpu::runFrame");
                return;
            }
        }
        f64 m[2] = {0.0, 0.0};
        for (usize p = 0; p < n; ++p) {
            if (f.valid[p] == 0u) {
                continue;
            }
            const f64 v[2] = {
                0.2126 * cpu.diSignal()[p * 4u] + 0.7152 * cpu.diSignal()[p * 4u + 1u] + 0.0722 * cpu.diSignal()[p * 4u + 2u],
                0.2126 * cpu.giSignal()[p * 4u] + 0.7152 * cpu.giSignal()[p * 4u + 1u] + 0.0722 * cpu.giSignal()[p * 4u + 2u]};
            Ensemble* es[2] = {&di, &gi};
            for (u32 k = 0; k < 2u; ++k) {
                es[k]->sum[p] += v[k];
                es[k]->sum2[p] += v[k] * v[k];
                m[k] += v[k];
            }
        }
        di.runMean.push_back(m[0] / f.validCount);
        gi.runMean.push_back(m[1] / f.validCount);
        for (u32 t = 0; t < 16u; ++t) {
            f64 ts[2] = {0.0, 0.0};
            u32 count = 0;
            forTile(t, [&](usize p) {
                if (f.valid[p] == 0u) {
                    return;
                }
                ts[0] += 0.2126 * cpu.diSignal()[p * 4u] + 0.7152 * cpu.diSignal()[p * 4u + 1u] + 0.0722 * cpu.diSignal()[p * 4u + 2u];
                ts[1] += 0.2126 * cpu.giSignal()[p * 4u] + 0.7152 * cpu.giSignal()[p * 4u + 1u] + 0.0722 * cpu.giSignal()[p * 4u + 2u];
                ++count;
            });
            di.runTile.push_back(count > 0u ? ts[0] / count : 0.0);
            gi.runTile.push_back(count > 0u ? ts[1] / count : 0.0);
        }
    }
}

struct Compare {
    f64 mean = 0.0;
    f64 ref = 0.0;
    f64 sigma = 0.0;
    f64 z = 0.0;
    f64 relative = 0.0;
    f64 worstTileZ = 0.0;
    f64 meanPixelVariance = 0.0;
};

Compare compare(const Fixture& f, const Ensemble& e, const std::vector<f64>& ref, const std::vector<f64>& refSe) {
    Compare c{};
    const f64 R = e.runs;
    f64 m1 = 0.0;
    f64 m2 = 0.0;
    for (f64 v : e.runMean) {
        m1 += v;
        m2 += v * v;
    }
    c.mean = m1 / R;
    const f64 runVar = std::max(0.0, (m2 - R * c.mean * c.mean) / (R - 1.0));
    f64 refSum = 0.0;
    f64 refVar = 0.0;
    f64 pixVar = 0.0;
    for (usize p = 0; p < ref.size(); ++p) {
        if (f.valid[p] == 0u) {
            continue;
        }
        refSum += ref[p];
        refVar += refSe[p] * refSe[p];
        const f64 pm = e.sum[p] / R;
        pixVar += std::max(0.0, (e.sum2[p] - R * pm * pm) / (R - 1.0));
    }
    c.ref = refSum / f.validCount;
    const f64 refSeImage2 = refVar / (static_cast<f64>(f.validCount) * f.validCount);
    c.sigma = std::sqrt(runVar / R + refSeImage2);
    c.z = (c.mean - c.ref) / c.sigma;
    c.relative = (c.mean - c.ref) / c.ref;
    c.meanPixelVariance = pixVar / f.validCount;
    // 4 x 4 tiles: run-to-run spread of each tile mean + the reference's standard error of that tile
    for (u32 t = 0; t < 16u; ++t) {
        f64 refm = 0.0;
        f64 refv = 0.0;
        u32 count = 0;
        forTile(t, [&](usize p) {
            if (f.valid[p] == 0u) {
                return;
            }
            refm += ref[p];
            refv += refSe[p] * refSe[p];
            ++count;
        });
        if (count == 0u) {
            continue;
        }
        f64 t1 = 0.0;
        f64 t2 = 0.0;
        for (u32 r = 0; r < e.runs; ++r) {
            const f64 v = e.runTile[static_cast<usize>(r) * 16u + t];
            t1 += v;
            t2 += v * v;
        }
        const f64 tm = t1 / R;
        const f64 tv = std::max(0.0, (t2 - R * tm * tm) / (R - 1.0));
        const f64 sigma = std::sqrt(tv / R + refv / (static_cast<f64>(count) * count));
        const f64 z = std::fabs(tm - refm / count) / std::max(sigma, 1e-30);
        c.worstTileZ = std::max(c.worstTileZ, z);
    }
    return c;
}

void print(const char* tag, const Compare& c) {
    std::printf("%s: mean %.6f ref %.6f (rel %+.4f) sigma %.2e z %+.2f, worst tile z %.2f, mean pixel variance %.4e\n", tag, c.mean,
                c.ref, c.relative, c.sigma, c.z, c.worstTileZ, c.meanPixelVariance);
}

RestirSettings fullSettings(bool unbiased) {
    RestirSettings s{};
    s.unbiased = unbiased;
    s.diCandidates = 4;
    s.diSpatialIterations = 1;
    s.diNeighbors = 4;
    s.diRadius = 3.f;
    s.giSpatialIterations = 1;
    s.giNeighbors = 4;
    s.giRadius = 3.f;
    return s;
}

RestirSettings risOnlySettings(bool unbiased) {
    RestirSettings s = fullSettings(unbiased);
    s.diTemporal = false;
    s.giTemporal = false;
    s.diSpatialIterations = 0;
    s.giSpatialIterations = 0;
    return s;
}

void testUnbiased(Fixture& f) {
    const RestirSettings s = fullSettings(true);
    computeReferences(f, s);
    Ensemble di;
    Ensemble gi;
    runEnsemble(f, s, kRuns, kFrames, di, gi);
    const Compare cd = compare(f, di, f.refDi, f.refDiSe);
    const Compare cg = compare(f, gi, f.refGi, f.refGiSe);
    print("unbiased DI", cd);
    print("unbiased GI", cg);
    expect(std::fabs(cd.z) <= kSigma, "unbiased DI: image mean == reference within 3 sigma");
    expect(std::fabs(cg.z) <= kSigma, "unbiased GI: image mean == reference within 3 sigma");
    expect(cd.worstTileZ <= kTileSigma && cg.worstTileZ <= kTileSigma, "unbiased: every 4 x 4 tile within 3.5 sigma");
    expect(cd.ref > 0.0 && cg.ref > 0.0 && cd.mean > 0.0 && cg.mean > 0.0, "non-trivial signals");
    // RIS-only (initial candidates + visibility reuse) is unbiased on its own: the same check.
    Ensemble rdi;
    Ensemble rgi;
    runEnsemble(f, risOnlySettings(true), kRuns, 1u, rdi, rgi);
    const Compare rd = compare(f, rdi, f.refDi, f.refDiSe);
    const Compare rg = compare(f, rgi, f.refGi, f.refGiSe);
    print("RIS-only DI", rd);
    print("RIS-only GI", rg);
    expect(std::fabs(rd.z) <= kSigma && std::fabs(rg.z) <= kSigma, "RIS-only: image mean == reference within 3 sigma");
}

void testVariance(Fixture& f) {
    for (const bool unbiased : {true, false}) {
        Ensemble di;
        Ensemble gi;
        Ensemble rdi;
        Ensemble rgi;
        runEnsemble(f, fullSettings(unbiased), kRuns, kFrames, di, gi);
        runEnsemble(f, risOnlySettings(unbiased), kRuns, kFrames, rdi, rgi);
        std::vector<f64> zero(f.surfaces.size(), 0.0);
        const Compare a = compare(f, di, zero, zero);
        const Compare b = compare(f, rdi, zero, zero);
        const Compare c = compare(f, gi, zero, zero);
        const Compare d = compare(f, rgi, zero, zero);
        std::printf("variance (%s): DI ReSTIR %.4e vs RIS-only %.4e (x%.2f lower); GI ReSTIR %.4e vs one path %.4e (x%.2f lower)\n",
                    unbiased ? "unbiased" : "biased", a.meanPixelVariance, b.meanPixelVariance,
                    b.meanPixelVariance / std::max(a.meanPixelVariance, 1e-30), c.meanPixelVariance, d.meanPixelVariance,
                    d.meanPixelVariance / std::max(c.meanPixelVariance, 1e-30));
        expect(a.meanPixelVariance < b.meanPixelVariance, "DI ReSTIR variance below the RIS-only baseline");
        expect(c.meanPixelVariance < d.meanPixelVariance, "GI ReSTIR variance below the single-path baseline");
    }
}

void testBiased(Fixture& f) {
    RestirSettings s = fullSettings(false);
    computeReferences(f, s);
    Ensemble di;
    Ensemble gi;
    runEnsemble(f, s, kRuns, kFrames, di, gi);
    const Compare cd = compare(f, di, f.refDi, f.refDiSe);
    const Compare cg = compare(f, gi, f.refGi, f.refGiSe);
    print("biased DI", cd);
    print("biased GI", cg);
    expect(std::fabs(cd.relative) <= kBiasBound, "biased DI: |relative bias| <= kBiasBound");
    expect(std::fabs(cg.relative) <= kBiasBound, "biased GI: |relative bias| <= kBiasBound");
}

/// Temporal reuse mechanics: M-capping (history M <= mCap x canonical M) and reprojection through the UV motion.
void testReuse() {
    Fixture f;
    expect(makeFixture(f), "fixture");
    Tracer tracer(f.scene);
    const RestirCamera cam = restir_test::restirCamera(f.scene.camera);
    const u32 lights = static_cast<u32>(f.scene.table.size());
    for (const bool unbiased : {false, true}) {
        RestirSettings s = fullSettings(unbiased);
        s.diSpatialIterations = 0;
        s.giSpatialIterations = 0;
        s.diMCap = 5.f;
        s.giMCap = 3.f;
        RestirCpu cpu;
        bool capped = true;
        for (u32 frame = 0; frame < 10u; ++frame) {
            cpu.runFrame(s, cam, kWidth, kHeight, frame, 9u, f.surfaces.data(), nullptr, f.scene.tree.view(), f.scene.table.data(),
                         lights, tracer);
            // static view, temporal only: M = 1, 2, ... up to 1 + cap on every surface pixel
            const f32 di = std::min(static_cast<f32>(frame + 1u), 1.f + s.diMCap);
            const f32 gi = std::min(static_cast<f32>(frame + 1u), 1.f + s.giMCap);
            for (usize p = 0; p < f.surfaces.size(); ++p) {
                if (f.valid[p] != 0u) {
                    capped = capped && cpu.diFinal()[p].M == di && cpu.giFinal()[p].M == gi;
                } else {
                    capped = capped && cpu.diFinal()[p].M == 0.f && cpu.giFinal()[p].M == 0.f;
                }
            }
        }
        expect(capped, "temporal M grows by one per frame and is capped at 1 + mCap");
    }
    // Reprojection: frame 1 shows frame 0 shifted right by 3 pixels, the motion says so; every pixel whose source is on
    // screen finds its own history (M = 2), the 3 uncovered columns start over (M = 1).
    RestirSettings s = fullSettings(false);
    s.diSpatialIterations = 0;
    s.giSpatialIterations = 0;
    std::vector<RestirSurfaceF> shifted(f.surfaces.size());
    std::vector<f32> motion(f.surfaces.size() * 2u, 0.f);
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize p = static_cast<usize>(y) * kWidth + x;
            shifted[p] = x >= 3u ? f.surfaces[p - 3u] : RestirSurfaceF{};
            motion[p * 2u] = 3.f / static_cast<f32>(kWidth);
        }
    }
    RestirCpu cpu;
    cpu.runFrame(s, cam, kWidth, kHeight, 0u, 5u, f.surfaces.data(), nullptr, f.scene.tree.view(), f.scene.table.data(), lights,
                 tracer);
    cpu.runFrame(s, cam, kWidth, kHeight, 1u, 5u, shifted.data(), motion.data(), f.scene.tree.view(), f.scene.table.data(), lights,
                 tracer);
    u32 reused = 0;
    u32 fresh = 0;
    bool ok = true;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize p = static_cast<usize>(y) * kWidth + x;
            if (!rsValid(shifted[p])) {
                continue;
            }
            const f32 m = cpu.diFinal()[p].M;
            ok = ok && m == 2.f && cpu.giFinal()[p].M == 2.f;
            reused += m == 2.f ? 1u : 0u;
        }
    }
    // camera cut: the same surfaces with a motion pointing off screen -> no history anywhere
    for (usize p = 0; p < motion.size(); p += 2u) {
        motion[p] = 2.f;
    }
    cpu.runFrame(s, cam, kWidth, kHeight, 2u, 5u, shifted.data(), motion.data(), f.scene.tree.view(), f.scene.table.data(), lights,
                 tracer);
    for (usize p = 0; p < shifted.size(); ++p) {
        if (rsValid(shifted[p])) {
            ok = ok && cpu.diFinal()[p].M == 1.f;
            fresh += cpu.diFinal()[p].M == 1.f ? 1u : 0u;
        }
    }
    std::printf("reuse: M capped at 1 + mCap (DI 5, GI 3, 10 frames, both modes); shifted frame: %u pixels reprojected, off-screen "
                "motion: %u pixels restart\n",
                reused, fresh);
    expect(ok && reused > 0u && fresh == reused, "reprojection through the UV motion");
}

void testZeroAlloc() {
    Fixture f;
    expect(makeFixture(f), "fixture");
    RestirCpu cpu;
    cpu.reserve(kWidth, kHeight);
    Tracer tracer(f.scene);
    const RestirCamera cam = restir_test::restirCamera(f.scene.camera);
    std::vector<f32> motion(static_cast<usize>(kWidth) * kHeight * 2u, 0.f);
    for (usize i = 0; i < motion.size(); i += 2u) {
        motion[i] = 0.5f / kWidth;
    }
    for (const bool unbiased : {false, true}) {
        const RestirSettings s = fullSettings(unbiased);
        for (u32 frame = 0; frame < 2u; ++frame) {
            cpu.runFrame(s, cam, kWidth, kHeight, frame, 1u, f.surfaces.data(), motion.data(), f.scene.tree.view(),
                         f.scene.table.data(), static_cast<u32>(f.scene.table.size()), tracer);
        }
        t_allocations = 0;
        t_count = true;
        for (u32 frame = 2; frame < 10u; ++frame) {
            cpu.runFrame(s, cam, kWidth, kHeight, frame, 1u, f.surfaces.data(), motion.data(), f.scene.tree.view(),
                         f.scene.table.data(), static_cast<u32>(f.scene.table.size()), tracer);
        }
        t_count = false;
        std::printf("zero_alloc (%s): %llu operator new in 8 steady-state frames\n", unbiased ? "unbiased" : "biased", t_allocations);
        expect(t_allocations == 0u, "RestirCpu steady-state frames allocate nothing");
    }
    expect(cpu.stats().shadowRays > 0u && cpu.stats().giRays > 0u, "rays traced");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (all || suite == "unbiased" || suite == "variance" || suite == "biased") {
        Fixture f;
        expect(makeFixture(f), "fixture");
        if (all || suite == "unbiased") {
            testUnbiased(f);
        }
        if (all || suite == "variance") {
            testVariance(f);
        }
        if (all || suite == "biased") {
            testBiased(f);
        }
    }
    if (all || suite == "reuse") {
        testReuse();
    }
    if (all || suite == "zero_alloc") {
        testZeroAlloc();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
