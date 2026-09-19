#include <fuse/mechanics/timer_component.hpp>

namespace fuse::mechanics {

TimerComponent::TimerComponent() : Component("TimerComponent") {}

TimerComponent::TimerComponent(std::string name, f32 periodSec)
    : Component(std::move(name)), m_periodSec(periodSec) {}

void TimerComponent::tick(f32 dt) {
    if (!m_active || m_periodSec <= 0.f) {
        return;
    }

    m_elapsedSec += dt;
    while (m_elapsedSec >= m_periodSec) {
        m_elapsedSec -= m_periodSec;
        ++m_fireCount;
    }
}

} // namespace fuse::mechanics
