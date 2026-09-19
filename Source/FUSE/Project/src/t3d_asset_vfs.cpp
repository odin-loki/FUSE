#include <fuse/project/t3d_asset_vfs.hpp>

#include <fuse/io/vfs.hpp>
#include <fuse/log/logger.hpp>

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

} // namespace fuse::project
