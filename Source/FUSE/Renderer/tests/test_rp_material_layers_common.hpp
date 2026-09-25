#pragma once

// Asset plan W0.7 layered materials: scene / library helpers shared by the CPU gates
// (test_rp_material_layers_cpu.cpp) and the Lavapipe gates (test_rp_material_layers.cpp).

#include <fuse/renderer/material_layers/fusemat.hpp>
#include <fuse/renderer/material_layers/ml_reference.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace ml_test {

using namespace fuse::renderer::material_layers;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

/// The golden material-ball scene: 8 balls, one .fusemat each (tests/material_layers/*.fusemat.json).
inline constexpr const char* kBallFixtures[] = {
    "ball_0_plain",     "ball_1_stochastic", "ball_2_triplanar",   "ball_3_triplanar_stochastic",
    "ball_4_moss",      "ball_5_snow",       "ball_6_wet_detail",  "ball_7_all",
};
inline constexpr u32 kBallCount = 8u;
inline constexpr u32 kGoldenWidth = 256u;
inline constexpr u32 kGoldenHeight = 128u;
inline constexpr const char* kGoldenPng = "material_balls.png";

/// Extra materials appended after the balls (index kBallCount + ...).
inline constexpr u32 kMatPlainTiling = kBallCount + 0u;      ///< stone albedo, UV, plain tiling
inline constexpr u32 kMatStochasticTiling = kBallCount + 1u; ///< the same with stochastic tiling
inline constexpr u32 kMatTriplanar = kBallCount + 2u;        ///< stone, triplanar, uv_scale 1
inline constexpr u32 kMaterialCount = kBallCount + 3u;

inline bool readText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline bool readBytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

inline bool writeBytes(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

inline FuseMat stoneMaterial(const char* name, bool stochastic, bool triplanar) {
    FuseMat m{};
    m.name = name;
    m.category = FuseMatCategory::Stone;
    m.albedo[0] = m.albedo[1] = m.albedo[2] = 1.f;
    m.textures.albedo = "stone_albedo";
    m.uvScale = 1.f;
    m.stochastic = stochastic;
    m.triplanar = triplanar;
    return m;
}

/// Loads the 8 ball fixtures + the extra test materials into `lib` (+ the golden scene's balls).
inline bool buildLibrary(const std::string& fixtureDir, MlLibrary& lib) {
    for (const char* name : kBallFixtures) {
        std::string text;
        const std::string path = fixtureDir + "/" + name + ".fusemat.json";
        if (!readText(path, text)) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return false;
        }
        FuseMat m{};
        FuseMatResult r = parse_fusemat_json(text, m);
        if (!r.ok) {
            std::fprintf(stderr, "%s:\n%s", path.c_str(), r.describe().c_str());
            return false;
        }
        r = lib.addMaterial(m);
        if (!r.ok) {
            std::fprintf(stderr, "%s (resolve):\n%s", path.c_str(), r.describe().c_str());
            return false;
        }
    }
    const FuseMat extra[3] = {stoneMaterial("test/plain", false, false), stoneMaterial("test/stochastic", true, false),
                              stoneMaterial("test/triplanar", false, true)};
    for (const FuseMat& m : extra) {
        const FuseMatResult r = lib.addMaterial(m);
        if (!r.ok) {
            std::fprintf(stderr, "extra material:\n%s", r.describe().c_str());
            return false;
        }
    }
    const MlBallScene scene = ml_ball_scene(kGoldenWidth, kGoldenHeight, kBallCount);
    lib.balls() = scene.balls;
    return lib.materials().size() == kMaterialCount;
}

/// Deterministic random surfaces over every material (and one out-of-range id).
inline std::vector<MlSurface> randomSurfaces(u32 count, u32 seed) {
    std::vector<MlSurface> out(count);
    u32 state = seed;
    auto next = [&state]() {
        state = ml_hash(state + 0x9e3779b9u);
        return ml_unit(state);
    };
    for (u32 i = 0; i < count; ++i) {
        MlSurface& s = out[i];
        for (f32& p : s.position) {
            p = (next() * 2.f - 1.f) * 3.f;
        }
        s.viewDistance = next() * 20.f;
        f32 n[3] = {next() * 2.f - 1.f, next() * 2.f - 1.f, next() * 2.f - 1.f};
        const f32 l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (u32 c = 0; c < 3u; ++c) {
            s.normal[c] = l > 1e-3f ? n[c] / l * (0.8f + 0.4f * next()) : (c == 1u ? 1.f : 0.f);
            s.tangent[c] = next() * 2.f - 1.f;
        }
        s.tangentSign = next() < 0.5f ? -1.f : 1.f;
        s.material = i % 97u == 96u ? 0xFFFFFFFFu : i % kMaterialCount;
        s.uv[0] = (next() * 2.f - 1.f) * 2.f;
        s.uv[1] = (next() * 2.f - 1.f) * 2.f;
        for (f32& c : s.color) {
            c = next();
        }
    }
    return out;
}

/// A size x size grid of UV samples over [0, repeats)^2 (plain / stochastic tiling metric).
inline std::vector<MlSurface> tilingGrid(u32 size, u32 repeats, u32 material) {
    std::vector<MlSurface> out(static_cast<usize>(size) * size);
    const f32 step = static_cast<f32>(repeats) / static_cast<f32>(size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            MlSurface& s = out[static_cast<usize>(y) * size + x];
            s.uv[0] = (static_cast<f32>(x) + 0.5f) * step;
            s.uv[1] = (static_cast<f32>(y) + 0.5f) * step;
            s.material = material;
        }
    }
    return out;
}

inline std::vector<f32> luminance(const std::vector<MlResult>& r) {
    std::vector<f32> out(r.size());
    for (usize i = 0; i < r.size(); ++i) {
        out[i] = (r[i].albedo[0] + r[i].albedo[1] + r[i].albedo[2]) * (1.f / 3.f);
    }
    return out;
}

inline void meanStd(const std::vector<f32>& v, f64& mean, f64& stddev) {
    mean = 0.0;
    for (const f32 x : v) {
        mean += x;
    }
    mean /= static_cast<f64>(v.size());
    f64 var = 0.0;
    for (const f32 x : v) {
        var += (x - mean) * (x - mean);
    }
    stddev = std::sqrt(var / static_cast<f64>(v.size()));
}

/// A path across the rounded +X / +Y edge of a box (half extent 1, edge radius 0.25) at z = 0.3: the +X face, the
/// quarter-cylinder bevel, the +Y face. `uvBaseline`: per-face planar UVs (X face (z, y), Y face (x, z), switching at
/// the middle of the bevel), the unwrap a UV-mapped cube would have; the triplanar material ignores the UVs.
struct EdgePath {
    std::vector<MlSurface> surfaces;
    u32 bevelBegin = 0;
    u32 bevelEnd = 0;
};

inline EdgePath edgePath(u32 material, f32 step) {
    EdgePath e{};
    constexpr f32 r = 0.25f;
    constexpr f32 c = 1.f - r;
    constexpr f32 z = 0.3f;
    auto push = [&](f32 px, f32 py, f32 nx, f32 ny) {
        MlSurface s{};
        s.position[0] = px;
        s.position[1] = py;
        s.position[2] = z;
        s.normal[0] = nx;
        s.normal[1] = ny;
        s.normal[2] = 0.f;
        s.tangent[0] = 0.f;
        s.tangent[1] = 0.f;
        s.tangent[2] = 1.f;
        s.material = material;
        const bool xFace = nx >= ny;
        s.uv[0] = xFace ? z : px;
        s.uv[1] = xFace ? py : z;
        e.surfaces.push_back(s);
    };
    for (f32 y = 0.f; y < c; y += step) {
        push(1.f, y, 1.f, 0.f);
    }
    e.bevelBegin = static_cast<u32>(e.surfaces.size());
    const f32 arc = 1.5707963f * r;
    const u32 n = static_cast<u32>(arc / step);
    for (u32 i = 0; i <= n; ++i) {
        const f32 a = 1.5707963f * static_cast<f32>(i) / static_cast<f32>(n);
        push(c + r * std::cos(a), c + r * std::sin(a), std::cos(a), std::sin(a));
    }
    e.bevelEnd = static_cast<u32>(e.surfaces.size());
    for (f32 x = c - step; x > 0.f; x -= step) {
        push(x, 1.f, 0.f, 1.f);
    }
    return e;
}

/// Largest albedo step between consecutive samples in [begin, end) and outside it.
inline void edgeSteps(const std::vector<MlResult>& r, u32 begin, u32 end, f64& inside, f64& outside) {
    inside = outside = 0.0;
    for (usize i = 1; i < r.size(); ++i) {
        f64 d = 0.0;
        for (u32 c = 0; c < 3u; ++c) {
            d = std::fmax(d, std::fabs(static_cast<f64>(r[i].albedo[c]) - r[i - 1u].albedo[c]));
        }
        const bool in = i >= begin && i <= end;
        (in ? inside : outside) = std::fmax(in ? inside : outside, d);
    }
}

/// Image comparison on sRGB8 (RGB): pixels over `lsb`, the mean absolute difference, the worst difference.
struct ImageDiff {
    u32 over = 0;
    f64 mean = 0.0;
    u32 worst = 0;
};

inline ImageDiff compareRgba(const std::vector<u8>& a, const std::vector<u8>& b, u32 lsb) {
    ImageDiff d{};
    if (a.size() != b.size() || a.empty()) {
        d.over = 0xFFFFFFFFu;
        d.mean = 1e9;
        return d;
    }
    f64 sum = 0.0;
    for (usize p = 0; p < a.size() / 4u; ++p) {
        u32 worst = 0;
        for (u32 c = 0; c < 3u; ++c) {
            const u32 diff = static_cast<u32>(std::abs(static_cast<int>(a[p * 4u + c]) - static_cast<int>(b[p * 4u + c])));
            worst = diff > worst ? diff : worst;
            sum += diff;
        }
        d.worst = worst > d.worst ? worst : d.worst;
        d.over += worst > lsb ? 1u : 0u;
    }
    d.mean = sum / static_cast<f64>(a.size() / 4u * 3u);
    return d;
}

} // namespace ml_test
