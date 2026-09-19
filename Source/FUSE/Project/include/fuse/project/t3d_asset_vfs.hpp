#pragma once

#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/project/importer_extract.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/project/t3d_datablock_resolve.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

struct ProjectVfsMountResult {
    u32 mountsAdded = 0;
    bool gameMount = false;
    bool t3dMount = false;
    bool t2dMount = false;
    std::string note;
};

struct T3DMaterialVfsResolveResult {
    u32 materialCount = 0;
    u32 resolvedCount = 0;
    u32 unresolvedCount = 0;
    std::string note;
};

struct T3DMaterialVfsAsyncLoadResult {
    std::vector<fuse::io::LoadId> loadIds;
    u32 submittedCount = 0;
    u32 drainedCount = 0;
    std::string note;
};

/// Mount project asset roots into the process VFS (`/game/`, `/t3d/`, `/t2d/`).
[[nodiscard]] ProjectVfsMountResult mountProjectAssetRoots(const ProjectManifest& manifest);

/// Map legacy `MaterialAsset` refs to virtual paths and resolve via mounted VFS.
[[nodiscard]] std::string materialAssetToVirtualPath(const std::string& materialRef);

/// Resolve material bindings from a mission extract through the VFS registry.
[[nodiscard]] T3DMaterialVfsResolveResult resolveT3DMaterialVfsPaths(const T3DMissionExtract& extract);

/// Resolve material wire bindings from a scene through the VFS registry.
[[nodiscard]] T3DMaterialVfsResolveResult resolveT3DMaterialVfsFromBindings(
    const T3DDatablockResolveResult& bindings);

/// Submit async VFS reads for material refs (I/O lane stub; game thread drains into HandleTable).
[[nodiscard]] T3DMaterialVfsAsyncLoadResult submitT3DMaterialLoadsAsync(const T3DMissionExtract& extract);

/// Drain completed material loads from the VFS I/O lane into the asset handle table.
u32 drainT3DMaterialLoads(fuse::HandleTable<fuse::io::Asset>& table);

} // namespace fuse::project
