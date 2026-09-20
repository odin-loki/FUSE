#include <fuse/project/t3d_asset_vfs.hpp>

#include <fuse/io/vfs.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_content_hash.hpp>

#include <cstring>
#include <filesystem>

namespace fuse::project {

namespace {

std::filesystem::path firstExistingPath(const std::filesystem::path& primary,
                                        const std::filesystem::path& fallback) {
    if (!primary.empty() && std::filesystem::exists(primary)) {
        return primary;
    }
    if (!fallback.empty() && std::filesystem::exists(fallback)) {
        return fallback;
    }
    return primary.empty() ? fallback : primary;
}

bool tryCookCacheHit(CookCache* cache, const std::string& physicalPath, u32& cookCacheHits) {
    if (cache == nullptr || physicalPath.empty()) {
        return false;
    }

    const u64 sourceHash = hash_file_content(physicalPath);
    const u64 cacheKey = combine_cook_cache_key(sourceHash, 0);
    if (!is_valid_cook_cache_key(cacheKey)) {
        return false;
    }

    CookCacheEntry cached;
    if (cache->lookup(cacheKey, &cached) != CookCacheLookup::Hit) {
        return false;
    }

    ++cookCacheHits;
    return true;
}

void storeMaterialCookCacheEntry(CookCache& cache, const std::string& physicalPath,
                                 const std::string& virtualPath) {
    if (physicalPath.empty() || virtualPath.empty()) {
        return;
    }

    const u64 sourceHash = hash_file_content(physicalPath);
    const u64 cacheKey = combine_cook_cache_key(sourceHash, 0);
    const std::string outputPath = materialVirtualPathToCookOutput(virtualPath);
    if (!is_valid_cook_cache_key(cacheKey) || outputPath.empty()) {
        return;
    }

    cache.invalidate_stale_content_for_source(physicalPath, cacheKey);

    CookCacheEntry entry;
    entry.content_hash = cacheKey;
    entry.upstream_hash = 0;
    entry.output_path = outputPath;
    entry.source_path = physicalPath;
    entry.kind = CookAssetKind::Texture;
    cache.store(entry);
}

void mountIfMissing(fuse::io::MountKind kind, const std::string& physicalPath,
                    const std::string& virtualPrefix, ProjectVfsMountResult& result) {
    if (physicalPath.empty()) {
        return;
    }

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    std::string resolved;
    if (vfs.resolve(virtualPrefix, resolved)) {
        return;
    }

    vfs.mount(kind, physicalPath, virtualPrefix);
    ++result.mountsAdded;
}

} // namespace

ProjectVfsMountResult mountProjectAssetRoots(const ProjectManifest& manifest) {
    ProjectVfsMountResult result;
    if (manifest.projectRoot.empty()) {
        result.note = "empty project root";
        return result;
    }

    const std::filesystem::path root = std::filesystem::path(manifest.projectRoot).lexically_normal();
    const std::filesystem::path gamePath =
        firstExistingPath(root / "Assets", root / "assets").lexically_normal();
    const std::filesystem::path t3dPath =
        firstExistingPath(root / "data", root / "Data").lexically_normal();
    const std::filesystem::path t2dPath = root;

    mountIfMissing(fuse::io::MountKind::Game, gamePath.string(), "/game/", result);
    result.gameMount = result.mountsAdded > 0;
    const u32 beforeT3d = result.mountsAdded;
    mountIfMissing(fuse::io::MountKind::T3DLegacy, t3dPath.string(), "/t3d/", result);
    result.t3dMount = result.mountsAdded > beforeT3d;
    const u32 beforeT2d = result.mountsAdded;
    mountIfMissing(fuse::io::MountKind::T2DLegacy, t2dPath.string(), "/t2d/", result);
    result.t2dMount = result.mountsAdded > beforeT2d;
    result.note = "mounted " + std::to_string(result.mountsAdded) + " project asset root(s)";
    fuse::log::info("mountProjectAssetRoots: %s under %s", result.note.c_str(), root.string().c_str());
    return result;
}

std::string materialAssetToVirtualPath(const std::string& materialRef) {
    if (materialRef.empty()) {
        return {};
    }

    const std::size_t colon = materialRef.find(':');
    if (colon == std::string::npos) {
        return "/t3d/materials/" + materialRef + ".mat";
    }

    const std::string folder = materialRef.substr(0, colon);
    const std::string name = materialRef.substr(colon + 1);
    return "/t3d/materials/" + folder + "/" + name + ".mat";
}

std::string materialVirtualPathToCookOutput(const std::string& virtualPath) {
    constexpr const char* prefix = "/t3d/materials/";
    if (virtualPath.size() < std::strlen(prefix) ||
        virtualPath.compare(0, std::strlen(prefix), prefix) != 0) {
        return {};
    }

    std::string relative = virtualPath.substr(std::strlen(prefix));
    if (relative.size() >= 4 && relative.compare(relative.size() - 4, 4, ".mat") == 0) {
        relative.resize(relative.size() - 4);
    }

    return "cooked/materials/" + relative + ".fusetex";
}

u64 materialCookCacheKey(const std::string& physicalPath) {
    return combine_cook_cache_key(hash_file_content(physicalPath), 0);
}

T3DMaterialVfsResolveResult resolveT3DMaterialVfsPaths(const T3DMissionExtract& extract) {
    T3DMaterialVfsResolveResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    for (const T3DMaterialRefStub& material : extract.materials) {
        ++result.materialCount;
        const std::string virtualPath = materialAssetToVirtualPath(material.assetPath);
        std::string physicalPath;
        if (!virtualPath.empty() && vfs.resolve(virtualPath, physicalPath)) {
            ++result.resolvedCount;
        } else {
            ++result.unresolvedCount;
        }
    }

    for (const T3DSimObjectStub& object : extract.simObjects) {
        if (object.materialAsset.empty()) {
            continue;
        }
        ++result.materialCount;
        const std::string virtualPath = materialAssetToVirtualPath(object.materialAsset);
        std::string physicalPath;
        if (!virtualPath.empty() && vfs.resolve(virtualPath, physicalPath)) {
            ++result.resolvedCount;
        } else {
            ++result.unresolvedCount;
        }
    }

    result.note = "resolved " + std::to_string(result.resolvedCount) + "/" +
                  std::to_string(result.materialCount) + " material vfs paths";
    return result;
}

T3DMaterialVfsResolveResult resolveT3DMaterialVfsFromBindings(
    const T3DDatablockResolveResult& bindings) {
    T3DMaterialVfsResolveResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    for (const T3DResolvedBinding& binding : bindings.bindings) {
        if (binding.kind != "material") {
            continue;
        }

        ++result.materialCount;
        const std::string virtualPath = materialAssetToVirtualPath(binding.refName);
        std::string physicalPath;
        if (!virtualPath.empty() && vfs.resolve(virtualPath, physicalPath)) {
            ++result.resolvedCount;
        } else {
            ++result.unresolvedCount;
        }
    }

    result.note = "resolved " + std::to_string(result.resolvedCount) + "/" +
                  std::to_string(result.materialCount) + " material wire vfs paths";
    return result;
}

T3DMaterialVfsAsyncLoadResult submitT3DMaterialLoadsAsync(const T3DDatablockResolveResult& bindings,
                                                         CookCache* cache) {
    T3DMaterialVfsAsyncLoadResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    auto submitMaterialRef = [&](const std::string& materialRef) {
        const std::string virtualPath = materialAssetToVirtualPath(materialRef);
        if (virtualPath.empty()) {
            return;
        }

        std::string physicalPath;
        if (cache != nullptr && vfs.resolve(virtualPath, physicalPath) &&
            tryCookCacheHit(cache, physicalPath, result.cookCacheHits)) {
            return;
        }

        const fuse::io::LoadId loadId = vfs.submitLoadAsync(virtualPath);
        if (loadId != 0u) {
            result.loadIds.push_back(loadId);
            ++result.submittedCount;
        }
    };

    for (const T3DResolvedBinding& binding : bindings.bindings) {
        if (binding.kind == "material") {
            submitMaterialRef(binding.refName);
        }
    }

    result.note = "submitted " + std::to_string(result.submittedCount) + " async material vfs load(s)";
    if (result.cookCacheHits > 0u) {
        result.note += ", cook-cache hits=" + std::to_string(result.cookCacheHits);
    }
    return result;
}

T3DMaterialVfsAsyncLoadResult submitT3DMaterialLoadsAsync(const T3DMissionExtract& extract,
                                                         CookCache* cache) {
    T3DMaterialVfsAsyncLoadResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    auto submitMaterialRef = [&](const std::string& materialRef) {
        const std::string virtualPath = materialAssetToVirtualPath(materialRef);
        if (virtualPath.empty()) {
            return;
        }

        std::string physicalPath;
        if (cache != nullptr && vfs.resolve(virtualPath, physicalPath) &&
            tryCookCacheHit(cache, physicalPath, result.cookCacheHits)) {
            return;
        }

        const fuse::io::LoadId loadId = vfs.submitLoadAsync(virtualPath);
        if (loadId != 0u) {
            result.loadIds.push_back(loadId);
            ++result.submittedCount;
        }
    };

    for (const T3DMaterialRefStub& material : extract.materials) {
        submitMaterialRef(material.assetPath);
    }

    for (const T3DSimObjectStub& object : extract.simObjects) {
        if (!object.materialAsset.empty()) {
            submitMaterialRef(object.materialAsset);
        }
    }

    result.note = "submitted " + std::to_string(result.submittedCount) + " async material vfs load(s)";
    if (result.cookCacheHits > 0u) {
        result.note += ", cook-cache hits=" + std::to_string(result.cookCacheHits);
    }
    return result;
}

T3DMaterialCookCacheResult drainT3DMaterialLoads(fuse::HandleTable<fuse::io::Asset>& table,
                                               CookCache* cache) {
    T3DMaterialCookCacheResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    result.drainedCount = vfs.drainCompletedLoads(table);

    if (cache == nullptr) {
        result.note = "drained " + std::to_string(result.drainedCount) + " material vfs load(s)";
        return result;
    }

    for (const fuse::io::CompletedLoad& load : vfs.lastDrainedLoads()) {
        if (!load.success || load.asset.virtualPath.empty()) {
            continue;
        }

        std::string physicalPath;
        if (!vfs.resolve(load.asset.virtualPath, physicalPath)) {
            continue;
        }

        const std::string outputPath = materialVirtualPathToCookOutput(load.asset.virtualPath);
        if (!outputPath.empty()) {
            AssetCooker cooker;
            TextureImportDesc desc;
            desc.input_path = physicalPath;
            desc.output_path = outputPath;
            desc.generate_mipmaps = true;
            const CookRecord cooked = cooker.cook_texture(desc);
            if (cooked.ok) {
                storeMaterialCookCacheEntry(*cache, physicalPath, load.asset.virtualPath);
                ++result.cookCacheStores;
            }
        } else {
            storeMaterialCookCacheEntry(*cache, physicalPath, load.asset.virtualPath);
            ++result.cookCacheStores;
        }
    }

    result.note = "drained " + std::to_string(result.drainedCount) + " material vfs load(s), stored " +
                  std::to_string(result.cookCacheStores) + " cook-cache entries";
    return result;
}

} // namespace fuse::project
