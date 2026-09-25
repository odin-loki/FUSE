// WP-9.3 device-generated commands: CPU reference, layout emulation, capability gate.
// See include/fuse/renderer/vk/dgc/dgc_reference.hpp.
#include <fuse/renderer/vk/dgc/dgc_reference.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::renderer::dgc {

namespace {
constexpr u32 kInvalid = 0xFFFFFFFFu;

bool drawLess(const DgcResolvedDraw& a, const DgcResolvedDraw& b) {
    const u32 ka[6] = {a.pipeline, a.draw.firstInstance, a.draw.indexCount, a.draw.instanceCount, a.draw.firstIndex,
                       static_cast<u32>(a.draw.vertexOffset)};
    const u32 kb[6] = {b.pipeline, b.draw.firstInstance, b.draw.indexCount, b.draw.instanceCount, b.draw.firstIndex,
                       static_cast<u32>(b.draw.vertexOffset)};
    return std::lexicographical_compare(ka, ka + 6, kb, kb + 6);
}

bool drawEqual(const DgcResolvedDraw& a, const DgcResolvedDraw& b) {
    return a.pipeline == b.pipeline && std::memcmp(&a.draw, &b.draw, sizeof(DgcDrawIndexed)) == 0;
}
} // namespace

void dgc_generate_reference(const DgcReferenceInput& in, DgcReferenceOutput& out) {
    const u32 buckets = in.bucketCount == 0u ? 1u : in.bucketCount;
    out.sequences.clear();
    out.bucketArgs.assign(buckets, {});
    const u32 count = std::min(in.drawCount, in.maxDraws);
    for (u32 i = 0; i < count; ++i) {
        const DgcDrawIndexed& d = in.draws[i];
        const u32 slot = d.firstInstance;
        const u32 material = (in.instanceMaterials != nullptr && slot < in.instanceCount) ? in.instanceMaterials[slot] : kInvalid;
        const u32 bucket = dgc_select_bucket(material, in.materialBuckets, in.materialCount, buckets, in.defaultBucket);
        DgcSequence s{};
        s.pipelineIndex = bucket;
        s.draw = d;
        out.sequences.push_back(s);
        out.bucketArgs[bucket].push_back(d);
    }
}

bool dgc_emulate_layout(const DgcToken* tokens, u32 tokenCount, u32 stride, const void* stream, u64 streamBytes,
                        u32 sequenceCount, u32 maxSequences, u32 initialPipeline, std::vector<DgcCommand>& out) {
    const u8* base = static_cast<const u8*>(stream);
    const u32 count = std::min(sequenceCount, maxSequences);
    u32 bound = initialPipeline;
    for (u32 s = 0; s < count; ++s) {
        const u64 at = static_cast<u64>(s) * stride;
        if (at + stride > streamBytes) {
            return false;
        }
        for (u32 t = 0; t < tokenCount; ++t) {
            const u8* p = base + at + tokens[t].offset;
            switch (tokens[t].type) {
            case DgcTokenType::ExecutionSet: {
                u32 index = 0;
                std::memcpy(&index, p, sizeof(index));
                if (index != bound) {
                    DgcCommand c{};
                    c.kind = DgcCommand::Kind::BindPipeline;
                    c.pipeline = index;
                    out.push_back(c);
                    bound = index;
                }
                break;
            }
            case DgcTokenType::DrawIndexed: {
                DgcCommand c{};
                c.kind = DgcCommand::Kind::DrawIndexed;
                c.pipeline = bound;
                std::memcpy(&c.draw, p, sizeof(DgcDrawIndexed));
                out.push_back(c);
                break;
            }
            default:
                break;
            }
        }
    }
    return true;
}

void dgc_indirect_count_commands(const std::vector<std::vector<DgcDrawIndexed>>& bucketArgs, const u32* bucketCounts,
                                 std::vector<DgcCommand>& out) {
    for (u32 b = 0; b < static_cast<u32>(bucketArgs.size()); ++b) {
        DgcCommand bind{};
        bind.kind = DgcCommand::Kind::BindPipeline;
        bind.pipeline = b;
        out.push_back(bind);
        const u32 n = std::min(bucketCounts[b], static_cast<u32>(bucketArgs[b].size()));
        for (u32 i = 0; i < n; ++i) {
            DgcCommand c{};
            c.kind = DgcCommand::Kind::DrawIndexed;
            c.pipeline = b;
            c.draw = bucketArgs[b][i];
            out.push_back(c);
        }
    }
}

void dgc_resolve_draws(const std::vector<DgcCommand>& commands, std::vector<DgcResolvedDraw>& out) {
    out.clear();
    for (const DgcCommand& c : commands) {
        if (c.kind == DgcCommand::Kind::DrawIndexed) {
            out.push_back(DgcResolvedDraw{c.pipeline, c.draw});
        }
    }
}

bool dgc_same_draws(const std::vector<DgcCommand>& a, const std::vector<DgcCommand>& b) {
    std::vector<DgcResolvedDraw> ra;
    std::vector<DgcResolvedDraw> rb;
    dgc_resolve_draws(a, ra);
    dgc_resolve_draws(b, rb);
    if (ra.size() != rb.size()) {
        return false;
    }
    std::sort(ra.begin(), ra.end(), drawLess);
    std::sort(rb.begin(), rb.end(), drawLess);
    return std::equal(ra.begin(), ra.end(), rb.begin(), drawEqual);
}

const char* dgc_reason_text(DgcReason reason) {
    switch (reason) {
    case DgcReason::Supported: return "supported";
    case DgcReason::ForcedFallback: return "fallback forced by the caller (DgcMode::ForceFallback)";
    case DgcReason::NoVulkanBackend: return "no Vulkan backend (stub build)";
    case DgcReason::NoDevice: return "no valid Vulkan device";
    case DgcReason::NoBufferDeviceAddress: return "bufferDeviceAddress not enabled";
    case DgcReason::NoDrawIndirectCount: return "drawIndirectCount not enabled";
    case DgcReason::ExtensionNotEnabled: return "VK_EXT_device_generated_commands not enabled on the device";
    case DgcReason::Maintenance5NotEnabled: return "VK_KHR_maintenance5 not enabled on the device (needed for INDIRECT_BINDABLE pipelines)";
    case DgcReason::FeatureStateUnknown:
        return "deviceGeneratedCommands feature state unknown: the Vulkan headers predate the extension, pass the "
               "creator's VkDeviceCreateInfo::pNext chain (DgcSelectorDesc::enabledFeatureChain)";
    case DgcReason::FeatureNotEnabled: return "deviceGeneratedCommands feature not enabled at vkCreateDevice";
    case DgcReason::EntryPointsMissing: return "VK_EXT_device_generated_commands entry points not resolvable";
    case DgcReason::StagesUnsupported: return "pipeline binding in DGC does not support the vertex + fragment stages";
    case DgcReason::TooManyPipelines: return "bucket count above maxIndirectPipelineCount";
    case DgcReason::TooManySequences: return "maxDraws above maxIndirectSequenceCount";
    case DgcReason::LayoutLimits: return "indirect-commands layout above the device token / offset / stride limits";
    case DgcReason::BadBucketCount: return "bucket count is 0 or above kDgcMaxBuckets";
    case DgcReason::CreationFailed: return "execution set / indirect-commands layout / preprocess buffer creation failed";
    }
    return "unknown";
}

DgcReason dgc_evaluate_capability(const DgcCapabilityInputs& in) {
    if (in.bucketCount == 0u || in.bucketCount > kDgcMaxBuckets) {
        return DgcReason::BadBucketCount;
    }
    if (in.forceFallback) {
        return DgcReason::ForcedFallback;
    }
    if (!in.vulkanBackend) {
        return DgcReason::NoVulkanBackend;
    }
    if (!in.deviceValid) {
        return DgcReason::NoDevice;
    }
    if (!in.bufferDeviceAddress) {
        return DgcReason::NoBufferDeviceAddress;
    }
    if (!in.drawIndirectCount) {
        return DgcReason::NoDrawIndirectCount;
    }
    if (!in.extensionEnabled) {
        return DgcReason::ExtensionNotEnabled;
    }
    if (!in.maintenance5Enabled) {
        return DgcReason::Maintenance5NotEnabled;
    }
    if (!in.featureStateKnown) {
        return DgcReason::FeatureStateUnknown;
    }
    if (!in.featureEnabled) {
        return DgcReason::FeatureNotEnabled;
    }
    if (!in.entryPoints) {
        return DgcReason::EntryPointsMissing;
    }
    if ((in.pipelineBindingStages & in.requiredStages) != in.requiredStages) {
        return DgcReason::StagesUnsupported;
    }
    if (in.bucketCount > in.maxIndirectPipelineCount) {
        return DgcReason::TooManyPipelines;
    }
    if (in.maxDraws > in.maxIndirectSequenceCount) {
        return DgcReason::TooManySequences;
    }
    if (kDgcLayoutTokenCount > in.maxIndirectCommandsTokenCount ||
        kDgcTokenDrawIndexedOffset > in.maxIndirectCommandsTokenOffset ||
        kDgcSequenceStride > in.maxIndirectCommandsIndirectStride) {
        return DgcReason::LayoutLimits;
    }
    return DgcReason::Supported;
}

} // namespace fuse::renderer::dgc
