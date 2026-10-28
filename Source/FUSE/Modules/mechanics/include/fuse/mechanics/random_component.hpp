#pragma once

// Ore: ore/torque-addon/GMK/Engine/source/component/randomComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Deterministic xorshift32 leaf (GMK RandomComponent without SimObject/Con::).
/// next() returns [0, 1). tick() draws one sample only when stepOnTick is set.
class RandomComponent : public Component {
public:
    RandomComponent();
    explicit RandomComponent(std::string name, u32 seed = 1u);

    const char* typeName() const override { return "RandomComponent"; }

    u32 seed() const { return m_seed; }
    void setSeed(u32 seed);

    bool stepOnTick() const { return m_stepOnTick; }
    void setStepOnTick(bool stepOnTick) { m_stepOnTick = stepOnTick; }

    /// Next sample in [0, 1). Same seed always replays the same sequence.
    f32 next();

    /// Draws one sample when stepOnTick is set. Otherwise leaves the sequence alone.
    void tick();

    f32 value() const { return m_value; }
    u32 drawCount() const { return m_drawCount; }
    u32 tickCount() const { return m_tickCount; }

private:
    u32 m_seed = 1u;
    u32 m_state = 1u;
    bool m_stepOnTick = false;
    f32 m_value = 0.f;
    u32 m_drawCount = 0;
    u32 m_tickCount = 0;
};

} // namespace fuse::mechanics
