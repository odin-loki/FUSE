#pragma once
// Q6 action table (FUSE_EDITOR_QT_FIRST_PLAN.md §3 R4): every menu item, toolbar button and shortcut of the editor
// as data. Both shells build their menus from this table; the FUSE UI shell (fuse_editor_fuse) does today, the Qt
// shell keeps its objectName strings (`qtObjectName`) so the twin gates can address the same action in either shell.
#include <fuse/types.hpp>

#include <span>
#include <string_view>

namespace fuse::editor {

enum class ActionId : u16 {
    FileNewScene, FileOpenScene, FileSave, FileSaveAs, FileNewProject, FileOpenProject, FileQuit,
    EditUndo, EditRedo, EditPreferences,
    EntityCreateEmpty, EntityCreateCube, EntityCreateSdfSphere, EntityCreateSdfBox, EntityCreatePointLight,
    EntityCreateDirectionalLight, EntityCreateCamera, EntityDuplicate, EntityDelete, EntityGroup, EntityUngroup,
    EntityOpenScript,
    PlayPlay, PlayPause, PlayResume, PlayStep, PlayStop,
    ViewResetLayout, ViewModeLit, ViewModeUnlit, ViewModeWireframe, ViewModeLitWireframe, ViewShowMeshBounds,
    ViewShowColliders, ViewFrameSelected,
    Count
};

struct ActionDesc {
    ActionId id = ActionId::Count;
    const char* stableId = "";     ///< "file.new_scene" (shortcut map / scripting key)
    const char* menu = "";         ///< top-level menu title ("File", "Edit", "Entity", "Play", "View")
    const char* label = "";        ///< menu text (string-table key later, Q12)
    const char* shortcut = "";     ///< default shortcut ("Ctrl+N"), empty for none
    const char* qtObjectName = ""; ///< QAction::objectName in the Qt shell (twin gates)
    bool separatorBefore = false;
    bool toolbar = false;          ///< also on the main / transport toolbar
};

[[nodiscard]] std::span<const ActionDesc> editorActions();
[[nodiscard]] const ActionDesc& actionDesc(ActionId id);
[[nodiscard]] const ActionDesc* findAction(std::string_view stableIdOrQtName);
/// Menu titles in menu-bar order.
[[nodiscard]] std::span<const char* const> editorMenus();

} // namespace fuse::editor
