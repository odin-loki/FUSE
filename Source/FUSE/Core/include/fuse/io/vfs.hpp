#pragma once

#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::io {

/// Logical mount prefix for legacy and game content (WP-04 stub).
enum class MountKind : u8 {
    Game = 0,
    T3DLegacy = 1,
    T2DLegacy = 2,
};

struct MountPoint {
    MountKind kind = MountKind::Game;
    std::string physicalPath;
    std::string virtualPrefix;
};

/// Minimal VFS registry — resolves virtual paths to host paths.
class VirtualFileSystem {
public:
    static VirtualFileSystem& instance();

    void mount(MountKind kind, std::string_view physicalPath, std::string_view virtualPrefix);
    bool resolve(std::string_view virtualPath, std::string& outPhysical) const;
    u32 mountCount() const;

private:
    std::vector<MountPoint> m_mounts;
};

} // namespace fuse::io
