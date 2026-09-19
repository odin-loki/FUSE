#pragma once

// Ore: Engine/source/T3D/trigger.h (onEnterTrigger / onLeaveTrigger / tickPeriodMS)

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <unordered_set>

namespace fuse::mechanics {

struct AxisAlignedBox {
    float minX = 0.f;
    float minY = 0.f;
    float minZ = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    float maxZ = 0.f;

    bool contains(float x, float y, float z) const;
};

using TriggerEnterCallback = std::function<void(u32 objectId)>;
using TriggerLeaveCallback = std::function<void(u32 objectId)>;
using TriggerTickCallback = std::function<void()>;

/// Volume trigger component (T3D Trigger without polyhedron / physics bridge).
class TriggerZoneComponent : public Component {
public:
    TriggerZoneComponent();
    explicit TriggerZoneComponent(std::string name, AxisAlignedBox bounds);

    const char* typeName() const override { return "TriggerZoneComponent"; }

    const AxisAlignedBox& bounds() const { return m_bounds; }
    void setBounds(const AxisAlignedBox& bounds) { m_bounds = bounds; }

    u32 tickPeriodMs() const { return m_tickPeriodMs; }
    void setTickPeriodMs(u32 periodMs) { m_tickPeriodMs = periodMs; }

    void setOnEnter(TriggerEnterCallback callback) { m_onEnter = std::move(callback); }
    void setOnLeave(TriggerLeaveCallback callback) { m_onLeave = std::move(callback); }
    void setOnTick(TriggerTickCallback callback) { m_onTick = std::move(callback); }

    u32 enterCount() const { return m_enterCount; }
    u32 leaveCount() const { return m_leaveCount; }
    u32 tickCount() const { return m_tickCount; }

    /// Test one object position; fires enter/leave callbacks on transitions.
    void testObject(u32 objectId, float x, float y, float z);

    /// Advance periodic tick when `elapsedMs` crosses `tickPeriodMs`.
    void advance(u32 elapsedMs);

private:
    AxisAlignedBox m_bounds;
    u32 m_tickPeriodMs = 0;
    u32 m_lastTickMs = 0;
    u32 m_enterCount = 0;
    u32 m_leaveCount = 0;
    u32 m_tickCount = 0;
    std::unordered_set<u32> m_inside;
    TriggerEnterCallback m_onEnter;
    TriggerLeaveCallback m_onLeave;
    TriggerTickCallback m_onTick;
};

} // namespace fuse::mechanics
