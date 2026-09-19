#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/rotateComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Constant rotation leaf (GMK RotateComponent without SimObject/Con::).
class RotateComponent : public Component {
public:
    RotateComponent();
    explicit RotateComponent(std::string name, float degreesPerSecond = 0.f);

    const char* typeName() const override { return "RotateComponent"; }

    float degreesPerSecond() const { return m_degreesPerSecond; }
    void setDegreesPerSecond(float rate) { m_degreesPerSecond = rate; }

    float angleDeg() const { return m_angleDeg; }
    u32 tickCount() const { return m_tickCount; }

    void advance(float deltaSeconds);

private:
    float m_degreesPerSecond = 0.f;
    float m_angleDeg = 0.f;
    u32 m_tickCount = 0;
};

} // namespace fuse::mechanics
