// fuse_ui_bench_render: render performance of the CPU reference rasteriser and the GPU backend (ui.draw_list +
// ui.backdrop on whatever Vulkan device is present; lavapipe in CI) for a full frame, a damage repaint, backdrop blur
// and a text-heavy page. Prints a table and writes JSON (--out=<file>). No thresholds: a trend tool, not a gate
// (label "bench"). GPU times are a submit + wait minus the harness' own transfer round trip (the same submit with no
// UI work), so they are kernel time plus dispatch overhead.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <cstdio>
#include <string>
#include <string_view>

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
using Clock = std::chrono::steady_clock;
template <class F> double msPer(int n, F&& f) {
    f(); // warm
    const auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) f();
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / n;
}
struct Row {
    std::string name;
    double cpu = 0, gpu = -1, pack = 0;
};
} // namespace

int main(int argc, char** argv) {
    std::string out = "bench_render.json"; // ctest: the test working directory
    int iters = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.starts_with("--out=")) out = std::string(a.substr(6));
        if (a.starts_with("--iters=")) iters = std::max(1, std::atoi(argv[i] + 8));
    }
    constexpr u32 W = 1280, H = 800;
    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();

    // scenes: the Tier A sample, a 200x150 damage box of it, frosted panels over it, a page of text
    DrawList blur = ui.drawList();
    blur.backdrop({100, 100, 500, 400}, 12.f, {16, 16, 16, 16}, Color::rgb(0xffffff, 0.2f));
    blur.backdrop({700, 300, 400, 300}, 24.f, {8, 8, 8, 8}, Color::rgb(0x000000, 0.2f));
    DrawList text;
    text.rect({0, 0, f32(W), f32(H)}, Color::rgb(0x202020));
    {
        std::vector<DrawCmd> glyphs;
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.op == DrawOp::Glyph) glyphs.push_back(c);
        for (int row = 0; !glyphs.empty() && row < 50; ++row)
            for (int col = 0; col < 8; ++col)
                for (const DrawCmd& g0 : glyphs) {
                    DrawCmd g = g0;
                    g.rect.x = std::floor(g.rect.x * 0.3f) + f32(col * 160);
                    g.rect.y = std::floor(g.rect.y * 0.05f) + f32(row * 16);
                    text.push(g);
                }
    }
    struct Scene {
        const char* name;
        const DrawList* list;
        Rect scissor;
    } scenes[] = {{"full_frame", &ui.drawList(), {}},
                  {"damage_200x150", &ui.drawList(), {300, 200, 200, 150}},
                  {"backdrop_blur", &blur, {}},
                  {"text_page", &text, {}}};

    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    Image img;
    img.resize(int(W), int(H), 0xFF000000u);
    std::vector<Row> rows;
    for (const Scene& s : scenes) {
        Row row{s.name};
        row.cpu = msPer(iters, [&] { r.render(*s.list, img, s.scissor); });
        rows.push_back(row);
    }

    VkHeadless vk;
    UiVkRenderer gpu;
    std::string device = "none";
    if (UiVkRenderer::kernelAvailable() && vk.init(W, H) &&
        gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1}) && gpu.valid()) {
        device = vk.deviceName;
        Image init, res;
        init.resize(int(W), int(H), 0xFF000000u);
        const double base = msPer(iters, [&] { vk.run(init, res, [](VkCommandBuffer) {}); });
        GpuDrawData data;
        for (std::size_t i = 0; i < std::size(scenes); ++i) {
            const Scene& s = scenes[i];
            rows[i].pack = msPer(iters, [&] {
                packDrawList(*s.list, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, s.scissor);
            });
            data.atlasGeneration = ui.atlas().version();
            gpu.upload(0, data);
            const double t = msPer(iters, [&] { vk.run(init, res, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); }); });
            rows[i].gpu = std::max(0.0, t - base);
        }
    }

    std::printf("fuse_ui_bench_render %ux%u, %d iterations, GPU: %s\n", W, H, iters, device.c_str());
    std::printf("  %-16s %10s %10s %10s\n", "scene", "cpu ms", "gpu ms", "pack ms");
    for (const Row& row : rows) std::printf("  %-16s %10.3f %10.3f %10.3f\n", row.name.c_str(), row.cpu, row.gpu, row.pack);
    if (!out.empty()) {
        if (FILE* f = std::fopen(out.c_str(), "wb")) {
            std::fprintf(f, "{\"width\":%u,\"height\":%u,\"iterations\":%d,\"device\":\"%s\",\"scenes\":[", W, H, iters, device.c_str());
            for (std::size_t i = 0; i < rows.size(); ++i)
                std::fprintf(f, "%s{\"name\":\"%s\",\"cpu_ms\":%.4f,\"gpu_ms\":%.4f,\"pack_ms\":%.4f}", i ? "," : "",
                             rows[i].name.c_str(), rows[i].cpu, rows[i].gpu, rows[i].pack);
            std::fprintf(f, "]}\n");
            std::fclose(f);
        }
    }
    gpu.destroy();
    return 0;
}
