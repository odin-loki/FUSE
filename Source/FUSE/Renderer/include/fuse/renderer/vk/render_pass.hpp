#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

/// Color attachment format without pulling vulkan.h into public headers.
/// Default: VK_FORMAT_R8G8B8A8_UNORM (37).
struct RenderPassDesc {
    u32 colorFormat = 37;
    /// 0 = color only; 126 = D32_SFLOAT (`GpuFormat::D32Sfloat`).
    u32 depthFormat = 0;
    bool clearOnLoad = true;
    const char* debugName = nullptr;

    bool hasDepth() const { return depthFormat != 0; }
};

struct RenderPassInfo {
    bool valid = false;
    u32 colorFormat = 0;
    u32 depthFormat = 0;
    std::string message;
};

/// Minimal color render pass for B2.8 headless raster scaffolding, with optional depth.
class RenderPass {
public:
    static std::unique_ptr<RenderPass> create(VulkanDevice& device, const RenderPassDesc& desc = {});
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    const RenderPassInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }
    bool hasDepth() const { return m_info.depthFormat != 0; }

    void* nativeHandle() const;

private:
    RenderPass() = default;
    bool initialize(VulkanDevice& device, const RenderPassDesc& desc);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    RenderPassInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
