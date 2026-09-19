#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/simpleComponent.h (switch leaf)

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Named on/off switch leaf (GMK switch SimComponent distilled without Con::).
class SwitchComponent : public Component {
public:
    SwitchComponent();
    SwitchComponent(std::string name, bool initialState, std::string onLabel, std::string offLabel);

    const char* typeName() const override { return "SwitchComponent"; }

    bool state() const { return m_state; }
    const std::string& onLabel() const { return m_onLabel; }
    const std::string& offLabel() const { return m_offLabel; }
    u32 switchCount() const { return m_switchCount; }

    bool flip();
    void setState(bool state);

private:
    bool m_state = false;
    std::string m_onLabel = "on";
    std::string m_offLabel = "off";
    u32 m_switchCount = 0;
};

} // namespace fuse::mechanics
