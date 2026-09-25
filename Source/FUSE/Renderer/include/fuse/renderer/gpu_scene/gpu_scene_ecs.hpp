#pragma once

// WP-1.1 GPU scene: ECS extraction adapter (read-only on the ECS).
//
// extract(registry, scene) mirrors the registry into the GPU scene once per frame:
//   * entities with Transform + Mesh become instances (TagStatic adds kInstanceStatic);
//   * entities with Transform + PointLight / SpotLight / DirectionalLight become lights;
//   * entities that lost their components, died, or whose index was reused with a new generation
//     are removed; new ones are added with prev = cur (no motion on the first frame).
// Liveness is a serial pass per archetype chunk (it allocates GPU scene slots); packing and diffing
// run as the `gpu_scene_extract` single-source kernel over the chunk (gpu_scene_extract_kernel.hpp),
// on CpuParallel by default. Only rows whose packed bytes changed are marked dirty, so the upload
// volume of the following GpuScene::commit() tracks what the game actually changed.
//
// The registry is taken by non-const reference only because Registry has no const iteration API;
// the extractor reads components and never writes, adds, removes or creates anything.
//
// Steady state (no entity index beyond reserve(), no table growth) makes no heap allocations.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>

#include <span>
#include <vector>

namespace fuse::ecs {
class Registry;
struct Transform;
struct Mesh;
} // namespace fuse::ecs

namespace fuse::renderer::gpu_scene {

struct EcsExtractDesc {
    kernel::Backend backend = kernel::Backend::CpuParallel;
    bool extractLights = true;
    /// Optional map Mesh::vertex_buffer index -> GpuScene mesh row (the asset bridge owns it).
    /// Empty: the vertex-buffer handle index is the mesh row.
    const u32* meshRemap = nullptr;
    u32 meshRemapCount = 0;
    /// Entity indices to pre-size the maps for (avoids growth allocations while populating).
    u32 entityCapacity = 1024;
};

struct EcsExtractStats {
    u32 entities = 0;        ///< instance entities visited
    u32 added = 0;
    u32 removed = 0;
    u32 instanceWrites = 0;  ///< instance rows whose bytes changed (kernel)
    u32 transformWrites = 0; ///< transform rows whose bytes changed (kernel)
    u32 kernelLaunches = 0;
    u32 lights = 0;
    u32 lightsAdded = 0;
    u32 lightsRemoved = 0;
    u32 lightWrites = 0;
};

class GpuSceneEcsExtractor {
public:
    void init(const EcsExtractDesc& desc = {});
    EcsExtractStats extract(ecs::Registry& registry, GpuScene& scene);
    /// Removes every instance and light this extractor created.
    void releaseAll(GpuScene& scene);

    InstanceHandle instanceOf(ecs::EntityID id) const { return m_instances.handleOf(id); }
    LightHandle lightOf(ecs::EntityID id) const { return m_lights.handleOf(id); }
    const EcsExtractDesc& desc() const { return m_desc; }

private:
    /// Entity index -> GPU scene slot, with liveness stamps and a dense list for removal.
    class EntityMap {
    public:
        void reserve(u32 entities);
        /// Returns the tracked entry for `id` (resizing for new indices).
        SlotAllocator::Handle handleOf(ecs::EntityID id) const;
        struct Entry {
            u32 generation = 0; ///< entity generation (0 = untracked)
            SlotAllocator::Handle handle{};
            u32 lastSeen = 0;
            u32 listPos = 0;
        };
        Entry& entry(u32 index);
        void track(ecs::EntityID id, SlotAllocator::Handle handle, u32 frame);
        void untrack(u32 index);
        const std::vector<u32>& list() const { return m_list; }
        const std::vector<u32>& slots() const { return m_slot; }

    private:
        std::vector<Entry> m_entries;
        std::vector<u32> m_slot; ///< entity index -> slot (kernel view), kInvalidIndex when untracked
        std::vector<u32> m_list; ///< tracked entity indices
    };

    void extractChunk(GpuScene& scene, std::span<const ecs::EntityID> ids, std::span<const ecs::Transform> transforms,
                      std::span<const ecs::Mesh> meshes, u32 extraFlags, EcsExtractStats& stats);
    template <typename LightT>
    void extractLights(ecs::Registry& registry, GpuScene& scene, EcsExtractStats& stats);

    EcsExtractDesc m_desc{};
    EntityMap m_instances;
    EntityMap m_lights;
    u32 m_frame = 0;
};

} // namespace fuse::renderer::gpu_scene
