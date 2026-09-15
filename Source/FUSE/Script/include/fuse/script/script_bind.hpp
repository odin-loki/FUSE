#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::script::bind {

/// Lua-ready tagged value used by bind helpers until a real Lua stack is wired.
enum class ScriptValueKind : u8 {
    Nil,
    Bool,
    Number,
    String,
    EntityId,
    Transform,
};

struct ScriptValue {
    ScriptValueKind kind = ScriptValueKind::Nil;
    bool bool_value = false;
    f64 number_value = 0.0;
    std::string string_value;
    ecs::EntityID entity_id = ecs::EntityID::null();
    ecs::Transform transform{};
};

[[nodiscard]] ScriptValue push_entity_id(ecs::EntityID id);
[[nodiscard]] ecs::EntityID to_entity_id(const ScriptValue& value);
[[nodiscard]] bool is_entity_id(const ScriptValue& value);

[[nodiscard]] ScriptValue push_transform(const ecs::Transform& transform);
[[nodiscard]] ecs::Transform to_transform(const ScriptValue& value);
[[nodiscard]] bool is_transform(const ScriptValue& value);

} // namespace fuse::script::bind
