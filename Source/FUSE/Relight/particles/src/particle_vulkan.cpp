// FUSE Relight RL-3.6: the Vulkan compute backend (see particle_vulkan.hpp).
#include <fuse/relight/particles/particle_vulkan.hpp>

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#if __has_include("relight_particle_spv.h")
#include "relight_particle_spv.h"
#endif

namespace fuse::relight::particles {

bool vulkanKernelsAvailable() {
#if defined(FUSE_RELIGHT_PARTICLE_SPV)
    return true;
#else
    return false;
#endif
}

namespace {

using renderer::Buffer;
using renderer::BufferDesc;
using renderer::BufferUsage;
using renderer::MemoryUsage;

constexpr std::uint64_t kFenceTimeoutNs = 120ull * 1000000000ull;

BufferUsage usageOf(std::initializer_list<BufferUsage> list) {
    std::uint32_t bits = 0;
    for (BufferUsage u : list) {
        bits |= static_cast<std::uint32_t>(u);
    }
    return static_cast<BufferUsage>(bits);
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
             VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

struct VulkanParticleBackend::Impl {
    renderer::VulkanDevice& device;
    renderer::GpuAllocator& allocator;
    VkDevice vk = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    ManagerConfig config;
    std::uint32_t slots = 1;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> pipelines{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    Buffer particles{}, vertices{};
    Buffer positions{}, colors{}, texcoords{}, indices{}, animation{};
    struct Slot {
        Buffer constants{}, spawnContexts{}, spawnMap{}, counters{};
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
    };
    std::vector<Slot> frames;

    Impl(renderer::VulkanDevice& d, renderer::GpuAllocator& a) : device(d), allocator(a) {}

    ~Impl() {
        if (vk == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(vk);
        for (Slot& s : frames) {
            if (s.fence != VK_NULL_HANDLE) {
                vkDestroyFence(vk, s.fence, nullptr);
            }
            for (Buffer* b : {&s.constants, &s.spawnContexts, &s.spawnMap, &s.counters}) {
                if (b->handle != nullptr) {
                    allocator.destroyBuffer(*b);
                }
            }
        }
        for (Buffer* b : {&particles, &vertices, &positions, &colors, &texcoords, &indices, &animation}) {
            if (b->handle != nullptr) {
                allocator.destroyBuffer(*b);
            }
        }
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vk, commandPool, nullptr);
        }
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(vk, descriptorPool, nullptr);
        }
        for (VkPipeline p : pipelines) {
            if (p != VK_NULL_HANDLE) {
                vkDestroyPipeline(vk, p, nullptr);
            }
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk, pipelineLayout, nullptr);
        }
        if (setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, setLayout, nullptr);
        }
    }

    bool makeBuffer(std::size_t bytes, BufferUsage usage, MemoryUsage memory, const char* name, Buffer& out, std::string& error) {
        BufferDesc d{};
        d.size = std::max<std::size_t>(bytes, 16u);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = name;
        if (!allocator.createBuffer(d, out)) {
            error = std::string("buffer allocation failed: ") + name;
            return false;
        }
        if (memory != MemoryUsage::GpuOnly) {
            if (out.mapped == nullptr || (out.memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u) {
                error = std::string("host-visible coherent memory required: ") + name;
                return false;
            }
        }
        return true;
    }

    bool waitSlot(Slot& s) {
        if (!s.submitted) {
            return true;
        }
        if (vkWaitForFences(vk, 1, &s.fence, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
            return false;
        }
        s.submitted = false;
        return true;
    }

    bool waitAll() {
        bool ok = true;
        for (Slot& s : frames) {
            ok = waitSlot(s) && ok;
        }
        return ok;
    }
};

VulkanParticleBackend::VulkanParticleBackend(renderer::VulkanDevice& device, renderer::GpuAllocator& allocator)
    : m_impl(std::make_unique<Impl>(device, allocator)) {}

VulkanParticleBackend::~VulkanParticleBackend() = default;

void* VulkanParticleBackend::vertexBuffer() const { return m_impl->vertices.handle; }

bool VulkanParticleBackend::init(const ManagerConfig& config) {
    Impl& m = *m_impl;
#if !defined(FUSE_RELIGHT_PARTICLE_SPV)
    (void)config;
    (void)m;
    m_error = "particle kernels not built (glslangValidator unavailable)";
    return false;
#else
    m.config = config;
    m.slots = std::max(1u, config.framesInFlight);
    m.vk = static_cast<VkDevice>(m.device.nativeHandle());
    m.queue = static_cast<VkQueue>(m.device.queues().compute);
    m.queueFamily = m.device.queues().computeFamily;
    if (m.queue == VK_NULL_HANDLE) {
        m.queue = static_cast<VkQueue>(m.device.queues().graphics);
        m.queueFamily = m.device.queues().graphicsFamily;
    }
    if (m.vk == VK_NULL_HANDLE || m.queue == VK_NULL_HANDLE) {
        m_error = "no Vulkan device / queue";
        return false;
    }

    // Layouts and pipelines.
    std::array<VkDescriptorSetLayoutBinding, kBindingCount> bindings{};
    for (std::uint32_t i = 0; i < kBindingCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kBindingCount;
    sli.pBindings = bindings.data();
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPushConstants)};
    if (vkCreateDescriptorSetLayout(m.vk, &sli, nullptr, &m.setLayout) != VK_SUCCESS) {
        m_error = "vkCreateDescriptorSetLayout";
        return false;
    }
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m.setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(m.vk, &pli, nullptr, &m.pipelineLayout) != VK_SUCCESS) {
        m_error = "vkCreatePipelineLayout";
        return false;
    }
    const std::uint32_t* code[3] = {kRelightParticleSpawnSpv, kRelightParticleEvolveSpv, kRelightParticleBillboardSpv};
    const std::size_t bytes[3] = {sizeof(kRelightParticleSpawnSpv), sizeof(kRelightParticleEvolveSpv), sizeof(kRelightParticleBillboardSpv)};
    for (int i = 0; i < 3; ++i) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = bytes[i];
        mi.pCode = code[i];
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m.vk, &mi, nullptr, &module) != VK_SUCCESS) {
            m_error = "vkCreateShaderModule";
            return false;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = m.pipelineLayout;
        const VkResult r = vkCreateComputePipelines(m.vk, VK_NULL_HANDLE, 1, &ci, nullptr, &m.pipelines[static_cast<std::size_t>(i)]);
        vkDestroyShaderModule(m.vk, module, nullptr);
        if (r != VK_SUCCESS) {
            m_error = "vkCreateComputePipelines";
            return false;
        }
    }

    // Buffers.
    const BufferUsage storage = usageOf({BufferUsage::Storage});
    const BufferUsage pool = usageOf({BufferUsage::Storage, BufferUsage::TransferDst, BufferUsage::TransferSrc});
    const BufferUsage vertexPool = usageOf({BufferUsage::Storage, BufferUsage::TransferSrc, BufferUsage::Vertex});
    std::string& e = m_error;
    if (!m.makeBuffer(sizeof(GpuParticle) * config.particleCapacity, pool, MemoryUsage::GpuOnly, "relight.particles.pool", m.particles, e) ||
        !m.makeBuffer(sizeof(GpuParticleVertex) * config.vertexCapacity, vertexPool, MemoryUsage::GpuOnly, "relight.particles.vertices",
                      m.vertices, e) ||
        !m.makeBuffer(sizeof(float) * 3u * config.geometryVertexCapacity, storage, MemoryUsage::CpuToGpu, "relight.particles.positions",
                      m.positions, e) ||
        !m.makeBuffer(sizeof(std::uint32_t) * config.geometryVertexCapacity, storage, MemoryUsage::CpuToGpu, "relight.particles.colors",
                      m.colors, e) ||
        !m.makeBuffer(sizeof(float) * 2u * config.geometryVertexCapacity, storage, MemoryUsage::CpuToGpu, "relight.particles.texcoords",
                      m.texcoords, e) ||
        !m.makeBuffer(sizeof(std::uint32_t) * config.geometryIndexCapacity, storage, MemoryUsage::CpuToGpu, "relight.particles.indices",
                      m.indices, e) ||
        !m.makeBuffer(sizeof(Float4) * kAnimationTexels * config.maxSystems, storage, MemoryUsage::CpuToGpu, "relight.particles.animation",
                      m.animation, e)) {
        return false;
    }
    const BufferUsage counters = usageOf({BufferUsage::Storage, BufferUsage::TransferDst});
    m.frames.resize(m.slots);
    for (Impl::Slot& s : m.frames) {
        if (!m.makeBuffer(sizeof(GpuFrameConstants) * config.maxSystems, storage, MemoryUsage::CpuToGpu, "relight.particles.constants",
                          s.constants, e) ||
            !m.makeBuffer(sizeof(GpuSpawnContext) * config.maxSpawnContexts, storage, MemoryUsage::CpuToGpu,
                          "relight.particles.spawn_contexts", s.spawnContexts, e) ||
            !m.makeBuffer(sizeof(std::uint32_t) * config.particleCapacity, storage, MemoryUsage::CpuToGpu, "relight.particles.spawn_map",
                          s.spawnMap, e) ||
            !m.makeBuffer(sizeof(std::uint32_t) * config.maxSystems, counters, MemoryUsage::GpuToCpu, "relight.particles.counters",
                          s.counters, e)) {
            return false;
        }
        std::memset(s.counters.mapped, 0, sizeof(std::uint32_t) * config.maxSystems);
    }

    // Descriptor sets (one per frame slot), command buffers and fences.
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindingCount * m.slots};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = m.slots;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(m.vk, &dpi, nullptr, &m.descriptorPool) != VK_SUCCESS) {
        m_error = "vkCreateDescriptorPool";
        return false;
    }
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = m.queueFamily;
    if (vkCreateCommandPool(m.vk, &cpi, nullptr, &m.commandPool) != VK_SUCCESS) {
        m_error = "vkCreateCommandPool";
        return false;
    }
    for (Impl::Slot& s : m.frames) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m.descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m.setLayout;
        if (vkAllocateDescriptorSets(m.vk, &ai, &s.set) != VK_SUCCESS) {
            m_error = "vkAllocateDescriptorSets";
            return false;
        }
        const Buffer* buffers[kBindingCount] = {&s.constants, &m.particles, &s.spawnContexts, &s.spawnMap, &m.positions, &m.colors,
                                                &m.texcoords, &m.indices,   &m.animation,     &m.vertices, &s.counters};
        std::array<VkDescriptorBufferInfo, kBindingCount> infos{};
        std::array<VkWriteDescriptorSet, kBindingCount> writes{};
        for (std::uint32_t i = 0; i < kBindingCount; ++i) {
            infos[i] = VkDescriptorBufferInfo{static_cast<VkBuffer>(buffers[i]->handle), 0, VK_WHOLE_SIZE};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = s.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m.vk, kBindingCount, writes.data(), 0, nullptr);
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = m.commandPool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m.vk, &ci, &s.cmd) != VK_SUCCESS) {
            m_error = "vkAllocateCommandBuffers";
            return false;
        }
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(m.vk, &fi, nullptr, &s.fence) != VK_SUCCESS) {
            m_error = "vkCreateFence";
            return false;
        }
    }
    return true;
#endif
}

bool VulkanParticleBackend::execute(const FrameWork& work) {
    Impl& m = *m_impl;
    if (m.frames.empty()) {
        return false;
    }
    Impl::Slot& s = m.frames[static_cast<std::size_t>(work.serial % m.slots)];
    if (!m.waitSlot(s)) {
        m_error = "fence wait";
        return false;
    }
    // Shared host pools: every frame in flight may read them, so wait for all before rewriting.
    const bool geometryDirty = work.geometryVertexEnd > work.geometryVertexBegin || work.geometryIndexEnd > work.geometryIndexBegin;
    if (geometryDirty || !work.animationDirty.empty()) {
        if (!m.waitAll()) {
            m_error = "fence wait";
            return false;
        }
    }
    if (work.geometryVertexEnd > work.geometryVertexBegin) {
        const std::size_t b = work.geometryVertexBegin, n = work.geometryVertexEnd - work.geometryVertexBegin;
        std::memcpy(static_cast<float*>(m.positions.mapped) + b * 3u, work.positions.data() + b * 3u, n * 3u * sizeof(float));
        std::memcpy(static_cast<std::uint32_t*>(m.colors.mapped) + b, work.colors.data() + b, n * sizeof(std::uint32_t));
        std::memcpy(static_cast<float*>(m.texcoords.mapped) + b * 2u, work.texcoords.data() + b * 2u, n * 2u * sizeof(float));
    }
    if (work.geometryIndexEnd > work.geometryIndexBegin) {
        const std::size_t b = work.geometryIndexBegin, n = work.geometryIndexEnd - work.geometryIndexBegin;
        std::memcpy(static_cast<std::uint32_t*>(m.indices.mapped) + b, work.indices.data() + b, n * sizeof(std::uint32_t));
    }
    for (const std::uint32_t sys : work.animationDirty) {
        const std::size_t b = static_cast<std::size_t>(sys) * kAnimationTexels;
        std::memcpy(static_cast<Float4*>(m.animation.mapped) + b, work.animation.data() + b, kAnimationTexels * sizeof(Float4));
    }
    // This slot's per-frame arrays.
    for (const std::uint32_t sys : work.activeSystems) {
        const GpuFrameConstants& c = work.constants[sys];
        static_cast<GpuFrameConstants*>(s.constants.mapped)[sys] = c;
        if (c.spawnParticleCount > 0u) {
            std::memcpy(static_cast<std::uint32_t*>(s.spawnMap.mapped) + c.spawnMapBase, work.spawnMap.data() + c.spawnMapBase,
                        c.spawnParticleCount * sizeof(std::uint32_t));
        }
    }
    if (!work.spawnContexts.empty()) {
        std::memcpy(s.spawnContexts.mapped, work.spawnContexts.data(), work.spawnContexts.size_bytes());
    }

    VkCommandBuffer cmd = s.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    // Earlier frames' compute (and readback copies) before this frame's clears and dispatches.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdFillBuffer(cmd, static_cast<VkBuffer>(s.counters.handle), 0, sizeof(std::uint32_t) * m.config.maxSystems, 0u);
    for (const auto& clear : work.clears) {
        vkCmdFillBuffer(cmd, static_cast<VkBuffer>(m.particles.handle), static_cast<VkDeviceSize>(clear[0]) * sizeof(GpuParticle),
                        static_cast<VkDeviceSize>(clear[1]) * sizeof(GpuParticle), 0u);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipelineLayout, 0, 1, &s.set, 0, nullptr);
    for (std::uint32_t pass = 0; pass < 3u; ++pass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipelines[pass]);
        for (const std::uint32_t sys : work.activeSystems) {
            const GpuFrameConstants& c = work.constants[sys];
            const std::uint32_t items = pass == 0u ? c.spawnParticleCount : (pass == 1u ? c.simulateParticleCount : c.desc.maxNumParticles);
            if (items == 0u) {
                continue;
            }
            const GpuPushConstants push{sys};
            vkCmdPushConstants(cmd, m.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (items + kWorkgroupSize - 1u) / kWorkgroupSize, 1, 1);
        }
        if (pass < 2u) {
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
    }
    // Counters to the host; vertices to vertex input / copies.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    vkEndCommandBuffer(cmd);
    vkResetFences(m.vk, 1, &s.fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(m.queue, 1, &si, s.fence) != VK_SUCCESS) {
        m_error = "vkQueueSubmit";
        return false;
    }
    s.submitted = true;
    return true;
}

void VulkanParticleBackend::readRetirements(std::uint64_t serial, std::uint32_t* out, std::uint32_t slots) {
    Impl& m = *m_impl;
    std::fill_n(out, slots, 0u);
    if (m.frames.empty()) {
        return;
    }
    Impl::Slot& s = m.frames[static_cast<std::size_t>(serial % m.slots)];
    if (!m.waitSlot(s)) {
        m_error = "fence wait";
        return;
    }
    std::memcpy(out, s.counters.mapped, sizeof(std::uint32_t) * std::min(slots, m.config.maxSystems));
}

bool VulkanParticleBackend::waitIdle() { return m_impl->waitAll(); }

bool VulkanParticleBackend::uploadParticles(std::span<const GpuParticle> particles) {
    Impl& m = *m_impl;
    if (m.frames.empty() || particles.size() > m.config.particleCapacity || !m.waitAll()) {
        return false;
    }
    Buffer stage{};
    const std::size_t bytes = particles.size_bytes();
    bool ok = m.makeBuffer(bytes, usageOf({BufferUsage::TransferSrc}), MemoryUsage::CpuToGpu, "relight.particles.upload", stage, m_error);
    if (ok) {
        std::memcpy(stage.mapped, particles.data(), bytes);
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = m.commandPool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(m.vk, &ci, &cmd) == VK_SUCCESS;
        if (ok) {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                        VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(stage.handle), static_cast<VkBuffer>(m.particles.handle), 1, &region);
            barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkEndCommandBuffer(cmd);
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(m.vk, &fi, nullptr, &fence);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            ok = vkQueueSubmit(m.queue, 1, &si, fence) == VK_SUCCESS && vkWaitForFences(m.vk, 1, &fence, VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS;
            vkDestroyFence(m.vk, fence, nullptr);
            vkFreeCommandBuffers(m.vk, m.commandPool, 1, &cmd);
        }
    }
    if (stage.handle != nullptr) {
        m.allocator.destroyBuffer(stage);
    }
    return ok;
}

bool VulkanParticleBackend::readback(std::vector<GpuParticle>& particles, std::vector<GpuParticleVertex>& vertices) {
    Impl& m = *m_impl;
    if (m.frames.empty() || !m.waitAll()) {
        return false;
    }
    Buffer pStage{}, vStage{};
    const BufferUsage dst = usageOf({BufferUsage::TransferDst});
    const std::size_t pBytes = sizeof(GpuParticle) * m.config.particleCapacity;
    const std::size_t vBytes = sizeof(GpuParticleVertex) * m.config.vertexCapacity;
    bool ok = m.makeBuffer(pBytes, dst, MemoryUsage::GpuToCpu, "relight.particles.readback_particles", pStage, m_error) &&
              m.makeBuffer(vBytes, dst, MemoryUsage::GpuToCpu, "relight.particles.readback_vertices", vStage, m_error);
    if (ok) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = m.commandPool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(m.vk, &ci, &cmd) == VK_SUCCESS;
        if (ok) {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
            const VkBufferCopy pc{0, 0, pBytes};
            const VkBufferCopy vc{0, 0, vBytes};
            vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(m.particles.handle), static_cast<VkBuffer>(pStage.handle), 1, &pc);
            vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(m.vertices.handle), static_cast<VkBuffer>(vStage.handle), 1, &vc);
            barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
            vkEndCommandBuffer(cmd);
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(m.vk, &fi, nullptr, &fence);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            ok = vkQueueSubmit(m.queue, 1, &si, fence) == VK_SUCCESS &&
                 vkWaitForFences(m.vk, 1, &fence, VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS;
            vkDestroyFence(m.vk, fence, nullptr);
            vkFreeCommandBuffers(m.vk, m.commandPool, 1, &cmd);
        }
    }
    if (ok) {
        particles.resize(m.config.particleCapacity);
        vertices.resize(m.config.vertexCapacity);
        std::memcpy(particles.data(), pStage.mapped, pBytes);
        std::memcpy(vertices.data(), vStage.mapped, vBytes);
    }
    for (Buffer* b : {&pStage, &vStage}) {
        if (b->handle != nullptr) {
            m.allocator.destroyBuffer(*b);
        }
    }
    return ok;
}

} // namespace fuse::relight::particles
