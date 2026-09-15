#pragma once

#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Deferred frame pass identifiers — mirrors P5 §5.1 full frame pipeline.
enum class DeferredPassId : u8 {
    DepthPrepass = 0,
    GBuffer = 1,
    ShadowMaps = 2,
    VkToCudaSignal = 3,
    SdfRayMarch = 4,
    DdgiProbeUpdate = 5,
    ClusteredLightCull = 6,
    DeferredShading = 7,
    SdfShadows = 8,
    ScreenSpaceAo = 9,
    CudaToVkSignal = 10,
    TransparentPass = 11,
    AtmosphereSky = 12,
    TaaResolve = 13,
    PostProcessStack = 14,
    UiCompositePresent = 15,
    Count,
};

struct DeferredFramePipelineDesc {
    u32 width = 1920;
    u32 height = 1080;
    bool reversedZ = true;
};

struct DeferredFramePipelineStats {
    bool ready = false;
    u32 passCount = 0;
    u32 vulkanPassCount = 0;
    u32 cudaPassCount = 0;
    u32 barrierCount = 0;
};

/// B5.1 deferred pass schedule — populates a RenderGraph with the Phase 5 frame pipeline.
class DeferredFramePipeline {
public:
    explicit DeferredFramePipeline(const DeferredFramePipelineDesc& desc = {});

    void resetGraphStorage();
    void buildGraph(RenderGraph& graph, const GBuffer& gbuffer);

    static const char* passName(DeferredPassId id);
    static u32 passCount() { return static_cast<u32>(DeferredPassId::Count); }

    const DeferredFramePipelineStats& lastStats() const { return m_stats; }

private:
    DeferredFramePipelineDesc m_desc{};
    DeferredFramePipelineStats m_stats{};
};

} // namespace fuse::renderer
