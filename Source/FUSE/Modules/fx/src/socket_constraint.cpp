#include <fuse/fx/socket_constraint.hpp>

namespace fuse::fx {

namespace {

ConstraintKind kindFromToken(const std::string& token) {
    if (token == "point" || token == "impact") {
        return ConstraintKind::Point;
    }
    if (token == "shape" || token == "caster" || token == "target") {
        return ConstraintKind::Shape;
    }
    if (token == "effect") {
        return ConstraintKind::Effect;
    }
    if (token == "scene") {
        return ConstraintKind::Scene;
    }
    return ConstraintKind::Undefined;
}

} // namespace

bool parse_constraint_spec(const std::string& spec, ConstraintDef& out_def) {
    if (spec.empty()) {
        return false;
    }

    const std::size_t colon = spec.find(':');
    const std::string kindToken = colon == std::string::npos ? spec : spec.substr(0, colon);
    out_def.kind = kindFromToken(kindToken);
    if (out_def.kind == ConstraintKind::Undefined) {
        return false;
    }

    out_def.source_name = spec;
    if (colon != std::string::npos && colon + 1 < spec.size()) {
        out_def.node_name = spec.substr(colon + 1);
    }
    return true;
}

bool SocketConstraintManager::defineConstraint(ConstraintKind kind, const std::string& name) {
    if (name.empty() || kind == ConstraintKind::Undefined) {
        return false;
    }

    ConstraintEntry entry;
    entry.def.kind = kind;
    entry.def.source_name = name;
    m_constraints[name] = entry;
    return true;
}

bool SocketConstraintManager::setReferencePoint(const std::string& name, const fuse::math::Vec3& point) {
    const auto it = m_constraints.find(name);
    if (it == m_constraints.end()) {
        return false;
    }

    it->second.point = point;
    it->second.object = Handle<Object>::invalid();
    return true;
}

bool SocketConstraintManager::setReferenceObject(const std::string& name, Handle<Object> object) {
    const auto it = m_constraints.find(name);
    if (it == m_constraints.end()) {
        return false;
    }

    it->second.object = object;
    return true;
}

bool SocketConstraintManager::hasConstraint(const std::string& name) const {
    return m_constraints.find(name) != m_constraints.end();
}

ConstraintPose SocketConstraintManager::sample(const std::string& name) const {
    ConstraintPose pose;
    const auto it = m_constraints.find(name);
    if (it == m_constraints.end()) {
        return pose;
    }

    pose.position = it->second.point;
    pose.valid = true;
    return pose;
}

bool SocketConstraintManager::remapSocket(FxSocket& socket, const std::string& constraint_name) const {
    const auto it = m_constraints.find(constraint_name);
    if (it == m_constraints.end()) {
        return false;
    }

    if (it->second.object.isValid()) {
        socket.owner = it->second.object;
        return true;
    }

    return it->second.def.kind == ConstraintKind::Point;
}

} // namespace fuse::fx
