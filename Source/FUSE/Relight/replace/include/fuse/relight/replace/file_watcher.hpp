// FUSE Relight RL-3.4: file notifications for hot reload (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.2).
//
// DirectoryWatcher watches a list of directories and reports, without blocking, which of them changed since the
// last poll(). The engine polls once per frame, so a change is seen at the first frame boundary after the
// operating system reports it.
//
// Backends:
//   notify   Linux: inotify (non-blocking descriptor, one watch per directory, subdirectories added as they
//            appear); Windows: ReadDirectoryChangesW with overlapped I/O (recursive), checked with
//            GetOverlappedResult without waiting.
//   poll     a signature per watched directory (every file's relative path, size and modification time, plus the
//            content of small text files: layers, rtx.conf, JSON), recomputed at most every `pollIntervalMs`.
//   auto     notify, except under Wine (ntdll exports wine_get_version), where ReadDirectoryChangesW is not
//            dependable: poll. A notify backend that cannot be set up also falls back to poll.
// Each directory is watched recursively unless added with `recursive` false (search roots: only their children
// appearing or disappearing matters).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fuse::relight::replace {

enum class WatchBackend : std::uint8_t { Auto, Notify, Poll };
const char* watchBackendName(WatchBackend b);
/// "auto" / "notify" / "poll" (anything else: Auto).
WatchBackend parseWatchBackend(const std::string& s);

/// True when running under Wine (Windows builds only).
bool runningUnderWine();

struct WatchedDir {
    std::string dir;
    bool recursive = true;
};

class DirectoryWatcher {
public:
    DirectoryWatcher(std::vector<WatchedDir> dirs, WatchBackend backend, std::uint32_t pollIntervalMs);
    ~DirectoryWatcher();
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    /// Indices (into the constructor's list) of the directories that changed since the last call; sorted, unique.
    /// `force`: the poll backend ignores its interval.
    std::vector<std::size_t> poll(bool force = false);

    /// The backend in use: "inotify", "win32", or "poll".
    const char* backend() const;
    const std::vector<WatchedDir>& dirs() const { return m_dirs; }

    struct Impl; ///< backend state (file_watcher.cpp)

private:
    std::vector<WatchedDir> m_dirs;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fuse::relight::replace
