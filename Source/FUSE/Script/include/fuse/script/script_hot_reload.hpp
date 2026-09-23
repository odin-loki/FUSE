#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::script {

class ScriptRuntime;

/// Polls watched script files and hot-reloads changed ones into a `ScriptRuntime` (call once
/// per frame). A file counts as changed when its write time or size differs from the last
/// poll and its content hash differs from the last loaded version, so a reload lands on the
/// first poll after the write. Failed reloads keep the previous code running.
class ScriptHotReload {
public:
    /// Watch one file (module key = path). Returns false when it does not exist.
    bool watch(const char* path);
    /// Watch every regular file with `extension` directly inside `directory`; returns the
    /// number of newly watched files. Files added later are picked up by `poll`.
    usize watch_directory(const char* directory, const char* extension = ".lua");
    void clear();

    /// Load new/changed files into `runtime`; returns the number of successful (re)loads.
    usize poll(ScriptRuntime& runtime);

    [[nodiscard]] usize watched_count() const { return m_watched.size(); }
    [[nodiscard]] usize reload_count() const { return m_reloadCount; }
    [[nodiscard]] usize failure_count() const { return m_failureCount; }
    [[nodiscard]] const std::string& last_error() const { return m_lastError; }

private:
    struct WatchedScript {
        std::string path;
        s64 last_write = 0;
        u64 size = 0;
        u64 content_hash = 0;
        bool loaded = false;
    };

    bool add(const std::string& path);
    void scan_directories();

    std::vector<WatchedScript> m_watched;
    std::vector<std::pair<std::string, std::string>> m_directories;
    usize m_reloadCount = 0;
    usize m_failureCount = 0;
    std::string m_lastError;
};

} // namespace fuse::script
