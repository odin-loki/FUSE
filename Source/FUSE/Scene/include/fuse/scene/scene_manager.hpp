#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/scene_build_system.hpp>
#include <fuse/scene/bvh_stub.hpp>
#include <fuse/scene/math.hpp>
#include <fuse/scene/svo.hpp>
#include <fuse/spatial/bvh.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::scene {

struct SceneManagerDesc {
    const char* name = "Untitled";
    f32 worldSize = 4096.f;
    u32 svoDepth = 10;
    bool hasVoxels = true;
};

/// ECS-backed runtime scene container — B3.6 (Registry + BVH + SVO).
class SceneManager {
public:
    void init(const SceneManagerDesc& desc);
    void destroy();

    /// Per-frame simulation-side update: TransformSystem, CameraSystem, then keep the spatial BVH
    /// in sync with every Mesh/SDFObject (rebuild when the set changes, refit otherwise).
    void update(f32 deltaSeconds);

    /// Cull against the active camera through the spatial BVH and gather draw items, SDF objects
    /// and lights (B3.6). Call after update(). Returns empty data without an active camera.
    fuse::ecs::SceneData buildFrame(fuse::ecs::CullResult* cullOut = nullptr);
    /// Steady-state form of buildFrame(): fills `out` in place (capacity kept) and reuses the
    /// manager's cull buffers, so a frame loop that keeps its SceneData performs no heap
    /// allocation once the buffers have grown.
    void buildFrame(fuse::ecs::SceneData& out, fuse::ecs::CullResult* cullOut = nullptr);

    fuse::ecs::EntityID createCamera(f32 fovDegrees = 75.f, bool active = false);
    [[nodiscard]] fuse::ecs::EntityID activeCamera() const { return m_activeCamera; }

    bool rayCast(vec3 origin, vec3 direction, f32 maxDistance, fuse::ecs::EntityID& hitEntity,
                 f32& hitDistance) const;
    void querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const;

    [[nodiscard]] fuse::ecs::Registry& registry() { return m_registry; }
    [[nodiscard]] const fuse::ecs::Registry& registry() const { return m_registry; }
    [[nodiscard]] BVH& bvh() { return m_bvh; }
    [[nodiscard]] const BVH& bvh() const { return m_bvh; }
    [[nodiscard]] const fuse::spatial::BVH& spatialBvh() const { return m_spatialBvh; }
    [[nodiscard]] u32 spatialBvhRebuildCount() const { return m_spatialBvhRebuilds; }
    [[nodiscard]] SVO& svo() { return m_svo; }
    [[nodiscard]] const SVO& svo() const { return m_svo; }
    [[nodiscard]] const SceneManagerDesc& desc() const { return m_desc; }
    [[nodiscard]] bool isInitialized() const { return m_initialized; }
    [[nodiscard]] u32 frameIndex() const { return m_frameIndex; }

private:
    void refitBvh();
    void syncSpatialBvh();

    struct BvhSource {
        fuse::ecs::EntityID entity{};
        fuse::spatial::BVHLeafType type = fuse::spatial::BVHLeafType::Mesh;
        bool operator==(const BvhSource& other) const { return entity == other.entity && type == other.type; }
    };

    SceneManagerDesc m_desc{};
    fuse::ecs::Registry m_registry{};
    BVH m_bvh{};
    SVO m_svo{};
    fuse::spatial::BVH m_spatialBvh{};
    std::vector<BvhSource> m_bvhSources;          ///< BVH build index -> entity
    std::vector<BvhSource> m_bvhSourcesScratch;
    std::vector<fuse::spatial::BVHLeaf> m_bvhLeaves;
    fuse::ecs::CullScratch m_cullScratch;
    fuse::ecs::CullResult m_visible;
    u32 m_spatialBvhRebuilds = 0;
    fuse::ecs::EntityID m_activeCamera{};
    u32 m_frameIndex = 0;
    bool m_initialized = false;
};

} // namespace fuse::scene
