#include <fuse/world3d/scene_snapshot.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world2d/scene_transform.hpp>

namespace fuse::world3d {

void SceneTransformSoA3D::clear() {
    object.clear();
    worldX.clear();
    worldY.clear();
    worldZ.clear();
    worldMatrix.clear();
}

void SceneTransformSoA3D::reserve(u32 objectCount) {
    object.reserve(objectCount);
    worldX.reserve(objectCount);
    worldY.reserve(objectCount);
    worldZ.reserve(objectCount);
    worldMatrix.reserve(objectCount);
}

void SceneSnapshot3D::clear() {
    m_objects.clear();
    m_visibleCount = 0;
}

void SceneSnapshot3D::reserve(u32 objectCount) {
    m_objects.reserve(objectCount);
}

void SceneSnapshot3D::addObject(const ObjectDrawCmd3D& cmd) {
    m_objects.push_back(cmd);
}

namespace {

/// 2D nodes under a 3D hierarchy are recorded too (their world matrix composes through the 3D
/// parents; z comes from the matrix).
void appendNode(const SceneObject2D& node, const SceneObject2D* parent, SceneSnapshot3D& snapshot,
                SceneTransformSoA3D& soa) {
    const math::Mat4& world = node.worldMatrixUnder(parent);

    ObjectDrawCmd3D cmd;
    cmd.object = node.handle();
    cmd.x = world.data[12];
    cmd.y = world.data[13];
    cmd.z = world.data[14];
    cmd.visible = true;
    snapshot.addObject(cmd);

    soa.object.push_back(node.handle());
    soa.worldX.push_back(cmd.x);
    soa.worldY.push_back(cmd.y);
    soa.worldZ.push_back(cmd.z);
    soa.worldMatrix.push_back(world);

    for (Object* child : node.children()) {
        if (const SceneObject2D* childNode = asSceneObject2D(child)) {
            appendNode(*childNode, &node, snapshot, soa);
        }
    }
}

} // namespace

void fillSnapshotSoA(const SceneObject3D& node,
                     SceneSnapshot3D& snapshot,
                     SceneTransformSoA3D& soa,
                     bool includeNode) {
    snapshot.clear();
    soa.clear();
    node.worldMatrix();
    if (includeNode) {
        appendNode(node, node.sceneParent(), snapshot, soa);
        return;
    }

    for (Object* child : node.children()) {
        if (const SceneObject2D* childNode = asSceneObject2D(child)) {
            appendNode(*childNode, &node, snapshot, soa);
        }
    }
}

} // namespace fuse::world3d
