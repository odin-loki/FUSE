#pragma once

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

enum class GpuAllocatorMode : u8 {
    Stub,
    Vma,
};

struct GpuAllocatorInfo {
    bool valid = false;
    GpuAllocatorMode mode = GpuAllocatorMode::Stub;
    std::string message;
};

/// Optional VMA-backed GPU allocator; falls back to stub bookkeeping when VMA unavailable.
class GpuAllocator {
public:
    static std::unique_ptr<GpuAllocator> create(VulkanDevice& device);
    ~GpuAllocator();

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    const GpuAllocatorInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }
    bool isStub() const { return m_info.mode == GpuAllocatorMode::Stub; }

    void* nativeHandle() const;

    bool createBuffer(const BufferDesc& desc, Buffer& out);
    void destroyBuffer(Buffer& buffer);

    bool createImage(const TextureDesc& desc, Texture& out);
    void destroyImage(Texture& texture);

private:
    GpuAllocator() = default;
    bool initialize(VulkanDevice& device);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    GpuAllocatorInfo m_info;
    void* m_allocator = nullptr;
    u64 m_stubId = 1;
};

} // namespace fuse::renderer
