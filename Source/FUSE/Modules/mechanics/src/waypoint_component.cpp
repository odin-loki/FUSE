#include <fuse/mechanics/waypoint_component.hpp>

namespace fuse::mechanics {

WaypointComponent::WaypointComponent() : Component("waypoint") {}

WaypointComponent::WaypointComponent(std::string name, f32 x, f32 y, f32 z)
    : Component(std::move(name)), m_x(x), m_y(y), m_z(z) {}

void WaypointComponent::setPosition(f32 x, f32 y, f32 z) {
    m_x = x;
    m_y = y;
    m_z = z;
}

void WaypointComponent::markVisited() {
    m_visited = true;
    ++m_visitCount;
}

} // namespace fuse::mechanics
