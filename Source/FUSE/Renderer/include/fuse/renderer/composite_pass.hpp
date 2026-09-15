#pragma once

#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct CompositePassDesc {
    /// GRIA alpha — 0 = all CUDA, 1 = all raster (see P2 §2.9).
    float defaultBlend = 0.5f;
};

struct CompositePassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    float lastBlend = 0.f;
    std::string message;
};

/// B2.9 composite pass scaffold — merges raster (B2.8) + CUDA (B2.7) into the backbuffer before present.
class CompositePass {
public:
    static std::unique_ptr<CompositePass> create(const CompositePassDesc& desc);

    bool isReady() const { return m_stats.ready; }
    const CompositePassStats& lastStats() const { return m_stats; }

    /// Returns blend factor used for this frame (mirrors HybridComposer 3D→2D ordering inputs).
    float blendForFrame(const RenderCommandList& commands) const;

    /// Records logical composite work for the frame; returns false when not ready.
    bool recordFrame(const RenderCommandList& commands);

private:
    explicit CompositePass(const CompositePassDesc& desc);

    CompositePassDesc m_desc{};
    CompositePassStats m_stats{};
};

/// Inserts composite pass between 2D overlay work and present (HybridComposer order).
void resetCompositePassGraphStorage();
void addCompositePassToGraph(RenderGraph& graph, float blend);

} // namespace fuse::renderer
