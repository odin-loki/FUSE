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
};

struct VulkanInstanceInfo {
    VulkanBackendMode mode = VulkanBackendMode::Stub;
    bool valid = false;
    u32 apiVersion = 0;
    std::string message;
    std::vector<const char*> enabledLayers;
    std::vector<const char*> enabledExtensions;
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
