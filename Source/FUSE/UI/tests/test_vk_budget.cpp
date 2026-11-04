// fuse_ui_vk_budget (plan §6 "HUD GPU time at 1440p ≤ 0.5 ms"): GPU time of the fuse_ui_vk "ui.draw_list" kernel for
// a ~300-node game HUD (corner panels with labels and bars, a top bar, a hotbar) on a 2560x1440 target, measured with
// timestamp queries around UiVkRenderer::record over 60 frames (median). The budget is enforced on hardware GPUs;
// CPU devices (lavapipe / SwiftShader) report only. Skips (77) without a Vulkan device or timestamp support.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;

namespace {
/// A game HUD: four corner panels (status rows with bars), a top bar of labels and a hotbar of tool buttons.
void buildHud(UiContext& ui) {
    using namespace widgets;
    ui.setCss(ui.root(), "padding", "32px");
    for (int c = 0; c < 4; ++c) {
        const NodeHandle p = panel(ui, ui.root());
        ui.addClass(p, "card");
        ui.setCss(p, "position", "absolute");
        ui.setCss(p, c < 2 ? "top" : "bottom", "96px");
        ui.setCss(p, (c & 1) ? "right" : "left", "32px");
        ui.setCss(p, "width", "420px");
        for (int r = 0; r < 14; ++r) {
            const NodeHandle row = panel(ui, p);
            ui.addClass(row, "row");
            label(ui, row, "Stat " + std::to_string(c * 14 + r));
            const NodeHandle bar = progressBar(ui, row, 0.1f + 0.06f * float(r));
            ui.setCss(bar, "flex-grow", "1");
            label(ui, row, std::to_string(100 + r * 7));
        }
    }
    const NodeHandle top = panel(ui, ui.root());
    ui.addClass(top, "row");
    for (int i = 0; i < 24; ++i) label(ui, top, "Objective " + std::to_string(i));
    const NodeHandle hotbar = panel(ui, ui.root());
    ui.addClass(hotbar, "row");
    ui.setCss(hotbar, "position", "absolute");
    ui.setCss(hotbar, "bottom", "32px");
    ui.setCss(hotbar, "left", "900px");
    for (int i = 0; i < 12; ++i) toolButton(ui, hotbar, std::to_string(i + 1));
}
} // namespace

int main() {
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    constexpr u32 W = 2560, H = 1440;
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.phys, &props);
    if (props.limits.timestampPeriod <= 0.f) {
        std::printf("SKIP: %s has no timestamps\n", vk.deviceName);
        return 77;
    }
    const bool hardware = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
                          props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    UiVkRenderer gpu;
    UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    if (!gpu.valid()) return finish("fuse_ui_vk_budget");

    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    widgets::install(ui);
    buildHud(ui);
    ui.update();
    GpuDrawData data;
    packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
    UI_CHECK(gpu.upload(0, data));

    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    VkQueryPool pool = VK_NULL_HANDLE;
    UI_CHECK(vkCreateQueryPool(vk.device, &qpi, nullptr, &pool) == VK_SUCCESS);
    Image init, out;
    init.resize(int(W), int(H), 0xFF202020u);
    std::vector<double> ms;
    for (int frame = 0; frame < 64; ++frame) {
        vk.run(init, out, [&](VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd, pool, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
            gpu.record((void*)cmd, (void*)vk.view, 0);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
        });
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(vk.device, pool, 0, 2, sizeof ts, ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
            frame >= 4) // warm-up frames excluded
            ms.push_back(double(ts[1] - ts[0]) * double(props.limits.timestampPeriod) * 1e-6);
    }
    vkDestroyQueryPool(vk.device, pool, nullptr);
    UI_CHECK(!ms.empty());
    if (ms.empty()) return finish("fuse_ui_vk_budget");
    std::sort(ms.begin(), ms.end());
    const double median = ms[ms.size() / 2], p95 = ms[ms.size() * 95 / 100];
    // the HUD is really drawn: a good share of the target differs from the clear colour
    const auto drawn = std::count_if(out.pixels.begin(), out.pixels.end(), [](u32 p) { return p != 0xFF202020u; });
    UI_CHECK(out.width == int(W) && drawn > std::ptrdiff_t(W) * H / 50);
    std::printf("fuse_ui_vk_budget: %s (%s), %u nodes, %zu draw commands, 2560x1440: median %.3f ms, p95 %.3f ms GPU "
                "(budget 0.5 ms%s)\n",
                vk.deviceName, hardware ? "hardware" : "CPU device", ui.nodeCount(), ui.drawList().cmds().size(), median,
                p95, hardware ? "" : ", report only");
    if (hardware) UI_CHECK(median <= 0.5);
    gpu.destroy();
    return finish("fuse_ui_vk_budget");
}
