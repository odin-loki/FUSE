// WP-6.4 in-tree denoiser (SVGF / A-SVGF): CPU gates (stub-safe). The Lavapipe gates are test_rp_denoise.cpp.
//
//   layout        DenoiseFrameConstants / DenoisePush sizes; the GLSL and Slang mirrors of DenoiseFrameConstants
//                 (shaders/denoise/dn_common.{glsl,slang}) declare the same fields in the same order at the same
//                 offsets (parsed from the sources); SvgfBufferLayout sections are 256-aligned, disjoint (or
//                 ping-pong exactly as documented) and sized for the extent.
//   backends      the reference chain (every pass, A-SVGF on, 6 frames of the synthetic sequence) is bit-identical
//                 on CpuReference and CpuParallel (0, 2, 4 workers); kernel stats under the pass names.
//   estimator     the synthetic generator is a valid test input: the one-sample estimators are unbiased (mean of
//                 2048 realisations within 5 sigma of the converged value on every pixel, each signal), and the
//                 A-SVGF gradient samples only see lighting changes (static scene: cur == prev bit for bit;
//                 after a light step: cur == scale x prev).
//   disocclusion  history rejection: on the moving-object sequence every disoccluded pixel (all four history
//                 taps on another surface) restarts (length 1, temporal result == the new sample bit for bit),
//                 every stable pixel keeps its history; control: with the depth / normal tests disabled the same
//                 pixels keep (wrong) history; frame 0 and reset() start every pixel over; sky passes through.
//   quality       variance reduction factor and bias against the converged reference (ensemble of 32 noisy
//                 sequences, 16 frames, 128 x 96, last frame, all surface pixels) for the three signal variants in
//                 a dynamic (moving shadow, A-SVGF) and a static-lighting (SVGF) scenario; the thresholds are the
//                 WP-6.4 acceptance (kQuality below).
//   asvgf         A-SVGF against SVGF after an abrupt lighting change (GI x 0.25 at frame 10): mean relative error
//                 over the 4 frames after the change at most half of SVGF's; before the change RMSE no worse
//                 than 1.25 x SVGF's.
//   api           SvgfDenoiser without a device fails cleanly; resolve_constants clamps / flags; presets.
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/denoise/denoise_types.hpp>
#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/renderer/denoise/svgf_kernel.hpp>
#include <fuse/renderer/denoise/svgf_reference.hpp>
#include <fuse/renderer/denoise/svgf_synthetic.hpp>
#include <fuse/renderer/rg/graph.hpp>

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

using namespace fuse;
using namespace fuse::renderer;
using namespace fuse::renderer::denoise;
using svgf_kernel::F2;
using svgf_kernel::F4;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

const char* signalName(DenoiseSignal s) {
    return s == DenoiseSignal::Shadow ? "shadow" : (s == DenoiseSignal::Reflection ? "reflection" : "gi");
}

/// WP-6.4 acceptance thresholds: variance reduction factor and bias against the converged reference.
/// Ensemble of kQualityRealisations independent noisy sequences of kQualityFrames frames at 128 x 96 (the scene's
/// features scale with the width), evaluated on the last frame over all surface pixels (incl. the moving object's
/// disoccluded trail). Two scenarios per signal (presets):
///   dynamic  the object and its floor shadow move (1.5 px / frame) while the camera pans (0.37 px / frame): A-SVGF
///            (the producer's gradient samples), the moving shadow is the hard case for temporal reuse
///   static   the object moves with the world (static lighting, camera still panning): plain SVGF
/// Measured on the CPU reference (printed by this gate, 2026-09-24: see RENDERER-EXECUTION.md WP-6.4) and set with
/// margin; the Lavapipe gate applies the same numbers to the GPU with 16 realisations.
struct QualityThreshold {
    DenoiseSignal signal;
    bool dynamic;   ///< dynamic scene + A-SVGF, else static lighting + SVGF
    f64 minVrf;     ///< input variance / output variance
    f64 maxRelBias; ///< noise-corrected RMS bias / mean truth (QualityReport::relRmsBias)
};
constexpr u32 kQualityWidth = 128u;
constexpr u32 kQualityHeight = 96u;
constexpr u32 kQualityFrames = 16u;
constexpr u32 kQualityRealisations = 32u;
constexpr QualityThreshold kQuality[] = {
    {DenoiseSignal::Shadow, true, 30.0, 0.08},     {DenoiseSignal::Shadow, false, 60.0, 0.05},
    {DenoiseSignal::Reflection, true, 60.0, 0.09}, {DenoiseSignal::Reflection, false, 60.0, 0.08},
    {DenoiseSignal::Gi, true, 60.0, 0.10},         {DenoiseSignal::Gi, false, 60.0, 0.09},
};

// --- layout ---------------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define DN_FIELD(f) Field{#f, offsetof(DenoiseFrameConstants, f)}
const Field kFields[] = {
    DN_FIELD(signal), DN_FIELD(motion), DN_FIELD(depth), DN_FIELD(normal), DN_FIELD(gradientIn), DN_FIELD(guideCur),
    DN_FIELD(guidePrev), DN_FIELD(gradZ), DN_FIELD(histPrev), DN_FIELD(histCur), DN_FIELD(momPrev), DN_FIELD(momCur),
    DN_FIELD(accum), DN_FIELD(lambda), Field{"output_", offsetof(DenoiseFrameConstants, output)}, DN_FIELD(reserved0),
    DN_FIELD(width), DN_FIELD(height), DN_FIELD(strataW), DN_FIELD(strataH), DN_FIELD(normalImage),
    DN_FIELD(outputImage), DN_FIELD(flags), DN_FIELD(signalStride), DN_FIELD(alphaColor), DN_FIELD(alphaMoments),
    DN_FIELD(maxHistory), DN_FIELD(reprojDepth), DN_FIELD(reprojNormal), DN_FIELD(minReprojWeight),
    DN_FIELD(varianceHistory), DN_FIELD(varianceBoost), DN_FIELD(sigmaDepth), DN_FIELD(sigmaLuminance),
    DN_FIELD(sigmaVarianceLum), DN_FIELD(depthEpsilon), DN_FIELD(sigmaNormal), DN_FIELD(atrousIterations),
    DN_FIELD(historyTap), DN_FIELD(gradientIterations), DN_FIELD(gradientScale), DN_FIELD(gradientEpsilon),
    DN_FIELD(reserved1), DN_FIELD(reserved2),
};
#undef DN_FIELD

/// Parses `struct <name> { ... };` of a shader source: (name, scalar-layout offset) per field.
bool parseShaderStruct(const std::string& path, const char* structName, std::vector<std::string>& names,
                       std::vector<size_t>& offsets, size_t& size) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const std::string key = std::string("struct ") + structName + " {";
    const size_t begin = text.find(key);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + key.size(), end - begin - key.size()));
    std::string line;
    size_t offset = 0;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes;
    }
    size = offset;
    return true;
}

void testLayout() {
    expect(sizeof(DenoiseFrameConstants) == 240u, "DenoiseFrameConstants is 240 bytes");
    expect(sizeof(DenoisePush) == 64u, "DenoisePush is 64 bytes (<= 128 guaranteed push-constant bytes)");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_DENOISE_SHADER_DIR) + "/dn_common." + lang;
        std::vector<std::string> names;
        std::vector<size_t> offsets;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, "DnFrame", names, offsets, size);
        expect(parsed, "dn_common struct DnFrame parsed");
        if (!parsed) {
            continue;
        }
        bool same = names.size() == fieldCount && size == sizeof(DenoiseFrameConstants);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(), offsets[i],
                             kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: dn_common.%s DnFrame %zu fields, %zu bytes\n", lang, names.size(), size);
        expect(same, "shader DnFrame == DenoiseFrameConstants (names, order, offsets, size)");
        // Push constants: same 12 fields (GLSL block / Slang struct).
        std::vector<std::string> pn;
        std::vector<size_t> po;
        size_t psize = 0;
        if (std::string(lang) == "slang") {
            expect(parseShaderStruct(path, "DnPush", pn, po, psize) && psize == sizeof(DenoisePush) && pn.size() == 12u,
                   "Slang DnPush == DenoisePush (64 bytes, 12 fields)");
        }
    }

    for (const auto& extent : {std::pair<u32, u32>{1u, 1u}, {61u, 37u}, {64u, 48u}, {1920u, 1080u}}) {
        for (const bool keep : {false, true}) {
            const u32 w = extent.first;
            const u32 h = extent.second;
            const SvgfBufferLayout l = SvgfBufferLayout::compute(w, h, keep);
            const u64 n = static_cast<u64>(w) * h;
            const u64 ns = static_cast<u64>(strata(w)) * strata(h);
            struct Section {
                u64 at;
                u64 bytes;
            };
            std::vector<Section> state = {{l.guide[0], n * 16u}, {l.guide[1], n * 16u}, {l.hist[0], n * 16u},
                                          {l.hist[1], n * 16u},  {l.mom[0], n * 16u},   {l.mom[1], n * 16u}};
            std::vector<Section> work = {{l.gradZ, n * 8u}, {l.accum, n * 16u}, {l.variance, n * 16u}};
            if (keep) {
                for (const u64 a : l.atrous) {
                    work.push_back({a, n * 16u});
                }
                for (const u64 g : l.gradient) {
                    work.push_back({g, ns * 16u});
                }
            } else {
                work.push_back({l.atrous[0], n * 16u});
                work.push_back({l.gradient[0], ns * 16u});
                work.push_back({l.gradient[1], ns * 16u});
            }
            bool ok = l.strataW == strata(w) && l.strataH == strata(h) && l.outputBytes >= n * 16u;
            for (const auto* list : {&state, &work}) {
                const u64 limit = list == &state ? l.stateBytes : l.workBytes;
                for (size_t i = 0; i < list->size(); ++i) {
                    const Section& a = (*list)[i];
                    ok = ok && a.at % 256u == 0u && a.at + a.bytes <= limit;
                    for (size_t j = i + 1u; j < list->size(); ++j) {
                        const Section& b = (*list)[j];
                        ok = ok && (a.at + a.bytes <= b.at || b.at + b.bytes <= a.at);
                    }
                }
            }
            if (!keep) {
                // Ping-pong: variance -> atrous0 -> atrous1 ... alternate; gradient k and k + 1 differ.
                ok = ok && l.variance != l.atrous[0];
                for (u32 k = 0; k + 1u < kDenoiseMaxAtrous; ++k) {
                    ok = ok && l.atrous[k] != l.atrous[k + 1u];
                }
                for (u32 k = 0; k < kDenoiseMaxGradientIterations; ++k) {
                    ok = ok && l.gradient[k] != l.gradient[k + 1u];
                }
            }
            expect(ok, "SvgfBufferLayout sections aligned, disjoint and in bounds");
        }
    }
    std::printf("layout: 1920 x 1080 state %.1f MiB, work %.1f MiB, output %.1f MiB\n",
                static_cast<f64>(SvgfBufferLayout::compute(1920, 1080, false).stateBytes) / 1048576.0,
                static_cast<f64>(SvgfBufferLayout::compute(1920, 1080, false).workBytes) / 1048576.0,
                static_cast<f64>(SvgfBufferLayout::compute(1920, 1080, false).outputBytes) / 1048576.0);
}

// --- helpers --------------------------------------------------------------------------------------------
SvgfReferenceInputs inputsOf(const SyntheticFrame& f, bool gradients) {
    SvgfReferenceInputs in{};
    in.signal = f.signal.data();
    in.motion = f.motion.data();
    in.depth = f.depth.data();
    in.normals = f.normals.data();
    in.gradient = gradients ? f.gradient.data() : nullptr;
    return in;
}

bool sameBits(const std::vector<F4>& a, const std::vector<F4>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(F4)) == 0;
}

// --- backends -------------------------------------------------------------------------------------------
void testBackends() {
    auto& scheduler = jobs::JobScheduler::instance();
    SyntheticDesc d{};
    d.width = 61;
    d.height = 37;
    d.signal = DenoiseSignal::Gi;
    d.lightStepFrame = 4;
    SvgfSettings s = svgf_preset(DenoiseSignal::Gi);
    s.gradients = true;
    SvgfReference ref;
    ref.init(d.width, d.height, s);
    SyntheticFrame frame;
    std::vector<std::vector<F4>> outputs;
    kernel::reset_kernel_stats();
    scheduler.shutdown();
    for (u32 f = 0; f < 6u; ++f) {
        synthetic_frame(d, f, 7u, frame);
        expect(ref.runFrame(inputsOf(frame, true), kernel::Backend::CpuReference), "CpuReference frame");
        outputs.push_back(ref.output());
        outputs.push_back(ref.history(ref.parity()));
        outputs.push_back(ref.moments(ref.parity()));
    }
    kernel::KernelStats st{};
    expect(kernel::find_kernel_stats(svgf_kernel::kNameAtrous, st) && st.launches == 6u * 5u &&
               st.items == 6u * 5u * d.width * d.height,
           "kernel stats under \"denoise_atrous\" (6 frames x 5 iterations, one item per pixel)");
    expect(kernel::find_kernel_stats(svgf_kernel::kNameGradientAtrous, st) && st.launches == 6u * s.gradientIterations,
           "kernel stats under \"denoise_gradient_atrous\"");
    for (const u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        SvgfReference par;
        par.init(d.width, d.height, s);
        bool same = true;
        for (u32 f = 0; f < 6u; ++f) {
            synthetic_frame(d, f, 7u, frame);
            expect(par.runFrame(inputsOf(frame, true), kernel::Backend::CpuParallel), "CpuParallel frame");
            same = same && sameBits(par.output(), outputs[f * 3u]) && sameBits(par.history(par.parity()), outputs[f * 3u + 1u]) &&
                   sameBits(par.moments(par.parity()), outputs[f * 3u + 2u]);
        }
        std::printf("backends: CpuReference == CpuParallel bit for bit (6 frames, A-SVGF, %u workers): %s\n", workers,
                    same ? "yes" : "NO");
        expect(same, "CpuReference == CpuParallel bit for bit");
    }
    scheduler.shutdown();
}

// --- estimator ------------------------------------------------------------------------------------------
void testEstimator() {
    for (const DenoiseSignal sig : {DenoiseSignal::Shadow, DenoiseSignal::Reflection, DenoiseSignal::Gi}) {
        SyntheticDesc d{};
        d.signal = sig;
        const usize n = static_cast<usize>(d.width) * d.height;
        const u32 stride = signal_stride(sig);
        constexpr u32 kN = 2048;
        std::vector<f64> sum(n, 0.0);
        SyntheticFrame f;
        for (u32 r = 0; r < kN; ++r) {
            synthetic_frame(d, 5u, 100u + r, f);
            for (usize i = 0; i < n; ++i) {
                sum[i] += static_cast<f64>(synthetic_luminance(&f.signal[i * stride], stride));
            }
        }
        u32 outliers = 0;
        f64 worst = 0.0;
        for (usize i = 0; i < n; ++i) {
            const f64 t = static_cast<f64>(synthetic_luminance(&f.truth[i * stride], stride));
            // Per-sample variance of the estimator: Bernoulli t(1 - t), uniform x 2r: t^2 / 3, exponential: t^2.
            const f64 var = sig == DenoiseSignal::Shadow ? t * (1.0 - t) : (sig == DenoiseSignal::Reflection ? t * t / 3.0 : t * t);
            const f64 sigma = std::sqrt(var / kN);
            const f64 err = std::fabs(sum[i] / kN - t);
            worst = std::max(worst, sigma > 0.0 ? err / sigma : (err > 0.0 ? 1e9 : 0.0));
            outliers += err > 5.0 * sigma + 1e-6 ? 1u : 0u;
        }
        std::printf("estimator %-10s: mean of %u samples within 5 sigma of the converged value on %zu / %zu pixels (worst %.2f sigma)\n",
                    signalName(sig), kN, n - outliers, n, worst);
        expect(outliers == 0u, "one-sample estimators are unbiased");
    }
    // Gradient samples see lighting changes only.
    SyntheticDesc d{};
    d.signal = DenoiseSignal::Gi;
    d.objectSpeedPx = -d.panPx; // the object (and its floor shadow) static in the world: the lighting is static
    SyntheticFrame f;
    synthetic_frame(d, 5u, 3u, f);
    u32 valid = 0;
    u32 equal = 0;
    for (const F4& g : f.gradient) {
        valid += g.z > 0.f ? 1u : 0u;
        equal += g.z > 0.f && g.x == g.y ? 1u : 0u;
    }
    d.lightStepFrame = 5u;
    synthetic_frame(d, 5u, 3u, f);
    u32 scaled = 0;
    u32 validStep = 0;
    for (const F4& g : f.gradient) {
        if (g.z > 0.f) {
            ++validStep;
            scaled += std::fabs(g.x - 0.25f * g.y) <= 1e-6f * std::fabs(g.y) + 1e-7f ? 1u : 0u;
        }
    }
    std::printf("estimator gradients: %zu strata, %u valid; static lighting cur == prev on %u; light x 0.25: cur == 0.25 prev on %u / %u\n",
                f.gradient.size(), valid, equal, scaled, validStep);
    expect(valid > f.gradient.size() / 2u && equal == valid, "static lighting: gradient samples cur == prev");
    expect(validStep == valid && scaled == validStep, "light step: gradient samples cur == 0.25 x prev");
}

// --- disocclusion ---------------------------------------------------------------------------------------
void testDisocclusion() {
    SyntheticDesc d{};
    d.signal = DenoiseSignal::Gi;
    d.objectSpeedPx = 2.25f;
    const usize n = static_cast<usize>(d.width) * d.height;
    for (const bool control : {false, true}) {
        SvgfSettings s = svgf_preset(DenoiseSignal::Gi);
        if (control) {
            s.reprojDepth = 1e9f;
            s.reprojNormal = -2.f;
        }
        SvgfReference ref;
        ref.init(d.width, d.height, s);
        SyntheticFrame prev, cur;
        std::vector<u8> disocc, stable;
        u32 disTotal = 0, disRestart = 0, disExact = 0, stableTotal = 0, stableKept = 0, sky = 0, skyPass = 0;
        bool frame0 = true;
        bool resetOk = true;
        for (u32 f = 0; f < 12u; ++f) {
            synthetic_frame(d, f, 11u, cur);
            if (f == 8u) {
                ref.reset();
            }
            ref.runFrame(inputsOf(cur, false), kernel::Backend::CpuParallel);
            const std::vector<F4>& mom = ref.moments(ref.parity());
            const std::vector<F4>& acc = ref.accum();
            if (f == 0u || f == 8u) {
                for (usize i = 0; i < n; ++i) {
                    if (cur.surface[i] != kSurfSky && mom[i].z != 1.f) {
                        (f == 0u ? frame0 : resetOk) = false;
                    }
                }
            } else {
                synthetic_disocclusion(prev, cur, disocc, stable);
                for (usize i = 0; i < n; ++i) {
                    if (disocc[i] != 0u) {
                        ++disTotal;
                        disRestart += mom[i].z == 1.f ? 1u : 0u;
                        disExact += acc[i].x == cur.signal[i * 4u] && acc[i].y == cur.signal[i * 4u + 1u] &&
                                            acc[i].z == cur.signal[i * 4u + 2u]
                                        ? 1u
                                        : 0u;
                    }
                    if (stable[i] != 0u) {
                        ++stableTotal;
                        stableKept += mom[i].z > 1.f ? 1u : 0u;
                    }
                }
            }
            for (usize i = 0; i < n; ++i) {
                if (cur.surface[i] == kSurfSky) {
                    ++sky;
                    const F4& o = ref.output()[i];
                    skyPass += o.x == cur.signal[i * 4u] && o.y == cur.signal[i * 4u + 1u] && o.z == cur.signal[i * 4u + 2u] ? 1u : 0u;
                }
            }
            std::swap(prev, cur);
        }
        if (!control) {
            std::printf("disocclusion: %u disoccluded pixels over 11 frames (object at 2.25 px / frame, pan 0.37 px): %u restart "
                        "(length 1), %u temporal result == new sample; %u / %u stable pixels keep history; frame 0 / reset "
                        "restart every pixel: %s / %s; sky pass-through %u / %u\n",
                        disTotal, disRestart, disExact, stableKept, stableTotal, frame0 ? "yes" : "NO", resetOk ? "yes" : "NO",
                        skyPass, sky);
            expect(disTotal >= 100u, "the sequence disoccludes pixels every frame");
            expect(disRestart == disTotal && disExact == disTotal, "every disoccluded pixel rejects its history");
            expect(stableKept == stableTotal && stableTotal > 1000u, "stable pixels keep their history");
            expect(frame0 && resetOk, "frame 0 and reset() start every pixel over");
            expect(skyPass == sky && sky > 0u, "sky pixels pass the signal through");
        } else {
            std::printf("disocclusion control (depth / normal tests off): %u of %u disoccluded pixels keep wrong history\n",
                        disTotal - disRestart, disTotal);
            expect(disTotal - disRestart >= disTotal * 9u / 10u, "control: without the tests the history is not rejected");
        }
    }
}

// --- quality --------------------------------------------------------------------------------------------
struct EnsembleResult {
    QualityReport all;
    QualityReport stable;
};

EnsembleResult runEnsemble(const SvgfSettings& s, const SyntheticDesc& d, u32 frames, u32 realisations) {
    const usize n = static_cast<usize>(d.width) * d.height;
    LuminanceEnsemble in, out;
    in.init(n);
    out.init(n);
    SyntheticFrame f, prev;
    std::vector<u8> surface, disocc, stable;
    for (u32 r = 0; r < realisations; ++r) {
        SvgfReference ref;
        ref.init(d.width, d.height, s);
        for (u32 t = 0; t < frames; ++t) {
            std::swap(prev, f);
            synthetic_frame(d, t, 1000u + r * 7919u, f);
            ref.runFrame(inputsOf(f, s.gradients), kernel::Backend::CpuParallel);
        }
        in.add(f.signal.data(), f.stride);
        out.add(ref.output(), d.signal == DenoiseSignal::Shadow);
    }
    surface.assign(n, 0u);
    for (usize i = 0; i < n; ++i) {
        surface[i] = f.surface[i] != kSurfSky ? 1u : 0u;
    }
    synthetic_disocclusion(prev, f, disocc, stable);
    EnsembleResult e{};
    e.all = evaluate_quality(in, out, f.truth, f.stride, surface);
    e.stable = evaluate_quality(in, out, f.truth, f.stride, stable);
    return e;
}

void testQuality() {
    auto& scheduler = jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4);
    for (const QualityThreshold& q : kQuality) {
        SyntheticDesc d{};
        d.width = kQualityWidth;
        d.height = kQualityHeight;
        d.signal = q.signal;
        if (!q.dynamic) {
            d.objectSpeedPx = -d.panPx;
        }
        SvgfSettings s = svgf_preset(q.signal);
        s.gradients = q.dynamic;
        const EnsembleResult e = runEnsemble(s, d, kQualityFrames, kQualityRealisations);
        std::printf("quality %-10s %-7s (%s): VRF %.1f (input var %.4f -> %.6f), RMS bias %.2f%% of mean %.3f (input "
                    "%.2f%%), RMSE %.4f -> %.4f, %u px; stable px: VRF %.1f, RMS bias %.2f%%  [VRF >= %.0f, bias <= %.0f%%]\n",
                    signalName(q.signal), q.dynamic ? "dynamic" : "static", q.dynamic ? "A-SVGF" : "SVGF", e.all.vrf,
                    e.all.inputVariance, e.all.outputVariance, 100.0 * e.all.relRmsBias, e.all.meanTruth,
                    100.0 * e.all.inputRelRmsBias, e.all.inputRmse, e.all.outputRmse, e.all.pixels, e.stable.vrf,
                    100.0 * e.stable.relRmsBias, q.minVrf, 100.0 * q.maxRelBias);
        expect(e.all.vrf >= q.minVrf, "variance reduction factor meets the WP-6.4 threshold");
        expect(e.all.relRmsBias <= q.maxRelBias, "bias against the converged reference within the WP-6.4 threshold");
        // Metric sanity: the estimators are unbiased, so the input's noise-corrected RMS bias is estimation noise
        // only (largest for the heavy-tailed GI exponential: ~1-2% with 32 realisations).
        expect(e.all.inputRelRmsBias <= 0.03, "the noisy input measures unbiased (metric sanity)");
    }
    scheduler.shutdown();
}

// --- A-SVGF ---------------------------------------------------------------------------------------------
struct LagResult {
    f64 relErrAfter = 0.0; ///< mean |out - truth| / mean truth over the frames after the change
    f64 rmseBefore = 0.0;  ///< RMSE on the frame before the change
};

LagResult runLag(bool gradients, u32 stepFrame, u32 realisations) {
    SyntheticDesc d{};
    d.signal = DenoiseSignal::Gi;
    d.lightStepFrame = stepFrame;
    d.lightStepScale = 0.25f;
    SvgfSettings s = svgf_preset(DenoiseSignal::Gi);
    s.gradients = gradients;
    const usize n = static_cast<usize>(d.width) * d.height;
    LagResult res{};
    f64 errSum = 0.0;
    f64 truthSum = 0.0;
    f64 sq = 0.0;
    u64 count = 0;
    SyntheticFrame f;
    for (u32 r = 0; r < realisations; ++r) {
        SvgfReference ref;
        ref.init(d.width, d.height, s);
        for (u32 t = 0; t < stepFrame + 4u; ++t) {
            synthetic_frame(d, t, 500u + r * 31u, f);
            ref.runFrame(inputsOf(f, gradients), kernel::Backend::CpuParallel);
            const bool after = t >= stepFrame;
            const bool before = t + 1u == stepFrame;
            for (usize i = 0; i < n && (after || before); ++i) {
                if (f.surface[i] == kSurfSky) {
                    continue;
                }
                const F4& o = ref.output()[i];
                const f64 lo = 0.2126 * o.x + 0.7152 * o.y + 0.0722 * o.z;
                const f64 lt = synthetic_luminance(&f.truth[i * 4u], 4u);
                if (after) {
                    errSum += std::fabs(lo - lt);
                    truthSum += lt;
                } else {
                    sq += (lo - lt) * (lo - lt);
                    ++count;
                }
            }
        }
    }
    res.relErrAfter = truthSum > 0.0 ? errSum / truthSum : 0.0;
    res.rmseBefore = count > 0u ? std::sqrt(sq / static_cast<f64>(count)) : 0.0;
    return res;
}

void testAsvgf() {
    auto& scheduler = jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4);
    const LagResult svgf = runLag(false, 10u, 6u);
    const LagResult asvgf = runLag(true, 10u, 6u);
    std::printf("asvgf: light x 0.25 at frame 10, mean relative error over frames 10..13: SVGF %.3f, A-SVGF %.3f (%.2fx); "
                "RMSE at frame 9: SVGF %.4f, A-SVGF %.4f\n",
                svgf.relErrAfter, asvgf.relErrAfter, asvgf.relErrAfter / std::max(svgf.relErrAfter, 1e-12), svgf.rmseBefore,
                asvgf.rmseBefore);
    expect(asvgf.relErrAfter <= 0.5 * svgf.relErrAfter, "A-SVGF halves the temporal lag error after a lighting change");
    expect(asvgf.rmseBefore <= 1.25 * svgf.rmseBefore, "A-SVGF keeps the steady-state quality");
    scheduler.shutdown();
}

// --- api ------------------------------------------------------------------------------------------------
void testApi() {
    const DenoiseCapabilities caps = queryDenoiseCapabilities(nullptr);
    expect(!caps.denoise && caps.reason != nullptr, "no device: not usable, with a reason");
    SvgfDenoiser d;
    expect(!d.init(SvgfDenoiserDesc{}) && !d.valid(), "init without a device fails");
    DenoiseFrameDesc frame{};
    frame.width = 4;
    frame.height = 4;
    frame.signal = frame.motion = frame.depth = frame.normal = 1u;
    expect(!d.beginFrame(1u, frame), "beginFrame on an invalid denoiser fails");
    rg::Graph graph;
    const DenoiseGraphRefs refs = d.importInto(graph);
    expect(!refs.state.valid() && !refs.work.valid() && !refs.output.valid() && !refs.outputImage.valid(),
           "no refs from an invalid denoiser");
    d.addPasses(graph, refs, DenoiseGraphInputs{});
    expect(d.stats().passes == 0u, "no passes from an invalid denoiser");

    SvgfSettings s = svgf_preset(DenoiseSignal::Shadow);
    s.atrousIterations = 0u;
    s.historyTap = 7u;
    s.gradientIterations = 9u;
    s.gradients = true;
    DenoiseFrameConstants c = resolve_constants(s, 10u, 7u, true, true);
    expect(c.atrousIterations == 1u && c.historyTap == 0u && c.gradientIterations == kDenoiseMaxGradientIterations,
           "resolve_constants clamps the iteration counts");
    expect(c.signalStride == 1u && (c.flags & kDenoiseFlagScalar) != 0u && (c.flags & kDenoiseFlagHistory) != 0u &&
               (c.flags & kDenoiseFlagGradients) != 0u && (c.flags & kDenoiseFlagNormalOct) != 0u && c.strataW == 4u &&
               c.strataH == 3u,
           "resolve_constants flags / stride / strata");
    s.atrousIterations = 9u;
    s.historyTap = 2u;
    c = resolve_constants(s, 10u, 7u, false, false);
    expect(c.atrousIterations == kDenoiseMaxAtrous && c.historyTap == 2u && (c.flags & kDenoiseFlagHistory) == 0u,
           "a-trous count clamped to 5, history tap kept");
    const SvgfSettings r = svgf_preset(DenoiseSignal::Reflection);
    const SvgfSettings g = svgf_preset(DenoiseSignal::Gi);
    expect(signal_stride(r.signal) == 4u && signal_stride(g.signal) == 4u && r.maxHistory < g.maxHistory,
           "presets: RGB variants, reflections keep a shorter history");
    std::printf("api: without a device init fails (%s), no refs, no passes; constants clamp\n", caps.reason);
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
    if (all || suite == "backends") {
        testBackends();
        ran = true;
    }
    if (all || suite == "estimator") {
        testEstimator();
        ran = true;
    }
    if (all || suite == "disocclusion") {
        testDisocclusion();
        ran = true;
    }
    if (all || suite == "quality") {
        testQuality();
        ran = true;
    }
    if (all || suite == "asvgf") {
        testAsvgf();
        ran = true;
    }
    if (all || suite == "api") {
        testApi();
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
