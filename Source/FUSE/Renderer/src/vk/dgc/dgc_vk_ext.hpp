#pragma once
// WP-9.3: local mirror of VK_EXT_device_generated_commands (spec revision 1, Vulkan headers >= 1.3.296)
// for builds against older headers (the local SDK is 1.3.275). Names live in fuse::renderer::dgc::vkx
// so they never collide with the real declarations when newer headers define them; the numeric
// values and struct layouts are the registry's (checked against Engine/lib/dxvk's vulkan_core.h 1.4.350
// by the static_asserts in dgc_pipeline_selector.cpp when the real header is available).
// Include after <vulkan/vulkan.h>.
#include <cstdint>

namespace fuse::renderer::dgc::vkx {

#if defined(VK_USE_64_BIT_PTR_DEFINES) && (VK_USE_64_BIT_PTR_DEFINES == 1)
struct IndirectExecutionSetT;
struct IndirectCommandsLayoutT;
using IndirectExecutionSet = IndirectExecutionSetT*;
using IndirectCommandsLayout = IndirectCommandsLayoutT*;
#else
using IndirectExecutionSet = std::uint64_t;
using IndirectCommandsLayout = std::uint64_t;
#endif

inline constexpr const char* kExtensionName = "VK_EXT_device_generated_commands";
inline constexpr const char* kMaintenance5Name = "VK_KHR_maintenance5";

inline constexpr VkStructureType kSTypeFeatures = static_cast<VkStructureType>(1000572000);
inline constexpr VkStructureType kSTypeProperties = static_cast<VkStructureType>(1000572001);
inline constexpr VkStructureType kSTypeMemoryRequirementsInfo = static_cast<VkStructureType>(1000572002);
inline constexpr VkStructureType kSTypeExecutionSetCreateInfo = static_cast<VkStructureType>(1000572003);
inline constexpr VkStructureType kSTypeGeneratedCommandsInfo = static_cast<VkStructureType>(1000572004);
inline constexpr VkStructureType kSTypeLayoutCreateInfo = static_cast<VkStructureType>(1000572006);
inline constexpr VkStructureType kSTypeLayoutToken = static_cast<VkStructureType>(1000572007);
inline constexpr VkStructureType kSTypeWriteExecutionSetPipeline = static_cast<VkStructureType>(1000572008);
inline constexpr VkStructureType kSTypeExecutionSetPipelineInfo = static_cast<VkStructureType>(1000572010);
/// VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR
inline constexpr VkStructureType kSTypeMaintenance5Features = static_cast<VkStructureType>(1000470000);
/// VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR / BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR
inline constexpr VkStructureType kSTypePipelineCreateFlags2 = static_cast<VkStructureType>(1000470005);
inline constexpr VkStructureType kSTypeBufferUsageFlags2 = static_cast<VkStructureType>(1000470006);

inline constexpr std::uint64_t kPipelineCreate2IndirectBindable = 0x4000000000ull;
inline constexpr std::uint64_t kBufferUsage2PreprocessBuffer = 0x80000000ull;
inline constexpr std::uint64_t kBufferUsage2ShaderDeviceAddress = 0x00020000ull;
inline constexpr std::uint32_t kExecutionSetInfoTypePipelines = 0u;
inline constexpr std::uint32_t kLayoutUsageExplicitPreprocess = 1u;

struct PhysicalDeviceFeatures {
    VkStructureType sType;
    void* pNext;
    VkBool32 deviceGeneratedCommands;
    VkBool32 dynamicGeneratedPipelineLayout;
};

struct PhysicalDeviceProperties {
    VkStructureType sType;
    void* pNext;
    std::uint32_t maxIndirectPipelineCount;
    std::uint32_t maxIndirectShaderObjectCount;
    std::uint32_t maxIndirectSequenceCount;
    std::uint32_t maxIndirectCommandsTokenCount;
    std::uint32_t maxIndirectCommandsTokenOffset;
    std::uint32_t maxIndirectCommandsIndirectStride;
    VkFlags supportedIndirectCommandsInputModes;
    VkShaderStageFlags supportedIndirectCommandsShaderStages;
    VkShaderStageFlags supportedIndirectCommandsShaderStagesPipelineBinding;
    VkShaderStageFlags supportedIndirectCommandsShaderStagesShaderBinding;
    VkBool32 deviceGeneratedCommandsTransformFeedback;
    VkBool32 deviceGeneratedCommandsMultiDrawIndirectCount;
};

struct GeneratedCommandsMemoryRequirementsInfo {
    VkStructureType sType;
    const void* pNext;
    IndirectExecutionSet indirectExecutionSet;
    IndirectCommandsLayout indirectCommandsLayout;
    std::uint32_t maxSequenceCount;
    std::uint32_t maxDrawCount;
};

struct IndirectExecutionSetPipelineInfo {
    VkStructureType sType;
    const void* pNext;
    VkPipeline initialPipeline;
    std::uint32_t maxPipelineCount;
};

struct IndirectExecutionSetCreateInfo {
    VkStructureType sType;
    const void* pNext;
    std::uint32_t type; ///< VkIndirectExecutionSetInfoTypeEXT
    const void* info;   ///< union { pPipelineInfo; pShaderInfo; }
};

struct GeneratedCommandsInfo {
    VkStructureType sType;
    const void* pNext;
    VkShaderStageFlags shaderStages;
    IndirectExecutionSet indirectExecutionSet;
    IndirectCommandsLayout indirectCommandsLayout;
    VkDeviceAddress indirectAddress;
    VkDeviceSize indirectAddressSize;
    VkDeviceAddress preprocessAddress;
    VkDeviceSize preprocessSize;
    std::uint32_t maxSequenceCount;
    VkDeviceAddress sequenceCountAddress;
    std::uint32_t maxDrawCount;
};

struct WriteIndirectExecutionSetPipeline {
    VkStructureType sType;
    const void* pNext;
    std::uint32_t index;
    VkPipeline pipeline;
};

struct IndirectCommandsExecutionSetToken {
    std::uint32_t type; ///< VkIndirectExecutionSetInfoTypeEXT
    VkShaderStageFlags shaderStages;
};

struct IndirectCommandsLayoutToken {
    VkStructureType sType;
    const void* pNext;
    std::uint32_t type; ///< VkIndirectCommandsTokenTypeEXT
    const void* data;   ///< union of token-data pointers
    std::uint32_t offset;
};

struct IndirectCommandsLayoutCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    VkShaderStageFlags shaderStages;
    std::uint32_t indirectStride;
    VkPipelineLayout pipelineLayout;
    std::uint32_t tokenCount;
    const IndirectCommandsLayoutToken* pTokens;
};

/// VkPipelineCreateFlags2CreateInfoKHR / VkBufferUsageFlags2CreateInfoKHR (maintenance5).
struct Flags2CreateInfo {
    VkStructureType sType;
    const void* pNext;
    std::uint64_t flags;
};

using PfnGetGeneratedCommandsMemoryRequirements = void(VKAPI_PTR*)(VkDevice, const GeneratedCommandsMemoryRequirementsInfo*,
                                                                   VkMemoryRequirements2*);
using PfnCmdExecuteGeneratedCommands = void(VKAPI_PTR*)(VkCommandBuffer, VkBool32, const GeneratedCommandsInfo*);
using PfnCreateIndirectCommandsLayout = VkResult(VKAPI_PTR*)(VkDevice, const IndirectCommandsLayoutCreateInfo*,
                                                             const VkAllocationCallbacks*, IndirectCommandsLayout*);
using PfnDestroyIndirectCommandsLayout = void(VKAPI_PTR*)(VkDevice, IndirectCommandsLayout, const VkAllocationCallbacks*);
using PfnCreateIndirectExecutionSet = VkResult(VKAPI_PTR*)(VkDevice, const IndirectExecutionSetCreateInfo*,
                                                           const VkAllocationCallbacks*, IndirectExecutionSet*);
using PfnDestroyIndirectExecutionSet = void(VKAPI_PTR*)(VkDevice, IndirectExecutionSet, const VkAllocationCallbacks*);
using PfnUpdateIndirectExecutionSetPipeline = void(VKAPI_PTR*)(VkDevice, IndirectExecutionSet, std::uint32_t,
                                                               const WriteIndirectExecutionSetPipeline*);

struct Dispatch {
    PfnGetGeneratedCommandsMemoryRequirements getMemoryRequirements = nullptr;
    PfnCmdExecuteGeneratedCommands cmdExecute = nullptr;
    PfnCreateIndirectCommandsLayout createLayout = nullptr;
    PfnDestroyIndirectCommandsLayout destroyLayout = nullptr;
    PfnCreateIndirectExecutionSet createExecutionSet = nullptr;
    PfnDestroyIndirectExecutionSet destroyExecutionSet = nullptr;
    PfnUpdateIndirectExecutionSetPipeline updateExecutionSetPipeline = nullptr;

    bool complete() const {
        return getMemoryRequirements != nullptr && cmdExecute != nullptr && createLayout != nullptr &&
               destroyLayout != nullptr && createExecutionSet != nullptr && destroyExecutionSet != nullptr &&
               updateExecutionSetPipeline != nullptr;
    }
};

} // namespace fuse::renderer::dgc::vkx
