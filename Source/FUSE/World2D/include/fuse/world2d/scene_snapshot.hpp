#pragma once

#include <fuse/handle.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse {
class SceneObject2D;
}

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

/// Parallel arrays for worker-friendly reads (game thread writes, jobs read). `worldMatrix[i]` is the
/// node's full cached world matrix (TRS composed along its parents; UNI-WP05-1), `worldX/Y` its
/// translation and `worldRotation` its heading about +Z.
struct SceneTransformSoA2D {
    std::vector<Handle<Object>> object;
    std::vector<float> worldX;
    std::vector<float> worldY;
    std::vector<float> worldRotation;
    std::vector<math::Mat4> worldMatrix;
    std::vector<u32> layer;

    void clear();
    void reserve(u32 spriteCount);
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

    /// Game-thread post-pass after hierarchy fill (e.g. per-frame spin for demo sprites): adds
    /// `rotation` to every draw command's world rotation (the SoA keeps the pure world transform).
    void setSpriteRotations(float rotation);

private:
    std::vector<SpriteDrawCmd> m_sprites;
    u32 m_visibleCount = 0;
};

/// Depth-first walk of node and descendants; fills snapshot + SoA with world transforms. The walk
/// refreshes each node's cached world matrix from its parent's (one pass, no per-node chain walk).
/// When includeNode is false, only descendants of node are recorded (container roots stay out).
void fillSnapshotSoA(const SceneObject2D& node,
                     SceneSnapshot2D& snapshot,
                     SceneTransformSoA2D& soa,
                     bool includeNode = true);

} // namespace fuse::world2d
