#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/animateComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Keyframe animation leaf (GMK AnimateComponent without SimObject/Con::).
class AnimateComponent : public Component {
public:
    AnimateComponent();
    explicit AnimateComponent(std::string name, f32 durationSec = 1.f);

    const char* typeName() const override { return "AnimateComponent"; }

    f32 durationSec() const { return m_durationSec; }
    void setDurationSec(f32 durationSec) { m_durationSec = durationSec; }

    bool active() const { return m_active; }
    void setActive(bool active) { m_active = active; }

    void advance(f32 dt);
    f32 normalizedTime() const { return m_normalizedTime; }
    u32 cycleCount() const { return m_cycleCount; }

private:
    f32 m_durationSec = 1.f;
    f32 m_elapsedSec = 0.f;
    f32 m_normalizedTime = 0.f;
    bool m_active = true;
    u32 m_cycleCount = 0;
};

} // namespace fuse::mechanics
