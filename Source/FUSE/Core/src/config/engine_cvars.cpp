#include <fuse/config/engine_cvars.hpp>

namespace fuse::config {

EngineCVars register_engine_cvars(CVarRegistry& registry) {
    EngineCVars cvars{
        CVarEnum("r.tier", {"auto", "T0", "T1", "T2", "T3"}, 0,
                 "Renderer feature tier (auto = highest the device supports)",
                 CVarFlags::Archive | CVarFlags::RequiresRestart, registry),
        CVar<std::string>("r.upscaler", "auto", "Upscaler id: auto, off, fsr1, nis, cas, taau or a plugin id",
                          CVarFlags::Archive, registry),
        CVar<bool>("r.vsync", true, "Present with vertical sync", CVarFlags::Archive, registry),
        CVar<f32>("r.renderScale", 1.f, 0.25f, 2.f, "Render resolution / display resolution", CVarFlags::Archive,
                  registry),
        CVar<f32>("a.masterVolume", 1.f, 0.f, 1.f, "Master output volume", CVarFlags::Archive, registry),
        CVar<f32>("in.mouseSensitivity", 1.f, 0.01f, 20.f, "Mouse look sensitivity", CVarFlags::Archive, registry),
    };
    return cvars;
}

const EngineCVars& engine_cvars() {
    static const EngineCVars s_cvars = register_engine_cvars(CVarRegistry::global());
    return s_cvars;
}

} // namespace fuse::config
