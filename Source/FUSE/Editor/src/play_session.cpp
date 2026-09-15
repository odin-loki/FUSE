#include <fuse/editor/play_session.hpp>

#include <fuse/ecs/components/transform.hpp>

namespace fuse::editor {

PlayWorldSnapshot PlayWorldSnapshot::capture(EditorScene& editorScene) {
    PlayWorldSnapshot snapshot;
    editorScene.registry().each<ecs::Transform>(
        [&snapshot](ecs::EntityID id, const ecs::Transform& transform) {
            snapshot.entities.push_back({id, transform});
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
    m_hasWorldSnapshot = false;

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
    if (!m_controller.isPlaying() || !physics.simulationActive) {
        return;
    }

    ++m_sessionTickCount;
    m_tickAccumulator += dt;
    ++physics.stepCount;

    coalesceTransformDirty_(editorScene);
}

PlayWorldSnapshot PlaySession::captureWorldSnapshot(EditorScene& editorScene) const {
    return PlayWorldSnapshot::capture(editorScene);
}

void PlaySession::restoreWorldSnapshot(EditorScene& editorScene,
                                       const PlayWorldSnapshot& snapshot) const {
    snapshot.apply(editorScene);
}

void PlaySession::captureDirtySnapshot_(EditorScene& editorScene, const EditorState& state) {
    m_dirtySnapshot.sceneModified = state.sceneModified;
    m_dirtySnapshot.transformDirty.clear();

    editorScene.registry().each<ecs::Transform>([this](ecs::EntityID id, const ecs::Transform& transform) {
        m_dirtySnapshot.transformDirty.push_back({id, transform.dirty});
    });
}

void PlaySession::restoreDirtySnapshot_(EditorScene& editorScene, EditorState& state) const {
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
