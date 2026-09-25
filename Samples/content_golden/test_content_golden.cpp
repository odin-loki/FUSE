// FUSE asset plan W0.8 (docs/plans/FUSE_ASSET_PLAN.md §5.4, §6 Wave 0): golden renders of the content
// reference scenes on Lavapipe, compared with the vendored NVIDIA FLIP.
//
//   fuse_content_golden [--golden-dir D] [--artifact-dir D] [--shader-dir D] [--harness-shader-dir D]
//
// For every reference scene (reference_scenes.hpp) and lighting setup:
//   1. render through the existing G-buffer raster path (WP-0.7 HeadlessFrameRunner, projected
//      vertex stage, 960 x 540, validation + synchronization validation on) and resolve;
//   2. re-render from scratch: the frame is bit-identical and FLIP(first, second) == 0 exactly
//      (the W0.8 exit criterion "deterministic re-render FLIP = 0");
//   3. compare with the committed golden <golden-dir>/<scene>_<setup>.png: mean FLIP <= 0.05 (§5.4
//      pass); bit-exactness is reported. On failure <artifact-dir>/<name>.actual.png and
//      <name>.flip.png (magma heat map) are written;
//   4. FLIP sanity on the real frame (calibrated on Lavapipe, FLIP 1.7, default 67 PPD): one changed
//      pixel gives 0 < FLIP < 1e-4; a +1-code brightness shift stays under the 0.05 gate; a +8-code
//      shift lands in [0.07, 0.3] and another lighting setup's frame in > 0.05 (the gate can fail).
// Zero validation messages are required. FUSE_UPDATE_GOLDENS=1 rewrites the goldens instead of
// comparing (same policy as Tests/golden/renderer). Exit 77 without Vulkan / device / SPIR-V.
#include "flip_metric.hpp"
#include "frame_runner.hpp"
#include "golden.hpp"
#include "image_io.hpp"
#include "reference_scenes.hpp"

#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef FUSE_CONTENT_GOLDEN_DIR
#define FUSE_CONTENT_GOLDEN_DIR "Samples/content_golden/golden"
#endif
#ifndef FUSE_CONTENT_GOLDEN_ARTIFACT_DIR
#define FUSE_CONTENT_GOLDEN_ARTIFACT_DIR "content_golden_artifacts"
#endif
#ifndef FUSE_RP_HARNESS_SHADER_DIR
#define FUSE_RP_HARNESS_SHADER_DIR "shaders"
#endif
#ifndef FUSE_RP_STOCK_SHADER_DIR
#define FUSE_RP_STOCK_SHADER_DIR "shaders"
#endif

namespace {

using namespace fuse::content_golden;
using namespace fuse::renderer::harness;
using fuse::u32;
using fuse::u8;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const std::string& message) {
    std::printf("%s: %s\n", condition ? "ok  " : "FAIL", message.c_str());
    if (!condition) {
        ++g_failures;
    }
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

FlipResult flip(const ImageRgba8& reference, const ImageRgba8& test) {
    return computeFlip(reference.pixels.data(), test.pixels.data(), reference.width, reference.height);
}

void writeArtifacts(const std::string& dir, const std::string& name, const ImageRgba8& actual, const FlipResult& f) {
    ensureDirectory(dir);
    writePng(dir + "/" + name + ".actual.png", actual);
    if (!f.map.empty()) {
        ImageRgba8 heat(actual.width, actual.height);
        heat.pixels = flipHeatmap(f.map, actual.width, actual.height);
        writePng(dir + "/" + name + ".flip.png", heat);
    }
    std::printf("      artefacts: %s/%s.{actual,flip}.png\n", dir.c_str(), name.c_str());
}

} // namespace

int main(int argc, char** argv) {
    std::string goldenDir = FUSE_CONTENT_GOLDEN_DIR;
    std::string artifactDir = FUSE_CONTENT_GOLDEN_ARTIFACT_DIR;
    HarnessOptions options;
    options.width = kGoldenWidth;
    options.height = kGoldenHeight;
    options.shaderDir = FUSE_RP_STOCK_SHADER_DIR;
    options.harnessShaderDir = FUSE_RP_HARNESS_SHADER_DIR;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--golden-dir") goldenDir = argv[++i];
        else if (a == "--artifact-dir") artifactDir = argv[++i];
        else if (a == "--shader-dir") options.shaderDir = argv[++i];
        else if (a == "--harness-shader-dir") options.harnessShaderDir = argv[++i];
    }
    const bool update = goldenUpdateRequestedByEnv();

    fuse::core::initialize();
    std::string reason;
    auto runner = HeadlessFrameRunner::create(options, reason);
    if (!runner) {
        std::printf("SKIP: %s\n", reason.c_str());
        fuse::core::shutdown();
        return kSkip;
    }
    const ValidationReport v0 = runner->validation();
    std::printf("device: %s | validation layer %s, sync validation %s | %s\n", runner->deviceName().c_str(),
                v0.layerRequested ? "on" : "off", v0.syncRequested ? "on" : "off", update ? "UPDATE MODE" : "compare");

    const std::vector<LightingSetup> setups = lightingSetups();
    for (const std::string& sceneName : referenceSceneNames()) {
        const Scene scene = buildMaterialBallGrid();
        expect(scene.name == sceneName, "scene builder matches its name");
        FrameCapture first, second;
        std::string error;
        const bool ok1 = runner->render(scene, RasterShaders::Projected, first, error);
        expect(ok1, sceneName + ": render (" + error + ")");
        const bool ok2 = ok1 && runner->render(scene, RasterShaders::Projected, second, error);
        expect(ok2, sceneName + ": re-render (" + error + ")");
        if (!ok2) {
            continue;
        }
        std::printf("%s: %u draws, %llu triangles, %u covered pixels, gpu %.1f ms\n", sceneName.c_str(), first.draws,
                    static_cast<unsigned long long>(first.triangles), first.coveredPixels(), first.gpuFrameMs);
        expect(first.coveredPixels() > kGoldenWidth * kGoldenHeight / 4u, sceneName + ": the grid covers > 25 % of the frame");

        std::map<std::string, ImageRgba8> frames;
        for (const LightingSetup& setup : setups) {
            ResolveParams params;
            params.lightDir = setup.lightDir;
            params.background = setup.background;
            const fuse::math::Vec3 viewDir = scene.camera.eye - scene.camera.target;
            resolveCapture(first, viewDir, params);
            resolveCapture(second, viewDir, params);
            const std::string name = sceneName + "_" + setup.name;
            frames[setup.name] = first.lit;

            // 2. deterministic re-render.
            const FlipResult again = flip(first.lit, second.lit);
            expect(first.lit.pixels == second.lit.pixels && again.valid && again.mean == 0.0 && again.nonZero == 0u,
                   name + ": deterministic re-render is bit-identical, FLIP == 0 exactly (got " + fmt("%.9f", again.mean) + ")");

            // 3. committed golden.
            const std::string goldenPath = goldenDir + "/" + name + ".png";
            if (update) {
                expect(ensureDirectory(goldenDir) && writePng(goldenPath, first.lit), name + ": golden rewritten -> " + goldenPath);
                continue;
            }
            ImageRgba8 golden;
            std::string perr;
            if (!readPng(goldenPath, golden, &perr) || golden.width != first.lit.width || golden.height != first.lit.height) {
                expect(false, name + ": golden " + goldenPath + " missing or wrong size (" + perr +
                                  "); regenerate with FUSE_UPDATE_GOLDENS=1");
                writeArtifacts(artifactDir, name, first.lit, FlipResult{});
                continue;
            }
            const FlipResult g = flip(golden, first.lit);
            const bool exact = golden.pixels == first.lit.pixels;
            expect(g.valid && g.mean <= kGoldenMaxMeanFlip,
                   name + ": mean FLIP vs golden " + fmt("%.6f", g.mean) + " <= 0.05" + (exact ? " (bit-exact)" : " (NOT bit-exact)"));
            if (!(g.valid && g.mean <= kGoldenMaxMeanFlip)) {
                writeArtifacts(artifactDir, name, first.lit, g);
            }
        }
        if (update) {
            continue;
        }

        // 4. FLIP sanity on the real frame (sun setup).
        const ImageRgba8& sun = frames["sun"];
        ImageRgba8 onePixel = sun;
        u8* p = onePixel.at(kGoldenWidth / 2u, kGoldenHeight / 2u);
        p[0] = static_cast<u8>(255u - p[0]);
        p[1] = static_cast<u8>(255u - p[1]);
        const FlipResult f1 = flip(sun, onePixel);
        expect(f1.mean > 0.0 && f1.mean < 1e-4 && f1.nonZero >= 1u,
               "sanity: one changed pixel -> 0 < FLIP " + fmt("%.3g", f1.mean) + " < 1e-4");
        auto shifted = [&](u32 codes) {
            ImageRgba8 out = sun;
            for (std::size_t i = 0; i < out.pixels.size(); ++i) {
                if (i % 4u != 3u) {
                    out.pixels[i] = static_cast<u8>(out.pixels[i] > 255u - codes ? 255u : out.pixels[i] + codes);
                }
            }
            return out;
        };
        const FlipResult f1b = flip(sun, shifted(1u));
        expect(f1b.mean > 0.0 && f1b.mean <= kGoldenMaxMeanFlip,
               "sanity: +1-code brightness shift -> FLIP " + fmt("%.6f", f1b.mean) + " in (0, 0.05] (passes the gate)");
        const FlipResult f2 = flip(sun, shifted(8u));
        expect(f2.mean >= 0.07 && f2.mean <= 0.3,
               "sanity: +8-code brightness shift -> FLIP " + fmt("%.6f", f2.mean) + " in [0.07, 0.3] (fails the gate)");
        const FlipResult f3 = flip(sun, frames["interior"]);
        expect(f3.mean > kGoldenMaxMeanFlip,
               "sanity: interior frame vs sun golden -> FLIP " + fmt("%.6f", f3.mean) + " > 0.05 (gate fails)");
    }

    const ValidationReport v = runner->validation();
    std::printf("validation: %u error(s), %u warning(s)%s%s\n", v.errors, v.warnings, v.lastError.empty() ? "" : " last: ",
                v.lastError.c_str());
    expect(v.errors == 0u && v.warnings == 0u, "zero validation / sync-validation messages");
    runner.reset();
    fuse::core::shutdown();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
