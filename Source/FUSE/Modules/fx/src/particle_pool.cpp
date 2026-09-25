#include <fuse/fx/particle_pool.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/fx/particle_pool_kernel.hpp>

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
    namespace ppk = particle_pool_kernel;
    ppk::SlotParams params{};
    params.slots = {m_slots.data(), static_cast<u32>(m_slots.size())};
    u32 expired = 0;
    params.expired = &expired;
    params.dt = ppk::resolve_dt(ctx.dt);
    // Pools are small (tens of slots): the serial reference backend beats a job dispatch.
    if (!kernel::launch(kernel::Backend::CpuReference, ppk::slot_launch(params.slots.size), ppk::SlotKernel{}, params)
             .ok) {
        return;
    }
    m_activeCount = expired < m_activeCount ? m_activeCount - expired : 0u;
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

void ParticlePool::setSlotMotion(u32 slotIndex, float px, float py, float pz, float vx, float vy, float vz) {
    if (slotIndex >= m_slots.size()) {
        return;
    }

    ParticleSlot& slot = m_slots[slotIndex];
    slot.position.x = px;
    slot.position.y = py;
    slot.position.z = pz;
    slot.velocity.x = vx;
    slot.velocity.y = vy;
    slot.velocity.z = vz;
}

} // namespace fuse::fx
