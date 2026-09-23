// B2.11 gate: "Render graph compiles in < 1ms CPU time per frame".
// CPU-only: builds the per-frame graph the way RhiContext does, compiles it repeatedly and checks
// the median compile time. The 1 ms budget is enforced for optimised builds; Debug (-O0) reports
// the numbers and only fails on a gross regression.
#include <fuse/core/sanitizer.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/render_graph.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

#if defined(NDEBUG)
constexpr double kBudgetUs = 1000.0; // master plan B2.11
#else
constexpr double kBudgetUs = 10000.0; // unoptimised: catch only gross regressions
#endif
constexpr int kWarmup = 16;
constexpr int kSamples = 256;

double medianUs(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template <typename BuildFn>
double measureCompileUs(fuse::renderer::RenderGraph& graph, BuildFn&& build, const char* label) {
    std::vector<double> samples;
    samples.reserve(kSamples);
    for (int i = 0; i < kWarmup + kSamples; ++i) {
        graph.beginFrame(static_cast<fuse::u32>(i % 3));
        build(graph);
        const auto start = std::chrono::steady_clock::now();
        graph.compile();
        const auto end = std::chrono::steady_clock::now();
        if (i >= kWarmup) {
            samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
    }
    const double median = medianUs(samples);
    std::printf("%s: passes=%u culled=%u barriers=%u median=%.2fus p99=%.2fus (budget %.0fus)\n", label,
                graph.compileInfo().passCount, graph.compileInfo().culledPassCount, graph.compileInfo().barrierCount, median,
                samples[(samples.size() * 99) / 100], kBudgetUs);
    return median;
}

void testHybridFrameGraphBudget() {
    // Same population path RhiContext::submitFrame uses: clear3d → sprites2d → composite → present.
    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.f, 0.f, 0.f);
    for (int i = 0; i < 256; ++i) {
        commands.drawSprite2D(static_cast<float>(i), 0.f, 0.f, 255, 255, 255);
    }

    fuse::renderer::RenderGraph graph;
    const double median = measureCompileUs(
        graph,
        [&](fuse::renderer::RenderGraph& g) {
            fuse::renderer::populateRenderGraphFromCommandList(g, commands, 0.5f);
        },
        "hybrid frame graph");
    expectTrue(graph.compileInfo().compiled, "hybrid frame graph compiled");
    expectTrue(graph.compileInfo().passCount >= 4u, "hybrid frame graph has clear/sprites/composite/present");
    if (fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(median < kBudgetUs, "hybrid frame graph compile within budget");
    }
}

void testFullPassBudgetGraph() {
    // Worst case the fixed-storage graph allows: kMaxPassesPerFrame passes chained over transient
    // textures (each pass reads the previous target and writes its own), ending in present.
    constexpr fuse::u32 kPasses = fuse::renderer::RenderGraph::kMaxPassesPerFrame;
    constexpr fuse::u32 kTargets = 8u;
    std::array<std::array<fuse::renderer::RGTextureAccess, 2>, kPasses> accesses{};
    fuse::renderer::RGTextureAccess present{};
    present.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    present.access = fuse::renderer::RGResourceAccess::Present;

    fuse::renderer::RenderGraph graph;
    const double median = measureCompileUs(
        graph,
        [&](fuse::renderer::RenderGraph& g) {
            std::array<fuse::renderer::RGTextureRef, kTargets> targets{};
            for (fuse::u32 t = 0; t < kTargets; ++t) {
                fuse::renderer::TextureDesc desc{};
                desc.width = 1920;
                desc.height = 1080;
                targets[t] = g.createTransient(desc);
            }
            for (fuse::u32 p = 0; p + 1u < kPasses; ++p) {
                // The last chain pass resolves into the backbuffer so present keeps the chain live.
                const bool last = p + 2u == kPasses;
                accesses[p][0].texture =
                    last ? fuse::renderer::RGTextureRef{fuse::renderer::RenderGraph::kBackbufferTextureId}
                         : targets[p % kTargets];
                accesses[p][0].access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;
                accesses[p][1].texture = targets[(p + kTargets - 1u) % kTargets];
                accesses[p][1].access = fuse::renderer::RGResourceAccess::ShaderRead;

                fuse::renderer::RGPassDesc pass{};
                pass.name = "chain";
                pass.textureAccesses = accesses[p].data();
                pass.textureAccessCount = 2;
                g.addPass(pass);
            }
            fuse::renderer::RGPassDesc presentPass{};
            presentPass.name = "present";
            presentPass.textureAccesses = &present;
            presentPass.textureAccessCount = 1;
            g.addPass(presentPass);
        },
        "32-pass chain graph");
    expectTrue(graph.compileInfo().compiled, "32-pass graph compiled");
    expectTrue(graph.compileInfo().passCount == kPasses, "32-pass graph records every pass");
    expectTrue(graph.compileInfo().culledPassCount == 0u, "32-pass chain feeds present, nothing culled");
    expectTrue(graph.compileInfo().barrierCount >= kPasses - 2u,
               "write->sample transitions planned along the chain");
    if (fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(median < kBudgetUs, "32-pass graph compile within budget");
    }
}

} // namespace

int main() {
    testHybridFrameGraphBudget();
    testFullPassBudgetGraph();

    if (g_failures == 0) {
        std::printf("fuse_b2_render_graph_budget: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_render_graph_budget: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
