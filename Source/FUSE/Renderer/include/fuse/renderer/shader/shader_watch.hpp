#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer {

/// Poll-based shader source file watcher (B2.4).
/// CI-safe: no watcher thread and no ReadDirectoryChanges completion port.
class ShaderFileWatch {
public:
    /// Records `path` plus the current mtime/size snapshot. Missing files use mtime 0.
    /// Returns false if `path` is null.
    bool watch(const char* path);

    /// Count of watched files whose mtime or size changed since the last poll/watch.
    /// Updates snapshots. Missing files do not fail the poll (mtime 0).
    u32 pollChanged();

    u32 watchedCount() const;

private:
    struct Entry {
        std::string path;
        u64 mtime = 0;
        u64 size = 0;
    };

    std::vector<Entry> m_entries;
};

} // namespace fuse::renderer
