// FUSE Relight RL-4.4 CPU gates: the Relight light model and its WP-7.1 light-tree integration
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.3, §5.8 row `light_sample/pdf`, `light_tree_*`).
//
//   convert     the single-source D3DLIGHT9 conversion (light_d3d_convert, CpuReference) == RL-1.5's
//               convertLegacyLight bit for bit over 20k random lights (point / spot / directional, every attenuation
//               regime, least squares on and off, option overrides); the ff_lit lights (and the d3d8 twin, same
//               values) == the RL-1.5 capture reference (rl_translate_capture.py ref_light, float-emulated) and its
//               stable light hashes.
//   pdf         rlLightPdf of every light kind (sphere outside / inside, rect, disk, triangle one- / two-sided,
//               cylinder outside / inside the tube, distant cone) integrates to 1 ± 1e-3 over the sphere of directions
//               (stratified quadrature over a bounding cone, independent of the samplers); the light set's density
//               (tree pmf x light pdf) integrates to 1 - P(delta lights) ± 1e-3, and the tree pmf sums to 1.
//   sample      per light: sample.pdf == rlLightPdf(sample.wi), sample.radiance == rlLightEval(sample.wi), and the
//               sampled estimator of the cosine-weighted incident radiance == brute-force quadrature (4 sigma); the
//               light set (game + authored + emissive + fallback lights, WP-7.1 tree selection) the same against the
//               brute-force sum over every light, tree selection frequencies == pmf, sample pdf == set.pdf(), and
//               every light that contributes at a point has a non-zero selection probability (unbiased).
//   usd         UsdLux / Remix light mapping (SphereLight, RectLight, DiskLight, CylinderLight, DistantLight, the
//               transform, exposure, shaping cone / softness / focus, DomeLight rejected) and rlShaping semantics.
//   emissive    emissive triangles from captured geometry (16 / 32-bit indices, non-indexed, transforms, degenerate
//               and out-of-range triangles, the per-frame cap, two-sided flag, entry keys).
//   set         table / tree assembly (order game -> authored -> emissive, tree light index == table index, proxies),
//               refit vs rebuild, RL-1.5 rejection rules (invalid type, rtx.ignoreGame*Lights, "off", same hash), the
//               fallback light modes.
//   parity      light_d3d_convert and light_set_sample: CpuReference == CpuParallel bit for bit.
//   zero_alloc  steady-state frames (game lights, authored lights, emissive meshes, build / refit, sampling): no
//               operator-new call.
#include <fuse/relight/render/lights/light_set.hpp>

#include "light_kernels.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/relight/hash/light_hash.hpp>
#include <fuse/relight/mods/import/light_table.hpp>
#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>
#include <fuse/relight/scene/lights/light_options.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
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

using namespace fuse;
using namespace fuse::relight;
namespace rl = fuse::relight::render::lights;
namespace lk = fuse::relight::lightk;
namespace lt = fuse::renderer::light_tree;
using lk::float3;
using lk::float4;
using lk::RlLight;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

/// Stand-ins for options other packages own (the light set reads them by name).
struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
    FUSE_RELIGHT_OPTION("rtx", int, fallbackLightMode, 1, "Test stand-in for RL-4.2's option.");
    FUSE_RELIGHT_OPTION("rtx", options::Vec3f, fallbackLightRadiance, options::Vec3f(1.6f, 1.8f, 2.0f),
                        "Test stand-in for RL-4.2's option.");
    FUSE_RELIGHT_OPTION("rtx", options::Vec3f, fallbackLightDirection, options::Vec3f(-0.2f, -1.0f, 0.4f),
                        "Test stand-in for RL-4.2's option.");
};

/// rtx.conf text applied as an option layer for the scope's lifetime.
class ScopedConf {
public:
    explicit ScopedConf(const std::string& text) {
        static int s_counter = 0;
        const options::OptionConfig config = options::OptionConfig::parse(text);
        m_layer = options::OptionManager::acquireLayer(
            "", {7000u + static_cast<std::uint32_t>(s_counter++), "rl_lights_test"}, 1.0f, 0.1f, false, &config);
        options::OptionManager::applyPendingValues(nullptr, false);
    }
    ~ScopedConf() {
        m_layer = options::OptionLayerHandle();
        options::OptionManager::applyPendingValues(nullptr, false);
    }
    ScopedConf(const ScopedConf&) = delete;
    ScopedConf& operator=(const ScopedConf&) = delete;

private:
    options::OptionLayerHandle m_layer;
};

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1u) {}
    u32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
    float uniform() { return static_cast<float>(next() >> 7) * (1.f / 16777216.f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
};

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
bool sameBits3(const float3& a, const float3& b) { return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z); }
float3 f3(const scene::Float3& v) { return float3(v[0], v[1], v[2]); }
bool near(double a, double b, double rel, double abs = 0.0) { return std::fabs(a - b) <= rel * std::fabs(b) + abs; }
/// Value equality (+0 == -0).
bool eq3(const float3& a, const float3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
float maxc(const float3& c) { return std::max(c.x, std::max(c.y, c.z)); }

// ---- D3D lights ------------------------------------------------------------------------------------------------------

tap::Light d3dLight(u32 index, u32 type) {
    tap::Light l;
    l.index = index;
    l.enabled = true;
    l.type = type;
    return l;
}

/// ff_lit's float normalize (rl_math.h): sqrtf(dot) then a division per component.
tap::Vec3 appNormalize(float x, float y, float z) {
    const float l = std::sqrt(x * x + y * y + z * z);
    return {x / l, y / l, z / l};
}

/// The four lights of the ff_lit test app (Tests/relight/apps/scenes/ff_lit.cpp; d3d8_ff_lit sets the same values).
std::vector<tap::Light> ffLitLights() {
    std::vector<tap::Light> v;
    tap::Light dir = d3dLight(0, scene::d3dlight::DIRECTIONAL);
    dir.diffuse = {0.8f, 0.8f, 0.75f, 1.f};
    dir.direction = appNormalize(0.4f, -0.6f, 0.7f);
    v.push_back(dir);
    tap::Light pt = d3dLight(1, scene::d3dlight::POINT);
    pt.diffuse = {1.0f, 0.3f, 0.2f, 1.f};
    pt.position = {-1.6f, 1.0f, -1.0f};
    pt.range = 6.0f;
    pt.attenuation0 = 0.2f;
    pt.attenuation1 = 0.3f;
    pt.attenuation2 = 0.05f;
    v.push_back(pt);
    tap::Light spot = d3dLight(2, scene::d3dlight::SPOT);
    spot.diffuse = {0.2f, 0.4f, 1.0f, 1.f};
    spot.position = {1.5f, 2.0f, -1.5f};
    spot.direction = appNormalize(-0.3f, -1.0f, 0.6f);
    spot.range = 10.0f;
    spot.attenuation0 = 1.0f;
    spot.theta = 0.35f;
    spot.phi = 0.9f;
    spot.falloff = 1.0f;
    v.push_back(spot);
    tap::Light off = d3dLight(3, scene::d3dlight::POINT);
    off.enabled = false;
    off.diffuse = {0.0f, 1.0f, 0.0f, 1.f};
    off.position = {0.0f, 0.0f, -2.0f};
    off.range = 100.0f;
    off.attenuation0 = 1.0f;
    v.push_back(off);
    return v;
}

tap::Light randomD3dLight(Rng& rng, u32 index) {
    tap::Light l = d3dLight(index, 1u + rng.next() % 3u);
    if (rng.next() % 50u == 0u) {
        l.type = rng.next() % 2u == 0u ? 0u : 4u; // invalid
    }
    const float scale = rng.range(0.05f, 3.f);
    l.diffuse = {rng.range(0.f, 1.f) * scale, rng.range(0.f, 1.f) * scale, rng.range(0.f, 1.f) * scale, 1.f};
    if (rng.next() % 40u == 0u) {
        l.diffuse.g = -0.2f; // subtractive light: "off"
    }
    l.position = {rng.range(-50.f, 50.f), rng.range(-50.f, 50.f), rng.range(-50.f, 50.f)};
    l.direction = {rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)};
    l.range = rng.range(0.5f, 200.f);
    switch (rng.next() % 5u) {
    case 0: // constant only
        l.attenuation0 = rng.range(0.f, 2.f);
        break;
    case 1: // linear
        l.attenuation0 = rng.range(0.f, 1.f);
        l.attenuation1 = rng.range(0.001f, 1.f);
        break;
    case 2: // quadratic
        l.attenuation0 = rng.range(0.f, 1.f);
        l.attenuation1 = rng.range(0.f, 0.5f);
        l.attenuation2 = rng.range(0.0001f, 0.3f);
        break;
    case 3: // dark next to the light (constant term too large)
        l.attenuation0 = rng.range(300.f, 1000.f);
        break;
    default: // tiny terms
        l.attenuation0 = rng.range(0.f, 1e-6f);
        l.attenuation1 = rng.range(0.f, 1e-6f);
        l.attenuation2 = rng.range(0.f, 1e-6f);
        break;
    }
    l.phi = rng.range(0.05f, 3.1f);
    l.theta = rng.range(0.f, l.phi);
    l.falloff = rng.next() % 3u == 0u ? 1.f : rng.range(0.f, 4.f);
    return l;
}

/// CPU kernel conversion of one light (light_d3d_convert on CpuReference, as RelightLightSet::build runs it).
RlLight kernelConvert(const tap::Light& l, const lk::RlConvertParams& params) {
    float4 in[lk::kRlD3dWords];
    float4 out[lk::kRlLightWords];
    rl::packD3dLight(l, in);
    lk::ConvertParams p{};
    p.d3d = kernel::Span<const float4>{in, lk::kRlD3dWords};
    p.out = kernel::Span<float4>{out, lk::kRlLightWords};
    p.params = params;
    p.count = 1;
    const bool ok = kernel::launch(kernel::Backend::CpuReference,
                                   kernel::KernelLaunch{lk::kConvertName, kernel::extent1(1u), lk::kWorkgroup},
                                   lk::ConvertKernel{}, p)
                        .ok;
    check(ok, "light_d3d_convert launch");
    return lk::rlLightUnpack(out);
}

/// Compares the kernel conversion with RL-1.5's convertLegacyLight. Returns false (and reports) on a mismatch.
bool matchesRl15(const tap::Light& l, const RlLight& k, std::string& why) {
    const std::optional<scene::LightRecord> r = scene::convertLegacyLight(l);
    if (!r.has_value() || r->isOff()) {
        why = "RL-1.5 rejects it, the kernel gives kind " + std::to_string(k.kind);
        return k.kind == lk::kRlKindNone;
    }
    if (r->type == hash::LightType::Distant) {
        why = "distant";
        return k.kind == lk::kRlKindDistant && sameBits3(k.u, f3(r->direction)) && sameBits(k.radius, r->halfAngle) &&
               sameBits3(k.radiance, f3(r->radiance)) && (k.flags & lk::kRlFlagShaped) == 0u;
    }
    why = "sphere";
    const bool shaped = (k.flags & lk::kRlFlagShaped) != 0u;
    bool ok = k.kind == lk::kRlKindSphere && sameBits3(k.position, f3(r->position)) && sameBits(k.radius, r->radius) &&
              sameBits3(k.radiance, f3(r->radiance)) && shaped == r->shaping.enabled;
    if (ok && shaped) {
        ok = sameBits3(k.axis, f3(r->shaping.direction)) && sameBits(k.cosCone, r->shaping.cosConeAngle) &&
             sameBits(k.softness, r->shaping.coneSoftness) && sameBits(k.focus, r->shaping.focusExponent);
        why = "sphere shaping";
    }
    return ok;
}

void suiteConvert() {
    u32 checked = 0;
    u32 accepted = 0;
    u32 mismatches = 0;
    const char* confs[] = {
        "",
        "rtx.calculateLightIntensityUsingLeastSquares = False\n",
        "rtx.lightConversionSphereLightFixedRadius = 0.5\nrtx.lightConversionIntensityFactor = 2.5\n"
        "rtx.lightConversionMaxIntensity = 40\nrtx.lightConversionDistantLightFixedIntensity = 3\n"
        "rtx.lightConversionDistantLightFixedAngle = 0.2\n",
        "rtx.sceneScale = 0.01\n",
    };
    Rng rng(1);
    for (const char* text : confs) {
        ScopedConf conf(text);
        const lk::RlConvertParams params = rl::convertParamsFromOptions();
        for (u32 i = 0; i < 5000u; ++i) {
            const tap::Light l = randomD3dLight(rng, i);
            const RlLight k = kernelConvert(l, params);
            std::string why;
            ++checked;
            if (k.kind != lk::kRlKindNone) {
                ++accepted;
            }
            if (!matchesRl15(l, k, why)) {
                if (mismatches < 5u) {
                    std::fprintf(stderr, "  light %u (type %u, conf '%s'): %s\n", i, l.type, text, why.c_str());
                }
                ++mismatches;
            }
        }
    }
    std::printf("convert: %u lights (%u accepted), %u mismatch(es) against RL-1.5 convertLegacyLight\n", checked,
                accepted, mismatches);
    check(mismatches == 0u, "the kernel conversion == RL-1.5 convertLegacyLight bit for bit");
    check(accepted > checked / 2u, "most random lights convert");

    // ff_lit: the RL-1.5 capture reference (rl_translate_capture.py ref_light, float-emulated, run on these inputs).
    ScopedConf defaults("");
    const lk::RlConvertParams params = rl::convertParamsFromOptions();
    const std::vector<tap::Light> app = ffLitLights();
    struct Expected {
        u32 kind;
        hash::Hash64 hash;
        float radiance[3];
    };
    const Expected expected[3] = {
        {lk::kRlKindDistant, 0x52929e1042a73d88ull, {0.800000011920929f, 0.800000011920929f, 0.75f}},
        {lk::kRlKindSphere, 0x70660ffad292af8dull, {0.12710805237293243f, 0.03813241794705391f, 0.025421610102057457f}},
        {lk::kRlKindSphere, 0x8b0a310eb4127a3eull, {0.00397887360304594f, 0.00795774720609188f, 0.019894367083907127f}},
    };
    for (u32 i = 0; i < 3u; ++i) {
        const RlLight k = kernelConvert(app[i], params);
        std::string why;
        check(matchesRl15(app[i], k, why), "ff_lit light " + std::to_string(i) + " == RL-1.5 (" + why + ")");
        check(k.kind == expected[i].kind, "ff_lit light " + std::to_string(i) + " kind");
        check(scene::stableLightHash(app[i]) == expected[i].hash, "ff_lit light " + std::to_string(i) + " hash");
        for (u32 c = 0; c < 3u; ++c) {
            check(near(k.radiance[static_cast<int>(c)], expected[i].radiance[c], 1e-6),
                  "ff_lit light " + std::to_string(i) + " radiance");
        }
    }
    const RlLight dir = kernelConvert(app[0], params);
    check(near(dir.u.x, 0.39801493287086487, 1e-6) && near(dir.u.y, -0.5970223546028137, 1e-6) &&
              near(dir.u.z, 0.696526050567627, 1e-6) && near(dir.radius, 0.01744999922811985, 1e-6),
          "ff_lit directional: direction and half angle");
    const RlLight pt = kernelConvert(app[1], params);
    check(pt.radius == 4.f && (pt.flags & lk::kRlFlagShaped) == 0u && sameBits3(pt.position, float3(-1.6f, 1.f, -1.f)),
          "ff_lit point: radius 4, unshaped, raw position");
    const RlLight spot = kernelConvert(app[2], params);
    check((spot.flags & lk::kRlFlagShaped) != 0u && near(spot.cosCone, 0.9004471302032471, 1e-6) &&
              near(spot.softness, 0.08427941799163818, 1e-6) && spot.focus == 1.f &&
              near(spot.axis.x, -0.24913644790649414, 1e-6) && near(spot.axis.y, -0.8304547667503357, 1e-6) &&
              near(spot.axis.z, 0.4982728958129883, 1e-6),
          "ff_lit spot: shaping (cos cone, softness, focus, axis)");
    // The set: the disabled light is skipped, entries carry the stable hashes, table == the conversion.
    rl::RelightLightSet set;
    set.beginFrame();
    check(set.addGameLights(app.data(), static_cast<u32>(app.size())) == 3u, "ff_lit: 3 enabled lights added");
    check(set.build(), "ff_lit: build");
    for (u32 i = 0; i < 3u; ++i) {
        check(set.entries()[i].key == expected[i].hash && set.entries()[i].source == i &&
                  set.entries()[i].origin == rl::LightOrigin::Game,
              "ff_lit: entry " + std::to_string(i));
        std::string why;
        check(matchesRl15(app[i], set.light(i), why), "ff_lit: table entry " + std::to_string(i));
    }
    std::printf("convert: ff_lit (3 lights + 1 disabled) == RL-1.5 capture reference (radiance <= 1e-6, hashes)\n");
}

// ---- quadrature ----------------------------------------------------------------------------------------------------

struct Cone {
    float3 axis{0.f, 0.f, 1.f};
    double omMax = 2.0; ///< 1 - cos(half angle); 2 = the whole sphere
};

/// A cone of directions from p containing every direction towards L (conservative).
Cone boundingCone(const RlLight& L, const float3& p) {
    Cone c;
    if (L.kind == lk::kRlKindDistant) {
        c.axis = -L.u;
        c.omMax = std::min(2.0, double(lk::rlDistantOneMinusCos(L)) * 1.1 + 1e-9);
        return c;
    }
    float3 center = L.position;
    double r = 0.0;
    switch (L.kind) {
    case lk::kRlKindSphere:
        r = L.radius;
        break;
    case lk::kRlKindRect:
        r = lk::length(L.u) + lk::length(L.v);
        break;
    case lk::kRlKindDisk:
        r = std::max(lk::length(L.u), lk::length(L.v));
        break;
    case lk::kRlKindCylinder:
        r = std::sqrt(double(lk::dot(L.u, L.u)) + double(L.radius) * L.radius);
        break;
    case lk::kRlKindTriangle: {
        center = L.position + (L.u + L.v) * (1.f / 3.f);
        const float3 v[3] = {L.position, L.position + L.u, L.position + L.v};
        for (const float3& x : v) {
            r = std::max(r, double(lk::length(x - center)));
        }
        break;
    }
    default:
        break;
    }
    r *= 1.001;
    const float3 d = center - p;
    const double dist = lk::length(d);
    if (dist <= r * 1.01) {
        c.omMax = 2.0;
        return c;
    }
    c.axis = d * (1.f / static_cast<float>(dist));
    const double s2 = (r * r) / (dist * dist);
    c.omMax = std::min(2.0, (s2 / (1.0 + std::sqrt(1.0 - s2))) * 1.02 + 1e-7);
    return c;
}

/// Stratified (jittered) quadrature of f(wi) over the cone: n x n cells equal in solid angle.
template <class F>
double coneIntegral(const Cone& cone, u32 n, Rng& rng, F&& f) {
    float3 t;
    float3 b;
    lk::rlBasis(cone.axis, t, b);
    const double dOmega = 2.0 * 3.14159265358979323846 * cone.omMax / (double(n) * double(n));
    double sum = 0.0;
    for (u32 i = 0; i < n; ++i) {
        double row = 0.0;
        for (u32 j = 0; j < n; ++j) {
            const double om = cone.omMax * (double(i) + double(rng.uniform())) / double(n);
            const double phi = 2.0 * 3.14159265358979323846 * (double(j) + double(rng.uniform())) / double(n);
            const double cosT = 1.0 - om;
            const double sinT = std::sqrt(std::max(0.0, om * (2.0 - om)));
            const float3 wi = lk::normalize(t * static_cast<float>(sinT * std::cos(phi)) +
                                            b * static_cast<float>(sinT * std::sin(phi)) +
                                            cone.axis * static_cast<float>(cosT));
            row += f(wi);
        }
        sum += row;
    }
    return sum * dOmega;
}

/// Integral of rlLightPdf(L, p, .) over the sphere of directions. Direction-space stratified quadrature, except for
/// cylinders: their direction density sums both intersections of a ray with the tube and has an integrable 1 / cos
/// singularity along the silhouette that direction-space quadrature resolves poorly (float round-off near tangency),
/// so it is integrated over the lateral surface instead: integral pdf(w) dw = integral over x of
/// pdf(w(x)) |cos_x| / (t_x^2 N(w(x))) dA, N = the number of tube hits along w(x) (counted here in double).
double pdfIntegral(const RlLight& L, const float3& p, Rng& rng, u32 n) {
    if (L.kind != lk::kRlKindCylinder) {
        return coneIntegral(boundingCone(L, p), n, rng, [&](const float3& wi) { return double(lk::rlLightPdf(L, p, wi)); });
    }
    const double h = lk::length(L.u);
    const float3 A = L.u * static_cast<float>(1.0 / h);
    float3 t;
    float3 b;
    lk::rlBasis(A, t, b);
    const double r = L.radius;
    const double dA = (2.0 * h / n) * (2.0 * 3.14159265358979323846 * r / n);
    double sum = 0.0;
    for (u32 i = 0; i < n; ++i) {
        for (u32 j = 0; j < n; ++j) {
            const double z = -h + 2.0 * h * (double(i) + double(rng.uniform())) / n;
            const double phi = 2.0 * 3.14159265358979323846 * (double(j) + double(rng.uniform())) / n;
            const double nrm[3] = {t.x * std::cos(phi) + b.x * std::sin(phi), t.y * std::cos(phi) + b.y * std::sin(phi),
                                   t.z * std::cos(phi) + b.z * std::sin(phi)};
            double d[3];
            double dist2 = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double x = double(L.position[k]) + double(A[k]) * z + nrm[k] * r;
                d[k] = x - double(p[k]);
                dist2 += d[k] * d[k];
            }
            const double dist = std::sqrt(dist2);
            const double w[3] = {d[0] / dist, d[1] / dist, d[2] / dist};
            const double cosX = std::fabs(w[0] * nrm[0] + w[1] * nrm[1] + w[2] * nrm[2]);
            // Tube hits along w (double).
            const double o[3] = {double(p.x - L.position.x), double(p.y - L.position.y), double(p.z - L.position.z)};
            const double oa = o[0] * A.x + o[1] * A.y + o[2] * A.z;
            const double wa = w[0] * A.x + w[1] * A.y + w[2] * A.z;
            const double op[3] = {o[0] - A.x * oa, o[1] - A.y * oa, o[2] - A.z * oa};
            const double wp[3] = {w[0] - A.x * wa, w[1] - A.y * wa, w[2] - A.z * wa};
            const double qa = wp[0] * wp[0] + wp[1] * wp[1] + wp[2] * wp[2];
            const double qb = op[0] * wp[0] + op[1] * wp[1] + op[2] * wp[2];
            const double qc = op[0] * op[0] + op[1] * op[1] + op[2] * op[2] - r * r;
            const double disc = std::max(0.0, qb * qb - qa * qc);
            u32 hits = 0;
            for (int k = 0; k < 2; ++k) {
                const double th = k == 0 ? (-qb - std::sqrt(disc)) / qa : (-qb + std::sqrt(disc)) / qa;
                if (th > 0.0 && std::fabs(oa + th * wa) <= h) {
                    ++hits;
                }
            }
            hits = std::max(hits, 1u);
            const float3 wf(static_cast<float>(w[0]), static_cast<float>(w[1]), static_cast<float>(w[2]));
            sum += double(lk::rlLightPdf(L, p, wf)) * cosX / (dist2 * hits);
        }
    }
    return sum * dA;
}

// ---- test lights ---------------------------------------------------------------------------------------------------

struct NamedLight {
    const char* name;
    RlLight light;
};

std::vector<NamedLight> kindLights() {
    std::vector<NamedLight> v;
    v.push_back({"sphere", rl::makeSphereLight(float3(0.5f, 1.f, -0.3f), 0.7f, float3(2.f, 1.5f, 1.f))});
    RlLight shapedSphere = rl::makeSphereLight(float3(-1.f, 2.f, 0.5f), 0.4f, float3(5.f, 5.f, 4.f));
    rl::setShaping(shapedSphere, float3(0.2f, -1.f, 0.1f), 0.9f, 0.2f, 2.f);
    v.push_back({"sphere shaped", shapedSphere});
    v.push_back({"rect", rl::makeRectLight(float3(0.f, 2.f, 0.f), float3(0.8f, 0.f, 0.f), float3(0.f, 0.f, -0.5f),
                                           float3(3.f, 3.f, 3.f))});
    v.push_back({"rect two-sided", rl::makeRectLight(float3(1.f, 0.5f, 1.f), float3(0.f, 0.6f, 0.f),
                                                     float3(0.f, 0.f, 0.9f), float3(1.f, 2.f, 3.f), true)});
    v.push_back({"disk", rl::makeDiskLight(float3(-0.5f, 1.5f, 0.2f), float3(0.5f, 0.f, 0.f), float3(0.f, 0.3f, -0.4f),
                                           float3(2.f, 2.f, 2.f))});
    v.push_back({"triangle", rl::makeTriangleLight(float3(0.f, 1.f, 0.f), float3(1.f, 1.2f, 0.f), float3(0.f, 1.5f, 1.f),
                                                   float3(4.f, 1.f, 1.f))});
    v.push_back({"triangle two-sided", rl::makeTriangleLight(float3(-1.f, 0.f, 1.f), float3(-0.2f, 1.f, 1.2f),
                                                             float3(-1.f, 1.f, 0.5f), float3(1.f, 1.f, 1.f), true)});
    v.push_back({"cylinder", rl::makeCylinderLight(float3(0.3f, 1.2f, 0.4f), float3(0.9f, 0.1f, 0.f), 0.25f,
                                                   float3(3.f, 2.f, 1.f))});
    v.push_back({"distant", rl::makeDistantLight(float3(0.3f, -1.f, 0.2f), 0.12f, float3(2.f, 2.f, 1.5f))});
    v.push_back({"distant wide", rl::makeDistantLight(float3(-0.1f, -1.f, 0.5f), 0.9f, float3(1.f, 1.f, 1.f))});
    return v;
}

const float3 kPoints[] = {float3(0.f, -1.f, 0.f), float3(2.5f, 0.3f, -2.f), float3(-3.f, 3.5f, 1.5f),
                          float3(0.4f, 1.1f, 0.35f)};

void suitePdf() {
    Rng rng(2);
    u32 cases = 0;
    double worst = 0.0;
    for (const NamedLight& nl : kindLights()) {
        for (const float3& p : kPoints) {
            const double integral = pdfIntegral(nl.light, p, rng, 2048u);
            ++cases;
            worst = std::max(worst, std::fabs(integral - 1.0));
            check(std::fabs(integral - 1.0) <= 1e-3, std::string("pdf of ") + nl.name + " integrates to 1 (got " +
                                                          std::to_string(integral) + ")");
        }
    }
    // Inside a sphere light: uniform over the sphere of directions.
    {
        const RlLight s = rl::makeSphereLight(float3(0.f, 0.f, 0.f), 2.f, float3(1.f, 1.f, 1.f));
        Cone all;
        const double integral =
            coneIntegral(all, 512u, rng, [&](const float3& wi) { return double(lk::rlLightPdf(s, float3(0.3f, 0.2f, 0.f), wi)); });
        check(std::fabs(integral - 1.0) <= 1e-3, "pdf inside a sphere integrates to 1");
        ++cases;
    }
    std::printf("pdf: %u light / point cases integrate to 1 (worst |error| %.2e)\n", cases, worst);

    // The light set: sum_i pmf_i x integral(pdf_i) == 1 - P(delta), and the pmf sums to 1.
    rl::RelightLightSet set;
    set.beginFrame();
    for (const NamedLight& nl : kindLights()) {
        check(set.addLight(nl.light, 1u), std::string("add ") + nl.name);
    }
    set.addLight(rl::makeDistantLight(float3(0.f, -1.f, 0.f), 0.f, float3(1.f, 1.f, 1.f)), 2u); // delta
    check(set.build(), "set build");
    const float3 normals[] = {float3(0.f, 1.f, 0.f), float3(0.f, 0.f, 0.f), float3(0.6f, 0.f, 0.8f)};
    for (const float3& p : kPoints) {
        for (const float3& n : normals) {
            double pmfSum = 0.0;
            double deltaPmf = 0.0;
            double integral = 0.0;
            for (u32 i = 0; i < set.lightCount(); ++i) {
                const float pmf = set.pmf(p, n, i);
                pmfSum += pmf;
                const RlLight L = set.light(i);
                if (L.kind == lk::kRlKindDistant && !(L.area > 0.f)) {
                    deltaPmf += pmf;
                    continue;
                }
                if (pmf == 0.f) {
                    continue;
                }
                integral += double(pmf) * pdfIntegral(L, p, rng, 512u);
            }
            check(std::fabs(pmfSum - 1.0) <= 1e-5, "tree pmf sums to 1 (got " + std::to_string(pmfSum) + ")");
            check(std::fabs(integral - (1.0 - deltaPmf)) <= 1e-3,
                  "light-set pdf integrates to 1 - P(delta) (got " + std::to_string(integral) + " vs " +
                      std::to_string(1.0 - deltaPmf) + ")");
        }
    }
    std::printf("pdf: light set (%u lights, 1 delta) integrates to 1 - P(delta) at %zu points x %zu normals\n",
                set.lightCount(), std::size(kPoints), std::size(normals));
}

// ---- sampling --------------------------------------------------------------------------------------------------------

double cosPlus(const float3& n, const float3& wi) {
    if (n.x == 0.f && n.y == 0.f && n.z == 0.f) {
        return 1.0;
    }
    return std::max(0.0, double(lk::dot(n, wi)));
}

/// Brute force: integral of rlLightEval(L, p, wi) x cos+ over the directions (delta distant: radiance x cos).
double bruteForce(const RlLight& L, const float3& p, const float3& n, Rng& rng, u32 grid) {
    if (L.kind == lk::kRlKindDistant && !(L.area > 0.f)) {
        return double(maxc(L.radiance)) * cosPlus(n, -L.u);
    }
    return coneIntegral(boundingCone(L, p), grid, rng,
                        [&](const float3& wi) { return double(maxc(lk::rlLightEval(L, p, wi))) * cosPlus(n, wi); });
}

void suiteSample() {
    Rng rng(3);
    const float3 n(0.f, 1.f, 0.f);
    u32 cases = 0;
    u32 pdfMismatch = 0;
    u32 evalMismatch = 0;
    u32 samples = 0;
    for (const NamedLight& nl : kindLights()) {
        for (const float3& p : kPoints) {
            const RlLight& L = nl.light;
            constexpr u32 kM = 1u << 16;
            double sum = 0.0;
            double sum2 = 0.0;
            for (u32 k = 0; k < kM; ++k) {
                const lk::RlLightSample s = lk::rlLightSample(L, p, rng.uniform(), rng.uniform());
                double f = 0.0;
                if ((s.flags & lk::kRlSampleValid) != 0u && s.pdf > 0.f) {
                    ++samples;
                    const float pdf = lk::rlLightPdf(L, p, s.wi);
                    if (!near(pdf, s.pdf, 1e-3)) {
                        ++pdfMismatch;
                    }
                    const float3 e = lk::rlLightEval(L, p, s.wi);
                    if (!near(maxc(e), maxc(s.radiance), 1e-4, 1e-6)) {
                        ++evalMismatch;
                    }
                    f = double(maxc(s.radiance)) * cosPlus(n, s.wi) / double(s.pdf);
                }
                sum += f;
                sum2 += f * f;
            }
            const double mean = sum / kM;
            const double sigma = std::sqrt(std::max(0.0, sum2 / kM - mean * mean) / kM);
            const double ref = bruteForce(L, p, n, rng, 1024u);
            ++cases;
            check(std::fabs(mean - ref) <= 4.0 * sigma + 2e-3 * std::fabs(ref) + 1e-7,
                  std::string("sampled estimator of ") + nl.name + " == brute force (" + std::to_string(mean) + " vs " +
                      std::to_string(ref) + ", sigma " + std::to_string(sigma) + ")");
        }
    }
    std::printf("sample: %u light / point cases, %u samples: %u pdf and %u eval mismatch(es)\n", cases, samples,
                pdfMismatch, evalMismatch);
    check(pdfMismatch == 0u, "sample pdf == rlLightPdf(sample direction)");
    check(evalMismatch * 1000u <= samples, "sample radiance == rlLightEval(sample direction) (<= 0.1% edge rounding)");

    // The light set: game lights (the ff_lit table moved away from the shading points), authored lights of every
    // kind, an emissive mesh and the fallback light, selected by the WP-7.1 tree.
    rl::RelightLightSet set;
    set.beginFrame();
    std::vector<tap::Light> game = ffLitLights();
    for (tap::Light& l : game) {
        l.position.x += 9.f;
        l.position.y += 6.f;
    }
    set.addGameLights(game.data(), static_cast<u32>(game.size()));
    for (const NamedLight& nl : kindLights()) {
        set.addLight(nl.light, 7u);
    }
    const float quad[] = {-2.f, 3.f, -2.f, 2.f, 3.f, -2.f, 2.f, 3.f, 2.f, -2.f, 3.f, 2.f};
    const u16 quadIdx[] = {0, 1, 2, 0, 2, 3}; // lit side down (-Y)
    rl::EmissiveMesh mesh;
    mesh.positions = quad;
    mesh.vertexCount = 4;
    mesh.indices = quadIdx;
    mesh.indexCount = 6;
    mesh.radiance = float3(0.5f, 0.4f, 0.3f);
    mesh.key = 99;
    set.addEmissiveTriangles(mesh);
    set.addFallbackLight(rl::FallbackLight{2u, float3(0.3f, 0.3f, 0.3f), float3(0.1f, -1.f, 0.2f), 0.f}); // delta
    check(set.build(), "set build");
    const u32 count = set.lightCount();
    std::printf("sample: light set of %u lights (%u game, %u authored, %u emissive, fallback %s)\n", count,
                set.stats().gameLights, set.stats().authored, set.stats().emissiveTriangles,
                set.stats().fallback ? "yes" : "no");
    const float3 setPoints[] = {float3(0.f, -1.f, 0.f), float3(3.f, 0.5f, -2.5f), float3(-2.5f, 0.f, 3.f)};
    const float3 setNormals[] = {float3(0.f, 1.f, 0.f), lk::normalize(float3(0.3f, 1.f, -0.2f))};
    for (const float3& p : setPoints) {
        for (const float3& nn : setNormals) {
            constexpr u32 kM = 1u << 18;
            std::vector<u32> hits(count, 0u);
            double sum = 0.0;
            double sum2 = 0.0;
            u32 pdfBad = 0;
            for (u32 k = 0; k < kM; ++k) {
                const rl::LightSetSample s = set.sample(p, nn, rng.uniform(), rng.uniform(), rng.uniform());
                double f = 0.0;
                if (s.light < count) {
                    ++hits[s.light];
                    const bool delta = (s.shape.flags & lk::kRlSampleDelta) != 0u;
                    if (!sameBits(s.pmf, set.pmf(p, nn, s.light))) {
                        ++pdfBad;
                    }
                    if (!delta && s.pdf > 0.f && !near(set.pdf(p, nn, s.light, s.shape.wi), s.pdf, 1e-4)) {
                        ++pdfBad;
                    }
                    if (s.pdf > 0.f) {
                        f = double(maxc(s.shape.radiance)) * cosPlus(nn, s.shape.wi) / double(s.pdf);
                    }
                }
                sum += f;
                sum2 += f * f;
            }
            check(pdfBad == 0u, "light-set sample pmf / pdf == set.pmf() / set.pdf()");
            const double mean = sum / kM;
            const double sigma = std::sqrt(std::max(0.0, sum2 / kM - mean * mean) / kM);
            double ref = 0.0;
            u32 freqBad = 0;
            u32 biased = 0;
            for (u32 i = 0; i < count; ++i) {
                const float pmf = set.pmf(p, nn, i);
                const double contribution = bruteForce(set.light(i), p, nn, rng, 256u);
                ref += contribution;
                if (contribution > 0.0 && !(pmf > 0.f)) {
                    ++biased;
                }
                const double freq = double(hits[i]) / kM;
                if (std::fabs(freq - pmf) > 5.0 * std::sqrt(double(pmf) * (1.0 - pmf) / kM) + 1e-5) {
                    ++freqBad;
                }
            }
            check(biased == 0u, "every contributing light has a non-zero selection probability");
            check(freqBad == 0u, "tree selection frequencies == pmf (5 sigma)");
            check(std::fabs(mean - ref) <= 4.0 * sigma + 3e-3 * std::fabs(ref),
                  "light-set estimator == brute force over every light (" + std::to_string(mean) + " vs " +
                      std::to_string(ref) + ", sigma " + std::to_string(sigma) + ")");
            std::printf("  p (%.1f %.1f %.1f): estimator %.6f +- %.6f, brute force %.6f\n", double(p.x), double(p.y),
                        double(p.z), mean, sigma, ref);
        }
    }
}

// ---- UsdLux / Remix lights -------------------------------------------------------------------------------------------

mods::import::LightParams usdLight(const char* type) {
    mods::import::LightParams p;
    p.usdType = type;
    p.values["color"] = {1.f, 0.5f, 0.25f};
    p.values["intensity"] = {4.f, 0.f, 0.f};
    p.values["exposure"] = {1.f, 0.f, 0.f};
    return p;
}

void suiteUsd() {
    const float3 radiance(8.f, 4.f, 2.f); // color x intensity x 2^exposure
    // Row-major D3D layout: scale 2, then translate (1, 2, 3).
    const float m[16] = {2.f, 0.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f, 0.f, 0.f, 2.f, 0.f, 1.f, 2.f, 3.f, 1.f};
    RlLight L;
    {
        mods::import::LightParams p = usdLight("SphereLight");
        p.values["radius"] = {0.5f, 0.f, 0.f};
        check(rl::lightFromUsd(p, m, L), "SphereLight");
        check(L.kind == lk::kRlKindSphere && eq3(L.position, float3(1.f, 2.f, 3.f)) && L.radius == 1.f &&
                  eq3(L.radiance, radiance) && near(L.area, 4.0 * 3.14159265358979 * 1.0, 1e-6) &&
                  (L.flags & lk::kRlFlagShaped) == 0u,
              "SphereLight: placed, scaled radius, radiance = color x intensity x 2^exposure, unshaped");
    }
    {
        mods::import::LightParams p = usdLight("RectLight");
        p.values["width"] = {2.f, 0.f, 0.f};
        p.values["height"] = {1.f, 0.f, 0.f};
        p.values["shaping:cone:angle"] = {60.f, 0.f, 0.f};
        p.values["shaping:cone:softness"] = {0.1f, 0.f, 0.f};
        p.values["shaping:focus"] = {2.f, 0.f, 0.f};
        check(rl::lightFromUsd(p, nullptr, L), "RectLight");
        check(L.kind == lk::kRlKindRect && eq3(L.u, float3(1.f, 0.f, 0.f)) && eq3(L.v, float3(0.f, -0.5f, 0.f)) &&
                  near(L.area, 2.0, 1e-6),
              "RectLight: half axes X x width / 2, -Y x height / 2, area width x height");
        check(lk::dot(lk::rlPlanarNormal(L), float3(0.f, 0.f, -1.f)) > 0.999f, "RectLight emits along -Z");
        check((L.flags & lk::kRlFlagShaped) != 0u && eq3(L.axis, float3(0.f, 0.f, -1.f)) &&
                  near(L.cosCone, std::cos(60.0 * 3.14159265358979 / 180.0), 1e-5) && near(L.softness, 0.1, 1e-6) &&
                  L.focus == 2.f,
              "RectLight shaping: axis -Z, cone 60 degrees, softness, focus");
        // Shaping semantics: zero outside the cone, smooth inside, focus on the axis.
        check(lk::rlShapingAt(L, float3(0.f, 5.f, -0.5f)) == 0.f, "shaping zero outside the cone");
        check(near(lk::rlShapingAt(L, float3(0.f, 0.f, -5.f)), 1.0, 1e-6), "shaping 1 on the axis");
        const float mid = lk::rlShapingAt(L, lk::normalize(float3(0.f, 0.6f, -1.f)) * 5.f);
        check(mid > 0.f && mid < 1.f, "shaping between 0 and 1 inside the cone");
    }
    {
        mods::import::LightParams p = usdLight("DiskLight");
        p.values["radius"] = {0.5f, 0.f, 0.f};
        check(rl::lightFromUsd(p, m, L), "DiskLight");
        check(L.kind == lk::kRlKindDisk && near(L.area, 3.14159265358979 * 1.0, 1e-6) &&
                  eq3(L.position, float3(1.f, 2.f, 3.f)),
              "DiskLight: scaled radius, area pi r^2");
    }
    {
        mods::import::LightParams p = usdLight("CylinderLight");
        p.values["length"] = {3.f, 0.f, 0.f};
        p.values["radius"] = {0.25f, 0.f, 0.f};
        check(rl::lightFromUsd(p, nullptr, L), "CylinderLight");
        check(L.kind == lk::kRlKindCylinder && eq3(L.u, float3(1.5f, 0.f, 0.f)) && L.radius == 0.25f &&
                  near(L.area, 2.0 * 3.14159265358979 * 0.25 * 3.0, 1e-6),
              "CylinderLight: axis X x length / 2, lateral area 2 pi r l");
    }
    {
        mods::import::LightParams p = usdLight("DistantLight");
        p.values["angle"] = {2.f, 0.f, 0.f};
        check(rl::lightFromUsd(p, m, L), "DistantLight");
        check(L.kind == lk::kRlKindDistant && eq3(L.u, float3(0.f, 0.f, -1.f)) &&
                  near(L.radius, 3.14159265358979 / 180.0, 1e-6) && eq3(L.radiance, radiance),
              "DistantLight: travels along -Z, half angle = angle / 2, radiance = irradiance");
    }
    {
        mods::import::LightParams p = usdLight("DomeLight");
        check(!rl::lightFromUsd(p, nullptr, L) && L.kind == lk::kRlKindNone, "DomeLight is not a Relight light");
        mods::import::LightParams t = usdLight("SphereLight");
        t.values["radius"] = {1.f, 0.f, 0.f};
        t.values["enableColorTemperature"] = {1.f, 0.f, 0.f};
        const char* warning = nullptr;
        check(rl::lightFromUsd(t, nullptr, L, &warning) && warning != nullptr, "colour temperature reported");
        mods::import::LightParams v = usdLight("SphereLight");
        v.values["radius"] = {1.f, 0.f, 0.f};
        v.values["volumetric_radiance_scale"] = {0.25f, 0.f, 0.f};
        check(rl::lightFromUsd(v, nullptr, L) && L.volumetricScale == 0.25f, "volumetric_radiance_scale carried");
    }
    // Packing round trip.
    for (const NamedLight& nl : kindLights()) {
        float4 w[lk::kRlLightWords];
        lk::rlLightPack(nl.light, w);
        const RlLight u = lk::rlLightUnpack(w);
        check(std::memcmp(&u, &nl.light, sizeof(RlLight)) == 0, std::string("pack / unpack ") + nl.name);
    }
    std::printf("usd: UsdLux sphere / rect / disk / cylinder / distant mapping, shaping, packing ok\n");
}

// ---- emissive triangles ------------------------------------------------------------------------------------------------

void suiteEmissive() {
    rl::RelightLightSet set;
    set.beginFrame();
    // Interleaved positions (stride 20: xyz + uv), a quad + a degenerate + an out-of-range triangle.
    const float verts[] = {0.f, 0.f, 0.f, 9.f, 9.f, 1.f, 0.f, 0.f, 9.f, 9.f, 1.f, 1.f, 0.f, 9.f, 9.f, 0.f, 1.f, 0.f, 9.f, 9.f};
    const u16 idx16[] = {0, 1, 2, 0, 2, 3, 0, 0, 1, 0, 1, 7};
    const float translate[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 10.f, 0.f, 0.f, 1.f};
    rl::EmissiveMesh mesh;
    mesh.positions = verts;
    mesh.stride = 20;
    mesh.vertexCount = 4;
    mesh.indices = idx16;
    mesh.indexCount = 12;
    mesh.objectToWorld = translate;
    mesh.radiance = float3(2.f, 1.f, 0.5f);
    mesh.key = 1234;
    check(set.addEmissiveTriangles(mesh) == 2u, "16-bit mesh: 2 triangles (degenerate and out-of-range skipped)");
    check(set.stats().emissiveSkipped == 2u, "2 skipped");
    const u32 idx32[] = {0, 1, 2};
    mesh.indices = idx32;
    mesh.index32 = true;
    mesh.indexCount = 3;
    mesh.twoSided = true;
    mesh.objectToWorld = nullptr;
    check(set.addEmissiveTriangles(mesh) == 1u, "32-bit mesh");
    rl::EmissiveMesh flat = mesh;
    flat.indices = nullptr;
    flat.vertexCount = 3;
    flat.twoSided = false;
    check(set.addEmissiveTriangles(flat) == 1u, "non-indexed mesh");
    rl::EmissiveMesh dark = flat;
    dark.radiance = float3(0.f, 0.f, 0.f);
    check(set.addEmissiveTriangles(dark) == 0u, "dark material adds nothing");
    set.setEmissiveTriangleLimit(5);
    mesh.indices = idx16;
    mesh.index32 = false;
    mesh.indexCount = 6;
    check(set.addEmissiveTriangles(mesh) == 1u, "the per-frame cap stops at 5 triangles");
    set.setEmissiveTriangleLimit(0);
    check(set.build(), "build");
    check(set.lightCount() == 5u && set.stats().emissiveTriangles == 5u, "5 emissive lights");
    const RlLight t0 = set.light(0);
    check(t0.kind == lk::kRlKindTriangle && sameBits3(t0.position, float3(10.f, 0.f, 0.f)) &&
              sameBits3(t0.u, float3(1.f, 0.f, 0.f)) && sameBits3(t0.v, float3(1.f, 1.f, 0.f)) &&
              near(t0.area, 0.5, 1e-6) && sameBits3(t0.radiance, float3(2.f, 1.f, 0.5f)) &&
              (t0.flags & lk::kRlFlagTwoSided) == 0u,
          "triangle 0: transformed vertices, area, radiance, one-sided");
    check(set.entries()[0].origin == rl::LightOrigin::Emissive && set.entries()[0].key == 1234u &&
              set.entries()[1].source == 1u,
          "emissive entries: key and triangle index");
    check((set.light(2).flags & lk::kRlFlagTwoSided) != 0u && (set.light(3).flags & lk::kRlFlagTwoSided) == 0u,
          "two-sided flag");
    check(set.tree().emitters().size() == 5u && set.tree().emitters()[0].kind == lt::kLtKindTriangle,
          "tree: triangle emitters");
    std::printf("emissive: indexed 16 / 32-bit, non-indexed, transform, degenerate / out-of-range / dark / cap ok\n");
}

// ---- set assembly --------------------------------------------------------------------------------------------------------

void suiteSet() {
    rl::RelightLightSet set;
    set.reserve(64, 16);
    const std::vector<tap::Light> app = ffLitLights();
    set.beginFrame();
    const RlLight rect = rl::makeRectLight(float3(0.f, 3.f, 0.f), float3(1.f, 0.f, 0.f), float3(0.f, 0.f, -1.f),
                                           float3(1.f, 1.f, 1.f));
    check(set.addLight(rect, 42u), "authored rect");
    check(!set.addLight(lk::rlLightNone(), 1u), "kind none rejected");
    check(!set.addLight(rl::makeSphereLight(float3(0.f), 1.f, float3(0.f)), 1u), "zero radiance rejected");
    check(!set.addLight(rl::makeSphereLight(float3(0.f), 0.f, float3(1.f)), 1u), "zero area rejected");
    check(set.addGameLights(app.data(), static_cast<u32>(app.size())) == 3u, "game lights");
    check(!set.addGameLight(app[1]), "same hash in a frame rejected");
    tap::Light bad = app[1];
    bad.type = 7;
    check(!set.addGameLight(bad), "invalid type rejected");
    tap::Light off = app[1];
    off.position.x = 5.f;
    off.diffuse = {0.f, 0.f, 0.f, 1.f};
    check(!set.addGameLight(off), "black light rejected");
    off.diffuse = {1.f, -1.f, 1.f, 1.f};
    check(!set.addGameLight(off), "subtractive light rejected");
    check(!set.addFallbackLight(rl::FallbackLight{}), "fallback (mode 1) not added when the frame has lights");
    check(set.build(), "build");
    check(set.gameLightCount() == 3u && set.lightCount() == 4u, "3 game + 1 authored");
    check(set.entries()[3].origin == rl::LightOrigin::Authored && set.entries()[3].key == 42u,
          "authored after the game lights");
    for (u32 i = 0; i < set.lightCount(); ++i) {
        check(set.proxies()[i].source == i && set.tree().emitters()[i].source == i, "tree light index == table index");
    }
    check(set.tree().emitters()[0].kind == lt::kLtKindDirectional && set.tree().emitters()[1].kind == lt::kLtKindPoint &&
              set.tree().emitters()[2].kind == lt::kLtKindSpot && set.tree().emitters()[3].kind == lt::kLtKindRect,
          "proxies: distant -> directional, sphere -> point, shaped sphere -> spot, rect -> rect");
    check(set.stats().builds == 1u && set.stats().refits == 0u, "first frame builds");
    check(set.gameInputs().size() == 3u * lk::kRlD3dWords && set.table().size() == 4u * lk::kRlLightWords,
          "packed inputs / table sizes");

    // Next frame, same kinds, moved: refit.
    set.beginFrame();
    std::vector<tap::Light> moved = app;
    moved[1].position.x += 0.5f;
    set.addGameLights(moved.data(), static_cast<u32>(moved.size()));
    set.addLight(rect, 42u);
    check(set.build() && set.stats().refits == 1u && set.stats().builds == 1u, "same kinds: refit");
    check(sameBits(set.light(1).position.x, -1.1f), "refit sees the moved light");
    // Kinds change: rebuild.
    set.beginFrame();
    set.addGameLights(app.data(), 2u);
    set.addLight(rect, 42u);
    set.addLight(rl::makeSphereLight(float3(1.f), 0.5f, float3(1.f)), 43u);
    check(set.build() && set.stats().builds == 2u, "different set: rebuild");

    // rtx.ignoreGame*Lights.
    {
        ScopedConf conf("rtx.ignoreGamePointLights = True\nrtx.ignoreGameDirectionalLights = True\n");
        set.beginFrame();
        check(set.addGameLights(app.data(), static_cast<u32>(app.size())) == 1u && set.stats().gameRejected == 2u,
              "rtx.ignoreGamePointLights / ignoreGameDirectionalLights");
    }
    {
        ScopedConf conf("rtx.ignoreGameSpotLights = True\n");
        set.beginFrame();
        check(set.addGameLights(app.data(), static_cast<u32>(app.size())) == 2u, "rtx.ignoreGameSpotLights");
    }
    // The conversion options reach the kernel.
    {
        ScopedConf conf("rtx.lightConversionSphereLightFixedRadius = 2\nrtx.sceneScale = 0.5\n");
        set.beginFrame();
        set.addGameLights(app.data(), static_cast<u32>(app.size()));
        check(set.build() && set.light(1).radius == 1.f, "sphere radius = fixed radius x scene scale");
    }

    // Fallback light.
    set.beginFrame();
    const rl::FallbackLight f = rl::fallbackLightFromOptions();
    check(f.mode == 1u && sameBits3(f.radiance, float3(1.6f, 1.8f, 2.0f)), "fallback options (defaults)");
    check(set.addFallbackLight(f) && set.stats().fallback, "fallback (mode 1) added to an empty frame");
    check(set.build() && set.lightCount() == 1u && set.light(0).kind == lk::kRlKindDistant &&
              near(set.light(0).radius, 2.5 * 3.14159265358979 / 180.0, 1e-6),
          "fallback distant light (half angle = angle / 2)");
    {
        ScopedConf conf("rtx.fallbackLightMode = 2\nrtx.fallbackLightRadiance = 1, 2, 3\n");
        const rl::FallbackLight g = rl::fallbackLightFromOptions();
        check(g.mode == 2u && sameBits3(g.radiance, float3(1.f, 2.f, 3.f)), "fallback options read by name");
        set.beginFrame();
        set.addLight(rect, 1u);
        check(set.addFallbackLight(g), "fallback (mode 2) always added");
    }
    {
        ScopedConf conf("rtx.fallbackLightMode = 0\n");
        set.beginFrame();
        check(!set.addFallbackLight(rl::fallbackLightFromOptions()), "fallback (mode 0) never added");
        check(set.build() && set.lightCount() == 0u, "an empty set builds");
        const rl::LightSetSample s = set.sample(float3(0.f), float3(0.f, 1.f, 0.f), 0.5f, 0.5f, 0.5f);
        check(s.light == lt::kLtInvalid && s.pdf == 0.f, "an empty set samples nothing");
    }
    std::printf("set: order, tree index == table index, proxies, refit / rebuild, rejection rules, options, fallback ok\n");
}

// ---- CpuReference == CpuParallel -------------------------------------------------------------------------------------------

void suiteParity() {
    Rng rng(5);
    constexpr u32 kLights = 4096;
    std::vector<float4> d3d(kLights * lk::kRlD3dWords);
    for (u32 i = 0; i < kLights; ++i) {
        rl::packD3dLight(randomD3dLight(rng, i), d3d.data() + i * lk::kRlD3dWords);
    }
    const lk::RlConvertParams params = rl::convertParamsFromOptions();
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    auto convert = [&](kernel::Backend backend, std::vector<float4>& out) {
        out.assign(kLights * lk::kRlLightWords, float4(-7.f, -7.f, -7.f, -7.f));
        lk::ConvertParams p{};
        p.d3d = kernel::Span<const float4>{d3d.data(), static_cast<u32>(d3d.size())};
        p.out = kernel::Span<float4>{out.data(), static_cast<u32>(out.size())};
        p.params = params;
        p.count = kLights;
        return kernel::launch(backend, kernel::KernelLaunch{lk::kConvertName, kernel::extent1(kLights), lk::kWorkgroup},
                              lk::ConvertKernel{}, p)
            .ok;
    };
    std::vector<float4> ref;
    std::vector<float4> par;
    scheduler.shutdown();
    check(convert(kernel::Backend::CpuReference, ref), "convert CpuReference");
    scheduler.initialize(3u);
    check(convert(kernel::Backend::CpuParallel, par), "convert CpuParallel");
    check(std::memcmp(ref.data(), par.data(), ref.size() * sizeof(float4)) == 0, "convert: CpuReference == CpuParallel");

    // light_set_sample over a mixed set.
    rl::RelightLightSet set;
    set.beginFrame();
    const std::vector<tap::Light> app = ffLitLights();
    set.addGameLights(app.data(), static_cast<u32>(app.size()));
    for (const NamedLight& nl : kindLights()) {
        set.addLight(nl.light, 3u);
    }
    check(set.build(), "set build");
    constexpr u32 kQueries = 8192;
    std::vector<float4> queries(kQueries * lk::kRlQueryWords);
    for (u32 i = 0; i < kQueries; ++i) {
        float4* q = queries.data() + i * lk::kRlQueryWords;
        q[0] = float4(rng.range(-6.f, 6.f), rng.range(-6.f, 6.f), rng.range(-6.f, 6.f), rng.uniform());
        const float3 n = (i % 3u) == 0u ? float3(0.f) : lk::normalize(float3(rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), 1.f));
        q[1] = float4(n.x, n.y, n.z, rng.uniform());
        const float3 w = lk::normalize(float3(rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)));
        q[2] = float4(w.x, w.y, w.z, rng.uniform());
        q[3] = float4((i % 4u) == 0u ? -1.f : float(rng.next() % (set.lightCount() + 1u)), 0.f, 0.f, 0.f);
    }
    auto sampleAll = [&](kernel::Backend backend, std::vector<float4>& out) {
        out.assign(kQueries * lk::kRlResultWords, float4(-7.f, -7.f, -7.f, -7.f));
        lk::SetSampleParams p{};
        p.tree = set.tree().view();
        p.table = kernel::Span<const float4>{set.table().data(), static_cast<u32>(set.table().size())};
        p.queries = kernel::Span<const float4>{queries.data(), static_cast<u32>(queries.size())};
        p.results = kernel::Span<float4>{out.data(), static_cast<u32>(out.size())};
        p.lightCount = set.lightCount();
        p.count = kQueries;
        return kernel::launch(backend,
                              kernel::KernelLaunch{lk::kSetSampleName, kernel::extent1(kQueries), lk::kWorkgroup},
                              lk::SetSampleKernel{}, p)
            .ok;
    };
    scheduler.shutdown();
    check(sampleAll(kernel::Backend::CpuReference, ref), "sample CpuReference");
    scheduler.initialize(3u);
    check(sampleAll(kernel::Backend::CpuParallel, par), "sample CpuParallel");
    scheduler.shutdown();
    check(std::memcmp(ref.data(), par.data(), ref.size() * sizeof(float4)) == 0, "sample: CpuReference == CpuParallel");
    // The kernel's results == RelightLightSet::sample.
    u32 bad = 0;
    for (u32 i = 0; i < kQueries; ++i) {
        const float4* q = queries.data() + i * lk::kRlQueryWords;
        const float4* r = ref.data() + i * lk::kRlResultWords;
        const rl::LightSetSample s = set.sample(float3(q[0].x, q[0].y, q[0].z), float3(q[1].x, q[1].y, q[1].z), q[0].w,
                                                q[1].w, q[2].w);
        const float light = s.light == lt::kLtInvalid ? -1.f : float(s.light);
        if (!sameBits(r[0].w, light) || !sameBits(r[1].w, s.pdf) || !sameBits(r[2].w, s.pmf)) {
            ++bad;
        }
    }
    check(bad == 0u, "light_set_sample == RelightLightSet::sample");
    std::printf("parity: %u conversions, %u light-set queries: CpuReference == CpuParallel bit for bit\n", kLights,
                kQueries);
}

// ---- zero_alloc ----------------------------------------------------------------------------------------------------------------

void suiteZeroAlloc() {
    rl::RelightLightSet set;
    set.reserve(1024, 256);
    std::vector<tap::Light> game = ffLitLights();
    {
        Rng g(7);
        for (u32 i = 0; i < 200u; ++i) {
            game.push_back(randomD3dLight(g, 8u + i));
        }
    }
    const std::vector<NamedLight> authored = kindLights();
    const float quad[] = {-2.f, 3.f, -2.f, 2.f, 3.f, -2.f, 2.f, 3.f, 2.f, -2.f, 3.f, 2.f};
    const u16 quadIdx[] = {0, 1, 2, 0, 2, 3}; // lit side down (-Y)
    rl::EmissiveMesh mesh;
    mesh.positions = quad;
    mesh.vertexCount = 4;
    mesh.indices = quadIdx;
    mesh.indexCount = 6;
    mesh.radiance = float3(0.5f, 0.4f, 0.3f);
    const rl::FallbackLight fallback = rl::fallbackLightFromOptions();
    Rng rng(6);
    constexpr u32 kWarmup = 8;
    constexpr u32 kFrames = 72;
    unsigned long long total = 0;
    u32 builds = 0;
    u32 refits = 0;
    float sink = 0.f;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const bool measure = frame >= kWarmup;
        for (tap::Light& l : game) {
            l.position.y += 0.01f;
        }
        mesh.radiance.x = 0.5f + 0.001f * float(frame);
        t_allocations = 0;
        t_count = measure;
        set.beginFrame();
        set.addGameLights(game.data(), static_cast<u32>(game.size()));
        // Every 8th frame drops two authored lights: a rebuild with fewer lights (capacity kept).
        const usize authoredCount = (frame % 8u) == 7u ? authored.size() - 2u : authored.size();
        for (usize i = 0; i < authoredCount; ++i) {
            set.addLight(authored[i].light, i);
        }
        for (u32 k = 0; k < 16u; ++k) {
            set.addEmissiveTriangles(mesh);
        }
        set.addFallbackLight(fallback);
        const bool built = set.build();
        for (u32 k = 0; k < 256u; ++k) {
            const rl::LightSetSample s = set.sample(float3(rng.range(-4.f, 4.f), 0.f, rng.range(-4.f, 4.f)),
                                                    float3(0.f, 1.f, 0.f), rng.uniform(), rng.uniform(), rng.uniform());
            sink += s.pdf + set.pdf(float3(0.f), float3(0.f, 1.f, 0.f), s.light, s.shape.wi);
        }
        t_count = false;
        check(built, "frame build");
        if (measure) {
            total += t_allocations;
        }
    }
    builds = set.stats().builds;
    refits = set.stats().refits;
    std::printf("zero_alloc: %u steady-state frames (%u lights, %u builds / %u refits overall, 256 samples each): "
                "%llu operator-new calls (sink %g)\n",
                kFrames - kWarmup, set.lightCount(), builds, refits, total, double(sink));
    check(total == 0u, "steady-state light-set frames make no heap allocation");
    check(builds > 2u && refits > 2u, "the loop exercised both rebuilds and refits");
}

} // namespace

int main(int argc, char** argv) {
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    (void)BorrowedStandIns::fallbackLightModeObject();
    (void)BorrowedStandIns::fallbackLightRadianceObject();
    (void)BorrowedStandIns::fallbackLightDirectionObject();
    (void)scene::LightOptions::ignoreGamePointLightsObject();
    options::OptionManager::applyPendingValues(nullptr, false);

    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"convert", suiteConvert}, {"pdf", suitePdf},     {"sample", suiteSample},
                            {"usd", suiteUsd},         {"emissive", suiteEmissive}, {"set", suiteSet},
                            {"parity", suiteParity},   {"zero_alloc", suiteZeroAlloc}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            s.fn();
            ran = true;
        }
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
