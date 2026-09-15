#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/scene/bvh_stub.hpp>
#include <fuse/scene/math.hpp>
#include <fuse/scene/svo.hpp>
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

    void update(f32 deltaSeconds);

    fuse::ecs::EntityID createCamera(f32 fovDegrees = 75.f, bool active = false);
    [[nodiscard]] fuse::ecs::EntityID activeCamera() const { return m_activeCamera; }

    bool rayCast(vec3 origin, vec3 direction, f32 maxDistance, fuse::ecs::EntityID& hitEntity,
                 f32& hitDistance) const;
    void querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const;

    [[nodiscard]] fuse::ecs::Registry& registry() { return m_registry; }
    [[nodiscard]] const fuse::ecs::Registry& registry() const { return m_registry; }
    [[nodiscard]] BVH& bvh() { return m_bvh; }
    [[nodiscard]] const BVH& bvh() const { return m_bvh; }
    [[nodiscard]] SVO& svo() { return m_svo; }
    [[nodiscard]] const SVO& svo() const { return m_svo; }
    [[nodiscard]] const SceneManagerDesc& desc() const { return m_desc; }
    [[nodiscard]] bool isInitialized() const { return m_initialized; }
    [[nodiscard]] u32 frameIndex() const { return m_frameIndex; }

private:
    void refitBvh();

    SceneManagerDesc m_desc{};
    fuse::ecs::Registry m_registry{};
    BVH m_bvh{};
    SVO m_svo{};
    fuse::ecs::EntityID m_activeCamera{};
    u32 m_frameIndex = 0;
    bool m_initialized = false;
};

} // namespace fuse::scene
