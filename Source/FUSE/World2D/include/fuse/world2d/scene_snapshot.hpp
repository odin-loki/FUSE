#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::world2d {

/// Immutable draw data for worker cull — no raw SceneObject* crosses threads.
struct SpriteDrawCmd {
    Handle<Object> object = Handle<Object>::invalid();
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;
    u32 layer = 0;
    bool visible = true;
};

/// Double-buffered snapshot built on the game thread, read by parallel cull jobs.
class SceneSnapshot2D {
public:
    void clear();
    void reserve(u32 spriteCount);
    void addSprite(const SpriteDrawCmd& cmd);

    const std::vector<SpriteDrawCmd>& sprites() const { return m_sprites; }
    u32 visibleCount() const { return m_visibleCount; }

    void setVisibleCount(u32 count) { m_visibleCount = count; }

private:
    std::vector<SpriteDrawCmd> m_sprites;
    u32 m_visibleCount = 0;
};

} // namespace fuse::world2d
