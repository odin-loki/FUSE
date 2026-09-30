#pragma once

// GAP-GAME-LOOP-ECS: the runtime ECS frame schedule shared by World3D and the editor's PIE.
//
// One fixed step runs these stages, in this order, as systems of an ecs::SystemScheduler (Inline
// mode, deterministic order; each stage depends on the previous one):
//
//   Input -> Scripts -> Animation -> Physics -> Transform -> Camera -> Audio -> Vfx -> RenderExtract
//
//   Input          hook (platform input -> components); no-op without one
//   Scripts        hook (fuse::script::ScriptSystem::update, see bindScriptSystem below)
//   Animation      animation::AnimationSystem::update: every AnimatorRef, evaluated in parallel
//   Physics        physics::PhysicsManager::step over Transform + RigidBody + Collider entities
//                  (owned manager, an external one, or a hook that replaces it)
//   Transform      ecs::TransformSystem::update (TRS -> local_to_world)
//   Camera         ecs::CameraSystem::update (view / projection / frustum of active cameras)
//   Audio          audio::AudioEcsSystem::update (AudioSource / AudioListener -> AudioEngine)
//   Vfx            vfx::VfxEcsSystem::update (VfxEmitter -> ParticleSystem)
//   RenderExtract  hook, run once per frame after the frame's last fixed step
//
// `advance(frameDt)` accumulates frame time and runs whole `fixedDt` steps (at most
// `maxStepsPerFrame`; the excess is dropped to avoid a spiral of death). After init, a steady-state
// frame makes no heap allocations in the schedule itself or in its systems (see the brief's gate in
// World3D/tests/test_runtime_schedule.cpp).

#include <fuse/animation/animation_system.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/system_scheduler.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/types.hpp>
#include <fuse/vfx/particle_system.hpp>
#include <fuse/vfx/vfx_ecs_system.hpp>

#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
#include <fuse/audio/audio_ecs_system.hpp>
#include <fuse/audio/audio_engine.hpp>
#endif

#include <array>

namespace fuse::world3d {

enum class RuntimeStage : u8 {
    Input = 0,
    Scripts,
    Animation,
    Physics,
    Transform,
    Camera,
    Audio,
    Vfx,
    RenderExtract,
    Count,
};
inline constexpr u32 kRuntimeStageCount = static_cast<u32>(RuntimeStage::Count);

const char* runtimeStageName(RuntimeStage stage);

/// Stage hook: `user` is the pointer given to setHook.
using RuntimeStageHook = void (*)(void* user, ecs::Registry& registry, f32 dt);

inline constexpr u32 runtimeStageBit(RuntimeStage stage) { return 1u << static_cast<u32>(stage); }
inline constexpr u32 kAllRuntimeStages = (1u << kRuntimeStageCount) - 1u;

struct RuntimeScheduleDesc {
    f32 fixedDt = 1.f / 60.f;
    /// Fixed steps per advance() at most (0 = unlimited).
    u32 maxStepsPerFrame = 5;
    /// Stages that run (runtimeStageBit mask).
    u32 enabledStages = kAllRuntimeStages;
    /// Owned PhysicsManager pools (sized for a game level; the manager's own defaults reserve 64k bodies).
    physics::PhysicsManagerDesc physics = [] {
        physics::PhysicsManagerDesc desc{};
        desc.maxBodies = 4096;
        desc.maxContacts = 16384;
        desc.maxConstraints = 8192;
        return desc;
    }();
    vfx::VfxDesc vfx{};
    u32 expectedAnimators = 256;
    u32 expectedEmitters = 64;
    bool enableAudio = true;
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    /// Null output by default (headless / tests); a game sets output = Auto, pacing = Device.
    audio::AudioDesc audio = [] {
        audio::AudioDesc desc{};
        desc.output = audio::AudioOutputRequest::Null;
        desc.pacing = audio::AudioPacing::Caller;
        desc.frames_per_buf = 256;
        return desc;
    }();
    u32 expectedAudioSources = 64;
#endif
};

class RuntimeSchedule {
public:
    RuntimeSchedule() = default;
    ~RuntimeSchedule();
    RuntimeSchedule(const RuntimeSchedule&) = delete;
    RuntimeSchedule& operator=(const RuntimeSchedule&) = delete;

    /// Binds to `registry` (non-owning) and initialises the owned systems.
    bool init(ecs::Registry& registry, const RuntimeScheduleDesc& desc = {});
    void shutdown();
    [[nodiscard]] bool initialized() const { return m_registry != nullptr; }
    [[nodiscard]] ecs::Registry* registry() const { return m_registry; }
    [[nodiscard]] const RuntimeScheduleDesc& desc() const { return m_desc; }

    /// Hook for Input, Scripts, RenderExtract; a Physics hook replaces the built-in physics step
    /// (the editor's PlayModePhysicsState::stepHook). fn = nullptr clears it.
    void setHook(RuntimeStage stage, RuntimeStageHook fn, void* user);
    /// Step this manager instead of the owned one (nullptr restores the owned manager).
    void setExternalPhysics(physics::PhysicsManager* manager) { m_externalPhysics = manager; }
    void setStageEnabled(RuntimeStage stage, bool enabled);
    [[nodiscard]] bool stageEnabled(RuntimeStage stage) const {
        return (m_desc.enabledStages & runtimeStageBit(stage)) != 0u;
    }

    /// Frame update: accumulates `frameDt` and runs whole fixed steps; returns the steps run.
    /// RenderExtract runs once afterwards (also when no step was due).
    u32 advance(f32 frameDt);
    /// One pass of every stage with `dt` (RenderExtract included), bypassing the accumulator.
    void step(f32 dt);

    [[nodiscard]] f32 accumulator() const { return m_accumulator; }
    /// Fraction of a fixed step left in the accumulator (render interpolation weight).
    [[nodiscard]] f32 interpolationAlpha() const {
        return m_desc.fixedDt > 0.f ? m_accumulator / m_desc.fixedDt : 0.f;
    }
    [[nodiscard]] u64 stepCount() const { return m_stepCount; }
    [[nodiscard]] u32 droppedSteps() const { return m_droppedSteps; }

    /// Stages executed by the last step, in execution order (for tests / diagnostics).
    [[nodiscard]] const std::array<RuntimeStage, kRuntimeStageCount>& lastStepOrder() const { return m_lastOrder; }
    [[nodiscard]] u32 lastStepOrderCount() const { return m_lastOrderCount; }

    animation::AnimationSystem& animation() { return m_animation; }
    physics::PhysicsManager& physics() { return m_externalPhysics != nullptr ? *m_externalPhysics : m_physics; }
    physics::PhysicsManager& ownedPhysics() { return m_physics; }
    vfx::ParticleSystem& particles() { return m_particles; }
    vfx::VfxEcsSystem& vfx() { return m_vfx; }
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    audio::AudioEngine& audioEngine() { return m_audioEngine; }
    audio::AudioEcsSystem& audio() { return m_audio; }
#endif
    const ecs::SystemScheduler& scheduler() const { return m_scheduler; }

private:
    struct Hook {
        RuntimeStageHook fn = nullptr;
        void* user = nullptr;
    };

    void runStage_(RuntimeStage stage);
    void registerStages_();

    RuntimeScheduleDesc m_desc{};
    ecs::Registry* m_registry = nullptr;
    ecs::SystemScheduler m_scheduler;
    std::array<Hook, kRuntimeStageCount> m_hooks{};
    animation::AnimationSystem m_animation;
    physics::PhysicsManager m_physics;
    physics::PhysicsManager* m_externalPhysics = nullptr;
    physics::PhysicsStreamManager m_physicsStreams{};
    bool m_physicsReady = false;
    vfx::ParticleSystem m_particles;
    vfx::VfxEcsSystem m_vfx;
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    audio::AudioEngine m_audioEngine;
    audio::AudioEcsSystem m_audio;
    bool m_audioReady = false;
#endif
    f32 m_stepDt = 0.f;
    bool m_extractThisStep = true;
    f32 m_accumulator = 0.f;
    u64 m_stepCount = 0;
    u32 m_droppedSteps = 0;
    std::array<RuntimeStage, kRuntimeStageCount> m_lastOrder{};
    u32 m_lastOrderCount = 0;
};

} // namespace fuse::world3d
