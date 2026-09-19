#pragma once

#include <fuse/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace fuse::ai::uaisk {

enum class OsFileWatchBackend : u8 {
    StatPoll = 0,
    Inotify,
    FSEvents,
};

/// macOS FSEvents coalesce/latency stub (CoreServices not linked in umbrella build).
struct FSEventsWatchStats {
    u32 coalescedEventCount = 0;
    u32 latencyMs = 0;
};

/// OS file-watch status (inotify/FSEvents when portable; stat mtime + content-hash fallback).
struct OsFileWatchStatus {
    bool exists = false;
    bool readable = false;
    bool changed = false;
    u64 lastModifiedNs = 0;
    OsFileWatchBackend backend = OsFileWatchBackend::StatPoll;
    FSEventsWatchStats fsevents{};
    std::string content;
    std::string error;
};

/// Portable OS file-watch handle (inotify fd on Linux; stat poll elsewhere).
struct OsFileWatchHandle {
    int watchFd = -1;
    int inotifyFd = -1;
    std::string path;
    OsFileWatchBackend backend = OsFileWatchBackend::StatPoll;
    u64 lastModifiedNs = 0;
    u64 lastSize = 0;
    u64 lastPollNs = 0;
    FSEventsWatchStats fsevents{};
    bool active = false;
};

[[nodiscard]] OsFileWatchStatus readOsFileWatchStatus(std::string_view path);

/// Create an OS file-watch handle when the platform supports it (inotify on Linux).
[[nodiscard]] bool createOsFileWatch(std::string_view path, OsFileWatchHandle& outHandle);

/// Poll a watch handle — returns true when the file changed since the last poll.
[[nodiscard]] bool pollOsFileWatch(OsFileWatchHandle& handle, OsFileWatchStatus& outStatus);

void closeOsFileWatch(OsFileWatchHandle& handle);

} // namespace fuse::ai::uaisk
