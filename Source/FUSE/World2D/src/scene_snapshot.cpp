#include <fuse/world2d/scene_snapshot.hpp>

namespace fuse::world2d {

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

} // namespace fuse::world2d
