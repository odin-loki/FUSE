#pragma once

#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

enum class VulkanBackendMode : u8 {
    Stub = 0,
    Headless = 1,
};

struct VulkanInstanceDesc {
    const char* appName = "FUSE";
    u32 appVersion = 1;
    bool enableValidation = true;
    /// Optional WSI / platform extensions (e.g. GLFW `glfwGetRequiredInstanceExtensions`).
    /// When empty, `create` auto-requests `fuse::platform::requiredVulkanInstanceExtensions`
    /// and skips names the loader does not expose (headless CI still succeeds).
    const char* const* extraExtensions = nullptr;
    u32 extraExtensionCount = 0;
};

struct VulkanInstanceInfo {
    VulkanBackendMode mode = VulkanBackendMode::Stub;
    bool valid = false;
    u32 apiVersion = 0;
    std::string message;
    std::vector<const char*> enabledLayers;
    std::vector<const char*> enabledExtensions;

    /// True when `name` is in `enabledExtensions` (pointer-identity not required).
    bool instanceHasExtension(const char* name) const;
};

class VulkanInstance {
public:
    static std::unique_ptr<VulkanInstance> create(const VulkanInstanceDesc& desc);
    ~VulkanInstance();

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    const VulkanInstanceInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    /// Opaque handle for device creation (null when stubbed).
    void* nativeHandle() const;

private:
    VulkanInstance() = default;
    bool initialize(const VulkanInstanceDesc& desc);
    void shutdown();

    VulkanInstanceInfo m_info;
    void* m_handle = nullptr;
    void* m_debugMessenger = nullptr;
};

} // namespace fuse::renderer
