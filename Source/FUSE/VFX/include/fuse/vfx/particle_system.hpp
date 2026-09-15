#pragma once

#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/vfx/effect_instance.hpp>
#include <fuse/vfx/particle_emitter.hpp>
#include <fuse/vfx/vfx_desc.hpp>

#include <vector>

namespace fuse::vfx {

class ParticleSystem {
public:
    void init(const VfxDesc& desc);
    void destroy();
    void update(f32 dt);

    Handle<ParticleEmitter> create_emitter(const ParticleEmitterDesc& desc);
    void destroy_emitter(Handle<ParticleEmitter> handle);

    Handle<EffectInstance> spawn_effect(const ParticleEmitterDesc& desc, const math::Vec3& position,
                                        f32 duration = 0.f, u32 burst_count = 1);
    void stop_effect(Handle<EffectInstance> handle);

    ParticleEmitter* get_emitter(Handle<ParticleEmitter> handle);
    const ParticleEmitter* get_emitter(Handle<ParticleEmitter> handle) const;
    EffectInstance* get_effect(Handle<EffectInstance> handle);
    const EffectInstance* get_effect(Handle<EffectInstance> handle) const;

    u32 alive_particle_count() const;
    u32 emitter_count() const;
    u32 effect_count() const;
    bool is_initialized() const { return m_initialized; }
    const VfxDesc& desc() const { return m_desc; }
    VfxBackendKind backend_kind() const;

private:
    void cleanup_finished_effects_();

    VfxDesc m_desc{};
    bool m_initialized = false;
    HandleMap<ParticleEmitter> m_emitters;
    HandleMap<EffectInstance> m_effects;
    std::vector<Handle<ParticleEmitter>> m_activeEmitters;
    std::vector<Handle<EffectInstance>> m_activeEffects;
};

} // namespace fuse::vfx
