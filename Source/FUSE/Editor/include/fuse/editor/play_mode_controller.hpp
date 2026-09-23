#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/scene_snapshot.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <vector>

namespace fuse::editor {

/// Play-mode physics settings and status. While playing, `PlaySession` steps a real
/// `fuse::physics::PhysicsManager` (created on Play from `desc`, destroyed on Stop) against the
/// play registry every simulated step; `stepHook` replaces it (tests, custom solvers).
struct PlayModePhysicsState {
    using StepHook = std::function<void(ecs::Registry& registry, f32 dt)>;

    /// Editor-sized pools (the runtime defaults reserve for 64k bodies).
    static fuse::physics::PhysicsManagerDesc defaultDesc() {
        fuse::physics::PhysicsManagerDesc desc{};
        desc.maxBodies = 4096;
        desc.maxContacts = 16384;
        desc.maxConstraints = 8192;
        return desc;
    }

    bool simulationActive = false;
    u32 stepCount = 0;
    /// False: steps only advance counters (no physics).
    bool drivePhysics = true;
    fuse::physics::PhysicsManagerDesc desc = defaultDesc();
    /// When set, called instead of the built-in PhysicsManager for each simulated step.
    StepHook stepHook;
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
    fuse::scene::SceneSnapshot m_snapshot{};

    void takeSnapshot_(const scene::Scene& scene);
    void restoreSnapshot_(scene::Scene& scene);

    State m_state = State::Stopped;
    bool m_hasSnapshot = false;
};

} // namespace fuse::editor
