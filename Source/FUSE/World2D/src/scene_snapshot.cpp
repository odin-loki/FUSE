#include <fuse/world2d/scene_snapshot.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

#include <cmath>

namespace fuse::world2d {

void SceneTransformSoA2D::clear() {
    object.clear();
    worldX.clear();
    worldY.clear();
    worldRotation.clear();
    worldMatrix.clear();
    layer.clear();
}

void SceneTransformSoA2D::reserve(u32 spriteCount) {
    object.reserve(spriteCount);
    worldX.reserve(spriteCount);
    worldY.reserve(spriteCount);
    worldRotation.reserve(spriteCount);
    worldMatrix.reserve(spriteCount);
    layer.reserve(spriteCount);
}

void SceneSnapshot2D::clear() {
    m_sprites.clear();
    m_visibleCount = 0;
}

void SceneSnapshot2D::reserve(u32 spriteCount) {
    m_sprites.reserve(spriteCount);
}

void SceneSnapshot2D::addSprite(const SpriteDrawCmd& cmd) {
    m_sprites.push_back(cmd);
}

void SceneSnapshot2D::setSpriteRotations(float rotation) {
    for (SpriteDrawCmd& cmd : m_sprites) {
        cmd.rotation += rotation;
    }
}

namespace {

void appendNode(const SceneObject2D& node, const SceneObject2D* parent, SceneSnapshot2D& snapshot,
                SceneTransformSoA2D& soa) {
    const math::Mat4& world = node.worldMatrixUnder(parent);
    const float wx = world.data[12];
    const float wy = world.data[13];
    const float rotation = std::atan2(world.data[1], world.data[0]);

    SpriteDrawCmd cmd;
    cmd.object = node.handle();
    cmd.x = wx;
    cmd.y = wy;
    cmd.rotation = rotation;
    cmd.layer = static_cast<u32>(node.layer());
    cmd.visible = true;
    snapshot.addSprite(cmd);

    soa.object.push_back(node.handle());
    soa.worldX.push_back(wx);
    soa.worldY.push_back(wy);
    soa.worldRotation.push_back(rotation);
    soa.worldMatrix.push_back(world);
    soa.layer.push_back(static_cast<u32>(node.layer()));

    for (Object* child : node.children()) {
        if (const SceneObject2D* child2d = asSceneObject2D(child)) {
            appendNode(*child2d, &node, snapshot, soa);
        }
    }
}

} // namespace

void fillSnapshotSoA(const SceneObject2D& node,
                     SceneSnapshot2D& snapshot,
                     SceneTransformSoA2D& soa,
                     bool includeNode) {
    snapshot.clear();
    soa.clear();
    // Bring the ancestor chain up to date once; children then refresh from their parent's cache
    // (worldMatrix() on a child whose parent is current is a single version compare).
    node.worldMatrix();
    if (includeNode) {
        appendNode(node, node.sceneParent(), snapshot, soa);
        return;
    }

    for (Object* child : node.children()) {
        if (const SceneObject2D* child2d = asSceneObject2D(child)) {
            appendNode(*child2d, &node, snapshot, soa);
        }
    }
}

} // namespace fuse::world2d
