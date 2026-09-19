#include <fuse/renderer/vk/pipeline_cache.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

std::unique_ptr<PipelineCache> PipelineCache::create(VulkanDevice& device) {
    auto cache = std::unique_ptr<PipelineCache>(new PipelineCache());
    if (!cache->initialize(device)) {
        cache->m_info.valid = false;
    }
    return cache;
}

PipelineCache::~PipelineCache() {
    shutdown();
}

bool PipelineCache::initialize(VulkanDevice& device) {
    m_device = &device;

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(static_cast<VkDevice>(device.nativeHandle()), &createInfo, nullptr,
                              &pipelineCache) != VK_SUCCESS) {
        m_info.message = "vkCreatePipelineCache failed";
        return false;
    }

    m_handle = pipelineCache;
    m_info.valid = true;
    m_info.message = "in-memory pipeline cache ready";
    return true;
#else
    m_info.valid = true;
    m_info.message = "pipeline cache placeholder (stub backend)";
    return true;
#endif
}

void PipelineCache::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr && m_device != nullptr && m_device->isValid()) {
        vkDestroyPipelineCache(static_cast<VkDevice>(m_device->nativeHandle()),
                               static_cast<VkPipelineCache>(m_handle), nullptr);
    }
#endif
    m_handle = nullptr;
    m_device = nullptr;
    m_info = {};
}

bool PipelineCache::snapshotData(std::vector<u8>& outData) const {
    outData.clear();
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_info.valid || m_handle == nullptr || m_device == nullptr || !m_device->isValid()) {
        return false;
    }

    size_t dataSize = 0;
    if (vkGetPipelineCacheData(static_cast<VkDevice>(m_device->nativeHandle()),
                               static_cast<VkPipelineCache>(m_handle), &dataSize, nullptr) != VK_SUCCESS) {
        return false;
    }

    outData.resize(dataSize);
    if (dataSize == 0) {
        return true;
    }

    if (vkGetPipelineCacheData(static_cast<VkDevice>(m_device->nativeHandle()),
                               static_cast<VkPipelineCache>(m_handle), &dataSize, outData.data()) !=
        VK_SUCCESS) {
        outData.clear();
        return false;
    }

    outData.resize(dataSize);
    return true;
#else
    return false;
#endif
}

} // namespace fuse::renderer
