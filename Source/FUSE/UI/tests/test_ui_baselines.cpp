// fuse_ui_baselines (plan §6 / UI0 baselines, report only): measures what the §6 budgets are judged against on this
// machine and prints it —
//   * CPU reference rasteriser time per frame: the Tier A sample (488 x 400), the 1080p "8 cards" frame of
//     fuse_ui_vk_budget, the UI7 HUD scene at 1440p (scaled from 960 x 540)
//   * GPU time of the same frames on the compute tile kernel and the SDF-quad backend (timestamp queries)
//   * memory: glyph atlas, icon pack atlases, draw-list and packed GPU data per frame, renderer device memory
// Never fails on numbers (the gates hold the budgets); skips the GPU part without a Vulkan device.
#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/icons.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include "../Game/samples/hud_scene.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
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
struct Frame {
    const char* name;
    u32 w, h;
    const DrawList* list;
    const u8* atlas;
    u32 atlasSize;
};
double cpuMs(const Frame& f) {
    CpuRasterizer r;
    r.setGlyphAtlas(f.atlas, int(f.atlasSize));
    Image img;
    std::vector<double> ms;
    for (int i = 0; i < 11; ++i) {
        img.resize(int(f.w), int(f.h), 0xFF000000u);
        const auto t0 = std::chrono::steady_clock::now();
        r.render(*f.list, img);
        const auto t1 = std::chrono::steady_clock::now();
        if (i > 0) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}
double kib(std::size_t b) { return double(b) / 1024.0; }
} // namespace

int main() {
    // frames
    UiContext sample({.viewport = {488.f, 400.f}, .font = &font()});
    widgets::install(sample);
    buildSample(sample);
    sample.update();
    UiContext card({.viewport = {470.f, 520.f}, .font = &font()});
    widgets::install(card);
    buildSample(card);
    card.update();
    DrawList cards;
    cards.gradientRect({0, 0, 1920.f, 1080.f}, Color::rgb(0x202838), Color::rgb(0x101018), true);
    for (int i = 0; i < 8; ++i) appendShifted(cards, card.drawList(), f32(i % 4) * 480.f, f32(i / 4) * 540.f);
    UiContext hud({.viewport = game::demo::kHudViewport, .font = &font()});
    widgets::install(hud);
    game::install(hud);
    auto scene = game::demo::buildHudScene(hud);
    hud.setScale(2560.f / game::demo::kHudViewport.w); // 960 x 540 -> 2560 x 1440 device pixels
    hud.update();
    const Frame frames[] = {
        {"sample", 488, 400, &sample.drawList(), sample.atlas().pixels(), u32(sample.atlas().size())},
        {"cards-1080p", 1920, 1080, &cards, card.atlas().pixels(), u32(card.atlas().size())},
        {"hud-1440p", 2560, 1440, &hud.drawList(), hud.atlas().pixels(), u32(hud.atlas().size())},
    };

    std::printf("fuse_ui_baselines (report only)\n  CPU reference rasteriser, median of 10 frames:\n");
    for (const Frame& f : frames)
        std::printf("    %-12s %4ux%-4u %5zu cmds  %8.3f ms\n", f.name, f.w, f.h, f.list->size(), cpuMs(f));

    // memory
    std::printf("  memory:\n");
    std::printf("    glyph atlas (per UiContext)      %8.1f KiB (%dx%d R8)\n", kib(std::size_t(sample.atlas().size()) * std::size_t(sample.atlas().size())),
                sample.atlas().size(), sample.atlas().size());
#ifdef FUSE_UI_ICON_PACK
    if (auto set = IconSet::fromFile(FUSE_UI_ICON_PACK))
        std::printf("    icon pack atlas (%2u icons x %zu sizes) %6.1f KiB (%dx%d R8)\n", set->count(), set->sizes().size(),
                    kib(std::size_t(set->atlasWidth()) * std::size_t(set->atlasHeight())), set->atlasWidth(), set->atlasHeight());
#endif
    for (const Frame& f : frames) {
        GpuDrawData d;
        packDrawList(*f.list, f.w, f.h, d, f.atlas, f.atlasSize);
        std::printf("    %-12s draw list %7.1f KiB, packed GPU data %7.1f KiB (+ %.0f KiB atlas upload)\n", f.name,
                    kib(f.list->size() * sizeof(DrawCmd)), kib(d.cmdBytes() + d.tileBytes() + d.imageBytes()), kib(d.atlasBytes()));
    }

    // GPU
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("  GPU: kernel not compiled in\n");
        return finish("fuse_ui_baselines");
    }
    for (const Frame& f : frames) {
        VkHeadless vk;
        if (!vk.init(f.w, f.h)) {
            std::printf("  GPU: no Vulkan device\n");
            break;
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk.phys, &props);
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, qs.data());
        if (qs[vk.family].timestampValidBits == 0 || props.limits.timestampPeriod <= 0.f) {
            std::printf("  GPU: no timestamps\n");
            break;
        }
        UiVkRenderer comp, sdf;
        UI_CHECK(comp.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1}));
        fuse::ui::render::UiVkDesc sd{.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1};
        const bool haveSdf = fuse::ui::render::useSdfQuads(sd) && sdf.init(sd);
        if (&f == &frames[0]) {
            std::printf("  GPU (%s), median of 10 x 10 frames:\n", vk.deviceName);
            UiVkRenderer def;
            UI_CHECK(def.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device}));
            std::printf("    renderer device memory (default limits, 2 frames in flight): %.1f MiB\n",
                        double(def.deviceMemoryBytes()) / 1048576.0);
        }
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        VkQueryPool qp = VK_NULL_HANDLE;
        UI_CHECK(vkCreateQueryPool(vk.device, &qpi, nullptr, &qp) == VK_SUCCESS);
        GpuDrawData d;
        packDrawList(*f.list, f.w, f.h, d, f.atlas, f.atlasSize);
        Image base, out;
        base.resize(int(f.w), int(f.h), 0xFF000000u);
        auto time = [&](UiVkRenderer& r) {
            if (!r.upload(0, d)) return -1.0;
            std::vector<double> ms;
            for (int run = 0; run < 11; ++run) {
                vk.run(base, out, [&](VkCommandBuffer c) {
                    vkCmdResetQueryPool(c, qp, 0, 2);
                    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
                    for (int rep = 0; rep < 10; ++rep) {
                        r.record((void*)c, (void*)vk.view, 0);
                        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                                             nullptr, 0, nullptr);
                    }
                    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
                });
                u64 t[2] = {};
                vkGetQueryPoolResults(vk.device, qp, 0, 2, sizeof t, t, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (run > 0) ms.push_back(double(t[1] - t[0]) * props.limits.timestampPeriod * 1e-6 / 10.0);
            }
            std::sort(ms.begin(), ms.end());
            return ms[ms.size() / 2];
        };
        const double tc = time(comp), ts = haveSdf ? time(sdf) : -1.0;
        std::printf("    %-12s %4ux%-4u compute %7.3f ms   sdf quads %7.3f ms\n", f.name, f.w, f.h, tc, ts);
        vkDestroyQueryPool(vk.device, qp, nullptr);
    }
    (void)scene;
    return finish("fuse_ui_baselines");
}
