#include <fuse/renderer/vk/pipeline_cache.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

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

bool PipelineCache::recreate(VulkanDevice& device, const void* initialData, usize initialSize) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    if (m_handle != nullptr) {
        vkDestroyPipelineCache(static_cast<VkDevice>(device.nativeHandle()),
                               static_cast<VkPipelineCache>(m_handle), nullptr);
        m_handle = nullptr;
    }

    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    createInfo.initialDataSize = initialSize;
    createInfo.pInitialData = initialData;

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(static_cast<VkDevice>(device.nativeHandle()), &createInfo, nullptr,
                              &pipelineCache) != VK_SUCCESS) {
        m_info.message = "vkCreatePipelineCache failed";
        m_info.valid = false;
        return false;
    }

    m_device = &device;
    m_handle = pipelineCache;
    m_info.valid = true;
    m_info.dataByteCount = static_cast<u32>(initialSize);
    m_info.message = initialData != nullptr ? "pipeline cache restored from blob"
                                            : "in-memory pipeline cache ready";
    return true;
#else
    (void)device;
    (void)initialData;
    (void)initialSize;
    return false;
#endif
}

bool PipelineCache::initialize(VulkanDevice& device) {
    m_device = &device;
    return recreate(device, nullptr, 0);
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

bool PipelineCache::restoreFromData(const std::vector<u8>& data) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid()) {
        return false;
    }
    return recreate(*m_device, data.empty() ? nullptr : data.data(), data.size());
#else
    (void)data;
    return false;
#endif
}

bool PipelineCache::writeCacheFile(const char* path) const {
    if (path == nullptr) {
        return false;
    }

    std::vector<u8> blob;
    if (!snapshotData(blob)) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    if (!blob.empty()) {
        out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    }
    return out.good();
}

bool PipelineCache::readCacheFile(const char* path) {
    if (path == nullptr) {
        return false;
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return false;
    }

    const std::streamsize size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);

    std::vector<u8> blob(static_cast<size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(blob.data()), size);
        if (!in.good()) {
            return false;
        }
    }

    return restoreFromData(blob);
}

std::string PipelineCache::hashedFileName(u64 spirvHash) {
    std::ostringstream name;
    name << "fuse_pso_" << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
         << static_cast<unsigned long long>(spirvHash) << ".bin";
    return name.str();
}

bool PipelineCache::writeCacheFileForHash(const char* directory, u64 spirvHash) const {
    if (directory == nullptr) {
        return false;
    }
    const std::string path = std::string(directory) + "/" + hashedFileName(spirvHash);
    return writeCacheFile(path.c_str());
}

bool PipelineCache::readCacheFileForHash(const char* directory, u64 spirvHash) {
    if (directory == nullptr) {
        return false;
    }
    const std::string path = std::string(directory) + "/" + hashedFileName(spirvHash);
    return readCacheFile(path.c_str());
}

} // namespace fuse::renderer
