#include <fuse/vfx/particle_system.hpp>

#include <algorithm>

namespace fuse::vfx {

namespace {

template <typename T>
void remove_handle(std::vector<Handle<T>>& handles, Handle<T> target) {
    handles.erase(std::remove_if(handles.begin(), handles.end(),
                                 [target](Handle<T> handle) { return handle == target; }),
                  handles.end());
}

} // namespace

void ParticleSystem::init(const VfxDesc& desc) {
    destroy();
    m_desc = desc;
    m_initialized = true;
}

void ParticleSystem::destroy() {
    m_emitters = HandleMap<ParticleEmitter>{};
    m_effects = HandleMap<EffectInstance>{};
    m_activeEmitters.clear();
    m_activeEffects.clear();
    m_desc = VfxDesc{};
    m_initialized = false;
}

void ParticleSystem::update(f32 dt) {
    if (!m_initialized || dt <= 0.f) {
        return;
    }

    for (Handle<ParticleEmitter> handle : m_activeEmitters) {
        if (ParticleEmitter* emitter = m_emitters.get(handle)) {
            if (emitter->initialized() && emitter->enabled()) {
                emitter->simulate(dt);
            }
        }
    }

    for (Handle<EffectInstance> handle : m_activeEffects) {
        if (EffectInstance* effect = m_effects.get(handle)) {
            effect->tick(dt);
        }
    }

    cleanup_finished_effects_();
}

Handle<ParticleEmitter> ParticleSystem::create_emitter(const ParticleEmitterDesc& desc) {
    if (!m_initialized || m_activeEmitters.size() >= m_desc.max_emitters) {
        return Handle<ParticleEmitter>::invalid();
    }

    ParticleEmitter emitter{};
    emitter.init(desc);
    Handle<ParticleEmitter> handle = m_emitters.insert(std::move(emitter));
    m_activeEmitters.push_back(handle);
    return handle;
}

void ParticleSystem::destroy_emitter(Handle<ParticleEmitter> handle) {
    if (ParticleEmitter* emitter = get_emitter(handle)) {
        emitter->destroy();
    }
    m_emitters.remove(handle);
    remove_handle(m_activeEmitters, handle);
}

Handle<EffectInstance> ParticleSystem::spawn_effect(const ParticleEmitterDesc& desc,
                                                      const math::Vec3& position, f32 duration) {
    if (!m_initialized || m_activeEffects.size() >= m_desc.max_effect_instances) {
        return Handle<EffectInstance>::invalid();
    }

    Handle<ParticleEmitter> emitterHandle = create_emitter(desc);
    if (!emitterHandle.isValid()) {
        return Handle<EffectInstance>::invalid();
    }

    ParticleEmitter* emitter = get_emitter(emitterHandle);
    if (emitter == nullptr) {
        destroy_emitter(emitterHandle);
        return Handle<EffectInstance>::invalid();
    }

    emitter->set_position(position);
    emitter->burst(1);

    EffectInstance effect{};
    effect.emitter = emitterHandle;
    effect.position = position;
    effect.duration = duration;
    effect.playing = true;
    effect.auto_destroy = true;
    effect.play();

    Handle<EffectInstance> handle = m_effects.insert(std::move(effect));
    m_activeEffects.push_back(handle);
    return handle;
}

void ParticleSystem::stop_effect(Handle<EffectInstance> handle) {
    if (EffectInstance* effect = get_effect(handle)) {
        effect->stop();
        if (ParticleEmitter* emitter = get_emitter(effect->emitter)) {
            emitter->set_enabled(false);
        }
    }
}

ParticleEmitter* ParticleSystem::get_emitter(Handle<ParticleEmitter> handle) {
    return m_emitters.get(handle);
}

const ParticleEmitter* ParticleSystem::get_emitter(Handle<ParticleEmitter> handle) const {
    return m_emitters.get(handle);
}

EffectInstance* ParticleSystem::get_effect(Handle<EffectInstance> handle) {
    return m_effects.get(handle);
}

const EffectInstance* ParticleSystem::get_effect(Handle<EffectInstance> handle) const {
    return m_effects.get(handle);
}

u32 ParticleSystem::alive_particle_count() const {
    u32 total = 0;
    for (Handle<ParticleEmitter> handle : m_activeEmitters) {
        if (const ParticleEmitter* emitter = m_emitters.get(handle)) {
            total += emitter->alive_count();
        }
    }
    return total;
}

u32 ParticleSystem::emitter_count() const {
    return static_cast<u32>(m_activeEmitters.size());
}

u32 ParticleSystem::effect_count() const {
    return static_cast<u32>(m_activeEffects.size());
}

VfxBackendKind ParticleSystem::backend_kind() const {
#if defined(FUSE_HAS_CUDA)
    if (m_desc.gpu_simulation) {
        return VfxBackendKind::Cuda;
    }
#endif
    return VfxBackendKind::CpuReference;
}

void ParticleSystem::cleanup_finished_effects_() {
    std::vector<Handle<EffectInstance>> finished;
    for (Handle<EffectInstance> handle : m_activeEffects) {
        if (EffectInstance* effect = m_effects.get(handle)) {
            if (!effect->is_alive()) {
                finished.push_back(handle);
            }
        }
    }

    for (Handle<EffectInstance> handle : finished) {
        if (EffectInstance* effect = m_effects.get(handle)) {
            destroy_emitter(effect->emitter);
        }
        m_effects.remove(handle);
        remove_handle(m_activeEffects, handle);
    }
}

} // namespace fuse::vfx
