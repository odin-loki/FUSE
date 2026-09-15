#pragma once

#include <fuse/renderer/shader/shader_types.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

struct ShaderModuleInfo {
    bool valid = false;
    ShaderStage stage = ShaderStage::Vertex;
    u32 spirvWordCount = 0;
    std::string message;
};

/// Owns a Vulkan shader module when the backend is available; records metadata in stub mode.
class ShaderModule {
public:
    static std::unique_ptr<ShaderModule> create(VulkanDevice& device, ShaderStage stage,
                                                const u32* spirv, u32 wordCount);
    static std::unique_ptr<ShaderModule> createFromFile(VulkanDevice& device, ShaderStage stage,
                                                        const char* spirvPath);
    ~ShaderModule();

    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    const ShaderModuleInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }
    ShaderStage stage() const { return m_info.stage; }

    void* nativeHandle() const;

private:
    ShaderModule() = default;
    bool initialize(VulkanDevice& device, ShaderStage stage, const u32* spirv, u32 wordCount);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    ShaderModuleInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
