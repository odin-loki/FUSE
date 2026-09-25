#include <fuse/renderer/compute/gpu_radix_sort.hpp>

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kBins = 1u << GpuRadixSort::kDigitBits;
constexpr u32 kMaxScanLevels = 8;

/// Must match `RadixPush` in shaders/compute/radix_sort_common.glsl.
struct RadixPush {
    u32 count = 0;
    u32 shift = 0;
    u32 numBlocks = 0;
    u32 dataOffset = 0;
    u32 sumsOffset = 0;
    u32 dataLen = 0;
};

/// Scratch layout: level 0 = digit-major tile histograms (256 * numBlocks), level i+1 = chunk
/// totals of level i, until a level fits in one scan chunk; one trailing dummy slot takes the
/// top level's (unused) total.
struct ScanPlan {
    u32 numBlocks = 0;
    u32 levels = 0;
    u32 offset[kMaxScanLevels] = {};
    u32 len[kMaxScanLevels] = {};
    u32 dummy = 0;
    u64 totalUints = 0;
};

u32 divUp(u64 a, u64 b) {
    return static_cast<u32>((a + b - 1u) / b);
}

ScanPlan makeScanPlan(u32 count) {
    ScanPlan plan;
    plan.numBlocks = std::max(1u, divUp(count, GpuRadixSort::kTileSize));
    u64 offset = 0;
    u64 len = static_cast<u64>(kBins) * plan.numBlocks;
    while (plan.levels < kMaxScanLevels) {
        plan.offset[plan.levels] = static_cast<u32>(offset);
        plan.len[plan.levels] = static_cast<u32>(len);
        ++plan.levels;
        offset += len;
        const u64 chunks = divUp(len, GpuRadixSort::kScanChunk);
        if (chunks <= 1u) {
            break;
        }
        len = chunks;
    }
    plan.dummy = static_cast<u32>(offset);
    plan.totalUints = offset + 1u;
    return plan;
}

[[maybe_unused]] u64 keyBytes(RadixSortKeyType type) {
    return type == RadixSortKeyType::U64 ? 8u : 4u;
}

} // namespace

// ---------------------------------------------------------------------------------------------

u64 GpuRadixSort::scratchBytes(u32 count) {
    return makeScanPlan(count).totalUints * sizeof(u32);
}

#if defined(FUSE_VULKAN_BACKEND)

namespace {

enum PipelineIndex : u32 {
    kHistogram = 0,
    kScan = 1,
    kScanAdd = 2,
    kScatter = 3,
    kHistogram64 = 4,
    kScatter64 = 5,
};

constexpr const char* kShaderFiles[6] = {
    "radix_histogram.comp.spv", "radix_scan.comp.spv",          "radix_scan_add.comp.spv",
    "radix_scatter.comp.spv",   "radix_histogram_k64.comp.spv", "radix_scatter_k64.comp.spv",
};

/// Per-pass payload of the render graph passes a sort is made of (sized before passes are added,
/// so the user pointers stay valid).
struct RadixPassData {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    RadixPush push{};
    u32 groups = 0;
    VkBuffer copySrc[2] = {};
    VkBuffer copyDst[2] = {};
    VkBufferCopy regions[2] = {};
    u32 copyCount = 0;
    VkQueryPool queries = VK_NULL_HANDLE;
    u32 query = 0;
    VkPipelineStageFlagBits timestampStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
};

void radixDispatchPass(const rg::PassContext& context, void* user) {
    const auto* data = static_cast<const RadixPassData*>(user);
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, data->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, data->layout, 0, 1, &data->set, 0, nullptr);
    vkCmdPushConstants(cmd, data->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RadixPush), &data->push);
    vkCmdDispatch(cmd, data->groups, 1, 1);
}

void radixCopyPass(const rg::PassContext& context, void* user) {
    const auto* data = static_cast<const RadixPassData*>(user);
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    for (u32 i = 0; i < data->copyCount; ++i) {
        vkCmdCopyBuffer(cmd, data->copySrc[i], data->copyDst[i], 1, &data->regions[i]);
    }
}

void radixTimestampPass(const rg::PassContext& context, void* user) {
    const auto* data = static_cast<const RadixPassData*>(user);
    vkCmdWriteTimestamp(static_cast<VkCommandBuffer>(context.commandBuffer), data->timestampStage, data->queries,
                        data->query);
}

/// Graph buffers of one sort.
struct SortRefs {
    rg::BufferRef keys, values, keysTemp, valuesTemp, scratch;
};

constexpr u8 kCompute = rg::kStageCompute;

} // namespace

/// Render graph state: one reusable graph + inline executor (steady-state sorts do not allocate).
struct GpuRadixSort::GraphState {
    std::unique_ptr<rg::Executor> executor;
    rg::Graph graph;
    std::vector<RadixPassData> passes;
    u32 lastPassCount = 0;
    u32 lastBarrierCalls = 0;
};

/// Blocking host path: buffers grow on demand and are reused across calls.
struct GpuRadixSort::HostState {
    std::unique_ptr<GpuAllocator> ownedAllocator;
    GpuAllocator* allocator = nullptr;
    Buffer keys, values, keysTemp, valuesTemp, scratch, staging;
    u32 capacity = 0;
    std::unique_ptr<GpuRadixSortBinding> binding32;
    std::unique_ptr<GpuRadixSortBinding> binding64;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
};

GpuRadixSortBinding::~GpuRadixSortBinding() {
    if (m_pool != nullptr && m_device != nullptr) {
        vkDestroyDescriptorPool(static_cast<VkDevice>(m_device), static_cast<VkDescriptorPool>(m_pool), nullptr);
    }
}

std::unique_ptr<GpuRadixSort> GpuRadixSort::create(VulkanDevice& device, const GpuRadixSortDesc& desc) {
    auto sorter = std::unique_ptr<GpuRadixSort>(new GpuRadixSort());
    if (!sorter->initialize(device, desc)) {
        sorter->shutdown();
        sorter->m_valid = false;
    }
    return sorter;
}

GpuRadixSort::~GpuRadixSort() {
    shutdown();
}

bool GpuRadixSort::initialize(VulkanDevice& device, const GpuRadixSortDesc& desc) {
    m_device = &device;
    if (!device.isValid()) {
        m_message = "Vulkan device unavailable";
        return false;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    const char* debugName = desc.debugName != nullptr ? desc.debugName : "fuse.radix_sort";

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(device.nativePhysicalDevice()), &props);
    m_maxWorkgroupsX = props.limits.maxComputeWorkGroupCount[0];
    m_maxStorageBufferRange = props.limits.maxStorageBufferRange;
    m_timestampPeriodNs = props.limits.timestampPeriod;
    if (props.limits.maxComputeWorkGroupInvocations < kWorkgroupSize ||
        props.limits.maxComputeWorkGroupSize[0] < kWorkgroupSize ||
        props.limits.maxComputeSharedMemorySize < 16u * 1024u) {
        m_message = "device compute limits below radix sort needs (256 invocations, 16 KiB shared)";
        return false;
    }
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(static_cast<VkPhysicalDevice>(device.nativePhysicalDevice()),
                                             &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(static_cast<VkPhysicalDevice>(device.nativePhysicalDevice()),
                                             &familyCount, families.data());
    const u32 computeFamily = device.queues().computeFamily;
    m_timestamps = computeFamily < familyCount && families[computeFamily].timestampValidBits > 0 &&
                   props.limits.timestampPeriod > 0.f;

    std::string shaderDir;
    if (desc.shaderDir != nullptr) {
        shaderDir = desc.shaderDir;
    } else {
#if defined(FUSE_RADIX_SORT_SHADER_DIR)
        shaderDir = FUSE_RADIX_SORT_SHADER_DIR;
#else
        m_message = "no radix sort shader directory (shaders not built: glslangValidator missing)";
        return false;
#endif
    }

    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = static_cast<u32>(bindings.size());
    setInfo.pBindings = bindings.data();
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(vkDevice, &setInfo, nullptr, &setLayout) != VK_SUCCESS) {
        m_message = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    m_setLayout = setLayout;
    nameVkObject(vkDevice, vk_object_type::kDescriptorSetLayout, m_setLayout, debugName);

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(RadixPush);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(vkDevice, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        m_message = "vkCreatePipelineLayout failed";
        return false;
    }
    m_pipelineLayout = pipelineLayout;
    nameVkObject(vkDevice, vk_object_type::kPipelineLayout, m_pipelineLayout, debugName);

    for (u32 i = 0; i < 6; ++i) {
        const std::string path = shaderDir + "/" + kShaderFiles[i];
        auto module = ShaderModule::createFromFile(device, ShaderStage::Compute, path.c_str());
        if (module == nullptr || !module->isValid()) {
            m_message = "failed to load " + path;
            return false;
        }
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = static_cast<VkShaderModule>(module->nativeHandle());
        info.stage.pName = "main";
        info.layout = pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            m_message = "vkCreateComputePipelines failed for " + path;
            return false;
        }
        m_pipelines[i] = pipeline;
        const std::string name = std::string(debugName) + "." + kShaderFiles[i];
        nameVkObject(vkDevice, vk_object_type::kPipeline, pipeline, name.c_str());
    }

    m_host = std::make_unique<HostState>();
    if (desc.allocator != nullptr) {
        m_host->allocator = desc.allocator;
    } else {
        m_host->ownedAllocator = GpuAllocator::create(device);
        m_host->allocator = m_host->ownedAllocator.get();
    }
    if (m_host->allocator == nullptr || !m_host->allocator->isValid()) {
        m_message = "GpuAllocator unavailable";
        return false;
    }

    m_graph = std::make_unique<GraphState>();
    rg::ExecutorDesc executorDesc{};
    executorDesc.framesInFlight = 1;
    executorDesc.enableAsyncCompute = false;
    executorDesc.enableTransferQueue = false;
    executorDesc.name = "fuse.radix_sort.rg";
    m_graph->executor = rg::Executor::create(device, m_host->allocator, executorDesc);
    if (m_graph->executor == nullptr || !m_graph->executor->isValid()) {
        m_message = "render graph executor unavailable";
        return false;
    }
    m_graph->passes.reserve(64);

    m_valid = true;
    m_message = "ok";
    return true;
}

void GpuRadixSort::releaseHostBuffers() {
    if (m_host == nullptr || m_host->allocator == nullptr) {
        return;
    }
    m_host->binding32.reset();
    m_host->binding64.reset();
    for (Buffer* buffer : {&m_host->keys, &m_host->values, &m_host->keysTemp, &m_host->valuesTemp,
                           &m_host->scratch, &m_host->staging}) {
        if (buffer->handle != nullptr) {
            m_host->allocator->destroyBuffer(*buffer);
        }
        *buffer = Buffer{};
    }
    m_host->capacity = 0;
}

void GpuRadixSort::shutdown() {
    if (m_device == nullptr || !m_device->isValid()) {
        m_host.reset();
        return;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    if (m_host != nullptr) {
        if (m_host->fence != VK_NULL_HANDLE) {
            vkWaitForFences(vkDevice, 1, &m_host->fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vkDevice, m_host->fence, nullptr);
        }
        if (m_host->queries != VK_NULL_HANDLE) {
            vkDestroyQueryPool(vkDevice, m_host->queries, nullptr);
        }
        if (m_host->pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vkDevice, m_host->pool, nullptr);
        }
        releaseHostBuffers();
        m_host.reset();
    }
    m_graph.reset();
    for (void*& pipeline : m_pipelines) {
        if (pipeline != nullptr) {
            vkDestroyPipeline(vkDevice, static_cast<VkPipeline>(pipeline), nullptr);
            pipeline = nullptr;
        }
    }
    if (m_pipelineLayout != nullptr) {
        vkDestroyPipelineLayout(vkDevice, static_cast<VkPipelineLayout>(m_pipelineLayout), nullptr);
        m_pipelineLayout = nullptr;
    }
    if (m_setLayout != nullptr) {
        vkDestroyDescriptorSetLayout(vkDevice, static_cast<VkDescriptorSetLayout>(m_setLayout), nullptr);
        m_setLayout = nullptr;
    }
}

u32 GpuRadixSort::maxCount(RadixSortKeyType keyType) const {
    const u64 byTiles = static_cast<u64>(m_maxWorkgroupsX) * kTileSize;
    const u64 byRange = m_maxStorageBufferRange / keyBytes(keyType);
    // Scratch (≈ count / 16 uints) and scan offsets stay well inside 32 bits below this.
    const u64 limit = std::min<u64>({byTiles, byRange, 0x7FFFFFFFull});
    return static_cast<u32>(limit);
}

std::unique_ptr<GpuRadixSortBinding> GpuRadixSort::bind(const GpuRadixSortBuffers& buffers, u32 capacity,
                                                        RadixSortKeyType keyType) const {
    auto binding = std::unique_ptr<GpuRadixSortBinding>(new GpuRadixSortBinding());
    if (!m_valid || buffers.keys == nullptr || buffers.values == nullptr || buffers.keysTemp == nullptr ||
        buffers.valuesTemp == nullptr || buffers.scratch == nullptr || capacity > maxCount(keyType)) {
        return binding;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    binding->m_device = vkDevice;
    binding->m_buffers = buffers;
    binding->m_capacity = capacity;
    binding->m_keyType = keyType;

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return binding;
    }
    binding->m_pool = pool;
    nameVkObject(vkDevice, vk_object_type::kDescriptorPool, pool, "fuse.radix_sort.binding");

    const VkDescriptorSetLayout layouts[2] = {static_cast<VkDescriptorSetLayout>(m_setLayout),
                                              static_cast<VkDescriptorSetLayout>(m_setLayout)};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    if (vkAllocateDescriptorSets(vkDevice, &allocInfo, sets) != VK_SUCCESS) {
        return binding;
    }

    // Set 0: keys -> temp (even passes); set 1: temp -> keys (odd passes).
    const void* order[2][4] = {{buffers.keys, buffers.values, buffers.keysTemp, buffers.valuesTemp},
                               {buffers.keysTemp, buffers.valuesTemp, buffers.keys, buffers.values}};
    VkDescriptorBufferInfo infos[2][5] = {};
    VkWriteDescriptorSet writes[10] = {};
    for (u32 s = 0; s < 2; ++s) {
        for (u32 b = 0; b < 5; ++b) {
            infos[s][b].buffer = static_cast<VkBuffer>(const_cast<void*>(b < 4 ? order[s][b] : buffers.scratch));
            infos[s][b].offset = 0;
            infos[s][b].range = VK_WHOLE_SIZE;
            VkWriteDescriptorSet& write = writes[s * 5 + b];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = sets[s];
            write.dstBinding = b;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &infos[s][b];
        }
        binding->m_sets[s] = sets[s];
    }
    vkUpdateDescriptorSets(vkDevice, 10, writes, 0, nullptr);
    binding->m_valid = true;
    return binding;
}

namespace {

/// Pass-data entries appendSortPasses() fills for `passes` digits over `plan`.
u32 sortPassDataCount(u32 passes, const ScanPlan& plan) {
    return passes * (2u + plan.levels + (plan.levels - 1u)) + ((passes & 1u) != 0u ? 1u : 0u);
}

} // namespace

u32 GpuRadixSort::lastGraphPassCount() const {
    return m_graph != nullptr ? m_graph->lastPassCount : 0u;
}

u32 GpuRadixSort::lastGraphBarrierCalls() const {
    return m_graph != nullptr ? m_graph->lastBarrierCalls : 0u;
}

bool GpuRadixSort::recordSort(void* commandBuffer, const GpuRadixSortBinding& binding, u32 count, u32 keyBits,
                              GpuRadixSortStats* stats) const {
    const u32 maxBits = binding.keyType() == RadixSortKeyType::U64 ? 64u : 32u;
    if (!m_valid || commandBuffer == nullptr || !binding.isValid() || count > binding.capacity() ||
        keyBits > maxBits) {
        return false;
    }
    const u32 passes = count > 1u ? passCount(keyBits) : 0u;
    if (stats != nullptr) {
        stats->count = count;
        stats->passes = passes;
        stats->dispatches = 0;
        stats->scanLevels = 0;
    }
    if (passes == 0u) {
        return true;
    }

    GraphState& state = *m_graph;
    rg::Graph& graph = state.graph;
    graph.reset();
    const GpuRadixSortBuffers& b = binding.buffers();
    const u64 capacity = binding.capacity();
    const u64 kb = keyBytes(binding.keyType());
    SortRefs refs;
    refs.keys = graph.importBuffer({b.keys, capacity * kb, rg::kNoQueue, nullptr, "radix.keys"});
    refs.values = graph.importBuffer({b.values, capacity * 4u, rg::kNoQueue, nullptr, "radix.values"});
    refs.keysTemp = graph.importBuffer({b.keysTemp, capacity * kb, rg::kNoQueue, nullptr, "radix.keys_temp"});
    refs.valuesTemp = graph.importBuffer({b.valuesTemp, capacity * 4u, rg::kNoQueue, nullptr, "radix.values_temp"});
    refs.scratch = graph.importBuffer({b.scratch, scratchBytes(binding.capacity()), rg::kNoQueue, nullptr,
                                       "radix.scratch"});
    const ScanPlan plan = makeScanPlan(count);
    state.passes.clear();
    state.passes.resize(sortPassDataCount(passes, plan));
    const u32 dispatches = appendSortPasses(&graph, state.passes.data(), &binding, &refs, count, passes);

    const rg::ExecuteResult result =
        state.executor->recordInline(graph, commandBuffer, rg::QueueClass::AsyncCompute);
    state.lastPassCount = graph.stats().executedPasses;
    state.lastBarrierCalls = result.barrierCalls;
    if (!result.ok) {
        return false;
    }
    if (stats != nullptr) {
        stats->dispatches = dispatches;
        stats->scanLevels = plan.levels;
    }
    return true;
}

u32 GpuRadixSort::appendSortPasses(void* graphPtr, void* passDataPtr, const GpuRadixSortBinding* bindingPtr,
                                   const void* refsPtr, u32 count, u32 passes) const {
    rg::Graph& graph = *static_cast<rg::Graph*>(graphPtr);
    RadixPassData* data = static_cast<RadixPassData*>(passDataPtr);
    const GpuRadixSortBinding& binding = *bindingPtr;
    const SortRefs& refs = *static_cast<const SortRefs*>(refsPtr);
    const VkPipelineLayout layout = static_cast<VkPipelineLayout>(m_pipelineLayout);
    const bool k64 = binding.keyType() == RadixSortKeyType::U64;
    const VkPipeline histogram = static_cast<VkPipeline>(m_pipelines[k64 ? kHistogram64 : kHistogram]);
    const VkPipeline scatter = static_cast<VkPipeline>(m_pipelines[k64 ? kScatter64 : kScatter]);
    const VkPipeline scan = static_cast<VkPipeline>(m_pipelines[kScan]);
    const VkPipeline scanAdd = static_cast<VkPipeline>(m_pipelines[kScanAdd]);
    const ScanPlan plan = makeScanPlan(count);
    u32 next = 0;
    u32 dispatches = 0;

    auto dispatchData = [&](VkPipeline pipeline, VkDescriptorSet set, const RadixPush& push, u32 groups) {
        RadixPassData& pass = data[next++];
        pass.pipeline = pipeline;
        pass.layout = layout;
        pass.set = set;
        pass.push = push;
        pass.groups = groups;
        ++dispatches;
        return &pass;
    };

    for (u32 pass = 0; pass < passes; ++pass) {
        // Even digits: keys -> temp (descriptor set 0); odd digits: temp -> keys (set 1).
        const bool even = (pass & 1u) == 0u;
        const rg::BufferRef inKeys = even ? refs.keys : refs.keysTemp;
        const rg::BufferRef inValues = even ? refs.values : refs.valuesTemp;
        const rg::BufferRef outKeys = even ? refs.keysTemp : refs.keys;
        const rg::BufferRef outValues = even ? refs.valuesTemp : refs.values;
        const VkDescriptorSet set = static_cast<VkDescriptorSet>(binding.m_sets[pass & 1u]);
        RadixPush push{};
        push.count = count;
        push.shift = pass * kDigitBits;
        push.numBlocks = plan.numBlocks;

        graph.addPass("radix.histogram", radixDispatchPass, dispatchData(histogram, set, push, plan.numBlocks),
                      rg::QueueClass::AsyncCompute)
            .use(inKeys, rg::Access::StorageRead, {}, kCompute)
            .use(refs.scratch, rg::Access::StorageWrite, {}, kCompute);
        // Up-sweep: scan every level in place; each level's chunk totals form the next level.
        for (u32 level = 0; level < plan.levels; ++level) {
            RadixPush scanPush = push;
            scanPush.dataOffset = plan.offset[level];
            scanPush.dataLen = plan.len[level];
            scanPush.sumsOffset = level + 1u < plan.levels ? plan.offset[level + 1u] : plan.dummy;
            graph.addPass("radix.scan", radixDispatchPass,
                          dispatchData(scan, set, scanPush, divUp(plan.len[level], kScanChunk)),
                          rg::QueueClass::AsyncCompute)
                .use(refs.scratch, rg::Access::StorageReadWrite, {}, kCompute);
        }
        // Down-sweep: fold the scanned totals of level i+1 into level i.
        for (u32 level = plan.levels - 1u; level-- > 0u;) {
            RadixPush addPush = push;
            addPush.dataOffset = plan.offset[level];
            addPush.dataLen = plan.len[level];
            addPush.sumsOffset = plan.offset[level + 1u];
            graph.addPass("radix.scan_add", radixDispatchPass,
                          dispatchData(scanAdd, set, addPush, divUp(plan.len[level], kScanChunk)),
                          rg::QueueClass::AsyncCompute)
                .use(refs.scratch, rg::Access::StorageReadWrite, {}, kCompute);
        }
        graph.addPass("radix.scatter", radixDispatchPass, dispatchData(scatter, set, push, plan.numBlocks),
                      rg::QueueClass::AsyncCompute)
            .use(inKeys, rg::Access::StorageRead, {}, kCompute)
            .use(inValues, rg::Access::StorageRead, {}, kCompute)
            .use(refs.scratch, rg::Access::StorageRead, {}, kCompute)
            .use(outKeys, rg::Access::StorageWrite, {}, kCompute)
            .use(outValues, rg::Access::StorageWrite, {}, kCompute);
    }

    if ((passes & 1u) != 0u) {
        // Odd pass count: the result sits in the temp buffers.
        RadixPassData& copy = data[next++];
        const GpuRadixSortBuffers& b = binding.buffers();
        const VkDeviceSize keyBytesCount = static_cast<VkDeviceSize>(count) * keyBytes(binding.keyType());
        const VkDeviceSize valueBytesCount = static_cast<VkDeviceSize>(count) * sizeof(u32);
        copy.copyCount = 2;
        copy.copySrc[0] = static_cast<VkBuffer>(b.keysTemp);
        copy.copyDst[0] = static_cast<VkBuffer>(b.keys);
        copy.regions[0] = {0, 0, keyBytesCount};
        copy.copySrc[1] = static_cast<VkBuffer>(b.valuesTemp);
        copy.copyDst[1] = static_cast<VkBuffer>(b.values);
        copy.regions[1] = {0, 0, valueBytesCount};
        graph.addPass("radix.copy_back", radixCopyPass, &copy, rg::QueueClass::AsyncCompute)
            .use(refs.keysTemp, rg::Access::TransferSrc, {0, keyBytesCount})
            .use(refs.valuesTemp, rg::Access::TransferSrc, {0, valueBytesCount})
            .use(refs.keys, rg::Access::TransferDst, {0, keyBytesCount})
            .use(refs.values, rg::Access::TransferDst, {0, valueBytesCount});
    }
    return dispatches;
}

bool GpuRadixSort::ensureHostBuffers(u32 count, RadixSortKeyType keyType) {
    HostState& host = *m_host;
    const VkDevice vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    if (host.pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_device->queues().computeFamily;
        if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &host.pool) != VK_SUCCESS) {
            return false;
        }
        nameVkObject(vkDevice, vk_object_type::kCommandPool, host.pool, "fuse.radix_sort.pool");
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = host.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &host.cmd) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &host.fence) != VK_SUCCESS) {
            return false;
        }
        if (m_timestamps) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 2;
            if (vkCreateQueryPool(vkDevice, &queryInfo, nullptr, &host.queries) != VK_SUCCESS) {
                host.queries = VK_NULL_HANDLE;
            }
        }
    }

    if (count > host.capacity) {
        releaseHostBuffers();
        const u32 capacity = std::max(count, 1024u);
        const auto usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                    static_cast<u32>(BufferUsage::TransferSrc) |
                                                    static_cast<u32>(BufferUsage::TransferDst));
        // Keys are sized for u64 so one buffer set serves both key types.
        const struct {
            Buffer* buffer;
            u64 size;
            MemoryUsage memory;
            const char* name;
        } plan[] = {
            {&host.keys, u64{capacity} * 8u, MemoryUsage::GpuOnly, "fuse.radix_sort.keys"},
            {&host.values, u64{capacity} * 4u, MemoryUsage::GpuOnly, "fuse.radix_sort.values"},
            {&host.keysTemp, u64{capacity} * 8u, MemoryUsage::GpuOnly, "fuse.radix_sort.keys_temp"},
            {&host.valuesTemp, u64{capacity} * 4u, MemoryUsage::GpuOnly, "fuse.radix_sort.values_temp"},
            {&host.scratch, scratchBytes(capacity), MemoryUsage::GpuOnly, "fuse.radix_sort.scratch"},
            {&host.staging, u64{capacity} * 12u, MemoryUsage::GpuToCpu, "fuse.radix_sort.staging"},
        };
        for (const auto& entry : plan) {
            BufferDesc desc{};
            desc.size = static_cast<usize>(entry.size);
            desc.usage = usage;
            desc.memoryUsage = entry.memory;
            desc.name = entry.name;
            if (!host.allocator->createBuffer(desc, *entry.buffer) || entry.buffer->handle == nullptr) {
                releaseHostBuffers();
                return false;
            }
        }
        if (host.staging.mapped == nullptr) {
            releaseHostBuffers();
            return false;
        }
        host.capacity = capacity;
    }
    std::unique_ptr<GpuRadixSortBinding>& binding =
        keyType == RadixSortKeyType::U64 ? host.binding64 : host.binding32;
    if (binding == nullptr) {
        GpuRadixSortBuffers buffers{};
        buffers.keys = host.keys.handle;
        buffers.values = host.values.handle;
        buffers.keysTemp = host.keysTemp.handle;
        buffers.valuesTemp = host.valuesTemp.handle;
        buffers.scratch = host.scratch.handle;
        binding = bind(buffers, host.capacity, keyType);
    }
    return binding != nullptr && binding->isValid();
}

bool GpuRadixSort::sortHost(void* keys, u32* values, u32 count, u32 keyBits, RadixSortKeyType keyType,
                            GpuRadixSortStats* stats) {
    const auto start = std::chrono::steady_clock::now();
    const u32 maxBits = keyType == RadixSortKeyType::U64 ? 64u : 32u;
    // Host buffers hold keys as u64 whichever the key type, so both share the u64 range limit.
    if (!m_valid || keyBits > maxBits || count > maxCount(RadixSortKeyType::U64) ||
        (count > 0u && (keys == nullptr || values == nullptr))) {
        return false;
    }
    if (stats != nullptr) {
        *stats = GpuRadixSortStats{};
        stats->count = count;
    }
    if (count <= 1u || keyBits == 0u) {
        return true; // already sorted
    }
    if (!ensureHostBuffers(count, keyType)) {
        m_message = "radix sort host buffers unavailable";
        return false;
    }
    HostState& host = *m_host;
    const VkDevice vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    const u64 keyBytesTotal = u64{count} * keyBytes(keyType);
    const u64 valueBytesTotal = u64{count} * sizeof(u32);
    u8* staging = static_cast<u8*>(host.staging.mapped);
    std::memcpy(staging, keys, static_cast<usize>(keyBytesTotal));
    std::memcpy(staging + keyBytesTotal, values, static_cast<usize>(valueBytesTotal));

    const VkCommandBuffer cmd = host.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    if (host.queries != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, host.queries, 0, 2);
    }

    // upload -> [timestamp] -> sort passes -> [timestamp] -> readback -> host read, one graph: every
    // barrier (transfer -> compute, compute -> transfer, transfer -> host, the staging WAR) is
    // derived from the declared accesses.
    const GpuRadixSortBinding& binding = keyType == RadixSortKeyType::U64 ? *host.binding64 : *host.binding32;
    const u32 passes = passCount(keyBits);
    const ScanPlan plan = makeScanPlan(count);
    GraphState& state = *m_graph;
    rg::Graph& graph = state.graph;
    graph.reset();
    const u64 capacity = host.capacity;
    const rg::BufferRef stagingRef =
        graph.importBuffer({host.staging.handle, capacity * 12u, rg::kNoQueue, nullptr, "radix.staging"});
    SortRefs refs;
    refs.keys = graph.importBuffer({host.keys.handle, capacity * 8u, rg::kNoQueue, nullptr, "radix.keys"});
    refs.values = graph.importBuffer({host.values.handle, capacity * 4u, rg::kNoQueue, nullptr, "radix.values"});
    refs.keysTemp = graph.importBuffer({host.keysTemp.handle, capacity * 8u, rg::kNoQueue, nullptr, "radix.keys_temp"});
    refs.valuesTemp =
        graph.importBuffer({host.valuesTemp.handle, capacity * 4u, rg::kNoQueue, nullptr, "radix.values_temp"});
    refs.scratch =
        graph.importBuffer({host.scratch.handle, scratchBytes(host.capacity), rg::kNoQueue, nullptr, "radix.scratch"});
    const u32 extra = 4u; // upload, 2 timestamps, readback
    state.passes.clear();
    state.passes.resize(sortPassDataCount(passes, plan) + extra);
    RadixPassData& upload = state.passes[0];
    RadixPassData& tsBegin = state.passes[1];
    RadixPassData& tsEnd = state.passes[2];
    RadixPassData& readback = state.passes[3];

    const VkBuffer stagingBuffer = static_cast<VkBuffer>(host.staging.handle);
    const VkBuffer keyBuffer = static_cast<VkBuffer>(host.keys.handle);
    const VkBuffer valueBuffer = static_cast<VkBuffer>(host.values.handle);
    // Host writes to the mapped staging buffer are made available by vkQueueSubmit.
    upload.copyCount = 2;
    upload.copySrc[0] = stagingBuffer;
    upload.copyDst[0] = keyBuffer;
    upload.regions[0] = {0, 0, keyBytesTotal};
    upload.copySrc[1] = stagingBuffer;
    upload.copyDst[1] = valueBuffer;
    upload.regions[1] = {keyBytesTotal, 0, valueBytesTotal};
    graph.addPass("radix.upload", radixCopyPass, &upload, rg::QueueClass::AsyncCompute)
        .use(stagingRef, rg::Access::TransferSrc, {0, keyBytesTotal + valueBytesTotal})
        .use(refs.keys, rg::Access::TransferDst, {0, keyBytesTotal})
        .use(refs.values, rg::Access::TransferDst, {0, valueBytesTotal});
    if (host.queries != VK_NULL_HANDLE) {
        tsBegin.queries = host.queries;
        tsBegin.query = 0;
        tsBegin.timestampStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        graph.addPass("radix.timestamp_begin", radixTimestampPass, &tsBegin, rg::QueueClass::AsyncCompute).neverCull();
    }
    GpuRadixSortStats recordStats{};
    recordStats.count = count;
    recordStats.passes = passes;
    recordStats.scanLevels = plan.levels;
    recordStats.dispatches = appendSortPasses(&graph, state.passes.data() + extra, &binding, &refs, count, passes);
    if (host.queries != VK_NULL_HANDLE) {
        tsEnd.queries = host.queries;
        tsEnd.query = 1;
        tsEnd.timestampStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        graph.addPass("radix.timestamp_end", radixTimestampPass, &tsEnd, rg::QueueClass::AsyncCompute).neverCull();
    }
    readback.copyCount = 2;
    readback.copySrc[0] = keyBuffer;
    readback.copyDst[0] = stagingBuffer;
    readback.regions[0] = {0, 0, keyBytesTotal};
    readback.copySrc[1] = valueBuffer;
    readback.copyDst[1] = stagingBuffer;
    readback.regions[1] = {0, keyBytesTotal, valueBytesTotal};
    graph.addPass("radix.readback", radixCopyPass, &readback, rg::QueueClass::AsyncCompute)
        .use(refs.keys, rg::Access::TransferSrc, {0, keyBytesTotal})
        .use(refs.values, rg::Access::TransferSrc, {0, valueBytesTotal})
        .use(stagingRef, rg::Access::TransferDst, {0, keyBytesTotal + valueBytesTotal});
    graph.addPass("radix.host_read", nullptr, nullptr, rg::QueueClass::AsyncCompute)
        .use(stagingRef, rg::Access::HostRead, {0, keyBytesTotal + valueBytesTotal});
    const rg::ExecuteResult recorded = state.executor->recordInline(graph, cmd, rg::QueueClass::AsyncCompute);
    state.lastPassCount = graph.stats().executedPasses;
    state.lastBarrierCalls = recorded.barrierCalls;
    vkEndCommandBuffer(cmd);
    if (!recorded.ok) {
        m_message = "radix sort graph failed: " + state.executor->message();
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkResetFences(vkDevice, 1, &host.fence);
    if (vkQueueSubmit(static_cast<VkQueue>(m_device->queues().compute), 1, &submit, host.fence) != VK_SUCCESS ||
        vkWaitForFences(vkDevice, 1, &host.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        m_message = "radix sort submit/wait failed";
        return false;
    }

    std::memcpy(keys, staging, static_cast<usize>(keyBytesTotal));
    std::memcpy(values, staging + keyBytesTotal, static_cast<usize>(valueBytesTotal));
    if (stats != nullptr) {
        *stats = recordStats;
        if (host.queries != VK_NULL_HANDLE) {
            u64 ticks[2] = {};
            if (vkGetQueryPoolResults(vkDevice, host.queries, 0, 2, sizeof(ticks), ticks, sizeof(u64),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                stats->gpuMs = static_cast<f64>(ticks[1] - ticks[0]) * m_timestampPeriodNs * 1e-6;
            }
        }
        stats->totalMs =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return true;
}

#else // !FUSE_VULKAN_BACKEND

struct GpuRadixSort::HostState {};
struct GpuRadixSort::GraphState {};

u32 GpuRadixSort::lastGraphPassCount() const {
    return 0;
}

u32 GpuRadixSort::lastGraphBarrierCalls() const {
    return 0;
}

u32 GpuRadixSort::appendSortPasses(void*, void*, const GpuRadixSortBinding*, const void*, u32, u32) const {
    return 0;
}

GpuRadixSortBinding::~GpuRadixSortBinding() = default;

std::unique_ptr<GpuRadixSort> GpuRadixSort::create(VulkanDevice& device, const GpuRadixSortDesc& desc) {
    auto sorter = std::unique_ptr<GpuRadixSort>(new GpuRadixSort());
    sorter->initialize(device, desc);
    return sorter;
}

GpuRadixSort::~GpuRadixSort() = default;

bool GpuRadixSort::initialize(VulkanDevice& device, const GpuRadixSortDesc& desc) {
    (void)desc;
    m_device = &device;
    m_message = "GPU radix sort requires the Vulkan backend";
    return false;
}

void GpuRadixSort::shutdown() {}
void GpuRadixSort::releaseHostBuffers() {}
bool GpuRadixSort::ensureHostBuffers(u32, RadixSortKeyType) {
    return false;
}

u32 GpuRadixSort::maxCount(RadixSortKeyType) const {
    return 0;
}

std::unique_ptr<GpuRadixSortBinding> GpuRadixSort::bind(const GpuRadixSortBuffers&, u32, RadixSortKeyType) const {
    return std::unique_ptr<GpuRadixSortBinding>(new GpuRadixSortBinding());
}

bool GpuRadixSort::recordSort(void*, const GpuRadixSortBinding&, u32, u32, GpuRadixSortStats*) const {
    return false;
}

bool GpuRadixSort::sortHost(void*, u32*, u32, u32, RadixSortKeyType, GpuRadixSortStats*) {
    return false;
}

#endif

bool GpuRadixSort::sort(u32* keys, u32* values, u32 count, u32 keyBits, GpuRadixSortStats* stats) {
    return sortHost(keys, values, count, keyBits, RadixSortKeyType::U32, stats);
}

bool GpuRadixSort::sort64(u64* keys, u32* values, u32 count, u32 keyBits, GpuRadixSortStats* stats) {
    return sortHost(keys, values, count, keyBits, RadixSortKeyType::U64, stats);
}

} // namespace fuse::renderer
