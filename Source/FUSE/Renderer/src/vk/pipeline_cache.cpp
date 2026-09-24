#include <fuse/renderer/vk/pipeline_cache.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fuse::renderer {

namespace {

constexpr char kMagic[8] = {'F', 'U', 'S', 'E', 'P', 'S', 'O', '2'};
constexpr u32 kFormatVersion = 2;
constexpr usize kHeaderBytes = 128;
constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 fnv1a(const u8* data, usize size, u64 hash = kFnvOffset) {
    for (usize i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

void putU32(u8* out, u32 value) {
    for (u32 i = 0; i < 4u; ++i) {
        out[i] = static_cast<u8>((value >> (8u * i)) & 0xFFu);
    }
}
void putU64(u8* out, u64 value) {
    for (u32 i = 0; i < 8u; ++i) {
        out[i] = static_cast<u8>((value >> (8u * i)) & 0xFFu);
    }
}
u32 getU32(const u8* in) {
    u32 value = 0;
    for (u32 i = 0; i < 4u; ++i) {
        value |= static_cast<u32>(in[i]) << (8u * i);
    }
    return value;
}
u64 getU64(const u8* in) {
    u64 value = 0;
    for (u32 i = 0; i < 8u; ++i) {
        value |= static_cast<u64>(in[i]) << (8u * i);
    }
    return value;
}

// Header field offsets (bytes). Fixed layout, little endian; see pipeline_cache.hpp.
constexpr usize kOffVersion = 8;
constexpr usize kOffHeaderBytes = 12;
constexpr usize kOffVendor = 16;
constexpr usize kOffDevice = 20;
constexpr usize kOffDriver = 24;
constexpr usize kOffApi = 28;
constexpr usize kOffCacheUUID = 32;
constexpr usize kOffDeviceUUID = 48;
constexpr usize kOffDriverUUID = 64;
constexpr usize kOffContent = 80;
constexpr usize kOffManifestCount = 88;
constexpr usize kOffBlobBytes = 96;
constexpr usize kOffPayloadHash = 104;
constexpr usize kOffKeyHash = 112;
constexpr usize kOffHeaderHash = 120;

void writeKey(u8* header, const PipelineCacheKey& key) {
    putU32(header + kOffVendor, key.vendorID);
    putU32(header + kOffDevice, key.deviceID);
    putU32(header + kOffDriver, key.driverVersion);
    putU32(header + kOffApi, key.apiVersion);
    std::memcpy(header + kOffCacheUUID, key.pipelineCacheUUID, 16);
    std::memcpy(header + kOffDeviceUUID, key.deviceUUID, 16);
    std::memcpy(header + kOffDriverUUID, key.driverUUID, 16);
    putU64(header + kOffContent, key.contentHash);
}

PipelineCacheKey readKey(const u8* header) {
    PipelineCacheKey key;
    key.vendorID = getU32(header + kOffVendor);
    key.deviceID = getU32(header + kOffDevice);
    key.driverVersion = getU32(header + kOffDriver);
    key.apiVersion = getU32(header + kOffApi);
    std::memcpy(key.pipelineCacheUUID, header + kOffCacheUUID, 16);
    std::memcpy(key.deviceUUID, header + kOffDeviceUUID, 16);
    std::memcpy(key.driverUUID, header + kOffDriverUUID, 16);
    key.contentHash = getU64(header + kOffContent);
    return key;
}

std::string uniqueTempSuffix() {
    static std::atomic<u32> counter{0};
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    return ".tmp." + std::to_string(pid) + "." + std::to_string(counter.fetch_add(1u));
}

/// Writes `bytes` to `path` via a unique temp file + flush + fsync + rename.
bool writeFileAtomic(const std::string& path, const std::vector<u8>& bytes, std::string* error) {
    const std::string temp = path + uniqueTempSuffix();
    std::FILE* file = std::fopen(temp.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = "cannot open " + temp;
        }
        return false;
    }
    bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    ok = ok && std::fflush(file) == 0;
#if defined(_WIN32)
    ok = ok && _commit(_fileno(file)) == 0;
#else
    ok = ok && ::fsync(fileno(file)) == 0;
#endif
    ok = (std::fclose(file) == 0) && ok;
    if (!ok) {
        std::remove(temp.c_str());
        if (error != nullptr) {
            *error = "write failed for " + temp;
        }
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::remove(temp.c_str());
        if (error != nullptr) {
            *error = "rename to " + path + " failed: " + ec.message();
        }
        return false;
    }
    return true;
}

bool readWholeFile(const std::string& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

void moveAside(const std::string& path) {
    std::error_code ec;
    const std::string aside = path + ".corrupt";
    std::filesystem::remove(aside, ec);
    std::filesystem::rename(path, aside, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
    }
}

} // namespace

const char* pipelineCacheLoadStatusName(PipelineCacheLoadStatus status) {
    switch (status) {
    case PipelineCacheLoadStatus::Loaded:
        return "loaded";
    case PipelineCacheLoadStatus::Missing:
        return "missing";
    case PipelineCacheLoadStatus::KeyMismatch:
        return "key-mismatch";
    case PipelineCacheLoadStatus::Corrupt:
        return "corrupt";
    case PipelineCacheLoadStatus::DriverRejected:
        return "driver-rejected";
    case PipelineCacheLoadStatus::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

u64 PipelineCacheKey::hash() const {
    u8 bytes[kHeaderBytes] = {};
    writeKey(bytes, *this);
    return fnv1a(bytes + kOffVendor, kOffManifestCount - kOffVendor);
}

bool PipelineCacheKey::operator==(const PipelineCacheKey& other) const {
    return vendorID == other.vendorID && deviceID == other.deviceID && driverVersion == other.driverVersion &&
           apiVersion == other.apiVersion && contentHash == other.contentHash &&
           std::memcmp(pipelineCacheUUID, other.pipelineCacheUUID, 16) == 0 &&
           std::memcmp(deviceUUID, other.deviceUUID, 16) == 0 && std::memcmp(driverUUID, other.driverUUID, 16) == 0;
}

u64 combinePipelineKey(u64 seed, u64 value) {
    u8 bytes[8];
    putU64(bytes, value);
    return fnv1a(bytes, sizeof(bytes), seed == 0u ? kFnvOffset : seed);
}

u64 hashShaderContent(const std::vector<const std::vector<u32>*>& modules) {
    u64 hash = kFnvOffset;
    for (const std::vector<u32>* words : modules) {
        if (words == nullptr) {
            continue;
        }
        const u64 size = words->size();
        hash = fnv1a(reinterpret_cast<const u8*>(&size), sizeof(size), hash);
        if (!words->empty()) {
            hash = fnv1a(reinterpret_cast<const u8*>(words->data()), words->size() * sizeof(u32), hash);
        }
    }
    return hash;
}

std::unique_ptr<PipelineCache> PipelineCache::create(VulkanDevice& device) {
    return create(device, PipelineCacheDesc{});
}

std::unique_ptr<PipelineCache> PipelineCache::create(VulkanDevice& device, const PipelineCacheDesc& desc) {
    auto cache = std::unique_ptr<PipelineCache>(new PipelineCache());
    if (!cache->initialize(device)) {
        cache->m_info.valid = false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (cache->m_info.valid) {
        cache->m_creationFeedback = device.info().apiVersion >= VK_API_VERSION_1_3;
        if (desc.creationCacheControlEnabled && device.nativePhysicalDevice() != nullptr &&
            device.info().apiVersion >= VK_API_VERSION_1_3) {
            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(static_cast<VkPhysicalDevice>(device.nativePhysicalDevice()), &features);
            cache->m_creationCacheControl = features13.pipelineCreationCacheControl == VK_TRUE;
        }
    }
#else
    (void)desc;
#endif
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
    nameVkObject(device.nativeHandle(), vk_object_type::kPipelineCache, m_handle, "fuse.pipeline_cache");
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
    waitForPrecompile();
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

// ---- v2 ------------------------------------------------------------------------------------------

PipelineCacheKey PipelineCache::makeKey(u64 contentHash) const {
    PipelineCacheKey key;
    key.contentHash = contentHash;
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid() && m_device->nativePhysicalDevice() != nullptr) {
        VkPhysicalDeviceIDProperties ids{};
        ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &ids;
        vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice()), &props);
        key.vendorID = props.properties.vendorID;
        key.deviceID = props.properties.deviceID;
        key.driverVersion = props.properties.driverVersion;
        key.apiVersion = props.properties.apiVersion;
        std::memcpy(key.pipelineCacheUUID, props.properties.pipelineCacheUUID, 16);
        std::memcpy(key.deviceUUID, ids.deviceUUID, 16);
        std::memcpy(key.driverUUID, ids.driverUUID, 16);
    }
#endif
    return key;
}

std::string PipelineCache::fileNameV2(const PipelineCacheKey& key) {
    std::ostringstream name;
    name << "fuse_pso2_" << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
         << static_cast<unsigned long long>(key.hash()) << ".bin";
    return name.str();
}

bool PipelineCache::blobMatchesDevice(const u8* blob, usize size, std::string* reason) const {
    // VkPipelineCacheHeaderVersionOne: headerSize, headerVersion, vendorID, deviceID, UUID[16].
    if (size < 32u) {
        *reason = "blob smaller than VkPipelineCacheHeaderVersionOne";
        return false;
    }
    const u32 headerSize = getU32(blob);
    const u32 headerVersion = getU32(blob + 4);
    if (headerSize < 32u || headerSize > size || headerVersion != 1u /* VK_PIPELINE_CACHE_HEADER_VERSION_ONE */) {
        *reason = "blob has a malformed Vulkan cache header";
        return false;
    }
    const PipelineCacheKey self = makeKey(0);
    if (getU32(blob + 8) != self.vendorID || getU32(blob + 12) != self.deviceID ||
        std::memcmp(blob + 16, self.pipelineCacheUUID, 16) != 0) {
        *reason = "blob was written by another device or driver";
        return false;
    }
    return true;
}

PipelineCacheLoadResult PipelineCache::loadFileV2(const char* path, const PipelineCacheKey& key) {
    PipelineCacheLoadResult result;
    result.path = path != nullptr ? path : "";
    if (path == nullptr || !m_info.valid) {
        result.status = PipelineCacheLoadStatus::Unavailable;
        result.message = path == nullptr ? "null path" : "pipeline cache unavailable";
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        result.status = PipelineCacheLoadStatus::Missing;
        result.message = "no cache file";
        return result;
    }
    std::vector<u8> bytes;
    auto corrupt = [&](const std::string& why) {
        moveAside(result.path);
        result.status = PipelineCacheLoadStatus::Corrupt;
        result.message = why + " (moved to .corrupt, starting cold)";
        return result;
    };
    if (!readWholeFile(result.path, bytes)) {
        return corrupt("unreadable");
    }
    if (bytes.size() < kHeaderBytes) {
        return corrupt("truncated header");
    }
    const u8* header = bytes.data();
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        return corrupt("bad magic");
    }
    if (getU32(header + kOffVersion) != kFormatVersion || getU32(header + kOffHeaderBytes) != kHeaderBytes) {
        return corrupt("unsupported format version");
    }
    if (fnv1a(header, kOffHeaderHash) != getU64(header + kOffHeaderHash)) {
        return corrupt("header hash mismatch");
    }
    const PipelineCacheKey fileKey = readKey(header);
    if (fileKey != key || getU64(header + kOffKeyHash) != key.hash()) {
        result.status = PipelineCacheLoadStatus::KeyMismatch;
        result.message = "cache file belongs to another device, driver or shader set";
        return result;
    }
    const u64 manifestCount = getU32(header + kOffManifestCount);
    const u64 blobBytes = getU64(header + kOffBlobBytes);
    if (manifestCount > (bytes.size() - kHeaderBytes) / 8u ||
        kHeaderBytes + manifestCount * 8u + blobBytes != bytes.size()) {
        return corrupt("payload size mismatch");
    }
    const u8* payload = bytes.data() + kHeaderBytes;
    const usize payloadBytes = bytes.size() - kHeaderBytes;
    if (fnv1a(payload, payloadBytes) != getU64(header + kOffPayloadHash)) {
        return corrupt("payload hash mismatch");
    }

    std::unordered_set<u64> manifest;
    for (u64 i = 0; i < manifestCount; ++i) {
        manifest.insert(getU64(payload + i * 8u));
    }
    const u8* blob = payload + manifestCount * 8u;
    result.manifestCount = static_cast<u32>(manifestCount);
    result.blobBytes = blobBytes;

    bool blobAccepted = true;
    std::string reason;
    if (blobBytes > 0u && !blobMatchesDevice(blob, static_cast<usize>(blobBytes), &reason)) {
        blobAccepted = false;
    } else if (m_device != nullptr && !recreate(*m_device, blobBytes > 0u ? blob : nullptr,
                                                static_cast<usize>(blobBytes))) {
        blobAccepted = false;
        reason = "vkCreatePipelineCache rejected the blob";
    }
    if (!blobAccepted && m_device != nullptr) {
        recreate(*m_device, nullptr, 0);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_restored = manifest;
        m_manifest.insert(manifest.begin(), manifest.end());
    }
    result.status = blobAccepted ? PipelineCacheLoadStatus::Loaded : PipelineCacheLoadStatus::DriverRejected;
    result.message = blobAccepted ? "pipeline cache v2 restored" : reason + " (manifest kept, blob dropped)";
    return result;
}

bool PipelineCache::saveFileV2(const char* path, const PipelineCacheKey& key, std::string* error) const {
    if (path == nullptr) {
        if (error != nullptr) {
            *error = "null path";
        }
        return false;
    }
    std::vector<u8> blob;
    if (!snapshotData(blob)) {
        if (error != nullptr) {
            *error = "vkGetPipelineCacheData unavailable";
        }
        return false;
    }
    std::vector<u64> keys = manifestKeys();
    std::vector<u8> bytes(kHeaderBytes + keys.size() * 8u + blob.size(), 0u);
    u8* header = bytes.data();
    std::memcpy(header, kMagic, sizeof(kMagic));
    putU32(header + kOffVersion, kFormatVersion);
    putU32(header + kOffHeaderBytes, static_cast<u32>(kHeaderBytes));
    writeKey(header, key);
    putU32(header + kOffManifestCount, static_cast<u32>(keys.size()));
    putU64(header + kOffBlobBytes, blob.size());
    u8* payload = bytes.data() + kHeaderBytes;
    for (usize i = 0; i < keys.size(); ++i) {
        putU64(payload + i * 8u, keys[i]);
    }
    if (!blob.empty()) {
        std::memcpy(payload + keys.size() * 8u, blob.data(), blob.size());
    }
    putU64(header + kOffPayloadHash, fnv1a(payload, bytes.size() - kHeaderBytes));
    putU64(header + kOffKeyHash, key.hash());
    putU64(header + kOffHeaderHash, fnv1a(header, kOffHeaderHash));
    return writeFileAtomic(path, bytes, error);
}

PipelineCacheLoadResult PipelineCache::loadFromDirectory(const char* directory, u64 contentHash) {
    if (directory == nullptr) {
        PipelineCacheLoadResult result;
        result.message = "null directory";
        return result;
    }
    const PipelineCacheKey key = makeKey(contentHash);
    const std::string path = (std::filesystem::path(directory) / fileNameV2(key)).string();
    return loadFileV2(path.c_str(), key);
}

bool PipelineCache::saveToDirectory(const char* directory, u64 contentHash, std::string* error) const {
    if (directory == nullptr) {
        if (error != nullptr) {
            *error = "null directory";
        }
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const PipelineCacheKey key = makeKey(contentHash);
    const std::string path = (std::filesystem::path(directory) / fileNameV2(key)).string();
    return saveFileV2(path.c_str(), key, error);
}

bool PipelineCache::isWarm(u64 pipelineKey) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_restored.count(pipelineKey) != 0u;
}

void PipelineCache::notePipelineCreated(u64 pipelineKey, bool feedbackValid, bool driverCacheHit) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.lookups;
    if (m_restored.count(pipelineKey) != 0u) {
        ++m_stats.warmHits;
    } else {
        ++m_stats.misses;
    }
    if (feedbackValid) {
        ++m_stats.feedbackReports;
        if (driverCacheHit) {
            ++m_stats.driverHits;
        }
    }
    m_manifest.insert(pipelineKey);
    m_created.insert(pipelineKey);
}

void PipelineCache::noteCompileRequired() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.compileRequired;
}

u32 PipelineCache::manifestSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_manifest.size());
}

std::vector<u64> PipelineCache::manifestKeys() const {
    std::vector<u64> keys;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        keys.assign(m_manifest.begin(), m_manifest.end());
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

PipelineCacheStats PipelineCache::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void PipelineCache::resetStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats = {};
}

void PipelineCache::setPrecompileHook(PrecompileHook hook) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_precompileHook = std::move(hook);
}

u32 PipelineCache::precompileWarmSet(jobs::JobScheduler* scheduler) {
    std::vector<u64> keys;
    PrecompileHook hook;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_precompileHook) {
            return 0;
        }
        hook = m_precompileHook;
        for (u64 key : m_restored) {
            if (m_created.count(key) == 0u) {
                keys.push_back(key);
            }
        }
        std::sort(keys.begin(), keys.end());
        m_precompilePending += static_cast<u32>(keys.size());
        m_stats.precompileScheduled += static_cast<u32>(keys.size());
    }
    const bool async = scheduler != nullptr && scheduler->isInitialized() && !scheduler->isSingleThreaded();
    for (u64 key : keys) {
        auto job = [this, hook, key]() {
            const bool ok = hook(*this, key);
            // Notify under the lock: the waiter may destroy the cache as soon as it can reacquire it.
            std::lock_guard<std::mutex> lock(m_mutex);
            ++(ok ? m_stats.precompileSucceeded : m_stats.precompileFailed);
            --m_precompilePending;
            m_precompileDone.notify_all();
        };
        if (async) {
            scheduler->submit(std::move(job), jobs::JobPriority::Low);
        } else {
            job();
        }
    }
    return static_cast<u32>(keys.size());
}

void PipelineCache::waitForPrecompile() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_precompileDone.wait(lock, [this]() { return m_precompilePending == 0u; });
}

} // namespace fuse::renderer
