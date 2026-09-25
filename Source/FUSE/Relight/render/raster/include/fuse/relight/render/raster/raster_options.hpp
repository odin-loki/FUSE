// FUSE Relight RL-4.2: raster remaster options (RL-0.6 registry). relight.frame.mode = raster turns the remaster on
// (render/frame/frame_options.hpp); these tune it.
//
//   relight.raster.tier           auto | t0 | t1 | t2 (env FUSE_RELIGHT_RASTER_TIER, default auto): the highest
//                                 raster tier to render; the effective tier is min(this, the adopted device's
//                                 RendererCaps tier, FUSE_RENDER_TIER_MAX). See raster_scene.hpp "Tiers".
//   relight.raster.ambient        linear ambient radiance added to every lit surface (x albedo).
//   relight.raster.exposure       scale of every light's contribution (default pi: a D3D light of colour c at normal
//                                 incidence lights a white Lambertian surface to c, as the fixed-function pipeline
//                                 did; 1 = physical units, the Remix path tracer's before its tonemapper).
//   relight.raster.fog            apply the D3D fog state (linear / exp / exp2) in the deferred and forward passes.
//   relight.raster.decals         draw Decal-category draws in the decal pass (else as opaque geometry).
//   relight.raster.shadowMapSize  directional shadow map resolution (T1+).
//   rtx.fallbackLightMode         0 never, 1 when the frame has no light (Remix default), 2 always: a distant light
//   rtx.fallbackLightRadiance     of this radiance (used as the illuminance at normal incidence) travelling along
//   rtx.fallbackLightDirection    this direction (Remix rtx_options.h defaults).
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

#include <cstdint>
#include <string>

namespace fuse::relight::render::raster {

struct RasterOptions {
    using Vec3f = options::Vec3f;
    FUSE_RELIGHT_OPTION_ENV("relight.raster", std::string, tier, "auto", "FUSE_RELIGHT_RASTER_TIER",
                            "RL-4.2 raster remaster tier cap: auto, t0, t1 or t2 (also capped by the device tier and "
                            "FUSE_RENDER_TIER_MAX).");
    FUSE_RELIGHT_OPTION("relight.raster", Vec3f, ambient, Vec3f(0.03f, 0.03f, 0.035f),
                        "Linear ambient radiance added to every lit surface of the raster remaster (x albedo).");
    FUSE_RELIGHT_OPTION("relight.raster", float, exposure, 3.14159265f,
                        "Scale of every light's contribution in the raster remaster (pi: legacy-equivalent brightness).");
    FUSE_RELIGHT_OPTION("relight.raster", bool, fog, true, "Apply the D3D fog state in the raster remaster.");
    FUSE_RELIGHT_OPTION("relight.raster", bool, decals, true,
                        "Draw Decal-category draws in the raster remaster's decal pass (else as opaque geometry).");
    FUSE_RELIGHT_OPTION("relight.raster", int, shadowMapSize, 512,
                        "Directional shadow map resolution of the raster remaster (tier T1 and above).");
    FUSE_RELIGHT_OPTION("rtx", int, fallbackLightMode, 1,
                        "Fallback light: 0 never, 1 when the frame has no light, 2 always (raster remaster: a distant "
                        "light).");
    FUSE_RELIGHT_OPTION("rtx", Vec3f, fallbackLightRadiance, Vec3f(1.6f, 1.8f, 2.0f),
                        "Radiance of the fallback light (raster remaster: illuminance at normal incidence).");
    FUSE_RELIGHT_OPTION("rtx", Vec3f, fallbackLightDirection, Vec3f(-0.2f, -1.0f, 0.4f),
                        "Direction the fallback distant light travels.");
};

/// References every option above (static libraries: keeps the registrations linked).
void registerRasterOptions();

/// The options above, resolved now.
struct RasterConfig {
    int tier = -1;                       ///< -1 auto, else 0..2
    float ambient[3] = {0.03f, 0.03f, 0.035f};
    float exposure = 3.14159265f;
    bool fog = true;
    bool decals = true;
    std::uint32_t shadowMapSize = 512;
    std::uint32_t fallbackLightMode = 1;
    float fallbackRadiance[3] = {1.6f, 1.8f, 2.0f};
    float fallbackDirection[3] = {-0.2f, -1.0f, 0.4f};

    static RasterConfig fromOptions();
};

/// "auto" / "t0".."t2" / "0".."2" (case-insensitive) -> -1 / 0..2. False for anything else.
bool parseRasterTier(const std::string& text, int& out);

} // namespace fuse::relight::render::raster
