#include <fuse/world3d/scene_snapshot.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world2d/transform_stubs.hpp>

#include <cstring>

namespace fuse::world3d {

void SceneTransformSoA3D::clear() {
    object.clear();
    worldX.clear();
    worldY.clear();
    worldZ.clear();
}

void SceneTransformSoA3D::reserve(u32 objectCount) {
    object.reserve(objectCount);
    worldX.reserve(objectCount);
    worldY.reserve(objectCount);
    worldZ.reserve(objectCount);
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

void appendNode(const SceneObject3D& node, SceneSnapshot3D& snapshot, SceneTransformSoA3D& soa) {
    const WorldTransform3D world = node.worldTransform3D();

    ObjectDrawCmd3D cmd;
    cmd.object = node.handle();
    cmd.x = world.x;
    cmd.y = world.y;
    cmd.z = world.z;
    cmd.visible = true;
    snapshot.addObject(cmd);

    soa.object.push_back(node.handle());
    soa.worldX.push_back(world.x);
    soa.worldY.push_back(world.y);
    soa.worldZ.push_back(world.z);

    for (Object* child : node.children()) {
        if (child != nullptr && std::strcmp(child->typeName(), "SceneObject3D") == 0) {
            appendNode(*static_cast<SceneObject3D*>(child), snapshot, soa);
        } else if (const SceneObject2D* child2d = asSceneObject2D(child)) {
            const WorldTransform2D world2d = child2d->worldTransform();
            ObjectDrawCmd3D childCmd;
            childCmd.object = child2d->handle();
            childCmd.x = world2d.x;
            childCmd.y = world2d.y;
            childCmd.z = 0.f;
            childCmd.visible = true;
            snapshot.addObject(childCmd);

            soa.object.push_back(child2d->handle());
            soa.worldX.push_back(world2d.x);
            soa.worldY.push_back(world2d.y);
            soa.worldZ.push_back(0.f);
        }
    }
}

} // namespace

void fillSnapshotSoA(const SceneObject3D& node, SceneSnapshot3D& snapshot, SceneTransformSoA3D& soa) {
    snapshot.clear();
    soa.clear();
    appendNode(node, snapshot, soa);
}

} // namespace fuse::world3d
