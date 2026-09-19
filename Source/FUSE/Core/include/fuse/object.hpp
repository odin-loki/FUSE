#pragma once

#include <fuse/handle.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse {

/// Shared root for greenfield scene types (replaces dual SimObject over time).
///
/// Threading: scene graph mutation is game-thread only. Workers must not hold
/// raw Object* across job boundaries — publish fuse::Handle<Object> instead.
class Object {
public:
    Object();
    explicit Object(std::string name);
    virtual ~Object();

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    const std::string& name() const { return m_name; }
    void setName(std::string name);

    Object* parent() const { return m_parent; }
    const std::vector<Object*>& children() const { return m_children; }

    void addChild(Object* child);
    void removeChild(Object* child);
    /// Detach from current parent and attach under newParent (nullptr unparents).
    void reparent(Object* newParent);

    Handle<Object> handle() const { return m_handle; }

    /// Stable id from quarantined legacy SimObject/SceneObject (0 = none).
    u32 legacyId() const { return m_legacyId; }
    void setLegacyId(u32 id) { m_legacyId = id; }

    /// Torque class tag (e.g. StaticShape) — distinct from greenfield `typeName()`.
    const std::string& legacyClassName() const { return m_legacyClassName; }
    void setLegacyClassName(std::string name) { m_legacyClassName = std::move(name); }

    /// Torque internal/object name when different from display `name()`.
    const std::string& legacyInternalName() const { return m_legacyInternalName; }
    void setLegacyInternalName(std::string name) { m_legacyInternalName = std::move(name); }

    /// Parent SimObject internal name — hierarchy wiring deferred; stored for round-trip.
    const std::string& legacyParentName() const { return m_legacyParentName; }
    void setLegacyParentName(std::string name) { m_legacyParentName = std::move(name); }

    virtual const char* typeName() const { return "Object"; }

protected:
    void setHandle(Handle<Object> handle) { m_handle = handle; }

private:
    std::string m_name;
    Object* m_parent = nullptr;
    std::vector<Object*> m_children;
    Handle<Object> m_handle = Handle<Object>::invalid();
    u32 m_legacyId = 0;
    std::string m_legacyClassName;
    std::string m_legacyInternalName;
    std::string m_legacyParentName;
};

} // namespace fuse
