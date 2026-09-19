#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/pathComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::mechanics {

struct PathWaypoint {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
};

/// Path-follow leaf (GMK PathComponent without SimObject/Con::).
class PathComponent : public Component {
public:
    PathComponent();
    explicit PathComponent(std::string name);

    const char* typeName() const override { return "PathComponent"; }

    void addWaypoint(f32 x, f32 y, f32 z);
    u32 waypointCount() const { return static_cast<u32>(m_waypoints.size()); }
    u32 pathIndex() const { return m_pathIndex; }

    f32 x() const { return m_x; }
    f32 y() const { return m_y; }
    f32 z() const { return m_z; }

    void setPosition(f32 x, f32 y, f32 z);
    void advanceAlongPath(f32 speed, f32 dt);

    u32 tickCount() const { return m_tickCount; }
    bool finished() const { return m_finished; }

private:
    std::vector<PathWaypoint> m_waypoints;
    u32 m_pathIndex = 0;
    f32 m_x = 0.f;
    f32 m_y = 0.f;
    f32 m_z = 0.f;
    bool m_finished = false;
    u32 m_tickCount = 0;
};

} // namespace fuse::mechanics
