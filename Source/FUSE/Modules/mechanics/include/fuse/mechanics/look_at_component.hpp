#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/lookAtComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <string>

namespace fuse::mechanics {

/// Face-target leaf (GMK LookAtComponent without SimObject/Con::).
class LookAtComponent : public Component {
public:
    LookAtComponent();
    explicit LookAtComponent(std::string name, f32 turnSpeedDeg = 90.f);

    const char* typeName() const override { return "LookAtComponent"; }

    f32 yawDeg() const { return m_yawDeg; }
    f32 turnSpeedDeg() const { return m_turnSpeedDeg; }
    void setTurnSpeedDeg(f32 speed) { m_turnSpeedDeg = speed; }

    void setTarget(f32 targetX, f32 targetY);
    void advanceTowardTarget(f32 x, f32 y, f32 dt);

    u32 tickCount() const { return m_tickCount; }
    bool aligned() const { return m_aligned; }

private:
    f32 m_yawDeg = 0.f;
    f32 m_turnSpeedDeg = 90.f;
    f32 m_targetX = 0.f;
    f32 m_targetY = 0.f;
    bool m_aligned = true;
    u32 m_tickCount = 0;
};

} // namespace fuse::mechanics
