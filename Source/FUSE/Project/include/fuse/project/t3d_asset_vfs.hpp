#pragma once

#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/project/cook_cache.hpp>
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
    u32 cookCacheHits = 0;
    std::string note;
};

struct T3DMaterialCookCacheResult {
    u32 drainedCount = 0;
    u32 cookCacheStores = 0;
    std::string note;
};

struct T3DShaderVfsResolveResult {
    u32 shaderCount = 0;
    u32 resolvedCount = 0;
    u32 unresolvedCount = 0;
    std::string note;
};

struct T3DShaderVfsAsyncLoadResult {
    std::vector<fuse::io::LoadId> loadIds;
    u32 submittedCount = 0;
    u32 drainedCount = 0;
    u32 cookCacheHits = 0;
    std::string note;
};

struct T3DShaderCookCacheResult {
    u32 drainedCount = 0;
    u32 cookCacheStores = 0;
    std::string note;
};

/// Mount project asset roots into the process VFS (`/game/`, `/t3d/`, `/t2d/`).
[[nodiscard]] ProjectVfsMountResult mountProjectAssetRoots(const ProjectManifest& manifest);

/// Map legacy `MaterialAsset` refs to virtual paths and resolve via mounted VFS.
[[nodiscard]] std::string materialAssetToVirtualPath(const std::string& materialRef);

/// Map mounted `/t3d/materials/.../*.mat` virtual paths to cooked `.fusetex` outputs.
[[nodiscard]] std::string materialVirtualPathToCookOutput(const std::string& virtualPath);

/// Content-hash key for a resolved material source on disk (0 when unreadable).
[[nodiscard]] u64 materialCookCacheKey(const std::string& physicalPath);

/// Resolve material bindings from a mission extract through the VFS registry.
[[nodiscard]] T3DMaterialVfsResolveResult resolveT3DMaterialVfsPaths(const T3DMissionExtract& extract);

/// Resolve material wire bindings from a scene through the VFS registry.
[[nodiscard]] T3DMaterialVfsResolveResult resolveT3DMaterialVfsFromBindings(
    const T3DDatablockResolveResult& bindings);

/// Submit async VFS reads for material refs (I/O lane stub; game thread drains into HandleTable).
/// When `cache` is non-null, cache hits skip I/O lane submission.
[[nodiscard]] T3DMaterialVfsAsyncLoadResult submitT3DMaterialLoadsAsync(const T3DMissionExtract& extract,
                                                                       CookCache* cache = nullptr);

/// Submit async VFS reads for resolved scene material bindings.
[[nodiscard]] T3DMaterialVfsAsyncLoadResult submitT3DMaterialLoadsAsync(
    const T3DDatablockResolveResult& bindings, CookCache* cache = nullptr);

/// Drain completed material loads from the VFS I/O lane into the asset handle table.
/// When `cache` is non-null, successful drains are stored as texture cook-cache entries.
[[nodiscard]] T3DMaterialCookCacheResult drainT3DMaterialLoads(fuse::HandleTable<fuse::io::Asset>& table,
                                                               CookCache* cache = nullptr);

/// Remap legacy relative asset paths (`data/`, `game/`, `assets/`) to mounted VFS prefixes.
[[nodiscard]] std::string remapLegacyAssetPath(const std::string& legacyPath);

/// Map legacy `ShaderData = "Folder:Name"` refs to `/t3d/shaders/Folder/Name.cs`.
[[nodiscard]] std::string shaderAssetToVirtualPath(const std::string& shaderRef);

/// Map mounted `/t3d/shaders/.../*.cs` virtual paths to cooked `.fuseshader` outputs.
[[nodiscard]] std::string shaderVirtualPathToCookOutput(const std::string& virtualPath);

/// Count virtual-path mappings derived from resolved scene bindings (material + shader stubs).
[[nodiscard]] u32 countRemappedAssetVfsPaths(const T3DDatablockResolveResult& bindings);

/// Resolve shader wire bindings from a scene through the VFS registry.
[[nodiscard]] T3DShaderVfsResolveResult resolveT3DShaderVfsFromBindings(
    const T3DDatablockResolveResult& bindings);

/// Content-hash key for a resolved shader source on disk (0 when unreadable).
[[nodiscard]] u64 shaderCookCacheKey(const std::string& physicalPath);

/// Submit async VFS reads for shader refs (I/O lane stub; game thread drains into HandleTable).
/// When `cache` is non-null, cache hits skip I/O lane submission.
[[nodiscard]] T3DShaderVfsAsyncLoadResult submitT3DShaderLoadsAsync(const T3DMissionExtract& extract,
                                                                    CookCache* cache = nullptr);

[[nodiscard]] T3DShaderVfsAsyncLoadResult submitT3DShaderLoadsAsync(
    const T3DDatablockResolveResult& bindings, CookCache* cache = nullptr);

/// Drain completed shader loads from the VFS I/O lane into the asset handle table.
/// When `cache` is non-null, successful drains trigger `AssetCooker::cook_shader` on cache miss.
[[nodiscard]] T3DShaderCookCacheResult drainT3DShaderLoads(fuse::HandleTable<fuse::io::Asset>& table,
                                                           CookCache* cache = nullptr);

} // namespace fuse::project
