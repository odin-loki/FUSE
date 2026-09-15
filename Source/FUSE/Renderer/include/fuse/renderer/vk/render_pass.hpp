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
    bool clearOnLoad = true;
    const char* debugName = nullptr;
};

struct RenderPassInfo {
    bool valid = false;
    u32 colorFormat = 0;
    std::string message;
};

/// Minimal single-color-attachment render pass for B2.8 headless raster scaffolding.
class RenderPass {
public:
    static std::unique_ptr<RenderPass> create(VulkanDevice& device, const RenderPassDesc& desc = {});
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    const RenderPassInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

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
