#include <fuse/fx/particle_pool.hpp>

namespace fuse::fx {

ParticlePool::ParticlePool(u32 capacity) : m_slots(capacity) {}

bool ParticlePool::spawn(const fuse::math::Vec3& position,
                         const fuse::math::Vec3& velocity,
                         float lifetime,
                         float blend_weight) {
    if (lifetime <= 0.f) {
        return false;
    }

    for (ParticleSlot& slot : m_slots) {
        if (slot.alive) {
            continue;
        }

        slot.position = position;
        slot.velocity = velocity;
        slot.lifetime = lifetime;
        slot.age = 0.f;
        slot.blend_weight = blend_weight;
        slot.alive = true;
        ++m_activeCount;
        ++m_spawnCount;
        return true;
    }

    return false;
}

void ParticlePool::tick(const frame::FrameCtx& ctx) {
    const float dt = ctx.dt > 0.f ? ctx.dt : (1.f / 60.f);

    for (ParticleSlot& slot : m_slots) {
        if (!slot.alive) {
            continue;
        }

        slot.age += dt;
        slot.position.x += slot.velocity.x * dt;
        slot.position.y += slot.velocity.y * dt;
        slot.position.z += slot.velocity.z * dt;

        if (slot.age >= slot.lifetime) {
            slot.alive = false;
            if (m_activeCount > 0) {
                --m_activeCount;
            }
        }
    }
}

void ParticlePool::clear() {
    for (ParticleSlot& slot : m_slots) {
        slot.alive = false;
        slot.age = 0.f;
    }
    m_activeCount = 0;
}

void ParticlePool::setSlotAlive(u32 slotIndex, bool alive) {
    if (slotIndex >= m_slots.size()) {
        return;
    }

    ParticleSlot& slot = m_slots[slotIndex];
    if (slot.alive == alive) {
        return;
    }

    slot.alive = alive;
    if (alive) {
        ++m_activeCount;
    } else if (m_activeCount > 0) {
        --m_activeCount;
    }
}

} // namespace fuse::fx
