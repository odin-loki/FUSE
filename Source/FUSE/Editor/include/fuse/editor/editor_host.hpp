#pragma once

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/uaisk_tree_reload.hpp>
#include <fuse/cinematics/timeline_host_stub.hpp>
#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/editor/play_session.hpp>
#include <fuse/editor/runtime_viewport.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/handle.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/object.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::editor {

struct AiAgentEntityBinding {
    u32 agentIndex = 0;
    Handle<Object> entity = Handle<Object>::invalid();
};

class ConsolePanel;
class EditorScripting;

/// Outcome of the last project / scene file command (UNI-U6-FILE-1).
struct EditorFileResult {
    bool ok = false;
    CommandKind command = CommandKind::SaveScene;
    std::string path;
    std::string error;
};

/// One line of game-thread console output, queued for the UI (`EditorHost::drainConsoleOutput`).
struct EditorConsoleLine {
    fuse::log::Level level = fuse::log::Level::Info;
    std::string text;
};

/// Qt-free in-process editor runtime host (WP-08 / U6).
/// UI thread calls postFromUi(); game thread calls gameTick() to drain commands.
class EditorHost {
public:
    EditorHost();
    ~EditorHost();

    CommandQueue& commandQueue() { return m_queue; }
    const CommandQueue& commandQueue() const { return m_queue; }

    UndoStack& undoStack() { return m_undoStack; }
    const UndoStack& undoStack() const { return m_undoStack; }

    CommandStack& commandStack() { return m_commandStack; }
    const CommandStack& commandStack() const { return m_commandStack; }

    EditorScene& editorScene();
    const EditorScene& editorScene() const;

    EditorState& editorState() { return m_state; }
    const EditorState& editorState() const { return m_state; }

    scene::Scene& runtimeScene() { return m_runtimeScene; }
    const scene::Scene& runtimeScene() const { return m_runtimeScene; }

    PlaySession& playSession() { return m_playSession; }
    const PlaySession& playSession() const { return m_playSession; }

    const std::string& loadedProject() const { return m_loadedProject; }
    u32 selectedAiTreeProfileId() const { return m_selectedAiTreeProfileId; }
    u32 selectedAiAgentIndex() const { return m_selectedAiAgentIndex; }
    u32 aiCodegenReloadCount() const { return m_aiCodegenReloadCount; }
    u32 aiTreeFileReloadCount() const { return m_aiTreeFileReloadCount; }
    u32 aiTreeInotifyHotReloadCount() const { return m_aiTreeInotifyHotReloadCount; }
    u32 aiTreeOsPollCount() const { return m_aiTreeFileWatch.osPollCount(); }
    u32 aiTreeInotifyPollCount() const { return m_aiTreeFileWatch.inotifyPollCount(); }
    const std::vector<AiAgentEntityBinding>& aiAgentEntityBindings() const { return m_aiAgentEntityBindings; }
    fuse::ai::BehaviorRuntime& pieBehaviorRuntime() { return m_pieBehaviorRuntime; }
    const fuse::ai::BehaviorRuntime& pieBehaviorRuntime() const { return m_pieBehaviorRuntime; }
    fuse::ai::uaisk::TreeFileWatchRegistry& aiTreeFileWatch() { return m_aiTreeFileWatch; }
    const fuse::ai::uaisk::TreeFileWatchRegistry& aiTreeFileWatch() const { return m_aiTreeFileWatch; }
    const std::string& loadedCinematicsSeqAsset() const { return m_loadedCinematicsSeqAsset; }
    fuse::cinematics::TimelineMs cinematicsSeqScrubPreviewMs() const { return m_cinematicsSeqScrubPreviewMs; }
    const fuse::cinematics::SeqScrubPreview& cinematicsSeqScrubPreview() const { return m_cinematicsSeqScrubPreview; }
    u32 cinematicsSeqPreviewPaneWireCount() const { return m_cinematicsSeqPreviewPaneWireCount; }
    u32 cinematicsSeqTimelineHostWireCount() const { return m_cinematicsSeqTimelineHostWireCount; }
    fuse::cinematics::TimelineHostStub& cinematicsTimelineHost() { return m_cinematicsTimelineHost; }
    const fuse::cinematics::TimelineHostStub& cinematicsTimelineHost() const { return m_cinematicsTimelineHost; }
    u32 gameTickCount() const { return m_gameTickCount; }
    u32 commandsAppliedLastTick() const { return m_commandsAppliedLastTick; }

    RuntimeViewportHook& runtimeViewport() { return m_runtimeViewport; }
    const RuntimeViewportHook& runtimeViewport() const { return m_runtimeViewport; }

    void postFromUi(EditorCommand command);
    void gameTick();
    void setLoadedProject(std::string project);
    /// Clear the scene for a project switch (game thread; applied by the `project.root` command
    /// when the root changes): stops PIE, drops undo / command history and selection, and empties
    /// the editor registry + runtime scene so the new project's world loads into a clean scene.
    void resetSceneForProjectOpen();
    void setSelectedAiTreeProfileId(u32 profileId);
    void setSelectedAiAgentIndex(u32 agentIndex);
    void setAiAgentEntityBinding(u32 agentIndex, Handle<Object> entity);
    bool reloadAiCodegenProfile(u32 profileId, const std::string& uaiskModule, const std::string& csText);
    bool reloadAiTreeFromDisk(u32 profileId, const std::string& watchPath);
    bool reloadAiTreeViaInotify(u32 profileId, const std::string& watchPath);
    void setLoadedCinematicsSeqAsset(std::string assetText);
    void setCinematicsSeqScrubPreview(fuse::cinematics::TimelineMs timeMs,
                                      const fuse::cinematics::SeqScrubPreview& preview);
    void incrementCinematicsSeqPreviewPaneWireCount();
    void incrementCinematicsSeqTimelineHostWireCount();

    /// Undo/redo property edits recorded on the game-thread `CommandStack`.
    void undoPropertyEdit();
    void redoPropertyEdit();

    // ---- UNI-U6-FILE-1 / MP-B6-QT-SCENE-FILES: projects and scenes (game thread) --------------
    // The File commands (CommandKind::NewProject ... SaveSceneAs) call these; each records
    // `lastFileResult()` and logs a console line. A scene is the runtime scene (camera, names,
    // transforms, hierarchy) plus the editor ECS registry with every component, saved as a
    // `.fuselevel` v3 (scene::SceneSerialiser::saveWithRegistry). Loading replaces the scene: PIE is
    // stopped and the undo / command history is dropped. Saving is refused while PIE runs.

    /// Writes `<directory>/project.json` (manifest writer) with an empty default world per enabled
    /// dimension (`worlds/main3d.fuselevel`, `worlds/main2d.fuselevel`), then opens the project.
    bool newProject(const std::string& directory, const std::string& name,
                    u32 dimensionFlags = kProjectEnable3D | kProjectEnable2D | kProjectEnableUI);
    /// Loads `project.json` and opens its default 3D world (the 2D one when 3D is disabled). A
    /// missing default world file opens an empty scene bound to that path.
    bool openProject(const std::string& directory);
    /// Empty scene (camera only) of `dimension`; no file path until SaveSceneAs.
    bool newScene(scene::SceneDimension dimension, std::string name = {});
    /// `.fuselevel` v1 / v2 / v3; v1/v2 entities become Transform-only ECS entities.
    bool openScene(const std::string& path);
    bool saveScene();
    bool saveSceneAs(const std::string& path);

    [[nodiscard]] const EditorFileResult& lastFileResult() const { return m_lastFileResult; }
    [[nodiscard]] const std::string& currentScenePath() const { return m_currentScenePath; }
    [[nodiscard]] scene::SceneDimension currentSceneDimension() const { return m_sceneDimension; }
    [[nodiscard]] bool hasProject() const { return m_hasProject; }
    [[nodiscard]] const project::ProjectManifest& projectManifest() const { return m_projectManifest; }
    /// Unsaved changes: undo-stack / property-stack edits since the last save or load
    /// (UndoStack::isDirty, CommandStack::isDirty) or other scene edits (EditorState::sceneModified).
    [[nodiscard]] bool isSceneDirty() const;
    [[nodiscard]] u32 sceneSaveCount() const { return m_sceneSaveCount; }
    [[nodiscard]] u32 sceneLoadCount() const { return m_sceneLoadCount; }

    // ---- MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1: console (game thread) --------------------------
    /// Runs one console line on the game thread (CommandKind::ConsoleExec): `cvar <name> [value]`,
    /// `stat`, `lua <code>`, a ScriptConsole command (`echo`, `run`, `history`, ...) or else Lua
    /// source for the REPL (the PIE script VM while playing, the editor VM otherwise; a bare
    /// expression is printed). Output goes to the console queue. Returns false on errors.
    bool executeConsoleLine(const std::string& line);
    /// Queue a console line (any thread).
    void postConsoleOutput(fuse::log::Level level, std::string text);
    /// Move queued console output out (any thread; the UI drains it). Returns lines moved.
    usize drainConsoleOutput(std::vector<EditorConsoleLine>& out);
    /// Drain straight into a ConsolePanel's log (UI thread).
    usize drainConsoleOutput(ConsolePanel& console);
    [[nodiscard]] u32 consoleLinesExecuted() const { return m_consoleLinesExecuted; }
    [[nodiscard]] u32 scriptHotReloadCount() const { return m_scriptHotReloadCount; }

private:
    friend class RuntimeViewportHook;

    void ensureInitialized_();
    void applyCommand_(const EditorCommand& command);
    /// File / transport-step / console command kinds (editor_host.cpp); false when `command` is not one.
    bool applyHostCommand_(const EditorCommand& command);
    void pollHotReload_();
    bool fileFail_(CommandKind kind, const std::string& path, std::string error);
    bool fileOk_(CommandKind kind, const std::string& path, const std::string& message);
    void resetForSceneSwitch_();
    void markSceneClean_();
    void drainPropertyCommandQueue_();
    void syncPieAiBindings_();

    CommandQueue m_queue;
    CommandStack m_commandStack;
    // Scene before undo stack: commands hold registry references and release reserved entity
    // slots in their destructors, so the registry must be destroyed after the stack.
    EditorScene m_editorScene;
    UndoStack m_undoStack;
    EditorState m_state;
    scene::Scene m_runtimeScene;
    PlaySession m_playSession;
    PlayModePhysicsState m_physics;
    RuntimeViewportHook m_runtimeViewport;
    std::string m_loadedProject;
    std::string m_loadedCinematicsSeqAsset;
    fuse::cinematics::TimelineMs m_cinematicsSeqScrubPreviewMs = 0;
    fuse::cinematics::SeqScrubPreview m_cinematicsSeqScrubPreview;
    u32 m_cinematicsSeqPreviewPaneWireCount = 0;
    u32 m_cinematicsSeqTimelineHostWireCount = 0;
    fuse::cinematics::TimelineHostStub m_cinematicsTimelineHost;
    u32 m_selectedAiTreeProfileId = 0;
    u32 m_selectedAiAgentIndex = 0;
    u32 m_aiCodegenReloadCount = 0;
    u32 m_aiTreeFileReloadCount = 0;
    u32 m_aiTreeInotifyHotReloadCount = 0;
    std::vector<AiAgentEntityBinding> m_aiAgentEntityBindings;
    fuse::ai::uaisk::TreeFileWatchRegistry m_aiTreeFileWatch;
    fuse::ai::BehaviorRuntime m_pieBehaviorRuntime;
    u32 m_gameTickCount = 0;
    u32 m_commandsAppliedLastTick = 0;
    bool m_initialized = false;

    EditorFileResult m_lastFileResult;
    std::string m_currentScenePath;
    scene::SceneDimension m_sceneDimension = scene::SceneDimension::World3D;
    project::ProjectManifest m_projectManifest;
    bool m_hasProject = false;
    u32 m_sceneSaveCount = 0;
    u32 m_sceneLoadCount = 0;

    std::unique_ptr<EditorScripting> m_scripting; ///< created on the first Lua console line
    mutable std::mutex m_consoleMutex;
    std::vector<EditorConsoleLine> m_consoleOutput;
    u32 m_consoleLinesExecuted = 0;
    u32 m_scriptHotReloadCount = 0;
    std::string m_lastReportedScriptError;
};

} // namespace fuse::editor
