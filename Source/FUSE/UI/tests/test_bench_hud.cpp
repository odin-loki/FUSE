// Gate fuse_ui_bench_hud (plan §6): a typical game HUD (~300 nodes, Game/samples/hud_bench_scene.hpp) animated as in
// play; UI CPU per frame (HUD state + style, layout and paint into the draw list) median ≤ 0.5 ms in optimised builds
// (best of three 300-frame rounds).
// The CPU raster of a full frame is reported, not gated (the GPU draws the HUD in games). Debug builds report only.
#include "hud_bench_scene.hpp"
#include "test_util.hpp"

#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace hb = fuse::ui::game::bench;

int main() {
    UiContext ui({.viewport = hb::kBenchViewport, .font = &font()});
    widgets::install(ui);
    game::install(ui);
    Image img;
    const hb::HudBenchResult r = hb::runHudBench(ui, 300, 3, [&] {
        img.resize(int(hb::kBenchViewport.w), int(hb::kBenchViewport.h), 0xFF000000u);
        CpuRasterizer cr;
        cr.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        cr.render(ui.drawList(), img);
    });
    std::printf("HUD: %u nodes, %u draw commands; UI CPU per frame median %.3f ms, p95 %.3f ms (budget 0.5 ms); "
                "full CPU raster 1080p %.2f ms (not gated)\n", r.nodes, r.drawCmds, r.medianMs, r.p95Ms, r.rasterMs);
    std::printf("last frame: styled %u, measured %u, arranged %u, painted %u\n", ui.stats().styled, ui.stats().measured,
                ui.stats().arranged, ui.stats().painted);
    UI_CHECK(r.nodes >= 250 && r.nodes <= 400); // "typical HUD (~300 nodes)"
    UI_CHECK(r.drawCmds > 200);
#ifdef NDEBUG
    UI_CHECK(r.medianMs <= 0.5);
#else
    std::printf("(debug build: budget not enforced)\n");
#endif
    return finish("fuse_ui_bench_hud");
}
