// WP-9.3 device-generated commands: see include/fuse/renderer/vk/dgc/dgc_pipeline_selector.hpp.
#include <fuse/renderer/vk/dgc/dgc_pipeline_selector.hpp>

#include <fuse/renderer/culling/cull_types.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>

#include "dgc_spv.h"
#include "dgc_vk_ext.hpp"

#if defined(VK_EXT_device_generated_commands)
// Newer headers: the mirror must match the registry byte for byte.
static_assert(sizeof(fuse::renderer::dgc::vkx::GeneratedCommandsInfo) == sizeof(VkGeneratedCommandsInfoEXT), "mirror");
static_assert(sizeof(fuse::renderer::dgc::vkx::IndirectCommandsLayoutToken) == sizeof(VkIndirectCommandsLayoutTokenEXT),
              "mirror");
static_assert(sizeof(fuse::renderer::dgc::vkx::IndirectCommandsLayoutCreateInfo) ==
                  sizeof(VkIndirectCommandsLayoutCreateInfoEXT),
              "mirror");
static_assert(sizeof(fuse::renderer::dgc::vkx::PhysicalDeviceProperties) ==
                  sizeof(VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT),
              "mirror");
static_assert(static_cast<int>(fuse::renderer::dgc::vkx::kSTypeGeneratedCommandsInfo) ==
                  static_cast<int>(VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT),
              "mirror");
#endif
#endif

namespace fuse::renderer::dgc {

DgcCullInput dgc_cull_input(const culling::InstanceCuller& culler, const culling::CullGraphRefs& refs,
                            culling::CullPhase phase) {
    DgcCullInput in{};
    const u64 region = static_cast<u64>(culler.capacity()) * sizeof(culling::DrawIndexedIndirectCommand);
    const bool second = phase == culling::CullPhase::Phase2;
    in.args = refs.args;
    in.counts = refs.counts;
    in.argsBuffer = culler.argsBuffer();
    in.argsOffset = second ? region : 0u;
    in.argsBytes = region;
    in.countsBuffer = culler.countsBuffer();
    in.countOffset = (second ? culling::kCountPhase2Draws : culling::kCountPhase1Draws) * sizeof(u32);
    return in;
}

#if defined(FUSE_VULKAN_BACKEND)
struct DgcPipelineSelector::Ext {
    vkx::Dispatch fn{};
    vkx::PhysicalDeviceProperties props{};
    vkx::IndirectExecutionSet executionSet{};
    vkx::IndirectCommandsLayout layout{};
    VkBuffer preprocess = VK_NULL_HANDLE;
    VkDeviceMemory preprocessMemory = VK_NULL_HANDLE;
    VkDeviceAddress preprocessAddress = 0;
    VkDeviceSize preprocessSize = 0;
    VkDeviceAddress sequencesAddress = 0;
    VkDeviceAddress countAddress = 0;
};

namespace {
VkDevice vkDev(const DgcSelectorDesc& d) { return static_cast<VkDevice>(d.device->nativeHandle()); }

bool hasExtension(const VulkanDevice& device, const char* name) {
    for (const char* e : device.info().enabledExtensions) {
        if (e != nullptr && std::strcmp(e, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Walks a VkDeviceCreateInfo::pNext chain for the DGC feature struct.
bool chainFeature(const void* chain, bool& known) {
    for (const VkBaseInStructure* node = static_cast<const VkBaseInStructure*>(chain); node != nullptr;
         node = node->pNext) {
        if (node->sType == vkx::kSTypeFeatures) {
            known = true;
            return reinterpret_cast<const vkx::PhysicalDeviceFeatures*>(node)->deviceGeneratedCommands == VK_TRUE;
        }
    }
    return false;
}

u64 bufferAddress(VkDevice device, void* buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = static_cast<VkBuffer>(buffer);
    return buffer != nullptr ? vkGetBufferDeviceAddress(device, &info) : 0u;
}
} // namespace
#else
struct DgcPipelineSelector::Ext {};
#endif

DgcPipelineSelector::~DgcPipelineSelector() { destroy(); }

u64 DgcPipelineSelector::pipelineCreateFlags2() const {
#if defined(FUSE_VULKAN_BACKEND)
    return dgcCapable() ? vkx::kPipelineCreate2IndirectBindable : 0u;
#else
    return 0u;
#endif
}

bool DgcPipelineSelector::init(const DgcSelectorDesc& desc) {
    destroy();
    m_desc = desc;
    m_desc.framesInFlight = std::max(1u, m_desc.framesInFlight);
    m_desc.maxDraws = std::max(1u, m_desc.maxDraws);
    m_reason = DgcReason::NoVulkanBackend;
#if defined(FUSE_VULKAN_BACKEND)
    if (desc.bucketCount == 0u || desc.bucketCount > kDgcMaxBuckets) {
        m_reason = DgcReason::BadBucketCount;
        return false;
    }
    m_desc.defaultBucket = std::min(m_desc.defaultBucket, m_desc.bucketCount - 1u);
    if (desc.device == nullptr || !desc.device->isValid() || desc.allocator == nullptr || desc.bindless == nullptr) {
        m_reason = DgcReason::NoDevice;
        return false;
    }
    const RendererCaps& caps = desc.device->info().caps;
    if (!caps.bufferDeviceAddress) {
        m_reason = DgcReason::NoBufferDeviceAddress;
        return false;
    }
    if (!caps.drawIndirectCount) {
        m_reason = DgcReason::NoDrawIndirectCount;
        return false;
    }
    m_ext = new Ext();
    m_initialized = true; // let destroy() release partial state
    const u64 maxDraws = m_desc.maxDraws;
    if (!createPipeline() ||
        !createBuffer(m_sequences, maxDraws * kDgcSequenceStride, true, false, "dgc.sequences") ||
        !createBuffer(m_bucketArgs, maxDraws * m_desc.bucketCount * sizeof(DgcDrawIndexed), true, false, "dgc.bucket_args") ||
        !createBuffer(m_counts, dgc_count_words(m_desc.bucketCount) * sizeof(u32), true, false, "dgc.counts") ||
        !setMaterialBuckets(nullptr, 0u)) {
        destroy();
        m_reason = DgcReason::NoDevice;
        return false;
    }
    m_ext->sequencesAddress = m_sequences.deviceAddress;
    m_ext->countAddress = m_counts.deviceAddress + kDgcCountSequences * sizeof(u32);
    evaluateCapability();
    return true;
#else
    return false;
#endif
}

void DgcPipelineSelector::evaluateCapability() {
#if defined(FUSE_VULKAN_BACKEND)
    DgcCapabilityInputs in{};
    in.vulkanBackend = true;
    in.deviceValid = true;
    in.forceFallback = m_desc.mode == DgcMode::ForceFallback;
    in.bufferDeviceAddress = true;
    in.drawIndirectCount = true;
    in.bucketCount = m_desc.bucketCount;
    in.maxDraws = m_desc.maxDraws;
    const VulkanDevice& device = *m_desc.device;
    in.extensionEnabled = hasExtension(device, vkx::kExtensionName);
    in.maintenance5Enabled = hasExtension(device, vkx::kMaintenance5Name);
    if (device.info().caps.has(RenderFeature::DeviceGeneratedCommands)) {
        in.featureStateKnown = true; // headers define the extension: VulkanDevice enabled it
        in.featureEnabled = true;
    } else if (m_desc.enabledFeatureChain != nullptr) {
        in.featureEnabled = chainFeature(m_desc.enabledFeatureChain, in.featureStateKnown);
    }
    if (in.extensionEnabled && in.maintenance5Enabled && in.featureEnabled) {
        const VkDevice dev = vkDev(m_desc);
        m_ext->fn.getMemoryRequirements = reinterpret_cast<vkx::PfnGetGeneratedCommandsMemoryRequirements>(
            vkGetDeviceProcAddr(dev, "vkGetGeneratedCommandsMemoryRequirementsEXT"));
        m_ext->fn.cmdExecute = reinterpret_cast<vkx::PfnCmdExecuteGeneratedCommands>(
            vkGetDeviceProcAddr(dev, "vkCmdExecuteGeneratedCommandsEXT"));
        m_ext->fn.createLayout = reinterpret_cast<vkx::PfnCreateIndirectCommandsLayout>(
            vkGetDeviceProcAddr(dev, "vkCreateIndirectCommandsLayoutEXT"));
        m_ext->fn.destroyLayout = reinterpret_cast<vkx::PfnDestroyIndirectCommandsLayout>(
            vkGetDeviceProcAddr(dev, "vkDestroyIndirectCommandsLayoutEXT"));
        m_ext->fn.createExecutionSet = reinterpret_cast<vkx::PfnCreateIndirectExecutionSet>(
            vkGetDeviceProcAddr(dev, "vkCreateIndirectExecutionSetEXT"));
        m_ext->fn.destroyExecutionSet = reinterpret_cast<vkx::PfnDestroyIndirectExecutionSet>(
            vkGetDeviceProcAddr(dev, "vkDestroyIndirectExecutionSetEXT"));
        m_ext->fn.updateExecutionSetPipeline = reinterpret_cast<vkx::PfnUpdateIndirectExecutionSetPipeline>(
            vkGetDeviceProcAddr(dev, "vkUpdateIndirectExecutionSetPipelineEXT"));
        in.entryPoints = m_ext->fn.complete();
        m_ext->props = vkx::PhysicalDeviceProperties{};
        m_ext->props.sType = vkx::kSTypeProperties;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &m_ext->props;
        vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(device.nativePhysicalDevice()), &props2);
        in.maxIndirectPipelineCount = m_ext->props.maxIndirectPipelineCount;
        in.maxIndirectSequenceCount = m_ext->props.maxIndirectSequenceCount;
        in.maxIndirectCommandsTokenCount = m_ext->props.maxIndirectCommandsTokenCount;
        in.maxIndirectCommandsTokenOffset = m_ext->props.maxIndirectCommandsTokenOffset;
        in.maxIndirectCommandsIndirectStride = m_ext->props.maxIndirectCommandsIndirectStride;
        in.pipelineBindingStages = m_ext->props.supportedIndirectCommandsShaderStagesPipelineBinding;
    }
    in.requiredStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    m_reason = dgc_evaluate_capability(in);
#endif
}

void DgcPipelineSelector::destroyDgcObjects() {
#if defined(FUSE_VULKAN_BACKEND)
    m_dgcActive = false;
    if (m_ext == nullptr || m_desc.device == nullptr) {
        return;
    }
    const VkDevice dev = vkDev(m_desc);
    if (m_ext->layout != vkx::IndirectCommandsLayout{} && m_ext->fn.destroyLayout != nullptr) {
        m_ext->fn.destroyLayout(dev, m_ext->layout, nullptr);
    }
    if (m_ext->executionSet != vkx::IndirectExecutionSet{} && m_ext->fn.destroyExecutionSet != nullptr) {
        m_ext->fn.destroyExecutionSet(dev, m_ext->executionSet, nullptr);
    }
    m_ext->layout = vkx::IndirectCommandsLayout{};
    m_ext->executionSet = vkx::IndirectExecutionSet{};
    if (m_ext->preprocess != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, m_ext->preprocess, nullptr);
    }
    if (m_ext->preprocessMemory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, m_ext->preprocessMemory, nullptr);
    }
    m_ext->preprocess = VK_NULL_HANDLE;
    m_ext->preprocessMemory = VK_NULL_HANDLE;
    m_ext->preprocessAddress = 0;
    m_ext->preprocessSize = 0;
    m_stats.preprocessBytes = 0;
#endif
}

void DgcPipelineSelector::destroy() {
    if (!m_initialized) {
        return;
    }
#if defined(FUSE_VULKAN_BACKEND)
    destroyDgcObjects();
    const VkDevice dev = vkDev(m_desc);
    for (Buffer* b : {&m_sequences, &m_bucketArgs, &m_counts, &m_materials}) {
        if (b->handle != nullptr) {
            m_desc.allocator->destroyBuffer(*b);
        }
        *b = Buffer{};
    }
    if (m_pipeline != nullptr) {
        vkDestroyPipeline(dev, static_cast<VkPipeline>(m_pipeline), nullptr);
    }
    if (m_layout != nullptr) {
        vkDestroyPipelineLayout(dev, static_cast<VkPipelineLayout>(m_layout), nullptr);
    }
#endif
    delete m_ext;
    m_ext = nullptr;
    m_pipeline = nullptr;
    m_layout = nullptr;
    m_pipelines.clear();
    m_pipelineLayout = nullptr;
    m_materialCapacity = 0;
    m_materialBuckets.clear();
    m_initialized = false;
    m_dgcActive = false;
    m_stats = DgcStats{};
}

bool DgcPipelineSelector::createPipeline() {
#if defined(FUSE_VULKAN_BACKEND)
    const u32* code = nullptr;
    usize bytes = 0;
#if defined(FUSE_DGC_SLANG)
    if (m_desc.language == DgcKernelLanguage::Auto || m_desc.language == DgcKernelLanguage::Slang) {
        code = kDgcGenerateSlangSpv;
        bytes = sizeof(kDgcGenerateSlangSpv);
        m_language = "slang";
    }
#endif
#if defined(FUSE_DGC_GLSL)
    if (code == nullptr && (m_desc.language == DgcKernelLanguage::Auto || m_desc.language == DgcKernelLanguage::Glsl)) {
        code = kDgcGenerateGlslSpv;
        bytes = sizeof(kDgcGenerateGlslSpv);
        m_language = "glsl";
    }
#endif
    if (code == nullptr) {
        return false;
    }
    const VkDevice dev = vkDev(m_desc);
    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(m_desc.bindless->layoutHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DgcPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = setLayout != VK_NULL_HANDLE ? 1u : 0u;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        return false;
    }
    m_layout = layout;
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = bytes;
    moduleInfo.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        return false;
    }
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.flags = static_cast<VkPipelineCreateFlags>(m_desc.bindless->pipelineCreateFlags());
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(dev, module, nullptr);
    m_pipeline = pipeline;
    return result == VK_SUCCESS;
#else
    return false;
#endif
}

bool DgcPipelineSelector::createBuffer(Buffer& out, u64 bytes, bool indirect, bool hostVisible, const char* name) {
    out = Buffer{};
    BufferDesc desc{};
    desc.size = static_cast<usize>(bytes);
    desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::TransferDst) |
                                          static_cast<u32>(BufferUsage::TransferSrc) |
                                          static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                          (indirect ? static_cast<u32>(BufferUsage::Indirect) : 0u));
    desc.memoryUsage = hostVisible ? MemoryUsage::CpuToGpu : MemoryUsage::GpuOnly;
    desc.name = name;
    if (!m_desc.allocator->createBuffer(desc, out) || out.deviceAddress == 0u || (hostVisible && out.mapped == nullptr)) {
        if (out.handle != nullptr) {
            m_desc.allocator->destroyBuffer(out);
        }
        out = Buffer{};
        return false;
    }
    return true;
}

bool DgcPipelineSelector::setMaterialBuckets(const u32* buckets, u32 materialCount) {
    if (!m_initialized) {
        return false;
    }
    m_materialBuckets.assign(buckets != nullptr ? buckets : static_cast<const u32*>(nullptr),
                             buckets != nullptr ? buckets + materialCount : static_cast<const u32*>(nullptr));
    const u32 needed = std::max(16u, static_cast<u32>(m_materialBuckets.size()));
    if (needed > m_materialCapacity || m_materials.handle == nullptr) {
        // Growth recreates the ring: the caller must have retired the frames that used it.
        if (m_materials.handle != nullptr) {
            m_desc.allocator->destroyBuffer(m_materials);
        }
        const u32 capacity = std::max(needed, m_materialCapacity * 2u);
        if (!createBuffer(m_materials, static_cast<u64>(capacity) * m_desc.framesInFlight * sizeof(u32), false, true,
                          "dgc.material_buckets")) {
            m_materialCapacity = 0;
            return false;
        }
        m_materialCapacity = capacity;
    }
    return true;
}

bool DgcPipelineSelector::setPipelines(void* const* pipelines, u32 count, void* pipelineLayout) {
    if (!m_initialized || pipelines == nullptr || count != m_desc.bucketCount) {
        return false;
    }
    m_pipelines.assign(pipelines, pipelines + count);
    m_pipelineLayout = pipelineLayout;
#if defined(FUSE_VULKAN_BACKEND)
    destroyDgcObjects();
    if (!dgcCapable()) {
        return true; // fallback
    }
    const VkDevice dev = vkDev(m_desc);
    Ext& e = *m_ext;
    // Execution set: the bucket pipelines (index = bucket).
    vkx::IndirectExecutionSetPipelineInfo pipelineInfo{};
    pipelineInfo.sType = vkx::kSTypeExecutionSetPipelineInfo;
    pipelineInfo.initialPipeline = static_cast<VkPipeline>(m_pipelines[0]);
    pipelineInfo.maxPipelineCount = count;
    vkx::IndirectExecutionSetCreateInfo setInfo{};
    setInfo.sType = vkx::kSTypeExecutionSetCreateInfo;
    setInfo.type = vkx::kExecutionSetInfoTypePipelines;
    setInfo.info = &pipelineInfo;
    bool ok = e.fn.createExecutionSet(dev, &setInfo, nullptr, &e.executionSet) == VK_SUCCESS;
    if (ok && count > 1u) {
        vkx::WriteIndirectExecutionSetPipeline writes[kDgcMaxBuckets]{};
        for (u32 i = 1; i < count; ++i) {
            writes[i - 1u].sType = vkx::kSTypeWriteExecutionSetPipeline;
            writes[i - 1u].index = i;
            writes[i - 1u].pipeline = static_cast<VkPipeline>(m_pipelines[i]);
        }
        e.fn.updateExecutionSetPipeline(dev, e.executionSet, count - 1u, writes);
    }
    // Layout: kDgcLayoutTokens (the emulation interprets the same array).
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    vkx::IndirectCommandsExecutionSetToken setToken{vkx::kExecutionSetInfoTypePipelines, stages};
    vkx::IndirectCommandsLayoutToken tokens[kDgcLayoutTokenCount]{};
    for (u32 t = 0; t < kDgcLayoutTokenCount; ++t) {
        tokens[t].sType = vkx::kSTypeLayoutToken;
        tokens[t].type = static_cast<u32>(kDgcLayoutTokens[t].type);
        tokens[t].offset = kDgcLayoutTokens[t].offset;
        tokens[t].data = kDgcLayoutTokens[t].type == DgcTokenType::ExecutionSet ? &setToken : nullptr;
    }
    vkx::IndirectCommandsLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = vkx::kSTypeLayoutCreateInfo;
    layoutInfo.flags = 0u; // implicit preprocessing inside the execute; sequences run in order
    layoutInfo.shaderStages = stages;
    layoutInfo.indirectStride = kDgcSequenceStride;
    layoutInfo.pipelineLayout = static_cast<VkPipelineLayout>(pipelineLayout);
    layoutInfo.tokenCount = kDgcLayoutTokenCount;
    layoutInfo.pTokens = tokens;
    ok = ok && e.fn.createLayout(dev, &layoutInfo, nullptr, &e.layout) == VK_SUCCESS;
    // Preprocess buffer (VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT: not a GpuAllocator usage).
    if (ok) {
        vkx::GeneratedCommandsMemoryRequirementsInfo reqInfo{};
        reqInfo.sType = vkx::kSTypeMemoryRequirementsInfo;
        reqInfo.indirectExecutionSet = e.executionSet;
        reqInfo.indirectCommandsLayout = e.layout;
        reqInfo.maxSequenceCount = m_desc.maxDraws;
        reqInfo.maxDrawCount = 0u;
        VkMemoryRequirements2 req{};
        req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        e.fn.getMemoryRequirements(dev, &reqInfo, &req);
        e.preprocessSize = req.memoryRequirements.size;
        if (e.preprocessSize > 0u) {
            vkx::Flags2CreateInfo usage2{};
            usage2.sType = vkx::kSTypeBufferUsageFlags2;
            usage2.flags = vkx::kBufferUsage2PreprocessBuffer | vkx::kBufferUsage2ShaderDeviceAddress;
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.pNext = &usage2;
            bufferInfo.size = e.preprocessSize;
            bufferInfo.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT; // ignored: flags2 wins
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ok = vkCreateBuffer(dev, &bufferInfo, nullptr, &e.preprocess) == VK_SUCCESS;
            VkMemoryRequirements bufferReq{};
            if (ok) {
                vkGetBufferMemoryRequirements(dev, e.preprocess, &bufferReq);
            }
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(static_cast<VkPhysicalDevice>(m_desc.device->nativePhysicalDevice()),
                                                &memProps);
            const u32 typeBits = req.memoryRequirements.memoryTypeBits & bufferReq.memoryTypeBits;
            u32 type = UINT32_MAX;
            for (u32 pass = 0; pass < 2u && type == UINT32_MAX; ++pass) {
                for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
                    const bool local = (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u;
                    if ((typeBits & (1u << i)) != 0u && (local || pass == 1u)) {
                        type = i;
                        break;
                    }
                }
            }
            ok = ok && type != UINT32_MAX;
            if (ok) {
                VkMemoryAllocateFlagsInfo flags{};
                flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
                flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
                VkMemoryAllocateInfo alloc{};
                alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                alloc.pNext = &flags;
                alloc.allocationSize = std::max(bufferReq.size, req.memoryRequirements.size);
                alloc.memoryTypeIndex = type;
                ok = vkAllocateMemory(dev, &alloc, nullptr, &e.preprocessMemory) == VK_SUCCESS &&
                     vkBindBufferMemory(dev, e.preprocess, e.preprocessMemory, 0) == VK_SUCCESS;
            }
            if (ok) {
                e.preprocessAddress = bufferAddress(dev, e.preprocess);
                ok = e.preprocessAddress != 0u;
            }
        }
    }
    if (!ok) {
        destroyDgcObjects();
        m_reason = DgcReason::CreationFailed;
        return true; // the fallback still runs
    }
    m_stats.preprocessBytes = e.preprocessSize;
    m_dgcActive = true;
#endif
    return true;
}

bool DgcPipelineSelector::beginFrame(u64 frameSerial) {
    if (!m_initialized) {
        return false;
    }
    m_frameSerial = frameSerial;
    m_ringSlot = static_cast<u32>(frameSerial % m_desc.framesInFlight);
    if (!m_materialBuckets.empty()) {
        u8* dst = static_cast<u8*>(m_materials.mapped) + static_cast<u64>(m_ringSlot) * m_materialCapacity * sizeof(u32);
        std::memcpy(dst, m_materialBuckets.data(), m_materialBuckets.size() * sizeof(u32));
    }
    return true;
}

DgcGraphRefs DgcPipelineSelector::importInto(rg::Graph& graph) {
    DgcGraphRefs refs{};
    if (!m_initialized) {
        return refs;
    }
    refs.sequences = graph.importBuffer(rg::ImportedBuffer{m_sequences.handle, m_sequences.desc.size, rg::kNoQueue, nullptr,
                                                           "dgc.sequences"});
    refs.bucketArgs = graph.importBuffer(rg::ImportedBuffer{m_bucketArgs.handle, m_bucketArgs.desc.size, rg::kNoQueue,
                                                            nullptr, "dgc.bucket_args"});
    refs.counts = graph.importBuffer(
        rg::ImportedBuffer{m_counts.handle, m_counts.desc.size, rg::kNoQueue, nullptr, "dgc.counts"});
    refs.materials = graph.importBuffer(rg::ImportedBuffer{m_materials.handle, m_materials.desc.size, rg::kNoQueue,
                                                           nullptr, "dgc.material_buckets"});
    return refs;
}

void DgcPipelineSelector::addGenerate(rg::Graph& graph, const DgcGraphRefs& refs, const DgcCullInput& input,
                                      const gpu_scene::GpuSceneGraphRefs& scene, u64 sceneHeaderAddress) {
    if (!m_initialized) {
        return;
    }
#if defined(FUSE_VULKAN_BACKEND)
    const VkDevice dev = vkDev(m_desc);
    GenerateRecord& r = m_generate;
    r.self = this;
    r.counts = m_counts.handle;
    r.push = DgcPush{};
    r.push.args = bufferAddress(dev, input.argsBuffer) + input.argsOffset;
    r.push.drawCount = bufferAddress(dev, input.countsBuffer) + input.countOffset;
    r.push.scene = sceneHeaderAddress;
    r.push.materialBuckets =
        m_materials.deviceAddress + static_cast<u64>(m_ringSlot) * m_materialCapacity * sizeof(u32);
    r.push.sequences = m_sequences.deviceAddress;
    r.push.bucketArgs = m_bucketArgs.deviceAddress;
    r.push.counts = m_counts.deviceAddress;
    const u32 inputDraws = static_cast<u32>(input.argsBytes / sizeof(DgcDrawIndexed));
    r.push.maxDraws = m_desc.maxDraws;
    r.push.bucketCount = m_desc.bucketCount;
    r.push.materialCount = static_cast<u32>(m_materialBuckets.size());
    r.push.defaultBucket = m_desc.defaultBucket;
    r.push.flags = m_desc.writeBothStreams ? (kDgcWriteSequences | kDgcWriteBucketArgs)
                   : m_dgcActive           ? kDgcWriteSequences
                                           : kDgcWriteBucketArgs;
    r.groups = (std::min(m_desc.maxDraws, std::max(inputDraws, 1u)) + kDgcWorkgroup - 1u) / kDgcWorkgroup;
    const u64 countBytes = static_cast<u64>(countWords()) * sizeof(u32);
    graph.addPass("dgc.reset", &DgcPipelineSelector::recordReset, &r)
        .use(refs.counts, rg::Access::TransferDst, rg::BufferRange{0, countBytes});
    rg::PassBuilder pass = graph.addPass("dgc.generate", &DgcPipelineSelector::recordGenerate, &r);
    gpu_scene::GpuScene::useAll(pass, scene, rg::Access::StorageRead, rg::kStageCompute);
    pass.use(input.args, rg::Access::StorageRead, rg::BufferRange{input.argsOffset, input.argsBytes}, rg::kStageCompute)
        .use(input.counts, rg::Access::StorageRead, rg::BufferRange{input.countOffset, sizeof(u32)}, rg::kStageCompute)
        .use(refs.materials, rg::Access::StorageRead,
             rg::BufferRange{static_cast<u64>(m_ringSlot) * m_materialCapacity * sizeof(u32),
                             static_cast<u64>(m_materialCapacity) * sizeof(u32)},
             rg::kStageCompute)
        .use(refs.counts, rg::Access::StorageReadWrite, rg::BufferRange{0, countBytes}, rg::kStageCompute);
    if ((r.push.flags & kDgcWriteSequences) != 0u) {
        pass.use(refs.sequences, rg::Access::StorageWrite, {}, rg::kStageCompute);
    }
    if ((r.push.flags & kDgcWriteBucketArgs) != 0u) {
        pass.use(refs.bucketArgs, rg::Access::StorageWrite, {}, rg::kStageCompute);
    }
#else
    (void)graph;
    (void)refs;
    (void)input;
    (void)scene;
    (void)sceneHeaderAddress;
#endif
}

void DgcPipelineSelector::useDraws(rg::PassBuilder& pass, const DgcGraphRefs& refs) const {
    const u64 countBytes = static_cast<u64>(countWords()) * sizeof(u32);
    pass.use(m_dgcActive ? refs.sequences : refs.bucketArgs, rg::Access::IndirectRead)
        .use(refs.counts, rg::Access::IndirectRead, rg::BufferRange{0, countBytes});
}

void DgcPipelineSelector::recordDraws(void* commandBuffer) const {
#if defined(FUSE_VULKAN_BACKEND)
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    m_stats.recordedCommands = 0;
    if (m_pipelines.empty()) {
        return;
    }
    if (m_dgcActive) {
        // The execution set's initial pipeline must be bound; state bound before (descriptors, push
        // constants, dynamic state, index / vertex buffers) is inherited by the generated draws.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(m_pipelines[0]));
        vkx::GeneratedCommandsInfo info{};
        info.sType = vkx::kSTypeGeneratedCommandsInfo;
        info.shaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        info.indirectExecutionSet = m_ext->executionSet;
        info.indirectCommandsLayout = m_ext->layout;
        info.indirectAddress = m_ext->sequencesAddress;
        info.indirectAddressSize = static_cast<VkDeviceSize>(m_desc.maxDraws) * kDgcSequenceStride;
        info.preprocessAddress = m_ext->preprocessAddress;
        info.preprocessSize = m_ext->preprocessSize;
        info.maxSequenceCount = m_desc.maxDraws;
        info.sequenceCountAddress = m_ext->countAddress;
        info.maxDrawCount = 0u;
        m_ext->fn.cmdExecute(cmd, VK_FALSE, &info);
        m_stats.recordedCommands = 2u;
        ++m_stats.executes;
        return;
    }
    const VkBuffer args = static_cast<VkBuffer>(m_bucketArgs.handle);
    const VkBuffer counts = static_cast<VkBuffer>(m_counts.handle);
    const u64 bucketBytes = static_cast<u64>(m_desc.maxDraws) * sizeof(DgcDrawIndexed);
    for (u32 b = 0; b < m_desc.bucketCount; ++b) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(m_pipelines[b]));
        vkCmdDrawIndexedIndirectCount(cmd, args, b * bucketBytes, counts, (kDgcCountBucketBase + b) * sizeof(u32),
                                      m_desc.maxDraws, sizeof(DgcDrawIndexed));
        m_stats.recordedCommands += 2u;
        ++m_stats.indirectCountDraws;
    }
#else
    (void)commandBuffer;
#endif
}

void DgcPipelineSelector::recordReset(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const GenerateRecord& r = *static_cast<const GenerateRecord*>(user);
    vkCmdFillBuffer(static_cast<VkCommandBuffer>(context.commandBuffer), static_cast<VkBuffer>(r.counts), 0,
                    static_cast<VkDeviceSize>(r.self->countWords()) * sizeof(u32), 0u);
#else
    (void)context;
    (void)user;
#endif
}

void DgcPipelineSelector::recordGenerate(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const GenerateRecord& r = *static_cast<const GenerateRecord*>(user);
    const DgcPipelineSelector& self = *r.self;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(self.m_pipeline));
    self.m_desc.bindless->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.m_layout, 0);
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(self.m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DgcPush),
                       &r.push);
    vkCmdDispatch(cmd, r.groups, 1, 1);
#else
    (void)context;
    (void)user;
#endif
}

} // namespace fuse::renderer::dgc
