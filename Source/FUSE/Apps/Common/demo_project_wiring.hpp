#pragma once

#include <fuse/project/loader.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <string>

namespace fuse::demo::wiring {

struct ProjectRuntimeContext {
    project::ProjectVfsMountResult vfs{};
    bool ok = false;
};

struct World3DLoadResult {
    bool ok = false;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    u32 datablockBindings = 0;
    u32 materialBindings = 0;
    u32 materialVfsResolved = 0;
    std::string loadedPath;
    std::string note;
};

struct World2DBridgeResult {
    bool ok = false;
    u32 spriteCount = 0;
    u32 physicsBodyCount = 0;
    bool physicsEnabled = false;
    std::string note;
};

/// Mount `/game/`, `/t3d/`, `/t2d/` and apply `project.json` worker cap.
[[nodiscard]] ProjectRuntimeContext prepareProjectRuntime(const project::LoadResult& projectLoad);

/// Convert bundled `.mis` when `.fuselevel` is absent, then load the default 3D world.
[[nodiscard]] World3DLoadResult ensure3DWorldFromProject(const project::LoadResult& projectLoad,
                                                           scene::Scene& scene);

/// Convert bundled `.cs` when `.fuselevel` is absent, then bridge into `World2D`.
[[nodiscard]] World2DBridgeResult bridge2DWorldFromProject(const project::LoadResult& projectLoad,
                                                           world2d::World2D& world);

} // namespace fuse::demo::wiring
