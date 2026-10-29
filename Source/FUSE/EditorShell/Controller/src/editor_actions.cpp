#include <fuse/editor/editor_actions.hpp>

#include <array>

namespace fuse::editor {
namespace {
using A = ActionId;
constexpr std::array<ActionDesc, static_cast<usize>(ActionId::Count)> kActions{{
    {A::FileNewScene, "file.new_scene", "File", "New Scene", "Ctrl+N", "fuseActionNewScene", false, true},
    {A::FileOpenScene, "file.open_scene", "File", "Open Scene...", "Ctrl+O", "fuseActionOpenScene", false, true},
    {A::FileSave, "file.save", "File", "Save", "Ctrl+S", "fuseActionSaveScene", true, true},
    {A::FileSaveAs, "file.save_as", "File", "Save As...", "Ctrl+Shift+S", "fuseActionSaveSceneAs", false, false},
    {A::FileNewProject, "file.new_project", "File", "New Project...", "", "fuseActionNewProject", true, false},
    {A::FileOpenProject, "file.open_project", "File", "Open Project...", "Ctrl+Shift+O", "fuseActionOpenProject", false, false},
    {A::FileQuit, "file.quit", "File", "Quit", "Ctrl+Q", "fuseActionQuit", true, false},
    {A::EditUndo, "edit.undo", "Edit", "Undo", "Ctrl+Z", "fuseActionUndo", false, true},
    {A::EditRedo, "edit.redo", "Edit", "Redo", "Ctrl+Y", "fuseActionRedo", false, true},
    {A::EditPreferences, "edit.preferences", "Edit", "Preferences...", "", "fuseActionPreferences", true, false},
    {A::EntityCreateEmpty, "entity.create_empty", "Entity", "Create Empty", "Ctrl+Shift+N", "fuseActionCreateEmpty", false, false},
    {A::EntityCreateCube, "entity.create_cube", "Entity", "Create Cube", "", "fuseActionCreateCube", false, false},
    {A::EntityCreateSdfSphere, "entity.create_sdf_sphere", "Entity", "Create SDF Sphere", "", "fuseActionCreateSdfSphere", false, false},
    {A::EntityCreateSdfBox, "entity.create_sdf_box", "Entity", "Create SDF Box", "", "fuseActionCreateSdfBox", false, false},
    {A::EntityCreatePointLight, "entity.create_point_light", "Entity", "Create Point Light", "", "fuseActionCreatePointLight", false, false},
    {A::EntityCreateDirectionalLight, "entity.create_dir_light", "Entity", "Create Directional Light", "", "fuseActionCreateDirLight", false, false},
    {A::EntityCreateCamera, "entity.create_camera", "Entity", "Create Camera", "", "fuseActionCreateCamera", false, false},
    {A::EntityDuplicate, "entity.duplicate", "Entity", "Duplicate", "Ctrl+D", "fuseActionDuplicate", true, false},
    {A::EntityDelete, "entity.delete", "Entity", "Delete", "Delete", "fuseActionDelete", false, false},
    {A::EntityGroup, "entity.group", "Entity", "Group Selection", "Ctrl+G", "fuseActionGroup", false, false},
    {A::EntityUngroup, "entity.ungroup", "Entity", "Ungroup", "Ctrl+Shift+G", "fuseActionUngroup", false, false},
    {A::EntityOpenScript, "entity.open_script", "Entity", "Open Script", "Ctrl+E", "fuseActionOpenScript", true, false},
    {A::PlayPlay, "play.play", "Play", "Play", "F5", "fuseActionPlay", false, true},
    {A::PlayPause, "play.pause", "Play", "Pause", "F6", "fuseActionPause", false, true},
    {A::PlayResume, "play.resume", "Play", "Resume", "F7", "fuseActionResume", false, true},
    {A::PlayStep, "play.step", "Play", "Step", "F10", "fuseActionStep", false, true},
    {A::PlayStop, "play.stop", "Play", "Stop", "Shift+F5", "fuseActionStop", false, true},
    {A::PlayMaximizeOnPlay, "play.maximize_on_play", "Play", "Maximize Viewport on Play", "", "fuseActionMaximizeOnPlay", true, false},
    {A::ViewResetLayout, "view.reset_layout", "View", "Reset Layout", "", "fuseActionResetLayout", true, false},
    {A::ViewModeLit, "view.mode_lit", "View", "View Mode: Lit", "Alt+1", "fuseActionViewLit", true, false},
    {A::ViewModeUnlit, "view.mode_unlit", "View", "View Mode: Unlit", "Alt+2", "fuseActionViewUnlit", false, false},
    {A::ViewModeWireframe, "view.mode_wireframe", "View", "View Mode: Wireframe", "Alt+3", "fuseActionViewWireframe", false, false},
    {A::ViewModeLitWireframe, "view.mode_lit_wireframe", "View", "View Mode: Lit Wireframe", "Alt+4", "fuseActionViewLitWireframe", false, false},
    {A::ViewShowMeshBounds, "view.show_mesh_bounds", "View", "Show Mesh Bounds", "", "fuseActionShowMeshBounds", true, false},
    {A::ViewShowColliders, "view.show_colliders", "View", "Show All Colliders", "", "fuseActionShowColliders", false, false},
    {A::ViewFrameSelected, "view.frame_selected", "View", "Frame Selected", "F", "fuseActionFrameSelected", false, false},
    {A::ViewLayoutSingle, "view.layout_single", "View", "Viewport Layout: Single", "Ctrl+Alt+1", "fuseActionViewportSingle", true, false},
    {A::ViewLayoutTwo, "view.layout_two", "View", "Viewport Layout: Two (Perspective + Top)", "Ctrl+Alt+2", "fuseActionViewportTwo", false, false},
    {A::ViewLayoutQuad, "view.layout_quad", "View", "Viewport Layout: Quad (Perspective, Top, Front, Side)", "Ctrl+Alt+3", "fuseActionViewportQuad", false, false},
}};
constexpr std::array<const char*, 5> kMenus{{"File", "Edit", "Entity", "Play", "View"}};

constexpr bool ordered() {
    for (usize i = 0; i < kActions.size(); ++i)
        if (static_cast<usize>(kActions[i].id) != i) return false;
    return true;
}
static_assert(ordered(), "kActions must be in ActionId order");
} // namespace

std::span<const ActionDesc> editorActions() { return kActions; }
const ActionDesc& actionDesc(ActionId id) { return kActions[static_cast<usize>(id)]; }
const ActionDesc* findAction(std::string_view key) {
    for (const ActionDesc& a : kActions)
        if (key == a.stableId || key == a.qtObjectName) return &a;
    return nullptr;
}
std::span<const char* const> editorMenus() { return kMenus; }

} // namespace fuse::editor
