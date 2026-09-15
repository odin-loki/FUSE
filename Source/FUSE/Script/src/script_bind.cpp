#include <fuse/script/script_bind.hpp>

namespace fuse::script::bind {

ScriptValue push_entity_id(ecs::EntityID id) {
    ScriptValue value;
    value.kind = ScriptValueKind::EntityId;
    value.entity_id = id;
    return value;
}

ecs::EntityID to_entity_id(const ScriptValue& value) {
    if (!is_entity_id(value)) {
        return ecs::EntityID::null();
    }
    return value.entity_id;
}

bool is_entity_id(const ScriptValue& value) {
    return value.kind == ScriptValueKind::EntityId;
}

ScriptValue push_transform(const ecs::Transform& transform) {
    ScriptValue value;
    value.kind = ScriptValueKind::Transform;
    value.transform = transform;
    return value;
}

ecs::Transform to_transform(const ScriptValue& value) {
    if (!is_transform(value)) {
        return {};
    }
    return value.transform;
}

bool is_transform(const ScriptValue& value) {
    return value.kind == ScriptValueKind::Transform;
}

} // namespace fuse::script::bind
