#pragma once

#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Lightweight physics simulation flag for play mode (full PhysicsManager deferred).
struct PlayModePhysicsState {
    bool simulationActive = false;
    u32 stepCount = 0;
};

/// Play-in-editor transport controller (B6.12 stub — Qt toolbar deferred to U6 chrome).
class PlayModeController {
public:
    enum class State : u8 {
        Stopped = 0,
        Playing,
        Paused,
    };

    void enterPlay(scene::Scene& scene, PlayModePhysicsState& physics);
    void pause(scene::Scene& scene, PlayModePhysicsState& physics);
    void resume(scene::Scene& scene, PlayModePhysicsState& physics);
    void stop(scene::Scene& scene, PlayModePhysicsState& physics);

    State state() const { return m_state; }
    bool isPlaying() const { return m_state == State::Playing; }
    bool isPaused() const { return m_state == State::Paused; }
    bool hasSnapshot() const { return m_hasSnapshot; }

private:
    struct SceneSnapshot {
        std::string name;
        fuse::Camera camera;
        std::vector<std::string> objectNames;
    };

    void takeSnapshot_(const scene::Scene& scene);
    void restoreSnapshot_(scene::Scene& scene);

    State m_state = State::Stopped;
    SceneSnapshot m_snapshot{};
    bool m_hasSnapshot = false;
};

} // namespace fuse::editor
