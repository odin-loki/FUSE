#pragma once

#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::editor {

/// PIE play-session orchestrator (B6.12 deepen) — wraps `PlayModeController` with
/// `EditorState` sync, per-tick simulation stub, and ECS dirty-flag snapshot/restore.
class PlaySession {
public:
    void start(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
               PlayModePhysicsState& physics);
    void stop(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
              PlayModePhysicsState& physics);

    void pause(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics);
    void resume(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics);

    void tick(f32 dt, EditorScene& editorScene, PlayModePhysicsState& physics);

    bool isActive() const { return m_controller.state() != PlayModeController::State::Stopped; }
    bool isPlaying() const { return m_controller.isPlaying(); }
    bool isPaused() const { return m_controller.isPaused(); }

    u32 sessionTickCount() const { return m_sessionTickCount; }
    const PlayModeController& controller() const { return m_controller; }

private:
    struct DirtySnapshot {
        bool sceneModified = false;
        std::vector<std::pair<ecs::EntityID, bool>> transformDirty;
    };

    void captureDirtySnapshot_(EditorScene& editorScene, const EditorState& state);
    void restoreDirtySnapshot_(EditorScene& editorScene, EditorState& state) const;

    PlayModeController m_controller;
    DirtySnapshot m_dirtySnapshot{};
    u32 m_sessionTickCount = 0;
};

} // namespace fuse::editor
