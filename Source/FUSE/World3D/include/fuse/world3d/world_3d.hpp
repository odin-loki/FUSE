#pragma once

#include <fuse/dimension/idimension.hpp>
#include <fuse/physics/physics_world_3d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/scene_snapshot.hpp>

#include <memory>
#include <vector>

namespace fuse::world3d {

/// 3D dimension — composes T3D collision/render backends; no Box2D inheritance.
class World3D : public dimension::IDimension {
public:
    World3D();
    ~World3D() override;

    const char* dimensionName() const override { return "World3D"; }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }

    void tick(frame::FrameCtx& ctx) override;
    void render(frame::FrameCtx& ctx) override;

    void loadWorld(dimension::WorldHandle world) override;
    dimension::WorldHandle activeWorld() const override { return m_activeWorld; }

    SceneObject3D* root() { return m_root.get(); }
    const SceneObject3D* root() const { return m_root.get(); }

    void addObject(SceneObject3D* object);
    const SceneSnapshot3D& readSnapshot() const { return m_snapshot; }

    float clearColorR() const { return m_clearR; }
    float clearColorG() const { return m_clearG; }
    float clearColorB() const { return m_clearB; }
    void setClearColor(float r, float g, float b);

    physics::PhysicsWorld3D& physics() { return m_physics; }
    const physics::PhysicsWorld3D& physics() const { return m_physics; }
    void setPhysicsEnabled(bool enabled) { m_physicsEnabled = enabled; }
    bool isPhysicsEnabled() const { return m_physicsEnabled; }

private:
    void buildSnapshot();
    void runParallelCull();
    void syncPhysicsFromScene();
    void syncSceneFromPhysics();

    bool m_enabled = true;
    dimension::WorldHandle m_activeWorld = dimension::WorldHandle::invalid();
    std::unique_ptr<SceneObject3D> m_root;
    std::vector<SceneObject3D*> m_objects;

    SceneSnapshot3D m_snapshot;
    std::vector<bool> m_cullVisible;

    float m_clearR = 0.1f;
    float m_clearG = 0.15f;
    float m_clearB = 0.25f;

    physics::PhysicsWorld3D m_physics;
    std::vector<u32> m_physicsBodyIndices;
    bool m_physicsEnabled = false;
};

} // namespace fuse::world3d
