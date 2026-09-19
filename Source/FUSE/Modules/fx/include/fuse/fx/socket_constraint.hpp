#pragma once

// Ore: Engine/source/afx/afxConstraint.h (constraint defs + reference remapping)
//      third_party/addons/AFX-Template/ (spell socket specs)

#include <fuse/fx/fx_defs.hpp>
#include <fuse/fx/fx_socket.hpp>
#include <fuse/handle.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/object.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::fx {

enum class ConstraintKind {
    Undefined,
    Point,
    Shape,
    Effect,
    Scene,
};

/// Parsed constraint spec (ore: afxConstraintDef without StringTableEntry).
struct ConstraintDef {
    ConstraintKind kind = ConstraintKind::Undefined;
    std::string source_name;
    std::string node_name;
    float history_time = 0.f;
    u8 sample_rate = 0;
    bool runs_on_server = true;
    bool runs_on_client = true;
};

struct ConstraintPose {
    fuse::math::Vec3 position{};
    bool valid = false;
};

/// Runtime constraint binding (ore: afxConstraintMgr reference table).
class SocketConstraintManager {
public:
    bool defineConstraint(ConstraintKind kind, const std::string& name);
    bool setReferencePoint(const std::string& name, const fuse::math::Vec3& point);
    bool setReferenceObject(const std::string& name, Handle<Object> object);
    bool remapSocket(FxSocket& socket, const std::string& constraint_name) const;

    bool hasConstraint(const std::string& name) const;
    ConstraintPose sample(const std::string& name) const;

    u32 constraintCount() const { return static_cast<u32>(m_constraints.size()); }

private:
    struct ConstraintEntry {
        ConstraintDef def;
        fuse::math::Vec3 point{};
        Handle<Object> object = Handle<Object>::invalid();
    };

    std::unordered_map<std::string, ConstraintEntry> m_constraints;
};

/// Parse `caster`, `target`, `impact` style constraint tokens from AFX spell specs.
bool parse_constraint_spec(const std::string& spec, ConstraintDef& out_def);

} // namespace fuse::fx
