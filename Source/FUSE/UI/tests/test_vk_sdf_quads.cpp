// fuse_ui_vk_sdf_quads (UI4 gate, plan §4.9 / UI0 spike (c)): the SDF-quad raster backend (UiVkBackend::SdfQuads:
// instanced quads through the graphics pipeline, fixed-function premultiplied-over blending) against the CPU reference.
//   * scenes: the Tier A sample, a primitives sheet (gradients, borders, shadows, lines, nested rounded clips, images,
//     glyphs), backdrop blurs and custom effects between quad draws, a scissored incremental redraw (opaque clear)
//   * tolerance: max channel error <= 2, mean absolute channel error <= 0.1 / 255 (the coverage arithmetic before the
//     blend is the reference's; the blend is the hardware's, so exactness is not expected)
//   * validation clean (VK_LAYER_KHRONOS_validation + synchronization validation, when installed)
//   * timing (timestamp queries, report only): compute tile kernel vs SDF quads on typical 1080p and 4K frames
// Skips (77) without a Vulkan device.
#include "fuse/ui/effects.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <algorithm>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkBackend;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
struct Diff {
    int maxErr = 0;
    std::size_t differing = 0, total = 0;
    double sumErr = 0.0; ///< sum of absolute channel errors
    double mean() const { return total ? sumErr / double(total * 4) : 0.0; }
};
Diff compare(const Image& a, const Image& b) {
    Diff d;
    d.total = a.pixels.size();
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        int m = 0;
        for (int s = 0; s < 32; s += 8) {
            const int e = std::abs(int((a.pixels[i] >> s) & 0xFF) - int((b.pixels[i] >> s) & 0xFF));
            m = std::max(m, e);
            d.sumErr += e;
        }
        d.maxErr = std::max(d.maxErr, m);
        d.differing += m ? 1 : 0;
    }
    return d;
}
// Tolerance of the SDF-quad path: fixed-function blending rounds translucent overs differently from the reference's
// per-blend 8-bit quantisation (off by one where alpha < 1), so: max channel error <= 2, mean absolute channel error
// <= 0.1 / 255. The compute kernel stays bit-exact.
constexpr int kMaxErr = 2;
constexpr double kMaxMean = 0.1;
void check(const char* what, const Diff& sdf, const Diff& compute) {
    std::printf("  %-14s sdf: max err %d, mean %.4f, differing %6zu / %zu   (compute kernel: max err %d, differing %zu)\n",
                what, sdf.maxErr, sdf.mean(), sdf.differing, sdf.total, compute.maxErr, compute.differing);
    UI_CHECK(sdf.maxErr <= kMaxErr);
    UI_CHECK(sdf.mean() <= kMaxMean);
    UI_CHECK_EQ(compute.maxErr, 0);
}
void appendShifted(DrawList& out, const DrawList& src, f32 dx, f32 dy) {
    for (DrawCmd c : src.cmds()) {
        c.rect.x += dx;
        c.rect.y += dy;
        if (c.op == DrawOp::Line) {
            c.rect.w += dx;
            c.rect.h += dy;
        }
        out.push(c);
    }
}
} // namespace

int main() {
    if (!UiVkRenderer::kernelAvailable() || !UiVkRenderer::sdfQuadsAvailable()) {
        std::printf("SKIP: kernel / SDF-quad shaders not compiled in\n");
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
    UiVkRenderer comp, sdf;
    UI_CHECK(comp.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    fuse::ui::render::UiVkDesc sdfDesc{.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2};
    UI_CHECK(fuse::ui::render::useSdfQuads(sdfDesc));
    UI_CHECK(sdf.init(sdfDesc));
    UiVkRenderer noShaders; // the backend without its shaders refuses to start
    UI_CHECK(!noShaders.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .backend = UiVkBackend::SdfQuads}));
    if (!comp.valid() || !sdf.valid()) return finish("fuse_ui_vk_sdf_quads");
    UI_CHECK(sdf.backend() == UiVkBackend::SdfQuads && comp.backend() == UiVkBackend::Compute);
    EffectRegistry fx;
    fx.addBuiltins();
    UI_CHECK(comp.setEffects(fx) && sdf.setEffects(fx));

    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    const u8* atlas = ui.atlas().pixels();
    const u32 atlasSize = u32(ui.atlas().size());
    Image checker;
    checker.resize(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) checker.pixels[std::size_t(y) * 8 + std::size_t(x)] = ((x ^ y) & 1) ? 0xFF2080F0u : 0x80404040u;
    const Image* imgs[1] = {&checker};
    CpuRasterizer cpu;
    cpu.setGlyphAtlas(atlas, int(atlasSize));
    cpu.setEffects(&fx);
    cpu.addImage(&checker);

    GpuDrawData data;
    Image ref, outS, outC;
    auto run = [&](const char* what, const DrawList& dl, const Image& base, Rect scissor = {}, const Color* clear = nullptr) {
        ref = base;
        if (clear) { // the packed clear: an opaque rect over the scissor first
            DrawList cl;
            cl.rect(scissor, *clear);
            cpu.render(cl, ref, scissor);
        }
        cpu.render(dl, ref, scissor);
        packDrawList(dl, W, H, data, atlas, atlasSize, imgs, 1, scissor, clear);
        UI_CHECK(sdf.upload(0, data) && comp.upload(0, data));
        vk.run(base, outS, [&](VkCommandBuffer c) { sdf.record((void*)c, (void*)vk.view, 0); });
        vk.run(base, outC, [&](VkCommandBuffer c) { comp.record((void*)c, (void*)vk.view, 0); });
        check(what, compare(ref, outS), compare(ref, outC));
    };
    Image black, grey;
    black.resize(int(W), int(H), 0xFF000000u);
    grey.resize(int(W), int(H), 0xFF101010u);

    // 1. Tier A sample
    run("sample", ui.drawList(), black);
    // 2. primitives sheet
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
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Glyph) {
            DrawCmd g = c;
            g.rect.x = std::floor(g.rect.x * 0.5f) + 330;
            g.rect.y = std::floor(g.rect.y * 0.5f) + 220;
            dl.push(g);
        }
    run("primitives", dl, grey);
    // 3. backdrops and effects between quad draws
    DrawList bl;
    for (int i = 0; i < 25; ++i) bl.rect({f32(i * 20), 0.f, 20.f, 400.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
    for (int i = 0; i < 12; ++i) bl.rect({10.f + f32(i) * 40.f, 150.f, 18.f, 18.f}, Color::rgb(0xF0F0F0), 9.f);
    bl.backdrop({60.f, 40.f, 200.f, 120.f}, 12.f, {16.f, 16.f, 16.f, 16.f});
    bl.rect({80.f, 60.f, 60.f, 24.f}, Color::rgb(0xffffff, 0.6f), 6.f);
    bl.effect({250.f, 180.f, 200.f, 150.f}, u32(fx.find("ripple")), std::array<f32, 3>{6.f, 20.f, 40.f}, 0.3f, 7.f, {12, 12, 12, 12});
    bl.effect({20.f, 250.f, 200.f, 120.f}, u32(fx.find("desaturate")), std::array<f32, 1>{1.f}, 0.f, 0.f);
    bl.rect({300.f, 300.f, 120.f, 40.f}, Color::rgb(0x112233, 0.8f), 10.f);
    run("blur+effects", bl, black);
    // 4. scissored incremental redraw with the packed opaque clear
    const Color clear{0.f, 0.f, 0.f, 1.f};
    run("incremental", ui.drawList(), outS, {96.f, 48.f, 208.f, 160.f}, &clear);

    // validation covers the parity scenes; the timing loop records one slot ten times per command buffer
    // (re-writing its descriptor set), which is fine for timestamps but not a valid frame
    const int messages = VkHeadless::validationMessages();
    VkHeadless::validationRequested() = false;

    // 5. timing: compute kernel vs SDF quads, typical frames (8 sample cards per 1080p of area, + 2 frosted panels)
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.phys, &props);
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, qs.data());
    if (qs[vk.family].timestampValidBits == 0 || props.limits.timestampPeriod <= 0.f) {
        std::printf("  (no timestamps: timing skipped)\n");
    } else {
        UiContext card({.viewport = {470.f, 520.f}, .font = &font()});
        widgets::install(card);
        buildSample(card);
        card.update();
        std::printf("  timing (10 frames per submission, 20 submissions; ms per frame, median / min):\n");
        std::printf("  %-9s %-9s %6s %15s %15s\n", "res", "frame", "cmds", "compute", "sdf");
        for (const auto [tw, th] : {std::pair<u32, u32>{1920, 1080}, std::pair<u32, u32>{3840, 2160}}) {
            VkHeadless big;
            if (!big.init(tw, th)) continue;
            UiVkRenderer bc, bs;
            UI_CHECK(bc.init({.physicalDevice = (void*)big.phys, .device = (void*)big.device, .framesInFlight = 1}));
            fuse::ui::render::UiVkDesc bd{.physicalDevice = (void*)big.phys, .device = (void*)big.device, .framesInFlight = 1};
            fuse::ui::render::useSdfQuads(bd);
            UI_CHECK(bs.init(bd));
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 2;
            VkQueryPool qp = VK_NULL_HANDLE;
            UI_CHECK(vkCreateQueryPool(big.device, &qpi, nullptr, &qp) == VK_SUCCESS);
            Image base, scratch;
            base.resize(int(tw), int(th), 0xFF000000u);
            auto time = [&](UiVkRenderer& r, const GpuDrawData& d) {
                UI_CHECK(r.upload(0, d));
                constexpr int kReps = 10;
                std::vector<double> ms;
                for (int run = 0; run < 21; ++run) {
                    big.run(base, scratch, [&](VkCommandBuffer c) {
                        vkCmdResetQueryPool(c, qp, 0, 2);
                        vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
                        for (int rep = 0; rep < kReps; ++rep) {
                            r.record((void*)c, (void*)big.view, 0);
                            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                                 1, &mb, 0, nullptr, 0, nullptr);
                        }
                        vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
                    });
                    u64 t[2] = {};
                    vkGetQueryPoolResults(big.device, qp, 0, 2, sizeof t, t, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                    if (run > 0) ms.push_back(double(t[1] - t[0]) * props.limits.timestampPeriod * 1e-6 / kReps);
                }
                std::sort(ms.begin(), ms.end());
                return std::pair<double, double>{ms[ms.size() / 2], ms.front()};
            };
            const int cols = int(tw / 480), rows = int(th / 540);
            for (int variant = 0; variant < 3; ++variant) {
                DrawList frame;
                frame.gradientRect({0, 0, f32(tw), f32(th)}, Color::rgb(0x202838), Color::rgb(0x101018), true);
                for (int i = 0; i < cols * rows; ++i) appendShifted(frame, card.drawList(), f32(i % cols) * 480.f, f32(i / cols) * 540.f);
                const char* name = "full";
                if (variant == 1) {
                    name = "backdrop";
                    frame.backdrop({200.f, 150.f, 600.f, 360.f}, 12.f, {16.f, 16.f, 16.f, 16.f}, Color::rgb(0xffffff, 0.15f));
                    frame.backdrop({1100.f, 500.f, 500.f, 400.f}, 18.f, {12.f, 12.f, 12.f, 12.f}, Color::rgb(0x000000, 0.2f));
                    frame.rect({240.f, 190.f, 200.f, 40.f}, Color::rgb(0xeeeeee, 0.9f), 6.f);
                } else if (variant == 2) {
                    name = "incr";
                }
                GpuDrawData d;
                const Color black1{0.f, 0.f, 0.f, 1.f};
                if (variant == 2) // one widget's damage (a progress bar: ~775 x 386 px region in the 1080p budget gate)
                    packDrawList(frame, tw, th, d, card.atlas().pixels(), u32(card.atlas().size()), nullptr, 0,
                                 {24.f, 200.f, 440.f, 120.f}, &black1);
                else
                    packDrawList(frame, tw, th, d, card.atlas().pixels(), u32(card.atlas().size()));
                const auto tc = time(bc, d), ts = time(bs, d);
                std::printf("  %4ux%-4u %-9s %6zu %7.3f / %5.3f %7.3f / %5.3f\n", tw, th, name, d.cmds.size(), tc.first,
                            tc.second, ts.first, ts.second);
            }
            vkDestroyQueryPool(big.device, qp, nullptr);
        }
    }

    comp.destroy();
    sdf.destroy();
    if (validation) {
        std::printf("  validation messages: %d\n", messages);
        UI_CHECK_EQ(messages, 0);
    }
    return finish("fuse_ui_vk_sdf_quads");
}
