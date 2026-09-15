#pragma once

#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/renderer/atmosphere/sky_lut.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct SkyPassDesc {
    AtmosphereParams atmosphere{};
    math::Vec3 sun_direction{0.f, 0.3f, 1.f};
    math::Vec3 sun_color{1.f, 0.95f, 0.85f};
    f32 sun_intensity = 20.f;
    SkyLutDesc lut{};
};

struct SkyPassStats {
    bool ready = false;
    u32 framesRecorded = 0;
    bool lutReady = false;
    std::string message;
};

/// B5.8 atmosphere/sky pass scaffold — depth-tested sky fill via LUT (GPU deferred).
class SkyPass {
public:
    static std::unique_ptr<SkyPass> create(const SkyPassDesc& desc);

    bool isReady() const { return m_stats.ready; }
    const SkyPassStats& lastStats() const { return m_stats; }
    const SkyLut& lut() const { return m_lut; }

    bool recordFrame();

private:
    explicit SkyPass(const SkyPassDesc& desc);

    SkyPassDesc m_desc{};
    SkyPassStats m_stats{};
    SkyLut m_lut{};
};

void resetSkyPassGraphStorage();
void addSkyPassToGraph(RenderGraph& graph);

} // namespace fuse::renderer
