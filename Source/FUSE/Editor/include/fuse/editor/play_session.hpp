#pragma once

#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>
#include <fuse/world3d/runtime_schedule.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::script {
class ScriptVM;
class ScriptRuntime;
class ScriptSystem;
} // namespace fuse::script

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

/// Captured world snapshot metadata for PIE restore (B6.12 deepen follow-up).
struct WorldSnapshotInfo {
    bool captured = false;
    u32 entityCount = 0;
};

/// Read-only world-snapshot drain diagnostics (B6.12 deepen follow-up — drain guard).
struct WorldSnapshotPreflight {
    bool captured = false;
    u32 entityCount = 0;
    bool skipped = false;

    bool canDrain() const { return captured; }
};

/// Read-only dirty-snapshot restore diagnostics (B6.12 deepen follow-up — restore guard).
struct DirtySnapshotPreflight {
    bool captured = false;
    bool sceneModified = false;
    u32 entityCount = 0;
    u32 dirtyEntityCount = 0;
    bool skipped = false;

    bool canRestore() const { return captured; }
};

/// Combined PIE frame preflight — variable tick plus fixed-step drain (B6.12 deepen follow-up).
struct TickFixedStepPreflight {
    bool variableTickSkipped = false;
    FixedStepPreflight fixedStep{};

    bool canSimulateVariableTick() const { return !variableTickSkipped; }
    bool canDrainFixedStep() const { return fixedStep.canDrain(); }
    bool canSimulateFrame() const { return canSimulateVariableTick() || canDrainFixedStep(); }
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
///
/// `start` also copies the whole ECS registry (entity records, generations, free list and every
/// component column) and `stop` restores it, so entities spawned or destroyed during play and
/// non-Transform state such as RigidBody velocities return exactly to their edit-time values
/// (the same state `ecs::RegistrySerialiser` persists).
///
/// MP-B6-EDITOR-SCRIPT-PIE: when the play registry holds `ecs::Script` components (and the editor
/// is built with fuse_script), `start` creates a ScriptVM + ScriptRuntime + ScriptSystem bound to the
/// play registry (engine API: Entity / Physics on this session's PhysicsManager / Scene / CVar),
/// preloads the behaviour modules (paths as given, or relative to `setScriptRoot`), and enters
/// play; the Scripts stage of the runtime schedule runs `ScriptSystem::update`, physics contact
/// events of each step are dispatched to `on_collision` / `on_trigger_enter`, and `stop` exits
/// play (on_destroy), shuts the runtime down and restores the edit-time registry. Module files
/// are watched: `pollScriptHotReload` reloads changed modules into the live runtime.
class PlaySession {
public:
    PlaySession();
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    void start(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
               PlayModePhysicsState& physics);
    void stop(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
              PlayModePhysicsState& physics);

    void pause(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics);
    void resume(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics);

    /// Variable tick: advances the accumulator and simulates one step of `dt` (physics included).
    void tick(f32 dt, EditorScene& editorScene, PlayModePhysicsState& physics);
    /// Drains `tickAccumulator()` in `fixedDt` slices while playing; returns steps simulated.
    /// @param maxSteps 0 = unlimited; otherwise caps slices simulated this call (spiral guard).
    u32 consumeFixedSteps(f32 fixedDt, EditorScene& editorScene, PlayModePhysicsState& physics,
                          u32 maxSteps = 0);
    /// PIE frame update: `tick(dt)` then drain fixed slices; returns fixed steps simulated.
    /// Physics advances only in the fixed slices here (the variable tick does not also step it).
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
    /// Preflight variable tick plus fixed-step drain for one PIE frame.
    TickFixedStepPreflight preflightTickFixedStep(f32 dt, f32 fixedDt,
                                                  const PlayModePhysicsState& physics,
                                                  u32 maxSteps = 0) const;
    /// Preflight a variable tick without mutating session state.
    VariableTickPreflight preflightVariableTick(f32 dt, const PlayModePhysicsState& physics) const;
    bool shouldSkipVariableTick(f32 dt, const PlayModePhysicsState& physics) const;
    bool shouldSkipFixedStepDrain(f32 fixedDt, const PlayModePhysicsState& physics) const;
    bool hasPendingFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics) const;
    bool canConsumeFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                              u32 maxSteps = 0) const;
    /// Sub-fixed remainder left in `tickAccumulator()` after pending slices.
    f32 fixedAccumulatorRemainder(f32 fixedDt) const;
    bool shouldSkipDirtySnapshotRestore() const { return !m_hasDirtySnapshot; }
    /// Preflight dirty-flag restore without mutating editor state.
    DirtySnapshotPreflight preflightDirtySnapshot() const;
    bool canRestoreDirtySnapshot() const { return preflightDirtySnapshot().canRestore(); }
    bool shouldSkipWorldSnapshotRestore() const { return !m_hasWorldSnapshot; }
    /// Preflight world snapshot drain without mutating editor state.
    WorldSnapshotPreflight preflightWorldSnapshot() const;
    bool canDrainWorldSnapshot() const { return preflightWorldSnapshot().canDrain(); }
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
    bool worldSnapshotContainsEntity(ecs::EntityID entityId) const;
    bool shouldSkipWorldSnapshotDrain() const;
    bool shouldSkipDirtySnapshotDrain() const;
    bool hasWorldSnapshot() const { return m_hasWorldSnapshot; }
    /// True between `start` and `stop` — the full edit-time registry copy is held.
    bool hasRegistrySnapshot() const { return m_hasRegistrySnapshot; }
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
    /// Physics world stepped during play (live between `start` and `stop` unless a step hook or
    /// `drivePhysics = false` was set at start).
    fuse::physics::PhysicsManager& physicsWorld() { return m_physicsWorld; }
    const fuse::physics::PhysicsManager& physicsWorld() const { return m_physicsWorld; }
    bool physicsWorldLive() const { return m_physicsWorldLive; }
    /// GAP-GAME-LOOP-ECS: the runtime frame schedule PIE runs each simulated step (the same stage
    /// order as World3D; live between `start` and `stop`).
    world3d::RuntimeSchedule& runtimeSchedule() { return m_schedule; }

    /// Step transport: one simulated step of `fixedDt` (scripts, physics, ...) while paused.
    /// Returns false when not paused.
    bool stepPaused(f32 fixedDt, EditorScene& editorScene, PlayModePhysicsState& physics);
    [[nodiscard]] u32 manualStepCount() const { return m_manualStepCount; }

    // ---- MP-B6-EDITOR-SCRIPT-PIE -------------------------------------------------------------
    /// Off: PIE never creates the script runtime (default on).
    void setScriptsEnabled(bool enabled) { m_scriptsEnabled = enabled; }
    [[nodiscard]] bool scriptsEnabled() const { return m_scriptsEnabled; }
    /// Directory that relative Script module paths resolve against (the project root).
    void setScriptRoot(std::string root) { m_scriptRoot = std::move(root); }
    [[nodiscard]] const std::string& scriptRoot() const { return m_scriptRoot; }
    /// True between `start` and `stop` when the script runtime is running.
    [[nodiscard]] bool scriptsLive() const;
    [[nodiscard]] script::ScriptVM* scriptVm();
    [[nodiscard]] script::ScriptRuntime* scriptRuntime();
    [[nodiscard]] script::ScriptSystem* scriptSystem();
    [[nodiscard]] usize scriptAttachedCount() const;
    /// Why the script runtime did not start / the last script error.
    [[nodiscard]] const std::string& scriptLastError() const { return m_scriptError; }
    /// Contact callbacks dispatched to behaviours since `start`.
    [[nodiscard]] u64 scriptContactDispatchCount() const { return m_scriptContactDispatches; }
    /// Reload changed module files into the live runtime; returns modules reloaded (their paths
    /// appended to `reloaded` when given). No-op while scripts are not live.
    u32 pollScriptHotReload(std::vector<std::string>* reloaded = nullptr);
    [[nodiscard]] u32 scriptReloadCount() const { return m_scriptReloadCount; }

private:
    struct DirtySnapshot {
        bool sceneModified = false;
        std::vector<std::pair<ecs::EntityID, bool>> transformDirty;
    };

    void captureDirtySnapshot_(EditorScene& editorScene, const EditorState& state);
    void restoreDirtySnapshot_(EditorScene& editorScene, EditorState& state) const;
    void captureWorldSnapshot_(EditorScene& editorScene);
    void restoreWorldSnapshot_(EditorScene& editorScene) const;
    void tick_(f32 dt, EditorScene& editorScene, PlayModePhysicsState& physics, bool stepPhysics);
    /// `physicsDt` <= 0 advances counters only.
    void simulateStep_(EditorScene& editorScene, PlayModePhysicsState& physics, f32 physicsDt);
    void coalesceTransformDirty_(EditorScene& editorScene);
    static void stepPhysicsStage_(void* user, ecs::Registry& registry, f32 dt);
    static void scriptStage_(void* user, ecs::Registry& registry, f32 dt);
    void startScripting_(EditorScene& editorScene);
    void stopScripting_();

    struct PieScripting;
    std::unique_ptr<PieScripting> m_scripting;
    bool m_scriptsEnabled = true;
    std::string m_scriptRoot;
    std::string m_scriptError;
    u64 m_scriptContactDispatches = 0;
    u32 m_scriptReloadCount = 0;
    u32 m_manualStepCount = 0;

    PlayModeController m_controller;
    fuse::physics::PhysicsManager m_physicsWorld{};
    fuse::physics::PhysicsStreamManager m_physicsStreams{};
    bool m_physicsWorldLive = false;
    world3d::RuntimeSchedule m_schedule;
    PlayModePhysicsState* m_stepPhysicsState = nullptr;
    DirtySnapshot m_dirtySnapshot{};
    PlayWorldSnapshot m_worldSnapshot{};
    ecs::Registry m_registrySnapshot{};
    bool m_hasRegistrySnapshot = false;
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
