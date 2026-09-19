#pragma once

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fuse::ai::uaisk {

/// File-watch entry for UAISK tree profile hot-reload (stub — no OS watcher yet).
struct TreeFileWatchEntry {
    std::string path;
    std::string contentHash;
    u32 profileId = 0;
    u32 reloadCount = 0;
};

/// Registry that polls watched `.bt` / `.cs` assets and reloads tree profiles on change.
class TreeFileWatchRegistry {
public:
    void watchProfile(std::string_view path, u32 profileId, std::string_view initialContent);
    void setContent(std::string_view path, std::string_view content);

    /// Stub poll — compares content hashes and reloads changed profiles into `runtime`.
    u32 pollReloads(BehaviorRuntime& runtime, std::string* errorOut = nullptr);

    u32 watchCount() const { return static_cast<u32>(m_watches.size()); }
    u32 reloadCount() const { return m_reloadCount; }
    const TreeFileWatchEntry* entryFor(std::string_view path) const;

private:
    [[nodiscard]] static std::string hashContent(std::string_view content);
    bool reloadProfile(const TreeFileWatchEntry& entry, BehaviorRuntime& runtime, std::string* errorOut);

    std::unordered_map<std::string, TreeFileWatchEntry> m_watches;
    std::unordered_map<std::string, std::string> m_contents;
    u32 m_reloadCount = 0;
};

} // namespace fuse::ai::uaisk
