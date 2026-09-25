#pragma once

#include <fuse/dimension/idimension.hpp>
#include <fuse/physics/physics_world_2d.hpp>
#include <fuse/world2d/cull_fork_join.hpp>
#include <fuse/world2d/fuselevel_bridge.hpp>
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

    /// Game-thread phase: physics step + immutable snapshot build (no worker reads yet).
    void tickGameThread(frame::FrameCtx& ctx);
    /// Worker-safe cull over the snapshot built by tickGameThread (heap-free JobScheduler fork-join).
    void runParallelCull();

    void loadWorld(dimension::WorldHandle world) override;
    dimension::WorldHandle activeWorld() const override { return m_activeWorld; }

    /// Optional `.fuselevel` path consumed by the next `loadWorld()` call.
    void setFuselevelPath(std::string path) { m_pendingFuselevelPath = std::move(path); }
    const std::string& pendingFuselevelPath() const { return m_pendingFuselevelPath; }

    /// Optional project root + `defaultWorld2D` relative path for `loadWorld()` when no fuselevel path is set.
    void setProjectWorldSource(std::string projectRoot, std::string defaultWorld2DRel);
    const std::string& projectRoot() const { return m_projectRoot; }
    const std::string& defaultWorld2DRel() const { return m_defaultWorld2DRel; }

    const FuselevelLoadResult& lastFuselevelLoad() const { return m_lastFuselevelLoad; }

    /// Load a converted 2D world file immediately (also used by `loadWorld()` when a path is set).
    FuselevelLoadResult loadWorldFromFuselevel(const std::string& fuselevelPath);

    SceneObject2D* root() { return m_root.get(); }
    const SceneObject2D* root() const { return m_root.get(); }

    void addSprite(SceneObject2D* sprite);
    void adoptOwnedSprite(std::unique_ptr<SceneObject2D> sprite);
    const SceneSnapshot2D& readSnapshot() const { return m_snapshot; }
    const SceneTransformSoA2D& readTransformSoA() const { return m_transformSoA; }

    physics::PhysicsWorld2D& physics() { return m_physics; }
    const physics::PhysicsWorld2D& physics() const { return m_physics; }
    void setPhysicsEnabled(bool enabled);
    bool isPhysicsEnabled() const { return m_physicsEnabled; }

private:
    static constexpr u32 kNoPhysicsBody = 0xFFFFFFFFu;

    void attachPhysicsBodyForSprite_(SceneObject2D* sprite);
    void rebuildPhysicsBodies_();
    void buildSnapshot(frame::FrameCtx& ctx);
    void syncPhysicsFromScene();
    void syncSceneFromPhysics();
    void clearLoadedSprites_();

    bool m_enabled = true;
    dimension::WorldHandle m_activeWorld = dimension::WorldHandle::invalid();
    std::string m_pendingFuselevelPath;
    std::string m_projectRoot;
    std::string m_defaultWorld2DRel;
    FuselevelLoadResult m_lastFuselevelLoad{};
    std::unique_ptr<SceneObject2D> m_root;
    std::vector<SceneObject2D*> m_sprites;
    std::vector<std::unique_ptr<SceneObject2D>> m_ownedSprites;

    SceneSnapshot2D m_snapshot;
    SceneTransformSoA2D m_transformSoA;
    /// One byte per entry (not vector<bool>): cull jobs write neighbouring entries concurrently.
    std::vector<u8> m_cullVisible;
    CullForkJoin m_cullJobs;

    physics::PhysicsWorld2D m_physics;
    std::vector<u32> m_physicsBodyIndices;
    bool m_physicsEnabled = false;
};

} // namespace fuse::world2d
