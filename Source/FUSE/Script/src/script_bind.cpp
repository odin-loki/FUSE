#include <fuse/script/script_bind.hpp>

namespace fuse::script::bind {

ScriptValue push_nil() {
    ScriptValue value;
    value.kind = ScriptValueKind::Nil;
    return value;
}

bool is_nil(const ScriptValue& value) {
    return value.kind == ScriptValueKind::Nil;
}

ScriptValue push_bool(bool value) {
    ScriptValue out;
    out.kind = ScriptValueKind::Bool;
    out.bool_value = value;
    return out;
}

bool to_bool(const ScriptValue& value, bool default_value) {
    if (!is_bool(value)) {
        return default_value;
    }
    return value.bool_value;
}

bool is_bool(const ScriptValue& value) {
    return value.kind == ScriptValueKind::Bool;
}

ScriptValue push_number(f64 value) {
    ScriptValue out;
    out.kind = ScriptValueKind::Number;
    out.number_value = value;
    return out;
}

f64 to_number(const ScriptValue& value, f64 default_value) {
    if (!is_number(value)) {
        return default_value;
    }
    return value.number_value;
}

bool is_number(const ScriptValue& value) {
    return value.kind == ScriptValueKind::Number;
}

ScriptValue push_string(const char* value) {
    ScriptValue out;
    out.kind = ScriptValueKind::String;
    out.string_value = (value != nullptr) ? value : "";
    return out;
}

ScriptValue push_string(const std::string& value) {
    return push_string(value.c_str());
}

const std::string& to_string(const ScriptValue& value) {
    static const std::string kEmpty;
    if (!is_string(value)) {
        return kEmpty;
    }
    return value.string_value;
}

bool is_string(const ScriptValue& value) {
    return value.kind == ScriptValueKind::String;
}

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

const char* kind_name(ScriptValueKind kind) {
    switch (kind) {
    case ScriptValueKind::Nil:
        return "nil";
    case ScriptValueKind::Bool:
        return "bool";
    case ScriptValueKind::Number:
        return "number";
    case ScriptValueKind::String:
        return "string";
    case ScriptValueKind::EntityId:
        return "entity_id";
    case ScriptValueKind::Transform:
        return "transform";
    default:
        return "unknown";
    }
}

bool values_equal(const ScriptValue& lhs, const ScriptValue& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }

    switch (lhs.kind) {
    case ScriptValueKind::Nil:
        return true;
    case ScriptValueKind::Bool:
        return lhs.bool_value == rhs.bool_value;
    case ScriptValueKind::Number:
        return lhs.number_value == rhs.number_value;
    case ScriptValueKind::String:
        return lhs.string_value == rhs.string_value;
    case ScriptValueKind::EntityId:
        return lhs.entity_id == rhs.entity_id;
    case ScriptValueKind::Transform:
        return lhs.transform.position.x == rhs.transform.position.x &&
               lhs.transform.position.y == rhs.transform.position.y &&
               lhs.transform.position.z == rhs.transform.position.z &&
               lhs.transform.rotation.x == rhs.transform.rotation.x &&
               lhs.transform.rotation.y == rhs.transform.rotation.y &&
               lhs.transform.rotation.z == rhs.transform.rotation.z &&
               lhs.transform.rotation.w == rhs.transform.rotation.w &&
               lhs.transform.scale.x == rhs.transform.scale.x &&
               lhs.transform.scale.y == rhs.transform.scale.y &&
               lhs.transform.scale.z == rhs.transform.scale.z &&
               lhs.transform.dirty == rhs.transform.dirty &&
               lhs.transform.parent == rhs.transform.parent;
    default:
        return false;
    }
}

} // namespace fuse::script::bind
