// ui.encode_pq / ui.encode_scrgb (UiVkEncoder): the GPU HDR output encode matches the CPU reference
// (color::Hdr10Packer / toHdr10 within 2 of 1023; scRGB linear * sdrWhite / 80 within half-float precision).
#include "fuse/ui/color_space.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#include "test_util.hpp"
#include "vk_headless.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

/// Encodes `src` into a fresh image of `fmt` (bytes per pixel `bpp`) and returns its raw bytes.
std::vector<u8> encode(VkHeadless& vk, UiVkEncoder& enc, const Image& src, VkFormat fmt, u32 bpp, f32 nits) {
    const u32 W = vk.w, H = vk.h;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
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
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    vkCreateImageView(vk.device, &vi, nullptr, &view);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(W) * H * bpp;
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
    Image ignored;
    vk.run(src, ignored, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        enc.record(cmd, 0, vk.view, view, 0, 0, int(W), int(H), nits);
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    });
    std::vector<u8> bytes(std::size_t(W) * H * bpp);
    void* p = nullptr;
    vkMapMemory(vk.device, bmem, 0, VK_WHOLE_SIZE, 0, &p);
    std::memcpy(bytes.data(), p, bytes.size());
    vkUnmapMemory(vk.device, bmem);
    vkDestroyBuffer(vk.device, buf, nullptr);
    vkFreeMemory(vk.device, bmem, nullptr);
    vkDestroyImageView(vk.device, view, nullptr);
    vkDestroyImage(vk.device, img, nullptr);
    vkFreeMemory(vk.device, mem, nullptr);
    return bytes;
}
} // namespace

int main() {
    if (!UiVkEncoder::kernelAvailable(UiVkEncoder::Mode::Hdr10) || !UiVkEncoder::kernelAvailable(UiVkEncoder::Mode::ScRgb)) {
        std::printf("SKIP: encode kernels not compiled in\n");
        return 77;
    }
    constexpr u32 W = 256, H = 64;
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    Image src; // every 8-bit level on each channel, mixed with the others, plus alpha steps
    src.resize(int(W), int(H));
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x)
            src.pixels[std::size_t(y) * W + x] = x | (((x * 7 + y * 13) & 0xFFu) << 8) | (((y * 4) & 0xFFu) << 16) |
                                                (u32(y * 4 & 0xFFu) << 24);
    const f32 nits = 203.f;
    {
        UiVkEncoder enc;
        UI_CHECK(enc.init((void*)vk.device, UiVkEncoder::Mode::Hdr10, 1));
        UI_CHECK(enc.outputFormat() == u32(VK_FORMAT_A2B10G10R10_UNORM_PACK32));
        const std::vector<u8> out = encode(vk, enc, src, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, nits);
        const C::Hdr10Packer pk(nits);
        int worst = 0, worstA = 0;
        for (std::size_t i = 0; i < std::size_t(W) * H; ++i) {
            u32 g;
            std::memcpy(&g, out.data() + i * 4, 4);
            const u32 c = pk.pack(src.pixels[i]);
            for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(int((g >> (10 * k)) & 1023u) - int((c >> (10 * k)) & 1023u)));
            worstA = std::max(worstA, std::abs(int(g >> 30) - int(std::lround(f32(src.pixels[i] >> 24) / 255.f * 3.f))));
        }
        std::printf("hdr10   max code err %d (alpha %d)\n", worst, worstA);
        UI_CHECK(worst <= 2);
        UI_CHECK(worstA <= 1);
    }
    {
        UiVkEncoder enc;
        UI_CHECK(enc.init((void*)vk.device, UiVkEncoder::Mode::ScRgb, 1));
        const std::vector<u8> out = encode(vk, enc, src, VK_FORMAT_R16G16B16A16_SFLOAT, 8, nits);
        f32 worst = 0;
        for (std::size_t i = 0; i < std::size_t(W) * H; ++i) {
            u16 h[4];
            std::memcpy(h, out.data() + i * 8, 8);
            const u32 p = src.pixels[i];
            for (int k = 0; k < 4; ++k) {
                const f32 v = f32((p >> (8 * k)) & 0xFFu) / 255.f;
                const f32 want = k == 3 ? v : C::srgbToLinear(v) * nits / 80.f;
                worst = std::max(worst, std::abs(halfToFloat(h[k]) - want) / std::max(want, 1e-3f));
            }
        }
        std::printf("scrgb   max rel err %.5f\n", double(worst));
        UI_CHECK(worst <= 2e-3f);
    }
    return finish("fuse_ui_vk_encode");
}
