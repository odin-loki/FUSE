#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace fuse::jobs {
class JobScheduler;
}

namespace fuse::renderer {

struct PipelineCacheInfo {
    bool valid = false;
    u32 dataByteCount = 0;
    std::string message;
};

// ---- Pipeline cache v2 (WP-0.5) ------------------------------------------------------------------
//
// On-disk file "fuse_pso2_<16 hex>.bin", the hex being PipelineCacheKey::hash():
//   PipelineCacheFileHeader (little endian, fixed 128 bytes; see pipeline_cache.cpp)
//   u64 manifest[manifestCount]   pipeline keys created with this cache (warm set)
//   u8  blob[blobBytes]           vkGetPipelineCacheData output
// The header carries magic, format version, the full key, a hash over manifest + blob and a hash
// over the header itself. Files are written to a unique temp file, flushed and renamed over the
// target (atomic on POSIX and NTFS). Any mismatch or damage on load is recovered: corrupt files
// are moved aside to "<name>.corrupt" and the cache starts empty; a key mismatch (driver update,
// other GPU, changed shaders) is simply a different file name, so it never overwrites another
// device's cache.

/// What a cache file is valid for. Two runs share a cache only when every field matches.
struct PipelineCacheKey {
    u32 vendorID = 0;
    u32 deviceID = 0;
    u32 driverVersion = 0;
    u32 apiVersion = 0;
    u8 pipelineCacheUUID[16] = {};
    u8 deviceUUID[16] = {};
    u8 driverUUID[16] = {};
    /// Caller-supplied hash of the shader content the cache belongs to (e.g. hashShaderContent over
    /// every SPIR-V module of a build). Different content -> different file.
    u64 contentHash = 0;

    u64 hash() const;
    bool operator==(const PipelineCacheKey& other) const;
    bool operator!=(const PipelineCacheKey& other) const { return !(*this == other); }
};

enum class PipelineCacheLoadStatus : u8 {
    Loaded,         ///< File matched and the driver accepted the blob.
    Missing,        ///< No file for this key (cold start).
    KeyMismatch,    ///< File header names another device / driver / content (left untouched).
    Corrupt,        ///< Bad magic, version, sizes or hashes; moved to "<file>.corrupt".
    DriverRejected, ///< Header fine but the driver refused the blob; manifest kept, blob dropped.
    Unavailable,    ///< Stub backend or invalid cache.
};

const char* pipelineCacheLoadStatusName(PipelineCacheLoadStatus status);

struct PipelineCacheLoadResult {
    PipelineCacheLoadStatus status = PipelineCacheLoadStatus::Unavailable;
    std::string path;
    std::string message;
    u32 manifestCount = 0;
    u64 blobBytes = 0;
};

/// Warm-start accounting. A lookup is a pipeline creation through the cache; it is a hit when the
/// pipeline key was in the manifest restored by loadFromDirectory (the pipeline was built with this
/// cache on a previous run). Driver-level hits come from VkPipelineCreationFeedback (core 1.3) and
/// are reported separately: drivers without a real cache (Lavapipe) never set the hit bit.
struct PipelineCacheStats {
    u32 lookups = 0;
    u32 warmHits = 0;
    u32 misses = 0;
    u32 feedbackReports = 0;
    u32 driverHits = 0;
    u32 compileRequired = 0; ///< FAIL_ON_PIPELINE_COMPILE_REQUIRED probes that were not cached.
    u32 precompileScheduled = 0;
    u32 precompileSucceeded = 0;
    u32 precompileFailed = 0;

    /// warmHits / lookups (1 when there were no lookups).
    double warmHitRate() const { return lookups == 0u ? 1.0 : static_cast<double>(warmHits) / lookups; }
};

/// Mixes `value` into a pipeline key (FNV-1a over its 8 bytes). Pipelines derive their manifest
/// key from shader SPIR-V hashes plus fixed-function state with this.
u64 combinePipelineKey(u64 seed, u64 value);

/// Creation outcome of one pipeline built through a PipelineCache (ComputePipelineInfo /
/// GraphicsPipelineInfo::cache).
struct PipelineCacheUse {
    u64 key = 0;               ///< Manifest key used for warm-start accounting.
    bool warmStart = false;    ///< Key was restored from disk.
    bool feedbackValid = false;
    bool driverCacheHit = false; ///< VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT
    bool compileRequired = false; ///< failIfNotCached probe returned VK_PIPELINE_COMPILE_REQUIRED
    u64 durationNs = 0;
};

/// FNV-1a over SPIR-V words of several modules, for PipelineCacheKey::contentHash.
u64 hashShaderContent(const std::vector<const std::vector<u32>*>& modules);

struct PipelineCacheDesc {
    /// Set when the device was created with VkPhysicalDeviceVulkan13Features::
    /// pipelineCreationCacheControl (VK_EXT_pipeline_creation_cache_control). Enables
    /// VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT probes (ComputePipelineDesc::
    /// failIfNotCached). Ignored unless the physical device supports the feature.
    bool creationCacheControlEnabled = false;
};

/// Device-scoped pipeline cache — in-memory + optional disk serialize/restore.
class PipelineCache {
public:
    static std::unique_ptr<PipelineCache> create(VulkanDevice& device);
    static std::unique_ptr<PipelineCache> create(VulkanDevice& device, const PipelineCacheDesc& desc);
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

    // ---- v2 ---------------------------------------------------------------------------------------

    /// Key of this device (IDs, driver version, pipelineCacheUUID, deviceUUID, driverUUID) and
    /// `contentHash`. Zeroed IDs in the stub backend.
    PipelineCacheKey makeKey(u64 contentHash) const;

    /// "fuse_pso2_<16 lowercase hex of key.hash()>.bin".
    static std::string fileNameV2(const PipelineCacheKey& key);

    /// Restores `directory/fileNameV2(makeKey(contentHash))`: validates the FUSE header, both hashes
    /// and the Vulkan blob header (vendor, device, pipelineCacheUUID) before the driver sees the
    /// data, then recreates the VkPipelineCache from it and loads the manifest (warm set).
    PipelineCacheLoadResult loadFromDirectory(const char* directory, u64 contentHash);

    /// Writes the current blob + manifest atomically to `directory/fileNameV2(makeKey(contentHash))`
    /// (creates `directory`). Returns false on the stub backend or any I/O failure; a failed write
    /// never leaves a partial file under the final name.
    bool saveToDirectory(const char* directory, u64 contentHash, std::string* error = nullptr) const;

    /// v2 file I/O on explicit paths (loadFromDirectory / saveToDirectory use these).
    PipelineCacheLoadResult loadFileV2(const char* path, const PipelineCacheKey& key);
    bool saveFileV2(const char* path, const PipelineCacheKey& key, std::string* error = nullptr) const;

    /// True when `pipelineKey` was restored from disk (built with this cache on an earlier run).
    bool isWarm(u64 pipelineKey) const;
    /// Pipeline creation through this cache: counts a lookup (warm hit or miss), adds the key to the
    /// manifest, and records creation feedback when `feedbackValid`. Thread-safe. Called by
    /// ComputePipeline / GraphicsPipeline.
    void notePipelineCreated(u64 pipelineKey, bool feedbackValid, bool driverCacheHit);
    /// A FAIL_ON_PIPELINE_COMPILE_REQUIRED probe missed (the pipeline was not built).
    void noteCompileRequired();
    u32 manifestSize() const;
    std::vector<u64> manifestKeys() const;
    PipelineCacheStats stats() const;
    void resetStats();

    /// VK_EXT_pipeline_creation_cache_control usable (desc flag + physical device support).
    bool creationCacheControl() const { return m_creationCacheControl; }
    /// VkPipelineCreationFeedback can be chained (Vulkan 1.3 device).
    bool creationFeedback() const { return m_creationFeedback; }

    // ---- background precompile -------------------------------------------------------------------

    /// Builds (and discards or keeps) the pipeline for `pipelineKey` through this cache. Runs on job
    /// worker threads: must only touch thread-safe state. Returns false when it cannot build the key.
    using PrecompileHook = std::function<bool(PipelineCache& cache, u64 pipelineKey)>;
    void setPrecompileHook(PrecompileHook hook);

    /// Runs the hook for every restored manifest key that has not been created in this session yet
    /// (the warm set), as jobs on `scheduler` (inline when null or not initialized). Returns the
    /// number of keys scheduled. Pair with waitForPrecompile() before saving or destroying.
    u32 precompileWarmSet(jobs::JobScheduler* scheduler);
    /// Blocks until every scheduled precompile job finished.
    void waitForPrecompile();

private:
    PipelineCache() = default;
    bool initialize(VulkanDevice& device);
    bool recreate(VulkanDevice& device, const void* initialData, usize initialSize);
    void shutdown();
    bool blobMatchesDevice(const u8* blob, usize size, std::string* reason) const;

    VulkanDevice* m_device = nullptr;
    PipelineCacheInfo m_info;
    void* m_handle = nullptr;
    bool m_creationCacheControl = false;
    bool m_creationFeedback = false;

    mutable std::mutex m_mutex;
    std::unordered_set<u64> m_restored;  ///< manifest from disk (warm set)
    std::unordered_set<u64> m_manifest;  ///< restored + created this session
    std::unordered_set<u64> m_created;   ///< created this session
    PipelineCacheStats m_stats;
    PrecompileHook m_precompileHook;
    u32 m_precompilePending = 0;
    std::condition_variable m_precompileDone;
};

} // namespace fuse::renderer
