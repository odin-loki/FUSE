#pragma once
// Editor-side display names for ECS entities (the ECS has no Name component). Names persist in a
// `<scene>.names` sidecar (`fuse-names 1`, one `<index> <generation> <name>` per line) beside the scene.
#include <fuse/ecs/entity.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>

namespace fuse::editor {

class EntityNames {
public:
    [[nodiscard]] const std::string* find(ecs::EntityID id) const;
    [[nodiscard]] std::string get(ecs::EntityID id) const;
    /// Empty `name` removes the entry. Leading / trailing whitespace and newlines are stripped.
    void set(ecs::EntityID id, std::string name);
    void clear() { m_names.clear(); }
    [[nodiscard]] usize size() const { return m_names.size(); }

    bool save(const std::string& path) const; ///< writes nothing (and removes a stale file) when empty
    bool load(const std::string& path);       ///< clears first; false only on a malformed file
    static std::string sidecarPath(const std::string& scenePath) { return scenePath + ".names"; }

private:
    static u64 key(ecs::EntityID id) { return (static_cast<u64>(id.generation) << 32) | id.index; }
    std::unordered_map<u64, std::string> m_names;
};

/// Undoable rename (one undo step).
class RenameEntityCommand final : public UndoCommand {
public:
    RenameEntityCommand(EntityNames& names, ecs::EntityID entity, std::string after)
        : m_names(names), m_entity(entity), m_before(names.get(entity)), m_after(std::move(after)) {}
    void execute() override { m_names.set(m_entity, m_after); }
    void undo() override { m_names.set(m_entity, m_before); }
    std::string description() const override { return "Rename entity"; }

private:
    EntityNames& m_names;
    ecs::EntityID m_entity;
    std::string m_before;
    std::string m_after;
};

} // namespace fuse::editor
