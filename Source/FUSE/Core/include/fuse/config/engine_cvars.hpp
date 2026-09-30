#pragma once

// GAP-CVAR: the engine-wide cvars declared by Core. Consumers bind to them in their own
// packages (runtime: r.* / a.*; input layer: in.mouseSensitivity; console: all of them).

#include <fuse/config/cvar.hpp>

namespace fuse::config {

struct EngineCVars {
    /// r.tier — renderer feature tier ("auto" picks the highest the device supports). Archive,
    /// RequiresRestart (pipelines / device features are chosen at start-up).
    CVarEnum renderTier;
    /// r.upscaler — upscaler registry id ("auto", "off", "fsr1", "nis", "cas", "taau", or a plugin id).
    CVar<std::string> upscaler;
    /// r.vsync — present with vertical sync.
    CVar<bool> vsync;
    /// r.renderScale — internal render resolution / display resolution, 0.25 .. 2.
    CVar<f32> renderScale;
    /// a.masterVolume — master output gain, 0 .. 1.
    CVar<f32> masterVolume;
    /// in.mouseSensitivity — scale applied to mouse look deltas by the action map, 0.01 .. 20.
    CVar<f32> mouseSensitivity;
};

/// Register (idempotent) the engine cvars in `registry` and return handles to them.
EngineCVars register_engine_cvars(CVarRegistry& registry);

/// The engine cvars in CVarRegistry::global() (registered on first call).
const EngineCVars& engine_cvars();

} // namespace fuse::config
