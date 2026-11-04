// hud_bench (plan §6, gate fuse_ui_bench_hud_mobile): the fuse_ui_bench_hud workload on a device. Shows a window, runs
// the ~300-node HUD of hud_bench_scene.hpp in an offscreen 1080p context (the same work as the desktop gate), shows
// and writes the result:
//   hud_bench [--state=fuse_demo_state.txt] [--frames=300] [--rounds=3]
// State file (read by App/android/android_gate.py --gate hud_bench): bench_done, bench_median_ms, bench_p95_ms,
// bench_nodes, bench_draw_cmds, bench_raster_ms. The app keeps running until the activity is stopped.
#include "hud_bench_scene.hpp"

#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/text/text.hpp"

#include <cstdio>
#include <string>

using namespace fuse::ui;
namespace hb = fuse::ui::game::bench;

namespace {
void writeState(const std::string& path, const std::string& s) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}
} // namespace

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "hud_bench", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& window = app.createWindow({.title = "FUSE HUD bench", .width = 400, .height = 300});
    UiContext& wui = window.ui();
    const NodeHandle col = widgets::panel(wui, wui.root(), "col");
    wui.setCss(col, "padding", "24px");
    const NodeHandle info = widgets::label(wui, col, "HUD bench running...", "info");
    const std::string statePath(app.args().get("state", "fuse_demo_state.txt"));
    writeState(statePath, "bench_done=0\n");
    fuse::app::NativePlatform native(app);
    native.show(window);
    for (int i = 0; i < 200 && window.framesPresented() < 1; ++i) app.step(0.005f);

    auto font = fuse::text::Font::bundled();
    if (!font) return app.fail("hud_bench: bundled font missing");
    UiContext ui({.viewport = hb::kBenchViewport, .font = &*font});
    widgets::install(ui);
    fuse::ui::game::install(ui);
    Image img;
    const hb::HudBenchResult r = hb::runHudBench(ui, int(app.args().number("frames", 300)), int(app.args().number("rounds", 3)), [&] {
        img.resize(int(hb::kBenchViewport.w), int(hb::kBenchViewport.h), 0xFF000000u);
        CpuRasterizer cr;
        cr.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        cr.render(ui.drawList(), img);
    });
    char buf[256];
    std::snprintf(buf, sizeof buf, "HUD: %u nodes, %u draw commands\nUI CPU per frame: median %.3f ms, p95 %.3f ms\n"
                  "CPU raster 1080p: %.1f ms", r.nodes, r.drawCmds, r.medianMs, r.p95Ms, r.rasterMs);
    std::printf("hud_bench: %s\n", buf);
    wui.setText(info, buf);
    std::snprintf(buf, sizeof buf, "bench_done=1\nbench_median_ms=%.4f\nbench_p95_ms=%.4f\nbench_nodes=%u\nbench_draw_cmds=%u\n"
                  "bench_raster_ms=%.2f\n", r.medianMs, r.p95Ms, r.nodes, r.drawCmds, r.rasterMs);
    writeState(statePath, buf);
    while (app.step()) {
    }
    return 0;
}
