#include <fuse/mechanics/polyhedron_trigger.hpp>

namespace fuse::mechanics {

void ConvexPolyhedron::add_half_space(float nx, float ny, float nz, float d) {
    planes.push_back({nx, ny, nz, d});
}

bool ConvexPolyhedron::contains(float x, float y, float z) const {
    for (const HalfSpace& plane : planes) {
        const float distance = plane.nx * x + plane.ny * y + plane.nz * z + plane.d;
        if (distance > 0.f) {
            return false;
        }
    }
    return !planes.empty();
}

ConvexPolyhedron ConvexPolyhedron::axis_aligned_box(float minX,
                                                    float minY,
                                                    float minZ,
                                                    float maxX,
                                                    float maxY,
                                                    float maxZ) {
    ConvexPolyhedron poly;
    poly.add_half_space(-1.f, 0.f, 0.f, minX);
    poly.add_half_space(1.f, 0.f, 0.f, -maxX);
    poly.add_half_space(0.f, -1.f, 0.f, minY);
    poly.add_half_space(0.f, 1.f, 0.f, -maxY);
    poly.add_half_space(0.f, 0.f, -1.f, minZ);
    poly.add_half_space(0.f, 0.f, 1.f, -maxZ);
    return poly;
}

PolyhedronTriggerZone::PolyhedronTriggerZone() = default;

PolyhedronTriggerZone::PolyhedronTriggerZone(std::string name, ConvexPolyhedron polyhedron)
    : Component(std::move(name)), m_polyhedron(std::move(polyhedron)) {}

void PolyhedronTriggerZone::testObject(u32 objectId, float x, float y, float z) {
    const bool inside = m_polyhedron.contains(x, y, z);
    const bool wasInside = m_inside.find(objectId) != m_inside.end();

    if (inside && !wasInside) {
        m_inside.insert(objectId);
        ++m_enterCount;
        if (m_onEnter) {
            m_onEnter(objectId);
        }
    } else if (!inside && wasInside) {
        m_inside.erase(objectId);
        ++m_leaveCount;
        if (m_onLeave) {
            m_onLeave(objectId);
        }
    }
}

} // namespace fuse::mechanics
