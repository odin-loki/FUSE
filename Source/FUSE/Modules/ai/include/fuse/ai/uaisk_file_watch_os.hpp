#pragma once

#include <fuse/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace fuse::ai::uaisk {

/// OS file-watch progress stub (stat mtime on Linux; content-hash fallback elsewhere).
struct OsFileWatchStatus {
    bool exists = false;
    bool readable = false;
    u64 lastModifiedNs = 0;
    std::string content;
    std::string error;
};

[[nodiscard]] OsFileWatchStatus readOsFileWatchStatus(std::string_view path);

} // namespace fuse::ai::uaisk
