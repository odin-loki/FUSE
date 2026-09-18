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

/// Preflight result for fixed-step drain without mutating the session (B6.12 deepen follow-up).
struct FixedStepPreflight {
    bool skipped = true;
    f32 fixedDt = 0.f;
    u32 pendingSteps = 0;
    u32 stepsAllowed = 0;
    u32 stepsDeferred = 0;
    bool cappedByMaxSteps = false;

    bool canDrain() const { return !skipped && stepsAllowed > 0; }
};

/// Preflight result for dirty-flag restore without mutating editor state (B6.12 deepen follow-up).
struct DirtySnapshotPreflight {
    bool skipped = true;
    u32 entityCount = 0;
    bool sceneModifiedCaptured = false;

    bool canRestore() const { return !skipped; }
};

/// ECS world snapshot for PIE restore (B6.12 deepen — transform payloads per entity).
struct PlayWorldSnapshot {
    std::vector<std::pair<ecs::EntityID, ecs::Transform>> entities;

    static PlayWorldSnapshot capture(EditorScene& editorScene);
    void apply(EditorScene& editorScene) const;

    bool empty() const { return entities.empty(); }
    usize entityCount() const { return entities.size(); }
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
    /// Drains `tickAccumulator()` in `fixedDt` slices while playing; returns steps simulated.
    /// @param maxSteps 0 = unlimited; otherwise caps slices simulated this call (spiral guard).
    u32 consumeFixedSteps(f32 fixedDt, EditorScene& editorScene, PlayModePhysicsState& physics,
                          u32 maxSteps = 0);
    /// PIE frame update: `tick(dt)` then drain fixed slices; returns fixed steps simulated.
    u32 tickFixedStep(f32 dt, f32 fixedDt, EditorScene& editorScene, PlayModePhysicsState& physics,
                      u32 maxSteps = 0);

    bool isActive() const { return m_controller.state() != PlayModeController::State::Stopped; }
    bool isPlaying() const { return m_controller.isPlaying(); }
    bool isPaused() const { return m_controller.isPaused(); }

    u32 sessionTickCount() const { return m_sessionTickCount; }
    f32 tickAccumulator() const { return m_tickAccumulator; }
    u32 coalescedDirtyCount() const { return m_coalescedDirtyCount; }
    u32 skippedInactiveTickCount() const { return m_skippedInactiveTickCount; }
    u32 skippedInactiveFixedStepCount() const { return m_skippedInactiveFixedStepCount; }
    /// Remaining fixed slices in `tickAccumulator()` at the last `consumeFixedSteps` call.
    u32 pendingFixedStepCount(f32 fixedDt) const;
    /// Preflight fixed-step drain: pending slices, maxSteps cap, and deferred remainder.
    FixedStepPreflight preflightFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                                           u32 maxSteps = 0) const;
    /// True when `tick(dt)` would be a no-op for the current session/physics/dt.
    bool shouldSkipVariableTick(f32 dt, const PlayModePhysicsState& physics) const;
    /// True when `consumeFixedSteps(fixedDt)` would be a no-op for the current session/physics/fixedDt.
    bool shouldSkipFixedStepDrain(f32 fixedDt, const PlayModePhysicsState& physics) const;
    u32 dirtySnapshotEntityCount() const {
        return m_hasDirtySnapshot ? static_cast<u32>(m_dirtySnapshot.transformDirty.size()) : 0u;
    }
    /// Preflight dirty-flag restore without mutating editor state.
    DirtySnapshotPreflight preflightDirtySnapshotRestore() const;
    /// True when `drainDirtySnapshot` would be guarded with no captured snapshot.
    bool shouldSkipDirtySnapshotDrain() const { return !m_hasDirtySnapshot; }
    bool hasWorldSnapshot() const { return m_hasWorldSnapshot; }
    bool hasDirtySnapshot() const { return m_hasDirtySnapshot; }

    const PlayWorldSnapshot& worldSnapshot() const { return m_worldSnapshot; }
    PlayWorldSnapshot captureWorldSnapshot(EditorScene& editorScene) const;
    void restoreWorldSnapshot(EditorScene& editorScene, const PlayWorldSnapshot& snapshot) const;
    /// Applies the stored PIE world snapshot when present; returns true when drained.
    bool drainWorldSnapshot(EditorScene& editorScene);
    void restoreDirtyFlags(EditorScene& editorScene, EditorState& state) const;
    /// Restores captured dirty flags when present; returns true when drained.
    bool drainDirtySnapshot(EditorScene& editorScene, EditorState& state);

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
    void simulateStep_(EditorScene& editorScene, PlayModePhysicsState& physics);
    void coalesceTransformDirty_(EditorScene& editorScene);

    PlayModeController m_controller;
    DirtySnapshot m_dirtySnapshot{};
    PlayWorldSnapshot m_worldSnapshot{};
    bool m_hasWorldSnapshot = false;
    bool m_hasDirtySnapshot = false;
    u32 m_sessionTickCount = 0;
    f32 m_tickAccumulator = 0.f;
    u32 m_coalescedDirtyCount = 0;
    u32 m_skippedInactiveTickCount = 0;
    u32 m_skippedInactiveFixedStepCount = 0;
};

} // namespace fuse::editor
