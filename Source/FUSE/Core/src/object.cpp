#include <fuse/object.hpp>

#include <algorithm>

namespace fuse {

Object::Object() : m_name("Object") {}

Object::Object(std::string name) : m_name(std::move(name)) {}

Object::~Object() {
    while (!m_children.empty()) {
        removeChild(m_children.back());
    }
    if (m_parent) {
        m_parent->removeChild(this);
    }
}

void Object::setName(std::string name) {
    m_name = std::move(name);
}

void Object::addChild(Object* child) {
    if (!child || child == this) {
        return;
    }
    if (child->m_parent == this) {
        return;
    }
    if (child->m_parent) {
        child->m_parent->removeChild(child);
    }
    child->m_parent = this;
    m_children.push_back(child);
}

void Object::removeChild(Object* child) {
    if (!child) {
        return;
    }
    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it == m_children.end()) {
        return;
    }
    m_children.erase(it);
    child->m_parent = nullptr;
}

} // namespace fuse
