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

/// Device-scoped pipeline cache — in-memory + optional disk serialize/restore.
class PipelineCache {
public:
    static std::unique_ptr<PipelineCache> create(VulkanDevice& device);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    const PipelineCacheInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }
    void* nativeHandle() const { return m_handle; }

    /// Snapshot cache blob for disk restore.
    bool snapshotData(std::vector<u8>& outData) const;

    /// Recreate cache from a prior `snapshotData` blob (no-op when stub backend).
    bool restoreFromData(const std::vector<u8>& data);

    /// Write/read cache blob to disk for developer warm-start (returns false on stub backend).
    bool writeCacheFile(const char* path) const;
    bool readCacheFile(const char* path);

    /// "fuse_pso_<16 lowercase hex digits>.bin" from spirvHash; hash 0 still produces a well-formed name.
    static std::string hashedFileName(u64 spirvHash);

    /// Writes snapshot to directory/hashedFileName(spirvHash). directory must be non-null.
    bool writeCacheFileForHash(const char* directory, u64 spirvHash) const;

    /// Reads directory/hashedFileName(spirvHash) via readCacheFile.
    bool readCacheFileForHash(const char* directory, u64 spirvHash);

private:
    PipelineCache() = default;
    bool initialize(VulkanDevice& device);
    bool recreate(VulkanDevice& device, const void* initialData, usize initialSize);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    PipelineCacheInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
