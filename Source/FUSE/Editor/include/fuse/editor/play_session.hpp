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

/// Read-only fixed-step drain diagnostics (B6.12 deepen follow-up — spiral guard).
struct FixedStepPreflight {
    u32 pending = 0;
    u32 allowed = 0;
    u32 deferred = 0;
    u32 maxSteps = 0;
    bool wouldCap = false;
    bool skipped = false;

    bool canDrain() const { return !skipped && allowed > 0; }
};

/// Read-only variable-tick guard diagnostics (B6.12 deepen follow-up — inactive tick).
struct VariableTickPreflight {
    bool skipped = false;
    bool wouldSimulate = false;
    bool wouldAdvanceAccumulator = false;
};

/// Captured dirty-flag metadata for PIE restore (B6.12 deepen follow-up).
struct DirtySnapshotInfo {
    bool captured = false;
    bool sceneModified = false;
    u32 entityCount = 0;
    u32 dirtyEntityCount = 0;
};

/// Read-only dirty-snapshot restore diagnostics (B6.12 deepen follow-up — restore guard).
struct DirtySnapshotPreflight {
    bool captured = false;
    bool sceneModified = false;
    u32 entityCount = 0;
    bool skipped = false;

    bool canRestore() const { return captured; }
};

/// Read-only world-snapshot restore diagnostics (B6.12 deepen follow-up — restore guard).
struct WorldSnapshotPreflight {
    bool captured = false;
    u32 entityCount = 0;
    bool skipped = false;

    bool canRestore() const { return captured; }
};

/// Captured world snapshot metadata for PIE restore (B6.12 deepen follow-up).
struct WorldSnapshotInfo {
    bool captured = false;
    u32 entityCount = 0;
};

/// Combined PIE frame preflight — variable tick plus fixed-step drain (B6.12 deepen follow-up).
struct TickFixedStepPreflight {
    bool variableTickSkipped = false;
    FixedStepPreflight fixedStep{};

    bool wouldSimulateFrame() const { return !variableTickSkipped || fixedStep.canDrain(); }
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
    /// Fixed slices deferred by the last `consumeFixedSteps` maxSteps cap (0 when unlimited).
    u32 lastDeferredFixedStepCount() const { return m_lastDeferredFixedStepCount; }
    /// Remaining fixed slices in `tickAccumulator()` at the last `consumeFixedSteps` call.
    u32 pendingFixedStepCount(f32 fixedDt) const;
    /// Preflight a fixed-step drain without mutating session state.
    FixedStepPreflight preflightFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                                           u32 maxSteps = 0) const;
    /// Preflight a variable tick without mutating session state.
    VariableTickPreflight preflightVariableTick(f32 dt, const PlayModePhysicsState& physics) const;
    /// Preflight variable tick plus fixed-step drain for one PIE frame.
    TickFixedStepPreflight preflightTickFixedStep(f32 dt, f32 fixedDt,
                                                  const PlayModePhysicsState& physics,
                                                  u32 maxSteps = 0) const;
    bool shouldSkipVariableTick(f32 dt, const PlayModePhysicsState& physics) const;
    bool shouldSkipFixedStepDrain(f32 fixedDt, const PlayModePhysicsState& physics) const;
    bool hasPendingFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics) const;
    bool canConsumeFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                              u32 maxSteps = 0) const;
    /// Sub-fixed remainder left in `tickAccumulator()` after pending slices.
    f32 fixedAccumulatorRemainder(f32 fixedDt) const;
    bool shouldSkipDirtySnapshotRestore() const { return !m_hasDirtySnapshot; }
    bool shouldSkipDirtySnapshotDrain() const { return shouldSkipDirtySnapshotRestore(); }
    bool shouldSkipWorldSnapshotRestore() const { return !m_hasWorldSnapshot; }
    bool shouldSkipWorldSnapshotDrain() const { return shouldSkipWorldSnapshotRestore(); }
    /// Preflight dirty-flag restore without mutating editor state.
    DirtySnapshotPreflight preflightDirtySnapshot() const;
    bool canRestoreDirtySnapshot() const { return preflightDirtySnapshot().canRestore(); }
    /// Preflight world-snapshot restore without mutating editor state.
    WorldSnapshotPreflight preflightWorldSnapshot() const;
    bool canRestoreWorldSnapshot() const { return preflightWorldSnapshot().canRestore(); }
    u32 dirtySnapshotEntityCount() const {
        return m_hasDirtySnapshot ? static_cast<u32>(m_dirtySnapshot.transformDirty.size()) : 0u;
    }
    bool dirtySnapshotSceneModified() const {
        return m_hasDirtySnapshot && m_dirtySnapshot.sceneModified;
    }
    DirtySnapshotInfo dirtySnapshotInfo() const;
    ecs::EntityID dirtySnapshotEntityAt(usize index) const;
    bool transformDirtyAt(usize index) const;
    bool transformDirtyForEntity(ecs::EntityID entityId) const;
    u32 dirtySnapshotDirtyEntityCount() const;
    WorldSnapshotInfo worldSnapshotInfo() const;
    ecs::EntityID worldSnapshotEntityAt(usize index) const;
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
    u32 m_lastDeferredFixedStepCount = 0;
};

} // namespace fuse::editor
