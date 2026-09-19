#pragma once

// Ore: Engine/source/afx/util/afxParticlePool.h (CPU pool stub without GameBase/BitStream)

#include <fuse/frame/frame_ctx.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::fx {

enum class ParticlePoolType {
    Normal,
    TwoPass,
};

struct ParticleSlot {
    fuse::math::Vec3 position{};
    fuse::math::Vec3 velocity{};
    float lifetime = 0.f;
    float age = 0.f;
    float blend_weight = 1.f;
    bool alive = false;
};

/// CPU particle pool stub for spell phrase playback (no GPU emitters yet).
class ParticlePool {
public:
    explicit ParticlePool(u32 capacity = 64);

    void set_pool_type(ParticlePoolType type) { m_poolType = type; }
    ParticlePoolType pool_type() const { return m_poolType; }

    u32 capacity() const { return static_cast<u32>(m_slots.size()); }
    u32 activeCount() const { return m_activeCount; }
    u32 spawnCount() const { return m_spawnCount; }

    bool spawn(const fuse::math::Vec3& position,
               const fuse::math::Vec3& velocity,
               float lifetime,
               float blend_weight = 1.f);

    void tick(const frame::FrameCtx& ctx);
    void clear();

    void setSlotAlive(u32 slotIndex, bool alive);

    const std::vector<ParticleSlot>& slots() const { return m_slots; }

private:
    ParticlePoolType m_poolType = ParticlePoolType::Normal;
    std::vector<ParticleSlot> m_slots;
    u32 m_activeCount = 0;
    u32 m_spawnCount = 0;
};

} // namespace fuse::fx
