#include <fuse/mechanics/delay_component.hpp>

namespace fuse::mechanics {

DelayComponent::DelayComponent() = default;

DelayComponent::DelayComponent(std::string name, u32 delayMs)
    : Component(std::move(name))
    , m_delayMs(delayMs) {}

void DelayComponent::advance(u32 deltaMs) {
    if (m_fired || m_delayMs == 0) {
        return;
    }

    m_elapsedMs += deltaMs;
    if (m_elapsedMs >= m_delayMs) {
        m_fired = true;
        ++m_fireCount;
    }
}

void DelayComponent::reset() {
    m_elapsedMs = 0;
    m_fired = false;
}

} // namespace fuse::mechanics
