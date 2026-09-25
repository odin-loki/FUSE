// WP-0.6 GPU profiler: timestamp-query zones per render-graph pass (+ TracyVk in FUSE_TRACY builds).
// See include/fuse/renderer/vk/gpu_profiler.hpp for the frame/zone model.
#include <fuse/renderer/vk/gpu_profiler.hpp>

#include <fuse/profiler/profiler.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#if FUSE_PROFILER_TRACY_ACTIVE
#define FUSE_GPU_PROFILER_TRACY 1
// Resolve every Vulkan entry point through vkGet*ProcAddr (works with and without volk, WP-0.2).
#define TRACY_VK_USE_SYMBOL_TABLE
// Tracy's inline code trips GCC flow warnings (-Wmaybe-uninitialized in VkCtx::Collect) that
// SYSTEM include paths do not silence once inlined into this TU.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <tracy/TracyVulkan.hpp>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
#endif

#if !defined(FUSE_GPU_PROFILER_TRACY)
#define FUSE_GPU_PROFILER_TRACY 0
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kMaxDepth = 16;

struct ZoneRecord {
    char name[GpuZoneTiming::kNameCapacity] = {};
    rg::QueueClass queue = rg::QueueClass::Graphics;
    u32 depth = 0;
    bool ended = false;
};

struct Slot {
    u64 frame = 0;
    bool pending = false;
    u32 count = 0;
    u32 skipped = 0;
    std::vector<ZoneRecord> zones;
};

struct OpenZone {
    u32 zone = GpuProfiler::kInvalidZone;
    bool tracyScope = false;
};

#if defined(FUSE_VULKAN_BACKEND)
void copyName(char (&dst)[GpuZoneTiming::kNameCapacity], const char* src) {
    if (src == nullptr) {
        src = "(unnamed)";
    }
    std::strncpy(dst, src, GpuZoneTiming::kNameCapacity - 1u);
    dst[GpuZoneTiming::kNameCapacity - 1u] = '\0';
}
#endif

} // namespace

struct GpuProfiler::Impl {
    GpuProfilerDesc desc{};
    std::vector<Slot> slots;
    u32 slot = 0;
    u64 frame = 0;
    bool inFrame = false;
    OpenZone open[kMaxDepth];
    u32 depth = 0;
    bool supported[rg::kQueueClassCount] = {false, false, false};
    u64 mask[rg::kQueueClassCount] = {0, 0, 0};
    double periodNs = 1.0;
    std::vector<u64> results; // (value, availability) pairs of one slot

#if defined(FUSE_VULKAN_BACKEND)
    VkDevice device = VK_NULL_HANDLE;
    VkQueryPool pool = VK_NULL_HANDLE;
    PFN_vkCmdWriteTimestamp cmdWriteTimestamp = nullptr;
    PFN_vkCmdResetQueryPool cmdResetQueryPool = nullptr;
    PFN_vkGetQueryPoolResults getQueryPoolResults = nullptr;
    PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
#if FUSE_GPU_PROFILER_TRACY
    tracy::VkCtx* tracyCtx[rg::kQueueClassCount] = {nullptr, nullptr, nullptr};
    bool tracyOwner[rg::kQueueClassCount] = {false, false, false}; ///< distinct queues own a context
    bool tracyCollected[rg::kQueueClassCount] = {false, false, false};
    alignas(tracy::VkCtxScope) unsigned char scopes[kMaxDepth][sizeof(tracy::VkCtxScope)];
#endif
#endif

    u32 queryBase(u32 slotIndex) const { return slotIndex * desc.maxZonesPerFrame * 2u; }
    u32 queryCount() const { return static_cast<u32>(slots.size()) * desc.maxZonesPerFrame * 2u; }
};

std::unique_ptr<GpuProfiler> GpuProfiler::create(VulkanDevice& device, const GpuProfilerDesc& desc) {
    std::unique_ptr<GpuProfiler> profiler(new GpuProfiler());
    profiler->m_impl = std::make_unique<Impl>();
    profiler->m_valid = profiler->initialize(device, desc);
    if (!profiler->m_valid) {
        profiler->shutdown();
    }
    return profiler;
}

GpuProfiler::~GpuProfiler() {
    shutdown();
}

bool GpuProfiler::timestampsSupported(rg::QueueClass queue) const {
    return m_valid && m_impl->supported[static_cast<u32>(queue)];
}

u32 GpuProfiler::tracyContextCount() const {
#if FUSE_GPU_PROFILER_TRACY
    u32 count = 0;
    for (u32 q = 0; q < rg::kQueueClassCount; ++q) {
        count += m_impl->tracyOwner[q] ? 1u : 0u;
    }
    return count;
#else
    return 0;
#endif
}

void* GpuProfiler::queryPool() const {
#if defined(FUSE_VULKAN_BACKEND)
    return m_impl != nullptr ? reinterpret_cast<void*>(m_impl->pool) : nullptr;
#else
    return nullptr;
#endif
}

#if defined(FUSE_VULKAN_BACKEND)

namespace {

/// Records `record` into a one-time command buffer on `family`/`queue`, submits and waits idle.
/// Only used at creation (initial query reset, TracyVk calibration).
template <typename Fn>
bool withOneTimeCommandBuffer(VkDevice device, u32 family, VkQueue queue, Fn&& record) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(device, &allocInfo, &cmd) == VK_SUCCESS;
    if (ok) {
        ok = record(cmd, queue);
    }
    vkDestroyCommandPool(device, pool, nullptr);
    return ok;
}

} // namespace

bool GpuProfiler::initialize(VulkanDevice& device, const GpuProfilerDesc& desc) {
    Impl& impl = *m_impl;
    impl.desc = desc;
    impl.desc.framesInFlight = std::max(desc.framesInFlight, 1u);
    impl.desc.maxZonesPerFrame = std::max(desc.maxZonesPerFrame, 1u);
    if (impl.desc.name == nullptr) {
        impl.desc.name = "fuse.gpu";
    }
    if (!device.isValid() || device.nativeHandle() == nullptr) {
        m_message = "GpuProfiler: no Vulkan device";
        return false;
    }
    impl.device = static_cast<VkDevice>(device.nativeHandle());
    const VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    if (props.limits.timestampPeriod <= 0.f) {
        m_message = "GpuProfiler: device reports timestampPeriod 0 (no timestamp queries)";
        return false;
    }
    impl.periodNs = static_cast<double>(props.limits.timestampPeriod);

    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    const VulkanQueues& queues = device.queues();
    const u32 familyOf[rg::kQueueClassCount] = {queues.graphicsFamily, queues.computeFamily, queues.transferFamily};
    const VkQueue queueOf[rg::kQueueClassCount] = {static_cast<VkQueue>(queues.graphics),
                                                   static_cast<VkQueue>(queues.compute),
                                                   static_cast<VkQueue>(queues.transfer)};
    for (u32 q = 0; q < rg::kQueueClassCount; ++q) {
        const u32 family = familyOf[q];
        const u32 bits = family < familyCount && queueOf[q] != VK_NULL_HANDLE ? families[family].timestampValidBits : 0u;
        impl.supported[q] = bits > 0u;
        impl.mask[q] = bits >= 64u ? ~u64{0} : ((u64{1} << bits) - 1u);
    }
    if (!impl.supported[0]) {
        m_message = "GpuProfiler: the graphics queue family has no timestamp support";
        return false;
    }

    impl.cmdWriteTimestamp =
        reinterpret_cast<PFN_vkCmdWriteTimestamp>(vkGetDeviceProcAddr(impl.device, "vkCmdWriteTimestamp"));
    impl.cmdResetQueryPool =
        reinterpret_cast<PFN_vkCmdResetQueryPool>(vkGetDeviceProcAddr(impl.device, "vkCmdResetQueryPool"));
    impl.getQueryPoolResults =
        reinterpret_cast<PFN_vkGetQueryPoolResults>(vkGetDeviceProcAddr(impl.device, "vkGetQueryPoolResults"));
    impl.destroyQueryPool =
        reinterpret_cast<PFN_vkDestroyQueryPool>(vkGetDeviceProcAddr(impl.device, "vkDestroyQueryPool"));
    if (impl.cmdWriteTimestamp == nullptr || impl.cmdResetQueryPool == nullptr || impl.getQueryPoolResults == nullptr ||
        impl.destroyQueryPool == nullptr) {
        m_message = "GpuProfiler: query entry points missing";
        return false;
    }

    impl.slots.resize(impl.desc.framesInFlight);
    for (Slot& slot : impl.slots) {
        slot.zones.resize(impl.desc.maxZonesPerFrame);
    }
    impl.results.resize(static_cast<size_t>(impl.desc.maxZonesPerFrame) * 4u);
    m_latest.zones.reserve(impl.desc.maxZonesPerFrame);

    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = impl.queryCount();
    if (vkCreateQueryPool(impl.device, &poolInfo, nullptr, &impl.pool) != VK_SUCCESS) {
        impl.pool = VK_NULL_HANDLE;
        m_message = "GpuProfiler: vkCreateQueryPool failed";
        return false;
    }
    std::string poolName = std::string(impl.desc.name) + ".timestamps";
    nameVkObject(impl.device, vk_object_type::kQueryPool, impl.pool, poolName.c_str());

    // Every query starts reset, so host reads never touch an uninitialized query.
    const VkQueryPool pool = impl.pool;
    const u32 count = impl.queryCount();
    const bool reset = withOneTimeCommandBuffer(
        impl.device, familyOf[0], queueOf[0], [&](VkCommandBuffer cmd, VkQueue queue) {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            vkCmdResetQueryPool(cmd, pool, 0, count);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            return vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
        });
    if (!reset) {
        m_message = "GpuProfiler: initial query reset failed";
        return false;
    }

#if FUSE_GPU_PROFILER_TRACY
    if (impl.desc.enableTracy) {
        const VkInstance instance = static_cast<VkInstance>(device.instanceHandle());
        for (u32 q = 0; q < rg::kQueueClassCount; ++q) {
            if (!impl.supported[q]) {
                continue;
            }
            // Queue classes sharing a VkQueue share its context.
            for (u32 prev = 0; prev < q; ++prev) {
                if (impl.tracyCtx[prev] != nullptr && queueOf[prev] == queueOf[q]) {
                    impl.tracyCtx[q] = impl.tracyCtx[prev];
                }
            }
            if (impl.tracyCtx[q] != nullptr) {
                continue;
            }
            withOneTimeCommandBuffer(impl.device, familyOf[q], queueOf[q], [&](VkCommandBuffer cmd, VkQueue queue) {
                // calibrated = true only picks up vkGetCalibratedTimestampsEXT when the device
                // enabled it (null otherwise, then TracyVk uses the device time domain).
                impl.tracyCtx[q] = tracy::CreateVkContext(instance, physical, impl.device, queue, cmd,
                                                          vkGetInstanceProcAddr, vkGetDeviceProcAddr, true);
                return impl.tracyCtx[q] != nullptr;
            });
            if (impl.tracyCtx[q] != nullptr) {
                impl.tracyOwner[q] = true;
                static const char* const kQueueNames[rg::kQueueClassCount] = {"graphics", "compute", "transfer"};
                const std::string ctxName = std::string(impl.desc.name) + "." + kQueueNames[q];
                impl.tracyCtx[q]->Name(ctxName.c_str(), static_cast<uint16_t>(ctxName.size()));
            }
        }
    }
#endif
    return true;
}

void GpuProfiler::shutdown() {
    if (m_impl == nullptr) {
        return;
    }
    Impl& impl = *m_impl;
#if FUSE_GPU_PROFILER_TRACY
    for (u32 q = 0; q < rg::kQueueClassCount; ++q) {
        if (impl.tracyOwner[q] && impl.tracyCtx[q] != nullptr) {
            tracy::DestroyVkContext(impl.tracyCtx[q]);
        }
        impl.tracyCtx[q] = nullptr;
        impl.tracyOwner[q] = false;
    }
#endif
    if (impl.pool != VK_NULL_HANDLE && impl.destroyQueryPool != nullptr) {
        impl.destroyQueryPool(impl.device, impl.pool, nullptr);
    }
    impl.pool = VK_NULL_HANDLE;
    m_valid = false;
}

void GpuProfiler::beginFrame() {
    if (!m_valid) {
        return;
    }
    Impl& impl = *m_impl;
    if (impl.inFrame) {
        endFrame();
    }
    ++impl.frame;
    impl.slot = static_cast<u32>(impl.frame % impl.slots.size());
    Slot& slot = impl.slots[impl.slot];
    if (slot.pending && !resolveSlot(impl.slot, true)) {
        ++m_stats.framesDropped; // still in flight: never block the frame loop on it
    }
    slot.pending = false;
    slot.frame = impl.frame;
    slot.count = 0;
    slot.skipped = 0;
    impl.depth = 0;
    impl.inFrame = true;
#if FUSE_GPU_PROFILER_TRACY
    std::fill(std::begin(impl.tracyCollected), std::end(impl.tracyCollected), false);
#endif
    ++m_stats.framesBegun;
}

void GpuProfiler::endFrame() {
    if (!m_valid || !m_impl->inFrame) {
        return;
    }
    Impl& impl = *m_impl;
    Slot& slot = impl.slots[impl.slot];
    // Zones left open cannot resolve (their end query was never written): drop them.
    while (impl.depth > 0u) {
        const OpenZone& top = impl.open[--impl.depth];
        if (top.zone != kInvalidZone) {
            slot.zones[top.zone].ended = false;
        }
        ++slot.skipped;
    }
    slot.pending = true;
    impl.inFrame = false;
    FUSE_PROFILE_FRAME_MARK();
}

u32 GpuProfiler::beginZone(void* commandBuffer, const char* name, rg::QueueClass queue) {
    if (!m_valid || commandBuffer == nullptr) {
        return kInvalidZone;
    }
    Impl& impl = *m_impl;
    Slot& slot = impl.slots[impl.slot];
    const u32 q = static_cast<u32>(queue) < rg::kQueueClassCount ? static_cast<u32>(queue) : 0u;
    if (!impl.inFrame || impl.depth >= kMaxDepth) {
        ++m_stats.zonesSkipped;
        if (impl.inFrame) {
            ++slot.skipped;
        }
        return kInvalidZone;
    }
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    OpenZone& open = impl.open[impl.depth++];
    open.zone = kInvalidZone;
    open.tracyScope = false;

    if (impl.supported[q] && slot.count < impl.desc.maxZonesPerFrame) {
        const u32 zone = slot.count++;
        ZoneRecord& record = slot.zones[zone];
        copyName(record.name, name);
        record.queue = static_cast<rg::QueueClass>(q);
        record.depth = impl.depth - 1u;
        record.ended = false;
        const u32 query = impl.queryBase(impl.slot) + zone * 2u;
        impl.cmdResetQueryPool(cmd, impl.pool, query, 2u);
        impl.cmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl.pool, query);
        open.zone = zone;
    } else {
        ++slot.skipped;
        ++m_stats.zonesSkipped;
    }

#if FUSE_GPU_PROFILER_TRACY
    if (impl.tracyCtx[q] != nullptr) {
        tracy::VkCtx* ctx = impl.tracyCtx[q];
        // Queue classes sharing a context collect once per frame.
        bool collected = false;
        for (u32 other = 0; other < rg::kQueueClassCount; ++other) {
            collected = collected || (impl.tracyCtx[other] == ctx && impl.tracyCollected[other]);
        }
        if (!collected) {
            ctx->Collect(cmd);
            impl.tracyCollected[q] = true;
            ++m_stats.tracyCollects;
        }
        const char* zoneName = name != nullptr ? name : "(unnamed)";
        static constexpr char kFunction[] = "fuse::renderer::GpuProfiler";
        new (impl.scopes[impl.depth - 1u]) tracy::VkCtxScope(ctx, __LINE__, __FILE__, sizeof(__FILE__) - 1u, kFunction,
                                                              sizeof(kFunction) - 1u, zoneName, std::strlen(zoneName),
                                                              cmd, true);
        open.tracyScope = true;
    }
#endif
    return open.zone;
}

void GpuProfiler::endZone(void* commandBuffer, u32 zone) {
    if (!m_valid || !m_impl->inFrame || commandBuffer == nullptr) {
        return;
    }
    Impl& impl = *m_impl;
    if (impl.depth == 0u) {
        ++m_stats.zonesSkipped;
        return;
    }
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    const u32 index = --impl.depth;
    OpenZone& open = impl.open[index];
#if FUSE_GPU_PROFILER_TRACY
    if (open.tracyScope) {
        std::launder(reinterpret_cast<tracy::VkCtxScope*>(impl.scopes[index]))->~VkCtxScope();
        open.tracyScope = false;
    }
#endif
    Slot& slot = impl.slots[impl.slot];
    if (open.zone == kInvalidZone) {
        return;
    }
    if (open.zone != zone) {
        // Unbalanced end: close the innermost zone anyway so the stack stays consistent.
        ++m_stats.zonesSkipped;
    }
    ZoneRecord& record = slot.zones[open.zone];
    impl.cmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl.pool,
                           impl.queryBase(impl.slot) + open.zone * 2u + 1u);
    record.ended = true;
    ++m_stats.zonesRecorded;
    open.zone = kInvalidZone;
}

bool GpuProfiler::resolveSlot(u32 slotIndex, bool publish) {
    Impl& impl = *m_impl;
    Slot& slot = impl.slots[slotIndex];
    if (!slot.pending) {
        return false;
    }
    if (slot.count > 0u) {
        const u32 queries = slot.count * 2u;
        const VkResult result = impl.getQueryPoolResults(
            impl.device, impl.pool, impl.queryBase(slotIndex), queries, sizeof(u64) * 2u * queries, impl.results.data(),
            sizeof(u64) * 2u, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            return false;
        }
        for (u32 zone = 0; zone < slot.count; ++zone) {
            if (!slot.zones[zone].ended) {
                continue; // end query never written: excluded below
            }
            if (impl.results[zone * 4u + 1u] == 0u || impl.results[zone * 4u + 3u] == 0u) {
                return false;
            }
        }
    }
    if (publish) {
        m_latest.frame = slot.frame;
        m_latest.valid = true;
        m_latest.skippedZones = slot.skipped;
        m_latest.zones.clear();
        for (u32 zone = 0; zone < slot.count; ++zone) {
            const ZoneRecord& record = slot.zones[zone];
            if (!record.ended) {
                ++m_latest.skippedZones;
                continue;
            }
            const u64 mask = impl.mask[static_cast<u32>(record.queue)];
            GpuZoneTiming timing;
            std::memcpy(timing.name, record.name, sizeof(timing.name));
            timing.queue = record.queue;
            timing.depth = record.depth;
            timing.beginNs = static_cast<u64>(static_cast<double>(impl.results[zone * 4u] & mask) * impl.periodNs);
            timing.endNs = static_cast<u64>(static_cast<double>(impl.results[zone * 4u + 2u] & mask) * impl.periodNs);
            m_latest.zones.push_back(timing);
        }
        ++m_stats.framesResolved;
    }
    slot.pending = false;
    return true;
}

u32 GpuProfiler::resolve() {
    if (!m_valid) {
        return 0;
    }
    Impl& impl = *m_impl;
    u32 resolved = 0;
    for (;;) {
        u32 oldest = UINT32_MAX;
        for (u32 i = 0; i < impl.slots.size(); ++i) {
            const bool open = impl.inFrame && i == impl.slot;
            if (impl.slots[i].pending && !open &&
                (oldest == UINT32_MAX || impl.slots[i].frame < impl.slots[oldest].frame)) {
                oldest = i;
            }
        }
        if (oldest == UINT32_MAX || !resolveSlot(oldest, true)) {
            return resolved;
        }
        ++resolved;
    }
}

#else // stub backend

bool GpuProfiler::initialize(VulkanDevice&, const GpuProfilerDesc& desc) {
    m_impl->desc = desc;
    m_message = "GpuProfiler: stub backend (no Vulkan)";
    return false;
}

void GpuProfiler::shutdown() {
    m_valid = false;
}

void GpuProfiler::beginFrame() {}

void GpuProfiler::endFrame() {
    if (m_valid) {
        FUSE_PROFILE_FRAME_MARK();
    }
}

u32 GpuProfiler::beginZone(void*, const char*, rg::QueueClass) {
    return kInvalidZone;
}

void GpuProfiler::endZone(void*, u32) {}

bool GpuProfiler::resolveSlot(u32, bool) {
    return false;
}

u32 GpuProfiler::resolve() {
    return 0;
}

#endif

rg::PassHooks GpuProfiler::passHooks() {
    rg::PassHooks hooks;
    hooks.user = this;
    hooks.begin = [](const rg::PassContext& context, const char* name, void* user) {
        static_cast<GpuProfiler*>(user)->beginZone(context.commandBuffer, name, context.queue);
    };
    hooks.end = [](const rg::PassContext& context, const char*, void* user) {
        GpuProfiler* self = static_cast<GpuProfiler*>(user);
        u32 zone = kInvalidZone;
#if defined(FUSE_VULKAN_BACKEND)
        if (self->m_valid && self->m_impl->depth > 0u) {
            zone = self->m_impl->open[self->m_impl->depth - 1u].zone;
        }
#endif
        self->endZone(context.commandBuffer, zone);
    };
    return hooks;
}

} // namespace fuse::renderer
