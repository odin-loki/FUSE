#include <fuse/fx/parameter_bind.hpp>

namespace fuse::fx::bind {

void ParameterBinder::bindFloat(const std::string& name, float value) {
    ParameterValue slot;
    slot.kind = ParameterKind::Float;
    slot.floatValue = value;
    m_bindings[name] = slot;
}

void ParameterBinder::bindInt(const std::string& name, s32 value) {
    ParameterValue slot;
    slot.kind = ParameterKind::Int;
    slot.intValue = value;
    m_bindings[name] = slot;
}

void ParameterBinder::bindBool(const std::string& name, bool value) {
    ParameterValue slot;
    slot.kind = ParameterKind::Bool;
    slot.boolValue = value;
    m_bindings[name] = slot;
}

void ParameterBinder::bindHandle(const std::string& name, Handle<Object> value) {
    ParameterValue slot;
    slot.kind = ParameterKind::Handle;
    slot.handleValue = value;
    m_bindings[name] = slot;
}

bool ParameterBinder::has(const std::string& name) const {
    return m_bindings.find(name) != m_bindings.end();
}

const ParameterValue* ParameterBinder::get(const std::string& name) const {
    const auto it = m_bindings.find(name);
    return (it != m_bindings.end()) ? &it->second : nullptr;
}

void ParameterBinder::clear() {
    m_bindings.clear();
}

CastBinding ParameterBinder::resolveCastBinding() const {
    CastBinding binding;

    if (const ParameterValue* caster = get("caster")) {
        if (caster->kind == ParameterKind::Handle) {
            binding.caster = caster->handleValue;
        }
    }

    if (const ParameterValue* target = get("target")) {
        if (target->kind == ParameterKind::Handle) {
            binding.target = target->handleValue;
        }
    }

    return binding;
}

} // namespace fuse::fx::bind
