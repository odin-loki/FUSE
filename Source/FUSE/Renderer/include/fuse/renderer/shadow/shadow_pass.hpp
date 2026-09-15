#pragma once

#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/shadow/directional_shadow.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct ShadowPassDesc {
    bool recordAtlasWrite = true;
};

struct ShadowPassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    std::string message;
};

/// Render-graph shadow pass scaffold — records directional CSM work (B5.5).
class ShadowPass {
public:
    static std::unique_ptr<ShadowPass> create(const ShadowPassDesc& desc = {});

    bool isReady() const { return m_stats.ready; }
    const ShadowPassStats& lastStats() const { return m_stats; }

    bool recordFrame(const DirectionalShadow& shadows, RenderGraph& graph);

private:
    explicit ShadowPass(const ShadowPassDesc& desc);

    ShadowPassDesc m_desc{};
    ShadowPassStats m_stats{};
};

/// Inserts the shadow_maps pass node with atlas depth write access.
void resetShadowPassGraphStorage();
void addShadowPassToGraph(RenderGraph& graph, const DirectionalShadow& shadows);

} // namespace fuse::renderer
