#pragma once

#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::editor {

/// ECS world snapshot for PIE restore (B6.12 deepen — transform payloads per entity).
struct PlayWorldSnapshot {
    std::vector<std::pair<ecs::EntityID, ecs::Transform>> entities;

    static PlayWorldSnapshot capture(EditorScene& editorScene);
    void apply(EditorScene& editorScene) const;
};

/// PIE play-session orchestrator (B6.12 deepen) — wraps `PlayModeController` with
/// `EditorState` sync, tick accumulator, world snapshot capture/restore, and ECS
/// dirty-flag snapshot/restore with coalesced dirty marking during play.
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
    f32 tickAccumulator() const { return m_tickAccumulator; }
    u32 coalescedDirtyCount() const { return m_coalescedDirtyCount; }
    bool hasWorldSnapshot() const { return m_hasWorldSnapshot; }

    const PlayWorldSnapshot& worldSnapshot() const { return m_worldSnapshot; }
    PlayWorldSnapshot captureWorldSnapshot(EditorScene& editorScene) const;
    void restoreWorldSnapshot(EditorScene& editorScene, const PlayWorldSnapshot& snapshot) const;

    const PlayModeController& controller() const { return m_controller; }

private:
    struct DirtySnapshot {
        bool sceneModified = false;
        std::vector<std::pair<ecs::EntityID, bool>> transformDirty;
    };

    void captureDirtySnapshot_(EditorScene& editorScene, const EditorState& state);
    void restoreDirtySnapshot_(EditorScene& editorScene, EditorState& state) const;
    void captureWorldSnapshot_(EditorScene& editorScene);
    void restoreWorldSnapshot_(EditorScene& editorScene) const;
    void coalesceTransformDirty_(EditorScene& editorScene);

    PlayModeController m_controller;
    DirtySnapshot m_dirtySnapshot{};
    PlayWorldSnapshot m_worldSnapshot{};
    bool m_hasWorldSnapshot = false;
    u32 m_sessionTickCount = 0;
    f32 m_tickAccumulator = 0.f;
    u32 m_coalescedDirtyCount = 0;
};

} // namespace fuse::editor
