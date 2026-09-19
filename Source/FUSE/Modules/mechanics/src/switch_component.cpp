#include <fuse/mechanics/switch_component.hpp>

namespace fuse::mechanics {

SwitchComponent::SwitchComponent() : Component("switch") {}

SwitchComponent::SwitchComponent(std::string name, bool initialState, std::string onLabel, std::string offLabel)
    : Component(std::move(name))
    , m_state(initialState)
    , m_onLabel(std::move(onLabel))
    , m_offLabel(std::move(offLabel)) {}

bool SwitchComponent::flip() {
    m_state = !m_state;
    ++m_switchCount;
    return m_state;
}

void SwitchComponent::setState(bool state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    ++m_switchCount;
}

} // namespace fuse::mechanics
