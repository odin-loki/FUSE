#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/radioComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Radio broadcast leaf (GMK RadioComponent without SimObject/Con::).
class RadioComponent : public Component {
public:
    RadioComponent();
    explicit RadioComponent(std::string name, std::string channel = "default");

    const char* typeName() const override { return "RadioComponent"; }

    const std::string& channel() const { return m_channel; }
    void setChannel(const std::string& channel) { m_channel = channel; }

    bool broadcasting() const { return m_broadcasting; }
    u32 broadcastCount() const { return m_broadcastCount; }

    void startBroadcast();
    void stopBroadcast();

private:
    std::string m_channel = "default";
    bool m_broadcasting = false;
    u32 m_broadcastCount = 0;
};

} // namespace fuse::mechanics
