// fuse_ui_vk_damage (UI4 gate): damage-driven incremental GPU redraw, as the fuse_app presenter does it (the canvas
// keeps the last frame; the next one redraws DrawList::redrawRegion(damage) only, packed with that scissor + clear).
//   * one widget changes (the progress bar of the Tier A sample): the redraw region is a small part of the window and
//     only the tiles it touches are dispatched; pixels outside it are not written (a sentinel there survives)
//   * the incremental image equals a full GPU re-render and the CPU reference exactly
//   * a backdrop next to the change: the region grows to its sampled area, and the result still equals a full render
//     (the raw damage alone would not)
// Skips (77) without a Vulkan device.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
constexpr u32 W = 488, H = 400;
constexpr u32 kSentinel = 0xFF00FF00u;

std::size_t differing(const Image& a, const Image& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) n += a.pixels[i] != b.pixels[i];
    return n;
}
// tiles the packed frame dispatches (all segments)
u32 dispatchedTiles(const GpuDrawData& d) {
    u32 n = 0;
    for (const auto& s : d.segments)
        n += ((s.tile1 & 0xFFFFu) - (s.tile0 & 0xFFFFu)) * ((s.tile1 >> 16) - (s.tile0 >> 16));
    return n;
}
// previous frame with every pixel outside `keep` replaced by the sentinel
Image poisoned(const Image& prev, Rect keep) {
    Image p = prev;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            if (!keep.contains({f32(x) + 0.5f, f32(y) + 0.5f})) p.pixels[std::size_t(y) * std::size_t(p.width) + std::size_t(x)] = kSentinel;
    return p;
}
bool outsideIsSentinel(const Image& img, Rect keep) {
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (!keep.contains({f32(x) + 0.5f, f32(y) + 0.5f}) && img.at(x, y) != kSentinel) return false;
    return true;
}
bool insideEquals(const Image& a, const Image& b, Rect keep) {
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x)
            if (keep.contains({f32(x) + 0.5f, f32(y) + 0.5f}) && a.at(x, y) != b.at(x, y)) return false;
    return true;
}
// copies `region` of `inc` over `prev` (what the canvas holds after the incremental frame)
Image composed(const Image& prev, const Image& inc, Rect region) {
    Image o = prev;
    for (int y = 0; y < o.height; ++y)
        for (int x = 0; x < o.width; ++x)
            if (region.contains({f32(x) + 0.5f, f32(y) + 0.5f})) o.pixels[std::size_t(y) * std::size_t(o.width) + std::size_t(x)] = inc.at(x, y);
    return o;
}
} // namespace

int main() {
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    std::printf("device: %s\n", vk.deviceName);
    UiVkRenderer gpu;
    UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    if (!gpu.valid()) return finish("fuse_ui_vk_damage");
    const Rect whole{0.f, 0.f, f32(W), f32(H)};
    const Color black{0.f, 0.f, 0.f, 1.f};
    GpuDrawData data;
    Image black0, frame1, frame2, full2, cpu2;
    black0.resize(int(W), int(H), 0xFF000000u);
    const u32 allTiles = ((W + 15) / 16) * ((H + 15) / 16);

    // 1. one widget changes
    {
        UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
        widgets::install(ui);
        buildSample(ui);
        ui.update();
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(gpu.upload(0, data));
        vk.run(black0, frame1, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });

        ui.setValue(ui.find("progress"), 0.25f);
        ui.update();
        const Rect damage = ui.damage();
        const Rect region = ui.drawList().redrawRegion(damage, whole);
        UI_CHECK(!region.empty());
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, region, &black);
        const u32 tiles = dispatchedTiles(data);
        std::printf("widget: damage %.0fx%.0f at (%.0f, %.0f), redraw %.0f px (%.2f %% of the window), %u / %u tiles\n",
                    damage.w, damage.h, damage.x, damage.y, region.w * region.h, 100.0 * region.w * region.h / (W * H),
                    tiles, allTiles);
        UI_CHECK(region.w * region.h < 0.1f * f32(W * H));
        UI_CHECK(tiles * 5 < allTiles);
        UI_CHECK(gpu.upload(1, data));
        Image inc;
        vk.run(poisoned(frame1, region), inc, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        UI_CHECK(outsideIsSentinel(inc, region)); // nothing outside the region was written
        frame2 = composed(frame1, inc, region);

        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(gpu.upload(0, data));
        vk.run(black0, full2, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        fuse::ui::test::render(ui, cpu2);
        const std::size_t dGpu = differing(frame2, full2), dCpu = differing(frame2, cpu2);
        std::printf("widget: incremental vs full GPU: %zu px differ, vs CPU: %zu px differ\n", dGpu, dCpu);
        UI_CHECK_EQ(dGpu, std::size_t(0));
        UI_CHECK_EQ(dCpu, std::size_t(0));
        UI_CHECK(differing(frame1, frame2) > 0); // the change is visible
    }

    // 2. a change next to a backdrop: the region grows over the backdrop's sampled area
    {
        auto build = [](DrawList& dl, Color stripe) {
            dl.clear();
            for (int i = 0; i < 25; ++i)
                dl.rect({f32(i * 20), 0.f, 20.f, 400.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
            dl.rect({40.f, 250.f, 30.f, 30.f}, stripe, 4.f); // the changing widget, just left of the panel's margin
            dl.backdrop({80.f, 200.f, 220.f, 140.f}, 12.f, {16.f, 16.f, 16.f, 16.f}, Color::rgb(0xffffff, 0.2f));
            dl.rect({100.f, 220.f, 80.f, 20.f}, Color::rgb(0x101010, 0.7f), 4.f); // drawn over the blur
            dl.rect({60.f, 330.f, 120.f, 40.f}, Color::rgb(0x30c060), 6.f);        // over the margin, after the blur
        };
        DrawList a, b;
        build(a, Color::rgb(0xffcc00));
        build(b, Color::rgb(0x00ccff));
        Image fa, fb;
        packDrawList(a, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        vk.run(black0, fa, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        const Rect damage{40.f, 250.f, 30.f, 30.f};
        const Rect region = b.redrawRegion(damage, whole);
        std::printf("backdrop: damage %.0fx%.0f -> redraw %.0fx%.0f at (%.0f, %.0f)\n", damage.w, damage.h, region.w,
                    region.h, region.x, region.y);
        UI_CHECK(region.w > 220.f && region.h > 140.f);
        packDrawList(b, W, H, data, nullptr, 0, nullptr, 0, region, &black);
        UI_CHECK(gpu.upload(1, data));
        Image inc;
        vk.run(poisoned(fa, region), inc, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        UI_CHECK(outsideIsSentinel(inc, region));
        const Image fInc = composed(fa, inc, region);
        packDrawList(b, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        vk.run(black0, fb, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        Image cpuB;
        cpuB.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer().render(b, cpuB);
        UI_CHECK(insideEquals(fb, cpuB, whole));
        const std::size_t d = differing(fInc, fb);
        std::printf("backdrop: incremental vs full: %zu px differ\n", d);
        UI_CHECK_EQ(d, std::size_t(0));
        // the raw (ungrown) damage alone would leave a stale blur: grown region is needed
        Rect raw = damage;
        raw.w += 16.f; // reaches into the backdrop's margin
        packDrawList(b, W, H, data, nullptr, 0, nullptr, 0, raw, &black);
        UI_CHECK(gpu.upload(1, data));
        Image rawInc;
        vk.run(fa, rawInc, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        const std::size_t stale = differing(rawInc, fb);
        std::printf("backdrop: raw damage only: %zu px differ (stale blur)\n", stale);
        UI_CHECK(stale > 0);
    }
    gpu.destroy();
    return finish("fuse_ui_vk_damage");
}
