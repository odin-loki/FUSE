// fuse_ui_vk_budget (UI4 gate): GPU time of "ui.draw_list" per typical 1920x1080 frame, measured with timestamp
// queries around UiVkRenderer::record (10 frames back to back per submission, median of 10 submissions):
//   * full:        8 copies of the Tier A sample card (text, controls, shadows, gradients) over a gradient background
//   * blur-only:   one 600x360 frosted panel over a full-window gradient
//   * backdrop:    the 8 cards with two frosted-glass panels (blur passes)
//   * incremental: one progress-bar change redrawn through DrawList::redrawRegion (the fuse_app present path)
// Budget on GPUs: full <= 1.0 ms, backdrop <= 2.0 ms, incremental <= 0.25 ms. Software devices (lavapipe) only report.
// Skips (77) without a Vulkan device or timestamp support.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <algorithm>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
constexpr u32 W = 1920, H = 1080;

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
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.phys, &props);
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, qs.data());
    if (qs[vk.family].timestampValidBits == 0 || props.limits.timestampPeriod <= 0.f) {
        std::printf("SKIP: no timestamps on %s\n", vk.deviceName);
        return 77;
    }
    const bool software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    std::printf("device: %s%s\n", vk.deviceName, software ? " (software: report only)" : "");
    UiVkRenderer gpu;
    UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    if (!gpu.valid()) return finish("fuse_ui_vk_budget");
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    VkQueryPool qp = VK_NULL_HANDLE;
    UI_CHECK(vkCreateQueryPool(vk.device, &qpi, nullptr, &qp) == VK_SUCCESS);

    // the sample card, eight times over a 1080p window
    UiContext ui({.viewport = {470.f, 520.f}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    auto build = [&](DrawList& dl, bool backdrops) {
        dl.clear();
        dl.gradientRect({0, 0, f32(W), f32(H)}, Color::rgb(0x202838), Color::rgb(0x101018), true);
        for (int i = 0; i < 8; ++i) appendShifted(dl, ui.drawList(), f32(i % 4) * 480.f, f32(i / 4) * 540.f);
        if (backdrops) {
            dl.backdrop({200.f, 150.f, 600.f, 360.f}, 12.f, {16.f, 16.f, 16.f, 16.f}, Color::rgb(0xffffff, 0.15f));
            dl.backdrop({1100.f, 500.f, 500.f, 400.f}, 18.f, {12.f, 12.f, 12.f, 12.f}, Color::rgb(0x000000, 0.2f));
            dl.rect({240.f, 190.f, 200.f, 40.f}, Color::rgb(0xeeeeee, 0.9f), 6.f);
        }
    };
    GpuDrawData data;
    Image black, out;
    black.resize(int(W), int(H), 0xFF000000u);
    auto measure = [&](const char* what, Rect scissor, const Color* clear, const DrawList& dl, double budgetMs) {
        packDrawList(dl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, scissor, clear);
        UI_CHECK(gpu.upload(0, data));
        // kReps frames back to back per submission (keeps the clocks up), barriers between them like real frames
        constexpr int kReps = 10;
        std::vector<double> ms;
        for (int run = 0; run < 11; ++run) {
            vk.run(black, out, [&](VkCommandBuffer c) {
                vkCmdResetQueryPool(c, qp, 0, 2);
                vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
                for (int rep = 0; rep < kReps; ++rep) {
                    gpu.record((void*)c, (void*)vk.view, 0);
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                         &mb, 0, nullptr, 0, nullptr);
                }
                vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
            });
            u64 t[2] = {};
            vkGetQueryPoolResults(vk.device, qp, 0, 2, sizeof t, t, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (run > 0) ms.push_back(double(t[1] - t[0]) * props.limits.timestampPeriod * 1e-6 / kReps); // run 0: warm-up
        }
        std::sort(ms.begin(), ms.end());
        const double med = ms[ms.size() / 2];
        std::printf("  %-12s %6zu cmds, %2zu segments: median %.3f ms (min %.3f, max %.3f), budget %.2f ms\n", what,
                    data.cmds.size(), data.segments.size(), med, ms.front(), ms.back(), budgetMs);
        if (!software) UI_CHECK(med <= budgetMs);
    };

    DrawList full, frosted;
    build(full, false);
    build(frosted, true);
    measure("full", {}, nullptr, full, 1.0);
    DrawList blurOnly;
    blurOnly.gradientRect({0, 0, f32(W), f32(H)}, Color::rgb(0x3355ff), Color::rgb(0xff5533), false);
    blurOnly.backdrop({200.f, 150.f, 600.f, 360.f}, 12.f, {16.f, 16.f, 16.f, 16.f});
    measure("blur-only", {}, nullptr, blurOnly, 1.0);
    measure("backdrop", {}, nullptr, frosted, 2.0);
    // incremental: one widget changes in the first card
    ui.setValue(ui.find("progress"), 0.3f);
    ui.update();
    DrawList next;
    build(next, true);
    const Rect region = next.redrawRegion(ui.damage(), {0.f, 0.f, f32(W), f32(H)});
    const Color clear{0.f, 0.f, 0.f, 1.f};
    std::printf("  incremental region %.0fx%.0f\n", region.w, region.h);
    measure("incremental", region, &clear, next, 0.25);

    vkDestroyQueryPool(vk.device, qp, nullptr);
    gpu.destroy();
    return finish("fuse_ui_vk_budget");
}
