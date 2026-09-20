#pragma once

#include <fuse/project/importer_extract.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::world2d {
class World2D;
}

namespace fuse::scene {
class Scene;
}

namespace fuse::project {

struct T2DRuntimeBridgeResult {
    bool ok = false;
    u32 nodeCount = 0;
    u32 spriteCount = 0;
    u32 animatedSpriteCount = 0;
    u32 physicsBodyCount = 0;
    u32 circleBodyCount = 0;
    u32 boxBodyCount = 0;
    u32 collisionLayerCount = 0;
    std::string note;
};

/// Bridge T2D toybox/module extract into runtime `World2D` sprites (game thread).
T2DRuntimeBridgeResult populateWorld2DFromModuleExtract(fuse::world2d::World2D& world,
                                                        const T2DModuleExtract& extract);

/// Bridge T2D module extract into a `.fuselevel`-compatible scene hierarchy.
u32 populateSceneFromModuleExtract(fuse::scene::Scene& scene, const T2DModuleExtract& extract);

/// Load module text from disk and populate `World2D` (full runtime module bridge).
T2DRuntimeBridgeResult bridgeT2DModuleToRuntime(fuse::world2d::World2D& world,
                                              const std::string& modulePath);

} // namespace fuse::project
