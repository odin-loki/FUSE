#pragma once

// Ore: Engine/source/T3D/trigger.h (Polyhedron mTriggerPolyhedron) + math/mPolyhedron.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <unordered_set>
#include <vector>

namespace fuse::mechanics {

/// Convex polyhedron as intersection of inward-facing half-spaces (T3D Trigger ore).
struct ConvexPolyhedron {
    struct HalfSpace {
        float nx = 0.f;
        float ny = 0.f;
        float nz = 0.f;
        float d = 0.f;
    };

    std::vector<HalfSpace> planes;

    void add_half_space(float nx, float ny, float nz, float d);
    bool contains(float x, float y, float z) const;

    static ConvexPolyhedron axis_aligned_box(float minX,
                                             float minY,
                                             float minZ,
                                             float maxX,
                                             float maxY,
                                             float maxZ);
};

using PolyhedronEnterCallback = std::function<void(u32 objectId)>;
using PolyhedronLeaveCallback = std::function<void(u32 objectId)>;

/// Volume trigger using convex polyhedron containment (T3D Trigger without physics bridge).
class PolyhedronTriggerZone : public Component {
public:
    PolyhedronTriggerZone();
    explicit PolyhedronTriggerZone(std::string name, ConvexPolyhedron polyhedron);

    const char* typeName() const override { return "PolyhedronTriggerZone"; }

    const ConvexPolyhedron& polyhedron() const { return m_polyhedron; }
    void setPolyhedron(const ConvexPolyhedron& polyhedron) { m_polyhedron = polyhedron; }

    void setOnEnter(PolyhedronEnterCallback callback) { m_onEnter = std::move(callback); }
    void setOnLeave(PolyhedronLeaveCallback callback) { m_onLeave = std::move(callback); }

    u32 enterCount() const { return m_enterCount; }
    u32 leaveCount() const { return m_leaveCount; }

    void testObject(u32 objectId, float x, float y, float z);

private:
    ConvexPolyhedron m_polyhedron;
    u32 m_enterCount = 0;
    u32 m_leaveCount = 0;
    std::unordered_set<u32> m_inside;
    PolyhedronEnterCallback m_onEnter;
    PolyhedronLeaveCallback m_onLeave;
};

} // namespace fuse::mechanics
