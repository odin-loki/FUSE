// fuse_ui_vk_parity (UI4 gate): the GPU backend ("ui.draw_list" compute kernel) against the CPU reference rasteriser
// (the specification), on the Tier A sample, a primitives sheet (gradients, borders, shadows, lines, nested rounded
// clips, images, glyphs), a scissored redraw, an external texture and backdrop blurs (full + scissored). Gate: max channel error <= 2/255 and < 0.5 % of pixels differing at
// all. Also: packing + upload of an unchanged frame does not allocate. Skips (77) without a Vulkan device.
#include "fuse/ui/icons.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
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

#ifdef FUSE_UI_ICON_PACK
    // 6. icons (cooked .fuseicons, plan UI4): Glyph commands over icon coverage in the context's glyph atlas, cooked
    //    and resampled sizes, three tints; the kernel needs nothing new
    if (auto set = IconSet::fromFile(FUSE_UI_ICON_PACK)) {
        UiContext iu({.viewport = {f32(W), f32(H)}, .font = &font()});
        widgets::install(iu);
        iu.setIcons(&*set);
        (void)iu.addStyleSheet(".row { flex-direction: row; gap: 4px; } .a { color: #3d8bfd; } .b { color: rgba(255, 204, 0, 0.6); }");
        const char* tints[] = {"", "a", "b"};
        for (int size : {16, 20, 24, 32, 40, 48}) {
            const NodeHandle row = widgets::panel(iu, iu.root());
            iu.addClass(row, "row");
            for (u32 i = 0; i < 9; ++i) {
                const NodeHandle n = widgets::icon(iu, row, set->name((u32(size) + i * 5) % set->count()), f32(size));
                if (*tints[i % 3]) iu.addClass(n, tints[i % 3]);
            }
        }
        iu.update();
        Image cpu6;
        fuse::ui::test::render(iu, cpu6);
        init.resize(int(W), int(H), 0xFF000000u);
        packDrawList(iu.drawList(), W, H, data, iu.atlas().pixels(), u32(iu.atlas().size()));
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer cmd) { gpu.record((void*)cmd, (void*)vk.view, 0); });
        check("icons", compare(cpu6, out));
    } else {
        std::fprintf(stderr, "icon pack: %s\n", set.error().c_str());
        UI_CHECK(false);
    }
#endif

    // 5. external texture slot (zero-copy engine frames): an opaque checker as a sampled GPU image bound to slot 0;
    //    the Image carries externalSlot = 0 so packDrawList ships no pixels and the kernel samples the texture. The
    //    CPU reference rasterises the same checker from Image::pixels.
    {
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
        VkDeviceMemory extMem = VK_NULL_HANDLE;
        VkImageView extView = VK_NULL_HANDLE;
        UI_CHECK(vkCreateImage(vk.device, &ii, nullptr, &ext) == VK_SUCCESS);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, ext, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = vk.memType(req.memoryTypeBits, 0);
        UI_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &extMem) == VK_SUCCESS);
        vkBindImageMemory(vk.device, ext, extMem, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = ext;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        UI_CHECK(vkCreateImageView(vk.device, &vci, nullptr, &extView) == VK_SUCCESS);
        // upload the checker through the harness staging buffer (one-off submit)
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

        Image extImg = opaque;
        extImg.externalSlot = 0;
        DrawList el;
        el.rect({0, 0, 488, 400}, Color::rgb(0x202020));
        DrawCmd e1;
        e1.op = DrawOp::Image;
        e1.rect = {40, 30, 400, 300}; // scaled 8x8 -> 400x300 (nearest)
        el.push(e1);
        el.rect({100, 100, 60, 40}, Color::rgb(0xffcc00, 0.5f)); // UI drawn over the external frame
        Image cpu5;
        cpu5.resize(int(W), int(H), 0xFF000000u);
        init.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer r5;
        r5.addImage(&extImg);
        r5.render(el, cpu5);
        const Image* eimgs[1] = {&extImg};
        packDrawList(el, W, H, data, nullptr, 0, eimgs, 1);
        UI_CHECK_EQ(data.images[3], 1u);         // slot 0 + 1
        UI_CHECK(data.images.size() <= 5u);       // table + 1 placeholder word: no pixel copy
        gpu.setExternalImage(0, (void*)extView);
        UI_CHECK(gpu.externalImage(0) == (void*)extView);
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("external", compare(cpu5, out));
        UI_CHECK_EQ(out.at(45, 35), 0xFF404040u);  // checker texel (0,0) straight from the GPU texture
        // unbind before destroying (later records bind the 1x1 dummy)
        gpu.setExternalImage(0, nullptr);
        vkDeviceWaitIdle(vk.device);
        vkDestroyImageView(vk.device, extView, nullptr);
        vkDestroyImage(vk.device, ext, nullptr);
        vkFreeMemory(vk.device, extMem, nullptr);
    }

    // 6. backdrop blur (frosted glass): stripes + dots, then a plain backdrop, a tinted one inside a rounded clip,
    //    one touching the target edge, and UI drawn over them (segments continue after each blur)
    DrawList bl;
    for (int i = 0; i < 25; ++i)
        bl.rect({f32(i * 20), 0.f, 20.f, 400.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
    for (int i = 0; i < 12; ++i) bl.rect({10.f + f32(i) * 40.f, 150.f, 18.f, 18.f}, Color::rgb(0xF0F0F0), 9.f);
    bl.line({0, 390}, {488, 210}, Color::rgb(0x33ff99), 4.f);
    bl.backdrop({60.f, 40.f, 200.f, 120.f}, 12.f, {16.f, 16.f, 16.f, 16.f});
    bl.rect({80.f, 60.f, 60.f, 24.f}, Color::rgb(0xffffff, 0.6f), 6.f);
    bl.pushClip({250.f, 180.f, 200.f, 180.f}, 20.f);
    bl.backdrop({230.5f, 170.25f, 240.f, 150.f}, 20.f, {8.f, 8.f, 8.f, 8.f}, Color::rgb(0xffffff, 0.25f));
    bl.popClip();
    bl.backdrop({400.f, 0.f, 88.f, 90.f}, 7.f, {0.f, 0.f, 0.f, 0.f}, Color::rgb(0x000000, 0.3f));
    bl.rect({300.f, 300.f, 120.f, 40.f}, Color::rgb(0x112233, 0.8f), 10.f);
    {
        Image cpu6;
        cpu6.resize(int(W), int(H), 0xFF000000u);
        init.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer().render(bl, cpu6);
        packDrawList(bl, W, H, data);
        UI_CHECK_EQ(data.segments.size(), std::size_t(4));
        UI_CHECK(gpu.upload(1, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        check("backdrop", compare(cpu6, out));
        // scissored redraw with a backdrop over existing content
        const Rect bsc{120, 100, 250, 200};
        Image cpu7 = cpu2;
        CpuRasterizer().render(bl, cpu7, bsc);
        packDrawList(bl, W, H, data, nullptr, 0, nullptr, 0, bsc);
        UI_CHECK(gpu.upload(0, data));
        vk.run(cpu2, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("backdrop-sc", compare(cpu7, out));
    }

    // 4. steady state: packing + uploading the same frame again does not allocate
    packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
    packDrawList(bl, W, H, data);
    const auto before = g_allocs.load();
    for (int i = 0; i < 10; ++i) {
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(gpu.upload(u32(i & 1), data));
        packDrawList(bl, W, H, data);
        UI_CHECK(gpu.upload(u32(i & 1), data));
    }
    UI_CHECK_EQ(g_allocs.load() - before, 0ull);
    gpu.destroy();

    // 6. persistent pipeline cache: miss writes the file, the next init hits it, a corrupted file is rejected and
    //    rewritten, and every variant still renders
    {
        using fuse::ui::render::PipelineCacheUse;
        const std::filesystem::path cacheFile = std::filesystem::temp_directory_path() / "fuse_ui_vk_parity.vkpipelinecache";
        std::error_code ec;
        std::filesystem::remove(cacheFile, ec);
        const std::string path = cacheFile.string();
        const auto initWith = [&](UiVkRenderer& r) {
            return r.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1,
                           .pipelineCachePath = path.c_str()});
        };
        UiVkRenderer a;
        UI_CHECK(initWith(a) && a.pipelineCacheUse() == PipelineCacheUse::Miss);
        a.destroy();
        UI_CHECK(std::filesystem::file_size(cacheFile, ec) > 32u);
        UiVkRenderer b;
        UI_CHECK(initWith(b) && b.pipelineCacheUse() == PipelineCacheUse::Hit);
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(b.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer cmd) { b.record((void*)cmd, (void*)vk.view, 0); });
        check("cached", compare(cpu, out));
        std::printf("pipeline cache: miss then hit (%.2f ms with the cache)\n", b.pipelineMs());
        b.destroy();
        if (std::FILE* f = std::fopen(path.c_str(), "r+b")) { // break the driver blob's device UUID
            std::fseek(f, 24 + 16, SEEK_SET); // our 24-byte header, then the blob's UUID at 16
            const int byte = std::fgetc(f);
            std::fseek(f, 24 + 16, SEEK_SET);
            std::fputc(0x5A ^ byte, f);
            std::fclose(f);
        }
        UiVkRenderer c;
        UI_CHECK(initWith(c) && c.pipelineCacheUse() == PipelineCacheUse::Rejected);
        c.destroy();
        UiVkRenderer d;
        UI_CHECK(initWith(d) && d.pipelineCacheUse() == PipelineCacheUse::Hit);
        d.destroy();
        std::filesystem::remove(cacheFile, ec);
    }
    return finish("fuse_ui_vk_parity");
}
