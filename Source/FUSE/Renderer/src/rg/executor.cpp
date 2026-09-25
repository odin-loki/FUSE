#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/sync_model.hpp>

#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#if defined(FUSE_VMA_AVAILABLE)
// Same configuration as src/vk/allocator.cpp (which holds VMA_IMPLEMENTATION); declarations only.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1002000
#include <vk_mem_alloc.h>
#endif
#endif

namespace fuse::renderer::rg {

#if defined(FUSE_VULKAN_BACKEND)

namespace {

bool hasExtension(const std::vector<const char*>& extensions, const char* name) {
    for (const char* extension : extensions) {
        if (extension != nullptr && std::strcmp(extension, name) == 0) {
            return true;
        }
    }
    return false;
}

VkImageViewType viewTypeFor(u32 depth, u32 layers) {
    if (depth > 1u) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    return layers > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

/// Physical transient: one VkImage/VkBuffer plus the memory block (slot) it is bound into.
struct Physical {
    bool image = true;
    // signature
    u32 format = 0, width = 0, height = 0, depth = 0, mips = 0, layers = 0, usage = 0;
    u64 size = 0;
    u32 first = 0, last = 0;
    // objects
    VkImage vkImage = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer vkBuffer = VK_NULL_HANDLE;
    VkMemoryRequirements req{};
    u32 slot = UINT32_MAX;
    u32 prevOccupant = UINT32_MAX; ///< physical index
};

struct Block {
    u64 size = 0;
    u64 alignment = 1;
    u32 typeBits = 0;
    u32 last = 0;               ///< last execution position of the newest occupant
    u32 lastOccupant = UINT32_MAX;
    u32 members = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE; ///< native path / resolved from VMA
    u64 offset = 0;
#if defined(FUSE_VMA_AVAILABLE)
    VmaAllocation allocation = VK_NULL_HANDLE;
#endif
};

struct FrameSlot {
    VkCommandPool pool[kQueueClassCount] = {};
    std::vector<VkCommandBuffer> commandBuffers[kQueueClassCount];
    u32 used[kQueueClassCount] = {};
    u64 retire[kQueueClassCount] = {};
    /// Signalled by the slot's last submission on each queue (host retirement that every
    /// validation layer version understands; timelines cover the GPU-side ordering).
    VkFence fence[kQueueClassCount] = {};
    bool fenceSubmitted[kQueueClassCount] = {};
    /// Binary semaphores for this slot's intra-frame cross-queue edges (reused once retired).
    std::vector<VkSemaphore> binary;
    u32 binaryUsed = 0;
};

} // namespace

struct Executor::Impl {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue[kQueueClassCount] = {};
    u32 family[kQueueClassCount] = {};
    bool available[kQueueClassCount] = {true, false, false};
    VkSemaphore timeline[kQueueClassCount] = {};
    u64 value[kQueueClassCount] = {};
    u64 frameEnd[kQueueClassCount] = {}; ///< last value signalled by the previous frame
    std::vector<FrameSlot> slots;
    u32 slotIndex = 0;

    PFN_vkCmdPipelineBarrier2 barrier2 = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
#if defined(FUSE_VMA_AVAILABLE)
    VmaAllocator vma = VK_NULL_HANDLE;
#endif

    // Transient heap
    std::vector<Physical> physicals;
    std::vector<Block> blocks;
    std::vector<u32> resourceToPhysical; ///< graph resource index -> physical (UINT32_MAX = none)
    std::vector<u32> sortScratch;
    bool aliasing = true;

    // Recording scratch (cleared, never shrunk)
    std::vector<VkImageMemoryBarrier2> images2;
    std::vector<VkBufferMemoryBarrier2> buffers2;
    std::vector<VkImageMemoryBarrier> images1;
    std::vector<VkBufferMemoryBarrier> buffers1;
    std::vector<VkCommandBuffer> batchCommandBuffers;
    std::vector<u64> batchValues;
    std::vector<VkSemaphore> edgeSemaphores; ///< [consumer batch * 3 + producer queue]
    std::vector<VkSemaphore> signalScratch;
    std::vector<u64> signalValueScratch;
};

std::unique_ptr<Executor> Executor::create(VulkanDevice& device, GpuAllocator* allocator, const ExecutorDesc& desc) {
    auto executor = std::unique_ptr<Executor>(new Executor());
    if (!executor->initialize(device, allocator, desc)) {
        executor->shutdown();
        executor->m_valid = false;
    }
    return executor;
}

Executor::~Executor() {
    shutdown();
}

bool Executor::initialize(VulkanDevice& device, GpuAllocator* allocator, const ExecutorDesc& desc) {
    m_device = &device;
    m_desc = desc;
    m_desc.framesInFlight = std::max(desc.framesInFlight, 1u);
    m_impl = std::make_unique<Impl>();
    if (!device.isValid() || device.nativeHandle() == nullptr) {
        m_message = "Vulkan device unavailable";
        return false;
    }
    Impl& impl = *m_impl;
    impl.device = static_cast<VkDevice>(device.nativeHandle());
    impl.physical = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    impl.aliasing = desc.enableAliasing;
    vkGetPhysicalDeviceMemoryProperties(impl.physical, &impl.memoryProperties);

    const VulkanQueues& queues = device.queues();
    impl.queue[0] = static_cast<VkQueue>(queues.graphics);
    impl.family[0] = queues.graphicsFamily;
    impl.queue[1] = static_cast<VkQueue>(queues.compute);
    impl.family[1] = queues.computeFamily;
    impl.queue[2] = static_cast<VkQueue>(queues.transfer);
    impl.family[2] = queues.transferFamily;
    impl.available[0] = impl.queue[0] != VK_NULL_HANDLE;
    impl.available[1] = desc.enableAsyncCompute && impl.queue[1] != VK_NULL_HANDLE && impl.queue[1] != impl.queue[0] &&
                        impl.family[1] != impl.family[0];
    impl.available[2] = desc.enableTransferQueue && impl.queue[2] != VK_NULL_HANDLE &&
                        impl.queue[2] != impl.queue[0] && impl.queue[2] != impl.queue[1] &&
                        impl.family[2] != impl.family[0] && impl.family[2] != impl.family[1];
    if (!impl.available[0]) {
        m_message = "no graphics queue";
        return false;
    }
    if (!device.info().timelineSemaphore) {
        m_message = "timeline semaphores not enabled";
        return false;
    }

    // synchronization2 only when the logical device enabled it (WP-0.1 RendererCaps; core 1.3 or
    // VK_KHR_synchronization2). Otherwise (FUSE_VK_ALLOW_1_2 devices) barriers go through
    // synchronization1 with the same masks.
    const VulkanDeviceInfo& info = device.info();
    const bool sync2Enabled = (info.caps.valid && info.caps.synchronization2) ||
                              hasExtension(info.enabledExtensions, "VK_KHR_synchronization2");
    if (desc.allowSync2 && sync2Enabled) {
        if (info.apiVersion >= VK_API_VERSION_1_3) {
            impl.barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
                vkGetDeviceProcAddr(impl.device, "vkCmdPipelineBarrier2"));
        }
        if (impl.barrier2 == nullptr && hasExtension(info.enabledExtensions, "VK_KHR_synchronization2")) {
            impl.barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
                vkGetDeviceProcAddr(impl.device, "vkCmdPipelineBarrier2KHR"));
        }
    }
    m_sync2 = impl.barrier2 != nullptr;
    impl.beginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(impl.device, "vkCmdBeginDebugUtilsLabelEXT"));
    impl.endLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(impl.device, "vkCmdEndDebugUtilsLabelEXT"));
    m_labels = impl.beginLabel != nullptr && impl.endLabel != nullptr;

#if defined(FUSE_VMA_AVAILABLE)
    if (allocator != nullptr && allocator->isValid() && allocator->info().mode == GpuAllocatorMode::Vma) {
        impl.vma = static_cast<VmaAllocator>(allocator->nativeHandle());
    } else if (info.vmaAllocator != nullptr) {
        impl.vma = static_cast<VmaAllocator>(info.vmaAllocator);
    }
#else
    (void)allocator;
#endif

    const char* name = desc.name != nullptr ? desc.name : "fuse.rg";
    for (u32 q = 0; q < kQueueClassCount; ++q) {
        if (!impl.available[q]) {
            continue;
        }
        VkSemaphoreTypeCreateInfo typeInfo{};
        typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = 0;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphoreInfo.pNext = &typeInfo;
        if (vkCreateSemaphore(impl.device, &semaphoreInfo, nullptr, &impl.timeline[q]) != VK_SUCCESS) {
            m_message = "vkCreateSemaphore (timeline) failed";
            return false;
        }
        nameVkObject(impl.device, vk_object_type::kSemaphore, impl.timeline[q], name);
    }

    impl.slots.resize(m_desc.framesInFlight);
    for (FrameSlot& slot : impl.slots) {
        for (u32 q = 0; q < kQueueClassCount; ++q) {
            if (!impl.available[q]) {
                continue;
            }
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = impl.family[q];
            if (vkCreateCommandPool(impl.device, &poolInfo, nullptr, &slot.pool[q]) != VK_SUCCESS) {
                m_message = "vkCreateCommandPool failed";
                return false;
            }
            nameVkObject(impl.device, vk_object_type::kCommandPool, slot.pool[q], name);
            slot.commandBuffers[q].reserve(8);
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(impl.device, &fenceInfo, nullptr, &slot.fence[q]) != VK_SUCCESS) {
                m_message = "vkCreateFence failed";
                return false;
            }
        }
    }
    impl.images2.reserve(64);
    impl.buffers2.reserve(64);
    impl.images1.reserve(64);
    impl.buffers1.reserve(64);
    impl.batchCommandBuffers.reserve(16);
    impl.batchValues.reserve(16);
    impl.edgeSemaphores.reserve(48);
    impl.signalScratch.reserve(16);
    impl.signalValueScratch.reserve(16);

    m_valid = true;
    m_message = m_sync2 ? "ok (synchronization2)" : "ok (synchronization1 fallback: synchronization2 not enabled)";
    return true;
}

void Executor::shutdown() {
    if (m_impl == nullptr || m_impl->device == VK_NULL_HANDLE) {
        m_impl.reset();
        return;
    }
    waitIdle();
    Impl& impl = *m_impl;
    destroyTransients();
    for (FrameSlot& slot : impl.slots) {
        for (u32 q = 0; q < kQueueClassCount; ++q) {
            if (slot.pool[q] != VK_NULL_HANDLE) {
                vkDestroyCommandPool(impl.device, slot.pool[q], nullptr);
            }
            if (slot.fence[q] != VK_NULL_HANDLE) {
                vkDestroyFence(impl.device, slot.fence[q], nullptr);
            }
        }
        for (VkSemaphore semaphore : slot.binary) {
            vkDestroySemaphore(impl.device, semaphore, nullptr);
        }
    }
    impl.slots.clear();
    for (VkSemaphore& semaphore : impl.timeline) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl.device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
        }
    }
    m_impl.reset();
}

bool Executor::queueAvailable(QueueClass queue) const {
    return m_impl != nullptr && m_impl->available[static_cast<u32>(queue)];
}

u32 Executor::queueFamily(QueueClass queue) const {
    if (m_impl == nullptr) {
        return UINT32_MAX;
    }
    const u32 q = queueAvailable(queue) ? static_cast<u32>(queue) : 0u;
    return m_impl->family[q];
}

CompileOptions Executor::compileOptions() const {
    CompileOptions options;
    options.asyncComputeAvailable = queueAvailable(QueueClass::AsyncCompute);
    options.transferAvailable = queueAvailable(QueueClass::Transfer);
    return options;
}

bool Executor::waitIdle() {
    if (m_impl == nullptr || m_impl->device == VK_NULL_HANDLE) {
        return false;
    }
    Impl& impl = *m_impl;
    for (FrameSlot& slot : impl.slots) {
        for (u32 q = 0; q < kQueueClassCount; ++q) {
            if (slot.fenceSubmitted[q]) {
                if (vkWaitForFences(impl.device, 1, &slot.fence[q], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                    return false;
                }
                vkResetFences(impl.device, 1, &slot.fence[q]);
                slot.fenceSubmitted[q] = false;
            }
        }
    }
    VkSemaphore semaphores[kQueueClassCount];
    u64 values[kQueueClassCount];
    u32 count = 0;
    for (u32 q = 0; q < kQueueClassCount; ++q) {
        if (impl.timeline[q] != VK_NULL_HANDLE && impl.value[q] > 0u) {
            semaphores[count] = impl.timeline[q];
            values[count] = impl.value[q];
            ++count;
        }
    }
    if (count == 0u) {
        return true;
    }
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = count;
    waitInfo.pSemaphores = semaphores;
    waitInfo.pValues = values;
    return vkWaitSemaphores(impl.device, &waitInfo, UINT64_MAX) == VK_SUCCESS;
}

// --- transient heap -----------------------------------------------------------------------------

void Executor::destroyTransients() {
    Impl& impl = *m_impl;
    for (Physical& physical : impl.physicals) {
        if (physical.view != VK_NULL_HANDLE) {
            vkDestroyImageView(impl.device, physical.view, nullptr);
        }
        if (physical.vkImage != VK_NULL_HANDLE) {
            vkDestroyImage(impl.device, physical.vkImage, nullptr);
        }
        if (physical.vkBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(impl.device, physical.vkBuffer, nullptr);
        }
    }
    impl.physicals.clear();
    for (Block& block : impl.blocks) {
#if defined(FUSE_VMA_AVAILABLE)
        if (block.allocation != VK_NULL_HANDLE) {
            vmaFreeMemory(impl.vma, block.allocation);
            continue;
        }
#endif
        if (block.memory != VK_NULL_HANDLE) {
            vkFreeMemory(impl.device, block.memory, nullptr);
        }
    }
    impl.blocks.clear();
    m_transientStats.transients = 0;
    m_transientStats.allocations = 0;
    m_transientStats.aliasedResources = 0;
    m_transientStats.bytesRequired = 0;
    m_transientStats.bytesAllocated = 0;
    m_transientStats.vmaAllocationDelta = 0;
}

bool Executor::realizeTransients(Graph& graph) {
    Impl& impl = *m_impl;
    impl.resourceToPhysical.assign(graph.m_resources.size(), UINT32_MAX);

    // Transients in first-use order (ties: declaration order).
    std::vector<u32>& order = impl.sortScratch;
    order.clear();
    for (u32 r = 0; r < graph.m_resources.size(); ++r) {
        const Graph::Resource& resource = graph.m_resources[r];
        if (!resource.imported && resource.life.first != UINT32_MAX) {
            order.push_back(r);
        }
    }
    // Stable insertion sort: allocation-free (std::stable_sort may take a temporary buffer) and
    // the list is nearly sorted already (declaration order usually follows first use).
    for (usize i = 1; i < order.size(); ++i) {
        const u32 value = order[i];
        const u32 key = graph.m_resources[value].life.first;
        usize j = i;
        while (j > 0u && graph.m_resources[order[j - 1u]].life.first > key) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = value;
    }

    auto usageFor = [](const Graph::Resource& resource) {
        u32 usage = resource.usage;
        if (usage == 0u) {
            usage = resource.image ? vkc::kImageUsageTransferDst : vkc::kBufferUsageTransferDst;
        }
        return usage;
    };

    // Same signature as the cached heap: reuse everything (steady state, no allocation).
    bool same = order.size() == impl.physicals.size();
    for (u32 i = 0; same && i < order.size(); ++i) {
        const Graph::Resource& r = graph.m_resources[order[i]];
        const Physical& p = impl.physicals[i];
        same = p.image == r.image && p.first == r.life.first && p.last == r.life.last && p.usage == usageFor(r) &&
               (r.image ? (p.format == r.format && p.width == r.width && p.height == r.height && p.depth == r.depth &&
                           p.mips == r.mips && p.layers == r.layers)
                        : p.size == r.size);
    }
    const bool rebuild = !same;
    if (rebuild) {
        waitIdle();
        destroyTransients();
        ++m_transientStats.rebuilds;
#if defined(FUSE_VMA_AVAILABLE)
        u32 vmaBefore = 0;
        if (impl.vma != VK_NULL_HANDLE) {
            VmaTotalStatistics total{};
            vmaCalculateStatistics(impl.vma, &total);
            vmaBefore = total.total.statistics.allocationCount;
        }
#endif
        const char* baseName = m_desc.name != nullptr ? m_desc.name : "fuse.rg";
        impl.physicals.reserve(order.size());
        for (const u32 r : order) {
            const Graph::Resource& resource = graph.m_resources[r];
            Physical physical;
            physical.image = resource.image;
            physical.first = resource.life.first;
            physical.last = resource.life.last;
            physical.usage = usageFor(resource);
            if (resource.image) {
                physical.format = resource.format;
                physical.width = resource.width;
                physical.height = resource.height;
                physical.depth = resource.depth;
                physical.mips = resource.mips;
                physical.layers = resource.layers;
                VkImageCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                info.imageType = resource.depth > 1u ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
                info.format = static_cast<VkFormat>(resource.format);
                info.extent = {resource.width, resource.height, resource.depth};
                info.mipLevels = resource.mips;
                info.arrayLayers = resource.layers;
                info.samples = VK_SAMPLE_COUNT_1_BIT;
                info.tiling = VK_IMAGE_TILING_OPTIMAL;
                info.usage = physical.usage;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (vkCreateImage(impl.device, &info, nullptr, &physical.vkImage) != VK_SUCCESS) {
                    m_message = "transient vkCreateImage failed";
                    return false;
                }
                vkGetImageMemoryRequirements(impl.device, physical.vkImage, &physical.req);
                nameVkObject(impl.device, vk_object_type::kImage, physical.vkImage,
                             resource.name != nullptr ? resource.name : baseName);
            } else {
                physical.size = resource.size;
                VkBufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                info.size = std::max<u64>(resource.size, 4u);
                info.usage = physical.usage;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                if (vkCreateBuffer(impl.device, &info, nullptr, &physical.vkBuffer) != VK_SUCCESS) {
                    m_message = "transient vkCreateBuffer failed";
                    return false;
                }
                vkGetBufferMemoryRequirements(impl.device, physical.vkBuffer, &physical.req);
                nameVkObject(impl.device, vk_object_type::kBuffer, physical.vkBuffer,
                             resource.name != nullptr ? resource.name : baseName);
            }

            // First-fit block whose occupants are all dead before this transient's first use.
            u32 slot = UINT32_MAX;
            if (impl.aliasing) {
                for (u32 b = 0; b < impl.blocks.size(); ++b) {
                    const Block& block = impl.blocks[b];
                    if (block.last < physical.first && (block.typeBits & physical.req.memoryTypeBits) != 0u) {
                        slot = b;
                        break;
                    }
                }
            }
            if (slot == UINT32_MAX) {
                Block block;
                block.typeBits = physical.req.memoryTypeBits;
                impl.blocks.push_back(block);
                slot = static_cast<u32>(impl.blocks.size() - 1u);
            } else {
                ++m_transientStats.aliasedResources;
            }
            Block& block = impl.blocks[slot];
            block.size = std::max<u64>(block.size, physical.req.size);
            block.alignment = std::max<u64>(block.alignment, physical.req.alignment);
            block.typeBits &= physical.req.memoryTypeBits;
            block.last = physical.last;
            physical.slot = slot;
            physical.prevOccupant = block.lastOccupant;
            block.lastOccupant = static_cast<u32>(impl.physicals.size());
            ++block.members;
            m_transientStats.bytesRequired += physical.req.size;
            impl.physicals.push_back(physical);
        }

        // Allocate every block once, then bind all its occupants at offset 0 (aliasing).
        for (Block& block : impl.blocks) {
            VkMemoryRequirements req{};
            req.size = block.size;
            req.alignment = block.alignment;
            req.memoryTypeBits = block.typeBits;
#if defined(FUSE_VMA_AVAILABLE)
            if (impl.vma != VK_NULL_HANDLE) {
                VmaAllocationCreateInfo create{};
                create.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                VmaAllocationInfo allocationInfo{};
                if (vmaAllocateMemory(impl.vma, &req, &create, &block.allocation, &allocationInfo) != VK_SUCCESS) {
                    m_message = "transient vmaAllocateMemory failed";
                    return false;
                }
                block.memory = allocationInfo.deviceMemory;
                block.offset = allocationInfo.offset;
                vmaSetAllocationName(impl.vma, block.allocation, "fuse.rg.transient_heap");
                m_transientStats.bytesAllocated += allocationInfo.size;
                continue;
            }
#endif
            u32 typeIndex = UINT32_MAX;
            for (u32 t = 0; t < impl.memoryProperties.memoryTypeCount; ++t) {
                if ((block.typeBits & (1u << t)) != 0u &&
                    (impl.memoryProperties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
                    typeIndex = t;
                    break;
                }
            }
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.allocationSize = block.size;
            allocate.memoryTypeIndex = typeIndex;
            if (typeIndex == UINT32_MAX ||
                vkAllocateMemory(impl.device, &allocate, nullptr, &block.memory) != VK_SUCCESS) {
                m_message = "transient vkAllocateMemory failed";
                return false;
            }
            m_transientStats.bytesAllocated += block.size;
        }
        for (Physical& physical : impl.physicals) {
            Block& block = impl.blocks[physical.slot];
            VkResult bound = VK_ERROR_UNKNOWN;
#if defined(FUSE_VMA_AVAILABLE)
            if (block.allocation != VK_NULL_HANDLE) {
                bound = physical.image ? vmaBindImageMemory(impl.vma, block.allocation, physical.vkImage)
                                       : vmaBindBufferMemory(impl.vma, block.allocation, physical.vkBuffer);
            } else
#endif
            {
                bound = physical.image ? vkBindImageMemory(impl.device, physical.vkImage, block.memory, 0)
                                       : vkBindBufferMemory(impl.device, physical.vkBuffer, block.memory, 0);
            }
            if (bound != VK_SUCCESS) {
                m_message = "transient memory bind failed";
                return false;
            }
            constexpr u32 kViewUsages = vkc::kImageUsageSampled | vkc::kImageUsageStorage |
                                        vkc::kImageUsageColorAttachment | vkc::kImageUsageDepthStencilAttachment;
            if (physical.image && (physical.usage & kViewUsages) != 0u) {
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = physical.vkImage;
                viewInfo.viewType = viewTypeFor(physical.depth, physical.layers);
                viewInfo.format = static_cast<VkFormat>(physical.format);
                viewInfo.subresourceRange.aspectMask = aspectForFormat(physical.format);
                viewInfo.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                viewInfo.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
                if (vkCreateImageView(impl.device, &viewInfo, nullptr, &physical.view) != VK_SUCCESS) {
                    m_message = "transient vkCreateImageView failed";
                    return false;
                }
            }
        }
        m_transientStats.transients = static_cast<u32>(impl.physicals.size());
        m_transientStats.allocations = static_cast<u32>(impl.blocks.size());
#if defined(FUSE_VMA_AVAILABLE)
        if (impl.vma != VK_NULL_HANDLE) {
            VmaTotalStatistics total{};
            vmaCalculateStatistics(impl.vma, &total);
            m_transientStats.vmaAllocationDelta = total.total.statistics.allocationCount - vmaBefore;
        }
#endif
    }

    // Resource table: graph refs -> physical objects (and alias predecessors for the planner).
    for (u32 i = 0; i < order.size(); ++i) {
        const u32 r = order[i];
        Graph::Resource& resource = graph.m_resources[r];
        const Physical& physical = impl.physicals[i];
        impl.resourceToPhysical[r] = i;
        resource.handle = physical.image ? static_cast<void*>(physical.vkImage) : static_cast<void*>(physical.vkBuffer);
        resource.view = physical.image ? static_cast<void*>(physical.view) : nullptr;
        resource.aliasSlot = physical.slot;
        resource.aliasPrev = physical.prevOccupant != UINT32_MAX ? order[physical.prevOccupant] : UINT32_MAX;
    }
    return true;
}

bool Executor::transientMemory(const Graph& graph, TextureRef ref, TransientMemory& out) const {
    if (m_impl == nullptr || ref.id == 0u || ref.id > m_impl->resourceToPhysical.size() ||
        !graph.m_resources[ref.id - 1u].image) {
        return false;
    }
    const u32 p = m_impl->resourceToPhysical[ref.id - 1u];
    if (p == UINT32_MAX) {
        return false;
    }
    const Physical& physical = m_impl->physicals[p];
    const Block& block = m_impl->blocks[physical.slot];
    out.deviceMemory = block.memory;
    out.offset = block.offset;
    out.size = physical.req.size;
    out.slot = physical.slot;
    return true;
}

bool Executor::transientMemory(const Graph& graph, BufferRef ref, TransientMemory& out) const {
    if (m_impl == nullptr || ref.id == 0u || ref.id > m_impl->resourceToPhysical.size() ||
        graph.m_resources[ref.id - 1u].image) {
        return false;
    }
    const u32 p = m_impl->resourceToPhysical[ref.id - 1u];
    if (p == UINT32_MAX) {
        return false;
    }
    const Physical& physical = m_impl->physicals[p];
    const Block& block = m_impl->blocks[physical.slot];
    out.deviceMemory = block.memory;
    out.offset = block.offset;
    out.size = physical.req.size;
    out.slot = physical.slot;
    return true;
}

// --- recording ----------------------------------------------------------------------------------

bool Executor::prepare(Graph& graph, u8 forceQueue) {
    if (!m_valid) {
        return false;
    }
    CompileOptions options = compileOptions();
    options.forceQueue = forceQueue;
    if (!graph.compile(options)) {
        m_message = "graph has no live passes";
        return false;
    }
    if (!realizeTransients(graph)) {
        return false;
    }
    return graph.plan();
}

void Executor::recordBarriers(const Graph& graph, const BarrierRange& range, void* commandBuffer,
                              ExecuteResult& result, bool late) {
    if (range.empty() || m_desc.debugSkipBarriers) {
        return;
    }
    const std::vector<ImageBarrier>& imageBarriers = late ? graph.m_lateImageBarriers : graph.m_imageBarriers;
    Impl& impl = *m_impl;
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    auto family = [&](u8 queue) -> u32 { return queue == kNoQueue ? VK_QUEUE_FAMILY_IGNORED : impl.family[queue]; };

    if (impl.barrier2 != nullptr) {
        impl.images2.clear();
        impl.buffers2.clear();
        for (u32 i = range.imageBegin; i < range.imageBegin + range.imageCount; ++i) {
            const ImageBarrier& b = imageBarriers[i];
            const Graph::Resource& resource = graph.m_resources[b.resource - 1u];
            if (resource.handle == nullptr) {
                continue;
            }
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = b.srcStages;
            barrier.srcAccessMask = b.srcAccess;
            barrier.dstStageMask = b.dstStages;
            barrier.dstAccessMask = b.dstAccess;
            barrier.oldLayout = static_cast<VkImageLayout>(b.oldLayout);
            barrier.newLayout = static_cast<VkImageLayout>(b.newLayout);
            barrier.srcQueueFamilyIndex = family(b.srcQueue);
            barrier.dstQueueFamilyIndex = family(b.dstQueue);
            barrier.image = static_cast<VkImage>(resource.handle);
            barrier.subresourceRange = {aspectForFormat(resource.format), b.baseMip, b.mipCount, b.baseLayer,
                                        b.layerCount};
            impl.images2.push_back(barrier);
        }
        for (u32 i = range.bufferBegin; i < range.bufferBegin + range.bufferCount; ++i) {
            const BufferBarrier& b = graph.m_bufferBarriers[i];
            const Graph::Resource& resource = graph.m_resources[b.resource - 1u];
            if (resource.handle == nullptr) {
                continue;
            }
            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = b.srcStages;
            barrier.srcAccessMask = b.srcAccess;
            barrier.dstStageMask = b.dstStages;
            barrier.dstAccessMask = b.dstAccess;
            barrier.srcQueueFamilyIndex = family(b.srcQueue);
            barrier.dstQueueFamilyIndex = family(b.dstQueue);
            barrier.buffer = static_cast<VkBuffer>(resource.handle);
            barrier.offset = b.offset;
            barrier.size = b.size == 0u ? VK_WHOLE_SIZE : b.size;
            impl.buffers2.push_back(barrier);
        }
        if (impl.images2.empty() && impl.buffers2.empty()) {
            return;
        }
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<u32>(impl.images2.size());
        dependency.pImageMemoryBarriers = impl.images2.data();
        dependency.bufferMemoryBarrierCount = static_cast<u32>(impl.buffers2.size());
        dependency.pBufferMemoryBarriers = impl.buffers2.data();
        impl.barrier2(cmd, &dependency);
        ++result.barrierCalls;
        result.imageBarriers += dependency.imageMemoryBarrierCount;
        result.bufferBarriers += dependency.bufferMemoryBarrierCount;
        return;
    }

    // synchronization1: one call, stage masks OR-ed over the batch (conservative, same queue).
    impl.images1.clear();
    impl.buffers1.clear();
    u32 srcStages = 0;
    u32 dstStages = 0;
    for (u32 i = range.imageBegin; i < range.imageBegin + range.imageCount; ++i) {
        const ImageBarrier& b = imageBarriers[i];
        const Graph::Resource& resource = graph.m_resources[b.resource - 1u];
        if (resource.handle == nullptr) {
            continue;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = toSync1Access(b.srcAccess);
        barrier.dstAccessMask = toSync1Access(b.dstAccess);
        barrier.oldLayout = static_cast<VkImageLayout>(b.oldLayout);
        barrier.newLayout = static_cast<VkImageLayout>(b.newLayout);
        barrier.srcQueueFamilyIndex = family(b.srcQueue);
        barrier.dstQueueFamilyIndex = family(b.dstQueue);
        barrier.image = static_cast<VkImage>(resource.handle);
        barrier.subresourceRange = {aspectForFormat(resource.format), b.baseMip, b.mipCount, b.baseLayer,
                                    b.layerCount};
        srcStages |= static_cast<u32>(b.srcStages);
        dstStages |= static_cast<u32>(b.dstStages);
        impl.images1.push_back(barrier);
    }
    for (u32 i = range.bufferBegin; i < range.bufferBegin + range.bufferCount; ++i) {
        const BufferBarrier& b = graph.m_bufferBarriers[i];
        const Graph::Resource& resource = graph.m_resources[b.resource - 1u];
        if (resource.handle == nullptr) {
            continue;
        }
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = toSync1Access(b.srcAccess);
        barrier.dstAccessMask = toSync1Access(b.dstAccess);
        barrier.srcQueueFamilyIndex = family(b.srcQueue);
        barrier.dstQueueFamilyIndex = family(b.dstQueue);
        barrier.buffer = static_cast<VkBuffer>(resource.handle);
        barrier.offset = b.offset;
        barrier.size = b.size == 0u ? VK_WHOLE_SIZE : b.size;
        srcStages |= static_cast<u32>(b.srcStages);
        dstStages |= static_cast<u32>(b.dstStages);
        impl.buffers1.push_back(barrier);
    }
    if (impl.images1.empty() && impl.buffers1.empty()) {
        return;
    }
    vkCmdPipelineBarrier(cmd, toSync1Stages(srcStages, true), toSync1Stages(dstStages, false), 0, 0, nullptr,
                         static_cast<u32>(impl.buffers1.size()), impl.buffers1.data(),
                         static_cast<u32>(impl.images1.size()), impl.images1.data());
    ++result.barrierCalls;
    result.imageBarriers += static_cast<u32>(impl.images1.size());
    result.bufferBarriers += static_cast<u32>(impl.buffers1.size());
}

void Executor::recordBatchPasses(Graph& graph, u32 batchIndex, void* commandBuffer, ExecuteResult& result) {
    Impl& impl = *m_impl;
    const Batch& batch = graph.m_batches[batchIndex];
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    for (u32 pos = batch.orderBegin; pos < batch.orderBegin + batch.orderCount; ++pos) {
        const u32 passIndex = graph.m_order[pos];
        const Graph::Pass& pass = graph.m_passes[passIndex];
        recordBarriers(graph, pass.pre, commandBuffer, result);
        recordBarriers(graph, pass.late, commandBuffer, result, true);

        PassContext context;
        context.commandBuffer = commandBuffer;
        context.queue = pass.queue;
        context.passIndex = passIndex;
        context.graph = &graph;
        context.executor = this;
        if (m_labels) {
            VkDebugUtilsLabelEXT label{};
            label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = pass.name;
            impl.beginLabel(cmd, &label);
            ++result.debugLabels;
        }
        if (m_hooks.begin != nullptr) {
            m_hooks.begin(context, pass.name, m_hooks.user);
        }
        if (pass.fn != nullptr) {
            pass.fn(context, pass.user);
        }
        if (m_hooks.end != nullptr) {
            m_hooks.end(context, pass.name, m_hooks.user);
        }
        if (m_labels) {
            impl.endLabel(cmd);
        }
        ++result.executedPasses;
    }
    recordBarriers(graph, batch.post, commandBuffer, result);
}

bool Executor::beginFrameSlot() {
    Impl& impl = *m_impl;
    impl.slotIndex = (impl.slotIndex + 1u) % static_cast<u32>(impl.slots.size());
    FrameSlot& slot = impl.slots[impl.slotIndex];
    VkSemaphore semaphores[kQueueClassCount];
    u64 values[kQueueClassCount];
    u32 count = 0;
    for (u32 q = 0; q < kQueueClassCount; ++q) {
        if (slot.retire[q] > 0u) {
            semaphores[count] = impl.timeline[q];
            values[count] = slot.retire[q];
            ++count;
        }
    }
    if (count > 0u) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = count;
        waitInfo.pSemaphores = semaphores;
        waitInfo.pValues = values;
        if (vkWaitSemaphores(impl.device, &waitInfo, UINT64_MAX) != VK_SUCCESS) {
            return false;
        }
    }
    for (u32 q = 0; q < kQueueClassCount; ++q) {
        if (slot.fenceSubmitted[q]) {
            if (vkWaitForFences(impl.device, 1, &slot.fence[q], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                return false;
            }
            vkResetFences(impl.device, 1, &slot.fence[q]);
            slot.fenceSubmitted[q] = false;
        }
        if (slot.pool[q] != VK_NULL_HANDLE && slot.used[q] > 0u) {
            vkResetCommandPool(impl.device, slot.pool[q], 0);
        }
        slot.used[q] = 0;
        slot.retire[q] = 0;
    }
    slot.binaryUsed = 0;
    return true;
}

ExecuteResult Executor::execute(Graph& graph, const SubmitDesc& submit) {
    ExecuteResult result;
    const u32 rebuilds = m_transientStats.rebuilds;
    if (!prepare(graph)) {
        return result;
    }
    result.transientsRebuilt = m_transientStats.rebuilds != rebuilds;
    if (!beginFrameSlot()) {
        m_message = "frame slot wait failed";
        return result;
    }
    Impl& impl = *m_impl;
    FrameSlot& slot = impl.slots[impl.slotIndex];
    const std::vector<Batch>& batches = graph.m_batches;

    // Record every batch into its own command buffer.
    impl.batchCommandBuffers.clear();
    impl.batchValues.clear();
    for (u32 b = 0; b < batches.size(); ++b) {
        const u32 q = static_cast<u32>(batches[b].queue);
        if (slot.used[q] == slot.commandBuffers[q].size()) {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = slot.pool[q];
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            VkCommandBuffer fresh = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(impl.device, &allocInfo, &fresh) != VK_SUCCESS) {
                m_message = "vkAllocateCommandBuffers failed";
                return result;
            }
            slot.commandBuffers[q].push_back(fresh);
        }
        const VkCommandBuffer cmd = slot.commandBuffers[q][slot.used[q]++];
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        recordBatchPasses(graph, b, cmd, result);
        vkEndCommandBuffer(cmd);
        impl.batchCommandBuffers.push_back(cmd);
        impl.batchValues.push_back(0);
        ++result.commandBuffers;
    }

    // Cross-queue edges inside the frame: one binary semaphore per (producer batch -> consumer
    // batch) edge by default, so synchronization validation of every layer version can follow
    // them; timeline waits on the producer's value with timelineCrossQueueWaits. Every batch also
    // signals its queue's timeline (retirement, frame pacing, cross-frame ordering).
    const u32 batchCount = static_cast<u32>(batches.size());
    impl.edgeSemaphores.assign(static_cast<usize>(batchCount) * kQueueClassCount, VK_NULL_HANDLE);
    if (!m_desc.timelineCrossQueueWaits) {
        for (u32 b = 0; b < batchCount; ++b) {
            for (u32 other = 0; other < kQueueClassCount; ++other) {
                if (batches[b].waitBatch[other] < 0) {
                    continue;
                }
                if (slot.binaryUsed == slot.binary.size()) {
                    VkSemaphoreCreateInfo semaphoreInfo{};
                    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    VkSemaphore fresh = VK_NULL_HANDLE;
                    if (vkCreateSemaphore(impl.device, &semaphoreInfo, nullptr, &fresh) != VK_SUCCESS) {
                        m_message = "vkCreateSemaphore (binary edge) failed";
                        return result;
                    }
                    slot.binary.push_back(fresh);
                }
                impl.edgeSemaphores[b * kQueueClassCount + other] = slot.binary[slot.binaryUsed++];
            }
        }
    }

    // Submit in order: every wait targets a signal submitted earlier.
    i32 lastGraphics = -1;
    i32 lastOnQueue[kQueueClassCount] = {-1, -1, -1};
    for (u32 b = 0; b < batchCount; ++b) {
        if (batches[b].queue == QueueClass::Graphics) {
            lastGraphics = static_cast<i32>(b);
        }
        lastOnQueue[static_cast<u32>(batches[b].queue)] = static_cast<i32>(b);
    }
    bool firstOnQueue[kQueueClassCount] = {true, true, true};
    bool firstGraphics = true;
    for (u32 b = 0; b < batchCount; ++b) {
        const Batch& batch = batches[b];
        const u32 q = static_cast<u32>(batch.queue);
        constexpr u32 kMaxWaits = 2u * kQueueClassCount + 1u;
        VkSemaphore waitSemaphores[kMaxWaits];
        u64 waitValues[kMaxWaits];
        VkPipelineStageFlags waitStages[kMaxWaits];
        u32 waitCount = 0;
        auto addWait = [&](VkSemaphore semaphore, u64 value, VkPipelineStageFlags stages) {
            waitSemaphores[waitCount] = semaphore;
            waitValues[waitCount] = value;
            waitStages[waitCount] = stages;
            ++waitCount;
        };
        for (u32 other = 0; other < kQueueClassCount; ++other) {
            if (other == q) {
                continue;
            }
            u64 timelineValue = 0;
            const VkSemaphore edge = impl.edgeSemaphores[b * kQueueClassCount + other];
            if (edge != VK_NULL_HANDLE) {
                addWait(edge, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                ++result.binaryWaits;
            } else if (batch.waitBatch[other] >= 0) {
                timelineValue = impl.batchValues[static_cast<u32>(batch.waitBatch[other])];
            }
            if (firstOnQueue[q]) {
                timelineValue = std::max(timelineValue, impl.frameEnd[other]);
            }
            if (timelineValue > 0u && impl.timeline[other] != VK_NULL_HANDLE) {
                addWait(impl.timeline[other], timelineValue, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                ++result.timelineWaits;
            }
        }
        if (batch.queue == QueueClass::Graphics && firstGraphics && submit.waitSemaphore != nullptr) {
            addWait(static_cast<VkSemaphore>(submit.waitSemaphore), 0, toSync1Stages(submit.waitStages, false));
        }
        firstOnQueue[q] = false;
        if (batch.queue == QueueClass::Graphics) {
            firstGraphics = false;
        }

        impl.signalScratch.clear();
        impl.signalValueScratch.clear();
        const u64 signalValue = ++impl.value[q];
        impl.signalScratch.push_back(impl.timeline[q]);
        impl.signalValueScratch.push_back(signalValue);
        for (u32 consumer = b + 1u; consumer < batchCount; ++consumer) {
            const VkSemaphore edge = impl.edgeSemaphores[consumer * kQueueClassCount + q];
            if (edge != VK_NULL_HANDLE && batches[consumer].waitBatch[q] == static_cast<i32>(b)) {
                impl.signalScratch.push_back(edge);
                impl.signalValueScratch.push_back(0);
            }
        }
        if (static_cast<i32>(b) == lastGraphics && submit.signalSemaphore != nullptr) {
            impl.signalScratch.push_back(static_cast<VkSemaphore>(submit.signalSemaphore));
            impl.signalValueScratch.push_back(0);
        }
        impl.batchValues[b] = signalValue;

        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.waitSemaphoreValueCount = waitCount;
        timelineInfo.pWaitSemaphoreValues = waitValues;
        timelineInfo.signalSemaphoreValueCount = static_cast<u32>(impl.signalValueScratch.size());
        timelineInfo.pSignalSemaphoreValues = impl.signalValueScratch.data();
        VkSubmitInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.pNext = &timelineInfo;
        info.waitSemaphoreCount = waitCount;
        info.pWaitSemaphores = waitSemaphores;
        info.pWaitDstStageMask = waitStages;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &impl.batchCommandBuffers[b];
        info.signalSemaphoreCount = static_cast<u32>(impl.signalScratch.size());
        info.pSignalSemaphores = impl.signalScratch.data();
        const bool lastOfQueue = lastOnQueue[q] == static_cast<i32>(b);
        const VkFence fence = lastOfQueue ? slot.fence[q] : VK_NULL_HANDLE;
        if (vkQueueSubmit(impl.queue[q], 1, &info, fence) != VK_SUCCESS) {
            m_message = "vkQueueSubmit failed";
            return result;
        }
        if (lastOfQueue) {
            slot.fenceSubmitted[q] = true;
        }
        ++result.submissions;
        slot.retire[q] = signalValue;
        result.signalled[q] = signalValue;
    }
    if (submit.fence != nullptr && batchCount > 0u) {
        // Caller fence: signalled when the queue of the frame's last batch has finished it.
        const u32 q = static_cast<u32>(batches[batchCount - 1u].queue);
        if (vkQueueSubmit(impl.queue[q], 0, nullptr, static_cast<VkFence>(submit.fence)) != VK_SUCCESS) {
            m_message = "vkQueueSubmit (fence) failed";
            return result;
        }
    }
    for (u32 q = 0; q < kQueueClassCount; ++q) {
        impl.frameEnd[q] = impl.value[q];
    }
    result.ok = true;
    return result;
}

ExecuteResult Executor::recordInline(Graph& graph, void* commandBuffer, QueueClass queue) {
    ExecuteResult result;
    const u32 rebuilds = m_transientStats.rebuilds;
    if (commandBuffer == nullptr || !prepare(graph, static_cast<u8>(queue))) {
        return result;
    }
    result.transientsRebuilt = m_transientStats.rebuilds != rebuilds;
    for (u32 b = 0; b < graph.m_batches.size(); ++b) {
        recordBatchPasses(graph, b, commandBuffer, result);
    }
    result.ok = true;
    return result;
}

#else // !FUSE_VULKAN_BACKEND

struct Executor::Impl {};

std::unique_ptr<Executor> Executor::create(VulkanDevice& device, GpuAllocator* allocator, const ExecutorDesc& desc) {
    auto executor = std::unique_ptr<Executor>(new Executor());
    executor->initialize(device, allocator, desc);
    return executor;
}
Executor::~Executor() = default;
bool Executor::initialize(VulkanDevice& device, GpuAllocator*, const ExecutorDesc& desc) {
    m_device = &device;
    m_desc = desc;
    m_message = "render graph executor requires the Vulkan backend";
    return false;
}
void Executor::shutdown() {}
bool Executor::queueAvailable(QueueClass) const {
    return false;
}
u32 Executor::queueFamily(QueueClass) const {
    return UINT32_MAX;
}
CompileOptions Executor::compileOptions() const {
    return {};
}
bool Executor::prepare(Graph&, u8) {
    return false;
}
ExecuteResult Executor::execute(Graph&, const SubmitDesc&) {
    return {};
}
ExecuteResult Executor::recordInline(Graph&, void*, QueueClass) {
    return {};
}
bool Executor::waitIdle() {
    return true;
}
bool Executor::transientMemory(const Graph&, TextureRef, TransientMemory&) const {
    return false;
}
bool Executor::transientMemory(const Graph&, BufferRef, TransientMemory&) const {
    return false;
}
bool Executor::realizeTransients(Graph&) {
    return false;
}
void Executor::destroyTransients() {}
void Executor::recordBatchPasses(Graph&, u32, void*, ExecuteResult&) {}
void Executor::recordBarriers(const Graph&, const BarrierRange&, void*, ExecuteResult&, bool) {}
bool Executor::beginFrameSlot() {
    return false;
}

#endif

} // namespace fuse::renderer::rg
