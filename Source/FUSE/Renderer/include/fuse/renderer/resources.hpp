#pragma once

#include <fuse/handle.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

struct Texture;
struct Buffer;
struct SamplerEntry;

using TextureHandle = fuse::Handle<Texture>;
using BufferHandle = fuse::Handle<Buffer>;
using SamplerHandle = fuse::Handle<SamplerEntry>;

/// Mirrors VkFormat numeric values for common paths; cast at Vulkan boundary.
enum class GpuFormat : u32 {
    Undefined = 0,
    R8G8B8A8Unorm = 37,
    R8G8B8A8Srgb = 43,
    R16G16Sfloat = 83, // VK_FORMAT_R16G16_SFLOAT (76 is R16_SFLOAT)
    R16G16B16A16Sfloat = 97,
    D32Sfloat = 126,
    R32Sfloat = 100,
};

enum class BufferUsage : u32 {
    None = 0,
    TransferSrc = 1u << 0,
    TransferDst = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Index = 1u << 4,
    Vertex = 1u << 5,
    ShaderDeviceAddress = 1u << 6,
};

enum class ImageUsage : u32 {
    None = 0,
    TransferSrc = 1u << 0,
    TransferDst = 1u << 1,
    Sampled = 1u << 2,
    Storage = 1u << 3,
    ColorAttachment = 1u << 4,
    DepthStencilAttachment = 1u << 5,
};

enum class MemoryUsage : u8 {
    GpuOnly,
    CpuToGpu,
    GpuToCpu,
    CpuOnly,
};

struct TextureDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    GpuFormat format = GpuFormat::R8G8B8A8Unorm;
    ImageUsage usage = ImageUsage::Sampled;
    bool cubeMap = false;
    bool cudaInterop = false;
    const char* name = nullptr;
    /// GpuOnly: optimal tiling, device-local. Host-visible usages (CpuToGpu / GpuToCpu / CpuOnly)
    /// use linear tiling so `Texture::mapped` addresses texel rows directly; they are limited to
    /// single-mip, single-layer 2D colour images (the Vulkan linear-tiling guarantee).
    MemoryUsage memoryUsage = MemoryUsage::GpuOnly;
};

struct BufferDesc {
    usize size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryUsage memoryUsage = MemoryUsage::GpuOnly;
    bool cudaInterop = false;
    const char* name = nullptr;
};

/// GPU image resource — native handles opaque until B2.4 descriptor updates land.
struct Texture {
    void* image = nullptr;
    void* view = nullptr;
    void* allocation = nullptr;
    TextureDesc desc{};
    u32 bindlessIndex = UINT32_MAX;
    void* exportedHandle = nullptr;  // Win32 HANDLE or fd as void*
    u64 allocationSize = 0;
    /// Current `VkImageLayout` (as u32, 0 = UNDEFINED) of mip 0 and of mips 1.. as left by the
    /// transitions ResourceManager records (uploads, mip generation, readback). Code that
    /// transitions the image elsewhere reports it through `ResourceManager::setTextureLayout`.
    u32 layout = 0;
    u32 mipTailLayout = 0;
    /// Upload-queue serial of the newest copy into this image (0: never uploaded).
    u64 lastUploadSerial = 0;
    /// Persistently mapped texels for host-visible (linear) textures; null for GpuOnly.
    void* mapped = nullptr;
    /// Row pitch of `mapped` in bytes (linear textures only).
    u64 mappedRowPitch = 0;
    /// `VkMemoryPropertyFlags` of the backing memory type (0 in the stub backend).
    u32 memoryPropertyFlags = 0;
};

/// GPU buffer resource — device_address populated when BDA extension enabled.
struct Buffer {
    void* handle = nullptr;
    void* allocation = nullptr;
    void* mapped = nullptr;
    BufferDesc desc{};
    u64 deviceAddress = 0;
    u32 bindlessIndex = UINT32_MAX;
    void* exportedHandle = nullptr;  // Win32 HANDLE or fd as void*
    u64 allocationSize = 0;
    /// Upload-queue serial of the newest copy into this buffer (0: never uploaded).
    u64 lastUploadSerial = 0;
    /// `VkMemoryPropertyFlags` of the backing memory type (0 in the stub backend).
    u32 memoryPropertyFlags = 0;
};

struct SamplerDesc {
    u32 minFilter = 0; // VkFilter numeric (0 = NEAREST, 1 = LINEAR)
    u32 magFilter = 0;
    u32 addressMode = 0; // VkSamplerAddressMode numeric (0 = REPEAT)
    bool anisotropy = false;
    float maxAnisotropy = 1.f;
    float minLod = 0.f;
    float maxLod = 1000.f;
    float mipLodBias = 0.f;
    bool compareEnable = false;
    u32 compareOp = 1; // VK_COMPARE_OP_LESS
    const char* name = nullptr;
};

struct SamplerEntry {
    /// VkSampler when FUSE_VULKAN_BACKEND and the device is valid; packed filter bits otherwise.
    void* handle = nullptr;
    u32 bindlessIndex = UINT32_MAX;
};

} // namespace fuse::renderer
