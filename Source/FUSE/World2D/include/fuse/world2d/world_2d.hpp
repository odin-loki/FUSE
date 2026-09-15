#pragma once

#include <fuse/dimension/idimension.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/scene_snapshot.hpp>

#include <memory>
#include <vector>

namespace fuse::world2d {

/// 2D dimension — composes legacy T2D backends; does not inherit Box2D or GL types.
class World2D : public dimension::IDimension {
public:
    World2D();
    ~World2D() override;

    const char* dimensionName() const override { return "World2D"; }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }

    void tick(frame::FrameCtx& ctx) override;
    void render(frame::FrameCtx& ctx) override;

    void loadWorld(dimension::WorldHandle world) override;
    dimension::WorldHandle activeWorld() const override { return m_activeWorld; }

    SceneObject2D* root() { return m_root.get(); }
    const SceneObject2D* root() const { return m_root.get(); }

    void addSprite(SceneObject2D* sprite);
    const SceneSnapshot2D& readSnapshot() const { return m_snapshot; }

private:
    void buildSnapshot(frame::FrameCtx& ctx);
    void runParallelCull();

    bool m_enabled = true;
    dimension::WorldHandle m_activeWorld = dimension::WorldHandle::invalid();
    std::unique_ptr<SceneObject2D> m_root;
    std::vector<SceneObject2D*> m_sprites;

    SceneSnapshot2D m_snapshot;
    std::vector<bool> m_cullVisible;
};

} // namespace fuse::world2d
