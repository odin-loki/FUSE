#include <fuse/world2d/scene_snapshot.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse::world2d {

void SceneTransformSoA2D::clear() {
    object.clear();
    worldX.clear();
    worldY.clear();
    layer.clear();
}

void SceneTransformSoA2D::reserve(u32 spriteCount) {
    object.reserve(spriteCount);
    worldX.reserve(spriteCount);
    worldY.reserve(spriteCount);
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

namespace {

void appendNode(const SceneObject2D& node, SceneSnapshot2D& snapshot, SceneTransformSoA2D& soa) {
    const WorldTransform2D world = node.worldTransform();

    SpriteDrawCmd cmd;
    cmd.object = node.handle();
    cmd.x = world.x;
    cmd.y = world.y;
    cmd.layer = static_cast<u32>(node.layer());
    cmd.visible = true;
    snapshot.addSprite(cmd);

    soa.object.push_back(node.handle());
    soa.worldX.push_back(world.x);
    soa.worldY.push_back(world.y);
    soa.layer.push_back(static_cast<u32>(node.layer()));

    for (Object* child : node.children()) {
        if (const SceneObject2D* child2d = asSceneObject2D(child)) {
            appendNode(*child2d, snapshot, soa);
        }
    }
}

} // namespace

void fillSnapshotSoA(const SceneObject2D& node, SceneSnapshot2D& snapshot, SceneTransformSoA2D& soa) {
    snapshot.clear();
    soa.clear();
    appendNode(node, snapshot, soa);
}

} // namespace fuse::world2d
