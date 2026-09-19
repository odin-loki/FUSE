#include <fuse/mechanics/message_component.hpp>

namespace fuse::mechanics {

MessageComponent::MessageComponent() = default;

MessageComponent::MessageComponent(std::string name, std::string message)
    : Component(std::move(name)), m_message(std::move(message)) {}

void MessageComponent::setMessage(std::string message) {
    m_message = std::move(message);
}

void MessageComponent::send() {
    if (m_message.empty()) {
        return;
    }

    m_sentMessages.push_back(m_message);
    ++m_sendCount;
}

} // namespace fuse::mechanics
