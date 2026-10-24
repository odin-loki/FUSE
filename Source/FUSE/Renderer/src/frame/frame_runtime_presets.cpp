#include <fuse/renderer/frame/frame_runtime_presets.hpp>

namespace fuse::renderer::frame {

void applyLavapipeComposerDefaults(FrameComposerDesc& d) {
    d.clusters = ClusterDesc{};
    d.vsmClipmap.levels = 12;
    d.vsmClipmap.firstLevelExtent = 4.f;
    d.vsmClipmap.markRadiusTexels = 2.5f;
    d.vsmClipmap.texelsPerPixel = 2.f;
    d.vsmPoolPagesX = 16;
    d.vsmPoolPagesY = 16;
    DDGIDesc& v = d.ddgiVolume;
    v.grid_origin = {-6.f, 0.25f, -8.f};
    v.probe_spacing = {3.f, 1.5f, 3.f};
    v.grid_dims = {5u, 3u, 5u};
    v.rays_per_probe = 64;
    v.probes_per_frame = 75;
    v.irradiance_res = 6;
    v.depth_res = 8;
    v.hysteresis = 0.9f;
    v.max_ray_distance = 20.f;
    d.maxSplats = 64;
    d.maxSplatEntries = 1u << 14;
    d.frameGen = false;
}

void applyLavapipeTier0FrameSettings(FrameSettings& fs, u32 renderWidth, u32 renderHeight) {
    fs = FrameSettings{};
    fs.vsmFilter.pcfRadius = 1;
    fs.ssfxSettings.ssgi_params.sample_sqrt = 2;
    fs.atmosphere.sizes.transWidth = 64;
    fs.atmosphere.sizes.transHeight = 32;
    fs.atmosphere.sizes.skyWidth = 64;
    fs.atmosphere.sizes.skyHeight = 48;
    fs.atmosphere.sizes.apWidth = 16;
    fs.atmosphere.sizes.apHeight = 16;
    fs.atmosphere.sizes.apDepth = 16;
    fs.atmosphere.sampling.transSteps = 32;
    fs.atmosphere.sampling.msDirSqrt = 4;
    fs.atmosphere.sampling.skySteps = 16;
    fs.atmosphere.ozoneAbsorption = {0.650e-6f, 1.881e-6f, 0.085e-6f};
    fs.fogSettings.gridX = 24;
    fs.fogSettings.gridY = 16;
    fs.fogSettings.gridZ = 24;
    fs.fogSettings.farPlane = 30.f;
    fs.fogSettings.medium.density = 0.04f;
    fs.fogSettings.ambient[0] = 0.02f;
    fs.fogSettings.ambient[1] = 0.025f;
    fs.fogSettings.ambient[2] = 0.03f;
    clouds::CloudSettings& c = fs.cloudSettings;
    c.noise.shapeSize = 32;
    c.noise.detailSize = 16;
    c.noise.weatherSize = 32;
    if (renderWidth != 0u && renderHeight != 0u) {
        c.resolution.width = renderWidth;
        c.resolution.height = renderHeight;
    }
    c.resolution.block = 2;
    c.sampling.primarySteps = 24;
    c.sampling.lightSteps = 3;
    c.medium.coverageBias = 0.2f;
    fs.splats = false;
    fs.restir = false;
    fs.frameGen = false;
    fs.upscaler = FrameUpscaler::None;
    fs.ui = true;
}

} // namespace fuse::renderer::frame
