// FUSE Relight RL-3.6 CPU gates (rl_particles_<suite>): run as `fuse_relight_particles_tests <suite> --fixtures <dir>`.
//
//   curve       curve baking (linear, step, Bezier with and without tangent times, gradients, invalid curves)
//   desc        schema fallbacks, primvars -> description (legacy fallbacks, tokens, flags, broadcast, issues), the
//               fixture mod through the USD reader and through the RL-3.2 importer's relight_particles records ==
//               the scenario descriptions of particle_scenarios.hpp (hash), animation table, index topology, the
//               global preset from the options
//   rng         the hash stream (KAT, exact [0, 1) floats, uniformity)
//   parity      CpuReference == CpuParallel (0, 2, 4 workers) bit-exact over 150 frames of the three fixture systems
//               (particles, vertices, retirement counters, ring counters, draws); kernel stats names and items
//   fixtures    the fixture systems over 180 frames: per-frame live counts, visible counts and spawns equal the
//               golden Tests/relight/fixtures/particles/expected.json; analytic bounds (ballistic height, emitter
//               extent + speed x lifetime, max-velocity clamps), Poisson and burst expectations, constant count
//   billboards  the billboard kernel against an independent double-precision reference (every billboard type,
//               rotation, velocity alignment, motion trails, sprite sheets, flips) and geometric checks
//   zero_alloc  no heap allocation in steady state (CpuReference and CpuParallel with 2 workers)
//   bridge      spawning from replaced / attached geometry through a ReplacementEngine on the fixture mod: mesh
//               replacement parts, material replacements on kept originals, ParticleEmitter draws (global preset),
//               hideEmitter, previous transforms
//   options     rtx.particles.enable / enableSpawning / timeScale
//
// `--update-golden` rewrites expected.json from the CpuReference run (review the diff before committing).
#include "particle_scenarios.hpp"

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/relight/mods/import/mod_importer.hpp>
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/particles/particle_kernels.hpp>
#include <fuse/relight/particles/particle_mods.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/particles/particle_options.hpp>
#include <fuse/relight/replace/replacement_engine.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// ---- allocation counter (zero_alloc) --------------------------------------------------------------------------------

namespace {
std::atomic<bool> g_countAllocs{false};
std::atomic<std::uint64_t> g_allocs{0};
} // namespace

// GCC may inline the replacement operators into callers and then report a false -Wmismatched-new-delete at -O2;
// replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define RL_PT_NOINLINE __attribute__((noinline))
#else
#define RL_PT_NOINLINE
#endif

RL_PT_NOINLINE void* operator new(std::size_t n) {
    if (g_countAllocs.load(std::memory_order_relaxed)) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc();
}
RL_PT_NOINLINE void* operator new[](std::size_t n) { return operator new(n); }
RL_PT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
RL_PT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
RL_PT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
RL_PT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace rl_particles_test;
namespace fs = std::filesystem;
namespace pk = fuse::relight::particles::kernels;
namespace kernel = fuse::kernel;
namespace json = fuse::relight::capture::exporter::json;
namespace replace = fuse::relight::replace;
namespace usd = fuse::relight::mods::usd;

int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

fs::path g_fixtures;
bool g_updateGolden = false;

bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

/// rtx.conf text applied as an option layer for the scope's lifetime (the RL-0.6 test pattern).
class ScopedConf {
public:
    explicit ScopedConf(const std::string& text) {
        static int s_counter = 0;
        const fuse::relight::options::OptionConfig config = fuse::relight::options::OptionConfig::parse(text);
        m_layer = fuse::relight::options::OptionManager::acquireLayer(
            "", {7100u + static_cast<std::uint32_t>(s_counter++), "rl_particles_test"}, 1.0f, 0.1f, false, &config);
        fuse::relight::options::OptionManager::applyPendingValues(nullptr, false);
    }
    ~ScopedConf() {
        m_layer = fuse::relight::options::OptionLayerHandle();
        fuse::relight::options::OptionManager::applyPendingValues(nullptr, false);
    }
    ScopedConf(const ScopedConf&) = delete;
    ScopedConf& operator=(const ScopedConf&) = delete;

private:
    fuse::relight::options::OptionLayerHandle m_layer;
};

// ---- curve ----------------------------------------------------------------------------------------------------------

void testCurve() {
    std::vector<float> out;
    CHECK(bakeFloatCurve(curve({0.f, 1.f}, {0.f, 10.f}), out));
    CHECK(out.size() == kCurveResolution);
    bool linear = true;
    for (std::uint32_t i = 0; i < kCurveResolution; ++i) {
        linear = linear && approx(out[i], 10.0 * i / 255.0, 1e-4);
    }
    CHECK(linear);
    // Outside the key range the first / last interval is used and extrapolated (upstream: the interval is clamped,
    // its parameter is not).
    CHECK(bakeFloatCurve(curve({0.25f, 0.75f}, {2.f, 4.f}), out));
    CHECK(approx(out.front(), 1.0, 1e-6) && approx(out.back(), 5.0, 1e-6) && approx(out[128], 2.f + 2.f * ((128.f / 255.f) - 0.25f) / 0.5f, 1e-5));
    // Step: hold the key.
    FloatCurve step = curve({0.f, 0.5f, 1.f}, {1.f, 5.f, 9.f});
    step.outTangentTypes = {TangentType::Step, TangentType::Linear, TangentType::Linear};
    CHECK(bakeFloatCurve(step, out));
    CHECK(out[0] == 1.f && out[100] == 1.f && out[127] == 1.f && approx(out[200], 5.f + 4.f * (200.f / 255.f - 0.5f) / 0.5f, 1e-5));
    // Bezier without tangent times: y1 = y0 + out, y2 = y3 + in.
    FloatCurve bz = curve({0.f, 1.f}, {0.f, 1.f});
    bz.inTangentValues = {0.f, 0.f};
    bz.outTangentValues = {0.f, 0.f};
    CHECK(bakeFloatCurve(bz, out));
    const double t = 64.0 / 255.0;
    const double smooth = 3 * (1 - t) * t * t * 0.0 + 3 * (1 - t) * (1 - t) * t * 0.0 + t * t * t * 1.0 + 3 * (1 - t) * t * t * 1.0;
    CHECK(approx(out[64], smooth, 1e-5));
    // Bezier with tangent times (Newton on X): ends are exact, the middle of a symmetric curve is its midpoint.
    FloatCurve bt = curve({0.f, 1.f}, {0.f, 1.f});
    bt.inTangentValues = {0.f, 0.f};
    bt.outTangentValues = {0.f, 0.f};
    bt.inTangentTimes = {0.f, -0.3f};
    bt.outTangentTimes = {0.3f, 0.f};
    CHECK(bakeFloatCurve(bt, out));
    CHECK(out.front() == 0.f && approx(out.back(), 1.f, 1e-6));
    CHECK(approx(evalBezierFCurve(0.f, 0.3f, 0.7f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.5f), 0.5, 1e-5));
    bool monotonic = true;
    for (std::uint32_t i = 1; i < kCurveResolution; ++i) {
        monotonic = monotonic && out[i] >= out[i - 1] - 1e-6f;
    }
    CHECK(monotonic);
    // Invalid (size mismatch) and empty curves bake to the default and report false.
    CHECK(!bakeFloatCurve(curve({0.f, 1.f}, {3.f}), out, kCurveResolution, 7.f));
    CHECK(out.size() == kCurveResolution && out[0] == 7.f && out[255] == 7.f);
    CHECK(!bakeFloatCurve(FloatCurve{}, out, kCurveResolution, -1.f) && out[10] == -1.f);
    // Tangent tokens.
    CHECK(parseTangentType("step") == TangentType::Step && parseTangentType("auto") == TangentType::Auto &&
          parseTangentType("bogus") == TangentType::Linear);
    // Gradient.
    std::vector<std::array<float, 4>> g;
    ColorGradient grad;
    grad.times = {0.f, 1.f};
    grad.values = {{0.f, 0.f, 0.f, 1.f}, {1.f, 0.5f, 0.25f, 0.f}};
    CHECK(bakeColorGradient(grad, g));
    CHECK(g.size() == kCurveResolution && g[0][3] == 1.f && approx(g[255][1], 0.5, 1e-6) && approx(g[51][0], 0.2, 1e-5));
    CHECK(!bakeColorGradient(ColorGradient{}, g) && g[3][0] == 1.f);
    // Channel combination: an absent channel takes its default.
    std::vector<float> x(kCurveResolution, 2.f);
    Channel c;
    CHECK(combineChannels({&x, nullptr}, {true, false}, {9.f, 8.f}, c));
    CHECK(c[17][0] == 2.f && c[17][1] == 8.f);
    CHECK(!combineChannels({&x}, {false}, {5.f}, c) && c[0][0] == 5.f);
}

// ---- desc -----------------------------------------------------------------------------------------------------------

json::Value parseJson(const std::string& s) {
    auto v = json::parse(s);
    return v ? *v : json::Value();
}

std::uint64_t hashOf(const ParticleSystemDesc& d) { return hashDesc(d); }

void testDesc() {
    const ParticleSystemDesc def = schemaDefaultDesc();
    CHECK(def.gpu.maxNumParticles == 10000 && def.gpu.minTimeToLive == 1.f && def.gpu.turbulenceForce == 5.f &&
          def.gpu.turbulenceFrequency == 0.05f && def.gpu.collisionRestitution == 0.5f && def.gpu.collisionThickness == 5.f &&
          def.gpu.motionTrailMultiplier == 1.f && def.gpu.flags == 0u);
    CHECK(def.minSize.size() == 2 && def.minSize[0][0] == 10.f && def.minSize[1][0] == 0.f && def.minColor[1][3] == 0.f);
    CHECK(!def.constantCount());

    // Primvars: legacy pairs, broadcast, tokens, flags, deprecated maxSpeed, type issues.
    std::vector<std::string> issues;
    const ParticleSystemDesc a = descFromPrimvars(parseJson(R"({
        "maxNumParticles": 64, "spawnRatePerSecond": 64, "minSpawnSize": 5, "maxTargetSize": [7, 8],
        "minSpawnColor": [0.5, 0.25, 1, 1], "billboardType": "FaceWorldUp", "spriteSheetMode": "OverrideMaterial_Random",
        "collisionMode": "Kill", "randomFlipAxis": "Vertical", "hideEmitter": true, "useTurbulence": 1,
        "restrictVelocityY": false, "maxSpeed": 25, "attractorPosition": [1, 2, 3],
        "maxSpawnColor": [1, 2], "gravityForce": "heavy", "minColor:times": [0, 1], "minColor:values": [[1, 1, 1, 1]]
    })"),
                                                    &issues);
    CHECK(a.gpu.maxNumParticles == 64 && a.constantCount());
    CHECK(a.minSize[0][0] == 5.f && a.minSize[0][1] == 5.f && a.maxSize[1][0] == 7.f && a.maxSize[1][1] == 8.f);
    CHECK(a.minColor.size() == 2 && a.minColor[0][1] == 0.25f && a.minColor[1][3] == 0.f); // invalid gradient -> legacy
    CHECK(a.maxColor[0][0] == 1.f);                                                         // wrong size: fallback kept
    CHECK(a.gpu.billboardType == static_cast<std::uint32_t>(BillboardType::FaceWorldUp));
    CHECK(a.gpu.spriteSheetMode == static_cast<std::uint32_t>(SpriteSheetMode::OverrideMaterialRandom));
    CHECK(a.gpu.collisionMode == static_cast<std::uint32_t>(CollisionMode::Kill));
    CHECK(a.gpu.randomFlipAxis == static_cast<std::uint32_t>(RandomFlipAxis::Vertical));
    CHECK(a.gpu.flags == (kFlagHideEmitter | kFlagUseTurbulence));
    CHECK(a.maxVelocity[0][0] == 25.f && a.maxVelocity[1][2] == 25.f);
    CHECK(a.gpu.attractorPosition[2] == 3.f);
    CHECK(issues.size() == 2); // maxSpawnColor (tuple size), gravityForce (not a number)
    const ParticleSystemDesc unknownToken = descFromPrimvars(parseJson(R"({"billboardType": "Sideways"})"));
    CHECK(unknownToken.gpu.billboardType == static_cast<std::uint32_t>(BillboardType::FaceCameraSpherical));

    // Curves win over the legacy pairs; an absent axis takes the channel default.
    const ParticleSystemDesc b = descFromPrimvars(parseJson(R"({
        "minSize:y:times": [0, 1], "minSize:y:values": [2, 4], "minSpawnSize": [99, 99],
        "maxVelocity:z:times": [0, 1], "maxVelocity:z:values": [5, 5]
    })"));
    CHECK(b.minSize.size() == kCurveResolution && b.minSize[0][0] == 10.f && b.minSize[0][1] == 2.f && b.minSize[255][1] == 4.f);
    CHECK(b.maxVelocity.size() == kCurveResolution && b.maxVelocity[9][0] == -1.f && b.maxVelocity[9][2] == 5.f);

    // The fixture mod through the USD reader == the scenario descriptions.
    const std::string modPath = (g_fixtures / "scenes" / "mod.usda").generic_string();
    const usd::ComposedStage stage = usd::readStage(modPath);
    const usd::RemixMod mod = usd::collectRemixMod(stage);
    CHECK(mod.meshes.size() == 1 && mod.materials.size() == 2);
    std::map<std::string, std::uint64_t> usdHashes;
    for (const usd::MeshReplacement& m : mod.meshes) {
        CHECK(m.particles.has_value());
        if (m.particles) {
            std::vector<std::string> iss;
            usdHashes["fountain"] = hashOf(descFromUsd(*m.particles, &iss));
            CHECK(iss.empty());
        }
    }
    for (const usd::MaterialReplacement& m : mod.materials) {
        CHECK(m.particles.has_value());
        if (m.particles) {
            usdHashes[m.hash == 0x5A4B50ull ? "sparks" : "smoke"] = hashOf(descFromUsd(*m.particles));
        }
    }
    const std::vector<Scenario> sc = scenarios();
    for (const Scenario& s : sc) {
        const bool same = usdHashes.count(s.name) && usdHashes[s.name] == hashOf(s.desc);
        if (!same) {
            std::fprintf(stderr, "  %s: USD description differs from the scenario\n", s.name.c_str());
        }
        CHECK(same);
    }

    // The same systems through the RL-3.2 importer's relight_particles records.
    fuse::relight::mods::import::ImportOptions o;
    o.root = (g_fixtures / "scenes").generic_string();
    o.gameId = "unit";
    const auto imported = fuse::relight::mods::import::importMod(o);
    CHECK(imported.ok && imported.counts.particles == 3);
    std::size_t matched = 0;
    for (const auto& [path, bytes] : imported.files) {
        if (path.rfind("poco/relight_particles/", 0) != 0) {
            continue;
        }
        const auto rec = json::parse(std::string(bytes.begin(), bytes.end()));
        CHECK(rec.has_value());
        const auto d = rec ? descFromRecord(*rec) : std::nullopt;
        CHECK(d.has_value());
        if (d) {
            const std::uint64_t h = hashOf(*d);
            for (const Scenario& s : sc) {
                matched += h == hashOf(s.desc) ? 1u : 0u;
            }
        }
    }
    CHECK(matched == 3);
    CHECK(!descFromRecord(parseJson(R"({"kind": "mesh"})")).has_value());

    // Animation table: texel 255 is birth (channel start), texel 0 death (channel end).
    std::vector<Float4> table(kAnimationTexels);
    const ParticleSystemDesc f = fountainDesc();
    buildAnimationTable(f, table.data());
    CHECK(table[kRowMinColor * kAnimationWidth + 255].v[3] == 1.f && table[kRowMinColor * kAnimationWidth + 0].v[3] == 0.f);
    CHECK(approx(table[kRowMinSize * kAnimationWidth + 255].v[0], 4.0, 1e-5) && approx(table[kRowMinSize * kAnimationWidth].v[0], 12.0, 1e-4));
    CHECK(table[kRowMaxVelocity * kAnimationWidth + 3].v[0] == 0.f && table[kRowMinSize * kAnimationWidth + 7].v[2] == 0.f);
    // Two-key legacy channels interpolate linearly across the table.
    const ParticleSystemDesc sp = sparksDesc();
    buildAnimationTable(sp, table.data());
    CHECK(approx(table[kRowMinSize * kAnimationWidth + 128].v[0], 2.0 - 1.0 * (1.0 - 128.0 / 255.0), 1e-5));

    // Index topology (upstream): quad 0 1 2 / 2 1 3; trail adds the head quads.
    std::vector<std::uint32_t> idx;
    buildIndices(f, 2, idx);
    CHECK(idx.size() == 12 && idx[0] == 0 && idx[3] == 2 && idx[5] == 3 && idx[6] == 4 && idx[11] == 7);
    buildIndices(sp, 1, idx);
    CHECK(idx.size() == 18 && idx[6] == 1 && idx[7] == 4 && idx[17] == 7 && verticesPerParticle(sp) == 8);

    // Global preset from the options.
    {
        ScopedConf conf("rtx.particles.globalPreset.spawnRatePerSecond = 250\nrtx.particles.globalPreset.billboardType = 2\n"
                        "rtx.particles.globalPreset.minSpawnSize = 3, 4\n");
        const ParticleSystemDesc preset = globalPresetDesc();
        CHECK(preset.gpu.spawnRatePerSecond == 250.f && preset.gpu.billboardType == 2u && preset.gpu.maxNumParticles == 10000u);
        CHECK(preset.minSize[0][0] == 3.f && preset.minSize[0][1] == 4.f && preset.gpu.spriteSheetRows == 1u);
        CHECK(preset.maxVelocity[0][0] == -1.f);
    }
    CHECK(globalPresetDesc().gpu.spawnRatePerSecond == 100.f);
}

// ---- rng ------------------------------------------------------------------------------------------------------------

void testRng() {
    // KAT of the prospector hash (values of this implementation; the GPU gate checks the GLSL twin against it).
    CHECK(pk::uintHash(0u) == 0u);
    CHECK(pk::uintHash(1u) == 0x86D2FA73u && pk::uintHash(0xDEADBEEFu) == 0x2A2ACAF2u);
    CHECK(pk::uintHash3(1u, 2u, 3u) == pk::uintHash(1u ^ pk::uintHash(2u) ^ pk::uintHash(3u)));
    CHECK(pk::unorm23ToFloat(0u) == 0.f && pk::unorm23ToFloat(0xFFFFFFFFu) < 1.f && pk::unorm23ToFloat(0x7FFFFFu) == 1.f - std::ldexp(1.f, -23));
    double sum = 0.0;
    std::uint32_t buckets[10] = {};
    pk::SlotRandom r{0u, 7u, 99u};
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        const float u = r.next();
        CHECK(u >= 0.f && u < 1.f);
        sum += u;
        ++buckets[static_cast<int>(u * 10.f)];
    }
    CHECK(approx(sum / n, 0.5, 0.005));
    for (std::uint32_t b : buckets) {
        CHECK(std::fabs(static_cast<double>(b) - n / 10.0) < 5.0 * std::sqrt(n / 10.0));
    }
}

// ---- helpers for running scenarios -----------------------------------------------------------------------------------

struct Snapshot {
    std::vector<GpuParticle> particles;
    std::vector<GpuParticleVertex> vertices;
    std::vector<SystemCounters> counters;
    std::vector<SystemDraw> draws;
};

bool bitEqual(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

std::vector<std::vector<Snapshot>> runCpu(kernel::Backend backend, std::uint32_t frames, std::uint32_t framesInFlight,
                                          std::uint32_t snapshotEvery) {
    ParticleSystemManager m(smallConfig(framesInFlight));
    CpuParticleBackend b(backend);
    b.init(m.config());
    Driver d(scenarios());
    CHECK(d.registerMeshes(m));
    std::vector<std::vector<Snapshot>> out(1);
    d.run(m, b, 0, frames, [&](std::uint32_t f) {
        if (f % snapshotEvery != snapshotEvery - 1) {
            return;
        }
        Snapshot s;
        s.particles.assign(b.particles().begin(), b.particles().end());
        s.vertices.assign(b.vertices().begin(), b.vertices().end());
        for (std::uint32_t i = 0; i < m.config().maxSystems; ++i) {
            s.counters.push_back(m.counters(i));
        }
        s.draws.assign(m.draws().begin(), m.draws().end());
        out[0].push_back(std::move(s));
    });
    return out;
}

bool drawsEqual(const std::vector<SystemDraw>& a, const std::vector<SystemDraw>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].system != b[i].system || a[i].descHash != b[i].descHash || a[i].vertexCount != b[i].vertexCount ||
            a[i].vertexBase != b[i].vertexBase || a[i].generation != b[i].generation) {
            return false;
        }
    }
    return true;
}

// ---- parity ---------------------------------------------------------------------------------------------------------

void testParity() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const auto ref = runCpu(kernel::Backend::CpuReference, 150, 2, 10);
    for (std::uint32_t workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        const auto par = runCpu(kernel::Backend::CpuParallel, 150, 2, 10);
        bool same = par[0].size() == ref[0].size();
        for (std::size_t i = 0; same && i < ref[0].size(); ++i) {
            const Snapshot& a = ref[0][i];
            const Snapshot& b = par[0][i];
            same = bitEqual(a.particles.data(), b.particles.data(), a.particles.size() * sizeof(GpuParticle)) &&
                   bitEqual(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(GpuParticleVertex)) &&
                   a.counters == b.counters && drawsEqual(a.draws, b.draws);
        }
        std::printf("CpuReference == CpuParallel (%u workers): %s\n", workers, same ? "bit-exact" : "DIFFERENT");
        CHECK(same);
    }
    scheduler.shutdown();
    // Stats: every pass is recorded under its name with its item count.
    kernel::KernelStats s{};
    CHECK(kernel::find_kernel_stats(pk::kSpawnName, s) && s.launches > 0 && s.items > 0);
    CHECK(kernel::find_kernel_stats(pk::kEvolveName, s) && s.launches > 0);
    CHECK(kernel::find_kernel_stats(pk::kBillboardName, s) && s.launches > 0 && s.items % 64u == 0u);
    // Particles actually live and move (the parity is not about empty pools).
    const Snapshot& last = ref[0].back();
    std::uint32_t alive = 0;
    for (const GpuParticle& p : last.particles) {
        alive += p.state == kParticleAlive && p.timeToLive > 0.f ? 1u : 0u;
    }
    std::printf("live particles at frame 150: %u\n", alive);
    CHECK(alive > 500);
}

// ---- fixtures -------------------------------------------------------------------------------------------------------

struct FrameLog {
    std::vector<std::uint32_t> count, visible, spawned;
};

struct Bounds {
    double lo[3], hi[3];
};

std::string jsonArray(const std::vector<std::uint32_t>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        s += (i ? "," : "") + std::to_string(v[i]);
    }
    return s + "]";
}

void testFixtures() {
    constexpr std::uint32_t kFrames = 180;
    ParticleSystemManager m(smallConfig(2));
    CpuParticleBackend b(kernel::Backend::CpuReference);
    b.init(m.config());
    Driver d(scenarios());
    CHECK(d.registerMeshes(m));
    std::vector<FrameLog> logs(d.list.size());
    // Analytic bounds (see expected.json "bounds" for the derivations).
    const Bounds bounds[3] = {
        {{-100.0, -300.0, -50.001}, {100.0, 20.5, 50.001}},   // fountain: apex 20.4, 200 t - 490 t^2 >= -290 at 1 s
        {{-310.0, 5.0, -160.0}, {10.0, 180.0, 160.0}},        // sparks: emitter at (-150, 20, 0), 20 cm quad
        {{40.0, -1.0, -110.0}, {260.0, 240.0, 110.0}},        // smoke: emitter at (150, 0, 0), 40 cm quad
    };
    bool inBounds = true;
    std::vector<std::uint32_t> spawnedNow(d.list.size(), 0);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        m.beginFrame(frameInput(f), b);
        for (std::size_t i = 0; i < d.list.size(); ++i) {
            SpawnRequest r;
            r.desc = &d.list[i].desc;
            r.descHash = d.hashes[i];
            r.materialKey = d.list[i].materialKey;
            r.mesh = d.meshes[i];
            r.objectToWorld = d.list[i].world(f);
            r.prevObjectToWorld = d.list[i].world(f == 0 ? 0 : f - 1);
            spawnedNow[i] = m.spawn(r).particles;
        }
        CHECK(m.simulate(b));
        for (std::size_t i = 0; i < d.list.size(); ++i) {
            const std::int32_t slot = m.findSystem(d.hashes[i], d.list[i].materialKey);
            CHECK(slot >= 0);
            if (slot < 0) {
                continue;
            }
            const SystemCounters c = m.counters(static_cast<std::uint32_t>(slot));
            const std::uint32_t base = m.particleBase(static_cast<std::uint32_t>(slot));
            std::uint32_t visible = 0;
            for (std::uint32_t k = 0; k < d.list[i].desc.gpu.maxNumParticles; ++k) {
                const GpuParticle& p = b.particles()[base + k];
                if (p.state != kParticleAlive || p.timeToLive <= 0.f) {
                    continue;
                }
                ++visible;
                for (int a = 0; a < 3; ++a) {
                    if (p.position[a] < bounds[i].lo[a] || p.position[a] > bounds[i].hi[a]) {
                        if (inBounds) {
                            std::fprintf(stderr, "  %s frame %u: particle %u axis %d at %g outside [%g, %g]\n", d.list[i].name.c_str(), f, k, a,
                                         p.position[a], bounds[i].lo[a], bounds[i].hi[a]);
                        }
                        inBounds = false;
                    }
                }
            }
            logs[i].count.push_back(c.particleCount);
            logs[i].visible.push_back(visible);
            logs[i].spawned.push_back(spawnedNow[i]);
        }
    }
    CHECK(inBounds);

    // Analytic expectations.
    auto total = [](const std::vector<std::uint32_t>& v) {
        std::uint64_t s = 0;
        for (std::uint32_t x : v) {
            s += x;
        }
        return s;
    };
    // fountain: Poisson(600 x dt) per frame -> 1800 +- 5 sigma over 180 frames; steady visible ~ 600 x E[ttl] = 450.
    const std::uint64_t fs = total(logs[0].spawned);
    std::printf("fountain: spawned %llu over %u frames, visible at the end %u, live count %u\n", static_cast<unsigned long long>(fs),
                kFrames, logs[0].visible.back(), logs[0].count.back());
    CHECK(fs > 1800 - 5 * 43 && fs < 1800 + 5 * 43);
    CHECK(logs[0].visible.back() > 330 && logs[0].visible.back() < 570);
    CHECK(logs[0].count.back() >= logs[0].visible.back() && logs[0].count.back() <= 1000);
    // sparks: constant count: 64 slots every frame; every slot respawns when asleep.
    bool constant = true;
    for (std::size_t f = 1; f < kFrames; ++f) {
        constant = constant && logs[1].count[f] == 64u && logs[1].visible[f] >= 40u && logs[1].visible[f] <= 64u;
    }
    CHECK(constant && logs[1].spawned[0] == 64u);
    // smoke: bursts of Poisson(480 x 0.25 = 120) every 250 ms (frames 0, 15, 30, ...), nothing in between.
    bool bursts = true;
    std::uint64_t burstTotal = 0;
    std::uint32_t burstCount = 0;
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        const bool burstFrame = f % 15u == 0u;
        if (burstFrame) {
            ++burstCount;
            burstTotal += logs[2].spawned[f];
        } else {
            bursts = bursts && logs[2].spawned[f] == 0u;
        }
    }
    std::printf("smoke: %u bursts, %llu particles\n", burstCount, static_cast<unsigned long long>(burstTotal));
    CHECK(bursts && burstCount == 12 && burstTotal > 12 * 120 - 5 * 38 && burstTotal < 12 * 120 + 5 * 38);

    // Golden per-frame counts.
    const fs::path golden = g_fixtures / "expected.json";
    if (g_updateGolden) {
        std::ofstream o(golden, std::ios::binary);
        o << "{\n  \"note\": \"RL-3.6 golden: per-frame live count (ring), visible (alive, time to live > 0) and spawned "
             "particles of the fixture systems, CpuReference, 180 frames at 60 Hz. Regenerate with fuse_relight_particles_tests "
             "fixtures --update-golden. Analytic bounds (asserted in test_particles.cpp): fountain y <= 200^2 / (2 x 980) = 20.4, "
             "y >= 200 x 1 - 490 x 1^2 = -290 (1 s life), |z| <= 50, |x| <= 50 + 20 (emitter sway) + 0.5 x 60 cm/s x 1 s; sparks within 300 cm/s x 0.5 s of the 20 cm "
             "emitter; smoke |x - 150|, |z| <= 20 + 30 cm/s x 3 s, y <= 80 cm/s x 3 s.\",\n  \"frames\": "
          << kFrames << ",\n";
        for (std::size_t i = 0; i < d.list.size(); ++i) {
            o << "  \"" << d.list[i].name << "\": {\n    \"count\": " << jsonArray(logs[i].count) << ",\n    \"visible\": "
              << jsonArray(logs[i].visible) << ",\n    \"spawned\": " << jsonArray(logs[i].spawned) << "\n  }"
              << (i + 1 < d.list.size() ? "," : "") << "\n";
        }
        o << "}\n";
        std::printf("golden written: %s\n", golden.generic_string().c_str());
        return;
    }
    std::ifstream in(golden, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const auto g = json::parse(ss.str());
    CHECK(g.has_value());
    if (!g) {
        return;
    }
    for (std::size_t i = 0; i < d.list.size(); ++i) {
        const json::Value* e = g->get(d.list[i].name);
        CHECK(e != nullptr);
        if (e == nullptr) {
            continue;
        }
        for (const auto& [key, series] : {std::pair<const char*, const std::vector<std::uint32_t>*>{"count", &logs[i].count},
                                           {"visible", &logs[i].visible},
                                           {"spawned", &logs[i].spawned}}) {
            const json::Value* arr = e->get(key);
            bool same = arr != nullptr && arr->isArray() && arr->a.size() == series->size();
            for (std::size_t f = 0; same && f < series->size(); ++f) {
                same = static_cast<std::uint32_t>(arr->a[f].n) == (*series)[f];
                if (!same) {
                    std::fprintf(stderr, "  %s %s frame %zu: %u, golden %u\n", d.list[i].name.c_str(), key, f, (*series)[f],
                                 static_cast<std::uint32_t>(arr->a[f].n));
                }
            }
            CHECK(same);
        }
    }
}

// ---- billboards -----------------------------------------------------------------------------------------------------

struct D3 {
    double x, y, z;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dotd(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 crossd(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 normd(D3 a) { return a * (1.0 / std::sqrt(dotd(a, a))); }

/// Independent reference of one particle's billboard (double precision, written from the upstream shader's
/// description, not from the kernel): nullopt when culled.
struct RefQuad {
    std::vector<D3> pos;
    std::vector<std::array<double, 2>> uv;
    std::array<double, 4> color; ///< B G R A
};

double sampleRef(const std::vector<Float4>& table, std::uint32_t row, double u, int c, bool randomize, double seed) {
    const double tx = std::clamp(u * 256.0 - 0.5, 0.0, 255.0);
    const int x0 = static_cast<int>(std::floor(tx));
    const int x1 = std::min(x0 + 1, 255);
    const double f = tx - x0;
    auto at = [&](std::uint32_t r, int x) { return static_cast<double>(table[r * kAnimationWidth + static_cast<std::uint32_t>(x)].v[c]); };
    const double a = at(row, x0) * (1 - f) + at(row, x1) * f;
    if (!randomize) {
        return a;
    }
    const double b = at(row + 1, x0) * (1 - f) + at(row + 1, x1) * f;
    return a * (1 - seed) + b * seed;
}

std::optional<RefQuad> referenceBillboard(const ParticleSystemDesc& desc, const std::vector<Float4>& table, const GpuParticle& p,
                                          const FrameInput& in) {
    const GpuSystemDesc& g = desc.gpu;
    if (p.state != kParticleAlive || p.timeToLive <= 0.f) {
        return std::nullopt;
    }
    const double initial = std::max(1.0 / 30.0, g.minTimeToLive + (g.maxTimeToLive - g.minTimeToLive) * static_cast<double>(p.randSeed));
    const double life = p.timeToLive / initial;
    const double sx = sampleRef(table, kRowMinSize, life, 0, true, p.randSeed) * in.sceneScale;
    const double sy = sampleRef(table, kRowMinSize, life, 1, true, p.randSeed) * in.sceneScale;
    std::array<double, 4> color{};
    const double base[4] = {(p.color & 0xFF) / 255.0, ((p.color >> 8) & 0xFF) / 255.0, ((p.color >> 16) & 0xFF) / 255.0, (p.color >> 24) / 255.0};
    for (int c = 0; c < 4; ++c) {
        color[static_cast<std::size_t>(c)] = base[c] * sampleRef(table, kRowMinColor, life, c, true, p.randSeed);
    }
    if (color[3] < in.resolveTransparencyThreshold || std::max(sx, sy) < 0.1 * in.sceneScale) {
        return std::nullopt;
    }
    const D3 pos{p.position[0], p.position[1], p.position[2]};
    const D3 camRight{in.viewToWorld[0], in.viewToWorld[1], in.viewToWorld[2]};
    const D3 camUp{in.viewToWorld[4], in.viewToWorld[5], in.viewToWorld[6]};
    const D3 camPos{in.viewToWorld[12], in.viewToWorld[13], in.viewToWorld[14]};
    const D3 up = normd({in.up[0], in.up[1], in.up[2]});
    D3 right{}, bup{};
    switch (static_cast<BillboardType>(g.billboardType)) {
    case BillboardType::FaceCameraUpAxisLocked: {
        D3 toCam = normd(camPos - pos);
        D3 plane = normd(toCam - up * dotd(toCam, up));
        right = normd(crossd(plane, up));
        bup = up;
        break;
    }
    case BillboardType::FaceCameraPosition: {
        D3 n = normd(pos - camPos);
        D3 upIn = normd(up - n * dotd(up, n));
        right = normd(crossd(upIn, n));
        bup = upIn;
        break;
    }
    case BillboardType::FaceWorldUp: {
        const D3 ref = std::fabs(up.z) < 0.98 ? D3{0, 0, 1} : D3{1, 0, 0};
        right = normd(ref - up * dotd(ref, up));
        bup = normd(crossd(up, right));
        break;
    }
    default:
        right = camRight;
        bup = camUp;
        break;
    }
    // Screen-size culling is not exercised (the fixture's particles are far larger than 2 pixels): checked apart.
    const D3 vel{p.velocity[0], p.velocity[1], p.velocity[2]};
    double s = 0, c = 1;
    const bool trail = desc.motionTrail();
    if ((g.flags & kFlagAlignParticlesToVelocity) != 0u) {
        const double len = std::sqrt(dotd(vel, vel));
        if (len >= 1e-6) {
            const D3 v = vel * (1.0 / len);
            const double x = -dotd(v, right), y = dotd(v, bup);
            const double l = std::hypot(x, y);
            if (l > 0) {
                s = x / l;
                c = y / l;
            }
        }
    } else {
        s = std::sin(p.rotation);
        c = std::cos(p.rotation);
    }
    const std::uint32_t rows = std::max(1u, g.spriteSheetRows), cols = std::max(1u, g.spriteSheetCols);
    const std::uint32_t frames = rows * cols;
    std::uint32_t frame = 0;
    if (frames > 1 && g.spriteSheetMode == static_cast<std::uint32_t>(SpriteSheetMode::OverrideMaterialLifetime)) {
        frame = std::min(frames - 1, static_cast<std::uint32_t>(std::floor((1.0 - life) * frames)));
    } else if (frames > 1 && g.spriteSheetMode == static_cast<std::uint32_t>(SpriteSheetMode::OverrideMaterialRandom)) {
        frame = std::min(frames - 1, static_cast<std::uint32_t>(p.randSeed * frames));
    }
    const double cw = (p.uvMinMax[2] - p.uvMinMax[0]) / cols, ch = (p.uvMinMax[3] - p.uvMinMax[1]) / rows;
    const double u0 = p.uvMinMax[0] + (frame % cols) * cw, v0 = p.uvMinMax[1] + (frame / cols) * ch;
    double fx = 1, fy = 1;
    switch (static_cast<RandomFlipAxis>(g.randomFlipAxis)) {
    case RandomFlipAxis::Horizontal: fx = p.randSeed < 0.5f ? -1 : 1; break;
    case RandomFlipAxis::Vertical: fy = p.randSeed < 0.5f ? -1 : 1; break;
    case RandomFlipAxis::Both:
        fx = (p.randSeed < 0.25f || (p.randSeed >= 0.5f && p.randSeed < 0.75f)) ? -1 : 1;
        fy = p.randSeed < 0.5f ? -1 : 1;
        break;
    default: break;
    }
    RefQuad q;
    q.color = {color[2], color[1], color[0], color[3]};
    const float* offs = trail ? pk::kTrailOffsets : pk::kQuadOffsets;
    const std::uint32_t n = trail ? 8u : 4u;
    const double dt = std::min(1.0 / 30.0, static_cast<double>(in.deltaTimeSecs));
    for (std::uint32_t k = 0; k < n; ++k) {
        const double ox = offs[k * 2], oy = offs[k * 2 + 1];
        D3 w{};
        if (trail) {
            const double mx = -dotd(vel, right), my = -dotd(vel, bup);
            const double speed = std::hypot(mx, my);
            const double dx = speed > 0 ? mx / speed : 0.0, dy = speed > 0 ? my / speed : 1.0;
            const double width = ox * sx;
            const double height = oy * sy + (k >= 4 ? speed * g.motionTrailMultiplier * dt : 0.0);
            w = right * (width * -dy + height * dx) + bup * (width * dx + height * dy);
        } else {
            const double lx = ox * sx, ly = oy * sy;
            w = right * (c * lx - s * ly) + bup * (s * lx + c * ly);
        }
        q.pos.push_back(pos + w);
        const double uy = trail ? oy : -oy;
        q.uv.push_back({(ox * fx + 0.5) * cw + u0, (uy * fy + 0.5) * ch + v0});
    }
    return q;
}

void checkBillboards(const char* label, const ParticleSystemDesc& desc, std::uint32_t frames) {
    ParticleSystemManager m(smallConfig(1));
    CpuParticleBackend b(kernel::Backend::CpuReference);
    b.init(m.config());
    Scenario s{label, desc, quadEmitter(60.f), 77, &smokeWorld};
    Driver d({s});
    CHECK(d.registerMeshes(m));
    d.run(m, b, 0, frames, [](std::uint32_t) {});
    const std::int32_t slot = m.findSystem(d.hashes[0], 77);
    CHECK(slot >= 0);
    if (slot < 0) {
        return;
    }
    std::vector<Float4> table(kAnimationTexels);
    buildAnimationTable(desc, table.data());
    const FrameInput in = frameInput(frames - 1);
    const SystemCounters c = m.counters(static_cast<std::uint32_t>(slot));
    // After simulate the ring advanced; the vertices describe the last frame (tail and count of that frame).
    const GpuFrameConstants& k = m.constants()[static_cast<std::size_t>(slot)];
    const std::uint32_t vpp = verticesPerParticle(desc);
    std::uint32_t checked = 0, culled = 0, bad = 0;
    double maxPos = 0, maxUv = 0;
    for (std::uint32_t i = 0; i < desc.gpu.maxNumParticles; ++i) {
        const GpuParticleVertex* v = &b.vertices()[k.vertexBase + i * vpp];
        if (i >= k.particleCount) {
            bool zero = true;
            for (std::uint32_t j = 0; j < vpp; ++j) {
                zero = zero && v[j].position[0] == 0.f && v[j].position[1] == 0.f && v[j].position[2] == 0.f && v[j].color == 0u;
            }
            bad += zero ? 0u : 1u;
            continue;
        }
        const GpuParticle& p = b.particles()[k.particleBase + (i + k.particleTailOffset) % desc.gpu.maxNumParticles];
        const auto ref = referenceBillboard(desc, table, p, in);
        if (!ref) {
            ++culled;
            bad += v[0].color == 0u && v[0].position[0] == 0.f ? 0u : 1u;
            continue;
        }
        ++checked;
        for (std::uint32_t j = 0; j < vpp; ++j) {
            for (int a = 0; a < 3; ++a) {
                const double r = a == 0 ? ref->pos[j].x : (a == 1 ? ref->pos[j].y : ref->pos[j].z);
                const double e = std::fabs(v[j].position[a] - r) / (1.0 + std::fabs(r));
                maxPos = std::max(maxPos, e);
            }
            maxUv = std::max({maxUv, std::fabs(v[j].texcoord[0] - ref->uv[j][0]), std::fabs(v[j].texcoord[1] - ref->uv[j][1])});
            for (int ch = 0; ch < 4; ++ch) {
                const double got = ((v[j].color >> (8 * ch)) & 0xFF) / 255.0;
                bad += std::fabs(got - ref->color[static_cast<std::size_t>(ch)]) <= 1.0 / 255.0 + 1e-6 ? 0u : 1u;
            }
        }
        // Geometric sanity: the quad (or the tail quad of a trail) is centred on the particle.
        if (vpp == 4) {
            D3 center{0, 0, 0};
            for (std::uint32_t j = 0; j < 4; ++j) {
                center = center + D3{v[j].position[0], v[j].position[1], v[j].position[2]} * 0.25;
            }
            const double dc = std::sqrt(dotd(center - D3{p.position[0], p.position[1], p.position[2]},
                                             center - D3{p.position[0], p.position[1], p.position[2]}));
            bad += dc < 1e-3 * (1.0 + std::fabs(p.position[1])) ? 0u : 1u;
        }
    }
    // Tolerances: the kernel is f32 with 4 mantissa bits chopped (2^-19 relative) against a double reference; the
    // basis goes through a few normalizations (~1e-7 relative each).
    std::printf("billboards %-24s: %u checked, %u culled, max pos err %.2e (rel), max uv err %.2e, count %u\n", label, checked,
                culled, maxPos, maxUv, c.particleCount);
    CHECK(checked > 10 && bad == 0 && maxPos < 1e-5 && maxUv < 1e-5);
}

void testBillboards() {
    for (std::uint32_t type = 0; type < 4; ++type) {
        ParticleSystemDesc d = fountainDesc();
        d.gpu.billboardType = type;
        d.gpu.initialRotationDeviationDegrees = 60.f;
        d.minRotationSpeed = {{2.f, 0.f, 0.f, 0.f}, {-1.f, 0.f, 0.f, 0.f}};
        const char* names[4] = {"spherical+rotation", "up-axis-locked", "camera-position", "world-up"};
        checkBillboards(names[type], d, 40);
    }
    ParticleSystemDesc sheet = fountainDesc();
    sheet.gpu.spriteSheetRows = 2;
    sheet.gpu.spriteSheetCols = 4;
    sheet.gpu.spriteSheetMode = static_cast<std::uint32_t>(SpriteSheetMode::OverrideMaterialLifetime);
    sheet.gpu.randomFlipAxis = static_cast<std::uint32_t>(RandomFlipAxis::Horizontal);
    checkBillboards("sheet-lifetime+flipH", sheet, 40);
    sheet.gpu.spriteSheetMode = static_cast<std::uint32_t>(SpriteSheetMode::OverrideMaterialRandom);
    sheet.gpu.randomFlipAxis = static_cast<std::uint32_t>(RandomFlipAxis::Vertical);
    checkBillboards("sheet-random+flipV", sheet, 40);
    checkBillboards("sparks (trail, aligned)", sparksDesc(), 30);
    checkBillboards("smoke (turbulence)", smokeDesc(), 90);

    // Screen-size culling: a 1 cm particle 5 m away is below 2 pixels at 1280 x 720 and is culled.
    ParticleSystemDesc tiny = fountainDesc();
    tiny.minSize = {{0.2f, 0.2f, 0.f, 0.f}, {0.2f, 0.2f, 0.f, 0.f}};
    tiny.maxSize = tiny.minSize;
    ParticleSystemManager m(smallConfig(1));
    CpuParticleBackend b(kernel::Backend::CpuReference);
    b.init(m.config());
    Driver d({Scenario{"tiny", tiny, quadEmitter(20.f), 5, &smokeWorld}});
    CHECK(d.registerMeshes(m));
    d.run(m, b, 0, 10, [](std::uint32_t) {});
    std::uint32_t nonZero = 0;
    for (const GpuParticleVertex& v : b.vertices()) {
        nonZero += v.color != 0u ? 1u : 0u;
    }
    CHECK(nonZero == 0u && m.counters(0).particleCount > 0u);
}

// ---- zero_alloc -----------------------------------------------------------------------------------------------------

void testZeroAlloc() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (int mode = 0; mode < 2; ++mode) {
        scheduler.shutdown();
        if (mode == 1) {
            scheduler.initialize(2);
        }
        const kernel::Backend be = mode == 0 ? kernel::Backend::CpuReference : kernel::Backend::CpuParallel;
        ParticleSystemManager m(smallConfig(2));
        CpuParticleBackend b(be);
        b.init(m.config());
        Driver d(scenarios());
        CHECK(d.registerMeshes(m));
        d.run(m, b, 0, 60, [](std::uint32_t) {}); // warm up: systems created, stats registered
        g_allocs = 0;
        g_countAllocs = true;
        d.run(m, b, 60, 120, [](std::uint32_t) {});
        g_countAllocs = false;
        std::printf("zero_alloc %s: %llu allocations over 120 steady frames\n", mode == 0 ? "CpuReference" : "CpuParallel(2)",
                    static_cast<unsigned long long>(g_allocs.load()));
        CHECK(g_allocs.load() == 0u);
    }
    scheduler.shutdown();
}

// ---- bridge ---------------------------------------------------------------------------------------------------------

replace::DrawInput drawWithKey(std::uint32_t index, std::uint64_t key, std::uint64_t texture, const replace::Mat4d& world,
                               std::uint64_t instance) {
    replace::DrawInput d;
    d.index = index;
    d.geometryValid = true;
    d.hashes[fuse::relight::hash::HashComponent::GeometryDescriptor] = key;
    d.materialHash = texture;
    d.objectToWorld = world;
    d.instanceId = instance;
    return d;
}

replace::Mat4d translation4(double x, double y, double z) {
    replace::Mat4d m = replace::identity4d();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

void testBridge(const fs::path& tmp) {
    const fs::path root = tmp / "bridge";
    fs::remove_all(root);
    fs::create_directories(root / "rtx-remix" / "mods" / "scenes");
    fs::copy_file(g_fixtures / "scenes" / "mod.usda", root / "rtx-remix" / "mods" / "scenes" / "mod.usda");
    replace::EngineConfig cfg;
    cfg.roots = {{(root / "rtx-remix" / "mods").generic_string(), replace::ModKind::Remix}};
    cfg.gameId = "unit";
    cfg.hotReload = false;
    cfg.optionLayers = false;
    {
        // The preset's default 10000 particles exceed the test pools: a smaller preset through the options.
        ScopedConf preset("rtx.particles.globalPreset.numberOfParticlesPerMaterial = 500\n");
        replace::ReplacementEngine engine(cfg);
        ParticleSystemManager m(smallConfig(1));
        CpuParticleBackend b(kernel::Backend::CpuReference);
        b.init(m.config());
        ParticleCatalog catalog(engine);
        ParticleEmitterBridge bridge(m, catalog);
        const EmitterData quad = quadEmitter(40.f);
        const EmitterMeshId original = m.registerMesh(quad.view());
        std::uint32_t hiddenParts = 0, hiddenOriginal = 0;
        EmitterFrameStats last{};
        for (std::uint32_t f = 0; f < 40; ++f) {
            engine.beginFrame(f);
            bridge.beginFrame(f);
            m.beginFrame(frameInput(f), b);
            // Fountain: the mesh replacement (hidden original, one part), moving along +x.
            const replace::DrawInput a = drawWithKey(0, 0xF0074A10ull, 0x1234, translation4(10.0 * f, 0, 0), 11);
            const replace::ReplacedDraw ra = engine.replaceDraw(a);
            CHECK(ra.meshReplaced && ra.parts.size() == 1 && !ra.drawOriginal);
            const EmitterResult ea = bridge.processDraw(a, ra, original);
            hiddenParts += (ea.hiddenParts & 1u) != 0u ? 1u : 0u;
            // Sparks: a kept original with the sparks material (texture hash 0x5A4B50).
            const replace::DrawInput s = drawWithKey(1, 0x77, 0x5A4B50ull, translation4(-200, 0, 0), 12);
            const replace::ReplacedDraw rs = engine.replaceDraw(s);
            CHECK(!rs.meshReplaced && rs.materialReplaced && rs.drawOriginal);
            const EmitterResult es = bridge.processDraw(s, rs, original);
            hiddenOriginal += es.hideOriginal ? 1u : 0u;
            // A ParticleEmitter-tagged draw without USD particles: the global preset.
            replace::DrawInput p = drawWithKey(2, 0x78, 0x999, translation4(0, 0, -300), 13);
            p.categories.set(fuse::relight::scene::InstanceCategories::ParticleEmitter);
            const replace::ReplacedDraw rp = engine.replaceDraw(p);
            bridge.processDraw(p, rp, original);
            // An untagged draw with no replacement: nothing.
            const replace::DrawInput n = drawWithKey(3, 0x79, 0x998, replace::identity4d(), 14);
            CHECK(bridge.processDraw(n, engine.replaceDraw(n), original).requests == 0u);
            engine.endFrame({});
            CHECK(m.simulate(b));
            last = bridge.stats();
        }
        for (const std::string& diag : catalog.diagnostics()) {
            std::fprintf(stderr, "  catalog: %s\n", diag.c_str());
        }
        CHECK(catalog.diagnostics().empty());
        std::printf("bridge stats: %u emitters (%u mesh, %u material, %u preset), %u particles, %u hidden; %u systems\n", last.emitters,
                    last.meshSystems, last.materialSystems, last.presetSystems, last.particles, last.hiddenEmitters, m.activeSystemCount());
        CHECK(last.emitters == 3u && last.meshSystems == 1u && last.materialSystems == 1u && last.presetSystems == 1u);
        CHECK(hiddenParts == 40u && hiddenOriginal == 0u && last.hiddenEmitters == 1u);
        CHECK(m.activeSystemCount() == 3u);
        // The fountain spawns from the part's geometry (100 x 100 quad) at the draw's transform: every particle of the
        // first frames lies above the quad's footprint, which moves 10 cm per frame along x.
        std::uint32_t found = 0;
        for (const SystemDraw& dr : m.draws()) {
            const ParticleSystemDesc* desc = m.systemDesc(dr.system);
            CHECK(desc != nullptr);
            if (desc == nullptr || dr.descHash != hashDesc(fountainDesc())) {
                continue;
            }
            ++found;
            const std::uint32_t base = m.particleBase(dr.system);
            std::uint32_t alive = 0, inside = 0;
            for (std::uint32_t k = 0; k < desc->gpu.maxNumParticles; ++k) {
                const GpuParticle& q = b.particles()[base + k];
                if (q.state != kParticleAlive || q.timeToLive <= 0.f) {
                    continue;
                }
                ++alive;
                // Emitter at x in [0, 390] over the run; 0.5 x 600 cm/s inherited for <= 1 s; y in [0, 20.4].
                inside += q.position[0] >= -50.f - 1.f && q.position[0] <= 390.f + 50.f + 300.f + 1.f && std::fabs(q.position[2]) <= 50.001f &&
                                  q.position[1] >= -300.f && q.position[1] <= 20.5f
                              ? 1u
                              : 0u;
            }
            std::printf("bridge fountain: %u alive, %u inside the analytic bounds\n", alive, inside);
            CHECK(alive > 100 && inside == alive);
        }
        CHECK(found == 1u);
        // The sparks system spawned from the draw's own geometry at (-200, 0, 0).
        bool sparksNearEmitter = false;
        for (const SystemDraw& dr : m.draws()) {
            if (dr.descHash != hashDesc(sparksDesc())) {
                continue;
            }
            const std::uint32_t base = m.particleBase(dr.system);
            std::uint32_t alive = 0, near200 = 0;
            for (std::uint32_t k = 0; k < 64; ++k) {
                const GpuParticle& q = b.particles()[base + k];
                if (q.state == kParticleAlive && q.timeToLive > 0.f) {
                    ++alive;
                    near200 += std::fabs(q.position[0] + 200.f) <= 20.f + 150.f ? 1u : 0u;
                }
            }
            sparksNearEmitter = alive > 20 && near200 == alive;
        }
        CHECK(sparksNearEmitter);
    }
    fs::remove_all(root);
}

// ---- options --------------------------------------------------------------------------------------------------------

void testOptions() {
    ParticleSystemManager m(smallConfig(1));
    CpuParticleBackend b(kernel::Backend::CpuReference);
    b.init(m.config());
    Driver d(scenarios());
    CHECK(d.registerMeshes(m));
    d.run(m, b, 0, 20, [](std::uint32_t) {});
    CHECK(m.activeSystemCount() == 3u);
    // Spawning off: no new particles, systems keep simulating.
    std::uint32_t spawned = 0;
    {
        ScopedConf conf("rtx.particles.enableSpawning = False\n");
        d.run(m, b, 20, 5, [&](std::uint32_t) {
            for (std::uint32_t i = 0; i < 3; ++i) {
                spawned += m.counters(i).spawnCount;
            }
        });
    }
    CHECK(spawned == 0u && m.activeSystemCount() == 3u);
    // Time scale 0: nothing moves.
    {
        ScopedConf conf("rtx.particles.timeScale = 0\nrtx.particles.enableSpawning = False\n");
        std::vector<GpuParticle> before(b.particles().begin(), b.particles().end());
        d.run(m, b, 25, 3, [](std::uint32_t) {});
        bool frozen = true;
        for (std::size_t i = 0; i < before.size(); ++i) {
            frozen = frozen && std::memcmp(before[i].position, b.particles()[i].position, sizeof(before[i].position)) == 0;
        }
        CHECK(frozen);
    }
    // Disabled: every system is dropped.
    {
        ScopedConf conf("rtx.particles.enable = False\n");
        d.run(m, b, 28, 1, [](std::uint32_t) {});
        CHECK(m.activeSystemCount() == 0u && m.draws().empty());
    }
    d.run(m, b, 29, 1, [](std::uint32_t) {});
    CHECK(m.activeSystemCount() == 3u);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <suite> --fixtures <dir> [--update-golden]\n", argv[0]);
        return 2;
    }
    const std::string suite = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
            g_fixtures = argv[++i];
        } else if (std::strcmp(argv[i], "--update-golden") == 0) {
            g_updateGolden = true;
        }
    }
    registerParticleOptions();
    const fs::path tmp = fs::current_path() / "rl_particles_tmp";
    if (suite == "curve") {
        testCurve();
    } else if (suite == "desc") {
        testDesc();
    } else if (suite == "rng") {
        testRng();
    } else if (suite == "parity") {
        testParity();
    } else if (suite == "fixtures") {
        testFixtures();
    } else if (suite == "billboards") {
        testBillboards();
    } else if (suite == "zero_alloc") {
        testZeroAlloc();
    } else if (suite == "bridge") {
        testBridge(tmp);
        std::error_code ec;
        fs::remove_all(tmp, ec);
    } else if (suite == "options") {
        testOptions();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS rl_particles_%s\n", suite.c_str());
    return 0;
}
