#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

struct PipelineCacheInfo {
    bool valid = false;
    u32 dataByteCount = 0;
    std::string message;
};

/// Device-scoped pipeline cache scaffold — in-memory only until serialize/restore lands.
class PipelineCache {
public:
    static std::unique_ptr<PipelineCache> create(VulkanDevice& device);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    const PipelineCacheInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }
    void* nativeHandle() const { return m_handle; }

    /// Snapshot cache blob for future disk restore (no-op when stub backend).
    bool snapshotData(std::vector<u8>& outData) const;

private:
    PipelineCache() = default;
    bool initialize(VulkanDevice& device);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    PipelineCacheInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
