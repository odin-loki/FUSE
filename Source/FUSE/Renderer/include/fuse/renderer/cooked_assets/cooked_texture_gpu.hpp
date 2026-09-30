#pragma once

// E06 (AP-RT-COOKED): cooked `.fusetex` textures (fuse/asset/cooked_texture.hpp) as sampled GPU images.
//
// A cooked texture (BC1 / BC4 / BC5 / BC6H / BC7; 2D, array or cube; every mip level the cook wrote) becomes one
// VkImage with its native VK_FORMAT_BC* format (sRGB view format for sRGB BC1 / BC7) when the device enabled
// textureCompressionBC and reports SAMPLED_IMAGE + TRANSFER_DST for the format in optimal tiling; otherwise the
// blocks are decoded on the CPU (bcn_decode.hpp) into R8G8B8A8 (UNORM / SRGB) or R16G16B16A16_SFLOAT (BC6H) and
// uploaded instead. Both paths stage the whole chain (mip-major, every layer per mip) through UploadQueue::stageImage
// and record one copy; the image ends SHADER_READ_ONLY_OPTIMAL for graphics work submitted after the caller's
// flush (UploadQueue visibility contract). Sampling either image returns the same values up to the implementation's
// BCn interpolation precision (<= 2 / 255 for BC1 / BC4 / BC5 on Lavapipe; BC7 and BC6H decode exactly).

#include <fuse/asset/cooked_texture.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::cooked_assets {

/// Native Vulkan format of a cooked texture's blocks (sRGB variants for BC1 / BC7 when `srgb`).
[[nodiscard]] GpuFormat bc_native_format(asset::BcFormat format, bool srgb);
/// Format of the CPU-decoded fallback: R8G8B8A8 (sRGB when `srgb` and BC1 / BC7) or R16G16B16A16_SFLOAT (BC6H).
[[nodiscard]] GpuFormat bc_fallback_format(asset::BcFormat format, bool srgb);
[[nodiscard]] bool is_block_compressed(GpuFormat format);
/// True when `device` enabled textureCompressionBC and supports sampling + transfer-dst of `format` (optimal
/// tiling). Always false in the stub backend or without a device.
[[nodiscard]] bool device_supports_bc_format(const VulkanDevice* device, GpuFormat format);

enum class CookedTexturePath : u8 {
    Native = 0, ///< VK_FORMAT_BC* image, blocks copied as cooked
    CpuDecode,  ///< decoded on the CPU (no textureCompressionBC / unsupported format, or forced)
};

[[nodiscard]] const char* cooked_texture_path_name(CookedTexturePath path);

struct CookedTextureUploadDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    UploadQueue* upload = nullptr;
    /// Decode on the CPU even when the device samples the BC format (tests, debugging).
    bool forceCpuDecode = false;
    const char* name = "cooked_texture";
};

struct CookedTextureGpu {
    Texture image{};
    CookedTexturePath path = CookedTexturePath::Native;
    GpuFormat format = GpuFormat::Undefined;
    u64 stagedBytes = 0; ///< bytes copied through the staging ring
};

/// Creates the image and records its upload on desc.upload (the caller flushes the queue before sampling).
/// Fails (false, nothing left allocated) on an invalid texture, a chain larger than the staging ring, or an
/// allocation / staging failure.
bool upload_cooked_texture(const CookedTextureUploadDesc& desc, const asset::CookedTexture& texture,
                           CookedTextureGpu& out, std::string* error = nullptr);

} // namespace fuse::renderer::cooked_assets
