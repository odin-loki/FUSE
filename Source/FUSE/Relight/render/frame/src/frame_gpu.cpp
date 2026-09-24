// FUSE Relight RL-4.1: FUSE's GPU work on the host's device (see frame_gpu.hpp).
#include <fuse/relight/render/frame/frame_gpu.hpp>

#include <fuse/relight/render/frame/vk_dispatch.hpp>
#include <fuse/renderer/rg/graph.hpp>

#include <algorithm>
#include <vector>

namespace fuse::relight::render::frame {

namespace rg = fuse::renderer::rg;

namespace {

constexpr std::uint32_t kLayoutGeneral = VK_IMAGE_LAYOUT_GENERAL;

VkImageSubresourceRange fullRange(const tap::HostImageInfo& i) {
    return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, i.mipLevels, 0, i.arrayLayers};
}

VkMemoryBarrier2 globalBarrier(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                               VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    return b;
}

} // namespace

struct FrameGpu::Impl {
    static constexpr std::uint32_t kRing = 2;
    tap::IFrameHost* host = nullptr;
    VkDispatch vk;
    VkQueue queue = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkSemaphore release = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kRing] = {};
    VkFence fence[kRing] = {};
    bool pending[kRing] = {};
    std::uint32_t next = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    rg::Graph graph;
    std::string lastError;

    bool beginSlot(std::uint32_t& slot);
    bool submitOneOff(VkImage image, const VkImageSubresourceRange& range, std::uint64_t& submissions);
    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
};

FrameGpu::FrameGpu() = default;
FrameGpu::~FrameGpu() { shutdown(); }

bool FrameGpu::ready() const { return m_impl && m_impl->host != nullptr && m_impl->pool != VK_NULL_HANDLE; }

bool FrameGpu::init(tap::IFrameHost& host, std::string* error) {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    Impl& d = *m_impl;
    shutdown();
    const tap::VulkanDevice v = host.vulkan();
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(static_cast<std::uintptr_t>(host.getInstanceProcAddr()));
    const char* missing = nullptr;
    if (!d.vk.load(gipa, vkHandle<VkInstance>(v.instance), vkHandle<VkPhysicalDevice>(v.physicalDevice),
                   vkHandle<VkDevice>(v.device), &missing)) {
        m_error = std::string("Vulkan entry point missing: ") + (missing ? missing : "?");
        if (error) {
            *error = m_error;
        }
        return false;
    }
    d.vk.GetPhysicalDeviceMemoryProperties(d.vk.physicalDevice, &d.memory);
    d.queue = vkHandle<VkQueue>(v.queue);
    d.acquire = vkHandle<VkSemaphore>(host.acquireSemaphore());
    d.release = vkHandle<VkSemaphore>(host.releaseSemaphore());
    if (d.queue == VK_NULL_HANDLE || d.acquire == VK_NULL_HANDLE || d.release == VK_NULL_HANDLE) {
        m_error = "host has no queue or timeline semaphores";
        if (error) {
            *error = m_error;
        }
        return false;
    }

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = v.queueFamily;
    if (d.vk.CreateCommandPool(d.vk.device, &pool, nullptr, &d.pool) != VK_SUCCESS) {
        m_error = "vkCreateCommandPool failed";
        d.pool = VK_NULL_HANDLE;
        if (error) {
            *error = m_error;
        }
        return false;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = d.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = Impl::kRing;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    bool ok = d.vk.AllocateCommandBuffers(d.vk.device, &alloc, d.cmd) == VK_SUCCESS;
    for (std::uint32_t i = 0; ok && i < Impl::kRing; ++i) {
        ok = d.vk.CreateFence(d.vk.device, &fence, nullptr, &d.fence[i]) == VK_SUCCESS;
    }
    d.host = &host;
    if (!ok) {
        m_error = "command ring creation failed";
        if (error) {
            *error = m_error;
        }
        shutdown();
        return false;
    }
    return true;
}

bool FrameGpu::waitIdle() {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    Impl& d = *m_impl;
    if (!d.host) {
        return true;
    }
    bool ok = true;
    for (std::uint32_t i = 0; i < Impl::kRing; ++i) {
        if (d.pending[i]) {
            ok = d.vk.WaitForFences(d.vk.device, 1, &d.fence[i], VK_TRUE, 10'000'000'000ull) == VK_SUCCESS && ok;
            d.pending[i] = false;
        }
    }
    return ok;
}

void FrameGpu::shutdown() {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    Impl& d = *m_impl;
    if (!d.vk.loaded()) {
        d.host = nullptr;
        return;
    }
    waitIdle();
    for (std::uint32_t i = 0; i < Impl::kRing; ++i) {
        if (d.fence[i] != VK_NULL_HANDLE) {
            d.vk.DestroyFence(d.vk.device, d.fence[i], nullptr);
            d.fence[i] = VK_NULL_HANDLE;
        }
        d.cmd[i] = VK_NULL_HANDLE;
    }
    if (d.pool != VK_NULL_HANDLE) {
        d.vk.DestroyCommandPool(d.vk.device, d.pool, nullptr);
        d.pool = VK_NULL_HANDLE;
    }
    d.host = nullptr;
    d.vk = VkDispatch{};
}

std::uint32_t FrameGpu::Impl::memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool FrameGpu::Impl::beginSlot(std::uint32_t& slot) {
    slot = next;
    next = (next + 1) % kRing;
    if (pending[slot]) {
        if (vk.WaitForFences(vk.device, 1, &fence[slot], VK_TRUE, 10'000'000'000ull) != VK_SUCCESS) {
            lastError = "timeout waiting for a FUSE submission";
            return false;
        }
        pending[slot] = false;
    }
    vk.ResetFences(vk.device, 1, &fence[slot]);
    vk.ResetCommandBuffer(cmd[slot], 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk.BeginCommandBuffer(cmd[slot], &begin) == VK_SUCCESS;
}

bool FrameGpu::Impl::submitOneOff(VkImage image, const VkImageSubresourceRange& range, std::uint64_t& submissions) {
    std::uint32_t slot = 0;
    if (!beginSlot(slot)) {
        return false;
    }
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vk.CmdPipelineBarrier2(cmd[slot], &dep);
    if (vk.EndCommandBuffer(cmd[slot]) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferSubmitInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cb.commandBuffer = cmd[slot];
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cb;
    host->lockQueue();
    const VkResult r = vk.QueueSubmit2(queue, 1, &submit, fence[slot]);
    host->unlockQueue();
    if (r != VK_SUCCESS) {
        lastError = "vkQueueSubmit2 (layout init) failed";
        return false;
    }
    pending[slot] = true;
    ++submissions;
    return true;
}

bool FrameGpu::createImage(const tap::HostImageInfo& like, std::uint32_t usage, bool general, GpuImage& out) {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    Impl& d = *m_impl;
    out = GpuImage{};
    if (!ready()) {
        return false;
    }
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags = like.flags;
    ci.imageType = static_cast<VkImageType>(like.imageType);
    ci.format = static_cast<VkFormat>(like.format);
    ci.extent = VkExtent3D{like.width, like.height, std::max(like.depth, 1u)};
    ci.mipLevels = std::max(like.mipLevels, 1u);
    ci.arrayLayers = std::max(like.arrayLayers, 1u);
    ci.samples = static_cast<VkSampleCountFlagBits>(like.samples ? like.samples : 1u);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat viewFormats[4] = {};
    VkImageFormatListCreateInfo list{};
    const std::uint32_t viewCount = std::min<std::uint32_t>(like.viewFormatCount, 4u);
    if (viewCount) {
        for (std::uint32_t i = 0; i < viewCount; ++i) {
            viewFormats[i] = static_cast<VkFormat>(like.viewFormats[i]);
        }
        list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        list.viewFormatCount = viewCount;
        list.pViewFormats = viewFormats;
        ci.pNext = &list;
    }
    VkImage image = VK_NULL_HANDLE;
    if (d.vk.CreateImage(d.vk.device, &ci, nullptr, &image) != VK_SUCCESS) {
        m_error = "vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements req{};
    d.vk.GetImageMemoryRequirements(d.vk.device, image, &req);
    VkMemoryAllocateInfo mem{};
    mem.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem.allocationSize = req.size;
    mem.memoryTypeIndex = d.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem.memoryTypeIndex == UINT32_MAX) {
        mem.memoryTypeIndex = d.memoryType(req.memoryTypeBits, 0);
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (mem.memoryTypeIndex == UINT32_MAX || d.vk.AllocateMemory(d.vk.device, &mem, nullptr, &memory) != VK_SUCCESS ||
        d.vk.BindImageMemory(d.vk.device, image, memory, 0) != VK_SUCCESS) {
        if (memory != VK_NULL_HANDLE) {
            d.vk.FreeMemory(d.vk.device, memory, nullptr);
        }
        d.vk.DestroyImage(d.vk.device, image, nullptr);
        m_error = "image memory allocation failed";
        return false;
    }
    out.image.vkImage = vkValue(image);
    out.image.info = like;
    out.image.info.vkImage = out.image.vkImage;
    out.image.info.usage = usage;
    out.image.info.depth = ci.extent.depth;
    out.image.info.mipLevels = ci.mipLevels;
    out.image.info.arrayLayers = ci.arrayLayers;
    out.image.info.samples = static_cast<std::uint32_t>(ci.samples);
    out.memory = vkValue(memory);
    if (general && !d.submitOneOff(image, fullRange(out.image.info), m_submissions)) {
        m_error = d.lastError;
        destroyImage(out);
        return false;
    }
    return true;
}

void FrameGpu::destroyImage(GpuImage& image) {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    Impl& d = *m_impl;
    if (d.vk.loaded() && image.image.vkImage) {
        d.vk.DestroyImage(d.vk.device, vkHandle<VkImage>(image.image.vkImage), nullptr);
    }
    if (d.vk.loaded() && image.memory) {
        d.vk.FreeMemory(d.vk.device, vkHandle<VkDeviceMemory>(image.memory), nullptr);
    }
    image = GpuImage{};
}

FrameSubmitStats FrameGpu::submitFrame(FramePass pass, const GpuImage* input, const GpuImage& output,
                                       std::uint32_t solidRgb, std::uint64_t waitAcquire, std::uint64_t signalRelease) {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    Impl& d = *m_impl;
    FrameSubmitStats stats;
    if (!ready() || !output.valid() || (pass == FramePass::Passthrough && (!input || !input->valid()))) {
        m_error = "frame submission without images";
        return stats;
    }

    // ---- the frame graph (RG v2: Vulkan-free compile + plan; FUSE records the barriers itself) ----
    d.graph.reset();
    auto import = [&](const GpuImage& img, const char* name) {
        rg::ImportedImage i;
        i.image = reinterpret_cast<void*>(static_cast<std::uintptr_t>(img.image.vkImage));
        i.format = img.image.info.format;
        i.width = img.image.info.width;
        i.height = img.image.info.height;
        i.depth = img.image.info.depth;
        i.mipLevels = img.image.info.mipLevels;
        i.arrayLayers = img.image.info.arrayLayers;
        i.initialLayout = kLayoutGeneral; // the hand-over layout (frame_host.hpp)
        i.finalLayout = kLayoutGeneral;
        i.name = name;
        return d.graph.importImage(i);
    };
    const rg::TextureRef out = import(output, "fuse.output");
    rg::TextureRef in{};
    if (pass == FramePass::Passthrough) {
        in = import(*input, "fuse.input");
        d.graph.addPass("fuse.passthrough", nullptr, nullptr)
            .use(in, rg::Access::TransferSrc)
            .use(out, rg::Access::TransferDst)
            .neverCull();
    } else {
        d.graph.addPass("fuse.solid", nullptr, nullptr).use(out, rg::Access::TransferDst).neverCull();
    }
    rg::CompileOptions options;
    options.forceQueue = static_cast<std::uint8_t>(rg::QueueClass::Graphics);
    if (!d.graph.compile(options) || !d.graph.plan()) {
        m_error = "frame graph compile / plan failed";
        return stats;
    }
    auto imageOf = [&](std::uint32_t resource) -> VkImage {
        if (resource == out.id) {
            return vkHandle<VkImage>(output.image.vkImage);
        }
        if (in.valid() && resource == in.id) {
            return vkHandle<VkImage>(input->image.vkImage);
        }
        return VK_NULL_HANDLE;
    };

    std::uint32_t slot = 0;
    if (!d.beginSlot(slot)) {
        m_error = d.lastError;
        return stats;
    }
    const VkCommandBuffer cmd = d.cmd[slot];
    std::vector<VkImageMemoryBarrier2> barriers;
    auto record = [&](const std::vector<rg::ImageBarrier>& list, const rg::BarrierRange& range) {
        barriers.clear();
        for (std::uint32_t i = range.imageBegin; i < range.imageBegin + range.imageCount; ++i) {
            const rg::ImageBarrier& b = list[i];
            VkImageMemoryBarrier2 v{};
            v.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            v.srcStageMask = b.srcStages;
            v.srcAccessMask = b.srcAccess;
            v.dstStageMask = b.dstStages;
            v.dstAccessMask = b.dstAccess;
            if (v.dstStageMask == 0) {
                // The graph's final transitions (back to the GENERAL hand-over layout) have no consumer inside the
                // graph: make them part of a dependency chain the host's next use (same queue) and the release
                // signal pick up.
                v.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                v.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            }
            v.oldLayout = static_cast<VkImageLayout>(b.oldLayout);
            v.newLayout = static_cast<VkImageLayout>(b.newLayout);
            v.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; // one queue
            v.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            v.image = imageOf(b.resource);
            v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, b.baseMip, b.mipCount, b.baseLayer, b.layerCount};
            if (v.image != VK_NULL_HANDLE) {
                barriers.push_back(v);
            }
        }
        if (barriers.empty()) {
            return;
        }
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        d.vk.CmdPipelineBarrier2(cmd, &dep);
        ++stats.barrierCalls;
        stats.imageBarriers += dep.imageMemoryBarrierCount;
    };
    auto global = [&](const VkMemoryBarrier2& b) {
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        d.vk.CmdPipelineBarrier2(cmd, &dep);
        ++stats.barrierCalls;
    };

    // The host's work on these images (copies into / out of them) precedes on the same queue.
    global(globalBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
    for (const std::uint32_t p : d.graph.executionOrder()) {
        record(d.graph.imageBarriers(), d.graph.passBarriers(p));
        record(d.graph.lateImageBarriers(), d.graph.passLateBarriers(p));
        if (pass == FramePass::Solid) {
            VkClearColorValue c{};
            c.float32[0] = static_cast<float>((solidRgb >> 16) & 0xffu) / 255.0f;
            c.float32[1] = static_cast<float>((solidRgb >> 8) & 0xffu) / 255.0f;
            c.float32[2] = static_cast<float>(solidRgb & 0xffu) / 255.0f;
            c.float32[3] = 1.0f;
            const VkImageSubresourceRange range = fullRange(output.image.info);
            d.vk.CmdClearColorImage(cmd, vkHandle<VkImage>(output.image.vkImage), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &c, 1, &range);
        } else {
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = region.srcSubresource;
            region.extent = VkExtent3D{std::min(input->image.info.width, output.image.info.width),
                                       std::min(input->image.info.height, output.image.info.height), 1};
            d.vk.CmdCopyImage(cmd, vkHandle<VkImage>(input->image.vkImage), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              vkHandle<VkImage>(output.image.vkImage), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        ++stats.passes;
    }
    for (const rg::Batch& b : d.graph.batches()) {
        record(d.graph.imageBarriers(), b.post);
    }
    global(globalBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
    stats.graphBatches = static_cast<std::uint32_t>(d.graph.batches().size());
    if (d.vk.EndCommandBuffer(cmd) != VK_SUCCESS) {
        m_error = "vkEndCommandBuffer failed";
        return stats;
    }

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = d.acquire;
    wait.value = waitAcquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = d.release;
    signal.value = signalRelease;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cb.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cb;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    d.host->lockQueue();
    const VkResult r = d.vk.QueueSubmit2(d.queue, 1, &submit, d.fence[slot]);
    d.host->unlockQueue();
    if (r != VK_SUCCESS) {
        m_error = "vkQueueSubmit2 failed";
        return stats;
    }
    d.pending[slot] = true;
    ++m_submissions;
    stats.ok = true;
    return stats;
}

std::uint64_t FrameGpu::acquireCompleted() const {
    if (!m_impl) {
        return 0;
    }
    const Impl& d = *m_impl;
    std::uint64_t v = 0;
    if (d.vk.loaded() && d.acquire != VK_NULL_HANDLE) {
        d.vk.GetSemaphoreCounterValue(d.vk.device, d.acquire, &v);
    }
    return v;
}

bool FrameGpu::waitRelease(std::uint64_t value, std::uint64_t timeoutNs) const {
    if (!m_impl) {
        return false;
    }
    const Impl& d = *m_impl;
    if (!d.vk.loaded() || d.release == VK_NULL_HANDLE) {
        return false;
    }
    VkSemaphoreWaitInfo w{};
    w.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    w.semaphoreCount = 1;
    w.pSemaphores = &d.release;
    w.pValues = &value;
    return d.vk.WaitSemaphores(d.vk.device, &w, timeoutNs) == VK_SUCCESS;
}

} // namespace fuse::relight::render::frame
