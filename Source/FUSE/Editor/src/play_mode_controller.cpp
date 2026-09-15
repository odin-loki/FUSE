#include <fuse/editor/play_mode_controller.hpp>

namespace fuse::editor {

void PlayModeController::enterPlay(scene::Scene& scene, PlayModePhysicsState& physics) {
    takeSnapshot_(scene);
    physics.simulationActive = true;
    physics.stepCount = 0;
    m_state = State::Playing;
}

void PlayModeController::pause(scene::Scene& scene, PlayModePhysicsState& physics) {
    if (m_state != State::Playing) {
        return;
    }

    physics.simulationActive = false;
    m_state = State::Paused;
    (void)scene;
}

void PlayModeController::resume(scene::Scene& scene, PlayModePhysicsState& physics) {
    if (m_state != State::Paused) {
        return;
    }

    physics.simulationActive = true;
    m_state = State::Playing;
    (void)scene;
}

void PlayModeController::stop(scene::Scene& scene, PlayModePhysicsState& physics) {
    if (m_hasSnapshot) {
        restoreSnapshot_(scene);
    }

    physics.simulationActive = false;
    physics.stepCount = 0;
    m_state = State::Stopped;
    m_hasSnapshot = false;
}

void PlayModeController::takeSnapshot_(const scene::Scene& scene) {
    m_snapshot = scene.captureSnapshot();
    m_hasSnapshot = true;
}

void PlayModeController::restoreSnapshot_(scene::Scene& scene) {
    m_snapshot.apply(scene);
}

} // namespace fuse::editor
