// WP-0.7 renderer golden gates on the existing G-buffer raster path (Lavapipe in CI).
//
//   fuse_rp_harness_golden --scene <name>[,<name>...] [--roundtrip] [--tier-cap Tn] [--require-tier Tn]
//                          [--golden-name-suffix S] [--golden-dir D] [--artifact-dir D] [--shader-dir D]
//
// Each scene is rendered by HeadlessFrameRunner (GBufferRasterPass, six MRT attachments + D32, all
// read back), resolved on the CPU to a 128x128 sRGB image and compared with
// Tests/golden/renderer/<scene><suffix>.png (FLIP/SSIM when the image-metrics library is linked,
// else PSNR; plus a zero-pixel budget). Validation + synchronization validation are on; any
// validation error fails the run.
//
// --roundtrip is the WP-0.7 exit test:
//   1. sync validation is live (negative control: an unsynchronised WAW is reported);
//   2. gbuffer_quads through the *stock* gbuffer.vert/frag: coverage and depth match the scene
//      layout exactly (the fuse_b5_rhi_gbuffer_pass geometry);
//   3. rendering twice is bit-identical; the PNG encode/decode of the frame is lossless;
//   4. the golden check passes;
//   5. a deliberate one-pixel change of the frame fails the gate (programmatically, with the diff
//      artefact written), while the unmodified frame still passes.
//
// Exit 77 (ctest SKIP_RETURN_CODE) without the Vulkan backend, the SPIR-V or a device, or when the
// device tier is below --require-tier.
#include "frame_runner.hpp"
#include "golden.hpp"
#include "image_io.hpp"
#include "scene.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/golden_px.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef FUSE_RP_GOLDEN_DIR
#define FUSE_RP_GOLDEN_DIR "Tests/golden/renderer"
#endif
#ifndef FUSE_RP_HARNESS_ARTIFACT_DIR
#define FUSE_RP_HARNESS_ARTIFACT_DIR "rp_harness_artifacts"
#endif
#ifndef FUSE_RP_HARNESS_SHADER_DIR
#define FUSE_RP_HARNESS_SHADER_DIR "shaders"
#endif
#ifndef FUSE_RP_STOCK_SHADER_DIR
#define FUSE_RP_STOCK_SHADER_DIR "shaders"
#endif

namespace {

using namespace fuse::renderer::harness;
using fuse::f32;
using fuse::f64;
using fuse::s64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

struct Args {
    std::vector<std::string> scenes;
    bool roundtrip = false;
    int tierCap = -1;
    int requireTier = -1;
    std::string suffix;
    std::string goldenDir = FUSE_RP_GOLDEN_DIR;
    std::string artifactDir = FUSE_RP_HARNESS_ARTIFACT_DIR;
    std::string shaderDir = FUSE_RP_STOCK_SHADER_DIR;
    std::string harnessShaderDir = FUSE_RP_HARNESS_SHADER_DIR;
};

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--scene" && value(v)) {
            usize start = 0;
            while (start <= v.size()) {
                const usize comma = v.find(',', start);
                const std::string name = v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (name == "all") {
                    for (const std::string& s : sceneNames()) {
                        args.scenes.push_back(s);
                    }
                } else if (!name.empty()) {
                    args.scenes.push_back(name);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1u;
            }
        } else if (a == "--roundtrip") {
            args.roundtrip = true;
        } else if (a == "--tier-cap" && value(v)) {
            args.tierCap = parseTier(v);
            if (args.tierCap < 0) {
                return false;
            }
        } else if (a == "--require-tier" && value(v)) {
            args.requireTier = parseTier(v);
            if (args.requireTier < 0) {
                return false;
            }
        } else if (a == "--golden-name-suffix" && value(v)) {
            args.suffix = v;
        } else if (a == "--golden-dir" && value(v)) {
            args.goldenDir = v;
        } else if (a == "--artifact-dir" && value(v)) {
            args.artifactDir = v;
        } else if (a == "--shader-dir" && value(v)) {
            args.shaderDir = v;
        } else if (a == "--harness-shader-dir" && value(v)) {
            args.harnessShaderDir = v;
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str());
            return false;
        }
    }
    if (args.roundtrip && args.scenes.empty()) {
        args.scenes.push_back("gbuffer_quads");
    }
    return !args.scenes.empty();
}

/// Per-scene gate. Lavapipe renders are deterministic, so every scene starts with a zero-pixel
/// budget; relax per scene (never globally) if a hardware runner needs it. On other devices the
/// thresholds stay the same and only the coverage-ambiguous pixels are exempt (main()): measured
/// on an RTX 3090, instance_grid_1k / _100k differ from the goldens on 7 / 59 pixels, every one a
/// centre within a sub-pixel step of a visible edge between different surfaces (box faces meeting on
/// a pixel row; sub-pixel cubes), and every gated pixel is exact (write_gbuffer rounds its UNORM8 /
/// f16 attachment values itself and encodes normals with correctly rounded operations, so no
/// device-dependent conversion or builtin precision reaches the G-buffer).
GoldenSpec specFor(const std::string&) {
    GoldenSpec spec{};
    spec.metric = GoldenMetric::Flip;
    spec.threshold = 0.01;       // mean FLIP
    spec.psnrFallbackDb = 40.0;  // until the image-metrics library is linked
    spec.pixelTolerance = 2;
    spec.maxDifferingPixels = 0;
    return spec;
}

/// The goldens are Lavapipe renders, and Lavapipe is deterministic: on it every pixel is gated.
bool isGoldenRasteriser(const std::string& deviceName) {
    return deviceName.find("llvmpipe") != std::string::npos;
}

/// Distance from (px, py) to the segment a-b.
f64 segmentDistance(f64 px, f64 py, f64 ax, f64 ay, f64 bx, f64 by) {
    const f64 dx = bx - ax, dy = by - ay;
    const f64 len2 = dx * dx + dy * dy;
    f64 t = len2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const f64 qx = ax + t * dx - px, qy = ay + t * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}

/// One triangle's sample at a pixel centre: its plane depth there, the depth slack of vertex snapping
/// ((|dz/dx| + |dz/dy|) * band) and the batch (= the G-buffer content it writes: material + normal).
struct CentreSample {
    usize pixel;
    f64 depth;
    f64 slack;
    u32 batch;
};

struct AmbiguityMasks {
    std::vector<u8> nearEdge;  ///< centre within `band` of any drawn triangle's edge (the previous rule)
    std::vector<u8> ambiguous; ///< ... and the device's coverage / depth choice can change the pixel
};

/// Pixels whose content the rasteriser may legitimately decide differently from the goldens' (the scene
/// is projected on the CPU, exactly as the harness renders it). Coverage of a centre within `band`
/// pixels of a triangle edge depends on the device's vertex snapping (subPixelPrecisionBits, its
/// rounding) and tie rules, and the depth test between two surfaces at the same depth depends on the
/// snapped planes; Vulkan leaves both implementation-defined. Such a centre only matters when the
/// alternative outcome writes different G-buffer content, so a pixel is exempt when
///   - a triangle whose edge passes within `band` of the centre (it may or may not cover it) would be
///     in front of, or within snapping slack of, the surface that robustly covers the centre (the
///     nearest triangle containing it farther than `band` from its edges; the clear colour if none),
///     and belongs to another batch; or
///   - two triangles of different batches robustly cover the centre within snapping slack of the
///     nearest depth (an intersection line).
/// Hidden edges (behind the visible surface) and edges between triangles of one batch (a quad's
/// diagonal, a mesh's flat faces) no longer exempt their pixels.
AmbiguityMasks coverageAmbiguousPixels(const Scene& scene, u32 width, u32 height, f64 band) {
    const usize pixels = static_cast<usize>(width) * height;
    AmbiguityMasks masks{std::vector<u8>(pixels, 0u), std::vector<u8>(pixels, 0u)};
    const ProjectedScene ps = projectScene(scene, static_cast<f32>(width) / static_cast<f32>(height));
    constexpr f64 kDepthEps = 1e-6; // D32 interpolation rounding
    std::vector<CentreSample> robust;
    std::vector<CentreSample> edge;
    for (usize batch = 0; batch < ps.firstVertex.size(); ++batch) {
        const usize first = ps.firstVertex[batch];
        const usize count = ps.vertexCount[batch];
        for (usize t = first; t + 3u <= first + count; t += 3u) {
            f64 x[3], y[3], z[3];
            for (usize k = 0; k < 3u; ++k) {
                const f32* v = ps.vertices.data() + (t + k) * 3u;
                x[k] = (static_cast<f64>(v[0]) * 0.5 + 0.5) * width;
                y[k] = (static_cast<f64>(v[1]) * 0.5 + 0.5) * height;
                z[k] = static_cast<f64>(v[2]);
            }
            const f64 area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            if (area == 0.0) {
                continue; // never rasterised
            }
            // Plane z = z0 + dzdx (x - x0) + dzdy (y - y0).
            const f64 dzdx = ((z[1] - z[0]) * (y[2] - y[0]) - (z[2] - z[0]) * (y[1] - y[0])) / area;
            const f64 dzdy = ((z[2] - z[0]) * (x[1] - x[0]) - (z[1] - z[0]) * (x[2] - x[0])) / area;
            const f64 slack = (std::fabs(dzdx) + std::fabs(dzdy)) * band;
            const f64 x0 = std::min({x[0], x[1], x[2]}) - band, x1 = std::max({x[0], x[1], x[2]}) + band;
            const f64 y0 = std::min({y[0], y[1], y[2]}) - band, y1 = std::max({y[0], y[1], y[2]}) + band;
            // Centres px + 0.5 in [x0, x1].
            const s64 pxLo = std::max<s64>(0, static_cast<s64>(std::ceil(x0 - 0.5)));
            const s64 pxHi = std::min<s64>(static_cast<s64>(width) - 1, static_cast<s64>(std::floor(x1 - 0.5)));
            const s64 pyLo = std::max<s64>(0, static_cast<s64>(std::ceil(y0 - 0.5)));
            const s64 pyHi = std::min<s64>(static_cast<s64>(height) - 1, static_cast<s64>(std::floor(y1 - 0.5)));
            for (s64 py = pyLo; py <= pyHi; ++py) {
                for (s64 px = pxLo; px <= pxHi; ++px) {
                    const f64 cx = static_cast<f64>(px) + 0.5, cy = static_cast<f64>(py) + 0.5;
                    const usize i = static_cast<usize>(py) * width + static_cast<usize>(px);
                    bool nearEdge = false;
                    bool inside = true;
                    for (u32 e = 0; e < 3u; ++e) {
                        const u32 f = (e + 1u) % 3u;
                        nearEdge = nearEdge || segmentDistance(cx, cy, x[e], y[e], x[f], y[f]) <= band;
                        const f64 side = (x[f] - x[e]) * (cy - y[e]) - (y[f] - y[e]) * (cx - x[e]);
                        inside = inside && (area > 0.0 ? side >= 0.0 : side <= 0.0);
                    }
                    if (!nearEdge && !inside) {
                        continue;
                    }
                    const CentreSample s{i, z[0] + dzdx * (cx - x[0]) + dzdy * (cy - y[0]), slack,
                                         static_cast<u32>(batch)};
                    if (nearEdge) {
                        masks.nearEdge[i] = 1u;
                        edge.push_back(s);
                    } else {
                        robust.push_back(s);
                    }
                }
            }
        }
    }
    // The robust surface per centre: the nearest robustly covering triangle (LESS: the first drawn wins
    // a tie; `robust` is in draw order).
    constexpr u32 kNone = 0xFFFFFFFFu;
    std::vector<f64> winDepth(pixels, 1.0);
    std::vector<f64> winSlack(pixels, 0.0);
    std::vector<u32> winBatch(pixels, kNone);
    for (const CentreSample& s : robust) {
        if (s.depth < winDepth[s.pixel] && s.depth >= 0.0) {
            winDepth[s.pixel] = s.depth;
            winSlack[s.pixel] = s.slack;
            winBatch[s.pixel] = s.batch;
        }
    }
    auto contends = [&](const CentreSample& s) {
        if (s.depth < 0.0 || s.depth > 1.0) {
            return false; // clipped away there
        }
        const usize i = s.pixel;
        return s.batch != winBatch[i] && s.depth <= winDepth[i] + winSlack[i] + s.slack + kDepthEps;
    };
    for (const CentreSample& s : edge) {
        if (contends(s)) {
            masks.ambiguous[s.pixel] = 1u;
        }
    }
    for (const CentreSample& s : robust) {
        if (winBatch[s.pixel] != kNone && contends(s)) {
            masks.ambiguous[s.pixel] = 1u;
        }
    }
    return masks;
}

RasterShaders shadersFor(const Scene& scene) {
    for (const Batch& b : scene.batches) {
        if (!b.screenSpace) {
            return RasterShaders::Projected;
        }
    }
    return RasterShaders::Stock;
}

/// gbuffer_quads layout check (fuse_b5_rhi_gbuffer_pass geometry, stock shaders): near quad
/// [-0.5,1]^2 at depth 0.25, far quad [-1,0.5]^2 at 0.75, drawn near-first.
void checkQuadLayout(const FrameCapture& c) {
    u32 nearCount = 0, farCount = 0, clearCount = 0;
    for (const float d : c.depth) {
        nearCount += d == 0.25f ? 1u : 0u;
        farCount += d == 0.75f ? 1u : 0u;
        clearCount += d == 0.f ? 1u : 0u;
    }
    const u32 w = c.width, h = c.height;
    std::printf("gbuffer_quads depth coverage: near %u, far %u, clear %u\n", nearCount, farCount, clearCount);
    expect(nearCount == (w * 3u / 4u) * (h * 3u / 4u), "stock path: near quad covers exactly 3/4 x 3/4");
    expect(farCount == (w * 3u / 4u) * (h * 3u / 4u) - (w / 2u) * (h / 2u),
           "stock path: far quad visible only outside the overlap (depth test)");
    expect(nearCount + farCount + clearCount == w * h, "stock path: every depth texel is near, far or clear");
    const usize overlap = static_cast<usize>(h / 2u) * w + w / 2u;
    expect(std::fabs(c.albedo[overlap].x - 0.9f) < 1.f / 255.f + 1e-4f, "stock path: overlap keeps the near albedo");
}

/// CPU gate. No heap allocation. A failure returns non-zero before any later skip.
int testGoldenPxOk() {
    if (!fuse::renderer::golden_px_ok(1)) {
        std::fprintf(stderr, "FAIL: golden_px_ok(1)\n");
        return EXIT_FAILURE;
    }
    if (!fuse::renderer::golden_px_ok(4096)) {
        std::fprintf(stderr, "FAIL: golden_px_ok(4096)\n");
        return EXIT_FAILURE;
    }
    if (fuse::renderer::golden_px_ok(0)) {
        std::fprintf(stderr, "FAIL: golden_px_ok(0)\n");
        return EXIT_FAILURE;
    }
    if (fuse::renderer::golden_px_ok(4097)) {
        std::fprintf(stderr, "FAIL: golden_px_ok(4097)\n");
        return EXIT_FAILURE;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (const int goldenPxRc = testGoldenPxOk()) {
        return goldenPxRc;
    }
    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::fprintf(stderr, "usage: %s --scene <name|all>[,...] [--roundtrip] [--tier-cap Tn] [--require-tier Tn]\n",
                     argv[0]);
        return EXIT_FAILURE;
    }
    fuse::core::initialize();
    int exitCode = EXIT_SUCCESS;
    {
        HarnessOptions options{};
        options.shaderDir = args.shaderDir;
        options.harnessShaderDir = args.harnessShaderDir;
        options.tierCap = args.tierCap;
        std::string reason;
        auto runner = HeadlessFrameRunner::create(options, reason);
        if (runner == nullptr) {
            std::printf("SKIP fuse_rp_harness_golden: %s\n", reason.c_str());
            fuse::core::shutdown();
            return kSkip;
        }
        const ValidationReport v0 = runner->validation();
        std::printf("device: %s | tier %s (%s) | validation layer %s, sync validation %s | metrics: %s\n",
                    runner->deviceName().c_str(), ("T" + std::to_string(runner->tier().tier)).c_str(),
                    runner->tier().summary.c_str(), v0.layerRequested ? "on" : "OFF", v0.syncRequested ? "on" : "OFF",
                    goldenQualityMetricsAvailable() ? "FLIP/SSIM" : "PSNR fallback");
        if (args.requireTier >= 0 && static_cast<int>(runner->tier().tier) < args.requireTier) {
            std::printf("SKIP fuse_rp_harness_golden: device tier T%u below required T%d\n", runner->tier().tier,
                        args.requireTier);
            runner.reset();
            fuse::core::shutdown();
            return kSkip;
        }
        if (args.tierCap >= 0) {
            expect(static_cast<int>(runner->tier().tier) <= args.tierCap, "tier cap honoured by the device");
        }

        if (args.roundtrip && v0.syncRequested) {
            const u32 hazards = runner->runSyncHazardControl();
            std::printf("sync validation negative control: %u error(s) for an unsynchronised WAW\n", hazards);
            expect(hazards > 0u, "synchronization validation is live (negative control reports a hazard)");
        } else if (args.roundtrip) {
            std::printf("NOTE: validation layer unavailable or disabled — sync negative control skipped\n");
        }
        runner->resetValidation();

        // Devices other than the goldens' rasteriser: pixels whose coverage is implementation-defined
        // are exempt (coverageAmbiguousPixels). The band is one sub-pixel step of this device plus one
        // of Lavapipe's (8 bits): each rasteriser moved every vertex by at most its step.
        const bool goldenRasteriser = isGoldenRasteriser(runner->deviceName());
        const u32 subPixelBits = std::max(runner->subPixelPrecisionBits(), 4u); // Vulkan minimum 4
        const f64 ambiguityBand = std::ldexp(1.0, -static_cast<int>(subPixelBits)) + std::ldexp(1.0, -8);
        if (!goldenRasteriser) {
            std::printf("not the goldens' rasteriser (Lavapipe): pixel centres within %.5f px of a triangle edge "
                        "(subPixelPrecisionBits %u) are exempt from the golden gate\n",
                        ambiguityBand, subPixelBits);
        }
        GoldenStore store(args.goldenDir, args.artifactDir);
        for (const std::string& name : args.scenes) {
            Scene scene;
            if (!buildSceneByName(name, scene)) {
                expect(false, "unknown scene " + name);
                continue;
            }
            const RasterShaders shaders = shadersFor(scene);
            FrameCapture capture;
            std::string error;
            if (!runner->render(scene, shaders, capture, error)) {
                expect(false, name + ": render failed: " + error);
                continue;
            }
            std::printf("%-20s %s shaders, %u draws, %llu tris, covered %u px, project %.1f ms, frame %.1f ms, "
                        "readback %.1f ms\n",
                        name.c_str(), shaders == RasterShaders::Stock ? "stock" : "projected", capture.draws,
                        static_cast<unsigned long long>(capture.triangles), capture.coveredPixels(), capture.projectMs,
                        capture.gpuFrameMs, capture.readbackMs);
            expect(capture.coveredPixels() > 0u, name + ": frame covers pixels");
            if (name == "gbuffer_quads") {
                checkQuadLayout(capture);
            }
            if (name == "hud_overlay") {
                u32 hud = 0;
                for (const u8 m : capture.hudMask) {
                    hud += m;
                }
                expect(hud > 0u, "hud_overlay: HUD pixels carry the HUD shading model");
            }

            const std::string goldenName = name + args.suffix;
            GoldenSpec spec = specFor(name);
            AmbiguityMasks ambiguity;
            if (!goldenRasteriser) {
                ambiguity = coverageAmbiguousPixels(scene, capture.width, capture.height, ambiguityBand);
                spec.exempt = &ambiguity.ambiguous;
                u32 count = 0;
                u32 nearEdge = 0;
                for (usize i = 0; i < ambiguity.ambiguous.size(); ++i) {
                    count += ambiguity.ambiguous[i];
                    nearEdge += ambiguity.nearEdge[i];
                }
                std::printf("%-20s %u of %u px coverage-ambiguous (%.2f %%; %u near any drawn edge)\n", name.c_str(),
                            count, capture.width * capture.height,
                            100.0 * count / static_cast<f64>(capture.width * capture.height), nearEdge);
            }
            const GoldenResult result = store.check(goldenName, capture.lit, spec);
            expect(result.passed, name + ": golden gate (" + result.message + ")");
            if (!result.passed) {
                ensureDirectory(args.artifactDir);
                writeCaptureExr(args.artifactDir + "/" + goldenName + ".gbuffer.exr", capture);
            }

            if (args.roundtrip) {
                FrameCapture again;
                expect(runner->render(scene, shaders, again, error) && again.lit.pixels == capture.lit.pixels,
                       name + ": second render is bit-identical");
                std::vector<u8> png;
                ImageRgba8 decoded;
                expect(encodePng(capture.lit, png) && decodePng(png.data(), png.size(), decoded) &&
                           decoded.pixels == capture.lit.pixels,
                       name + ": PNG round trip of the frame is lossless");
                std::printf("%s: frame PNG %zu bytes\n", name.c_str(), png.size());

                // Deliberate one-pixel change must fail the gate; checked against the stored golden
                // with update mode forced off (so FUSE_UPDATE_GOLDENS=1 cannot mask it).
                GoldenStore gate(args.goldenDir, args.artifactDir + "/deliberate_failure");
                gate.setUpdateMode(false);
                ImageRgba8 mutated = capture.lit;
                u8* p = mutated.at(mutated.width / 3u, mutated.height / 5u);
                p[0] = static_cast<u8>(p[0] ^ 0x40u);
                const GoldenResult bad = gate.check(goldenName, mutated, specFor(name));
                std::printf("deliberate one-pixel change: %s -> %s\n", bad.message.c_str(),
                            bad.passed ? "PASSED (wrong)" : "rejected");
                expect(!bad.passed && bad.differingPixels == 1u, name + ": a one-pixel change fails the golden gate");
                expect(!bad.diffPath.empty() && std::filesystem::exists(bad.diffPath),
                       name + ": failure writes the diff artefact");
                expect(gate.check(goldenName, capture.lit, specFor(name)).passed,
                       name + ": the unmodified frame passes the same gate");
            }
        }

        const ValidationReport v = runner->validation();
        std::printf("validation: %u error(s), %u warning(s)%s%s\n", v.errors, v.warnings,
                    v.lastError.empty() ? "" : " — last: ", v.lastError.substr(0, 400).c_str());
        expect(v.errors == 0u, "zero validation / sync-validation errors while rendering");
        store.printSummary();
        runner.reset();
    }
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_rp_harness_golden: all checks passed\n");
    } else {
        std::fprintf(stderr, "fuse_rp_harness_golden: %d failure(s)\n", g_failures);
        exitCode = EXIT_FAILURE;
    }
    return exitCode;
}
