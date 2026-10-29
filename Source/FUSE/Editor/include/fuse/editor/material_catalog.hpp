#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// One editor-authored material: a display name plus the authoring fields the Material Editor edits.
struct MaterialCatalogEntry {
    std::string name;
    MaterialEditState state{};
};

/// Per-scene material catalog backing the Material Editor dock (GUI-3). Ids are indices and match
/// `Mesh::material_id` / `SDFObject::material_id`. Persisted beside the scene as `<scene>.materials`
/// ("FUSEEDMAT 1" text). Qt-free and renderer-free; a renderer bridge can mirror it into a
/// `renderer::MaterialSystem` by id.
class MaterialCatalog {
public:
    MaterialCatalog() { resetToDefaults(); }

    /// Default palette: Default, Red Plastic, Gold, Chrome, Rubber (id 0 stays "Default").
    void resetToDefaults();
    [[nodiscard]] u32 count() const { return static_cast<u32>(m_entries.size()); }
    [[nodiscard]] bool valid(u32 id) const { return id < m_entries.size(); }
    [[nodiscard]] const MaterialCatalogEntry& entry(u32 id) const { return m_entries.at(id); }
    void setState(u32 id, const MaterialEditState& state);
    u32 add(MaterialCatalogEntry entry);
    void removeLast();
    /// Bumped on every change (panels poll it).
    [[nodiscard]] u32 revision() const { return m_revision; }

    bool save(const std::string& path, std::string* error = nullptr) const;
    /// Replaces the catalog; false (unchanged) on a missing or malformed file.
    bool load(const std::string& path, std::string* error = nullptr);
    static std::string sidecarPath(const std::string& scenePath) { return scenePath + ".materials"; }

private:
    std::vector<MaterialCatalogEntry> m_entries;
    u32 m_revision = 0;
};

/// Undoable edit of one catalog material; consecutive edits of the same id merge (slider drags).
class MaterialEditCommand final : public UndoCommand {
public:
    MaterialEditCommand(MaterialCatalog& catalog, u32 id, const MaterialEditState& before,
                        const MaterialEditState& after, std::string description = "Edit material")
        : m_catalog(catalog), m_id(id), m_before(before), m_after(after), m_description(std::move(description)) {}
    void execute() override { m_catalog.setState(m_id, m_after); }
    void undo() override { m_catalog.setState(m_id, m_before); }
    std::string description() const override { return m_description; }
    bool merge(const UndoCommand& other) override;

private:
    MaterialCatalog& m_catalog;
    u32 m_id;
    MaterialEditState m_before;
    MaterialEditState m_after;
    std::string m_description;
};

/// Undoable "New material" (appends; undo removes it again).
class AddMaterialCommand final : public UndoCommand {
public:
    AddMaterialCommand(MaterialCatalog& catalog, MaterialCatalogEntry entry)
        : m_catalog(catalog), m_entry(std::move(entry)) {}
    void execute() override { m_catalog.add(m_entry); }
    void undo() override { m_catalog.removeLast(); }
    std::string description() const override { return "New material '" + m_entry.name + "'"; }

private:
    MaterialCatalog& m_catalog;
    MaterialCatalogEntry m_entry;
};

/// Undoable material assignment to an entity's Mesh and / or SDFObject.
class AssignMaterialCommand final : public UndoCommand {
public:
    AssignMaterialCommand(ecs::Registry& registry, ecs::EntityID entity, u32 before, u32 after)
        : m_registry(registry), m_entity(entity), m_before(before), m_after(after) {}
    void execute() override { apply_(m_after); }
    void undo() override { apply_(m_before); }
    std::string description() const override { return "Assign material"; }

    /// Material id of the entity (SDFObject first, then Mesh); false when it has neither.
    static bool materialOf(const ecs::Registry& registry, ecs::EntityID entity, u32& id);

private:
    void apply_(u32 id);
    ecs::Registry& m_registry;
    ecs::EntityID m_entity;
    u32 m_before;
    u32 m_after;
};

} // namespace fuse::editor
