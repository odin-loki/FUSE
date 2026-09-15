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
    R16G16Sfloat = 76,
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
    bool cudaInterop = false;
    const char* name = nullptr;
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
};

/// GPU buffer resource — device_address populated when BDA extension enabled.
struct Buffer {
    void* handle = nullptr;
    void* allocation = nullptr;
    void* mapped = nullptr;
    BufferDesc desc{};
    u64 deviceAddress = 0;
    u32 bindlessIndex = UINT32_MAX;
};

struct SamplerDesc {
    u32 minFilter = 0; // VkFilter::VK_FILTER_LINEAR
    u32 magFilter = 0;
    u32 addressMode = 0; // VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT
    const char* name = nullptr;
};

struct SamplerEntry {
    void* handle = nullptr;
    u32 bindlessIndex = UINT32_MAX;
};

} // namespace fuse::renderer
