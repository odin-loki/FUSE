#pragma once

// UNI-U7-MIS-1: level-wide environment settings as ECS components, so a converted Torque level
// (Skylight, SkyBox / ScatterSky, LevelInfo) carries them in its `.fuselevel` v3 ECS block and the
// runtime / editor read them like any other component. One entity per source object; a level
// normally has at most one of each. Plain data only (u32 flags instead of bool: serialised bytes
// carry no padding).

#include <fuse/asset/asset_id.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {

/// Ambient / sky light (T3D `Skylight`): constant ambient term plus the DDGI switch the renderer's
/// diffuse GI uses for indirect sky light.
struct AmbientLight {
    static constexpr const char* component_name = "AmbientLight";

    vec3 color = {1.f, 1.f, 1.f, 0.f};
    f32 intensity = 1.f;
    /// 1 = indirect diffuse from the DDGI probe volume, 0 = constant ambient only.
    u32 use_ddgi = 1u;
    /// DDGI probe spacing in metres.
    f32 ddgi_probe_spacing = 2.f;
    f32 reserved = 0.f;
};

/// Sky / atmosphere settings (T3D `SkyBox` = cubemap sky, `ScatterSky` = physically based scattering).
struct SkyAtmosphere {
    static constexpr const char* component_name = "SkyAtmosphere";

    enum Mode : u32 {
        Cubemap = 0,
        Scattering = 1,
    };

    u32 mode = Cubemap;
    u32 reserved = 0u;
    /// Cubemap sky material (cooked `.fusemat`); invalid for Scattering.
    fuse::asset::AssetId sky_material{};
    f32 sky_brightness = 25.f;
    f32 rayleigh_scattering = 0.0035f;
    f32 mie_scattering = 0.0045f;
    f32 sun_scale = 1.f;
    f32 exposure = 1.f;
    f32 reserved2 = 0.f;
};

/// Distance fog + camera clip (T3D `LevelInfo`).
struct EnvironmentFog {
    static constexpr const char* component_name = "EnvironmentFog";

    vec3 color = {0.6f, 0.6f, 0.7f, 0.f};
    /// Linear clear colour behind the scene (LevelInfo canvasClearColor).
    vec3 clear_color = {0.f, 0.f, 0.f, 0.f};
    f32 density = 0.f;
    /// Distance before fog starts (metres).
    f32 density_offset = 0.f;
    /// Fog height above the ground (LevelInfo fogAtmosphereHeight, 0 = uniform).
    f32 atmosphere_height = 0.f;
    /// Far clip / visible distance and near clip (metres).
    f32 visible_distance = 1000.f;
    f32 near_clip = 0.1f;
    f32 reserved[3] = {0.f, 0.f, 0.f};
};

} // namespace fuse::ecs
