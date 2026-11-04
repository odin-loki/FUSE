// fuse_ui_vk UI4 gates (plan §9 UI4), one executable per gate (FUSE_UI_VK_GATE):
//   golden      GPU vs the CPU reference over whole scenes (Tier A sample, game HUD, editor-like docks), scored with
//               NVIDIA FLIP (vendor/nvidia-flip): compute kernel max channel error <= 2 and mean FLIP <= 0.001; the
//               SDF-quad backend max channel error <= 4 and mean FLIP <= 0.005. Error maps written on failure.
//   tier0       UI-T0 (plan §4.9): Vulkan 1.1 instance, a device with no features and no extensions, Khronos
//               validation on; every shader's SPIR-V declares no descriptor-indexing / buffer-address capability; the
//               scenes, a backdrop blur and an external texture render exactly like the CPU with 0 validation messages.
//   zero_alloc  the steady-state GPU frame loop (update, pack, upload, record, submit, wait; full and damage-scissored
//               frames) makes no heap allocation over 120 frames.
// Exit 77 without a Vulkan device or the compiled kernel.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#if defined(FUSE_UI_VK_GATES_HUD)
#include "../Game/samples/hud_scene.hpp"
#endif
#if defined(FUSE_UI_VK_GATES_FLIP)
#include "flip_metric.hpp"
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#ifndef FUSE_UI_VK_GATE
#define FUSE_UI_VK_GATE "golden"
#endif
#ifndef FUSE_UI_VK_GATE_OUT
#define FUSE_UI_VK_GATE_OUT "."
#endif

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
using fuse::ui::render::UiVkBackend;
using fuse::ui::render::UiVkDesc;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::render::packDrawList;
namespace w = fuse::ui::widgets;

namespace {

// ---- scenes ------------------------------------------------------------------------------------------------------------
/// An editor-like window: menu bar, tool row, docks (outliner tree, inspector property grid, console table) around a
/// centre with a tab bar, a splitter, a list and line edits.
std::shared_ptr<w::StringTableModel> buildEditor(UiContext& ui) {
    w::install(ui);
    auto model = std::make_shared<w::StringTableModel>("Level\tTime\tMessage");
    const char* levels[] = {"info", "warn", "error"};
    for (int i = 0; i < 40; ++i)
        model->addRow(std::string(levels[i % 3]) + "\t" + std::to_string(12 + i) + ".25 s\tasset streamed: mesh_" + std::to_string(i));
    const NodeHandle col = w::panel(ui, ui.root(), "editor");
    ui.setCss(col, "flex-grow", "1");
    const NodeHandle bar = w::menuBar(ui, col, "menu");
    w::menuBarMenu(ui, bar, "File", "New\nOpen\nSave", "file");
    w::menuBarMenu(ui, bar, "Edit", "Undo\nRedo", "edit");
    w::menuBarMenu(ui, bar, "View", "Docks\nFullscreen", "view");
    const NodeHandle tools = w::panel(ui, col, "tools");
    ui.addClass(tools, "row");
    for (const char* t : {"Move", "Rotate", "Scale", "Play", "Pause"}) w::toolButton(ui, tools, t);
    w::lineEdit(ui, tools, "search assets", "search");
    const NodeHandle area = w::dockArea(ui, col, "dock");
    ui.setCss(area, "flex-grow", "1");
    const NodeHandle outl = w::dockWidget(ui, area, "Outliner", w::DockSide::Left, "outliner");
    w::treeView(ui, w::dockBody(ui, outl), "World\n\tCamera\n\tLights\n\t\tSun\n\t\tFill\n\tTerrain\n\tProps\n\t\tCrate\n\t\tLantern", "tree");
    const NodeHandle insp = w::dockWidget(ui, area, "Inspector", w::DockSide::Right, "inspector");
    const NodeHandle pg = w::propertyGrid(ui, w::dockBody(ui, insp), "props");
    w::propertyRow(ui, pg, {.name = "Transform", .kind = w::PropKind::Group});
    w::propertyRow(ui, pg, {.name = "Name", .kind = w::PropKind::Text}, "Lantern");
    w::propertyRow(ui, pg, {.name = "Scale", .kind = w::PropKind::Number, .lo = 0.f, .hi = 10.f, .step = 0.5f}, "1.5");
    w::propertyRow(ui, pg, {.name = "Light", .kind = w::PropKind::Group});
    w::propertyRow(ui, pg, {.name = "Shadows", .kind = w::PropKind::Bool}, "true");
    w::propertyRow(ui, pg, {.name = "Mode", .kind = w::PropKind::Enum, .options = "Point\nSpot\nArea"}, "1");
    w::propertyRow(ui, pg, {.name = "Tint", .kind = w::PropKind::Color}, "#FFB347FF");
    const NodeHandle cons = w::dockWidget(ui, area, "Console", w::DockSide::Bottom, "console");
    w::tableView(ui, w::dockBody(ui, cons), &model->model(), "log");
    const NodeHandle centre = w::dockRegion(ui, area, w::DockSide::Center);
    w::tabBar(ui, centre, "Scene\nGame\nAnimation", 0, "tabs");
    const NodeHandle split = w::splitter(ui, centre, false, "split");
    ui.setCss(split, "flex-grow", "1");
    w::listView(ui, w::splitterPane(ui, split, 160.f), "Materials\nMeshes\nTextures\nSounds\nScripts", 2, "assets");
    const NodeHandle right = w::splitterPane(ui, split);
    w::label(ui, right, "Viewport", "vp");
    w::progressBar(ui, right, 0.4f, "bake");
    return model;
}

/// Calls fn(name, ui) for each scene, laid out and painted.
template <class F>
void forEachScene(F&& fn) {
    {
        UiContext ui({.viewport = {488.f, 400.f}, .font = &font()});
        w::install(ui);
        buildSample(ui);
        ui.update();
        fn("sample", ui);
    }
#if defined(FUSE_UI_VK_GATES_HUD)
    {
        namespace g = fuse::ui::game;
        UiContext ui({.viewport = g::demo::kHudViewport, .font = &font()});
        w::install(ui);
        g::install(ui);
        auto s = g::demo::buildHudScene(ui);
        fn("hud", ui);
    }
#endif
    {
        UiContext ui({.viewport = {1024.f, 640.f}, .font = &font()});
        const auto model = buildEditor(ui);
        ui.update();
        fn("editor", ui);
    }
}

// ---- comparison --------------------------------------------------------------------------------------------------------
int maxChannelError(const Image& a, const Image& b, std::size_t* differing = nullptr) {
    int e = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        int m = 0;
        for (int s = 0; s < 32; s += 8) m = std::max(m, std::abs(int((a.pixels[i] >> s) & 0xFF) - int((b.pixels[i] >> s) & 0xFF)));
        e = std::max(e, m);
        n += m ? 1 : 0;
    }
    if (differing) *differing = n;
    return e;
}
void writePng(const std::string& name, const Image& img) {
    const std::string path = std::string(FUSE_UI_VK_GATE_OUT) + "/" + name;
    stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    std::printf("  wrote %s\n", path.c_str());
}

/// CPU reference of the context's draw list (also owns the image views the GPU pack needs).
void cpuRender(UiContext& ui, CpuRasterizer& r, Image& out) {
    out.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.setEffects(ui.effects());
    r.render(ui.drawList(), out);
}
bool gpuRender(VkHeadless& vk, UiVkRenderer& gpu, UiContext& ui, const CpuRasterizer& r, Image& out) {
    GpuDrawData data;
    packDrawList(ui.drawList(), vk.w, vk.h, data, ui.atlas().pixels(), u32(ui.atlas().size()), r.images(), r.imageCount());
    if (!gpu.upload(0, data)) return false;
    Image init;
    init.resize(int(vk.w), int(vk.h), 0xFF000000u);
    vk.run(init, out, [&](VkCommandBuffer cmd) { gpu.record((void*)cmd, (void*)vk.view, 0); });
    return true;
}

// ---- golden ------------------------------------------------------------------------------------------------------------
int gateGolden() {
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    int skipped = 0, scenes = 0;
    forEachScene([&](const char* name, UiContext& ui) {
        ++scenes;
        VkHeadless vk;
        if (!vk.init(u32(ui.viewport().w), u32(ui.viewport().h))) { ++skipped; return; }
        if (scenes == 1) std::printf("device: %s\n", vk.deviceName);
        CpuRasterizer r;
        Image cpu;
        cpuRender(ui, r, cpu);
        if (std::getenv("FUSE_UI_VK_GOLDEN_DUMP")) writePng(std::string("vk_golden_") + name + ".png", cpu);
        struct Backend { const char* name; bool sdf; int maxErr; double maxFlip; };
        // SDF quads round once per blend (±1-2 / 255 each); stacked translucent HUD layers add up to a few steps
        const Backend backends[] = {{"compute", false, 2, 0.001}, {"sdf-quads", true, 4, 0.005}};
        for (const Backend& b : backends) {
            UiVkDesc d{.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1};
            if (b.sdf && !fuse::ui::render::useSdfQuads(d)) continue;
            UiVkRenderer gpu;
            UI_CHECK(gpu.init(d));
            Image out;
            UI_CHECK(gpu.valid() && gpuRender(vk, gpu, ui, r, out));
            if (!gpu.valid() || out.width != cpu.width) continue;
            std::size_t differing = 0;
            const int err = maxChannelError(cpu, out, &differing);
            double flipMean = 0.0;
            float flipMax = 0.f;
#if defined(FUSE_UI_VK_GATES_FLIP)
            const auto flip = fuse::content_golden::computeFlip(reinterpret_cast<const std::uint8_t*>(cpu.pixels.data()),
                                                                reinterpret_cast<const std::uint8_t*>(out.pixels.data()),
                                                                u32(cpu.width), u32(cpu.height));
            UI_CHECK(flip.valid);
            flipMean = flip.mean;
            flipMax = flip.max;
#endif
            std::printf("  %-7s %-9s %4dx%-4d max channel err %d, differing %zu, FLIP mean %.6f max %.4f\n", name, b.name,
                        cpu.width, cpu.height, err, differing, flipMean, double(flipMax));
            const bool ok = err <= b.maxErr && flipMean <= b.maxFlip;
            UI_CHECK(ok);
            if (!ok) {
                const std::string base = std::string("vk_golden_") + name + "_" + b.name;
                writePng(base + "_cpu.png", cpu);
                writePng(base + "_gpu.png", out);
#if defined(FUSE_UI_VK_GATES_FLIP)
                Image heat;
                heat.resize(cpu.width, cpu.height);
                const auto rgba = fuse::content_golden::flipHeatmap(flip.map, u32(cpu.width), u32(cpu.height));
                std::memcpy(heat.pixels.data(), rgba.data(), rgba.size());
                writePng(base + "_flip.png", heat);
#endif
            }
        }
    });
    if (skipped == scenes) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    return finish("fuse_ui_vk_golden");
}

// ---- tier0 -------------------------------------------------------------------------------------------------------------
/// SPIR-V capabilities that need a device feature or extension UI-T0 does not enable (descriptor indexing, dynamic
/// indexing of descriptor arrays, buffer device address, formatless storage images).
bool forbiddenCapability(u32 c) {
    return (c >= 28 && c <= 31) || c == 55 || c == 56 || (c >= 5301 && c <= 5312) || c == 5347;
}
int checkSpirv(const char* what, std::span<const u32> spv) {
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        std::printf("  %s: no SPIR-V\n", what);
        return 1;
    }
    int bad = 0;
    std::string caps;
    for (std::size_t i = 5; i < spv.size();) {
        const u32 op = spv[i] & 0xFFFFu, wc = spv[i] >> 16;
        if (wc == 0) break;
        if (op == 17 && i + 1 < spv.size()) { // OpCapability
            caps += " " + std::to_string(spv[i + 1]);
            if (forbiddenCapability(spv[i + 1])) ++bad;
        }
        i += wc;
    }
    const u32 ver = spv[1];
    std::printf("  %-14s SPIR-V %u.%u, capabilities%s%s\n", what, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF, caps.c_str(),
                bad ? " (needs a feature beyond UI-T0)" : "");
    return bad + (ver > 0x00010300u ? 1 : 0); // Vulkan 1.1 consumes SPIR-V <= 1.3
}

/// An 8x8 sampled checker on the device, bound to external slot 0, against the CPU raster of the same pixels.
void externalCase(VkHeadless& vk, UiVkRenderer& gpu) {
    Image opaque;
    opaque.resize(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) opaque.pixels[std::size_t(y) * 8 + std::size_t(x)] = ((x ^ y) & 1) ? 0xFF2080F0u : 0xFF404040u;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {8, 8, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage ext = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    UI_CHECK(vkCreateImage(vk.device, &ii, nullptr, &ext) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, ext, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = vk.memType(req.memoryTypeBits, 0);
    UI_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &mem) == VK_SUCCESS);
    vkBindImageMemory(vk.device, ext, mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = ext;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    UI_CHECK(vkCreateImageView(vk.device, &vci, nullptr, &view) == VK_SUCCESS);
    std::memcpy(vk.stagingPtr, opaque.pixels.data(), opaque.pixels.size() * 4);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = vk.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(vk.device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = ext;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {8, 8, 1};
    vkCmdCopyBufferToImage(cmd, vk.staging, ext, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue);
    vkFreeCommandBuffers(vk.device, vk.pool, 1, &cmd);

    Image img = opaque;
    img.externalSlot = 0;
    DrawList dl;
    dl.rect({0, 0, f32(vk.w), f32(vk.h)}, Color::rgb(0x202020));
    DrawCmd e;
    e.op = DrawOp::Image;
    e.rect = {40, 30, 400, 300};
    dl.push(e);
    dl.rect({100, 100, 60, 40}, Color::rgb(0xffcc00, 0.5f));
    Image cpu, out, init;
    cpu.resize(int(vk.w), int(vk.h), 0xFF000000u);
    init.resize(int(vk.w), int(vk.h), 0xFF000000u);
    CpuRasterizer r;
    r.addImage(&img);
    r.render(dl, cpu);
    const Image* imgs[1] = {&img};
    GpuDrawData data;
    packDrawList(dl, vk.w, vk.h, data, nullptr, 0, imgs, 1);
    gpu.setExternalImage(0, (void*)view);
    UI_CHECK(gpu.upload(0, data));
    vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
    const int err = maxChannelError(cpu, out);
    std::printf("  external texture: max channel err %d\n", err);
    UI_CHECK_EQ(err, 0);
    gpu.setExternalImage(0, nullptr);
    vkDeviceWaitIdle(vk.device);
    vkDestroyImageView(vk.device, view, nullptr);
    vkDestroyImage(vk.device, ext, nullptr);
    vkFreeMemory(vk.device, mem, nullptr);
}

int gateTier0() {
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("SKIP: kernel not compiled in\n");
        return 77;
    }
    // static: no shader asks for more than a Vulkan 1.1 device without features
    int bad = checkSpirv("draw_list", UiVkRenderer::kernelSpirv());
    for (const char* fx : {"desaturate", "ripple", "grain"}) {
        const auto spv = UiVkRenderer::builtinEffectSpirv(fx);
        if (!spv.empty()) bad += checkSpirv(fx, spv);
    }
    UiVkDesc sdfShaders{};
    if (fuse::ui::render::useSdfQuads(sdfShaders)) {
        bad += checkSpirv("sdf_quad.vert", sdfShaders.sdfVertex);
        bad += checkSpirv("sdf_quad.frag", sdfShaders.sdfFragment);
    }
    UI_CHECK_EQ(bad, 0);

    const bool validation = VkHeadless::validationAvailable();
    VkHeadless::validationRequested() = validation;
    if (!validation) std::printf("  VK_LAYER_KHRONOS_validation not installed: running without it\n");
    int skipped = 0, scenes = 0;
    bool printed = false;
    auto deviceChecks = [&](VkHeadless& vk) {
        UI_CHECK(VkHeadless::kApiVersion == VK_API_VERSION_1_1);
        UI_CHECK_EQ(vk.deviceExtensionCount, 0u);
        UI_CHECK(!vk.deviceFeaturesEnabled);
        if (printed) return;
        printed = true;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(vk.phys, &p);
        std::printf("device: %s (Vulkan %u.%u), instance 1.1, device features: none, extensions: none, validation %s\n",
                    vk.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), validation ? "on" : "off");
    };
    forEachScene([&](const char* name, UiContext& ui) {
        ++scenes;
        VkHeadless vk;
        if (!vk.init(u32(ui.viewport().w), u32(ui.viewport().h))) { ++skipped; return; }
        deviceChecks(vk);
        CpuRasterizer r;
        Image cpu, out;
        cpuRender(ui, r, cpu);
        UiVkRenderer gpu;
        UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1}));
        if (!gpu.valid() || !gpuRender(vk, gpu, ui, r, out)) { UI_CHECK(false); return; }
        const int err = maxChannelError(cpu, out);
        std::printf("  %-7s compute: max channel err %d\n", name, err);
        UI_CHECK_EQ(err, 0);
        if (std::string(name) == "sample") {
            UiVkDesc d{.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1};
            if (fuse::ui::render::useSdfQuads(d)) { // render pass + graphics pipeline: still Vulkan 1.0 / 1.1 core
                UiVkRenderer sdf;
                UI_CHECK(sdf.init(d));
                Image s;
                if (sdf.valid() && gpuRender(vk, sdf, ui, r, s)) {
                    const int se = maxChannelError(cpu, s);
                    std::printf("  %-7s sdf-quads: max channel err %d\n", name, se);
                    UI_CHECK(se <= 2);
                }
            }
        }
    });
    if (skipped == scenes) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    // backdrop blur passes and an external (zero-copy) texture slot
    {
        constexpr u32 W = 488, H = 400;
        VkHeadless vk;
        if (vk.init(W, H)) {
            deviceChecks(vk);
            UiVkRenderer gpu;
            UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1}));
            DrawList bl;
            for (int i = 0; i < 25; ++i) bl.rect({f32(i * 20), 0.f, 20.f, 400.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
            bl.line({0, 390}, {488, 210}, Color::rgb(0x33ff99), 4.f);
            bl.backdrop({60.f, 40.f, 200.f, 120.f}, 12.f, {16.f, 16.f, 16.f, 16.f});
            bl.pushClip({250.f, 180.f, 200.f, 180.f}, 20.f);
            bl.backdrop({230.5f, 170.25f, 240.f, 150.f}, 20.f, {8.f, 8.f, 8.f, 8.f}, Color::rgb(0xffffff, 0.25f));
            bl.popClip();
            bl.rect({300.f, 300.f, 120.f, 40.f}, Color::rgb(0x112233, 0.8f), 10.f);
            Image cpu, out, init;
            cpu.resize(int(W), int(H), 0xFF000000u);
            init.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer().render(bl, cpu);
            GpuDrawData data;
            packDrawList(bl, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            const int err = maxChannelError(cpu, out);
            std::printf("  backdrop blur: max channel err %d\n", err);
            UI_CHECK_EQ(err, 0);
            externalCase(vk, gpu);
        }
    }
    const int messages = VkHeadless::validationMessages();
    std::printf("validation messages: %d\n", messages);
    UI_CHECK_EQ(messages, 0);
    return finish("fuse_ui_vk_tier0");
}

// ---- zero_alloc --------------------------------------------------------------------------------------------------------
int gateZeroAlloc() {
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
    if (!gpu.valid()) return finish("fuse_ui_vk_zero_alloc");
    UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
    w::install(ui);
    buildSample(ui);
    ui.update();
    const Rect a = ui.rect(ui.find("apply")), c = ui.rect(ui.find("cancel"));
    const Vec2 pa{a.x + a.w * 0.5f, a.y + a.h * 0.5f}, pc{c.x + c.w * 0.5f, c.y + c.h * 0.5f};
    const NodeHandle progress = ui.find("progress");

    // everything a frame needs, created up front: two command buffers + fences (one per ring slot)
    VkCommandBuffer cmds[2];
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = vk.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    vkAllocateCommandBuffers(vk.device, &cai, cmds);
    VkFence fences[2];
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (VkFence& f : fences) vkCreateFence(vk.device, &fci, nullptr, &f);
    VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.srcQueueFamilyIndex = toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = vk.image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkMemoryBarrier frameDep{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; // previous frame's writes before this frame's blend
    frameDep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    frameDep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    GpuDrawData data;
    const Color black{0.f, 0.f, 0.f, 1.f};
    const Rect whole{0.f, 0.f, f32(W), f32(H)};
    u64 frame = 0;
    u32 incremental = 0;
    auto runFrame = [&](bool full) {
        const u32 slot = u32(frame & 1);
        vkWaitForFences(vk.device, 1, &fences[slot], VK_TRUE, UINT64_MAX);
        vkResetFences(vk.device, 1, &fences[slot]);
        // a hover moves between two buttons and the progress bar changes: repaints, no structural change
        ui.pointerMove(frame & 1 ? pc : pa);
        ui.setValue(progress, (frame & 2) ? 0.3f : 0.7f);
        ui.update(1.f / 60.f);
        Rect dmg = full ? Rect{} : ui.drawList().redrawRegion(ui.damage(), whole);
        if (!full && dmg.empty()) dmg = whole;
        incremental += full ? 0u : 1u;
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, dmg,
                     full ? nullptr : &black);
        UI_CHECK(gpu.upload(slot, data));
        VkCommandBuffer cmd = cmds[slot];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        if (frame == 0)
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &toGeneral);
        else
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &frameDep,
                                 0, nullptr, 0, nullptr);
        gpu.record((void*)cmd, (void*)vk.view, slot);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(vk.queue, 1, &si, fences[slot]);
        ++frame;
    };
    for (int i = 0; i < 20; ++i) runFrame(i < 2); // warm-up: capacities, glyphs, pipeline state
    incremental = 0;
    const auto before = g_allocs.load();
    for (int i = 0; i < 120; ++i) runFrame(i % 10 == 0);
    const auto allocs = g_allocs.load() - before;
    vkWaitForFences(vk.device, 2, fences, VK_TRUE, UINT64_MAX);
    std::printf("steady state: %d frames (%u incremental), %llu heap allocation(s)\n", 120, incremental, allocs);
    UI_CHECK_EQ(allocs, 0ull);

    // the incrementally maintained image still matches a full CPU raster of the last frame
    {
        VkCommandBuffer cmd = cmds[0];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier b = toGeneral;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.staging, 1, &copy);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.queue);
        Image gpuImg, cpu;
        gpuImg.resize(int(W), int(H));
        std::memcpy(gpuImg.pixels.data(), vk.stagingPtr, std::size_t(W) * H * 4);
        fuse::ui::test::render(ui, cpu);
        const int err = maxChannelError(cpu, gpuImg);
        std::printf("after %llu frames: GPU image vs CPU raster max channel err %d\n", (unsigned long long)frame, err);
        UI_CHECK(err <= 2);
    }
    for (VkFence f : fences) vkDestroyFence(vk.device, f, nullptr);
    vkFreeCommandBuffers(vk.device, vk.pool, 2, cmds);
    return finish("fuse_ui_vk_zero_alloc");
}

} // namespace

int main() {
    const std::string gate = FUSE_UI_VK_GATE;
    if (gate == "golden") return gateGolden();
    if (gate == "tier0") return gateTier0();
    if (gate == "zero_alloc") return gateZeroAlloc();
    std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
    return 2;
}
