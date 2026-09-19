#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/followComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Follow-target leaf (GMK FollowComponent without SimObject/Con::).
class FollowComponent : public Component {
public:
    FollowComponent();
    explicit FollowComponent(std::string name, f32 speed = 1.f);

    const char* typeName() const override { return "FollowComponent"; }

    f32 speed() const { return m_speed; }
    void setSpeed(f32 speed) { m_speed = speed; }

    u32 targetObjectId() const { return m_targetObjectId; }
    void setTargetObjectId(u32 objectId) { m_targetObjectId = objectId; }

    f32 x() const { return m_x; }
    f32 y() const { return m_y; }
    f32 z() const { return m_z; }

    void setPosition(f32 x, f32 y, f32 z);
    void advanceToward(f32 targetX, f32 targetY, f32 targetZ, f32 dt);

    u32 tickCount() const { return m_tickCount; }
    bool arrived() const { return m_arrived; }

private:
    f32 m_speed = 1.f;
    u32 m_targetObjectId = 0;
    f32 m_x = 0.f;
    f32 m_y = 0.f;
    f32 m_z = 0.f;
    bool m_arrived = true;
    u32 m_tickCount = 0;
};

} // namespace fuse::mechanics
