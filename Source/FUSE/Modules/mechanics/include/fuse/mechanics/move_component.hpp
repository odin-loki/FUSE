#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/moveComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Move-to-target leaf (GMK MoveComponent without SimObject/Con::).
class MoveComponent : public Component {
public:
    MoveComponent();
    explicit MoveComponent(std::string name, f32 speed = 1.f);

    const char* typeName() const override { return "MoveComponent"; }

    f32 speed() const { return m_speed; }
    void setSpeed(f32 speed) { m_speed = speed; }

    f32 x() const { return m_x; }
    f32 y() const { return m_y; }
    f32 z() const { return m_z; }

    void setTarget(f32 x, f32 y, f32 z);
    void advance(f32 dt);

    u32 tickCount() const { return m_tickCount; }
    bool arrived() const { return m_arrived; }

private:
    f32 m_speed = 1.f;
    f32 m_x = 0.f;
    f32 m_y = 0.f;
    f32 m_z = 0.f;
    f32 m_targetX = 0.f;
    f32 m_targetY = 0.f;
    f32 m_targetZ = 0.f;
    bool m_arrived = true;
    u32 m_tickCount = 0;
};

} // namespace fuse::mechanics
