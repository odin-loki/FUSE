// Gate fuse_ui_zero_alloc_steady: after warm-up, frames with hover / press / value / state changes and repaints
// perform zero heap allocations (global operator new is counted).
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"

#include <atomic>
#include <new>

namespace {
std::atomic<long> g_allocs{0};
bool g_counting = false;
} // namespace

// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t) noexcept {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
}

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    UiTestDriver drv(ui);
    const NodeHandle slider = ui.find("exposure"), bloom = ui.find("bloom"), apply = ui.find("apply");
    int clicks = 0;
    ui.connect(apply, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    auto frame = [&](int i) {
        drv.hover(i % 2 ? "apply" : "cancel");
        drv.click("apply");
        drv.click("bloom");
        ui.setValue(slider, 0.25f + 0.5f * f32(i % 2));
        ui.setText(ui.find("subtitle"), i % 2 ? "Retained core, CSS-subset themes, CPU reference rasteriser."
                                                : "Retained core, CSS-subset themes, CPU reference rasterizer.");
        ui.setFocus(i % 2 ? slider : bloom, FocusReason::Keyboard);
        drv.key(Key::Right);
        ui.update(1.f / 60.f);
        ui.update(1.f / 60.f); // idle frame
    };
    for (int i = 0; i < 8; ++i) frame(i); // warm-up: atlas, style table, scratch capacity
    g_counting = true;
    for (int i = 0; i < 50; ++i) frame(i);
    g_counting = false;
    std::printf("allocations in 50 steady frames: %ld (clicks %d)\n", g_allocs.load(), clicks);
    UI_CHECK_NO_ALLOC(g_allocs.load());
    UI_CHECK(clicks >= 50);
    return finish("fuse_ui_zero_alloc_steady");
}
