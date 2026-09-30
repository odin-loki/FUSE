#include <fuse/world3d/runtime_schedule.hpp>

#include <fuse/ecs/systems/camera_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>

namespace fuse::world3d {

const char* runtimeStageName(RuntimeStage stage) {
    switch (stage) {
    case RuntimeStage::Input:
        return "Input";
    case RuntimeStage::Scripts:
        return "Scripts";
    case RuntimeStage::Animation:
        return "Animation";
    case RuntimeStage::Physics:
        return "Physics";
    case RuntimeStage::Transform:
        return "Transform";
    case RuntimeStage::Camera:
        return "Camera";
    case RuntimeStage::Audio:
        return "Audio";
    case RuntimeStage::Vfx:
        return "Vfx";
    case RuntimeStage::RenderExtract:
        return "RenderExtract";
    case RuntimeStage::Count:
        break;
    }
    return "?";
}

RuntimeSchedule::~RuntimeSchedule() {
    shutdown();
}

bool RuntimeSchedule::init(ecs::Registry& registry, const RuntimeScheduleDesc& desc) {
    shutdown();
    m_desc = desc;
    m_registry = &registry;

    m_animation.init(desc.expectedAnimators);
    m_physics.init(desc.physics);
    m_physicsReady = true;
    m_particles.init(desc.vfx);
    m_vfx.init(m_particles, desc.expectedEmitters);
    bool ok = true;
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    if (desc.enableAudio) {
        m_audioEngine.init(desc.audio);
        m_audioReady = m_audio.init(m_audioEngine, desc.expectedAudioSources);
        ok = m_audioReady;
    }
#endif
    registerStages_();
    // Compile the stage order now so the first frame does not allocate.
    (void)m_scheduler.execution_order();
    return ok;
}

void RuntimeSchedule::shutdown() {
    if (m_registry == nullptr) {
        return;
    }
    m_scheduler.clear();
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    m_audio.shutdown();
    if (m_audioReady || m_audioEngine.is_initialized()) {
        m_audioEngine.destroy();
    }
    m_audioReady = false;
#endif
    m_vfx.shutdown();
    m_particles.destroy();
    if (m_physicsReady) {
        m_physics.destroy();
        m_physicsReady = false;
    }
    m_animation.shutdown();
    m_hooks = {};
    m_externalPhysics = nullptr;
    m_registry = nullptr;
    m_accumulator = 0.f;
    m_stepCount = 0;
    m_droppedSteps = 0;
    m_lastOrderCount = 0;
}

void RuntimeSchedule::setHook(RuntimeStage stage, RuntimeStageHook fn, void* user) {
    if (stage == RuntimeStage::Count) {
        return;
    }
    m_hooks[static_cast<u32>(stage)] = Hook{fn, fn != nullptr ? user : nullptr};
}

void RuntimeSchedule::setStageEnabled(RuntimeStage stage, bool enabled) {
    if (stage == RuntimeStage::Count) {
        return;
    }
    if (enabled) {
        m_desc.enabledStages |= runtimeStageBit(stage);
    } else {
        m_desc.enabledStages &= ~runtimeStageBit(stage);
    }
}

void RuntimeSchedule::registerStages_() {
    m_scheduler.clear();
    m_scheduler.set_execution_mode(ecs::SystemScheduler::ExecutionMode::Inline);
    const char* previous = nullptr;
    for (u32 i = 0; i < kRuntimeStageCount; ++i) {
        const RuntimeStage stage = static_cast<RuntimeStage>(i);
        ecs::SystemScheduler::SystemDesc desc;
        desc.name = runtimeStageName(stage);
        desc.run = [this, stage]() { runStage_(stage); };
        if (previous != nullptr) {
            desc.dependencies.emplace_back(previous);
        }
        m_scheduler.register_system(desc);
        previous = runtimeStageName(stage);
    }
}

void RuntimeSchedule::runStage_(RuntimeStage stage) {
    if (!stageEnabled(stage)) {
        return;
    }
    if (stage == RuntimeStage::RenderExtract && !m_extractThisStep) {
        return;
    }
    ecs::Registry& registry = *m_registry;
    const f32 dt = m_stepDt;
    const Hook& hook = m_hooks[static_cast<u32>(stage)];

    switch (stage) {
    case RuntimeStage::Input:
    case RuntimeStage::Scripts:
    case RuntimeStage::RenderExtract:
        if (hook.fn != nullptr) {
            hook.fn(hook.user, registry, dt);
        }
        break;
    case RuntimeStage::Animation:
        m_animation.update(registry, dt);
        break;
    case RuntimeStage::Physics:
        if (hook.fn != nullptr) {
            hook.fn(hook.user, registry, dt);
        } else {
            physics().step(registry, dt, m_physicsStreams);
        }
        break;
    case RuntimeStage::Transform:
        ecs::TransformSystem::update(registry);
        break;
    case RuntimeStage::Camera:
        ecs::CameraSystem::update(registry);
        break;
    case RuntimeStage::Audio:
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
        if (m_audioReady) {
            m_audio.update(registry, dt);
        }
#endif
        break;
    case RuntimeStage::Vfx:
        m_vfx.update(registry, dt);
        break;
    case RuntimeStage::Count:
        return;
    }
    if (m_lastOrderCount < kRuntimeStageCount) {
        m_lastOrder[m_lastOrderCount++] = stage;
    }
}

void RuntimeSchedule::step(f32 dt) {
    if (m_registry == nullptr) {
        return;
    }
    m_stepDt = dt;
    m_lastOrderCount = 0;
    m_scheduler.run_all();
    ++m_stepCount;
}

u32 RuntimeSchedule::advance(f32 frameDt) {
    if (m_registry == nullptr) {
        return 0;
    }
    const f32 fixedDt = m_desc.fixedDt;
    if (frameDt > 0.f) {
        m_accumulator += frameDt;
    }
    u32 due = 0;
    if (fixedDt > 0.f) {
        // Tolerate float drift: 1/60 accumulated sixty times must not leave a step behind.
        const f32 slack = fixedDt * 1e-4f;
        while (m_accumulator + slack >= fixedDt * static_cast<f32>(due + 1u)) {
            ++due;
        }
    }
    u32 steps = due;
    if (m_desc.maxStepsPerFrame > 0u && steps > m_desc.maxStepsPerFrame) {
        m_droppedSteps += steps - m_desc.maxStepsPerFrame;
        steps = m_desc.maxStepsPerFrame;
    }
    m_accumulator -= fixedDt * static_cast<f32>(due);
    if (m_accumulator < 0.f) {
        m_accumulator = 0.f;
    }

    for (u32 i = 0; i < steps; ++i) {
        m_extractThisStep = i + 1u == steps;
        step(fixedDt);
    }
    m_extractThisStep = true;
    if (steps == 0u) {
        // No simulation step this frame: still hand the renderer the current state.
        m_stepDt = 0.f;
        m_lastOrderCount = 0;
        runStage_(RuntimeStage::RenderExtract);
    }
    return steps;
}

} // namespace fuse::world3d
