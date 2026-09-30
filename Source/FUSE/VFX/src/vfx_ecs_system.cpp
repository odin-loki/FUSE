#include <fuse/vfx/vfx_ecs_system.hpp>

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/vfx/particle_system.hpp>

namespace fuse::vfx {

ParticleEmitterDesc VfxEmitter::to_desc() const {
    ParticleEmitterDesc desc;
    desc.max_particles = max_particles;
    desc.emit_rate = emit_rate;
    desc.lifetime_min = lifetime_min;
    desc.lifetime_max = lifetime_max;
    desc.velocity_min = velocity_min;
    desc.velocity_max = velocity_max;
    desc.position_spread = position_spread;
    desc.size_start = size_start;
    desc.size_end = size_end;
    desc.color_start = color_start;
    desc.color_end = color_end;
    desc.alpha_start = alpha_start;
    desc.alpha_end = alpha_end;
    desc.gravity = gravity;
    desc.drag = drag;
    desc.material_id = material_id;
    return desc;
}

bool VfxEcsSystem::init(ParticleSystem& particles, usize expected_emitters) {
    shutdown();
    m_particles = &particles;
    m_tracked.reserve(expected_emitters);
    m_index.reserve(expected_emitters);
    return particles.is_initialized();
}

void VfxEcsSystem::shutdown() {
    if (m_particles != nullptr) {
        for (const Tracked& tracked : m_tracked) {
            m_particles->destroy_emitter(tracked.emitter);
        }
    }
    m_tracked.clear();
    m_index.clear();
    m_particles = nullptr;
    m_stamp = 0;
    m_stats = {};
}

Handle<ParticleEmitter> VfxEcsSystem::emitter_of(ecs::EntityID entity) const {
    const auto it = m_index.find(key_of(entity));
    return it == m_index.end() ? Handle<ParticleEmitter>::invalid() : m_tracked[it->second].emitter;
}

void VfxEcsSystem::update(ecs::Registry& registry, f32 dt) {
    if (m_particles == nullptr) {
        return;
    }
    ++m_stamp;
    const u32 stamp = m_stamp;

    registry.each<VfxEmitter>([&](ecs::EntityID entity, VfxEmitter& component) {
        const u64 key = key_of(entity);
        auto it = m_index.find(key);
        if (it == m_index.end()) {
            Tracked tracked;
            tracked.entity = entity;
            tracked.emitter = m_particles->create_emitter(component.to_desc());
            it = m_index.emplace(key, static_cast<u32>(m_tracked.size())).first;
            m_tracked.push_back(tracked);
            ++m_stats.emitters_created;
            component.emitter_index = tracked.emitter.index();
            component.emitter_generation = tracked.emitter.generation();
            if (ParticleEmitter* emitter = m_particles->get_emitter(tracked.emitter)) {
                math::Vec3 position = component.offset;
                if (const ecs::Transform* transform = registry.get<ecs::Transform>(entity)) {
                    const ecs::mat4& m = transform->local_to_world;
                    position = position + math::Vec3{m.data[12], m.data[13], m.data[14]};
                }
                emitter->set_position(position);
                if (component.burst_on_start > 0u) {
                    emitter->burst(component.burst_on_start);
                }
            }
        }
        Tracked& tracked = m_tracked[it->second];
        tracked.stamp = stamp;
        ParticleEmitter* emitter = m_particles->get_emitter(tracked.emitter);
        if (emitter == nullptr) {
            return;
        }
        math::Vec3 position = component.offset;
        if (const ecs::Transform* transform = registry.get<ecs::Transform>(entity)) {
            const ecs::mat4& m = transform->local_to_world;
            position = position + math::Vec3{m.data[12], m.data[13], m.data[14]};
        }
        emitter->set_position(position);
        if (emitter->enabled() != component.enabled) {
            emitter->set_enabled(component.enabled);
        }
    });

    for (u32 i = 0; i < static_cast<u32>(m_tracked.size());) {
        if (m_tracked[i].stamp == stamp) {
            ++i;
            continue;
        }
        m_particles->destroy_emitter(m_tracked[i].emitter);
        m_index.erase(key_of(m_tracked[i].entity));
        ++m_stats.emitters_released;
        const u32 last = static_cast<u32>(m_tracked.size()) - 1u;
        if (i != last) {
            m_tracked[i] = m_tracked[last];
            m_index[key_of(m_tracked[i].entity)] = i;
        }
        m_tracked.pop_back();
    }

    m_particles->update(dt);

    u32 alive = 0;
    for (const Tracked& tracked : m_tracked) {
        const ParticleEmitter* emitter = m_particles->get_emitter(tracked.emitter);
        VfxEmitter* component = registry.get<VfxEmitter>(tracked.entity);
        if (emitter == nullptr || component == nullptr) {
            continue;
        }
        component->alive_particles = emitter->alive_count();
        alive += component->alive_particles;
    }
    m_stats.alive_particles = alive;
}

} // namespace fuse::vfx
