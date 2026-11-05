// Renderer plan Phase 9 exit (docs/unification/RENDERER-EXECUTION.md, "Exit: measured against conventional path";
// docs/unification/RENDERER-HW-RUN.md): "Each technique is measured against the conventional path for memory, quality
// and cost. Techniques that do not win are not shipped." Discrete GPU only (77 otherwise).
//
// 1. Neural texture compression (WP-9.1) vs block compression (E06 BCn) and raw 8-bit textures.
//    Content: three procedural 6-channel materials (albedo RGB, tangent-space normal XY, roughness), S x S texels,
//    quantised to 8 bits (the "artist source"). NTC = the in-tree WP-9.1 network (2D multiresolution hash grid ->
//    fp16 MLP -> 6 channels, one network per material), fitted on the CPU with the in-tree NeuralTrainer (Adam, MSE),
//    two table sizes (memory points). Conventional = BC7 albedo + BC5 normal + BC4 roughness (and BC1 albedo) with
//    the in-tree cook encoders when this target can see them (Tools/FUSE/Cook, fuse_cook_stubs), and raw RGBA8 / RG8
//    / R8. Memory = bytes resident on the GPU (NTC: the NeuralGpu blob = header + fp16 parameters). Quality = PSNR per
//    channel group, albedo-luminance SSIM and mean normal angle error vs the 8-bit source, every texel (the NTC
//    decode runs on the GPU, neural.infer). Cost = GPU time of neural.infer decoding one material (6 channels) at
//    every pixel of a 2560 x 1440 frame (inputs: a ~1:1 texel:pixel tiling), portable and cooperative-matrix kernels.
//    BCn sampling cost is not measured (no in-tree kernel that only samples a texture; it is the fixed-function
//    texture unit), see the printed note.
// 2. 3D gaussian splatting (WP-9.2) vs triangles through the visibility buffer (WP-1.4 / 1.5 via FrameComposer).
//    Content: one textured unit sphere (analytic albedo in the uvSphere's (u, v)). Splat representation: N flat
//    surface gaussians (Fibonacci points, tangent-plane discs, SH degree 0 = the albedo at the centre). Mesh
//    representation: mr_test::uvSphere (meshlet mesh) + an S x S albedo texture (RGBA8 or BC7). Ground truth: an
//    analytic ray cast of the sphere (albedo at the exact hit, 1 sample at the pixel centre, black background).
//    Quality = PSNR of each unlit albedo image vs the ground truth at 2560 x 1440: the splat image is the GPU output
//    (premultiplied radiance over black), the mesh image is a CPU rasterisation of the same triangles with the same
//    camera (perspective-correct UVs, bilinear texture), i.e. what the visibility buffer + material resolve sample.
//    Cost: GPU time of the gsplat.* passes (preprocess, scan, emit, sort, ranges, raster) at 2560 x 1440 vs the
//    FrameComposer's geometry passes (instance cull, Hi-Z, visibility buffer, material resolve, motion) for the mesh.
//
// Noise: nvidia-smi utilisation printed before each timing block; warm-up frames discarded; K trials x N frames, the
// value is the median of the trial medians, the spread is min..max of the trial medians.
//
// Gates (robust facts only): everything ran, every number finite, memory accounting consistent (blob / file sizes
// vs parameter counts, BC block counts), NTC PSNR above a sane floor (20 dB albedo), GPU decode == CPU reference
// (portable kernel), splat sort did not overflow, splat / mesh PSNR above 15 dB. Exit 0 / 1 / 77.
//
// Options: --quick (small textures / short fits / fewer splats, smoke), --size S, --steps N, --frames N, --trials K.

// Renderer headers first: the common header pulls <vulkan/vulkan.h> (Win32 platform -> windows.h, whose
// near / far macros break forward_reference.hpp).
#include <fuse/renderer/neural/neural_gpu.hpp>
#include <fuse/renderer/neural/neural_mlp.hpp>
#include <fuse/renderer/gsplat/gsplat.hpp>
#include <fuse/renderer/gsplat/gsplat_reference.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include "test_rp_material_resolve_scene.hpp"
#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/frame/frame_runtime_presets.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material/material.hpp>
#endif

#include "test_rp_hw_bench_common.hpp"

#if defined(FUSE_P9_BC_UNITY)
// The cook encoders compiled into this test (include path Tools/FUSE/Cook/include, no fuse_cook_stubs link).
#include "../../../../Tools/FUSE/Cook/src/bc7_encoder.cpp"
#define BitWriter BcnBitWriter // both files define these in an anonymous namespace
#define BitReader BcnBitReader
#include "../../../../Tools/FUSE/Cook/src/bcn_encoder.cpp"
#undef BitWriter
#undef BitReader
#define P9_HAVE_BC 1
#elif __has_include(<fuse/cook/bcn_encoder.hpp>)
#include <fuse/cook/bcn_encoder.hpp>
#define P9_HAVE_BC 1
#else
#define P9_HAVE_BC 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(FUSE_VULKAN_BACKEND)
int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return 77;
}
#else

namespace {

using namespace hw_bench;
using namespace fuse::renderer;
using namespace fuse::renderer::neural;
using namespace fuse::renderer::gsplat;
using fuse::u16;
using fuse::i32;

constexpr f64 kPi = 3.14159265358979323846;

// --- options ---------------------------------------------------------------------------------------------
struct Options {
    u32 texSize = 1024;
    u32 steps = 3000;
    u32 batch = 4096;
    u32 frames = 40;
    u32 warmup = 10;
    u32 trials = 3;
    u32 width = 2560;
    u32 height = 1440;
    std::vector<u32> splatCounts{100000u, 300000u, 1000000u};
    bool quick = false;
};
Options g_opt;

// --- small utilities ---------------------------------------------------------------------------------------
f64 nowSeconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

std::string gpuUtilization() {
#if defined(_WIN32)
    FILE* p = _popen("nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>NUL", "r");
#else
    FILE* p = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>/dev/null", "r");
#endif
    if (p == nullptr) {
        return "n/a";
    }
    char buf[256] = {};
    std::string out;
    while (std::fgets(buf, sizeof(buf), p) != nullptr) {
        out += buf;
    }
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (out.empty()) {
        return "n/a";
    }
    // "util, memMiB"
    const usize comma = out.find(',');
    if (comma == std::string::npos) {
        return out;
    }
    return out.substr(0, comma) + "% util," + out.substr(comma + 1) + " MiB used";
}

/// Median of trial medians and the spread of the trial medians.
struct Timed {
    f64 value = 0.0;
    f64 lo = 0.0;
    f64 hi = 0.0;
    bool finite = false;
};

Timed combineTrials(const std::vector<f64>& trialMedians) {
    Timed t{};
    if (trialMedians.empty()) {
        return t;
    }
    t.value = quantile(trialMedians, 0.5);
    t.lo = *std::min_element(trialMedians.begin(), trialMedians.end());
    t.hi = *std::max_element(trialMedians.begin(), trialMedians.end());
    t.finite = std::isfinite(t.value) && t.value > 0.0;
    return t;
}

f64 psnrFromMse(f64 mse) { return mse <= 1e-20 ? 200.0 : 10.0 * std::log10(1.0 / mse); }

// --- procedural textures -----------------------------------------------------------------------------------
u32 hash3(u32 x, u32 y, u32 s) {
    u32 h = x * 374761393u + y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
f32 hashf(u32 x, u32 y, u32 s) { return static_cast<f32>(hash3(x, y, s) >> 8) * (1.f / 16777216.f); }

/// Periodic value noise (period `cells` per unit), u, v in [0, 1).
f32 valueNoise(f32 u, f32 v, u32 cells, u32 seed) {
    const f32 x = u * static_cast<f32>(cells);
    const f32 y = v * static_cast<f32>(cells);
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const i32 ix = static_cast<i32>(fx);
    const i32 iy = static_cast<i32>(fy);
    f32 tx = x - fx;
    f32 ty = y - fy;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    auto at = [&](i32 a, i32 b) {
        const u32 c = cells;
        return hashf(static_cast<u32>(((a % static_cast<i32>(c)) + static_cast<i32>(c)) % static_cast<i32>(c)),
                     static_cast<u32>(((b % static_cast<i32>(c)) + static_cast<i32>(c)) % static_cast<i32>(c)), seed);
    };
    const f32 a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
    const f32 b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
    return a + (b - a) * ty;
}

f32 fbm(f32 u, f32 v, u32 baseCells, u32 octaves, u32 seed) {
    f32 sum = 0.f;
    f32 amp = 0.5f;
    f32 norm = 0.f;
    u32 cells = baseCells;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(u, v, cells, seed + o * 101u);
        norm += amp;
        amp *= 0.5f;
        cells *= 2u;
    }
    return sum / norm;
}

f32 q8(f32 v) { return std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f; }

constexpr u32 kChannels = 6u; ///< albedo RGB, normal XY (0..1 encoded), roughness

/// A material: S x S texels x 6 channels in [0, 1], 8-bit quantised.
struct MaterialTex {
    const char* name = "";
    u32 size = 0;
    std::vector<f32> texels; ///< 6 per texel, row-major
};

MaterialTex makeMaterial(u32 kind, u32 S) {
    MaterialTex m;
    m.size = S;
    m.texels.assign(static_cast<usize>(S) * S * kChannels, 0.f);
    std::vector<f32> height(static_cast<usize>(S) * S);
    const f32 inv = 1.f / static_cast<f32>(S);
    for (u32 y = 0; y < S; ++y) {
        for (u32 x = 0; x < S; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) * inv;
            const f32 v = (static_cast<f32>(y) + 0.5f) * inv;
            f32 rgb[3] = {0.5f, 0.5f, 0.5f};
            f32 h = 0.f;
            f32 rough = 0.5f;
            if (kind == 0u) { // bricks: 8 x 16 running bond, sharp mortar lines
                m.name = "bricks";
                const f32 rowsF = 16.f;
                const f32 row = std::floor(v * rowsF);
                const f32 uu = u * 8.f + (std::fmod(row, 2.f) * 0.5f);
                const f32 col = std::floor(uu);
                const f32 bx = uu - col;
                const f32 by = v * rowsF - row;
                const f32 edge = std::min(std::min(bx, 1.f - bx) * 0.5f, std::min(by, 1.f - by)); // brick units
                const bool mortar = edge < 0.06f;
                const u32 id = hash3(static_cast<u32>(std::fmod(col, 8.f)), static_cast<u32>(row), 7u);
                const f32 tint = 0.75f + 0.45f * static_cast<f32>(id >> 8) * (1.f / 16777216.f);
                const f32 grain = fbm(u, v, 32u, 4u, 11u);
                if (mortar) {
                    const f32 g = 0.62f + 0.15f * grain;
                    rgb[0] = g;
                    rgb[1] = g * 0.97f;
                    rgb[2] = g * 0.92f;
                    h = 0.1f * grain;
                    rough = 0.9f;
                } else {
                    rgb[0] = 0.55f * tint * (0.8f + 0.4f * grain);
                    rgb[1] = 0.24f * tint * (0.8f + 0.4f * grain);
                    rgb[2] = 0.16f * tint * (0.8f + 0.4f * grain);
                    h = 0.6f + 0.4f * std::min(1.f, (edge - 0.06f) * 8.f) + 0.08f * grain;
                    rough = 0.55f + 0.3f * fbm(u, v, 64u, 3u, 23u);
                }
            } else if (kind == 1u) { // rock: fbm everywhere, high-frequency detail
                m.name = "rock";
                const f32 n = fbm(u, v, 8u, 7u, 31u);
                const f32 n2 = fbm(u, v, 16u, 5u, 47u);
                const f32 t = std::clamp((n - 0.3f) * 2.f, 0.f, 1.f);
                rgb[0] = 0.25f + 0.45f * t + 0.1f * n2;
                rgb[1] = 0.22f + 0.38f * t + 0.08f * n2;
                rgb[2] = 0.18f + 0.30f * t;
                h = n;
                rough = 0.35f + 0.55f * n2;
            } else { // tiles: 8 x 8 checker, grout, smooth gradient + scratches
                m.name = "tiles";
                const f32 tu = u * 8.f;
                const f32 tv = v * 8.f;
                const f32 cu = std::floor(tu);
                const f32 cv = std::floor(tv);
                const f32 fx = tu - cu;
                const f32 fy = tv - cv;
                const f32 edge = std::min(std::min(fx, 1.f - fx), std::min(fy, 1.f - fy));
                const bool grout = edge < 0.04f;
                const bool dark = (static_cast<u32>(cu) + static_cast<u32>(cv)) % 2u == 0u;
                const f32 scratch = valueNoise(u, v * 0.05f + u, 256u, 59u) > 0.93f ? 0.12f : 0.f;
                if (grout) {
                    rgb[0] = rgb[1] = rgb[2] = 0.3f;
                    h = 0.f;
                    rough = 0.85f;
                } else {
                    const f32 g = 0.08f * (fx + fy);
                    rgb[0] = dark ? 0.12f + g : 0.85f - g;
                    rgb[1] = dark ? 0.2f + g : 0.82f - g;
                    rgb[2] = dark ? 0.35f + g : 0.75f - g;
                    rgb[0] += scratch;
                    rgb[1] += scratch;
                    rgb[2] += scratch;
                    h = 1.f - scratch;
                    rough = 0.15f + 0.1f * fbm(u, v, 16u, 3u, 71u) + scratch * 2.f;
                }
            }
            f32* t = &m.texels[(static_cast<usize>(y) * S + x) * kChannels];
            t[0] = q8(rgb[0]);
            t[1] = q8(rgb[1]);
            t[2] = q8(rgb[2]);
            t[5] = q8(rough);
            height[static_cast<usize>(y) * S + x] = h;
        }
    }
    // Normals from the height field (central differences, wrapped).
    const f32 strength = kind == 1u ? 6.f : 3.f;
    for (u32 y = 0; y < S; ++y) {
        for (u32 x = 0; x < S; ++x) {
            const f32 hx = height[static_cast<usize>(y) * S + (x + 1u) % S] - height[static_cast<usize>(y) * S + (x + S - 1u) % S];
            const f32 hy = height[static_cast<usize>((y + 1u) % S) * S + x] - height[static_cast<usize>((y + S - 1u) % S) * S + x];
            f32 nx = -hx * strength * static_cast<f32>(S) / 256.f;
            f32 ny = -hy * strength * static_cast<f32>(S) / 256.f;
            const f32 len = std::sqrt(nx * nx + ny * ny + 1.f);
            nx /= len;
            ny /= len;
            f32* t = &m.texels[(static_cast<usize>(y) * S + x) * kChannels];
            t[3] = q8(nx * 0.5f + 0.5f);
            t[4] = q8(ny * 0.5f + 0.5f);
        }
    }
    return m;
}

// --- texture quality ---------------------------------------------------------------------------------------
struct TexQuality {
    f64 psnrAlbedo = 0.0;
    f64 psnrNormal = 0.0;
    f64 psnrRough = 0.0;
    f64 psnrAll = 0.0;
    f64 ssimAlbedo = 0.0; ///< luminance SSIM, 8 x 8 windows, stride 4
    f64 normalDeg = 0.0;  ///< mean angle between decoded and source normals
};

f64 luminance(const f32* t) { return 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2]; }

/// `decoded`: 6 floats per texel (clamped to [0, 1] here).
TexQuality measureTexture(const MaterialTex& src, const std::vector<f32>& decoded) {
    TexQuality q{};
    const u32 S = src.size;
    const usize n = static_cast<usize>(S) * S;
    f64 se[kChannels] = {};
    f64 angle = 0.0;
    std::vector<f32> la(n), lb(n);
    for (usize i = 0; i < n; ++i) {
        const f32* a = &src.texels[i * kChannels];
        f32 b[kChannels];
        for (u32 c = 0; c < kChannels; ++c) {
            b[c] = std::clamp(decoded[i * kChannels + c], 0.f, 1.f);
            const f64 d = static_cast<f64>(a[c]) - b[c];
            se[c] += d * d;
        }
        la[i] = static_cast<f32>(luminance(a));
        lb[i] = static_cast<f32>(luminance(b));
        auto unpack = [](f32 x, f32 y, f64 (&nrm)[3]) {
            nrm[0] = x * 2.0 - 1.0;
            nrm[1] = y * 2.0 - 1.0;
            nrm[2] = std::sqrt(std::max(0.0, 1.0 - nrm[0] * nrm[0] - nrm[1] * nrm[1]));
            const f64 l = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]) + 1e-12;
            for (f64& v : nrm) {
                v /= l;
            }
        };
        f64 na[3], nb[3];
        unpack(a[3], a[4], na);
        unpack(b[3], b[4], nb);
        const f64 dot = std::clamp(na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2], -1.0, 1.0);
        angle += std::acos(dot) * 180.0 / kPi;
    }
    const f64 nn = static_cast<f64>(n);
    q.psnrAlbedo = psnrFromMse((se[0] + se[1] + se[2]) / (3.0 * nn));
    q.psnrNormal = psnrFromMse((se[3] + se[4]) / (2.0 * nn));
    q.psnrRough = psnrFromMse(se[5] / nn);
    q.psnrAll = psnrFromMse((se[0] + se[1] + se[2] + se[3] + se[4] + se[5]) / (6.0 * nn));
    q.normalDeg = angle / nn;
    // SSIM (luminance)
    const f64 c1 = 0.01 * 0.01;
    const f64 c2 = 0.03 * 0.03;
    f64 ssim = 0.0;
    u32 windows = 0;
    for (u32 y = 0; y + 8u <= S; y += 4u) {
        for (u32 x = 0; x + 8u <= S; x += 4u) {
            f64 ma = 0, mb = 0, va = 0, vb = 0, cov = 0;
            for (u32 j = 0; j < 8u; ++j) {
                for (u32 i = 0; i < 8u; ++i) {
                    const usize p = static_cast<usize>(y + j) * S + x + i;
                    ma += la[p];
                    mb += lb[p];
                }
            }
            ma /= 64.0;
            mb /= 64.0;
            for (u32 j = 0; j < 8u; ++j) {
                for (u32 i = 0; i < 8u; ++i) {
                    const usize p = static_cast<usize>(y + j) * S + x + i;
                    va += (la[p] - ma) * (la[p] - ma);
                    vb += (lb[p] - mb) * (lb[p] - mb);
                    cov += (la[p] - ma) * (lb[p] - mb);
                }
            }
            va /= 63.0;
            vb /= 63.0;
            cov /= 63.0;
            ssim += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++windows;
        }
    }
    q.ssimAlbedo = windows != 0u ? ssim / windows : 0.0;
    return q;
}

// --- NTC fit -----------------------------------------------------------------------------------------------
struct NtcVariant {
    const char* name;
    u32 log2Table;
};

NeuralNetConfig ntcConfig(u32 S, u32 log2Table) {
    NeuralNetConfig c{};
    c.inputDims = 2;
    c.encoding = NeuralEncoding::HashGrid;
    c.levels = 16;
    c.featuresPerLevel = 2;
    c.log2TableSize = log2Table;
    c.baseResolution = 16;
    // finest level ~ the texture resolution
    c.perLevelScale = static_cast<f32>(std::pow(static_cast<f64>(S) / 16.0, 1.0 / 15.0));
    c.hiddenWidth = 64;
    c.hiddenLayers = 2;
    c.outputDims = kChannels;
    c.hiddenActivation = NeuralActivation::ReLU;
    c.outputActivation = NeuralActivation::None;
    c.precision = NeuralPrecision::F16;
    return c;
}

struct NtcFit {
    NeuralNet net;
    f64 seconds = 0.0;
    f32 finalLoss = 0.f;
    bool ok = false;
};

void fitNtc(const MaterialTex& tex, const NeuralNetConfig& config, u64 seed, NtcFit& out) {
    const f64 t0 = nowSeconds();
    if (!out.net.init(config, seed)) {
        return;
    }
    const u32 S = tex.size;
    const u32 batch = g_opt.batch;
    std::vector<f32> x(static_cast<usize>(batch) * 2u), y(static_cast<usize>(batch) * kChannels);
    u64 state = seed * 0x9E3779B97F4A7C15ull + 1u;
    auto next = [&]() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    };
    // Two phases: lr 1e-2 then 1e-3 for the last quarter (fresh Adam moments).
    const u32 phase1 = g_opt.steps * 3u / 4u;
    f32 loss = 0.f;
    for (u32 phase = 0; phase < 2u; ++phase) {
        NeuralAdamDesc adam{};
        adam.learningRate = phase == 0u ? 1e-2f : 1e-3f;
        NeuralTrainer trainer;
        if (!trainer.init(out.net, adam, batch)) {
            return;
        }
        const u32 steps = phase == 0u ? phase1 : g_opt.steps - phase1;
        for (u32 s = 0; s < steps; ++s) {
            for (u32 i = 0; i < batch; ++i) {
                const u64 r = next();
                const u32 tx = static_cast<u32>(r % S);
                const u32 ty = static_cast<u32>((r >> 32) % S);
                x[i * 2u] = (static_cast<f32>(tx) + 0.5f) / static_cast<f32>(S);
                x[i * 2u + 1u] = (static_cast<f32>(ty) + 0.5f) / static_cast<f32>(S);
                std::memcpy(&y[static_cast<usize>(i) * kChannels], &tex.texels[(static_cast<usize>(ty) * S + tx) * kChannels],
                            kChannels * sizeof(f32));
            }
            loss = trainer.step(out.net, x.data(), y.data(), batch);
            if (!(loss >= 0.f)) {
                return;
            }
        }
    }
    out.finalLoss = loss;
    out.seconds = nowSeconds() - t0;
    out.ok = true;
}

// --- GPU buffers -------------------------------------------------------------------------------------------
struct GpuBuf {
    Buffer b{};
    u8 queue = rg::kNoQueue;
};

bool makeBuffer(HwContext& ctx, GpuBuf& out, u64 size, MemoryUsage mem, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(size);
    d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                       static_cast<u32>(BufferUsage::TransferDst) | static_cast<u32>(BufferUsage::TransferSrc));
    d.memoryUsage = mem;
    d.name = name;
    return ctx.allocator->createBuffer(d, out.b) && out.b.deviceAddress != 0u &&
           (mem == MemoryUsage::GpuOnly || out.b.mapped != nullptr);
}

void freeBuffer(HwContext& ctx, GpuBuf& b) {
    if (b.b.handle != nullptr) {
        ctx.allocator->destroyBuffer(b.b);
    }
    b = GpuBuf{};
}

bool uploadToBuffer(HwContext& ctx, GpuBuf& dst, const void* data, usize bytes) {
    usize offset = 0;
    usize done = 0;
    const usize chunk = 32u * 1024u * 1024u;
    while (done < bytes) {
        const usize n = std::min(chunk, bytes - done);
        if (!ctx.upload.stage(static_cast<const u8*>(data) + done, n, offset) ||
            !ctx.upload.recordBufferCopy(dst.b.handle, offset, done, n)) {
            return false;
        }
        ctx.upload.flush();
        if (!ctx.upload.waitAll()) {
            return false;
        }
        done += n;
    }
    return true;
}

// --- NTC GPU ----------------------------------------------------------------------------------------------
bool initNeuralGpu(HwContext& ctx, NeuralGpu& gpu, bool coop) {
    NeuralGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.framesInFlight = 3;
    d.initialSlotBytes = 8u * 1024u * 1024u;
    d.allowCooperativeMatrix = coop;
    return gpu.init(d);
}

/// One neural.infer frame. Returns the pass time (ms) or a negative value on failure.
f64 runInferFrame(HwContext& ctx, NeuralGpu& gpu, rg::Graph& graph, const NeuralNet& net, GpuBuf& in, GpuBuf& out,
                  u32 count, bool* usedCoop) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!gpu.beginFrame(ctx.serial, net)) {
        return -1.0;
    }
    graph.reset();
    const NeuralGraphRefs refs = gpu.importInto(graph);
    const rg::BufferRef inRef = graph.importBuffer(rg::ImportedBuffer{in.b.handle, in.b.desc.size, in.queue, &in.queue, "p9.nn_in"});
    const rg::BufferRef outRef =
        graph.importBuffer(rg::ImportedBuffer{out.b.handle, out.b.desc.size, out.queue, &out.queue, "p9.nn_out"});
    if (!gpu.addInferPass(graph, refs, inRef, 0u, in.b.deviceAddress, outRef, 0u, out.b.deviceAddress, count)) {
        return -1.0;
    }
    graph.addPass("p9.sink", nullptr, nullptr).use(outRef, rg::Access::HostRead).neverCull();
    if (usedCoop != nullptr) {
        *usedCoop = gpu.stats().coopPasses > 0u;
    }
    ctx.profiler->beginFrame();
    const rg::ExecuteResult r = ctx.executor->execute(graph);
    ctx.profiler->endFrame();
    const bool waited = ctx.executor->waitIdle();
    ctx.profiler->resolve();
    gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!r.ok || !waited) {
        return -1.0;
    }
    const auto z = zoneTimes(ctx.profiler->latest());
    const auto it = z.find("neural.infer");
    return it != z.end() ? it->second : -1.0;
}

Timed timeInfer(HwContext& ctx, NeuralGpu& gpu, const NeuralNet& net, GpuBuf& in, GpuBuf& out, u32 count, bool& usedCoop,
                f64& p10, f64& p90) {
    rg::Graph graph;
    std::vector<f64> trialMedians;
    std::vector<f64> all;
    for (u32 t = 0; t < g_opt.trials; ++t) {
        for (u32 w = 0; w < g_opt.warmup; ++w) {
            runInferFrame(ctx, gpu, graph, net, in, out, count, &usedCoop);
        }
        std::vector<f64> samples;
        for (u32 f = 0; f < g_opt.frames; ++f) {
            const f64 ms = runInferFrame(ctx, gpu, graph, net, in, out, count, &usedCoop);
            if (ms > 0.0) {
                samples.push_back(ms);
                all.push_back(ms);
            }
        }
        if (!samples.empty()) {
            trialMedians.push_back(quantile(samples, 0.5));
        }
    }
    p10 = quantile(all, 0.1);
    p90 = quantile(all, 0.9);
    return combineTrials(trialMedians);
}

// --- BCn ---------------------------------------------------------------------------------------------------
struct BcResult {
    bool ok = false;
    u64 bytes = 0;        ///< level 0, all three textures
    f64 encodeSeconds = 0.0;
    std::vector<f32> decoded;
};

#if P9_HAVE_BC
/// albedo: BC7 (or BC1), normal: BC5, roughness: BC4.
BcResult runBc(const MaterialTex& m, bool bc1Albedo) {
    using namespace fuse::cook;
    BcResult r{};
    const u32 S = m.size;
    const usize n = static_cast<usize>(S) * S;
    BcSourceImage albedo{S, S, std::vector<u8>(n * 4u), {}};
    BcSourceImage normal{S, S, std::vector<u8>(n * 4u), {}};
    BcSourceImage rough{S, S, std::vector<u8>(n * 4u), {}};
    for (usize i = 0; i < n; ++i) {
        const f32* t = &m.texels[i * kChannels];
        auto b = [](f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
        albedo.rgba8[i * 4u + 0u] = b(t[0]);
        albedo.rgba8[i * 4u + 1u] = b(t[1]);
        albedo.rgba8[i * 4u + 2u] = b(t[2]);
        albedo.rgba8[i * 4u + 3u] = 255u;
        normal.rgba8[i * 4u + 0u] = b(t[3]);
        normal.rgba8[i * 4u + 1u] = b(t[4]);
        normal.rgba8[i * 4u + 3u] = 255u;
        rough.rgba8[i * 4u + 0u] = b(t[5]);
        rough.rgba8[i * 4u + 3u] = 255u;
    }
    const BcFormat af = bc1Albedo ? BcFormat::BC1 : BcFormat::BC7;
    std::vector<u8> ba, bn, br;
    const f64 t0 = nowSeconds();
    // The three encodes in parallel (each encoder is single-threaded and deterministic).
    bool okA = false, okN = false, okR = false;
    std::thread ta([&] { okA = encode_bc_image(af, albedo, ba); });
    std::thread tn([&] { okN = encode_bc_image(BcFormat::BC5, normal, bn); });
    okR = encode_bc_image(BcFormat::BC4, rough, br);
    ta.join();
    tn.join();
    r.encodeSeconds = nowSeconds() - t0;
    if (!okA || !okN || !okR) {
        return r;
    }
    const u64 blocks = bc_block_count(S, S);
    if (ba.size() != blocks * bc_block_bytes(af) || bn.size() != blocks * 16u || br.size() != blocks * 8u) {
        std::fprintf(stderr, "BC block accounting mismatch\n");
        return r;
    }
    r.bytes = ba.size() + bn.size() + br.size();
    BcSourceImage da, dn, dr;
    if (!decode_bc_image(af, ba.data(), ba.size(), S, S, da) || !decode_bc_image(BcFormat::BC5, bn.data(), bn.size(), S, S, dn) ||
        !decode_bc_image(BcFormat::BC4, br.data(), br.size(), S, S, dr)) {
        return r;
    }
    r.decoded.resize(n * kChannels);
    for (usize i = 0; i < n; ++i) {
        f32* o = &r.decoded[i * kChannels];
        o[0] = da.rgba8[i * 4u + 0u] / 255.f;
        o[1] = da.rgba8[i * 4u + 1u] / 255.f;
        o[2] = da.rgba8[i * 4u + 2u] / 255.f;
        o[3] = dn.rgba8[i * 4u + 0u] / 255.f;
        o[4] = dn.rgba8[i * 4u + 1u] / 255.f;
        o[5] = dr.rgba8[i * 4u + 0u] / 255.f;
    }
    r.ok = true;
    return r;
}

/// RGBA8 albedo texture -> BC7 -> decoded RGBA8 (for the splat-vs-mesh comparison's BC7 variant).
bool bc7RoundTrip(const std::vector<u8>& rgba, u32 S, std::vector<u8>& out, u64& bytes) {
    using namespace fuse::cook;
    BcSourceImage img{S, S, rgba, {}};
    std::vector<u8> blocks;
    if (!encode_bc_image(BcFormat::BC7, img, blocks)) {
        return false;
    }
    bytes = blocks.size();
    BcSourceImage dec;
    if (!decode_bc_image(BcFormat::BC7, blocks.data(), blocks.size(), S, S, dec)) {
        return false;
    }
    out = dec.rgba8;
    return true;
}
#endif

u64 withMips(u64 level0) { return level0 + level0 / 3u; } ///< full chain ~ 4/3 x level 0 (exact for BC down to 4x4 within a few %)

// ============================================================================================================
// Part 1: neural texture compression
// ============================================================================================================
struct NtcRow {
    std::string name;
    u64 bytes = 0;       ///< per material, level 0
    u64 bytesMips = 0;   ///< per material, with a mip chain (NTC: same network; no in-tree mip support)
    TexQuality q{};      ///< mean over the materials
    f64 fitSeconds = 0;  ///< mean per material (CPU fit / BC encode)
    Timed costPortable{};
    Timed costCoop{};
    f64 portP10 = 0, portP90 = 0, coopP10 = 0, coopP90 = 0;
    bool coopRan = false;
    bool measuredCost = false;
};

void addQuality(TexQuality& acc, const TexQuality& q, f64 w) {
    acc.psnrAlbedo += q.psnrAlbedo * w;
    acc.psnrNormal += q.psnrNormal * w;
    acc.psnrRough += q.psnrRough * w;
    acc.psnrAll += q.psnrAll * w;
    acc.ssimAlbedo += q.ssimAlbedo * w;
    acc.normalDeg += q.normalDeg * w;
}

int runNtc(HwContext& ctx, std::vector<NtcRow>& rows) {
    const u32 S = g_opt.texSize;
    std::printf("\n=== Part 1: neural texture compression vs BCn / RGBA8 (%u x %u, 6 channels, 3 materials) ===\n", S, S);
    std::vector<MaterialTex> mats;
    for (u32 k = 0; k < 3u; ++k) {
        mats.push_back(makeMaterial(k, S));
    }
    const NeuralCapabilities caps = queryNeuralCapabilities(ctx.device.get());
    std::printf("neural caps: gpu %s, device coopmat %s, coopmat kernel %s (%s)\n", caps.gpu ? "yes" : "no",
                ctx.device->info().caps.cooperativeMatrix ? "yes" : "no", caps.coopMatrixKernelBuilt ? "built" : "not built",
                caps.reason);
    expect(caps.gpu, "neural GPU inference available on the discrete GPU");
    if (!caps.gpu) {
        return 1;
    }

    // --- conventional: raw 8-bit ----------------------------------------------------------------------
    {
        NtcRow r{};
        r.name = "RGBA8+RG8+R8 (raw)";
        r.bytes = static_cast<u64>(S) * S * (4u + 2u + 1u);
        r.bytesMips = withMips(r.bytes);
        r.q.psnrAlbedo = r.q.psnrNormal = r.q.psnrRough = r.q.psnrAll = 200.0;
        r.q.ssimAlbedo = 1.0;
        rows.push_back(r);
    }
    // --- conventional: BCn ------------------------------------------------------------------------------
#if P9_HAVE_BC
    for (u32 variant = 0; variant < 2u; ++variant) {
        NtcRow r{};
        r.name = variant == 0u ? "BC7+BC5+BC4" : "BC1+BC5+BC4";
        bool ok = true;
        for (const MaterialTex& m : mats) {
            const BcResult bc = runBc(m, variant == 1u);
            ok = ok && bc.ok;
            if (!bc.ok) {
                break;
            }
            r.bytes = bc.bytes;
            r.fitSeconds += bc.encodeSeconds / 3.0;
            const TexQuality q = measureTexture(m, bc.decoded);
            std::printf("  %-12s %-8s albedo %.2f dB  normal %.2f dB  rough %.2f dB  SSIM %.4f  normal %.2f deg  encode %.1f s\n",
                        r.name.c_str(), m.name, q.psnrAlbedo, q.psnrNormal, q.psnrRough, q.ssimAlbedo, q.normalDeg,
                        bc.encodeSeconds);
            addQuality(r.q, q, 1.0 / 3.0);
        }
        expect(ok, "BCn encode / decode with the in-tree cook encoders");
        const u64 expectBytes = static_cast<u64>((S + 3u) / 4u) * ((S + 3u) / 4u) * (variant == 0u ? 16u + 16u + 8u : 8u + 16u + 8u);
        expect(r.bytes == expectBytes, "BC byte accounting == blocks x block bytes");
        r.bytesMips = withMips(r.bytes);
        if (ok) {
            rows.push_back(r);
        }
    }
#else
    std::printf("  BCn: the cook encoders (Tools/FUSE/Cook, fuse/cook/bcn_encoder.hpp) are not visible to this target; "
                "BCn rows use the format's fixed rate only (no quality)\n");
    {
        NtcRow r{};
        r.name = "BC7+BC5+BC4 (rate only)";
        r.bytes = static_cast<u64>(S) * S * 5u / 2u;
        r.bytesMips = withMips(r.bytes);
        r.q.psnrAll = -1.0;
        rows.push_back(r);
    }
#endif

    // --- NTC fits (CPU, all materials x variants in parallel) ----------------------------------------
    const std::vector<NtcVariant> variants = g_opt.quick ? std::vector<NtcVariant>{{"NTC hash 2^12", 12u}}
                                                         : std::vector<NtcVariant>{{"NTC hash 2^13", 13u}, {"NTC hash 2^15", 15u}};
    std::vector<NtcFit> fits(variants.size() * mats.size());
    {
        std::printf("  fitting %zu networks on the CPU (%u steps x %u samples, Adam, one thread each)...\n", fits.size(),
                    g_opt.steps, g_opt.batch);
        const f64 t0 = nowSeconds();
        std::vector<std::thread> threads;
        for (usize v = 0; v < variants.size(); ++v) {
            for (usize m = 0; m < mats.size(); ++m) {
                threads.emplace_back([&, v, m] {
                    fitNtc(mats[m], ntcConfig(S, variants[v].log2Table), 1000u + v * 10u + m, fits[v * mats.size() + m]);
                });
            }
        }
        for (std::thread& t : threads) {
            t.join();
        }
        std::printf("  fits done in %.1f s wall\n", nowSeconds() - t0);
    }

    // --- GPU decode for quality (portable kernel, every texel), + coopmat decode quality -------------
    const usize texels = static_cast<usize>(S) * S;
    GpuBuf qin, qout;
    if (!makeBuffer(ctx, qin, texels * 2u * sizeof(f32), MemoryUsage::CpuToGpu, "p9.q_in") ||
        !makeBuffer(ctx, qout, texels * kChannels * sizeof(f32), MemoryUsage::GpuToCpu, "p9.q_out")) {
        std::fprintf(stderr, "FAIL: quality buffers\n");
        return 1;
    }
    {
        f32* x = static_cast<f32*>(qin.b.mapped);
        for (u32 y = 0; y < S; ++y) {
            for (u32 xx = 0; xx < S; ++xx) {
                x[(static_cast<usize>(y) * S + xx) * 2u] = (static_cast<f32>(xx) + 0.5f) / static_cast<f32>(S);
                x[(static_cast<usize>(y) * S + xx) * 2u + 1u] = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(S);
            }
        }
    }
    NeuralGpu portable, coop;
    if (!initNeuralGpu(ctx, portable, false)) {
        std::fprintf(stderr, "FAIL: NeuralGpu init (portable)\n");
        return 1;
    }
    const bool coopInit = initNeuralGpu(ctx, coop, true);
    const bool coopReady = coopInit && coop.coopMatrixReady();
    std::printf("  kernels: portable %s; cooperative matrix %s\n", portable.kernelLanguage(),
                coopReady ? "pipeline created" : "unavailable (pipeline not created)");

    // Timing buffers: 1440p, device local; uv tiles the texture ~1:1 (texel per pixel) with wrap.
    const u32 count = g_opt.width * g_opt.height;
    GpuBuf tin, tout;
    if (!makeBuffer(ctx, tin, static_cast<u64>(count) * 2u * sizeof(f32), MemoryUsage::GpuOnly, "p9.t_in") ||
        !makeBuffer(ctx, tout, static_cast<u64>(count) * kChannels * sizeof(f32), MemoryUsage::GpuOnly, "p9.t_out")) {
        std::fprintf(stderr, "FAIL: timing buffers\n");
        return 1;
    }
    {
        std::vector<f32> uv(static_cast<usize>(count) * 2u);
        for (u32 y = 0; y < g_opt.height; ++y) {
            for (u32 x = 0; x < g_opt.width; ++x) {
                const usize p = static_cast<usize>(y) * g_opt.width + x;
                uv[p * 2u] = std::fmod((static_cast<f32>(x) + 0.5f) / static_cast<f32>(S), 1.f);
                uv[p * 2u + 1u] = std::fmod((static_cast<f32>(y) + 0.5f) / static_cast<f32>(S), 1.f);
            }
        }
        if (!uploadToBuffer(ctx, tin, uv.data(), uv.size() * sizeof(f32))) {
            std::fprintf(stderr, "FAIL: upload of the 1440p inputs\n");
            return 1;
        }
    }

    rg::Graph graph;
    for (usize v = 0; v < variants.size(); ++v) {
        NtcRow r{};
        r.name = variants[v].name;
        TexQuality coopQ{};
        bool allOk = true;
        for (usize m = 0; m < mats.size(); ++m) {
            NtcFit& fit = fits[v * mats.size() + m];
            expect(fit.ok, "NTC fit ran (finite loss)");
            if (!fit.ok) {
                allOk = false;
                continue;
            }
            const NeuralNet& net = fit.net;
            // Memory accounting: blob = 256-byte header + fp16 params padded to 4 bytes; weight file = 80 + 2 x params.
            const u64 blob = net.gpuBlobBytes();
            const u64 expectBlob = sizeof(NeuralGpuHeader) + ((static_cast<u64>(net.paramCount()) * 2u + 3u) / 4u) * 4u;
            std::vector<u8> file;
            writeNeuralWeights(net, file);
            expect(blob == expectBlob, "NTC GPU blob bytes == header + fp16 parameters");
            expect(file.size() == kNeuralFileHeaderBytes + static_cast<usize>(net.paramCount()) * 2u,
                   "NTC weight file bytes == 80 + 2 x parameters");
            r.bytes = blob;
            r.fitSeconds += fit.seconds / static_cast<f64>(mats.size());
            // Decode every texel on the GPU (portable).
            const f64 ms = runInferFrame(ctx, portable, graph, net, qin, qout, static_cast<u32>(texels), nullptr);
            expect(ms > 0.0, "neural.infer quality decode ran");
            std::vector<f32> dec(static_cast<const f32*>(qout.b.mapped),
                                 static_cast<const f32*>(qout.b.mapped) + texels * kChannels);
            // GPU == CPU reference on a subset (portable kernel: transcendental-free config -> near bit-exact).
            {
                NeuralScratch scratch;
                f64 maxAbs = 0.0;
                const f32* x = static_cast<const f32*>(qin.b.mapped);
                for (usize i = 0; i < texels; i += 97u) {
                    f32 ref[kNnMaxWidth];
                    net.infer(x + i * 2u, ref, scratch);
                    for (u32 c = 0; c < kChannels; ++c) {
                        const f64 d = std::fabs(static_cast<f64>(ref[c]) - dec[i * kChannels + c]);
                        maxAbs = std::max(maxAbs, std::isfinite(d) ? d : 1e30);
                    }
                }
                expect(maxAbs <= 1e-4, "NTC GPU decode (portable) == CPU reference within 1e-4");
                if (m == 0u) {
                    std::printf("  %s: GPU portable vs CPU reference max |d| %.3g\n", r.name.c_str(), maxAbs);
                }
            }
            const TexQuality q = measureTexture(mats[m], dec);
            std::printf("  %-14s %-8s albedo %.2f dB  normal %.2f dB  rough %.2f dB  SSIM %.4f  normal %.2f deg  fit %.0f s "
                        "(loss %.2e, %u params)\n",
                        r.name.c_str(), mats[m].name, q.psnrAlbedo, q.psnrNormal, q.psnrRough, q.ssimAlbedo, q.normalDeg,
                        fit.seconds, static_cast<f64>(fit.finalLoss), net.paramCount());
            expect(std::isfinite(q.psnrAll), "NTC PSNR finite");
            expect(q.psnrAlbedo > 20.0, "NTC albedo PSNR above the 20 dB sanity floor");
            addQuality(r.q, q, 1.0 / static_cast<f64>(mats.size()));
            if (coopReady) {
                bool used = false;
                const f64 cms = runInferFrame(ctx, coop, graph, net, qin, qout, static_cast<u32>(texels), &used);
                if (cms > 0.0 && used) {
                    std::vector<f32> cdec(static_cast<const f32*>(qout.b.mapped),
                                          static_cast<const f32*>(qout.b.mapped) + texels * kChannels);
                    const TexQuality cq = measureTexture(mats[m], cdec);
                    addQuality(coopQ, cq, 1.0 / static_cast<f64>(mats.size()));
                    f64 maxAbs = 0.0, se = 0.0;
                    for (usize i = 0; i < cdec.size(); ++i) {
                        const f64 d = std::fabs(static_cast<f64>(cdec[i]) - dec[i]);
                        maxAbs = std::max(maxAbs, std::isfinite(d) ? d : 1e30);
                        se += d * d;
                    }
                    std::printf("  %-14s %-8s coopmat vs portable decode: max |d| %.3g, rms %.3g\n", r.name.c_str(), mats[m].name,
                                maxAbs, std::sqrt(se / static_cast<f64>(cdec.size())));
                    expect(maxAbs < 0.1, "coopmat decode within 0.1 of the portable decode (fp16 activations)");
                    r.coopRan = true;
                }
            }
        }
        r.bytesMips = r.bytes; // no in-tree mip support for NTC (one network per material, finest level only)
        if (r.coopRan) {
            std::printf("  %-14s cooperative-matrix decode (fp16 activations): albedo %.2f dB normal %.2f dB rough %.2f dB "
                        "(portable: %.2f / %.2f / %.2f)\n",
                        r.name.c_str(), coopQ.psnrAlbedo, coopQ.psnrNormal, coopQ.psnrRough, r.q.psnrAlbedo, r.q.psnrNormal,
                        r.q.psnrRough);
            expect(coopQ.psnrAlbedo > 20.0, "NTC coopmat decode albedo PSNR above the sanity floor");
        }
        // GPU cost: one material's network at every 1440p pixel.
        if (allOk) {
            std::printf("  timing %s at %u x %u (%u samples); GPU: %s\n", r.name.c_str(), g_opt.width, g_opt.height, count,
                        gpuUtilization().c_str());
            bool used = false;
            r.costPortable = timeInfer(ctx, portable, fits[v * mats.size()].net, tin, tout, count, used, r.portP10, r.portP90);
            expect(r.costPortable.finite, "neural.infer (portable) timing finite");
            if (coopReady) {
                bool usedC = false;
                r.costCoop = timeInfer(ctx, coop, fits[v * mats.size()].net, tin, tout, count, usedC, r.coopP10, r.coopP90);
                r.coopRan = r.coopRan || usedC;
                if (!usedC) {
                    r.costCoop = Timed{};
                }
            }
            r.measuredCost = true;
        }
        rows.push_back(r);
    }
    ctx.executor->waitIdle();
    portable.destroy();
    coop.destroy();
    for (GpuBuf* b : {&qin, &qout, &tin, &tout}) {
        freeBuffer(ctx, *b);
    }
    return 0;
}

// ============================================================================================================
// Part 2: gaussian splats vs triangles
// ============================================================================================================
constexpr f32 kSphereR = 1.f;
constexpr f32 kFovY = 50.f * 3.14159265f / 180.f;
constexpr f32 kEye[3] = {0.f, 0.45f, -2.6f};
constexpr f32 kTarget[3] = {0.f, 0.f, 0.f};
constexpr f32 kUp[3] = {0.f, 1.f, 0.f};
constexpr f32 kNear = 0.1f;
constexpr f32 kFar = 50.f;

/// The sphere's albedo in the uvSphere's (u, v): smooth colour waves + a sharp-edged checker.
void sphereAlbedo(f32 u, f32 v, f32 (&rgb)[3]) {
    const f32 tp = 6.28318531f;
    const bool check = ((static_cast<i32>(std::floor(u * 24.f)) + static_cast<i32>(std::floor(v * 12.f))) & 1) != 0;
    const f32 k = check ? 0.25f : 0.f;
    rgb[0] = 0.15f + 0.3f * (0.5f + 0.5f * std::sin(tp * 5.f * u)) + k;
    rgb[1] = 0.2f + 0.35f * (0.5f + 0.5f * std::cos(tp * 3.f * v + tp * 2.f * u)) + k * 0.8f;
    rgb[2] = 0.3f + 0.2f * (0.5f + 0.5f * std::sin(tp * 7.f * v)) + k * 0.4f;
}

void dirToUv(f32 nx, f32 ny, f32 nz, f32& u, f32& v) {
    const f32 theta = std::acos(std::clamp(ny, -1.f, 1.f));
    f32 phi = std::atan2(nz, nx);
    if (phi < 0.f) {
        phi += 6.28318531f;
    }
    u = phi / 6.28318531f;
    v = theta / 3.14159265f;
}

/// camera-space ray of pixel (x, y) in world space (GsCamera conventions: view rows R | t).
void pixelRay(const GsCamera& c, f32 px, f32 py, f32 (&origin)[3], f32 (&dir)[3]) {
    const f32 dc[3] = {(px - c.cx) / c.fx, (py - c.cy) / c.fy, 1.f};
    // world = R^T (camera - t)
    for (u32 i = 0; i < 3u; ++i) {
        dir[i] = c.view[0 * 4 + i] * dc[0] + c.view[1 * 4 + i] * dc[1] + c.view[2 * 4 + i] * dc[2];
        origin[i] = -(c.view[0 * 4 + i] * c.view[3] + c.view[1 * 4 + i] * c.view[7] + c.view[2 * 4 + i] * c.view[11]);
    }
}

/// Ground truth: analytic sphere, albedo at the hit, pixel centres; rgb per pixel (3 floats), black background.
std::vector<f32> groundTruth(const GsCamera& cam, u32 W, u32 H, u64& coveredPixels) {
    std::vector<f32> img(static_cast<usize>(W) * H * 3u, 0.f);
    coveredPixels = 0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            f32 o[3], d[3];
            pixelRay(cam, static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, o, d);
            const f64 a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const f64 b = 2.0 * (o[0] * d[0] + o[1] * d[1] + o[2] * d[2]);
            const f64 c = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - kSphereR * kSphereR;
            const f64 disc = b * b - 4 * a * c;
            if (disc < 0.0) {
                continue;
            }
            const f64 t = (-b - std::sqrt(disc)) / (2 * a);
            const f32 p[3] = {static_cast<f32>(o[0] + t * d[0]), static_cast<f32>(o[1] + t * d[1]), static_cast<f32>(o[2] + t * d[2])};
            f32 u, v, rgb[3];
            dirToUv(p[0] / kSphereR, p[1] / kSphereR, p[2] / kSphereR, u, v);
            sphereAlbedo(u, v, rgb);
            std::memcpy(&img[(static_cast<usize>(y) * W + x) * 3u], rgb, sizeof(rgb));
            ++coveredPixels;
        }
    }
    return img;
}

f64 imagePsnr(const std::vector<f32>& a, const std::vector<f32>& b) {
    f64 se = 0.0;
    for (usize i = 0; i < a.size(); ++i) {
        const f64 d = static_cast<f64>(a[i]) - std::clamp(b[i], 0.f, 1.f);
        se += d * d;
    }
    return psnrFromMse(se / static_cast<f64>(a.size()));
}

/// PSNR restricted to pixels where either image is non-black (the object, not the empty background).
f64 objectPsnr(const std::vector<f32>& gt, const std::vector<f32>& b, const std::vector<u8>& mask) {
    f64 se = 0.0;
    u64 n = 0;
    for (usize p = 0; p < mask.size(); ++p) {
        if (mask[p] == 0u) {
            continue;
        }
        for (u32 c = 0; c < 3u; ++c) {
            const f64 d = static_cast<f64>(gt[p * 3u + c]) - std::clamp(b[p * 3u + c], 0.f, 1.f);
            se += d * d;
        }
        n += 3u;
    }
    return n != 0u ? psnrFromMse(se / static_cast<f64>(n)) : 0.0;
}

/// Splats on the sphere surface (Fibonacci points, flat tangent discs, SH degree 0 = albedo at the centre).
std::vector<GsSplat> sphereSplats(u32 n) {
    std::vector<GsSplat> out(n);
    const f32 golden = 3.14159265f * (3.f - std::sqrt(5.f));
    const f32 spacing = std::sqrt(4.f * 3.14159265f * kSphereR * kSphereR / static_cast<f32>(n));
    for (u32 i = 0; i < n; ++i) {
        const f32 y = 1.f - 2.f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
        const f32 r = std::sqrt(std::max(0.f, 1.f - y * y));
        const f32 a = golden * static_cast<f32>(i);
        const f32 nrm[3] = {r * std::cos(a), y, r * std::sin(a)};
        GsSplat& s = out[i];
        for (u32 k = 0; k < 3u; ++k) {
            s.position[k] = nrm[k] * kSphereR;
        }
        s.opacity = 0.95f;
        s.scale[0] = 0.6f * spacing;
        s.scale[1] = 0.6f * spacing;
        s.scale[2] = 0.02f * spacing;
        // quaternion taking +z to the normal
        f32 q[4] = {1.f + nrm[2], -nrm[1], nrm[0], 0.f};
        f32 ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (ql < 1e-6f) {
            q[0] = 0.f;
            q[1] = 1.f;
            q[2] = 0.f;
            q[3] = 0.f;
            ql = 1.f;
        }
        for (u32 k = 0; k < 4u; ++k) {
            s.rotation[k] = q[k] / ql;
        }
        f32 u, v, rgb[3];
        dirToUv(nrm[0], nrm[1], nrm[2], u, v);
        sphereAlbedo(u, v, rgb);
        for (u32 c = 0; c < 3u; ++c) {
            s.sh[c] = (rgb[c] - 0.5f) / 0.28209479f;
        }
    }
    return out;
}

/// CPU rasterisation of the triangle mesh (what the visibility buffer + material resolve sample): pixel-centre
/// coverage, perspective-correct UVs, bilinear texture (wrap u, clamp v), nearest depth.
std::vector<f32> rasterMesh(const mr_test::SourceMesh& mesh, const std::vector<u8>& texRgba, u32 S, const GsCamera& cam, u32 W,
                            u32 H) {
    std::vector<f32> img(static_cast<usize>(W) * H * 3u, 0.f);
    std::vector<f32> zbuf(static_cast<usize>(W) * H, 1e30f);
    const usize nv = mesh.positions.size() / 3u;
    std::vector<f32> sx(nv), sy(nv), sz(nv);
    for (usize i = 0; i < nv; ++i) {
        const f32* p = &mesh.positions[i * 3u];
        f32 c[3];
        for (u32 r = 0; r < 3u; ++r) {
            c[r] = cam.view[r * 4 + 0] * p[0] + cam.view[r * 4 + 1] * p[1] + cam.view[r * 4 + 2] * p[2] + cam.view[r * 4 + 3];
        }
        sz[i] = c[2];
        sx[i] = cam.fx * c[0] / c[2] + cam.cx;
        sy[i] = cam.fy * c[1] / c[2] + cam.cy;
    }
    auto sample = [&](f32 u, f32 v, f32 (&rgb)[3]) {
        const f32 x = u * static_cast<f32>(S) - 0.5f;
        const f32 y = std::clamp(v * static_cast<f32>(S) - 0.5f, 0.f, static_cast<f32>(S - 1u));
        const f32 fx = std::floor(x);
        const f32 fy = std::floor(y);
        const f32 tx = x - fx;
        const f32 ty = y - fy;
        auto texel = [&](i32 ix, i32 iy, u32 c) {
            const u32 xx = static_cast<u32>(((ix % static_cast<i32>(S)) + static_cast<i32>(S)) % static_cast<i32>(S));
            const u32 yy = static_cast<u32>(std::clamp(iy, 0, static_cast<i32>(S) - 1));
            return texRgba[(static_cast<usize>(yy) * S + xx) * 4u + c] / 255.f;
        };
        const i32 ix = static_cast<i32>(fx);
        const i32 iy = static_cast<i32>(fy);
        for (u32 c = 0; c < 3u; ++c) {
            const f32 a = texel(ix, iy, c) + (texel(ix + 1, iy, c) - texel(ix, iy, c)) * tx;
            const f32 b = texel(ix, iy + 1, c) + (texel(ix + 1, iy + 1, c) - texel(ix, iy + 1, c)) * tx;
            rgb[c] = a + (b - a) * ty;
        }
    };
    for (usize t = 0; t + 2u < mesh.indices.size(); t += 3u) {
        const u32 i0 = mesh.indices[t], i1 = mesh.indices[t + 1u], i2 = mesh.indices[t + 2u];
        if (sz[i0] <= kNear || sz[i1] <= kNear || sz[i2] <= kNear) {
            continue;
        }
        const f32 x0 = sx[i0], y0 = sy[i0], x1 = sx[i1], y1 = sy[i1], x2 = sx[i2], y2 = sy[i2];
        const f32 area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-12f) {
            continue;
        }
        const i32 minX = std::max(0, static_cast<i32>(std::floor(std::min({x0, x1, x2}))));
        const i32 maxX = std::min(static_cast<i32>(W) - 1, static_cast<i32>(std::ceil(std::max({x0, x1, x2}))));
        const i32 minY = std::max(0, static_cast<i32>(std::floor(std::min({y0, y1, y2}))));
        const i32 maxY = std::min(static_cast<i32>(H) - 1, static_cast<i32>(std::ceil(std::max({y0, y1, y2}))));
        const f32 iz0 = 1.f / sz[i0], iz1 = 1.f / sz[i1], iz2 = 1.f / sz[i2];
        for (i32 y = minY; y <= maxY; ++y) {
            for (i32 x = minX; x <= maxX; ++x) {
                const f32 px = static_cast<f32>(x) + 0.5f;
                const f32 py = static_cast<f32>(y) + 0.5f;
                f32 w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                f32 w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                f32 w2 = 1.f - w0 - w1;
                if (w0 < 0.f || w1 < 0.f || w2 < 0.f) {
                    continue;
                }
                const f32 iz = w0 * iz0 + w1 * iz1 + w2 * iz2;
                const f32 z = 1.f / iz;
                const usize p = static_cast<usize>(y) * W + static_cast<usize>(x);
                if (z >= zbuf[p]) {
                    continue;
                }
                zbuf[p] = z;
                const f32 u = (w0 * mesh.uvs[i0 * 2u] * iz0 + w1 * mesh.uvs[i1 * 2u] * iz1 + w2 * mesh.uvs[i2 * 2u] * iz2) * z;
                const f32 v =
                    (w0 * mesh.uvs[i0 * 2u + 1u] * iz0 + w1 * mesh.uvs[i1 * 2u + 1u] * iz1 + w2 * mesh.uvs[i2 * 2u + 1u] * iz2) * z;
                f32 rgb[3];
                sample(u, v, rgb);
                std::memcpy(&img[p * 3u], rgb, sizeof(rgb));
            }
        }
    }
    return img;
}

u64 meshletBytes(const geometry::MeshletMesh& m) {
    return static_cast<u64>(m.meshlets.size()) * sizeof(m.meshlets[0]) + m.meshlet_vertices.size() * 4u +
           m.meshlet_triangles.size() * 4u + m.positions.size() * 2u + m.normals.size() * 4u + m.tangents.size() * 4u +
           m.uvs.size() * 4u + m.source_vertices.size() * 4u;
}

struct GsRow {
    std::string name;
    u64 primitives = 0;
    u64 bytes = 0;      ///< geometry (+ texture) bytes on the GPU
    f64 psnr = 0.0;     ///< full image
    f64 psnrObject = 0.0;
    Timed cost{};
    f64 p10 = 0, p90 = 0;
    std::map<std::string, f64> stageMs; ///< per pass (median of the last trial)
    u64 entries = 0;
    u32 capacity = 0;
    u32 dropped = 0;
};

/// GPU work of one splat frame. readback: copies Output + Counters into `rb`.
bool splatFrame(HwContext& ctx, GsplatRenderer& gs, rg::Graph& graph, const GsCamera& cam, GpuBuf* rb,
                std::map<std::string, f64>* zones) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    GsSettings settings{};
    settings.shDegree = 0;
    if (!gs.beginFrame(ctx.serial, cam, settings, g_opt.width, g_opt.height, nullptr)) {
        return false;
    }
    graph.reset();
    const GsGraphRefs refs = gs.importInto(graph);
    gs.addPasses(graph, refs, rg::TextureRef{});
    if (rb != nullptr) {
        const rg::BufferRef r = graph.importBuffer(rg::ImportedBuffer{rb->b.handle, rb->b.desc.size, rb->queue, &rb->queue, "p9.gs_rb"});
        gs.addCopy(graph, refs, GsCopySource::Output, r, 0u);
        gs.addCopy(graph, refs, GsCopySource::Counters, r, gs.copyBytes(GsCopySource::Output));
        graph.addPass("p9.gs_host", nullptr, nullptr).use(r, rg::Access::HostRead).neverCull();
    } else {
        graph.addPass("p9.gs_sink", nullptr, nullptr).use(refs.image, rg::Access::StorageRead).neverCull();
    }
    ctx.profiler->beginFrame();
    const rg::ExecuteResult res = ctx.executor->execute(graph);
    ctx.profiler->endFrame();
    const bool waited = ctx.executor->waitIdle();
    ctx.profiler->resolve();
    gs.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (zones != nullptr) {
        *zones = zoneTimes(ctx.profiler->latest());
    }
    return res.ok && waited;
}

f64 sumZones(const std::map<std::string, f64>& z, const char* prefix) {
    f64 s = 0.0;
    for (const auto& [k, v] : z) {
        if (k.rfind(prefix, 0) == 0u) {
            s += v;
        }
    }
    return s;
}

/// Mesh-path passes of the FrameComposer frame: everything except lighting and the composer's own frame.* passes.
bool isGeometryZone(const std::string& name) {
    return name.rfind("light", 0) != 0u && name.rfind("frame.", 0) != 0u && name.rfind("p9.", 0) != 0u &&
           name.rfind("cluster", 0) != 0u;
}

int runGsplat(HwContext& ctx, std::vector<GsRow>& rows, u64& covered) {
    const u32 W = g_opt.width;
    const u32 H = g_opt.height;
    std::printf("\n=== Part 2: gaussian splats vs triangles (visibility buffer), %u x %u ===\n", W, H);
    const GsCapabilities caps = queryGsplatCapabilities(ctx.device.get());
    expect(caps.gsplat, "gaussian splatting available on the discrete GPU");
    if (!caps.gsplat) {
        std::fprintf(stderr, "gsplat: %s\n", caps.reason);
        return 1;
    }
    const GsCamera cam = gs_camera_look_at(kEye, kTarget, kUp, kFovY, W, H, kNear, kFar);
    const std::vector<f32> gt = groundTruth(cam, W, H, covered);
    std::printf("ground truth: analytic sphere covers %llu px (%.1f %% of the frame)\n", static_cast<unsigned long long>(covered),
                100.0 * static_cast<f64>(covered) / (static_cast<f64>(W) * H));
    std::vector<u8> mask(static_cast<usize>(W) * H, 0u);
    for (usize p = 0; p < mask.size(); ++p) {
        mask[p] = (gt[p * 3u] + gt[p * 3u + 1u] + gt[p * 3u + 2u]) > 0.f ? 1u : 0u;
    }

    // --- splats ----------------------------------------------------------------------------------------
    for (u32 n : g_opt.splatCounts) {
        GsRow row{};
        char name[64];
        std::snprintf(name, sizeof(name), "splats %u (SH0)", n);
        row.name = name;
        row.primitives = n;
        const std::vector<GsSplat> splats = sphereSplats(n);
        row.bytes = static_cast<u64>(n) * sizeof(GsSplat);
        // Sort capacity from a CPU preprocess of this camera (the sort runs over the whole capacity).
        GsFrameConstants fc{};
        GsSettings settings{};
        settings.shDegree = 0;
        gs_resolve_constants(cam, settings, W, H, n, 0u, 1u, false, fc);
        u64 entries = 0;
        for (const GsSplat& s : splats) {
            entries += gs_tiles_touched(gs_preprocess_splat(s, fc));
        }
        row.entries = entries;
        const u32 capacity = static_cast<u32>(std::min<u64>(((entries * 5u / 4u) + (1u << 20) - 1u) & ~((1ull << 20) - 1u), 1ull << 28));
        row.capacity = capacity;
        GsplatRenderer gs;
        GsplatDesc d{};
        d.device = ctx.device.get();
        d.allocator = ctx.allocator.get();
        d.bindless = &ctx.bindless;
        d.maxSplats = n;
        d.maxEntries = capacity;
        if (!gs.init(d) || !gs.setSplats(splats.data(), n, 0u)) {
            expect(false, "GsplatRenderer init / setSplats");
            continue;
        }
        GpuBuf rb;
        const u64 outBytes = static_cast<u64>(W) * H * 16u;
        if (!makeBuffer(ctx, rb, outBytes + 64u, MemoryUsage::GpuToCpu, "p9.gs_rb")) {
            expect(false, "splat readback buffer");
            gs.destroy();
            continue;
        }
        rg::Graph graph;
        const bool ok = splatFrame(ctx, gs, graph, cam, &rb, nullptr);
        expect(ok, "splat frame ran");
        const f32* out = static_cast<const f32*>(rb.b.mapped);
        u32 counters[4] = {};
        std::memcpy(counters, static_cast<const u8*>(rb.b.mapped) + gs.copyBytes(GsCopySource::Output), sizeof(counters));
        row.dropped = counters[1];
        expect(counters[1] == 0u, "splat sort capacity: no dropped entries");
        std::vector<f32> img(static_cast<usize>(W) * H * 3u);
        bool finite = true;
        for (usize p = 0; p < static_cast<usize>(W) * H; ++p) {
            for (u32 c = 0; c < 3u; ++c) {
                img[p * 3u + c] = out[p * 4u + c]; // premultiplied over black
                finite = finite && std::isfinite(out[p * 4u + c]);
            }
        }
        expect(finite, "splat image finite");
        row.psnr = imagePsnr(gt, img);
        row.psnrObject = objectPsnr(gt, img, mask);
        expect(row.psnr > 15.0, "splat PSNR above the 15 dB sanity floor");
        // timing
        std::printf("  timing %s (entries %llu, capacity %u); GPU: %s\n", row.name.c_str(), static_cast<unsigned long long>(entries),
                    capacity, gpuUtilization().c_str());
        std::vector<f64> trialMedians, all;
        for (u32 t = 0; t < g_opt.trials; ++t) {
            for (u32 w = 0; w < g_opt.warmup; ++w) {
                splatFrame(ctx, gs, graph, cam, nullptr, nullptr);
            }
            std::vector<f64> samples;
            std::map<std::string, std::vector<f64>> perPass;
            for (u32 f = 0; f < g_opt.frames; ++f) {
                std::map<std::string, f64> z;
                if (!splatFrame(ctx, gs, graph, cam, nullptr, &z)) {
                    continue;
                }
                const f64 total = sumZones(z, "gsplat.");
                if (total > 0.0) {
                    samples.push_back(total);
                    all.push_back(total);
                }
                for (const auto& [k, v] : z) {
                    if (k.rfind("gsplat.", 0) == 0u) {
                        perPass[k].push_back(v);
                    }
                }
            }
            if (!samples.empty()) {
                trialMedians.push_back(quantile(samples, 0.5));
            }
            row.stageMs.clear();
            for (const auto& [k, v] : perPass) {
                row.stageMs[k] = quantile(v, 0.5);
            }
        }
        row.cost = combineTrials(trialMedians);
        row.p10 = quantile(all, 0.1);
        row.p90 = quantile(all, 0.9);
        expect(row.cost.finite, "splat timing finite");
        std::printf("  %-22s PSNR %.2f dB (object %.2f dB)  GPU %.3f ms [%.3f..%.3f]  passes:", row.name.c_str(), row.psnr,
                    row.psnrObject, row.cost.value, row.cost.lo, row.cost.hi);
        for (const auto& [k, v] : row.stageMs) {
            std::printf(" %s %.3f", k.c_str() + 7, v);
        }
        std::printf("\n");
        ctx.executor->waitIdle();
        gs.destroy();
        freeBuffer(ctx, rb);
        rows.push_back(row);
    }

    // --- triangles ------------------------------------------------------------------------------------
    const u32 S = g_opt.texSize;
    std::vector<u8> texRgba(static_cast<usize>(S) * S * 4u);
    for (u32 y = 0; y < S; ++y) {
        for (u32 x = 0; x < S; ++x) {
            f32 rgb[3];
            sphereAlbedo((static_cast<f32>(x) + 0.5f) / static_cast<f32>(S), (static_cast<f32>(y) + 0.5f) / static_cast<f32>(S), rgb);
            for (u32 c = 0; c < 3u; ++c) {
                texRgba[(static_cast<usize>(y) * S + x) * 4u + c] = static_cast<u8>(std::lround(std::clamp(rgb[c], 0.f, 1.f) * 255.f));
            }
            texRgba[(static_cast<usize>(y) * S + x) * 4u + 3u] = 255u;
        }
    }
    std::vector<u8> texBc7;
    u64 bc7Bytes = 0;
#if P9_HAVE_BC
    const bool haveBc7 = bc7RoundTrip(texRgba, S, texBc7, bc7Bytes);
    expect(haveBc7, "BC7 round trip of the sphere texture");
#else
    const bool haveBc7 = false;
#endif
    const u32 tess[][2] = {{128u, 256u}, {256u, 512u}};
    for (const auto& t : tess) {
        const mr_test::SourceMesh src = mr_test::uvSphere(t[0], t[1], kSphereR);
        geometry::MeshletMesh mesh;
        if (!mr_test::build(src, mesh)) {
            expect(false, "meshlet build of the uv sphere");
            continue;
        }
        const u64 tris = mesh.triangle_count();
        expect(tris == src.indices.size() / 3u, "meshlet triangles == source triangles");
        const u64 geoBytes = meshletBytes(mesh);
        // Quality (CPU raster of the same triangles; texture RGBA8 and BC7).
        for (u32 texVariant = 0; texVariant < (haveBc7 ? 2u : 1u); ++texVariant) {
            GsRow row{};
            char name[96];
            std::snprintf(name, sizeof(name), "mesh %llu tris + %s %u^2", static_cast<unsigned long long>(tris),
                          texVariant == 0u ? "RGBA8" : "BC7", S);
            row.name = name;
            row.primitives = tris;
            row.bytes = geoBytes + withMips(texVariant == 0u ? static_cast<u64>(S) * S * 4u : bc7Bytes);
            const std::vector<f32> img = rasterMesh(src, texVariant == 0u ? texRgba : texBc7, S, cam, W, H);
            row.psnr = imagePsnr(gt, img);
            row.psnrObject = objectPsnr(gt, img, mask);
            expect(row.psnr > 15.0, "mesh PSNR above the 15 dB sanity floor");
            rows.push_back(row);
        }
        // GPU cost: FrameComposer frame (T0, scene stage + lighting + resolve only) with this mesh.
        GsRow& costRow = rows[rows.size() - (haveBc7 ? 2u : 1u)];
        gpu_scene::GpuScene scene;
        gpu_scene::GpuSceneDesc sd{};
        sd.device = ctx.device.get();
        sd.allocator = ctx.allocator.get();
        sd.upload = &ctx.upload;
        sd.bindless = &ctx.bindless;
        sd.instanceCapacity = 16;
        sd.meshCapacity = 4;
        if (!scene.init(sd) || !scene.gpuEnabled()) {
            expect(false, "GpuScene init");
            continue;
        }
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        scene.beginFrame(ctx.serial);
        if (scene.addMeshletMesh(mesh) != 0u) {
            expect(false, "addMeshletMesh");
            continue;
        }
        Material mat{};
        mat.baseColor = fuse::math::Vec3{0.6f, 0.5f, 0.4f};
        mat.roughness = 0.6f;
        scene.setMaterial(0u, mat);
        gpu_scene::InstanceDesc id{};
        id.mesh = 0;
        id.material = 0;
        scene.addInstance(id);
        gpu_scene::GpuLight sun{};
        sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
        sun.direction[0] = -0.3f;
        sun.direction[1] = -0.8f;
        sun.direction[2] = 0.5f;
        sun.color[0] = sun.color[1] = sun.color[2] = 1.f;
        sun.intensity = 2.f;
        const u32 sunSlot = scene.addLight(sun).slot;
        const bool committed = scene.commit().ok;
        ctx.upload.flush();
        expect(committed && ctx.upload.waitAll(), "scene commit");

        frame::FrameComposer composer;
        frame::FrameComposerDesc cd{};
        frame::applyLavapipeComposerDefaults(cd);
        cd.device = ctx.device.get();
        cd.allocator = ctx.allocator.get();
        cd.bindless = &ctx.bindless;
        cd.upload = &ctx.upload;
        cd.scene = &scene;
        cd.tier = frame::FrameTier::T0;
        cd.renderWidth = W;
        cd.renderHeight = H;
        cd.displayWidth = W;
        cd.displayHeight = H;
        cd.instanceCapacity = 16;
        cd.meshCapacity = 4;
        cd.lightCapacity = 8;
        cd.frameGen = false;
        if (!composer.init(cd)) {
            std::fprintf(stderr, "FrameComposer init: %s\n", composer.reason());
            expect(false, "FrameComposer init");
            scene.destroy();
            continue;
        }
        frame::FrameSettings fs{};
        frame::applyLavapipeTier0FrameSettings(fs, W, H);
        fs.vsm = false;
        fs.ddgi = false;
        fs.ssfx = false;
        fs.ssrc = false;
        fs.sky = false;
        fs.sunDisk = false;
        fs.aerialPerspective = false;
        fs.fog = false;
        fs.clouds = false;
        fs.splats = false;
        fs.restir = false;
        fs.rtShadows = false;
        fs.rtReflections = false;
        fs.forward = false;
        fs.frameGen = false;
        fs.upscaler = frame::FrameUpscaler::None;
        fs.post = false;
        fs.ui = false;
        std::printf("  timing %s (FrameComposer T0 scene stage); GPU: %s\n", costRow.name.c_str(), gpuUtilization().c_str());
        rg::Graph graph;
        u32 frameIndex = 0;
        auto meshFrame = [&](std::map<std::string, f64>* zones) {
            ++ctx.serial;
            ctx.bindless.setFrameSerial(ctx.serial);
            scene.beginFrame(ctx.serial);
            composer.beginSceneFrame(ctx.serial);
            const bool sc = scene.commit().ok;
            const bool cc = composer.commitScene();
            ctx.upload.flush();
            frame::FrameDesc fd{};
            fd.serial = ctx.serial;
            fd.frameIndex = frameIndex++;
            std::memcpy(fd.camera.eye, kEye, sizeof(kEye));
            std::memcpy(fd.camera.target, kTarget, sizeof(kTarget));
            fd.camera.fovY = kFovY;
            fd.camera.nearPlane = kNear;
            fd.camera.farPlane = kFar;
            fd.sun.slot = sunSlot;
            if (!composer.beginFrame(fd, fs)) {
                return false;
            }
            graph.reset();
            const frame::FrameGraphOutputs outs = composer.addFrame(graph);
            graph.addPass("p9.sink", nullptr, nullptr).use(outs.output, rg::Access::SampledRead).neverCull();
            ctx.profiler->beginFrame();
            const rg::ExecuteResult r = ctx.executor->execute(graph);
            ctx.profiler->endFrame();
            const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
            ctx.profiler->resolve();
            composer.collectRetired(ctx.serial);
            scene.collectRetired(ctx.serial);
            ctx.bindless.collectRetired(ctx.serial);
            if (zones != nullptr) {
                *zones = zoneTimes(ctx.profiler->latest());
            }
            return sc && cc && r.ok && waited;
        };
        std::vector<f64> trialMedians, all;
        bool printedZones = false;
        for (u32 tr = 0; tr < g_opt.trials; ++tr) {
            for (u32 w = 0; w < g_opt.warmup; ++w) {
                meshFrame(nullptr);
            }
            std::vector<f64> samples;
            std::map<std::string, std::vector<f64>> perPass;
            for (u32 f = 0; f < g_opt.frames; ++f) {
                std::map<std::string, f64> z;
                if (!meshFrame(&z)) {
                    expect(false, "mesh frame ran");
                    continue;
                }
                f64 geo = 0.0;
                for (const auto& [k, v] : z) {
                    perPass[k].push_back(v);
                    if (isGeometryZone(k)) {
                        geo += v;
                    }
                }
                if (geo > 0.0) {
                    samples.push_back(geo);
                    all.push_back(geo);
                }
            }
            if (!samples.empty()) {
                trialMedians.push_back(quantile(samples, 0.5));
            }
            costRow.stageMs.clear();
            for (const auto& [k, v] : perPass) {
                costRow.stageMs[k] = quantile(v, 0.5);
            }
            if (!printedZones) {
                printedZones = true;
                std::printf("    composer passes (median ms; * = counted as the mesh path):");
                for (const auto& [k, v] : costRow.stageMs) {
                    std::printf(" %s%s %.3f", isGeometryZone(k) ? "*" : "", k.c_str(), v);
                }
                std::printf("\n");
            }
        }
        costRow.cost = combineTrials(trialMedians);
        costRow.p10 = quantile(all, 0.1);
        costRow.p90 = quantile(all, 0.9);
        expect(costRow.cost.finite, "mesh-path timing finite");
        // the BC7 row shares the GPU cost (the material is untextured on the GPU: see the note)
        if (haveBc7) {
            rows.back().cost = costRow.cost;
            rows.back().p10 = costRow.p10;
            rows.back().p90 = costRow.p90;
        }
        ctx.executor->waitIdle();
        composer.destroy();
        scene.destroy();
    }
    return 0;
}

const char* fmtBytes(u64 b, char* buf, usize n) {
    if (b >= 1024ull * 1024ull) {
        std::snprintf(buf, n, "%.2f MiB", static_cast<f64>(b) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, n, "%.1f KiB", static_cast<f64>(b) / 1024.0);
    }
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto num = [&](u32& out) {
            if (i + 1 < argc) {
                out = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
            }
        };
        if (a == "--quick") {
            g_opt.quick = true;
            g_opt.texSize = 256;
            g_opt.steps = 300;
            g_opt.batch = 2048;
            g_opt.frames = 8;
            g_opt.warmup = 3;
            g_opt.trials = 2;
            g_opt.splatCounts = {50000u};
        } else if (a == "--size") {
            num(g_opt.texSize);
        } else if (a == "--steps") {
            num(g_opt.steps);
        } else if (a == "--frames") {
            num(g_opt.frames);
        } else if (a == "--trials") {
            num(g_opt.trials);
        }
    }
    HwContext ctx;
    HwSetupDesc sd{};
    sd.appName = "fuse_rp_hw_bench_p9";
    sd.stagingBytes = 64u * 1024u * 1024u;
    if (const int rc = setup(ctx, sd); rc != 0) {
        return rc;
    }
    std::printf("P9 exit bench: %u x %u, texture %u^2, NTC fit %u x %u, %u trials x %u frames (+%u warm-up)%s\n", g_opt.width,
                g_opt.height, g_opt.texSize, g_opt.steps, g_opt.batch, g_opt.trials, g_opt.frames, g_opt.warmup,
                P9_HAVE_BC ? ", BCn via the in-tree cook encoders" : ", no BCn encoder visible");
    std::printf("GPU before: %s\n", gpuUtilization().c_str());

    std::vector<NtcRow> ntc;
    if (runNtc(ctx, ntc) != 0) {
        ++g_failures;
    }
    std::vector<GsRow> gsr;
    u64 covered = 0;
    if (runGsplat(ctx, gsr, covered) != 0) {
        ++g_failures;
    }

    // --- tables ---------------------------------------------------------------------------------------
    char b1[32], b2[32];
    std::printf("\n--- P9 table 1: texture compression, per material (%u^2, albedo RGB + normal XY + roughness) ---\n",
                g_opt.texSize);
    std::printf("%-26s %12s %12s %8s %8s %8s %7s %7s %9s %26s %26s\n", "technique", "bytes L0", "bytes+mips", "albedo", "normal",
                "rough", "SSIM", "n.deg", "fit/enc s", "1440p decode portable ms", "1440p decode coopmat ms");
    for (const NtcRow& r : ntc) {
        char cp[48] = "-", cc[48] = "-";
        if (r.measuredCost) {
            std::snprintf(cp, sizeof(cp), "%.3f [%.3f..%.3f]", r.costPortable.value, r.costPortable.lo, r.costPortable.hi);
            if (r.costCoop.finite) {
                std::snprintf(cc, sizeof(cc), "%.3f [%.3f..%.3f]", r.costCoop.value, r.costCoop.lo, r.costCoop.hi);
            } else {
                std::snprintf(cc, sizeof(cc), "n/a");
            }
        } else if (r.name.rfind("BC", 0) == 0u || r.name.rfind("RGBA8", 0) == 0u) {
            std::snprintf(cp, sizeof(cp), "HW texture unit (n/m)");
        }
        if (r.q.psnrAll < 0.0) {
            std::printf("%-26s %12s %12s %8s %8s %8s %7s %7s %9s %26s %26s\n", r.name.c_str(), fmtBytes(r.bytes, b1, 32),
                        fmtBytes(r.bytesMips, b2, 32), "-", "-", "-", "-", "-", "-", cp, cc);
            continue;
        }
        std::printf("%-26s %12s %12s %8.2f %8.2f %8.2f %7.4f %7.2f %9.1f %26s %26s\n", r.name.c_str(), fmtBytes(r.bytes, b1, 32),
                    fmtBytes(r.bytesMips, b2, 32), std::min(r.q.psnrAlbedo, 99.0), std::min(r.q.psnrNormal, 99.0),
                    std::min(r.q.psnrRough, 99.0), r.q.ssimAlbedo, r.q.normalDeg, r.fitSeconds, cp, cc);
    }
    std::printf("(PSNR in dB vs the 8-bit source, 99 = lossless; NTC bytes = NeuralGpu blob, no mip chain in tree; "
                "decode = neural.infer over %u x %u samples, median of trial medians [min..max])\n",
                g_opt.width, g_opt.height);

    std::printf("\n--- P9 table 2: gaussian splats vs triangles, textured unit sphere at %u x %u (%llu px covered) ---\n",
                g_opt.width, g_opt.height, static_cast<unsigned long long>(covered));
    std::printf("%-34s %10s %12s %9s %9s %28s %10s\n", "representation", "prims", "GPU bytes", "PSNR", "obj PSNR",
                "GPU ms median [trial min..max]", "p10..p90");
    for (const GsRow& r : gsr) {
        char c[48] = "-", pp[40] = "-";
        if (r.cost.finite) {
            std::snprintf(c, sizeof(c), "%.3f [%.3f..%.3f]", r.cost.value, r.cost.lo, r.cost.hi);
            std::snprintf(pp, sizeof(pp), "%.3f..%.3f", r.p10, r.p90);
        }
        std::printf("%-34s %10llu %12s %9.2f %9.2f %28s %10s\n", r.name.c_str(), static_cast<unsigned long long>(r.primitives),
                    fmtBytes(r.bytes, b1, 32), r.psnr, r.psnrObject, c, pp);
    }
    std::printf("(splat GPU ms = sum of gsplat.* passes; mesh GPU ms = FrameComposer instance cull + Hi-Z + visibility buffer + "
                "material resolve + motion passes, untextured material on the GPU; quality: unlit albedo vs the analytic "
                "ray cast)\n");
    std::printf("GPU after: %s\n", gpuUtilization().c_str());

    // --- verdicts -------------------------------------------------------------------------------------
    std::printf("\n--- P9 verdicts (\"techniques that do not win are not shipped\") ---\n");
    const NtcRow* bc7 = nullptr;
    const NtcRow* bestNtc = nullptr;
    for (const NtcRow& r : ntc) {
        if (r.name == "BC7+BC5+BC4") {
            bc7 = &r;
        }
        if (r.name.rfind("NTC", 0) == 0u && (bestNtc == nullptr || r.q.psnrAll > bestNtc->q.psnrAll)) {
            bestNtc = &r;
        }
    }
    if (bestNtc != nullptr && bc7 != nullptr) {
        const bool memWin = bestNtc->bytesMips < bc7->bytesMips;
        const bool qWin = bestNtc->q.psnrAll >= bc7->q.psnrAll;
        std::printf("NTC (%s) vs BC7+BC5+BC4: memory %s (%.2f vs %.2f MiB with mips), quality %s (%.2f vs %.2f dB all-channel "
                    "PSNR), cost loses (%.3f ms/material/1440p ALU decode vs fixed-function BC sampling) -> %s\n",
                    bestNtc->name.c_str(), memWin ? "wins" : "loses", bestNtc->bytesMips / 1048576.0, bc7->bytesMips / 1048576.0,
                    qWin ? "wins" : "loses", bestNtc->q.psnrAll, bc7->q.psnrAll, bestNtc->costPortable.value,
                    (memWin && qWin) ? "WINS memory+quality, loses cost: ship only if the frame budget allows"
                                     : "DOES NOT WIN: not shipped");
    } else if (bestNtc != nullptr) {
        std::printf("NTC: no BCn quality reference in this build (cook encoders not visible); NTC best all-channel PSNR %.2f dB at "
                    "%.2f MiB vs BC7+BC5+BC4's fixed %.2f MiB\n",
                    bestNtc->q.psnrAll, bestNtc->bytes / 1048576.0, withMips(static_cast<u64>(g_opt.texSize) * g_opt.texSize * 5u / 2u) / 1048576.0);
    }
    const GsRow* bestSplat = nullptr;
    const GsRow* mesh = nullptr;
    for (const GsRow& r : gsr) {
        if (r.name.rfind("splats", 0) == 0u && (bestSplat == nullptr || r.psnrObject > bestSplat->psnrObject)) {
            bestSplat = &r;
        }
        if (r.name.rfind("mesh", 0) == 0u && mesh == nullptr) {
            mesh = &r; // the lighter tessellation (RGBA8 texture)
        }
    }
    if (bestSplat != nullptr && mesh != nullptr) {
        const bool memWin = bestSplat->bytes < mesh->bytes;
        const bool qWin = bestSplat->psnrObject >= mesh->psnrObject;
        const bool cWin = bestSplat->cost.value < mesh->cost.value;
        std::printf("Gaussian splats (%s) vs %s: memory %s (%.2f vs %.2f MiB), quality %s (%.2f vs %.2f dB object PSNR), cost %s "
                    "(%.3f vs %.3f ms) -> %s\n",
                    bestSplat->name.c_str(), mesh->name.c_str(), memWin ? "wins" : "loses", bestSplat->bytes / 1048576.0,
                    mesh->bytes / 1048576.0, qWin ? "wins" : "loses", bestSplat->psnrObject, mesh->psnrObject,
                    cWin ? "wins" : "loses", bestSplat->cost.value, mesh->cost.value,
                    (memWin && qWin && cWin) ? "WINS" : "DOES NOT WIN on surface content: not shipped as a mesh replacement");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}

#endif // FUSE_VULKAN_BACKEND
