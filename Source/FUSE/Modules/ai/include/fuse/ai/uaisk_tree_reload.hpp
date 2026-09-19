#pragma once

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/uaisk_file_watch_os.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fuse::ai::uaisk {

/// File-watch entry for UAISK tree profile hot-reload (OS stat mtime + content-hash fallback).
struct TreeFileWatchEntry {
    std::string path;
    std::string contentHash;
    u32 profileId = 0;
    u32 reloadCount = 0;
    u64 lastModifiedNs = 0;
    bool osWatchEnabled = false;
    OsFileWatchBackend osBackend = OsFileWatchBackend::StatPoll;
};

/// Registry that polls watched `.bt` / `.cs` assets and reloads tree profiles on change.
class TreeFileWatchRegistry {
public:
    void watchProfile(std::string_view path, u32 profileId, std::string_view initialContent);
    void watchProfileFromDisk(std::string_view path, u32 profileId);
    void setContent(std::string_view path, std::string_view content);

    /// Stub poll — compares content hashes and reloads changed profiles into `runtime`.
    u32 pollReloads(BehaviorRuntime& runtime, std::string* errorOut = nullptr);

    /// OS poll — stat mtime / disk read and reload changed profiles into `runtime`.
    u32 pollOsFileChanges(BehaviorRuntime& runtime, std::string* errorOut = nullptr);

    /// Inotify/FSEvents poll — uses portable OS watch handles when available.
    u32 pollInotifyFileChanges(BehaviorRuntime& runtime, std::string* errorOut = nullptr);

    u32 watchCount() const { return static_cast<u32>(m_watches.size()); }
    u32 reloadCount() const { return m_reloadCount; }
    u32 osPollCount() const { return m_osPollCount; }
    u32 osReloadCount() const { return m_osReloadCount; }
    u32 inotifyPollCount() const { return m_inotifyPollCount; }
    u32 inotifyReloadCount() const { return m_inotifyReloadCount; }
    const TreeFileWatchEntry* entryFor(std::string_view path) const;

private:
    [[nodiscard]] static std::string hashContent(std::string_view content);
    bool reloadProfile(const TreeFileWatchEntry& entry, BehaviorRuntime& runtime, std::string* errorOut);
    bool applyOsStatus_(TreeFileWatchEntry& entry, std::string_view content, u64 lastModifiedNs,
                        BehaviorRuntime& runtime, std::string* errorOut);

    std::unordered_map<std::string, TreeFileWatchEntry> m_watches;
    std::unordered_map<std::string, std::string> m_contents;
    u32 m_reloadCount = 0;
    u32 m_osPollCount = 0;
    u32 m_osReloadCount = 0;
    u32 m_inotifyPollCount = 0;
    u32 m_inotifyReloadCount = 0;
    std::unordered_map<std::string, OsFileWatchHandle> m_osHandles;
};

} // namespace fuse::ai::uaisk
