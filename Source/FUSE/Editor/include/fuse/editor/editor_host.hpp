#pragma once

#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/editor/play_session.hpp>
#include <fuse/editor/runtime_viewport.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

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
    const std::string& loadedCinematicsSeqAsset() const { return m_loadedCinematicsSeqAsset; }
    u32 gameTickCount() const { return m_gameTickCount; }
    u32 commandsAppliedLastTick() const { return m_commandsAppliedLastTick; }

    RuntimeViewportHook& runtimeViewport() { return m_runtimeViewport; }
    const RuntimeViewportHook& runtimeViewport() const { return m_runtimeViewport; }

    void postFromUi(EditorCommand command);
    void gameTick();
    void setLoadedProject(std::string project);
    void setSelectedAiTreeProfileId(u32 profileId);
    void setLoadedCinematicsSeqAsset(std::string assetText);

private:
    friend class RuntimeViewportHook;

    void ensureInitialized_();
    void applyCommand_(const EditorCommand& command);

    CommandQueue m_queue;
    UndoStack m_undoStack;
    EditorScene m_editorScene;
    EditorState m_state;
    scene::Scene m_runtimeScene;
    PlaySession m_playSession;
    PlayModePhysicsState m_physics;
    RuntimeViewportHook m_runtimeViewport;
    std::string m_loadedProject;
    std::string m_loadedCinematicsSeqAsset;
    u32 m_selectedAiTreeProfileId = 0;
    u32 m_gameTickCount = 0;
    u32 m_commandsAppliedLastTick = 0;
    bool m_initialized = false;
};

} // namespace fuse::editor
