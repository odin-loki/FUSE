#pragma once

// Ore: ore/torque-addon/GMK/Engine/source/component/proximityComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <unordered_set>

namespace fuse::mechanics {

using ProximityEnterCallback = std::function<void(u32 objectId)>;
using ProximityLeaveCallback = std::function<void(u32 objectId)>;

/// Sphere proximity leaf (GMK ProximityComponent without SimObject/Con::).
/// Watches a point against a radius. Enter and leave fire once per crossing,
/// the same edge policy as TriggerZoneComponent (no physics library).
class ProximityComponent : public Component {
public:
    ProximityComponent();
    explicit ProximityComponent(std::string name, f32 radius = 1.f);

    const char* typeName() const override { return "ProximityComponent"; }

    f32 radius() const { return m_radius; }
    void setRadius(f32 radius) { m_radius = radius > 0.f ? radius : 0.f; }

    f32 centerX() const { return m_centerX; }
    f32 centerY() const { return m_centerY; }
    f32 centerZ() const { return m_centerZ; }
    void setCenter(f32 x, f32 y, f32 z) {
        m_centerX = x;
        m_centerY = y;
        m_centerZ = z;
    }

    void setOnEnter(ProximityEnterCallback callback) { m_onEnter = std::move(callback); }
    void setOnLeave(ProximityLeaveCallback callback) { m_onLeave = std::move(callback); }

    u32 enterCount() const { return m_enterCount; }
    u32 leaveCount() const { return m_leaveCount; }

    /// Inclusive sphere test around the center. The surface counts as inside.
    bool containsPoint(f32 x, f32 y, f32 z) const;

    /// Test one point. Fires enter/leave only on an outside/inside transition.
    void testObject(u32 objectId, f32 x, f32 y, f32 z);

private:
    f32 m_centerX = 0.f;
    f32 m_centerY = 0.f;
    f32 m_centerZ = 0.f;
    f32 m_radius = 1.f;
    u32 m_enterCount = 0;
    u32 m_leaveCount = 0;
    std::unordered_set<u32> m_inside;
    ProximityEnterCallback m_onEnter;
    ProximityLeaveCallback m_onLeave;
};

} // namespace fuse::mechanics
