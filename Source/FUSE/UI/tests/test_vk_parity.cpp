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
        {   // layered clips: per-pixel SVG mask (gradient fade + two-tone shapes), clipPath union (overlap, opposite
            // windings, mixed clip-rule), and DrawList::pushClipLayers directly with a radial ramp
            DrawList ml;
            ml.rect({0, 0, f32(W), f32(H)}, Color::rgb(0x101014));
            fuse::ui::svg::Document md;
            UI_CHECK(md.load(R"svg(<svg viewBox="0 0 200 100"><defs>
                <linearGradient id="fade" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="white"/><stop offset="1" stop-color="black"/></linearGradient>
                <mask id="m" maskContentUnits="userSpaceOnUse"><rect x="0" y="0" width="100" height="100" fill="url(#fade)"/><circle cx="50" cy="50" r="20" fill="#808080"/></mask>
                <clipPath id="u"><circle cx="130" cy="50" r="30"/><path d="M150 20 h40 v60 h-40 z M160 30 v40 h20 v-40 z" clip-rule="evenodd"/><path d="M140 50 L170 20 L170 80 Z"/></clipPath></defs>
                <rect width="100" height="100" fill="orange" mask="url(#m)"/><rect x="100" width="100" height="100" fill="#3080ff" clip-path="url(#u)"/></svg>)svg"));
            md.render(ml, {0, 0, 480, 240});
            ClipLayer ring;
            ring.path.circle({120, 320}, 70);
            ring.gradient = 2;
            ring.toUnit = {1.f / 70.f, 0, 0, 1.f / 70.f, -120.f / 70.f, -320.f / 70.f};
            ring.value0 = 1.f, ring.value1 = 0.f;
            ClipLayer bar;
            bar.path.rect({60, 300, 300, 30});
            bar.value0 = bar.alpha0 = 0.5f;
            const ClipLayer two[] = {ring, bar};
            ml.pushClipLayers(two);
            ml.rect({0, 240, 480, 160}, Color::rgb(0x40FF80));
            ml.popClip();
            // radial gradient with a focal point (SVG fx / fy): t = 0 at the focus, 1 on the circle
            ml.gradientRect({380, 250, 100, 140}, Gradient::radialFocal({430, 320}, 50, 50, {405, 320},
                                                     {{0.f, Color::rgb(0xFFFFFF)}, {1.f, Color::rgb(0x0000FF)}}, GradientSpread::Reflect));
            Image cpuM;
            cpuM.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rm;
            rm.render(ml, cpuM);
            packDrawList(ml, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("clip_layers", compare(cpuM, out));
            auto g = [&](int x, int y) { return int((cpuM.at(x, y) >> 8) & 0xFF); }; // green channel
            auto r = [&](int x, int y) { return int(cpuM.at(x, y) & 0xFF); };
            // mask: the fade runs left (opaque) to right (clear); the grey circle replaces it with ~50 %
            UI_CHECK(r(10, 30) > 200 && r(230, 30) < 60 && r(100, 100) > 110 && r(100, 100) < 160);
            // union: inside the circle, the triangle over the frame's hole, and the frame: blue; the hole alone: background
            const auto blueAt = [&](int x, int y) { return ((cpuM.at(x, y) >> 16) & 0xFF) > 200; };
            UI_CHECK(blueAt(300, 120) && blueAt(396, 120) && blueAt(372, 60) && !blueAt(420, 120) && !blueAt(250, 20));
            UI_CHECK(r(405, 320) > 245 && r(455, 320) < r(430, 320) && r(379 + 100, 320) < 40); // white at the focus, not the centre
            UI_CHECK(r(410, 320) > r(450, 320) + 60); // asymmetric: 5 px from the focus vs 5 px from the far edge
            UI_CHECK(g(120, 320) > 200 && g(185, 345) < 60 && g(250, 315) > 100 && g(250, 315) < 160);
            if (const char* dir = std::getenv("FUSE_PARITY_OUT")) {
                const std::string f = std::string(dir) + "/clip_layers.ppm";
                if (FILE* fp = std::fopen(f.c_str(), "wb")) {
                    std::fprintf(fp, "P6 %d %d 255\n", cpuM.width, cpuM.height);
                    for (u32 px : cpuM.pixels) { const unsigned char rgb[3] = {u8(px), u8(px >> 8), u8(px >> 16)}; std::fwrite(rgb, 1, 3, fp); }
                    std::fclose(fp);
                }
            }
        {   // backdrop under clip paths (circle clip-path + layered radial mask): the blur's coverage is multiplied by
            // the active clip paths like any other command, CPU and GPU
            DrawList bc;
            for (int i = 0; i < 12; ++i) bc.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
            Path circ;
            circ.circle({140, 140}, 90);
            bc.pushClipPath(circ);
            bc.backdrop({40, 40, 220, 220}, 12.f, {0, 0, 0, 0}, Color::rgb(0xffffff, 0.2f));
            bc.popClip();
            ClipLayer fade;
            fade.path.rect({300, 60, 160, 300});
            fade.gradient = 1;
            fade.toUnit = {1.f / 160.f, 0, 0, 0, -300.f / 160.f, 0};
            fade.value0 = 1.f, fade.value1 = 0.f;
            const ClipLayer one[] = {fade};
            bc.pushClipLayers(one);
            bc.backdrop({290, 50, 180, 320}, 8.f, {16, 16, 16, 16});
            bc.popClip();
            Image cpuB;
            cpuB.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rb;
            rb.render(bc, cpuB);
            packDrawList(bc, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("backdrop_clip", compare(cpuB, out));
        }
        {   // backdrop-filter colour functions, with and without blur: each panel one function, plus a chain
            DrawList bf;
            for (int i = 0; i < 12; ++i) bf.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu, i % 3 ? 1.f : 0.6f));
            bf.gradientRect({0, 200, f32(W), 60}, Color::rgb(0x30c060), Color::rgb(0xf0e020), false);
            const ColorMatrix fx[] = {ColorMatrix::brightness(1.4f), ColorMatrix::contrast(1.8f), ColorMatrix::saturate(2.5f),
                                      ColorMatrix::grayscale(1.f), ColorMatrix::hueRotate(120.f), ColorMatrix::invert(0.8f),
                                      ColorMatrix::sepia(1.f), ColorMatrix::opacity(0.4f),
                                      ColorMatrix::sepia(0.6f).then(ColorMatrix::hueRotate(-45.f)).then(ColorMatrix::contrast(0.7f))};
            for (int k = 0; k < 9; ++k) {
                const Rect r{f32(8 + (k % 5) * 94), f32(20 + (k / 5) * 190), 86, 170};
                bf.backdrop(r, k & 1 ? 0.f : 6.f, {10, 10, 10, 10}, fx[k], k == 8 ? Color::rgb(0xffffff, 0.15f) : Color{});
            }
            Image cpuF;
            cpuF.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rf;
            rf.render(bf, cpuF);
            packDrawList(bf, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("backdrop_filter", compare(cpuF, out));
        }
        {   // element filter layers: matrix, blur, drop-shadow, nested, under a rounded clip and a path clip;
            // backdrop-filter drop-shadow
            DrawList fl;
            for (int i = 0; i < 12; ++i) fl.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
            fl.pushFilter(ColorMatrix::opacity(0.5f).then(ColorMatrix::sepia(1.f)));
            fl.rect({20, 20, 120, 80}, Color::rgb(0x40ff40));
            fl.rect({80, 50, 120, 80}, Color::rgb(0xffffff), 12.f);
            fl.popFilter();
            fl.pushFilter(ColorMatrix{}, 4.f, DropShadow{{6, 8}, 9.f, Color::rgb(0x000000, 0.7f)});
            fl.rect({240, 30, 90, 90}, Color::rgb(0xffe020), 20.f);
            fl.pushFilter(ColorMatrix::invert(1.f));
            fl.rect({260, 50, 50, 50}, Color::rgb(0x2040ff));
            fl.popFilter();
            fl.popFilter();
            fl.pushClip({30, 160, 160, 160}, 24.f);
            fl.pushFilter(ColorMatrix::hueRotate(90.f), 2.f, DropShadow{{-5, 4}, 3.f, Color::rgb(0xff00ff)});
            fl.rect({10, 150, 150, 100}, Color::rgb(0x30c0c0, 0.8f));
            fl.popFilter();
            fl.popClip();
            Path circ;
            circ.circle({330, 250}, 70);
            fl.pushClipPath(circ);
            fl.pushFilter(ColorMatrix::grayscale(1.f), 0.f, DropShadow{{8, 8}, 0.f, Color::rgb(0x00ff00, 0.6f)});
            fl.rect({280, 200, 80, 80}, Color::rgb(0xff2020));
            fl.popFilter();
            fl.popClip();
            Image cpuL;
            cpuL.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rl;
            rl.render(fl, cpuL);
            packDrawList(fl, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("filter_layer", compare(cpuL, out));

            DrawList bs;
            for (int i = 0; i < 12; ++i) bs.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
            bs.backdrop({60, 60, 180, 160}, 5.f, {14, 14, 14, 14}, ColorMatrix::opacity(0.5f), DropShadow{{10, 12}, 8.f, Color::rgb(0x000000, 0.8f)});
            bs.backdrop({280, 80, 140, 140}, 0.f, {0, 0, 0, 0}, ColorMatrix::opacity(0.3f), DropShadow{{-6, 6}, 0.f, Color::rgb(0x00ff00)});
            Image cpuS;
            cpuS.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rs;
            rs.render(bs, cpuS);
            packDrawList(bs, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("backdrop_shadow", compare(cpuS, out));

            // CSS-order chains (nested, inner layers chained: no double clip coverage), several drop-shadows, an SVG
            // filter program with every supported primitive
            FilterProgram prog;
            UI_CHECK(parseSvgFilter(
                "<filter id=\"p\"><feGaussianBlur in=\"SourceAlpha\" stdDeviation=\"3 2\" result=\"b\"/>"
                "<feOffset in=\"b\" dx=\"6\" dy=\"5\" result=\"o\"/>"
                "<feFlood flood-color=\"#3060ff\" flood-opacity=\"0.7\" result=\"f\"/>"
                "<feComposite in=\"f\" in2=\"o\" operator=\"in\" result=\"s\"/>"
                "<feColorMatrix in=\"SourceGraphic\" type=\"saturate\" values=\"0.3\" result=\"g\"/>"
                "<feBlend in=\"g\" in2=\"s\" mode=\"screen\" result=\"m\"/>"
                "<feComposite in=\"m\" in2=\"SourceGraphic\" operator=\"arithmetic\" k1=\"0.2\" k2=\"0.6\" k3=\"0.3\" k4=\"0.02\" result=\"a\"/>"
                "<feMerge><feMergeNode in=\"s\"/><feMergeNode in=\"a\"/></feMerge></filter>", prog));
            FilterProgram banded;
            UI_CHECK(parseSvgFilter(
                "<filter><feMorphology operator=\"erode\" radius=\"3\" result=\"e\"/>"
                "<feConvolveMatrix in=\"e\" order=\"3 5\" kernelMatrix=\"1 0 -1 2 0 -2 1 0 -1 0 1 0 1 1 1\" divisor=\"3\" bias=\"0.1\" result=\"c\"/>"
                "<feGaussianBlur in=\"SourceAlpha\" stdDeviation=\"4\" result=\"b\"/>"
                "<feDiffuseLighting in=\"b\" surfaceScale=\"5\" result=\"l\"><feDistantLight azimuth=\"45\" elevation=\"30\"/></feDiffuseLighting>"
                "<feComposite in=\"c\" in2=\"l\" operator=\"arithmetic\" k1=\"1\" k2=\"0.3\"/></filter>", banded));
            const auto chainList = [&](DrawList& cl, f32 big) {
                for (int i = 0; i < 12; ++i) cl.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
                cl.pushClip({20, 20, 220 * big, 200 * big}, 18.f);
                cl.pushFilter(ColorMatrix::invert(1.f), 0.f, DropShadow{{0, 9}, 4.f, Color::rgb(0x00ff00, 0.8f)});
                cl.pushFilter(ColorMatrix{}, 0.f, DropShadow{{8, 0}, 2.f, Color::rgb(0xff0000)}, true);
                cl.pushFilter(ColorMatrix::sepia(1.f), 3.f, {}, true);
                cl.rect({40, 40, 140 * big, 120 * big}, Color::rgb(0x2080ff, 0.9f), 10.f);
                cl.popFilter();
                cl.popFilter();
                cl.popFilter();
                cl.popClip();
                cl.pushFilter(prog);
                cl.rect({270, 60, 120 * big, 110 * big}, Color::rgb(0xffc040), 16.f);
                cl.rect({300, 90, 40, 40}, Color::rgb(0x8020ff, 0.6f));
                cl.popFilter();
                if (big > 1.f) { // neighbourhood primitives across band seams
                    cl.pushFilter(banded);
                    cl.rect({60, 250, 200, 100}, Color::rgb(0x40c080, 0.8f), 30.f);
                    cl.popFilter();
                }
            };
            DrawList cl;
            chainList(cl, 1.f);
            Image cpuC2;
            cpuC2.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rc;
            rc.render(cl, cpuC2);
            packDrawList(cl, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("filter_chain", compare(cpuC2, out));

            // the remaining SVG primitives, filter / primitive subregions, and an SVG program as backdrop-filter
            Image cells;
            cells.resize(6, 4);
            for (int i = 0; i < 24; ++i) cells.pixels[std::size_t(i)] = (i * 7) % 3 == 0 ? 0xFF2040E0u : (i % 2 ? 0xC0C0A000u : 0x80008040u);
            const FilterImageResolver imgs = [&](std::string_view n) -> const Image* { return n == "cells" ? &cells : nullptr; };
            FilterProgram fx, bk;
            UI_CHECK(parseSvgFilter(
                "<filter x=\"-20%\" y=\"-20%\" width=\"140%\" height=\"140%\">"
                "<feTurbulence type=\"fractalNoise\" baseFrequency=\"0.04 0.06\" numOctaves=\"3\" seed=\"7\" result=\"t\"/>"
                "<feDisplacementMap in=\"SourceGraphic\" in2=\"t\" scale=\"12\" xChannelSelector=\"R\" yChannelSelector=\"G\" result=\"d\"/>"
                "<feMorphology in=\"d\" operator=\"dilate\" radius=\"2 1\" result=\"m\"/>"
                "<feConvolveMatrix in=\"m\" order=\"3\" kernelMatrix=\"0 -1 0 -1 5 -1 0 -1 0\" result=\"c\"/>"
                "<feComponentTransfer in=\"c\" result=\"ct\"><feFuncR type=\"table\" tableValues=\"0 0.5 1\"/>"
                "<feFuncG type=\"gamma\" amplitude=\"1.2\" exponent=\"0.8\" offset=\"0.05\"/><feFuncB type=\"discrete\" tableValues=\"0 0.3 0.7 1\"/>"
                "<feFuncA type=\"linear\" slope=\"0.9\" intercept=\"0.05\"/></feComponentTransfer>"
                "<feGaussianBlur in=\"SourceAlpha\" stdDeviation=\"2\" result=\"ba\"/>"
                "<feSpecularLighting in=\"ba\" surfaceScale=\"4\" specularExponent=\"12\" lighting-color=\"#ffeecc\" result=\"sp\">"
                "<fePointLight x=\"150\" y=\"40\" z=\"80\"/></feSpecularLighting>"
                "<feComposite in=\"sp\" in2=\"SourceAlpha\" operator=\"in\" result=\"spi\"/>"
                "<feDiffuseLighting in=\"ba\" surfaceScale=\"3\" diffuseConstant=\"0.8\" result=\"di\">"
                "<feSpotLight x=\"0\" y=\"0\" z=\"120\" pointsAtX=\"100\" pointsAtY=\"100\" pointsAtZ=\"0\" specularExponent=\"2\" limitingConeAngle=\"40\"/>"
                "</feDiffuseLighting>"
                "<feBlend in=\"ct\" in2=\"di\" mode=\"multiply\" result=\"bl\"/>"
                "<feComposite in=\"spi\" in2=\"bl\" operator=\"arithmetic\" k2=\"1\" k3=\"1\" result=\"lit\"/>"
                "<feOffset in=\"SourceGraphic\" x=\"0\" y=\"0\" width=\"24\" height=\"18\" result=\"cell\"/>"
                "<feTile in=\"cell\" x=\"-10\" y=\"0\" width=\"60\" height=\"200\" result=\"tiles\"/>"
                "<feImage href=\"cells\" x=\"30\" y=\"-12\" width=\"60\" height=\"40\" result=\"im\"/>"
                "<feMerge><feMergeNode in=\"tiles\"/><feMergeNode in=\"im\"/><feMergeNode in=\"lit\"/></feMerge></filter>",
                fx, {}, imgs));
            UI_CHECK(parseSvgFilter(
                "<filter><feTurbulence baseFrequency=\"0.03\" numOctaves=\"2\" seed=\"2\" stitchTiles=\"stitch\" result=\"t\"/>"
                "<feDisplacementMap in=\"SourceGraphic\" in2=\"t\" scale=\"18\" xChannelSelector=\"B\" yChannelSelector=\"R\" result=\"d\"/>"
                "<feGaussianBlur in=\"d\" stdDeviation=\"3\" x=\"10%\" width=\"80%\" result=\"b\"/>"
                "<feColorMatrix in=\"b\" type=\"hueRotate\" values=\"90\"/></filter>", bk));
            DrawList sv;
            for (int i = 0; i < 12; ++i) sv.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
            sv.pushFilter(fx);
            sv.rect({40, 40, 150, 120}, Color::rgb(0x2080ff, 0.9f), 24.f);
            sv.rect({70, 70, 50, 40}, Color::rgb(0xffc040));
            sv.popFilter();
            sv.backdrop({250, 50, 180, 170}, bk, {20, 20, 20, 20}, Color::rgb(0xffffff, 0.1f));
            Image cpuV;
            cpuV.resize(int(W), int(H), 0xFF000000u);
            CpuRasterizer rv;
            rv.render(sv, cpuV);
            packDrawList(sv, W, H, data);
            UI_CHECK(gpu.upload(0, data));
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("filter_svg_primitives", compare(cpuV, out));

            // 2D affine transform layers: rotate, skew, matrix, nested, inside a filter, under a rounded clip
            const auto rot = [](f32 deg, f32 cx, f32 cy) { // rotate about (cx, cy)
                const f32 r = deg * 3.14159265f / 180.f, c = std::cos(r), s = std::sin(r);
                return std::array<f32, 6>{c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy};
            };
            const auto xfList = [&](DrawList& xl, f32 big) {
                for (int i = 0; i < 12; ++i) xl.rect({f32(i * 40), 0, 20, f32(H)}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
                xl.pushTransform(rot(30.f, 110.f, 90.f));
                xl.rect({40, 40, 140 * big, 100 * big}, Color::rgb(0x40ff40, 0.9f), 14.f);
                Path tri;
                tri.moveTo({60, 60});
                tri.lineTo({150, 70});
                tri.lineTo({90, 130});
                tri.close();
                xl.path(tri, Color::rgb(0x2020c0));
                xl.pushTransform({1.f, 0.f, 0.4f, 1.f, -30.f, 0.f}); // nested skewX
                xl.rect({70, 80, 60, 30}, Color::rgb(0xffffff, 0.8f), 6.f);
                xl.popTransform();
                xl.popTransform();
                xl.pushFilter(ColorMatrix::sepia(1.f), 2.f);
                xl.pushTransform({0.8f, -0.3f, 0.5f, 1.2f, 250.f, 60.f}); // matrix()
                xl.rect({0, 0, 120, 90}, Color::rgb(0xffc040), 20.f);
                xl.line({10, 10}, {110, 80}, Color::rgb(0x000000), 3.f);
                xl.popTransform();
                xl.popFilter();
                xl.pushClip({30, 220, 200, 150}, 20.f);
                xl.pushTransform(rot(-75.f, 130.f, 300.f));
                xl.rect({60, 250, 140, 100}, Color::rgb(0xa020ff, 0.7f), 8.f);
                xl.popTransform();
                xl.popClip();
                xl.pushTransform({-1.5f, 0.f, 0.f, 0.7f, 760.f, 120.f}); // mirrored scale
                xl.rect({300, 260, 80, 90}, Color::rgb(0x20e0e0), 30.f);
                xl.popTransform();
            };
            {
                DrawList xl;
                xfList(xl, 1.f);
                Image cpuXf;
                cpuXf.resize(int(W), int(H), 0xFF000000u);
                CpuRasterizer rx;
                rx.render(xl, cpuXf);
                packDrawList(xl, W, H, data);
                UI_CHECK(gpu.upload(0, data));
                vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
                check("transform_layer", compare(cpuXf, out));
            }
            {   // projective (CSS 3D) layer: per-pixel homography divide, CPU vs GPU
                DrawList pl;
                xfList(pl, 1.f);
                pl.pushProjective({1.1f, 0.15f, -20.f, 0.05f, 1.f, 10.f, 0.0008f, 0.0002f, 0.8f});
                pl.rect({120, 90, 260, 180}, Color::rgb(0xe08020, 0.8f), 24.f);
                pl.rect({160, 130, 60, 60}, Color::rgb(0x2040e0), 0.f);
                pl.popTransform();
                Image cpuP;
                cpuP.resize(int(W), int(H), 0xFF000000u);
                CpuRasterizer rp;
                rp.render(pl, cpuP);
                packDrawList(pl, W, H, data);
                UI_CHECK(gpu.upload(0, data));
                vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
                check("projective_layer", compare(cpuP, out));
            }

            // oversized layers on a small scratch: tiled (banded) layer ends match the CPU
            UiVkRenderer small;
            UI_CHECK(small.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2, .maxBlurPixels = 90000}));
            if (small.valid()) {
                DrawList tl;
                chainList(tl, 1.4f);
                Image cpuT;
                cpuT.resize(int(W), int(H), 0xFF000000u);
                CpuRasterizer rt;
                rt.render(tl, cpuT);
                packDrawList(tl, W, H, data);
                UI_CHECK(small.upload(0, data));
                vk.run(init, out, [&](VkCommandBuffer c) { small.record((void*)c, (void*)vk.view, 0); });
                check("filter_tiled", compare(cpuT, out));
                DrawList xt;
                xfList(xt, 2.2f);
                xt.pushTransform(rot(10.f, 230.f, 175.f)); // a big layer: tiled
                xt.rect({80, 60, 300, 230}, Color::rgb(0x103050, 0.5f), 40.f);
                xt.popTransform();
                Image cpuXt;
                cpuXt.resize(int(W), int(H), 0xFF000000u);
                CpuRasterizer rxt;
                rxt.render(xt, cpuXt);
                packDrawList(xt, W, H, data);
                UI_CHECK(small.upload(0, data));
                vk.run(init, out, [&](VkCommandBuffer c) { small.record((void*)c, (void*)vk.view, 0); });
                check("transform_tiled", compare(cpuXt, out));
            }
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

    // 8. persistent pack + incremental upload: an unchanged frame is kept (no pack, no bytes written); one changed
    //    colour repacks but writes only the differing pages, and still renders like the CPU
    {
        const u32 gen = ui.atlas().version();
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
        UI_CHECK(!data.reused);
        UI_CHECK(gpu.upload(0, data));
        const u64 v = data.contentVersion, b0 = gpu.uploadedBytes();
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
        UI_CHECK(data.reused);
        UI_CHECK_EQ(data.contentVersion, v);
        UI_CHECK(gpu.upload(0, data));
        UI_CHECK_EQ(gpu.uploadedBytes(), b0);
        // a different generation or scissor is never reused
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen + 1);
        UI_CHECK(!data.reused);
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {0, 0, 50, 50}, gen + 1);
        UI_CHECK(!data.reused);
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
        UI_CHECK(gpu.upload(0, data));
        DrawList edited = ui.drawList();
        for (std::size_t k = edited.cmds().size(); k-- > 0;)
            if (edited.cmds()[k].op == DrawOp::Glyph) {
                const_cast<DrawCmd&>(edited.cmds()[k]).fill = Color::rgb(0xff0000);
                break;
            }
        const u64 b1 = gpu.uploadedBytes();
        packDrawList(edited, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
        UI_CHECK(!data.reused);
        UI_CHECK(data.patched); // colour-only: patched in place, not repacked
        UI_CHECK(gpu.upload(0, data));
        const u64 wrote = gpu.uploadedBytes() - b1;
        UI_CHECK(wrote <= 3 * 4096); // the record's page and the appended fill
        UI_CHECK(wrote > 0 && wrote < data.cmdBytes() + data.tileBytes() + data.imageBytes());
        Image cpuE;
        cpuE.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer re;
        re.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        re.render(edited, cpuE);
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("incremental-upload", compare(cpuE, out));
        // a rect colour change patches its GpuCmd; a moved command is structural (full repack), and still correct
        DrawList e2 = edited;
        for (std::size_t k = 0; k < e2.cmds().size(); ++k)
            if (e2.cmds()[k].op == DrawOp::Rect) {
                const_cast<DrawCmd&>(e2.cmds()[k]).fill = Color::rgb(0x00ff88);
                break;
            }
        packDrawList(e2, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
        UI_CHECK(data.patched);
        UI_CHECK(gpu.upload(0, data));
        Image cpuR;
        cpuR.resize(int(W), int(H), 0xFF000000u);
        re.render(e2, cpuR);
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("patch-rect", compare(cpuR, out));
        for (std::size_t k = e2.cmds().size(); k-- > 0;)
            if (e2.cmds()[k].op == DrawOp::Glyph) {
                const_cast<DrawCmd&>(e2.cmds()[k]).rect.x += 3.f;
                break;
            }
        packDrawList(e2, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
        UI_CHECK(!data.patched && !data.reused);
        UI_CHECK(gpu.upload(0, data));
        cpuR.resize(int(W), int(H), 0xFF000000u);
        std::fill(cpuR.pixels.begin(), cpuR.pixels.end(), 0xFF000000u);
        re.render(e2, cpuR);
        vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        check("moved-glyph", compare(cpuR, out));
        // structural edits keyed by source node: a glyph inserted mid-list, then one removed; records of other nodes
        // keep their slots, so the upload stays a fraction of the buffers
        {
            std::vector<DrawCmd> cmds = e2.cmds();
            std::size_t mid = cmds.size() / 2;
            while (mid < cmds.size() && cmds[mid].op != DrawOp::Glyph) ++mid;
            UI_CHECK(mid < cmds.size());
            DrawCmd extra = cmds[mid];
            extra.rect.x += 1.f;
            cmds.insert(cmds.begin() + std::ptrdiff_t(mid), extra);
            DrawList ins;
            for (const DrawCmd& c : cmds) ins.push(c);
            const u64 b2 = gpu.uploadedBytes();
            packDrawList(ins, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
            UI_CHECK(gpu.upload(0, data));
            const u64 insWrote = gpu.uploadedBytes() - b2;
            UI_CHECK(data.incremental); // re-emitted only the changed range
            std::fill(cpuR.pixels.begin(), cpuR.pixels.end(), 0xFF000000u);
            re.render(ins, cpuR);
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            check("inserted-glyph", compare(cpuR, out));
            cmds.erase(cmds.begin() + std::ptrdiff_t(mid) + 1);
            DrawList rem;
            for (const DrawCmd& c : cmds) rem.push(c);
            packDrawList(rem, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
            UI_CHECK(gpu.upload(0, data));
            std::fill(cpuR.pixels.begin(), cpuR.pixels.end(), 0xFF000000u);
            re.render(rem, cpuR);
            vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
            UI_CHECK(data.incremental);
            check("removed-glyph", compare(cpuR, out));
            // a rect (GpuCmd) inserted mid-list and one removed: same incremental path through the cmd slots
            std::size_t rm = cmds.size() / 3;
            while (rm < cmds.size() && cmds[rm].op != DrawOp::Glyph) ++rm;
            DrawCmd box{};
            box.op = DrawOp::Rect;
            box.rect = {40.f, 30.f, 90.f, 50.f};
            box.fill = Color{0.2f, 0.6f, 0.9f, 0.7f};
            box.radius[0] = box.radius[1] = box.radius[2] = box.radius[3] = 6.f;
            box.node = cmds[rm].node;
            cmds.insert(cmds.begin() + std::ptrdiff_t(rm), box);
            for (int step = 0; step < 2; ++step) {
                if (step == 1) cmds.erase(cmds.begin() + std::ptrdiff_t(rm) + 1); // and drop the glyph after it
                DrawList rl;
                for (const DrawCmd& c : cmds) rl.push(c);
                packDrawList(rl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
                UI_CHECK(data.incremental);
                UI_CHECK(gpu.upload(0, data));
                std::fill(cpuR.pixels.begin(), cpuR.pixels.end(), 0xFF000000u);
                re.render(rl, cpuR);
                vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
                check(step ? "rect+removed" : "inserted-rect", compare(cpuR, out));
            }
            // clip push / pop, a stop gradient, a path, then backdrops (multi-pass frames): all re-emit only the range
            {
                const std::vector<DrawCmd> base = cmds;
                const std::size_t at = base.size() / 2;
                const auto frame = [&](const char* name, auto&& extra, bool incr) {
                    DrawList fl;
                    for (std::size_t i = 0; i < at; ++i) fl.pushFrom(e2, base[i]);
                    extra(fl);
                    for (std::size_t i = at; i < base.size(); ++i) fl.pushFrom(e2, base[i]);
                    packDrawList(fl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()), nullptr, 0, {}, gen);
                    if (incr) UI_CHECK(data.incremental);
                    if (incr && !data.incremental) std::printf("  %s: not incremental\n", name);
                    UI_CHECK(gpu.upload(0, data));
                    std::fill(cpuR.pixels.begin(), cpuR.pixels.end(), 0xFF000000u);
                    re.render(fl, cpuR);
                    vk.run(init, out, [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
                    check(name, compare(cpuR, out));
                };
                const auto none = [](DrawList&) {};
                frame("base", none, false);
                frame("clip-range", [](DrawList& l) {
                    l.pushClip({30.f, 20.f, 120.f, 60.f}, 10.f);
                    l.rect({20.f, 10.f, 160.f, 90.f}, Color{0.9f, 0.3f, 0.2f, 0.8f});
                    l.popClip();
                }, true);
                frame("stop-gradient", [](DrawList& l) {
                    l.gradientRect({50.f, 40.f, 140.f, 70.f},
                                   Gradient::linear({50.f, 40.f}, {190.f, 110.f},
                                                    {{0.f, Color{1.f, 0.f, 0.f, 1.f}}, {0.5f, Color{0.f, 1.f, 0.f, 0.6f}}, {1.f, Color{0.f, 0.f, 1.f, 1.f}}}),
                                   {8.f, 8.f, 8.f, 8.f});
                }, true);
                frame("path-insert", [](DrawList& l) {
                    Path pth;
                    pth.circle({90.f, 60.f}, 35.f);
                    l.path(pth, Color{0.1f, 0.7f, 0.4f, 0.9f}, false, Color{0.f, 0.f, 0.f, 1.f}, 2.f);
                }, true);
                frame("path-before", [](DrawList& l) { // a path ahead of it: its edge index moves, its words do not
                    Path q;
                    q.rect({20.f, 20.f, 30.f, 25.f});
                    l.path(q, Color{0.8f, 0.2f, 0.6f, 1.f});
                    Path pth;
                    pth.circle({90.f, 60.f}, 35.f);
                    l.path(pth, Color{0.1f, 0.7f, 0.4f, 0.9f}, false, Color{0.f, 0.f, 0.f, 1.f}, 2.f);
                }, true);
                const u32 imgWords = u32(data.images.size());
                frame("path-removed", [](DrawList& l) {
                    Path pth;
                    pth.circle({90.f, 60.f}, 35.f);
                    l.path(pth, Color{0.1f, 0.7f, 0.4f, 0.9f}, false, Color{0.f, 0.f, 0.f, 1.f}, 2.f);
                }, true);
                UI_CHECK(data.images.size() < imgWords); // the removed path's words at the arena tail are given back
                frame("backdrop-insert", [](DrawList& l) { l.backdrop({40.f, 30.f, 120.f, 60.f}, 9.f, {6.f, 6.f, 6.f, 6.f}); }, true);
                frame("backdrop-moved", [](DrawList& l) { l.backdrop({52.f, 36.f, 120.f, 60.f}, 9.f, {6.f, 6.f, 6.f, 6.f}); }, true);
                frame("multipass-rect", [](DrawList& l) {
                    l.backdrop({52.f, 36.f, 120.f, 60.f}, 9.f, {6.f, 6.f, 6.f, 6.f});
                    l.rect({60.f, 40.f, 50.f, 30.f}, Color{1.f, 1.f, 1.f, 0.5f}, 4.f);
                }, true);
                frame("multipass-clip", [](DrawList& l) {
                    l.backdrop({52.f, 36.f, 120.f, 60.f}, 9.f, {6.f, 6.f, 6.f, 6.f});
                    l.pushClip({60.f, 40.f, 40.f, 20.f}, 6.f);
                    l.rect({60.f, 40.f, 50.f, 30.f}, Color{1.f, 1.f, 1.f, 0.5f}, 4.f);
                    l.popClip();
                }, true);
                frame("backdrop-removed", none, true);
            }
            std::printf("  buffers: cmds %zu, tiles %zu, images %zu bytes\n", data.cmdBytes(), data.tileBytes(), data.imageBytes());
            std::printf("  structural: insert wrote %llu of %zu bytes\n", (unsigned long long)insWrote,
                        data.cmdBytes() + data.tileBytes() + data.imageBytes());
        }
        std::printf("  incremental: %llu of %zu bytes written\n", (unsigned long long)wrote,
                    data.cmdBytes() + data.tileBytes() + data.imageBytes());
    }

    // 4. steady state: packing + uploading the same frame again does not allocate
    packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
    UI_CHECK(gpu.upload(0, data)); // warm both slots' page shadows
    UI_CHECK(gpu.upload(1, data));
    const auto before = g_allocs.load();
    for (int i = 0; i < 10; ++i) {
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(gpu.upload(u32(i & 1), data));
    }
    UI_CHECK_EQ(g_allocs.load() - before, 0ull);
    gpu.destroy();
    return finish("fuse_ui_vk_parity");
}
