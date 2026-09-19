#include <fuse/ai/uaisk_file_watch_os.hpp>

#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <sys/stat.h>
#endif

namespace fuse::ai::uaisk {

OsFileWatchStatus readOsFileWatchStatus(std::string_view path) {
    OsFileWatchStatus status{};
    if (path.empty()) {
        status.error = "empty path";
        return status;
    }

    const std::string pathStr(path);

#if defined(__linux__)
    struct stat fileStat {};
    if (stat(pathStr.c_str(), &fileStat) != 0) {
        status.error = "stat failed";
        return status;
    }
    status.exists = true;
    status.lastModifiedNs =
        static_cast<u64>(fileStat.st_mtim.tv_sec) * 1'000'000'000ull +
        static_cast<u64>(fileStat.st_mtim.tv_nsec);
#else
    status.exists = true;
#endif

    std::ifstream stream(pathStr, std::ios::binary);
    if (!stream.is_open()) {
        status.error = "open failed";
        return status;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    status.content = buffer.str();
    status.readable = true;

#if !defined(__linux__)
    u64 hash = 2166136261u;
    for (unsigned char ch : status.content) {
        hash ^= static_cast<u64>(ch);
        hash *= 16777619u;
    }
    status.lastModifiedNs = hash;
#endif

    return status;
}

} // namespace fuse::ai::uaisk
