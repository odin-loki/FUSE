#pragma once

#include <fuse/object.hpp>
#include <fuse/types.hpp>

namespace fuse {

/// 2D scene node — xy transform, layer, sort key (WP-05 start).
/// Legacy T2D SceneObject adapters will wrap this type; do not inherit legacy GL types here.
class SceneObject2D : public Object {
public:
    SceneObject2D();
    explicit SceneObject2D(std::string name);

    const char* typeName() const override { return "SceneObject2D"; }

    float x() const { return m_x; }
    float y() const { return m_y; }
    void setPosition(float x, float y);

    s32 layer() const { return m_layer; }
    void setLayer(s32 layer) { m_layer = layer; }

    u32 sortKey() const { return m_sortKey; }
    void setSortKey(u32 key) { m_sortKey = key; }

private:
    float m_x = 0.f;
    float m_y = 0.f;
    s32 m_layer = 0;
    u32 m_sortKey = 0;
};

} // namespace fuse
