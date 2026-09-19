#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/messageComponent.h

#include <fuse/mechanics/component.hpp>

#include <string>
#include <vector>

namespace fuse::mechanics {

/// Console message leaf (GMK MessageComponent without SimObject/Con::).
class MessageComponent : public Component {
public:
    MessageComponent();
    explicit MessageComponent(std::string name, std::string message = {});

    const char* typeName() const override { return "MessageComponent"; }

    const std::string& message() const { return m_message; }
    const std::vector<std::string>& sentMessages() const { return m_sentMessages; }
    u32 sendCount() const { return m_sendCount; }

    void setMessage(std::string message);
    void send();

private:
    std::string m_message;
    std::vector<std::string> m_sentMessages;
    u32 m_sendCount = 0;
};

} // namespace fuse::mechanics
