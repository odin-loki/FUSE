#pragma once

// Shared collision shape pool (GAP-PHYS-HULL-MESH): convex hulls, triangle meshes, signed-distance
// samplers and voxel volumes, referenced by index from CollisionShapeSoA::shapeRefs (params keep the
// shape's origin-centred bounding half extents so bounds, inertia and the primitive paths stay valid).
//
// One process-wide pool (`ShapePool::global()`): shapes are registered at load / cook / spawn time and
// read concurrently by the narrowphase and queries. Storage is chunked, so an entry never moves once
// registered. Registration and release take a mutex; lookups are lock-free. Releasing a shape that a
// running simulation still references is the caller's error (release after the bodies are gone).
//
// Cooked `.fusecol` assets bind (asset id, piece index) -> reference, so an ECS Collider can name its
// shape by asset id (fuse/physics/assets/collision_asset.hpp loads and binds them).

#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/shapes/convex_hull.hpp>
#include <fuse/physics/shapes/sdf_sampler.hpp>
#include <fuse/physics/shapes/tri_mesh.hpp>
#include <fuse/types.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <tuple>
#include <vector>

namespace fuse::physics {

class VoxelVolume;

class ShapePool {
public:
    static constexpr u32 kChunkBits = 8u;
    static constexpr u32 kChunkSize = 1u << kChunkBits;
    static constexpr u32 kMaxChunks = 4096u; ///< 1M shapes

    static ShapePool& global();

    ShapePool();
    ~ShapePool();
    ShapePool(const ShapePool&) = delete;
    ShapePool& operator=(const ShapePool&) = delete;

    /// Registers a shape; returns its reference (kNoShapeRef when the pool is full or the shape is
    /// empty).
    u32 addHull(ConvexHull hull);
    u32 addMesh(TriMesh mesh);
    u32 addSdf(std::shared_ptr<const SdfSampler> sdf);
    /// Voxel volumes stay owned by the caller (e.g. a destructible in the PhysicsManager): the pool
    /// keeps a non-owning view that the caller must release before the volume goes away. Carving
    /// the volume in place refreshes the collision shape.
    u32 addVoxelView(const VoxelVolume* volume);
    /// Frees the slot (and the owned shape); stale references then resolve to nothing.
    void release(u32 ref);

    [[nodiscard]] CollisionShapeType type(u32 ref) const;
    [[nodiscard]] bool valid(u32 ref) const;
    [[nodiscard]] const ConvexHull* hull(u32 ref) const;
    [[nodiscard]] const TriMesh* mesh(u32 ref) const;
    [[nodiscard]] const SdfSampler* sdf(u32 ref) const;
    [[nodiscard]] const VoxelVolume* voxel(u32 ref) const;

    /// Origin-centred bounding half extents in the body frame (what CollisionShapeSoA::params holds).
    [[nodiscard]] vec3 halfExtents(u32 ref) const;
    /// Body-frame bounds.
    bool localBounds(u32 ref, vec3& lo, vec3& hi) const;

    /// Asset binding: (asset id, piece) -> reference.
    void bindAsset(u64 assetId, u32 piece, CollisionShapeType type, u32 ref);
    [[nodiscard]] u32 findAsset(u64 assetId, u32 piece, CollisionShapeType type) const;
    void unbindAsset(u64 assetId);

    [[nodiscard]] u32 liveCount() const { return m_live.load(std::memory_order_relaxed); }

private:
    struct Entry {
        CollisionShapeType type = CollisionShapeType::Sphere;
        bool used = false;
        std::unique_ptr<ConvexHull> hull;
        std::unique_ptr<TriMesh> mesh;
        std::shared_ptr<const SdfSampler> sdf;
        const VoxelVolume* voxel = nullptr;
    };
    using Chunk = std::array<Entry, kChunkSize>;

    const Entry* entry(u32 ref) const;
    u32 insert(Entry entry);

    std::array<std::atomic<Chunk*>, kMaxChunks> m_chunks{};
    std::vector<std::unique_ptr<Chunk>> m_owned;
    std::vector<u32> m_free;
    u32 m_next = 0;
    std::atomic<u32> m_live{0};
    mutable std::mutex m_mutex;
    std::map<std::tuple<u64, u32, u32>, u32> m_assets; ///< (asset id, piece, type) -> reference
};

/// Body-frame bounds of a pooled shape at a pose, as a world AABB.
aabb pooledShapeWorldBounds(u32 ref, vec3 position, const quat& orientation);

} // namespace fuse::physics
