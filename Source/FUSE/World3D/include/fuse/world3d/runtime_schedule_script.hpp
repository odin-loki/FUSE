#pragma once

// Header-only glue: runs a fuse::script::ScriptSystem in the RuntimeSchedule's Scripts stage.
// Include it only from targets that link fuse_script (World3D itself does not depend on Lua).

#include <fuse/script/script_system.hpp>
#include <fuse/world3d/runtime_schedule.hpp>

namespace fuse::world3d {

inline void bindScriptSystem(RuntimeSchedule& schedule, script::ScriptSystem& scripts) {
    schedule.setHook(
        RuntimeStage::Scripts,
        [](void* user, ecs::Registry&, f32 dt) { static_cast<script::ScriptSystem*>(user)->update(dt); },
        &scripts);
}

} // namespace fuse::world3d
