#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>

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

[[nodiscard]] ScriptValue push_nil();
[[nodiscard]] bool is_nil(const ScriptValue& value);

[[nodiscard]] ScriptValue push_bool(bool value);
[[nodiscard]] bool to_bool(const ScriptValue& value, bool default_value = false);
[[nodiscard]] bool is_bool(const ScriptValue& value);

[[nodiscard]] ScriptValue push_number(f64 value);
[[nodiscard]] f64 to_number(const ScriptValue& value, f64 default_value = 0.0);
[[nodiscard]] bool is_number(const ScriptValue& value);

[[nodiscard]] ScriptValue push_string(const char* value);
[[nodiscard]] ScriptValue push_string(const std::string& value);
[[nodiscard]] const std::string& to_string(const ScriptValue& value);
[[nodiscard]] bool is_string(const ScriptValue& value);

[[nodiscard]] ScriptValue push_entity_id(ecs::EntityID id);
[[nodiscard]] ecs::EntityID to_entity_id(const ScriptValue& value);
[[nodiscard]] bool is_entity_id(const ScriptValue& value);

[[nodiscard]] ScriptValue push_transform(const ecs::Transform& transform);
[[nodiscard]] ecs::Transform to_transform(const ScriptValue& value);
[[nodiscard]] bool is_transform(const ScriptValue& value);

[[nodiscard]] const char* kind_name(ScriptValueKind kind);

/// Deep equality for tagged values (kind + payload).
[[nodiscard]] bool values_equal(const ScriptValue& lhs, const ScriptValue& rhs);

/// Named property table for script instance data (Lua-ready stub until ECS `Script` lands).
class PropertyStore {
public:
    void set_property(const std::string& name, const ScriptValue& value);
    [[nodiscard]] bool has_property(const std::string& name) const;
    [[nodiscard]] const ScriptValue* get_property(const std::string& name) const;
    [[nodiscard]] ScriptValue get_property_or(const std::string& name,
                                              const ScriptValue& default_value) const;
    bool remove_property(const std::string& name);
    void clear();
    [[nodiscard]] usize count() const { return m_properties.size(); }

private:
    std::unordered_map<std::string, ScriptValue> m_properties;
};

using ScriptMethodFn = std::function<ScriptValue(const ScriptValue* args, usize argc)>;

/// Optional named method table for script-facing API stubs.
class MethodTable {
public:
    void register_method(const std::string& name, ScriptMethodFn fn);
    bool unregister_method(const std::string& name);
    [[nodiscard]] bool has_method(const std::string& name) const;
    [[nodiscard]] ScriptValue invoke(const std::string& name,
                                     const ScriptValue* args = nullptr,
                                     usize argc = 0) const;
    void clear();
    [[nodiscard]] usize count() const { return m_methods.size(); }

private:
    std::unordered_map<std::string, ScriptMethodFn> m_methods;
};

} // namespace fuse::script::bind
