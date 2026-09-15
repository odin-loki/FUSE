#pragma once

#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/types.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::io {

/// Logical mount prefix for legacy and game content (WP-04).
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

using LoadId = u64;

struct CompletedLoad {
    LoadId id = 0;
    bool success = false;
    Asset asset;
};

/// Minimal VFS registry — resolves virtual paths and submits async reads on the I/O lane.
class VirtualFileSystem {
public:
    static VirtualFileSystem& instance();

    void mount(MountKind kind, std::string_view physicalPath, std::string_view virtualPrefix);
    bool resolve(std::string_view virtualPath, std::string& outPhysical) const;
    u32 mountCount() const;

    /// Submit a blocking read on the job scheduler (I/O lane). Returns 0 when resolve fails.
    LoadId submitLoadAsync(std::string_view virtualPath);

    /// Game thread: move completed worker loads into HandleTable pending publishes.
    u32 drainCompletedLoads(HandleTable<Asset>& table);

    /// Completed loads waiting for drain (observable from any thread).
    u32 completedLoadCount() const;

    /// Stub: completed load ids in FIFO completion order (no drain).
    std::vector<LoadId> peekCompletedLoadOrder() const;

    /// Game thread: inspect the most recent drain batch (tests / logging).
    const std::vector<CompletedLoad>& lastDrainedLoads() const { return m_lastDrained; }

private:
    void pushCompleted(CompletedLoad load);

    std::vector<MountPoint> m_mounts;

    LoadId m_nextLoadId = 1;
    mutable std::mutex m_completedMutex;
    std::vector<CompletedLoad> m_completed;
    std::vector<CompletedLoad> m_lastDrained;
};

} // namespace fuse::io
