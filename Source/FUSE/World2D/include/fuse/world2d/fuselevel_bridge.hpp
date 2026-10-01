#pragma once

#if defined(FUSE_WORLD2D_HAS_FUSELEVEL) && FUSE_WORLD2D_HAS_FUSELEVEL
#include <fuse/scene/wire_runtime_bind.hpp>
#endif
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::world2d {

class World2D;

struct WireStubRuntimeEntry {
    std::string kind;
    std::string owner;
    std::string value;
};

struct FuselevelLoadResult {
    bool ok = false;
    /// UNI-U7-WORLD-1: `.fuselevel` header version and whether its v3 ECS block was used.
    u32 fileVersion = 0;
    bool hasEcsBlock = false;
    /// Sprites whose physics settings came from Collider + RigidBody components (v3).
    u32 physicsBodies = 0;
    u32 entityCount = 0;
    u32 wireStubCount = 0;
    u32 wireStubResolved = 0;
#if defined(FUSE_WORLD2D_HAS_FUSELEVEL) && FUSE_WORLD2D_HAS_FUSELEVEL
    fuse::scene::WireRuntimeBindResult wireBindings{};
    fuse::scene::LegacyDatablockTable legacyTable{};
#endif
    std::string note;
    std::vector<WireStubRuntimeEntry> wireStubs;
};

/// Loads a converted `.fuselevel` into a World2D scene graph. Skips `__fuse.wire|*` entities
/// for sprite creation but records parsed wiring metadata for runtime follow-up.
/// Without FUSE_WORLD2D_HAS_FUSELEVEL (FUSE_BUILD_PROJECT=OFF) this returns ok=false with a note.
FuselevelLoadResult populateWorld2DFromFuselevel(World2D& world, const std::string& fuselevelPath);

struct FuselevelSaveResult {
    bool ok = false;
    u32 entityCount = 0;   ///< scene entities written (sprites + wire stubs)
    u32 physicsBodies = 0; ///< sprites written with Collider + RigidBody
    std::string note;
};

/// UNI-U7-WORLD-1: saves the World2D scene graph as `.fuselevel` v3 (SceneDimension::World2D): one scene
/// entity per sprite under the world root (pre-order, local TRS, parent index) and the same entity index
/// in the ECS block with a Transform (+ Collider / RigidBody for physics sprites: Box -> Box half extents,
/// Circle / None -> Sphere; Collider::shape_piece keeps the PhysicsShape2D and Collider::scalar the
/// radius), plus the wire stubs of the last load under their owners. Loading the file with
/// populateWorld2DFromFuselevel and saving again reproduces it byte for byte.
/// Without FUSE_WORLD2D_HAS_FUSELEVEL this returns ok = false.
#if defined(FUSE_WORLD2D_HAS_FUSELEVEL) && FUSE_WORLD2D_HAS_FUSELEVEL
FuselevelSaveResult saveWorld2DToFuselevel(const World2D& world, const std::string& fuselevelPath);
#else
inline FuselevelSaveResult saveWorld2DToFuselevel(const World2D&, const std::string&) {
    FuselevelSaveResult result;
    result.note = "fuselevel saving unavailable (built without FUSE_BUILD_PROJECT)";
    return result;
}
#endif

} // namespace fuse::world2d
