#include <fuse/io/vfs.hpp>

namespace fuse::io {

VirtualFileSystem& VirtualFileSystem::instance() {
    static VirtualFileSystem vfs;
    return vfs;
}

void VirtualFileSystem::mount(MountKind kind, std::string_view physicalPath, std::string_view virtualPrefix) {
    MountPoint point;
    point.kind = kind;
    point.physicalPath = std::string(physicalPath);
    point.virtualPrefix = std::string(virtualPrefix);
    m_mounts.push_back(std::move(point));
}

bool VirtualFileSystem::resolve(std::string_view virtualPath, std::string& outPhysical) const {
    for (const MountPoint& mount : m_mounts) {
        if (virtualPath.size() < mount.virtualPrefix.size()) {
            continue;
        }
        if (virtualPath.substr(0, mount.virtualPrefix.size()) != mount.virtualPrefix) {
            continue;
        }

        const std::string_view remainder = virtualPath.substr(mount.virtualPrefix.size());
        outPhysical = mount.physicalPath;
        if (!remainder.empty() && remainder.front() != '/') {
            outPhysical.push_back('/');
        }
        outPhysical.append(remainder);
        return true;
    }
    return false;
}

u32 VirtualFileSystem::mountCount() const {
    return static_cast<u32>(m_mounts.size());
}

} // namespace fuse::io
