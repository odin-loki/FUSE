#include <fuse/renderer/vk/gpu_alloc_stats.hpp>

#include <fuse/renderer/resources.hpp>

namespace fuse::renderer {

namespace {

GpuStatsHookFn g_statsHook = nullptr;
void* g_statsHookUserData = nullptr;

usize bytesPerPixel(GpuFormat format) {
    switch (format) {
    case GpuFormat::R8G8B8A8Unorm:
    case GpuFormat::R8G8B8A8Srgb:
        return 4;
    case GpuFormat::R16G16Sfloat:
        return 4;
    case GpuFormat::R16G16B16A16Sfloat:
        return 8;
    case GpuFormat::D32Sfloat:
    case GpuFormat::R32Sfloat:
        return 4;
    case GpuFormat::Undefined:
    default:
        return 4;
    }
}

} // namespace

void setGlobalGpuStatsHook(GpuStatsHookFn hook, void* userData) {
    g_statsHook = hook;
    g_statsHookUserData = userData;
}

void clearGlobalGpuStatsHook() {
    g_statsHook = nullptr;
    g_statsHookUserData = nullptr;
}

void notifyGpuStats(const char* allocatorName, const GpuAllocStats& stats) {
    if (g_statsHook != nullptr) {
        g_statsHook(allocatorName, stats, g_statsHookUserData);
    }
}

namespace gpu_alloc_detail {

void recordBufferAlloc(GpuAllocStats& stats, usize bytes) {
    stats.bufferBytes += bytes;
    stats.usedBytes += bytes;
    stats.bufferCount += 1;
    stats.allocCount += 1;
    updatePeak(stats);
}

void recordImageAlloc(GpuAllocStats& stats, usize bytes) {
    stats.imageBytes += bytes;
    stats.usedBytes += bytes;
    stats.imageCount += 1;
    stats.allocCount += 1;
    updatePeak(stats);
}

void recordBufferFree(GpuAllocStats& stats, usize bytes) {
    if (bytes <= stats.bufferBytes) {
        stats.bufferBytes -= bytes;
    } else {
        stats.bufferBytes = 0;
    }
    if (bytes <= stats.usedBytes) {
        stats.usedBytes -= bytes;
    } else {
        stats.usedBytes = 0;
    }
    if (stats.bufferCount > 0) {
        stats.bufferCount -= 1;
    }
    stats.freeCount += 1;
}

void recordImageFree(GpuAllocStats& stats, usize bytes) {
    if (bytes <= stats.imageBytes) {
        stats.imageBytes -= bytes;
    } else {
        stats.imageBytes = 0;
    }
    if (bytes <= stats.usedBytes) {
        stats.usedBytes -= bytes;
    } else {
        stats.usedBytes = 0;
    }
    if (stats.imageCount > 0) {
        stats.imageCount -= 1;
    }
    stats.freeCount += 1;
}

void recordFailedAlloc(GpuAllocStats& stats) {
    stats.failedAllocs += 1;
}

void updatePeak(GpuAllocStats& stats) {
    if (stats.usedBytes > stats.peakUsedBytes) {
        stats.peakUsedBytes = stats.usedBytes;
    }
}

usize estimateImageBytes(const TextureDesc& desc) {
    const usize pixelBytes = bytesPerPixel(desc.format);
    const usize width = desc.width > 0 ? desc.width : 1u;
    const usize height = desc.height > 0 ? desc.height : 1u;
    const usize depth = desc.depth > 0 ? desc.depth : 1u;
    const usize mips = desc.mipLevels > 0 ? desc.mipLevels : 1u;
    const usize layers = desc.arrayLayers > 0 ? desc.arrayLayers : 1u;
    return width * height * depth * pixelBytes * mips * layers;
}

} // namespace gpu_alloc_detail

} // namespace fuse::renderer
