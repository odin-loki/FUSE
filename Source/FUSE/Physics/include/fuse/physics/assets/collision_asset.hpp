#pragma once

// Cooked collision asset `.fusecol` (GAP-PHYS-HULL-MESH part 2): the convex hulls and static triangle
// meshes of one model, produced by the collision cook (Tools/FUSE/Cook collision_cook, `fuse_cook
// --collision`) from an FMSH and loaded here into the shared shape pool.
//
// Layout (little-endian, 4-byte aligned):
//   u32 magic 'FCOL' (0x4C4F4346), u32 version (1), u32 hull count, u32 mesh count, u32 flags (0)
//   per hull:  u32 vertex count, u32 face count, u32 face-index count,
//              vertices (3 x f32), faces (u32 first index, u32 index count), face indices (u32)
//              (planes, edges, bounds and mass properties are rebuilt by finalizeConvexHull on load)
//   per mesh:  u32 vertex count, u32 triangle count, u32 BVH node count,
//              vertices (3 x f32), BVH-ordered triangle indices (3 x u32),
//              nodes (3 x f32 min, u32 first, 3 x f32 max, u32 count), edge flags (u8 per triangle, padded to 4)
//   u32 FNV-1a hash of every preceding byte
// The bytes depend only on the geometry (deterministic cooks).

#include <fuse/physics/shapes/convex_hull.hpp>
#include <fuse/physics/shapes/tri_mesh.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::physics {

constexpr u32 kCollisionAssetMagic = 0x4C4F4346u; // "FCOL"
constexpr u32 kCollisionAssetVersion = 1u;

struct CollisionAsset {
    std::vector<ConvexHull> hulls;
    std::vector<TriMesh> meshes;
};

std::vector<u8> serializeCollisionAsset(const CollisionAsset& asset);
/// Validates and decodes (hash, bounds, topology, BVH). False with `error` on any inconsistency.
bool deserializeCollisionAsset(const u8* data, usize size, CollisionAsset& out, std::string* error = nullptr);
bool readCollisionAssetFile(const std::string& path, CollisionAsset& out, std::string* error = nullptr);
bool writeCollisionAssetFile(const std::string& path, const CollisionAsset& asset, std::string* error = nullptr);

/// Shape-pool references of a registered asset (piece i of each kind).
struct CollisionAssetRefs {
    u64 assetId = 0;
    std::vector<u32> hullRefs;
    std::vector<u32> meshRefs;
};

/// Moves the asset's shapes into the shape pool and binds (assetId, piece) so an ECS Collider with
/// shape_asset = assetId finds them. Re-registering an id replaces the binding.
CollisionAssetRefs registerCollisionAsset(CollisionAsset asset, u64 assetId);
/// Reads a `.fusecol` file and registers it.
bool loadCollisionAssetFile(const std::string& path, u64 assetId, CollisionAssetRefs* refs = nullptr,
                            std::string* error = nullptr);
/// Releases the asset's shapes (the bodies using them must be gone).
void unregisterCollisionAsset(const CollisionAssetRefs& refs);

} // namespace fuse::physics
