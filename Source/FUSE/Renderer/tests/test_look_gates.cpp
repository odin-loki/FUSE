// Look system gates (engine-native look: effect graph, .fuselook blend space, 3D LUT grading, lens and
// HDR-output kernels). Rows:
//  1. Schema: parameter table sanity; effect-graph ordering rules; .fuselook parse, canonical write and
//     round trip (parse(write(d)) == d, write idempotent); versioning (required, future versions rejected);
//     type / range errors carry a JSON path; unknown keys are warnings; the 3 sample looks load.
//  2. Blend: LUT weights always sum to 1; time-of-day keys hit exactly, interpolate, and wrap around
//     24 h continuously; weather weighted sum + normalisation + step rule; volume falloff is continuous
//     across the boundary (Lipschitz bound), hard edges step, priority order wins; log-lerp is geometric;
//     evaluation is deterministic.
//  3. LUTs: identity LUT -> identity (tetrahedral and trilinear); neutral grade bakes to the exact identity
//     lattice; .cube import vs a known file (TITLE, comments, DOMAIN, 1D, shaper+3D, errors) and lossless
//     write/read; tetrahedral vs trilinear accuracy (affine exactness, neutral-axis preservation,
//     convergence 32 -> 64); grade generation (saturation 0 -> greys, white balance neutralises the
//     Planckian white); LUT blending.
//  4. Kernels: CpuReference == CpuParallel bit-exact for the whole chain at 0/2/4 workers and every output
//     encoding; bloom kernels == renderer::bloom_image; tonemap / grain == the existing post code; analytic
//     vignette falloff, radial CA offset, lens-dirt modulation, flare symmetry and ghost position, PQ
//     (ST 2084) vs double-precision reference and published code values, scRGB scaling, HDR shoulder C1.
//  5. Integration: the neutral look through LookPostChain matches PostStack::processFrame; apply_look_to_post_stack.
//  6. Hot reload (edit, broken edit keeps last good, LUT edit) and 0 heap allocations per frame in steady state.

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_cas_bridge.hpp>
#include <fuse/renderer/look/look_kernels.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/look_post_chain.hpp>
#include <fuse/renderer/look/look_schema.hpp>
#include <fuse/renderer/look/look_system.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#if __has_include(<fuse/renderer/upscale/upscale_passes.hpp>)
#include <fuse/renderer/upscale/upscale_passes.hpp>
#define FUSE_LOOK_TEST_HAS_CAS 1
#else
#define FUSE_LOOK_TEST_HAS_CAS 0
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

// ---- global heap counter (whole binary, every thread) ------------------------------------------

namespace {
std::atomic<std::uint64_t> g_heapAllocations{0};
// FUSE_LOOK_ALLOC_TRAP=1: abort on the first allocation inside the measured steady-state window (debugging).
std::atomic<bool> g_allocTrap{false};
void countAllocation() {
    g_heapAllocations.fetch_add(1u, std::memory_order_relaxed);
    if (g_allocTrap.load(std::memory_order_relaxed)) {
        std::abort();
    }
}
#if defined(_WIN32)
void* testAlignedAlloc(std::size_t alignment, std::size_t size) { return _aligned_malloc(size, alignment); }
void testAlignedFree(void* ptr) { _aligned_free(ptr); }
#else
void* testAlignedAlloc(std::size_t alignment, std::size_t size) { return std::aligned_alloc(alignment, size); }
void testAlignedFree(void* ptr) { std::free(ptr); }
#endif
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    countAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    countAllocation();
    return std::malloc(size == 0 ? 1 : size);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    countAllocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1u) / align * align;
    if (void* p = testAlignedAlloc(align, rounded)) {
        return p;
    }
    throw std::bad_alloc();
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::align_val_t) noexcept { testAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::align_val_t) noexcept { testAlignedFree(ptr); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    testAlignedFree(ptr);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    testAlignedFree(ptr);
}

namespace {

using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec2;
using fuse::math::Vec3;
namespace kernel = fuse::kernel;
namespace lk = fuse::renderer::look::kernels;
using namespace fuse::renderer::look;

int g_failures = 0;
int g_checks = 0;

void expectTrue(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f64 actual, f64 expected, f64 eps, const char* message) {
    ++g_checks;
    if (!(std::fabs(actual - expected) <= eps)) {
        std::fprintf(stderr, "FAIL: %s (expected %.9g, got %.9g, eps %.3g)\n", message, expected, actual, eps);
        ++g_failures;
    }
}

void setWorkers(u32 workers) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
}

std::string samplesDir() {
    return FUSE_LOOK_SAMPLES_DIR;
}

std::filesystem::path scratchDir() {
    std::filesystem::path p = std::filesystem::temp_directory_path() /
                              ("fuse_look_gates_" + std::to_string(static_cast<unsigned long long>(
                                                         std::hash<std::thread::id>{}(std::this_thread::get_id()))));
    std::filesystem::create_directories(p);
    return p;
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

f32 maxAbs(const Vec3& a, const Vec3& b) {
    return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

// Deterministic HDR test frame: gradient sky, bright sun disc, emissive strip, dark corner, saturated patches.
std::vector<Vec3> makeHdrFrame(u32 w, u32 h) {
    std::vector<Vec3> img(static_cast<size_t>(w) * h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
            Vec3 c{0.05f + 0.25f * u, 0.08f + 0.2f * (1.f - v), 0.12f + 0.3f * v};
            const f32 dx = u - 0.72f;
            const f32 dy = v - 0.3f;
            if (dx * dx + dy * dy < 0.004f) {
                c = Vec3{40.f, 32.f, 20.f};
            }
            if (v > 0.8f && v < 0.84f && u > 0.1f && u < 0.5f) {
                c = Vec3{6.f, 1.5f, 0.3f};
            }
            if (u < 0.15f && v > 0.85f) {
                c = Vec3{0.002f, 0.002f, 0.003f};
            }
            if (u > 0.4f && u < 0.5f && v > 0.45f && v < 0.6f) {
                c = Vec3{0.02f, 0.6f, 0.05f};
            }
            img[static_cast<size_t>(y) * w + x] = c;
        }
    }
    return img;
}

// ---------------------------------------------------------------------------------------------
// 1. Schema

void testParamTable() {
    const LookParamBlock& d = look_default_params();
    bool inRange = true;
    bool unique = true;
    bool fuseNames = true;
    for (u32 i = 0; i < kLookParamCount; ++i) {
        const LookParamInfo& info = look_param_info(i);
        inRange = inRange && static_cast<u32>(info.id) == i;
        for (u32 c = 0; c < look_param_components(info.type); ++c) {
            inRange = inRange && d.get(info.id, c) >= info.min && d.get(info.id, c) <= info.max;
        }
        for (u32 j = 0; j < i; ++j) {
            const LookParamInfo& o = look_param_info(j);
            unique = unique && !(o.effect == info.effect && std::strcmp(o.key, info.key) == 0);
        }
        std::string lower(info.key);
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        // FUSE-owned schema: no third-party injector identifiers.
        fuseNames = fuseNames && lower.find("enb") == std::string::npos && lower.find("intensitynight") == std::string::npos;
    }
    expectTrue(inRange, "param table: ids ordered, defaults inside [min, max]");
    expectTrue(unique, "param table: (node, key) unique");
    expectTrue(fuseNames, "param table: FUSE-owned names only");
    LookParamBlock b = d;
    expectTrue(look_clamp_params(b) == 0u, "defaults are already clamped");
    b.set(LookParam::BloomIntensity, 1e9f);
    b.set(LookParam::TmOperator, 2.4f);
    b.set(LookParam::VignetteEnabled, 0.7f);
    b.set(LookParam::GradeGain, std::nanf(""), 1);
    look_clamp_params(b);
    expectTrue(b.get(LookParam::BloomIntensity) == 10.f && b.get(LookParam::TmOperator) == 2.f &&
                   b.get(LookParam::VignetteEnabled) == 1.f && b.get(LookParam::GradeGain, 1) == 1.f,
               "clamp: range, enum snap, bool snap, NaN -> default");
    const LookResolved r = look_resolve(d);
    expectTrue(r.tonemap.op == LookToneMapOperator::Aces && r.bloom.enabled && !r.dof.enabled &&
                   r.output.paper_white_nits == 200.f && r.grade.curve[4].x == 1.f,
               "resolve maps the default block");
    u32 e = 0;
    expectTrue(look_param_enum_from_name(LookParam::TmOperator, "reinhard", e) && e == 2u &&
                   !look_param_enum_from_name(LookParam::TmOperator, "bogus", e),
               "enum names");
}

void testGraphValidation() {
    expectTrue(LookEffectGraph::makeDefault().validate().ok(), "default graph validates");
    const auto make = [](std::initializer_list<LookEffect> nodes) {
        LookEffectGraph g;
        for (LookEffect n : nodes) {
            g.push(n);
        }
        return g.validate();
    };
    using E = LookEffect;
    expectTrue(make({E::Bloom, E::Vignette, E::Exposure, E::ToneMap, E::ColorGrade, E::OutputTransform}).ok(),
               "vignette in the HDR section is legal");
    expectTrue(make({E::ToneMap, E::OutputTransform}).ok(), "minimal graph (tonemap + output)");
    expectTrue(make({}).error == LookGraphError::Empty, "empty graph rejected");
    expectTrue(make({E::Bloom, E::Bloom, E::ToneMap, E::OutputTransform}).error == LookGraphError::DuplicateNode,
               "duplicate node rejected");
    expectTrue(make({E::Bloom, E::OutputTransform}).error == LookGraphError::MissingToneMap, "tonemap required");
    expectTrue(make({E::Bloom, E::ToneMap}).error == LookGraphError::MissingOutputTransform, "output required");
    expectTrue(make({E::ToneMap, E::OutputTransform, E::FilmGrain}).error == LookGraphError::OutputNotLast,
               "output must be last");
    expectTrue(make({E::ToneMap, E::Bloom, E::OutputTransform}).error == LookGraphError::DomainMismatch,
               "HDR node after tonemap rejected (HDR-before-tonemap)");
    expectTrue(make({E::ColorGrade, E::ToneMap, E::OutputTransform}).error == LookGraphError::DomainMismatch,
               "display node before tonemap rejected");
    expectTrue(make({E::ToneMap, E::Exposure, E::OutputTransform}).error == LookGraphError::ExposureAfterToneMap,
               "exposure after tonemap rejected");
    expectTrue(make({E::LensDirt, E::Bloom, E::ToneMap, E::OutputTransform}).error ==
                   LookGraphError::LensDirtWithoutBloom,
               "lens dirt needs bloom first");
    expectTrue(make({E::LensFlare, E::ToneMap, E::OutputTransform}).error == LookGraphError::LensFlareWithoutBloom,
               "lens flare needs bloom");
    expectTrue(make({E::ToneMap, E::FilmGrain, E::Sharpen, E::OutputTransform}).error ==
                   LookGraphError::GrainBeforeSharpen,
               "grain before sharpen rejected");
    expectTrue(make({E::Bloom, E::AmbientOcclusion, E::ToneMap, E::OutputTransform}).error == LookGraphError::StageOrder,
               "pre-upscale node after post-upscale node rejected");
}

const char* kRichLook = R"({
  "schemaVersion": 1,
  "kind": "fuse.look",
  "name": "rich",
  "description": "round trip \"quoted\" \\ text",
  "graph": ["bloom", "lensDirt", "exposure", "toneMap", "colorGrade", "vignette", "outputTransform"],
  "base": {
    "bloom": { "intensity": 0.123456789, "tint": [1, 0.5, 0.25], "enabled": true },
    "toneMap": { "operator": "reinhard", "calibrateMidGrey": false },
    "depthOfField": { "bokehBlades": 9, "fStop": 1.4 },
    "colorGrade": { "lut": null, "gain": [1.1, 1.0, 0.9] },
    "futureNode": { "x": 1 }
  },
  "timeOfDay": { "interpolation": "smooth", "keys": [
    { "hour": 21.5, "settings": { "exposure": { "biasEv": -1.25 } } },
    { "hour": 5, "settings": { "exposure": { "biasEv": 0.5 }, "colorGrade": { "lut": "a.cube" } } } ] },
  "weather": [ { "name": "fog", "settings": { "vignette": { "intensity": 0.6 } } } ],
  "volumes": [
    { "name": "s", "shape": "sphere", "center": [1, 2, 3], "radius": 4, "falloff": 2, "priority": -3, "weight": 0.5,
      "settings": { "filmGrain": { "intensity": 0.1 } } },
    { "name": "b", "shape": "box", "center": [0, 0, 0], "halfExtents": [1, 2, 3], "falloff": 0, "priority": 7,
      "settings": {} } ],
  "overrides": [ { "name": "damage", "settings": { "chromaticAberration": { "enabled": true, "intensity": 0.02 } } } ],
  "futureTopLevel": 42
})";

void testSchema() {
    LookDocument doc;
    LookParseResult res;
    expectTrue(look_parse(kRichLook, doc, res), "rich look parses");
    if (!res.ok) {
        std::fprintf(stderr, "  error: %s\n", res.error.c_str());
        return;
    }
    expectTrue(res.warnings.size() == 2u, "unknown node and top-level key produce warnings (forward compatible)");
    expectTrue(doc.name == "rich" && doc.has_graph && doc.graph.size() == 7u && doc.time_keys.size() == 2u &&
                   doc.weather.size() == 1u && doc.volumes.size() == 2u && doc.overrides.size() == 1u,
               "rich look structure");
    expectTrue(doc.base.mask.test(LookParam::BloomIntensity) && doc.base.values.get(LookParam::BloomIntensity) == 0.123456789f &&
                   doc.base.values.get(LookParam::TmOperator) == 2.f && doc.base.lut_set && doc.base.lut_path.empty() &&
                   doc.base.mask.count() == 8u,
               "settings values, enum, null LUT, override mask");
    const std::string text = look_write(doc);
    LookDocument again;
    LookParseResult res2;
    expectTrue(look_parse(text, again, res2) && res2.warnings.empty(), "canonical text re-parses without warnings");
    expectTrue(again == doc, "round trip: parse(write(doc)) == doc");
    expectTrue(look_write(again) == text, "canonical write is idempotent");

    const auto errorOf = [](const std::string& t) {
        LookDocument d;
        LookParseResult r;
        look_parse(t, d, r);
        return r.ok ? std::string() : r.error;
    };
    const std::string head = R"({"schemaVersion": 1, "kind": "fuse.look", )";
    expectTrue(errorOf(R"({"kind": "fuse.look"})").find("$.schemaVersion: required") == 0u, "schemaVersion required");
    expectTrue(errorOf(R"({"schemaVersion": 2, "kind": "fuse.look"})").find("unsupported version 2") != std::string::npos,
               "future schemaVersion rejected");
    expectTrue(errorOf(R"({"schemaVersion": 1.5, "kind": "fuse.look"})").find("$.schemaVersion") == 0u,
               "non-integer schemaVersion rejected");
    expectTrue(errorOf(R"({"schemaVersion": 1, "kind": "other"})").find("$.kind") == 0u, "kind checked");
    expectTrue(errorOf(head + R"("base": {"bloom": {"intensity": "x"}}})") == "$.base.bloom.intensity: expected number, got string",
               "type error carries the JSON path");
    expectTrue(errorOf(head + R"("base": {"bloom": {"intensity": 99}}})").find("$.base.bloom.intensity: value out of range") == 0u,
               "range error");
    expectTrue(errorOf(head + R"("base": {"toneMap": {"operator": "hable"}}})").find("unknown value 'hable'") != std::string::npos,
               "unknown enum value");
    expectTrue(errorOf(head + R"("base": {"depthOfField": {"bokehBlades": 5.5}}})").find("expected an integer") != std::string::npos,
               "int parameter rejects fractions");
    expectTrue(errorOf(head + R"("graph": ["toneMap", "bloom", "outputTransform"]})").find("domain_mismatch") != std::string::npos,
               "invalid graph order rejected at load");
    expectTrue(errorOf(head + R"("timeOfDay": {"keys": [{"hour": 24}]}})").find("$.timeOfDay.keys[0].hour") == 0u,
               "hour range");
    expectTrue(errorOf(head + R"("weather": [{"name": "a"}, {"name": "a"}]})").find("duplicate name") != std::string::npos,
               "duplicate weather names");
    expectTrue(errorOf("{\n  \"schemaVersion\": 1,\n  \"kind\": \"fuse.look\" \n  \"x\": 1}").find("4:") == 0u,
               "syntax error reports line:col");
    expectTrue(!errorOf(R"({"schemaVersion": 1, "kind": "fuse.look", "a": 1, "a": 2})").empty(), "duplicate JSON keys rejected");
}

void testSamples() {
    const char* names[] = {"neutral", "filmic_warm", "stormy_night"};
    for (const char* name : names) {
        LookSystem sys;
        LookParseResult diag;
        const std::string path = samplesDir() + "/" + name + ".fuselook";
        const bool ok = sys.loadFile(path.c_str(), diag);
        char label[160];
        std::snprintf(label, sizeof(label), "sample %s loads without warnings (%s)", name, diag.error.c_str());
        expectTrue(ok && diag.warnings.empty(), label);
        LookDocument doc;
        LookParseResult r;
        look_load_file(path.c_str(), doc, r);
        LookDocument again;
        look_parse(look_write(doc), again, r);
        std::snprintf(label, sizeof(label), "sample %s round-trips", name);
        expectTrue(again == doc, label);
    }
    LookSystem neutral;
    LookParseResult diag;
    neutral.loadFile((samplesDir() + "/neutral.fuselook").c_str(), diag);
    neutral.evaluate({});
    const Lut3D id = Lut3D::identity(kLutSizeDefault);
    expectTrue(neutral.gradeLut().data.size() == id.data.size() &&
                   std::memcmp(neutral.gradeLut().data.data(), id.data.data(), id.data.size() * sizeof(Vec3)) == 0,
               "neutral sample bakes the exact identity LUT");

    LookSystem storm;
    storm.loadFile((samplesDir() + "/stormy_night.fuselook").c_str(), diag);
    expectTrue(storm.weatherIndex("storm") == 0 && storm.overrideIndex("lightning") == 0 && storm.lutSlotCount() == 2u,
               "stormy_night: weather/override lookup and LUT slots");
    const f32 w[2] = {1.f, 0.f};
    const f32 flash = 1.f;
    LookFrameInput in{};
    in.camera_position = {100.f, 0.f, 0.f};
    in.weather_weights = w;
    in.weather_count = 2;
    const f32 calm = storm.evaluate(in).resolved.grade.saturation;
    in.override_weights = &flash;
    in.override_count = 1;
    const LookEvaluation& lit = storm.evaluate(in);
    expectTrue(calm == 0.55f && lit.resolved.grade.saturation == 0.4f && lit.resolved.exposure.bias_ev == 1.5f,
               "stormy_night: storm weather then lightning override");
    in.override_count = 0;
    in.camera_position = {0.f, 1.f, 0.f};
    const LookEvaluation& camp = storm.evaluate(in);
    expectNear(camp.lut_weights[0], 0.85, 1e-6, "camp volume (weight .85) blends toward the identity LUT");
}

// ---------------------------------------------------------------------------------------------
// 2. Blend

const char* kBlendLook = R"({
  "schemaVersion": 1, "kind": "fuse.look", "name": "blend",
  "base": { "exposure": { "biasEv": 0 }, "depthOfField": { "focusDistanceM": 10 }, "colorGrade": { "lut": "id.cube" } },
  "timeOfDay": { "interpolation": "linear", "keys": [
    { "hour": 4, "settings": { "exposure": { "biasEv": 2 }, "colorGrade": { "lut": "inv.cube" } } },
    { "hour": 22, "settings": { "exposure": { "biasEv": -1 }, "toneMap": { "operator": "neutral" } } } ] },
  "weather": [
    { "name": "a", "settings": { "bloom": { "intensity": 1 }, "depthOfField": { "focusDistanceM": 1000 },
                                 "toneMap": { "operator": "filmic" }, "colorGrade": { "lut": null } } },
    { "name": "b", "settings": { "bloom": { "intensity": 3 }, "toneMap": { "operator": "reinhard" } } } ],
  "volumes": [
    { "name": "hi", "shape": "sphere", "center": [0, 0, 0], "radius": 5, "falloff": 4, "priority": 9,
      "settings": { "vignette": { "intensity": 0.9 }, "colorGrade": { "lut": "inv.cube" } } },
    { "name": "lo", "shape": "box", "center": [0, 0, 0], "halfExtents": [5, 5, 5], "falloff": 2, "priority": 1,
      "settings": { "vignette": { "intensity": 0.1 } } },
    { "name": "edge", "shape": "sphere", "center": [100, 0, 0], "radius": 3, "falloff": 0, "priority": 0,
      "settings": { "filmGrain": { "intensity": 0.5 } } } ]
})";

void writeBlendLuts(const std::filesystem::path& dir) {
    Lut3D id = Lut3D::identity(5);
    writeFile(dir / "id.cube", lut_write_cube(id));
    Lut3D inv = id;
    for (Vec3& v : inv.data) {
        v = Vec3{1.f - v.x, 1.f - v.y, 1.f - v.z};
    }
    writeFile(dir / "inv.cube", lut_write_cube(inv));
}

void testBlend() {
    const std::filesystem::path dir = scratchDir();
    writeBlendLuts(dir);
    LookSystem sys;
    LookParseResult diag;
    expectTrue(sys.loadFromString(kBlendLook, dir.string(), diag), "blend look loads");
    if (!diag.ok) {
        std::fprintf(stderr, "  error: %s\n", diag.error.c_str());
        return;
    }
    LookFrameInput in{};
    in.camera_position = {1000.f, 0.f, 0.f};

    // Time of day: exact at keys, linear between, wraparound 22 -> 4 across midnight.
    in.time_of_day_hours = 4.f;
    expectTrue(sys.evaluate(in).resolved.exposure.bias_ev == 2.f, "ToD: value at key hour is exact");
    in.time_of_day_hours = 13.f;
    expectNear(sys.evaluate(in).resolved.exposure.bias_ev, 2.0 + (13.0 - 4.0) / 18.0 * -3.0, 1e-5, "ToD: linear between keys");
    in.time_of_day_hours = 1.f;
    const LookEvaluation& night = sys.evaluate(in);
    expectTrue(night.tod_key_a == 1u && night.tod_key_b == 0u, "ToD: 01:00 lies between the 22:00 and 04:00 keys");
    expectNear(night.tod_fraction, 0.5, 1e-6, "ToD wraparound fraction (22 -> 4 over midnight)");
    expectNear(night.resolved.exposure.bias_ev, 0.5, 1e-5, "ToD wraparound value");
    in.time_of_day_hours = 23.9999f;
    const f32 beforeMidnight = sys.evaluate(in).resolved.exposure.bias_ev;
    in.time_of_day_hours = 0.0001f;
    const f32 afterMidnight = sys.evaluate(in).resolved.exposure.bias_ev;
    expectTrue(std::fabs(beforeMidnight - afterMidnight) < 1e-3f, "ToD continuous across 24 -> 0");
    in.time_of_day_hours = 24.f + 13.f;
    const f32 wrapped = sys.evaluate(in).resolved.exposure.bias_ev;
    in.time_of_day_hours = 13.f - 48.f;
    const f32 negative = sys.evaluate(in).resolved.exposure.bias_ev;
    in.time_of_day_hours = 13.f;
    expectTrue(wrapped == sys.evaluate(in).resolved.exposure.bias_ev && negative == wrapped, "ToD hours wrap modulo 24");
    f32 maxStep = 0.f;
    f32 prev = 0.f;
    for (u32 i = 0; i <= 2400u; ++i) {
        in.time_of_day_hours = static_cast<f32>(i) * 0.01f;
        const f32 v = sys.evaluate(in).resolved.exposure.bias_ev;
        if (i > 0u) {
            maxStep = std::max(maxStep, std::fabs(v - prev));
        }
        prev = v;
    }
    expectTrue(maxStep <= 3.f / 6.f * 0.01f * 1.01f, "ToD: Lipschitz continuous over the whole day (incl. wrap)");
    // Step (enum) parameter switches at the midpoint.
    in.time_of_day_hours = 12.9f;
    expectTrue(sys.evaluate(in).resolved.tonemap.op == LookToneMapOperator::Aces, "ToD step: before midpoint keeps A");
    in.time_of_day_hours = 13.1f;
    expectTrue(sys.evaluate(in).resolved.tonemap.op == LookToneMapOperator::Neutral, "ToD step: after midpoint takes B");
    // LUT weights through ToD.
    in.time_of_day_hours = 4.f;
    const LookEvaluation& at4 = sys.evaluate(in);
    expectTrue(at4.lut_slot_count == 3u && at4.lut_weights[2] == 1.f && at4.lut_weights[1] == 0.f,
               "ToD key with LUT selects its slot");

    // Weather.
    in.time_of_day_hours = 13.f;
    f32 w[2] = {0.f, 0.f};
    in.weather_weights = w;
    in.weather_count = 2;
    expectTrue(sys.evaluate(in).resolved.bloom.intensity == 0.05f, "weather weight 0 -> base value");
    w[0] = 1.f;
    const LookEvaluation& wa = sys.evaluate(in);
    expectTrue(wa.resolved.bloom.intensity == 1.f && wa.resolved.tonemap.op == LookToneMapOperator::Filmic,
               "weather weight 1 -> state value");
    expectNear(wa.resolved.dof.focus_distance_m, 1000.0, 1e-2, "weather weight 1 (log blend) reaches the state value");
    w[0] = 0.5f;
    w[1] = 0.5f;
    const LookEvaluation& wab = sys.evaluate(in);
    expectNear(wab.resolved.bloom.intensity, 2.0, 1e-6, "two weathers at 0.5 average");
    expectNear(wab.resolved.dof.focus_distance_m, std::sqrt(10.0 * 1000.0), 1e-2,
               "log-lerp parameter (focus distance): weather blend is geometric");
    w[0] = 2.f;
    w[1] = 2.f;
    expectNear(sys.evaluate(in).resolved.bloom.intensity, 2.0, 1e-6, "weather weights > 1 are normalised");
    w[0] = 0.3f;
    w[1] = 0.45f;
    expectTrue(sys.evaluate(in).resolved.tonemap.op == LookToneMapOperator::Reinhard, "weather step: argmax weight wins");
    w[0] = 0.2f;
    w[1] = 0.2f;
    expectTrue(sys.evaluate(in).resolved.tonemap.op == LookToneMapOperator::Neutral,
               "weather step: remainder weight keeps the current (ToD) value");

    // LUT weights sum to 1 for random inputs across every layer.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<f32> uni(0.f, 1.f);
    f32 worstSum = 0.f;
    bool nonNegative = true;
    for (u32 i = 0; i < 500u; ++i) {
        in.time_of_day_hours = uni(rng) * 24.f;
        w[0] = uni(rng) * 1.2f;
        w[1] = uni(rng) * 1.2f;
        in.camera_position = {uni(rng) * 16.f - 8.f, uni(rng) * 16.f - 8.f, uni(rng) * 16.f - 8.f};
        const LookEvaluation& e = sys.evaluate(in);
        f32 s = 0.f;
        for (u32 k = 0; k < e.lut_slot_count; ++k) {
            s += e.lut_weights[k];
            nonNegative = nonNegative && e.lut_weights[k] >= 0.f;
        }
        worstSum = std::max(worstSum, std::fabs(s - 1.f));
    }
    expectTrue(worstSum < 1e-5f && nonNegative, "LUT blend weights are non-negative and sum to 1");

    // Volumes: continuity across the boundary, priority, hard edges.
    w[0] = w[1] = 0.f;
    in.time_of_day_hours = 13.f;
    f32 maxDelta = 0.f;
    f32 last = 0.f;
    for (u32 i = 0; i <= 1400u; ++i) {
        in.camera_position = {static_cast<f32>(i) * 0.01f, 0.f, 0.f}; // 0 .. 14 m, crossing both volumes' falloff
        const f32 v = sys.evaluate(in).resolved.vignette.intensity;
        if (i > 0u) {
            maxDelta = std::max(maxDelta, std::fabs(v - last));
        }
        last = v;
    }
    // smoothstep slope <= 1.5 / falloff; intensity span <= 0.9; step 1 cm; two volumes.
    expectTrue(maxDelta <= (1.5f / 2.f * 0.9f + 1.5f / 4.f * 0.9f) * 0.01f, "volume blend is continuous across boundaries");
    in.camera_position = {0.f, 0.f, 0.f};
    const LookEvaluation& inside = sys.evaluate(in);
    expectTrue(inside.resolved.vignette.intensity == 0.9f && inside.lut_weights[2] == 1.f,
               "inside overlapping volumes the higher priority wins (applied last)");
    in.camera_position = {20.f, 0.f, 0.f};
    expectTrue(sys.evaluate(in).resolved.vignette.intensity == 0.3f, "outside all falloffs -> base value");
    in.camera_position = {102.99f, 0.f, 0.f};
    expectTrue(sys.evaluate(in).resolved.film_grain.intensity == 0.5f, "hard-edged volume: inside");
    in.camera_position = {103.01f, 0.f, 0.f};
    expectTrue(sys.evaluate(in).resolved.film_grain.intensity == 0.02f, "hard-edged volume: outside");
    LookVolumeDesc vd;
    vd.center = {0.f, 0.f, 0.f};
    vd.radius = 5.f;
    vd.falloff = 4.f;
    expectNear(look_volume_alpha(vd, {7.f, 0.f, 0.f}), 0.5, 1e-6, "sphere falloff midpoint = 0.5");
    vd.shape = LookVolumeShape::Box;
    vd.half_extents = {1.f, 1.f, 1.f};
    vd.falloff = 2.f;
    expectNear(look_volume_alpha(vd, {2.f, 0.f, 0.f}), 0.5, 1e-6, "box falloff midpoint = 0.5");

    // Runtime volume insertion respects priority.
    LookVolumeDesc rt;
    rt.center = {0.f, 0.f, 0.f};
    rt.radius = 1.f;
    rt.priority = 100;
    rt.settings.values.set(LookParam::VignetteIntensity, 0.05f);
    rt.settings.mask.set(LookParam::VignetteIntensity);
    expectTrue(sys.addVolume(rt, diag), "runtime volume added");
    in.camera_position = {0.f, 0.f, 0.f};
    expectTrue(sys.evaluate(in).resolved.vignette.intensity == 0.05f, "runtime volume with top priority wins");

    // Determinism.
    in.time_of_day_hours = 7.3f;
    w[0] = 0.25f;
    w[1] = 0.6f;
    in.camera_position = {3.f, 2.f, 1.f};
    const LookParamBlock a = sys.evaluate(in).params;
    const LookParamBlock b = sys.evaluate(in).params;
    expectTrue(std::memcmp(&a, &b, sizeof(a)) == 0, "evaluation is deterministic (bitwise)");
    expectNear(look_blend_value(2.f, 8.f, 0.5f, LookBlendMode::LogLerp), 4.0, 1e-6, "log-lerp midpoint = geometric mean");
    expectTrue(look_blend_value(2.f, 8.f, 1.f, LookBlendMode::LogLerp) == 8.f, "log-lerp endpoint exact");
}

// ---------------------------------------------------------------------------------------------
// 3. LUTs

Vec3 nonlinearGrade(const Vec3& c) {
    // Neutral-preserving, non-affine: luminance-dependent saturation + per-channel gamma on chroma.
    const f32 l = lk::luminance709(c);
    const f32 s = 0.4f + 1.2f * l * l;
    return Vec3{l + (c.x - l) * s, l + (c.y - l) * s, l + (c.z - l) * s * (1.f - 0.3f * l)};
}

Lut3D lutOf(u32 n, Vec3 (*fn)(const Vec3&)) {
    Lut3D lut = Lut3D::identity(n);
    for (Vec3& v : lut.data) {
        v = fn(v);
    }
    return lut;
}

void testLuts() {
    std::mt19937 rng(99);
    std::uniform_real_distribution<f32> uni(0.f, 1.f);
    // Identity.
    for (u32 n : {kLutSizeDefault, kLutSizeHigh}) {
        const Lut3D id = Lut3D::identity(n);
        f32 errT = 0.f;
        f32 errL = 0.f;
        for (u32 i = 0; i < 20000u; ++i) {
            const Vec3 c{uni(rng), uni(rng), uni(rng)};
            errT = std::max(errT, maxAbs(lut_sample(id, c, LutInterpolation::Tetrahedral), c));
            errL = std::max(errL, maxAbs(lut_sample(id, c, LutInterpolation::Trilinear), c));
        }
        char label[128];
        std::snprintf(label, sizeof(label), "identity %u^3 LUT -> identity (tetra %.2g, trilinear %.2g)", n, errT, errL);
        expectTrue(errT < 1e-6f && errL < 1e-6f, label);
    }
    LookResolved neutral = look_resolve(look_default_params());
    Lut3D gen;
    lut_generate_grade(neutral, 33, gen);
    const Lut3D id33 = Lut3D::identity(33);
    expectTrue(std::memcmp(gen.data.data(), id33.data.data(), id33.data.size() * sizeof(Vec3)) == 0,
               "neutral grade parameters generate the exact identity lattice");

    // Affine LUT: both interpolants exact.
    const auto affine = [](const Vec3& c) {
        return Vec3{0.8f * c.x + 0.1f * c.y + 0.05f, 0.2f * c.x + 0.7f * c.z + 0.02f, 0.9f * c.z - 0.1f * c.y + 0.1f};
    };
    const Lut3D aff = lutOf(17, +affine);
    f32 affT = 0.f;
    f32 affL = 0.f;
    for (u32 i = 0; i < 20000u; ++i) {
        const Vec3 c{uni(rng), uni(rng), uni(rng)};
        affT = std::max(affT, maxAbs(lut_sample(aff, c), affine(c)));
        affL = std::max(affL, maxAbs(lut_sample(aff, c, LutInterpolation::Trilinear), affine(c)));
    }
    expectTrue(affT < 2e-6f && affL < 2e-6f, "affine LUT: tetrahedral and trilinear exact");

    // Nonlinear: accuracy, neutral axis, convergence.
    f32 maxT[2] = {};
    f32 maxL[2] = {};
    f64 meanT[2] = {};
    f64 meanL[2] = {};
    f32 greyT = 0.f;
    f32 greyL = 0.f;
    const u32 sizes[2] = {kLutSizeDefault, kLutSizeHigh};
    for (u32 s = 0; s < 2u; ++s) {
        const Lut3D lut = lutOf(sizes[s], nonlinearGrade);
        std::mt19937 r2(7);
        for (u32 i = 0; i < 50000u; ++i) {
            const Vec3 c{uni(r2), uni(r2), uni(r2)};
            const Vec3 ref = nonlinearGrade(c);
            const f32 et = maxAbs(lut_sample(lut, c), ref);
            const f32 el = maxAbs(lut_sample(lut, c, LutInterpolation::Trilinear), ref);
            maxT[s] = std::max(maxT[s], et);
            maxL[s] = std::max(maxL[s], el);
            meanT[s] += et;
            meanL[s] += el;
            if (s == 0u) {
                const f32 g = uni(r2);
                const Vec3 gt = lut_sample(lut, {g, g, g});
                const Vec3 gl = lut_sample(lut, {g, g, g}, LutInterpolation::Trilinear);
                greyT = std::max({greyT, std::fabs(gt.x - gt.y), std::fabs(gt.y - gt.z), std::fabs(gt.x - g)});
                greyL = std::max({greyL, std::fabs(gl.x - gl.y), std::fabs(gl.y - gl.z), std::fabs(gl.x - g)});
            }
        }
        meanT[s] /= 50000.0;
        meanL[s] /= 50000.0;
    }
    std::printf("  LUT accuracy vs analytic (max / mean abs error):\n");
    for (u32 s = 0; s < 2u; ++s) {
        std::printf("    %u^3  tetrahedral %.3g / %.3g   trilinear %.3g / %.3g\n", sizes[s], maxT[s], meanT[s], maxL[s],
                    meanL[s]);
    }
    std::printf("    neutral axis (32^3): tetrahedral %.3g, trilinear %.3g\n", greyT, greyL);
    expectTrue(greyT < 2e-6f, "tetrahedral interpolation preserves the neutral axis exactly");
    expectTrue(greyL > greyT, "trilinear drifts off the neutral axis (tetrahedral does not)");
    expectTrue(maxT[0] < 2e-3f && maxT[1] < maxT[0] && meanT[1] < meanT[0], "tetrahedral error small and converges 32 -> 64");
    expectTrue(maxT[0] <= maxL[0] * 1.5f && meanT[0] <= meanL[0] * 1.5, "tetrahedral accuracy comparable to trilinear");

    // .cube import: hand-written known file (Adobe spec keywords, comments, blank lines, DOMAIN).
    const char* known = "# comment line\n"
                        "TITLE \"FUSE known 2x2x2\"\n"
                        "\n"
                        "LUT_3D_SIZE 2\n"
                        "DOMAIN_MIN 0 0 0\n"
                        "DOMAIN_MAX 1 1 1\n"
                        "# red fastest\n"
                        "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n0.5 0.25 1e-1\n";
    Lut3D k;
    CubeParseResult cr;
    expectTrue(lut_parse_cube(known, k, cr) && k.size == 2u && k.title == "FUSE known 2x2x2" && !cr.resampled,
               ".cube known file parses (size, title)");
    expectTrue(k.at(1, 0, 0).x == 1.f && k.at(0, 1, 0).y == 1.f && k.at(0, 0, 1).z == 1.f && k.at(1, 1, 1).x == 0.5f &&
                   k.at(1, 1, 1).y == 0.25f && k.at(1, 1, 1).z == 0.1f,
               ".cube data order is red fastest; values exact");
    expectNear(lut_sample(k, {1.f, 1.f, 1.f}).y, 0.25, 0, ".cube corner sample");
    Lut3D rt;
    CubeParseResult cr2;
    expectTrue(lut_parse_cube(lut_write_cube(k), rt, cr2) && rt.title == k.title &&
                   std::memcmp(rt.data.data(), k.data.data(), k.data.size() * sizeof(Vec3)) == 0,
               ".cube write -> read is lossless");
    // DOMAIN remap: file domain [0, 2] of an identity-in-index table doubles the input spacing.
    const char* domain = "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"
                         "0 0 0\n2 0 0\n0 2 0\n2 2 0\n0 0 2\n2 0 2\n0 2 2\n2 2 2\n";
    Lut3D dl;
    expectTrue(lut_parse_cube(domain, dl, cr, 9) && cr.resampled && dl.size == 9u, "DOMAIN_MAX 2 is baked to 9^3");
    expectNear(lut_sample(dl, {0.5f, 0.25f, 1.f}).x, 0.5, 1e-6, "DOMAIN remap keeps identity mapping");
    // 1D-only LUT (per-channel squares) baked to 3D.
    std::string oneD = "TITLE \"sq\"\nLUT_1D_SIZE 5\n";
    for (u32 i = 0; i < 5u; ++i) {
        const f32 x = static_cast<f32>(i) / 4.f;
        oneD += std::to_string(x * x) + " " + std::to_string(x * x) + " " + std::to_string(x) + "\n";
    }
    Lut3D l1;
    expectTrue(lut_parse_cube(oneD, l1, cr, 5) && l1.size == 5u, "1D .cube baked into a 3D LUT");
    expectNear(lut_sample(l1, {0.5f, 0.75f, 0.25f}).x, 0.25, 1e-6, "1D LUT red channel");
    expectNear(lut_sample(l1, {0.5f, 0.75f, 0.25f}).y, 0.5625, 1e-6, "1D LUT green channel");
    // Shaper (1D) + 3D with input ranges.
    std::string shaper = "LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE 0 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n";
    for (u32 i = 0; i < 8u; ++i) {
        shaper += std::to_string(i & 1u) + " " + std::to_string((i >> 1u) & 1u) + " " + std::to_string((i >> 2u) & 1u) + "\n";
    }
    Lut3D ls;
    expectTrue(lut_parse_cube(shaper, ls, cr, 3), "shaper 1D + 3D parses");
    expectNear(lut_sample(ls, {1.f, 0.5f, 0.f}).x, 0.5, 1e-6, "shaper range [0, 2] halves the input");
    // Errors.
    const auto err = [](const char* t) {
        Lut3D l;
        CubeParseResult r;
        lut_parse_cube(t, l, r);
        return r.error;
    };
    expectTrue(err("LUT_3D_SIZE 2\n0 0 0\n").find("3D table has 1 entries, expected 8") != std::string::npos,
               ".cube wrong entry count");
    expectTrue(err("LUT_3D_SIZE 2\nFOO 1\n").find("line 2: unknown keyword 'FOO'") == 0u, ".cube unknown keyword with line");
    expectTrue(err("LUT_3D_SIZE 2\n0 0\n").find("line 2") == 0u, ".cube malformed row");
    expectTrue(err("0 0 0\n").find("missing LUT_3D_SIZE") != std::string::npos, ".cube missing size");
    // Sample LUT files.
    Lut3D warm;
    expectTrue(lut_load_cube((samplesDir() + "/luts/fuse_warm_print.cube").c_str(), warm, cr) && warm.size == 17u &&
                   warm.title == "FUSE warm print",
               "sample .cube loads");

    // Generation from grading params.
    LookResolved g = neutral;
    g.grade.saturation = 0.f;
    lut_generate_grade(g, 17, gen);
    f32 chroma = 0.f;
    for (const Vec3& v : gen.data) {
        chroma = std::max({chroma, std::fabs(v.x - v.y), std::fabs(v.y - v.z)});
    }
    expectTrue(chroma < 1e-6f, "saturation 0 -> every lattice point grey");
    g = neutral;
    g.grade.gain = {2.f, 1.f, 1.f};
    lut_generate_grade(g, 17, gen);
    const Vec3 half = gen.data[lk::lut_index(4, 4, 4, 17)]; // encoded 0.25
    const f32 expectR = lk::srgb_encode(std::min(2.f * lk::srgb_decode(0.25f), 1.f));
    expectNear(half.x, expectR, 1e-6, "gain doubles linear red");
    expectNear(half.y, 0.25, 1e-6, "gain leaves green");
    // White balance: the Planckian white of the setting becomes neutral.
    f32 m[9];
    white_balance_matrix(3200.f, 0.f, m);
    f32 warmWhite[9];
    white_balance_matrix(6504.f, 0.f, warmWhite);
    expectTrue(std::fabs(warmWhite[0] - 1.f) < 1e-6f && std::fabs(warmWhite[1]) < 1e-6f && std::fabs(warmWhite[4] - 1.f) < 1e-6f,
               "white balance at 6504 K is the identity");
    // Rec.709 rgb of the 3200 K Planckian white (xy from the same fit) -> neutral after the matrix.
    f64 x = 0.0, y = 0.0;
    white_balance_source_xy(3200.f, 0.f, x, y);
    expectTrue(std::fabs(x - 0.42318 - (0.31271 - 0.31343)) < 2e-3 && std::fabs(y - 0.39907 - (0.32902 - 0.32361)) < 2e-3,
               "3200 K source white on the (D65-anchored) Planckian locus");
    const f64 X = x / y, Z = (1.0 - x - y) / y;
    const Vec3 rgb{static_cast<f32>(3.2404542 * X - 1.5371385 - 0.4985314 * Z),
                   static_cast<f32>(-0.9692660 * X + 1.8760108 + 0.0415560 * Z),
                   static_cast<f32>(0.0556434 * X - 0.2040259 + 1.0572252 * Z)};
    const Vec3 out{m[0] * rgb.x + m[1] * rgb.y + m[2] * rgb.z, m[3] * rgb.x + m[4] * rgb.y + m[5] * rgb.z,
                   m[6] * rgb.x + m[7] * rgb.y + m[8] * rgb.z};
    char wbLabel[160];
    std::snprintf(wbLabel, sizeof(wbLabel), "white balance 3200 K neutralises tungsten white (rgb %.5f %.5f %.5f)",
                  static_cast<f64>(out.x), static_cast<f64>(out.y), static_cast<f64>(out.z));
    expectTrue(std::fabs(out.x / out.y - 1.f) < 1e-3f && std::fabs(out.z / out.y - 1.f) < 1e-3f, wbLabel);
    white_balance_matrix(8000.f, 0.f, m);
    expectTrue(m[0] + m[1] + m[2] > m[6] + m[7] + m[8], "temperature above 6504 K warms (red gain > blue gain)");

    // LUT blending.
    const Lut3D idl = Lut3D::identity(9);
    Lut3D inv = idl;
    for (Vec3& v : inv.data) {
        v = Vec3{1.f - v.x, 1.f - v.y, 1.f - v.z};
    }
    Lut3D mid;
    expectTrue(lut_lerp(idl, inv, 0.5f, mid), "lut_lerp runs");
    f32 midErr = 0.f;
    for (const Vec3& v : mid.data) {
        midErr = std::max(midErr, maxAbs(v, {0.5f, 0.5f, 0.5f}));
    }
    expectTrue(midErr < 1e-6f, "blend(identity, invert, 0.5) = 0.5 grey");
    const Lut3D* three[3] = {&idl, &inv, &idl};
    const f32 wts[3] = {0.2f, 0.3f, 0.5f};
    Lut3D mix3;
    lut_blend(three, wts, 3, mix3);
    expectNear(lut_sample(mix3, {0.2f, 0.4f, 0.9f}).x, 0.7 * 0.2 + 0.3 * 0.8, 1e-6, "3-way weighted LUT blend");

    // Bake / blend kernels: CpuReference == CpuParallel bit-exact.
    g = neutral;
    g.grade.temperature_k = 5000.f;
    g.grade.contrast = 1.3f;
    g.grade.saturation = 1.4f;
    g.grade.gamma = {1.1f, 1.f, 0.9f};
    g.grade.lift = {0.02f, 0.f, -0.01f};
    g.grade.curve[1] = {0.2f, 0.25f, 0.3f};
    Lut3D refL;
    lut_generate_grade(g, 33, refL, kernel::Backend::CpuReference);
    bool bakeParity = true;
    bool blendParity = true;
    Lut3D refB;
    lut_blend(three, wts, 3, refB, kernel::Backend::CpuReference);
    for (u32 workers : {0u, 2u, 4u}) {
        setWorkers(workers);
        Lut3D par;
        lut_generate_grade(g, 33, par, kernel::Backend::CpuParallel);
        bakeParity = bakeParity && std::memcmp(par.data.data(), refL.data.data(), refL.data.size() * sizeof(Vec3)) == 0;
        Lut3D pb;
        lut_blend(three, wts, 3, pb, kernel::Backend::CpuParallel);
        blendParity = blendParity && std::memcmp(pb.data.data(), refB.data.data(), refB.data.size() * sizeof(Vec3)) == 0;
    }
    expectTrue(bakeParity, "look_lut_bake: CpuReference == CpuParallel bit-exact (0/2/4 workers)");
    expectTrue(blendParity, "look_lut_blend: CpuReference == CpuParallel bit-exact (0/2/4 workers)");
    // LUT bake with blended external LUTs == sampling the explicitly blended LUT.
    const std::filesystem::path dir = scratchDir();
    writeBlendLuts(dir);
    LookSystem sys;
    LookParseResult diag;
    sys.setConfig({17u, kernel::Backend::CpuParallel});
    sys.loadFromString(kBlendLook, dir.string(), diag);
    LookFrameInput in{};
    in.time_of_day_hours = 13.f;                        // ToD: half inv (04:00 key), half base id LUT
    in.camera_position = {5.f + 4.f * 0.5f, 0.f, 0.f}; // "hi" volume (inv LUT) alpha 0.5
    const LookEvaluation& e = sys.evaluate(in);
    expectNear(e.lut_weights[1], 0.25, 1e-6, "ToD + volume LUT weights (id)");
    expectNear(e.lut_weights[2], 0.75, 1e-6, "ToD + volume LUT weights (inv)");
    f32 bakeErr = 0.f;
    for (u32 i = 0; i < 17u * 17u * 17u; i += 7u) {
        const Vec3 xx = Lut3D::identity(17).data[i];
        Vec3 expect = xx * e.lut_weights[0];
        for (u32 s = 1; s < e.lut_slot_count; ++s) {
            expect = expect + lut_sample(sys.lutSlot(s), xx) * e.lut_weights[s];
        }
        bakeErr = std::max(bakeErr, maxAbs(sys.gradeLut().data[i], expect));
    }
    expectTrue(bakeErr < 2e-6f, "baked LUT == weighted blend of the profile LUTs");
}

// ---------------------------------------------------------------------------------------------
// 4. Kernels

LookResolved everythingOn() {
    LookParamBlock b = look_default_params();
    b.set(LookParam::DirtEnabled, 1.f);
    b.set(LookParam::FlareEnabled, 1.f);
    b.set(LookParam::FlareIntensity, 0.8f);
    b.set(LookParam::SharpenEnabled, 1.f);
    b.set(LookParam::CaEnabled, 1.f);
    b.set(LookParam::CaIntensity, 0.01f);
    b.set(LookParam::VignetteRoundness, 0.5f);
    b.setColor(LookParam::VignetteColor, {0.01f, 0.f, 0.02f});
    b.set(LookParam::GrainResponse, 0.5f);
    b.set(LookParam::GradeSaturation, 1.2f);
    b.set(LookParam::GradeTemperatureK, 7000.f);
    b.setColor(LookParam::GradeCurve1, {0.22f, 0.25f, 0.27f});
    b.set(LookParam::ExpAutoEnabled, 0.f);
    return look_resolve(b);
}

std::vector<Vec3> runChain(kernel::Backend backend, LookOutputEncoding enc, const LookResolved& look,
                           const Lut3D& lut, const std::vector<Vec3>& hdr, u32 w, u32 h,
                           const LookEffectGraph& graph = LookEffectGraph::makeDefault()) {
    LookPostChain chain;
    chain.init({w, h, enc, 0.f, backend});
    std::vector<Vec3> out(static_cast<size_t>(w) * h);
    LookChainInput in{};
    in.hdr = hdr.data();
    in.frame_seed = 0xC0FFEEull;
    chain.process(in, graph, look, lut, out.data());
    return out;
}

void testChainParity() {
    const u32 w = 83, h = 61; // partial edge workgroups
    const std::vector<Vec3> hdr = makeHdrFrame(w, h);
    const LookResolved look = everythingOn();
    Lut3D lut;
    lut_generate_grade(look, 17, lut, kernel::Backend::CpuReference);
    setWorkers(0);
    for (LookOutputEncoding enc : {LookOutputEncoding::Srgb, LookOutputEncoding::Hdr10Pq, LookOutputEncoding::ScRgb}) {
        const std::vector<Vec3> ref = runChain(kernel::Backend::CpuReference, enc, look, lut, hdr, w, h);
        bool ok = true;
        u64 mismatches = 0;
        for (u32 workers : {0u, 2u, 4u}) {
            setWorkers(workers);
            const std::vector<Vec3> par = runChain(kernel::Backend::CpuParallel, enc, look, lut, hdr, w, h);
            const kernel::ParityReport r = kernel::compare_bitwise(std::span<const Vec3>(ref), std::span<const Vec3>(par));
            ok = ok && r.ok;
            mismatches += r.mismatches;
        }
        char label[160];
        std::snprintf(label, sizeof(label),
                      "full look chain (%s): CpuReference == CpuParallel bit-exact at 0/2/4 workers (%llu mismatches)",
                      look_output_encoding_name(enc), static_cast<unsigned long long>(mismatches));
        expectTrue(ok, label);
    }
    // Every node ran.
    LookPostChain chain;
    chain.init({w, h, LookOutputEncoding::Srgb, 0.f, kernel::Backend::CpuParallel});
    std::vector<Vec3> out(static_cast<size_t>(w) * h);
    LookChainInput in{};
    in.hdr = hdr.data();
    in.frame_seed = 3;
    chain.process(in, LookEffectGraph::makeDefault(), look, lut, out.data());
    const LookChainStats& st = chain.lastStats();
    expectTrue(st.executed_count == 11u && !st.executed_effect(LookEffect::DepthOfField) &&
                   !st.executed_effect(LookEffect::MotionBlur) &&
                   !st.executed_effect(LookEffect::AmbientOcclusion) && st.executed_effect(LookEffect::LensFlare),
               "chain executes every enabled node (DoF/motion blur skipped without depth/velocity, AO external)");
    kernel::KernelStats ks{};
    expectTrue(kernel::find_kernel_stats("look_vignette", ks) && ks.launches > 0u && ks.failed_launches == 0u,
               "kernel stats recorded under look_vignette");
    const kernel::LaunchRecord lr = kernel::last_launch();
    expectTrue(std::strcmp(lr.name, "look_output_encode") == 0 && lr.items == static_cast<u64>(w) * h,
               "output encode is the last launch, one item per pixel");
    // GPU request without a device falls back to CpuParallel and matches.
    const std::vector<Vec3> ref = runChain(kernel::Backend::CpuReference, LookOutputEncoding::Srgb, look, lut, hdr, w, h);
    const std::vector<Vec3> viaCuda = runChain(kernel::Backend::Cuda, LookOutputEncoding::Srgb, look, lut, hdr, w, h);
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        expectTrue(kernel::last_launch().backend == kernel::Backend::CpuParallel, "CUDA request without device falls back");
        expectTrue(kernel::compare_bitwise(std::span<const Vec3>(ref), std::span<const Vec3>(viaCuda)).ok,
                   "fallback output == CpuReference");
    }
    // Sharpen hook replaces the built-in kernel.
    struct Hook {
        static bool copy(const Vec3* src, Vec3* dst, u32 ww, u32 hh, f32, void* user) {
            std::copy(src, src + static_cast<size_t>(ww) * hh, dst);
            ++*static_cast<u32*>(user);
            return true;
        }
    };
    u32 calls = 0;
    chain.setSharpenHook(&Hook::copy, &calls);
    chain.process(in, LookEffectGraph::makeDefault(), look, lut, out.data());
    expectTrue(calls == 1u && chain.lastStats().sharpen_hook_used, "sharpen hook (CAS seam) is used when registered");

    // The upscaler module's AMD CAS port through the bridge: same result as calling run_cas directly on the
    // chain's pre-sharpen image (graph ends right after sharpen so the output is observable in scRGB 80 nits).
#if FUSE_LOOK_TEST_HAS_CAS
    expectTrue(LookCasSharpener::available(), "CAS bridge available in this build");
    LookCasSharpener cas;
    cas.init(w, h);
    LookEffectGraph g;
    for (LookEffect e : {LookEffect::Exposure, LookEffect::ToneMap, LookEffect::Sharpen, LookEffect::OutputTransform}) {
        g.push(e);
    }
    LookPostChain c2;
    c2.init({w, h, LookOutputEncoding::ScRgb, 0.f, kernel::Backend::CpuParallel});
    LookParamBlock pb = look_default_params();
    pb.set(LookParam::SharpenEnabled, 1.f);
    pb.set(LookParam::SharpenSharpness, 0.7f);
    pb.set(LookParam::ExpAutoEnabled, 0.f);
    pb.set(LookParam::OutPaperWhiteNits, 80.f);
    const LookResolved sl = look_resolve(pb);
    std::vector<Vec3> noSharpen(out.size());
    LookEffectGraph g0;
    for (LookEffect e : {LookEffect::Exposure, LookEffect::ToneMap, LookEffect::OutputTransform}) {
        g0.push(e);
    }
    c2.process(in, g0, sl, lut, noSharpen.data());
    c2.setSharpenHook(&LookCasSharpener::hook, &cas);
    std::vector<Vec3> viaBridge(out.size());
    c2.process(in, g, sl, lut, viaBridge.data());
    std::vector<fuse::math::Vec4> s4(out.size()), d4(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        s4[i] = fuse::math::Vec4{noSharpen[i], 1.f};
    }
    fuse::renderer::upscale::run_cas({s4.data(), w, h}, {d4.data(), w, h}, 0.7f);
    f32 casErr = 0.f;
    f32 casDelta = 0.f;
    for (size_t i = 0; i < out.size(); ++i) {
        casErr = std::max(casErr, maxAbs(viaBridge[i], {d4[i].x, d4[i].y, d4[i].z}));
        casDelta = std::max(casDelta, maxAbs(viaBridge[i], noSharpen[i]));
    }
    expectTrue(cas.calls() == 1u && c2.lastStats().sharpen_hook_used && casErr == 0.f && casDelta > 1e-3f,
               "sharpen node runs the upscaler module's AMD CAS through LookCasSharpener (== run_cas)");
#endif
}

void testExistingParity() {
    // Bloom kernels == renderer::bloom_image / bloom_composite, bit for bit.
    const u32 w = 97, h = 45;
    std::vector<Vec3> hdr = makeHdrFrame(w, h);
    fuse::renderer::BloomParams bp{};
    std::vector<Vec3> expected;
    fuse::renderer::bloom_composite(hdr.data(), w, h, bp, expected);
    LookParamBlock b = look_default_params();
    b.set(LookParam::BloomThreshold, bp.threshold);
    b.set(LookParam::BloomKnee, bp.knee);
    b.set(LookParam::BloomIntensity, bp.intensity);
    b.set(LookParam::BloomScatter, bp.scatter);
    const LookResolved look = look_resolve(b);
    LookEffectGraph g;
    g.push(LookEffect::Bloom);
    g.push(LookEffect::ToneMap);
    g.push(LookEffect::OutputTransform);
    LookPostChain chain;
    chain.init({w, h, LookOutputEncoding::ScRgb, 0.f, kernel::Backend::CpuParallel});
    std::vector<Vec3> bloomOnly;
    fuse::renderer::bloom_image(hdr.data(), w, h, bp, bloomOnly);
    std::vector<Vec3> out(static_cast<size_t>(w) * h);
    LookChainInput in{};
    in.hdr = hdr.data();
    chain.process(in, g, look, Lut3D::identity(2), out.data());
    expectTrue(chain.lastStats().bloom_levels == fuse::renderer::bloom_level_count(w, h, bp),
               "look bloom pyramid depth == bloom_level_count");
    expectTrue(kernel::compare_bitwise(std::span<const Vec3>(chain.bloomBuffer()), std::span<const Vec3>(bloomOnly)).ok,
               "look bloom kernels == renderer::bloom_image (bit-exact)");

    // Tone map kernels == renderer::apply_tone_map; grain == film_grain_noise; vignette ~ vignette_factor.
    bool toneOk = true;
    for (u32 op = 0; op < 4u; ++op) {
        for (u32 i = 0; i < 2000u; ++i) {
            const f32 v = std::pow(10.f, -4.f + 7.f * static_cast<f32>(i) / 2000.f);
            const Vec3 e = fuse::renderer::apply_tone_map({v, v * 0.5f, v * 2.f}, static_cast<fuse::renderer::ToneMapper>(op));
            toneOk = toneOk && lk::tonemap_channel(v, op) == e.x && lk::tonemap_channel(v * 0.5f, op) == e.y &&
                     lk::tonemap_channel(v * 2.f, op) == e.z;
        }
    }
    expectTrue(toneOk, "look tonemap operators == renderer::apply_tone_map (bit-exact)");
    bool grainOk = true;
    for (u32 y = 0; y < 50u; ++y) {
        for (u32 x = 0; x < 50u; ++x) {
            grainOk = grainOk && lk::grain_noise(77u, x, y) == fuse::renderer::film_grain_noise(77u, x, y);
        }
    }
    expectTrue(grainOk, "look grain noise == renderer::film_grain_noise (bit-exact)");
    f32 vigErr = 0.f;
    for (u32 y = 0; y < 90u; ++y) {
        for (u32 x = 0; x < 160u; ++x) {
            vigErr = std::max(vigErr, std::fabs(lk::vignette_gain(0.3f, 2.f, 0.f, x, y, 160, 90) -
                                                fuse::renderer::vignette_factor(0.3f, x, y, 160, 90)));
        }
    }
    expectTrue(vigErr < 1e-6f, "vignette (roundness 0, falloff 2) == legacy vignette_factor");
    expectNear(lk::srgb_encode(0.18f), fuse::renderer::linear_to_srgb({0.18f, 0.f, 0.f}).x, 0, "sRGB OETF matches");
}

void testAnalyticKernels() {
    // Vignette falloff: gain = 1 - k r^p with r normalised to the frame corner.
    const u32 W = 200, H = 100;
    const f32 k = 0.6f;
    const f32 p = 3.f;
    for (f32 roundness : {0.f, 1.f}) {
        f32 err = 0.f;
        for (u32 y = 0; y < H; y += 7u) {
            for (u32 x = 0; x < W; x += 5u) {
                const f64 u = (x + 0.5) / W * 2.0 - 1.0;
                const f64 v = (y + 0.5) / H * 2.0 - 1.0;
                const f64 xs = 1.0 + (2.0 - 1.0) * roundness;
                const f64 r = std::sqrt((u * u * xs * xs + v * v) / (xs * xs + 1.0));
                err = std::max(err, static_cast<f32>(std::fabs(lk::vignette_gain(k, p, roundness, x, y, W, H) -
                                                               (1.0 - k * std::pow(r, p)))));
            }
        }
        expectTrue(err < 2e-6f, "vignette matches 1 - k r^p");
    }
    expectTrue(lk::vignette_gain(k, p, 1.f, 149, 50, W, H) == lk::vignette_gain(k, p, 1.f, 50, 50, W, H),
               "vignette symmetric");
    // Round (roundness 1): equal gain at equal pixel distance horizontally and vertically.
    expectNear(lk::vignette_gain(k, 2.f, 1.f, 100 + 30, 50, W, H), lk::vignette_gain(k, 2.f, 1.f, 100, 50 + 30, W, H), 1e-6,
               "roundness 1 is circular in pixels");
    expectNear(lk::vignette_gain(k, p, 0.f, 0, 0, 4096, 4096), 1.0 - k * std::pow((4095.0 / 4096.0), p), 1e-5,
               "vignette at the corner ~ 1 - k");

    // Chromatic aberration: red sampled at c + d(1+k) -> on a horizontal ramp R(x) = x the offset is k * dx.
    const u32 cw = 128, ch = 64;
    std::vector<Vec3> ramp(static_cast<size_t>(cw) * ch);
    for (u32 y = 0; y < ch; ++y) {
        for (u32 x = 0; x < cw; ++x) {
            ramp[static_cast<size_t>(y) * cw + x] = Vec3{static_cast<f32>(x) + 0.5f, 0.25f, static_cast<f32>(x) + 0.5f};
        }
    }
    std::vector<Vec3> caOut(ramp.size());
    const f32 kca = 0.02f;
    kernel::launch(kernel::Backend::CpuReference, {lk::ChromaticAberrationKernel::kName, kernel::extent2(cw, ch), lk::kImageWorkgroup},
                   lk::ChromaticAberrationKernel{},
                   lk::ChromaticAberrationParams{{ramp.data(), static_cast<u32>(ramp.size())},
                                                 {caOut.data(), static_cast<u32>(caOut.size())}, cw, ch, kca});
    f32 caErr = 0.f;
    bool greenExact = true;
    for (u32 y = 0; y < ch; ++y) {
        for (u32 x = 4; x < cw - 4u; ++x) {
            const size_t i = static_cast<size_t>(y) * cw + x;
            const f32 dx = static_cast<f32>(x) + 0.5f - 0.5f * cw;
            caErr = std::max({caErr, std::fabs((caOut[i].x - ramp[i].x) - kca * dx), std::fabs((caOut[i].z - ramp[i].z) + kca * dx)});
            greenExact = greenExact && caOut[i].y == ramp[i].y;
        }
    }
    expectTrue(caErr < 1e-4f, "CA radial offset: red +k*d, blue -k*d (pixels)");
    expectTrue(greenExact, "CA leaves green untouched");

    // Lens dirt: color += bloom * dirt * intensity * tint.
    std::vector<Vec3> col(64, Vec3{0.1f, 0.2f, 0.3f});
    std::vector<Vec3> bloom(64, Vec3{1.f, 2.f, 4.f});
    std::vector<f32> dirt(4, 0.5f);
    kernel::launch(kernel::Backend::CpuReference, {lk::LensDirtKernel::kName, kernel::extent2(8, 8), lk::kImageWorkgroup},
                   lk::LensDirtKernel{},
                   lk::LensDirtParams{{col.data(), 64}, {bloom.data(), 64}, {dirt.data(), 4}, 8, 8, 2, 2, 2.f, {1.f, 0.5f, 0.f}});
    expectTrue(maxAbs(col[27], {0.1f + 1.f, 0.2f + 1.f, 0.3f}) < 1e-6f, "lens dirt adds bloom * dirt * intensity * tint");
    std::fill(dirt.begin(), dirt.end(), 0.f);
    std::fill(col.begin(), col.end(), Vec3{0.1f, 0.2f, 0.3f});
    kernel::launch(kernel::Backend::CpuReference, {lk::LensDirtKernel::kName, kernel::extent2(8, 8), lk::kImageWorkgroup},
                   lk::LensDirtKernel{},
                   lk::LensDirtParams{{col.data(), 64}, {bloom.data(), 64}, {dirt.data(), 4}, 8, 8, 2, 2, 2.f, {1.f, 1.f, 1.f}});
    expectTrue(col[10].x == 0.1f && col[10].z == 0.3f, "zero dirt leaves the image unchanged");

    // Lens flare: zero input -> zero; point-symmetric input -> point-symmetric flare; ghost position.
    const u32 fw = 64, fh = 64;
    std::vector<Vec3> bright(static_cast<size_t>(fw) * fh);
    std::vector<Vec3> flare(bright.size());
    lk::LensFlareParams fp{};
    fp.bright = {bright.data(), static_cast<u32>(bright.size())};
    fp.dst = {flare.data(), static_cast<u32>(flare.size())};
    fp.w = fw;
    fp.h = fh;
    const auto runFlare = [&]() {
        kernel::launch(kernel::Backend::CpuReference, {lk::LensFlareKernel::kName, kernel::extent2(fw, fh), lk::kImageWorkgroup},
                       lk::LensFlareKernel{}, fp);
    };
    runFlare();
    bool zero = true;
    for (const Vec3& v : flare) {
        zero = zero && v.x == 0.f && v.y == 0.f && v.z == 0.f;
    }
    expectTrue(zero, "flare of a black bright pass is exactly zero");
    bright[static_cast<size_t>(20) * fw + 12] = Vec3{50.f, 50.f, 50.f};
    bright[static_cast<size_t>(fh - 1 - 20) * fw + (fw - 1 - 12)] = Vec3{50.f, 50.f, 50.f};
    runFlare();
    f32 asym = 0.f;
    f32 peak = 0.f;
    for (u32 y = 0; y < fh; ++y) {
        for (u32 x = 0; x < fw; ++x) {
            const Vec3 a = flare[static_cast<size_t>(y) * fw + x];
            const Vec3 b = flare[static_cast<size_t>(fh - 1 - y) * fw + (fw - 1 - x)];
            asym = std::max(asym, std::fabs(a.y - b.y));
            peak = std::max(peak, a.y);
        }
    }
    expectTrue(peak > 0.f && asym <= 1e-4f * peak, "flare is point-symmetric for point-symmetric input");
    std::fill(bright.begin(), bright.end(), Vec3{});
    const u32 sx = 24, sy = 32;
    bright[static_cast<size_t>(sy) * fw + sx] = Vec3{100.f, 100.f, 100.f};
    fp.ghost_count = 1;
    fp.ghost_spacing = 0.5f;
    fp.halo_intensity = 0.f;
    fp.chromatic_shift = 0.f;
    runFlare();
    u32 bestX = 0;
    f32 best = 0.f;
    for (u32 x = 0; x < fw; ++x) {
        const f32 v = flare[static_cast<size_t>(sy) * fw + x].y;
        if (v > best) {
            best = v;
            bestX = x;
        }
    }
    // Ghost 1 at u satisfies u + (0.5 - u) s = u0  ->  u = (u0 - 0.5 s) / (1 - s).
    const f32 u0 = (sx + 0.5f) / fw;
    const f32 ug = (u0 - 0.5f * 0.5f) / (1.f - 0.5f);
    expectTrue(best > 0.f && std::fabs((static_cast<f32>(bestX) + 0.5f) / fw - ug) <= 1.5f / fw,
               "ghost appears at the analytic mirrored position");

    // PQ (ST 2084) encode vs double-precision reference and published code values.
    const auto pqRef = [](f64 nits) {
        const f64 m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
        const f64 c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
        const f64 y = std::pow(nits / 10000.0, m1);
        return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
    };
    f64 pqErr = 0.0;
    for (f64 nits : {0.0, 0.005, 0.1, 1.0, 10.0, 48.0, 80.0, 100.0, 203.0, 400.0, 1000.0, 2000.0, 4000.0, 10000.0}) {
        pqErr = std::max(pqErr, std::fabs(lk::pq_encode_nits(static_cast<f32>(nits)) - pqRef(nits)));
    }
    char pqLabel[128];
    std::snprintf(pqLabel, sizeof(pqLabel), "PQ encode == ST 2084 double reference (max |err| %.3g < 1e-5)", pqErr);
    expectTrue(pqErr < 1e-5, pqLabel); // f32 pow; 1e-5 = 1% of a 10-bit PQ code
    expectTrue(std::lround(lk::pq_encode_nits(100.f) * 1023.f) == 520 && std::lround(lk::pq_encode_nits(1000.f) * 1023.f) == 769 &&
                   std::lround(lk::pq_encode_nits(10000.f) * 1023.f) == 1023 && std::lround(lk::pq_encode_nits(0.f) * 1023.f) == 0,
               "PQ 10-bit full-range codes: 100 nits -> 520, 1000 -> 769, 10000 -> 1023, 0 -> 0");
    expectNear(lk::pq_encode_nits(203.f), 0.58069, 5e-5, "BT.2408 reference white 203 nits ~ 58% PQ");
    f64 rtErr = 0.0;
    for (f32 n = 0.5f; n < 10000.f; n *= 1.7f) {
        rtErr = std::max(rtErr, static_cast<f64>(std::fabs(lk::pq_decode_nits(lk::pq_encode_nits(n)) - n) / n));
    }
    expectTrue(rtErr < 5e-4, "PQ decode(encode(n)) round trip (relative)");
    // HDR10 output: display white at paper white -> PQ(paper white); scRGB scaling; peak clamp.
    const Vec3 hdr10 = lk::output_encode({1.f, 1.f, 1.f}, lk::kEncodeHdr10Pq, 203.f, 1000.f);
    expectNear(hdr10.y, pqRef(203.0), 5e-6, "HDR10: 1.0 = paper white nits (Rec.2020 white preserved)");
    const Vec3 clipped = lk::output_encode({100.f, 100.f, 100.f}, lk::kEncodeHdr10Pq, 203.f, 1000.f);
    expectNear(clipped.x, pqRef(1000.0), 1e-5, "HDR10 clamps at the display peak");
    expectNear(lk::output_encode({1.f, 0.5f, 0.f}, lk::kEncodeScRgb, 80.f, 1000.f).x, 1.0, 0, "scRGB: 80 nits paper white -> 1.0");
    expectNear(lk::output_encode({1.f, 0.5f, 0.f}, lk::kEncodeScRgb, 200.f, 1000.f).y, 1.25, 1e-6, "scRGB: 200 nits paper white");
    expectNear(lk::output_encode({100.f, 0.f, 0.f}, lk::kEncodeScRgb, 200.f, 1000.f).x, 12.5, 1e-6, "scRGB: peak clamp (1000 / 80)");
    // HDR shoulder: identity below lmax/2, C1 at the knee, bounded by lmax.
    const f32 lmax = 5.f;
    expectTrue(lk::tonemap_hdr_shoulder(0.18f, lmax) == 0.18f, "HDR shoulder keeps mid grey");
    const f32 ks = 2.5f;
    const f32 eps = 1e-3f;
    const f32 slope = (lk::tonemap_hdr_shoulder(ks + eps, lmax) - lk::tonemap_hdr_shoulder(ks, lmax)) / eps;
    expectNear(slope, 1.0, 2e-3, "HDR shoulder is C1 at the knee");
    expectTrue(lk::tonemap_hdr_shoulder(1e6f, lmax) <= lmax && lk::tonemap_hdr_shoulder(20.f, lmax) > 4.9f,
               "HDR shoulder approaches the peak asymptotically");
}

// ---------------------------------------------------------------------------------------------
// 5. Integration with the existing post stack

void testPostStackIntegration() {
    const u32 w = 64, h = 40;
    const std::vector<Vec3> hdr = makeHdrFrame(w, h);
    LookParamBlock b = look_default_params();
    b.set(LookParam::ExpAutoEnabled, 0.f);
    b.set(LookParam::MbEnabled, 0.f);
    const LookResolved look = look_resolve(b);

    fuse::renderer::PostStack stack;
    stack.init({w, h});
    apply_look_to_post_stack(look, stack);
    expectTrue(stack.bloom().params().intensity == look.bloom.intensity && stack.colorGrade().params().vignette == 0.3f &&
                   stack.colorGrade().params().film_grain == 0.02f && !stack.autoExposure().params().enabled &&
                   stack.colorGrade().params().tone_mapper == fuse::renderer::ToneMapper::ACES,
               "apply_look_to_post_stack feeds bloom / grade / exposure parameters");
    fuse::renderer::PostFrameInput pin{};
    pin.hdr = hdr.data();
    pin.width = w;
    pin.height = h;
    pin.frame_seed = 99;
    std::vector<Vec3> legacy;
    expectTrue(stack.processFrame(pin, legacy), "existing PostStack::processFrame still runs with look parameters");

    LookPostChain chain;
    chain.init({w, h, LookOutputEncoding::Srgb, 0.f, kernel::Backend::CpuParallel});
    Lut3D lut;
    lut_generate_grade(look, 32, lut);
    std::vector<Vec3> out(static_cast<size_t>(w) * h);
    LookChainInput in{};
    in.hdr = hdr.data();
    in.frame_seed = 99;
    chain.process(in, LookEffectGraph::makeDefault(), look, lut, out.data());
    f32 err = 0.f;
    for (size_t i = 0; i < out.size(); ++i) {
        err = std::max(err, maxAbs(out[i], legacy[i]));
    }
    std::printf("  neutral look chain vs PostStack::processFrame: max |diff| = %.3g (sRGB code values)\n", err);
    expectTrue(err < 5e-5f, "neutral look through LookPostChain == PostStack::processFrame");
    expectNear(chain.lastStats().exposure_ev, stack.totalExposureEv(), 1e-6, "same total exposure (mid-grey calibration)");

    b.set(LookParam::BloomEnabled, 0.f);
    b.set(LookParam::VignetteEnabled, 0.f);
    b.set(LookParam::GrainEnabled, 0.f);
    apply_look_to_post_stack(look_resolve(b), stack);
    expectTrue(stack.bloom().params().intensity == 0.f && stack.colorGrade().params().vignette == 0.f &&
                   stack.colorGrade().params().film_grain == 0.f,
               "disabled look nodes zero the legacy stack's strengths");
}

// ---------------------------------------------------------------------------------------------
// 6. Hot reload and allocations

std::string hotLook(f32 intensity, const char* lut) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  R"({"schemaVersion": 1, "kind": "fuse.look", "name": "hot", "base": {"bloom": {"intensity": %.3f}, "colorGrade": {"lut": "%s"}}})",
                  static_cast<double>(intensity), lut);
    return buf;
}

void touchNewer(const std::filesystem::path& p, const std::string& text) {
    const auto before = std::filesystem::last_write_time(p);
    writeFile(p, text);
    if (std::filesystem::last_write_time(p) == before) {
        std::filesystem::last_write_time(p, before + std::chrono::seconds(1));
    }
}

void testHotReload() {
    const std::filesystem::path dir = scratchDir();
    writeBlendLuts(dir);
    const std::filesystem::path file = dir / "hot.fuselook";
    writeFile(file, hotLook(0.1f, "id.cube"));
    LookSystem sys;
    LookParseResult diag;
    expectTrue(sys.loadFile(file.string().c_str(), diag), "hot look loads");
    sys.enableHotReload(true);
    expectTrue(!sys.pollHotReload(), "no change -> no reload");
    expectNear(sys.evaluate({}).resolved.bloom.intensity, 0.1, 1e-6, "initial value");
    touchNewer(file, hotLook(0.25f, "id.cube"));
    expectTrue(sys.pollHotReload() && sys.reloadCount() == 1u, "edited look file reloads");
    expectNear(sys.evaluate({}).resolved.bloom.intensity, 0.25, 1e-6, "reloaded value is live");
    touchNewer(file, "{ \"schemaVersion\": 1, \"kind\": \"fuse.look\", ");
    expectTrue(!sys.pollHotReload() && !sys.lastReloadError().empty() && sys.failedReloadCount() == 1u,
               "broken edit is reported");
    expectNear(sys.evaluate({}).resolved.bloom.intensity, 0.25, 1e-6, "broken edit keeps the last good look");
    touchNewer(file, hotLook(0.5f, "inv.cube"));
    expectTrue(sys.pollHotReload() && sys.lastReloadError().empty(), "fixed file reloads");
    const Vec3 before = sys.evaluate({}).resolved.grade.enabled ? sys.gradeLut().data[0] : Vec3{};
    expectTrue(before.x == 1.f, "inverted LUT active");
    Lut3D half = Lut3D::identity(3);
    for (Vec3& v : half.data) {
        v = Vec3{0.5f, 0.5f, 0.5f};
    }
    touchNewer(dir / "inv.cube", lut_write_cube(half));
    expectTrue(sys.pollHotReload(), "editing a referenced .cube reloads the look");
    expectNear(sys.evaluate({}).resolved.grade.enabled ? sys.gradeLut().data[0].x : 0.f, 0.5, 1e-6,
               "reloaded LUT is baked");
}

void testSteadyStateAllocations(const char* look_name, LookOutputEncoding encoding) {
    setWorkers(2);
    const u32 w = 96, h = 54;
    const std::vector<Vec3> hdr = makeHdrFrame(w, h);
    LookSystem sys;
    LookParseResult diag;
    const std::string path = samplesDir() + "/" + look_name + ".fuselook";
    sys.loadFile(path.c_str(), diag);
    sys.enableHotReload(true);
    LookPostChain chain;
    chain.init({w, h, encoding, 1000.f, kernel::Backend::CpuParallel});
    LookCasSharpener cas;
    if (LookCasSharpener::available()) {
        cas.init(w, h);
        chain.setSharpenHook(&LookCasSharpener::hook, &cas);
    }
    std::vector<Vec3> out(static_cast<size_t>(w) * h);
    f32 weather[2] = {0.f, 0.f};
    f32 lightning = 0.f;
    const auto frame = [&](u32 i) {
        LookFrameInput in{};
        in.time_of_day_hours = static_cast<f32>(i) * 0.37f;
        in.camera_position = {static_cast<f32>(i % 30u) - 15.f, 1.f, 0.f}; // crosses the sample volumes
        weather[0] = 0.5f + 0.5f * std::sin(static_cast<f32>(i) * 0.3f);
        weather[1] = 0.3f;
        lightning = (i % 7u) == 0u ? 1.f : 0.f;
        in.weather_weights = weather;
        in.weather_count = 2;
        in.override_weights = &lightning;
        in.override_count = 1;
        sys.pollHotReload();
        const LookEvaluation& e = sys.evaluate(in);
        LookChainInput ci{};
        ci.hdr = hdr.data();
        ci.frame_seed = i + 1u;
        chain.process(ci, sys.graph(), e.resolved, sys.gradeLut(), out.data());
    };
    for (u32 i = 0; i < 4u; ++i) {
        frame(i);
    }
    u32 rebakes = 0;
    const std::uint64_t before = g_heapAllocations.load();
    g_allocTrap.store(std::getenv("FUSE_LOOK_ALLOC_TRAP") != nullptr);
    for (u32 i = 4; i < 40u; ++i) {
        frame(i);
        rebakes += sys.lastEvaluation().lut_rebaked ? 1u : 0u;
    }
    g_allocTrap.store(false);
    const std::uint64_t allocs = g_heapAllocations.load() - before;
    char label[200];
    std::snprintf(label, sizeof(label),
                  "%s (%s): 0 heap allocations per frame in steady state (36 frames: evaluate + %u LUT re-bakes + "
                  "chain of %u nodes%s + hot-reload poll; %llu allocations)",
                  look_name, look_output_encoding_name(encoding), rebakes, chain.lastStats().executed_count,
                  cas.calls() > 0u ? " incl. CAS" : "", static_cast<unsigned long long>(allocs));
    std::printf("  %s\n", label);
    expectTrue(allocs == 0u && rebakes > 10u, label);
}

} // namespace

int main() {
    fuse::core::initialize();
    setWorkers(2);
    testParamTable();
    testGraphValidation();
    testSchema();
    testSamples();
    testBlend();
    testLuts();
    testChainParity();
    testExistingParity();
    testAnalyticKernels();
    setWorkers(2);
    testPostStackIntegration();
    testHotReload();
    testSteadyStateAllocations("stormy_night", LookOutputEncoding::Hdr10Pq);
    testSteadyStateAllocations("filmic_warm", LookOutputEncoding::Srgb);
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::core::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d look gate check(s) failed\n", g_failures, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("Look system gates passed (%d checks)\n", g_checks);
    return EXIT_SUCCESS;
}
