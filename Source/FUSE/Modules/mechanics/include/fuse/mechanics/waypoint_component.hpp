#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/waypointComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Named waypoint marker leaf (GMK WaypointComponent without SimObject/Con::).
class WaypointComponent : public Component {
public:
    WaypointComponent();
    explicit WaypointComponent(std::string name, f32 x = 0.f, f32 y = 0.f, f32 z = 0.f);

    const char* typeName() const override { return "WaypointComponent"; }

    f32 x() const { return m_x; }
    f32 y() const { return m_y; }
    f32 z() const { return m_z; }

    void setPosition(f32 x, f32 y, f32 z);
    void markVisited();
    bool visited() const { return m_visited; }
    u32 visitCount() const { return m_visitCount; }

private:
    f32 m_x = 0.f;
    f32 m_y = 0.f;
    f32 m_z = 0.f;
    bool m_visited = false;
    u32 m_visitCount = 0;
};

} // namespace fuse::mechanics
