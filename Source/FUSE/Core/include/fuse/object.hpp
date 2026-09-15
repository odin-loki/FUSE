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

    virtual const char* typeName() const { return "Object"; }

protected:
    void setHandle(Handle<Object> handle) { m_handle = handle; }

private:
    std::string m_name;
    Object* m_parent = nullptr;
    std::vector<Object*> m_children;
    Handle<Object> m_handle = Handle<Object>::invalid();
};

} // namespace fuse
