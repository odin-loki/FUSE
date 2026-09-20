#include <fuse/ai/uaisk_file_watch_os.hpp>
#include <fuse/ai/uaisk_fsevents_coreservices_stub.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <cerrno>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <chrono>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

namespace fuse::ai::uaisk {

namespace {

bool readFileContent(const std::string& pathStr, OsFileWatchStatus& status) {
    std::ifstream stream(pathStr, std::ios::binary);
    if (!stream.is_open()) {
        status.error = "open failed";
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    status.content = buffer.str();
    status.readable = true;
    return true;
}

u64 statModifiedNs(const std::string& pathStr, u64& outSize) {
#if defined(__linux__) || defined(__APPLE__)
    struct stat fileStat {};
    if (stat(pathStr.c_str(), &fileStat) != 0) {
        outSize = 0;
        return 0;
    }
    outSize = static_cast<u64>(fileStat.st_size);
#if defined(__linux__)
    return static_cast<u64>(fileStat.st_mtim.tv_sec) * 1'000'000'000ull +
           static_cast<u64>(fileStat.st_mtim.tv_nsec);
#else
    return static_cast<u64>(fileStat.st_mtimespec.tv_sec) * 1'000'000'000ull +
           static_cast<u64>(fileStat.st_mtimespec.tv_nsec);
#endif
#else
    outSize = 0;
    return 0;
#endif
}

u64 contentHashNs(const std::string& content) {
    u64 hash = 2166136261u;
    for (unsigned char ch : content) {
        hash ^= static_cast<u64>(ch);
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

OsFileWatchStatus readOsFileWatchStatus(std::string_view path) {
    OsFileWatchStatus status{};
    if (path.empty()) {
        status.error = "empty path";
        return status;
    }

    const std::string pathStr(path);
    u64 fileSize = 0;
    const u64 mtimeNs = statModifiedNs(pathStr, fileSize);
    if (mtimeNs == 0 && fileSize == 0) {
        std::ifstream probe(pathStr);
        if (!probe.is_open()) {
            status.error = "stat failed";
            return status;
        }
    }

    status.exists = true;
    status.lastModifiedNs = mtimeNs;
    status.backend = OsFileWatchBackend::StatPoll;

    if (!readFileContent(pathStr, status)) {
        return status;
    }

#if !defined(__linux__) && !defined(__APPLE__)
    status.lastModifiedNs = contentHashNs(status.content);
#endif

    return status;
}

bool createOsFileWatch(std::string_view path, OsFileWatchHandle& outHandle) {
    closeOsFileWatch(outHandle);
    outHandle = OsFileWatchHandle{};
    if (path.empty()) {
        return false;
    }

    outHandle.path = std::string(path);

#if defined(__linux__)
    outHandle.inotifyFd = inotify_init1(IN_NONBLOCK);
    if (outHandle.inotifyFd < 0) {
        outHandle.backend = OsFileWatchBackend::StatPoll;
    } else {
        outHandle.watchFd = inotify_add_watch(outHandle.inotifyFd, outHandle.path.c_str(),
                                              IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF);
        if (outHandle.watchFd >= 0) {
            outHandle.backend = OsFileWatchBackend::Inotify;
            outHandle.active = true;
            u64 fileSize = 0;
            outHandle.lastModifiedNs = statModifiedNs(outHandle.path, fileSize);
            outHandle.lastSize = fileSize;
            return true;
        }
        close(outHandle.inotifyFd);
        outHandle.inotifyFd = -1;
    }
#elif defined(__APPLE__)
    // FSEvents stub — CoreServices API not linked; stat poll with coalesce/latency tracking.
    const fsevents_stub::CoreServicesWatchConfig csConfig = fsevents_stub::makeDefaultCoreServicesWatchConfig();
    (void)fsevents_stub::createFileEventStreamStub(outHandle.path.c_str(), csConfig);
    outHandle.backend = OsFileWatchBackend::FSEvents;
    u64 fileSize = 0;
    outHandle.lastModifiedNs = statModifiedNs(outHandle.path, fileSize);
    outHandle.lastSize = fileSize;
    outHandle.lastPollNs = outHandle.lastModifiedNs;
    outHandle.fsevents.latencyMs = csConfig.latencyMs;
    outHandle.fsevents.coreServicesCreateFlags = csConfig.createFlags;
    outHandle.fseventsCoalesceThreshold = csConfig.coalesceThreshold;
    outHandle.active = true;
    return true;
#endif

    u64 fileSize = 0;
    outHandle.lastModifiedNs = statModifiedNs(outHandle.path, fileSize);
    outHandle.lastSize = fileSize;
    outHandle.backend = OsFileWatchBackend::StatPoll;
    outHandle.active = true;
    return true;
}

bool pollOsFileWatch(OsFileWatchHandle& handle, OsFileWatchStatus& outStatus) {
    outStatus = OsFileWatchStatus{};
    if (!handle.active || handle.path.empty()) {
        outStatus.error = "inactive watch";
        return false;
    }

    bool changed = false;

#if defined(__linux__)
    if (handle.backend == OsFileWatchBackend::Inotify && handle.inotifyFd >= 0) {
        char buffer[512];
        const ssize_t bytesRead = read(handle.inotifyFd, buffer, sizeof(buffer));
        if (bytesRead > 0) {
            changed = true;
        } else if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            outStatus.error = "inotify read failed";
        }
        outStatus.backend = OsFileWatchBackend::Inotify;
    }
#elif defined(__APPLE__)
    if (handle.backend == OsFileWatchBackend::FSEvents) {
        outStatus.backend = OsFileWatchBackend::FSEvents;
        const auto now = std::chrono::steady_clock::now();
        const u64 nowNs = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        if (handle.lastPollNs != 0 && nowNs > handle.lastPollNs) {
            const u64 deltaMs = (nowNs - handle.lastPollNs) / 1'000'000ull;
            handle.fsevents.latencyMs = static_cast<u32>(std::min<u64>(deltaMs, 250ull));
        }
        handle.lastPollNs = nowNs;
        outStatus.fsevents = handle.fsevents;
    }
#endif

    u64 fileSize = 0;
    const u64 mtimeNs = statModifiedNs(handle.path, fileSize);
    if (mtimeNs != 0) {
        outStatus.exists = true;
        outStatus.lastModifiedNs = mtimeNs;
        if (mtimeNs != handle.lastModifiedNs || fileSize != handle.lastSize) {
            changed = true;
#if defined(__APPLE__)
            if (handle.backend == OsFileWatchBackend::FSEvents) {
                ++handle.fsevents.coalescedEventCount;
                outStatus.fsevents = handle.fsevents;
            }
#endif
        }
    }

    if (!changed) {
        outStatus.changed = false;
        return false;
    }

#if defined(__APPLE__)
    if (handle.backend == OsFileWatchBackend::FSEvents &&
        handle.fsevents.coalescedEventCount >= handle.fseventsCoalesceThreshold) {
        ++handle.fsevents.reloadSkipCount;
        outStatus.fsevents = handle.fsevents;
        outStatus.changed = false;
        return false;
    }
#endif

    if (!readFileContent(handle.path, outStatus)) {
        return false;
    }

    handle.lastModifiedNs = outStatus.lastModifiedNs;
    handle.lastSize = fileSize;
    outStatus.changed = true;
    if (outStatus.backend == OsFileWatchBackend::StatPoll) {
        outStatus.backend = handle.backend;
    }
    return true;
}

void closeOsFileWatch(OsFileWatchHandle& handle) {
#if defined(__linux__)
    if (handle.inotifyFd >= 0) {
        if (handle.watchFd >= 0) {
            inotify_rm_watch(handle.inotifyFd, handle.watchFd);
        }
        close(handle.inotifyFd);
    }
#endif
    handle.watchFd = -1;
    handle.inotifyFd = -1;
    handle.active = false;
}

} // namespace fuse::ai::uaisk
