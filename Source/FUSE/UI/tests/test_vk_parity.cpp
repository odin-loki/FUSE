// fuse_ui_vk_parity (UI4 gate): the GPU backend ("ui.draw_list" compute kernel) against the CPU reference rasteriser
// (the specification), on the Tier A sample, a primitives sheet (gradients, borders, shadows, lines, nested rounded
// clips, images, glyphs) and a scissored redraw. Gate: max channel error <= 2/255 and < 0.5 % of pixels differing at
// all. Also: packing + upload of an unchanged frame does not allocate. Skips (77) without a Vulkan device.
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"
#if FUSE_PARITY_EXTRAS
#include "fuse/ui/extras/datavis3d.hpp"
#include "fuse/ui/extras/svg.hpp"
#endif

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

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

    // 6. backdrop blur on the GPU (ui.backdrop between draw segments), incl. a second blur over the first and a
    //    scissored (damage) repaint that clears its box like presentHeadless
    UI_CHECK(gpu.backdropOnGpu());
    {
        DrawList bl;
        for (int i = 0; i < 12; ++i)
            bl.rect({f32(i * 40), 0, 20, 400}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
        bl.gradientRect({0, 150, 488, 60}, Color::rgb(0xffffff), Color::rgb(0x000000), true);
        bl.backdrop({60, 60, 240, 200}, 12.f, {20, 20, 20, 20}, Color::rgb(0xffffff, 0.25f));
        bl.rect({80, 80, 40, 40}, Color::rgb(0x00ff00));
        bl.backdrop({200, 120, 220, 220}, 6.f, {8, 8, 8, 8});
        bl.rect({400, 300, 60, 60}, Color::rgb(0xffff00, 0.5f));
        Image cpu6;
        cpu6.resize(int(W), int(H), 0xFF000000u);
        init.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer r6;
        r6.render(bl, cpu6);
        packDrawList(bl, W, H, data);
        UI_CHECK_EQ(data.passes.size(), 5u); // draw, blur, draw, blur, draw
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("backdrop", compare(cpu6, out));

        // damage repaint: clear the box to opaque black (kPassClear) and redraw only inside it, over old content
        const Rect sc6{150, 90, 180, 170};
        Image prev = cpu2; // unrelated earlier frame as canvas history
        Image cpu7 = prev;
        for (int y = 90; y < 260; ++y)
            for (int x = 150; x < 330; ++x) cpu7.pixels[std::size_t(y) * W + std::size_t(x)] = 0xFF000000u;
        r6.render(bl, cpu7, sc6);
        packDrawList(bl, W, H, data, nullptr, 0, nullptr, 0, sc6);
        data.passes[0].flags |= fuse::ui::render::kPassClear;
        UI_CHECK_EQ(data.push.origin[0], 150u);
        UI_CHECK_EQ(data.push.size[0], 180u);
        UI_CHECK(gpu.upload(1, data));
        vk.run(prev, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        check("damage-blur", compare(cpu7, out));
        UI_CHECK_EQ(out.at(5, 5), prev.at(5, 5)); // outside the box: untouched
    }

    // 8. vector paths: SVG fill (non-zero + even-odd), stroke over a gradient fill, radial gradients, a clipped path
    {
        DrawList pl;
        pl.rect({0, 0, 488, 400}, Color::rgb(0x181818));
        pl.path(Path::fromSvg("M40 40 L200 60 L120 200 Z M60 60 L150 80 L110 150 Z"), Color::rgb(0x44aaff), false);
        pl.path(Path::fromSvg("M240 40 l160 20 l-80 140 z m20 20 l90 20 l-40 70 z"), Color::rgb(0xffaa44, 0.8f), true);
        Path star;
        for (int i = 0; i < 10; ++i) {
            const f32 a = f32(i) * 0.6283185f - 1.5707963f, r = i & 1 ? 30.f : 70.f;
            if (i == 0) star.moveTo({120 + r * std::cos(a), 300 + r * std::sin(a)});
            else star.lineTo({120 + r * std::cos(a), 300 + r * std::sin(a)});
        }
        star.close();
        pl.gradientPath(star, Color::rgb(0xffff00), Color::rgb(0xff0000), kGradientRadial);
        pl.path(Path().circle({330, 290}, 70), Color::rgb(0x22cc66, 0.6f), false, Color::rgb(0xffffff), 4.f);
        pl.path(Path::fromSvg("M260 380 C300 300 380 460 440 360"), Color{}, false, Color::rgb(0xff44ff), 3.f);
        // true SVG arcs: a rotated elliptical ring (even-odd) with a stroked large-arc segment
        pl.path(Path::fromSvg("M20 330 A50 25 30 1 1 120 330 A50 25 30 1 1 20 330 Z M45 330 a25 12 30 1 0 50 0 a25 12 30 1 0 -50 0 z"),
                Color::rgb(0x66dd88), true, Color::rgb(0x224422), 1.5f);
        pl.gradientRect({20, 210, 60, 60}, Color::rgb(0x0000ff), Color::rgb(0x00ffff), false); // radial-free rect
        pl.cmds().back().gradient = kGradientRadial;
        pl.pushClip({300, 0, 100, 120}, 16.f);
        pl.path(Path().circle({350, 60}, 80), Color::rgb(0xcc2222));
        pl.popClip();
        Image cpu8;
        cpu8.resize(int(W), int(H), 0xFF000000u);
        init.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer r8;
        r8.render(pl, cpu8);
        packDrawList(pl, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("paths", compare(cpu8, out));
        UI_CHECK(cpu8.at(120, 300) != 0xFF181818u);          // star filled
    }

    // 9. multi-stop gradients at any angle: linear (CSS angles, SVG points, repeat / reflect), elliptical radial,
    //    transformed, on rounded rects and paths (with stroke), translated draw list
    {
        DrawList gl;
        gl.rect({0, 0, 488, 400}, Color::rgb(0x101010));
        const std::vector<GradientStop> rainbow = {{0.f, Color::rgb(0xff0000)}, {0.25f, Color::rgb(0xffff00)}, {0.5f, Color::rgb(0x00ff00)},
                                                   {0.75f, Color::rgb(0x00ffff)}, {1.f, Color::rgb(0x0000ff, 0.5f)}};
        gl.gradientRect({10, 10, 150, 90}, Gradient::linearAngle({10, 10, 150, 90}, 30.f, rainbow), {12, 12, 12, 12});
        gl.gradientRect({170, 10, 150, 90}, Gradient::linearAngle({170, 10, 150, 90}, 135.f, rainbow));
        gl.gradientRect({330, 10, 150, 90}, Gradient::linear({340, 20}, {370, 40}, {{0.f, Color::rgb(0xffffff)}, {1.f, Color::rgb(0x2244ff)}}, GradientSpread::Reflect));
        gl.gradientRect({10, 110, 150, 90}, Gradient::linear({10, 110}, {40, 110}, {{0.f, Color::rgb(0xff00ff)}, {0.5f, Color::rgb(0x000000)}, {1.f, Color::rgb(0x00ff00)}}, GradientSpread::Repeat));
        gl.gradientRect({170, 110, 150, 90}, Gradient::radialAt({245, 155}, 75.f, 45.f, rainbow), {45, 45, 45, 45});
        Gradient tg = Gradient::radialAt({0, 0}, 1.f, 1.f, {{0.f, Color::rgb(0xffffff)}, {0.6f, Color::rgb(0xff8800)}, {1.f, Color::rgb(0x330000)}}, GradientSpread::Reflect);
        tg.transform({30.f, 15.f, -10.f, 20.f, 405.f, 155.f}); // rotated, sheared ellipse
        gl.gradientRect({330, 110, 150, 90}, tg);
        Path star;
        for (int i = 0; i < 10; ++i) {
            const f32 a = f32(i) * 0.6283185f - 1.5707963f, r = i & 1 ? 40.f : 90.f;
            if (i == 0) star.moveTo({120 + r * std::cos(a), 310 + r * std::sin(a)});
            else star.lineTo({120 + r * std::cos(a), 310 + r * std::sin(a)});
        }
        star.close();
        gl.gradientPath(star, Gradient::linearAngle(star.bounds(), 200.f, rainbow), false, Color::rgb(0xffffff), 2.f);
        DrawList sub; // moved by translate(): the gradient moves with the geometry
        sub.gradientPath(Path().circle({60, 60}, 55), Gradient::radialAt({60, 60}, 55.f, 55.f, rainbow, GradientSpread::Pad));
        sub.translate({270, 250});
        for (const DrawCmd& c : sub.cmds()) gl.pushFrom(sub, c);
        Image cpu9;
        cpu9.resize(int(W), int(H), 0xFF000000u);
        init.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer r9;
        r9.render(gl, cpu9);
        packDrawList(gl, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("gradients", compare(cpu9, out));
        auto ch = [&](int x, int y, int k) { return int((cpu9.at(x, y) >> (8 * k)) & 0xFF); };
        UI_CHECK(ch(330, 310, 0) > 200 && ch(330, 310, 1) < 40);             // translated radial: red centre
        UI_CHECK(ch(245, 155, 0) > 200 && ch(245, 155, 2) < 40);             // radial centre red
        if (const char* dir = std::getenv("FUSE_PARITY_OUT")) {
            const std::string f = std::string(dir) + "/gradients.ppm";
            if (FILE* fp = std::fopen(f.c_str(), "wb")) {
                std::fprintf(fp, "P6 %d %d 255\n", cpu9.width, cpu9.height);
                for (u32 px : cpu9.pixels) { const unsigned char rgb[3] = {u8(px), u8(px >> 8), u8(px >> 16)}; std::fwrite(rgb, 1, 3, fp); }
                std::fclose(fp);
            }
        }
        // clip paths: nested star + even-odd ring clip over rects, a gradient, a stroked path and a rounded clip
        DrawList cl;
        cl.rect({0, 0, f32(W), f32(H)}, Color::rgb(0x202020));
        cl.pushClipPath(star);
        cl.rect({0, 200, 260, 220}, Color::rgb(0x33cc66));
        cl.gradientRect({0, 200, 260, 220}, Gradient::linearAngle({0, 200, 260, 220}, 45.f, rainbow));
        cl.pushClipPath(Path().circle({120, 310}, 50).circle({120, 310}, 25), true);
        cl.rect({0, 200, 260, 220}, Color::rgb(0xffffff, 0.7f));
        cl.popClip();
        cl.popClip();
        cl.pushClip({300, 20, 200, 160}, 20.f);
        cl.pushClipPath(Path().circle({400, 100}, 70));
        cl.path(Path().moveTo({300, 30}).lineTo({500, 170}), {}, false, Color::rgb(0xffcc00), 9.f);
        cl.rect({300, 100, 200, 100}, Color::rgb(0x3366ff, 0.8f));
        cl.popClip();
        cl.popClip();
        Image cpuC;
        cpuC.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer rc;
        rc.render(cl, cpuC);
        packDrawList(cl, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("clip_paths", compare(cpuC, out));
        auto cc = [&](int x, int y) { return cpuC.at(x, y) & 0xFFFFFFu; };
        UI_CHECK(cc(10, 210) == 0x202020u);            // outside the star: background
        UI_CHECK(cc(120, 310) != 0x202020u);           // star centre (ring hole): gradient
        UI_CHECK(cc(330, 30) == 0x202020u);            // outside the circle, inside the rounded rect
#if FUSE_PARITY_EXTRAS
        // SVG text / clipPath / pattern / multi-stop gradients and 3D surface / scatter / bars: CPU == Vulkan
        DrawList xl;
        xl.rect({0, 0, f32(W), f32(H)}, Color::rgb(0x101014));
        fuse::ui::svg::Document doc;
        UI_CHECK(doc.load(R"svg(<svg viewBox="0 0 200 100"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="red"/>
            <stop offset=".5" stop-color="lime"/><stop offset="1" stop-color="blue"/></linearGradient><clipPath id="c"><circle cx="50" cy="50" r="40"/></clipPath>
            <pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse"><rect width="5" height="5" fill="orange"/></pattern></defs>
            <rect width="100" height="100" fill="url(#g)" clip-path="url(#c)"/><rect x="110" y="10" width="80" height="40" fill="url(#p)"/>
            <text x="150" y="85" font-size="24" text-anchor="middle" fill="white">Ag</text></svg>)svg"));
        doc.render(xl, {0, 0, 240, 120});
        namespace d3 = fuse::ui::dv3d;
        d3::Scene sf;
        sf.rows = sf.cols = 12;
        for (u32 r = 0; r < 12; ++r)
            for (u32 c = 0; c < 12; ++c) sf.values.push_back(std::sin(f32(c) * 0.5f) * std::cos(f32(r) * 0.4f));
        d3::render(sf, xl, {250, 0, 240, 200}, Color::rgb(0xFFFFFF));
        d3::Scene sc;
        sc.kind = d3::Kind::Scatter;
        for (int i = 0; i < 60; ++i) sc.points.push_back({std::cos(f32(i) * 0.3f), f32(i) * 0.05f, std::sin(f32(i) * 0.3f)});
        d3::render(sc, xl, {0, 120, 240, 200}, Color::rgb(0xFFFFFF));
        d3::Scene sb;
        sb.kind = d3::Kind::Bars;
        sb.rows = 3, sb.cols = 4;
        sb.values = {1, 2, 3, 4, 2, 3, 4, 5, 3, 4, 5, 6};
        d3::render(sb, xl, {250, 200, 240, 200}, Color::rgb(0xFFFFFF));
        Image cpuX;
        cpuX.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer rx;
        rx.render(xl, cpuX);
        packDrawList(xl, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("svg+dataviz3d", compare(cpuX, out));
        if (const char* dir = std::getenv("FUSE_PARITY_OUT")) {
            const std::string f = std::string(dir) + "/dataviz3d.ppm";
            if (FILE* fp = std::fopen(f.c_str(), "wb")) {
                std::fprintf(fp, "P6 %d %d 255\n", cpuX.width, cpuX.height);
                for (u32 px : cpuX.pixels) { const unsigned char rgb[3] = {u8(px), u8(px >> 8), u8(px >> 16)}; std::fwrite(rgb, 1, 3, fp); }
                std::fclose(fp);
            }
        }
#endif
    }

    // 7. glyph atlas uploads only when its generation changes
    {
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        data.atlasGeneration = ui.atlas().version();
        UI_CHECK(gpu.upload(0, data));
        const auto n0 = gpu.atlasUploads();
        UI_CHECK(gpu.upload(0, data));
        UI_CHECK(gpu.upload(0, data));
        UI_CHECK_EQ(gpu.atlasUploads(), n0);
        data.atlasGeneration += 1;
        UI_CHECK(gpu.upload(0, data));
        UI_CHECK_EQ(gpu.atlasUploads(), n0 + 1);
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("atlas-skip", compare(cpu, out));
    }

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
