#include <fuse/renderer/shader/shader_module.hpp>

#include <fuse/renderer/shader/shader_io.hpp>

#include <utility>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

std::unique_ptr<ShaderModule> ShaderModule::create(VulkanDevice& device, ShaderStage stage,
                                                   const u32* spirv, u32 wordCount) {
    auto module = std::unique_ptr<ShaderModule>(new ShaderModule());
    if (!module->initialize(device, stage, spirv, wordCount)) {
        module->m_info.valid = false;
    }
    return module;
}

std::unique_ptr<ShaderModule> ShaderModule::createFromFile(VulkanDevice& device, ShaderStage stage,
                                                           const char* spirvPath) {
    std::string error;
    std::vector<u32> words = loadSpirvFile(spirvPath, &error);
    if (words.empty()) {
        auto module = std::unique_ptr<ShaderModule>(new ShaderModule());
        module->m_info.stage = stage;
        module->m_info.message = error.empty() ? "failed to load SPIR-V file" : error;
        return module;
    }

    return create(device, stage, words.data(), static_cast<u32>(words.size()));
}

ShaderModule::~ShaderModule() {
    shutdown();
}

void* ShaderModule::nativeHandle() const {
    return m_handle;
}

bool ShaderModule::initialize(VulkanDevice& device, ShaderStage stage, const u32* spirv,
                              u32 wordCount) {
    m_device = &device;
    m_info.stage = stage;
    m_info.spirvWordCount = wordCount;

    if (!isValidSpirvHeader(spirv, wordCount)) {
        m_info.message = "invalid SPIR-V header";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<std::size_t>(wordCount) * sizeof(u32);
    createInfo.pCode = spirv;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateShaderModule(static_cast<VkDevice>(device.nativeHandle()), &createInfo, nullptr,
                             &shaderModule);
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreateShaderModule failed";
        return false;
    }

    m_handle = shaderModule;
    m_info.valid = true;
    m_info.message = "shader module created";
    return true;
#else
    m_info.message = "shader module recorded in stub mode";
    m_info.valid = true;
    return true;
#endif
}

void ShaderModule::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr && m_device != nullptr && m_device->isValid()) {
        vkDestroyShaderModule(static_cast<VkDevice>(m_device->nativeHandle()),
                              static_cast<VkShaderModule>(m_handle), nullptr);
    }
#endif
    m_handle = nullptr;
    m_device = nullptr;
}

} // namespace fuse::renderer
