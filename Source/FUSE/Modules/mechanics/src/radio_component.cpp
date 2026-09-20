#include <fuse/mechanics/radio_component.hpp>

namespace fuse::mechanics {

RadioComponent::RadioComponent() : Component("radio") {}

RadioComponent::RadioComponent(std::string name, std::string channel)
    : Component(std::move(name)), m_channel(std::move(channel)) {}

void RadioComponent::startBroadcast() {
    m_broadcasting = true;
    ++m_broadcastCount;
}

void RadioComponent::stopBroadcast() {
    m_broadcasting = false;
}

} // namespace fuse::mechanics
