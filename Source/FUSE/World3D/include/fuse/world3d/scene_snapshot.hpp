#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::world3d {

struct ObjectDrawCmd3D {
    Handle<Object> object = Handle<Object>::invalid();
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    bool visible = true;
};

class SceneSnapshot3D {
public:
    void clear();
    void reserve(u32 objectCount);
    void addObject(const ObjectDrawCmd3D& cmd);

    const std::vector<ObjectDrawCmd3D>& objects() const { return m_objects; }
    u32 visibleCount() const { return m_visibleCount; }

    void setVisibleCount(u32 count) { m_visibleCount = count; }

private:
    std::vector<ObjectDrawCmd3D> m_objects;
    u32 m_visibleCount = 0;
};

} // namespace fuse::world3d
