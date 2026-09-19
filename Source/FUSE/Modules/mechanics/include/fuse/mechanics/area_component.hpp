#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/areaComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Area-volume leaf (GMK AreaComponent without SimObject/Con::).
class AreaComponent : public Component {
public:
    AreaComponent();
    explicit AreaComponent(std::string name, float radius = 1.f);

    const char* typeName() const override { return "AreaComponent"; }

    float radius() const { return m_radius; }
    void setRadius(float radius) { m_radius = radius; }

    bool contains(float x, float y, float z) const;
    bool containsPoint(float x, float y, float z) const { return contains(x, y, z); }

    u32 enterCount() const { return m_enterCount; }
    u32 leaveCount() const { return m_leaveCount; }
    bool testObject(u32 objectId, float x, float y, float z);

private:
    float m_radius = 1.f;
    u32 m_trackedObjectId = 0;
    bool m_objectInside = false;
    u32 m_enterCount = 0;
    u32 m_leaveCount = 0;
};

} // namespace fuse::mechanics
