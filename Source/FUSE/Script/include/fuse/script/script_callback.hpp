#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <functional>

namespace fuse::script {

using ScriptCallbackId = u32;
static constexpr ScriptCallbackId kInvalidScriptCallback = 0;

enum class ScriptEventKind : u8 {
    OnStart,
    OnUpdate,
    OnDestroy,
    OnCollision,
    OnTriggerEnter,
};

struct ScriptCallbackContext {
    ecs::EntityID entity = ecs::EntityID::null();
    ecs::EntityID other = ecs::EntityID::null();
    f32 dt = 0.f;
};

using ScriptCallbackFn = std::function<void(const ScriptCallbackContext&)>;

} // namespace fuse::script
