#pragma once

#include <fuse/dimension/idimension.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/world2d/cull_fork_join.hpp>
#include <fuse/world2d/scene_handle_table.hpp>
#include <fuse/world3d/render_scene.hpp>
#include <fuse/world3d/runtime_schedule.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/scene_snapshot.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::world3d {

class World3D;

/// UNI-U7-WORLD-1: what World3D::loadWorldFromFuselevel instantiated.
struct World3DLevelLoadResult {
    bool ok = false;
    std::string path;
    std::string note;
    /// `.fuselevel` header version (1..3) and whether it carried the v3 ECS block.
    u32 fileVersion = 0;
    bool hasEcsBlock = false;
    u32 sceneEntities = 0;  ///< scene-table entities (including wire stubs)
    u32 wireStubs = 0;      ///< `__fuse.wire|*` entities (not mirrored)
    u32 objects = 0;        ///< SceneObject3D mirrors published in the world
    u32 ecsEntities = 0;    ///< live entities in the world registry after the load
    u32 meshes = 0;
    u32 directionalLights = 0;
    u32 pointLights = 0;
    u32 spotLights = 0;
    u32 colliders = 0;
    u32 rigidBodies = 0;
    u32 spawnMarkers = 0;
    u32 physicsLinked = 0;  ///< mirrors whose pose follows a Collider + RigidBody entity
};

/// Physics view of a World3D: the PhysicsManager its runtime schedule steps plus the helpers the
/// pre-schedule `World3D::physics()` (PhysicsWorld3D) call sites use.
class World3DPhysics {
public:
    explicit World3DPhysics(World3D& world) : m_world(&world) {}

    /// Static plane dot(normal, p) = distance (an ECS body in the world's registry).
    ecs::EntityID addStaticPlane(const math::Vec3& normal, f32 distance);
    /// Bodies in the simulation as of the last step.
    u32 bodyCount() const;
    /// Touching contacts found by the last step.
    u32 contactCount() const;
    physics::PhysicsManager& manager() const;

private:
    World3D* m_world;
};

/// 3D dimension — composes T3D collision/render backends; no Box2D inheritance.
///
/// Game loop (GAP-GAME-LOOP-ECS): tickGameThread runs the RuntimeSchedule over the world's own ECS
/// registry (input -> scripts -> animation -> PhysicsManager -> transform -> camera -> audio -> VFX
/// -> render extraction, fixed-dt accumulator). Scene objects added with physics enabled get an ECS
/// body (Transform + RigidBody + Collider from the object's physics shape) that the PhysicsManager
/// steps; the object's world pose follows the body. Objects are published as Handle<Object> in the
/// world's SceneHandleTable (UNI-WP05-1); the snapshot and cull jobs only carry handles.
class World3D : public dimension::IDimension {
public:
    World3D();
    ~World3D() override;

    const char* dimensionName() const override { return "World3D"; }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }

    void tick(frame::FrameCtx& ctx) override;
    void render(frame::FrameCtx& ctx) override;

    /// Game-thread phase: physics step + immutable snapshot build (no worker reads yet).
    void tickGameThread(frame::FrameCtx& ctx);
    /// Worker-safe cull over the snapshot built by tickGameThread (heap-free JobScheduler fork-join).
    void runParallelCull();

    /// Records the handle; loads the pending `.fuselevel` (setFuselevelPath) when one is set.
    void loadWorld(dimension::WorldHandle world) override;
    dimension::WorldHandle activeWorld() const override { return m_activeWorld; }

    // --- UNI-U7-WORLD-1: `.fuselevel` levels (parity with World2D::loadWorldFromFuselevel) -------------------
    /// Optional `.fuselevel` consumed by the next loadWorld() call.
    void setFuselevelPath(std::string path) { m_pendingFuselevelPath = std::move(path); }
    const std::string& pendingFuselevelPath() const { return m_pendingFuselevelPath; }
    /// Replaces the world's level: the file's ECS block (v3; v1/v2 files get Transform-only entities)
    /// becomes the world's own registry (exact entity ids: ECS entity index i == scene entity i), so the
    /// runtime schedule simulates it and World3D::render draws it; every non-stub scene entity is
    /// mirrored as a SceneObject3D (name, local TRS from its Transform, hierarchy) published in the
    /// handle table. Physics comes from the level's Collider + RigidBody components (no per-object
    /// default bodies); mirrors of such entities follow their body. Material rows referenced by level
    /// meshes get default RenderMaterial3D rows when missing. Previous level objects, bodies and schedule
    /// hooks are dropped (the schedule restarts on the new registry). Without FUSE_BUILD_PROJECT this
    /// returns ok = false.
    World3DLevelLoadResult loadWorldFromFuselevel(const std::string& fuselevelPath);
    const World3DLevelLoadResult& lastLevelLoad() const { return m_lastLevelLoad; }
    /// Saves the world as `.fuselevel` v3 (World3D dimension): the loaded level's scene table (entity
    /// transforms refreshed from their ECS Transforms) or, without a level, an empty scene table, plus
    /// the world's whole registry. Load + save without changes reproduces the file byte for byte.
    bool saveWorld(const std::string& fuselevelPath, std::string* error = nullptr);
    /// World-space position of the first SpawnMarker entity (false when the level has none).
    bool findSpawnPoint(f32 outPosition[3]);

    SceneObject3D* root() { return m_root.get(); }
    const SceneObject3D* root() const { return m_root.get(); }

    /// Parents `object` under the root and publishes it (the returned handle is what snapshots carry).
    Handle<Object> addObject(SceneObject3D* object);
    /// Unpublishes `object` (its handle goes stale), detaches it and drops its physics body.
    void removeObject(SceneObject3D* object);
    /// Removes dynamic objects from the scene root (retains the world root node).
    void clearDynamicObjects();
    /// Game thread: object of a live handle (nullptr once removed or destroyed).
    SceneObject3D* resolve(Handle<Object> handle) const;
    bool isLive(Handle<Object> handle) const { return m_handles.valid(handle); }
    const SceneHandleTable& handles() const { return m_handles; }
    u32 objectCount() const { return static_cast<u32>(m_objects.size()); }
    /// Physics body entity of an object (null when it has none).
    ecs::EntityID bodyOf(Handle<Object> handle) const;
    const SceneSnapshot3D& readSnapshot() const { return m_snapshot; }
    const SceneTransformSoA3D& readTransformSoA() const { return m_transformSoA; }

    float clearColorR() const { return m_clearR; }
    float clearColorG() const { return m_clearG; }
    float clearColorB() const { return m_clearB; }
    void setClearColor(float r, float g, float b);

    // --- E03 GPU scene (render_scene.hpp) ----------------------------------------------------------------------
    /// The world's ECS scene (Transform + Mesh + lights + Camera) that World3D::render hands the GPU renderer.
    /// Owned by the world (initialised on first use, `kRegistryCapacity` entities) unless setRenderRegistry
    /// points it at another registry (the editor renders its edited scene this way).
    static constexpr usize kRegistryCapacity = 4096;
    ecs::Registry& registry();
    /// External registry to render instead of the owned one (null restores the owned registry).
    void setRenderRegistry(ecs::Registry* registry) { m_externalRegistry = registry; }
    bool usesExternalRegistry() const { return m_externalRegistry != nullptr; }

    void setCamera(const RenderCamera3D& camera) { m_camera = camera; }
    const RenderCamera3D& camera() const { return m_camera; }

    /// Material row `id` (ecs::Mesh::material_id). Bumps materialVersion().
    void setMaterial(u32 id, const RenderMaterial3D& material);
    const std::vector<RenderMaterial3D>& materials() const { return m_materials; }
    u64 materialVersion() const { return m_materialVersion; }

    /// Helpers that fill Transform (position / scale and local_to_world) + the component, in the render registry.
    /// `halfExtent` scales the unit builtin mesh per axis.
    ecs::EntityID spawnMesh(BuiltinMesh mesh, u32 materialId, const f32 center[3], const f32 halfExtent[3]);
    /// Sun shining toward -`toSun` (normalised here).
    ecs::EntityID spawnDirectionalLight(const f32 toSun[3], const f32 color[3], f32 intensity);
    ecs::EntityID spawnPointLight(const f32 position[3], const f32 color[3], f32 intensity, f32 radius);

    /// GPU renderer (not owned; null detaches). World3D::render calls it on the render thread.
    void setRenderer(IWorld3DRenderer* renderer) { m_renderer = renderer; }
    IWorld3DRenderer* renderer() const { return m_renderer; }
    /// Frames the attached renderer recorded / refused.
    u32 gpuFramesRendered() const { return m_gpuFrames; }
    u32 gpuFramesFailed() const { return m_gpuFailures; }

    // --- GAP-GAME-LOOP-ECS runtime schedule ------------------------------------------------------------
    /// Schedule settings used when the schedule starts (first tick or first schedule()/physics() call).
    void setRuntimeScheduleDesc(const RuntimeScheduleDesc& desc) { m_scheduleDesc = desc; }
    /// The world's frame schedule over its own registry (initialised on first use).
    RuntimeSchedule& schedule();
    /// Physics of the world (PhysicsManager stepped by the schedule, see World3DPhysics).
    World3DPhysics physics() { return World3DPhysics(*this); }
    physics::PhysicsManager& physicsManager() { return schedule().physics(); }
    /// Static collision plane dot(normal, p) = distance as an ECS body.
    ecs::EntityID addStaticPlane(const math::Vec3& normal, f32 distance);
    /// Physics bodies for scene objects (objects added while enabled, and existing ones on enable).
    void setPhysicsEnabled(bool enabled);
    bool isPhysicsEnabled() const { return m_physicsEnabled; }

private:
    struct ObjectEntry {
        Handle<Object> handle = Handle<Object>::invalid();
        ecs::EntityID body = ecs::EntityID::null();
        /// World pose last exchanged with the body (a difference is a game-side move).
        math::Vec3 syncedTranslation{};
        math::Quat syncedRotation{};
        u64 syncedVersion = 0;
        bool synced = false;
        /// Mirror of a loaded level entity: the node and its entity belong to the level (the entity is
        /// never destroyed or created by the per-object physics paths).
        bool levelOwned = false;
    };
    struct LevelState;
    struct LevelStateDeleter {
        void operator()(LevelState* state) const;
    };

    void buildSnapshot();
    ecs::Registry& ownRegistry_();
    void createBody_(ObjectEntry& entry, SceneObject3D& object);
    void destroyBody_(ObjectEntry& entry);
    void pruneDeadObjects_();
    void clearLevel_();
    void syncPhysicsFromScene();
    void syncSceneFromPhysics();
    static void renderExtractHook_(void* user, ecs::Registry& registry, f32 dt);

    bool m_enabled = true;
    dimension::WorldHandle m_activeWorld = dimension::WorldHandle::invalid();
    /// Declared before the nodes so it outlives them during destruction.
    SceneHandleTable m_handles;
    std::unique_ptr<SceneObject3D> m_root;
    std::vector<ObjectEntry> m_objects;

    SceneSnapshot3D m_snapshot;
    SceneTransformSoA3D m_transformSoA;
    /// One byte per entry (not vector<bool>): cull jobs write neighbouring entries concurrently.
    std::vector<u8> m_cullVisible;
    world2d::CullForkJoin m_cullJobs;

    float m_clearR = 0.1f;
    float m_clearG = 0.15f;
    float m_clearB = 0.25f;

    ecs::Registry m_registry;
    bool m_registryReady = false;
    ecs::Registry* m_externalRegistry = nullptr;
    RenderCamera3D m_camera{};
    std::vector<RenderMaterial3D> m_materials;
    u64 m_materialVersion = 0;
    IWorld3DRenderer* m_renderer = nullptr;
    u32 m_gpuFrames = 0;
    u32 m_gpuFailures = 0;

    std::string m_pendingFuselevelPath;
    World3DLevelLoadResult m_lastLevelLoad{};
    /// Loaded level (scene table + mirror nodes); defined in world_3d_level.cpp.
    std::unique_ptr<LevelState, LevelStateDeleter> m_level;

    RuntimeScheduleDesc m_scheduleDesc{};
    RuntimeSchedule m_schedule;
    bool m_physicsEnabled = false;
};

} // namespace fuse::world3d
