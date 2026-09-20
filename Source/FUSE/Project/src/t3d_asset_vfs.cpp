#include <fuse/project/t3d_asset_vfs.hpp>

#include <fuse/io/vfs.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/import_desc.hpp>

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

void storeShaderCookCacheEntry(CookCache& cache, const std::string& physicalPath,
                               const std::string& virtualPath) {
    if (physicalPath.empty() || virtualPath.empty()) {
        return;
    }

    const u64 sourceHash = hash_file_content(physicalPath);
    const u64 cacheKey = combine_cook_cache_key(sourceHash, 0);
    const std::string outputPath = shaderVirtualPathToCookOutput(virtualPath);
    if (!is_valid_cook_cache_key(cacheKey) || outputPath.empty()) {
        return;
    }

    cache.invalidate_stale_content_for_source(physicalPath, cacheKey);

    CookCacheEntry entry;
    entry.content_hash = cacheKey;
    entry.upstream_hash = 0;
    entry.output_path = outputPath;
    entry.source_path = physicalPath;
    entry.kind = CookAssetKind::Shader;
    cache.store(entry);
}

ShaderImportDesc::Stage inferShaderStageFromPath(const std::string& physicalPath) {
    const std::filesystem::path input = std::filesystem::path(physicalPath);
    const std::string ext = input.extension().string();
    if (ext == ".cs" || ext == ".comp") {
        return ShaderImportDesc::Stage::Compute;
    }
    if (ext == ".vert") {
        return ShaderImportDesc::Stage::Vertex;
    }
    return ShaderImportDesc::Stage::Fragment;
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

std::string remapLegacyAssetPath(const std::string& legacyPath) {
    if (legacyPath.empty()) {
        return {};
    }
    if (legacyPath.front() == '/') {
        return legacyPath;
    }

    auto startsWith = [](const std::string& path, const char* prefix) {
        const std::size_t prefixLen = std::strlen(prefix);
        return path.size() >= prefixLen && path.compare(0, prefixLen, prefix) == 0;
    };

    if (startsWith(legacyPath, "data/") || startsWith(legacyPath, "Data/")) {
        return "/t3d/" + legacyPath.substr(legacyPath.find('/') + 1);
    }
    if (startsWith(legacyPath, "game/") || startsWith(legacyPath, "Game/") ||
        startsWith(legacyPath, "assets/") || startsWith(legacyPath, "Assets/")) {
        return "/game/" + legacyPath.substr(legacyPath.find('/') + 1);
    }
    if (startsWith(legacyPath, "levels/") || startsWith(legacyPath, "art/") ||
        startsWith(legacyPath, "shaders/")) {
        return "/t3d/" + legacyPath;
    }

    return "/t3d/" + legacyPath;
}

std::string shaderAssetToVirtualPath(const std::string& shaderRef) {
    if (shaderRef.empty()) {
        return {};
    }

    if (shaderRef.front() == '/') {
        return shaderRef;
    }

    const std::size_t colon = shaderRef.find(':');
    if (colon == std::string::npos) {
        if (shaderRef.size() >= 3 && shaderRef.compare(shaderRef.size() - 3, 3, ".cs") == 0) {
            return remapLegacyAssetPath(shaderRef);
        }
        return "/t3d/shaders/" + shaderRef + ".cs";
    }

    const std::string folder = shaderRef.substr(0, colon);
    const std::string name = shaderRef.substr(colon + 1);
    return "/t3d/shaders/" + folder + "/" + name + ".cs";
}

std::string shaderVirtualPathToCookOutput(const std::string& virtualPath) {
    constexpr const char* prefix = "/t3d/shaders/";
    if (virtualPath.size() < std::strlen(prefix) ||
        virtualPath.compare(0, std::strlen(prefix), prefix) != 0) {
        return {};
    }

    std::string relative = virtualPath.substr(std::strlen(prefix));
    if (relative.size() >= 3 && relative.compare(relative.size() - 3, 3, ".cs") == 0) {
        relative.resize(relative.size() - 3);
    }

    return "cooked/shaders/" + relative + ".fuseshader";
}

u32 countRemappedAssetVfsPaths(const T3DDatablockResolveResult& bindings) {
    u32 count = 0;
    for (const T3DResolvedBinding& binding : bindings.bindings) {
        if (binding.kind == "material") {
            if (!materialAssetToVirtualPath(binding.refName).empty()) {
                ++count;
            }
            continue;
        }
        if (binding.kind == "shader") {
            if (!shaderAssetToVirtualPath(binding.refName).empty()) {
                ++count;
            }
        }
    }
    return count;
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

T3DShaderVfsResolveResult resolveT3DShaderVfsFromBindings(const T3DDatablockResolveResult& bindings) {
    T3DShaderVfsResolveResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    for (const T3DResolvedBinding& binding : bindings.bindings) {
        if (binding.kind != "shader") {
            continue;
        }

        ++result.shaderCount;
        const std::string virtualPath = shaderAssetToVirtualPath(binding.refName);
        std::string physicalPath;
        if (!virtualPath.empty() && vfs.resolve(virtualPath, physicalPath)) {
            ++result.resolvedCount;
        } else {
            ++result.unresolvedCount;
        }
    }

    result.note = "resolved " + std::to_string(result.resolvedCount) + "/" +
                  std::to_string(result.shaderCount) + " shader wire vfs paths";
    return result;
}

u64 shaderCookCacheKey(const std::string& physicalPath) {
    return combine_cook_cache_key(hash_file_content(physicalPath), 0);
}

T3DShaderVfsAsyncLoadResult submitT3DShaderLoadsAsync(const T3DDatablockResolveResult& bindings,
                                                      CookCache* cache) {
    T3DShaderVfsAsyncLoadResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    auto submitShaderRef = [&](const std::string& shaderRef) {
        const std::string virtualPath = shaderAssetToVirtualPath(shaderRef);
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
        if (binding.kind == "shader") {
            submitShaderRef(binding.refName);
        }
    }

    result.note = "submitted " + std::to_string(result.submittedCount) + " async shader vfs load(s)";
    if (result.cookCacheHits > 0u) {
        result.note += ", cook-cache hits=" + std::to_string(result.cookCacheHits);
    }
    return result;
}

T3DShaderVfsAsyncLoadResult submitT3DShaderLoadsAsync(const T3DMissionExtract& /*extract*/,
                                                      CookCache* /*cache*/) {
    T3DShaderVfsAsyncLoadResult result;
    result.note = "submitted 0 async shader vfs load(s)";
    return result;
}

T3DShaderCookCacheResult drainT3DShaderLoads(fuse::HandleTable<fuse::io::Asset>& table,
                                             CookCache* cache) {
    T3DShaderCookCacheResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    result.drainedCount = vfs.drainCompletedLoads(table);

    if (cache == nullptr) {
        result.note = "drained " + std::to_string(result.drainedCount) + " shader vfs load(s)";
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

        const std::string outputPath = shaderVirtualPathToCookOutput(load.asset.virtualPath);
        if (!outputPath.empty()) {
            AssetCooker cooker;
            ShaderImportDesc desc;
            desc.input_path = physicalPath;
            desc.output_path = outputPath;
            desc.stage = inferShaderStageFromPath(physicalPath);
            const CookRecord cooked = cooker.cook_shader(desc);
            if (cooked.ok) {
                storeShaderCookCacheEntry(*cache, physicalPath, load.asset.virtualPath);
                ++result.cookCacheStores;
            }
        } else {
            storeShaderCookCacheEntry(*cache, physicalPath, load.asset.virtualPath);
            ++result.cookCacheStores;
        }
    }

    result.note = "drained " + std::to_string(result.drainedCount) + " shader vfs load(s), stored " +
                  std::to_string(result.cookCacheStores) + " cook-cache entries";
    return result;
}

} // namespace fuse::project
