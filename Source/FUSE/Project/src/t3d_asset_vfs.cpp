#include <fuse/project/t3d_asset_vfs.hpp>

#include <fuse/io/vfs.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/project/t3d_material_parse.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace fuse::project {

namespace {

ShaderImportDesc::Stage inferShaderStageFromPath(const std::string& physicalPath);

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

u64 materialImportCacheKey(const std::string& physicalPath, const std::string& virtualPath) {
    TextureImportDesc desc;
    desc.input_path = physicalPath;
    desc.output_path = materialVirtualPathToCookOutput(virtualPath);
    return combine_cook_cache_key(hash_texture_import(desc), 0);
}

u64 shaderImportCacheKey(const std::string& physicalPath, const std::string& virtualPath) {
    ShaderImportDesc desc;
    desc.input_path = physicalPath;
    desc.output_path = shaderVirtualPathToCookOutput(virtualPath);
    desc.stage = inferShaderStageFromPath(physicalPath);
    return combine_cook_cache_key(hash_shader_import(desc), 0);
}

bool tryCookCacheHit(CookCache* cache, u64 cacheKey, u32& cookCacheHits) {
    if (cache == nullptr || !is_valid_cook_cache_key(cacheKey)) {
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

    const u64 cacheKey = materialImportCacheKey(physicalPath, virtualPath);
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

    const u64 cacheKey = shaderImportCacheKey(physicalPath, virtualPath);
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

bool samePhysicalPath(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) {
        return left == right;
    }

    const std::filesystem::path leftPath = std::filesystem::path(left).lexically_normal();
    const std::filesystem::path rightPath = std::filesystem::path(right).lexically_normal();
    std::error_code ec;
    if (std::filesystem::exists(leftPath, ec) && std::filesystem::exists(rightPath, ec)) {
        const bool equivalent = std::filesystem::equivalent(leftPath, rightPath, ec);
        if (!ec) {
            return equivalent;
        }
    }
    return leftPath.generic_string() == rightPath.generic_string();
}

/// Mount `virtualPrefix` at `physicalPath`, replacing a stale prefix left by a previous project.
void mountOrRemap(fuse::io::MountKind kind, const std::string& physicalPath,
                  const std::string& virtualPrefix, ProjectVfsMountResult& result) {
    if (physicalPath.empty()) {
        return;
    }

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    std::string resolved;
    while (vfs.resolve(virtualPrefix, resolved)) {
        if (samePhysicalPath(resolved, physicalPath)) {
            return;
        }
        if (!vfs.unmount(virtualPrefix)) {
            break;
        }
        ++result.mountsRemapped;
    }

    vfs.mount(kind, physicalPath, virtualPrefix);
    ++result.mountsAdded;
}

/// Cook the Material definition in `physicalPath` (a drained `/t3d/materials/.../Name.mat`) to `outputPath`.
/// Picks the block named (or mapped to) `Name`, else the only / first block. False when nothing was written.
bool cookDrainedMaterialDefinition(const std::string& physicalPath, const std::string& virtualPath,
                                   const std::string& outputPath) {
    const T3DMaterialParseResult parsed = parseT3DMaterialFile(physicalPath);
    if (parsed.materials.empty()) {
        fuse::log::warn("drainT3DMaterialLoads: %s has no Material definition; nothing cooked", virtualPath.c_str());
        return false;
    }
    const std::filesystem::path output(outputPath);
    const std::string wanted = output.stem().string();
    const T3DMaterialDef* chosen = &parsed.materials.front();
    for (const T3DMaterialDef& def : parsed.materials) {
        if (def.name == wanted || def.mapTo == wanted) {
            chosen = &def;
            break;
        }
    }
    T3DMaterialDef def = *chosen;
    def.name = wanted; // the .fusemat is named after the reference it serves
    T3DMaterialCookOptions options;
    options.outputDir = output.parent_path().generic_string();
    options.gameRoot = std::filesystem::path(physicalPath).parent_path().generic_string();
    const T3DMaterialCookResult cooked = cookT3DMaterials({def}, options);
    return cooked.fusematWritten == 1u;
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

    mountOrRemap(fuse::io::MountKind::Game, gamePath.string(), "/game/", result);
    mountOrRemap(fuse::io::MountKind::T3DLegacy, t3dPath.string(), "/t3d/", result);
    mountOrRemap(fuse::io::MountKind::T2DLegacy, t2dPath.string(), "/t2d/", result);

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    std::string probe;
    result.gameMount = vfs.resolve("/game/", probe);
    result.t3dMount = vfs.resolve("/t3d/", probe);
    result.t2dMount = vfs.resolve("/t2d/", probe);
    result.note = "mounted " + std::to_string(result.mountsAdded) + " project asset root(s), remapped " +
                  std::to_string(result.mountsRemapped);
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

    // UNI-U7-MAT-1: the material definition cooks to a `.fusemat` (its maps to `textures/*.fusetex` beside it).
    return "cooked/materials/" + relative + ".fusemat";
}

std::string materialVirtualPathToRefName(const std::string& virtualPath) {
    constexpr const char* prefix = "/t3d/materials/";
    if (virtualPath.size() < std::strlen(prefix) ||
        virtualPath.compare(0, std::strlen(prefix), prefix) != 0) {
        return {};
    }

    std::string relative = virtualPath.substr(std::strlen(prefix));
    if (relative.size() >= 4 && relative.compare(relative.size() - 4, 4, ".mat") == 0) {
        relative.resize(relative.size() - 4);
    }

    const std::size_t slash = relative.rfind('/');
    if (slash == std::string::npos) {
        return relative;
    }

    return relative.substr(0, slash) + ":" + relative.substr(slash + 1);
}

std::string remapLegacyAssetPath(const std::string& legacyPath) {
    if (legacyPath.empty()) {
        return {};
    }

    std::string path = legacyPath;
    for (char& ch : path) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path.erase(0, 2);
    }
    if (!path.empty() && path.front() == '/') {
        return path;
    }

    auto startsWithInsensitive = [&](const char* prefix) {
        const std::size_t prefixLen = std::strlen(prefix);
        if (path.size() < prefixLen) {
            return false;
        }
        for (std::size_t i = 0; i < prefixLen; ++i) {
            const unsigned char left = static_cast<unsigned char>(path[i]);
            const unsigned char right = static_cast<unsigned char>(prefix[i]);
            if (std::tolower(left) != std::tolower(right)) {
                return false;
            }
        }
        return true;
    };

    if (startsWithInsensitive("data/")) {
        return "/t3d/" + path.substr(path.find('/') + 1);
    }
    if (startsWithInsensitive("game/") || startsWithInsensitive("assets/")) {
        return "/game/" + path.substr(path.find('/') + 1);
    }
    if (startsWithInsensitive("levels/") || startsWithInsensitive("art/") ||
        startsWithInsensitive("shaders/")) {
        return "/t3d/" + path;
    }

    return "/t3d/" + path;
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

std::string shaderVirtualPathToRefName(const std::string& virtualPath) {
    constexpr const char* prefix = "/t3d/shaders/";
    if (virtualPath.size() < std::strlen(prefix) ||
        virtualPath.compare(0, std::strlen(prefix), prefix) != 0) {
        return {};
    }

    std::string relative = virtualPath.substr(std::strlen(prefix));
    if (relative.size() >= 3 && relative.compare(relative.size() - 3, 3, ".cs") == 0) {
        relative.resize(relative.size() - 3);
    }

    const std::size_t slash = relative.rfind('/');
    if (slash == std::string::npos) {
        return relative;
    }

    return relative.substr(0, slash) + ":" + relative.substr(slash + 1);
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

u64 materialCookCacheKey(const std::string& physicalPath, const std::string& virtualPath) {
    return materialImportCacheKey(physicalPath, virtualPath);
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
            tryCookCacheHit(cache, materialImportCacheKey(physicalPath, virtualPath), result.cookCacheHits)) {
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
            tryCookCacheHit(cache, materialImportCacheKey(physicalPath, virtualPath), result.cookCacheHits)) {
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
            // UNI-U7-MAT-1: the drained file is the T3D material *definition*. Parse its Material block,
            // strictly cook the maps it names (BC7 albedo / BC5 normal) and emit the `.fusemat`; a file
            // without a usable definition produces nothing (no placeholder texture).
            if (cookDrainedMaterialDefinition(physicalPath, load.asset.virtualPath, outputPath)) {
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

T3DShaderVfsResolveResult resolveT3DShaderVfsPaths(const T3DMissionExtract& extract) {
    T3DShaderVfsResolveResult result;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();

    for (const T3DShaderRefStub& shader : extract.shaders) {
        ++result.shaderCount;
        const std::string virtualPath = shaderAssetToVirtualPath(shader.shaderRef);
        std::string physicalPath;
        if (!virtualPath.empty() && vfs.resolve(virtualPath, physicalPath)) {
            ++result.resolvedCount;
        } else {
            ++result.unresolvedCount;
        }
    }

    for (const T3DSimObjectStub& object : extract.simObjects) {
        if (object.shaderAsset.empty()) {
            continue;
        }
        ++result.shaderCount;
        const std::string virtualPath = shaderAssetToVirtualPath(object.shaderAsset);
        std::string physicalPath;
        if (!virtualPath.empty() && vfs.resolve(virtualPath, physicalPath)) {
            ++result.resolvedCount;
        } else {
            ++result.unresolvedCount;
        }
    }

    result.note = "resolved " + std::to_string(result.resolvedCount) + "/" +
                  std::to_string(result.shaderCount) + " shader vfs paths";
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

u64 shaderCookCacheKey(const std::string& physicalPath, const std::string& virtualPath) {
    return shaderImportCacheKey(physicalPath, virtualPath);
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
            tryCookCacheHit(cache, shaderImportCacheKey(physicalPath, virtualPath), result.cookCacheHits)) {
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

T3DShaderVfsAsyncLoadResult submitT3DShaderLoadsAsync(const T3DMissionExtract& extract,
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
            tryCookCacheHit(cache, shaderImportCacheKey(physicalPath, virtualPath), result.cookCacheHits)) {
            return;
        }

        const fuse::io::LoadId loadId = vfs.submitLoadAsync(virtualPath);
        if (loadId != 0u) {
            result.loadIds.push_back(loadId);
            ++result.submittedCount;
        }
    };

    for (const T3DShaderRefStub& shader : extract.shaders) {
        submitShaderRef(shader.shaderRef);
    }

    for (const T3DSimObjectStub& object : extract.simObjects) {
        if (!object.shaderAsset.empty()) {
            submitShaderRef(object.shaderAsset);
        }
    }

    result.note = "submitted " + std::to_string(result.submittedCount) + " async shader vfs load(s)";
    if (result.cookCacheHits > 0u) {
        result.note += ", cook-cache hits=" + std::to_string(result.cookCacheHits);
    }
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
