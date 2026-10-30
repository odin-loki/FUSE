// fuse_ui_vk_hdr: the HDR draw target (UiVkRenderer::setTargetHdr, ui_draw_list_hdr / ui_backdrop_hdr into RGBA16F).
// SDR content matches the CPU reference rasteriser once quantised (float blending skips the 8-bit rounding after each
// blend, so it may differ by a code or two); color::hdr colours keep their extended values above 1.0; and the
// float-source encoder (ui_encode_pq_hdr) turns them into PQ codes brighter than SDR white. Skips (77) without Vulkan.
#include "fuse/ui/color_space.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::render;
using namespace fuse::ui::test;
namespace C = fuse::ui::color;

namespace {
f32 halfToFloat(u16 h) {
    const u32 s = h >> 15, e = (h >> 10) & 31u, m = h & 1023u;
    f32 v = e == 0 ? std::ldexp(f32(m), -24) : std::ldexp(f32(m | 1024u), int(e) - 25);
    return s ? -v : v;
}
std::vector<u16> blackHalf(u32 W, u32 H) {
    std::vector<u16> v(std::size_t(W) * H * 4, 0);
    for (std::size_t i = 0; i < std::size_t(W) * H; ++i) v[i * 4 + 3] = 0x3C00; // alpha 1.0
    return v;
}
void compareSdr(const char* what, const Image& cpu, const std::vector<u16>& gpu) {
    int maxErr = 0;
    std::size_t over1 = 0;
    for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
        int m = 0;
        for (int k = 0; k < 4; ++k) {
            const int q = int(std::floor(std::clamp(halfToFloat(gpu[i * 4 + std::size_t(k)]), 0.f, 1.f) * 255.f + 0.5f));
            m = std::max(m, std::abs(q - int((cpu.pixels[i] >> (8 * k)) & 0xFFu)));
        }
        maxErr = std::max(maxErr, m);
        over1 += m > 1 ? 1 : 0;
    }
    std::printf("%-10s max err %d, >1 on %zu / %zu\n", what, maxErr, over1, cpu.pixels.size());
    UI_CHECK(maxErr <= 3);
    UI_CHECK(double(over1) < 0.005 * double(cpu.pixels.size()));
}
} // namespace

int main() {
    if (!UiVkRenderer::kernelAvailable() || !UiVkEncoder::kernelAvailable(UiVkEncoder::Mode::Hdr10, true)) {
        std::printf("SKIP: kernels not compiled in\n");
        return 77;
    }
    constexpr u32 W = 488, H = 400;
    VkHeadless vk;
    if (!vk.init(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, 8)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    UiVkRenderer gpu;
    UI_CHECK(gpu.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 2}));
    if (!gpu.valid()) return finish("fuse_ui_vk_hdr");
    UI_CHECK(gpu.hdrTargetAvailable());
    UI_CHECK(gpu.setTargetHdr(true));
    const std::vector<u16> black = blackHalf(W, H);
    std::vector<u16> out(black.size());
    GpuDrawData data;

    // 1. Tier A sample (rects, borders, text) as SDR content
    {
        UiContext ui({.viewport = {f32(W), f32(H)}, .font = &font()});
        widgets::install(ui);
        buildSample(ui);
        ui.update();
        Image cpu;
        fuse::ui::test::render(ui, cpu);
        packDrawList(ui.drawList(), W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
        UI_CHECK(gpu.upload(0, data));
        vk.runBytes(black.data(), out.data(), [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 0); });
        compareSdr("sample", cpu, out);
    }
    // 2. backdrop blur on the float target
    {
        DrawList bl;
        for (int i = 0; i < 12; ++i) bl.rect({f32(i * 40), 0, 20, 400}, Color::rgb(i & 1 ? 0xff4422u : 0x22aaffu));
        bl.backdrop({60, 60, 240, 200}, 12.f, {20, 20, 20, 20}, Color::rgb(0xffffff, 0.25f));
        bl.rect({80, 80, 40, 40}, Color::rgb(0x00ff00));
        Image cpu;
        cpu.resize(int(W), int(H), 0xFF000000u);
        CpuRasterizer r;
        r.render(bl, cpu);
        packDrawList(bl, W, H, data);
        UI_CHECK(gpu.upload(1, data));
        vk.runBytes(black.data(), out.data(), [&](VkCommandBuffer c) { gpu.record((void*)c, (void*)vk.view, 1); });
        compareSdr("backdrop", cpu, out);
    }
    // 3. HDR colours: 4x SDR white, 1000-nit red, and a half-transparent 4x white over black
    {
        DrawList dl;
        const Color white4 = C::hdr(Color{1, 1, 1, 1}, 4.f);
        const Color red1000 = C::hdrNits(Color{1, 0, 0, 1}, 1000.f);
        UI_CHECK(white4.r > 1.5f);
        UI_CHECK(std::abs(C::hdrBrightness(white4) - 4.f) < 1e-3f);
        dl.rect({10, 10, 100, 100}, white4);
        dl.rect({120, 10, 100, 100}, red1000);
        dl.rect({230, 10, 100, 100}, white4.withAlpha(0.5f));
        dl.rect({340, 10, 100, 100}, Color{1, 1, 1, 1}); // SDR white reference
        packDrawList(dl, W, H, data);
        UI_CHECK(gpu.upload(0, data));
        UiVkEncoder enc;
        UI_CHECK(enc.init((void*)vk.device, UiVkEncoder::Mode::Hdr10, 1, true));
        UI_CHECK(enc.hdrSource());
        // output image + readback buffer for the PQ encode
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        ii.extent = {W, H, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage img{};
        vkCreateImage(vk.device, &ii, nullptr, &img);
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk.device, img, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = vk.memType(mr.memoryTypeBits, 0);
        VkDeviceMemory mem{};
        vkAllocateMemory(vk.device, &mai, nullptr, &mem);
        vkBindImageMemory(vk.device, img, mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{};
        vkCreateImageView(vk.device, &vi, nullptr, &view);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize(W) * H * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buf{};
        vkCreateBuffer(vk.device, &bi, nullptr, &buf);
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements(vk.device, buf, &br);
        VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bai.allocationSize = br.size;
        bai.memoryTypeIndex = vk.memType(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory bmem{};
        vkAllocateMemory(vk.device, &bai, nullptr, &bmem);
        vkBindBufferMemory(vk.device, buf, bmem, 0);
        const f32 nits = 203.f;
        vk.runBytes(black.data(), out.data(), [&](VkCommandBuffer cmd) {
            gpu.record((void*)cmd, (void*)vk.view, 0);
            VkImageMemoryBarrier b[2]{};
            for (auto& x : b) {
                x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            b[0].image = vk.image;
            b[0].oldLayout = b[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b[1].image = img;
            b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
            enc.record(cmd, 0, vk.view, view, 0, 0, int(W), int(H), nits);
            b[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b[1]);
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
        });
        auto at = [&](int x, int y, int k) { return halfToFloat(out[(std::size_t(y) * W + std::size_t(x)) * 4 + std::size_t(k)]); };
        const f32 w4 = at(60, 60, 0), r1000 = at(170, 60, 0), half4 = at(280, 60, 0), sdr = at(390, 60, 0);
        std::printf("hdr       4x white %.4f (want %.4f), 1000-nit red %.4f (want %.4f), 50%% 4x %.4f, SDR %.4f\n", double(w4),
                    double(white4.r), double(r1000), double(red1000.r), double(half4), double(sdr));
        UI_CHECK(std::abs(w4 - white4.r) < 2e-3f);
        UI_CHECK(std::abs(r1000 - red1000.r) < 2e-3f);
        UI_CHECK(std::abs(half4 - white4.r * 0.5f) < 2e-3f);
        UI_CHECK(std::abs(sdr - 1.f) < 1e-3f);
        std::vector<u32> pq(std::size_t(W) * H);
        void* p = nullptr;
        vkMapMemory(vk.device, bmem, 0, VK_WHOLE_SIZE, 0, &p);
        std::memcpy(pq.data(), p, pq.size() * 4);
        vkUnmapMemory(vk.device, bmem);
        auto code = [&](int x, int y) { return int(pq[std::size_t(y) * W + std::size_t(x)] & 1023u); };
        const int cw4 = code(60, 60), csdr = code(390, 60);
        const int want4 = int(std::lround(C::pqEncode(4.f * nits) * 1023.f)), wantSdr = int(std::lround(C::pqEncode(nits) * 1023.f));
        std::printf("pq        4x white code %d (want %d), SDR white %d (want %d)\n", cw4, want4, csdr, wantSdr);
        UI_CHECK(std::abs(cw4 - want4) <= 2);
        UI_CHECK(std::abs(csdr - wantSdr) <= 2);
        UI_CHECK(cw4 > csdr + 100);
        vkDestroyBuffer(vk.device, buf, nullptr);
        vkFreeMemory(vk.device, bmem, nullptr);
        vkDestroyImageView(vk.device, view, nullptr);
        vkDestroyImage(vk.device, img, nullptr);
        vkFreeMemory(vk.device, mem, nullptr);
    }
    return finish("fuse_ui_vk_hdr");
}
