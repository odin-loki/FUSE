// E06 (AP-RT-COOKED): see include/fuse/renderer/cooked_assets/cooked_texture_gpu.hpp.
#include <fuse/renderer/cooked_assets/cooked_texture_gpu.hpp>

#include <fuse/renderer/cooked_assets/bcn_decode.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::cooked_assets {

namespace {

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

} // namespace

GpuFormat bc_native_format(asset::BcFormat format, bool srgb) {
    switch (format) {
    case asset::BcFormat::BC1:
        return srgb ? GpuFormat::Bc1RgbSrgb : GpuFormat::Bc1RgbUnorm;
    case asset::BcFormat::BC4:
        return GpuFormat::Bc4Unorm;
    case asset::BcFormat::BC5:
        return GpuFormat::Bc5Unorm;
    case asset::BcFormat::BC6H:
        return GpuFormat::Bc6hUfloat;
    case asset::BcFormat::BC7:
        return srgb ? GpuFormat::Bc7Srgb : GpuFormat::Bc7Unorm;
    }
    return GpuFormat::Undefined;
}

GpuFormat bc_fallback_format(asset::BcFormat format, bool srgb) {
    if (format == asset::BcFormat::BC6H) {
        return GpuFormat::R16G16B16A16Sfloat;
    }
    const bool colour = format == asset::BcFormat::BC1 || format == asset::BcFormat::BC7;
    return colour && srgb ? GpuFormat::R8G8B8A8Srgb : GpuFormat::R8G8B8A8Unorm;
}

bool is_block_compressed(GpuFormat format) {
    switch (format) {
    case GpuFormat::Bc1RgbUnorm:
    case GpuFormat::Bc1RgbSrgb:
    case GpuFormat::Bc4Unorm:
    case GpuFormat::Bc5Unorm:
    case GpuFormat::Bc6hUfloat:
    case GpuFormat::Bc7Unorm:
    case GpuFormat::Bc7Srgb:
        return true;
    default:
        return false;
    }
}

bool device_supports_bc_format(const VulkanDevice* device, GpuFormat format) {
#if defined(FUSE_VULKAN_BACKEND)
    if (device == nullptr || !device->isValid() || !device->info().textureCompressionBC ||
        device->nativePhysicalDevice() == nullptr) {
        return false;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(static_cast<VkPhysicalDevice>(device->nativePhysicalDevice()),
                                        static_cast<VkFormat>(static_cast<u32>(format)), &props);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (props.optimalTilingFeatures & needed) == needed;
#else
    (void)device;
    (void)format;
    return false;
#endif
}

const char* cooked_texture_path_name(CookedTexturePath path) {
    return path == CookedTexturePath::Native ? "native" : "cpu_decode";
}

bool upload_cooked_texture(const CookedTextureUploadDesc& desc, const asset::CookedTexture& texture,
                           CookedTextureGpu& out, std::string* error) {
    out = CookedTextureGpu{};
    if (desc.allocator == nullptr || desc.upload == nullptr) {
        return fail(error, "cooked texture upload: no allocator / upload queue");
    }
    if (texture.width == 0u || texture.height == 0u || texture.layers == 0u || texture.levels.empty() ||
        (texture.cube && texture.layers % 6u != 0u)) {
        return fail(error, "cooked texture upload: empty or inconsistent texture");
    }
    const u32 levels = static_cast<u32>(texture.levels.size());
    const u32 blockBytes = asset::bc_block_bytes(texture.format);
    for (u32 l = 0; l < levels; ++l) {
        const asset::CookedTexture::Level& level = texture.levels[l];
        const u32 w = texture.width >> l > 0u ? texture.width >> l : 1u;
        const u32 h = texture.height >> l > 0u ? texture.height >> l : 1u;
        if (level.width != w || level.height != h ||
            level.blocks.size() != static_cast<usize>(asset::bc_block_count(w, h)) * blockBytes * texture.layers) {
            return fail(error, "cooked texture upload: level " + std::to_string(l) + " does not match the header");
        }
    }
    const GpuFormat native = bc_native_format(texture.format, texture.srgb);
    const bool nativePath = !desc.forceCpuDecode && device_supports_bc_format(desc.device, native);

    UploadImageDesc ud{};
    ud.width = texture.width;
    ud.height = texture.height;
    ud.mipLevels = levels;
    ud.layerCount = texture.layers;
    std::vector<u8> chain;
    if (nativePath) {
        ud.bytesPerTexel = blockBytes;
        ud.blockWidth = 4u;
        ud.blockHeight = 4u;
        usize total = 0;
        for (const asset::CookedTexture::Level& level : texture.levels) {
            total += level.blocks.size();
        }
        chain.reserve(total);
        for (const asset::CookedTexture::Level& level : texture.levels) {
            chain.insert(chain.end(), level.blocks.begin(), level.blocks.end());
        }
    } else {
        DecodedTexture decoded;
        if (!decode_cooked_texture(texture, decoded)) {
            return fail(error, "cooked texture upload: CPU decode failed");
        }
        ud.bytesPerTexel = decoded.texelBytes;
        chain = std::move(decoded.bytes);
    }
    const usize staging = UploadQueue::imageStagingBytes(ud);
    if (staging == 0u || staging > desc.upload->ringCapacity()) {
        return fail(error, "cooked texture upload: " + std::to_string(staging) + " staged bytes exceed the " +
                               std::to_string(desc.upload->ringCapacity()) + "-byte staging ring");
    }

    TextureDesc td{};
    td.width = texture.width;
    td.height = texture.height;
    td.mipLevels = levels;
    td.arrayLayers = texture.layers;
    td.cubeMap = texture.cube;
    td.format = nativePath ? native : bc_fallback_format(texture.format, texture.srgb);
    td.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
    td.name = desc.name;
    if (!desc.allocator->createImage(td, out.image)) {
        out = CookedTextureGpu{};
        return fail(error, "cooked texture upload: image creation failed");
    }
    usize offset = 0;
    if (!desc.upload->stageImage(chain.data(), ud, offset) || !desc.upload->recordImageCopy(out.image.image, offset, ud)) {
        desc.allocator->destroyImage(out.image);
        out = CookedTextureGpu{};
        return fail(error, "cooked texture upload: staging / copy recording failed");
    }
    out.path = nativePath ? CookedTexturePath::Native : CookedTexturePath::CpuDecode;
    out.format = td.format;
    out.stagedBytes = static_cast<u64>(UploadQueue::imageSourceBytes(ud));
    return true;
}

} // namespace fuse::renderer::cooked_assets
