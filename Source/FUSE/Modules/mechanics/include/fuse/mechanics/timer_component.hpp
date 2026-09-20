#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/timerComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>

namespace fuse::mechanics {

/// Periodic timer leaf (GMK TimerComponent without SimObject/Con::).
class TimerComponent : public Component {
public:
    TimerComponent();
    explicit TimerComponent(std::string name, f32 periodSec = 1.f);

    const char* typeName() const override { return "TimerComponent"; }

    f32 periodSec() const { return m_periodSec; }
    void setPeriodSec(f32 periodSec) { m_periodSec = periodSec; }

    bool active() const { return m_active; }
    void setActive(bool active) { m_active = active; }

    void tick(f32 dt);
    void setOnFire(std::function<void()> callback) { m_onFire = std::move(callback); }
    u32 fireCount() const { return m_fireCount; }
    u32 callbackFireCount() const { return m_callbackFireCount; }
    f32 elapsedSec() const { return m_elapsedSec; }

private:
    f32 m_periodSec = 1.f;
    f32 m_elapsedSec = 0.f;
    bool m_active = true;
    u32 m_fireCount = 0;
    u32 m_callbackFireCount = 0;
    std::function<void()> m_onFire;
};

} // namespace fuse::mechanics
