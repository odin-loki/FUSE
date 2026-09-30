#include <fuse/world2d/scene_handle_table.hpp>

#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse {

SceneHandleTable::~SceneHandleTable() {
    // Nodes that outlive the table must not call back into it; their handles become invalid.
    for (u32 index = 0; index < static_cast<u32>(m_liveGeneration.size()); ++index) {
        const u32 generation = m_liveGeneration[index];
        if (generation == 0u) {
            continue;
        }
        SceneObject2D** slot = m_table.get(Handle<SceneObject2D*>(index, generation));
        if (slot != nullptr && *slot != nullptr) {
            (*slot)->m_handleTable = nullptr;
            (*slot)->setHandle(Handle<Object>::invalid());
        }
    }
}

Handle<Object> SceneHandleTable::publish(SceneObject2D& node) {
    if (node.m_handleTable == this && valid(node.handle())) {
        return node.handle();
    }
    if (node.m_handleTable != nullptr) {
        node.m_handleTable->unpublish(node);
    }
    SceneObject2D* pointer = &node;
    const Handle<SceneObject2D*> slot = m_table.insert(std::move(pointer));
    if (slot.index() >= m_liveGeneration.size()) {
        m_liveGeneration.resize(static_cast<usize>(slot.index()) + 1u, 0u);
    }
    m_liveGeneration[slot.index()] = slot.generation();
    const Handle<Object> handle(slot.index(), slot.generation());
    node.m_handleTable = this;
    node.setHandle(handle);
    return handle;
}

void SceneHandleTable::unpublish(SceneObject2D& node) {
    if (node.m_handleTable != this) {
        return;
    }
    const Handle<Object> handle = node.handle();
    if (valid(handle)) {
        m_table.remove(toSlot_(handle));
        m_liveGeneration[handle.index()] = 0u;
    }
    node.m_handleTable = nullptr;
    node.setHandle(Handle<Object>::invalid());
}

void SceneHandleTable::onNodeDestroyed_(SceneObject2D& node) {
    unpublish(node);
}

SceneObject2D* SceneHandleTable::resolve(Handle<Object> handle) const {
    SceneObject2D* const* slot = m_table.get(toSlot_(handle));
    return slot != nullptr ? *slot : nullptr;
}

bool SceneHandleTable::valid(Handle<Object> handle) const {
    return m_table.valid(toSlot_(handle));
}

} // namespace fuse
