#pragma once

#include <fuse/handle.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse {
class SceneObject3D;
}

namespace fuse::world3d {

struct ObjectDrawCmd3D {
    Handle<Object> object = Handle<Object>::invalid();
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    bool visible = true;
};

/// Worker-readable transform columns. `worldMatrix[i]` is the node's full cached world matrix
/// (parent chain TRS composition, UNI-WP05-1); worldX/Y/Z are its translation.
struct SceneTransformSoA3D {
    std::vector<Handle<Object>> object;
    std::vector<float> worldX;
    std::vector<float> worldY;
    std::vector<float> worldZ;
    std::vector<math::Mat4> worldMatrix;

    void clear();
    void reserve(u32 objectCount);
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

/// Depth-first fill (2D and 3D scene nodes) with the cached world matrices, refreshed top-down in the
/// same pass. When includeNode is false, only descendants of node are recorded (container roots stay out).
void fillSnapshotSoA(const SceneObject3D& node,
                     SceneSnapshot3D& snapshot,
                     SceneTransformSoA3D& soa,
                     bool includeNode = true);

} // namespace fuse::world3d
