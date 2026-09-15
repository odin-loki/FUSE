#pragma once

#include <fuse/fx/cast_pipeline.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>

namespace fuse::fx::bind {

/// Runtime substitution slot kind — ore analogue: AFX `do_runtime_substitutions` fields.
enum class ParameterKind : u8 {
    Float,
    Int,
    Bool,
    Handle,
};

struct ParameterValue {
    ParameterKind kind = ParameterKind::Float;
    float floatValue = 0.f;
    s32 intValue = 0;
    bool boolValue = false;
    Handle<Object> handleValue = Handle<Object>::invalid();
};

/// Named parameter table for cast/effect runtime substitution.
class ParameterBinder {
public:
    void bindFloat(const std::string& name, float value);
    void bindInt(const std::string& name, s32 value);
    void bindBool(const std::string& name, bool value);
    void bindHandle(const std::string& name, Handle<Object> value);

    bool has(const std::string& name) const;
    const ParameterValue* get(const std::string& name) const;
    u32 count() const { return static_cast<u32>(m_bindings.size()); }
    void clear();

    /// Resolve well-known cast slots (`caster`, `target`) into a `CastBinding`.
    CastBinding resolveCastBinding() const;

private:
    std::unordered_map<std::string, ParameterValue> m_bindings;
};

} // namespace fuse::fx::bind
