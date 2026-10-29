// fuse_ui_vk_parity (UI4 gate): the GPU backend ("ui.draw_list" compute kernel) against the CPU reference rasteriser
// (the specification), on the Tier A sample, a primitives sheet (gradients, borders, shadows, lines, nested rounded
// clips, images, glyphs) and a scissored redraw. Gate: max channel error <= 2/255 and < 0.5 % of pixels differing at
// all. Also: packing + upload of an unchanged frame does not allocate. Skips (77) without a Vulkan device.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

static std::atomic<unsigned long long> g_allocs{0};
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
struct Diff {
    int maxErr = 0;
    std::size_t differing = 0, total = 0;
};
Diff compare(const Image& a, const Image& b) {
    Diff d;
    d.total = a.pixels.size();
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        int m = 0;
        for (int s = 0; s < 32; s += 8) m = std::max(m, std::abs(int((a.pixels[i] >> s) & 0xFF) - int((b.pixels[i] >> s) & 0xFF)));
        d.maxErr = std::max(d.maxErr, m);
        d.differing += m ? 1 : 0;
    }
    return d;
}
void check(const char* what, const Diff& d) {
    std::printf("%-12s max err %d, differing %zu / %zu\n", what, d.maxErr, d.differing, d.total);
    UI_CHECK(d.maxErr <= 2);
    UI_CHECK(double(d.differing) < 0.005 * double(d.total));
}
} // namespace

int main() {
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    constexpr u32 W = 488, H = 400;
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    std::printf("device: %s\n", vk.deviceName);
    UiVkRenderer gpu;
    UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    if (!gpu.valid()) return finish("fuse_ui_vk_parity");

    // 1. Tier A sample
    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    Image cpu, out, init;
    fuse::ui::test::render(ui, cpu);
    init.resize(int(W), int(H), 0xFF000000u);
    GpuDrawData data;
    packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
    UI_CHECK(gpu.upload(0, data));
    vk.run(init, out, [&](VkCommandBuffer cmd) { gpu.record((void*)cmd, (void*)vk.view, 0); });
    check("sample", compare(cpu, out));

    // 2. primitives sheet, with an image
    Image checker;
    checker.resize(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) checker.pixels[std::size_t(y) * 8 + std::size_t(x)] = ((x ^ y) & 1) ? 0xFF2080F0u : 0x80404040u;
    DrawList dl;
    dl.gradientRect({10, 10, 200, 80}, Color::rgb(0x3355ff), Color::rgb(0xff5533, 0.5f), true, {12, 4, 12, 4});
    dl.roundedRect({220, 10, 120, 80}, Color::rgb(0x223344), {16, 16, 16, 16}, Color::rgb(0xffffff, 0.8f), 3.f);
    dl.shadow({30, 120, 140, 70}, Color::rgb(0x000000, 0.6f), 10.f, 8.f);
    dl.rect({30, 120, 140, 70}, Color::rgb(0xeeeeee), 8.f);
    dl.line({200, 120}, {460, 190}, Color::rgb(0x33ff99), 3.5f);
    dl.line({200, 190}, {460, 125}, Color::rgb(0xff3399, 0.7f), 1.f);
    dl.pushClip({20, 210, 300, 160}, 24.f);
    dl.rect({0, 200, 488, 200}, Color::rgb(0x884422));
    dl.pushClip({60, 230, 400, 60});
    dl.gradientRect({0, 220, 488, 80}, Color::rgb(0x00ff00, 0.3f), Color::rgb(0x0000ff, 0.9f), false);
    dl.popClip();
    DrawCmd ic;
    ic.op = DrawOp::Image;
    ic.rect = {150, 300, 96, 64};
    dl.push(ic);
    dl.popClip();
    // glyphs from the context's atlas: reuse the sample's glyph commands, shifted
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Glyph) {
            DrawCmd g = c;
            g.rect.x = std::floor(g.rect.x * 0.5f) + 330;
            g.rect.y = std::floor(g.rect.y * 0.5f) + 220;
            dl.push(g);
        }
    Image cpu2;
    cpu2.resize(int(W), int(H), 0xFF101010u);
    init.resize(int(W), int(H), 0xFF101010u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    const Image* imgs[1] = {&checker};
    r.addImage(&checker);
    r.render(dl, cpu2);
    packDrawList(dl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), imgs, 1);
    UI_CHECK(gpu.upload(1, data));
    vk.run(init, out, [&](VkCommandBuffer cmd) { gpu.record((void*)cmd, (void*)vk.view, 1); });
    check("primitives", compare(cpu2, out));

    // 3. scissored redraw over existing content (damage path)
    const Rect sc{100, 50, 200, 150};
    Image cpu3 = cpu2;
    r.render(ui.drawList(), cpu3, sc);
    packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, sc);
    UI_CHECK(gpu.upload(0, data));
    vk.run(cpu2, out, [&](VkCommandBuffer cmd) { gpu.record((void*)cmd, (void*)vk.view, 0); });
    check("scissor", compare(cpu3, out));

    // 4. steady state: packing + uploading the same frame again does not allocate
    packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
    const auto before = g_allocs.load();
    for (int i = 0; i < 10; ++i) {
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(gpu.upload(u32(i & 1), data));
    }
    UI_CHECK_EQ(g_allocs.load() - before, 0ull);
    gpu.destroy();
    return finish("fuse_ui_vk_parity");
}
