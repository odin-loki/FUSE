#pragma once

#include <fuse/dimension/idimension.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/physics_world_2d.hpp>
#include <fuse/world2d/cull_fork_join.hpp>
#include <fuse/world2d/fuselevel_bridge.hpp>
#include <fuse/world2d/scene_handle_table.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/scene_snapshot.hpp>
#include <fuse/world2d/sprite_image_map.hpp>

#include <memory>
#include <vector>

namespace fuse::world2d {

/// Ortho view center for standalone play / hybrid 2D framing (parity with World3D::RenderCamera3D at spawn).
struct RenderView2D {
    f32 center[2] = {0.f, 0.f};
    bool valid = false;
};

/// 2D dimension — composes legacy T2D backends; does not inherit Box2D or GL types.
class World2D : public dimension::IDimension {
    friend FuselevelLoadResult populateWorld2DFromFuselevel(World2D&, const std::string&);

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

    /// The loaded level's v3 ECS block (SpawnMarker, Transform, …). Empty until a fuselevel load succeeds.
    static constexpr usize kRegistryCapacity = 4096;
    ecs::Registry& registry();
    /// World-space position of the first active SpawnMarker entity (false when the level has none).
    bool findSpawnPoint(f32 outPosition[3]);
    /// Center the 2D view on the level spawn (XY of the marker).
    bool applySpawnViewCamera();
    /// Standalone play: snapshot view/physics, frame spawn, enable physics.
    bool enterPlayMode();
    /// Restore the snapshot taken by `enterPlayMode()` (no-op when not playing).
    bool exitPlayMode();
    [[nodiscard]] bool isPlayModeActive() const { return m_playModeActive; }

    void setView(const RenderView2D& view) { m_view = view; }
    const RenderView2D& view() const { return m_view; }

    SceneObject2D* root() { return m_root.get(); }
    const SceneObject2D* root() const { return m_root.get(); }

    /// Parents `sprite` under the world root and publishes it in the world's handle table
    /// (UNI-WP05-1): the returned Handle<Object> is what snapshots and worker jobs carry.
    Handle<Object> addSprite(SceneObject2D* sprite);
    /// Unpublishes `sprite` (its handle goes stale) and detaches it from the root; ownership is
    /// released too when the world owned it (the node is destroyed).
    void removeSprite(SceneObject2D* sprite);
    /// removeSprite for a live handle; false when the handle is stale.
    bool destroySprite(Handle<Object> handle);
    void adoptOwnedSprite(std::unique_ptr<SceneObject2D> sprite);

    /// Game thread: node of a live handle (nullptr once removed or destroyed).
    SceneObject2D* resolve(Handle<Object> handle) const { return m_handles.resolve(handle); }
    bool isLive(Handle<Object> handle) const { return m_handles.valid(handle); }
    const SceneHandleTable& handles() const { return m_handles; }
    u32 spriteCount() const { return static_cast<u32>(m_sprites.size()); }
    const SceneSnapshot2D& readSnapshot() const { return m_snapshot; }
    const SceneTransformSoA2D& readTransformSoA() const { return m_transformSoA; }

    physics::PhysicsWorld2D& physics() { return m_physics; }
    const physics::PhysicsWorld2D& physics() const { return m_physics; }
    void setPhysicsEnabled(bool enabled);
    bool isPhysicsEnabled() const { return m_physicsEnabled; }

    void setSpriteImageMapSearchRoots(SpriteImageMapSearchRoots roots) {
        m_spriteMapRoots = std::move(roots);
        // Samples embed the search roots (project / repo paths). A new root set must re-read sheets.
        m_spriteMapSamples.clear();
    }

private:
    static constexpr u32 kNoPhysicsBody = 0xFFFFFFFFu;

    void attachPhysicsBodyForSprite_(SceneObject2D* sprite);
    void rebuildPhysicsBodies_();
    void buildSnapshot(frame::FrameCtx& ctx);
    void syncPhysicsFromScene();
    void syncSceneFromPhysics();
    void clearLoadedSprites_();
    ecs::Registry& ownRegistry_();
    void destroyLevelRegistry_();
    /// Takes ownership of a fuselevel ECS block (called from fuselevel_bridge after load).
    void adoptLevelRegistry_(ecs::Registry&& registry);
    /// Drops sprites whose node was destroyed while published (stale handles); rebuilds physics.
    void pruneDeadSprites_();
    /// Centre RGB of one atlas column. The strip is sampled once per map (filesystem paths, PNG
    /// decode, procedural hash); later ticks only index the stored bytes.
    bool cachedSpriteImageMapColor_(const std::string& imageMap, u32 frameIndex, u32 frameCount, u8& r, u8& g,
                                    u8& b);

    bool m_enabled = true;
    dimension::WorldHandle m_activeWorld = dimension::WorldHandle::invalid();
    std::string m_pendingFuselevelPath;
    std::string m_projectRoot;
    std::string m_defaultWorld2DRel;
    FuselevelLoadResult m_lastFuselevelLoad{};
    /// Declared before the nodes so it outlives them during destruction.
    SceneHandleTable m_handles;
    std::unique_ptr<SceneObject2D> m_root;
    /// Published sprites (resolved through m_handles every tick: a node destroyed elsewhere is
    /// dropped instead of being dereferenced).
    std::vector<Handle<Object>> m_sprites;
    std::vector<std::unique_ptr<SceneObject2D>> m_ownedSprites;

    SceneSnapshot2D m_snapshot;
    SceneTransformSoA2D m_transformSoA;
    /// One byte per entry (not vector<bool>): cull jobs write neighbouring entries concurrently.
    std::vector<u8> m_cullVisible;
    CullForkJoin m_cullJobs;

    physics::PhysicsWorld2D m_physics;
    std::vector<u32> m_physicsBodyIndices;
    bool m_physicsEnabled = false;
    SpriteImageMapSearchRoots m_spriteMapRoots;
    struct SpriteMapSample {
        std::string imageMap;
        u32 frameCount = 0;
        /// Tight RGB triples, column `frame` at `frame * 3`.
        std::vector<u8> rgb;
    };
    std::vector<SpriteMapSample> m_spriteMapSamples;

    ecs::Registry m_registry;
    bool m_registryReady = false;

    struct PlayModeSnapshot {
        RenderView2D view{};
        bool viewValid = false;
        bool physicsEnabled = false;
    };
    bool m_playModeActive = false;
    PlayModeSnapshot m_playSnapshot{};
    RenderView2D m_view{};
};

} // namespace fuse::world2d
