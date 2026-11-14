// fuse_ui_vk_custom_effect (UI4 gate, plan §4.9 "custom shaders"): custom shader effects (fuse/ui/effects.hpp) on the
// GPU backend against their CPU reference callbacks.
//   * built-ins desaturate / ripple / grain over a busy scene (stripes, dots, glyphs, gradients): plain, with a rounded
//     mask, inside a rounded clip, at several times, UI drawn after them, and a scissored redraw: bit-exact (max channel
//     error 0) on any Vulkan device
//   * a user effect compiled by fuse_ui_add_effect (tests/data/fx_vignette_image.glsl) that samples an image and calls
//     native exp / sin: within the documented tolerance (max channel error <= 2, < 1 % of pixels differ)
//   * the `effect: name(params)` style property reaches the draw list (defaults filled in), animated effects repaint
//     every update, the incremental redraw region grows over an effect's source region
//   * validation clean (VK_LAYER_KHRONOS_validation + synchronization validation, when installed)
//   * steady state: packing + uploading an effect frame does not allocate
// Skips (77) without a Vulkan device.
#include "fuse/ui/effects.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#if __has_include("kTestFxVignette_spv.h")
#include "kTestFxVignette_spv.h"
#define FUSE_UI_TEST_HAS_VIGNETTE 1
#endif

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>

static std::atomic<unsigned long long> g_allocs{0};
// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t) noexcept {
    ++g_allocs;
}

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
void checkExact(const char* what, const Diff& d) {
    std::printf("  %-22s max err %d, differing %zu / %zu (exact)\n", what, d.maxErr, d.differing, d.total);
    UI_CHECK_EQ(d.maxErr, 0);
}
void checkTolerance(const char* what, const Diff& d) {
    std::printf("  %-22s max err %d, differing %zu / %zu (tolerance: <= 2, < 1 %%)\n", what, d.maxErr, d.differing, d.total);
    UI_CHECK(d.maxErr <= 2);
    UI_CHECK(double(d.differing) < 0.01 * double(d.total));
}

// CPU twin of tests/data/fx_vignette_image.glsl (same structure; exp / sin are the C library's)
EffectColor vignetteImage(const EffectPixel& p) {
    const EffectColor c = p.src->at(p.x, p.y);
    const f32 u = ((f32(p.x) + 0.5f) - p.rect.x) / p.rect.w;
    const f32 v = ((f32(p.y) + 0.5f) - p.rect.y) / p.rect.h;
    const int w = p.image ? p.image->width : 0, h = p.image ? p.image->height : 0;
    const EffectColor t = fx::imageTexel(p.image, int(u * f32(w)), int(v * f32(h)));
    const f32 dx = u - 0.5f, dy = v - 0.5f;
    const f32 k = std::exp(-(dx * dx + dy * dy) * p.params[0]) * (0.9f + 0.1f * std::sin(p.time * 3.f + u * 10.f));
    EffectColor o;
    for (std::size_t i = 0; i < 4; ++i) o[i] = (c[i] + (t[i] - c[i]) * p.params[1]) * k;
    return o;
}

void scene(DrawList& dl, const DrawList& glyphs) {
    for (int i = 0; i < 25; ++i) dl.rect({f32(i * 20), 0.f, 20.f, 400.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
    for (int i = 0; i < 12; ++i) dl.rect({10.f + f32(i) * 40.f, 150.f, 18.f, 18.f}, Color::rgb(0xF0F0F0), 9.f);
    dl.gradientRect({20, 230, 440, 60}, Color::rgb(0x00ff00, 0.6f), Color::rgb(0xffff00, 0.9f), false, {10, 10, 10, 10});
    dl.line({0, 390}, {488, 210}, Color::rgb(0x33ff99), 4.f);
    for (const DrawCmd& c : glyphs.cmds())
        if (c.op == DrawOp::Glyph) dl.push(c);
}
} // namespace

int main() {
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    const bool validation = VkHeadless::validationAvailable();
    VkHeadless::validationRequested() = validation;
    constexpr u32 W = 488, H = 400;
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    std::printf("device: %s%s\n", vk.deviceName, validation ? " (validation on)" : " (validation layer not installed)");
    UiVkRenderer gpu;
    UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    if (!gpu.valid()) return finish("fuse_ui_vk_custom_effect");

    EffectRegistry fx;
    fx.addBuiltins();
    UI_CHECK_EQ(fx.count(), 3u);
    const u32 desat = u32(fx.find("desaturate")), ripple = u32(fx.find("ripple")), grain = u32(fx.find("grain"));
    UI_CHECK(fx.at(ripple).animated && fx.at(grain).animated && !fx.at(desat).animated);
#if FUSE_UI_TEST_HAS_VIGNETTE
    const u32 vignette = fx.add({.name = "vignette-image", .cpu = &vignetteImage, .spirv = kTestFxVignette,
                                 .defaults = {2.f, 0.35f}, .exact = false});
#else
    const u32 vignette = fx.add({.name = "vignette-image", .cpu = &vignetteImage, .defaults = {2.f, 0.35f}, .exact = false});
    std::printf("  (test effect SPIR-V not built: vignette case skipped on the GPU)\n");
#endif
    UI_CHECK(gpu.setEffects(fx));
    for (u32 id = 0; id < 3; ++id) UI_CHECK(gpu.hasEffect(id));
    UI_CHECK(!UiVkRenderer::builtinEffectSpirv("ripple").empty() && UiVkRenderer::builtinEffectSpirv("nope").empty());

    // glyphs from the sample tree's atlas
    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    const u8* atlas = ui.atlas().pixels();
    const u32 atlasSize = u32(ui.atlas().size());

    Image checker;
    checker.resize(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) checker.pixels[std::size_t(y) * 8 + std::size_t(x)] = ((x ^ y) & 1) ? 0xFF20C0F0u : 0xFF603010u;
    const Image* imgs[1] = {&checker};

    CpuRasterizer cpu;
    cpu.setGlyphAtlas(atlas, int(atlasSize));
    cpu.setEffects(&fx);
    cpu.addImage(&checker);
    GpuDrawData data;
    Image ref, out, init;
    auto run = [&](const DrawList& dl, Rect scissor, const Image& base) {
        ref = base;
        cpu.render(dl, ref, scissor);
        packDrawList(dl, W, H, data, atlas, atlasSize, imgs, 1, scissor);
        UI_CHECK(gpu.upload(0, data));
        vk.run(base, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        return compare(ref, out);
    };
    init.resize(int(W), int(H), 0xFF000000u);

    // 1. built-ins, exact
    struct Case {
        const char* name;
        u32 id;
        std::array<f32, kEffectParams> params;
        f32 time;
    };
    const Case cases[] = {
        {"desaturate", desat, {1.f, 1.f, 1.f, 1.f}, 0.f},
        {"desaturate sepia", desat, {0.85f, 1.f, 0.82f, 0.6f}, 0.f},
        {"ripple t=0", ripple, {6.f, 20.f, 40.f}, 0.f},
        {"ripple t=0.37", ripple, {9.5f, 31.f, 55.f}, 0.37f},
        {"grain t=1.2", grain, {0.2f, 24.f}, 1.2f},
        {"grain static", grain, {0.35f, 0.f}, 3.f},
    };
    for (const Case& k : cases) {
        DrawList dl;
        scene(dl, ui.drawList());
        const f32 margin = fx.margin(k.id, k.params.data());
        dl.effect({40.f, 30.f, 260.f, 170.f}, k.id, k.params, k.time, margin);                       // square
        dl.effect({250.5f, 120.25f, 200.f, 150.f}, k.id, k.params, k.time, margin, {24, 24, 6, 6});  // rounded, fractional
        dl.rect({60.f, 60.f, 80.f, 30.f}, Color::rgb(0xffffff, 0.6f), 6.f);                          // UI after the effect
        dl.pushClip({300.f, 290.f, 170.f, 100.f}, 18.f);
        dl.effect({280.f, 280.f, 220.f, 140.f}, k.id, k.params, k.time, margin);                      // in a rounded clip
        dl.popClip();
        dl.effect({0.f, 0.f, 50.f, 400.f}, k.id, k.params, k.time, margin);                           // at the target edge
        packDrawList(dl, W, H, data, atlas, atlasSize, imgs, 1);
        UI_CHECK_EQ(data.segments.size(), std::size_t(4)); // each effect ends a segment; nothing after the last
        checkExact(k.name, run(dl, {}, init));
        { // the effect changes the picture (the same list without effects)
            DrawList plain;
            for (const DrawCmd& c : dl.cmds())
                if (c.op != DrawOp::Effect) plain.push(c);
            Image p = init;
            cpu.render(plain, p);
            const Diff changed = compare(p, ref);
            std::printf("  %-22s changes %zu pixels\n", "", changed.differing);
            UI_CHECK(changed.differing > 5000);
        }
        // scissored redraw over the previous frame (damage path), region grown over the effects' source regions
        const Rect region = dl.redrawRegion({270.f, 140.f, 30.f, 30.f}, {0.f, 0.f, f32(W), f32(H)});
        UI_CHECK(region.x <= 250.5f - margin && region.right() >= 450.5f + margin);
        const Image prev = out;
        char name[64];
        std::snprintf(name, sizeof name, "%s (scissor)", k.name);
        checkExact(name, run(dl, region, prev));
    }

    // 2. user effect (fuse_ui_add_effect) sampling an image, native transcendental functions: tolerance
#if FUSE_UI_TEST_HAS_VIGNETTE
    {
        DrawList dl;
        scene(dl, ui.drawList());
        const std::array<f32, kEffectParams> p{2.5f, 0.4f};
        dl.effect({30.f, 40.f, 400.f, 300.f}, vignette, p, 0.75f, 0.f, {20, 20, 20, 20}, 0);
        checkTolerance("vignette-image (user)", run(dl, {}, init));
    }
#else
    (void)vignette;
#endif

    // 3. style hookup: `effect: name(params)`
    {
        UiContext su({.viewport = {f32(W), f32(H)}, .font = &font()});
        widgets::install(su);
        su.setEffects(&fx);
        (void)su.addStyleSheet(R"css(
            .grey { effect: desaturate(0.8); border-top-left-radius: 12px; }
            .wave { effect: ripple(5px, 18); }
            .plain { effect: none; }
        )css");
        buildSample(su);
        const NodeHandle card = su.find("card");
        su.addClass(card, "grey");
        const NodeHandle apply = su.find("apply");
        su.addClass(apply, "wave");
        su.update();
        UI_CHECK(su.computed(card).effect == hashId("desaturate") && su.computed(card).effectParamCount == 1);
        UI_CHECK(su.computed(apply).effectParamCount == 2 && su.computed(apply).effectParams[0] == 5.f);
        const DrawCmd* cardFx = nullptr;
        const DrawCmd* applyFx = nullptr;
        for (const DrawCmd& c : su.drawList().cmds())
            if (c.op == DrawOp::Effect) (c.node == card.index ? cardFx : applyFx) = &c;
        UI_CHECK(cardFx && applyFx);
        if (cardFx && applyFx) {
            UI_CHECK(cardFx->u == desat && cardFx->fill.r == 0.8f && cardFx->fill.g == 1.f && cardFx->radius[0] == 12.f);
            UI_CHECK(applyFx->u == ripple && applyFx->fill.r == 5.f && applyFx->fill.g == 18.f && applyFx->fill.b == 40.f);
            UI_CHECK_EQ(applyFx->borderWidth, 6.f); // margin: 1 + |amplitude|
            // the card's effect comes after everything its subtree draws
            UI_CHECK(cardFx > applyFx);
        }
        // animated: repaints every update, time advances
        UI_CHECK(su.needsUpdate());
        su.update(0.25f);
        UI_CHECK(su.stats().repainted);
        f32 t = -1.f;
        for (const DrawCmd& c : su.drawList().cmds())
            if (c.op == DrawOp::Effect && c.u == ripple) t = c.blur;
        UI_CHECK_EQ(t, 0.25f);
        UI_CHECK(!su.damage().empty());
        // the UiContext frame on the GPU vs the CPU reference
        Image a, b;
        fuse::ui::test::render(su, a);
        packDrawList(su.drawList(), W, H, data, su.atlas().pixels(), u32(su.atlas().size()));
        UI_CHECK(gpu.upload(1, data));
        vk.run(init, b, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        checkExact("styled context", compare(a, b));
        // bad values / none
        PropValue v;
        UI_CHECK(parsePropValue(Prop::Effect, "grain(0.1, 12)", v) && v.trackCount == 2 && v.ident == hashId("grain"));
        UI_CHECK(parsePropValue(Prop::Effect, "none", v) && v.ident == 0u);
        UI_CHECK(!parsePropValue(Prop::Effect, "grain(0.1", v));
        UI_CHECK(!parsePropValue(Prop::Effect, "gr ain", v));
        UI_CHECK(!parsePropValue(Prop::Effect, "grain(a, b)", v));
        su.removeClass(apply, "wave");
        su.removeClass(card, "grey");
        su.update(0.1f);
        su.update(0.1f);
        UI_CHECK(!su.needsUpdate()); // no animated effect left: the context sleeps again
    }

    // 4. an unknown effect id and a CPU-only effect are skipped / drawn by the CPU only (no crash, no GPU pass)
    {
        DrawList dl;
        scene(dl, ui.drawList());
        dl.effect({10.f, 10.f, 100.f, 100.f}, 31, std::array<f32, 1>{1.f}, 0.f, 0.f);
        Image a = init, b;
        cpu.render(dl, a);
        packDrawList(dl, W, H, data, atlas, atlasSize, imgs, 1);
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, b, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        checkExact("unknown effect id", compare(a, b));
    }

    // 5. steady state: packing + uploading an effect frame again does not allocate
    {
        DrawList dl;
        scene(dl, ui.drawList());
        dl.effect({40.f, 30.f, 260.f, 170.f}, ripple, std::array<f32, 3>{6.f, 20.f, 40.f}, 0.5f, 7.f);
        dl.effect({250.f, 120.f, 200.f, 150.f}, grain, std::array<f32, 2>{0.2f, 24.f}, 0.5f, 0.f);
        packDrawList(dl, W, H, data, atlas, atlasSize, imgs, 1);
        const auto before = g_allocs.load();
        for (int i = 0; i < 10; ++i) {
            dl.cmds()[dl.size() - 1].blur = 0.5f + f32(i) * 0.016f; // time moves on
            packDrawList(dl, W, H, data, atlas, atlasSize, imgs, 1);
            UI_CHECK(gpu.upload(u32(i & 1), data));
        }
        UI_CHECK_EQ(g_allocs.load() - before, 0ull);
    }

    gpu.destroy();
    if (validation) {
        std::printf("  validation messages: %d\n", VkHeadless::validationMessages());
        UI_CHECK_EQ(VkHeadless::validationMessages(), 0);
    }
    return finish("fuse_ui_vk_custom_effect");
}
