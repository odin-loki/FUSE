#pragma once
// WP-9.3 device-generated commands: CPU reference of the generate kernel, CPU emulation of the
// indirect-commands layout (the command stream a DGC-capable GPU generates from the sequences),
// the indirect-count draw list it must equal, the capability gate and the CPU command-count model.
// Header + src/vk/dgc/dgc_reference.cpp; no Vulkan dependency (runs in the stub tree).
#include <fuse/renderer/vk/dgc/dgc_types.hpp>

#include <vector>

namespace fuse::renderer::dgc {

// --- indirect-commands layout (shared by the Vulkan layout and the emulation) ----------------------
/// Numeric values of VkIndirectCommandsTokenTypeEXT.
enum class DgcTokenType : u32 {
    ExecutionSet = 0u,
    PushConstant = 1u,
    SequenceIndex = 2u,
    IndexBuffer = 3u,
    VertexBuffer = 4u,
    DrawIndexed = 5u,
    Draw = 6u,
};

struct DgcToken {
    DgcTokenType type = DgcTokenType::DrawIndexed;
    u32 offset = 0; ///< byte offset inside the sequence
};

/// The layout the selector creates on the device (VkIndirectCommandsLayoutCreateInfoEXT) and the
/// emulation interprets: execution-set pipeline index, then an indexed draw.
inline constexpr DgcToken kDgcLayoutTokens[2] = {
    {DgcTokenType::ExecutionSet, kDgcTokenExecutionSetOffset},
    {DgcTokenType::DrawIndexed, kDgcTokenDrawIndexedOffset},
};
inline constexpr u32 kDgcLayoutTokenCount = 2u;

// --- command stream -----------------------------------------------------------------------------
struct DgcCommand {
    enum class Kind : u8 { BindPipeline, DrawIndexed };
    Kind kind = Kind::DrawIndexed;
    u32 pipeline = 0; ///< BindPipeline: the pipeline index; DrawIndexed: the pipeline bound for it
    DgcDrawIndexed draw{};
};

/// A resolved draw: the pipeline it runs with and its arguments.
struct DgcResolvedDraw {
    u32 pipeline = 0;
    DgcDrawIndexed draw{};
};

// --- reference generator ------------------------------------------------------------------------
struct DgcReferenceInput {
    const DgcDrawIndexed* draws = nullptr; ///< culled args (WP-1.3 layout: firstInstance = slot)
    u32 drawCount = 0;                     ///< GPU draw count
    u32 maxDraws = 0;
    const u32* instanceMaterials = nullptr; ///< GpuInstance::material by slot
    u32 instanceCount = 0;                  ///< slots >= this read as kInvalidIndex (material out of range)
    const u32* materialBuckets = nullptr;
    u32 materialCount = 0;
    u32 bucketCount = 1;
    u32 defaultBucket = 0;
};

struct DgcReferenceOutput {
    std::vector<DgcSequence> sequences;                 ///< sequence i = draw i (deterministic order)
    std::vector<std::vector<DgcDrawIndexed>> bucketArgs; ///< per bucket, culled order (the GPU order is atomic)
};

/// What dgc_generate writes, computed on the CPU.
void dgc_generate_reference(const DgcReferenceInput& in, DgcReferenceOutput& out);

/// Emulates vkCmdExecuteGeneratedCommandsEXT for a layout: walks min(sequenceCount, maxSequences)
/// sequences of `stream` (stride bytes each) and appends the commands the device would generate.
/// An EXECUTION_SET token emits a BindPipeline only when the index differs from the bound one
/// (`initialPipeline` is bound before the call, as the spec requires). Unknown tokens are ignored.
/// Returns false when the stream is too short for the sequences read.
bool dgc_emulate_layout(const DgcToken* tokens, u32 tokenCount, u32 stride, const void* stream, u64 streamBytes,
                        u32 sequenceCount, u32 maxSequences, u32 initialPipeline, std::vector<DgcCommand>& out);

/// The fallback's command stream: for every bucket b, BindPipeline(b) and then its draws
/// (bucketCounts[b] entries of bucketArgs[b], i.e. what vkCmdDrawIndexedIndirectCount executes).
void dgc_indirect_count_commands(const std::vector<std::vector<DgcDrawIndexed>>& bucketArgs, const u32* bucketCounts,
                                 std::vector<DgcCommand>& out);

/// The (pipeline, draw) pairs of a command stream.
void dgc_resolve_draws(const std::vector<DgcCommand>& commands, std::vector<DgcResolvedDraw>& out);

/// Same multiset of (pipeline, draw) pairs (draw order inside the stream does not matter for an
/// opaque depth-tested pass without equal-depth ties).
bool dgc_same_draws(const std::vector<DgcCommand>& a, const std::vector<DgcCommand>& b);

// --- capability gate ------------------------------------------------------------------------------
enum class DgcReason : u8 {
    Supported = 0,
    ForcedFallback,         ///< DgcSelectorDesc::mode == ForceFallback
    NoVulkanBackend,        ///< stub build
    NoDevice,
    NoBufferDeviceAddress,
    NoDrawIndirectCount,
    ExtensionNotEnabled,    ///< VK_EXT_device_generated_commands not enabled on the device
    Maintenance5NotEnabled, ///< VK_KHR_maintenance5 (pipeline flags 2) not enabled
    FeatureStateUnknown,    ///< no creator feature chain: the enabled feature bit cannot be read back
    FeatureNotEnabled,      ///< deviceGeneratedCommands feature bit not enabled at vkCreateDevice
    EntryPointsMissing,     ///< vkGetDeviceProcAddr returned null for a DGC entry point
    StagesUnsupported,      ///< supportedIndirectCommandsShaderStagesPipelineBinding lacks vertex / fragment
    TooManyPipelines,       ///< bucketCount > maxIndirectPipelineCount
    TooManySequences,       ///< maxDraws > maxIndirectSequenceCount
    LayoutLimits,           ///< token count / offset / stride above the device limits
    BadBucketCount,         ///< 0 or > kDgcMaxBuckets
    CreationFailed,         ///< execution set / layout / preprocess buffer creation failed
};

const char* dgc_reason_text(DgcReason reason);

struct DgcCapabilityInputs {
    bool vulkanBackend = false;
    bool deviceValid = false;
    bool forceFallback = false;
    bool bufferDeviceAddress = false;
    bool drawIndirectCount = false;
    bool extensionEnabled = false;
    bool maintenance5Enabled = false;
    bool featureStateKnown = false; ///< a feature chain (or RendererCaps) says what was enabled
    bool featureEnabled = false;
    bool entryPoints = false;
    u32 maxIndirectPipelineCount = 0;
    u32 maxIndirectSequenceCount = 0;
    u32 maxIndirectCommandsTokenCount = 0;
    u32 maxIndirectCommandsTokenOffset = 0;
    u32 maxIndirectCommandsIndirectStride = 0;
    u32 pipelineBindingStages = 0; ///< VkShaderStageFlags
    u32 requiredStages = 0x11u;    ///< VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    u32 bucketCount = 0;
    u32 maxDraws = 0;
};

/// The first failing requirement in the order above (Supported when all hold).
DgcReason dgc_evaluate_capability(const DgcCapabilityInputs& in);

// --- CPU command-count model ------------------------------------------------------------------------
/// Draw-related commands the raster pass records: DGC = bind initial pipeline + execute (2, any
/// bucket count); fallback = bucketCount x (bind pipeline + draw indexed indirect count).
inline constexpr u32 dgc_recorded_commands(bool dgc, u32 bucketCount) { return dgc ? 2u : 2u * bucketCount; }

} // namespace fuse::renderer::dgc
