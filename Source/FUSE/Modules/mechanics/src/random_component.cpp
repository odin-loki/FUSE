#include <fuse/mechanics/random_component.hpp>

namespace fuse::mechanics {

namespace {

// xorshift32 locks up if the state is zero. Seed 0 maps to this fixed nonzero state.
constexpr u32 kZeroSeedState = 0x6D2B79F5u;

// 24-bit mantissa keeps the result strictly inside [0, 1).
constexpr f32 kUnitInterval = 1.0f / 16777216.0f;

u32 xorshift32(u32 state) {
    u32 x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

} // namespace

RandomComponent::RandomComponent() = default;

RandomComponent::RandomComponent(std::string name, u32 seed)
    : Component(std::move(name)) {
    setSeed(seed);
}

void RandomComponent::setSeed(u32 seed) {
    m_seed = seed;
    m_state = seed == 0u ? kZeroSeedState : seed;
    m_value = 0.f;
}

f32 RandomComponent::next() {
    m_state = xorshift32(m_state);
    m_value = static_cast<f32>(m_state >> 8) * kUnitInterval;
    ++m_drawCount;
    return m_value;
}

void RandomComponent::tick() {
    if (!m_stepOnTick) {
        return;
    }

    next();
    ++m_tickCount;
}

} // namespace fuse::mechanics
