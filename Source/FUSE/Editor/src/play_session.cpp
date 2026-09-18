#include <fuse/editor/play_session.hpp>

#include <fuse/ecs/components/transform.hpp>

#include <algorithm>

namespace fuse::editor {

PlayWorldSnapshot PlayWorldSnapshot::capture(EditorScene& editorScene) {
    PlayWorldSnapshot snapshot;
    editorScene.registry().each<ecs::Transform>(
        [&snapshot](ecs::EntityID id, const ecs::Transform& transform) {
            snapshot.entities.push_back({id, transform});
        });
    std::sort(snapshot.entities.begin(), snapshot.entities.end(),
              [](const std::pair<ecs::EntityID, ecs::Transform>& lhs,
                 const std::pair<ecs::EntityID, ecs::Transform>& rhs) {
                  if (lhs.first.index != rhs.first.index) {
                      return lhs.first.index < rhs.first.index;
                  }
                  return lhs.first.generation < rhs.first.generation;
              });
    return snapshot;
}

void PlayWorldSnapshot::apply(EditorScene& editorScene) const {
    for (const std::pair<ecs::EntityID, ecs::Transform>& entry : entities) {
        ecs::Transform* transform = editorScene.registry().get<ecs::Transform>(entry.first);
        if (transform != nullptr) {
            *transform = entry.second;
        }
    }
}

void PlaySession::start(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
                        PlayModePhysicsState& physics) {
    if (isActive()) {
        return;
    }

    captureDirtySnapshot_(editorScene, state);
    captureWorldSnapshot_(editorScene);
    m_controller.enterPlay(scene, physics);
    m_sessionTickCount = 0;
    m_tickAccumulator = 0.f;
    m_coalescedDirtyCount = 0;
    m_skippedInactiveTickCount = 0;
    m_skippedInactiveFixedStepCount = 0;
    m_lastDeferredFixedStepCount = 0;

    state.playing = true;
    state.paused = false;
}

void PlaySession::stop(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
                       PlayModePhysicsState& physics) {
    if (!isActive()) {
        return;
    }

    m_controller.stop(scene, physics);
    restoreWorldSnapshot_(editorScene);
    restoreDirtySnapshot_(editorScene, state);
    m_sessionTickCount = 0;
    m_tickAccumulator = 0.f;
    m_coalescedDirtyCount = 0;
    m_skippedInactiveTickCount = 0;
    m_skippedInactiveFixedStepCount = 0;
    m_lastDeferredFixedStepCount = 0;
    m_hasWorldSnapshot = false;
    m_hasDirtySnapshot = false;

    state.playing = false;
    state.paused = false;
}

void PlaySession::pause(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics) {
    if (!m_controller.isPlaying()) {
        return;
    }

    m_controller.pause(scene, physics);
    state.paused = true;
}

void PlaySession::resume(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics) {
    if (!m_controller.isPaused()) {
        return;
    }

    m_controller.resume(scene, physics);
    state.paused = false;
}

void PlaySession::tick(f32 dt, EditorScene& editorScene, PlayModePhysicsState& physics) {
    if (shouldSkipVariableTick(dt, physics)) {
        ++m_skippedInactiveTickCount;
        return;
    }

    m_tickAccumulator += dt;
    simulateStep_(editorScene, physics);
}

u32 PlaySession::consumeFixedSteps(f32 fixedDt, EditorScene& editorScene,
                                   PlayModePhysicsState& physics, u32 maxSteps) {
    if (shouldSkipFixedStepDrain(fixedDt, physics)) {
        ++m_skippedInactiveFixedStepCount;
        m_lastDeferredFixedStepCount = 0;
        return 0;
    }

    u32 steps = 0;
    while (m_tickAccumulator >= fixedDt) {
        if (maxSteps > 0 && steps >= maxSteps) {
            break;
        }

        m_tickAccumulator -= fixedDt;
        simulateStep_(editorScene, physics);
        ++steps;
    }

    m_lastDeferredFixedStepCount = pendingFixedStepCount(fixedDt);
    return steps;
}

u32 PlaySession::tickFixedStep(f32 dt, f32 fixedDt, EditorScene& editorScene,
                               PlayModePhysicsState& physics, u32 maxSteps) {
    tick(dt, editorScene, physics);
    return consumeFixedSteps(fixedDt, editorScene, physics, maxSteps);
}

u32 PlaySession::pendingFixedStepCount(f32 fixedDt) const {
    if (fixedDt <= 0.f) {
        return 0;
    }

    return static_cast<u32>(m_tickAccumulator / fixedDt);
}

FixedStepPreflight PlaySession::preflightFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                                                    u32 maxSteps) const {
    FixedStepPreflight preflight{};
    preflight.maxSteps = maxSteps;

    if (shouldSkipFixedStepDrain(fixedDt, physics)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.pending = pendingFixedStepCount(fixedDt);
    if (maxSteps == 0) {
        preflight.allowed = preflight.pending;
        preflight.deferred = 0;
        return preflight;
    }

    preflight.allowed = preflight.pending < maxSteps ? preflight.pending : maxSteps;
    preflight.deferred = preflight.pending > preflight.allowed ? preflight.pending - preflight.allowed : 0;
    preflight.wouldCap = preflight.deferred > 0;
    return preflight;
}

VariableTickPreflight PlaySession::preflightVariableTick(f32 dt,
                                                         const PlayModePhysicsState& physics) const {
    VariableTickPreflight preflight{};
    preflight.skipped = shouldSkipVariableTick(dt, physics);
    if (preflight.skipped) {
        return preflight;
    }

    preflight.wouldSimulate = true;
    preflight.wouldAdvanceAccumulator = dt > 0.f;
    return preflight;
}

TickFixedStepPreflight PlaySession::preflightTickFixedStep(f32 dt, f32 fixedDt,
                                                           const PlayModePhysicsState& physics,
                                                           u32 maxSteps) const {
    TickFixedStepPreflight preflight{};
    preflight.variableTickSkipped = shouldSkipVariableTick(dt, physics);
    preflight.fixedStep = preflightFixedSteps(fixedDt, physics, maxSteps);
    return preflight;
}

DirtySnapshotPreflight PlaySession::preflightDirtySnapshot() const {
    DirtySnapshotPreflight preflight{};

    if (shouldSkipDirtySnapshotRestore()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.captured = true;
    preflight.sceneModified = m_dirtySnapshot.sceneModified;
    preflight.entityCount = static_cast<u32>(m_dirtySnapshot.transformDirty.size());
    return preflight;
}

WorldSnapshotPreflight PlaySession::preflightWorldSnapshot() const {
    WorldSnapshotPreflight preflight{};

    if (shouldSkipWorldSnapshotRestore()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.captured = true;
    preflight.entityCount = static_cast<u32>(m_worldSnapshot.entities.size());
    return preflight;
}

bool PlaySession::shouldSkipVariableTick(f32 dt, const PlayModePhysicsState& physics) const {
    return !m_controller.isPlaying() || !physics.simulationActive || dt < 0.f;
}

bool PlaySession::shouldSkipFixedStepDrain(f32 fixedDt, const PlayModePhysicsState& physics) const {
    return !m_controller.isPlaying() || !physics.simulationActive || fixedDt <= 0.f;
}

bool PlaySession::hasPendingFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics) const {
    if (shouldSkipFixedStepDrain(fixedDt, physics)) {
        return false;
    }

    return pendingFixedStepCount(fixedDt) > 0;
}

bool PlaySession::canConsumeFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                                       u32 maxSteps) const {
    const FixedStepPreflight preflight = preflightFixedSteps(fixedDt, physics, maxSteps);
    return preflight.canDrain();
}

f32 PlaySession::fixedAccumulatorRemainder(f32 fixedDt) const {
    if (fixedDt <= 0.f) {
        return 0.f;
    }

    return m_tickAccumulator - static_cast<f32>(pendingFixedStepCount(fixedDt)) * fixedDt;
}

DirtySnapshotInfo PlaySession::dirtySnapshotInfo() const {
    DirtySnapshotInfo info{};
    info.captured = m_hasDirtySnapshot;
    if (!m_hasDirtySnapshot) {
        return info;
    }

    info.sceneModified = m_dirtySnapshot.sceneModified;
    info.entityCount = static_cast<u32>(m_dirtySnapshot.transformDirty.size());
    info.dirtyEntityCount = dirtySnapshotDirtyEntityCount();
    return info;
}

ecs::EntityID PlaySession::dirtySnapshotEntityAt(usize index) const {
    if (!m_hasDirtySnapshot || index >= m_dirtySnapshot.transformDirty.size()) {
        return ecs::EntityID{};
    }

    return m_dirtySnapshot.transformDirty[index].first;
}

bool PlaySession::transformDirtyAt(usize index) const {
    if (!m_hasDirtySnapshot || index >= m_dirtySnapshot.transformDirty.size()) {
        return false;
    }

    return m_dirtySnapshot.transformDirty[index].second;
}

bool PlaySession::transformDirtyForEntity(ecs::EntityID entityId) const {
    if (!m_hasDirtySnapshot || !entityId.valid()) {
        return false;
    }

    for (const std::pair<ecs::EntityID, bool>& entry : m_dirtySnapshot.transformDirty) {
        if (entry.first == entityId) {
            return entry.second;
        }
    }

    return false;
}

u32 PlaySession::dirtySnapshotDirtyEntityCount() const {
    if (!m_hasDirtySnapshot) {
        return 0;
    }

    u32 dirtyCount = 0;
    for (const std::pair<ecs::EntityID, bool>& entry : m_dirtySnapshot.transformDirty) {
        if (entry.second) {
            ++dirtyCount;
        }
    }

    return dirtyCount;
}

WorldSnapshotInfo PlaySession::worldSnapshotInfo() const {
    WorldSnapshotInfo info{};
    info.captured = m_hasWorldSnapshot;
    if (!m_hasWorldSnapshot) {
        return info;
    }

    info.entityCount = static_cast<u32>(m_worldSnapshot.entities.size());
    return info;
}

ecs::EntityID PlaySession::worldSnapshotEntityAt(usize index) const {
    if (!m_hasWorldSnapshot || index >= m_worldSnapshot.entities.size()) {
        return ecs::EntityID{};
    }

    return m_worldSnapshot.entities[index].first;
}

PlayWorldSnapshot PlaySession::captureWorldSnapshot(EditorScene& editorScene) const {
    return PlayWorldSnapshot::capture(editorScene);
}

void PlaySession::restoreWorldSnapshot(EditorScene& editorScene,
                                       const PlayWorldSnapshot& snapshot) const {
    if (snapshot.empty()) {
        return;
    }

    snapshot.apply(editorScene);
}

bool PlaySession::drainWorldSnapshot(EditorScene& editorScene) {
    if (shouldSkipWorldSnapshotDrain()) {
        return false;
    }

    m_worldSnapshot.apply(editorScene);
    m_hasWorldSnapshot = false;
    return true;
}

void PlaySession::restoreDirtyFlags(EditorScene& editorScene, EditorState& state) const {
    if (!m_hasDirtySnapshot) {
        return;
    }

    restoreDirtySnapshot_(editorScene, state);
}

bool PlaySession::drainDirtySnapshot(EditorScene& editorScene, EditorState& state) {
    if (shouldSkipDirtySnapshotDrain()) {
        return false;
    }

    restoreDirtySnapshot_(editorScene, state);
    m_hasDirtySnapshot = false;
    return true;
}

void PlaySession::captureDirtySnapshot_(EditorScene& editorScene, const EditorState& state) {
    m_dirtySnapshot.sceneModified = state.sceneModified;
    m_dirtySnapshot.transformDirty.clear();

    editorScene.registry().each<ecs::Transform>([this](ecs::EntityID id, const ecs::Transform& transform) {
        m_dirtySnapshot.transformDirty.push_back({id, transform.dirty});
    });
    std::sort(m_dirtySnapshot.transformDirty.begin(), m_dirtySnapshot.transformDirty.end(),
              [](const std::pair<ecs::EntityID, bool>& lhs,
                 const std::pair<ecs::EntityID, bool>& rhs) {
                  if (lhs.first.index != rhs.first.index) {
                      return lhs.first.index < rhs.first.index;
                  }
                  return lhs.first.generation < rhs.first.generation;
              });
    m_hasDirtySnapshot = true;
}

void PlaySession::restoreDirtySnapshot_(EditorScene& editorScene, EditorState& state) const {
    if (!m_hasDirtySnapshot) {
        return;
    }

    state.sceneModified = m_dirtySnapshot.sceneModified;

    for (const std::pair<ecs::EntityID, bool>& entry : m_dirtySnapshot.transformDirty) {
        ecs::Transform* transform = editorScene.registry().get<ecs::Transform>(entry.first);
        if (transform != nullptr) {
            transform->dirty = entry.second;
        }
    }
}

void PlaySession::captureWorldSnapshot_(EditorScene& editorScene) {
    m_worldSnapshot = PlayWorldSnapshot::capture(editorScene);
    m_hasWorldSnapshot = true;
}

void PlaySession::restoreWorldSnapshot_(EditorScene& editorScene) const {
    if (!m_hasWorldSnapshot) {
        return;
    }

    m_worldSnapshot.apply(editorScene);
}

void PlaySession::simulateStep_(EditorScene& editorScene, PlayModePhysicsState& physics) {
    ++m_sessionTickCount;
    ++physics.stepCount;
    coalesceTransformDirty_(editorScene);
}

void PlaySession::coalesceTransformDirty_(EditorScene& editorScene) {
    editorScene.registry().each<ecs::Transform>([this](ecs::EntityID, ecs::Transform& transform) {
        if (transform.dirty) {
            ++m_coalescedDirtyCount;
            return;
        }

        transform.dirty = true;
    });
}

} // namespace fuse::editor
