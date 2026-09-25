// WP-8.3 volumetric clouds: CPU gates (no device; also run in the stub tree). Lavapipe gates: test_rp_clouds.cpp.
//
//   layout     CloudParams / CloudPush sizes; the GLSL and Slang mirrors of CloudParams and of the push block
//              (names, order, offsets); buffer sections 256-aligned and disjoint
//   noise      Worley / Perlin tileable (exact equality one period apart), Perlin 0 on the lattice, Worley 1 at
//              its feature points; baked volumes in [0, 1], not constant, and seamless across the wrap (the wrap
//              neighbour step no larger than the interior steps)
//   density    zero outside the layer and at zero coverage, monotone in the coverage bias, homogeneous mode;
//              altitude / sphere / segment geometry vs double precision
//   analytic   homogeneous shell: vertical ray (sun at the zenith) transmittance and single scattering vs the
//              closed forms; oblique rays vs a double-precision brute-force integral; HG normalised; octaves
//              add energy, powder never does; a noise ray converges with the step count
//   temporal   Bayer order covers every block pixel once per block^2 frames; static camera without jitter:
//              the amortised image == the full-resolution march after block^2 frames (exact); a translated
//              camera fetches the history at the reprojected uv; a moving pixel is clamped to its fresh
//              neighbourhood; history leaving the screen is rejected
//   composite  no atmosphere: background x T + L; scene depth in front keeps the background; with the WP-8.2
//              CPU LUTs and no clouds the composite is exactly the sky; aerial perspective dims the clouds
//   api        settings validation, record resolve (flags, Bayer offset, previous camera), stub behaviour
#include <fuse/renderer/clouds/cloud_reference.hpp>
#include <fuse/renderer/clouds/volumetric_clouds.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::clouds;
namespace at = fuse::renderer::atmosphere;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec3;

constexpr f64 kPiD = 3.14159265358979323846;
constexpr f64 kDeg = kPiD / 180.0;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLe(f64 value, f64 bound, const char* message) {
    if (!(value <= bound)) {
        std::fprintf(stderr, "FAIL: %s (got %.6g, bound %.6g)\n", message, value, bound);
        ++g_failures;
    }
}

Vec3 dirElAz(f64 el, f64 az) {
    return Vec3{static_cast<f32>(std::cos(el) * std::sin(az)), static_cast<f32>(std::sin(el)),
                static_cast<f32>(std::cos(el) * std::cos(az))};
}

/// Small settings for the gates.
CloudSettings smallSettings() {
    CloudSettings s{};
    s.noise.shapeSize = 16;
    s.noise.detailSize = 8;
    s.noise.weatherSize = 16;
    s.noise.shapeFrequency = 2;
    s.noise.detailFrequency = 2;
    s.noise.weatherFrequency = 2;
    s.resolution = CloudResolution{16, 8, 4, 16, 8};
    s.sampling.primarySteps = 24;
    s.sampling.lightSteps = 4;
    s.medium.coverageBias = 0.15f;
    return s;
}

CloudFrame defaultFrame() {
    CloudFrame f{};
    f.camera.position = Vec3{0.f, 50.f, 0.f};
    f.camera.forward = dirElAz(20.0 * kDeg, 0.0);
    f.camera.right = Vec3{1.f, 0.f, 0.f};
    f.camera.up = Vec3{0.f, std::cos(static_cast<f32>(20.0 * kDeg)), -std::sin(static_cast<f32>(20.0 * kDeg))};
    f.sunDirection = dirElAz(35.0 * kDeg, 40.0 * kDeg);
    f.sunIlluminance = Vec3{1.f, 0.95f, 0.9f};
    f.ambient = Vec3{0.1f, 0.12f, 0.15f};
    return f;
}

CloudParams resolved(const CloudSettings& s, const CloudFrame& f, const CloudHistoryState& h = {}) {
    CloudParams p{};
    expect(resolve_cloud_params(s, f, h, p), "resolve_cloud_params");
    return p;
}

struct Noise {
    std::vector<ClTexel> shape, detail, weather;
    void build(const CloudParams& p) {
        build_shape_noise(p, shape);
        build_detail_noise(p, detail);
        build_weather(p, weather);
    }
    ClNoiseView view() const { return ClNoiseView{shape.data(), detail.data(), weather.data()}; }
};

// --- layout ----------------------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define CL_FIELD(n) Field{#n, offsetof(CloudParams, n)}
const Field kFields[] = {
    CL_FIELD(shapeNoise), CL_FIELD(detailNoise), CL_FIELD(weather), CL_FIELD(fresh), CL_FIELD(historyIn),
    CL_FIELD(historyOut), CL_FIELD(result), CL_FIELD(background), CL_FIELD(depth), CL_FIELD(atmosphere),
    CL_FIELD(shapeSize), CL_FIELD(detailSize), CL_FIELD(weatherSize), CL_FIELD(shapeFrequency),
    CL_FIELD(detailFrequency), CL_FIELD(weatherFrequency), CL_FIELD(seed), CL_FIELD(flags), CL_FIELD(width),
    CL_FIELD(height), CL_FIELD(freshWidth), CL_FIELD(freshHeight), CL_FIELD(block), CL_FIELD(offsetX),
    CL_FIELD(offsetY), CL_FIELD(frameIndex), CL_FIELD(outWidth), CL_FIELD(outHeight), CL_FIELD(primarySteps),
    CL_FIELD(lightSteps), CL_FIELD(octaves), CL_FIELD(cycle), CL_FIELD(reserved0), CL_FIELD(reserved1),
    CL_FIELD(planetRadius), CL_FIELD(cloudBottom), CL_FIELD(cloudTop), CL_FIELD(maxDistance), CL_FIELD(shapeScale),
    CL_FIELD(detailScale), CL_FIELD(weatherScale), CL_FIELD(coverageScale), CL_FIELD(coverageBias),
    CL_FIELD(densityScale), CL_FIELD(detailStrength), CL_FIELD(albedo), CL_FIELD(phaseForward),
    CL_FIELD(phaseBackward), CL_FIELD(phaseBlend), CL_FIELD(msAttenuation), CL_FIELD(msExtinction),
    CL_FIELD(msEccentricity), CL_FIELD(powderStrength), CL_FIELD(lightDistance), CL_FIELD(ambientScale),
    CL_FIELD(ambientBottom), CL_FIELD(transmittanceCutoff), CL_FIELD(staticThreshold), CL_FIELD(maxHistoryCount),
    CL_FIELD(motionHistoryCount), CL_FIELD(depthRejection), CL_FIELD(reserved3), CL_FIELD(sunDir),
    CL_FIELD(sunIlluminance), CL_FIELD(ambient), CL_FIELD(windOffset), CL_FIELD(windDelta), CL_FIELD(cameraPos),
    CL_FIELD(camForward), CL_FIELD(camRight), CL_FIELD(camUp), CL_FIELD(prevCameraPos), CL_FIELD(prevForward),
    CL_FIELD(prevRight), CL_FIELD(prevUp),
};
#undef CL_FIELD

bool parseShaderStruct(const std::string& path, const std::string& open, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const size_t begin = text.find(open);
    const size_t end = text.find("}", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + open.size(), end - begin - open.size()));
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
    size = (offset + 7u) / 8u * 8u;
    return true;
}

void testLayout() {
    expect(sizeof(CloudParams) == 496u, "CloudParams is 496 bytes");
    expect(sizeof(CloudPush) == 32u, "CloudPush is 32 bytes");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_CLOUDS_SHADER_DIR) + "/cl_common." + lang;
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, "struct CloudParams {", offsets, size, names);
        expect(parsed, "cl_common struct CloudParams parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == sizeof(CloudParams);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(),
                             offsets[i], kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: cl_common.%s CloudParams %zu fields, %zu bytes\n", lang, offsets.size(), size);
        expect(same, "shader CloudParams == C++ CloudParams (names, order, offsets, size)");
        std::vector<size_t> pOffsets;
        std::vector<std::string> pNames;
        size_t pSize = 0;
        const bool pushParsed = parseShaderStruct(
            path, std::string(lang) == "slang" ? "struct CloudPush {" : "uniform CloudPushBlock {", pOffsets, pSize, pNames);
        const std::vector<std::string> pExpected = {"params", "src", "dst", "mode", "reserved"};
        const std::vector<size_t> pExpectedOffsets = {offsetof(CloudPush, params), offsetof(CloudPush, src),
                                                      offsetof(CloudPush, dst), offsetof(CloudPush, mode),
                                                      offsetof(CloudPush, reserved)};
        expect(pushParsed && pNames == pExpected && pOffsets == pExpectedOffsets && pSize == sizeof(CloudPush),
               "shader push block == CloudPush");
    }
    for (const CloudSettings& s : {CloudSettings{}, smallSettings()}) {
        const CloudParams p = resolved(s, defaultFrame());
        const CloudBufferLayout l = CloudBufferLayout::compute(p);
        bool ok = l.noiseBytes > 0u && l.frameBytes > 0u;
        const u64 texels[kCloudSectionCount] = {
            u64{p.shapeSize} * p.shapeSize * p.shapeSize, u64{p.detailSize} * p.detailSize * p.detailSize,
            u64{p.weatherSize} * p.weatherSize,           u64{p.freshWidth} * p.freshHeight * 2u,
            u64{p.width} * p.height * 2u,                 u64{p.width} * p.height * 2u,
            u64{p.outWidth} * p.outHeight};
        for (u32 i = 0; i < kCloudSectionCount; ++i) {
            const bool noise = i < static_cast<u32>(CloudSection::Fresh);
            ok = ok && l.offset[i] % 256u == 0u && l.bytes[i] == texels[i] * 16u &&
                 l.offset[i] + l.bytes[i] <= (noise ? l.noiseBytes : l.frameBytes);
            for (u32 j = i + 1u; j < kCloudSectionCount; ++j) {
                if ((j < static_cast<u32>(CloudSection::Fresh)) == noise) {
                    ok = ok && (l.offset[i] + l.bytes[i] <= l.offset[j] || l.offset[j] + l.bytes[j] <= l.offset[i]);
                }
            }
        }
        expect(ok, "buffer sections 256-aligned, sized and disjoint");
    }
}

// --- noise ----------------------------------------------------------------------------------------------------
void testNoise() {
    u32 tileMismatch = 0;
    f32 latticeMax = 0.f;
    u32 rng = 7u;
    auto next = [&rng]() {
        rng = cl_hash(rng);
        return cl_unit(rng);
    };
    for (u32 i = 0; i < 400u; ++i) {
        const u32 period = 1u << (1u + i % 4u);
        const f32 P = static_cast<f32>(period);
        // Dyadic coordinates: q + P is exact in f32, so the two evaluations see the same cell fraction.
        auto dyadic = [&]() { return std::floor(next() * P * 1024.f) / 1024.f; };
        const Vec3 q{dyadic(), dyadic(), dyadic()};
        const u32 seed = i * 13u;
        if (cl_worley(q, period, seed) != cl_worley(Vec3{q.x + P, q.y, q.z + P}, period, seed) ||
            cl_perlin(q, period, seed) != cl_perlin(Vec3{q.x, q.y + P, q.z + P}, period, seed)) {
            ++tileMismatch;
        }
        const Vec3 lattice{std::floor(q.x), std::floor(q.y), std::floor(q.z)};
        latticeMax = std::max(latticeMax, std::fabs(cl_perlin(lattice, period, seed)));
    }
    std::printf("noise: %u of 400 samples differ one period apart; |perlin| on the lattice <= %g\n", tileMismatch,
                latticeMax);
    expect(tileMismatch == 0u, "Worley / Perlin tile exactly with their period");
    expect(latticeMax == 0.f, "Perlin is 0 on the lattice");
    // Worley at a feature point: the cell (1, 2, 3) of period 4, seed 5.
    {
        const u32 h = cl_hash4(1u, 2u, 3u, 5u);
        const Vec3 feature{1.f + cl_unit(h), 2.f + cl_unit(cl_hash(h)), 3.f + cl_unit(cl_hash(h ^ 0x9e3779b9u))};
        expectLe(1.0 - cl_worley(feature, 4u, 5u), 1e-6, "Worley == 1 at a feature point");
    }
    const CloudSettings s = smallSettings();
    const CloudParams p = resolved(s, defaultFrame());
    Noise n;
    n.build(p);
    struct Vol {
        const char* name;
        const std::vector<ClTexel>* data;
        u32 size;
        u32 dims;
        u32 channels;
    };
    const Vol vols[] = {{"shape", &n.shape, p.shapeSize, 3u, 4u},
                        {"detail", &n.detail, p.detailSize, 3u, 3u},
                        {"weather", &n.weather, p.weatherSize, 2u, 3u}};
    for (const Vol& v : vols) {
        for (u32 c = 0; c < v.channels; ++c) {
            f64 lo = 1e9, hi = -1e9, sum = 0.0, sum2 = 0.0;
            f64 interior = 0.0, wrap = 0.0;
            const u32 N = v.size;
            auto at = [&](u32 x, u32 y, u32 z) {
                const ClTexel& t = (*v.data)[(static_cast<usize>(z) * N + y) * N + x];
                const f32 ch[4] = {t.r, t.g, t.b, t.a};
                return static_cast<f64>(ch[c]);
            };
            const u32 depth = v.dims == 3u ? N : 1u;
            for (u32 z = 0; z < depth; ++z) {
                for (u32 y = 0; y < N; ++y) {
                    for (u32 x = 0; x < N; ++x) {
                        const f64 value = at(x, y, z);
                        lo = std::min(lo, value);
                        hi = std::max(hi, value);
                        sum += value;
                        sum2 += value * value;
                        const f64 dx = std::fabs(at((x + 1u) % N, y, z) - value);
                        const f64 dy = std::fabs(at(x, (y + 1u) % N, z) - value);
                        const f64 step = std::max(dx, dy);
                        if (x + 1u == N || y + 1u == N) {
                            wrap = std::max(wrap, step);
                        } else {
                            interior = std::max(interior, step);
                        }
                    }
                }
            }
            const f64 count = static_cast<f64>(N) * N * depth;
            const f64 mean = sum / count;
            const f64 sd = std::sqrt(std::max(0.0, sum2 / count - mean * mean));
            std::printf("noise: %-7s ch%u range [%.3f, %.3f] mean %.3f sd %.3f; max step interior %.3f wrap %.3f\n",
                        v.name, c, lo, hi, mean, sd, interior, wrap);
            expect(lo >= 0.0 && hi <= 1.0, "noise channel within [0, 1]");
            expect(sd > 0.02, "noise channel not constant");
            expect(wrap <= interior * 1.25 + 1e-6, "noise seamless across the wrap");
        }
    }
    // The wrapped trilinear sampler is continuous across the tile edge.
    f64 edge = 0.0;
    for (u32 i = 0; i < 64u; ++i) {
        const f32 y = next();
        const f32 z = next();
        const ClTexel a = cl_sample3(n.shape.data(), p.shapeSize, 1.f - 1e-6f, y, z);
        const ClTexel b = cl_sample3(n.shape.data(), p.shapeSize, 0.f, y, z);
        edge = std::max(edge, static_cast<f64>(std::fabs(a.r - b.r)));
    }
    expectLe(edge, 1e-4, "wrapped trilinear sampling continuous at the tile edge");
}

// --- density --------------------------------------------------------------------------------------------------
void testDensity() {
    CloudSettings s = smallSettings();
    const CloudFrame f = defaultFrame();
    const CloudParams p = resolved(s, f);
    Noise n;
    n.build(p);
    const ClNoiseView nv = n.view();
    u32 outside = 0, inside = 0, positive = 0;
    u32 rng = 11u;
    auto next = [&rng]() {
        rng = cl_hash(rng);
        return cl_unit(rng);
    };
    for (u32 i = 0; i < 2000u; ++i) {
        const Vec3 pos{(next() - 0.5f) * 40000.f, next() * 5000.f, (next() - 0.5f) * 40000.f};
        const f32 d = cl_density_at(p, nv, pos);
        f32 r = 0.f;
        Vec3 up{};
        const f32 alt = cl_altitude(p, pos, r, up);
        if (alt < p.cloudBottom || alt > p.cloudTop) {
            outside += d != 0.f ? 1u : 0u;
        } else {
            ++inside;
            positive += d > 0.f ? 1u : 0u;
            expect(d >= 0.f && d <= p.densityScale, "density within [0, densityScale]");
        }
    }
    std::printf("density: %u in-layer samples, %u with cloud; %u non-zero outside the layer\n", inside, positive, outside);
    expect(outside == 0u, "zero density outside the layer");
    expect(positive > inside / 20u && positive < inside, "partial coverage in the layer");
    // Coverage: none at bias -1, monotone in the bias.
    u32 nonMonotone = 0;
    u32 zeroCoverage = 0;
    for (u32 i = 0; i < 300u; ++i) {
        const Vec3 pos{(next() - 0.5f) * 40000.f, 1500.f + next() * 2500.f, (next() - 0.5f) * 40000.f};
        f32 prev = -1.f;
        for (f32 bias : {-1.f, -0.2f, 0.f, 0.2f, 0.5f}) {
            CloudSettings b = s;
            b.medium.coverageBias = bias;
            const CloudParams pb = resolved(b, f);
            const f32 d = cl_density_at(pb, nv, pos);
            if (bias == -1.f && d != 0.f) {
                ++zeroCoverage;
            }
            if (d + 1e-7f < prev) {
                ++nonMonotone;
            }
            prev = d;
        }
    }
    expect(zeroCoverage == 0u, "zero coverage -> zero density");
    expect(nonMonotone == 0u, "density non-decreasing in the coverage bias");
    CloudSettings h = s;
    h.medium.homogeneous = true;
    const CloudParams ph = resolved(h, f);
    expect(cl_density_at(ph, nv, Vec3{100.f, 2000.f, -300.f}) == ph.densityScale &&
               cl_density_at(ph, nv, Vec3{100.f, 1000.f, -300.f}) == 0.f,
           "homogeneous mode: densityScale inside the layer only");
    // Height gradient in [0, 1], 0 at the bottom and above each type's top.
    bool gradientOk = true;
    for (f32 type : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        for (u32 i = 0; i <= 100u; ++i) {
            const f32 g = cl_height_gradient(static_cast<f32>(i) / 100.f, type);
            gradientOk = gradientOk && g >= 0.f && g <= 1.f;
        }
        gradientOk = gradientOk && cl_height_gradient(0.f, type) == 0.f;
    }
    gradientOk = gradientOk && cl_height_gradient(0.5f, 0.f) == 0.f && cl_height_gradient(0.5f, 1.f) > 0.9f;
    expect(gradientOk, "height gradient in [0, 1]; stratus thin, cumulus tall");
    // Geometry vs double.
    f64 altErr = 0.0, sphereErr = 0.0;
    const f64 R = p.planetRadius;
    for (u32 i = 0; i < 500u; ++i) {
        const Vec3 pos{(next() - 0.5f) * 60000.f, next() * 6000.f - 50.f, (next() - 0.5f) * 60000.f};
        f32 r = 0.f;
        Vec3 up{};
        const f32 alt = cl_altitude(p, pos, r, up);
        const f64 yc = static_cast<f64>(pos.y) + R;
        const f64 rd = std::sqrt(static_cast<f64>(pos.x) * pos.x + yc * yc + static_cast<f64>(pos.z) * pos.z);
        altErr = std::max(altErr, std::fabs(static_cast<f64>(alt) - (rd - R)));
        const Vec3 d = dirElAz((next() * 180.0 - 90.0) * kDeg, next() * 360.0 * kDeg);
        const f32 mu = d.x * up.x + d.y * up.y + d.z * up.z;
        f32 t0 = 0.f, t1 = 0.f;
        const f64 H = 4000.0;
        if (cl_sphere(p, alt, r, mu, static_cast<f32>(H), t0, t1)) {
            for (f32 t : {t0, t1}) {
                const f64 x = pos.x + static_cast<f64>(d.x) * t;
                const f64 y = pos.y + static_cast<f64>(d.y) * t + R;
                const f64 z = pos.z + static_cast<f64>(d.z) * t;
                // f32 roots: relative to the root distance (far roots cross the planet, ~1.3e7 m).
                const f64 scale = std::max(1000.0, std::fabs(static_cast<f64>(t)));
                sphereErr = std::max(sphereErr, std::fabs(std::sqrt(x * x + y * y + z * z) - (R + H)) / scale);
            }
        }
    }
    std::printf("density: altitude error %.3g m, sphere-root radius error %.3g x distance (f32 world coordinates)\n", altErr,
                sphereErr);
    expectLe(altErr, 0.05, "cl_altitude vs double");
    expectLe(sphereErr, 2e-6, "cl_sphere roots on the sphere");
}

// --- analytic -------------------------------------------------------------------------------------------------
/// Double-precision brute force for a homogeneous shell: in-scattered radiance (single scattering, constant sun
/// illuminance E, sun transfer = phase x exp(-light depth to the top)) and transmittance of the first segment.
struct Brute {
    f64 L = 0.0;
    f64 T = 1.0;
};

f64 sphereFar(f64 ox, f64 oy, f64 oz, f64 dx, f64 dy, f64 dz, f64 R) {
    const f64 b = ox * dx + oy * dy + oz * dz;
    const f64 c = ox * ox + oy * oy + oz * oz - R * R;
    const f64 disc = b * b - c;
    if (disc < 0.0) {
        return -1.0;
    }
    return -b + std::sqrt(disc);
}

Brute bruteHomogeneous(const CloudParams& p, const Vec3& origin, const Vec3& dir, f64 E) {
    const f64 R = p.planetRadius;
    const f64 ox = origin.x, oy = static_cast<f64>(origin.y) + R, oz = origin.z;
    const f64 dl = std::sqrt(static_cast<f64>(dir.x) * dir.x + static_cast<f64>(dir.y) * dir.y + static_cast<f64>(dir.z) * dir.z);
    const f64 dx = dir.x / dl, dy = dir.y / dl, dz = dir.z / dl;
    // Camera below the layer: from the inner sphere's exit to the outer sphere's exit.
    const f64 t0 = sphereFar(ox, oy, oz, dx, dy, dz, R + p.cloudBottom);
    const f64 t1 = std::min(sphereFar(ox, oy, oz, dx, dy, dz, R + p.cloudTop), static_cast<f64>(p.maxDistance));
    Brute b{};
    if (!(t1 > t0)) {
        return b;
    }
    const f64 sigma = p.densityScale;
    const f64 lx = p.sunDir[0], ly = p.sunDir[1], lz = p.sunDir[2];
    const f64 cosT = dx * lx + dy * ly + dz * lz;
    const f64 g = p.phaseForward;
    const f64 phase = (1.0 - g * g) / (4.0 * kPiD * std::pow(1.0 + g * g - 2.0 * g * cosT, 1.5));
    const u32 n = 200000;
    const f64 dt = (t1 - t0) / n;
    f64 L = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const f64 t = t0 + (i + 0.5) * dt;
        const f64 px = ox + dx * t, py = oy + dy * t, pz = oz + dz * t;
        const f64 light = std::min(sphereFar(px, py, pz, lx, ly, lz, R + p.cloudTop), static_cast<f64>(p.lightDistance));
        L += sigma * p.albedo * E * phase * std::exp(-sigma * (t - t0)) * std::exp(-sigma * light) * dt;
    }
    b.L = L;
    b.T = std::exp(-sigma * (t1 - t0));
    return b;
}

CloudSettings analyticSettings(u32 steps) {
    CloudSettings s = smallSettings();
    s.medium.homogeneous = true;
    s.medium.densityScale = 0.0008f;
    s.medium.albedo = 1.f;
    s.lighting.octaves = 1;
    s.lighting.powderStrength = 0.f;
    s.lighting.phaseBlend = 0.f;
    s.sampling.primarySteps = steps;
    s.sampling.lightSteps = 8;
    s.sampling.lightDistance = 1e6f;
    s.sampling.transmittanceCutoff = 0.f;
    s.temporal.jitter = false;
    return s;
}

void testAnalytic() {
    const ClNoiseView none{};
    // Vertical ray from the origin, sun at the zenith: L = E p(1) sigma D exp(-sigma D), T = exp(-sigma D).
    {
        const CloudSettings s = analyticSettings(64);
        CloudFrame f = defaultFrame();
        f.sunDirection = Vec3{0.f, 1.f, 0.f};
        f.ambient = Vec3{0.f, 0.f, 0.f};
        f.sunIlluminance = Vec3{2.f, 2.f, 2.f};
        const CloudParams p = resolved(s, f);
        const ClRay ray = cl_integrate(p, none, ClAtmosphereView{}, Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 1.f, 0.f}, 0.5f);
        const f64 D = p.cloudTop - p.cloudBottom;
        const f64 sigma = p.densityScale;
        const f64 g = p.phaseForward;
        const f64 phase = (1.0 - g * g) / (4.0 * kPiD * std::pow(1.0 - g, 3.0));
        const f64 T = std::exp(-sigma * D);
        const f64 L = 2.0 * phase * sigma * D * T;
        const f64 eT = std::fabs(ray.transmittance - T) / T;
        const f64 eL = std::fabs(ray.scatter.x - L) / L;
        std::printf("analytic: vertical slab (tau %.2f): T %.6g vs %.6g (rel %.2g), L %.6g vs %.6g (rel %.2g)\n",
                    sigma * D, ray.transmittance, T, eT, ray.scatter.x, L, eL);
        expectLe(eT, 1e-4, "homogeneous slab transmittance == exp(-sigma D)");
        expectLe(eL, 2e-3, "homogeneous slab single scattering == closed form");
        // T-weighted mean distance: bottom + 1 / sigma - D T / (1 - T).
        const f64 depth = p.cloudBottom + 1.0 / sigma - D * T / (1.0 - T);
        const f64 depthErr = std::fabs(ray.depth - depth) / D;
        expectLe(depthErr, 0.01, "cloud depth == the transmittance-weighted mean distance");
    }
    // Oblique rays vs the double brute force.
    f64 worst64 = 0.0, worst512 = 0.0, worstT = 0.0;
    for (f64 el : {80.0, 45.0, 25.0, 12.0}) {
        for (f64 sunEl : {70.0, 30.0}) {
            for (u32 steps : {64u, 512u}) {
                const CloudSettings s = analyticSettings(steps);
                CloudFrame f = defaultFrame();
                f.sunDirection = dirElAz(sunEl * kDeg, 120.0 * kDeg);
                f.sunIlluminance = Vec3{1.f, 1.f, 1.f};
                f.ambient = Vec3{0.f, 0.f, 0.f};
                const CloudParams p = resolved(s, f);
                const Vec3 o{30.f, 10.f, -20.f};
                const Vec3 d = dirElAz(el * kDeg, 10.0 * kDeg);
                const ClRay ray = cl_integrate(p, none, ClAtmosphereView{}, o, d, 0.5f);
                const Brute b = bruteHomogeneous(p, o, d, 1.0);
                const f64 e = std::fabs(ray.scatter.x - b.L) / b.L;
                (steps == 64u ? worst64 : worst512) = std::max(steps == 64u ? worst64 : worst512, e);
                worstT = std::max(worstT, std::fabs(ray.transmittance - b.T) / std::max(b.T, 1e-3));
            }
        }
    }
    std::printf("analytic: oblique rays vs double brute force: L rel err %.3g (64 steps), %.3g (512); T %.3g\n", worst64,
                worst512, worstT);
    expectLe(worst64, 0.01, "single scattering vs brute force (64 steps)");
    expectLe(worst512, 0.002, "single scattering vs brute force (512 steps)");
    expectLe(worstT, 1e-3, "transmittance vs brute force");
    // HG normalised; octaves add energy; powder never does.
    {
        const CloudParams p = resolved(analyticSettings(64), defaultFrame());
        for (f32 g : {0.f, 0.5f, 0.8f, -0.3f}) {
            f64 sum = 0.0;
            const u32 n = 20000;
            for (u32 i = 0; i < n; ++i) {
                const f32 c = -1.f + 2.f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
                sum += 2.0 * kPiD * cl_hg(c, g) * (2.0 / n);
            }
            expectLe(std::fabs(sum - 1.0), 2e-3, "Henyey-Greenstein integrates to 1");
        }
        CloudSettings s3 = analyticSettings(64);
        s3.lighting.octaves = 3;
        CloudSettings sp = s3;
        sp.lighting.powderStrength = 1.f;
        const CloudParams p3 = resolved(s3, defaultFrame());
        const CloudParams pp = resolved(sp, defaultFrame());
        bool ok = true;
        for (f32 c : {-1.f, -0.5f, 0.f, 0.7f, 1.f}) {
            for (f32 tau : {0.f, 0.1f, 1.f, 5.f}) {
                ok = ok && cl_sun_transfer(p3, c, tau) >= cl_sun_transfer(p, c, tau) - 1e-7f &&
                     cl_sun_transfer(pp, c, tau) <= cl_sun_transfer(p3, c, tau) + 1e-7f;
            }
        }
        expect(ok, "multiple-scattering octaves add energy; Beer-Powder never adds energy");
    }
    // A noise ray converges with the step count.
    {
        CloudSettings s = smallSettings();
        s.sampling.transmittanceCutoff = 0.f;
        s.temporal.jitter = false;
        const CloudFrame f = defaultFrame();
        Noise n;
        n.build(resolved(s, f));
        f64 e64 = 0.0, e16 = 0.0, norm = 0.0;
        for (u32 k = 0; k < 16u; ++k) {
            const Vec3 d = dirElAz((12.0 + 4.0 * k) * kDeg, (17.0 * k) * kDeg);
            s.sampling.primarySteps = 1024;
            const ClRay ref = cl_integrate(resolved(s, f), n.view(), ClAtmosphereView{}, f.camera.position, d, 0.5f);
            s.sampling.primarySteps = 64;
            const ClRay r64 = cl_integrate(resolved(s, f), n.view(), ClAtmosphereView{}, f.camera.position, d, 0.5f);
            s.sampling.primarySteps = 16;
            const ClRay r16 = cl_integrate(resolved(s, f), n.view(), ClAtmosphereView{}, f.camera.position, d, 0.5f);
            e64 += std::fabs(r64.scatter.x - ref.scatter.x);
            e16 += std::fabs(r16.scatter.x - ref.scatter.x);
            norm += std::fabs(ref.scatter.x);
        }
        std::printf("analytic: noise rays vs 1024 steps: rel L1 %.3g (16 steps), %.3g (64 steps)\n", e16 / norm, e64 / norm);
        expect(norm > 0.0, "noise rays meet clouds");
        expectLe(e64 / norm, 0.05, "noise ray at 64 steps within 5% of 1024 steps");
        expect(e64 <= e16, "error decreases with the step count");
    }
}

// --- temporal --------------------------------------------------------------------------------------------------
void testTemporal() {
    for (u32 block : {2u, 4u}) {
        std::vector<u32> seen(block * block, 0u);
        for (u32 i = 0; i < block * block; ++i) {
            u32 x = 0, y = 0;
            cloud_block_offset(block, i, x, y);
            ++seen[y * block + x];
        }
        expect(std::all_of(seen.begin(), seen.end(), [](u32 v) { return v == 1u; }),
               "Bayer order covers every block pixel once per block^2 frames");
    }
    // Static camera, no jitter: after 16 frames the history equals the full-resolution march.
    CloudSettings s = smallSettings();
    s.temporal.jitter = false;
    const CloudFrame f = defaultFrame();
    Noise n;
    n.build(resolved(s, f));
    const ClNoiseView nv = n.view();
    CloudParams p0 = resolved(s, f);
    const u32 W = p0.width, H = p0.height;
    std::vector<ClTexel> fresh(static_cast<usize>(p0.freshWidth) * p0.freshHeight * 2u);
    std::vector<ClTexel> hist[2] = {std::vector<ClTexel>(static_cast<usize>(W) * H * 2u),
                                    std::vector<ClTexel>(static_cast<usize>(W) * H * 2u)};
    CloudHistoryState state{};
    auto frame = [&](const CloudFrame& fr, const CloudSettings& st) {
        const CloudParams p = resolved(st, fr, state);
        for (u32 fy = 0; fy < p.freshHeight; ++fy) {
            for (u32 fx = 0; fx < p.freshWidth; ++fx) {
                march_texel(p, nv, ClAtmosphereView{}, fx, fy, &fresh[(fy * p.freshWidth + fx) * 2u]);
            }
        }
        const u32 out = state.frameIndex & 1u;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                reconstruct_texel(p, fresh.data(), hist[out ^ 1u].data(), x, y, &hist[out][(y * W + x) * 2u]);
            }
        }
        state.previousCamera = fr.camera;
        state.previousWind = fr.windOffset;
        state.frameIndex += 1u;
        state.historyValid = true;
        return out;
    };
    u32 last = 0;
    for (u32 i = 0; i < 16u; ++i) {
        last = frame(f, s);
    }
    CloudSettings full = s;
    full.resolution.block = 1;
    const CloudParams pf = resolved(full, f);
    u32 mismatch = 0;
    f32 minCount = 1e9f;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            ClTexel ref[2];
            march_texel(pf, nv, ClAtmosphereView{}, x, y, ref);
            const ClTexel& h = hist[last][(y * W + x) * 2u];
            if (h.r != ref[0].r || h.g != ref[0].g || h.b != ref[0].b || h.a != ref[0].a) {
                ++mismatch;
            }
            minCount = std::min(minCount, hist[last][(y * W + x) * 2u + 1u].g);
        }
    }
    std::printf("temporal: static, no jitter, 16 frames of 1/16: %u of %u pixels differ from the full march; min "
                "count %.0f\n",
                mismatch, W * H, minCount);
    expect(mismatch == 0u, "static amortised image == full-resolution march after 16 frames");
    expect(minCount >= 1.f, "every pixel marched once per 16 frames");

    // Reprojection: a translated camera. History = a known function of the previous uv (r = u, g = v): the
    // non-marched pixels must fetch it at the reprojected uv of their block's cloud depth.
    {
        CloudParams p = resolved(s, f);
        CloudFrame moved = f;
        moved.camera.position = f.camera.position + Vec3{300.f, 0.f, 0.f};
        CloudHistoryState st{};
        st.previousCamera = f.camera;
        st.previousWind = f.windOffset;
        st.frameIndex = 5;
        st.historyValid = true;
        p = resolved(s, moved, st);
        std::vector<ClTexel> fr(static_cast<usize>(p.freshWidth) * p.freshHeight * 2u);
        for (u32 i = 0; i < p.freshWidth * p.freshHeight; ++i) {
            fr[i * 2u] = ClTexel{0.f, 0.f, 0.f, 0.f};
            fr[i * 2u + 1u] = ClTexel{8000.f, 1.f, 0.f, 0.f}; // clouds 8 km away
        }
        std::vector<ClTexel> hin(static_cast<usize>(W) * H * 2u);
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                hin[(y * W + x) * 2u] = ClTexel{(x + 0.5f) / W, (y + 0.5f) / H, 0.f, 1.f};
                hin[(y * W + x) * 2u + 1u] = ClTexel{8000.f, 4.f, 1.f, 0.f};
            }
        }
        f64 worst = 0.0;
        u32 checked = 0;
        for (u32 y = 1; y + 1 < H; ++y) {
            for (u32 x = 1; x + 1 < W; ++x) {
                ClTexel out[2];
                // Wide neighbourhood bounds: make the clamp a no-op for this check.
                std::vector<ClTexel> wide = fr;
                for (u32 i = 0; i < p.freshWidth * p.freshHeight; ++i) {
                    wide[i * 2u] = ClTexel{(i % 2u) == 0u ? -10.f : 10.f, (i % 2u) == 0u ? -10.f : 10.f, 0.f, 1.f};
                }
                reconstruct_texel(p, wide.data(), hin.data(), x, y, out);
                const bool isFresh = x % p.block == p.offsetX && y % p.block == p.offsetY;
                if (isFresh) {
                    continue;
                }
                // Independent projection in double.
                const f64 u = (x + 0.5) / W, v = (y + 0.5) / H;
                const Vec3 dir = cl_view_dir(p, static_cast<f32>(u), static_cast<f32>(v));
                const f64 wx = moved.camera.position.x + static_cast<f64>(dir.x) * 8000.0 - f.camera.position.x;
                const f64 wy = moved.camera.position.y + static_cast<f64>(dir.y) * 8000.0 - f.camera.position.y;
                const f64 wz = moved.camera.position.z + static_cast<f64>(dir.z) * 8000.0 - f.camera.position.z;
                const Vec3 fw = f.camera.forward.normalized(), rt = f.camera.right.normalized(), upv = f.camera.up.normalized();
                const f64 z = wx * fw.x + wy * fw.y + wz * fw.z;
                const f64 pu = 0.5 + 0.5 * (wx * rt.x + wy * rt.y + wz * rt.z) / (z * f.camera.tanHalfFovX);
                const f64 pv = 0.5 - 0.5 * (wx * upv.x + wy * upv.y + wz * upv.z) / (z * f.camera.tanHalfFovY);
                const f64 cu = std::clamp(pu, 0.5 / W, 1.0 - 0.5 / W);
                const f64 cv = std::clamp(pv, 0.5 / H, 1.0 - 0.5 / H);
                // Catmull-Rom reproduces a linear ramp exactly away from the clamped border taps.
                const f64 fx = pu * W - 0.5, fy = pv * H - 0.5;
                if (fx < 1.0 || fx > W - 3.0 || fy < 1.0 || fy > H - 3.0) {
                    continue;
                }
                worst = std::max({worst, std::fabs(out[0].r - cu), std::fabs(out[0].g - cv)});
                ++checked;
            }
        }
        std::printf("temporal: translated camera, %u reprojected pixels: history uv error %.3g\n", checked, worst);
        expect(checked > W * H / 8u, "reprojection check covers the image");
        expectLe(worst, 1e-4, "history fetched at the reprojected uv");
        // Rejection by depth: fresh neighbourhood in [0.2, 0.3]; a history at another depth (40 km vs the
        // fresh 8 km) is clamped into it, a history at the same depth is kept.
        std::vector<ClTexel> narrow = fr;
        for (u32 i = 0; i < p.freshWidth * p.freshHeight; ++i) {
            narrow[i * 2u] = ClTexel{0.2f + 0.1f * static_cast<f32>(i % 2u), 0.25f, 0.f, 1.f};
        }
        std::vector<ClTexel> far = hin;
        for (usize i = 0; i < far.size() / 2u; ++i) {
            far[i * 2u + 1u].r = 40000.f;
        }
        bool clamped = true;
        u32 kept = 0;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                ClTexel out[2];
                reconstruct_texel(p, narrow.data(), far.data(), x, y, out);
                clamped = clamped && out[0].r >= 0.2f - 1e-6f && out[0].r <= 0.3f + 1e-6f && out[0].g == 0.25f;
                reconstruct_texel(p, narrow.data(), hin.data(), x, y, out);
                kept += (out[0].g < 0.2f || out[0].g > 0.3f) ? 1u : 0u;
            }
        }
        expect(clamped, "history at another depth clamped to the fresh neighbourhood");
        expect(kept > W * H / 4u, "history at a consistent depth kept (not clamped)");
        // Rejection: a camera turned 180 degrees -> no pixel reprojects on screen -> the fresh block sample.
        CloudFrame turned = moved;
        turned.camera.forward = f.camera.forward * -1.f;
        turned.camera.right = f.camera.right * -1.f;
        const CloudParams pt = resolved(s, turned, st);
        bool rejected = true;
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                ClTexel out[2];
                reconstruct_texel(pt, narrow.data(), hin.data(), x, y, out);
                const ClTexel& fb = narrow[((y / pt.block) * pt.freshWidth + x / pt.block) * 2u];
                rejected = rejected && out[0].r == fb.r && out[0].g == fb.g;
            }
        }
        expect(rejected, "history reprojecting off screen is rejected");
    }
}

// --- composite -------------------------------------------------------------------------------------------------
void testComposite() {
    CloudSettings s = smallSettings();
    CloudFrame f = defaultFrame();
    std::vector<ClTexel> hist(static_cast<usize>(s.resolution.width) * s.resolution.height * 2u);
    for (usize i = 0; i < hist.size() / 2u; ++i) {
        hist[i * 2u] = ClTexel{0.3f, 0.2f, 0.1f, 0.4f};
        hist[i * 2u + 1u] = ClTexel{5000.f, 1.f, 1.f, 0.f};
    }
    std::vector<ClTexel> bg(static_cast<usize>(s.resolution.outWidth) * s.resolution.outHeight, ClTexel{1.f, 2.f, 3.f, 0.f});
    std::vector<f32> depth(bg.size(), 1e9f);
    depth[3] = 100.f;
    f.backgroundAddress = 1; // flags only: the CPU twin reads the vectors
    f.depthAddress = 1;
    const CloudParams p = resolved(s, f);
    const ClTexel a = composite_texel(p, hist.data(), ClAtmosphereView{}, bg.data(), depth.data(), 0, 0);
    const ClTexel b = composite_texel(p, hist.data(), ClAtmosphereView{}, bg.data(), depth.data(), 3, 0);
    expect(std::fabs(a.r - (1.f * 0.4f + 0.3f)) < 1e-6f && std::fabs(a.b - (3.f * 0.4f + 0.1f)) < 1e-6f && a.a == 0.4f,
           "composite = background x T + L");
    expect(b.r == 1.f && b.g == 2.f && b.b == 3.f && b.a == 1.f, "scene depth in front of the clouds keeps the background");
    // With the WP-8.2 CPU LUTs: no clouds -> exactly the sky; clouds -> aerial perspective dims them.
    at::AtmosphereLutSettings as{};
    as.sizes = at::AtmosphereLutSizes{64, 16, 16, 16, 48, 27, 16, 16, 16};
    at::AtmosphereLutView av{};
    av.sunDirection = f.sunDirection;
    av.cameraPosition = f.camera.position;
    av.forward = f.camera.forward;
    av.right = f.camera.right;
    av.up = f.camera.up;
    av.tanHalfFovX = f.camera.tanHalfFovX;
    av.tanHalfFovY = f.camera.tanHalfFovY;
    at::AtParams ap{};
    expect(at::resolve_at_params(as, av, ap), "atmosphere resolve");
    at::AtCpuLuts luts;
    luts.build(ap);
    const at::AtLutView lv = luts.view();
    CloudFrame fa = defaultFrame();
    fa.atmosphere = &ap;
    fa.atmosphereAddress = 1;
    const CloudParams pa = resolved(s, fa);
    const ClAtmosphereView atmView{&ap, &lv};
    std::vector<ClTexel> clear(hist.size());
    for (usize i = 0; i < clear.size() / 2u; ++i) {
        clear[i * 2u] = ClTexel{0.f, 0.f, 0.f, 1.f};
        clear[i * 2u + 1u] = ClTexel{20000.f, 0.f, 0.f, 0.f};
    }
    f64 skyErr = 0.0, dim = 0.0;
    for (u32 y = 0; y < pa.outHeight; ++y) {
        for (u32 x = 0; x < pa.outWidth; ++x) {
            const ClTexel c = composite_texel(pa, clear.data(), atmView, nullptr, nullptr, x, y);
            const f32 u = (x + 0.5f) / pa.outWidth, v = (y + 0.5f) / pa.outHeight;
            const Vec3 sky = at::at_sky_radiance(ap, lv, cl_view_dir(pa, u, v), true);
            skyErr = std::max(skyErr, static_cast<f64>(std::fabs(c.r - sky.x) + std::fabs(c.g - sky.y) + std::fabs(c.b - sky.z)));
        }
    }
    { // An opaque white cloud far away: the aerial perspective dims it and adds its in-scattering.
        std::vector<ClTexel> opaque(hist.size());
        for (usize i = 0; i < opaque.size() / 2u; ++i) {
            opaque[i * 2u] = ClTexel{1.f, 1.f, 1.f, 0.f};
            opaque[i * 2u + 1u] = ClTexel{30000.f, 1.f, 1.f, 0.f};
        }
        const ClTexel c = composite_texel(pa, opaque.data(), atmView, nullptr, nullptr, 7, 4);
        Vec3 sc{}, tr{};
        at::at_aerial(ap, lv, (7 + 0.5f) / pa.outWidth, (4 + 0.5f) / pa.outHeight, 30000.f, sc, tr);
        dim = tr.z;
        expect(std::fabs(c.b - (tr.z + sc.z * ap.sunIlluminance[2])) < 1e-5f, "composite = aerialT x L + aerialS x E (opaque)");
    }
    std::printf("composite: no clouds vs sky max |diff| %.3g; aerial transmittance at 30 km (blue) %.3f\n", skyErr, dim);
    expectLe(skyErr, 1e-6, "no clouds: the composite is the sky");
    expect(dim < 0.95 && dim > 0.0, "aerial perspective dims distant clouds");
}

// --- api ---------------------------------------------------------------------------------------------------------
void testApi() {
    expect(cloud_settings_valid(CloudSettings{}), "default settings valid");
    expect(cloud_settings_valid(smallSettings()), "small settings valid");
    auto bad = [](auto mutate) {
        CloudSettings s = smallSettings();
        mutate(s);
        return !cloud_settings_valid(s);
    };
    expect(bad([](CloudSettings& s) { s.noise.shapeSize = 24; }), "non-power-of-two noise size rejected");
    expect(bad([](CloudSettings& s) { s.noise.detailFrequency = 64; }), "frequency above size rejected");
    expect(bad([](CloudSettings& s) { s.resolution.block = 3; }), "block 3 rejected");
    expect(bad([](CloudSettings& s) { s.resolution.width = 18; }), "width not a multiple of the block rejected");
    expect(bad([](CloudSettings& s) { s.medium.cloudTop = s.medium.cloudBottom; }), "empty layer rejected");
    expect(bad([](CloudSettings& s) { s.sampling.primarySteps = 0; }), "zero steps rejected");
    expect(bad([](CloudSettings& s) { s.lighting.octaves = 9; }), "too many octaves rejected");
    CloudFrame f = defaultFrame();
    CloudParams p{};
    f.camera.forward = Vec3{0.f, 0.f, 0.f};
    expect(!resolve_cloud_params(smallSettings(), f, CloudHistoryState{}, p), "degenerate camera rejected");
    f = defaultFrame();
    CloudHistoryState h{};
    h.frameIndex = 21;
    h.historyValid = true;
    h.previousCamera = f.camera;
    h.previousCamera.position = Vec3{5.f, 6.f, 7.f};
    h.previousWind = Vec3{1.f, 0.f, 0.f};
    f.windOffset = Vec3{4.f, 0.f, 0.f};
    p = resolved(smallSettings(), f, h);
    u32 ox = 0, oy = 0;
    cloud_block_offset(4, 21, ox, oy);
    expect(p.offsetX == ox && p.offsetY == oy && p.cycle == 1u && p.frameIndex == 21u, "Bayer offset and cycle");
    expect((p.flags & kClFlagHistory) != 0u && (p.flags & kClFlagJitter) != 0u && (p.flags & kClFlagAtmosphere) == 0u,
           "flags");
    expect(p.prevCameraPos[0] == 5.f && p.windDelta[0] == 3.f, "previous camera and wind delta");
    expect(p.freshWidth == 4u && p.freshHeight == 2u, "fresh extent = resolution / block");
    CloudSettings noTemporal = smallSettings();
    noTemporal.temporal.enabled = false;
    expect((resolved(noTemporal, f, h).flags & kClFlagHistory) == 0u, "temporal off: no history flag");
    CloudParams q = p;
    expect(cloud_noise_equal(p, q), "noise equality");
    q.seed += 1u;
    expect(!cloud_noise_equal(p, q), "noise seed change detected");
    expect(!queryCloudCapabilities(nullptr).clouds, "no device -> no clouds");
    VolumetricClouds clouds;
    expect(!clouds.init(CloudGpuDesc{}), "init without a device fails");
    expect(!clouds.valid(), "not valid");
}


} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "noise") {
        testNoise();
    }
    if (all || suite == "density") {
        testDensity();
    }
    if (all || suite == "analytic") {
        testAnalytic();
    }
    if (all || suite == "temporal") {
        testTemporal();
    }
    if (all || suite == "composite") {
        testComposite();
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
