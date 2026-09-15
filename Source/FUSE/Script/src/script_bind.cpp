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

} // namespace fuse::script::bind
