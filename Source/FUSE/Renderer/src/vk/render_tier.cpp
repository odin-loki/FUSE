#include <fuse/renderer/vk/render_tier.hpp>

#include <cstdlib>
#include <cstring>

namespace fuse::renderer {

namespace {

constexpr const char* kNoFallback = "none (T0 requirement: device rejected)";

constexpr RenderFeatureInfo kFeatureInfo[kRenderFeatureCount] = {
    {"synchronization2", RenderTier::T0, true, kNoFallback},
    {"dynamicRendering", RenderTier::T0, true, kNoFallback},
    {"maintenance4", RenderTier::T0, true, kNoFallback},
    {"timelineSemaphore", RenderTier::T0, true, kNoFallback},
    {"descriptorIndexing", RenderTier::T0, true, kNoFallback},
    {"bufferDeviceAddress", RenderTier::T0, true, kNoFallback},
    {"drawIndirectCount", RenderTier::T0, true, kNoFallback},
    {"multiDrawIndirect", RenderTier::T0, true, kNoFallback},
    {"shaderDrawParameters", RenderTier::T0, true, kNoFallback},
    {"shaderInt64", RenderTier::T0, true, kNoFallback},
    {"shaderBufferInt64Atomics", RenderTier::T0, true, kNoFallback},
    {"shaderImageInt64Atomics", RenderTier::T0, false, "buffer-backed 64-bit visibility buffer (shaderBufferInt64Atomics)"},
    {"descriptorBuffer", RenderTier::T0, false, "descriptor-set bindless registry"},
    {"deviceGeneratedCommands", RenderTier::T0, false, "indirect draws with CPU pipeline binning"},
    {"shaderObject", RenderTier::T0, false, "VkPipeline objects + pipeline cache"},
    {"taskShader", RenderTier::T1, true, "compute culling + multi-draw-indirect-count (Phase 1 path)"},
    {"meshShader", RenderTier::T1, true, "vertex-shader indirect path (Phase 1)"},
    {"accelerationStructure", RenderTier::T2, true, "SDF / voxel scene tracing in compute"},
    {"rayQuery", RenderTier::T2, true, "screen-space + SDF tracing"},
    {"rayTracingPipeline", RenderTier::T3, true, "ray query in compute (T2 path)"},
    {"cooperativeMatrix", RenderTier::T3, true, "scalar shader math / CPU reference (no tensor units)"},
};

u64 maskOfTier(RenderTier tier, bool requiredOnly) {
    u64 mask = 0;
    for (u32 i = 0; i < kRenderFeatureCount; ++i) {
        if (kFeatureInfo[i].tier == tier && (!requiredOnly || kFeatureInfo[i].requiredForTier)) {
            mask |= u64{1} << i;
        }
    }
    return mask;
}

void appendName(std::string& out, const char* name) {
    if (!out.empty()) {
        out += ',';
    }
    out += name;
}

} // namespace

const RenderFeatureInfo& renderFeatureInfo(RenderFeature feature) {
    const u32 index = static_cast<u32>(feature);
    return kFeatureInfo[index < kRenderFeatureCount ? index : 0];
}

const char* renderTierName(RenderTier tier) {
    switch (tier) {
    case RenderTier::T0:
        return "T0";
    case RenderTier::T1:
        return "T1";
    case RenderTier::T2:
        return "T2";
    case RenderTier::T3:
        return "T3";
    }
    return "T?";
}

bool parseRenderTier(const char* text, RenderTier& out) {
    if (text == nullptr) {
        return false;
    }
    const char* digit = text;
    if (*digit == 'T' || *digit == 't') {
        ++digit;
    }
    if (digit[0] < '0' || digit[0] > '3' || digit[1] != '\0') {
        return false;
    }
    out = static_cast<RenderTier>(digit[0] - '0');
    return true;
}

RenderTier renderTierMaxFromEnv() {
    RenderTier tier = kMaxRenderTier;
    parseRenderTier(std::getenv("FUSE_RENDER_TIER_MAX"), tier);
    return tier;
}

u64 renderT0RequiredMask() {
    return maskOfTier(RenderTier::T0, true);
}

u64 renderFeaturesAboveTier(RenderTier tier) {
    u64 mask = 0;
    for (u32 i = 0; i < kRenderFeatureCount; ++i) {
        if (static_cast<u8>(kFeatureInfo[i].tier) > static_cast<u8>(tier)) {
            mask |= u64{1} << i;
        }
    }
    return mask;
}

RenderTier renderTierFromMask(u64 mask) {
    RenderTier tier = RenderTier::T0;
    if ((mask & renderT0RequiredMask()) != renderT0RequiredMask()) {
        return tier;
    }
    for (u8 t = 1; t <= static_cast<u8>(kMaxRenderTier); ++t) {
        const u64 required = maskOfTier(static_cast<RenderTier>(t), true);
        if ((mask & required) != required) {
            break;
        }
        tier = static_cast<RenderTier>(t);
    }
    return tier;
}

std::string renderMissingT0(u64 mask) {
    std::string out;
    const u64 required = renderT0RequiredMask();
    for (u32 i = 0; i < kRenderFeatureCount; ++i) {
        const u64 bit = u64{1} << i;
        if ((required & bit) != 0 && (mask & bit) == 0) {
            appendName(out, kFeatureInfo[i].name);
        }
    }
    return out;
}

const char* RendererCaps::fallbackFor(RenderFeature feature) const {
    return has(feature) ? nullptr : renderFeatureInfo(feature).fallback;
}

std::string RendererCaps::missingT0() const {
    // VK_MAKE_API_VERSION(0, 1, 3, 0) without the Vulkan headers (this file builds in the stub).
    constexpr u32 kVulkan13 = (1u << 22) | (3u << 12);
    std::string out = apiVersion < kVulkan13 ? std::string("Vulkan 1.3") : std::string();
    const std::string features = renderMissingT0(enabledMask);
    if (!features.empty()) {
        appendName(out, features.c_str());
    }
    return out;
}

void RendererCaps::setEnabledMask(u64 mask) {
    enabledMask = mask;
    synchronization2 = has(RenderFeature::Synchronization2);
    dynamicRendering = has(RenderFeature::DynamicRendering);
    maintenance4 = has(RenderFeature::Maintenance4);
    timelineSemaphore = has(RenderFeature::TimelineSemaphore);
    descriptorIndexing = has(RenderFeature::DescriptorIndexing);
    bufferDeviceAddress = has(RenderFeature::BufferDeviceAddress);
    drawIndirectCount = has(RenderFeature::DrawIndirectCount);
    multiDrawIndirect = has(RenderFeature::MultiDrawIndirect);
    shaderDrawParameters = has(RenderFeature::ShaderDrawParameters);
    shaderInt64 = has(RenderFeature::ShaderInt64);
    shaderBufferInt64Atomics = has(RenderFeature::ShaderBufferInt64Atomics);
    shaderImageInt64Atomics = has(RenderFeature::ShaderImageInt64Atomics);
    descriptorBuffer = has(RenderFeature::DescriptorBuffer);
    deviceGeneratedCommands = has(RenderFeature::DeviceGeneratedCommands);
    shaderObject = has(RenderFeature::ShaderObject);
    taskShader = has(RenderFeature::TaskShader);
    meshShader = has(RenderFeature::MeshShader);
    accelerationStructure = has(RenderFeature::AccelerationStructure);
    rayQuery = has(RenderFeature::RayQuery);
    rayTracingPipeline = has(RenderFeature::RayTracingPipeline);
    cooperativeMatrix = has(RenderFeature::CooperativeMatrix);
}

std::string RendererCaps::summary() const {
    std::string out = renderTierName(tier);
    out += " (hw ";
    out += renderTierName(hardwareTier);
    out += ", cap ";
    out += renderTierName(tierCap);
    out += meetsT0 ? ")" : ", T0 incomplete: " + missingT0() + ")";
    std::string enabled;
    std::string fallbacks;
    for (u32 i = 0; i < kRenderFeatureCount; ++i) {
        const auto feature = static_cast<RenderFeature>(i);
        if (has(feature)) {
            appendName(enabled, kFeatureInfo[i].name);
        } else {
            if (!fallbacks.empty()) {
                fallbacks += "; ";
            }
            fallbacks += kFeatureInfo[i].name;
            fallbacks += supports(feature) ? " (supported, not enabled)" : "";
            fallbacks += " -> ";
            fallbacks += kFeatureInfo[i].fallback;
        }
    }
    out += " enabled: " + (enabled.empty() ? std::string("-") : enabled);
    if (!fallbacks.empty()) {
        out += " | fallback: " + fallbacks;
    }
    return out;
}

} // namespace fuse::renderer
